// core/samp/dsp.cpp - see dsp.h.
#include "dsp.h"

#include <math.h>
#include <string.h>

#include "../platform.h"

namespace core {
namespace samp {

namespace {
inline float fabs_(float v) { return v < 0 ? -v : v; }
inline float at_max(const float* x, int ch, uint32_t i) {
  float a = fabs_(x[(size_t)i * ch]);
  if (ch == 2) {
    float b = fabs_(x[(size_t)i * 2 + 1]);
    if (b > a) a = b;
  }
  return a;
}
inline float mono(const float* x, int ch, uint32_t i) {
  return ch == 2 ? 0.5f * (x[(size_t)i * 2] + x[(size_t)i * 2 + 1]) : x[i];
}
inline void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
}  // namespace

CORE_COLD float peak(const float* x, int ch, uint32_t n) {
  float p = 0;
  for (uint32_t i = 0; i < n; ++i) {
    float a = at_max(x, ch, i);
    if (a > p) p = a;
  }
  return p;
}

CORE_COLD float rms(const float* x, int ch, uint32_t a, uint32_t b) {
  if (b <= a) return 0;
  double s = 0;
  for (size_t i = (size_t)a * ch; i < (size_t)b * ch; ++i) s += (double)x[i] * x[i];
  return (float)sqrt(s / ((double)(b - a) * ch));
}

CORE_COLD uint32_t first_above(const float* x, int ch, uint32_t n, float thr, uint32_t from) {
  for (uint32_t i = from; i < n; ++i)
    if (at_max(x, ch, i) > thr) return i;
  return n;
}

CORE_COLD uint32_t last_above(const float* x, int ch, uint32_t n, float thr) {
  for (uint32_t i = n; i > 0; --i)
    if (at_max(x, ch, i - 1) > thr) return i;
  return 0;
}

CORE_COLD bool trim(const float* x, int ch, uint32_t n, uint32_t floor_n, Trim* out, float min_db) {
  float fl = db_to_lin(min_db);
  if (floor_n > 0) {
    float r = rms(x, ch, 0, floor_n < n ? floor_n : n) * 3.98f;   // + 12 dB
    if (r > fl) fl = r;
  }
  uint32_t s = first_above(x, ch, n, fl);
  if (s >= n) return false;
  uint32_t e = last_above(x, ch, n, fl);
  uint32_t pre = kRate / 500, post = kRate / 100;   // 2 ms, 10 ms
  out->start = s > pre ? s - pre : 0;
  out->end = e + post < n ? e + post : n;
  out->floor = fl;
  return true;
}

CORE_COLD int transients(const float* x, int ch, uint32_t n, uint32_t* out, int max, float sens, uint32_t min_gap_ms) {
  if (max <= 0) return 0;
  const uint32_t hop = 256;
  int k = 0;
  out[k++] = 0;
  uint32_t nh = n / hop;
  if (nh < 2) return k;
  float hist[8] = {0};
  int hn = 0;
  float pk = peak(x, ch, n), floor_e = (pk * 0.03f) * (pk * 0.03f);   // -30 dB under the peak, as energy
  uint32_t gap = min_gap_ms * kRate / 1000, last = 0;
  float prev = 0;
  for (uint32_t h = 0; h < nh; ++h) {
    double e = 0;
    for (uint32_t i = h * hop; i < (h + 1) * hop; ++i) {
      float m = mono(x, ch, i);
      e += (double)m * m;
    }
    float ef = (float)(e / hop);
    float mean = 0;
    for (int j = 0; j < hn; ++j) mean += hist[j];
    mean = hn ? mean / hn : 0;
    bool rise = h > 0 && ef > floor_e && ef > sens * mean && ef > prev;
    if (rise) {
      // the onset inside this hop (or the one before, when it already rose there): the first frame over a quarter
      // of this hop's peak
      uint32_t a = h * hop >= hop ? h * hop - hop : 0, b = (h + 1) * hop;
      float hp = 0;
      for (uint32_t i = a; i < b; ++i) {
        float v = at_max(x, ch, i);
        if (v > hp) hp = v;
      }
      uint32_t at = first_above(x, ch, b, hp * 0.25f, a);
      if (at - last >= gap && at > 0) {
        if (k < max) out[k++] = at;
        last = at;
      }
    }
    prev = ef;
    if (hn < 8) hist[hn++] = ef;
    else {
      memmove(hist, hist + 1, sizeof hist - sizeof hist[0]);
      hist[7] = ef;
    }
  }
  return k;
}

CORE_COLD int grid(const float* x, int ch, uint32_t n, int slices, uint32_t* out) {
  if (slices < 1) slices = 1;
  uint32_t w = kRate / 1000;
  for (int k = 0; k < slices; ++k) {
    uint32_t t = (uint32_t)((uint64_t)n * k / slices);
    uint32_t best = t;
    if (k > 0) {
      uint32_t a = t > w ? t - w : 0, b = t + w < n ? t + w : n - 1;
      uint32_t d = ~0u;
      for (uint32_t i = a; i < b; ++i) {
        float p = mono(x, ch, i), q = mono(x, ch, i + 1);
        if ((p <= 0 && q > 0) || (p >= 0 && q < 0)) {
          uint32_t dd = i + 1 > t ? i + 1 - t : t - i - 1;
          if (dd < d) d = dd, best = i + 1;
        }
      }
    }
    out[k] = best;
  }
  return slices;
}

CORE_COLD bool sustained(const float* x, uint32_t a, uint32_t b) {
  if (b <= a + 8 * 480) return false;
  float r[8], m = 0;
  for (int k = 0; k < 8; ++k) {
    r[k] = rms(x, 1, a + (b - a) * k / 8, a + (b - a) * (k + 1) / 8);
    m += r[k];
  }
  m /= 8;
  if (m < 0.003f) return false;   // -50 dBFS
  for (int k = 0; k < 8; ++k)
    if (r[k] < m * 0.708f || r[k] > m * 1.413f) return false;   // +-3 dB
  return true;
}

CORE_COLD Loop find_loop(const float* x, uint32_t a, uint32_t b, uint32_t min_ms, float max_score) {
  Loop L = {false, 0, 0, 0, 1e9f};
  const uint32_t W = 4096, lag_lo = kRate / 2000, lag_hi = kRate / 30;
  if (b <= a + W + lag_hi + 64) return L;
  uint32_t mid = a + (b - a) / 2 - (W + lag_hi) / 2;
  // the normalised autocorrelation
  static float acf[kRate / 30 + 1];
  double e0 = 0;
  for (uint32_t i = 0; i < W; ++i) e0 += (double)x[mid + i] * x[mid + i];
  if (e0 <= 1e-9) return L;
  float best = 0;
  for (uint32_t lag = lag_lo; lag <= lag_hi; ++lag) {
    double s = 0, e1 = 0;
    for (uint32_t i = 0; i < W; ++i) s += (double)x[mid + i] * x[mid + i + lag], e1 += (double)x[mid + i + lag] * x[mid + i + lag];
    acf[lag] = (float)(s / sqrt(e0 * e1 + 1e-12));
    if (acf[lag] > best) best = acf[lag];
  }
  uint32_t P = 0;
  for (uint32_t lag = lag_lo + 1; lag < lag_hi && !P; ++lag)
    if (acf[lag] >= best * 0.9f && acf[lag] >= acf[lag - 1] && acf[lag] >= acf[lag + 1]) P = lag;
  if (!P || best < 0.5f) return L;
  // refine the period between frames (parabola through the peak)
  float y0 = acf[P - 1], y1 = acf[P], y2 = acf[P + 1], den = y0 - 2 * y1 + y2;
  float per = (float)P + (den != 0 ? 0.5f * (y0 - y2) / den : 0);
  L.period = per;
  uint32_t min_len = min_ms * kRate / 1000;
  int k = (int)ceilf(min_len / per);
  if (k < 1) k = 1;
  float pw = rms(x, 1, a, b);
  pw = pw * pw * 32 + 1e-9f;
  const uint32_t S = 32;
  uint32_t half = a + (b - a) / 2;
  for (; k * per < (float)(b - a) * 0.6f; k *= 2) {
    uint32_t len0 = (uint32_t)(k * per + 0.5f);
    for (uint32_t s = a + S; s + len0 + 8 < b && s < half; ++s) {
      if (!(x[s - 1] < 0 && x[s] >= 0)) continue;   // rising zero crossings
      for (int d = -4; d <= 4; ++d) {
        uint32_t e = s + len0 + d;
        if (e + 1 >= b || e <= s + S) continue;
        double err = 0;
        for (uint32_t i = 0; i < S; ++i) {
          float u = x[e - S + i] - x[s - S + i];
          err += (double)u * u;
        }
        float r1 = rms(x, 1, s, s + (e - s) / 2), r2 = rms(x, 1, s + (e - s) / 2, e);
        float score = (float)(err / pw) + (r1 + r2 > 0 ? fabs_(r1 - r2) / (r1 + r2) : 0);
        if (score < L.score) L.score = score, L.s = s, L.e = e;
      }
    }
    if (L.score < max_score) break;   // short enough and clean: done
  }
  L.ok = L.score < max_score && L.e > L.s;
  return L;
}

CORE_COLD void crossfade_loop(float* x, uint32_t s, uint32_t e, uint32_t xf) {
  if (xf == 0 || s < xf || e < s + xf) return;
  for (uint32_t i = 0; i < xf; ++i) {
    float t = (i + 0.5f) / xf;
    float wi = sinf(1.5707963f * t), wo = cosf(1.5707963f * t);
    x[e - xf + i] = x[e - xf + i] * wo + x[s - xf + i] * wi;
  }
}

CORE_COLD int wav_header(uint8_t* o, int ch, int bits, uint32_t rate, uint32_t frames, bool loop, uint32_t loop_s,
                         uint32_t loop_e, int root) {
  uint32_t data = wav_data_bytes(ch, bits, frames);
  uint32_t smpl = loop ? 8 + 60 : 0;
  int p = 0;
  memcpy(o + p, "RIFF", 4), put32(o + p + 4, 4 + 24 + smpl + 8 + data + (data & 1)), memcpy(o + p + 8, "WAVE", 4);
  p += 12;
  memcpy(o + p, "fmt ", 4), put32(o + p + 4, 16);
  put16(o + p + 8, 1), put16(o + p + 10, (uint16_t)ch), put32(o + p + 12, rate);
  put32(o + p + 16, rate * ch * (bits / 8)), put16(o + p + 20, (uint16_t)(ch * (bits / 8))), put16(o + p + 22, (uint16_t)bits);
  p += 24;
  if (loop) {
    memcpy(o + p, "smpl", 4), put32(o + p + 4, 60);
    memset(o + p + 8, 0, 60);
    put32(o + p + 16, (uint32_t)(1000000000.0 / rate));   // sample period, ns
    put32(o + p + 20, (uint32_t)root);                    // MIDI unity note
    put32(o + p + 36, 1);                                 // one loop
    put32(o + p + 44 + 0, 0);                             // cue id
    put32(o + p + 44 + 4, 0);                             // forward
    put32(o + p + 44 + 8, loop_s);
    put32(o + p + 44 + 12, loop_e);
    p += 68;
  }
  memcpy(o + p, "data", 4), put32(o + p + 4, data);
  return p + 8;
}

CORE_COLD void note_name(int note, char* out) {
  static const char* const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  int o = note / 12 - 1;
  sniprintf(out, 6, "%s%d", n[note % 12], o);
}

}  // namespace samp
}  // namespace core
