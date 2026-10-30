// core/bundle/manifest.h - a bundle's manifest.json (docs/BUNDLES.md "Manifest"; the bundles design 3.3): what the
// bundle holds and where it came from. The first entry of every bundle; the export writes it, the import reads it
// (bounded: at most kManifestMax bytes, json.h's reader) and checks every entry against it.
//
// The project's pool lists (core/lib/pools.h: samples / wavetables / instruments.pool.txt, pool-used.txt) are opaque
// kind "pool" entries with their format; "formats" records each list format's version beside sqp / song / slots. An
// older bundle's "pools" descriptors and "bank_a" record (the per-machine pools before the pools redesign) are read
// past: its pool files are imported as they are.
#pragma once
#include <stdint.h>

#include "json.h"
#include "zip.h"

namespace core {
namespace bundle {

constexpr int kFormat = 1;          // "version": what this Sidecar writes
constexpr int kReader = 1;          // the newest "min_reader" it reads
constexpr uint32_t kManifestMax = 1u << 20;
constexpr int kCardPath = 201;      // a card path (fsweb's 200 + NUL)
constexpr int kFormats = 8;         // "formats" beyond sqp / song / slots (the pool lists') at most
constexpr int kSlots = 5, kFxSlots = 4;

enum Kind : uint8_t { kKindPool = 0, kKindProject, kKindSample, kKindSfz, kKindTake, kKindSong, kKindSeq, kKindPackage,
                      kKindRytm, kKinds };
// The bundle reader each file format version needs (the per-file format versioning seam: a newer .sqp or song file
// raises the bundle's min_reader without a new manifest format); 1 for everything this Sidecar writes today.
int reader_for(const char* fmt, int ver);
const char* kind_name(int k);       // "pool" "project" "sample" "sfz" "take" "song" "seq" "package" "rytm"
int kind_of(const char* s);         // -1 unknown

struct MEntry {
  char name[kNameMax + 1];          // in the bundle
  uint8_t kind;
  bool abs_refs;                    // an .sfz with absolute sample= / default_path= values
  uint32_t size, crc;
  uint8_t sha[32];
  int first_path, npaths;           // its card paths in Manifest::p (samples, sfz, takes)
  char src[kCardPath];              // export: where its bytes are read (the snapshot copy or the card file)
  int group;                        // export / import: the instrument (.sfz) group it belongs to, -1 none
  char fmt[16];                     // the file's format name ("sqp", "song", "slots", "pool", ...), "" not known
  int fmt_ver;                      // and its version (0 unknown)
};
struct MPath { char path[kCardPath]; int entry; };
struct FormatVer { char name[16]; int ver; };   // "samplepool" 1
struct Slot { int slot; char id[32]; char ver[12]; char name[12]; int fp; char pkg[kNameMax + 1]; char target[8]; };
struct Print { int slot, fp, n; char name[12]; };

struct Manifest {
  int version, min_reader;
  char created[24], clock[12], sc_version[32], board[8];
  char rytm_name[24], key[24];
  Print prints[kSlots];
  int nprints;
  Print fxprints[kFxSlots];
  int nfxprints;
  Slot slots[kSlots];
  int nslots;
  char sendfx_route[8], fx_in[8];
  int f_sqp, f_song, f_slots;
  FormatVer formats[kFormats];      // the pool lists' formats (and any other a newer writer names)
  int nformats;
  void add_format(const char* name, int ver);   // once a name (the highest version kept)
  MEntry* e;
  int ne, cap_e;
  MPath* p;
  int np, cap_p;
  char (*missing)[kCardPath];
  int nmissing, cap_missing;

  void alloc(int entries, int paths, int missing);   // once
  void clear();
  int add_entry(const char* name, int kind);          // -1 full
  bool add_path(int entry, const char* path);         // the entry's paths are added together (after it)
  int find_entry(const char* name) const;
  int find_path(const char* path) const;              // case ignored (FAT): the MPath index
  uint64_t bytes() const;
  int count(int kind) const;
};

// The manifest's JSON (entries in m.e order); false: no room (kManifestMax)
bool write_manifest(const Manifest& m, JOut& o);
// Untrusted text in: false and err when any field is wrong (names, sizes, hashes, nesting, a newer min_reader)
bool read_manifest(const char* text, uint32_t n, Manifest* m, char* err, int errcap);

}  // namespace bundle
}  // namespace core
