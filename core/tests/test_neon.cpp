// core/tests/test_neon.cpp - every NEON kernel against its portable C (core/simd.h), bit for bit unless a check says
// otherwise, on random blocks with denormals, NaN and over-range samples. Apple Silicon runs them natively; on x86 only
// the C exists and this test checks nothing but that it builds.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "simd.h"
#include "plaits/dsp/fm/operator.h"
#include "fx/send_fx.h"
#include "daisy_engine.h"

#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

using namespace core::simd;

static uint32_t g_r = 12345;
static uint32_t rnd() { return g_r = g_r * 1664525u + 1013904223u; }
static float frand() { return (float)(int32_t)rnd() / 2147483648.0f; }
// a sample a block can hold: mostly audio, some loud, some tiny, a rare NaN or infinity
static float sample(bool nan = true) {
  uint32_t k = rnd() >> 24;
  float x = frand();
  if (k < 200) return x;
  if (k < 230) return x * 4.0f;
  if (k < 250) return x * 1e-40f;
  if (!nan) return 0.0f;
  return k < 253 ? NAN : k < 255 ? INFINITY : -INFINITY;
}
static bool same(float a, float b) { return memcmp(&a, &b, 4) == 0 || (a != a && b != b); }
// The FMA kernels against the C: clang fuses the C's multiply-adds as the lanes do (bit-exact there), GCC fuses fewer or others,
// so the honest check is a bound, the same on both: tol absolute. worst: what was seen, printed by main.
static float near_worst;
static bool near(const float* a, const float* b, int n, float tol) {
  for (int i = 0; i < n; ++i) {
    float e = fabsf(a[i] - b[i]);
    if (a[i] != a[i] && b[i] != b[i]) continue;
    if (e > near_worst) near_worst = e;
    if (!(e <= tol)) return false;
  }
  return true;
}
static void report(const char* name) {
  printf("%-10s largest difference from the C %g\n", name, near_worst);
  near_worst = 0.0f;
}

#if CORE_NEON
static void test_clip_rows() {
  for (int t = 0; t < 20000; ++t) {
    int n = 1 + rnd() % 16, i = rnd() % core::kVoices, wrap = rnd() % 16;
    float o[16];
    int row[16];
    for (int s = 0; s < n; ++s) o[s] = sample(), row[s] = (wrap + s) % 16;
    int16_t a[16][core::kVoices], b[16][core::kVoices];
    memset(a, 0x55, sizeof a), memset(b, 0x55, sizeof b);
    clip_rows_c(a, row, i, o, n), clip_rows_neon(b, row, i, o, n);
    CHECK(!memcmp(a, b, sizeof a));
  }
}
static void test_be32() {
  for (int t = 0; t < 20000; ++t) {
    int n = 1 + rnd() % 20, off = rnd() % 4;
    int32_t x[20];
    uint8_t a[100], b[100];
    for (int i = 0; i < n; ++i) x[i] = (int32_t)rnd();
    memset(a, 7, sizeof a), memset(b, 7, sizeof b);
    be32_c(a + off, x, n), be32_neon(b + off, x, n);
    CHECK(!memcmp(a, b, sizeof a));
  }
}
static void test_peak07() {
  for (int t = 0; t < 20000; ++t) {
    int n = 1 + rnd() % 16;
    float o[16];
    for (int s = 0; s < n; ++s) o[s] = sample(false);
    CHECK(same(peak07_c(o, n), peak07_neon(o, n)));
  }
  // NaN: never over the knee, as the C's compare
  float o[12] = {0.1f, NAN, 0.2f, NAN, NAN, NAN, NAN, NAN, 0.3f, 0, 0, NAN};
  CHECK(!(peak07_neon(o, 12) > 0.8f));
}
static void test_mix2() {
  for (int t = 0; t < 5000; ++t) {
    int n = 1 + rnd() % 128;
    static float st[128][8], xa[2][128], xb[2][128];
    float g[2][8], ga[2][8], gb[2][8], dg[2][8];
    for (int i = 0; i < n; ++i)
      for (int v = 0; v < 8; ++v) st[i][v] = sample(false);
    for (int s = 0; s < 2; ++s)
      for (int v = 0; v < 8; ++v) g[s][v] = frand(), dg[s][v] = frand() * 0.01f;
    memcpy(ga, g, sizeof g), memcpy(gb, g, sizeof g);
    mix2_c(st, n, ga, dg, xa[0], xa[1]), mix2_neon(st, n, gb, dg, xb[0], xb[1]);
    // the sums of 8 products of up to 4 x 1.3: a few ulp of 40; the gains' ramps are plain adds, exact on both
    CHECK(near(xa[0], xb[0], n, 2e-5f) && near(xa[1], xb[1], n, 2e-5f) && !memcmp(ga, gb, sizeof ga));
  }
  report("mix2");
}
template <class T>
static void test_cloud_line(float fb) {
  static T a, b;
  a.Clear(), b.Clear();
  a.no_neon = true;
  a.delay = b.delay = a.target = b.target = 40.0f;
  a.mod_amount = b.mod_amount = 12.0f, a.mod_rate = b.mod_rate = 0.001f;
  static float in[16], x[16], y[16];
  for (int t = 0; t < 40000; ++t) {
    int n = 1 + rnd() % 16;
    for (int i = 0; i < n; ++i) in[i] = frand() * ((t >> 8) & 1 ? 1e-22f : 0.5f);
    // through the 4-sample limit, and the line nearly full (the batch's writes must not reach the reads)
    if (t % 300 == 0) a.target = b.target = a.delay = b.delay = (rnd() & 1) ? 3.0f + (float)(rnd() % 90) : 118.0f + (float)(rnd() % 8);
    a.Process(in, x, n), b.Process(in, y, n);
    CHECK(near(x, y, n, 1e-6f));
  }
  report("delay");
  (void)fb;
}
static void test_cloud() {
  using namespace core::fx::cloudseed;
  test_cloud_line<ModDelay<128>>(0);
  {
    static ModAllpass<128> p, q;
    p.Clear(), q.Clear();
    p.no_neon = true;
    p.feedback = q.feedback = 0.6f;
    p.delay = q.delay = p.target = q.target = 40.0f;
    p.mod_amount = q.mod_amount = 12.0f, p.mod_rate = q.mod_rate = 0.001f;
    float in[16], x[16], y[16];
    for (int t = 0; t < 40000; ++t) {
      int n = 1 + rnd() % 16;
      for (int i = 0; i < n; ++i) in[i] = frand() * ((t >> 8) & 1 ? 1e-22f : 0.5f);
      if (t % 300 == 0) p.target = q.target = 3.0f + (float)(rnd() % 90);
      memcpy(y, in, sizeof y);
      p.Process(in, x, n), q.Process(y, y, n);   // (in place, as the diffuser runs it)
      CHECK(near(x, y, n, 4e-5f));   // the feedback holds the ulps: bounded, about 1 / (1 - fb) of them over 8 x full scale
    }
    report("allpass");
  }
}
static void test_resonator() {
  static rytm::BlockResonator a;
  a.Init(0.015f);
  // one object: each sample through the C and through NEON on two copies
  static rytm::BlockResonator c, n;
  c = a, n = a;
  float worst = 0.0f, top = 0.0f;
  for (int b = 0; b < 2000; ++b) {
    float f0 = 0.002f + 0.1f * fabsf(frand()), st = fabsf(frand()), br = fabsf(frand()), dm = fabsf(frand());
    c.Prepare(f0, st, br, dm), n.Prepare(f0, st, br, dm);
    for (int s = 0; s < 12; ++s) {
      float x = frand() * 0.2f, p = c.ProcessC(x), q = n.Process(x);
      float e = fabsf(p - q);
      if (e > worst) worst = e;
      if (fabsf(p) > top) top = fabsf(p);
      CHECK(!memcmp(&c, &n, sizeof c));   // every mode's state and coefficient: only the output's sum may differ
    }
  }
  // the output's sum only, in another order: a few ulp of the largest mode, and it does not accumulate (no state differs)
  printf("resonator: largest output %g, largest difference %g\n", top, worst);
  CHECK(worst <= 1e-6f * (top + 1.0f));
}
static void test_wt() {
  static float tab[2][2048 + 8];
  for (int f = 0; f < 2; ++f)
    for (int i = 0; i < 2056; ++i) tab[f][i] = frand();
  for (int t = 0; t < 40000; ++t) {
    int nu = 1 + rnd() % 7, lsz = 1 << (4 + rnd() % 8), warp = rnd() & 1;
    float w[7], ft = fabsf(frand()), sr = 1 + 7 * fabsf(frand());
    uint32_t a[7], b[7], inc[7];
    for (int j = 0; j < 7; ++j) a[j] = b[j] = rnd(), inc[j] = rnd() >> 6, w[j] = fabsf(frand());
    float x = wt_unison_c(a, inc, w, nu, warp, sr, lsz, tab[0] + 1, tab[1] + 1, ft);
    float y = wt_unison_neon(b, inc, w, nu, warp, sr, lsz, tab[0] + 1, tab[1] + 1, ft);
    CHECK(near(&x, &y, 1, 2e-5f) && !memcmp(a, b, sizeof a));   // up to 7 x 1.3 x a Hermite's overshoot: a few ulp of it
  }
  report("wavetable");
}
#ifdef RYTM_FM_NEON
template <int n, int m, bool add>
static void test_op() {
  using namespace plaits::fm;
  for (int t = 0; t < 4000; ++t) {
    size_t size = 1 + rnd() % 24;
    Operator a[3], b[3];
    float f[3], am[3], fb[2] = {0, 0}, mod[24], x[24], y[24];
    for (int k = 0; k < 3; ++k) {
      a[k].phase = b[k].phase = rnd(), a[k].amplitude = b[k].amplitude = fabsf(frand()) * 2.0f;
      f[k] = fabsf(frand()) * 0.6f, am[k] = fabsf(frand()) * 5.0f;
    }
    for (size_t s = 0; s < size; ++s) mod[s] = frand() * 30.0f, x[s] = y[s] = frand();
    RenderOperatorsC<n, m, add>(a, f, am, fb, 0, mod, x, size);
    RenderOperators<n, m, add>(b, f, am, fb, 0, mod, y, size);
    // n = 1: the sine's own FMAs, a few ulp of the block (x, y start at up to 1, amplitudes up to 4). A chain: GCC's C does not
    // fuse the modulator's y * a + 32 (a phase ulp of 3.8e-6 turns x amplitude 4 x 2 pi) where clang's and the lanes do, and
    // each further operator multiplies that by the same 25. Rounding of the phase, per sample: it does not accumulate.
    CHECK(near(x, y, (int)size, n == 1 ? 2e-5f : n == 2 ? 1e-3f : 3e-2f));
    for (int k = 0; k < n; ++k) CHECK(a[k].phase == b[k].phase && same(a[k].amplitude, b[k].amplitude));
  }
  report("fm-op");
}
#endif
#endif

int main() {
#if CORE_NEON
  test_clip_rows();
  test_be32();
  test_peak07();
  test_mix2();
  test_cloud();
  test_resonator();
  test_wt();
#ifdef RYTM_FM_NEON
  test_op<1, -2, false>(), test_op<1, -2, true>(), test_op<1, -1, false>(), test_op<1, -1, true>();
  test_op<2, -2, true>(), test_op<3, -1, false>(), test_op<3, -2, true>();
#endif
  printf("test_neon: every kernel matches its C\n");
#else
  printf("test_neon: no NEON here\n");
#endif
  return 0;
}
