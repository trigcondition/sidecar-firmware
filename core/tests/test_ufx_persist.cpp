// core/tests/test_ufx_persist.cpp - the user effects' slots (UFX1..UFX4, Plugin API 3) are kept per project in
// /.rytm/fxslots/<project>.txt (core/user/um.cpp fx_save / fx_take). Checked, across a real restart (the program runs
// itself again on the same card: a fresh um state that reads the files):
//   - um=fx puts the effect in the project's file; after the restart it is in its slot again (READY), the same package;
//   - an emptied slot stays empty; two projects keep their own assignments;
//   - a package that is not installed: the slot MISSING, the file not rewritten by the load, and a later change of
//     another slot keeps that slot's line (the assignment comes back when the package is installed);
//   - a file that does not parse: the slots empty, the file never written over;
//   - a project with no file keeps what is loaded (as before) and writes nothing until a change.
// The packages are the probe effects in tests/api3_pkgs (arm64 Mac only, as test_api3).
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "host_link.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"

using namespace core;

#if defined(__aarch64__)
static std::vector<std::string> g_log;
static void sink(const char* s, int n) { g_log.emplace_back(s, (size_t)n); }
static void out(const uint8_t*, int) {}
static std::string slurp(const std::string& p) {
  std::string b;
  FILE* f = fopen(host_path(p.c_str()).c_str(), "rb");
  if (!f) return "<none>";
  char t[4096];
  size_t n;
  while ((n = fread(t, 1, sizeof t, f)) > 0) b.append(t, n);
  fclose(f);
  return b;
}
static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
static void idle_all() {
  int quiet = 0;
  for (int i = 0; i < 200000; ++i) {
    if (um::idle()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static void cmd(const std::string& c) {
  char o[200];
  CHECK(um::command(c.c_str(), o, sizeof o));
  idle_all();
}
static void goto_project(const char* p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x10};
  for (const char* c = p; *c; ++c) m.push_back((uint8_t)*c);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
  idle_all();
  CHECK(!strcmp(core::project, p));
}
static bool ufx(int u, rmp_slot_state st, const char* id) {   // UFX u+1
  um::SlotView v = um::slot_view(5 + u);
  if (v.state != st || strcmp(v.id, id)) fprintf(stderr, "UFX%d: state %d id '%s' (wanted %d '%s')\n", u + 1, (int)v.state, v.id, (int)st, id);
  return v.state == st && !strcmp(v.id, id);
}
static int logged(const std::string& s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}
static void boot() {
  host_set_log_sink(sink);
  hooks.to_rytm = ::out;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  idle_all();
}

int main(int argc, char** argv) {
  if (argc == 3 && !strcmp(argv[1], "restart")) {   // ---- the second run: a restart on the same card
    host_set_card(argv[2]);
    boot();
    CHECK(!strcmp(core::project, "P1"));
    // the boot's project: both effects back, the same packages
    CHECK(ufx(0, RMP_SLOT_READY, "fx.pingpong") && ufx(1, RMP_SLOT_EMPTY, "") && ufx(2, RMP_SLOT_READY, "fx.api3_fx"));
    CHECK(ufx(3, RMP_SLOT_EMPTY, ""));
    CHECK(slurp("/.rytm/fxslots/P1.txt").find("\tfx.pingpong 1.0.0\n") != std::string::npos);
    goto_project("P2");   // emptied UFX1 stays empty, its own UFX2
    CHECK(ufx(0, RMP_SLOT_EMPTY, "") && ufx(1, RMP_SLOT_READY, "fx.api3_fx") && ufx(2, RMP_SLOT_EMPTY, ""));
    std::string gone = slurp("/.rytm/fxslots/P3.txt"), bad = slurp("/.rytm/fxslots/P4.txt");
    goto_project("P3");   // a package that is not installed
    CHECK(ufx(0, RMP_SLOT_MISSING, "fx.gone") && ufx(1, RMP_SLOT_EMPTY, ""));
    CHECK(slurp("/.rytm/fxslots/P3.txt") == gone);   // the load wrote nothing
    cmd("um=fx:2:fx.api3_fx");   // another slot changes: the missing one's line is kept
    CHECK(ufx(0, RMP_SLOT_MISSING, "fx.gone") && ufx(1, RMP_SLOT_READY, "fx.api3_fx"));
    std::string g2 = slurp("/.rytm/fxslots/P3.txt");
    CHECK(g2 != gone && g2.find("\tfx.gone 1.0.0\n") != std::string::npos && g2.find("\tfx.api3_fx 1.0.0\n") != std::string::npos);
    goto_project("P4");   // a file that does not parse
    CHECK(ufx(0, RMP_SLOT_EMPTY, "") && ufx(1, RMP_SLOT_EMPTY, "") && ufx(2, RMP_SLOT_EMPTY, ""));
    CHECK(logged("fxslots/P4.txt") >= 1);
    cmd("um=fx:1:fx.pingpong");   // in memory only
    CHECK(ufx(0, RMP_SLOT_READY, "fx.pingpong"));
    CHECK(slurp("/.rytm/fxslots/P4.txt") == bad);
    goto_project("P1");
    CHECK(ufx(0, RMP_SLOT_READY, "fx.pingpong") && ufx(2, RMP_SLOT_READY, "fx.api3_fx"));
    printf("test_ufx_persist: PASS\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-ufxp-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/fxslots"));
  for (const char* p : {"P1", "P2", "P3", "P4"}) CHECK(fs.mkdir((std::string("/.rytm/projects/") + p).c_str()));
  put("/.rytm/project", "P1");
  for (const char* n : {"ufx_pingpong", "api3_fx"}) {
    FILE* f = fopen((std::string("tests/api3_pkgs/") + n + ".a64.rmp").c_str(), "rb");
    CHECK(f);
    std::string b;
    char t[65536];
    size_t k;
    while ((k = fread(t, 1, sizeof t, f)) > 0) b.append(t, k);
    fclose(f);
    put(std::string("/Plugins/") + n + ".rmp", b);
  }
  boot();
  CHECK(!strcmp(core::project, "P1"));
  CHECK(slurp("/.rytm/fxslots/P1.txt") == "<none>");   // nothing written by a load
  cmd("um=fx:1:fx.pingpong");
  cmd("um=fx:3:fx.api3_fx");
  CHECK(ufx(0, RMP_SLOT_READY, "fx.pingpong") && ufx(2, RMP_SLOT_READY, "fx.api3_fx"));
  std::string p1 = slurp("/.rytm/fxslots/P1.txt");
  CHECK(p1 != "<none>" && p1.find("\tfx.pingpong 1.0.0\n") != std::string::npos && p1.find("\tfx.api3_fx 1.0.0\n") != std::string::npos);
  // P2: UFX1 set then emptied, UFX2 set
  goto_project("P2");
  cmd("um=fx:1:fx.pingpong");
  cmd("um=fx:1:-");
  cmd("um=fx:3:-");   // (P2 had no file: it started with P1's UFX3, which this change wrote into its file)
  cmd("um=fx:2:fx.api3_fx");
  CHECK(ufx(0, RMP_SLOT_EMPTY, "") && ufx(1, RMP_SLOT_READY, "fx.api3_fx") && ufx(2, RMP_SLOT_EMPTY, ""));
  put("/.rytm/fxslots/P3.txt", "0abc GONE\tfx.gone 1.0.0\n\n\n\n");
  put("/.rytm/fxslots/P4.txt", "0abc X\tbad id!! zzz\n");
  goto_project("P1");   // back in this run: the file's
  CHECK(ufx(0, RMP_SLOT_READY, "fx.pingpong") && ufx(1, RMP_SLOT_EMPTY, "") && ufx(2, RMP_SLOT_READY, "fx.api3_fx"));
  CHECK(slurp("/.rytm/fxslots/P1.txt") == p1);
  put("/.rytm/project", "P1");
  std::string self = std::string("\"") + argv[0] + "\" restart \"" + tmpl + "\"";
  fflush(stdout);
  int rc = system(self.c_str());
  CHECK(rc == 0);
  return 0;
}
#else
int main() {
  printf("test_ufx_persist: RMPA images run on an AArch64 host only: skipped\n");
  return 0;
}
#endif
