// core/tests/test_rytm_out.cpp - the Rytm's own scene, PERF macros and mutes from the Sidecar (core/mod/rytm_out.h,
// spec-050 2.7, 5.2): a scene lock fires once at halfway each way, PERF follow at most every 5 ms with the last value
// sent, the mod command in place of the CCs when the hello says so, mutes by CC 94 on each track's channel, a refusal
// falling back to the CCs, a lock leaving sends nothing.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "song/song.h"

using namespace core;
using Msg = std::vector<uint8_t>;

static std::vector<Msg> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void run_ms(int ms) {
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), mod::tick(now_us());
}
static mod::Tid T(const char* n) {
  mod::Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
static void fresh() {
  mod::reset();
  scene::reset();
  at(g_us + 1000), mod::tick(now_us());
  g_out.clear();
}
static int count(const Msg& m) {
  int n = 0;
  for (const Msg& o : g_out) n += o == m;
  return n;
}
static int count_cc(uint8_t st, uint8_t c, int* last = nullptr) {
  int n = 0;
  for (const Msg& o : g_out)
    if (o.size() == 3 && o[0] == st && o[1] == c) ++n, last ? *last = o[2] : 0;
  return n;
}
static void sweep(int from, int to, int ms) {   // the fader from -> to over ms, a set each millisecond
  for (int i = 1; i <= ms; ++i) {
    scene::set_fader((uint16_t)(from + (to - from) * i / ms), scene::kConsole);
    run_ms(1);
  }
}

static void scn_fires_once_at_halfway() {
  fresh();
  CHECK(scene::lock(1, T("RYTM:SCN"), 5 * 128));   // B only: scene 5; A leaves the Rytm's (0) below halfway
  scene::set_ab(0, 1);
  run_ms(5);
  CHECK(g_out.empty());
  sweep(0, 16383, 200);
  run_ms(30);
  CHECK(count({0xBD, 92, 5}) == 1 && count_cc(0xBD, 92) == 1);   // ch 14 (the auto channel)
  sweep(16383, 0, 200);
  run_ms(30);
  CHECK(count_cc(0xBD, 92) == 1);   // B only: below halfway the Rytm is left alone (never a scene the user chose, off)
  sweep(0, 16383, 200);
  run_ms(30);
  CHECK(count({0xBD, 92, 5}) == 2);   // up again: sent again (the Rytm may have changed meanwhile)
  printf("rytm out: a B-only RYTM:SCN fires at halfway on the way up, leaves the Rytm alone below\n");
}

// review M1: a lock at 0 goes out (a new entry has no value applied); both sides locked: one CC each way
static void scn_zero_sent() {
  fresh();
  CHECK(scene::lock(0, T("RYTM:SCN"), 0) && scene::lock(1, T("RYTM:SCN"), 5 * 128));
  scene::set_ab(0, 1);
  run_ms(5);
  CHECK(count({0xBD, 92, 0}) == 1 && g_out.size() == 1);   // A: scene off, at once
  sweep(0, 16383, 100);
  run_ms(30);
  g_out.clear();
  sweep(16383, 0, 100);
  run_ms(30);
  CHECK(count({0xBD, 92, 0}) == 1 && g_out.size() == 1);   // back to A: one BD 5C 00
  fresh();
  CHECK(scene::lock(0, T("RYTM:SCN"), 12 * 128) && scene::lock(1, T("RYTM:PERF2"), 0));
  scene::set_ab(0, 0);   // A == B: both at their values
  scene::lock(0, T("RYTM:PERF2"), 0);
  run_ms(5);
  CHECK(count({0xBD, 92, 12}) == 1 && count({0xBD, 36, 0}) == 1);   // scene 12 (CC 92 1..12 selects); PERF2 at 0 sent
  printf("rytm out: a lock at 0 is sent; A 0 / B 5 sends one CC 92 = 0 back at A\n");
}

static void perf_follow_rate_limited() {
  fresh();
  CHECK(scene::lock(0, T("RYTM:PERF4"), 0) && scene::lock(1, T("RYTM:PERF4"), 127 * 128));
  scene::set_ab(0, 1);
  run_ms(5);
  sweep(0, 16383, 100);
  int n100 = count_cc(0xBD, 39);   // PERF4 = CC 39; no GLOBAL read: on the auto channel (14)
  run_ms(40);
  int last = -1, n = count_cc(0xBD, 39, &last);
  printf("rytm out: PERF4 follow over a 100 ms sweep: %d messages (then %d), the last %d\n", n100, n, last);
  CHECK(n100 > 5 && n100 <= 21 && last == 127);
  int prev = -1;
  for (const Msg& o : g_out)
    if (o.size() == 3 && o[0] == 0xBD && o[1] == 39) {
      CHECK((int)o[2] > prev);
      prev = o[2];
    }
  printf("rytm out: a macro follows at most every 5 ms, its last value sent\n");
}

// review M4 (rytm-emu, notes scenes050): the PERF channel as GLOBAL says; PERF OFF or none read: the auto channel;
// CC 92 = 1..12 selects a scene, 0 clears
static void perf_channel_from_global() {
  song::MidiConfig c;
  song::config_default(&c);
  c.valid = true, c.perf_ch = 15;
  song::set_midi_config(c);
  fresh();
  CHECK(scene::lock(0, T("RYTM:PERF1"), 100 * 128) && scene::lock(0, T("RYTM:SCN"), 7 * 128));
  scene::set_ab(0, 0);
  run_ms(5);
  CHECK(count({0xBE, 35, 100}) == 1 && count({0xBD, 92, 7}) == 1);   // PERF on 15, the scene on the auto channel
  c.perf_ch = 0;   // PERF CHANNEL OFF
  song::set_midi_config(c);
  fresh();
  CHECK(scene::lock(0, T("RYTM:PERF1"), 100 * 128));
  scene::set_ab(0, 0);
  run_ms(5);
  CHECK(count({0xBD, 35, 100}) == 1 && count_cc(0xBE, 35) == 0);   // OFF: the auto channel
  fresh();
  CHECK(scene::lock(0, T("RYTM:SCN"), 0));
  scene::set_ab(0, 0);
  run_ms(5);
  CHECK(count({0xBD, 92, 0}) == 1);   // 0 clears the Rytm's scene
  song::config_default(&c);
  song::set_midi_config(c);
  printf("rytm out: PERF on GLOBAL's PERF channel, else (OFF, not read) the auto channel\n");
}

static void host_cmd_replaces_ccs() {
  fresh();
  mod::rytm_host_cmd(true);
  CHECK(scene::lock(1, T("RYTM:SCN"), 5 * 128) && scene::lock(1, T("RYTM:PERF4"), 127 * 128));
  scene::set_ab(0, 1);
  sweep(0, 16383, 100);
  run_ms(40);
  CHECK(count({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 2, 5, 0xF7}) == 1);
  CHECK(count({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 4, 127, 0xF7}) == 1);
  for (const Msg& o : g_out) CHECK(o[0] == 0xF0);   // no CC at all
  mod::rytm_host_cmd(false);
  printf("rytm out: the mod command replaces the CCs\n");
}

static void mute_fallback_per_track_channel() {
  fresh();
  song::MidiConfig c;
  song::config_default(&c);
  c.track_ch[2] = 7;   // track 3 on ch 7 (as a GLOBAL dump says)
  song::set_midi_config(c);
  mod::rytm_mute(1 << 2 | 1 << 0, 1 << 5);
  CHECK(g_out.size() == 3 && count({0xB6, 94, 1}) == 1 && count({0xB0, 94, 1}) == 1 && count({0xB5, 94, 0}) == 1);
  g_out.clear();
  mod::rytm_host_cmd(true);
  mod::rytm_mute(1 << 11, 1 << 7);
  CHECK(g_out.size() == 1 && g_out[0] == Msg({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 1, 0, 1 << 4, 0, 1, 0xF7}));
  mod::rytm_host_cmd(false);
  song::config_default(&c);
  song::set_midi_config(c);
  printf("rytm out: mutes by CC 94 on each track's channel, or the MUTE op\n");
}

static void ack_refused_falls_back() {
  fresh();
  mod::rytm_host_cmd(true);
  const uint8_t ack[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 0x10, 2, 1, 0xF7};   // 2D 01 10 <op 2> <status 1>
  engine_sysex(ack, (int)sizeof ack);
  CHECK(mod::rytm_out_stats().refused == 1);
  CHECK(scene::lock(0, T("RYTM:SCN"), 0) && scene::lock(1, T("RYTM:SCN"), 3 * 128));   // (both sides: B down to A)
  scene::set_ab(0, 1);
  sweep(0, 16383, 50);
  run_ms(30);
  CHECK(count({0xBD, 92, 3}) == 1 && count({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 2, 3, 0xF7}) == 0);
  // a hello whose caps say "host command": the command again
  const uint8_t hello[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, (uint8_t)(1 | mod::kCapsHostCmd), 0xF7};
  engine_sysex(hello, (int)sizeof hello);
  g_out.clear();
  sweep(16383, 0, 50);
  run_ms(30);
  CHECK(count({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 2, 0, 0xF7}) == 1);
  const uint8_t hello0[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, 0xF7};   // caps without it: the CCs
  engine_sysex(hello0, (int)sizeof hello0);
  printf("rytm out: a refusal falls back to the CCs until the next hello\n");
}

static void restore_sends_nothing() {
  fresh();
  CHECK(scene::lock(1, T("RYTM:SCN"), 5 * 128));
  scene::set_ab(0, 1);
  sweep(0, 16383, 50);
  run_ms(30);
  CHECK(count({0xBD, 92, 5}) == 1);
  g_out.clear();
  CHECK(scene::unlock(1, T("RYTM:SCN")));
  run_ms(20);
  CHECK(g_out.empty() && !mod::find(T("RYTM:SCN")));   // the Rytm keeps its scene
  printf("rytm out: a lock leaving sends nothing\n");
}

int main() {
  char tmpl[] = "/tmp/core_rytmout_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  hooks.to_rytm = out;
  scn_fires_once_at_halfway();
  scn_zero_sent();
  perf_follow_rate_limited();
  perf_channel_from_global();
  host_cmd_replaces_ccs();
  mute_fallback_per_track_channel();
  ack_refused_falls_back();
  restore_sends_nothing();
  printf("test_rytm_out: PASS\n");
  return 0;
}
