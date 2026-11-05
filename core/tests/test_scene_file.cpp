// core/tests/test_scene_file.cpp - the scenes' files (core/mod/scene.h, spec-050 2.8): the format round trip, a kit
// change from the RAM store only, a kit without its own file, a line from a newer build locking the file, a kit saved
// mid-morph keeping the knobs, a fader move alone, a file over kPostMax through the queue, the store's headroom, a
// project switch saving the old project's edit.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "bundle/kits.h"
#include "bundle/rename.h"
#include "fmt/fmt.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "msamp/cardq.h"
#include "store/store.h"

using namespace core;
using mod::Tid;

static std::string g_card;
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void run_ms(int ms) {
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), mod::tick(now_us());
}
static Tid T(const char* n) {
  Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
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
static std::string held(const char* path) {   // the store's bytes of path ("<none>": not present)
  const uint8_t* d;
  uint32_t n;
  return store::get(path, &d, &n) == store::kPresent ? std::string((const char*)d, n) : "<none>";
}
static void card_core_turn() {
  host_card_core(0);
  while (cardq::service()) {
  }
  host_card_core(3);
}
// the saves wait a second after the last change; then every write lands (the card core's turns too)
static void flush() {
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
  for (int i = 0; i < 400 && strcmp(core::project, name); ++i) {
    if (rename::state() == rename::kAsk) rename::answer(false);   // (a new name, not a rename of the project before)
    at(g_us + 20000), idle_save(), card_core_turn();
  }
  loaded();
  CHECK(!strcmp(core::project, name));
}
static void kit(int k) {   // 3B: the Rytm's active kit (0-based)
  const uint8_t m[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, (uint8_t)k, 0xF7};
  engine_sysex(m, 8);
}
static void lk(int n1, const char* name, float v) { CHECK(scene::lock(n1 - 1, T(name), (uint16_t)(v * 128))); }

static void round_trip() {
  kit(0);
  CHECK(scene::kit() == 0);
  for (int n = 0; n < scene::kScenes; ++n)
    for (int k = 0; k <= n; ++k) CHECK(scene::lock(n, mod::tid(k % 2 ? mod::kV : mod::kIns, k % 8, k % 12), (uint16_t)(n * 1000 + k)));
  CHECK(scene::lock(5, T("RYTM:SCN"), 640) && scene::lock(5, T("MFX:MIX2"), 12800) && scene::lock(5, T("S12:F2"), 1));
  scene::set_ab(2, 6), scene::set_sel(4), scene::set_fader(8192, scene::kConsole);
  scene::save_due();
  flush();
  std::string f = rd("/.rytm/scene/SCN/k001.txt");
  CHECK(f.find("#sidecar scene 1\n") == 0 && f.find("AB 3 7\n") != std::string::npos && f.find("F 8192\n") != std::string::npos);
  CHECK(f.find("S 5\n") != std::string::npos && f.find("L 6 RYTM:SCN 640\n") != std::string::npos);
  scene::State before = scene::state();
  scene::load();   // (it clears what is in memory first)
  const scene::State& s = scene::state();
  CHECK(s.a == 2 && s.b == 6 && s.sel == 4 && s.fader == 8192 && !scene::locked());
  for (int n = 0; n < scene::kScenes; ++n) {
    CHECK(s.s[n].n == before.s[n].n);
    for (int i = 0; i < s.s[n].n; ++i) CHECK(s.s[n].l[i].t == before.s[n].l[i].t && s.s[n].l[i].v == before.s[n].l[i].v);
  }
  printf("scene file: the round trip (%zu bytes)\n", f.size());
}

static void kit_switch_reads_ram_only() {
  for (int n = 0; n < scene::kScenes; ++n) scene::clear(n);
  lk(1, "V3:P5", 50);
  scene::save_due();
  flush();
  kit(5);   // kit 6: none of its own (and no project default): empty
  CHECK(scene::kit() == 5 && scene::state().s[0].n == 0);
  lk(2, "FX1:P3", 20);
  scene::save_due();
  flush();
  host_card_model(true);
  uint64_t ctl = host_card_us(3);
  kit(0);
  CHECK(scene::kit() == 0 && scene::state().s[0].n == 1 && scene::state().s[0].l[0].t == T("V3:P5"));
  kit(5);
  CHECK(scene::state().s[1].n == 1 && scene::state().s[1].l[0].t == T("FX1:P3"));
  kit(0);
  CHECK(host_card_us(3) == ctl);   // the store only: no card call on the control core
  host_card_model(false);
  printf("scene file: a kit change reads the RAM store only\n");
}

static void kit_without_file_uses_project_default() {
  wr("/.rytm/scene/SCD.txt", "#sidecar scene 1\nAB 1 2\nL 1 V2:P3 6400\n");
  to_project("SCD");
  kit(8);   // kit 9: no file of its own
  CHECK(scene::kit() == 8 && !scene::locked() && scene::state().s[0].n == 1 && scene::state().s[0].l[0].t == T("V2:P3"));
  lk(1, "V2:P4", 10);
  scene::save_due();
  flush();
  CHECK(rd("/.rytm/scene/SCD/k009.txt").find("L 1 V2:P4 1280") != std::string::npos);
  CHECK(rd("/.rytm/scene/SCD.txt") == "#sidecar scene 1\nAB 1 2\nL 1 V2:P3 6400\n");   // the default untouched
  printf("scene file: a kit without its own file starts from the project's\n");
}

static void unknown_line_locks_file() {
  const char* text = "#sidecar scene 1\nAB 2 0\nL 2 V1:P1 100\nL 2 ZZ9:Q1 5\nL 2 V1:P2 200\n";
  wr("/.rytm/scene/SCL/k002.txt", text);
  to_project("SCL");
  kit(1);
  CHECK(scene::kit() == 1 && scene::locked() && scene::state().s[1].n == 2 && scene::state().a == 1);
  lk(3, "V1:P3", 9);
  scene::save_due();
  flush();
  CHECK(rd("/.rytm/scene/SCL/k002.txt") == text && held("/.rytm/scene/SCL/k002.txt") == text);
  printf("scene file: a line from a newer build locks the file (read, never written)\n");
}

static void save_mid_morph_stores_knobs() {
  to_project("SCM");
  kit(0);
  master::set_lev(101);
  v[0].p[0] = 40;
  CHECK(scene::lock(0, T("MFX:LEV"), 60 * 128) && scene::lock(1, T("MFX:LEV"), 120 * 128));
  CHECK(scene::lock(0, T("V1:P1"), 20 * 128) && scene::lock(1, T("V1:P1"), 100 * 128));
  scene::set_ab(0, 1);
  scene::set_fader(8192, scene::kConsole);
  run_ms(30);
  CHECK(master::played_lev() != 101 && master::control.lev == 101 && fabsf(v[0].p[0] - 60.0f) < 0.01f);
  ins::settings_due();
  scene::save_due();
  flush();
  std::string insf = rd("/.rytm/ins/SCM/k001.txt"), scf = rd("/.rytm/scene/SCM/k001.txt");
  CHECK(insf.find("MFX 0 RET 101 ") != std::string::npos);   // the knob, never the morph
  CHECK(scf.find("L 1 MFX:LEV 7680\n") != std::string::npos && scf.find("L 2 V1:P1 12800\n") != std::string::npos);
  for (size_t i = 0, e; i < scf.size(); i = e + 1) {   // only the scene's own lines
    e = scf.find('\n', i);
    std::string l = scf.substr(i, e - i);
    CHECK(l[0] == '#' || !l.compare(0, 3, "AB ") || !l.compare(0, 2, "F ") || !l.compare(0, 2, "S ") || !l.compare(0, 2, "L "));
  }
  // SAVE KIT (4C 01 01 kit 1): the record holds the knob and the scenes' file as it is
  const uint8_t m[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4C, 1, (uint8_t)kits::kSave, 0, 1, 0, 1, 0xF7};
  engine_sysex(m, 12);
  for (int i = 0; i < 20; ++i) idle_save(), card_core_turn();
  flush();
  std::string rec = rd("/.rytm/kits/SCM/k001.sks");
  CHECK(rec.find("MFX 0 RET 101 ") != std::string::npos);
  printf("scene file: a save mid-morph stores the knobs\n");
  for (int n = 0; n < scene::kScenes; ++n) scene::clear(n);
  scene::set_ab(-1, -1);
  run_ms(2);
  CHECK(master::played_lev() == 101);
  master::set_lev(100);
}

static void fader_move_alone_not_dirty() {
  lk(1, "V4:P1", 3);
  scene::save_due();
  flush();
  CHECK(!scene::dirty());
  scene::set_fader(5000, scene::kConsole);
  CHECK(!scene::dirty());
  lk(1, "V4:P2", 3);
  CHECK(scene::dirty());
  scene::save_due();
  flush();
  CHECK(rd("/.rytm/scene/SCM/k001.txt").find("F 5000\n") != std::string::npos);
  printf("scene file: a fader move alone does not make the file dirty\n");
}

static void large_file_queued() {
  kit(3);
  for (int n = 0; n < scene::kScenes; ++n)
    for (int k = 0; k < scene::kLocks; ++k) CHECK(scene::lock(n, k < 48 ? mod::tid(mod::kIns, k / 12 + n % 9, k % 12) : mod::tid(mod::kV, (k - 48) % 8, (k - 48) / 8), 16383));
  scene::save_due();
  flush();
  std::string f = rd("/.rytm/scene/SCM/k004.txt");
  printf("scene file: 768 locks: %zu bytes (kPostMax %u)\n", f.size(), (unsigned)fmt::kPostMax);
  CHECK(f.size() > fmt::kPostMax);
  size_t ls = 0;
  for (size_t i = 0; (i = f.find("\nL ", i)) != std::string::npos; ++i) ++ls;
  CHECK(ls == 768);
  scene::load();   // (it clears what is in memory first)
  int n = 0;
  for (int s = 0; s < scene::kScenes; ++s) n += scene::state().s[s].n;
  CHECK(n == 768);
  printf("scene file: a file over kPostMax goes through the queue, whole\n");
}

// Task 10: a 12 KB scene file (768 locks) through SAVE KIT / RELOAD KIT whole (kits.cpp: the record's limit kRecMax)
static void kit_record_with_big_scene_file() {
  kit(3);   // (large_file_queued's kit 4: 768 locks)
  int n0 = 0;
  for (int s = 0; s < scene::kScenes; ++s) n0 += scene::state().s[s].n;
  CHECK(n0 == 768);
  const uint8_t sv[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4C, 1, (uint8_t)kits::kSave, 0, 4, 0, 4, 0xF7};
  engine_sysex(sv, 12);
  for (int i = 0; i < 20; ++i) idle_save(), card_core_turn();
  flush();
  std::string rec = rd("/.rytm/kits/SCM/k004.sks");
  CHECK(rec.find("=scene\n") != std::string::npos);
  for (int s = 0; s < scene::kScenes; ++s) scene::clear(s);
  flush();
  const uint8_t rl[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4C, 1, (uint8_t)kits::kReload, 0, 4, 0, 4, 0xF7};
  engine_sysex(rl, 12);
  for (int i = 0; i < 20; ++i) idle_save(), card_core_turn();
  flush();
  int n = 0;
  for (int s = 0; s < scene::kScenes; ++s) n += scene::state().s[s].n;
  printf("scene file: a kit record with a %zu-byte scene section: %d locks back after RELOAD KIT\n", rec.size(), n);
  CHECK(n == 768);
}

static void store_entries_headroom() {
  to_project("SCH");
  printf("scene file: a project's store: %d entries of kMaxEntries %d\n", store::entries(), store::kMaxEntries);
  CHECK(store::entries() < store::kMaxEntries);
  CHECK(store::entries() <= 900 || store::kMaxEntries >= 1536);
}

static void project_switch_saves_old() {
  to_project("SCP");
  kit(0);
  lk(1, "V5:P5", 77);
  scene::set_ab(0, -1);
  run_ms(2);
  CHECK(mod::find(T("V5:P5")));
  to_project("SCQ");
  flush();
  CHECK(rd("/.rytm/scene/SCP/k001.txt").find("L 1 V5:P5 9856") != std::string::npos);
  run_ms(2);
  CHECK(scene::state().s[0].n == 0 && !mod::find(T("V5:P5")));
  printf("scene file: a project switch saves the old project's edit, loads the new one's\n");
}

int main() {
  char tmpl[] = "/tmp/core_scenefile_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  cardq::set_worker(true);
  to_project("SCN");
  round_trip();
  kit_switch_reads_ram_only();
  kit_without_file_uses_project_default();
  unknown_line_locks_file();
  save_mid_morph_stores_knobs();
  fader_move_alone_not_dirty();
  large_file_queued();
  kit_record_with_big_scene_file();
  store_entries_headroom();
  project_switch_saves_old();
  printf("test_scene_file: PASS\n");
  return 0;
}
