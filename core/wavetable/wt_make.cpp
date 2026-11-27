// core/wavetable/wt_make.cpp - see wt_make.h. Cold.
#include "wt_make.h"

#if CORE_MSAMP

#include <math.h>
#include <string.h>

#include "tables.h"

namespace core {
namespace wt {
namespace make {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kBudget = 400000;       // multiply-adds a step (the harmonics)
constexpr int kScan = 65536;          // samples a step of the scans
constexpr float kVoicedConf = 0.6f;   // a frame's pitch is clear at this confidence (1 - YIN's difference)
constexpr int kSegMax = 8192;

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
CORE_COLD int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

CORE_COLD int Maker::begin(const float* x, int ch, uint32_t n, uint32_t rate, int nframes, float* out) {
  if (!x || !out || (ch != 1 && ch != 2) || !rate || nframes < 1 || nframes > kMaxFrames) return kErrArg;
  if (n < (uint32_t)kMinSelection) return kErrShort;
  x_ = x, ch_ = ch, n_ = n, rate_ = rate, nf_ = nframes, out_ = out;
  method_ = kNone, period_ = 0, voiced_ = 0, peak_ = 0, pos_ = 0, f_ = 0, open_ = false, direct_ = false, emax_ = 0;
  for (int i = 0; i < kMaxFrames; ++i) T_[i] = conf_[i] = 0;
  memset(prev_, 0, sizeof prev_);
  for (int m = 0; m < kFrameLen; ++m) {
    double a = 2.0 * kPi * m / kFrameLen;
    cs_[m] = (float)cos(a), sn_[m] = (float)sin(a);
  }
  st_ = kPeak;
  return 0;
}

CORE_COLD int Maker::progress() const {
  switch (st_) {
    case kPeak: return n_ ? (int)(5ull * pos_ / n_) : 0;
    case kDetect: return 5 + 15 * f_ / nf_;
    case kDecide: return 20;
    case kBuild: return 20 + 75 * f_ / nf_;
    case kNorm: return (peak_ >= 0 ? 95 : 97) + (int)((peak_ >= 0 ? 2ull : 3ull) * pos_ / ((uint64_t)nf_ * kFrameLen));
    case kDone: return 100;
    default: return 0;
  }
}

// ---- the period at frame f's place

CORE_COLD void Maker::detect(int f) {
  T_[f] = 0, conf_[f] = 0;
  const int64_t c = nf_ > 1 ? (int64_t)((uint64_t)f * (n_ - 1) / (nf_ - 1)) : n_ / 2;
  int D = (int)(rate_ / 12000);
  if (D < 1) D = 1;
  const int rd = (int)(rate_ / D);
  int tmin = rd / 2000;
  if (tmin < 4) tmin = 4;
  int tmax = rd / 40;
  if (tmax > 2700 / D) tmax = 2700 / D;   // (the cycle's longest: 8192 / 3 samples)
  if (tmax > 1099) tmax = 1099;
  const int avail = (int)(n_ / (uint32_t)D);
  if (2 * tmax > avail) tmax = avail / 2;
  if (tmax < tmin + 3) return;
  const int J = tmax, W = J + tmax;
  const int64_t s0 = clamp64(c - (int64_t)W * D / 2, 0, (int64_t)n_ - (int64_t)W * D);
  double mean = 0;
  for (int i = 0; i < W; ++i) {
    float a = 0;
    for (int k = 0; k < D; ++k) a += mono((uint32_t)(s0 + (int64_t)i * D + k));
    dec_[i] = a / D;
    mean += dec_[i];
  }
  mean /= W;
  double energy = 0;
  for (int i = 0; i < W; ++i) dec_[i] -= (float)mean, energy += (double)dec_[i] * dec_[i];
  if (energy < 1e-9 * W) return;
  // YIN: the difference function, its cumulative mean normalisation
  cmnd_[0] = 1;
  double run = 0;
  for (int tau = 1; tau <= tmax; ++tau) {
    double d = 0;
    for (int j = 0; j < J; ++j) {
      float e = dec_[j] - dec_[j + tau];
      d += (double)e * e;
    }
    run += d;
    cmnd_[tau] = run > 0 ? (float)(d * tau / run) : 1.0f;
  }
  int tau = -1;
  for (int t = tmin; t < tmax; ++t)
    if (cmnd_[t] < 0.15f) {
      while (t + 1 < tmax && cmnd_[t + 1] < cmnd_[t]) ++t;
      tau = t;
      break;
    }
  if (tau < 0) {
    tau = tmin;
    for (int t = tmin; t < tmax; ++t)
      if (cmnd_[t] < cmnd_[tau]) tau = t;
  }
  float cf = 1.0f - cmnd_[tau];
  cf = cf < 0 ? 0 : cf > 1 ? 1 : cf;
  double tf = tau;
  if (tau > 1 && tau < tmax) {
    double a = cmnd_[tau - 1], b = cmnd_[tau], d = cmnd_[tau + 1], den = a - 2 * b + d;
    if (den > 1e-12) tf += 0.5 * (a - d) / den;
  }
  double T0 = tf * D;
  conf_[f] = cf;
  T_[f] = (float)T0;
  // the same at the full rate, around T0
  int lo = (int)floor(T0) - D - 1, hi = (int)ceil(T0) + D + 1;
  if (lo < 2) lo = 2;
  int Lr = 4 * (int)T0;
  if (Lr > (int)n_ - hi - 1) Lr = (int)n_ - hi - 1;
  if (Lr > kSegMax - hi - 2) Lr = kSegMax - hi - 2;
  if (Lr < (int)T0 || Lr < 16) return;
  const int64_t sr = clamp64(c - (int64_t)(Lr + hi) / 2, 0, (int64_t)n_ - Lr - hi - 1);
  double m2 = 0;
  for (int i = 0; i < Lr + hi + 1; ++i) xw_[i] = mono((uint32_t)(sr + i)), m2 += xw_[i];
  m2 /= Lr + hi + 1;
  for (int i = 0; i < Lr + hi + 1; ++i) xw_[i] -= (float)m2;
  double best = 1e30, dm = 0, dp = 0, db = 0;
  int bt = lo;
  double dn[64];
  const int cnt = hi - lo + 1 < 64 ? hi - lo + 1 : 64;
  for (int k = 0; k < cnt; ++k) {
    const int t = lo + k;
    double num = 0, den = 1e-20;
    for (int j = 0; j < Lr; ++j) {
      float a = xw_[j], b = xw_[j + t], e = a - b;
      num += (double)e * e, den += (double)a * a + (double)b * b;
    }
    dn[k] = num / den;
    if (dn[k] < best) best = dn[k], bt = t;
  }
  double r = bt;
  const int kb = bt - lo;
  if (kb > 0 && kb < cnt - 1) {
    dm = dn[kb - 1], db = dn[kb], dp = dn[kb + 1];
    double den = dm - 2 * db + dp;
    if (den > 1e-15) r += 0.5 * (dm - dp) / den;
  }
  T_[f] = (float)r;
}

// ---- the period track: the frames with a clear pitch kept in step with each other, the others holding the last
CORE_COLD void Maker::decide() {
  int nv = 0, anchor = 0;
  for (int f = 0; f < nf_; ++f) {
    if (T_[f] > 0 && conf_[f] >= kVoicedConf) ++nv;
    if (conf_[f] > conf_[anchor]) anchor = f;
  }
  voiced_ = (float)nv / nf_;
  method_ = kSpectral;
  period_ = 0;
  if (voiced_ < 0.5f) return;
  auto voiced = [&](int f) { return T_[f] > 0 && conf_[f] >= kVoicedConf; };
  auto pick = [&](float t, float prev) {   // t, 2t or t / 2: whichever is nearest the last period (an octave jump mended)
    float best = t, bd = fabsf(logf(t / prev));
    const float cand[2] = {2 * t, 0.5f * t};
    for (float cd : cand) {
      float d = fabsf(logf(cd / prev));
      if (d < bd - 0.05f) best = cd, bd = d;
    }
    return best;
  };
  if (!voiced(anchor)) return;
  float prev = T_[anchor];
  for (int f = anchor + 1; f < nf_; ++f) {
    if (voiced(f)) T_[f] = prev = pick(T_[f], prev);
    else T_[f] = prev;
  }
  prev = T_[anchor];
  for (int f = anchor - 1; f >= 0; --f) {
    if (voiced(f)) T_[f] = prev = pick(T_[f], prev);
    else T_[f] = prev;
  }
  // the median period (an insertion sort of up to 256)
  float s[kMaxFrames];
  for (int i = 0; i < nf_; ++i) {
    float v = T_[i];
    int j = i;
    while (j > 0 && s[j - 1] > v) s[j] = s[j - 1], --j;
    s[j] = v;
  }
  period_ = s[nf_ / 2];
  if (period_ < 4.0f || (float)n_ < 1.5f * period_) {   // too short for a cycle or two: the spectral way
    period_ = 0;
    return;
  }
  method_ = kPitched;
}

// ---- building a frame

CORE_COLD void Maker::win_pitched(int f) {
  const double T = T_[f];
  const int64_t c = nf_ > 1 ? (int64_t)((uint64_t)f * (n_ - 1) / (nf_ - 1)) : n_ / 2;
  int L = (int)floor(3.0 * T + 0.5);
  if (L > kSegMax) L = kSegMax;
  if (L > (int)n_) L = (int)n_;
  len_ = L;
  start_ = (uint32_t)clamp64(c - L / 2, 0, (int64_t)n_ - L);
  per_ = T;
  double sw = 0, sx = 0;
  for (int k = 0; k < L; ++k) {
    float w = 0.5f - 0.5f * (float)cos(2.0 * kPi * (k + 0.5) / L);
    xw_[k] = w;   // (the window first, x after the mean is known)
    sw += w, sx += (double)w * mono(start_ + (uint32_t)k);
  }
  const float mean = (float)(sx / sw);
  for (int k = 0; k < L; ++k) xw_[k] *= mono(start_ + (uint32_t)k) - mean;
  sumw_ = sw;
  int hm = (int)ceil(T / 2.0 - 1e-6) - 1;
  hmax_ = clampi(hm, 1, 512);
  h_ = 1;
  direct_ = false;
}

CORE_COLD void Maker::win_spectral(int f) {
  const int64_t c = nf_ > 1 ? (int64_t)((uint64_t)f * (n_ - 1) / (nf_ - 1)) : n_ / 2;
  int W = n_ >= 2560 ? kFrameLen : (int)(n_ * 4 / 5);
  if (W < 32) W = 32;
  const int Lx = W / 4;
  const int span = W + Lx;
  const uint32_t s = (uint32_t)clamp64(c - span / 2, 0, (int64_t)n_ - span);
  double mean = 0;
  for (int k = 0; k < W; ++k) {
    float v;
    if (k < Lx) {   // the cycle's start fades in from the window's own end beyond W (equal power)
      float a = (k + 0.5f) / Lx;
      v = mono(s + (uint32_t)k) * sqrtf(a) + mono(s + (uint32_t)(W + k)) * sqrtf(1.0f - a);
    } else {
      v = mono(s + (uint32_t)k);
    }
    xw_[k] = v, mean += v;
  }
  mean /= W;
  for (int k = 0; k < W; ++k) xw_[k] -= (float)mean;
  len_ = W, sumw_ = W, per_ = W;
  hmax_ = clampi((W - 1) / 2, 1, 512);
  h_ = 1;
  direct_ = W == kFrameLen;
  if (direct_) memcpy(out_ + (size_t)f * kFrameLen, xw_, sizeof(float) * kFrameLen);
}

// harmonics h_.. of the window in xw_ (harmonic h at h / per_ cycles a sample, phase 0 at the window's middle) until
// the budget (multiply-adds) is spent; returns the budget left
CORE_COLD int Maker::harm_pass(int budget) {
  while (h_ <= hmax_ && budget > 0) {
    const double w = 2.0 * kPi * h_ / per_;
    const double wr = cos(w), wi = -sin(w);
    double zr = cos(w * len_ / 2.0), zi = sin(w * len_ / 2.0), sr = 0, si = 0;
    for (int k = 0; k < len_; ++k) {
      sr += xw_[k] * zr, si += xw_[k] * zi;
      double t = zr * wr - zi * wi;
      zi = zr * wi + zi * wr, zr = t;
    }
    c_[2 * h_] = (float)(2.0 * sr / sumw_), c_[2 * h_ + 1] = (float)(2.0 * si / sumw_);
    ++h_;
    budget -= len_;
  }
  return budget;
}

// the harmonics in c_ (hmax_ of them) turned to the place that best matches the frame before's, then one cycle of 2048
CORE_COLD void Maker::finish_frame(int f) {
  const int hm = hmax_;
  if (method_ == kPitched) {
    double en = 0;
    for (int h = 1; h <= hm; ++h) en += (double)c_[2 * h] * c_[2 * h] + (double)c_[2 * h + 1] * c_[2 * h + 1];
    float pr[2 * 33];
    if (f == 0 || emax_ == 0) {   // the first frame: its fundamental a sine (a rising zero crossing at the cycle's start)
      memset(pr, 0, sizeof pr);
      pr[3] = -1.0f;
    } else {
      memcpy(pr, prev_, sizeof pr);
    }
    const int hc = hm < 32 ? hm : 32;
    float ar[33], ai[33];
    for (int h = 1; h <= hc; ++h) {
      ar[h] = c_[2 * h] * pr[2 * h] + c_[2 * h + 1] * pr[2 * h + 1];
      ai[h] = c_[2 * h + 1] * pr[2 * h] - c_[2 * h] * pr[2 * h + 1];
    }
    float r[kFrameLen];
    int bs = 0;
    for (int s = 0; s < kFrameLen; ++s) {
      float acc = 0;
      for (int h = 1; h <= hc; ++h) {
        int m = (h * s) & (kFrameLen - 1);
        acc += ar[h] * cs_[m] - ai[h] * sn_[m];
      }
      r[s] = acc;
      if (acc > r[bs]) bs = s;
    }
    double d = 0;
    {
      float a = r[(bs + kFrameLen - 1) & (kFrameLen - 1)], b = r[bs], cc = r[(bs + 1) & (kFrameLen - 1)];
      float den = a - 2 * b + cc;
      if (den < -1e-12f) d = 0.5 * (a - cc) / den;
    }
    const double th = 2.0 * kPi * (bs + d) / kFrameLen;
    for (int h = 1; h <= hm; ++h) {   // c_h e^(i h th)
      const double cr = cos(h * th), ci = sin(h * th);
      float re = c_[2 * h], im = c_[2 * h + 1];
      c_[2 * h] = (float)(re * cr - im * ci), c_[2 * h + 1] = (float)(re * ci + im * cr);
    }
    if (en > emax_) emax_ = (float)en;
    if (en >= 1e-4 * emax_) {   // (a quiet frame is no reference)
      memset(prev_, 0, sizeof prev_);
      memcpy(prev_ + 2, c_ + 2, sizeof(float) * 2 * (hm < 32 ? hm : 32));
    }
  }
  memset(re_, 0, sizeof re_), memset(im_, 0, sizeof im_);
  for (int h = 1; h <= hm; ++h) {
    re_[h] = 0.5f * c_[2 * h], im_[h] = 0.5f * c_[2 * h + 1];
    re_[kFrameLen - h] = re_[h], im_[kFrameLen - h] = -im_[h];
  }
  fft_inplace(re_, im_, kFrameLen, +1);
  memcpy(out_ + (size_t)f * kFrameLen, re_, sizeof(float) * kFrameLen);
}

CORE_COLD bool Maker::build_step() {
  if (!open_) {
    if (method_ == kPitched) win_pitched(f_);
    else win_spectral(f_);
    open_ = true;
    if (direct_) {   // (a 2048-sample cycle: copied)
      open_ = false;
      ++f_;
      return true;
    }
  }
  harm_pass(kBudget);
  if (h_ > hmax_) {
    finish_frame(f_);
    open_ = false;
    ++f_;
  }
  return true;
}

CORE_COLD int Maker::step() {
  switch (st_) {
    case kPeak: {
      const uint32_t e = pos_ + kScan < n_ ? pos_ + kScan : n_;
      for (; pos_ < e; ++pos_) {
        float a = fabsf(mono(pos_));
        if (a > peak_) peak_ = a;
      }
      if (pos_ >= n_) {
        if (!(peak_ > 1e-5f)) return st_ = kIdle, kErrSilent;
        st_ = kDetect, f_ = 0;
      }
      return 1;
    }
    case kDetect:
      detect(f_);
      if (++f_ >= nf_) st_ = kDecide;
      return 1;
    case kDecide:
      decide();
      st_ = kBuild, f_ = 0, open_ = false, emax_ = 0;
      return 1;
    case kBuild:
      build_step();
      if (f_ >= nf_) st_ = kNorm, pos_ = 0, peak_ = 0;
      return 1;
    case kNorm: {   // one gain for the table: its loudest sample 0.95 (the frames keep their own levels)
      const uint32_t total = (uint32_t)nf_ * kFrameLen;
      if (peak_ >= 0) {   // pass 1: the peak
        const uint32_t e = pos_ + kScan < total ? pos_ + kScan : total;
        for (; pos_ < e; ++pos_) {
          float a = fabsf(out_[pos_]);
          if (a > peak_) peak_ = a;
        }
        if (pos_ >= total) {
          pos_ = 0;
          gain_ = peak_ > 1e-9f ? 0.95f / peak_ : 0.0f;
          peak_ = -1;
        }
        return 1;
      }
      const uint32_t e = pos_ + kScan < total ? pos_ + kScan : total;
      for (; pos_ < e; ++pos_) out_[pos_] *= gain_;
      if (pos_ >= total) st_ = kDone;
      return st_ == kDone ? 0 : 1;
    }
    default:
      return 0;
  }
}

// A wavetable's header: mono 32-bit float, a fact chunk, and the 'clm ' chunk Serum and Vital read (the frame size
// 2048); at most kTableHeaderMax bytes. wav_read.h parses it.
CORE_COLD int table_wav_header(uint8_t* o, uint32_t rate, uint32_t frames) {
  static const char kClm[] = "<!>2048 10000000 wavetable (Sidecar)";   // (36 characters, then a NUL: 37, a pad byte)
  const uint32_t clm = sizeof kClm, data = frames * 4;
  auto w32 = [](uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); };
  auto w16 = [](uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); };
  uint8_t* p = o;
  memcpy(p, "RIFF", 4), p += 4;
  w32(p, 4 + 24 + 12 + 8 + clm + (clm & 1) + 8 + data + (data & 1)), p += 4;
  memcpy(p, "WAVEfmt ", 8), p += 8;
  w32(p, 16), p += 4;
  w16(p, 3), w16(p + 2, 1), w32(p + 4, rate), w32(p + 8, rate * 4), w16(p + 12, 4), w16(p + 14, 32), p += 16;
  memcpy(p, "fact", 4), w32(p + 4, 4), w32(p + 8, frames), p += 12;
  memcpy(p, "clm ", 4), w32(p + 4, clm), p += 8;
  memcpy(p, kClm, clm), p += clm;
  if (clm & 1) *p++ = 0;
  memcpy(p, "data", 4), w32(p + 4, data), p += 8;
  return (int)(p - o);
}

CORE_COLD int make_table(const float* x, int ch, uint32_t n, uint32_t rate, int nframes, float* out, Method* method,
                         float* period) {
  static Maker m;
  int r = m.begin(x, ch, n, rate, nframes, out);
  if (r < 0) return r;
  while ((r = m.step()) > 0) {
  }
  if (method) *method = m.method();
  if (period) *period = m.period();
  return r;
}

}  // namespace make
}  // namespace wt
}  // namespace core

#endif
