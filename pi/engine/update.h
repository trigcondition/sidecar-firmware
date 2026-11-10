// pi/engine/update.h - the firmware update from the web page (docs/web-update.md): the release's update file (.scu,
// both boards' kernels and the web app) uploaded by the page, checked, booted ONCE as a trial (the firmware's tryboot,
// the same on the Pi 4 and the Pi 5), and made permanent by the new engine itself once it has run 30 s. Any reboot
// before that boots the old engine again, which notices and cleans up. Portable (hosted flags), over fsweb's card
// Backend: web.cpp serves the endpoints, update_pi.cpp runs the boot check and the commit on the Pi;
// pi/engine/tests/test_update.cpp runs all of it on a folder, power cuts included.
//
// What the next boot reads (config.txt, cmdline.txt, engine.img, kernel_2712.img, sdboot) is never written while the
// old engine runs: install writes only new files (the trial kernel, cmdline-try.txt, tryboot.txt, its own state under
// /.rytm/update). Only the trial engine's commit renames the old kernel to .prev and the trial into place - two
// renames, the one window where a power cut leaves no kernel (milliseconds; FAT has no atomic replace).
#ifndef _pi_engine_update_h
#define _pi_engine_update_h
#include <stdint.h>

#include "fsweb.h"

namespace upd {

// ---- the file (tools/make_update.py writes it; docs/web-update.md "The update file")
constexpr uint32_t kHeader = 512;
constexpr int kMaxParts = 6;
enum : uint32_t { kPartPi4 = 1, kPartPi5 = 2, kPartWeb = 3 };
constexpr uint64_t kPi4Max = 0x800000u - 0x80000u + 32;   // sdboot.patch's SDB_MAXLEN and the 32-byte header
constexpr uint64_t kPi5Max = 16u << 20;
constexpr uint64_t kWebMax = 8u << 20;
constexpr uint32_t kWebFiles = 1024;
constexpr uint64_t kFileMax = 40u << 20;
constexpr uint32_t kCommitAfterS = 30;                     // the trial's uptime before it makes itself permanent

struct Part { uint32_t kind, flags; uint64_t off, size; uint8_t sha[32]; };
struct Header { char version[24], commit[24]; uint32_t flags; int n; Part part[kMaxParts]; };

// The header's checks (magic, format, its sha256, the parts in order and in bounds, ending at the file's end).
// nullptr: good; else the reason, a sentence.
const char* parse_header(const uint8_t* h, uint64_t file_size, Header* out);
// MAJOR.MINOR.PATCH[-pre]: <0 a older than b, 0 the same, >0 newer; a release is above its pre-releases and "-dev"
// below every other pre-release. Unparseable: compared as text (never "newer" by accident: -2).
int cmp_version(const char* a, const char* b);
// The marker every kernel with this code carries ("sidecar-webupdate-1 v=<version> c=<commit>"): the update file's
// kernels must carry the header's, and "previous" asks the .prev kernel for it (can it commit itself?)
const char* stamp();

// ---- the boot files of a trial
const char* boot_name(int board);    // "/engine.img" (Pi 4, sdboot's), "/kernel_2712.img" (Pi 5)
const char* trial_name(int board);   // "/engine-try.img", "/kernel_2712-try.img"
// tryboot.txt for the trial from config.txt (a C string). nullptr: done (out NUL-terminated); else why not.
const char* tryboot_txt(int board, const char* config, char* out, int cap);
// cmdline-try.txt: cmdline.txt's first line and " updtrial=1"
bool cmdline_try(const char* cmdline, char* out, int cap);

// ---- the engine's side
struct Env {
  const fsweb::Backend* b;
  int board;              // 4, 5
  const char* version;    // the running engine's (SIDECAR_VERSION)
  const char* commit;     // its commit, "-dirty" included
  bool enabled;           // webupdate=1, a card, not a test mode
  const char* off_why;    // why not, when !enabled
  bool trial;             // this boot is a trial (cmdline updtrial=1)
  uint32_t uptime_s;
};
enum Reboot { kNone = 0, kPlain = 1, kTry = 2 };

// POST /update/put: a chunk of the file, /fs/put's protocol (200 "ok <next>" | "done <n>", 409 "expected <off>")
void put(const Env& e, uint64_t off, uint64_t total, uint32_t crc, const uint8_t* data, uint32_t n, fsweb::Result* r);
// POST /update/install: the uploaded file checked and its kernel staged as the trial. kTry: reboot with the tryboot flag.
Reboot install(const Env& e, bool force, fsweb::Result* r);
// POST /update/previous: the .prev kernel as a trial (kTry); one without this code only with force, swapped at once (kPlain)
Reboot previous(const Env& e, bool force, fsweb::Result* r);
void discard(const Env& e, fsweb::Result* r);   // POST /update/discard: the uploaded file deleted
// Every boot with a card, before the web server: a trial engine waits for its commit; any other engine finishes an
// interrupted commit or cleans up a trial that never committed (the revert). Returns true when a trial waits.
bool boot(const Env& e);
bool commit_due(const Env& e);   // a trial that has run kCommitAfterS
bool commit(const Env& e);       // the trial made permanent (true) - the old kernel kept as .prev
int state_json(const Env& e, char* out, int cap);   // GET /update/state

}  // namespace upd
#endif
