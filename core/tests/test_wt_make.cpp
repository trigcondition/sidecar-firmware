// core/tests/test_wt_make.cpp - MAKE WT's converter (core/wavetable/wt_make.h): a table from a sample. A band-limited
// saw at 110 / 220 / 437.3 Hz and at 44.1 / 48 / 96 kHz gives frames that are the 2048-sample saw cycle; vibrato of
// +-50 cents keeps the frames aligned; a saw to square sweep morphs in steps no larger than the sweep's own; stereo is
// summed; a short selection repeats its cycle; silence is refused; noise takes the spectral way, never NaN; the WAV a
// table is written as reads back through wav_read.h with `clm ` 2048 and N frames.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "check.h"
#include "wavetable/tables.h"
#include "wavetable/wav_read.h"
#include "wavetable/wt_make.h"

using namespace core;
using namespace core::wt::make;

static const double kPi = 3.14159265358979323846;

// sum of sin(2 pi h f t) / h over h up to the source's Nyquist (a band-limited saw); sq: odd h only
static std::vector<float> additive(double f, double secs, uint32_t rate, double vib_cents = 0, double vib_hz = 5.5,
                                   int kind = 0, double mix_end = 0) {
  size_t n = (size_t)(secs * rate);
  std::vector<float> x(n);
  double ph = 0;
  for (size_t i = 0; i < n; ++i) {
    double t = (double)i / rate;
    double fi = f * pow(2.0, vib_cents / 1200.0 * sin(2 * kPi * vib_hz * t));
    ph += fi / rate;
    double m = mix_end > 0 ? t / secs : 0;   // 0 saw .. 1 square over the sweep
    int hm = (int)(rate / 2 / (f * pow(2.0, fabs(vib_cents) / 1200.0)) - 0.01);
    double a = 0;
    for (int h = 1; h <= hm; ++h) {
      double amp = 1.0 / h;
      if (mix_end > 0 && (h & 1) == 0) amp *= 1.0 - m;
      a += amp * sin(2 * kPi * h * ph);
    }
    x[i] = (float)(0.3 * a);
  }
  return x;
}
// the cycle a table frame should be: the same saw as 2048 samples with hm harmonics
static std::vector<float> saw_cycle(int hm) {
  std::vector<float> c(2048);
  for (int i = 0; i < 2048; ++i) {
    double a = 0;
    for (int h = 1; h <= hm; ++h) a += sin(2 * kPi * h * i / 2048.0) / h;
    c[i] = (float)a;
  }
  return c;
}
// the relative error of frame f against ref, both at unit peak, the best of a few samples' shift
static double frame_err(const float* fr, const std::vector<float>& ref, int maxshift = 3) {
  double pf = 0, pr = 0;
  for (int i = 0; i < 2048; ++i) pf = fmax(pf, fabs(fr[i])), pr = fmax(pr, fabs(ref[i]));
  double best = 1e9;
  for (int s = -maxshift; s <= maxshift; ++s) {
    double e = 0, en = 0;
    for (int i = 0; i < 2048; ++i) {
      double d = fr[i] / pf - ref[(i + s + 2048) & 2047] / pr;
      e += d * d, en += (ref[i] / pr) * (ref[i] / pr);
    }
    best = fmin(best, sqrt(e / en));
  }
  return best;
}
// the phase of the fundamental (radians) of a frame
static double fund_phase(const float* fr) {
  double re = 0, im = 0;
  for (int i = 0; i < 2048; ++i) re += fr[i] * cos(2 * kPi * i / 2048.0), im -= fr[i] * sin(2 * kPi * i / 2048.0);
  return atan2(im, re);
}
static bool finite(const std::vector<float>& t) {
  for (float v : t)
    if (!std::isfinite(v)) return false;
  return true;
}

int main() {
  // 1. a saw at three pitches and three rates: every frame is the saw's cycle
  {
    const double fs[3] = {110.0, 220.0, 437.3};
    const uint32_t rates[3] = {44100, 48000, 96000};
    for (double f : fs)
      for (uint32_t rate : rates) {
        std::vector<float> x = additive(f, 1.0, rate);
        std::vector<float> t(64 * 2048);
        Method m = kNone;
        float per = 0;
        CHECK(make_table(x.data(), 1, (uint32_t)x.size(), rate, 64, t.data(), &m, &per) == 0);
        CHECK(m == kPitched);
        CHECK(fabs(per - rate / f) < 0.02 * rate / f);
        int hm = (int)ceil(per / 2 - 1e-6) - 1;
        if (hm > 512) hm = 512;
        std::vector<float> ref = saw_cycle(hm);
        double worst = 0;
        for (int fr = 0; fr < 64; ++fr) worst = fmax(worst, frame_err(&t[fr * 2048], ref));
        printf("saw %.1f Hz @ %u: worst frame error %.4f (period %.2f, %d harmonics)\n", f, rate, worst, per, hm);
        CHECK(worst < 0.05);
        CHECK(finite(t));
      }
  }
  // 2. vibrato +-50 cents: the frames stay aligned (the fundamental's phase moves little from frame to frame)
  {
    std::vector<float> x = additive(220.0, 2.0, 48000, 50.0);
    std::vector<float> t(64 * 2048);
    Method m = kNone;
    CHECK(make_table(x.data(), 1, (uint32_t)x.size(), 48000, 64, t.data(), &m) == 0);
    CHECK(m == kPitched);
    double worst = 0;
    for (int fr = 1; fr < 64; ++fr) {
      double d = fund_phase(&t[fr * 2048]) - fund_phase(&t[(fr - 1) * 2048]);
      d = fabs(remainder(d, 2 * kPi));
      worst = fmax(worst, d);
    }
    printf("vibrato: worst fundamental phase step %.4f rad\n", worst);
    CHECK(worst < 0.15);
    std::vector<float> ref = saw_cycle(100);
    double w2 = 0;
    for (int fr = 0; fr < 64; ++fr) w2 = fmax(w2, frame_err(&t[fr * 2048], ref, 8));
    printf("vibrato: worst frame error against the saw %.4f\n", w2);
    CHECK(w2 < 0.3);
  }
  // 3. a timbre sweep, saw to square over 2 s: the morph's steps are no larger than the sweep's own
  {
    std::vector<float> x = additive(220.0, 2.0, 48000, 0, 5.5, 1, 1);
    std::vector<float> t(64 * 2048);
    CHECK(make_table(x.data(), 1, (uint32_t)x.size(), 48000, 64, t.data()) == 0);
    double worst = 0, tot = 0;
    for (int fr = 1; fr < 64; ++fr) {
      double d = 0, e = 0;
      for (int i = 0; i < 2048; ++i) {
        double a = t[fr * 2048 + i] - t[(fr - 1) * 2048 + i];
        d += a * a, e += (double)t[fr * 2048 + i] * t[fr * 2048 + i];
      }
      worst = fmax(worst, sqrt(d / (e + 1e-12)));
      tot += sqrt(d / (e + 1e-12));
    }
    // the sweep's own step: the first frame against the last, spread over the 63 steps
    double d = 0, e = 0;
    for (int i = 0; i < 2048; ++i) {
      double a = t[63 * 2048 + i] - t[i];
      d += a * a, e += (double)t[63 * 2048 + i] * t[63 * 2048 + i];
    }
    double own = sqrt(d / e) / 63.0;
    printf("sweep: worst step %.4f, mean %.4f, the sweep's own %.4f\n", worst, tot / 63, own);
    CHECK(worst < 4 * own + 0.02);
  }
  // 4. stereo summed: the same table as the mono one
  {
    std::vector<float> x = additive(220.0, 1.0, 48000);
    std::vector<float> st(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) st[2 * i] = x[i], st[2 * i + 1] = x[i];
    std::vector<float> a(32 * 2048), b(32 * 2048);
    CHECK(make_table(x.data(), 1, (uint32_t)x.size(), 48000, 32, a.data()) == 0);
    CHECK(make_table(st.data(), 2, (uint32_t)x.size(), 48000, 32, b.data()) == 0);
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d = fmax(d, fabs(a[i] - b[i]));
    CHECK(d < 1e-4);
  }
  // 5. a short selection (5 periods, 64 frames): the frames repeat the cycle
  {
    std::vector<float> x = additive(110.0, 5.0 / 110.0, 48000);
    std::vector<float> t(64 * 2048);
    Method m = kNone;
    CHECK(make_table(x.data(), 1, (uint32_t)x.size(), 48000, 64, t.data(), &m) == 0);
    CHECK(m == kPitched);
    std::vector<float> ref = saw_cycle(217);
    double worst = 0;
    for (int fr = 0; fr < 64; ++fr) worst = fmax(worst, frame_err(&t[fr * 2048], ref, 4));
    printf("short selection (5 periods): worst frame error %.4f\n", worst);
    CHECK(worst < 0.15);
    CHECK(finite(t));
  }
  // 6. silence is refused; noise takes the spectral way and is finite; a very short selection too
  {
    std::vector<float> z(48000, 0.0f);
    std::vector<float> t(16 * 2048);
    CHECK(make_table(z.data(), 1, (uint32_t)z.size(), 48000, 16, t.data()) == kErrSilent);
    CHECK(make_table(z.data(), 1, 10, 48000, 16, t.data()) == kErrShort);
    std::vector<float> nz(48000);
    uint32_t r = 12345;
    for (auto& v : nz) r = r * 1664525u + 1013904223u, v = ((r >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    Method m = kNone;
    CHECK(make_table(nz.data(), 1, (uint32_t)nz.size(), 48000, 16, t.data(), &m) == 0);
    CHECK(m == kSpectral && finite(t));
    float pk = 0;
    for (float v : t) pk = fmaxf(pk, fabsf(v));
    CHECK(fabsf(pk - 0.95f) < 1e-3f);
    // a short noise burst (1000 frames): four fifths of it as the cycle, resampled
    CHECK(make_table(nz.data(), 1, 1000, 48000, 16, t.data(), &m) == 0);
    CHECK(m == kSpectral && finite(t));
    // a short hit of silence then sound: no NaN either way
    std::vector<float> tail(48000, 0.0f);
    for (int i = 40000; i < 48000; ++i) tail[i] = nz[i];
    CHECK(make_table(tail.data(), 1, 48000, 48000, 16, t.data(), &m) == 0 && finite(t));
  }
  // 7. a decaying pluck-like tone with a long silent tail keeps the frame levels apart and stays finite
  {
    std::vector<float> x = additive(196.0, 1.0, 48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] *= (float)exp(-4.0 * i / 48000.0);
    std::vector<float> t(32 * 2048);
    Method m = kNone;
    CHECK(make_table(x.data(), 1, (uint32_t)x.size(), 48000, 32, t.data(), &m) == 0 && m == kPitched && finite(t));
    float p0 = 0, p31 = 0;
    for (int i = 0; i < 2048; ++i) p0 = fmaxf(p0, fabsf(t[i])), p31 = fmaxf(p31, fabsf(t[31 * 2048 + i]));
    printf("decay: first frame peak %.3f, last %.3f\n", p0, p31);
    CHECK(p0 > 4 * p31);
  }
  // 8. the stepper is bounded and reports progress
  {
    std::vector<float> x = additive(55.0, 3.0, 96000);
    std::vector<float> t(256 * 2048);
    Maker mk;
    CHECK(mk.begin(x.data(), 1, (uint32_t)x.size(), 96000, 256, t.data()) == 0);
    int steps = 0, last = -1;
    while (mk.step() > 0) {
      ++steps;
      int p = mk.progress();
      CHECK(p >= last);
      last = p;
    }
    printf("55 Hz @ 96 kHz, 256 frames: %d steps\n", steps);
    CHECK(mk.method() == kPitched && mk.progress() == 100 && steps > 256);
  }
  // 9. the WAV a table is written as reads back: float32, mono, `clm ` 2048, N frames (the Saver's header)
  {
    std::vector<uint8_t> w(256);
    int hl = table_wav_header(w.data(), 48000, 64 * 2048);
    std::vector<float> body(64 * 2048);
    for (size_t i = 0; i < body.size(); ++i) body[i] = (float)sin(i * 0.01);
    w.resize(hl + body.size() * 4);
    memcpy(&w[hl], body.data(), body.size() * 4);
    wavread::Info in;
    CHECK(wavread::parse(w.data(), (uint32_t)w.size(), &in));
    CHECK(in.format == 3 && in.ch == 1 && in.bits == 32 && in.frames == 64u * 2048 && in.clm_frame == 2048);
    CHECK(wt::detect_frame_size(in.frames, in.clm_frame) == 2048);
    CHECK(wt::frame_count(in.frames, 2048) == 64);
    CHECK(fabsf(wavread::sample(in, &w[in.data_off + 4 * 100]) - body[100]) < 1e-7f);
  }
  printf("test_wt_make OK\n");
  return 0;
}
