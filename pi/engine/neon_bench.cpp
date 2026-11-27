// pi/engine/neon_bench.cpp - each NEON kernel (core/simd.h and its neighbours) against its C on this CPU, at boot after the
// machines' bench: the A72's own answer to what an Apple core cannot say (docs/superpowers/specs/2026-10-31-neon-pass.md).
// A row: the same input through both, repeated; ticks of core::cycles(). Hosted, no card, the render untouched.
#include <math.h>
#include <string.h>

#include <algorithm>

#include "bridge.h"
#include "daisy_engine.h"
#include "fx/send_fx.h"
#include "plaits/dsp/fm/operator.h"
#include "platform.h"
#include "simd.h"

namespace bridge {
namespace {
const int kN = 3000;
template <class F>
uint64_t ticks(F f) {
  uint32_t t0 = core::cycles();
  for (int i = 0; i < kN; ++i) {
    f();
    asm volatile("" : : : "memory");   // (the results count)
  }
  return (uint32_t)(core::cycles() - t0);
}
uint32_t g_r = 1;
float rnd() { return (float)(int32_t)(g_r = g_r * 1664525u + 1013904223u) * (1.0f / 2147483648.0f); }
float g_x[24], g_y[96], g_z[96];
float g_st[96][8], g_tab[2][520];
void fill() {
  for (auto& x : g_x) x = 0.5f * rnd();
  for (auto& s : g_st)
    for (auto& x : s) x = 0.3f * rnd();
  for (auto& f : g_tab)
    for (auto& x : f) x = rnd();
}
using plaits::fm::Operator;
Operator g_op[3];
struct Line {
  core::fx::cloudseed::ModDelay<4096> delay;
  core::fx::cloudseed::ModAllpass<1600> ap;
};
Line* g_line;
rytm::BlockResonator g_res;
}  // namespace

const unsigned kNeonRows = 7;
const char* neon_bench(unsigned row, uint64_t* c, uint64_t* n) {
  static bool once;
  if (!once) {
    once = true;
    fill();
    g_line = new Line;
    g_line->delay.Clear(), g_line->ap.Clear();
    g_line->ap.feedback = 0.6f;
    g_line->delay.target = g_line->delay.delay = 900.0f, g_line->ap.target = g_line->ap.delay = 500.0f;
    g_line->delay.mod_amount = g_line->ap.mod_amount = 8.0f, g_line->delay.mod_rate = g_line->ap.mod_rate = 0.0001f;
    g_res.Init(0.015f);
    g_res.Prepare(0.01f, 0.4f, 0.5f, 0.6f);
  }
  using namespace core::simd;
  static int16_t ring[12][core::kVoices];   // the render ring (simd.h clip_rows: kVoices columns)
  static int rows[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  const float f[3] = {0.01f, 0.02f, 0.03f}, a[3] = {1.0f, 0.5f, 0.3f};
  float fb[2] = {0, 0};
#if CORE_NEON
#define BOTH(cc, nn) *c = ticks([&] { cc; }), *n = ticks([&] { nn; })
#else
#define BOTH(cc, nn) *c = ticks([&] { cc; }), *n = *c
#endif
  switch (row) {
    case 0: BOTH(clip_rows_c(ring, rows, 3, g_x, 12), clip_rows_neon(ring, rows, 3, g_x, 12)); return "clip";
    case 1: {
      float g[2][8] = {}, d[2][8] = {};
      BOTH(mix2_c(g_st, 96, g, d, g_y, g_z), mix2_neon(g_st, 96, g, d, g_y, g_z));
      return "mix2";
    }
    case 2:
      BOTH((plaits::fm::RenderOperatorsC<1, -2, true>(g_op, f, a, fb, 0, g_x, g_y, 24)),
           (plaits::fm::RenderOperators<1, -2, true>(g_op, f, a, fb, 0, g_x, g_y, 24)));
      return "fm-op";
    case 3: {
      g_res.Init(0.015f);
      g_res.Prepare(0.01f, 0.4f, 0.5f, 0.6f);
      *c = ticks([&] { for (int s = 0; s < 12; ++s) g_y[s] = g_res.ProcessC(g_x[s]); });
      g_res.Init(0.015f);
      g_res.Prepare(0.01f, 0.4f, 0.5f, 0.6f);
      *n = ticks([&] { for (int s = 0; s < 12; ++s) g_y[s] = g_res.Process(g_x[s]); });
      return "resonator";
    }
    case 4: {
      g_line->delay.no_neon = true;
      *c = ticks([&] { g_line->delay.Process(g_x, g_y, 16); });
      g_line->delay.no_neon = false;
      *n = ticks([&] { g_line->delay.Process(g_x, g_y, 16); });
      return "delay";
    }
    case 5: {
      g_line->ap.no_neon = true;
      *c = ticks([&] { g_line->ap.Process(g_x, g_y, 16); });
      g_line->ap.no_neon = false;
      *n = ticks([&] { g_line->ap.Process(g_x, g_y, 16); });
      return "allpass";
    }
    case 6: {
      uint32_t ph[7] = {1, 2, 3, 4, 5, 6, 7}, inc[7] = {3000000, 3000100, 2999900, 3000200, 2999800, 3000300, 2999700};
      const float w[7] = {1, 1, 1, 1, 1, 1, 1};
      BOTH(g_y[0] = wt_unison_c(ph, inc, w, 7, 0, 1, 256, g_tab[0] + 1, g_tab[1] + 1, 0.3f),
           g_y[0] = wt_unison_neon(ph, inc, w, 7, 0, 1, 256, g_tab[0] + 1, g_tab[1] + 1, 0.3f));
      return "wavetable x7";
    }
  }
  return nullptr;
}
}  // namespace bridge
