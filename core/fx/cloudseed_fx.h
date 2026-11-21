// core/fx/cloudseed_fx.h - CLOUDSEED (send FX TYPE 8): Cloud Seed's algorithmic reverb, ported from CloudSeedCore
// (github.com/GhostNoteAudio/CloudSeedCore, commit deb21de: DSP/ReverbChannel.h, DelayLine.h, ModulatedDelay.h,
// AllpassDiffuser.h, ModulatedAllpass.h, Lp1.h, Hp1.h, Biquad.cpp, RandomBuffer.cpp, LcgRandom.h, Utils.h,
// Parameters.h, Programs.h), MIT:
//
//   Copyright (c) 2024 Ghost Note Audio / Ghost Note Engineering Ltd
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without
//   limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the following
//   conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//   TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// (also in core/fx/THIRD_PARTY_LICENSES.txt). Kept from Cloud Seed: the channel's signal flow (input low/high cut,
// pre-delay, the early allpass diffuser, N late lines each a modulated delay -> allpass diffuser -> low/high shelf
// -> low-pass in the feedback loop, tapped post-diffuser, summed x 1/sqrt(N)), its modulated delay and allpass
// (8-sample modulation updates, linear interpolation), the per-line T60 feedback gain, the LCG random seeds with
// the L/R cross-seed, the filter formulas. Changed for the Sidecar: fixed buffers sized for 48 kHz and the macro
// ranges below (no allocation, no std::vector, no rand()); at most 6 late lines of 3 diffuser stages and 6 early
// stages (Cloud Seed: 12 / 8 / 12) for the CPU; no multitap (its 256 taps a sample); the feedback FIFO is a fixed
// 16-sample ring and the line delay is shortened by it (Cloud Seed's block-sized FIFO adds one block); delay times
// glide to a new size instead of jumping; denormals flushed in the loops; the incremental clear (ClearStep).
//
// Knobs P2..P5 (0..127, all p-lockable; 0 = ROOM as designed; P1 is the send's ROUTE, not used here): P2 CHARACTER x
// SIZE: the zone 0..31 ROOM, 32..63 HALL, 64..95 DARK PLATE (after Cloud Seed's factory "Dark Plate"), 96..127
// AMBIENCE (huge, no shimmer), within it the size x1..x2.5 (line, diffuser and pre-delay times, each capped at its
// buffer; a zone change goes through SendFx's fade and clear); P3 DECAY: the preset's T60 x1..x10 (exponential); P4
// DAMPING: the in-loop low-pass and the input high cut down to 1/16 (4 octaves darker); P5 MOD: the line and diffuser
// modulation depth x1..x5 and rate x1..x3.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fx/knob_labels.h"
#include "simd.h"

namespace core {
namespace fx {

// The Rytm's labels for P2..P5 (knob_labels.h, 0x24): P2's four characters, each with its size step (0..31).
// Continuous (no step): a size step 0..31 inside each character.
inline constexpr Zone kCloudSeedZones[] = {{0, 0, 0, 1, 0, "ROOM"}, {0, 32, 0, 1, 0, "HALL"}, {0, 64, 0, 1, 0, "PLATE"},
                                           {0, 96, 0, 1, 0, "AMBNC"}};
inline constexpr KnobLabels kCloudSeedLabels = {{"CHAR", "DCY", "DAMP", "MOD"}, kCloudSeedZones, 4, {"Character", "Decay", "Damping", "Modulation"}};
namespace cloudseed {

constexpr float kPi = 3.14159265358979f;
inline float flush(float x) { return (x > -1e-20f && x < 1e-20f) ? 0.0f : x; }
inline float db2gain(float db) { return powf(10.0f, db * 0.05f); }
// sin(2 pi p) for p in [0, 1): a parabola refined once (error < 0.1 %), for the 8-sample modulation updates
inline float sin2pi(float p) {
  float q = p - 0.5f, y = 8.0f * q * (1.0f - 2.0f * fabsf(q));   // ~ sin(2 pi q) = -sin(2 pi p)
  y += 0.225f * (y * fabsf(y) - y);
  return -y;
}

// LcgRandom + RandomBuffer::Generate(seed, count, crossSeed) into out[count]
inline void random_buffer(uint64_t seed, int count, float cross, float* out) {
  uint64_t a = seed, b = ~seed;
  for (int i = 0; i < count; ++i) {
    a = (22695477ull * a + 1) & 0xFFFFFFFFull;
    b = (22695477ull * b + 1) & 0xFFFFFFFFull;
    float fa = (float)(uint32_t)a / 4294967295.0f, fb = (float)(uint32_t)b / 4294967295.0f;
    out[i] = fa * (1.0f - cross) + fb * cross;
  }
}

struct Lp1 {   // Cloud Seed Lp1
  float b0 = 1, a1 = 0, y = 0;
  void Set(float hz, float fs) {
    if (hz >= fs * 0.5f) hz = fs * 0.499f;
    float x = 2.0f * kPi * hz / fs, nn = 2.0f - cosf(x), alpha = nn - sqrtf(nn * nn - 1.0f);
    a1 = alpha, b0 = 1.0f - alpha;
  }
  float Process(float x) { return y = b0 * x + a1 * y; }
  void Flush() { y = flush(y); }
};
struct Hp1 {   // Cloud Seed Hp1
  Lp1 lp;
  void Set(float hz, float fs) { lp.Set(hz, fs); }
  float Process(float x) { return x - lp.Process(x); }
};
struct Shelf {   // Cloud Seed Biquad, LowShelf / HighShelf (Q unused for shelves)
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  void Set(bool high, float fc, float db, float fs) {
    float V = powf(10.0f, fabsf(db) / 20.0f), K = tanf(kPi * fc / fs), s2 = sqrtf(2.0f), s2v = sqrtf(2.0f * V), n;
    if (!high) {
      if (db >= 0) {
        n = 1 / (1 + s2 * K + K * K);
        b0 = (1 + s2v * K + V * K * K) * n, b1 = 2 * (V * K * K - 1) * n, b2 = (1 - s2v * K + V * K * K) * n;
        a1 = 2 * (K * K - 1) * n, a2 = (1 - s2 * K + K * K) * n;
      } else {
        n = 1 / (1 + s2v * K + V * K * K);
        b0 = (1 + s2 * K + K * K) * n, b1 = 2 * (K * K - 1) * n, b2 = (1 - s2 * K + K * K) * n;
        a1 = 2 * (V * K * K - 1) * n, a2 = (1 - s2v * K + V * K * K) * n;
      }
    } else {
      if (db >= 0) {
        n = 1 / (1 + s2 * K + K * K);
        b0 = (V + s2v * K + K * K) * n, b1 = 2 * (K * K - V) * n, b2 = (V - s2v * K + K * K) * n;
        a1 = 2 * (K * K - 1) * n, a2 = (1 - s2 * K + K * K) * n;
      } else {
        n = 1 / (V + s2v * K + K * K);
        b0 = (1 + s2 * K + K * K) * n, b1 = 2 * (K * K - 1) * n, b2 = (1 - s2 * K + K * K) * n;
        a1 = 2 * (K * K - V) * n, a2 = (V - s2v * K + K * K) * n;
      }
    }
  }
  void Clear() { x1 = x2 = y1 = y2 = 0.0f; }
  float Process(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1, y2 = y1, x1 = x, y1 = y;
    return y;
  }
  void Flush() { x1 = flush(x1), x2 = flush(x2), y1 = flush(y1), y2 = flush(y2); }
};

// ModulatedDelay (N samples of buffer); the delay glides toward its target at each modulation update
template <int N>
struct ModDelay {
  static constexpr int kUpdate = 8;
  float buf[N];
  int w = 0, da = 1, count = kUpdate;
  bool no_neon = false;   // the test's reference: the C alone
  float phase = 0.3f, ga = 1, gb = 0, delay = 100, target = 100, mod_amount = 0, mod_rate = 0;
  void Clear() { memset(buf, 0, sizeof buf); }
  void Update() {
    delay += (target - delay) * 0.02f;
    phase += mod_rate * kUpdate;
    if (phase > 1.0f) phase -= floorf(phase);
    float d = delay + mod_amount * sin2pi(phase);
    if (d < 1.0f) d = 1.0f;
    if (d > N - 3) d = (float)(N - 3);
    da = (int)d;
    float p = d - (float)da;
    ga = 1.0f - p, gb = p;
  }
  void Process(const float* in, float* out, int n) {
    for (int i = 0; i < n;) {
      if (count >= kUpdate) Update(), count = 0;
      int run = kUpdate - count < n - i ? kUpdate - count : n - i;
      count += run;
      int e = i + run;
#if CORE_NEON
      // 4 samples at a time while the reads stay clear of this batch's writes (behind them, or, the line nearly full, far
      // enough ahead: the batch writes first) and nothing wraps: the same products, lane by lane
      for (const float32x4_t va = vdupq_n_f32(ga), vb = vdupq_n_f32(gb); !no_neon && da >= 4 && da <= N - 5 && i + 4 <= e; i += 4) {
        int ia = w - da, ib = ia - 1;
        ia += N & -(ia < 0), ib += N & -(ib < 0);
        if (w + 4 > N || ia + 4 > N || ib + 4 > N) break;
        vst1q_f32(buf + w, vld1q_f32(in + i));
        vst1q_f32(out + i, vfmaq_f32(vmulq_f32(vld1q_f32(buf + ib), vb), vld1q_f32(buf + ia), va));
        w += 4;
        if (w >= N) w = 0;
      }
#endif
      for (; i < e; ++i) {
        buf[w] = in[i];
        int ia = w - da, ib = ia - 1;
        ia += N & -(ia < 0), ib += N & -(ib < 0);
        out[i] = buf[ia] * ga + buf[ib] * gb;
        if (++w >= N) w = 0;
      }
    }
  }
};

// ModulatedAllpass (interpolated, modulated when mod_amount > 0)
template <int N>
struct ModAllpass {
  static constexpr int kUpdate = 8;
  float buf[N];
  int idx = 0, da = 1, db = 2, count = kUpdate;
  bool no_neon = false;   // the test's reference: the C alone
  float phase = 0.3f, ga = 1, gb = 0, delay = 100, target = 100, feedback = 0.5f, mod_amount = 0, mod_rate = 0;
  void Clear() { memset(buf, 0, sizeof buf); }
  void Update() {
    delay += (target - delay) * 0.02f;
    phase += mod_rate * kUpdate;
    if (phase > 1.0f) phase -= floorf(phase);
    float m = mod_amount >= delay ? delay - 1.0f : mod_amount;
    float d = delay + m * sin2pi(phase);
    if (d < 1.0f) d = 1.0f;
    if (d > N - 3) d = (float)(N - 3);
    da = (int)d, db = da + 1;
    float p = d - (float)da;
    ga = 1.0f - p, gb = p;
  }
  void Process(const float* in, float* out, int n) {
    for (int i = 0; i < n;) {
      if (count >= kUpdate) Update(), count = 0;
      int run = kUpdate - count < n - i ? kUpdate - count : n - i;
      count += run;
      int e = i + run;
#if CORE_NEON
      for (const float32x4_t va = vdupq_n_f32(ga), vb = vdupq_n_f32(gb), fb = vdupq_n_f32(feedback), tiny = vdupq_n_f32(1e-20f);
           !no_neon && da >= 4 && da <= N - 5 && i + 4 <= e; i += 4) {
        int ia = idx - da, ib = idx - db;
        ia += N & -(ia < 0), ib += N & -(ib < 0);
        if (idx + 4 > N || ia + 4 > N || ib + 4 > N) break;
        float32x4_t o = vfmaq_f32(vmulq_f32(vld1q_f32(buf + ib), vb), vld1q_f32(buf + ia), va);
        float32x4_t v = vfmaq_f32(vld1q_f32(in + i), o, fb);
        v = vreinterpretq_f32_u32(vbicq_u32(vreinterpretq_u32_f32(v), vcaltq_f32(v, tiny)));   // the denormals' guard below
        vst1q_f32(buf + idx, v);
        vst1q_f32(out + i, vfmsq_f32(o, v, fb));
        idx += 4;
        if (idx >= N) idx = 0;
      }
#endif
      for (; i < e; ++i) {
        int ia = idx - da, ib = idx - db;
        ia += N & -(ia < 0), ib += N & -(ib < 0);
        float o = buf[ia] * ga + buf[ib] * gb;
        float v = in[i] + o * feedback;
        v = fabsf(v) < 1e-20f ? 0.0f : v;   // a decaying loop's denormals (branch-free select)
        buf[idx] = v;
        out[i] = o - v * feedback;
        if (++idx >= N) idx = 0;
      }
    }
  }
};

// AllpassDiffuser: S stages, the stage delays seeded 0.1..1 x the delay
template <int N, int S>
struct Diffuser {
  ModAllpass<N> f[S];
  float seeds[S * 3], scale[S];   // scale: each stage's 0.1..1 x the delay
  int stages = 1;
  void Clear() {
    for (auto& s : f) s.Clear();
  }
  void Seed(int seed, float cross) {
    random_buffer((uint64_t)seed, S * 3, cross, seeds);
    for (int i = 0; i < S; ++i) scale[i] = powf(10.0f, seeds[i]) * 0.1f;
  }
  // delay in samples, mod amount in samples, mod rate in Hz
  void Configure(int n, float delay, float feedback, float mod, float rate, float fs, bool snap) {
    stages = n < 1 ? 1 : n > S ? S : n;
    for (int i = 0; i < S; ++i) {
      float t = delay * scale[i];
      if (t > N - 140) t = (float)(N - 140);
      f[i].target = (float)(int)t;
      if (snap) f[i].delay = f[i].target, f[i].phase = 0.01f + 0.98f * seeds[S * 2 + i];
      f[i].feedback = feedback;
      f[i].mod_amount = mod * (0.85f + 0.3f * seeds[S + i]);
      if (f[i].mod_amount > 128.0f) f[i].mod_amount = 128.0f;
      f[i].mod_rate = rate * (0.85f + 0.3f * seeds[S * 2 + i]) / fs;
    }
  }
  void Process(float* io, int n) {
    for (int i = 0; i < stages; ++i) f[i].Process(io, io, n);
  }
};

// Sizes (48 kHz): pre-delay <= 100 ms, early stages <= 52 ms, late lines <= 1.5 x 240 ms + modulation, late
// diffuser stages <= 30 ms
constexpr int kPreN = 4864, kEarlyN = 2688, kEarlyS = 6, kLineN = 17536, kLateN = 1600, kLateS = 3, kLines = 6,
              kFifo = 16;
constexpr float kLineMsMax = 240.0f;

struct Line {   // Cloud Seed DelayLine (TapPostDiffuser)
  ModDelay<kLineN> delay;
  Diffuser<kLateN, kLateS> diff;
  Shelf lo, hi;
  Lp1 lp;
  float fifo[kFifo];   // the feedback: the line's output kFifo samples ago
  int fpos = 0;
  float feedback = 0;
  bool lo_on = false, hi_on = false;
};

struct Preset {
  float lowcut, highcut, predelay_ms;
  int early_n;
  float early_ms, early_fb, early_mod_ms, early_mod_hz;
  int lines, late_n;
  float line_ms, line_mod_ms, line_mod_hz, t60;
  float late_ms, late_fb, late_mod_ms, late_mod_hz;
  float cutoff, lo_hz, lo_db, hi_hz, hi_db;
  float early_db, late_db;
  int seed_diff, seed_delay, seed_post;
};
// early_n 0 = no early diffuser; lo_db / hi_db 0 = that shelf off
// No low shelf in these (lo_db 0: the input low cut keeps the tails clean) and at most 2 late diffuser stages but in
// DARK PLATE (its only diffusion): the CPU (two CLOUDSEEDs under 15 % of a Pi 4 core, test_send_fx's estimate).
static const Preset kPresets[4] = {
    // ROOM: short, dense, bright-ish
    {120, 11000, 3, 4, 14, 0.65f, 0.15f, 0.6f, 4, 2, 38, 0.25f, 0.55f, 0.75f, 9, 0.6f, 0.15f, 0.45f, 8000, 0, 0,
     4000, -3, -6, -2, 211, 418, 735},
    // HALL: medium-large, smooth
    {90, 12000, 18, 4, 30, 0.7f, 0.35f, 0.8f, 6, 2, 90, 0.45f, 0.4f, 2.0f, 18, 0.7f, 0.3f, 0.3f, 9000, 0, 0, 4500,
     -3, -9, 0, 512, 284, 903},
    // DARK PLATE (Cloud Seed's program: low cut, no early diffuser, 96 ms lines, 4.8 s, post diffusion 31.6 ms at
    // 0.85, high shelf 4.5 kHz -4.6 dB; here 6 lines x 3 stages and a 6 kHz in-loop low-pass)
    {200, 18000, 0, 0, 20, 0.7f, 0.0f, 0.0f, 6, 3, 96, 0.68f, 0.095f, 2.6f, 30, 0.85f, 0.37f, 0.058f, 6000, 0, 0,
     4520, -4.6f, -30, -4, 184, 218, 365},
    // AMBIENCE: huge, dark, slowly moving, no shimmer
    {140, 9000, 40, 4, 45, 0.75f, 0.9f, 0.3f, 6, 2, 150, 1.1f, 0.25f, 7.0f, 28, 0.75f, 0.7f, 0.2f, 5000, 0, 0,
     3000, -4, -12, 0, 777, 642, 129},
};

class Channel {   // ReverbChannel, one side
 public:
  ModDelay<kPreN> pre;
  Diffuser<kEarlyN, kEarlyS> early;
  Line line[kLines];
  Hp1 lowcut;
  Lp1 highcut;
  int lines = 4, early_n = 0;
  float early_out = 0, late_out = 1, seeds[kLines * 3];
  float cross = 0;
  void Clear() {
    pre.Clear();
    early.Clear();
    for (auto& l : line) l.delay.Clear(), l.diff.Clear();
    ClearSmall();
  }
  void ClearSmall() {   // everything but the delay buffers
    for (auto& l : line) l.lo.Clear(), l.hi.Clear(), l.lp.y = 0, memset(l.fifo, 0, sizeof l.fifo);
    lowcut.lp.y = highcut.y = 0;
  }
  void Process(const float* in, float* out, int n) {
    float t[kFifo], e[kFifo], sum[kFifo], x[kFifo];
    for (int i = 0; i < n; ++i) {
      float v = highcut.Process(lowcut.Process(in[i]));
      t[i] = v * v < 1e-9f ? 0.0f : v;   // Cloud Seed: zero tiny input (CPU spikes)
    }
    pre.Process(t, t, n);
    if (early_n > 0) early.Process(t, n);
    for (int i = 0; i < n; ++i) e[i] = t[i];
    for (int i = 0; i < n; ++i) sum[i] = 0.0f;
    for (int k = 0; k < lines; ++k) {
      Line& L = line[k];
      int fp = L.fpos;
      for (int i = 0; i < n; ++i) x[i] = t[i] + L.fifo[(fp + i) & (kFifo - 1)] * L.feedback;
      L.delay.Process(x, x, n);
      L.diff.Process(x, n);
      if (L.lo_on) {
        Shelf f = L.lo;
        for (int i = 0; i < n; ++i) x[i] = f.Process(x[i]);
        f.Flush(), L.lo = f;
      }
      if (L.hi_on) {
        Shelf f = L.hi;
        for (int i = 0; i < n; ++i) x[i] = f.Process(x[i]);
        f.Flush(), L.hi = f;
      }
      Lp1 lp = L.lp;
      for (int i = 0; i < n; ++i) {
        float y = lp.Process(x[i]);
        L.fifo[(fp + i) & (kFifo - 1)] = flush(y);
        sum[i] += y;
      }
      lp.Flush(), L.lp = lp;
      L.fpos = (fp + n) & (kFifo - 1);
    }
    lowcut.lp.Flush(), highcut.Flush();
    float g = late_out / sqrtf((float)lines);
    for (int i = 0; i < n; ++i) out[i] = early_out * e[i] + g * sum[i];
  }
};

class CloudSeedFx {
 public:
  CloudSeedFx() { Clear(); }
  void Clear() {
    l_.Clear();
    r_.Clear();
    configured_ = false;
    clear_at_ = -1;
  }
  // The clear spread over calls: true when done. Each call zeroes at most `bytes` (one buffer at a time, in pieces).
  bool ClearStep(int bytes) {
    if (clear_at_ < 0) clear_at_ = 0, clear_off_ = 0;
    while (bytes > 0) {
      float* p;
      int len;
      if (!Buffer(clear_at_, &p, &len)) {
        l_.ClearSmall(), r_.ClearSmall();
        clear_at_ = -1;
        configured_ = false;
        return true;
      }
      int take = (len - clear_off_) * 4 < bytes ? len - clear_off_ : bytes / 4;
      if (take <= 0) take = 1;
      memset(p + clear_off_, 0, take * sizeof(float));
      clear_off_ += take, bytes -= take * 4;
      if (clear_off_ >= len) ++clear_at_, clear_off_ = 0;
    }
    return false;
  }
  void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float /*a*/) {
    if (!configured_ || k[0] != k_[0] || k[1] != k_[1] || k[2] != k_[2] || k[3] != k_[3]) {
      Configure(k, sr, !configured_ || (k[0] >> 5) != (k_[0] >> 5));
      memcpy(k_, k, 4);
      configured_ = true;
    }
    float ol[kFifo], orr[kFifo];
    for (int i = 0; i < n; i += kFifo) {
      int m = n - i < kFifo ? n - i : kFifo;
      l_.Process(in + i, ol, m);
      r_.Process(in + i, orr, m);
      for (int j = 0; j < m; ++j) out[i + j][0] += ol[j], out[i + j][1] += orr[j];
    }
  }
 private:
  // the big buffers in order (both channels): pre, early stages, lines' delays and diffuser stages
  bool Buffer(int i, float** p, int* len) {
    const int per = 1 + kEarlyS + kLines * (1 + kLateS);
    if (i >= 2 * per) return false;
    Channel& c = i < per ? l_ : r_;
    i %= per;
    if (i == 0) return *p = c.pre.buf, *len = kPreN, true;
    i -= 1;
    if (i < kEarlyS) return *p = c.early.f[i].buf, *len = kEarlyN, true;
    i -= kEarlyS;
    Line& L = c.line[i / (1 + kLateS)];
    int j = i % (1 + kLateS);
    if (j == 0) return *p = L.delay.buf, *len = kLineN, true;
    return *p = L.diff.f[j - 1].buf, *len = kLateN, true;
  }
  static float k01(uint8_t v) { return (float)(v > 127 ? 127 : v) * (1.0f / 127.0f); }
  // snap (a new character, or the first call): the seeds, the fixed filters, the delays set at once; otherwise (P2..P5
  // turned or p-locked, maybe every step) only what the macros move, without transcendentals but the lines' gains
  void Configure(const uint8_t* k, float fs, bool snap) {
    const Preset& P = kPresets[(k[0] > 127 ? 127 : k[0]) >> 5];
    float size = 1.0f + 1.5f * (float)((k[0] > 127 ? 127 : k[0]) & 31) / 31.0f, decay = powf(10.0f, k01(k[1])),
          dark = powf(16.0f, -k01(k[2])), mod = 1.0f + 4.0f * k01(k[3]), rate = 1.0f + 2.0f * k01(k[3]);
    float ms = fs / 1000.0f;
    Lp1 cut, high;
    cut.Set(P.cutoff * dark < 300.0f ? 300.0f : P.cutoff * dark, fs);
    high.Set(P.highcut * dark < 400.0f ? 400.0f : P.highcut * dark, fs);
    for (int ch = 0; ch < 2; ++ch) {
      Channel& c = ch ? r_ : l_;
      float cross = ch ? 0.0f : 1.0f;   // EqCrossSeed 0: L the second series, R the first (Cloud Seed)
      if (snap) {
        c.lowcut.Set(P.lowcut, fs);
        c.early.Seed(P.seed_diff, cross);
        random_buffer((uint64_t)P.seed_delay, kLines * 3, cross, c.seeds);
        c.early_n = P.early_n, c.lines = P.lines;
        c.early_out = P.early_n > 0 ? db2gain(P.early_db) : 0.0f;
        c.late_out = db2gain(P.late_db);
        c.pre.mod_amount = 0, c.pre.mod_rate = 0;
      }
      c.highcut.b0 = high.b0, c.highcut.a1 = high.a1;
      float pd = P.predelay_ms * size * ms;
      c.pre.target = pd > kPreN - 8 ? (float)(kPreN - 8) : pd;
      if (snap) c.pre.delay = c.pre.target;
      c.early.Configure(P.early_n, P.early_ms * size * ms, P.early_fb, P.early_mod_ms * mod * ms,
                        P.early_mod_hz * rate, fs, snap);
      const float* seeds = c.seeds;
      float line_ms = P.line_ms * size;
      if (line_ms > kLineMsMax) line_ms = kLineMsMax;
      float t60 = P.t60 * decay * fs;
      for (int i = 0; i < kLines; ++i) {
        Line& L = c.line[i];
        float amt = P.line_mod_ms * mod * ms * (0.7f + 0.3f * seeds[i]);
        if (amt > 120.0f) amt = 120.0f;
        float d = (0.5f + 1.0f * seeds[kLines * 2 + i]) * line_ms * ms;
        if (d < amt + 2.0f) d = amt + 2.0f;
        // the loop: the line's delay + the kFifo ring = d (Cloud Seed: + one block)
        float dl = d - (float)kFifo;
        if (dl < amt + 2.0f) dl = amt + 2.0f;
        L.delay.target = dl;
        if (snap) L.delay.delay = dl, L.delay.phase = 0.01f + 0.98f * seeds[i];
        L.delay.mod_amount = amt;
        L.delay.mod_rate = P.line_mod_hz * rate * (0.7f + 0.3f * seeds[kLines + i]) / fs;
        L.feedback = db2gain(d / t60 * -60.0f);
        if (snap) {
          L.diff.Seed(P.seed_post * (i + 1), cross);
          L.lo_on = P.lo_db != 0, L.hi_on = P.hi_db != 0;
          if (L.lo_on) L.lo.Set(false, P.lo_hz, P.lo_db, fs);
          if (L.hi_on) L.hi.Set(true, P.hi_hz, P.hi_db, fs);
        }
        L.diff.Configure(P.late_n, P.late_ms * size * ms, P.late_fb, P.late_mod_ms * mod * ms, P.late_mod_hz * rate,
                         fs, snap);
        L.lp.b0 = cut.b0, L.lp.a1 = cut.a1;
      }
    }
  }
  Channel l_, r_;
  uint8_t k_[4] = {0, 0, 0, 0};
  bool configured_ = false;
  int clear_at_ = -1, clear_off_ = 0;
};

}  // namespace cloudseed
using cloudseed::CloudSeedFx;
}  // namespace fx
}  // namespace core
