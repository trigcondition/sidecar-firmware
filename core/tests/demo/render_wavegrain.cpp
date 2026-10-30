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
#include "notes.h"
#include "voices.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"

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

void wavetable_demo(const std::string& dir) {
  const int F = kWaveMachine;
  // P1..P16: TBL POS WARP AMT UNI SUB CUT RES ATK DEC SUS ENV CHD INV SPR VOI
  double t = 0;
  // 1. a pad progression on BASIC: unison, a slow POS drift, a soft low-pass
  for (int c = 0; c < 4; ++c, t += 2) {
    std::vector<float> p = {0, 30, 0, 0, 100, 30, 46, 20, 50, 90, 110, 80, (float)kChd[c], 1, 1, 0};
    at(t, [=] { wg::trig(F, 0, kRoot[c], 100, p); });
    sweep(t + 0.02, t + 1.9, F, p, 1, 30.0f + 10 * c, 60.0f + 10 * c);
    at(t + 1.95, [] { wg::off(0); });
  }
  // 2. POS across each built-in table, a held Am7 (VOCAL, ORGAN, FM, SYNC, PWM)
  const int tbls[5] = {2, 3, 4, 5, 1};
  for (int k = 0; k < 5; ++k, t += 3) {
    std::vector<float> p = {(float)tbls[k], 0, 0, 0, 60, 0, 64, 0, 5, 100, 120, 64, 7, 0, 0, 0};
    at(t, [=] { wg::trig(F, 0, 57, 100, p); });
    sweep(t + 0.02, t + 2.8, F, p, 1, 0, 127);
    at(t + 2.9, [] { wg::off(0); });
  }
  // 3. the warps' amount on a saw arpeggio: SYNC, PWM, FM (3 s each)
  for (int w = 1; w <= 3; ++w, t += 3) {
    std::vector<float> p = {0, kSawPosV(), (float)(w * 32 + 8), 0, 30, 20, 56, 30, 0, 80, 80, 64, 0, 0, 0, 0};
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
    std::vector<float> p = {0, 84, 0, 0, 0, 0, 64, 0, 10, 100, 120, 64, 6, 1, 0, 0};
    at(t, [=] { wg::trig(F, 0, 53, 100, p); });
    sweep(t + 0.02, t + 3.0, F, p, 4, 0, 127);
    p[4] = 127;
    p[7] = 90;
    sweep(t + 3.02, t + 7.0, F, p, 6, 10, 120);
    at(t + 7.1, [] { wg::off(0); });
    t += 7.5;
  }
  // 5. plucks: SUS 0, short DEC, the mod envelope opening POS on VOCAL
  {
    const int mel[16] = {57, 60, 64, 67, 69, 67, 64, 60, 53, 57, 60, 64, 65, 64, 60, 57};
    for (int n = 0; n < 16; ++n) {
      std::vector<float> p = {2, 10, 0, 0, 50, 40, 50, 40, 0, 72, 0, 110, 0, 0, 0, 0};
      at(t + n * 0.25, [=] { wg::trig(F, 0, mel[n], 100 + (n % 4 == 0 ? 20 : 0), p); });
      at(t + n * 0.25 + 0.1, [] { wg::off(0); });
    }
    t += 4.5;
  }
  std::vector<float> y = run(t + 1.5, false);
  wg::write_wav16(dir + "/wavetable_demo.wav", y);
  printf("wavetable_demo.wav: %.1f s, rms %.3f\n", y.size() / 48000.0, wg::rms(y));
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
  if (dir == "--bench") {
    host_set_card("/tmp/wg-demo-empty-card");
    hooks.to_rytm = [](const uint8_t*, int) {};
    voices_init();
    double t0 = wg::now_s();
    wt::begin(false);
    double t1 = wg::now_s();
    gr::begin(false);
    printf("begin: WAVETABLE's built-ins %.1f ms, GRANULAR's %.1f ms (Mac)\n", (t1 - t0) * 1e3, (wg::now_s() - t1) * 1e3);
    bench();
    return 0;
  }
  host_set_card("/tmp/wg-demo-empty-card");
  hooks.to_rytm = [](const uint8_t*, int) {};
  voices_init();
  wt::begin(false);
  gr::begin(false);
  wavetable_demo(dir);
  voices_init();
  granular_demo(dir);
  return 0;
}
