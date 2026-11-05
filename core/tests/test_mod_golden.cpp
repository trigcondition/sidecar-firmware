// core/tests/test_mod_golden.cpp - the LFOs' golden traces (plan-050 Task 1, spec-050 1.9): 13 scripted scenarios
// through the real engine on the fake clock, driven through lfo::tick as the control loop drives it (a pass every
// 250 us), recorded on 0.4.0 before the modulation layer (core/mod) and compared after it, bit for bit. Per pass it
// hashes (FNV-1a 64) six field groups: (a) v[0..7].p[0..15] as raw float bits, (b) fx::control.knob, (c)
// ins::control.play and .peq, (d) out::control.lev / pan / wid, (e) the internal user_report calls (a user machine
// faked through hooks.user_report), (f) every byte sent to the Rytm; every 50 passes the running hash is a checkpoint.
// The file: "MODG", u32 scenarios, per scenario u32 checkpoints, that many u64, the scenario's final lfo::stats()
// (five u32). Modes: none = compare, --record = write, --dump = every pass's group hashes on stdout (diff a dump here
// against one from a dev worktree to find the first pass and group that moved). Built with -DGOLDEN_A64 (the Pi's
// compiler, tools/golden_a64.sh) it reads and writes lfo_trace_a64.bin: each compiler matches its own file.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "seq/seq.h"
#include "ui/page.h"

using namespace core;

#ifdef GOLDEN_A64
#define GOLDEN_NAME "lfo_trace_a64.bin"
#else
#define GOLDEN_NAME "lfo_trace.bin"
#endif

namespace {

constexpr int kScenarios = 13, kPasses = 4000, kEvery = 50;

// ---- FNV-1a 64
constexpr uint64_t kFnv0 = 1469598103934665603ull, kFnvP = 1099511628211ull;
uint64_t fnv(uint64_t h, const void* d, size_t n) {
  const uint8_t* b = (const uint8_t*)d;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * kFnvP;
  return h;
}
template <class T> uint64_t fnv_v(uint64_t h, const volatile T* d, size_t n) {   // a volatile array, element by element
  for (size_t i = 0; i < n; ++i) {
    T x = d[i];
    h = fnv(h, &x, sizeof x);
  }
  return h;
}

// ---- the world: the fake clock, the Rytm's side
uint64_t g_us = 1000000;
void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
uint64_t g_rytm = kFnv0;   // the bytes sent to the Rytm since the last pass
void rytm_out(const uint8_t* m, int n) { g_rytm = fnv(g_rytm, m, (size_t)n); }
int g_internal = 0;        // user_report calls with user_report_internal set (the LFO's parameter calls)
void user_report(int, int, int, int, int, int) {
  if (user_report_internal) ++g_internal;
}
void sysex(std::vector<uint8_t> m) { engine_sysex(m.data(), (int)m.size()); }
std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
// a voice report: 0x03 PARAMS or 0x04 TRIG for voice vi on machine byte `machine` (P1..P16 with their fractions)
void report(int cmd, int vi, int machine, const float* p, int note = 60) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(machine | vi)};
  if (cmd == 0x04) m.push_back((uint8_t)note), m.push_back(100);
  for (int k = 0; k < 16; ++k) {
    int iv = (int)p[k];
    m.push_back((uint8_t)iv), m.push_back((uint8_t)((p[k] - iv) * 128));
  }
  m.push_back(0xF7);
  sysex(m);
}
void voice_at(int cmd, int vi, int machine, int seed, int note = 60) {   // a report with values from a seed
  float p[16];
  for (int k = 0; k < 16; ++k) p[k] = (float)((seed * 7 + k * 13 + vi * 5) % 120) + (float)((seed + k) % 4) * 0.25f;
  report(cmd, vi, machine, p, note);
}
// the send FX's 0x20 for track 12 (both slots' knobs: TYPE, P1, P2..P5): FX1 pairs 1 2 3, FX2 pairs 0 4 5
void sendfx(const uint8_t a[6], const uint8_t b[6]) {
  uint8_t w[12];
  w[0] = b[0], w[1] = b[1];
  w[2] = a[0], w[3] = a[1], w[4] = a[2], w[5] = a[3], w[6] = a[4], w[7] = a[5];
  w[8] = b[2], w[9] = b[3], w[10] = b[4], w[11] = b[5];
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x20, 0x01, 12};
  m.insert(m.end(), w, w + 12);
  m.push_back(0xF7);
  CHECK(fx::take_sendfx(m.data(), (int)m.size()));
}
void sendfx_at(int seed) {
  uint8_t a[6] = {1, 10, (uint8_t)(20 + seed % 90), (uint8_t)(100 - seed % 80), 64, (uint8_t)(seed * 3 % 128)};
  uint8_t b[6] = {2, 30, 5, (uint8_t)(seed * 11 % 128), (uint8_t)(120 - seed % 50), 77};
  sendfx(a, b);
}
void lfo_set(int i, const char* kv) {
  char c[96], o[200];
  snprintf(c, sizeof c, "lfo=%d:%s", i, kv);
  CHECK(lfo::command(c, o, sizeof o));
  if (strstr(o, "cannot")) fprintf(stderr, "golden: %s -> %s\n", c, o);
  CHECK(!strstr(o, "cannot"));
}
void lfo_all(int i, const char* tgt, int par, int wav, int rate, int sync, int trig, int dep) {
  char b[40];
  snprintf(b, sizeof b, "tgt:%s", tgt), lfo_set(i, b);
  snprintf(b, sizeof b, "sync:%d", sync), lfo_set(i, b);
  snprintf(b, sizeof b, "par:%d", par), lfo_set(i, b);
  snprintf(b, sizeof b, "wav:%d", wav), lfo_set(i, b);
  snprintf(b, sizeof b, "rate:%d", rate), lfo_set(i, b);
  snprintf(b, sizeof b, "trig:%d", trig), lfo_set(i, b);
  snprintf(b, sizeof b, "dep:%d", dep), lfo_set(i, b);
}
void loaded() {   // a project load: the store filled from the idle passes, then the LFOs loaded from it
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
void open_project(const char* name) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x10};
  for (const char* c = name; *c; ++c) m.push_back((uint8_t)*c);
  m.push_back(0xF7);
  sysex(m);
  loaded();
}
void kit(int k) { sysex(msg(0x3B, {(uint8_t)k})); }
void wr_file(const std::string& p, const char* text) {
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}

// a MIDI clock on fx::clock_in: a tick every `tick_us` while on
struct Clk {
  bool on = false;
  uint64_t tick_us = 20833, next = 0;
  void start(uint64_t us) { on = true, tick_us = us, next = g_us; }
  void pass() {
    while (on && g_us >= next) fx::clock_in(0xF8), next += tick_us;
  }
} g_clk;

// the sequencer's side for scenario 9 (as test_seq.cpp's harness): messages that arrive at host frames
struct Wire {
  int64_t at;
  std::vector<uint8_t> m;
};
std::vector<Wire> g_wire;
bool g_seq = false;   // scenario 9: pre_block every pass
int64_t g_t0 = 0, g_next_k = 0;
uint32_t g_locks0 = 0;
constexpr double kPeriod = 6000;   // 120 BPM: 48000 * 60 / 120 / 4 frames a step
std::vector<uint8_t> blk4(int64_t frame) {
  uint32_t b = (uint32_t)(frame / 32) & ((1u << 28) - 1);
  return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)};
}
void seq_pass() {
  int64_t h = (int64_t)seq::frames();
  for (;;) {   // the Rytm's ticks of track 1 (length 16, speed 2) fired up to now, arriving 700 frames later
    int64_t tf = g_t0 + (int64_t)(g_next_k * kPeriod + 0.5);
    if (tf > h + 2) break;
    std::vector<uint8_t> b = blk4(tf);
    b.insert(b.end(), {1, 0, (uint8_t)(g_next_k % 16), 16, 2});
    g_wire.push_back({tf + 700, msg(0x2C, b)});
    ++g_next_k;
  }
  for (size_t i = 0; i < g_wire.size();) {
    if (g_wire[i].at <= h) {
      sysex(g_wire[i].m);
      g_wire.erase(g_wire.begin() + (long)i);
    } else {
      ++i;
    }
  }
  seq::pre_block();
}

// the remote page (scenario 10)
uint32_t g_ms = 0;
void msg3(uint8_t cmd, int a, int b, int c) { sysex({0xF0, 0x7D, 0x52, 0x54, cmd, 1, (uint8_t)a, (uint8_t)b, (uint8_t)c, 0xF7}); }

// ---- the scenarios: set up at pass -1, then actions before the tick of pass i
void begin_scenario() {
  lfo::reset();
  ins::reset();
  out::reset();
  ui::remote_reset();
  g_clk = Clk();
  g_seq = false;
  g_wire.clear();
  hooks.user_report = nullptr;
  for (int vi = 0; vi < kVoices; ++vi) voice_at(0x03, vi, kPlaits, vi);
  sendfx_at(0);
}

// a smooth random-like DESIGN (16 steps: value + 64 in hex, shape 2 SMOOTH): "pre:rnd" is not used here, its seed is
// the address of a stack copy (lfo.cpp preset()), so it differs run to run
const char* const kRndDes = "des:0f2" "6a2" "3c2" "7e2" "102" "552" "2d2" "7f2" "002" "482" "6c2" "1e2" "392" "702" "222" "5b2";

void script(int s, int i) {
  switch (s) {
    case 0:   // a SINE on V1 P3 depth 32, PLAITS
      if (i < 0) lfo_all(1, "V1", 2, lfo::kSine, 90, 0, lfo::kFree, 32);
      if (i == 2000) voice_at(0x03, 0, kPlaits, 9);
      break;
    case 1:   // every wave and the DESIGN presets on V2..V8, free and synced; a MIDI clock at 120, then 97 BPM
      if (i < 0) {
        fx::clock_in(0xFA);
        g_clk.start(20833);
        for (int l = 0; l < 7; ++l) {
          char t[4];
          snprintf(t, sizeof t, "V%d", l + 2);
          lfo_all(l + 1, t, (l * 3) % 16, l, l & 1 ? 2 + l : 70 + 8 * l, l & 1, lfo::kFree, 10 + 7 * l - 30);
        }
        lfo_all(8, "V2", 9, lfo::kDesign, 5, 1, lfo::kFree, 50);
        lfo_set(8, "pre:ramp");
      }
      if (i == 1000) lfo_set(8, "pre:tri"), lfo_set(2, "wav:7"), lfo_set(2, "pre:steps");
      if (i == 2000) g_clk.tick_us = 25773, lfo_set(8, "pre:steps"), lfo_set(4, "wav:7"), lfo_set(4, kRndDes);
      if (i == 3000) lfo_set(8, kRndDes), lfo_set(1, "sync:1"), lfo_set(1, "rate:4"), lfo_set(6, "sync:0");
      break;
    case 2:   // TRIG retrig, 0x04s between the ticks
      if (i < 0) {
        lfo_all(1, "V1", 2, lfo::kSawUp, 100, 0, lfo::kOnTrig, 40);
        lfo_all(2, "V1", 5, lfo::kSampleHold, 60, 0, lfo::kOnTrig, -30);
        lfo_all(3, "FX1", 1, lfo::kRandom, 110, 0, lfo::kOnTrig, 25);
        lfo_all(4, "V2", 3, lfo::kTri, 80, 0, lfo::kOnTrig, 20);
      }
      if (i > 0 && i % 300 == 0) voice_at(0x04, 0, kPlaits, i / 300, 48 + i / 300);
      if (i > 0 && i % 700 == 0) voice_at(0x04, 1, kPlaits, i / 700);
      if (i > 0 && i % 450 == 0) lfo::midi_in(0x90 | (i / 450 % 12), 60, 100);
      if (i == 2500) lfo::midi_in(0x80, 60, 0), lfo::midi_in(0x9D, 60, 100), lfo::midi_in(0x91, 60, 0);
      break;
    case 3:   // PLAY retrig: FA, FC, FB, F2
      if (i < 0) {
        lfo_all(1, "V1", 2, lfo::kSawDown, 70, 0, lfo::kOnPlay, 45);
        lfo_all(2, "V3", 4, lfo::kSine, 8, 1, lfo::kOnPlay, -40);
        lfo_all(3, "FX2", 2, lfo::kSquare, 50, 0, lfo::kOnPlay, 30);
        g_clk.start(20833);
      }
      if (i == 100) fx::clock_in(0xFA);
      if (i == 1200) fx::clock_in(0xFC);
      if (i == 1800) fx::clock_in(0xFB);
      if (i == 2400) fx::clock_in(0xF2, 5);
      if (i == 2900) fx::clock_in(0xFA);
      if (i == 3300) g_clk.on = false;
      break;
    case 4:   // FX1 P2..P5 and FX2, 0x20 reports between
      if (i < 0) {
        for (int k = 0; k < 4; ++k) lfo_all(k + 1, "FX1", k, k, 60 + 15 * k, 0, lfo::kFree, 20 + 10 * k - 35);
        lfo_all(5, "FX2", 0, lfo::kRandom, 100, 0, lfo::kFree, 63);
        lfo_all(6, "FX2", 3, lfo::kSquare, 3, 1, lfo::kFree, -64);
        g_clk.start(20833);
      }
      if (i > 0 && i % 400 == 0) sendfx_at(i / 400);
      if (i == 2200) lfo_set(1, "off");
      break;
    case 5:   // I1 A1..B4, I1 params 8..11 (the POST EQ), B2, O3 LEV PAN WID
      if (i < 0) {
        char b[80];
        ins::set_type(0, 0, ins::kDelay), ins::set_type(0, 1, ins::kTrem), ins::set_type(ins::kBus0 + 1, 0, ins::kComp);
        CHECK(ins::command("ins=v1:a:p1:100", b, sizeof b) && ins::command("ins=v1:b:p3:20", b, sizeof b));
        lfo_all(1, "I1", 0, lfo::kSine, 100, 0, lfo::kFree, 40);
        lfo_all(2, "I1", 3, lfo::kTri, 90, 0, lfo::kFree, -50);
        lfo_all(3, "I1", 5, lfo::kSawUp, 80, 0, lfo::kFree, 63);
        lfo_all(4, "I1", 8, lfo::kRandom, 110, 0, lfo::kFree, 33);
        lfo_all(5, "I1", 11, lfo::kSquare, 70, 0, lfo::kFree, -20);
        lfo_all(6, "B2", 1, lfo::kSine, 95, 0, lfo::kFree, 60);
        lfo_all(7, "O3", 0, lfo::kSine, 100, 0, lfo::kFree, 45);
        lfo_all(8, "O3", 1, lfo::kTri, 85, 0, lfo::kFree, -64);
      }
      if (i == 2000) lfo_set(1, "par:7"), lfo_set(2, "par:4"), lfo_set(4, "par:9"), lfo_set(5, "par:10"), lfo_set(7, "par:2");
      if (i == 3000) lfo_set(3, "par:1"), lfo_set(6, "par:6"), lfo_set(8, "off");
      break;
    case 6:   // two LFOs on one target, one switched off, then both (restore), then back
      if (i < 0) {
        lfo_all(1, "V1", 2, lfo::kSine, 100, 0, lfo::kFree, 40);
        lfo_all(2, "V1", 2, lfo::kSawDown, 90, 0, lfo::kFree, 35);
        lfo_all(3, "FX1", 2, lfo::kTri, 90, 0, lfo::kFree, 30);
        lfo_all(4, "FX1", 2, lfo::kSine, 70, 0, lfo::kFree, -30);
      }
      if (i == 1000) lfo_set(2, "off"), lfo_set(4, "off");
      if (i == 2000) lfo_set(1, "off"), lfo_set(3, "off");
      if (i == 2500) voice_at(0x03, 0, kPlaits, 3), sendfx_at(5);
      if (i == 3000) lfo_all(2, "V1", 2, lfo::kSquare, 110, 0, lfo::kFree, -60), lfo_all(4, "FX1", 2, lfo::kSquare, 100, 0, lfo::kFree, 60);
      break;
    case 7:   // a user machine on V1: a 0x33 naming U1 for voice 0, 0x03 reports with 0x50, no package
      if (i < 0) {
        hooks.user_report = user_report;
        sysex(msg(0x33, {0, 1, 35, 0x50}));
        voice_at(0x03, 0, 0x50, 1);
        CHECK(v[0].user == 0);
        lfo_all(1, "V1", 2, lfo::kSine, 120, 0, lfo::kFree, 50);
        lfo_all(2, "V1", 7, lfo::kRandom, 127, 0, lfo::kFree, 30);
      }
      if (i > 0 && i % 500 == 0) voice_at(0x03, 0, 0x50, i / 500);
      if (i > 0 && i % 777 == 0) voice_at(0x04, 0, 0x50, i / 777);
      if (i == 3000) lfo_set(1, "off");
      if (i == 3600) lfo_set(2, "off");
      if (i == 3999) {
        voice_at(0x03, 0, kPlaits, 0);
        sysex(msg(0x33, {0, 1, 35, 0}));
      }
      break;
    case 8:   // three LFOs through a kit change (0x3B) and a project change (0x10)
      if (i < 0) {
        open_project("GOLDA");
        kit(2);
        lfo_all(1, "V1", 2, lfo::kSine, 100, 0, lfo::kFree, 40);
        lfo_all(2, "FX1", 1, lfo::kTri, 90, 0, lfo::kFree, 30);
        lfo_all(3, "I2", 2, lfo::kSawUp, 80, 0, lfo::kFree, 50);
      }
      if (i == 1000) {   // kit 4: its own file (two LFOs)
        kit(3);
        CHECK(lfo::kit() == 3 && lfo::get(0).target == lfo::kTVoice + 2 && lfo::get(1).target == lfo::kTFx + 1);
      }
      if (i == 1500) lfo_all(3, "O1", 1, lfo::kSine, 100, 0, lfo::kFree, 40);
      if (i == 2000) kit(2);   // back: kit 3's LFOs as saved at the change
      if (i == 3000) open_project("GOLDB");
      if (i == 3300) lfo_all(1, "V2", 4, lfo::kSquare, 100, 0, lfo::kFree, -40);
      if (i == 3999) open_project("GOLDA");
      break;
    case 9:   // a sequencer step lock on V1 P3 under an LFO on V1 P3: the lock is the base
      if (i < 0) {
        char b[200];
        voice_at(0x03, 0, kFm, 2);
        CHECK(seq::command("seq=track:1:on", b, sizeof b));
        if (!strstr(b, "mask 0x001")) fprintf(stderr, "golden: %s\n", b);
        CHECK(strstr(b, "mask 0x001"));
        seq::Track* t = seq::edit_track(0, true);
        CHECK(t);
        t->length = 16;
        CHECK(seq::add_note(t->step[0], {48, 100, 48, 0, 0}));
        CHECK(seq::add_note(t->step[2], {50, 100, 48, 0, 0}));
        CHECK(seq::set_lock(t->step[2], 2, 100 << 7 | 32) && seq::set_lock(t->step[2], 4, 12 << 7));
        CHECK(seq::add_note(t->step[5], {52, 100, 48, 0, 0}));
        seq::touched();
        seq::set_latency(192);
        int tp = 120 * 120;
        sysex(msg(0x2D, {7, (uint8_t)(tp >> 14), (uint8_t)(tp >> 7 & 0x7F), (uint8_t)(tp & 0x7F)}));
        lfo_all(1, "V1", 2, lfo::kSine, 100, 0, lfo::kFree, 30);
        lfo_all(2, "V1", 4, lfo::kTri, 90, 0, lfo::kFree, -25);
        g_seq = true;
        g_locks0 = seq::stats().locks_applied;
        g_t0 = (int64_t)seq::frames() + 3000, g_next_k = 0;
        std::vector<uint8_t> b2 = {1, 1};
        std::vector<uint8_t> x = blk4(g_t0);
        b2.insert(b2.end(), x.begin(), x.end());
        g_wire.push_back({g_t0 + 700, msg(0x2D, b2)});
      }
      if (i == 1700) voice_at(0x03, 0, kFm, 6);   // the Rytm's knob under the lock: a new base after it
      if (i == 3500) lfo_set(1, "off");
      if (i == 3999) {
        char b[200];
        CHECK(seq::stats().locks_applied >= g_locks0 + 2);   // the lock and the bases back: both applied
        int64_t now = (int64_t)seq::frames();
        std::vector<uint8_t> b2 = {1, 0};
        std::vector<uint8_t> x = blk4(now - 700);
        b2.insert(b2.end(), x.begin(), x.end());
        sysex(msg(0x2D, b2));
        seq::pre_block();
        CHECK(seq::command("seq=track:1:off", b, sizeof b));
        g_seq = false;
      }
      break;
    case 10:   // the remote page's set_param / on_enc on a modulated param (their 0x03 and frames in group f)
      if (i < 0) {
        ui::list_pacing(false);
        voice_at(0x03, 0, kPlaits, 4);
        lfo_all(1, "V1", 1, lfo::kSine, 100, 0, lfo::kFree, 30);
        lfo_all(2, "V1", 0, lfo::kTri, 60, 0, lfo::kFree, 20);
        msg3(0x30, 0, 1, 0);
        g_ms = 1000;
        ui::remote_tick(g_ms);
        for (int k = 0; k < 8 && ui::remote_stats().page != 1; ++k)
          msg3(0x31, 0, 0x41, 1), msg3(0x31, 0, 0x41, 0), ui::remote_tick(g_ms += 30);
        CHECK(ui::remote_stats().page == 1 && ui::remote_stats().voice == 0);
      }
      if (i >= 0 && i % 40 == 0) ui::remote_tick(g_ms += 10);
      if (i > 0 && i % 300 == 0) msg3(0x32, 1, 64 + (i / 300 % 2 ? 5 : -3), 0);   // HRM (P2, the LFO's)
      if (i > 0 && i % 900 == 0) msg3(0x32, 0, 64 + 40, 0);                      // ENG (P1, the other LFO's)
      if (i == 2000) voice_at(0x03, 0, kPlaits, 8);
      if (i == 3999) {
        CHECK(ui::remote_stats().edits >= 10);   // set_param made its 0x03s
        msg3(0x30, 0, 0, 1), ui::list_pacing(true);
      }
      break;
    case 11:   // lfo=1:tgt:V2 and lfo=1:par:5 while LFO 1 runs: the old entry restored, the new made in one tick
      if (i < 0) {
        lfo_all(1, "V1", 2, lfo::kSine, 100, 0, lfo::kFree, 40);
        lfo_all(2, "FX2", 1, lfo::kSine, 100, 0, lfo::kFree, 40);
      }
      if (i == 1000) lfo_set(1, "tgt:V2");
      if (i == 1700) lfo_set(1, "par:5");
      if (i == 2400) lfo_set(2, "tgt:FX1"), lfo_set(1, "tgt:O2");
      if (i == 3100) lfo_set(1, "tgt:V1"), lfo_set(2, "tgt:I4");
      break;
    case 12:   // an LFO on a list knob: PLAITS MODEL, JUNO PAT (and voices.cpp's base_of on JUNO's knobs)
      if (i < 0) {
        voice_at(0x03, 1, kJuno, 3);
        lfo_all(1, "V1", 0, lfo::kSawUp, 70, 0, lfo::kFree, 63);
        lfo_all(2, "V2", 0, lfo::kTri, 60, 0, lfo::kFree, 40);
        lfo_all(3, "V2", 2, lfo::kSine, 90, 0, lfo::kFree, 30);
      }
      if (i > 0 && i % 250 == 0) voice_at(0x03, 1, kJuno, 3 + i / 250);
      if (i > 0 && i % 600 == 0) voice_at(0x04, 1, kJuno, 2 + i / 600);
      if (i > 0 && i % 650 == 0) voice_at(0x04, 0, kPlaits, i / 650);
      break;
  }
}

struct Golden {
  std::vector<std::vector<uint64_t>> cp;   // per scenario its checkpoints
  std::vector<std::vector<uint32_t>> st;   // per scenario the final lfo::stats()
  bool write(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite("MODG", 1, 4, f);
    uint32_t n = (uint32_t)cp.size();
    fwrite(&n, 4, 1, f);
    for (size_t s = 0; s < cp.size(); ++s) {
      uint32_t k = (uint32_t)cp[s].size();
      fwrite(&k, 4, 1, f);
      fwrite(cp[s].data(), 8, k, f);
      fwrite(st[s].data(), 4, 5, f);
    }
    return fclose(f) == 0;
  }
  bool read(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char m[4];
    uint32_t n = 0;
    bool ok = fread(m, 1, 4, f) == 4 && !memcmp(m, "MODG", 4) && fread(&n, 4, 1, f) == 1 && n <= 64;
    for (uint32_t s = 0; ok && s < n; ++s) {
      uint32_t k = 0;
      ok = fread(&k, 4, 1, f) == 1 && k <= 100000;
      std::vector<uint64_t> c(k);
      std::vector<uint32_t> t(5);
      ok = ok && fread(c.data(), 8, k, f) == k && fread(t.data(), 4, 5, f) == 5;
      cp.push_back(c), st.push_back(t);
    }
    fclose(f);
    return ok;
  }
  bool same(const Golden& o, int* s, int* c) const {
    for (size_t i = 0; i < cp.size() || i < o.cp.size(); ++i) {
      *s = (int)i;
      if (i >= cp.size() || i >= o.cp.size()) return *c = -1, false;
      for (size_t k = 0; k < cp[i].size() || k < o.cp[i].size(); ++k)
        if (k >= cp[i].size() || k >= o.cp[i].size() || cp[i][k] != o.cp[i][k]) return *c = (int)k, false;
      if (st[i] != o.st[i]) return *c = -2, false;   // the stats
    }
    return true;
  }
};

uint64_t run(int scenario, std::vector<uint64_t>* checkpoints, bool dump) {
  begin_scenario();
  script(scenario, -1);
  uint64_t h = kFnv0;
  for (int i = 0; i < kPasses; ++i) {
    g_rytm = kFnv0, g_internal = 0;
    script(scenario, i);
    g_clk.pass();
    if (g_seq) seq_pass();
    at(g_us + 250);
    lfo::tick(now_us());
    uint64_t g[6];
    g[0] = kFnv0;
    for (int vi = 0; vi < kVoices; ++vi) g[0] = fnv(g[0], v[vi].p, sizeof v[vi].p);
    g[1] = fnv_v(kFnv0, &fx::control.knob[0][0], sizeof fx::control.knob);
    g[2] = fnv_v(fnv_v(kFnv0, &ins::control.play[0][0][0], sizeof ins::control.play), &ins::control.peq[0][0],
                 sizeof ins::control.peq);
    g[3] = fnv_v(fnv_v(fnv_v(kFnv0, out::control.lev, kVoices), out::control.pan, kVoices), out::control.wid, kVoices);
    g[4] = fnv(kFnv0, &g_internal, sizeof g_internal);
    g[5] = g_rytm;
    h = fnv(h, g, sizeof g);
    if (dump)
      printf("%2d %4d %016llx %016llx %016llx %016llx %016llx %016llx\n", scenario, i, (unsigned long long)g[0],
             (unsigned long long)g[1], (unsigned long long)g[2], (unsigned long long)g[3], (unsigned long long)g[4],
             (unsigned long long)g[5]);
    if (i % kEvery == kEvery - 1) checkpoints->push_back(h);
  }
  return h;
}

std::string golden_path(const char* file, const char* name) {
  std::string f = file;
  return f.substr(0, f.rfind('/')) + "/golden/mod/" + name;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "";
  bool dump = !strcmp(mode, "--dump");
  char tmpl[] = "/tmp/core_modgold_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples'").c_str()) == 0);
  std::string card = tmpl;
  // scenario 8's kit 4: a file of its own (two LFOs), read at the kit change from the RAM store
  wr_file(card + "/.rytm/lfo/GOLDA/k004.txt",
          "# Sidecar LFOs\n1 V3 5 1 90 0 0 33 64 0\n2 FX2 2 3 4 1 0 -21 64 0\n");
  host_set_card(tmpl);
  hooks.to_rytm = rytm_out;
  at(g_us);
  CHECK(engine_begin());
  open_project("GOLD");
  Golden g;
  for (int s = 0; s < kScenarios; ++s) {
    std::vector<uint64_t> cp;
    run(s, &cp, dump);
    const lfo::Stats& st = lfo::stats();
    g.cp.push_back(cp);
    g.st.push_back({st.ticks, st.voice_sets, st.user_calls, st.fx_sets, st.restores});
    if (!dump)
      printf("scenario %2d: ticks %u voice_sets %u user_calls %u fx_sets %u restores %u\n", s, st.ticks, st.voice_sets,
             st.user_calls, st.fx_sets, st.restores);
  }
  if (dump) return 0;
  std::string path = golden_path(__FILE__, GOLDEN_NAME);
  if (!strcmp(mode, "--record")) {
    CHECK(g.write(path));
    printf("test_mod_golden: recorded %s\n", path.c_str());
    return 0;
  }
  Golden want;
  if (!want.read(path)) fprintf(stderr, "golden: cannot read %s\n", path.c_str());
  CHECK(want.cp.size() == (size_t)kScenarios);
  int s = -1, c = -1;
  if (!g.same(want, &s, &c)) {
    fprintf(stderr, "golden: scenario %d, checkpoint %d differs (-2: the stats; passes %d..%d); run --dump here and on dev and diff\n",
            s, c, c * kEvery, c * kEvery + kEvery - 1);
    CHECK(false);
  }
  puts("test_mod_golden: ok");
  return 0;
}
