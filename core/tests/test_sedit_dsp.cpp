// core/tests/test_sedit_dsp.cpp - the sample editor's edit functions (samp/sedit_dsp.h).
#include <math.h>
#include <string.h>

#include "check.h"
#include "samp/sedit_dsp.h"

using namespace core::sedit;

static const int N = 1000;
static float x[2 * N], y[2 * N], old[2 * N];

static void ramp() {
  for (int f = 0; f < N; ++f) { x[2 * f] = f / 1000.f; x[2 * f + 1] = -f / 1000.f; }
}
static void finite() { for (int i = 0; i < 2 * N; ++i) CHECK(isfinite(x[i])); }

// sedit-050 #7: the zero-crossing snap
static void t_zero_snap() {
  static float m[4800], st[9600], dc[4800];
  for (int f = 0; f < 4800; ++f) m[f] = sinf(2 * (float)M_PI * 100 * (f + 0.3f) / 48000);   // crossings near 0, 240, 480..
  uint32_t o = 0;
  CHECK(zero_near(m, 1, 4800, 1000, 1000 - kSnapReach, 1000 + kSnapReach, &o) && (o == 960 || o == 959));
  CHECK(fabsf(m[o]) < fabsf(m[o + 1]) + 1e-6f || fabsf(m[o]) < fabsf(m[o - 1]) + 1e-6f);
  CHECK(zero_near(m, 1, 4800, 1000, 1001, 1000 + kSnapReach, &o) && (o == 1200 || o == 1199));   // (only past 1000)
  CHECK(!zero_near(m, 1, 4800, 1000, 1001, 1100, &o));   // none in that window
  // stereo: both channels near 0 first (the right a quarter period behind: never both near 0 here) else the sum's
  for (int f = 0; f < 4800; ++f)   // in phase, a crossing on a frame (960): both near 0 together
    st[2 * f] = sinf(2 * (float)M_PI * 100 * f / 48000), st[2 * f + 1] = 0.5f * st[2 * f];
  CHECK(zero_near(st, 2, 4800, 1000, 1000 - kSnapReach, 1000 + kSnapReach, &o) && o == 960);
  for (int f = 0; f < 4800; ++f) st[2 * f] = m[f], st[2 * f + 1] = 0.9f * sinf(2 * (float)M_PI * 100 * (f + 120.3f) / 48000);
  CHECK(zero_near(st, 2, 4800, 1000, 1000 - kSnapReach, 1000 + kSnapReach, &o));   // the sum's crossing
  float p0 = st[2 * (o - 1)] + st[2 * (o - 1) + 1], p1 = st[2 * o] + st[2 * o + 1], p2 = st[2 * (o + 1)] + st[2 * (o + 1) + 1];
  CHECK((p0 < 0) != (p1 < 0) || (p1 < 0) != (p2 < 0));
  CHECK(fabsf(st[2 * o]) > kNearZero || fabsf(st[2 * o + 1]) > kNearZero);   // (not a both-near-0 frame)
  // a DC offset: no crossing in the window, nothing found (the marker keeps the place asked for)
  for (int f = 0; f < 4800; ++f) dc[f] = 0.3f + 0.1f * m[f];
  CHECK(!zero_near(dc, 1, 4800, 1000, 1000 - kSnapReach, 1000 + kSnapReach, &o));
  // the ends: 0 and frames are never candidates (lo >= 1, hi <= frames - 1)
  CHECK(!zero_near(m, 1, 4800, 0, 0, 10, &o));   // (the sine starts above 0 at frame 0: no crossing in 1..10)
}

// sedit-050 #8: transients of a drum-like signal: four hits (a click and a decaying 60 Hz body) at known places, with
// a quiet noise floor between; fed in uneven pieces
static void t_onsets() {
  static float d[96000], s2[192000];
  const uint32_t hits[4] = {0, 12000, 30000, 30000 + 1200};   // the last 25 ms after the third: inside the 50 ms gap
  uint32_t r = 12345;
  for (int f = 0; f < 96000; ++f) {
    r = r * 1103515245u + 12345u;
    d[f] = ((float)((r >> 9) & 0xFFFF) / 65536.0f - 0.5f) * 0.002f;   // -60 dBFS noise
  }
  const uint32_t at[4] = {hits[0], hits[1], hits[2], 60000};
  for (int h = 0; h < 4; ++h)
    for (uint32_t k = 0; k < 20000 && at[h] + k < 96000; ++k)
      d[at[h] + k] += 0.8f * expf(-(float)k / 4000.0f) * sinf(2 * (float)M_PI * 60 * k / 48000 + 0.5f);
  for (uint32_t k = 0; k < 2000; ++k) d[hits[3] + k] += 0.9f * expf(-(float)k / 300.0f) * sinf(2 * (float)M_PI * 900 * k / 48000);
  Onsets o;
  o.begin(48000);
  for (uint32_t s = 0, step = 1000; s < 96000; s += step, step = step == 1000 ? 7777 : 1000)
    o.feed(d, 1, s, s + step < 96000 ? s + step : 96000);
  o.end(d, 1);
  CHECK(o.n == 4);
  for (int h = 0; h < 4; ++h) CHECK(o.at[h] >= at[h] && o.at[h] < at[h] + kOnsetHop);   // (the hit at 31200: in the gap)
  // stereo: the same onsets
  for (int f = 0; f < 96000; ++f) s2[2 * f] = d[f], s2[2 * f + 1] = 0.5f * d[f];
  o.begin(48000);
  o.feed(s2, 2, 0, 96000);
  o.end(s2, 2);
  CHECK(o.n == 4);
  // silence: none
  static float q[48000];
  o.begin(48000);
  o.feed(q, 1, 0, 48000);
  o.end(q, 1);
  CHECK(o.n == 0);
}

int main() {
  // t_gain
  ramp();
  gain_range(x, 2, 0, 1000, kGainUpF);
  CHECK(fabsf(x[2 * 500] - 0.5f * 1.41254f) < 1e-6f);
  finite();
  // t_norm
  ramp();
  float p = peak_range(x, 2, 100, 200);
  CHECK(fabsf(p - 0.199f) < 1e-6f);
  gain_range(x, 2, 100, 200, kNormPeak / p);
  CHECK(fabsf(peak_range(x, 2, 100, 200) - kNormPeak) < 1e-6f);
  finite();
  // t_fade
  ramp();
  memcpy(old, x, sizeof x);
  fade_range(x, 2, 0, 1000, 0, 1000, true);
  CHECK(x[0] == 0.f);
  CHECK(fabsf(x[2 * 999] - 0.999f) < 1e-6f);
  finite();
  memcpy(y, old, sizeof y);
  fade_range(y, 2, 0, 300, 0, 1000, true);
  fade_range(y, 2, 300, 1000, 0, 1000, true);
  {
    float one[2 * N];
    memcpy(one, old, sizeof one);
    fade_range(one, 2, 0, 1000, 0, 1000, true);
    CHECK(!memcmp(one, y, sizeof y));
  }
  // fade out
  ramp();
  fade_range(x, 2, 0, 1000, 0, 1000, false);
  CHECK(x[2 * 999] == 0.f && x[0] == 0.f && fabsf(x[2 * 500] - 0.5f * (1.f - 500.f / 999.f)) < 1e-6f);
  finite();
  // t_reverse
  ramp();
  memcpy(old, x, sizeof x);
  reverse_pairs(x, 2, 0, 1000, 0, 200);
  reverse_pairs(x, 2, 0, 1000, 200, 500);
  CHECK(x[0] == old[2 * 999] && x[2 * 999] == old[0] && x[2 * 500] == old[2 * 499]);
  CHECK(x[1] == old[2 * 999 + 1]);
  finite();
  ramp();
  reverse_pairs(x, 2, 0, 999, 0, 499);   // odd length keeps the middle frame
  CHECK(x[2 * 499] == old[2 * 499] && x[0] == old[2 * 998] && x[2 * 998] == old[0]);
  finite();
  // t_crop
  ramp();
  copy_frames(y, x, 2, 250, 750, 0);
  CHECK(y[0] == x[2 * 250]);
  CHECK(y[2 * 499] == x[2 * 749]);
  // t_trim
  {
    static float s[19200];
    memset(s, 0, sizeof s);
    for (int i = 0; i < 9600; ++i) s[4800 + i] = 0.5f * cosf(6.2831853f * 440.f * i / 48000.f);
    uint32_t a = 0, b = 0;
    CHECK(trim_bounds(s, 1, 19200, 48000, &a, &b));
    CHECK(a == 4800 - 96);
    CHECK(b == 14400 + 480);
    memset(s, 0, sizeof s);
    CHECK(!trim_bounds(s, 1, 19200, 48000, &a, &b));
  }
  // t_degenerate_regions
  int64_t a = 500, b = 500;
  clamp_region(1000, &a, &b); CHECK(a == 500 && b == 501);
  a = 999; b = 999; clamp_region(1000, &a, &b); CHECK(a == 998 && b == 999);
  a = 700; b = 200; clamp_region(1000, &a, &b); CHECK(a == 200 && b == 700);
  a = -5; b = 5000; clamp_region(1000, &a, &b); CHECK(a == 0 && b == 1000);
  a = 3; b = 9; clamp_region(0, &a, &b); CHECK(a == 0 && b == 0);
  a = 0; b = 0; clamp_region(1, &a, &b); CHECK(a == 0 && b == 1);
  a = 1000; b = 1000; clamp_region(1000, &a, &b); CHECK(a == 999 && b == 1000);
  ramp();
  float old20 = x[20];
  fade_range(x, 2, 10, 11, 10, 11, true);  CHECK(x[20] == old20);
  fade_range(x, 2, 10, 11, 10, 11, false); CHECK(x[20] == 0.f);
  finite();
  float z[200] = {0};
  CHECK(peak_range(z, 2, 0, 100) == 0.f);
  CHECK(!strcmp(kFnNames[kGainDown], "GAIN -3") && !strcmp(kFnNames[kCrop], "CROP"));
  t_zero_snap();
  t_onsets();
  printf("test_sedit_dsp: PASS\n");
  return 0;
}
