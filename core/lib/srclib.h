// core/lib/srclib.h - the source BROWSER of GRANULAR (SRC), WAVETABLE (TBL) and MULTISAMPLE (INST), and the machines'
// list entries composed from their POOL lists (core/lib/pools.h: SAMPLES, WAVETABLES, INSTRUMENTS). Pi and Mac only
// (CORE_MSAMP; the code helpers are everywhere).
//
//   browser  a browser per machine over the card's folders (the Rytm's FUNC + YES file list, the touchscreen's FILES,
//            a fill's listing): the root lists the machine's virtual folders - GRANULAR @INPUTS/ (IN MAIN BD ... CY-CB)
//            and @BUILT-IN/ (PAD BELLS), WAVETABLE @BUILT-IN/ (BASIC ... SYNC) - then the card's own folders (the
//            Sidecar's system folders left out), each folder its subfolders and the files the machine reads (GRANULAR /
//            WAVETABLE: .wav .aif .aiff; MULTISAMPLE: .sfz), sorted (case ignored), at most kListing a folder. From
//            the library's RAM index (lib::list, core/lib/library.h; Task 6b of the RAM store): no card I/O on the
//            calling core (the Pi's core 3) - a folder is listed when a browser enters it, one whole listing a step(),
//            listed again when lib::gen() moves (the library saw the card change); until the library's boot load has a
//            snapshot the folder shows as being read. A folder is one the library knows: holding samples, or seen by its
//            walk (an empty folder from the walk's end on). kCache listings are kept. Folders are places to browse
//            only: a file plays once it is in a pool row.
//   entries  compose(list, out): the machine's list, one entry a code (bank * 128 + row: the Rytm's 14-bit word),
//            kEntries long - row 0 of each bank OFF, an empty row a hole, a file its resolved path (MISSING: silent),
//            an input / built-in its fixed source.
#pragma once
#include <stdint.h>

namespace core {
namespace srclib {

enum Kind : int { kGr = 0, kWt = 1, kMs = 2, kKinds = 3 };   // = pools::kSamples, kTables, kInstr
constexpr int kFill = 3;            // a fourth browser of audio files, the pool lists' fill's own (no virtual folders,
                                    // no folder check on cd: nothing opened on the calling core; a user's never moved)
constexpr int kPath = 160;
constexpr int kName = 40;
constexpr int kBanks = 8;           // rui1038: banks A..H
constexpr int kBankRows = 128;      // rows a bank (row 0 OFF)
constexpr int kListA = 128;         // bank A's rows
constexpr int kEntries = kBanks * kBankRows;   // a machine's list: entry = code
constexpr int kListing = 1024;      // a folder's entries at most (folders and files)
constexpr int kCache = 6;           // folder listings kept
constexpr int kListPer = 2048;      // the library's folder entries one step looks at (a step within ~1 ms on the Pi)

// a list entry's role (Entry::lib)
enum Lib : uint8_t { kLibNone = 0, kLibOff, kLibHole, kLibMissing };

// rui1038: P1 (the Rytm's value: integer + fraction / 128, as decode_params gives it) -> the source code bank * 128 +
// index; from 0021's 14-bit hi << 7 | lo; and back to the 48 / 2A bytes (index, lo = bank << 4)
inline int code_of(float p1) {
  if (!(p1 > 0)) return 0;
  int i = (int)p1;
  if (i > 127) return 127;
  int lo = (int)((p1 - (float)i) * 128.0f + 0.5f);
  return (lo > 127 ? 7 : lo >> 4) * kBankRows + i;
}
inline int code_of14(int v14) { return ((v14 & 0x7f) >> 4) * kBankRows + ((v14 >> 7) & 127); }
inline int bank_of(int code) { return (code >> 7) & 7; }
inline int index_of(int code) { return code & 127; }
inline int lo_of(int code) { return bank_of(code) << 4; }

#if CORE_MSAMP
// A machine list's entry (GRANULAR, WAVETABLE and MULTISAMPLE use it): entry e = code e
struct Entry {
  char path[kPath];   // the resolved file ("" an input / built-in / OFF / empty / missing)
  char name[kName];   // the pool row's name ("OFF", "---" empty)
  char grp[24];       // "BANK A" .. "BANK H"; "LIVE" an input, "BUILT-IN" a built-in
  uint8_t lib;        // Lib
  int8_t fixed;       // an input's / built-in's index (the machine's fixed source), -1 none
  uint8_t type;       // pools::Type
  uint8_t st;         // lib::State (files)
  uint32_t size;      // the ref's size (0 unknown)
};
inline bool playable(const Entry& e) { return e.lib == kLibNone; }
inline bool silent(const Entry* list, int e) { return e >= 0 && e < kEntries && !playable(list[e]); }
// The machine's list from its pool list (kind k = the pool list's number): kEntries entries into out
int compose(int k, Entry* out);
uint32_t pool_sig(int k);            // the pool list's sig (compose again when it moves)

bool begun();
void begin(bool card);               // boot (nm::begin): the browsers at the root
bool step();                         // one bounded job (a folder listed from the library's RAM); true: did one
void changed(const char* path);      // the card changed under path (the web's upload, rename, delete): listed again
                                     // (from the library's snapshot: the change once the library has it, its gen())
const char* machine_dir(int k);      // "granular" "wavetable" "multisample"
bool hidden_name(const char* name);  // a card root folder the library leaves out (Plugins, PRESETS, Bundles, ...)
bool accepts(int k, const char* name);   // a file machine k lists (by its extension)

// The browser of machine k (its folder "" is the root; "@INPUTS" / "@BUILT-IN" its virtual folders)
const char* cwd(int k);
bool cd(int k, const char* path);    // a folder ("" the root); false: not a folder
// a browser shown where it is (the pool page's REPLACE): its folder listed - attached if nothing was since boot, the
// root if its folder went (control core)
void open(int k);
void up(int k);
bool listing_done(int k);            // the folder is listed whole (else still being read)
int count(int k);                    // its entries (virtual folders, folders, then files)
bool item(int k, int i, const char** name, bool* dir, uint32_t* size);   // racy on other cores (drawing only)
bool item_path(int k, int i, char* out, int cap);   // "/folder/file.wav", or "@INPUTS/IN" / "@BUILT-IN/PAD"
uint32_t view_gen(int k);            // changes with the browser's folder and listing (any core)
// a virtual item's path -> its pools type (pools::kInput / kBuiltin) and index; false: a card path
bool special(int k, const char* path, int* type, int* id);

// "browse=gr|wt|ms:cd:PATH|up|root|open:I", "browse=changed:PATH" (and the older "lib=changed:PATH")
bool command(const char* p, char* out, int cap);
struct Stats {
  uint32_t dirs_read, entries_read, steps, evicted;
};
const Stats& stats();
void reset_for_tests();              // every browser at the root, the cache empty
#endif

}  // namespace srclib
}  // namespace core
