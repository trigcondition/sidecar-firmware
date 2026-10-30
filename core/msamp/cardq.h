// core/msamp/cardq.h - one card-I/O queue (docs/SAMPLING.md "The card queue"): the streaming notes' card work
// (MULTISAMPLE's opens and reads, msamp.h) posted by the control core, run by one card core in priority order before
// the lower ones (the recorder's writes, then file uploads) are let at the card.
//
// Why: Circle's EMMC driver transfers by PIO with busy-waits and FatFs holds a spin lock per volume for a whole call
// (docs/MULTISAMPLE.md "Where it runs on Circle"). With the stream reads on core 3 and the recorder's 192 KB writes on
// core 0, a read that came during a write spun until it ended (100-137 ms, measured on the Pi), core 3's MIDI and
// SysEx waiting with it, and the lock's order was whoever spun first. Now:
//   - the control core (core 3) never touches the card for a stream: it posts a request and takes the result on a
//     later pass (no blocking, no spinning);
//   - the card core (core 0's loop, which writes the recorder) runs every posted request before each recorder write,
//     and the recorder writes in kPiece pieces (64 KB), so a stream read waits one piece at most;
//   - busy() tells the lower priorities to hold off while a request waits or a stream is below its low mark (the
//     recorder while its ring has room, an upload for up to kUploadWaitMs a write).
// Without a card core (set_worker(false): the Mac's tests and library, by default) a posted request runs at once in
// post(): the old single-core behaviour, step for step.
//
// Requests are the caller's (static, no allocation here). One producer (the control core), one consumer (the card
// core): a ring of pointers, an atomic state per request.
#pragma once
#include <stdint.h>

#include "../platform.h"

namespace core {
namespace cardq {

constexpr int kQueue = 16;               // requests waiting at most
constexpr uint32_t kPiece = 65536;       // the recorder's write unit (a 192 KB chunk in three)
constexpr uint32_t kUploadWaitMs = 200;  // an upload's write waits at most this long for the streams
constexpr int kPathMax = 192;
constexpr uint32_t kCopyMax = 65536;     // kCopyReq: bytes a request copies at most

// fix1041: kCloseReq closes f (FatFs takes the volume's lock for that too); kWriteReq writes a whole small file: its
// folders made, path opened for writing (truncated), buf[0..len) written, closed; got the bytes written (-1 failed).
// kAtomicReq (core/fmt/fmt.h write_atomic): buf[0..len) to path as kWriteReq does, but through <path>.tmp, read back and
// renamed into place (the old file kept until the new one is whole); got the bytes written (-1 failed, the old file kept).
// kExistsReq: got 1 when path is on the card (a file or a folder), 0 not (the pool lists: an open that failed on a file
// that is there is an error, not an absent list)
// kRenameReq (core/bundle/rename.h): path renamed to the NUL-ended name in buf (a file or a folder); got 1 done, 0 not
// (the target there already, or the card refused)
// kListReq (2026-10-26, the RAM store's fill: core/store/store.h): the names of path's entries (a folder; its files and
// folders, '.' and '..' left out) into buf[0..len), each NUL-ended, one after the other; got the bytes used, or
// kListMissing (no such folder, as the card says it - FR_NO_FILE / FR_NO_PATH: nothing in it is on the card) or
// kListUnread (there but not opened as a folder, its names did not fit in len, a card error reading it, or the card
// could not tell whether it is there: the caller asks per path as before). Why: a fill asked for each of a project's ~500 paths
// with an open and, for the many that are not there, an exists - two FatFs path walks each, measured 7.3 s at a boot
// for 258 paths; one walk of the folder answers all of them.
// kCopyReq (2026-10-27, core/bundle/prjslot.h): up to len bytes of the file at path, from off, appended to the NUL-ended
// path in buf (off 0: its folders made, the target created - refused when it is there already, never written over); got
// the bytes copied (0: path ends at off - the copy is complete), -1 failed. Bounded (len, 64 KB a call), so a project's
// copy is many short requests and the streams keep their turn.
// kRemoveReq (2026-10-27; fmt::post_remove and the project trash, core/bundle/prjslot.h): path removed - a file or an
// empty folder; got 1 removed (checked gone) or not there, 0 not removed (a folder with files, a card error).
enum Kind : uint8_t { kOpenReq, kReadReq, kCloseReq, kWriteReq, kAtomicReq, kExistsReq, kRenameReq, kListReq, kCopyReq,
                      kRemoveReq, kKinds };   // (not kOpen / kRead: core::kRead is a file mode)
constexpr int32_t kListMissing = -1, kListUnread = -2;
enum State : uint32_t { kFree = 0, kPosted, kRunning, kDone, kCancelled };

struct Req {
  volatile uint32_t state;   // State; kDone / kCancelled / kFree: the caller's again
  Kind kind;                 // kOpenReq, kReadReq
  File* f;                   // kOpenReq: opened here (core::kRead); kReadReq: read from (its position set to off);
                             // kCloseReq: closed; kWriteReq: the handle written through (the caller's, closed after)
  char path[kPathMax];       // kOpenReq, kWriteReq
  uint64_t off;              // kReadReq
  uint8_t* buf;
  uint32_t len;
  int32_t got;               // kReadReq: bytes read (-1 an error); kOpenReq: 1 opened, 0 not
  uint32_t posted_us;
};

void set_worker(bool on);    // a card core runs service() (the Pi: core 0); off: post() runs the request itself
bool worker();
// The control core's
bool post(Req* r);           // false: the queue is full (r untouched)
bool cancel(Req* r);         // a posted request that has not started: it will not (true); false: running or done
void wait(Req* r);           // until r is done (a running request: one card operation)
// A request still waiting after kTakeBackUs (the card core is held up: a blocking TCP connect, a long upload): the
// control core runs it itself (cancelled there, run here: true). The streams never wait on a stalled card core.
// (fmt's posted settings / pattern writes do not take back: they stay posted, fmt::post_held - phase 3, 2026-10-26)
constexpr uint32_t kTakeBackUs = 200000;   // (over a card's own pauses inside a write: 100-137 ms measured)
bool take_back(Req* r);
bool idle(const Req* r);     // done, cancelled or never posted: the caller may reuse r (and its File)
void set_need(uint32_t ahead_ms, uint32_t low_ms);   // the neediest stream's audio ahead (~0u: no stream), each pass
// fix1041: a load's card operation, step by step on the control core (GRANULAR's and WAVETABLE's loads, a pool file's
// write): call it each step with the same request until it returns true (done: r->got); false while it waits
// (posted, running, or the queue full: posted at a later call). Nothing waits or spins. Without a card core it runs at
// once (true at the first call), the old path.
bool async(Req* r, Kind kind, File* f, const char* path, uint64_t off, uint8_t* buf, uint32_t len);
// The card core's
Req* service();              // the oldest posted request run (nullptr: none waiting)
int pending();
uint32_t oldest_ms();        // the age of the oldest posted request still waiting (ms); 0 when none (/status "store")
bool busy();                 // a request waits, or a stream's audio ahead is under its low mark
struct Stats {
  uint32_t posted, served, cancelled, full, yields, upload_waits, taken_back;
  uint32_t max_pending, max_wait_us;   // the longest a request waited for the card core
  uint32_t ran[kKinds];                // requests run (by the card core, a post without one, or a take-back), by kind
};
const Stats& stats();
void note_yield();           // the recorder held a write back (stats)
void note_upload_wait();     // an upload's write waited (stats)
void reset();                // tests: the queue empty, the stats cleared, no worker

}  // namespace cardq
}  // namespace core
