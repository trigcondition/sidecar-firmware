// core/tests/test_scene.cpp - morph scenes (core/mod/scene.h, spec-050 section 2) on the modulation layer, on the fake
// clock: the morph table (both sides, one side following the base, stepped targets at halfway with hysteresis, list
// knobs), A == B, offset kinds, the fader's smoothing, the 64-lock limit, an A / B change within kEntries, the index;
// a manual edit wins until the fader moves, the Rytm's reports never hold.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/send_fx.h"
#include "machines.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "ui/page.h"
#include "ui/remote.h"

using namespace core;
using mod::Tid;

static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void run_ms(int ms) {   // the control loop every 250 us: a tick each millisecond
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), mod::tick(now_us());
}
static Tid T(const char* n) {
  Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
static void fader_to(int f) {
  scene::set_fader((uint16_t)f, scene::kConsole);
  run_ms(25);
}
static void report(int cmd, int vi, int machine, const float* p) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(machine | vi)};
  if (cmd == 0x04) m.push_back(60), m.push_back(100);
  for (int k = 0; k < 16; ++k) {
    int iv = (int)p[k];
    m.push_back((uint8_t)iv), m.push_back((uint8_t)((p[k] - iv) * 128));
  }
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void report_p1(int cmd, int vi, float p1) {   // a PLAITS voice's report with P1 = p1, the rest 10
  float p[16];
  for (float& x : p) x = 10;
  p[0] = p1;
  report(cmd, vi, kPlaits, p);
}
static void scene_reset_with_base(float b) {   // nothing modulated, no scenes; V1's P1 plays b
  mod::reset();
  scene::reset();
  v[0].p[0] = b;
  at(g_us + 1000), mod::tick(now_us());   // (the first call only starts the clock)
}
static void lock_ab(Tid t, float a, float b) {
  CHECK(scene::lock(0, t, (uint16_t)(a * 128)));
  CHECK(scene::lock(1, t, (uint16_t)(b * 128)));
  scene::set_ab(0, 1);
}

static void morph_table() {
  scene_reset_with_base(40);
  scene::lock(0, T("V1:P1"), 20 * 128);
  scene::lock(1, T("V1:P1"), 100 * 128);
  scene::set_ab(0, 1);
  fader_to(0);
  CHECK(v[0].p[0] == 20.0f);
  fader_to(8192);
  CHECK(fabsf(v[0].p[0] - 60.0f) < 0.01f);   // f = 0.50003
  fader_to(16383);
  CHECK(v[0].p[0] == 100.0f);
  CHECK(scene::unlock(1, T("V1:P1")));
  fader_to(16383);
  CHECK(v[0].p[0] == 40.0f);   // A only: toward the base
  fader_to(4096);
  CHECK(fabsf(v[0].p[0] - 25.0f) < 0.01f);   // 20 + (40 - 20) x 0.25
  scene::clear(0);
  run_ms(2);
  CHECK(v[0].p[0] == 40.0f && !mod::find(T("V1:P1")));   // nothing locks it: back to its base, the entry gone
  printf("scene: the morph table (both sides, one side toward the base, nothing)\n");
}

static void one_sided_follows_base() {   // B only; the Rytm's 0x03 moves the base under it (PLAITS's P3: no list)
  scene_reset_with_base(40);
  report_p1(0x03, 0, 3);
  v[0].p[2] = 40;
  scene::lock(1, T("V1:P3"), 100 * 128);
  scene::set_ab(0, 1);
  fader_to(8192);
  CHECK(fabsf(v[0].p[2] - 70.0f) < 0.01f);
  float p[16];
  for (int k = 0; k < 16; ++k) p[k] = v[0].p[k];
  p[0] = 3, p[2] = 80;
  report(0x03, 0, kPlaits, p);
  CHECK(fabsf(v[0].p[2] - 70.0f) < 0.01f);   // the report moves the base only: the morph plays on
  run_ms(2);                                 // the next tick's morph is on the new base
  CHECK(fabsf(v[0].p[2] - 90.0f) < 0.01f && mod::find(T("V1:P3"))->base == 80.0f);
  printf("scene: a one-sided lock follows the base (a 0x03 under it)\n");
}

static float played(Tid t) {
  const mod::Entry* e = mod::find(t);
  return e ? e->applied : -1;
}
static void stepped_halfway_hysteresis() {   // T1:SPD (kSeq: step mode, no writer yet): 0 in A, 2 in B
  scene_reset_with_base(40);
  lock_ab(T("T1:SPD"), 0, 2);
  fader_to((int)(0.50f * 16383));
  CHECK(played(T("T1:SPD")) == 0);
  fader_to((int)(0.52f * 16383));
  CHECK(played(T("T1:SPD")) == 2);   // 0.52 >= 0.5156
  fader_to((int)(0.49f * 16383));
  CHECK(played(T("T1:SPD")) == 2);   // 0.49 is not < 0.4844
  fader_to((int)(0.48f * 16383));
  CHECK(played(T("T1:SPD")) == 0);
  // one side only (B): the base (0 for a kind without one yet) below, B above, the same switch points
  scene::unlock(0, T("T1:SPD"));
  fader_to((int)(0.51f * 16383));
  CHECK(played(T("T1:SPD")) == 0);
  fader_to((int)(0.52f * 16383));
  CHECK(played(T("T1:SPD")) == 2);
  fader_to((int)(0.49f * 16383));
  CHECK(played(T("T1:SPD")) == 2);
  printf("scene: a stepped target switches at halfway, with 1/64 hysteresis\n");
}

static void list_knob_steps() {   // PLAITS's list knob (its MODEL) on V1: 3 in A, 7 in B, never in between
  scene_reset_with_base(40);
  report_p1(0x03, 0, 3);
  int k = -1;
  for (int i = 0; i < 16 && k < 0; ++i)
    if (ui::remote_is_list(0, i)) k = i;
  CHECK(k >= 0);
  CHECK(!ui::remote_is_list(0, 15) || k != 15);
  char nm[16];
  snprintf(nm, sizeof nm, "V1:P%d", k + 1);
  lock_ab(T(nm), 3, 7);
  bool saw3 = false, saw7 = false;
  for (int f = 0; f <= 16383; f += 512) {
    fader_to(f);
    CHECK(v[0].p[k] == 3.0f || v[0].p[k] == 7.0f);
    saw3 |= v[0].p[k] == 3.0f, saw7 |= v[0].p[k] == 7.0f;
  }
  CHECK(saw3 && saw7);
  printf("scene: a list knob (V1:P%d) steps, never through the values between\n", k + 1);
}

static void a_equals_b() {
  scene_reset_with_base(40);
  scene::lock(3, T("V1:P1"), 77 * 128);
  scene::set_ab(3, 3);
  for (int f : {0, 5000, 16383}) {
    fader_to(f);
    CHECK(v[0].p[0] == 77.0f);
  }
  scene::set_ab(-1, 3);   // no A: the missing side is "neither" for all (B only)
  fader_to(0);
  CHECK(v[0].p[0] == 40.0f);
  printf("scene: A == B plays its locks whatever the fader\n");
}

static void offset_kind_morph() {   // MFX:LEV, knob 100; A 60, B 120: the played LEV, the knob untouched
  scene_reset_with_base(40);
  master::set_lev(100);
  lock_ab(T("MFX:LEV"), 60, 120);
  fader_to(8192);
  CHECK(abs(master::played_lev() - 90) <= 1 && master::control.lev == 100);
  fader_to(16383);
  CHECK(master::played_lev() == 120 && master::control.lev == 100);
  scene::clear(0), scene::clear(1);
  run_ms(2);
  CHECK(master::played_lev() == 100);
  printf("scene: an offset kind (MFX:LEV) plays the morph, its knob untouched\n");
}

static void fader_smoothing_20ms() {
  scene_reset_with_base(40);
  scene::set_fader(0, scene::kConsole);
  run_ms(30);
  scene::set_fader(16383, scene::kConsole);
  CHECK(scene::state().fader == 16383);
  run_ms(10);
  printf("scene: the played fader 10 ms after 0 -> 16383: %d\n", scene::fader_played());
  CHECK(abs((int)scene::fader_played() - 8192) < 600);
  run_ms(11);
  CHECK(scene::fader_played() == 16383);
  printf("scene: the fader is smoothed over 20 ms\n");
}

static void sixty_fifth_lock_refused() {
  scene_reset_with_base(40);
  for (int k = 0; k < 64; ++k) CHECK(scene::lock(0, mod::tid(mod::kV, k / 16, k % 16), 0));
  CHECK(!scene::lock(0, T("FX1:P2"), 0));
  CHECK(scene::lock(0, T("V1:P1"), 5) && scene::state().s[0].n == 64);   // its own lock again: replaced, not refused
  CHECK(!scene::lock(12, T("V1:P1"), 5) && !scene::lock(1, 0, 5) && !scene::lock(1, T("V1:P1"), 16384));
  printf("scene: the 65th lock is refused\n");
}

// 64 locks in each of scenes 0, 1, 2 on different targets, 8 LFO-like and 32 route-like targets from a source
static void src_40(uint32_t, uint32_t) {
  for (int i = 0; i < 40; ++i) mod::offset(i < 8 ? i : 8 + i, mod::tid(mod::kIns, i / 12, i % 12), 1, 0.5f);
}
static Tid lock_target(int sc, int k) {   // scene 0: V1..V4's P1..P16, 1: V5..V8's, 2: I5..I10's
  if (sc == 0) return mod::tid(mod::kV, k / 16, k % 16);
  if (sc == 1) return mod::tid(mod::kV, 4 + k / 16, k % 16);
  return mod::tid(mod::kIns, 4 + k / 12, k % 12);
}
static void ab_switch_full_scenes() {
  scene_reset_with_base(40);
  ins::reset();
  mod::set_source_fn(src_40);
  for (int sc = 0; sc < 3; ++sc)
    for (int k = 0; k < 64; ++k) CHECK(scene::lock(sc, lock_target(sc, k), (uint16_t)(30 * 128)));
  scene::set_ab(0, 1);
  run_ms(2);
  CHECK(mod::find(lock_target(1, 5)) && mod::find(lock_target(0, 63)));
  scene::set_ab(0, 2);
  run_ms(2);
  CHECK(mod::stats().dropped == 0);
  CHECK(!mod::find(lock_target(1, 5)) && mod::find(lock_target(2, 63)));   // scene 1's targets back at their bases
  char o[200];
  mod::command("mod=stats", o, sizeof o);
  printf("scene: an A / B change of full scenes: %s\n", o);
  mod::set_source_fn(nullptr);
  ins::reset();
}

static void index_finds_after_deletes() {   // the 512-slot index: 200 targets in, every other one out
  mod::reset();
  scene::reset();
  static int phase;
  phase = 0;
  mod::set_source_fn([](uint32_t, uint32_t) {
    for (int i = 0; i < 200; ++i)
      if (phase == 0 || i % 2) mod::offset(0, i < 128 ? mod::tid(mod::kV, i / 16, i % 16) : mod::tid(mod::kIns, (i - 128) / 12, (i - 128) % 12), 0, 1);
  });
  at(g_us + 1000), mod::tick(now_us());
  run_ms(2);
  for (int i = 0; i < 200; ++i) CHECK(mod::find(i < 128 ? mod::tid(mod::kV, i / 16, i % 16) : mod::tid(mod::kIns, (i - 128) / 12, (i - 128) % 12)));
  phase = 1;
  run_ms(2);
  for (int i = 0; i < 200; ++i) {
    Tid t = i < 128 ? mod::tid(mod::kV, i / 16, i % 16) : mod::tid(mod::kIns, (i - 128) / 12, (i - 128) % 12);
    CHECK((mod::find(t) != nullptr) == (i % 2 == 1));
  }
  phase = 0;   // the even ones back: into the free entries, found again
  run_ms(2);
  for (int i = 0; i < 200; ++i) CHECK(mod::find(i < 128 ? mod::tid(mod::kV, i / 16, i % 16) : mod::tid(mod::kIns, (i - 128) / 12, (i - 128) % 12)));
  mod::set_source_fn(nullptr);
  mod::reset();
  ins::reset();
  printf("scene: the target index finds every entry after deletes\n");
}

// ---- Task 8: a manual edit wins until the fader moves (spec-050 2.5); the Rytm's reports never hold
static void msg3(uint8_t cmd, int a, int b, int c) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, (uint8_t)a, (uint8_t)b, (uint8_t)c, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static uint32_t g_ms = 1000;
static void open_v1_page() {   // the remote page on V1 (PLAITS): its encoder B is HRM (P2)
  ui::list_pacing(false);
  msg3(0x30, 0, 1, 0);
  ui::remote_tick(g_ms += 10);
  for (int k = 0; k < 8 && ui::remote_stats().page != 1; ++k)
    msg3(0x31, 0, 0x41, 1), msg3(0x31, 0, 0x41, 0), ui::remote_tick(g_ms += 30);
  CHECK(ui::remote_stats().page == 1 && ui::remote_stats().voice == 0);
}
static void close_page() {
  msg3(0x30, 0, 0, 1);
  ui::list_pacing(true);
}
static void manual_wins_until_fader_moves() {
  scene_reset_with_base(40);
  report_p1(0x03, 0, 3);
  v[0].p[1] = 40;
  lock_ab(T("V1:P2"), 20, 100);
  fader_to(8192);
  CHECK(fabsf(v[0].p[1] - 60.0f) < 0.01f);
  open_v1_page();
  uint32_t before = ui::remote_stats().edits;
  msg3(0x32, 1, 64 + 30, 0);   // B +30: HRM from its base 40 to 70, a page edit
  ui::remote_tick(g_ms += 10);
  CHECK(ui::remote_stats().edits > before);
  CHECK(v[0].p[1] == 70.0f && mod::find(T("V1:P2"))->held == 1);
  run_ms(5);
  CHECK(v[0].p[1] == 70.0f && mod::find(T("V1:P2"))->base == 70.0f);   // the edit is the base too (its 0x03)
  fader_to(8193);
  CHECK(fabsf(v[0].p[1] - 60.0f) < 0.01f && mod::find(T("V1:P2"))->held == 0);   // the morph takes it again
  close_page();
  printf("scene: a page edit wins until the fader moves\n");
}
static void rytm_reports_never_hold() {
  scene_reset_with_base(40);
  report_p1(0x03, 0, 3);
  v[0].p[1] = 40;
  lock_ab(T("V1:P2"), 20, 100);
  fader_to(8192);
  float p[16];
  for (int k = 0; k < 16; ++k) p[k] = v[0].p[k];
  p[0] = 3, p[1] = 70;
  report(0x04, 0, kPlaits, p);   // a trig with a p-lock
  p[1] = 72;
  report(0x03, 0, kPlaits, p);   // the Rytm's knob / its own LFO / its scene
  run_ms(2);
  const mod::Entry* e = mod::find(T("V1:P2"));
  CHECK(e && e->held == 0 && e->base == 72.0f && fabsf(v[0].p[1] - 60.0f) < 0.01f);
  // a 0x20 under an FX1:P3 lock: the base, never held
  lock_ab(T("FX1:P3"), 10, 30);
  run_ms(2);
  const uint8_t sx[] = {0xF0, 0x7D, 0x52, 0x54, 0x20, 0x01, 12, 2, 30, 1, 10, 20, 50, 64, 0, 0, 0, 0, 0, 0xF7};
  CHECK(fx::take_sendfx(sx, (int)sizeof sx));
  run_ms(2);
  e = mod::find(T("FX1:P3"));
  CHECK(e && e->held == 0 && e->base == 50.0f && fx::control.knob[0][3] == 20);
  printf("scene: the Rytm's 0x03 / 0x04 / 0x20 move the base, never hold\n");
}
static void page_edit_holds_offset_kind() {   // MFX:LEV: a console / page edit holds, a file load does not
  scene_reset_with_base(40);
  master::set_lev(100);
  lock_ab(T("MFX:LEV"), 60, 120);
  fader_to(8192);
  CHECK(abs(master::played_lev() - 90) <= 1);
  char o[200];
  CHECK(master::command("mfx=lev:30", o, sizeof o));
  run_ms(2);
  CHECK(master::control.lev == 30 && master::played_lev() == 30 && mod::find(T("MFX:LEV"))->held == 1);
  fader_to(8193);
  CHECK(abs(master::played_lev() - 90) <= 1 && mod::find(T("MFX:LEV"))->held == 0);
  // the knob set by a file's line (a kit / project load): not a manual edit
  const char* w[] = {"MFX", "0", "RET", "40"};
  master::from_line(w, 4);
  run_ms(2);
  CHECK(master::control.lev == 40 && mod::find(T("MFX:LEV"))->held == 0 && abs(master::played_lev() - 90) <= 1);
  master::set_lev(100);
  printf("scene: a console edit of an offset kind holds; a file load does not\n");
}

static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}

int main() {
  char tmpl[] = "/tmp/core_scene_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'C', 'N', 0xF7};
  engine_sysex(proj, sizeof proj);
  loaded();
  morph_table();
  one_sided_follows_base();
  stepped_halfway_hysteresis();
  list_knob_steps();
  a_equals_b();
  offset_kind_morph();
  fader_smoothing_20ms();
  sixty_fifth_lock_refused();
  ab_switch_full_scenes();
  index_finds_after_deletes();
  manual_wins_until_fader_moves();
  rytm_reports_never_hold();
  page_edit_holds_offset_kind();
  printf("test_scene: PASS\n");
  return 0;
}
