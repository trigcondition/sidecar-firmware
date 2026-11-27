// core/tests/demo/render_wavegrain.cpp - WAVETABLE's and GRANULAR's demo renders (make -C core demo-wavegrain
// OUT=dir): a chord progression, then a sweep of each main knob, through the engine's own path (host_sysex reports,
// render_voices with its soft limiter), voice 0 as the Overbridge return gets it. Writes OUT/wavetable_demo.wav and
// OUT/granular_demo.wav (48 kHz mono 16-bit); afconvert -f m4af -d aac turns them into m4a.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "granular/granular.h"
#include "poly.h"
#include "notes.h"
#include "voices.h"
#include "wavetable/wavetable.h"
#include "wavetable/wt_make.h"
#include "wg_util.h"
#include "pool_util.h"
#include "lib/pools.h"
#include "lib/srclib.h"

using namespace core;

namespace {
float kSawPosV() { return 84 + 85 / 128.0f; }   // BASIC's saw frame
struct Ev {
  double t;
  std::function<void()> f;
};
std::vector<Ev> evs;
void at(double t, std::function<void()> f) { evs.push_back({t, std::move(f)}); }

// a knob ramp: from a to b over [t0, t1], a 0x03 every 10 ms with p[k] set
void sweep(double t0, double t1, int flag, std::vector<float> p, int k, float a, float b) {
  for (double t = t0; t <= t1 + 1e-9; t += 0.01) {
    float x = a + (b - a) * (float)((t - t0) / (t1 - t0));
    p[k] = x;
    at(t, [flag, p] { wg::params(flag, 0, p); });
  }
}

// a drum loop of our own, as if the Rytm's AUDIO IN carried it (GRANULAR's live demo): kick, snare, hats, 120 BPM
float drum(uint64_t i) {
  double t = i / 48000.0, beat = std::fmod(t, 0.5), bar = std::fmod(t, 2.0);
  double y = 0;
  y += 0.8 * std::exp(-beat * 18) * std::sin(2 * M_PI * (45 + 120 * std::exp(-beat * 40)) * beat);   // kick each beat
  double sn = std::fmod(bar + 1.5, 1.0);   // snare on 2 and 4
  uint32_t r = (uint32_t)i * 2654435761u;
  double noise = (double)(r >> 8) / 16777216.0 - 0.5;
  y += 0.5 * std::exp(-sn * 25) * (noise + 0.4 * std::sin(2 * M_PI * 190 * sn));
  double hh = std::fmod(t, 0.125);
  y += 0.15 * std::exp(-hh * 120) * noise;
  return (float)y;
}

std::vector<float> run(double secs, bool live) {
  std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
  static int16_t ring[kBlock][kVoices];
  uint64_t mach[kMachines] = {0};
  std::vector<float> out;
  size_t e = 0;
  uint64_t frame = 0;
  int32_t in[kBlock * 12];
  for (double t = 0; t < secs; t += kBlock / 48000.0) {
    while (e < evs.size() && evs[e].t <= t) evs[e++].f();
    for (int i = 0; i < 4000 && (wt::step() || gr::step()); ++i) {}
    if (live) {
      for (int i = 0; i < kBlock; ++i) {
        int32_t x = (int32_t)(drum(frame + i) * 2.0e9);
        for (int c = 0; c < 12; ++c) in[i * 12 + c] = c >= 10 ? x : 0;
      }
      gr::live_in(in, kBlock, 12);
    }
    frame += kBlock;
    render_voices(ring, kBlock, 0, 0, 1, mach);
    for (int s = 0; s < kBlock; ++s) out.push_back(ring[s][0] / 32768.0f);
  }
  evs.clear();
  return out;
}

// the chords: Am7, Fmaj7, Cadd9, Gsus4 (root, CHD), each 2 s
const int kRoot[4] = {57, 53, 48, 55}, kChd[4] = {7, 6, 13, 4};
// a chord on voice 0 as a poly track plays it (WAVETABLE has no chord words: its notes one by one, live notes on a
// channel of their own), the sound p reported first
constexpr int kChordCh = 3;
void chord_on(double t, int flag, const std::vector<float>& p, int root, int chd, int inv) {
  at(t, [=] {
    wg::params(flag, 0, p);
    uint8_t ns[poly::kMaxNotes];
    int n = poly::expand(root, {chd, inv, 0, 0}, ns);
    for (int i = 0; i < n; ++i) wt::live_on(0, ns[i], 100, kChordCh, ns[i]);
  });
}
void chord_off(double t) {
  at(t, [] { wt::live_off(0, kChordCh, -1); });
}

void wavetable_demo(const std::string& dir) {
  const int F = kWaveMachine;
  // P1..P16: TBL POS WARP AMT UNI SUB CUT RES ATK DEC SUS ENV CHD INV SPR VOI
  double t = 0;
  // 1. a pad progression on BASIC: unison, a slow POS drift, a soft low-pass
  for (int c = 0; c < 4; ++c, t += 2) {
    std::vector<float> p = {1, 30, 0, 0, 100, 30, 46, 20, 50, 90, 110, 80, 0, 0, 0, 0};
    chord_on(t, F, p, kRoot[c], kChd[c], 1);
    sweep(t + 0.02, t + 1.9, F, p, 1, 30.0f + 10 * c, 60.0f + 10 * c);
    chord_off(t + 1.95);
  }
  // 2. POS across each built-in table, a held Am7 (VOCAL, ORGAN, FM, SYNC, PWM)
  const int tbls[5] = {3, 4, 5, 6, 2};   // VOCAL ORGAN FM SYNC PWM (A003..A006, A002)
  for (int k = 0; k < 5; ++k, t += 3) {
    std::vector<float> p = {(float)tbls[k], 0, 0, 0, 60, 0, 64, 0, 5, 100, 120, 64, 0, 0, 0, 0};
    chord_on(t, F, p, 57, 7, 0);
    sweep(t + 0.02, t + 2.8, F, p, 1, 0, 127);
    chord_off(t + 2.9);
  }
  // 3. the warps' amount on a saw arpeggio: SYNC, PWM, FM (3 s each)
  for (int w = 1; w <= 3; ++w, t += 3) {
    std::vector<float> p = {1, kSawPosV(), (float)((w * 128 + 64) / 9), 0, 30, 20, 56, 30, 0, 80, 80, 64, 0, 0, 0, 0};
    const int arp[8] = {45, 52, 57, 60, 64, 60, 57, 52};
    for (int n = 0; n < 12; ++n) {
      at(t + n * 0.25, [=] {
        std::vector<float> q = p;
        q[3] = 127.0f * n / 11;
        wg::trig(F, 0, arp[n % 8], 110, q);
      });
      at(t + n * 0.25 + 0.2, [] { wg::off(0); });
    }
  }
  // 4. UNI 0 -> 127 on a held Fmaj7, then CUT from closed through open to high-pass with resonance
  {
    std::vector<float> p = {1, 84, 0, 0, 0, 0, 64, 0, 10, 100, 120, 64, 0, 0, 0, 0};
    chord_on(t, F, p, 53, 6, 1);
    sweep(t + 0.02, t + 3.0, F, p, 4, 0, 127);
    p[4] = 127;
    p[7] = 90;
    sweep(t + 3.02, t + 7.0, F, p, 6, 10, 120);
    chord_off(t + 7.1);
    t += 7.5;
  }
  // 5. plucks: SUS 0, short DEC, the mod envelope opening POS on VOCAL
  {
    const int mel[16] = {57, 60, 64, 67, 69, 67, 64, 60, 53, 57, 60, 64, 65, 64, 60, 57};
    for (int n = 0; n < 16; ++n) {
      std::vector<float> p = {3, 10, 0, 0, 50, 40, 50, 40, 0, 72, 0, 110, 0, 0, 0, 0};
      at(t + n * 0.25, [=] { wg::trig(F, 0, mel[n], 100 + (n % 4 == 0 ? 20 : 0), p); });
      at(t + n * 0.25 + 0.1, [] { wg::off(0); });
    }
    t += 4.5;
  }
  std::vector<float> y = run(t + 1.5, false);
  wg::write_wav16(dir + "/wavetable_demo.wav", y);
  printf("wavetable_demo.wav: %.1f s, rms %.3f\n", y.size() / 48000.0, wg::rms(y));
}

// Three phrases through WAVETABLE's new patches, one voice, each scaled to the same loudness (rms -20 dBFS, less where the
// peak would pass -0.1 dBFS): chords (Am7 Fmaj7 C G, a bar each at 100 BPM, the notes of a chord together), bass (a
// two-bar line twice, a note each step), pad (one chord held 8 s with POS swept 0 > 127 > 0 while it is held, then
// released). Files: wt-warps-<patch>-<phrase>.wav.
//   plain    A only: BASIC, no warp, one oscillator, LP12
//   mix      A + B: BASIC and SYNC (the table) half and half, two unison pairs, LP12
//   bend / squeeze / formant   A only, the warp's AMT mid-way, LP12
//   ladder   the ladder LP, RES 90, CUT swept 30 > 110 > 30 over the phrase
//   lp24     LP24, RES 60, the same sweep
const double kBeat = 0.6;
float tri(double t, double len, float a, float top) {
  double x = std::min(std::max(t / len, 0.0), 1.0);
  return a + (top - a) * (float)(1 - std::fabs(2 * x - 1));
}
struct Patch { const char* name; std::vector<float> p; bool sweep; float fenv; };
struct Phrase { const char* name; double secs; };
void warps_demo(const std::string& dir) {
  const int F = kWaveMachine;
  auto wz = [](int z) { return (float)((z * 128 + 64) / 9); };
  auto fz = [](int z) { return (float)(z * 16 + 8); };
  // P1..P16: TBL POS WARP AMT UNI SUB CUT RES ATK DEC SUS ENV TBLB POSB MIX FTYPE
  const std::vector<Patch> patches = {
      {"plain", {1, 70, 0, 0, 0, 0, 90, 30, 0, 0, 0, 64, 0, 0, 0, fz(1)}, false, 0.25f},
      {"mix", {1, 70, 0, 0, 40, 0, 90, 30, 0, 0, 0, 64, 6, 70, 64, fz(1)}, false, 0.0f},
      {"bend", {1, 70, wz(6), 30, 0, 0, 95, 30, 0, 0, 0, 64, 0, 0, 0, fz(1)}, false, 0.0f},
      {"squeeze", {1, 70, wz(7), 40, 0, 0, 95, 30, 0, 0, 0, 64, 0, 0, 0, fz(1)}, false, 0.0f},
      {"formant", {1, 70, wz(4), 70, 0, 0, 95, 30, 0, 0, 0, 64, 0, 0, 0, fz(1)}, false, 0.0f},
      {"ladder", {1, 70, 0, 0, 20, 0, 60, 90, 0, 0, 0, 64, 0, 0, 0, fz(7)}, true, 0.0f},
      {"lp24", {1, 70, 0, 0, 20, 0, 60, 60, 0, 0, 0, 64, 0, 0, 0, fz(2)}, true, 0.0f},
  };
  const int chords[4][4] = {{57, 60, 64, 67}, {53, 57, 60, 64}, {48, 55, 60, 64}, {55, 59, 62, 67}};
  const int bass[16] = {33, 33, 45, 33, 40, 33, 43, 45, 36, 36, 48, 36, 43, 36, 31, 35};
  const int pad[4] = {48, 55, 60, 64};
  const Phrase phrases[3] = {{"chords", 4 * 4 * kBeat + 1.5}, {"bass", 32 * kBeat / 2 + 1.0}, {"pad", 11.5}};
  for (const Patch& pt : patches)
    for (int ph = 0; ph < 3; ++ph) {
      voices_init();
      wt::reset();
      const double secs = phrases[ph].secs;
      auto knobs = [&](double t) {   // the patch with the phrase's envelope and its sweeps at t
        std::vector<float> p = pt.p;
        if (ph == 0) p[8] = 30, p[9] = 80, p[10] = 100;
        else if (ph == 1) p[8] = 10, p[9] = 70, p[10] = 70;
        else p[8] = 85, p[9] = 95, p[10] = 115;
        if (ph == 2) p[1] = tri(t, 8.0, 0, 127);   // POS 0 > 127 > 0 while it is held
        if (pt.sweep) p[6] = tri(t, secs - 1.0, 30, 110);
        return p;
      };
      at(-0.02, [=] { wt::set_fenv(0, pt.fenv); });
      at(-0.01, [=] { wg::params(F, 0, knobs(0)); });
      for (double t = 0; t < secs; t += 0.01) at(t, [=] { wg::params(F, 0, knobs(t)); });
      if (ph == 0) {
        const double bar = 4 * kBeat;
        for (int c = 0; c < 4; ++c) {
          at(c * bar, [=] { for (int n : chords[c]) wt::live_on(0, n, 100, kChordCh, n); });
          at(c * bar + bar * 0.92, [] { wt::live_off(0, kChordCh, -1); });
        }
      } else if (ph == 1) {
        const double step = kBeat / 2;
        for (int i = 0; i < 32; ++i) {
          at(i * step, [=] { wg::trig(F, 0, bass[i % 16], i % 4 == 0 ? 127 : 95, knobs(i * step)); });
          at(i * step + step * 0.7, [] { wg::off(0); });
        }
      } else {
        at(0.0, [=] { for (int n : pad) wt::live_on(0, n, 100, kChordCh, n); });
        at(8.0, [] { wt::live_off(0, kChordCh, -1); });
      }
      std::vector<float> y = run(secs, false);
      double r = wg::rms(y), pk = 0;
      for (float x : y) pk = std::max(pk, (double)std::fabs(x));
      float g = (float)std::min(0.1 / std::max(r, 1e-9), 0.989 / std::max(pk, 1e-9));
      for (float& x : y) x *= g;
      std::string path = dir + "/wt-warps-" + pt.name + "-" + phrases[ph].name + ".wav";
      wg::write_wav16(path, y);
      printf("%s: %.1f s, raw rms %.3f peak %.3f, written x %.2f (rms %.1f dBFS)\n", path.c_str(), y.size() / 48000.0, r, pk, g,
             20 * std::log10(r * g));
    }
}

// ---- tables made from samples (MAKE WT, core/wavetable/wt_make.h): three sources synthesised here, each converted with
// the editor's converter (64 frames) into /WAVETABLES on the demo card, then played by WAVETABLE with POS swept 0 > 127 > 0
// over 8 s on a held note, each scaled to the same loudness. Files in DIR: fromsample-<name>-source.wav (the sample),
// fromsample-<name>-table.wav (the table: float32, `clm ` 2048, as MAKE WT writes it), fromsample-<name>-sweep.wav.
struct Source { const char* name; std::vector<float> x; int note; };
std::vector<Source> g_src;
std::vector<float> make_vowel() {   // a sung-like tone: 196 Hz, vibrato +-35 cents at 5.5 Hz, the vowel gliding /a/ -> /o/ over 3 s
  const double secs = 3.0;
  std::vector<float> x((size_t)(secs * 48000));
  double ph = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    double t = (double)i / 48000.0, u = t / secs;
    double f = 196.0 * std::pow(2.0, 35.0 / 1200.0 * std::sin(2 * M_PI * 5.5 * t));
    ph += f / 48000.0;
    const double f1 = 800 + (450 - 800) * u, f2 = 1200 + (800 - 1200) * u, f3 = 2500;
    double a = 0;
    for (int h = 1; h * f < 20000 && h < 200; ++h) {
      double hz = h * f, g = 1.0 / std::pow(h, 1.1);
      double res = 1.0 * std::exp(-std::pow(std::log(hz / f1) / 0.22, 2)) + 0.5 * std::exp(-std::pow(std::log(hz / f2) / 0.2, 2)) +
                   0.25 * std::exp(-std::pow(std::log(hz / f3) / 0.2, 2)) + 0.03;
      a += g * res * std::sin(2 * M_PI * h * ph);
    }
    double env = std::min(1.0, t / 0.05) * std::min(1.0, (secs - t) / 0.15);
    x[i] = (float)(0.5 * a * env);
  }
  return x;
}
std::vector<float> make_pluck() {   // a Karplus-Strong string, D3 (146.8 Hz: a 327-sample loop), 2.5 s
  std::vector<float> x((size_t)(2.5 * 48000));
  const int L = 327;
  std::vector<float> d(L);
  uint32_t r = 7;
  for (int i = 0; i < L; ++i) r = r * 1664525u + 1013904223u, d[i] = ((r >> 8) & 0xFFFF) / 32768.0f - 1.0f;
  float prev = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    int k = (int)(i % L);
    float y = d[k];
    x[i] = y * 0.7f;
    d[k] = 0.4975f * (y + prev);   // the loop's low-pass and its loss
    prev = y;
  }
  return x;
}
std::vector<float> make_noise() {   // a noise burst through a resonant low-pass sweeping 6 kHz > 300 Hz, 0.6 s
  std::vector<float> x((size_t)(0.6 * 48000));
  uint32_t r = 99;
  double lo = 0, bp = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    double u = (double)i / x.size(), fc = 6000.0 * std::pow(0.05, u);
    double F = 2 * std::sin(M_PI * fc / 48000.0), q = 0.25;
    r = r * 1664525u + 1013904223u;
    double in = ((r >> 8) & 0xFFFF) / 32768.0 - 1.0;
    lo += F * bp;
    double hp = in - lo - q * bp;
    bp += F * hp;
    x[i] = (float)(0.8 * lo * std::exp(-4.0 * u));
  }
  return x;
}
// writes the sources and their tables (card: the demo card's /WAVETABLES, dir: the output); the converter's report printed
void fromsample_make(const std::string& dir, const std::string& card) {
  system(("mkdir -p '" + card + "/WAVETABLES'").c_str());
  g_src = {{"vowel", make_vowel(), 55}, {"pluck", make_pluck(), 50}, {"noise", make_noise(), 48}};
  for (const Source& s : g_src) {
    std::vector<float> t((size_t)64 * wt::make::kFrameLen);
    wt::make::Method m = wt::make::kNone;
    float per = 0;
    int r = wt::make::make_table(s.x.data(), 1, (uint32_t)s.x.size(), 48000, 64, t.data(), &m, &per);
    CHECK(r == 0);
    std::vector<uint8_t> w(wt::make::kTableHeaderMax);
    int hl = wt::make::table_wav_header(w.data(), 48000, (uint32_t)t.size());
    w.resize((size_t)hl + t.size() * 4);
    memcpy(&w[hl], t.data(), t.size() * 4);
    wg::write_file(dir + "/fromsample-" + s.name + "-table.wav", w);
    wg::write_file(card + "/WAVETABLES/fromsample-" + s.name + ".wav", w);
    wg::write_wav16(dir + "/fromsample-" + s.name + "-source.wav", s.x);
    printf("%s: %.2f s of source -> 64 frames, %s%s%.1f Hz\n", s.name, s.x.size() / 48000.0, m == wt::make::kPitched ? "PITCHED " : "SPECTRAL ",
           m == wt::make::kPitched ? "at " : "(no pitch) ", per > 0 ? 48000.0f / per : 0.0f);
  }
}
void fromsample_demo(const std::string& dir) {
  const int F = kWaveMachine;
  const char* rows[3] = {"A007", "A008", "A009"};
  for (int k = 0; k < 3; ++k) {
    CHECK(pu::put(pools::kTables, rows[k], ("/WAVETABLES/fromsample-" + std::string(g_src[k].name) + ".wav").c_str()) == pools::kOk);
  }
  const double secs = 9.0;
  for (int k = 0; k < 3; ++k) {
    voices_init();
    wt::reset();
    auto knobs = [&](double t) {   // P1..P16: TBL POS WARP AMT UNI SUB CUT RES ATK DEC SUS ENV TBLB POSB MIX FTYPE
      std::vector<float> p = {(float)(7 + k), tri(t, 8.0, 0, 127), 0, 0, 20, 0, 90, 20, 30, 110, 120, 64, 0, 0, 0, 0};
      return p;
    };
    at(-0.01, [=] { wg::params(F, 0, knobs(0)); });
    for (double t = 0; t < secs; t += 0.01) at(t, [=] { wg::params(F, 0, knobs(t)); });
    at(0.0, [=] { wg::trig(F, 0, g_src[k].note, 100, knobs(0)); });
    at(8.2, [] { wg::off(0); });
    std::vector<float> y = run(secs, false);
    double r = wg::rms(y), pk = 0;
    for (float x : y) pk = std::max(pk, (double)std::fabs(x));
    float g = (float)std::min(0.1 / std::max(r, 1e-9), 0.989 / std::max(pk, 1e-9));
    for (float& x : y) x *= g;
    std::string path = dir + "/fromsample-" + g_src[k].name + "-sweep.wav";
    wg::write_wav16(path, y);
    printf("%s: %.1f s, raw rms %.3f peak %.3f, written x %.2f (rms %.1f dBFS)\n", path.c_str(), y.size() / 48000.0, r, pk, g,
           20 * std::log10(r * g));
  }
}

void granular_demo(const std::string& dir) {
  const int F = kGranMachine;
  auto sz = [](double s) { return (float)(127 * std::log(s / 0.005) / std::log(200.0)); };
  auto dn = [](double d) { return (float)(127 * std::log(d) / std::log(200.0)); };
  // P1..P16: SRC POS SPRY SIZE DENS PRND REV WIN ATK REL HOLD ANIM CHD INV SPR VOI
  double t = 0;
  // 1. the PAD source as a cloud, the progression
  for (int c = 0; c < 4; ++c, t += 2) {
    std::vector<float> p = {10, 20, 40, sz(0.15), dn(40), 6, 10, 0, 50, 75, 0, 0, (float)kChd[c], 0, 1, 0};
    p[0] = 10;
    at(t, [=] { wg::trig(F, 0, kRoot[c] + 12, 100, p); });
    at(t + 1.9, [] { wg::off(0); });
  }
  // 2. the knobs one by one on a held A: SIZE, DENS, SPRY, PRND, REV, WIN
  {
    std::vector<float> p = {10, 30, 10, sz(0.08), dn(30), 0, 0, 0, 20, 80, 0, 0, 0, 0, 0, 0};
    at(t, [=] { wg::trig(F, 0, 69, 100, p); });
    sweep(t + 0.02, t + 4, F, p, 3, 0, 127);   // SIZE 5 ms -> 1 s
    p[3] = sz(0.08);
    sweep(t + 4.02, t + 8, F, p, 4, 0, 127);   // DENS 1 -> 200
    p[4] = dn(40);
    sweep(t + 8.02, t + 11, F, p, 2, 0, 127);   // SPRY
    p[2] = 20;
    sweep(t + 11.02, t + 14, F, p, 5, 0, 127);   // PRND
    p[5] = 0;
    sweep(t + 14.02, t + 17, F, p, 6, 0, 127);   // REV
    p[6] = 0;
    for (int w = 0; w < 4; ++w) {   // WIN: HANN TRI TUKEY PERC
      std::vector<float> q = p;
      q[7] = (float)(w * 32 + 8);
      q[3] = sz(0.12);
      at(t + 17 + w, [=] { wg::params(F, 0, q); });
    }
    at(t + 21, [] { wg::off(0); });
    t += 21.5;
  }
  // 3. BELLS frozen at a place, a spray, a minor chord
  {
    std::vector<float> p = {11, 40, 30, sz(0.06), dn(70), 0, 30, 112, 0, 80, 127, 0, 2, 0, 1, 0};
    at(t, [=] { wg::trig(F, 0, 64, 100, p); });
    sweep(t + 0.02, t + 3.8, F, p, 1, 40, 100);
    at(t + 4, [] { wg::off(0); });
    t += 4.5;
  }
  // 3b. ANIM on the PAD cloud: off, then deeper and faster (1BAR, then 1/4)
  {
    std::vector<float> p = {10, 40, 10, sz(0.1), dn(40), 0, 0, 0, 30, 80, 60, 0, 0, 0, 0, 0};
    at(t, [=] { wg::trig(F, 0, 57, 100, p); });
    at(t + 2, [=] { std::vector<float> q = p; q[11] = 56; wg::params(F, 0, q); });
    at(t + 6, [=] { std::vector<float> q = p; q[11] = 90; wg::params(F, 0, q); });
    at(t + 9, [] { wg::off(0); });
    t += 9.5;
  }
  std::vector<float> a = run(t + 1.0, false);
  // 4. LIVE: the drum loop on AUDIO IN, granulated: POS back in time, freeze, feedback, pitched up a fifth
  t = 0;
  {
    std::vector<float> p = {0, 10, 5, sz(0.05), dn(60), 0, 0, 0, 0, 60, 0, 0, 0, 0, 0, 0};
    at(0.0, [=] { wg::trig(F, 0, 60, 120, p); });
    sweep(2.0, 5.0, F, p, 1, 10, 100);   // POS: further back
    p[1] = 60;
    at(5.0, [=] { std::vector<float> q = p; q[10] = 127; wg::params(F, 0, q); });   // frozen
    at(6.5, [=] { std::vector<float> q = p; q[10] = 127; q[3] = sz(0.2); q[2] = 60; wg::params(F, 0, q); });
    at(8.0, [=] { std::vector<float> q = p; wg::params(F, 0, q); });   // thawed
    sweep(8.02, 11.0, F, p, 10, 0, 110);   // HOLD below FREEZE: feedback
    p[10] = 60;
    at(11.0, [=] { wg::trig(F, 0, 67, 120, p); });   // a fifth up
    at(13.0, [] { wg::off(0); });
  }
  std::vector<float> b = run(14.0, true);
  a.insert(a.end(), b.begin(), b.end());
  wg::write_wav16(dir + "/granular_demo.wav", a);
  printf("granular_demo.wav: %.1f s, rms %.3f\n", a.size() / 48000.0, wg::rms(a));
}
// The Pi's boot bench on this Mac (pi/engine/bridge.cpp bench: voice 0 on the machine with notes.cpp's defaults, note
// 48, a retrigger on 55 halfway, 4000 blocks = 1 s of audio): % of a core per voice, and the Pi 4's estimate from the
// ratio of the bench's measured Pi numbers (docs/pi-engine.md "Boot bench") to this Mac's for the same machines.
void bench() {
  NoteAdapter na([](const uint8_t* d, int n) { host_sysex(d, n); });
  const char* m[] = {"plaits", "juno", "fm", "daisy", "dd", "wt", "gr"};
  const double pi4[] = {0.73, 0.54, 1.22, 0.97, 0.23, 0, 0};   // measured on the Pi 4 at 1500 MHz
  double pct[7], ratio = 0;
  int nr = 0;
  static int16_t blk[kBlock][kVoices];
  for (int k = 0; k < 7; ++k) {
    voices_init();
    na.SetMachine(0, m[k]);
    na.Channel(0x90, 48, 100);
    uint64_t mach[kMachines] = {0}, sum = 0;
    for (int b = 0; b < 4000; ++b) {
      for (int i = 0; i < 100 && (wt::step() || gr::step()); ++i) {}
      sum += render_voices(blk, kBlock, 0, 0, 1, mach);
      if (b == 2000) na.Channel(0x90, 55, 100);
    }
    na.Channel(0x80, 55, 0);
    wt::silence(0), gr::silence(0);
    pct[k] = sum / 1e9 * 100;   // the Mac's cycles() are nanoseconds; 1 s of audio
    if (pi4[k] > 0) ratio += pi4[k] / pct[k], ++nr;
  }
  ratio /= nr;
  printf("bench (this Mac; Pi 4 at 1500 MHz = %.1f x the Mac, from plaits juno fm daisy dd):\n", ratio);
  for (int k = 0; k < 7; ++k)
    printf("  %-6s %.3f %% of a core per voice on the Mac -> Pi 4 %s%.2f %% (%d voices a core at 80 %%)\n", m[k], pct[k],
           pi4[k] > 0 ? "measured " : "estimate ", pi4[k] > 0 ? pi4[k] : pct[k] * ratio,
           (int)(80 / (pi4[k] > 0 ? pi4[k] : pct[k] * ratio)));
}
}  // namespace

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  // an empty card: the project's pools from their template (WAVETABLES A001..A006 the built-ins), as the engine boots
  const std::string card = "/tmp/wg-demo-empty-card";
  system(("mkdir -p '" + card + "'").c_str());
  const bool fromsample = dir == "--fromsample";   // --fromsample DIR: tables made from synthesised samples (MAKE WT)
  if (fromsample) {
    dir = argc > 2 ? argv[2] : ".";
    system(("rm -rf '" + card + "/WAVETABLES'").c_str());
    fromsample_make(dir, card);
  }
  host_set_card(card.c_str());
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, 1000000);
  voices_init();
  card_ok = true;
  srclib::begin(true);
  pools::begin(true);
  if (dir == "--bench") {
    double t0 = wg::now_s();
    wt::begin(true);
    double t1 = wg::now_s();
    gr::begin(true);
    pu::steps();
    printf("begin: WAVETABLE's built-ins %.1f ms, GRANULAR's %.1f ms (Mac)\n", (t1 - t0) * 1e3, (wg::now_s() - t1) * 1e3);
    bench();
    return 0;
  }
  wt::begin(true);
  gr::begin(true);
  pu::steps();
  if (fromsample) {
    fromsample_demo(dir);
    return 0;
  }
  if (dir == "--warps") {   // --warps DIR: the new patches' phrases
    warps_demo(argc > 2 ? argv[2] : ".");
    return 0;
  }
  wavetable_demo(dir);
  voices_init();
  granular_demo(dir);
  return 0;
}
