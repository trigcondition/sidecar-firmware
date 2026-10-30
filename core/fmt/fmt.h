// core/fmt/fmt.h - the format registry (docs/superpowers/specs/2026-10-22-formats-and-library-design.md sections 3-4,
// docs/LIBRARY.md "Formats"): one table of every file format the Sidecar reads or writes on the card, one header
// convention per file shape, one load path (sniff, upgrade in memory, refuse newer or unreadable files: locked, never
// written over), one atomic write (core 0 only), one place for every format's path.
//
// Rules (spec 3.3): a writer writes the current version only; a file newer than this build reads is refused clearly
// (kNewer: locked) unless its min_reader says this build may read it (kForeignNewer: read, never written back); a
// damaged file is locked the same way (kUnreadable); upgrades are pure functions on bytes (no card I/O, no globals),
// vN -> vN+1, run in memory by load().
//
// F1 (this file's phase): every format in the spec's inventory is registered at the version its files have today
// (v1 for every unversioned one) with a sniffer and a reader; no chains yet (F3 adds the first upgraders), no change on
// the card. Settings files (class S) keep the key=value lines they do not understand (kv_keep / kv_append).
//
// Portable (Pi, Mac core, tests): load(), the sniffers, readers, header helpers and path() touch no card; the header
// helpers and path() may run on any core, load() on one core at a time (some readers use static scratch: the
// pattern, the take); write_atomic(), AtomicWriter and recover() are the card core's (core 0 on the Pi; cardq's kAtomicReq
// posts one from the control core).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../platform.h"

namespace core {
namespace fmt {

enum Class : uint8_t { kProject = 0, kSetting, kDerived, kExchange };
enum Shape : uint8_t { kText = 0, kKeyValue, kJson, kBinary };
const char* class_name(int c);   // "project" "setting" "derived" "exchange"
const char* shape_name(int s);   // "text" "key=value" "json" "binary"

struct Bytes {
  const uint8_t* p;
  uint32_t n;
};
struct Buf {   // the caller's memory (no allocation here)
  uint8_t* p;
  uint32_t n, cap;
};
struct Ctx {   // what an upgrade may use (spec 3.1): no card I/O
  const char* key;                     // the project's key ("" none)
  void (*log)(const char* line);       // may be null
};
struct Sniffed {
  uint16_t version;      // the file's (1 for today's unversioned files)
  uint16_t min_reader;   // the lowest reader version that reads it right (= version when the file does not say)
  uint32_t body;         // where the content after the header starts (0: no header)
};
// bytes -> what the file says it is; false: not this format at all (a different file, binary junk, too short)
typedef bool (*Sniff)(const Bytes& in, Sniffed* out);
// the current version's reader (pure): true when the bytes are a file this build can use; why gets the reason
typedef bool (*Read)(const Bytes& in, const Sniffed& s, char* why, int cap);
struct Upgrade {
  uint16_t from;   // from -> from + 1
  bool (*up)(const Bytes& in, Buf* out, Ctx* c);
};
struct Format {
  const char* name;          // "pool", "bankpool", "slots", "sqp", "song", ... (spec section 1)
  uint8_t cls, shape;
  uint16_t current;          // the only version written
  uint16_t oldest;           // the oldest version this build reads (through the chain or natively)
  uint16_t native;           // the oldest version the reader reads as it is (sqp: 1; else = current)
  Sniff sniff;
  Read read;
  const Upgrade* chain;      // oldest .. native-1 (up to the version the reader reads; empty in F1)
  int nchain;
  uint16_t (*min_reader_of)(const Bytes& current_bytes);   // the lowest version that reads this content right
  const char* where;         // the path rule (path()), for the docs and the tests
  uint32_t max_bytes;        // a file bigger than this is refused (kUnreadable)
};
int count();
const Format* at(int i);
const Format* find(const char* name);

enum Status : uint8_t {
  kOk = 0,         // the current version (or one the reader reads natively): out holds the bytes as they are
  kUpgraded,       // an older version upgraded in memory to current: out holds the upgraded bytes (written at the next
                   // normal save, never by load)
  kForeignNewer,   // newer, but min_reader <= current: read as it is, never written back (spec 3.3 rule 3)
  kNewer,          // newer than this build reads: not loaded, locked
  kUnreadable,     // damaged, cut short, too big, not this format: not loaded, locked (spec 3.3 rule 4)
};
const char* status_name(int s);   // "ok" "upgraded" "foreign-newer" "newer" "unreadable"
struct Info {
  uint8_t st;
  uint16_t version, min_reader;
  bool locked;      // kNewer / kUnreadable: no save may write this file
  bool read_only;   // kForeignNewer (and locked): edits in RAM only
  char why[120];
};
// Sniff, refuse (newer with min_reader > current, damaged), upgrade in memory up to current, read. out: kOk /
// kForeignNewer copy the bytes, kUpgraded the upgraded bytes (out->cap too small: kUnreadable, "too big"). out may be
// null when the caller only wants the verdict. c may be null.
Status load(const Format& f, const Bytes& in, Buf* out, Info* info, Ctx* c = nullptr);
// Rule 2's line: "<file>: made by a newer Sidecar (format <name> v<N>); this Sidecar reads up to v<M>. Not loaded, not
// changed." (kUnreadable: "<file>: not a <name> file this Sidecar can read (<why>). Not loaded, not changed.")
int refusal(const Format& f, const Info& i, const char* file, char* out, int cap);

// ---- header conventions (spec 3.2)
// text: first line "#sidecar <name> <version> [min=<n>]"; false: no such line (an unversioned v1 file)
bool text_header(const Bytes& in, const char* name, Sniffed* out);
int put_text_header(char* out, int cap, const char* name, uint16_t version, uint16_t min_reader);
// key=value: first line "format=<name>/<version>"
bool kv_header(const Bytes& in, const char* name, Sniffed* out);
int put_kv_header(char* out, int cap, const char* name, uint16_t version);
// JSON: "format":"sidecar-<name>" and "version":N (optionally "min_reader":n) among the top object's first keys
bool json_header(const Bytes& in, const char* name, Sniffed* out);
// binary (new): magic[4] u16 version u16 min_reader u32 header_bytes u32 crc32(body), little-endian; the body starts at
// header_bytes (>= 16: a later version may grow the header)
constexpr uint32_t kBinHeader = 16;
bool bin_header(const Bytes& in, const char magic[4], Sniffed* out, uint32_t* body_crc);
void put_bin_header(uint8_t* out, const char magic[4], uint16_t version, uint16_t min_reader, uint32_t header_bytes,
                    uint32_t body_crc);

// ---- class S: the key=value lines a settings reader does not know, carried to the next write (spec 3.4)
struct KvKeep {
  char text[1024];   // whole lines, each with its '\n'
  uint32_t n;
};
// known: the keys this build reads (null-terminated list, "mode" for "mode=..."); '#' lines, blank lines and the
// format= header are not kept. Lines past 1 KB in all are dropped (counted in the return: the lines kept).
int kv_keep(const char* text, uint32_t len, const char* const* known, KvKeep* out);
// appends the kept lines to out[0..n) (cap the buffer's size): the new length (what does not fit is left out)
uint32_t kv_append(const KvKeep& k, char* out, uint32_t n, uint32_t cap);

// ---- paths (spec 4.2): generation 1 = today's places. key: core::project_key's; kit: lfo / ins a kit 0..127 (-1 the
// project's file, 128 its folder), sqp a pattern 0..127, bankpool the machine (0 GRANULAR, 1 WAVETABLE, 2
// MULTISAMPLE). Global formats ignore key and kit. Returns out, or null (unknown format, no key for a per-project one,
// too long).
const char* path(const char* format, const char* key, int kit, char* out, int cap);

// ---- atomic writes (spec 4.4), the card core's: <path>.tmp written, read back and compared, <path> -> <path>.prev,
// .tmp -> <path>, .prev removed. A cut at any point leaves <path> old or new: recover() at boot puts .prev back when
// <path> is missing and removes a stray .tmp.
bool write_atomic(const char* path, const void* data, uint32_t len);
void recover(const char* path);
// The boot sweep (2026-10-26): recover() for every <path>.tmp / <path>.prev under root (folders to a depth of 8).
// Some writers' readers never call recover() - a CHOP slice map is found by a scan for ".chop.sfz", the sampler's .sfz,
// the pool files - so a cut inside write_atomic's rename window left only the .prev and the file looked lost.
// sweep_boot(): /.rytm, /CHOP and /Samples, the trees the atomic writers use (formats.cpp's paths). The card core's, at
// boot before anything indexes the card. Returns the files it changed something for (0: a clean card). CORE_MSAMP
// builds (the Pi, the Mac core): a folder walk needs File::nextEntry.
int sweep(const char* root);
int sweep_boot();
// The same in pieces (a big file: the library's index.bin), one call of step() at most kPiece bytes of card I/O:
// begin(); then step() until it returns false; ok() says whether it is in place. The caller keeps data valid meanwhile.
class AtomicWriter {
 public:
  static constexpr uint32_t kPiece = 65536;
  void begin(const char* path, const uint8_t* data, uint32_t len, uint32_t piece = kPiece);
  bool step();            // true: more to do
  bool ok() const { return st_ == kDoneOk; }
  bool busy() const { return st_ != kIdle && st_ != kDoneOk && st_ != kDoneFail; }
  void cancel();          // its .tmp removed; the old file untouched
  uint32_t bytes_io() const { return io_; }   // the last step's card bytes

 private:
  enum { kIdle, kOpen, kWrite, kClose, kBack, kSwap, kDoneOk, kDoneFail };
  int st_ = kIdle;
  char path_[200];
  const uint8_t* data_ = nullptr;
  uint32_t len_ = 0, off_ = 0, piece_ = kPiece, io_ = 0;
  File f_;
};

// ---- the posted settings write (0.3.0 power-cut safety, spec 4.4 + fix1049): the small per-project and per-Sidecar
// files the control core saves (LFO, INSERT, keys.txt, fxin.txt, sendfx.txt, /.rytm/project) go to the card core
// through cardq's kAtomicReq, one at a time. Why posted and not write_atomic in place: core 3 runs idle_save() in the
// same loop that renders audio blocks and its OUT FIFO holds 2 ms, while on the card model the plain in-place write
// this replaced already cost ~11 ms of card time there and the atomic one would cost ~24 ms (test_powercut prints
// both). Posting takes all of it off core 3 (as seq's saves do, fix1049), so the safe write is also the quiet one.
//
//   char* t = post_begin();      null: one is in flight - the caller stays dirty and tries again
//   ... format at most kPostMax bytes into t ...
//   return post_end(path, len);  posted (without a card core: written now)
//
// The card core reads that buffer while the write is in flight, so post_begin() returning null is what keeps a second
// writer from overwriting bytes the card is still reading. Never hold the pointer across a post_end.
//
// post_step() is called once at the top of idle_save(), before any writer, so only one owns the buffer at a time; a
// failed write is logged there. post_wait(path) lets a pending write of that file land before a load reads it (as
// seq's cw_wait_file); post_wait() with no path waits for whatever is pending, which is what a project or kit change
// needs before its own save, so that save is not deferred past the load that clears the dirty flag. A wait may run
// the write on the calling core (cardq::take_back), so it is for a caller that waits on purpose (the rename's FLUSH;
// the loads without a RAM store), never for a knob turn, a save, a kit change or a pattern switch.
// post_step never takes a write back (phase 3, 2026-10-26): one the card core has not started after cardq::kTakeBackUs
// stays posted - the control core never writes the card itself - and is counted in post_held() (logged once).
//
// post_queue(path, text, len) (2026-10-26, the RAM store): a kit change's save without that wait - posted now when the
// slot is free, else a copy queued behind it, never a wait (oldest first, one entry per path - a newer text replaces
// it and goes to the tail, after every write queued before it - so bounded by the settings files, not by kit changes;
// post_step posts the next when the slot frees; post_begin
// is null while any wait, so a newer save of a queued file never lands before it). post_busy() counts the queue,
// post_wait(path) writes the queue in its order up to that path's write. False for a bad path or length, or out of
// memory (logged: the edit then stays in RAM only). An edit may now sit in RAM behind several queued writes before it
// reaches the card (the power-cut window): small next to the 1 s a save waits for the edits to settle.
// A text over kPostMax (up to kQueueMax: the sequencer's pattern files, 2026-10-26 phase 2) is always queued as its own
// copy - posted at once when the slot and the queue are free - and the slot writes from that copy (CORE_MSAMP only;
// elsewhere refused).
constexpr uint32_t kPostMax = 6144;
constexpr uint32_t kQueueMax = 1u << 20;
char* post_begin();
bool post_end(const char* path, uint32_t len);
// id (optional): this write's own number for post_result (0 when refused)
bool post_queue(const char* path, const char* text, uint32_t len, uint32_t* id = nullptr);
// a file removed through the same queue, in its turn (a queued write of it replaced; CORE_MSAMP only): post_result's
// ok when it is gone (or was not there)
bool post_remove(const char* path, uint32_t* id = nullptr);
bool post_step();   // true while a posted write waits for the card core
bool post_busy();
uint32_t post_held();   // posted writes the card core had not taken after cardq::kTakeBackUs (kept posted, never run here)
// fix round 1: the slot's write is held now (posted past kTakeBackUs). idle_save then goes on to the savers and the
// sequencer below it rather than return at post_step (that return would stop them for as long as the card core is
// held), and the LFO / INSERT idle saves queue (post_queue) instead of waiting for the slot
bool post_stalled();
void post_wait(const char* path = nullptr);
// 2026-10-26, the RAM store: a write of this file not yet on the card (queued, or posted and not finished); never waits.
// The store's fill reads no file while it is pending (it would hold the bytes the write is about to replace).
bool pending(const char* path);
// 2026-10-26: how many writes (queued, and the slot's once the card core has it) are not yet on the card - one still in cardq's ring is cardq::pending()'s, not counted here - and in *oldest_ms the age
// of the oldest of them (0 when none): /status "store". Diagnostics: plain reads from another core, torn at worst.
uint32_t unwritten(uint32_t* oldest_ms);
bool post_ok();     // the last finished write's result
// A write's own result (post_ok is the last finished write's, of any file): by the id post_queue gave it. Pending while
// queued or in the slot; ok / failed once done (the newest kPostResults finished writes are kept); unknown for an id
// never given, one replaced by a newer text of its path before it was written, or one older than the kept results.
// The SONG backup (core/song) waits on its own files with it: another file's failure in the same post_wait never
// hides its own (undo-replay review 2).
enum PostResult : uint8_t { kPostPending, kPostOk, kPostFailed, kPostUnknown };
constexpr int kPostResults = 64;
PostResult post_result(uint32_t id);

void reset_for_tests();

// A text file's lines through a read-ahead buffer: one File::read per sizeof buf bytes, not one per byte. On the Pi a
// one-byte read is a whole f_read, each taking FatFs's volume lock, which waits behind the card core's work: the per-kit
// LFO and insert files read a byte at a time took core 3 40 ms at every kit change [measured 2026-10-26: the Rytm's
// 3B, 40.2-40.6 ms, four switches]. line(): the next line without its \r and \n (cut at max - 1 characters), its
// length; -1 at the end (a last line without \n is still a line; a read error ends the file, as the byte reader did).
// The length, not just "there was a line": a line whose first byte is a NUL is not a blank one, and a loader must not
// take it for one (0.3.0: it would leave a damaged file unlocked for the next save to write over).
struct LineReader {
  explicit LineReader(File& file) : f(file) {}
  int line(char* out, int max);
  File& f;
  char buf[256];
  int n = 0, i = 0;
  bool end = false;
};
// The same lines from bytes already in RAM (2026-10-26: the RAM store, core/store/store.h, holds the per-kit LFO and
// insert files): the next line of d[0..n) from *at, by LineReader's rules exactly (\r dropped, cut at max - 1, a last
// line without \n still a line, its length so a NUL-led line is not a blank one); *at moves past its \n. -1 at the end.
int next_line(const uint8_t* d, uint32_t n, uint32_t* at, char* out, int max);

}  // namespace fmt
}  // namespace core
