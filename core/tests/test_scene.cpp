// core/tests/test_scene.cpp - morph scenes (core/mod/scene.h, spec-050 section 2) on the modulation layer, on the fake
// clock: the morph table (both sides, one side following the base, stepped targets at halfway with hysteresis, list
// knobs), A == B, offset kinds, the fader's smoothing, the 64-lock limit, an A / B change within kEntries, the index;
// a manual edit wins until the fader moves, the Rytm's reports never hold; the QPERF knob as the fader (2D 01 0E) and the
// Rytm's scene reported (0F) (plan-050 Task 47).
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
#include "hooks.h"
#include "machines.h"
#include "mod/learn.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
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

// review M3: an A / B change from two full scenes to two other full scenes, with 40 source entries: 40 + 4 x 64 = 296 >
// kEntries 256 - only the old scenes' targets released before the new ones are allocated keep it in the table
static void src_40(uint32_t, uint32_t) {   // 8 LFO-like and 32 route-like targets: I11..I13 (36) and O1:LEV..O4:LEV
  for (int i = 0; i < 40; ++i)
    mod::offset(i < 8 ? i : 8 + i, i < 36 ? mod::tid(mod::kIns, 10 + i / 12, i % 12) : mod::tid(mod::kOut, i - 36, 0), 1, 0.5f);
}
static Tid lock_target(int sc, int k) {   // 0: V1..V4, 1: V5..V8, 2: I1..I5 (first 64), 3: I6..I10 + O5..O7 (64)
  if (sc == 0) return mod::tid(mod::kV, k / 16, k % 16);
  if (sc == 1) return mod::tid(mod::kV, 4 + k / 16, k % 16);
  if (sc == 2) return mod::tid(mod::kIns, k / 12, k % 12);
  int n = 64 + k;   // I6:A1 .. (chains 5..9: 60 params), then O5..O7
  return n < 120 ? mod::tid(mod::kIns, n / 12, n % 12) : mod::tid(mod::kOut, 4 + (n - 120) / 3, (n - 120) % 3);
}
static void ab_switch_full_scenes() {
  scene_reset_with_base(40);
  ins::reset();
  mod::set_source_fn(src_40);
  for (int sc = 0; sc < 4; ++sc)
    for (int k = 0; k < 64; ++k) CHECK(scene::lock(sc, lock_target(sc, k), (uint16_t)(30 * 128)));
  scene::set_ab(0, 1);
  run_ms(2);
  CHECK(mod::find(lock_target(1, 5)) && mod::find(lock_target(0, 63)) && mod::stats().dropped == 0);
  scene::set_ab(2, 3);
  run_ms(2);
  char o[200];
  mod::command("mod=stats", o, sizeof o);
  printf("scene: an A / B change of full scenes to other full scenes: %s\n", o);
  CHECK(mod::stats().dropped == 0);   // (without the release: 296 entries wanted in one tick)
  CHECK(!mod::find(lock_target(0, 5)) && !mod::find(lock_target(1, 63)) && mod::find(lock_target(2, 0)) &&
        mod::find(lock_target(3, 63)));
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

// ---- Task 11: the console and /status (spec-050 2.10)
static std::string say(const char* line) {
  char o[600];
  CHECK(scene::command(line, o, sizeof o));
  return o;
}
static void console_round_trip() {
  scene_reset_with_base(40);
  CHECK(say("scene").find("scene: A 0 B 0 fader 0") == 0);
  CHECK(say("scene=lock:3:V1:P3:50").find("S3 1 lock") != std::string::npos);
  CHECK(scene::state().s[2].n == 1 && scene::state().s[2].l[0].t == T("V1:P3") && scene::state().s[2].l[0].v == 6400);
  say("scene=lock:3:MFX:LEV:100.5");
  CHECK(scene::state().s[2].l[1].t == T("MFX:LEV") && scene::state().s[2].l[1].v == 12864);
  say("scene=lock:7:V2:P1:5");
  std::string s = say("scene=ab:3:7");
  CHECK(s.find("A 3 B 7") != std::string::npos && scene::state().a == 2 && scene::state().b == 6);
  CHECK(say("scene=fader:8192").find("fader 8192") != std::string::npos && scene::state().fader == 8192);
  CHECK(say("scene=sel:7").find("sel 7") != std::string::npos && scene::state().sel == 6);
  say("scene=copy:3:5");
  CHECK(scene::state().s[4].n == 2);
  CHECK(say("scene=unlock:5:V1:P3").find("S5 1 lock") != std::string::npos && scene::state().s[4].n == 1);
  say("scene=clear:5");
  CHECK(scene::state().s[4].n == 0);
  // v2: EDIT is the held gesture (PAGE + T15 / T16), never latched: no console line for it
  CHECK(say("scene=edit:on").find("cannot") != std::string::npos && !scene::edit());
  for (const char* bad : {"scene=ab:13:1", "scene=lock:1:ZZ1:Q:3", "scene=fader:20000", "scene=lock:1:V1:P1:200",
                          "scene=sel:0", "scene=unlock:9:V1:P1", "scene=edit:maybe", "scene=what"})
    CHECK(say(bad).find("cannot") != std::string::npos);
  for (int k = 0; k < 64; ++k) scene::lock(9, mod::tid(mod::kV, k / 16, k % 16), 0);
  CHECK(say("scene=lock:10:FX1:P2:3").find("SCENE FULL") != std::string::npos);
  char o[200];
  CHECK(!scene::command("scenex", o, sizeof o) && !scene::command("mod", o, sizeof o));
  // the JSON: the fields, the scenes with locks only, closed; a full 768-lock state fits 16 KB
  static char js[16384];
  int n = scene::json(js, sizeof js);
  CHECK(n > 0 && n < (int)sizeof js);
  std::string j(js, (size_t)n);
  printf("scene: json %.160s...\n", js);
  CHECK(j.find("\"scene\":{\"a\":3,\"b\":7,\"fader\":8192,") == 0 && j.find("\"sel\":7,\"locked\":0,\"edit\":0,\"s\":[") != std::string::npos);
  CHECK(j.find("{\"n\":3,\"l\":[[\"V1:P3\",6400],[\"MFX:LEV\",12864]]}") != std::string::npos && j.find("{\"n\":5,") == std::string::npos);
  CHECK(j.compare(j.size() - 11, 11, ",\"rscn\":-1}") == 0 && j.find("],\"src\":") != std::string::npos);   // Task 47
  for (int sc = 0; sc < scene::kScenes; ++sc) {   // 768 distinct locks, the longest names: RT1x:SFRQ.., MFX:MIX.., S1x:Fn
    scene::clear(sc);
    for (int k = 0; k < scene::kLocks; ++k) {
      Tid t = k < 13 ? mod::rytm_tid(10 + k % 2, 25 + k) : k < 18 ? mod::tid(mod::kMfx, 0, k - 13) : k < 42 ? mod::tid(mod::kSnd, (k - 18) / 2, k % 2)
                                                                                               : mod::tid(mod::kIsnd, (k - 42) / 3, (k - 42) % 3);
      CHECK(scene::lock(sc, t, 16383));
    }
    CHECK(scene::state().s[sc].n == 64);
  }
  n = scene::json(js, sizeof js);
  printf("scene: json of the fullest state: %d bytes\n", n);
  CHECK(n > 0 && n < (int)sizeof js);
  scene::reset();
  printf("scene: the console's lines round trip; the JSON\n");
}

// ---- plan-050 Task 47: the QPERF knob (2D 01 0E <pos hi> <pos lo> <mask lo7> <mask hi5> <flags>), the Rytm's scene (0F)
static void qp(int pos, int mask, int flags) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x0E, (uint8_t)(pos >> 7), (uint8_t)(pos & 0x7F),
                       (uint8_t)(mask & 0x7F), (uint8_t)((mask >> 7) & 0x1F), (uint8_t)flags, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static void qperf_sets_fader() {
  scene_reset_with_base(40);
  scene::set_qp_mode(scene::kQpAlways);   // (Task 47's rules: the knob always the fader)
  qp(0, 0, 2);     // the first report: a baseline (M2)
  qp(255, 0, 2);
  CHECK(scene::state().fader == 16383);
  qp(0, 0, 2);
  CHECK(scene::state().fader == 0);
  qp(128, 0, 3);   // FUNC held: the same knob
  CHECK(scene::state().fader == 128 * 16383 / 255);
  run_ms(30);
  CHECK(scene::fader_played() == 128 * 16383 / 255);
  printf("scene: the QPERF report sets the fader (pos x 16383 / 255)\n");
}
static void qperf_ignored_until_moved() {
  scene_reset_with_base(40);
  scene::set_qp_mode(scene::kQpAlways);   // (Task 47's rules: the knob always the fader)
  fader_to(9000);
  qp(0, 0, 0);   // the hello's: the knob has not sent since the Rytm booted
  qp(77, 0, 0);
  CHECK(scene::state().fader == 9000);
  qp(77, 0, 2);    // the first position: a baseline
  CHECK(scene::state().fader == 9000);
  qp(80, 0, 2);
  CHECK(scene::state().fader == 80 * 16383 / 255);
  printf("scene: the QPERF position is ignored until flags bit 1 (moved since boot)\n");
}
static void qperf_smoothing_follows_interval() {
  scene_reset_with_base(40);
  scene::set_qp_mode(scene::kQpAlways);   // (Task 47's rules: the knob always the fader)
  qp(0, 0, 2);
  run_ms(200);
  // reports 16 ms apart, 4 positions each (257 fader counts): the played fader never jumps by a report's change,
  // never stands still between reports
  int last = scene::fader_played(), max_step = 0, flat = 0;
  for (int k = 1; k <= 20; ++k) {
    qp(4 * k, 0, 2);
    for (int ms = 0; ms < 16; ++ms) {
      run_ms(1);
      int p = scene::fader_played();
      max_step = p - last > max_step ? p - last : max_step;
      flat += p == last;
      last = p;
    }
  }
  printf("scene: QPERF reports 16 ms apart: the largest 1 ms step %d (a report's change 257), %d flat ms\n", max_step,
         flat);
  CHECK(max_step <= 257 / 8 && flat == 0);
  // 40 ms apart (a slow turn): smoothed over the interval, not 20 ms
  last = scene::fader_played(), flat = 0;
  for (int k = 21; k <= 26; ++k) {
    qp(4 * k, 0, 2);
    for (int ms = 0; ms < 40; ++ms) {
      run_ms(1);
      int p = scene::fader_played();
      flat += k > 21 && p == last;
      last = p;
    }
  }
  printf("scene: QPERF reports 40 ms apart: %d flat ms\n", flat);
  CHECK(flat <= 5);
  // a console move after: a source change, so the 200 ms handover glide (v2 Task 6), not a jump
  scene::set_fader(0, scene::kConsole);
  run_ms(21);
  CHECK(scene::fader_played() > 0);
  run_ms(180);
  CHECK(scene::fader_played() == 0);
  printf("scene: the QPERF fader is smoothed over the reports' interval (at least 20 ms)\n");
}
// Phase 7 review M2: the first report after each hello is a baseline only, whatever the source; a later change takes
static void hello35() {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static void qperf_baseline_after_hello() {
  scene_reset_with_base(40);
  scene::set_qp_mode(scene::kQpAlways);   // (Task 47's rules: the knob always the fader)
  scene::set_fader(3000, scene::kPage);   // (a kit file's F does the same: the fader set, the knob not its source)
  hello35();
  qp(200, 0, 2);   // the hello's report: the knob at 200 since the Rytm booted, bit 1 set
  run_ms(25);
  CHECK(scene::state().fader == 3000 && scene::fader_source() == scene::kPage);
  qp(200, 0x003, 2);   // the mask changed, the position did not
  CHECK(scene::state().fader == 3000 && scene::fader_source() == scene::kPage);
  qp(203, 0, 2);   // the knob moved: it takes the fader
  CHECK(scene::state().fader == 203 * 16383 / 255 && scene::fader_source() == scene::kQperf);
  // the link drops and comes back while the knob is the source: the next report a baseline again
  hello35();
  scene::set_fader(5000, scene::kPage);
  qp(90, 0, 2);
  CHECK(scene::state().fader == 5000 && scene::fader_source() == scene::kPage);
  qp(93, 0, 2);
  CHECK(scene::state().fader == 93 * 16383 / 255 && scene::fader_source() == scene::kQperf);
  printf("scene: the first QPERF report after a hello is a baseline; a change takes the fader\n");
}
// Phase 7 review M1: a 1-step flicker never steals the fader from another source nor lets a held edit go; 2 steps do;
// once the knob has the fader it follows every step
static void qperf_jitter_deadband() {
  scene_reset_with_base(40);
  scene::set_qp_mode(scene::kQpAlways);   // (Task 47's rules: the knob always the fader)
  qp(100, 0, 2);   // baseline
  scene::set_fader(4000, scene::kPage);
  for (int k = 0; k < 50; ++k) qp(k & 1 ? 101 : 100, 0, 2), run_ms(8);
  CHECK(scene::state().fader == 4000 && scene::fader_source() == scene::kPage);
  qp(99, 0, 2), run_ms(8);
  CHECK(scene::state().fader == 4000 && scene::fader_source() == scene::kPage);
  qp(102, 0, 2);   // 2 steps from 100: the knob takes it
  CHECK(scene::state().fader == 102 * 16383 / 255 && scene::fader_source() == scene::kQperf);
  qp(103, 0, 2);   // the knob's own: every step
  CHECK(scene::state().fader == 103 * 16383 / 255);
  qp(102, 0, 2);
  CHECK(scene::state().fader == 102 * 16383 / 255);
  printf("scene: a 1-step QPERF flicker does not steal a page's fader; 2 steps do; then every step\n");

  // a held manual edit (the knob the fader's source): a flicker does not let it go, 2 steps do
  scene_reset_with_base(40);
  scene::set_qp_mode(scene::kQpAlways);   // (Task 47's rules: the knob always the fader)
  report_p1(0x03, 0, 3);
  v[0].p[1] = 40;
  lock_ab(T("V1:P2"), 20, 100);
  qp(100, 0, 2);   // baseline
  qp(128, 0, 2);   // the knob takes the fader (about halfway)
  run_ms(150);
  CHECK(scene::fader_source() == scene::kQperf);
  open_v1_page();
  msg3(0x32, 1, 64 + 5, 0);   // a page edit: held
  ui::remote_tick(g_ms += 10);
  CHECK(mod::find(T("V1:P2"))->held == 1);
  float held = v[0].p[1];
  for (int k = 0; k < 20; ++k) qp(k & 1 ? 128 : 129, 0, 2), run_ms(8);
  CHECK(mod::find(T("V1:P2"))->held == 1 && v[0].p[1] == held);
  qp(130, 0, 2);   // 2 steps from 128: the morph takes it again
  run_ms(150);
  CHECK(mod::find(T("V1:P2"))->held == 0 && scene::state().fader == 130 * 16383 / 255);
  close_page();
  printf("scene: a 1-step QPERF flicker does not let a held edit go; 2 steps do\n");
}
// ---- 0.5.0 v2 Task 6: QPERF FADER (MUTED ONLY by default / OFF / ALWAYS), the mute edge, the handover glide
static void hello_caps2(int c2) {   // a hello whose caps2 is c2 (kCaps2QpMute: the mod reports the macros' mute, bit 2)
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, (uint8_t)c2, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static uint16_t qf(int pos) { return (uint16_t)(pos * 16383 / 255); }
static void qp_muted_only_ignores_unmuted() {
  scene_reset_with_base(40);
  CHECK(scene::qp_mode() == scene::kQpMutedOnly);   // the default
  hello_caps2(ui::kCaps2QpMute);
  scene::set_fader(3000, scene::kPage);
  qp(100, 0, 2), qp(200, 0, 2), qp(10, 0, 2), qp(250, 0, 3);
  CHECK(scene::state().fader == 3000 && scene::fader_source() == scene::kPage);
  printf("scene: QPERF FADER MUTED ONLY: the knob with the macros playing never moves the fader\n");
}
static void qp_muted_only_takes_when_muted() {
  scene_reset_with_base(40);
  hello_caps2(ui::kCaps2QpMute);
  scene::set_fader(3000, scene::kPage);
  qp(100, 0, 2 | 4);   // muted: the first position a baseline
  CHECK(scene::state().fader == 3000);
  qp(150, 0, 2 | 4);
  CHECK(scene::state().fader == qf(150) && scene::fader_source() == scene::kQperf);
  qp(151, 0, 2 | 4);   // then every step
  CHECK(scene::state().fader == qf(151));
  printf("scene: QPERF FADER MUTED ONLY: muted, the knob is the fader\n");
}
static void qp_mute_edge_is_baseline() {
  scene_reset_with_base(40);
  hello_caps2(ui::kCaps2QpMute);
  qp(100, 0, 6), qp(120, 0, 6);
  CHECK(scene::state().fader == qf(120));
  qp(200, 0, 2);   // unmuted: the macros again, the fader stays
  CHECK(scene::state().fader == qf(120));
  qp(200, 0, 6);   // muted again at 200: a baseline, no jump to 200
  CHECK(scene::state().fader == qf(120));
  qp(203, 0, 6);
  CHECK(scene::state().fader == qf(203));
  // ALWAYS: the mute edge is a baseline as well (the knob's position under a mute toggle is no move)
  scene::set_qp_mode(scene::kQpAlways);
  qp(60, 0, 2);   // (unmuted after muted: an edge too, a baseline)
  CHECK(scene::state().fader == qf(203));
  qp(62, 0, 2);
  CHECK(scene::state().fader == qf(62));
  qp(90, 0, 6);   // the edge: a baseline only
  CHECK(scene::state().fader == qf(62));
  qp(92, 0, 6);
  CHECK(scene::state().fader == qf(92));
  printf("scene: a QPERF mute edge makes the next position a baseline\n");
}
static void qp_off_never_moves() {
  scene_reset_with_base(40);
  hello_caps2(ui::kCaps2QpMute);
  char o[200];
  CHECK(scene::command("scene=qperf:off", o, sizeof o) && strstr(o, "qperf off"));
  CHECK(scene::qp_mode() == scene::kQpOff);
  scene::set_fader(3000, scene::kPage);
  for (int k = 0; k < 10; ++k) qp(k * 25, 0, k & 1 ? 6 : 2);
  CHECK(scene::state().fader == 3000 && scene::fader_source() == scene::kPage);
  CHECK(scene::command("scene=qperf:always", o, sizeof o) && scene::qp_mode() == scene::kQpAlways);
  CHECK(scene::command("scene=qperf:muted", o, sizeof o) && scene::qp_mode() == scene::kQpMutedOnly);
  CHECK(scene::command("scene=qperf:loud", o, sizeof o) && strstr(o, "cannot") && scene::qp_mode() == scene::kQpMutedOnly);
  printf("scene: QPERF FADER OFF: the knob never moves the fader; scene=qperf:muted|off|always\n");
}
static void qp_always_as_before() {
  scene_reset_with_base(40);
  hello_caps2(0);
  scene::set_qp_mode(scene::kQpAlways);
  scene::set_fader(3000, scene::kPage);
  qp(100, 0, 2);
  CHECK(scene::state().fader == 3000);
  qp(150, 0, 2);
  CHECK(scene::state().fader == qf(150) && scene::fader_source() == scene::kQperf);
  qp(170, 0, 6);   // (no caps2: bit 2 means nothing, no edge)
  CHECK(scene::state().fader == qf(170));
  printf("scene: QPERF FADER ALWAYS: the knob is the fader whatever the macros (Task 47's rules)\n");
}
static void qp_muted_only_without_caps_is_off() {
  scene_reset_with_base(40);
  hello_caps2(0);   // an older mod: no mute report, bit 2 not to be trusted
  scene::set_fader(3000, scene::kPage);
  for (int k = 0; k < 10; ++k) qp(k * 25, 0, 2 | 4);
  CHECK(scene::state().fader == 3000 && scene::fader_source() == scene::kPage);
  printf("scene: MUTED ONLY with a mod without the mute report acts as OFF\n");
}
// a source change glides over kHandoverMs: linear, no step above a 200th of the travel (+1)
static int glide_max_step(int ms, int* last) {
  int mx = 0;
  for (int k = 0; k < ms; ++k) {
    run_ms(1);
    int p = scene::fader_played(), d = p > *last ? p - *last : *last - p;
    mx = d > mx ? d : mx;
    *last = p;
  }
  return mx;
}
static void handover_glides_200ms() {
  scene_reset_with_base(40);
  scene::set_qp_mode(scene::kQpAlways);
  scene::set_fader(1000, scene::kLearn);
  run_ms(30);
  qp(10, 0, 2);    // baseline
  qp(250, 0, 2);   // the knob takes it from learn: 1000 -> 16061 over 200 ms
  int last = scene::fader_played();
  int mx = glide_max_step(100, &last);
  int mid = scene::fader_played();
  printf("scene: learn -> QPERF: at 100 ms %d (from 1000 to %d), largest 1 ms step %d\n", mid, qf(250), mx);
  CHECK(mx <= 16383 / 200 + 1 && std::abs(mid - (1000 + qf(250)) / 2) <= 100);
  glide_max_step(101, &last);
  CHECK(scene::fader_played() == qf(250));
  // QPERF -> page: the same
  scene::set_fader(0, scene::kPage);
  mx = glide_max_step(100, &last);
  CHECK(mx <= 16383 / 200 + 1 && scene::fader_played() > 0);
  glide_max_step(101, &last);
  CHECK(scene::fader_played() == 0);
  printf("scene: a change of the fader's source glides over 200 ms\n");
}
static void same_source_keeps_20ms() {
  scene_reset_with_base(40);
  scene::set_fader(1000, scene::kPage);
  run_ms(25);
  scene::set_fader(9000, scene::kPage);
  run_ms(10);
  CHECK(scene::fader_played() > 1000 && scene::fader_played() < 9000);
  run_ms(11);
  CHECK(scene::fader_played() == 9000);
  printf("scene: the same source keeps the 20 ms smoothing\n");
}
static std::vector<std::vector<uint8_t>> g_rytm;
static void rytm_cap(const uint8_t* m, int n) { g_rytm.emplace_back(m, m + n); }
static int ccs(uint8_t c) {
  int k = 0;
  for (const auto& o : g_rytm) k += o.size() == 3 && (o[0] & 0xF0) == 0xB0 && o[1] == c;
  return k;
}
// 0.5.0 v2 (S7): the Rytm's own scene and PERF macros are no scene targets (XY and MIDI learn keep them)
static void rytm_targets_refused() {
  scene_reset_with_base(40);
  CHECK(scene::lock(2, T("V1:P1"), 128));
  CHECK(!scene::lock(2, T("RYTM:SCN"), 5 * 128) && !scene::lock(2, T("RYTM:PERF3"), 127 * 128));
  CHECK(scene::state().s[2].n == 1 && scene::state().s[2].l[0].t == T("V1:P1"));
  CHECK(say("scene=lock:3:RYTM:PERF3:127").find("NOT A SCENE TARGET") != std::string::npos && scene::state().s[2].n == 1);
  printf("scene: RYTM:SCN / RYTM:PERFn are refused as locks (NOT A SCENE TARGET)\n");
}
static void rytm_targets_still_learnable() {
  scene_reset_with_base(40);
  learn::reset();
  hooks.to_rytm = rytm_cap;
  char o[400];
  CHECK(learn::command("learn=RYTM:PERF3:63", o, sizeof o) && !strstr(o, "cannot"));
  g_rytm.clear();
  CHECK(learn::midi(1, 0xB0, 21, 127));
  run_ms(30);
  CHECK(mod::find(T("RYTM:PERF3")) != nullptr && ccs(37) >= 1);   // PERF3 = CC 37 to the Rytm
  hooks.to_rytm = nullptr;
  learn::reset();
  printf("scene: a learn route to RYTM:PERF3 still plays (S7)\n");
}
static void scene_report_shown() {
  scene::reset();
  CHECK(scene::rytm_scene() < 0);
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x0F, 5, 0xF7};
  engine_sysex(m, (int)sizeof m);
  CHECK(scene::rytm_scene() == 5);
  char js[16384];
  int n = scene::json(js, sizeof js);
  CHECK(n > 0 && strstr(js, "\"rscn\":5"));
  const uint8_t off[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x0F, 0, 0xF7};
  engine_sysex(off, (int)sizeof off);
  CHECK(scene::rytm_scene() == 0);
  printf("scene: the Rytm's scene (2D 01 0F) kept and in the JSON\n");
}

// ---- 0.5.0 v2 P2 Task 10: EDIT on the Rytm's own pages (rytm-mods 0026 "ms"): 2D 01 13 the gesture, 2D 01 12 a turn
static void edit19(int ev, int side, int n) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x13, (uint8_t)ev, (uint8_t)side, (uint8_t)n, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static void cap18(int side, int t, int w, int d, int func, int base) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x12, (uint8_t)side, (uint8_t)t, (uint8_t)w, (uint8_t)(d & 0x7F),
                       (uint8_t)func, (uint8_t)(base >> 7 & 0x7F), (uint8_t)(base & 0x7F), 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static void clr20(int side, int t, int w) {   // 2D 01 14: a knob press under the side (0.5.2)
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x14, (uint8_t)side, (uint8_t)t, (uint8_t)w, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static std::string readout() {   // the last 5A 01 08 <n> <text> sent to the Rytm, "" none
  std::string r;
  for (const auto& o : g_rytm)
    if (o.size() >= 10 && o[4] == 0x5A && o[6] == 8 && o.size() == (size_t)o[7] + 9) r.assign(o.begin() + 8, o.end() - 1);
  return r;
}
static int lock_of(int n, Tid t) {   // scene n's lock of t (x 128), -1 none
  const scene::Scene& sc = scene::state().s[n];
  for (int i = 0; i < sc.n; ++i)
    if (sc.l[i].t == t) return sc.l[i].v;
  return -1;
}
static void edit_start(int a, int b) {
  scene_reset_with_base(40);
  hooks.to_rytm = rytm_cap;
  g_rytm.clear();
  scene::set_ab(a, b);
}
static void capture_first_turn_from_base() {
  edit_start(2, 5);
  edit19(scene::kEditHeld, 0, 0x7F);
  CHECK(scene::edit_side() == 0 && readout() == "EDIT A: SCENE 3");
  const Tid frq = mod::rytm_tid(0, 30);   // BD FLTR FRQ (word 30)
  CHECK(frq != 0);
  cap18(0, 0, 30, 3, 0, 64 * 128);
  CHECK(lock_of(2, frq) == 67 * 128);
  CHECK(readout() == "A FRQ 67 >64");
  hooks.to_rytm = nullptr;
  printf("scene: a turn under PAGE + T15 on a Rytm page locks A from the kit's value (2D 01 12), the readout\n");
}
static void capture_second_turn_from_lock() {
  edit_start(2, 5);
  edit19(scene::kEditHeld, 0, 0x7F);
  const Tid frq = mod::rytm_tid(0, 30);
  cap18(0, 0, 30, 3, 0, 64 * 128);
  cap18(0, 0, 30, -5, 0, 64 * 128);   // from the lock (67), not the base
  CHECK(lock_of(2, frq) == 62 * 128 && readout() == "A FRQ 62 <64");
  cap18(0, 0, 30, 2, 1, 64 * 128);    // FUNC: fine (8 a detent, 1/16 of a step)
  CHECK(lock_of(2, frq) == 62 * 128 + 16);
  cap18(0, 0, 30, 63, 0, 64 * 128), cap18(0, 0, 30, 63, 0, 64 * 128);   // clamped at the top
  CHECK(lock_of(2, frq) == 16383);
  hooks.to_rytm = nullptr;
  printf("scene: later turns move the lock from its value; FUNC fine; clamped\n");
}
static void capture_b_side() {
  edit_start(2, 5);
  edit19(scene::kEditHeld, 1, 0x7F);
  CHECK(scene::edit_side() == 1 && readout() == "EDIT B: SCENE 6");
  cap18(1, 3, 30, 1, 0, 10 * 128);
  CHECK(lock_of(5, mod::rytm_tid(3, 30)) == 11 * 128 && lock_of(2, mod::rytm_tid(3, 30)) < 0);
  CHECK(readout() == "B FRQ 11 >10");
  edit19(scene::kEditLetGo, 1, 0x7F);
  CHECK(scene::edit_side() < 0);
  hooks.to_rytm = nullptr;
  printf("scene: PAGE + T16 locks into B; its release ends EDIT\n");
}
static void capture_without_a_refused() {
  edit_start(-1, -1);
  edit19(scene::kEditHeld, 0, 0x7F);
  CHECK(readout() == "SET A: T1-12");
  cap18(0, 0, 30, 3, 0, 64 * 128);
  CHECK(scene::state().s[0].n == 0 && readout() == "SET A FIRST");
  edit19(scene::kEditPicked, 0, 7);   // T8 under the side: scene 8 is A
  CHECK(scene::state().a == 7 && scene::state().b < 0 && readout() == "EDIT A: SCENE 8");
  cap18(0, 0, 30, 3, 0, 64 * 128);
  CHECK(lock_of(7, mod::rytm_tid(0, 30)) == 67 * 128);
  hooks.to_rytm = nullptr;
  printf("scene: no A: refused (SET A FIRST); T<n> under the side picks the scene\n");
}
static void capture_sidecar_machine_to_kv() {
  edit_start(2, 5);
  const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 40, (uint8_t)kPlaits, 0xF7};   // BD on a Sidecar machine
  engine_sysex(m33, (int)sizeof m33);
  CHECK(ui::remote_track_machine(0) >= 0 && ui::remote_track_machine(0) != 0x7F && ui::remote_track_voice(0) >= 0);
  edit19(scene::kEditHeld, 0, 0x7F);
  cap18(0, 0, 9 + 2, 4, 0, 50 * 128);   // SRC word 11: its voice's P3
  CHECK(lock_of(2, mod::tid(mod::kV, ui::remote_track_voice(0), 2)) == 54 * 128);
  CHECK(lock_of(2, mod::rytm_tid(0, 11)) < 0);
  cap18(0, 0, 30, 1, 0, 0);             // FLTR stays the Rytm's own
  CHECK(lock_of(2, mod::rytm_tid(0, 30)) == 128);
  const uint8_t r33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 0, 0x7F, 0xF7};   // (back to a Rytm machine)
  engine_sysex(r33, (int)sizeof r33);
  hooks.to_rytm = nullptr;
  printf("scene: a Sidecar machine's SRC words lock its voice's P (V<n>:P<k>)\n");
}
static void capture_fx_track_rfx() {
  edit_start(2, 5);
  edit19(scene::kEditHeld, 0, 0x7F);
  cap18(0, 12, 9, 2, 0, 30 * 128);      // FX track, DELAY TIME (word 9)
  CHECK(lock_of(2, mod::tid(mod::kRfx, 0, 0)) == 32 * 128);
  cap18(0, 12, 4, 2, 0, 0);             // word 4 (LFO DST) is no target
  CHECK(scene::state().s[2].n == 1 && readout() == "NOT LOCKABLE");
  hooks.to_rytm = nullptr;
  printf("scene: the FX track's words lock RFX; a word with no target is refused\n");
}
static void capture_stepped_word_steps() {
  edit_start(2, 5);
  edit19(scene::kEditHeld, 0, 0x7F);
  const Tid mul = mod::rytm_tid(0, 1);  // LFO MUL: a list
  CHECK(mod::range(mul).mode == mod::kStep);
  cap18(0, 0, 1, 5, 0, 3 * 128);        // five detents of a fast turn: one entry
  CHECK(lock_of(2, mul) == 4 * 128);
  cap18(0, 0, 1, -1, 1, 3 * 128);
  CHECK(lock_of(2, mul) == 3 * 128);
  hooks.to_rytm = nullptr;
  printf("scene: a stepped word steps one entry a report\n");
}
static void capture_clear_removes_only_that_entry() {
  edit_start(2, 5);
  edit19(scene::kEditHeld, 0, 0x7F);
  const Tid frq = mod::rytm_tid(0, 30), frq3 = mod::rytm_tid(3, 30), lev = mod::rytm_tid(0, 33);
  cap18(0, 0, 30, 3, 0, 64 * 128);
  cap18(0, 3, 30, 2, 0, 64 * 128);
  cap18(0, 0, 33, 1, 0, 100 * 128);
  edit19(scene::kEditLetGo, 0, 0x7F);
  edit19(scene::kEditHeld, 1, 0x7F);
  cap18(1, 0, 30, 4, 0, 64 * 128);   // B's own FRQ lock
  edit19(scene::kEditLetGo, 1, 0x7F);
  edit19(scene::kEditHeld, 0, 0x7F);
  CHECK(scene::state().s[2].n == 3 && scene::state().s[5].n == 1);
  clr20(0, 0, 30);
  CHECK(lock_of(2, frq) < 0 && lock_of(2, frq3) == 66 * 128 && lock_of(2, lev) == 101 * 128 && scene::state().s[2].n == 2);
  CHECK(scene::state().s[5].n == 1 && lock_of(5, frq) == 68 * 128);   // B's FRQ and the other scenes untouched
  CHECK(readout() == "A FRQ CLR");
  clr20(0, 0, 30);                    // none left: nothing else goes
  CHECK(scene::state().s[2].n == 2 && readout() == "A FRQ NONE" && readout().size() <= (size_t)mod::kReadoutMax);
  clr20(0, 3, 30);
  CHECK(lock_of(2, frq3) < 0 && lock_of(2, lev) == 101 * 128 && scene::state().s[2].n == 1);
  hooks.to_rytm = nullptr;
  printf("scene: a knob press under the side (2D 01 14) removes that parameter's lock only, A's, the readout\n");
}
static void capture_clear_b_side_and_machine() {
  edit_start(2, 5);
  const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 40, (uint8_t)kPlaits, 0xF7};   // BD on a Sidecar machine
  engine_sysex(m33, (int)sizeof m33);
  const Tid p3 = mod::tid(mod::kV, ui::remote_track_voice(0), 2);
  edit19(scene::kEditHeld, 1, 0x7F);
  cap18(1, 0, 9 + 2, 4, 0, 50 * 128);
  cap18(1, 0, 30, 1, 0, 0);
  CHECK(lock_of(5, p3) == 54 * 128 && lock_of(5, mod::rytm_tid(0, 30)) == 128);
  clr20(1, 0, 11);                    // SRC word 11 on a Sidecar machine: its voice's P3
  CHECK(lock_of(5, p3) < 0 && lock_of(5, mod::rytm_tid(0, 30)) == 128 && scene::state().s[5].n == 1);
  CHECK(readout().compare(0, 2, "B ") == 0 && readout().find("CLR") != std::string::npos);
  const uint8_t r33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 0, 0x7F, 0xF7};
  engine_sysex(r33, (int)sizeof r33);
  hooks.to_rytm = nullptr;
  printf("scene: the press clears B's lock; a Sidecar machine's SRC word clears its voice's P\n");
}
// 0.5.2 rc4: a Sidecar machine's source / preset list knob (GRANULAR SRC P1, SAMPLER / DSP SAMPLE SMP P11, WAVETABLE
// TBLB P13, JUNO's preset P1) is NOT LOCKABLE: a morph of it would sweep through the pool; the other P words lock as before
static void capture_src_list_not_lockable() {
  struct M { int rm, vb, word; } ms[] = {{47, kGranMachine, 9}, {43, kDspSample, 19}, {45, kDspSample, 19},
                                         {46, kWaveMachine, 21}, {46, kWaveMachine, 9}, {41, kJuno, 9},
                                         {52, kMultiMachine, 9}, {49, kChopMachine, 9}, {40, kFm, 9}};
  for (const M& m : ms) {
    edit_start(2, 5);
    const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, (uint8_t)m.rm, (uint8_t)m.vb, 0xF7};
    engine_sysex(m33, (int)sizeof m33);
    const Tid p = mod::tid(mod::kV, ui::remote_track_voice(0), m.word - 9);
    edit19(scene::kEditHeld, 0, 0x7F);
    cap18(0, 0, m.word, 2, 0, 50 * 128);
    CHECK(lock_of(2, p) < 0 && scene::state().s[2].n == 0 && readout() == "NOT LOCKABLE");
    clr20(0, 0, m.word);
    CHECK(readout() == "NOT LOCKABLE");
    cap18(0, 0, 10, 2, 0, 50 * 128);   // P2: lockable as before
    CHECK(lock_of(2, mod::tid(mod::kV, ui::remote_track_voice(0), 1)) == 52 * 128 && scene::state().s[2].n == 1);
    const uint8_t r33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 0, 0x7F, 0xF7};
    engine_sysex(r33, (int)sizeof r33);
  }
  // the same P on a machine where it is no list stays lockable: DSP SAMPLE's P1 (ENG), PLAITS' P1, GRANULAR's P11
  struct L { int rm, vb, word; } ls[] = {{45, kDspSample, 9}, {40, kPlaits, 9}, {47, kGranMachine, 19}};
  for (const L& l : ls) {
    edit_start(2, 5);
    const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, (uint8_t)l.rm, (uint8_t)l.vb, 0xF7};
    engine_sysex(m33, (int)sizeof m33);
    edit19(scene::kEditHeld, 0, 0x7F);
    cap18(0, 0, l.word, 2, 0, 50 * 128);
    CHECK(lock_of(2, mod::tid(mod::kV, ui::remote_track_voice(0), l.word - 9)) == 52 * 128 && readout() != "NOT LOCKABLE");
    const uint8_t r33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 0, 0x7F, 0xF7};
    engine_sysex(r33, (int)sizeof r33);
  }
  hooks.to_rytm = nullptr;
  printf("scene: a Sidecar machine's source / preset list word is NOT LOCKABLE, its other P words lock\n");
}
static void capture_clear_refused() {
  edit_start(2, 5);
  const Tid frq = mod::rytm_tid(0, 30);
  edit19(scene::kEditHeld, 0, 0x7F);
  cap18(0, 0, 30, 3, 0, 64 * 128);
  CHECK(lock_of(2, frq) == 67 * 128);
  clr20(2, 0, 30);                    // an unknown side
  clr20(0, 13, 30);                   // an unknown track
  clr20(0, 0, 4);                     // a word with no target
  clr20(0, 0, 0x7F);                  // a word out of range
  CHECK(lock_of(2, frq) == 67 * 128 && scene::state().s[2].n == 1);
  CHECK(readout() == "NOT LOCKABLE");
  clr20(1, 0, 30);                    // the other side is not held
  CHECK(lock_of(2, frq) == 67 * 128 && readout() == "EDIT ENDED");
  edit19(scene::kEditLetGo, 0, 0x7F);
  clr20(0, 0, 30);                    // the side let go (a kit change): nothing is removed
  CHECK(lock_of(2, frq) == 67 * 128 && readout() == "EDIT ENDED");
  edit_start(-1, -1);
  edit19(scene::kEditHeld, 0, 0x7F);
  clr20(0, 0, 30);                    // A has no scene
  CHECK(readout() == "SET A FIRST");
  hooks.to_rytm = nullptr;
  printf("scene: a press with an unknown side / track / word, the other side, no gesture, no scene: refused, nothing removed\n");
}
static void capture_after_kit_change_refused() {
  edit_start(2, 5);
  edit19(scene::kEditHeld, 0, 0x7F);
  scene::kit_changed(scene::rytm_kit() == 9 ? 10 : 9);   // the gesture's scenes are gone with the kit
  int before = 0;
  for (int i = 0; i < scene::kScenes; ++i) before += scene::state().s[i].n;
  cap18(0, 0, 30, 3, 0, 64 * 128);
  int after = 0;
  for (int i = 0; i < scene::kScenes; ++i) after += scene::state().s[i].n;
  CHECK(after == before && readout() == "EDIT ENDED");
  hooks.to_rytm = nullptr;
  printf("scene: a turn after a kit change ended the gesture is refused (EDIT ENDED)\n");
}
static void readout_wire_format() {
  hooks.to_rytm = rytm_cap;
  g_rytm.clear();
  mod::rytm_readout("a frq 1~");
  CHECK(!g_rytm.empty());
  const std::vector<uint8_t> want = {0xF0, 0x7D, 0x52, 0x54, 0x5A, 1, 8, 8, 'A', ' ', 'F', 'R', 'Q', ' ', '1', '?', 0xF7};
  CHECK(g_rytm.back() == want);
  g_rytm.clear();
  mod::rytm_readout("12345678901234567890");   // 16 at most
  CHECK(g_rytm.size() == 1 && g_rytm[0][7] == 16 && g_rytm[0].size() == 25);
  hooks.to_rytm = nullptr;
  printf("scene: the readout's wire format (5A 01 08 <n> <ASCII 20..5F>, 16 at most)\n");
}
static void hello_arms_edit() {
  hooks.to_rytm = rytm_cap;
  auto armed = [] {
    int k = 0;
    for (const auto& o : g_rytm) k += o == std::vector<uint8_t>({0xF0, 0x7D, 0x52, 0x54, 0x5A, 1, 0x0B, 1, 0xF7});
    return k;
  };
  g_rytm.clear();
  hello_caps2(ui::kCaps2QpMute | ui::kCaps2Capture);
  CHECK(armed() == 1);
  g_rytm.clear();
  hello_caps2(ui::kCaps2QpMute);   // an older mod: never armed (it would refuse the op)
  CHECK(armed() == 0);
  // the mod's answer to 08 / 0B, even a refusal, is never "CCs from now on": a mute still goes as the mod command
  hello_caps2(ui::kCaps2Capture);
  const uint8_t hello[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 16, ui::kCaps2Capture, 0xF7};   // caps: the mod command
  engine_sysex(hello, (int)sizeof hello);
  const uint8_t ack[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x10, 8, 1, 0xF7};
  engine_sysex(ack, (int)sizeof ack);
  g_rytm.clear();
  mod::rytm_mute(1, 0);
  CHECK(!g_rytm.empty() && g_rytm.back()[4] == 0x5A && g_rytm.back()[6] == mod::kOpMute);
  hooks.to_rytm = nullptr;
  printf("scene: a hello with kCaps2Capture arms the Rytm's edit (5A 01 0B 1); 08 / 0B answers keep the mod command\n");
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
  console_round_trip();
  qperf_sets_fader();
  qperf_ignored_until_moved();
  qperf_smoothing_follows_interval();
  qperf_baseline_after_hello();
  qperf_jitter_deadband();
  qp_muted_only_ignores_unmuted();
  qp_muted_only_takes_when_muted();
  qp_mute_edge_is_baseline();
  qp_off_never_moves();
  qp_always_as_before();
  qp_muted_only_without_caps_is_off();
  handover_glides_200ms();
  same_source_keeps_20ms();
  rytm_targets_refused();
  rytm_targets_still_learnable();
  scene_report_shown();
  capture_first_turn_from_base();
  capture_second_turn_from_lock();
  capture_b_side();
  capture_without_a_refused();
  capture_sidecar_machine_to_kv();
  capture_fx_track_rfx();
  capture_stepped_word_steps();
  capture_after_kit_change_refused();
  capture_clear_removes_only_that_entry();
  capture_clear_b_side_and_machine();
  capture_clear_refused();
  capture_src_list_not_lockable();
  readout_wire_format();
  hello_arms_edit();
  printf("test_scene: PASS\n");
  return 0;
}
