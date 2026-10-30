// core/bundle/bundle.h - project bundles (docs/BUNDLES.md; the design: docs/superpowers/specs/
// 2026-10-22-project-bundles-pi-design.md): a Sidecar project as one <NAME>.sidecar.zip - its per-project files, every
// sample it uses, its song takes, its user machine packages and an optional Rytm project file - exported from and
// imported into a card. Portable (the Pi's core 0, the Mac's tests and tools): files through core::File / core::Fs.
// Pi and Mac only (CORE_MSAMP).
//
// One job at a time, run by step(): each call does one bounded piece (at most IoRule's piece of card I/O, a few file
// opens) and returns. On the Pi step() runs on core 0 (pi/engine/bundle_pi.cpp), never on the control core (core 3),
// and only while the streams do not need the card (Env::card_busy) and the pause rules allow (Env::rules: writes
// paused while the Rytm plays or a take records, everything paused while the Overbridge recorder records).
//
// Export:  start_export(key, dest) -> FLUSH (current project: the pending saves get time, the pool lists' too; a
//          recording song take stops it) -> COLLECT (the project files read into RAM; the samples and instruments its
//          pool lists name - pools::refs() for the current project, the list files' own parser for another -, members
//          and SFZ instruments resolved, slot maps, packages, sampler takes, the Rytm file) -> HASH (sha256 + CRC-32 of
//          every entry the library has not hashed; same content at two paths stored once; > 4 GiB refused with what
//          takes the space) -> WRITE (dest card: /Bundles/<name>.part, each entry's sha256 checked as written, then
//          renamed) or SERVE (dest browser: serve() answers any byte range; nothing written).
// Import:  start_verify(zip) -> VERIFY (central directory, local headers, manifest, every entry's sha256: one wrong
//          byte refuses the bundle, nothing written) -> PLAN (where every file goes: reused by hash, written at its
//          path, or under /Samples/Projects/<new key>/; the packages; space) -> plan_json() is the summary;
//          start_import(opts) -> [BACKUP: replace mode exports the old project first] -> STAGE (every new file into
//          /Bundles/.staging/<job>/, read back and its sha256 checked; SFZ absolute paths rewritten - the pool lists are
//          written as they are: their refs resolve by hash) -> COMMIT
//          (commit.txt, then the renames) -> LINK (the project index; the Rytm file to /Bundles/Rytm/).
//          Slot mode (2026-10-29, a card keyed by the Rytm's slots: prjslot.h): the import goes to a slot (ImportOpts::
//          slot; the default: the bundle's own slot when the Rytm's name there is the bundle's, else the slot with the
//          bundle's name, else the first empty one), every per-project file under its key @NNN, named as the Rytm names
//          that slot (else as the bundle). A slot with Sidecar files only with replace: its files go to the project
//          trash (one entry, restorable) in the same commit.txt as the new files ("M" lines), no BACKUP export; never
//          the Rytm's current slot. The bundle holds the Sidecar's side: the Rytm's project goes into that slot itself.
// recover() at boot: a staging folder with a complete commit.txt is rolled forward, any other is deleted: the card is
// as before an import or as after it, never half.
#pragma once
#include <stdint.h>

#include "../project_key.h"
#include "manifest.h"

namespace core {
namespace bundle {

constexpr uint32_t kPiece = 65536;          // a step's card I/O at most (the recorder's piece, cardq::kPiece)
constexpr uint32_t kSlowPiece = 16384;      // while the Rytm plays: reads only, this big, one every kSlowGapUs
constexpr uint32_t kSlowGapUs = 2000;
constexpr uint32_t kStickPiece = 32768;     // a USB stick's piece (the design 4.3; Circle's transfers block core 0)
constexpr int kDriveMax = 8;                // "USB3:" and its NUL
// On a USB stick (docs/STORAGE.md's volumes USB:, USB2:, USB3:) bundles are written to and found in <drive>/SIDECAR (and
// found at its root too); a path names its drive: "USB:/SIDECAR/AR PERC 20261022-1403.sidecar.zip".
constexpr const char* kStickFolder = "/SIDECAR";
constexpr int kMaxEntries = 4096, kMaxPaths = 8192, kMaxMissing = 256;
constexpr uint32_t kProjectRam = 32u << 20;   // the project files' snapshot (RAM) at most
constexpr uint64_t kSpaceMargin = 16u << 20;  // an import needs its new bytes plus this free

enum Phase : uint8_t { kIdle = 0, kFlush, kCollect, kHash, kWriting, kServe, kVerify, kPlan, kReady, kBackup, kStage,
                       kCommit, kLink, kDelete, kDone, kFailed, kCancelled };
const char* phase_name(int p);   // "IDLE" "FLUSH" "COLLECT" "HASH" "WRITE" "SERVE" "VERIFY" "PLAN" "READY" ...
enum Job : uint8_t { kNoJob = 0, kExport, kImport, kDeleteJob };
enum Dest : uint8_t { kToCard = 0, kToBrowser, kToVolume };   // kToVolume: ExportOpts::vol's /SIDECAR
enum Mode : uint8_t { kNew = 0, kReplace };

// What the job may do now (the pause rules, docs/BUNDLES.md "Never disturbing the audio"); 0: paused
struct IoRule {
  uint32_t read_piece, write_piece, read_gap_us;
  const char* why;   // "PLAYING" "SONG REC" "RECORDING" while something is held back ("" none)
};
// A removable volume the platform has mounted (the Pi: stor_pi's USB drives)
struct Volume {
  char drive[kDriveMax];   // "USB:"
  char name[8];            // "USB", "USB2"
  uint64_t free, total;    // bytes (free 0 with free_known false: not counted)
  bool free_known;
};
struct Env {
  void (*rules)(IoRule* r);                                  // null: full rate always
  bool (*card_busy)();                                       // cardq::busy() on the Pi; null: never
  void (*current)(char* rytm_name, int cap);                 // the Rytm's project now ("" none)
  bool (*song_recording)();                                  // a song take records (its journal open)
  // beside the library (core/lib/library.h: lib::find_file, the import's dedupe): another index's file with this
  // content, if one is known (null: none; either is read again before it is reused)
  bool (*find_by_hash)(const uint8_t sha[32], uint32_t size, char* path, int cap);
  void (*changed)(const char* path);                         // after a commit: what to rescan (/Plugins, /Samples, ...)
  const char* version;                                       // the Sidecar's ("0.2.0")
  const char* board;                                         // "pi4" "pi5" "mac"
  uint32_t flush_ms;                                         // FLUSH: the idle saves' time (default 1500)
  // removable volumes (all null: none). A job on one fails at once when it goes (a stick pulled) and does its stick
  // I/O in kStickPiece pieces, only at full rate (paused while the Rytm plays, a take records, the recorder records).
  int (*volumes)(Volume* out, int max);                      // the mounted ones now
  bool (*vol_ok)(const char* drive);                         // still mounted
  const char* (*vol_eject)(const char* drive);               // unmount it safely: null done, else why not
};
void set_env(const Env& e);
const Env& env();

struct Status {
  uint32_t seq;            // bumped on every change
  uint8_t job, phase;
  uint64_t done, total;    // bytes (or items) of the phase
  char key[kKeyMax];       // the project
  char file[kCardPath];    // the bundle (export: its name; import: its path)
  char msg[200];           // one line: what happened / what to do
  char waiting[16];        // "PLAYING" ... while paused ("" running)
  uint64_t bytes;          // the bundle's size (known from HASH / VERIFY)
  char vol[kDriveMax];     // the removable volume the job reads or writes ("" the card only)
};
const Status& status();
bool busy();                                    // a job runs (or waits for its import's start)

struct ExportOpts {
  bool unused_takes = true;    // the sampler takes in /Samples/<project>/ no pool names (open question 5)
  uint32_t epoch = 0;          // its time (0: the clock's, projects::clock_now)
  char vol[kDriveMax] = "";    // kToVolume: the drive ("USB:")
};
void set_usb_export(bool on);   // 0.3.0: false (export to a stick refused); the host tests set it
bool start_export(const char* key, int dest, const ExportOpts& o);   // false: busy, or no such project (msg says)
// Browser downloads: the archive's bytes [off, off + len) (len at most IoRule's read piece: *got may be less). false:
// not serving, past the end, or a file changed (the job fails: "X changed during export").
bool serve(uint64_t off, uint8_t* buf, uint32_t len, uint32_t* got);
const char* bundle_name();                      // the export's file name ("AR PERC 20261022-1403.sidecar.zip")

struct ImportOpts {
  char name[24] = "";           // the new project's Rytm name ("": the plan's)
  int mode = kNew;
  bool packages = true;         // the bundle's machines installed (open question 6: on by default)
  uint8_t skip_slots = 0;       // bit s: slot s+1's package left out
  int slot = 0;                 // slot mode: the Rytm's project slot the import goes to (1..128; 0: the plan's default)
};
bool start_verify(const char* zip_path);
int plan_json(char* out, int cap);              // the PLAN summary (READY), the done screen (DONE)
bool start_import(const ImportOpts& o);         // after READY
ImportOpts import_opts();                       // the options READY was planned with (a page that changes none keeps them)
bool start_delete(const char* key, const char* confirm);   // project files only, never the current project
// A bundle file deleted (the bundle lists' Delete, 2026-10-29): /Bundles/<name> on the card, <drive>/<name> or
// <drive>/SIDECAR/<name> on a stick, <name> a .sidecar.zip or an interrupted export's .sidecar.zip.part. Refused for
// the file a job uses (its export's .part, its import's bundle). Null done, else why not.
const char* remove_bundle(const char* path);
void cancel();                                  // before COMMIT: the .part / the staging folder removed
bool step();                                    // one piece; true: did something
void finish();                                  // DONE / FAILED / CANCELLED seen: back to IDLE (buffers kept)

void recover();                                 // boot, before the engine reads the project files
// A stick job's breadcrumb on the card: the step it was in (mark(), bundle_int.h); recover() logs one a reset left
// and puts it in Status::msg ("the last run stopped in: ...")
constexpr const char* kMarkPath = "/.rytm/bundle-mark.txt";   // (in a folder every Sidecar card has: none made)
// The bundle UIs' EJECT: a job on that drive is cancelled first (not during its COMMIT: "busy"), then the platform's
// eject. Null done, else why not.
const char* eject(const char* drive);
int drive_of(const char* path, char* drive);    // "USB:/x" -> "USB:" (its length), 0 a card path

// The projects the card knows (the union of the per-project folders' keys and the index), as JSON
// [{"key","rytm","used","date","imported","current","files","bytes"}]; bytes: the project files only.
int projects_json(char* out, int cap);
int bundles_json(const char* folder, char* out, int cap);   // the *.sidecar.zip (and .part) in a folder, with "path"
// The same for the PROJECT pages (core/bundle/ui.h): rows instead of JSON, at most max (the newest bundles first)
struct ProjectRow {
  char key[kKeyMax], rytm[24], date[20];
  uint32_t files, used;
  uint64_t bytes;
  bool current, imported;
};
struct BundleRow {
  char name[96], path[kCardPath], date[20];
  uint64_t size;
  bool incomplete;
};
int project_rows(ProjectRow* out, int max);
int bundle_rows(const char* folder, BundleRow* out, int max);
// The import's summary (READY) or done screen (DONE) as plan_json's, for the pages
constexpr int kPlanMachines = 8;
enum PkgAction : uint8_t { kPkgInstall = 0, kPkgReuse, kPkgConflict, kPkgSide, kPkgLeftOut, kPkgNotPi, kPkgRefused, kPkgCheck };
const char* pkg_action_name(int a);   // "install" "reuse" "conflict" "side-by-side" "left out" "not for the Pi" ...
struct PlanInfo {
  char name[24], from[24], step[300], rytm_file[64];
  bool clash, replace;
  uint8_t slot;    // slot mode: the target slot (clash: it has Sidecar files - replace on the web page); 0 name mode
  bool pick;       // slot mode, every slot taken: no default - the slot is picked on the web page
  uint64_t new_bytes, reused_bytes;
  uint32_t write, reuse, relocated, samples, songs, patterns, missing;
  int nmach;
  struct { uint8_t slot, action; char name[12], id[32], ver[12]; } mach[kPlanMachines];
};
bool plan_info(PlanInfo* out);   // false: no import READY or DONE
// The per-project files of key (the table both directions use): their card paths, folders last.
int project_files(const char* key, char (*out)[kCardPath], int max, bool* dirs);
bool project_exists(const char* key);

// The job's own measure (the loadstall-style test, /status): steps run, steps held back (paused or the streams first),
// the most card bytes one step read / wrote, the longest step's CPU time (the card model's time is the test's).
struct Stats {
  uint32_t steps, waits, max_read, max_write, max_us;
  uint64_t read, written;
};
const Stats& stats();
void stats_reset();

void reset_for_tests();

}  // namespace bundle
}  // namespace core
