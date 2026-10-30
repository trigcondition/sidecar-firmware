// core/tests/wg_util.h - helpers shared by test_wavetable.cpp, test_granular.cpp and tests/demo/render_wavegrain.cpp:
// reports into the engine, WAV files written for the card (any format, a 'clm ' chunk), a spectrum, a mono WAV out.
#pragma once
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "check.h"
#include "host_link.h"

namespace wg {

// A 0x04 / 0x03 report: voice byte vb (machine flag | voice), P1..P16
inline std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back((uint8_t)note); m.push_back((uint8_t)vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back((uint8_t)i); m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f)); }
  m.push_back(0xf7);
  return m;
}
inline void send(const std::vector<uint8_t>& m) { core::host_sysex(m.data(), (int)m.size()); }
inline void trig(int flag, int vi, int note, int vel, const std::vector<float>& p) { send(report(0x04, flag | vi, note, vel, p)); }
inline void params(int flag, int vi, const std::vector<float>& p) { send(report(0x03, flag | vi, -1, 0, p)); }
inline void off(int vi) { send({0xf0, 0x7d, 0x52, 0x54, 0x11, (uint8_t)vi, 0xf7}); }

// A WAV file's bytes: frames x ch samples from s(i, c) in -1..1; fmt 1 PCM (8/16/24/32) or 3 float (32), or
// extensible (fmt 0xFFFE carrying PCM/float); an optional 'clm ' text before the data
inline std::vector<uint8_t> wav_bytes(int frames, int ch, int bits, int rate, int fmt, const std::string& clm,
                                      const std::function<double(int, int)>& s, bool extensible = false) {
  std::vector<uint8_t> b;
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); };
  auto u16 = [&](uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); };
  b.insert(b.end(), {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  int bps = bits / 8;
  u32(extensible ? 40 : 16);
  u16(extensible ? 0xFFFE : (uint16_t)fmt); u16((uint16_t)ch); u32((uint32_t)rate); u32((uint32_t)(rate * ch * bps));
  u16((uint16_t)(ch * bps)); u16((uint16_t)bits);
  if (extensible) { u16(22); u16((uint16_t)bits); u32(3); u16((uint16_t)fmt); for (int i = 0; i < 14; ++i) b.push_back(0); }
  if (!clm.empty()) {
    b.insert(b.end(), {'c', 'l', 'm', ' '});
    u32((uint32_t)clm.size());
    b.insert(b.end(), clm.begin(), clm.end());
    if (clm.size() & 1) b.push_back(0);
  }
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  u32((uint32_t)(frames * ch * bps));
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) {
      double x = s(i, c);
      if (fmt == 3) {
        float f = (float)x;
        uint8_t q[4];
        memcpy(q, &f, 4);
        b.insert(b.end(), q, q + 4);
        continue;
      }
      if (bits == 8) { b.push_back((uint8_t)(128 + std::lround(x * 127))); continue; }
      int64_t v = std::llround(x * ((1LL << (bits - 1)) - 1));
      for (int k = 0; k < bps; ++k) b.push_back((uint8_t)(v >> (8 * k)));
    }
  uint32_t riff = (uint32_t)b.size() - 8;
  for (int i = 0; i < 4; ++i) b[4 + i] = (uint8_t)(riff >> (8 * i));
  return b;
}
inline void write_file(const std::string& path, const std::vector<uint8_t>& b) {
  FILE* f = fopen(path.c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
// mono 16-bit 48 kHz
inline void write_wav16(const std::string& path, const std::vector<float>& x) {
  write_file(path, wav_bytes((int)x.size(), 1, 16, 48000, 1, "", [&](int i, int) {
               double y = x[i];
               return y > 1 ? 1 : y < -1 ? -1 : y;
             }));
}

// The power spectrum of x (Hann window, zero-padded to a power of two): bin k at k * 48000 / N Hz
inline std::vector<double> spectrum(const std::vector<float>& x, int* nfft) {
  int n = 1;
  while (n < (int)x.size()) n <<= 1;
  std::vector<std::complex<double>> a(n);
  for (size_t i = 0; i < x.size(); ++i) a[i] = x[i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / (x.size() - 1)));
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (int len = 2; len <= n; len <<= 1) {
    std::complex<double> w = std::polar(1.0, -2 * M_PI / len);
    for (int i = 0; i < n; i += len) {
      std::complex<double> c = 1;
      for (int k = 0; k < len / 2; ++k, c *= w) {
        auto t = a[i + k + len / 2] * c;
        a[i + k + len / 2] = a[i + k] - t;
        a[i + k] += t;
      }
    }
  }
  std::vector<double> p(n / 2);
  for (int k = 0; k < n / 2; ++k) p[k] = std::norm(a[k]);
  *nfft = n;
  return p;
}
// Energy near the harmonics of f0 (within +-w Hz) vs everything else from 20 Hz up to hi Hz: the rest in dB below
inline double alias_db(const std::vector<float>& x, double f0, double hi, double w = 30) {
  int n;
  std::vector<double> p = spectrum(x, &n);
  double harm = 0, rest = 0, df = 48000.0 / n;
  for (size_t k = 0; k < p.size(); ++k) {
    double f = k * df;
    if (f < 20 || f > hi) continue;
    double h = std::round(f / f0);
    if (h >= 1 && std::fabs(f - h * f0) <= w) harm += p[k];
    else rest += p[k];
  }
  return 10 * std::log10((rest + 1e-30) / (harm + 1e-30));
}
inline double rms(const std::vector<float>& x, size_t a = 0, size_t b = 0) {
  if (!b) b = x.size();
  double s = 0;
  for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
  return std::sqrt(s / (double)(b - a));
}
inline double zc_freq(const std::vector<float>& x, size_t a = 0, size_t b = 0) {   // rising zero crossings a second
  if (!b) b = x.size();
  int n = 0;
  long first = -1, last = -1;
  for (size_t i = a + 1; i < b; ++i)
    if (x[i - 1] < 0 && x[i] >= 0) {
      if (first < 0) first = (long)i;
      last = (long)i, ++n;
    }
  return n > 1 ? (n - 1) * 48000.0 / (last - first) : 0;
}
inline double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace wg
