// core/tests/nomorph/test_morph_saved.cpp - with the scene morph off (SIDECAR_MORPH 0) a project's saved scene and XY data
// still loads and is written back unchanged (0.5.2 turns the morph on and finds it as it was), and does nothing meanwhile:
// a saved MORPH XY slot, a scene with locks and a fader. Built with -DSIDECAR_MORPH=0 (core/Makefile NOMORPH).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "engine.h"
#include "hooks.h"
#include "bundle/rename.h"
#include "fmt/fmt.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/xypad.h"
#include "msamp/cardq.h"
#include "store/store.h"

static_assert(!SIDECAR_MORPH, "build with -DSIDECAR_MORPH=0");

using namespace core;
using mod::Tid;

static std::string g_card;
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void run_ms(int ms) {   // the control loop every 250 us: a tick each millisecond
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), mod::tick(now_us());
}
static Tid T(const char* n) {
  Tid t = 0;
  CHECK(xypad::parse_target(n, &t));
  return t;
}
static std::string cmd(const char* c) {
  char o[600];
  CHECK(xypad::command(c, o, sizeof o));
  return o;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.02f; }
static std::string rd(const std::string& p) {
  std::string s;
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "<none>";
  char b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void wr(const std::string& p, const std::string& s) {
  CHECK(system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str()) == 0);
  FILE* f = fopen((g_card + p).c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static void card_core_turn() {
  host_card_core(0);
  while (cardq::service()) {
  }
  host_card_core(3);
}
static void flush() {   // the saves wait a second after the last change; then every write lands
  at(g_us + 1100000);
  for (int i = 0; i < 64; ++i) idle_save(), card_core_turn();
  for (int i = 0; i < 4000 && fmt::post_busy(); ++i) idle_save(), card_core_turn();
  CHECK(!fmt::post_busy());
}
static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save(), card_core_turn();
  CHECK(!project_loading());
}
static void to_project(const char* name) {   // 0x10: the Rytm's project; its files into the store, then loaded
  uint8_t m[40] = {0xF0, 0x7D, 0x52, 0x54, 0x10};
  int k = 5;
  for (const char* c = name; *c; ++c) m[k++] = (uint8_t)*c;
  m[k++] = 0xF7;
  engine_sysex(m, k);
  for (int i = 0; i < 400 && strcmp(core::prj().key, name); ++i) {
    if (rename::state() == rename::kAsk) rename::answer(false);
    at(g_us + 20000), idle_save(), card_core_turn();
  }
  loaded();
  CHECK(!strcmp(core::prj().key, name));
}

int main() {
  char tmpl[] = "/tmp/core_morphsaved_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  cardq::set_worker(true);
  const char* xy = "#sidecar xy 1\nH 0\nX 1 MORPH 50\n";
  const char* sc = "#sidecar scene 1\nAB 1 2\nF 8000\nS 1\nL 1 V1:P3 6400\nL 2 V1:P3 9600\n";
  wr("/.rytm/xy/SVD.txt", xy);
  wr("/.rytm/scene/SVD.txt", sc);
  to_project("SVD");
  Tid vp3 = 0;
  CHECK(mod::parse("V1:P3", &vp3));
  // loaded as saved
  CHECK(xypad::cfg().s[0][0].t == xypad::kMorph && xypad::cfg().s[0][0].depth == 50);
  CHECK(scene::state().a == 0 && scene::state().b == 1 && scene::state().fader == 8000 && scene::state().s[0].n == 1);
  // and inert: a touch on the pad, a minute of ticks, the fader sources: V1:P3 stays, the fader stays
  v[0].p[2] = 60;
  at(g_us + 1000), mod::tick(now_us());
  xypad::pos(xypad::kMax, 0);
  run_ms(50);
  xypad::up();
  run_ms(50);
  scene::set_fader(1000, scene::kQperf);
  scene::qperf(200, 0xFFF, 1);
  run_ms(50);
  CHECK(std::fabs(v[0].p[2] - 60) < 0.02f);
  CHECK(scene::state().fader == 8000);
  // a change to another slot saves the file: the MORPH slot is in it as it was, the scene file is not rewritten
  CHECK(xypad::set(1, 0, vp3, 10));
  flush();
  std::string saved = rd("/.rytm/xy/SVD/k001.txt");
  if (saved == "<none>") saved = rd("/.rytm/xy/SVD.txt");
  CHECK(saved.find("X 1 MORPH 50") != std::string::npos);
  CHECK(rd("/.rytm/scene/SVD.txt") == sc);
  printf("test_morph_saved: ok\n");
  return 0;
}
