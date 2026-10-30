// core/bundle/bundle_int.h - the bundle job's shared state (bundle.cpp, export.cpp, import.cpp). Not an interface.
#pragma once
#include "bundle.h"

#if CORE_MSAMP
#include "../platform.h"
#include "../project_key.h"
#include "../lib/sha256.h"

namespace core {
namespace bundle {

enum How : uint8_t { kOne = 0, kDir, kSongs };
struct ProjectFile {
  const char* card;    // "/.rytm/projects/%s/samples.pool.txt"
  const char* entry;   // "project/samples.pool.txt"; a folder's: "project/lfo/"
  uint8_t kind;
  const char* fmt;     // its format's name ("" from the file: .sqp)
  uint8_t how;
  bool old;            // not exported (no longer read): imported where it was, deleted with the project
};
extern const ProjectFile kProjectFiles[];
extern const int kNumProjectFiles;
extern const char* const kScanned[3];

constexpr uint32_t kTextMax = 4u << 20;    // a project file rewritten, commit.txt, an .sfz
constexpr uint32_t kImportRam = 8u << 20;  // an import's project-file and .sfz texts (kept from VERIFY)
constexpr int kMaxOps = kMaxPaths + 512;
constexpr int kPathHash = 16384;               // the path table (a power of two)
constexpr uint32_t kRamFile = 0xFFFFFFFFu; // ram_off: the entry's bytes are on the card (src)

struct Op {
  char kind;               // 'R' rename staged a -> final b, 'D' remove file a, 'd' remove folder a, 'M' a to the trash b
  char a[kCardPath];
  char b[kCardPath];
};
// import: where one card path of a bundle entry goes
enum Act : uint8_t { kActWrite = 0, kActReuse, kActSkip };
struct PathPlan {
  uint8_t act;
  bool relocated;
  bool ok_orig, ok_cand, ok_pre;   // a file with the entry's sha256 at its own path / the index's / the relocation's
  int entry;
  int group;                       // the .sfz entry it moves with (-1: placed on its own)
  char cand[kCardPath];            // the dedupe index's (or a library's) file with that content
  char to[kCardPath];              // the final card path
};
struct Shared {
  Status st;
  int job;
  uint32_t job_n;          // the staging folder's number
  uint32_t last_us;
  uint32_t rp, wp;         // this step's read / write piece
  uint8_t* io;             // a piece
  Manifest x;              // the export's (and an import's backup)
  Manifest m;              // the import's
  ZEntry* z;               // the export's archive entries: [0] manifest.json, then the manifest's
  ZEntry* iz;              // the import's (as read)
  Layout L;
  char* man;               // the manifest's text
  uint32_t man_len;
  uint8_t* ram;            // the export's project files (a snapshot)
  uint32_t ram_used;
  uint32_t* ram_off;
  uint8_t* cd;             // the import's central directory
  uint8_t* iram;           // the import's pools / picks / .sfz texts (kept by VERIFY's read: PLAN reads no card)
  uint32_t* iram_off;
  uint32_t iram_used;
  char* text;              // a file to rewrite, commit.txt
  Op* ops;
  int nops, op_i;
  PathPlan* paths;
  int* zix;                // import: manifest entry -> archive entry
  int* ht;                 // export: path -> entry
};
extern Shared g;

void ensure();
void bump();
void say(const char* fmt, ...);
bool fail(const char* fmt, ...);   // the job FAILED (msg), its files cleaned up; false
void set_phase(int p, uint64_t total);
bool file_size(const char* path, uint64_t* size);
bool is_dir(const char* path);
bool mkdirs_for(const char* file);
// A stick job's breadcrumb (2026-10-29: core 0 hung at the end of a USB export, the watchdog reset it and the RAM log
// was gone): the step about to run on the stick, written to the card (kMarkPath) before it; removed when the job
// ends. recover() at the next boot logs one that is still there - the step the Sidecar stopped in (bundle.h kMarkPath).
void mark(const char* step);
void unmark();
bool read_at(File& f, uint64_t off, uint8_t* buf, uint32_t n);   // (counted: stats())
bool write_all(File& f, const uint8_t* buf, uint32_t n);         // (counted)
int64_t read_small(const char* path, uint8_t* buf, uint32_t cap);
bool ends_ci(const char* s, const char* e);
bool under(const char* path, const char* dir);
bool in_scanned(const char* path);
uint32_t hash_ci(const char* s);
void rytm_name_of(const char* key, char* out, int cap);
bool is_current(const char* key);
bool key_ok(const char* key);   // a key project_key() makes (export / delete take nothing else)
void remove_tree(const char* path);
bool run_op(char kind, const char* a, const char* b);
int replace_deletes(const char* key);
// slot mode's import: each slot key's Sidecar files (files[1..128]: the scan's count, 0 none) and the index's name for
// it (names[1..128], "" none); one scan of the per-project folders
void slot_files(uint32_t* files, char (*names)[24]);

// the job's removable volume (Status::vol): its I/O now (stick pieces, full rate only), still needed (a pull fails it)
bool export_stick_now();
bool import_stick_now();
bool import_stick_needed();
bool vol_space(const char* drive, uint64_t* free);   // false: not mounted; *free 0 when not counted
void export_step();
void import_step();
bool export_begin(const char* key, int dest, const ExportOpts& o, bool sub, const char* folder);
bool export_sub_step();    // true at its end (done: export_done(), else failed)
bool export_done();
const char* export_path();
void ex_cleanup();
void im_cleanup();
void cleanup_failed();     // the job's own files (.part, staging) removed, its handles closed
void close_files();

}  // namespace bundle
}  // namespace core
#endif
