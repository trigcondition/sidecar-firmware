// core/samp/sedit_dsp.cpp - see sedit_dsp.h.
#include "sedit_dsp.h"

#if CORE_MSAMP

#include <math.h>
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

// ---- sedit-050: the zero-crossing snap
namespace {
inline float sum_at(const float* x, int ch, uint32_t f) { return ch == 2 ? x[(size_t)f * 2] + x[(size_t)f * 2 + 1] : x[f]; }
inline float absf(float v) { return v < 0 ? -v : v; }
inline uint32_t dist(uint32_t p, uint32_t q) { return p > q ? p - q : q - p; }
}  // namespace

bool zero_near(const float* x, int ch, uint32_t frames, uint32_t f, uint32_t lo, uint32_t hi, uint32_t* out) {
  if (!x || frames < 2) return false;
  if (lo < 1) lo = 1;
  if (hi > frames - 1) hi = frames - 1;
  if (lo > hi) return false;
  bool got = false;
  uint32_t best = 0;
  auto take = [&](uint32_t c) {
    if (!got || dist(c, f) < dist(best, f)) best = c, got = true;
  };
  if (ch == 2) {   // both channels near 0
    for (uint32_t i = lo; i <= hi; ++i)
      if (absf(x[(size_t)i * 2]) <= kNearZero && absf(x[(size_t)i * 2 + 1]) <= kNearZero) take(i);
    if (got) return *out = best, true;
  }
  for (uint32_t i = lo; i <= hi; ++i) {   // the sum's sign change between i - 1 and i
    float p = sum_at(x, ch, i - 1), q = sum_at(x, ch, i);
    if (q == 0.0f) take(i);
    else if ((p < 0.0f) != (q < 0.0f) && p != 0.0f) take(absf(q) <= absf(p) ? i : i - 1);
  }
  if (got) *out = best;
  return got;
}

// ---- sedit-050: the transients
void Onsets::begin(uint32_t rate) {
  n = 0;
  gap_ = (uint32_t)((uint64_t)(rate ? rate : 48000) * kOnsetGapMs / 1000);
  hop0_ = 0, acc_ = 0, cnt_ = 0, e1_ = e2_ = 0, any_ = false, last_ = 0;
}
void Onsets::hop_done(const float* x, int ch) {
  float e = cnt_ ? (float)(acc_ / cnt_) : 0.0f;
  float before = e1_ > e2_ ? e1_ : e2_;
  if (e > kOnsetFloor && e >= kOnsetRise * before && (!any_ || hop0_ - last_ >= gap_) && n < kOnsetsMax) {
    float half = 0.5f * sqrtf(e);
    uint32_t at_ = hop0_;
    for (uint32_t i = hop0_; i < hop0_ + cnt_; ++i)
      if (absf(sum_at(x, ch, i)) * (ch == 2 ? 0.5f : 1.0f) > half) {
        at_ = i;
        break;
      }
    at[n++] = at_;
    last_ = at_, any_ = true;
  }
  e2_ = e1_, e1_ = e;
  hop0_ += cnt_;
  acc_ = 0, cnt_ = 0;
}
void Onsets::feed(const float* x, int ch, uint32_t from, uint32_t to) {
  for (uint32_t i = from; i < to; ++i) {
    float s = sum_at(x, ch, i) * (ch == 2 ? 0.5f : 1.0f);
    acc_ += (double)s * s;
    if (++cnt_ == kOnsetHop) hop_done(x, ch);
  }
}
void Onsets::end(const float* x, int ch) {
  if (cnt_) hop_done(x, ch);
}

}  // namespace sedit
}  // namespace core

#endif  // CORE_MSAMP
