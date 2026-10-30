// core/msamp/sdtest.h - the SD card speed test (docs/MULTISAMPLE.md "SD card test"): is this card fast enough to
// stream MULTISAMPLE notes, with or without the recorder? Measured on the board's own card path (the Pi: Circle's
// EMMC driver + FatFs, through core::File), between blocks on the control core, one card operation a step.
//
//   files    /.rytm/sdtest.bin (the test file: min(256 MB, a quarter of the free space), at least 32 MB; refused
//            below 64 MB free) and /.rytm/sdtest2.bin (the recorder-like writes), both removed at the end, on a
//            cancel and at the next start. Nothing else on the card is touched.
//   phases   sequential write (64 KB writes) -> sequential read (64 KB) -> random reads of 4, 32 and 64 KB (200
//            each, 4 KB aligned) -> mixed: random 32 KB reads while 192 KB writes go to a second file (the
//            recorder's write size, one every 4 reads) -> clean up.
//   result   MB/s, IOPS and latency (p50 / p99 / worst) per size, the mixed reads' latency, and the verdict:
//            streaming voices (stereo 24-bit 48 kHz at unity pitch: 288 KB/s, 9 reads of 32 KB a second each) with
//            and without the recorder (13 files of 24-bit: 1.97 MB/s, docs/pi-engine.md), the read-ahead it will use
//            (1.5 x the worst read seen, with the writes too, + 30 ms; 100..600) and the stream count; written to /.rytm/sdtest.txt
//            (key=value lines) and given to msamp (set_stream_limits). A one-line summary in the log.
//   ui       the console's `sdtest` (the last result), `sdtest=start` (refused while MULTISAMPLE notes play;
//            `sdtest=start!` anyway: audio may drop while it runs), `sdtest=cancel`; json() for the web page and the
//            remote UI's line (line()).
#pragma once
#include <stdint.h>

namespace core {
namespace sdtest {

constexpr int kSizes = 3;                          // random reads: 4, 32, 64 KB
constexpr uint32_t kRandSize[kSizes] = {4096, 32768, 65536};
constexpr int kRandN = 200, kMixedN = 200;
constexpr uint32_t kSeqChunk = 65536, kRecChunk = 192 * 1024;
constexpr int kMinVoices = 8;                      // fewer: "too slow"

struct Lat {
  float iops, p50_ms, p99_ms, max_ms, mean_ms;
};
struct Measured {
  float write_mbps, read_mbps;
  Lat rand[kSizes];
  Lat mixed;                 // 32 KB reads while the recorder-like writes run
  float mixed_write_mbps;
  float write_max_ms;        // the worst 64 KB write
};
struct Verdict {
  int voices;                // stereo 24-bit streaming notes without the recorder
  int voices_rec;            // with it
  uint32_t ahead_ms;         // the read-ahead to use
  int streams;               // msamp's stream count
  int level;                 // 2 OK with the recorder, 1 OK without, 0 too slow
  char text[120];
};
// The verdict for measured numbers (pure: the host tests feed it set speeds).
Verdict judge(const Measured& m);

// The card the test uses (tests: a fake one with set speeds). Null members: core::File on the board's card.
struct Io {
  bool (*open)(int which, bool write);             // 0 the test file, 1 the recorder-like one
  int (*read)(int which, uint64_t off, void* buf, uint32_t n);
  int (*write)(int which, const void* buf, uint32_t n);
  void (*close)(int which);
  void (*remove)(int which);
  uint64_t (*free_bytes)();                        // 0: unknown (the test then uses 64 MB)
  uint32_t (*now_us)();
};
void set_io(const Io* io);                         // null: the board's card

enum State : uint8_t { kIdle, kRunning, kDone, kFailed, kCancelled };
bool start(uint32_t max_mb = 256);                 // false: already running, or no room (state kFailed, why())
void cancel();
bool step();                                       // one card operation; true while it runs
State state();
int progress();                                    // 0..100
const char* why();                                 // the failure or the phase
const Measured& measured();
const Verdict& verdict();                          // the last one (also read back from /.rytm/sdtest.txt at load())
void load();                                       // boot: the last result from the card, if any
int json(char* out, int cap);                      // {"state":..,"progress":..,"verdict":..,...}
const char* line();                                // the remote UI's SYSTEM page: "SD OK 24" (streams with the recorder),
                                                   // "SD 24 NR" (without it), "SD SLOW", "SD 40%" (testing), "SD -"
bool command(const char* p, bool audio_busy, char* out, int cap);   // "sdtest", "sdtest=start[!]", "sdtest=cancel"

}  // namespace sdtest
}  // namespace core
