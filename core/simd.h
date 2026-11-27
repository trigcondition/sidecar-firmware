// core/simd.h - the render loops' NEON kernels (AArch64: the Pi, the Mac; docs/superpowers/specs/2026-10-31-neon-pass.md).
// Each kernel: name_c, the portable C the Teensy and x86 run, and name_neon, the same arithmetic 4 wide; name picks. A
// lane does what the C does for that sample, in the same order. Which multiply-adds fuse in the C is the compiler's choice
// under -ffp-contract=fast, so a kernel with FMAs matches clang's C bit for bit and GCC's within a few ulp;
// core/tests/test_neon.cpp holds them to that, on both. No allocation, no branches on the signal beyond the C's: safe on the
// render cores.
#pragma once
#include <stdint.h>

#include "machines.h"   // kVoices: the render ring's columns

#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(CORE_NO_NEON)
#define CORE_NEON 1
#include <arm_neon.h>
#else
#define CORE_NEON 0
#endif

namespace core {
namespace simd {

inline int16_t clip16(float y) { return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y); }

// A block into its column of the ring: ring[row[s]][i] = o[s] * 0.7 * 32767, clipped to int16 (the rows wrap at the
// ring's end, so a stride store)
inline void clip_rows_c(int16_t (*ring)[kVoices], const int* row, int i, const float* o, int n) {
  for (int s = 0; s < n; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
}
// The largest |o[s] * 0.7|: whether a block is under the soft limiter's knee (NaN: not over, as the C's compare)
inline float peak07_c(const float* o, int n) {
  float m = 0.0f;
  for (int s = 0; s < n; ++s) {
    float y = o[s] * 0.7f, a = y < 0 ? -y : y;
    if (a > m) m = a;
  }
  return m;
}
// The send mixes of the two FX slots, one pass over the 8 stems, each frame's gains ramped first: x0[i] = sum g0[v] s[v],
// x1 the same with g1 (stems taken in pairs, the pairs' sums last, as the C always did). g and dg: [slot][stem]; g is
// worked on in place.
// Always inlined: on the Teensy it stays in its caller's section (FX_DSP, flash), out of the full ITCM.
__attribute__((always_inline)) inline void mix2_c(const float (*stems)[8], int n, float (*g)[8], const float (*dg)[8], float* x0, float* x1) {
  for (int i = 0; i < n; ++i) {
    float p0 = 0.0f, q0 = 0.0f, p1 = 0.0f, q1 = 0.0f;
#pragma GCC unroll 8
    for (int v = 0; v < 8; v += 2) {
      const float a = stems[i][v], b = stems[i][v + 1];
      g[0][v] += dg[0][v], g[0][v + 1] += dg[0][v + 1], g[1][v] += dg[1][v], g[1][v + 1] += dg[1][v + 1];
      p0 += a * g[0][v], q0 += b * g[0][v + 1], p1 += a * g[1][v], q1 += b * g[1][v + 1];
    }
    x0[i] = p0 + q0, x1[i] = p1 + q1;
  }
}

// Int32 samples to big-endian bytes (the Overbridge block; d need not be aligned)
inline void be32_c(uint8_t* d, const int32_t* x, int n) {
  for (int i = 0; i < n; ++i) {
    uint32_t v = (uint32_t)x[i];
    d[4 * i] = (uint8_t)(v >> 24), d[4 * i + 1] = (uint8_t)(v >> 16), d[4 * i + 2] = (uint8_t)(v >> 8), d[4 * i + 3] = (uint8_t)v;
  }
}
// WAVETABLE's unison: nu oscillators, each read from the frames A and B (Hermite, x[-1..2]; their mix ft) at its phase
// (warp 0 none, 1 SYNC at ratio sr), its phase then advanced; the sum of w[j] x each, in order.
inline float herm_c(const float* x, float t) {
  float xm1 = x[-1], x0 = x[0], x1 = x[1], x2 = x[2];
  float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, b = c + w + a;
  return ((a * t - b) * t + c) * t + x0;
}
inline float wt_unison_c(uint32_t* ph, const uint32_t* inc, const float* w, int nu, int warp, float sr, int lsz, const float* A,
                         const float* B, float ft, float o = 0.0f) {
  for (int j = 0; j < nu; ++j) {
    uint32_t p = ph[j];
    ph[j] = p + inc[j];
    float pf = (float)(p >> 8) * (1.0f / 16777216.0f);
    if (warp == 1) pf = pf * sr, pf -= (float)(int)pf;
    float x = pf * (float)lsz;
    int i = (int)x;
    float tt = x - (float)i;
    i &= lsz - 1;
    float a = herm_c(A + i, tt);
    o += w[j] * (a + (herm_c(B + i, tt) - a) * ft);
  }
  return o;
}

#if CORE_NEON
// four lanes' x[-1..2] (at p[0..3]) as four registers, lane by lane
inline void load4x4(const float* p0, const float* p1, const float* p2, const float* p3, float32x4_t* xm1, float32x4_t* x0,
                    float32x4_t* x1, float32x4_t* x2) {
  const float32x4_t a = vld1q_f32(p0 - 1), b = vld1q_f32(p1 - 1), c = vld1q_f32(p2 - 1), d = vld1q_f32(p3 - 1);
  const float32x4_t ab0 = vtrn1q_f32(a, b), ab1 = vtrn2q_f32(a, b), cd0 = vtrn1q_f32(c, d), cd1 = vtrn2q_f32(c, d);
  *xm1 = vreinterpretq_f32_f64(vtrn1q_f64(vreinterpretq_f64_f32(ab0), vreinterpretq_f64_f32(cd0)));
  *x0 = vreinterpretq_f32_f64(vtrn1q_f64(vreinterpretq_f64_f32(ab1), vreinterpretq_f64_f32(cd1)));
  *x1 = vreinterpretq_f32_f64(vtrn2q_f64(vreinterpretq_f64_f32(ab0), vreinterpretq_f64_f32(cd0)));
  *x2 = vreinterpretq_f32_f64(vtrn2q_f64(vreinterpretq_f64_f32(ab1), vreinterpretq_f64_f32(cd1)));
}
inline float32x4_t herm4(const float* p0, const float* p1, const float* p2, const float* p3, float32x4_t t) {
  float32x4_t xm1, x0, x1, x2;
  load4x4(p0, p1, p2, p3, &xm1, &x0, &x1, &x2);
  const float32x4_t half = vdupq_n_f32(0.5f);
  const float32x4_t c = vmulq_f32(vsubq_f32(x1, xm1), half), w = vsubq_f32(x0, x1);
  const float32x4_t cw = vaddq_f32(c, w);
  const float32x4_t a = vfmaq_f32(vaddq_f32(cw, w), vsubq_f32(x2, x0), half), b = vaddq_f32(cw, a);
  return vfmaq_f32(x0, vfmaq_f32(c, vfmaq_f32(vnegq_f32(b), a, t), t), t);
}
inline float wt_unison_neon(uint32_t* ph, const uint32_t* inc, const float* w, int nu, int warp, float sr, int lsz,
                            const float* A, const float* B, float ft) {
  const float32x4_t kf = vdupq_n_f32(1.0f / 16777216.0f), vl = vdupq_n_f32((float)lsz), vft = vdupq_n_f32(ft), vsr = vdupq_n_f32(sr);
  const int32x4_t mask = vdupq_n_s32(lsz - 1);
  float o = 0;
  int j = 0;
  for (; j + 3 <= nu; j += 4) {   // three or four lanes (three: the fourth reads frame 0's start and is dropped)
    const bool four = j + 4 <= nu;
    uint32x4_t p, in;
    if (four) {
      p = vld1q_u32(ph + j), in = vld1q_u32(inc + j);
      vst1q_u32(ph + j, vaddq_u32(p, in));
    } else {
      p = vdupq_n_u32(0), in = p;
      p = vsetq_lane_u32(ph[j], p, 0), p = vsetq_lane_u32(ph[j + 1], p, 1), p = vsetq_lane_u32(ph[j + 2], p, 2);
      for (int k = 0; k < 3; ++k) ph[j + k] += inc[j + k];
    }
    float32x4_t pf = vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(p, 8)), kf);
    if (warp == 1) {
      pf = vmulq_f32(pf, vsr);
      pf = vsubq_f32(pf, vcvtq_f32_s32(vcvtq_s32_f32(pf)));
    }
    const float32x4_t x = vmulq_f32(pf, vl);
    const int32x4_t it = vcvtq_s32_f32(x);
    const float32x4_t tt = vsubq_f32(x, vcvtq_f32_s32(it));
    const int32x4_t i = vandq_s32(it, mask);
    const int i0 = vgetq_lane_s32(i, 0), i1 = vgetq_lane_s32(i, 1), i2 = vgetq_lane_s32(i, 2), i3 = vgetq_lane_s32(i, 3);
    const float32x4_t a = herm4(A + i0, A + i1, A + i2, A + i3, tt), b = herm4(B + i0, B + i1, B + i2, B + i3, tt);
    const float32x4_t y = vfmaq_f32(a, vsubq_f32(b, a), vft);
    o = __builtin_fmaf(w[j], vgetq_lane_f32(y, 0), o);
    o = __builtin_fmaf(w[j + 1], vgetq_lane_f32(y, 1), o);
    o = __builtin_fmaf(w[j + 2], vgetq_lane_f32(y, 2), o);
    if (four) o = __builtin_fmaf(w[j + 3], vgetq_lane_f32(y, 3), o);
    else return o;
  }
  // one or two left
  return j < nu ? wt_unison_c(ph + j, inc + j, w + j, nu - j, warp, sr, lsz, A, B, ft, o) : o;
}

inline void be32_neon(uint8_t* d, const int32_t* x, int n) {
  int i = 0;
  for (; i + 4 <= n; i += 4) vst1q_u8(d + 4 * i, vrev32q_u8(vreinterpretq_u8_s32(vld1q_s32(x + i))));
  if (i < n) be32_c(d + 4 * i, x + i, n - i);
}
inline void clip_rows_neon(int16_t (*ring)[kVoices], const int* row, int i, const float* o, int n) {
  const float32x4_t k1 = vdupq_n_f32(0.7f), k2 = vdupq_n_f32(32767.0f), hi = vdupq_n_f32(32767.0f), lo = vdupq_n_f32(-32768.0f);
  int s = 0;
  for (; s + 4 <= n; s += 4) {
    float32x4_t y = vmulq_f32(vmulq_f32(vld1q_f32(o + s), k1), k2);
    int16x4_t v = vmovn_s32(vcvtq_s32_f32(vminq_f32(vmaxq_f32(y, lo), hi)));
    vst1_lane_s16(&ring[row[s]][i], v, 0);
    vst1_lane_s16(&ring[row[s + 1]][i], v, 1);
    vst1_lane_s16(&ring[row[s + 2]][i], v, 2);
    vst1_lane_s16(&ring[row[s + 3]][i], v, 3);
  }
  for (; s < n; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
}
inline float peak07_neon(const float* o, int n) {
  const float32x4_t k = vdupq_n_f32(0.7f);
  float32x4_t m = vdupq_n_f32(0.0f);
  int s = 0;
  for (; s + 4 <= n; s += 4) m = vmaxnmq_f32(m, vabsq_f32(vmulq_f32(vld1q_f32(o + s), k)));
  float r = vmaxvq_f32(m);
  for (; s < n; ++s) {
    float y = o[s] * 0.7f, a = y < 0 ? -y : y;
    if (a > r) r = a;
  }
  return r;
}
inline void mix2_neon(const float (*stems)[8], int n, float (*g)[8], const float (*dg)[8], float* x0, float* x1) {
  // one register per stem pair: {slot 0 v, slot 0 v+1, slot 1 v, slot 1 v+1} (named, not an array: GCC keeps an array in
  // memory at -O2)
#define G4(k) vcombine_f32(vld1_f32(g[0] + 2 * (k)), vld1_f32(g[1] + 2 * (k)))
#define D4(k) vcombine_f32(vld1_f32(dg[0] + 2 * (k)), vld1_f32(dg[1] + 2 * (k)))
  float32x4_t g0 = G4(0), g1 = G4(1), g2 = G4(2), g3 = G4(3);
  const float32x4_t d0 = D4(0), d1 = D4(1), d2 = D4(2), d3 = D4(3);
#undef G4
#undef D4
  for (int i = 0; i < n; ++i) {
    const float32x4_t s0 = vld1q_f32(stems[i]), s1 = vld1q_f32(stems[i] + 4);
    g0 = vaddq_f32(g0, d0), g1 = vaddq_f32(g1, d1), g2 = vaddq_f32(g2, d2), g3 = vaddq_f32(g3, d3);
    float32x4_t pq = vmulq_f32(vcombine_f32(vget_low_f32(s0), vget_low_f32(s0)), g0);
    pq = vfmaq_f32(pq, vcombine_f32(vget_high_f32(s0), vget_high_f32(s0)), g1);
    pq = vfmaq_f32(pq, vcombine_f32(vget_low_f32(s1), vget_low_f32(s1)), g2);
    pq = vfmaq_f32(pq, vcombine_f32(vget_high_f32(s1), vget_high_f32(s1)), g3);
    float32x2_t s = vpadd_f32(vget_low_f32(pq), vget_high_f32(pq));
    x0[i] = vget_lane_f32(s, 0), x1[i] = vget_lane_f32(s, 1);
  }
  vst1_f32(g[0], vget_low_f32(g0)), vst1_f32(g[1], vget_high_f32(g0));
  vst1_f32(g[0] + 2, vget_low_f32(g1)), vst1_f32(g[1] + 2, vget_high_f32(g1));
  vst1_f32(g[0] + 4, vget_low_f32(g2)), vst1_f32(g[1] + 4, vget_high_f32(g2));
  vst1_f32(g[0] + 6, vget_low_f32(g3)), vst1_f32(g[1] + 6, vget_high_f32(g3));
}
#endif

#if CORE_NEON
inline void clip_rows(int16_t (*ring)[kVoices], const int* row, int i, const float* o, int n) { clip_rows_neon(ring, row, i, o, n); }
inline float peak07(const float* o, int n) { return peak07_neon(o, n); }
inline void be32(uint8_t* d, const int32_t* x, int n) { be32_neon(d, x, n); }
inline void mix2(const float (*stems)[8], int n, float (*g)[8], const float (*dg)[8], float* x0, float* x1) { mix2_neon(stems, n, g, dg, x0, x1); }
inline float wt_unison(uint32_t* ph, const uint32_t* inc, const float* w, int nu, int warp, float sr, int lsz, const float* A,
                       const float* B, float ft) {
  return wt_unison_neon(ph, inc, w, nu, warp, sr, lsz, A, B, ft);
}
#else
inline void clip_rows(int16_t (*ring)[kVoices], const int* row, int i, const float* o, int n) { clip_rows_c(ring, row, i, o, n); }
inline float peak07(const float* o, int n) { return peak07_c(o, n); }
inline void be32(uint8_t* d, const int32_t* x, int n) { be32_c(d, x, n); }
__attribute__((always_inline)) inline void mix2(const float (*stems)[8], int n, float (*g)[8], const float (*dg)[8], float* x0, float* x1) { mix2_c(stems, n, g, dg, x0, x1); }
inline float wt_unison(uint32_t* ph, const uint32_t* inc, const float* w, int nu, int warp, float sr, int lsz, const float* A,
                       const float* B, float ft) {
  return wt_unison_c(ph, inc, w, nu, warp, sr, lsz, A, B, ft);
}
#endif

}  // namespace simd
}  // namespace core
