// core/wavetable/tables.cpp - see tables.h. Cold: loading and boot only.
#include "../platform.h"

#if CORE_MSAMP
#include "tables.h"

#include <math.h>
#include <string.h>

namespace core {
namespace wt {

namespace {
constexpr float kPi = 3.14159265358979f;

// In-place radix-2 complex FFT of n (a power of two) points; sign -1 forward, +1 inverse (unscaled).
CORE_COLD void fft(float* re, float* im, int n, int sign) {
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double a = sign * 2.0 * 3.14159265358979323846 / len;
    float wr = (float)cos(a), wi = (float)sin(a);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; ++k) {
        int p = i + k, q = p + len / 2;
        float tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
        re[q] = re[p] - tr, im[q] = im[p] - ti;
        re[p] += tr, im[p] += ti;
        float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr, cr = nr;
      }
    }
  }
}
CORE_COLD bool pow2(int n) { return n > 0 && !(n & (n - 1)); }
}  // namespace

CORE_COLD void analyze(const float* x, int n, float* c, float* scratch) {
  memset(c, 0, sizeof(float) * kSpec);
  int hmax = (n - 1) / 2;   // below the frame's own Nyquist
  if (hmax > kHarm) hmax = kHarm;
  if (pow2(n)) {
    float* re = scratch;
    float* im = scratch + n;
    for (int i = 0; i < n; ++i) re[i] = x[i], im[i] = 0;
    fft(re, im, n, -1);
    for (int h = 1; h <= hmax; ++h) c[2 * h] = 2.0f * re[h] / n, c[2 * h + 1] = 2.0f * im[h] / n;
    return;
  }
  for (int h = 1; h <= hmax; ++h) {   // a DFT (any length): a rotation a sample, in double
    double a = -2.0 * 3.14159265358979323846 * h / n, wr = cos(a), wi = sin(a), cr = 1, ci = 0, sr = 0, si = 0;
    for (int i = 0; i < n; ++i) {
      sr += x[i] * cr, si += x[i] * ci;
      double t = cr * wr - ci * wi;
      ci = cr * wi + ci * wr, cr = t;
    }
    c[2 * h] = (float)(2.0 * sr / n), c[2 * h + 1] = (float)(2.0 * si / n);
  }
}

CORE_COLD void synth_frame(const float* c, float* fr, float* scratch) {
  memset(fr, 0, sizeof(float) * kFrameFloats);
  for (int k = 0; k < kLevels; ++k) {
    int n = kLevelSize[k], hm = kLevelHarm[k];
    float* re = scratch;
    float* im = scratch + n;
    for (int i = 0; i < n; ++i) re[i] = im[i] = 0;
    for (int h = 1; h <= hm && h < n / 2; ++h) {   // y = 2 Re(sum Y[h] e^(+)) with Y[h] = c_h / 2
      re[h] = 0.5f * c[2 * h], im[h] = 0.5f * c[2 * h + 1];
      re[n - h] = re[h], im[n - h] = -im[h];
    }
    fft(re, im, n, +1);
    float* x = fr + level_off(k);
    for (int i = 0; i < n; ++i) x[i] = re[i];
    x[-1] = x[n - 1], x[n] = x[0], x[n + 1] = x[1];
  }
}

CORE_COLD float frame_peak(const float* fr) {
  const float* x = level(fr, 0);
  float m = 0;
  for (int i = 0; i < kLevelSize[0]; ++i) m = fabsf(x[i]) > m ? fabsf(x[i]) : m;
  return m;
}

CORE_COLD int detect_frame_size(uint32_t total, uint32_t clm) {
  if (clm >= 16 && clm <= 16384 && total >= clm && total % clm == 0) return (int)clm;
  static const int kSizes[4] = {2048, 1024, 512, 256};
  for (int s : kSizes)
    if (total >= (uint32_t)s && total % s == 0) return s;
  if (total >= 4 && total <= 8192) return (int)total;
  return kFrame;
}

CORE_COLD int frame_count(uint32_t total, int fs) {
  uint32_t n = fs > 0 ? total / (uint32_t)fs : 0;
  return n > (uint32_t)kMaxFrames ? kMaxFrames : (int)n;
}
CORE_COLD uint32_t source_frame(uint32_t total, int fs, int f) {
  uint32_t n = total / (uint32_t)fs;
  if (n <= (uint32_t)kMaxFrames) return (uint32_t)f;
  return (uint32_t)((uint64_t)f * (n - 1) / (kMaxFrames - 1));   // thinned evenly, the first and last kept
}

// ---- the built-ins
CORE_COLD const char* builtin_name(int b) {
  static const char* const k[kBuiltins] = {"BASIC", "PWM", "VOCAL", "ORGAN", "FM", "SYNC"};
  return b >= 0 && b < kBuiltins ? k[b] : "?";
}

namespace {
// x = sum b_h sin + a_h cos: c_h = a_h - i b_h
CORE_COLD void put(float* c, int h, float a, float b) {
  if (h < 1 || h > kHarm) return;
  c[2 * h] += a, c[2 * h + 1] -= b;
}
// sine, triangle, saw, square (band-limited by construction: their series)
CORE_COLD void shape(float* c, int s, float g) {
  for (int h = 1; h <= kHarm; ++h) {
    float b = 0;
    if (s == 0) b = h == 1 ? 1.0f : 0.0f;
    else if (s == 1) b = (h & 1) ? 8.0f / (kPi * kPi) / ((float)h * h) * (((h - 1) / 2) & 1 ? -1.0f : 1.0f) : 0.0f;
    else if (s == 2) b = 2.0f / kPi / h * (h & 1 ? 1.0f : -1.0f) * 0.9f;
    else b = (h & 1) ? 4.0f / kPi / h * 0.8f : 0.0f;
    put(c, h, 0, g * b);
  }
}
// A pulse of width w (0..1), DC removed, peak to peak about 2
CORE_COLD void pulse(float* c, float w) {
  for (int h = 1; h <= kHarm; ++h) {
    float a = sinf(2 * kPi * h * w) / (kPi * h), b = (1 - cosf(2 * kPi * h * w)) / (kPi * h);
    put(c, h, 2 * a * 0.8f, 2 * b * 0.8f);
  }
}
// The vowels' first three formants (Hz, an adult voice), a e i o u
const float kVowel[5][3] = {{800, 1150, 2900}, {400, 1700, 2600}, {280, 2250, 2900}, {450, 800, 2830}, {325, 700, 2530}};
}  // namespace

CORE_COLD void builtin_frame(int b, int f, float* c, float* scratch) {
  memset(c, 0, sizeof(float) * kSpec);
  float t = (float)f / (kBuiltinFrames - 1);   // 0..1 across the table
  switch (b) {
    case 0: {   // BASIC: sine -> triangle -> saw -> square, crossfaded spectra
      float u = t * 3;
      int s = (int)u;
      if (s > 2) s = 2;
      float w = u - s;
      shape(c, s, 1 - w);
      shape(c, s + 1, w);
      break;
    }
    case 1:   // PWM: a pulse from square (50 %) to a thin 4 %
      pulse(c, 0.5f - 0.46f * t);
      break;
    case 2: {   // VOCAL: a 110 Hz voice's harmonics under formant bumps, a -> e -> i -> o -> u
      float u = t * 4;
      int s = (int)u;
      if (s > 3) s = 3;
      float w = u - s;
      for (int h = 1; h <= 128; ++h) {
        float fh = 110.0f * h, a = 0;
        for (int k = 0; k < 3; ++k) {
          float F = kVowel[s][k] * (1 - w) + kVowel[s + 1][k] * w, bw = 80.0f + 40.0f * k;
          float d = (fh - F) / bw;
          a += expf(-0.5f * d * d) * (k == 0 ? 1.0f : k == 1 ? 0.6f : 0.3f);
        }
        a = a * 0.9f + 0.1f / h;   // the glottal pulse under it
        put(c, h, a * 0.35f, 0);
      }
      break;
    }
    case 3: {   // ORGAN: the harmonics come in one by one (1 .. 48), drawbar-like levels
      float n = 1 + 47 * t * t;
      for (int h = 1; h <= 64; ++h) {
        float e = n - (h - 1);
        e = e < 0 ? 0 : e > 1 ? 1 : e;
        float lvl = (h == 1 || h == 2 || h == 3 || h == 4 || h == 6 || h == 8) ? 1.0f : 0.5f;
        put(c, h, 0, e * lvl / sqrtf((float)h) * 0.6f);
      }
      break;
    }
    case 4:     // FM: a sine phase-modulated by its second harmonic, index 0 .. 6
    case 5: {   // SYNC: a saw hard-synced at 1 .. 8 times the fundamental
      float* x = scratch + 2 * kFrame;
      for (int i = 0; i < kFrame; ++i) {
        float ph = (float)i / kFrame;
        if (b == 4) {
          x[i] = sinf(2 * kPi * ph + 6.0f * t * sinf(4 * kPi * ph));
        } else {
          float r = 1 + 7 * t, q = ph * r;
          q -= floorf(q);
          x[i] = (2 * q - 1) * 0.8f * (1 - ph * 0.3f);   // a slight fall so each sync burst decays
        }
      }
      analyze(x, kFrame, c, scratch);
      if (b == 5)   // the table's own fold above harmonic 256: gently rolled off
        for (int h = 128; h <= kHarm; ++h) {
          float g = 1.0f - (h - 128) / 384.0f;
          c[2 * h] *= g, c[2 * h + 1] *= g;
        }
      break;
    }
    default: break;
  }
}

}  // namespace wt
}  // namespace core
#endif
