// core/tests/test_xypad.cpp - the XY pad (core/mod/xypad.h) on the modulation layer, on the fake clock: the mapping and
// the depth (negative too), the clamping, the sum with an LFO's offset, HOLD off / on at the release, the morph fader's
// return, a value-only target (RYTM, a seq field), a kit change while touching, the console, the files (the round trip,
// a kit without its own file, an unknown or removed target).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "hooks.h"
#include "bundle/rename.h"
#include "fmt/fmt.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/xypad.h"
#include "msamp/cardq.h"
#include "store/store.h"

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
  for (int i = 0; i < 400 && strcmp(core::project, name); ++i) {
    if (rename::state() == rename::kAsk) rename::answer(false);
    at(g_us + 20000), idle_save(), card_core_turn();
  }
  loaded();
  CHECK(!strcmp(core::project, name));
}
static void kit(int k) {   // 3B: the Rytm's active kit (0-based)
  const uint8_t m[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, (uint8_t)k, 0xF7};
  engine_sysex(m, 8);
}
static void fresh(float base) {   // nothing modulated, no slots, no scenes; V1's P3 plays base
  mod::reset();
  scene::reset();
  xypad::reset();
  v[0].p[2] = base;
  at(g_us + 1000), mod::tick(now_us());   // (the first call only starts the clock)
}
static float p3() { return v[0].p[2]; }

static void mapping_and_depth() {
  fresh(60);
  CHECK(xypad::set(0, 0, T("V1:P3"), 50));
  xypad::pos(0, 0);
  run_ms(2);
  CHECK(near(p3(), 60));                                    // position 0: nothing added
  xypad::pos(xypad::kMax, 0);
  run_ms(2);
  CHECK(near(p3(), 60 + 0.5f * 127.99f));                   // +50 % of the range at the right edge
  xypad::pos(8192, 0);
  run_ms(2);
  CHECK(near(p3(), 60 + 0.5f * 127.99f * 8192 / 16383));
  CHECK(xypad::set(0, 0, T("V1:P3"), -50));                 // inverted: down from the base
  run_ms(2);
  CHECK(near(p3(), 60 - 0.5f * 127.99f * 8192 / 16383));
  // Y moves its own slots only; two slots of one axis on the same target sum
  CHECK(xypad::set(1, 0, T("V1:P3"), 20) && xypad::set(1, 1, T("V1:P3"), 10));
  xypad::pos(0, xypad::kMax);
  run_ms(2);
  CHECK(near(p3(), 60 + 0.3f * 127.99f));
  printf("xypad: the mapping, the depth, an inverted slot, two slots summed\n");
}

static void clamping() {
  fresh(100);
  CHECK(xypad::set(0, 0, T("V1:P3"), 100));
  xypad::pos(xypad::kMax, 0);
  run_ms(2);
  CHECK(p3() == 127.99f);   // 100 + 127.99: the layer's clamp
  CHECK(xypad::set(0, 0, T("V1:P3"), -100));
  run_ms(2);
  CHECK(p3() == 0.0f);
  // an offset kind: the offset's -127..127 (the knob's span), the writer clamps the knob's sum
  CHECK(xypad::set(0, 0, T("MFX:LEV"), 100));
  run_ms(2);
  const mod::Entry* e = mod::find(T("MFX:LEV"));
  CHECK(e && e->applied == 127.0f);
  xypad::up();
  run_ms(2);
  CHECK(!mod::find(T("MFX:LEV")) && p3() == 100.0f);
  printf("xypad: clamped to the target's range (a voice's 0..127.99, an offset kind's 127)\n");
}

static void sums_with_lfo() {
  fresh(60);
  static Tid t;
  t = T("V1:P3");
  mod::set_source_fn([](uint32_t, uint32_t) { mod::offset(0, t, 10, 1.0f); });   // an LFO at +10
  run_ms(2);
  CHECK(near(p3(), 70));
  CHECK(xypad::set(0, 0, t, 25));
  xypad::pos(xypad::kMax, 0);
  run_ms(2);
  CHECK(near(p3(), 70 + 0.25f * 127.99f));
  xypad::up();   // HOLD off: the pad's part goes, the LFO's stays
  run_ms(2);
  CHECK(near(p3(), 70));
  mod::set_source_fn(nullptr);
  mod::reset();
  printf("xypad: sums with an LFO's offset on the same target\n");
}

static void hold_off_and_on() {
  fresh(60);
  CHECK(xypad::set(0, 0, T("V1:P3"), 100));
  xypad::pos(xypad::kMax / 2, 0);
  run_ms(2);
  CHECK(p3() > 100 && xypad::touching() && xypad::active());
  xypad::up();   // HOLD off: back to the base, the entry gone
  run_ms(2);
  CHECK(p3() == 60.0f && !mod::find(T("V1:P3")) && !xypad::active());
  xypad::set_hold(true);
  xypad::pos(xypad::kMax, 0);
  run_ms(2);
  float held = p3();
  CHECK(near(held, 127.99f));
  xypad::up();   // HOLD on: it stays
  run_ms(5);
  CHECK(p3() == held && !xypad::touching() && xypad::active());
  xypad::pos(0, 0);   // the next touch replaces it
  run_ms(2);
  CHECK(near(p3(), 60));
  xypad::pos(4000, 0);
  xypad::up();
  run_ms(2);
  float h2 = p3();
  CHECK(h2 > 60 && xypad::active());
  xypad::set_hold(false);   // HOLD off while held: released
  run_ms(2);
  CHECK(p3() == 60.0f && !xypad::active() && !mod::find(T("V1:P3")));
  printf("xypad: HOLD off returns at the release, HOLD on keeps the values until the next touch or HOLD off\n");
}

static void morph_fader_return() {
  fresh(60);
  scene::set_fader(3000, scene::kPage);
  CHECK(xypad::set(1, 0, xypad::kMorph, 100));
  xypad::pos(0, 8192);
  CHECK(scene::state().fader == 3000 + 8192 && scene::fader_source() == scene::kTouch);
  xypad::pos(0, xypad::kMax);
  CHECK(scene::state().fader == scene::kFaderMax);   // clamped
  xypad::up();
  CHECK(scene::state().fader == 3000);   // HOLD off: back where it was before the touch
  CHECK(xypad::set(1, 0, xypad::kMorph, -50));
  xypad::pos(0, xypad::kMax);
  CHECK(scene::state().fader == 0);   // 3000 - 8191.5: clamped at 0
  xypad::up();
  CHECK(scene::state().fader == 3000);
  // HOLD on: it stays; the next touch starts from the same base (not from the held position)
  xypad::set_hold(true);
  CHECK(xypad::set(1, 0, xypad::kMorph, 50));
  xypad::pos(0, 4000);
  xypad::up();
  uint16_t f1 = scene::state().fader;
  CHECK(f1 == 3000 + 2000);
  xypad::pos(0, 4000);
  CHECK(scene::state().fader == f1);
  xypad::up();
  xypad::set_hold(false);
  CHECK(scene::state().fader == 3000);
  // another source moved the fader meanwhile: the release leaves it there
  xypad::pos(0, 8000);
  scene::set_fader(12000, scene::kPage);
  xypad::up();
  CHECK(scene::state().fader == 12000);
  printf("xypad: the morph fader set directly, back to its position before the touch (HOLD off)\n");
}

static void value_only_targets() {
  fresh(60);
  CHECK(xypad::set(0, 0, T("RYTM:PERF1"), 100) && xypad::set(0, 1, T("T1:SWG"), -100));
  xypad::pos(xypad::kMax / 2, 0);
  run_ms(2);
  const mod::Entry* r = mod::find(T("RYTM:PERF1"));
  const mod::Entry* s = mod::find(T("T1:SWG"));
  CHECK(r && r->applied == 63.0f && s && s->applied == 65.0f);   // PERF 0 + 63.5 x 8191 / 8191.5; SWG from its top: 80 - 15
  xypad::up();
  run_ms(1);   // one tick
  r = mod::find(T("RYTM:PERF1"));
  CHECK(r && r->applied == 0.0f);    // the Rytm's macro once to the pad's zero point (it keeps what was sent)
  CHECK(!mod::find(T("T1:SWG")));    // the track's own value again: nothing sent
  run_ms(10);                        // (offered until rytm_out's 5 ms spacing has let it out)
  CHECK(!mod::find(T("RYTM:PERF1")));
  printf("xypad: a value-only target from its range's end; a RYTM macro back to its zero point at the release\n");
}

static void slot_cleared_while_touching() {
  fresh(60);
  CHECK(xypad::set(0, 2, T("V1:P3"), 100));
  xypad::pos(xypad::kMax, 0);
  run_ms(2);
  CHECK(p3() > 100);
  CHECK(xypad::clear(0, 2));   // the slot removed: its target back to its base at the next tick
  run_ms(2);
  CHECK(p3() == 60.0f && !mod::find(T("V1:P3")));
  xypad::up();
  printf("xypad: a slot cleared while touching lets its target go\n");
}

static void console() {
  fresh(60);
  std::string a = cmd("xy=set:x:1:RYTM:PERF3:-40");
  CHECK(a.find("X1 RYTM:PERF3 -40") != std::string::npos);
  CHECK(xypad::cfg().s[0][0].t == T("RYTM:PERF3") && xypad::cfg().s[0][0].depth == -40);
  a = cmd("xy=set:y:4:morph:100");
  CHECK(xypad::cfg().s[1][3].t == xypad::kMorph && a.find("Y4 MORPH +100") != std::string::npos);
  CHECK(cmd("xy=pos:100:200").empty() && xypad::touching() && xypad::x() == 100 && xypad::y() == 200);
  CHECK(cmd("xy=pos:99999:0").empty() && xypad::x() == xypad::kMax);
  CHECK(cmd("xy=up").empty() && !xypad::touching());
  CHECK(cmd("xy=hold:1").find("hold on") != std::string::npos && xypad::cfg().hold);
  CHECK(cmd("xy=hold:0").find("hold off") != std::string::npos && !xypad::cfg().hold);
  CHECK(cmd("xy=clear:x:1").find("X1") == std::string::npos && !xypad::cfg().s[0][0].t);
  for (const char* bad : {"xy=set:z:1:V1:P1:10", "xy=set:x:5:V1:P1:10", "xy=set:x:1:V9:P1:10", "xy=set:x:1:V1:P1:101",
                          "xy=set:x:1:V1:P1", "xy=pos:1", "xy=pos:a:b", "xy=hold:2", "xy=clear:x:0", "xy=nope"})
    CHECK(cmd(bad).find("cannot") != std::string::npos);
  char o[64];
  CHECK(!xypad::command("xyz", o, sizeof o) && !xypad::command("scene", o, sizeof o));
  CHECK(cmd("xy").find("xy: hold off") == 0);
  printf("xypad: the console (set, clear, hold, pos, up, the refusals)\n");
}

static void kit_change_while_touching() {
  to_project("XYK");
  kit(0);
  CHECK(xypad::kit() == 0);
  v[0].p[2] = 60;
  CHECK(xypad::set(0, 0, T("V1:P3"), 100));
  scene::set_fader(1000, scene::kPage);
  CHECK(xypad::set(1, 0, xypad::kMorph, 100));
  xypad::pos(xypad::kMax, 4000);
  run_ms(2);
  CHECK(p3() > 120 && scene::state().fader == 5000);
  kit(1);   // released first: the target back, the fader back; then kit 2's setup (none)
  run_ms(2);
  CHECK(xypad::kit() == 1 && !xypad::touching() && !xypad::active());
  CHECK(p3() == 60.0f && !mod::find(T("V1:P3")));
  CHECK(!xypad::cfg().s[0][0].t);
  CHECK(xypad::set(0, 0, T("V1:P3"), 50));
  xypad::pos(xypad::kMax, 0);   // the same finger still down: ignored until its up
  run_ms(2);
  CHECK(p3() == 60.0f && !xypad::touching());
  xypad::up();
  xypad::pos(xypad::kMax, 0);   // a new touch: kit 2's slot
  run_ms(2);
  CHECK(near(p3(), 60 + 0.5f * 127.99f));
  xypad::up();
  run_ms(2);
  // HOLD on at a kit change: the held state is the old kit's - dropped
  xypad::set_hold(true);
  xypad::pos(xypad::kMax, 0);
  xypad::up();
  run_ms(2);
  CHECK(xypad::active() && p3() > 100);
  kit(0);
  run_ms(2);
  CHECK(!xypad::active() && p3() == 60.0f && xypad::cfg().s[0][0].t == T("V1:P3") && xypad::cfg().s[0][0].depth == 100);
  printf("xypad: a kit change while touching releases first, the finger ignored until its up\n");
}

static void file_round_trip() {
  to_project("XYF");
  kit(2);
  CHECK(xypad::set(0, 0, T("RYTM:PERF1"), 100) && xypad::set(0, 3, T("I3:A2"), -25));
  CHECK(xypad::set(1, 1, xypad::kMorph, -50) && xypad::set(1, 2, T("V8:P16"), 7));
  CHECK(xypad::set(1, 3, T("RT4:FRQ"), 40));   // 0.5.0: a Rytm word, by name (rytm-param-mod H9)
  xypad::set_hold(true);
  CHECK(xypad::dirty());
  xypad::pos(100, 100);   // (a position is no edit)
  xypad::up();
  flush();
  CHECK(!xypad::dirty());
  std::string f = rd("/.rytm/xy/XYF/k003.txt");
  CHECK(f == "#sidecar xy 1\nH 1\nX 1 RYTM:PERF1 100\nX 4 I3:A2 -25\nY 2 MORPH -50\nY 3 V8:P16 7\nY 4 RT4:FRQ 40\n");
  xypad::Cfg before = xypad::cfg();
  xypad::load();
  const xypad::Cfg& c = xypad::cfg();
  CHECK(c.hold == before.hold && !xypad::locked());
  for (int a = 0; a < xypad::kAxes; ++a)
    for (int s = 0; s < xypad::kSlots; ++s) CHECK(c.s[a][s].t == before.s[a][s].t && c.s[a][s].depth == before.s[a][s].depth);
  // kit 6: no file of its own, the project's default
  wr("/.rytm/xy/XYF.txt", "#sidecar xy 1\nH 0\nY 1 RYTM:PERF2 30\n");
  to_project("XYG");
  to_project("XYF");   // (the store filled again: the default's text now in it)
  kit(5);
  CHECK(xypad::kit() == 5 && xypad::cfg().s[1][0].t == T("RYTM:PERF2") && !xypad::cfg().hold);
  CHECK(xypad::set(1, 0, T("RYTM:PERF2"), 31));
  flush();
  CHECK(rd("/.rytm/xy/XYF/k006.txt").find("Y 1 RYTM:PERF2 31") != std::string::npos);
  CHECK(rd("/.rytm/xy/XYF.txt") == "#sidecar xy 1\nH 0\nY 1 RYTM:PERF2 30\n");   // the default untouched
  kit(2);
  CHECK(xypad::cfg().hold && xypad::cfg().s[0][3].t == T("I3:A2"));
  printf("xypad file: the round trip (%zu bytes), a kit without its own file reads the project's\n", f.size());
}

static void unknown_target_locks_file() {
  const std::string text = "#sidecar xy 1\nH 1\nX 1 V1:P3 40\nX 2 ZZ9:Q1 50\nY 9 V1:P1 10\nQ 1\n";
  wr("/.rytm/xy/XYL/k002.txt", text);
  to_project("XYL");
  kit(1);
  CHECK(xypad::locked() && xypad::cfg().hold && xypad::cfg().s[0][0].t == T("V1:P3") && !xypad::cfg().s[0][1].t);
  CHECK(cmd("xy").find("LOCKED") != std::string::npos);
  CHECK(xypad::set(0, 2, T("V2:P2"), 10));   // an edit: played, never written over the newer file
  flush();
  CHECK(rd("/.rytm/xy/XYL/k002.txt") == text);
  // a removed target: a slot whose target this build no longer has never reaches the layer
  fresh(60);
  CHECK(!xypad::set(0, 0, mod::tid(mod::kV, 9, 0), 10));
  printf("xypad file: an unknown target or line locks the file (read, never written); no slot for a removed target\n");
}

// ---- review fixes (2026-11-11)
using Msg = std::vector<uint8_t>;
static std::vector<Msg> g_out;   // what rytm_out sent the Rytm (the hook)
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static int perf_cc(int macro, int* last) {   // the PERF macro's CCs sent (no GLOBAL read: the auto channel 14)
  static const uint8_t kCc[12] = {35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 46, 47};
  int n = 0;
  for (const Msg& o : g_out)
    if (o.size() == 3 && o[0] == 0xBD && o[1] == kCc[macro - 1]) ++n, *last = o[2];
  return n;
}

// H1: the release's settle goes out even when the macro's last value went out less than 5 ms before
static void rytm_settle_sent() {
  fresh(60);
  CHECK(xypad::set(0, 0, T("RYTM:PERF1"), 100));
  xypad::pos(xypad::kMax / 2, 0);
  run_ms(10);
  g_out.clear();
  xypad::pos(xypad::kMax, 0);
  run_ms(1);   // 127 out at once (5 ms since the last)
  int last = -1;
  CHECK(perf_cc(1, &last) == 1 && last == 127);
  xypad::up();   // within 5 ms of that send: the zero point waits for its time
  run_ms(20);
  CHECK(perf_cc(1, &last) == 2 && last == 0);
  printf("xypad: a RYTM macro's release settle reaches the Rytm when its last send was < 5 ms before\n");
}

// M1: a kit change mid-touch writes nothing to the new kit's macros
static void kit_change_settles_nothing_new() {
  to_project("XYM");
  kit(1);
  CHECK(xypad::set(1, 0, T("RYTM:PERF3"), -100));
  flush();
  kit(0);
  CHECK(xypad::set(0, 0, T("RYTM:PERF1"), 100));
  flush();
  xypad::pos(xypad::kMax, 0);
  run_ms(10);
  g_out.clear();
  kit(1);
  run_ms(20);
  int last = -1;
  CHECK(xypad::cfg().s[1][0].t == T("RYTM:PERF3"));
  CHECK(perf_cc(3, &last) == 0 && perf_cc(1, &last) == 0);
  xypad::up();
  printf("xypad: a kit change while touching sends no macro to the new kit\n");
}

// M4: SAVE / RELOAD of the live kit keeps a held state and a touching finger
static void same_kit_reload_keeps_state() {
  to_project("XYS");
  kit(0);
  v[0].p[2] = 60;
  CHECK(xypad::set(0, 0, T("V1:P3"), 100));
  xypad::set_hold(true);
  xypad::pos(xypad::kMax, 0);
  xypad::up();
  run_ms(2);
  CHECK(xypad::active() && p3() > 100);
  xypad::project_changed();   // kits' modules_again: the same project, the same kit
  run_ms(2);
  CHECK(xypad::active() && p3() > 100 && xypad::cfg().s[0][0].t == T("V1:P3"));
  xypad::set_hold(false);
  run_ms(2);
  xypad::pos(xypad::kMax, 0);
  run_ms(2);
  xypad::project_changed();
  xypad::pos(xypad::kMax / 2, 0);   // the finger still counts
  run_ms(2);
  CHECK(xypad::touching() && near(p3(), 60 + 0.5f * 127.99f));
  xypad::up();
  run_ms(2);
  CHECK(!xypad::active() && p3() == 60.0f);
  printf("xypad: SAVE / RELOAD of the live kit keeps HOLD's held state and the finger\n");
}

// M5: a value-only target on two slots takes its range's end once
static void value_only_anchor_once() {
  fresh(60);
  CHECK(xypad::set(0, 0, T("T1:SWG"), 100) && xypad::set(1, 0, T("T1:SWG"), 100));
  mod::Range r = mod::range(T("T1:SWG"));
  xypad::pos(0, 0);
  run_ms(2);
  const mod::Entry* s = mod::find(T("T1:SWG"));
  CHECK(s && s->applied == r.lo);
  xypad::pos(xypad::kMax / 2, xypad::kMax / 2);
  run_ms(2);
  s = mod::find(T("T1:SWG"));
  CHECK(s && near(s->applied, r.lo + (r.hi - r.lo) * 2 * 8191.0f / 16383.0f));
  xypad::up();
  run_ms(2);
  printf("xypad: a value-only target on two slots starts at its range's end once\n");
}

// L1: a RYTM slot cleared or retargeted while active returns its macro; a MORPH slot cleared gives the fader back
static void clear_returns_rytm_and_morph() {
  fresh(60);
  CHECK(xypad::set(0, 0, T("RYTM:PERF1"), 100));
  xypad::pos(xypad::kMax, 0);
  run_ms(10);
  g_out.clear();
  CHECK(xypad::clear(0, 0));
  run_ms(20);
  int last = -1;
  CHECK(perf_cc(1, &last) == 1 && last == 0);
  CHECK(xypad::set(0, 0, T("RYTM:PERF2"), -100));
  run_ms(10);
  g_out.clear();
  CHECK(xypad::set(0, 0, T("RYTM:PERF4"), 100));   // retargeted: PERF2 back to its zero point (127)
  run_ms(20);
  CHECK(perf_cc(2, &last) == 1 && last == 127);
  xypad::up();
  run_ms(20);
  fresh(60);
  scene::set_fader(1000, scene::kPage);
  CHECK(xypad::set(1, 0, xypad::kMorph, 100));
  xypad::pos(0, 4000);
  CHECK(scene::state().fader == 5000);
  CHECK(xypad::clear(1, 0));
  CHECK(scene::state().fader == 1000);
  xypad::up();
  printf("xypad: a cleared or retargeted RYTM slot returns its macro, a cleared MORPH slot the fader\n");
}

// L3 / L4: blank lines and indented comments read; a malformed depth locks
static void file_blank_and_bad_depth() {
  wr("/.rytm/xy/XYB/k001.txt", "#sidecar xy 1\n   \nH 1\n  # a note\n\t\nX 1 V1:P3 40\n");
  wr("/.rytm/xy/XYB/k002.txt", "#sidecar xy 1\nX 1 V1:P3 +0000001x\n");
  to_project("XYB");
  kit(0);
  CHECK(!xypad::locked() && xypad::cfg().hold && xypad::cfg().s[0][0].depth == 40);
  kit(1);
  CHECK(xypad::locked() && !xypad::cfg().s[0][0].t);
  printf("xypad file: blank lines and indented comments read; a malformed depth locks the file\n");
}

// re-review N1: a control tick late by more than the settle's window still sends the zero point
static void rytm_settle_late_tick() {
  fresh(60);
  CHECK(xypad::set(0, 0, T("RYTM:PERF1"), 100));
  xypad::pos(xypad::kMax, 0);
  run_ms(10);
  g_out.clear();
  xypad::up();
  at(g_us + 20000), mod::tick(now_us());   // one tick 20 ms later (a stall)
  run_ms(10);
  int last = -1;
  CHECK(perf_cc(1, &last) == 1 && last == 0);
  CHECK(!mod::find(T("RYTM:PERF1")));
  printf("xypad: a RYTM settle survives a late control tick\n");
}

int main() {
  char tmpl[] = "/tmp/core_xypad_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  cardq::set_worker(true);
  to_project("XYP");
  mapping_and_depth();
  clamping();
  sums_with_lfo();
  hold_off_and_on();
  morph_fader_return();
  value_only_targets();
  slot_cleared_while_touching();
  console();
  kit_change_while_touching();
  file_round_trip();
  unknown_target_locks_file();
  hooks.to_rytm = out;
  rytm_settle_sent();
  kit_change_settles_nothing_new();
  same_kit_reload_keeps_state();
  value_only_anchor_once();
  clear_returns_rytm_and_morph();
  file_blank_and_bad_depth();
  rytm_settle_late_tick();
  printf("test_xypad: PASS\n");
  return 0;
}
