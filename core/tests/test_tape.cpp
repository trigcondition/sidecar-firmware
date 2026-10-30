// core/tests/test_tape.cpp - SPACE (send FX TYPE 5, core/fx/tape_fx.h), the RE-201-style tape echo: the labels and
// zones the Rytm shows, MODE -> heads (+ spring), head timing at a TIME, the tempo-synced note values from the MIDI
// clock, TIME's tape-speed glide, REPEAT's runaway bounded by the saturation, the wow / flutter depth per AGE and a
// smooth read (no zipper), silence in -> silence out after the tail with no subnormals or NaN, the incremental clear,
// TYPE 5 through SendFx, and the CPU per 12-frame block beside the other send FX (the Mac as a proxy for a Pi 4 core).
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "check.h"
#include "timing.h"
#include "fx/send_fx.h"

using namespace core;
using namespace core::fx;

static const float kSr = 48000.0f;
static const float kA = 1.0f - std::exp(-1.0f / (0.010f * kSr));   // SendFx's 10 ms smoothing

// The rig: TapeFx alone, 12-frame blocks, a MIDI clock at fpt frames a tick (0: none)
struct Rig {
  std::unique_ptr<TapeFx> fx = std::make_unique<TapeFx>();
  uint8_t k[4] = {0, 0, 0, 0};   // P2..P5: TIME RPT MODE AGE
  double fpt = 0, next = 0;
  long frame = 0;
  uint32_t ticks = 0;
  std::vector<float> l, r;
  void run(int frames, float (*x)(long)) {
    float in[12], o[12][2];
    for (int i = 0; i < frames; i += 12) {
      int n = frames - i < 12 ? frames - i : 12;
      for (int j = 0; j < n; ++j) in[j] = x ? x(frame + j) : 0.0f, o[j][0] = o[j][1] = 0.0f;
      if (fpt > 0)
        while (next < (double)(frame + n)) ++ticks, next += fpt;   // the ticks due within this block, as the board takes them
      fx->Process(in, n, o, k, kSr, kA, ClockWord::pack((int32_t)ticks, ticks, fpt > 0));
      for (int j = 0; j < n; ++j) l.push_back(o[j][0]), r.push_back(o[j][1]);
      frame += n;
    }
  }
};
static float impulse(long i) { return i == 0 ? 0.5f : 0.0f; }
static float sine1k(long i) { return 0.25f * std::sin(6.283185307f * 1000.0f * (float)i / kSr); }
static uint32_t g_rng = 7;
static float burst(long i) {   // 200 ms of noise at 0.5
  if (i >= 9600) return 0.0f;
  g_rng = g_rng * 1664525u + 1013904223u;
  return 0.5f * ((float)(g_rng >> 8) / 8388608.0f - 1.0f);
}
static bool clean(float v) { return v == v && (v == 0.0f || std::fabs(v) >= 1e-30f) && std::fabs(v) < 1e3f; }

int main() {
  // ---- the labels the Rytm shows (0x24) and the names
  {
    CHECK(kTape == 5 && plays(kTape) && strcmp(type_name(kTape), "SPACE") == 0 && type_from_name("space") == kTape);
    const KnobLabels* lb = labels(kTape);
    CHECK(lb && !strcmp(lb->label[0], "TIME") && !strcmp(lb->label[1], "RPT") && !strcmp(lb->label[2], "MODE") &&
          !strcmp(lb->label[3], "AGE"));
    CHECK(lb->nzones == 12 && lb->nzones <= kMaxZones);
    const char* sync[4] = {"1/16", "1/8T", "1/8", "1/8D"};
    const char* mode[8] = {"H1", "H2", "H3", "H1+2", "H2+3", "H123", "H23+S", "H123S"};
    for (int z = 0; z < 4; ++z) {   // TIME: the number up to 63, the note values from 64 (continuous: no step)
      const Zone& q = lb->zones[z];
      CHECK(q.knob == 0 && q.lo == 64 + 16 * z && !q.show && !q.step && !strcmp(q.name, sync[z]));
    }
    for (int z = 0; z < 8; ++z) {   // MODE: a pure selector, stepped one mode a detent
      const Zone& q = lb->zones[4 + z];
      CHECK(q.knob == 2 && q.lo == 16 * z && !q.show && q.step && !strcmp(q.name, mode[z]) && strlen(q.name) <= 5);
    }
    uint8_t m[160];
    CHECK(sysex_labels(m, kTape) == 24 + 12 * 8 && memcmp(&m[7], "TIME", 4) == 0);
  }
  // ---- MODE -> heads, spring; TIME -> free delay or note value
  {
    const uint8_t heads[8] = {1, 2, 4, 3, 6, 7, 6, 7};
    for (int z = 0; z < 8; ++z)
      for (int v = 16 * z; v < 16 * z + 16; ++v) {
        CHECK(TapeFx::Mode((uint8_t)v) == z && TapeFx::kModeHeads[z] == heads[z]);
        CHECK(TapeFx::ModeSpring(z) == (z >= 6));
      }
    CHECK(TapeFx::SyncTicks(0) == 0 && TapeFx::SyncTicks(63) == 0);
    CHECK(TapeFx::SyncTicks(64) == 6 && TapeFx::SyncTicks(79) == 6 && TapeFx::SyncTicks(80) == 8 &&
          TapeFx::SyncTicks(96) == 12 && TapeFx::SyncTicks(112) == 18 && TapeFx::SyncTicks(127) == 18);
    CHECK(std::fabs(TapeFx::FreeDelay(0, kSr) - 0.040f * kSr) < 1.0f);
    CHECK(std::fabs(TapeFx::FreeDelay(63, kSr) - 0.400f * kSr) < 2.0f);
    for (int v = 1; v < 64; ++v) CHECK(TapeFx::FreeDelay((uint8_t)v, kSr) > TapeFx::FreeDelay((uint8_t)(v - 1), kSr));
  }
  // ---- head timing: H123 at TIME 32, an impulse: echoes at d1, 2 d1, 3 d1 (the heads' 1:2:3), none before
  {
    Rig g;
    g.k[0] = 32, g.k[2] = 80;   // H123, REPEAT 0, AGE 0
    g.run(48000, impulse);
    const float d1 = TapeFx::FreeDelay(32, kSr);
    CHECK(std::fabs(g.fx->HeadDelay(0) - d1) < 0.01f && std::fabs(g.fx->HeadDelay(2) - 3 * d1) < 0.03f);
    for (int h = 1; h <= 3; ++h) {
      int at = (int)std::lround(h * d1), best = -1;
      float peak = 0;
      for (int i = at - 40; i <= at + 40; ++i)
        if (std::fabs(g.l[i] + g.r[i]) > peak) peak = std::fabs(g.l[i] + g.r[i]), best = i;
      CHECK(std::abs(best - at) <= 2 && peak > 0.05f);
    }
    float before = 0;
    for (int i = 0; i < (int)d1 - 10; ++i) before = std::fmax(before, std::fabs(g.l[i]) + std::fabs(g.r[i]));
    CHECK(before < 1e-6f);
    // H1 alone: one echo, nothing at 2 d1 or 3 d1 (REPEAT 0)
    Rig h1;
    h1.k[0] = 32;
    h1.run(48000, impulse);
    float e2 = 0;
    for (int i = (int)(1.5f * d1); i < 48000; ++i) e2 = std::fmax(e2, std::fabs(h1.l[i]));
    CHECK(e2 < 1e-4f);
    // H2 alone: the first echo at 2 d1
    Rig h2;
    h2.k[0] = 32, h2.k[2] = 16;
    h2.run(48000, impulse);
    float e1 = 0;
    for (int i = 0; i < (int)(1.5f * d1); ++i) e1 = std::fmax(e1, std::fabs(h2.l[i]));
    CHECK(e1 < 1e-6f);
  }
  // ---- tempo sync: a 24 ppqn clock at 120 BPM (1000 frames a tick) and 90 BPM; the note values on the shortest head
  {
    const struct { double fpt; uint8_t v; float ticks; } cs[] = {
        {1000.0, 64, 6}, {1000.0, 80, 8}, {1000.0, 96, 12}, {1000.0, 112, 18}, {48000.0 * 60 / 90 / 24, 96, 12}};
    for (auto& c : cs) {
      Rig g;
      g.fpt = c.fpt, g.k[0] = c.v;
      g.run(4 * 48000, nullptr);
      CHECK(std::fabs(g.fx->FramesPerTick() - (float)c.fpt) < 0.01f * (float)c.fpt);
      CHECK(std::fabs(g.fx->HeadDelay(0) - c.ticks * (float)c.fpt) < 0.01f * c.ticks * (float)c.fpt);
    }
    // no clock: 120 BPM at 48 kHz (1000 frames a tick); 1/8 = 12000
    Rig g;
    g.k[0] = 96;
    g.run(48000, nullptr);
    CHECK(std::fabs(g.fx->HeadDelay(0) - 12000.0f) < 1.0f);
    // the clock stops: the last tempo stays
    Rig s;
    s.fpt = 48000.0 * 60 / 100 / 24, s.k[0] = 96;
    s.run(2 * 48000, nullptr);
    s.fpt = 0;
    s.run(2 * 48000, nullptr);
    CHECK(std::fabs(s.fx->HeadDelay(0) - 12 * 1200.0f) < 0.01f * 12 * 1200.0f);
  }
  // ---- TIME is tape speed: a change glides (the echoes bend in pitch), never jumps; settled within a second
  {
    Rig g;
    g.k[0] = 20;
    g.run(24000, nullptr);
    const float a = TapeFx::FreeDelay(20, kSr), b = TapeFx::FreeDelay(40, kSr);
    CHECK(std::fabs(g.fx->HeadDelay(0) - a) < 0.01f);
    g.k[0] = 40;
    g.run(12, nullptr);
    float d = g.fx->HeadDelay(0);
    CHECK(d > a && d < a + 0.01f * (b - a));   // one block: barely moved
    g.run(4800, nullptr);
    d = g.fx->HeadDelay(0);
    CHECK(d > a + 0.3f * (b - a) && d < a + 0.9f * (b - a));   // 100 ms: on its way
    g.run(48000, nullptr);
    CHECK(std::fabs(g.fx->HeadDelay(0) - b) < 0.5f);
  }
  // ---- REPEAT: below 100 % a tail that dies out to silence (no subnormals, no NaN); above it the runaway, bounded
  {
    Rig g;
    g.k[0] = 20, g.k[1] = 60, g.k[2] = 80, g.k[3] = 64;   // H123, REPEAT 60 %, AGE 64
    g.run(30 * 48000, burst);
    bool ok = true;
    for (size_t i = 0; i < g.l.size(); ++i) ok &= clean(g.l[i]) && clean(g.r[i]);
    CHECK(ok);
    float tail = 0;
    for (size_t i = g.l.size() - 48000; i < g.l.size(); ++i) tail = std::fmax(tail, std::fabs(g.l[i]) + std::fabs(g.r[i]));
    CHECK(tail < 1e-7f);
    // its last second is exactly zero once the flush has caught every value (the read of zeros is zero)
    g.run(30 * 48000, nullptr);
    float tail2 = 0;
    for (size_t i = g.l.size() - 48000; i < g.l.size(); ++i) tail2 = std::fmax(tail2, std::fabs(g.l[i]) + std::fabs(g.r[i]));
    CHECK(tail2 == 0.0f);
  }
  for (int mode = 0; mode < 8; ++mode) {   // REPEAT 127 in every MODE: it keeps going, never past the tape's ceiling
    Rig g;
    g.k[0] = 10, g.k[1] = 127, g.k[2] = (uint8_t)(16 * mode), g.k[3] = 100;
    g.run(20 * 48000, burst);
    float peak = 0;
    double e = 0;
    bool ok = true;
    for (size_t i = 0; i < g.l.size(); ++i) {
      ok &= clean(g.l[i]) && clean(g.r[i]);
      peak = std::fmax(peak, std::fmax(std::fabs(g.l[i]), std::fabs(g.r[i])));
      if (i >= g.l.size() - 48000) e += (double)g.l[i] * g.l[i];
    }
    float last = (float)std::sqrt(e / 48000);
    CHECK(ok && peak < 2.0f && last > 0.02f);   // still sounding after 20 s: self-oscillation
  }
  {   // the dub siren: an H1 runaway with TIME swept hard (each pass pitched) keeps howling, and holds when TIME rests
    Rig g;
    g.k[0] = 30, g.k[1] = 120, g.k[2] = 0, g.k[3] = 80;
    g.run(2 * 48000, burst);
    for (int b = 0; b < 8 * 4000; ++b) {   // 8 s: TIME 5..55 at 0.25 Hz
      g.k[0] = (uint8_t)(30 + 25 * std::sin(6.283185307f * 0.25f * (float)b * 12 / kSr));
      g.run(12, nullptr);
    }
    g.k[0] = 30;
    g.run(6 * 48000, nullptr);
    double e = 0;
    for (size_t i = g.l.size() - 48000; i < g.l.size(); ++i) e += (double)g.l[i] * g.l[i];
    CHECK(std::sqrt(e / 48000) > 0.02);
  }
  {   // 100 % is the edge: REPEAT 95 decays, a burst's echo 10 s later well below its start
    Rig g;
    g.k[0] = 10, g.k[1] = 95, g.k[2] = 80;
    g.run(12 * 48000, burst);
    double e0 = 0, e1 = 0;
    for (int i = 9600; i < 48000; ++i) e0 += (double)g.l[i] * g.l[i];
    for (size_t i = g.l.size() - 38400; i < g.l.size(); ++i) e1 += (double)g.l[i] * g.l[i];
    CHECK(e1 < 0.1 * e0);
  }
  // ---- wow and flutter: AGE 0 still, AGE 127 the full depth, AGE 64 about a quarter of it (squared); smooth reads
  {
    float range[3];
    const uint8_t ages[3] = {0, 64, 127};
    for (int a = 0; a < 3; ++a) {
      Rig g;
      g.k[0] = 32, g.k[3] = ages[a];
      g.run(24000, nullptr);
      float lo = 1e9f, hi = -1e9f;
      for (int b = 0; b < 4 * 4000; ++b) {   // 4 s, the delay each block
        g.run(12, nullptr);
        lo = std::fmin(lo, g.fx->HeadDelay(0)), hi = std::fmax(hi, g.fx->HeadDelay(0));
      }
      range[a] = hi - lo;
    }
    // the wow's depth: pitch +-0.6 % at 0.55 Hz (A = 0.006 / (2 pi 0.55) s = 83 samples), the first head 0.7 of it
    CHECK(range[0] < 0.01f);
    CHECK(range[2] > 2 * 0.7f * 0.5f * TapeFx::WowSamples(kSr) && range[2] < 2 * (0.7f * TapeFx::WowSamples(kSr) + 4));
    CHECK(range[1] > 0.12f * range[2] && range[1] < 0.45f * range[2]);
    // a 1 kHz sine through H1 at AGE 127: no step from sample to sample beyond the sine's own slope (+ the wow's)
    Rig g;
    g.k[0] = 32, g.k[3] = 127;
    g.run(4 * 48000, sine1k);
    float dmax = 0, amp = 0;
    for (size_t i = 48000; i < g.l.size(); ++i)
      dmax = std::fmax(dmax, std::fabs(g.l[i] - g.l[i - 1])), amp = std::fmax(amp, std::fabs(g.l[i]));
    CHECK(amp > 0.05f && dmax < 1.1f * amp * 6.283185307f * 1000.0f / kSr);
  }
  // ---- the clear in pieces: true when done, then silence in -> silence out at once (the tape blank)
  {
    Rig g;
    g.k[0] = 20, g.k[1] = 90, g.k[2] = 112;
    g.run(48000, burst);
    int calls = 0;
    while (!g.fx->ClearStep(24 * 1024)) ++calls;
    CHECK(calls >= (int)(sizeof(float) * TapeFx::kTape / (24 * 1024)) - 1);
    g.l.clear(), g.r.clear();
    g.run(48000, nullptr);
    float m = 0;
    for (size_t i = 0; i < g.l.size(); ++i) m = std::fmax(m, std::fabs(g.l[i]) + std::fabs(g.r[i]));
    CHECK(m == 0.0f);
  }
  // ---- through SendFx: TYPE 5 on FX 1, a send from BD: an echo comes back, no fault; a change to OFF and back clean
  {
    auto fx = std::make_unique<SendFx>(kSr);
    memset((void*)&control, 0, sizeof control);
    control.knob[0][0] = kTape, control.knob[0][2] = 20, control.knob[0][3] = 50, control.knob[0][4] = 80;
    control.send[0][0] = 127;
    float st[12][kStems] = {}, o[12][2];
    float peak = 0;
    for (int b = 0; b < 4000; ++b) {
      for (int j = 0; j < 12; ++j) st[j][0] = (b * 12 + j) % 12000 < 200 ? 0.3f : 0.0f;
      fx->Process(st, 12, o, control);
      for (int j = 0; j < 12; ++j) peak = std::fmax(peak, std::fabs(o[j][0]));
    }
    CHECK(peak > 0.05f && fx->Faults() == 0 && state(0) == kStReady);
    control.knob[0][0] = kOff;
    for (int b = 0; b < 2000; ++b) fx->Process(st, 12, o, control);
    CHECK(!fx->Clearing(0));   // the tape zeroed in pieces while off
    control.knob[0][0] = kTape;
    for (int j = 0; j < 12; ++j) st[j][0] = 0.0f;
    float m = 0;
    for (int b = 0; b < 4000; ++b) {
      fx->Process(st, 12, o, control);
      for (int j = 0; j < 12; ++j) m = std::fmax(m, std::fabs(o[j][0]) + std::fabs(o[j][1]));
    }
    CHECK(m == 0.0f);   // back from a clean tape: no old echo
  }
  // ---- CPU per 12-frame block, all sends up (test_send_fx's ratio: Mac x13.2 = a Pi 4 core)
  {
    const double kPiRatio = 3.7 / 0.28, kBlockUs = 12 / kSr * 1e6;
    struct C { uint8_t t1, m1, t2, m2; const char* name; } cases[] = {
        {kTape, 0, kOff, 0, "space H1"},
        {kTape, 80, kOff, 0, "space H123"},
        {kTape, 112, kOff, 0, "space H123S (spring)"},
        {kTape, 112, kTape, 112, "space H123S x2"},
        {kTape, 112, kCloudSeed, 96, "space H123S + cloudseed ambience"},
        {kTape, 112, kRings, 64, "space H123S + rings strings"},
    };
    for (auto& cs : cases) {
      auto fx = std::make_unique<SendFx>(kSr);
      memset((void*)&control, 0, sizeof control);
      control.knob[0][0] = cs.t1, control.knob[1][0] = cs.t2;
      control.knob[0][2] = 30, control.knob[0][3] = 110, control.knob[0][4] = cs.m1, control.knob[0][5] = 90;
      if (cs.t2 == kTape) control.knob[1][2] = 50, control.knob[1][3] = 80, control.knob[1][4] = cs.m2, control.knob[1][5] = 60;
      else if (cs.t2) control.knob[1][structure_knob(cs.t2)] = cs.m2;
      for (int tr = 0; tr < kTracks; ++tr) control.send[tr][0] = control.send[tr][1] = 100;
      float st[12][kStems], o[12][2];
      float acc = 0;
      for (int b = 0; b < 2000; ++b) {
        for (int j = 0; j < 12; ++j)
          for (int s2 = 0; s2 < kStems; ++s2) st[j][s2] = 0.01f * (float)(((b * 12 + j) * 7 + s2 * 3) % 11 - 5);
        fx->Process(st, 12, o, control);
      }
      const int blocks = 40000;
      CHECK(timing_retry(cs.name, [&] {   // (wall-clock: up to 3 tries - tests/timing.h)
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) {
          for (int j = 0; j < 12; ++j)
            for (int s2 = 0; s2 < kStems; ++s2) st[j][s2] = 0.01f * (float)(((b * 12 + j) * 7 + s2 * 3) % 11 - 5);
          fx->Process(st, 12, o, control);
          acc += o[5][1];
        }
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
        printf("test_tape: CPU %-34s %6.0f ns a 12-frame block (Mac %.2f %%; Pi 4 est. %.1f us, %.1f %% of a core) [%g]\n",
               cs.name, us * 1000, 100 * us / kBlockUs, us * kPiRatio, 100 * us * kPiRatio / kBlockUs, acc * 0);
        return us < 50.0;
      }));
      CHECK(fx->Faults() == 0);
    }
    printf("test_tape: TapeFx %zu bytes a slot (SendFx %zu)\n", sizeof(TapeFx), sizeof(SendFx));
  }
  printf("test_tape: PASS\n");
  return 0;
}
