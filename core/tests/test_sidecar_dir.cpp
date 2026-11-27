// core/tests/test_sidecar_dir.cpp - the Pi's own folder (core/device/sidecar_dir.h): which names in /.rytm are the Pi's;
// a card from before /.sidecar moved over once (files, their .prev, folders with their contents), a box's state left in
// /.rytm, nothing overwritten when /.sidecar already holds a name, a second boot moving nothing; a card with no /.rytm;
// and a box in slot 1 reaching the Pi's files at /.sidecar, not in its own folder.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "device/sidecar_dir.h"
#include "platform.h"

using namespace core;

static std::string g_dir;
static bool on_card(const char* p) {
  struct stat st;
  return stat((g_dir + p).c_str(), &st) == 0;
}
static void put(const char* p, const char* text) {
  FILE* f = fopen((g_dir + p).c_str(), "w");
  CHECK(f);
  fputs(text, f);
  fclose(f);
}
static std::string get(const char* p) {
  FILE* f = fopen((g_dir + p).c_str(), "r");
  if (!f) return "";
  char b[256] = {0};
  size_t n = fread(b, 1, sizeof b - 1, f);
  fclose(f);
  return std::string(b, n);
}

static void test_names() {
  for (const char* n : {"wifi.txt", "wifi.txt.prev", "wifi.try", "wpa.conf", "pair.txt", "display.txt", "display.txt.prev",
                        "display.txt.tmp", "prevboot.txt", "blackbox.log", "blackbox.1.log", "blackbox.try", "logs",
                        "recspeed.txt", "storbench-SD.txt", "sdtest.txt", "update", "www-next", "www-next.prev",
                        "www-next.swp", "env", "folders.bin", "files.bin", "library", "dx7.bin"})
    CHECK(sidecar_dir::pi_wide(n));
  for (const char* n : {"projects", "projects.txt", "project", "pools", "slots", "kits", "seq", "songs", "scene", "xy", "lfo",
                        "ins", "fm", "juno", "granular", "wavetable", "multisample", "fxslots", "sendfx.txt", "fxin.txt",
                        "keys.txt", "learn.txt", "preview.txt", "prjslot.txt", "trash", "bundle-mark.txt", "device",
                        "envelope", "wifi", "updates", "libraryx", "display.txtx"})
    CHECK(!sidecar_dir::pi_wide(n));
}

static void test_migrate() {
  system(("rm -rf '" + g_dir + "' && mkdir -p '" + g_dir + "/.rytm/www-next/assets' '" + g_dir + "/.rytm/library' '" + g_dir +
          "/.rytm/projects/a' '" + g_dir + "/.rytm/update'").c_str());
  put("/.rytm/wifi.txt", "on=1\nssid=x\n");
  put("/.rytm/wifi.txt.prev", "on=0\n");
  put("/.rytm/pair.txt", "pair\n");
  put("/.rytm/display.txt", "cpumhz=1200\nfan=on\n");
  put("/.rytm/blackbox.log", "log\n");
  put("/.rytm/blackbox.1.log", "old log\n");
  put("/.rytm/storbench-SD.txt", "bench\n");
  put("/.rytm/www-next/index.html.gz", "app");
  put("/.rytm/www-next/assets/a.js.gz", "js");
  put("/.rytm/library/index.bin", "idx");
  put("/.rytm/update/state.txt", "state=trial\n");
  put("/.rytm/env", "hash=1\n");
  put("/.rytm/keys.txt", "keys=channel\n");        // the box's own: stays
  put("/.rytm/projects/a/samples.pool.txt", "p");  // the box's own: stays
  put("/.rytm/projects.txt", "a\n");

  CHECK(sidecar_dir::migrate() == 11);
  for (const char* p : {"/.sidecar/wifi.txt", "/.sidecar/wifi.txt.prev", "/.sidecar/pair.txt", "/.sidecar/display.txt",
                        "/.sidecar/blackbox.log", "/.sidecar/blackbox.1.log", "/.sidecar/storbench-SD.txt",
                        "/.sidecar/www-next/index.html.gz", "/.sidecar/www-next/assets/a.js.gz", "/.sidecar/library/index.bin",
                        "/.sidecar/update/state.txt", "/.sidecar/env"})
    CHECK(on_card(p));
  for (const char* p : {"/.rytm/wifi.txt", "/.rytm/pair.txt", "/.rytm/display.txt", "/.rytm/www-next", "/.rytm/library",
                        "/.rytm/update", "/.rytm/env", "/.rytm/blackbox.log"})
    CHECK(!on_card(p));
  CHECK(get("/.sidecar/display.txt") == "cpumhz=1200\nfan=on\n" && get("/.sidecar/wifi.txt") == "on=1\nssid=x\n");
  CHECK(on_card("/.rytm/keys.txt") && on_card("/.rytm/projects/a/samples.pool.txt") && on_card("/.rytm/projects.txt"));
  CHECK(!on_card("/.sidecar/keys.txt") && !on_card("/.sidecar/projects"));

  CHECK(sidecar_dir::migrate() == 0);   // the second boot: nothing left to move

  // a name in both (an older build booted again and wrote its own): /.sidecar's kept, /.rytm's left, nothing lost
  put("/.rytm/wifi.txt", "on=0\nssid=old\n");
  put("/.rytm/recspeed.txt", "SD=9000\n");
  CHECK(sidecar_dir::migrate() == 1);
  CHECK(get("/.sidecar/wifi.txt") == "on=1\nssid=x\n" && get("/.rytm/wifi.txt") == "on=0\nssid=old\n");
  CHECK(get("/.sidecar/recspeed.txt") == "SD=9000\n" && !on_card("/.rytm/recspeed.txt"));
}

static void test_fresh_card() {   // no /.rytm at all: /.sidecar made, nothing moved
  system(("rm -rf '" + g_dir + "' && mkdir -p '" + g_dir + "'").c_str());
  CHECK(sidecar_dir::migrate() == 0);
  struct stat st;
  CHECK(stat((g_dir + "/.sidecar").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
  CHECK(!on_card("/.rytm"));
}

int main() {
  test_names();
  char tmpl[] = "/tmp/sidecar-dir-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_dir = tmpl;
  host_set_card(tmpl);
  test_migrate();
  test_fresh_card();
  system(("rm -rf '" + g_dir + "'").c_str());
  printf("test_sidecar_dir: PASS\n");
  return 0;
}
