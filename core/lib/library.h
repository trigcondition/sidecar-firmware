// core/lib/library.h - the content-addressed sample library (docs/LIBRARY.md; the design:
// docs/superpowers/specs/2026-10-22-formats-and-library-design.md section 6 and 9): every sample and instrument on the
// card known by the sha256 of its bytes, the path a hint. Pools, instruments, takes and bundles refer to samples by
// hash (lib::Ref); resolve() finds where that content is now.
//
// On the card, /.rytm/library/ (core/fmt: formats "library", "library-log", "library-state"):
//   index.bin   SLIB v1: 64-byte records sorted by sha256 (unhashed ones after), then the path table; CRC-32 of the body
//   log.txt     "#sidecar library-log 1", then the changes since index.bin: "+ <sha|-> <size> <fatdt> <crc> <quick>
//               <flags> <path>", "- <path>", "> <old> <new>" (a move: the record keeps its hash). Compacted into a new
//               index.bin (fmt::AtomicWriter, in pieces) at kCompactLines lines and after the boot walk.
//   state.txt   "format=library-state/1", tree=<the walk's fingerprint>, files=, bytes=, unhashed=, gone=, dups=
//
// Cores. Everything that touches the card runs on the card core (core 0 on the Pi): step() (the boot load, the walk,
// the hashing, the log and the compaction, one bounded piece a call), touched(), moved(), command(). The RAM index is
// published to every core as a snapshot: resolve(), ref_for_path(), find(), gen(), stats() read it without a lock and
// without touching the card (safe on core 3, the control core: a reader marks the snapshot it reads; core 0 never
// rebuilds a snapshot a reader holds, it waits a later step). hash_now() may be called from any core (a lock-free ring).
//
// The scheduler (step) keeps the bundle job's rules: nothing while the card queue has work or a stream is low
// (Env::card_busy); while the Rytm plays or a take records: reads of kSlowPiece every kSlowGapUs, no card writes (the
// log waits in RAM); while the Overbridge recorder records: nothing; while a bundle job runs: nothing (the bundle
// first). Never on core 3. Only the card: a path on a removable volume ("USB:/...") is not indexed.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core {
namespace lib {

constexpr int kPath = 160;                 // a hint's longest path + 1 (pool.h's kPath)
constexpr uint32_t kPiece = 65536;         // a step's card I/O at most (cardq::kPiece)
constexpr uint32_t kSlowPiece = 16384;     // while the Rytm plays
constexpr uint32_t kSlowGapUs = 2000;
constexpr int kCompactLines = 2000;
constexpr uint32_t kMaxFiles = 400000;     // records at most (the RAM: ~64 B + the path a file, twice: two snapshots)

// ---- references (spec 6.6): "<sha256 64 hex>|- <size> <path hint>"
struct Ref {
  uint8_t sha[32];
  bool has_sha;
  uint64_t size;
  char hint[kPath];
};
bool parse_ref(const char* line, Ref* out);   // the text form (the hint runs to the line's end, a tab or CR / LF)
int format_ref(const Ref& r, char* out, int cap);   // its length, 0 when it does not fit

enum State : uint8_t { kPending = 0, kExact, kMoved, kUnverified, kChanged, kMissing };
const char* state_name(int s);   // "PENDING" "EXACT" "MOVED" "UNVERIFIED" "CHANGED" "MISSING"
struct Resolved {
  State st;
  char path[kPath];   // where to play it from ("" for kMissing): a copy (valid after gen() moves on)
  uint32_t rec;       // the record's index in the snapshot of this gen() (~0u none)
};
// RAM only, lock-free, safe on core 3 (spec 6.5): EXACT (the hint has that sha), MOVED (the sha elsewhere: the copy in
// the hint's folder, else the nearest path, else the first by name), UNVERIFIED (the sha unknown to the index while
// hashing is not done, the hint there with that size), CHANGED (the hint there with other content), MISSING; PENDING
// while the index is not loaded yet or its walk not done and the hint not found (played by path, as before).
// A ref without a sha resolves by its path: EXACT when hashed, UNVERIFIED while not, MISSING when not on the card.
State resolve(const Ref& r, Resolved* out);
bool ref_for_path(const char* path, Ref* out);   // a pick: its sha if known (has_sha false while not), its size
uint32_t gen();                                  // bumped on every published change: poll, then resolve again
// core 0: a writer's finished file. sha null: the file changed (its hash done later; a missing path: gone); crc only
// with a sha (0: not known)
void touched(const char* path, const uint8_t* sha_or_null, uint32_t crc_or_0);
void moved(const char* from, const char* to);    // core 0: a rename (a file or a folder): the hashes move with it
void hash_now(const char* path);                 // any core: hashed before the rest (a pick in a browser)
// The paths holding this content (hashed records), at most max. Core 0 only: the pointers stay valid until the
// library's next step() (another core: find_file, which copies). Returns how many.
int find(const uint8_t* sha, const char** paths, int max);
// The same with its size checked, one path copied (the bundle import's dedupe): with near, the copy resolve() would
// pick for that hint (its folder, else the nearest path), else the first by name
bool find_file(const uint8_t* sha, uint64_t size, char* path, int cap, const char* near = nullptr);
// A card file's sha256 and CRC-32 as the library knows them (the bundle export: no hash pass for an indexed file, the
// formats spec's L4): true when its record is hashed, on the card, readable, of that size and its CRC known. Any core
// (the snapshot); the caller checks the bytes again where it can (the export's WRITE does)
bool hashes_of(const char* path, uint64_t size, uint8_t sha[32], uint32_t* crc);

// ---- the folders (the sample browser's view, core/lib/srclib): the snapshot keeps its records' and the walk's folders
// in path order (case ignored), built by core 0 at each publish. RAM only, lock-free, any core (core 3: no card I/O, no
// allocation; O((entries + subfolders) log n), in pieces of a budget when the caller wants).
// One folder ("" the root, else "/A/B") as the snapshot knows it: its subfolders (once each: a folder the walk saw, or
// one holding records) and its files (the library's types: .wav .aif .aiff .sfz; the walk's scope: no dot names, not
// the Sidecar's root folders), fn called for each in path order (case ignored; ' ' and '.' sort before '/': the
// caller sorts as it shows them), name a NUL-terminated copy valid during the call, size 0 for a folder; fn returns
// false to stop. Returns the entries passed; -1 while there is no whole snapshot (the boot load not done: "being
// read"). In pieces: pos (at 0 the start) and budget (the entries one call looks at, 0 all): pos->done when the folder
// is through, else the next call goes on from pos->at; -2 when the snapshot changed since (pos->gen): from the start.
// pos->gen: the gen() of the snapshot listed. Measured: a folder of 1 000 files + one of 20 folders ~35 us on the Mac
// (20 000 records; the Pi ~3-5x).
typedef bool (*ListFn)(void* ctx, const char* name, bool dir, uint64_t size);
struct ListPos {
  uint32_t gen, at;
  bool done;
};
int list(const char* dir, ListFn fn, void* ctx, ListPos* pos = nullptr, uint32_t budget = 0);
int is_folder(const char* dir);   // 1 a folder of the snapshot ("" always), 0 not, -1 no whole snapshot yet

enum Phase : uint8_t { kOff = 0, kLoading, kWalking, kHashing, kReady, kFailed };
const char* phase_name(int p);   // "OFF" "LOADING" "WALKING" "HASHING" "READY" "FAILED"
struct Stats {
  uint32_t files, unhashed, missing_refs, dups;   // missing_refs: references found missing (0 until pools use Refs)
  uint64_t bytes;
  uint8_t state, pct;                             // state: Phase; pct: the share of bytes hashed
  // (beyond the spec's 9: the status lines)
  uint32_t hashed, gone, unreadable, recordings;  // gone: on the card at the last walk, not now; recordings: indexed only
  uint64_t hashed_bytes, dup_bytes;
  uint32_t walked;                                // entries the walk has seen (WALKING's progress)
  uint32_t log_lines, compactions, pending_log;   // the log on the card, compactions, lines waiting for a write
  char waiting[16];                               // "PLAYING" ... while held back ("" running)
  uint32_t seq;                                   // bumped with every change of these
};
Stats stats();   // a consistent copy (any core)

// ---- the scheduler's world (the board's; all null: full rate always, nothing else runs)
struct IoRule {
  uint32_t read_piece, write_piece, read_gap_us;   // 0: not allowed now
  const char* why;                                 // "PLAYING" "SONG REC" "RECORDING" while held back
};
struct Env {
  void (*rules)(IoRule* r);
  bool (*card_busy)();     // cardq::busy() on the Pi
  bool (*other_job)();     // a bundle job runs (the bundle first)
  int (*current_refs)(const char** paths, int max);   // the current project's referenced paths (hashed first)
};
void set_env(const Env& e);

void begin(bool card_ok);   // core 0, at boot: the load and the walk start at the next steps (no card I/O here)
bool step();                // core 0: one bounded piece; true: did something
bool busy();                // walking or hashing or writing
void rescan();              // a full walk again (lib=rescan)
void refs_changed();        // the current project changed: its references (Env::current_refs) hashed first again
void verify();              // every record hashed again; differences reported (lib=verify)
// "lib=status|rescan|verify|bench|find:<hex>|hash:<path>" (core 0); false: not a library command (srclib's lib=...)
bool command(const char* line, char* out, int cap);
int status_json(char* out, int cap);   // ",\"library\":{...}" for /status
// lib=bench's figures (also in the log): sha256 MB/s from RAM, the card's read MB/s, both x100 (0: not run)
struct Bench {
  uint32_t sha_mbs100, read_mbs100, crc_mbs100;
  bool running;
};
Bench bench();

// The library's own checks of an index.bin image (the format registry's reader): null good, else why
const char* index_check(const uint8_t* p, uint32_t n);

// Step accounting (the stall tests): steps run / held back, the most card bytes one step read / wrote, its longest
// CPU time, files hashed, the bytes hashed on the card (a writer's hash given to touched() reads nothing)
struct Work {
  uint32_t steps, waits, max_read, max_write, max_us, files_hashed, publishes;
  uint32_t folder_sorts;   // publishes that sorted the folder view again (the paths changed)
  uint64_t read, written;
};
const Work& work();
void reset_for_tests();

}  // namespace lib
}  // namespace core
