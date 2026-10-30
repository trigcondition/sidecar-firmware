// core/fx/out_mix.h - the OUT route's DSP (core/fx/out.h): the output stage's per-voice routing, crossfades, LEV / PAN /
// WID, the mute gate and the pairs' limit. Header-only, allocation-free; the Pi's output stage (pi/engine/output.cpp,
// core 0) runs it.
#pragma once
#include <math.h>
#include <stdint.h>

#include "fx/out.h"

namespace core {
namespace out {

class Mixer {
 public:
  Mixer() { Reset(); }
  void Reset() {
    for (int v = 0; v < kVoices; ++v) {
      S& s = s_[v];
      s.ph[kTrk] = 1.0f, s.ph[kPre] = s.ph[kPost] = 0.0f;
      s.gate = 1.0f;
      s.m[0] = s.m[3] = 1.0f, s.m[1] = s.m[2] = 0.0f;
      s.set = s.settled = s.moving = false;
      s.want = kTrk;
      tm_[v][0] = tm_[v][3] = 1.0f, tm_[v][1] = tm_[v][2] = 0.0f;
    }
    trk_ = true;
    ks_ = 1.0f - expf(-1.0f / (kSmoothMs * 0.001f * 48000.0f));
  }
  // Every voice wholly on TRK (no crossfade under way): the output stage's old path, bit for bit
  bool AllTrk(const Control& c) const {
    if (!trk_) return false;
    for (int v = 0; v < kVoices; ++v)
      if (c.route[v] != kTrk) return false;
    return true;
  }
  // Before a run of frames (a transport block): the voices' target matrices from LEV / PAN / WID; st bit v: voice v's
  // pair is a real stereo pair (balance; else PAN is equal power)
  void Block(const Control& c, uint8_t st) {
    for (int v = 0; v < kVoices; ++v)
      if (c.route[v] != kTrk || s_[v].ph[kTrk] < 1.0f) {
        float m[4];
        Matrix(c, v, (st >> v) & 1, m);
        if (m[0] != tm_[v][0] || m[1] != tm_[v][1] || m[2] != tm_[v][2] || m[3] != tm_[v][3]) {
          for (int k = 0; k < 4; ++k) tm_[v][k] = m[k];
          s_[v].settled = false;   // (the smoothing runs until it reaches the new target)
        }
      }
  }
  // One frame (after Block): l / r the 8 voices as rendered (int16; r = l for a voice rendered mono). Out: trk[v] the voice's TRK share (int16 scale, mono:
  // (l + r) / 2 x its crossfade gain), pre / post the pairs (full scale 1.0, before the trim and the limit: Pairs()),
  // feed[v] the stand-in for the voice's Rytm stem (full scale 1.0; 0 on TRK).
  // pk (optional): each voice's |L| / |R| into its pair kept as a running maximum (the display's meters)
  void Frame(const Control& c, const int16_t* l, const int16_t* r, float* trk, float* pre, float* post, float* feed,
             volatile float (*pk)[2] = nullptr) {
    const float ks = ks_;
    pre[0] = pre[1] = post[0] = post[1] = 0.0f;
    bool all = true;
    for (int v = 0; v < kVoices; ++v) {
      S& s = s_[v];
      int want = c.route[v] < kRoutes ? c.route[v] : kTrk;
      if (want != s.want) s.want = (uint8_t)want, s.moving = true;
      if (s.moving) {   // each destination's phase toward 0 / 1 (equal power: sin)
        bool at = true;
        for (int d = 0; d < kRoutes; ++d) {
          float t = d == want ? 1.0f : 0.0f, p = s.ph[d];
          if (p != t) p = t > p ? fminf(t, p + 1.0f / kXfadeFrames) : fmaxf(t, p - 1.0f / kXfadeFrames);
          s.ph[d] = p;
          at = at && p == t;
        }
        s.moving = !at;
      }
      float gt = s.ph[kTrk] >= 1.0f ? 1.0f : sinf(1.5707963f * s.ph[kTrk]);
      int32_t mi = ((int32_t)l[v] + (int32_t)r[v]) / 2;
      trk[v] = gt >= 1.0f ? (float)mi : (float)mi * gt;
      feed[v] = 0.0f;
      if (s.ph[kTrk] < 1.0f || s.ph[kPre] > 0.0f || s.ph[kPost] > 0.0f) all = false;
      if (s.ph[kPre] <= 0.0f && s.ph[kPost] <= 0.0f) {
        s.set = false;   // (wholly TRK: the stereo state restarts at its targets next time)
        continue;
      }
      // the gate (5 ms, smoothstep)
      float gt_target = c.open[v] ? 1.0f : 0.0f;
      if (s.gate != gt_target)
        s.gate = gt_target > s.gate ? fminf(1.0f, s.gate + 1.0f / kGateFrames) : fmaxf(0.0f, s.gate - 1.0f / kGateFrames);
      float gg = s.gate * s.gate * (3.0f - 2.0f * s.gate);
      // the 2x2 matrix: WID (M/S), PAN, LEV - smoothed toward its target
      const float* tm = tm_[v];
      if (!s.set) {
        for (int k = 0; k < 4; ++k) s.m[k] = tm[k];
        s.set = s.settled = true;
      } else if (!s.settled) {
        bool at = true;
        for (int k = 0; k < 4; ++k) {
          float d = tm[k] - s.m[k];
          s.m[k] = fabsf(d) < 1e-5f ? tm[k] : s.m[k] + d * ks;   // (settled: exactly the target)
          at = at && s.m[k] == tm[k];
        }
        s.settled = at;
      }
      float x = l[v] * (1.0f / 32768.0f), y = r[v] * (1.0f / 32768.0f);
      float ol = (s.m[0] * x + s.m[1] * y) * gg, orr = (s.m[2] * x + s.m[3] * y) * gg;
      float gp = s.ph[kPre] >= 1.0f ? 1.0f : sinf(1.5707963f * s.ph[kPre]);
      float gq = s.ph[kPost] >= 1.0f ? 1.0f : sinf(1.5707963f * s.ph[kPost]);
      if (pk) {
        float al = fabsf(ol), ar = fabsf(orr);
        if (al > pk[v][0]) pk[v][0] = al;
        if (ar > pk[v][1]) pk[v][1] = ar;
      }
      pre[0] += ol * gp, pre[1] += orr * gp;
      post[0] += ol * gq, post[1] += orr * gq;
      float lv = c.lev[v] * (1.0f / 100.0f);
      feed[v] = (x + y) * 0.5f * lv * gg * (1.0f - s.ph[kTrk]);
    }
    trk_ = all;
  }
  // The pairs as sent: the trim, then the soft limit
  static void Pairs(const Control& c, float* pre, float* post) {
    float tp = Trim(c.trim10[0]), tq = Trim(c.trim10[1]);
    pre[0] = Limit(pre[0] * tp, kLimitPre), pre[1] = Limit(pre[1] * tp, kLimitPre);
    post[0] = Limit(post[0] * tq, kLimitPost), post[1] = Limit(post[1] * tq, kLimitPost);
  }
  // linear up to kKnee x lim (a pair below it goes out sample-exact), a tanh knee above toward lim (never past it)
  static constexpr float kKnee = 0.7f;
  static float Limit(float x, float lim) {
    float a = x < 0 ? -x : x, k = kKnee * lim;
    if (a <= k) return x;
    a = k + (lim - k) * tanhf((a - k) / (lim - k));
    return x < 0 ? -a : a;
  }
  static float Trim(int db10) { return db10 ? powf(10.0f, (float)db10 / 200.0f) : 1.0f; }
  // The target matrix of voice v: [L<-l, L<-r, R<-l, R<-r]
  static void Matrix(const Control& c, int v, bool real_stereo, float* m) {
    float w = c.wid[v] * (1.0f / 64.0f);                 // 0 mono .. 1 as rendered .. ~2
    float a = 0.5f * (1.0f + w), b = 0.5f * (1.0f - w);  // L' = a l + b r, R' = b l + a r
    float p = (c.pan[v] - 64.0f) * (1.0f / 63.0f);       // -1..1
    p = p < -1.0f ? -1.0f : p > 1.0f ? 1.0f : p;
    float gl, gr;
    if (real_stereo) {   // balance: the far side down, the near side as is
      gl = p > 0.0f ? 1.0f - p : 1.0f;
      gr = p < 0.0f ? 1.0f + p : 1.0f;
    } else {             // equal power, centre = unity
      float th = (p + 1.0f) * 0.78539816f;
      gl = 1.41421356f * cosf(th), gr = 1.41421356f * sinf(th);
      if (c.pan[v] == 64) gl = gr = 1.0f;
    }
    float lv = c.lev[v] * (1.0f / 100.0f);
    m[0] = gl * a * lv, m[1] = gl * b * lv, m[2] = gr * b * lv, m[3] = gr * a * lv;
  }
  float Phase(int v, int d) const { return s_[v & 7].ph[d]; }
  float Gate(int v) const { return s_[v & 7].gate; }

 private:
  struct S {
    float ph[kRoutes];   // each destination's crossfade phase 0..1
    float gate;
    float m[4];          // the smoothed matrix
    bool set;            // m holds a target (else it starts at one, no glide)
    bool settled;        // m is its target
    bool moving;         // a crossfade runs
    uint8_t want;        // the route the phases go toward
  };
  S s_[kVoices];
  float tm_[kVoices][4] = {};
  bool trk_;
  float ks_;
};

}  // namespace out
}  // namespace core
