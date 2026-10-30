// core/lib/pools.h - the project's three POOL lists (docs/POOLS.md; the design:
// docs/superpowers/specs/2026-10-22-pools-tonverk-design.md): SAMPLES (GRANULAR's SRC, DSP SAMPLE's SMP on every bank (rui1045), the
// sampler's takes), WAVETABLES (WAVETABLE's TBL) and INSTRUMENTS (MULTISAMPLE's INST). Each is 8 banks x 128 rows, the
// address code = bank * 128 + row (srclib::code_of: the Rytm's 14-bit word, unchanged on the wire); row 0 of every bank
// is OFF. Rows never move: loading never renumbers, unloading leaves a hole (a lock on it plays silence).
//
//   entries  a sample (a lib::Ref to a WAV / AIFF), an input (GRANULAR's live IN MAIN BD ... CY-CB: samples list only),
//            a built-in (PAD / BELLS in samples, BASIC PWM VOCAL ORGAN FM SYNC in wavetables), an instrument (a Ref to the
//            .sfz plus one Ref per member sample, in the .sfz's order: members take no rows). A list holds one entry per
//            hash (per .sfz hash for instruments): loading a file already in it gives its existing row.
//   files    /.rytm/projects/<KEY>/samples.pool.txt, wavetables.pool.txt, instruments.pool.txt (formats samplepool,
//            wtpool, instpool v1: core/lib/pools_fmt.h), read with fmt::load and written with fmt::write_atomic on the
//            card core (cardq kOpenReq / kReadReq / kAtomicReq: the control core never touches the card for them). A
//            project without them opens with the template (inputs + PAD / BELLS in samples H001..H012, the six
//            built-in tables in wavetables A001..A006); the old per-machine pool files are not read (no migration).
//            A newer or damaged file locks its list (never written); a foreign-newer one is read, edits stay in RAM.
//   used     /.rytm/projects/<KEY>/pool-used.txt (format poolused v1, class D): the codes seen playing in this project
//            (the machines' reports, 48 commits, the Sidecar's own patterns and songs), written a second after a change.
//            SELECT UNUSED is "not seen playing" (a Rytm lock that has not played yet is not known: the warning says so).
//   resolve  every entry's Ref resolved through the library (lib::resolve: RAM only, lock-free) when lib::gen() moves;
//            Entry::path is where it plays from, Entry::st its state (MISSING: silent). A ref without a hash yet ("-")
//            gets it when the library has hashed the file, and the next save writes it.
//
// Cores: begin / step / the edits / seen are the control core's (core 3 on the Pi); at(), sig() and refs() may be read
// from any core (racy for drawing; refs() copies each ref under the list's sequence counter). Pi and Mac (CORE_MSAMP).
#pragma once
#include <stdint.h>

#include "library.h"

namespace core {
namespace pools {

enum List : int { kSamples = 0, kTables = 1, kInstr = 2, kLists = 3 };   // = the Rytm's 47 list numbers (0 1 2)
constexpr int kBanks = 8, kRows = 128, kCodes = kBanks * kRows;
constexpr int kName = 40;                 // a display name's 39 characters
constexpr int kMembers = 16384;           // instrument members, the instruments list in all
constexpr int kMembersPer = 1024;         // one instrument's at most
constexpr uint32_t kFileMax = 4u << 20;   // a list file's bytes at most
constexpr int kInputs = 10;               // IN MAIN BD SD RS-CP BT LT MT-HT CH-OH CY-CB
constexpr int kSmpBuiltins = 2;           // PAD BELLS
constexpr int kWtBuiltins = 6;            // BASIC PWM VOCAL ORGAN FM SYNC

enum Type : uint8_t { kEmpty = 0, kOff, kSample, kInput, kBuiltin, kInstrument };

inline int code(int bank, int row) { return (bank & 7) * kRows + (row & 127); }
inline int bank_of(int c) { return (c >> 7) & 7; }
inline int row_of(int c) { return c & 127; }
// "A010" (OFF rows "A000"); out holds 5 bytes
void row_text(int code, char* out);
// "A010" / "a10" / "H127" -> the code; false: not a row (bank A..H, row 0..127)
bool parse_row(const char* s, int* code, const char** end = nullptr);

const char* list_name(int l);    // "SAMPLES" "WAVETABLES" "INSTRUMENTS"
const char* list_tag(int l);     // "smp" "wt" "inst" (the console's, the used file's)
const char* format_of(int l);    // "samplepool" "wtpool" "instpool"
int list_of_tag(const char* t);  // -1 none
const char* input_name(int i);
const char* builtin_name(int list, int i);   // "" none
int builtins(int list);                      // samples 2, wavetables 6, instruments 0
int builtin_of(int list, const char* name);  // -1 none
int input_of(const char* name);              // -1 none
bool takes_type(int list, int type);         // which entry types a list holds
bool accepts_file(int list, const char* path);   // by extension: samples / wavetables .wav .aif .aiff, instruments .sfz

struct Member {
  lib::Ref ref;
  uint8_t st;   // lib::State, resolved
};
struct Entry {
  uint8_t type;           // Type
  uint8_t id;             // an input's / built-in's index
  uint8_t st;             // lib::State of its ref (an instrument: the worst of the .sfz and its members); kExact else
  uint8_t ch;             // channels (0 unknown: the cached extras)
  uint8_t ram;            // "always in RAM" (docs/STEREO.md: held whole, never streamed; kept in pool-used.txt)
  uint16_t m0, nm;        // an instrument's members: [m0, m0 + nm) of the list's member table
  uint32_t frames, rate;  // the cached extras (0 unknown)
  char name[kName];
  lib::Ref ref;           // a sample / an instrument's .sfz
  char path[lib::kPath];  // where it plays from: lib::resolve's path ("" no file, or missing)
};
inline bool has_file(const Entry& e) { return e.type == kSample || e.type == kInstrument; }
// it sounds: a sample / instrument not missing, an input, a built-in
bool playable(const Entry& e);

// ---- lifecycle (the control core)
void begin(bool card);      // boot: the current project's lists at the next steps (the card core reads them)
bool step();                // one bounded job (a read or write posted / taken back, a re-resolve); true: did one
bool ready();               // the project's lists are in (read, or the template)
bool saving();              // a write waits or runs
// 2026-10-27 (core/bundle/prjslot.h): the lists in memory are to's from now on when they were from's (a SAVE AS copy
// placed, the current project moved): their unsaved rows saved under to. While a slot job moves or copies files
// (prjslot::moving) no list is saved (a write landing in a folder being copied).
void rekey(const char* from, const char* to);
const char* project();      // the key the lists are of ("" before the first)
uint32_t sig(int list);     // changes with every change of the list (an entry, a state, the lock)
const Entry& at(int list, int code);   // any core (racy for drawing)
const Member* members(int list, int code, int* n);
bool locked(int list);      // refused (newer / damaged): never written, no edits
bool read_only(int list);   // foreign-newer: read, edits in RAM only
const char* lock_why(int list);
int count(int list);        // entries that are not empty / OFF
int first_free(int list, int bank);   // its first empty row's code (-1 full)
int find_hash(int list, const lib::Ref& r);   // the row holding that content (or that path, without a hash), -1 none

// ---- edits (the control core): codes are bank * 128 + row
enum Result : int {
  kOk = 0,         // done (*out the row)
  kExisting,       // the file is in the list already: *out its row, nothing changed
  kPending,        // an instrument: its .sfz is read on the card core first (set when done; pending() says)
  kErrOff,         // "000 IS OFF"
  kErrBuiltin,     // the row holds an input / built-in: unload it first ("BUILT-IN")
  kErrType,        // not a file / type this list takes
  kErrFull,        // the bank has no empty row
  kErrLocked,      // the list's file could not be read: nothing is changed
  kErrPath,        // no such file (or too long a path)
  kErrRow,         // not a row
  kErrUsed,        // P3: the row is in use (a voice is on it now, or it was seen playing): ask first (confirm)
  kErrPlaying,     // P3: the Rytm plays: a row in use is not replaced (FUNC held forces it)
  kErrBusy,        // an instrument's .sfz is still being read
  kErrEmpty,       // nothing to unload
};
const char* result_text(int r);   // the short text the Rytm / touchscreen show ("000 IS OFF", "BUILT-IN", ...)
// How a replace of a row in use is confirmed (P3)
enum Confirm : int { kAsk = 0, kConfirmed = 1, kForced = 2 };
// a file into a row (REPLACE when the row holds one): one entry per hash (a file already in another row: kExisting)
int set_file(int list, int code, const char* path, int* out, int confirm = kAsk);
// a file into the bank's first free row (a take, the web's +SMP / +WT / +INST): kExisting when it is there already
int add_file(int list, int bank, const char* path, int* out);
// the bank last shown on a view of the list (the bank list, the POOL views): where takes go (default A)
void shown(int list, int bank);
int shown_bank(int list);
// a take / an auto-sampled instrument: the first free row of the bank last shown, else of the next bank with room
int add_take(int list, const char* path, int* out);
// an input / built-in into an empty row (the browser's @INPUTS/ @BUILT-IN/ folders)
int set_special(int list, int code, int type, int id, int confirm = kAsk);
int unload(int list, int code, int confirm = kAsk);   // the row emptied (the card file and the library stay)
bool undo(int list);        // the last replace / unload of the list taken back (one level)
// a folder's files (name order) into the bank's empty rows: started now, done over the next steps (the folder's
// listing: srclib's browser); -1 refused (not a folder / locked)
int fill(int list, int bank, const char* folder);
bool filling();
bool pending();             // an instrument's .sfz read waits
int last_result(int* out);  // the last instrument's outcome once it is read (kPending while it waits; *out its row)
void note_info(int list, int code, uint32_t frames, uint8_t ch, uint32_t rate);   // a loader's cached extras
// "Always in RAM" (docs/STEREO.md): a row (code) or a whole list (code -1) held whole in the pool RAM, never streamed
// (MULTISAMPLE plays it in RAM mode whatever the streaming budget says). Kept in pool-used.txt ("ram smp A001",
// "ram inst *"); the console's pool=K:ram:ROW|all:on|off.
void set_ram(int list, int code, bool on);
bool always_ram(int list, int code);         // the row's flag or its list's
bool ram_all(int list);
void accept_changed(int list, int code);   // a CHANGED entry: its ref takes the file's new content (one tap)

// ---- the used set and P3
void seen(int list, int code);             // a report / commit / the Sidecar's pattern played this code
bool used(int list, int code);             // seen in this project
uint32_t used_count(int list, int code);
void voice_on(int list, int vi, int code); // the code voice vi's machine plays now (-1 none): "USED BY 3 SOUNDS"
int users(int list, int code);             // voices on it now
bool in_use(int list, int code);           // users or used
void set_playing_fn(bool (*fn)());         // the transport (the Rytm plays): P3 refuses then
bool playing();
// SELECT UNUSED: the rows holding a file (a sample / an instrument) not in use, into out (codes), at most max; how many
int select_unused(int list, int16_t* out, int max);
extern const char* const kUnusedWarning;   // "NOT SEEN PLAYING - A RYTM LOCK MAY STILL USE IT"

// ---- the references (the bundle collector, the library's "current project first", SELECT UNUSED): every Ref of the
// list, members included (member >= 0: its index in the instrument). Any core: each ref copied under the sequence
// counter, an instrument with its members in one window (a list changing meanwhile is read again). Returns the refs
// visited, or -1 when a row kept changing and was left out (the caller tries again, or reads the list files).
typedef void (*RefFn)(void* ctx, int list, int code, int member, const lib::Ref& r);
int refs(int list, RefFn fn, void* ctx);

// ---- the console: "pool" (stats), "pool=smp|wt|inst:set:A010:PATH", ":clear:A010", ":fill:B:FOLDER", ":unused",
// ":unload:A010[,A011..]", ":replace:A010:PATH[:yes|:force]", ":add:B:PATH", ":input:H001:IN", ":builtin:H011:PAD",
// ":list[:B]", ":undo", ":assign:A010:I[:yes]" (item I of the list's browser), ":accept:A010" (a CHANGED row), ":shown:B",
// "pool=status"
bool command(const char* p, char* out, int cap);
// the web's Pool tab: one bank of a list as JSON ({"list":..,"bank":..,"rows":[[row,type,name,state,path,used,users],..]})
int bank_json(int list, int bank, char* out, int cap);
int status_json(char* out, int cap);      // {"smp":{"n":..,"locked":..},...,"ram":{...}}

// ---- the pool RAM (the design's 4.1; the hash-keyed sample cache, core/pool/pcache.h): the budget and what the lists
// hold in it now, for the views' "MB and %" figure (any core)
struct Ram {
  uint64_t budget;       // bytes (poolram=MB; default 1 GB on 2 / 4 GB boards, 3 GB on 8 GB)
  uint64_t used;         // the shared sample cache's bytes (samples and instrument members, each hash once)
  uint64_t tables;       // WAVETABLE's tables (mip frames), counted in the same budget
  uint32_t entries;      // samples resident
  uint8_t pct;           // (used + tables) * 100 / budget
};
Ram ram();
bool resident(int list, int code);   // the row's audio is in RAM now (a sample / table / instrument)

struct Stats {
  uint32_t reads, saves, save_fails, merged, resolves, templates, fills, refused;
  uint32_t used_saves;
};
const Stats& stats();
void reset_for_tests();     // the lists empty, read again at the next step
void compact_for_tests(int list);   // the member table compacted now

}  // namespace pools
}  // namespace core
