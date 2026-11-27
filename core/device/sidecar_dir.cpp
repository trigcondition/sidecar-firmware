// core/device/sidecar_dir.cpp - see sidecar_dir.h.
#include "sidecar_dir.h"

#include <string.h>

#include "../core.h"
#include "../platform.h"

namespace core {
namespace sidecar_dir {

static const char* const kStems[] = {"wifi.txt", "wifi.try", "wpa.conf", "pair.txt", "display.txt", "prevboot.txt",
                                     "blackbox", "logs", "recspeed.txt", "storbench", "sdtest.txt", "update", "www-next",
                                     "env", "folders.bin", "files.bin", "library", "dx7.bin"};

bool pi_wide(const char* name) {
  for (const char* s : kStems) {
    size_t n = strlen(s);
    if (!strncmp(name, s, n) && (!name[n] || name[n] == '.' || name[n] == '-')) return true;
  }
  return false;
}

#if CORE_MSAMP
CORE_COLD int migrate() {
  Fs fs;
  fs.mkdir(kDir);
  constexpr int kMax = 48;   // (the stems' files with their .prev / .tmp and a few benches)
  static char names[kMax][64];   // collected first: a rename while FatFs walks a folder can skip entries
  int n = 0;
  {
    File d;
    if (!d.open("/.rytm", kRead) || !d.isDir()) return 0;
    char name[256];
    bool dir;
    uint64_t size;
    uint16_t fd, ft;
    while (n < kMax && d.nextEntry(name, sizeof name, &dir, &size, &fd, &ft))
      if (strlen(name) < sizeof names[0] && pi_wide(name)) strcpy(names[n++], name);
    d.close();
  }
  int moved = 0;
  for (int i = 0; i < n; ++i) {
    char from[96], to[96];
    sniprintf(from, sizeof from, "/.rytm/%s", names[i]);
    sniprintf(to, sizeof to, "%s/%s", kDir, names[i]);
    if (fs.exists(to)) {
      log(CORE_STR("sidecar_dir: %s kept where it is: %s is already there"), from, to);
      continue;
    }
    if (fs.rename(from, to)) ++moved;
    else log(CORE_STR("sidecar_dir: %s could not be moved to %s"), from, to);
  }
  if (moved) log(CORE_STR("sidecar_dir: %d Pi-wide entries moved from /.rytm to %s"), moved, kDir);
  return moved;
}
#else
int migrate() { return 0; }
#endif

}  // namespace sidecar_dir
}  // namespace core
