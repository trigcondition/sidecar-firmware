// core/tests/test_rytm_out.cpp - the Rytm's own scene, PERF macros and mutes from the Sidecar (core/mod/rytm_out.h,
// spec-050 2.7, 5.2; 0.5.0 v2: moved by the XY pad and MIDI learn, no scene target): RYTM:SCN once a change, PERF follow at most every 5 ms with the last value
// sent, the mod command in place of the CCs when the hello says so, mutes by CC 94 on each track's channel, a refusal
// falling back to the CCs, a value leaving sends nothing.
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
// 0.5.0 v2 (S7): the scenes no longer lock the Rytm's scene / macros; the XY pad and MIDI learn still move them. The
// writer is driven here as the layer's morph value each tick (the test source: mod::set_source_fn), so these cases test
// rytm_out alone, whatever source sets the value
struct Drive {
  mod::Tid t;
  float v;
};
static std::vector<Drive> g_drive;
static void drive_src(uint32_t, uint32_t) {
  for (const Drive& d : g_drive) mod::morph(d.t, d.v);
}
static void drive(const char* n, float v) {   // the target plays v from the next tick (until undrive)
  mod::Tid t = T(n);
  for (Drive& d : g_drive)
    if (d.t == t) return void(d.v = v);
  g_drive.push_back({t, v});
}
static void undrive(const char* n) {
  mod::Tid t = T(n);
  for (size_t i = 0; i < g_drive.size(); ++i)
    if (g_drive[i].t == t) g_drive.erase(g_drive.begin() + (long)i), mod::release(t);
}
static void fresh() {
  mod::reset();
  scene::reset();
  g_drive.clear();
  mod::set_source_fn(drive_src);
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
static void sweep(const char* n, float from, float to, int ms) {   // the value from -> to over ms, a set each millisecond
  for (int i = 1; i <= ms; ++i) {
    drive(n, from + (to - from) * (float)i / (float)ms);
    run_ms(1);
  }
}

// a scene number goes out once a change (CC 92 on the auto channel 14; 1..12 selects, 0 clears); a first value of 0 too
// (review M1: a new entry has no value applied)
static void scn_sent_on_change() {
  fresh();
  drive("RYTM:SCN", 5);
  run_ms(30);
  CHECK(count({0xBD, 92, 5}) == 1 && count_cc(0xBD, 92) == 1);
  drive("RYTM:SCN", 0);
  run_ms(30);
  CHECK(count({0xBD, 92, 0}) == 1 && count_cc(0xBD, 92) == 2);
  drive("RYTM:SCN", 12);
  run_ms(30);
  CHECK(count({0xBD, 92, 12}) == 1 && count_cc(0xBD, 92) == 3);
  fresh();
  drive("RYTM:SCN", 0), drive("RYTM:PERF2", 0);
  run_ms(5);
  CHECK(count({0xBD, 92, 0}) == 1 && count({0xBD, 36, 0}) == 1);   // values of 0 sent
  printf("rytm out: RYTM:SCN goes out once a change, a first 0 included; PERF2 at 0 sent\n");
}

static void perf_follow_rate_limited() {
  fresh();
  drive("RYTM:PERF4", 0);
  run_ms(5);
  sweep("RYTM:PERF4", 0, 127, 100);
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
  drive("RYTM:PERF1", 100), drive("RYTM:SCN", 7);
  run_ms(5);
  CHECK(count({0xBE, 35, 100}) == 1 && count({0xBD, 92, 7}) == 1);   // PERF on 15, the scene on the auto channel
  c.perf_ch = 0;   // PERF CHANNEL OFF
  song::set_midi_config(c);
  fresh();
  drive("RYTM:PERF1", 100);
  run_ms(5);
  CHECK(count({0xBD, 35, 100}) == 1 && count_cc(0xBE, 35) == 0);   // OFF: the auto channel
  fresh();
  drive("RYTM:SCN", 0);
  run_ms(5);
  CHECK(count({0xBD, 92, 0}) == 1);   // 0 clears the Rytm's scene
  song::config_default(&c);
  song::set_midi_config(c);
  printf("rytm out: PERF on GLOBAL's PERF channel, else (OFF, not read) the auto channel\n");
}

static void host_cmd_replaces_ccs() {
  fresh();
  mod::rytm_host_cmd(true);
  drive("RYTM:SCN", 5), drive("RYTM:PERF4", 127);
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
  drive("RYTM:SCN", 3);
  run_ms(30);
  CHECK(count({0xBD, 92, 3}) == 1 && count({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 2, 3, 0xF7}) == 0);
  // a hello whose caps say "host command": the command again
  const uint8_t hello[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, (uint8_t)(1 | mod::kCapsHostCmd), 0xF7};
  engine_sysex(hello, (int)sizeof hello);
  g_out.clear();
  drive("RYTM:SCN", 0);
  run_ms(30);
  CHECK(count({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 2, 0, 0xF7}) == 1);
  const uint8_t hello0[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, 0xF7};   // caps without it: the CCs
  engine_sysex(hello0, (int)sizeof hello0);
  printf("rytm out: a refusal falls back to the CCs until the next hello\n");
}

static void restore_sends_nothing() {
  fresh();
  drive("RYTM:SCN", 5);
  run_ms(30);
  CHECK(count({0xBD, 92, 5}) == 1);
  g_out.clear();
  undrive("RYTM:SCN");
  run_ms(20);
  CHECK(g_out.empty() && !mod::find(T("RYTM:SCN")));   // the Rytm keeps its scene
  printf("rytm out: a value leaving sends nothing\n");
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
  scn_sent_on_change();
  perf_follow_rate_limited();
  perf_channel_from_global();
  host_cmd_replaces_ccs();
  mute_fallback_per_track_channel();
  ack_refused_falls_back();
  restore_sends_nothing();
  printf("test_rytm_out: PASS\n");
  return 0;
}
