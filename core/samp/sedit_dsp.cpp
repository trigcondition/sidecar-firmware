// core/samp/sedit_dsp.cpp - see sedit_dsp.h.
#include "sedit_dsp.h"

#if CORE_MSAMP

#include <string.h>

#include "dsp.h"

namespace core {
namespace sedit {

UI_ROM const char* const kFnNames[kFns] = {"CROP", "NORMALISE", "TRIM", "FADE IN", "FADE OUT", "REVERSE", "GAIN +3", "GAIN -3"};

float peak_range(const float* x, int ch, uint32_t a, uint32_t b) {
  float p = 0.0f;
  for (size_t i = (size_t)a * ch, e = (size_t)b * ch; i < e; ++i) {
    float v = x[i] < 0 ? -x[i] : x[i];
    if (v > p) p = v;
  }
  return p;
}

void gain_range(float* x, int ch, uint32_t a, uint32_t b, float g) {
  for (size_t i = (size_t)a * ch, e = (size_t)b * ch; i < e; ++i) x[i] *= g;
}

void fade_range(float* x, int ch, uint32_t a, uint32_t b, uint32_t r0, uint32_t r1, bool in) {
  const uint32_t span = r1 - r0 - 1;   // r1 - r0 >= 1
  for (uint32_t f = a; f < b; ++f) {
    float t = span ? (float)(f - r0) / (float)span : 1.0f;
    float g = in ? t : 1.0f - t;
    for (int c = 0; c < ch; ++c) x[(size_t)f * ch + c] *= g;
  }
}

void reverse_pairs(float* x, int ch, uint32_t r0, uint32_t r1, uint32_t p0, uint32_t p1) {
  for (uint32_t i = p0; i < p1; ++i) {
    float* l = x + (size_t)(r0 + i) * ch;
    float* r = x + (size_t)(r1 - 1 - i) * ch;
    for (int c = 0; c < ch; ++c) {
      float t = l[c];
      l[c] = r[c];
      r[c] = t;
    }
  }
}

void copy_frames(float* dst, const float* src, int ch, uint32_t a, uint32_t b, uint32_t dst_off) {
  if (b > a) memmove(dst + (size_t)dst_off * ch, src + (size_t)a * ch, (size_t)(b - a) * ch * sizeof(float));
}

bool trim_bounds(const float* x, int ch, uint32_t n, uint32_t rate, uint32_t* a, uint32_t* b) {
  samp::Trim t;
  if (!samp::trim(x, ch, n, rate / 20, &t, -50.0f)) return false;
  *a = t.start;
  *b = t.end;
  return true;
}

void clamp_region(uint32_t frames, int64_t* a, int64_t* b) {
  if (!frames) {
    *a = *b = 0;
    return;
  }
  if (*b < *a) {
    int64_t t = *a;
    *a = *b;
    *b = t;
  }
  const int64_t n = frames;
  if (*a < 0) *a = 0;
  if (*a > n) *a = n;
  if (*b < 0) *b = 0;
  if (*b > n) *b = n;
  if (*a == *b) {
    if (*b + 1 < n) ++*b;   // (the last frame widens downwards)
    else if (*a > 0) --*a;
    else ++*b;
  }
}

}  // namespace sedit
}  // namespace core

#endif  // CORE_MSAMP
