// core/tests/test_mod.cpp - the modulation layer (core/mod, spec-050 section 1): target ids, names and ranges; the
// entries, the tick's 1 ms gate and the voice flush, the base hooks (on the fake clock, a test source for the LFOs).
#include <cstdio>
#include <cstring>
#include <functional>

#include "check.h"
#include "core.h"
#include "fx/out.h"
#include "mod/mod.h"
#include "mod/target.h"

using namespace core;
using namespace core::mod;

static void ids_and_names() {
  CHECK(kind_of(tid(kV, 2, 4)) == kV && index_of(tid(kV, 2, 4)) == 2 && param_of(tid(kV, 2, 4)) == 4);
  char b[16]; Tid t;
  const char* names[] = {"V3:P5", "FX1:P2", "S5:F1", "I3:A2", "B1:E1", "M1:B3", "X2:B1", "MFX:LEV", "MFX:MIX2",
                         "O4:PAN", "T1:SPD", "R1:GATE", "LOOP:BAL", "RYTM:SCN", "RYTM:PERF3"};
  for (const char* n : names) { CHECK(parse(n, &t)); CHECK(!strcmp(name(t, b, sizeof b), n)); }
  CHECK(parse("v3:p5", &t) && t == tid(kV, 2, 4));
  CHECK(!parse("V9:P1", &t) && !parse("FX1:P6", &t) && !parse("Q1:X", &t));
  CHECK(range(tid(kV, 0, 0)).hi > 127.98f && range(tid(kV, 0, 0)).mode == kCont);   // list knobs too: mod never rounds kV
  CHECK(range(tid(kSeq, 0, 1)).mode == kStep && range(tid(kSeq, 0, 2)).mode == kCont);
  // the indices behind the names: FX1:P2 is slot 0 param 0, B1:E1 bus 1's chain (8) param 8, M1:B3 chain 11 param 6
  CHECK(parse("FX1:P2", &t) && t == tid(kFx, 0, 0) && parse("B1:E1", &t) && t == tid(kIns, 8, 8));
  CHECK(parse("M1:B3", &t) && t == tid(kIns, 11, 6) && parse("RYTM:PERF3", &t) && t == tid(kRytm, 0, 3));
  CHECK(!parse("MFX1:LEV", &t) && !parse("V3", &t) && !parse("I9:A1", &t) && !parse("M3:A1", &t) && !parse("V3:P5x", &t));
  CHECK(!valid(0) && !valid(tid(kV, 8, 0)) && !valid(tid(kKinds, 0, 0)) && !name(tid(kV, 8, 0), b, sizeof b)[0]);
  printf("mod: ids and names\n");
}

// ---- Task 3: the entries, the tick and its gate, the base hooks (a test source in place of the LFOs)
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static std::function<void(uint32_t, uint32_t)> g_src;
static void src_tramp(uint32_t us, uint32_t dus) { if (g_src) g_src(us, dus); }
static int g_internal = 0;
static void count_user(int, int, int, int, int, int) { if (user_report_internal) ++g_internal; }
static void start() {   // no entries, the clock started at g_us (the first call only starts it)
  mod::reset();
  mod::set_source_fn(src_tramp);
  g_src = nullptr;
  at(g_us + 1000000), mod::tick(now_us());
}
static void tick_one(float depth, float w, Tid t) {        // one 1 ms tick with one source
  g_src = [=](uint32_t, uint32_t) { mod::offset(0, t, depth, w); };
  at(g_us + 1000), mod::tick(now_us());
}
static void base_is_what_plays_now() {
  v[0].p[2] = 60;
  start();
  tick_one(10, 0.5f, tid(kV, 0, 2));
  CHECK(v[0].p[2] == 65.0f);                                // base 60 (v.p when the entry was made) + 10 x 0.5
  CHECK(mod::find(tid(kV, 0, 2)) && mod::find(tid(kV, 0, 2))->base == 60.0f && mod::base_of(0, 2, 99) == 60.0f);
  float p[16] = {}; p[2] = 70;
  mod::voice_params(0, p, 16);                              // a report: the live entry's base moves, p modulated
  CHECK(p[2] == 75.0f && mod::base_of(0, 2, 0) == 70.0f);
  float q[16] = {}; q[5] = 30;
  mod::voice_params(0, q, 16);                              // P6 has no entry: untouched, no base kept
  CHECK(q[5] == 30.0f && !mod::find(tid(kV, 0, 5)) && mod::base_of(0, 5, 77) == 77.0f);
  float pl[16];
  mod::played(0, pl);
  CHECK(pl[2] == v[0].p[2]);
  // a send FX knob: the 0x20's value is the base, the modulated one plays
  fx::control.knob[0][3] = 40;
  tick_one(20, 0.5f, tid(kFx, 0, 1));
  CHECK(fx::control.knob[0][3] == 50 && mod::fx_knob(0, 3, 90) == 100 && mod::fx_knob(0, 2, 33) == 33 && mod::fx_knob(0, 0, 3) == 3);
  printf("mod: a new entry's base is what plays now; a report moves a live entry's base only\n");
}
static void two_sources_sum_then_clamp_once() {
  v[0].p[2] = 120;
  start();
  g_src = [](uint32_t, uint32_t) { mod::offset(0, tid(kV, 0, 2), 32, 1); mod::offset(1, tid(kV, 0, 2), -32, 0.5f); };
  at(g_us + 1000), mod::tick(now_us());
  CHECK(v[0].p[2] == 127.99f);                              // 120 + 32 - 16 = 136, clamped once
  printf("mod: two sources summed, then clamped once\n");
}
static void restore_on_leave() {
  v[0].p[2] = 60;
  start();
  tick_one(10, 1, tid(kV, 0, 2));
  CHECK(v[0].p[2] == 70.0f);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());                     // a tick with no source: back to the base
  CHECK(v[0].p[2] == 60.0f && mod::stats().restores == 1 && !mod::find(tid(kV, 0, 2)));
  // an offset kind back to 0: the OUT route's PAN
  out::reset();
  tick_one(20, 1, tid(kOut, 2, 1));
  CHECK(out::control.pan[2] == 84);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(out::control.pan[2] == 64 && mod::stats().restores == 2);
  printf("mod: a target nothing refers to goes back to its base\n");
}
static void sub_ms_passes_only_flush() {
  hooks.user_report = count_user;
  v[0].user = 0, v[0].p[2] = 60;
  start();
  uint64_t t0 = g_us;
  g_internal = 0;
  tick_one(10, 1, tid(kV, 0, 2));                           // dirties the user voice; flushed at once (first call)
  CHECK(g_internal == 1);
  tick_one(10, 0.5f, tid(kV, 0, 2));                        // dirty again, 1 ms later: waits for its 5 ms
  CHECK(g_internal == 1);
  uint32_t ticks = mod::stats().ticks;                      // (the source stays: the same value, nothing new written)
  for (int i = 0; i < 3; ++i) at(g_us + 250), mod::tick(now_us());   // 250 us passes: no tick, no restore, no call yet
  CHECK(mod::stats().ticks == ticks && mod::stats().restores == 0 && g_internal == 1);
  while (g_us + 250 < t0 + 6000) at(g_us + 250), mod::tick(now_us());
  CHECK(g_internal == 1);
  at(g_us + 250), mod::tick(now_us());                      // t0 + 6 ms: 5 ms after the first call
  CHECK(g_internal == 2 && mod::stats().restores == 0);
  v[0].user = -1;
  hooks.user_report = nullptr;
  printf("mod: the passes between ticks flush only\n");
}
static void user_machine_spacing() {
  hooks.user_report = count_user;
  v[0].user = 0, v[0].p[2] = 60;
  start();
  uint64_t t0 = g_us;
  static int n;
  n = 0;
  g_src = [](uint32_t, uint32_t) { mod::offset(0, tid(kV, 0, 2), 20, (float)(++n % 7) / 7.0f); };
  g_internal = 0;
  while (g_us + 250 <= t0 + 21000) at(g_us + 250), mod::tick(now_us());
  printf("mod: a user machine's calls in 21 ms of passes: %d (ticks %u)\n", g_internal, mod::stats().ticks);
  CHECK(g_internal == 5);                                   // t0 + 1, 6, 11, 16, 21 ms
  v[0].user = -1;
  hooks.user_report = nullptr;
}
static void restore_all_flushes_now() {
  hooks.user_report = count_user;
  v[0].user = 0, v[0].p[2] = 60;
  start();
  tick_one(10, 1, tid(kV, 0, 2));
  tick_one(10, 0.5f, tid(kV, 0, 2));                        // dirty, its 5 ms not passed
  g_internal = 0;
  mod::restore_all();
  CHECK(g_internal == 1 && v[0].p[2] == 60.0f && !mod::find(tid(kV, 0, 2)));
  v[0].user = -1;
  hooks.user_report = nullptr;
  printf("mod: restore_all flushes a waiting user machine at once\n");
}

int main() {
  ids_and_names();
  base_is_what_plays_now();
  two_sources_sum_then_clamp_once();
  restore_on_leave();
  sub_ms_passes_only_flush();
  user_machine_spacing();
  restore_all_flushes_now();
  printf("test_mod: PASS\n");
  return 0;
}
