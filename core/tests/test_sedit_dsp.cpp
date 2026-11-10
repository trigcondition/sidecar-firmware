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
  printf("test_sedit_dsp: PASS\n");
  return 0;
}
