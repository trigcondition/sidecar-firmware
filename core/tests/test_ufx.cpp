// core/tests/test_ufx.cpp - the user effects PHASER (plugins/ufx_phaser) and BBD (plugins/ufx_bbd) as the send FX plays
// them: real RMPA packages run in place on this arm64 Mac (kept built in tests/api3_pkgs: `make -C plugins/ufx_phaser
// fixtures`, `make -C plugins/ufx_bbd fixtures`; CI has no AArch64 compiler), loaded into UFX1 / UFX2 by um and run
// through fx::SendFx as core 2 runs them. Checked:
//   - both load (API 3, an effect, BBD's line in the arena) and give the Rytm their labels from the manifest: the
//     knob names, RATE / TIME's FREE zone with its number and the note values, PHASER's 4ST / 6ST / 8ST zones;
//   - PHASER: notches where four, six or eight equal allpass stages put the swept copy half a cycle behind (the sweep
//     held still at 600 Hz), unity where they are in phase; feedback deepens them and stays bounded; the right
//     channel's sweep runs behind the left's with SPRD; the LFO sweeps; nothing but finite numbers, no denormals;
//   - BBD: the echo at the TIME it says (FREE and a note value at the tempo), darker and noisier at a longer time, the
//     feedback's repeats decaying at noon and below the top (a loud burst gone after 10 s: the compander adds no gain),
//     a bounded runaway at the top for 20 s, the wobble widening the stereo, TONE closing the top; finite numbers, no
//     denormals;
//   - the cost of one send slot's call on this Mac (printed; the Pi's estimate is in the plugins' docs).
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "fx/send_fx.h"
#include "hooks.h"
#include "lfo.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"

using namespace core;

#if defined(__aarch64__)
static std::vector<std::string> g_log;
static void sink(const char* s, int n) { g_log.emplace_back(s, (size_t)n); }
static void out(const uint8_t*, int) {}
static std::string slurp(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  CHECK(f);
  std::string b;
  char t[65536];
  size_t n;
  while ((n = fread(t, 1, sizeof t, f)) > 0) b.append(t, n);
  fclose(f);
  return b;
}
static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
static void sysex(std::vector<uint8_t> m) { engine_sysex(m.data(), (int)m.size()); }
static void idle_all() {
  int quiet = 0;
  for (int i = 0; i < 200000; ++i) {
    if (um::idle()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static std::string cmd(const std::string& c) {
  char o[200];
  CHECK(um::command(c.c_str(), o, sizeof o));
  idle_all();
  return o;
}
static int logged(const std::string& s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}

static uint64_t g_t = 1000000;
static void clock_ticks(int n, uint32_t tick_us) {   // MIDI clock at the LFOs' clock input, the control loop's tick
  for (int i = 0; i < n; ++i) {
    g_t += tick_us;
    host_fake_time(true, g_t);
    lfo::clock(0xF8, 0);
    lfo::tick((uint32_t)g_t);
  }
}

// ---- the send FX with one effect on FX 1 (TYPE 64 + u), BD's stem sent fully; x the input, L/R the return
struct Run {
  std::unique_ptr<fx::SendFx> sfx;
  int u;
  Run(int u_, int p1, int p2, int p3, int p4) : sfx(std::make_unique<fx::SendFx>(48000.0f)), u(u_) {
    cmd("um=reload:f" + std::to_string(u + 1));   // a fresh state (the effect's states are um's, not the SendFx's)
    memset((void*)&fx::control, 0, sizeof fx::control);
    fx::control.send[0][0] = 127;
    fx::control.knob[0][0] = (uint8_t)(fx::kFirstUser + u);
    fx::control.knob[0][1] = 0;     // ROUTE PRE (its limiter at -6 dBFS: the levels below stay under it)
    knobs(p1, p2, p3, p4);
    CHECK(fx::state(0) == fx::kStReady);
  }
  void knobs(int p1, int p2, int p3, int p4) {
    fx::control.knob[0][2] = (uint8_t)p1, fx::control.knob[0][3] = (uint8_t)p2;
    fx::control.knob[0][4] = (uint8_t)p3, fx::control.knob[0][5] = (uint8_t)p4;
  }
  // n frames (a multiple of 12) of x through it into L, R
  // (tick_every: a MIDI clock tick each that many frames, the transport running)
  void go(const std::vector<float>& x, std::vector<float>* L, std::vector<float>* R, int tick_every = 0) {
    size_t n = x.size() / 12 * 12;
    int since = 0;
    L->assign(n, 0.0f), R->assign(n, 0.0f);
    float st[12][fx::kStems], o[12][2], m[12][2];
    for (size_t i = 0; i < n; i += 12) {
      for (int j = 0; j < 12; ++j) {
        for (int k = 0; k < fx::kStems; ++k) st[j][k] = 0.0f;
        st[j][0] = x[i + j];
      }
      if (tick_every && (since += 12) >= tick_every) since -= tick_every, clock_ticks(1, 20833);
      um::pre_block();   // the block's transport snapshot (the platform's audio loop does this)
      um::idle();   // the control core: the effect's fresh state when its TYPE came
      sfx->Process(st, 12, o, fx::control, m);
      for (int j = 0; j < 12; ++j) (*L)[i + j] = m[j][0] + o[j][0], (*R)[i + j] = m[j][1] + o[j][1];
    }
  }
};
static bool sane(const std::vector<float>& y, float limit) {   // finite, no denormals, under limit
  for (float v : y)
    if (!std::isfinite(v) || std::fabs(v) > limit || (v != 0.0f && std::fpclassify(v) == FP_SUBNORMAL)) return false;
  return true;
}
static float rms(const std::vector<float>& y, size_t a, size_t b) {
  double s = 0.0;
  for (size_t i = a; i < b && i < y.size(); ++i) s += (double)y[i] * y[i];
  return b > a ? (float)std::sqrt(s / (double)(b - a)) : 0.0f;
}
static std::vector<float> sine(float hz, size_t n, float amp = 0.5f) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) x[i] = amp * (float)std::sin(2.0 * M_PI * hz * (double)i / 48000.0);
  return x;
}
static std::vector<float> noise(size_t n, float amp, uint32_t seed = 1) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) {
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    x[i] = amp * ((float)(int32_t)seed / 2147483648.0f);
  }
  return x;
}
// the share of y's energy in [a, b) above about 4 kHz (a first difference against the plain sum: a cheap tilt meter)
static float hf_share(const std::vector<float>& y, size_t a, size_t b) {
  double e = 0.0, d = 0.0;
  for (size_t i = a + 1; i < b && i < y.size(); ++i) e += (double)y[i] * y[i], d += (double)(y[i] - y[i - 1]) * (y[i] - y[i - 1]);
  return e > 0.0 ? (float)(d / e) : 0.0f;
}
// the PHASER's level for a sine at hz (the sweep held still: DPTH 0), after it settles: gain against the input
static float ph_gain(int stages_zone, int fb, float hz, float amp = 0.1f) {
  Run r(0, 20, 0, fb, stages_zone * 43 + 21);
  std::vector<float> L, R;
  r.go(sine(hz, 48000, amp), &L, &R);
  return rms(L, 24000, 48000) / (amp / std::sqrt(2.0f));
}

int main() {
  char tmpl[] = "/tmp/core-ufx-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_set_log_sink(sink);
  host_fake_time(true, g_t);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/projects/UF"));
  put("/.rytm/project", "UF");
  for (const char* n : {"ufx_phaser", "ufx_bbd"})
    put(std::string("/Plugins/") + n + ".rmp", slurp(std::string("tests/api3_pkgs/") + n + ".a64.rmp"));
  hooks.to_rytm = ::out;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'U', 'F', 0xf7});
  idle_all();
  cmd("um=empty");

  // ---- both load, and their labels
  cmd("um=fx:1:fx.phaser");
  cmd("um=fx:2:fx.bbd");
  um::SlotView v = um::slot_view(5);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3 && !v.muted);
  v = um::slot_view(6);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3 && !v.muted);
  CHECK(logged("um: UFX1 fx.phaser: ready (API 3") == 1);
  CHECK(logged("um: UFX2 fx.bbd: ready (API 3, arena ") == 1);
  CHECK(v.arena >= 2u * 2u * 120000u * sizeof(float));   // 2 states (FX 1, FX 2) x 2.5 s of line (the want granted)
  CHECK(std::string(fx::user[0].name) == "PHASER" && std::string(fx::user[1].name) == "BBD");
  {
    const fx::KnobLabels* l = fx::labels(fx::kFirstUser);
    CHECK(l && std::string(l->label[0]) == "RATE" && std::string(l->label[1]) == "DPTH");
    CHECK(l && std::string(l->label[2]) == "FDBK" && std::string(l->label[3]) == "SPRD");
    CHECK(l && l->nzones == 12);   // RATE: FREE (the 8 names one zone) + 8 note values; SPRD: 3
    if (l && l->nzones == 12) {
      CHECK(std::string(l->zones[0].name) == "FREE" && l->zones[0].lo == 0 && l->zones[0].show && !l->zones[0].step);
      CHECK(std::string(l->zones[1].name) == "8BAR" && l->zones[1].lo == 64 && !l->zones[1].show && !l->zones[1].step);
      CHECK(std::string(l->zones[8].name) == "1/16" && l->zones[8].lo == 120);
      CHECK(l->zones[9].knob == 3 && std::string(l->zones[9].name) == "4ST" && l->zones[9].lo == 0 && l->zones[9].show);
      CHECK(std::string(l->zones[10].name) == "6ST" && l->zones[10].lo == 43);
      CHECK(std::string(l->zones[11].name) == "8ST" && l->zones[11].lo == 86 && !l->zones[11].step);
    }
    l = fx::labels(fx::kFirstUser + 1);
    CHECK(l && std::string(l->label[0]) == "TIME" && std::string(l->label[2]) == "W/F" && std::string(l->label[3]) == "TONE");
    CHECK(l && l->nzones == 9 && std::string(l->zones[1].name) == "1/16" && std::string(l->zones[8].name) == "1/2");
    uint8_t m[160];
    int n = fx::sysex_labels(m, fx::kFirstUser + 1);
    CHECK(n == 24 + 9 * 8 && memcmp(m + 7, "TIME", 4) == 0);
    CHECK((m[23] & 15) == (0 | 4) && memcmp(m + 26, "FREE", 4) == 0);   // knob 0, show, no step
  }

  // ---- PHASER: the notches of 4 / 6 / 8 stages at 600 Hz (each stage -90 degrees there), the sweep held
  {
    // N equal stages are half a cycle out at f = 600 x tan(pi (2k + 1) / (2N)) (k = 0..): 4ST 249 and 1449 Hz,
    // 6ST 161, 600 (in phase: no), ... ; in phase (a peak) at 600 Hz for 4 and 8 stages
    float n4a = ph_gain(0, 0, 248.5f), n4b = ph_gain(0, 0, 1448.5f), p4 = ph_gain(0, 0, 600.0f);
    float n6 = ph_gain(1, 0, 600.0f * std::tan((float)M_PI / 12.0f)), n8 = ph_gain(2, 0, 600.0f * std::tan((float)M_PI / 16.0f));
    float p8 = ph_gain(2, 0, 600.0f), off = ph_gain(0, 0, 400.0f);
    printf("test_ufx: PHASER notch 4ST %.4f %.4f, 6ST %.4f, 8ST %.4f; peak 4ST %.3f 8ST %.3f; 400 Hz %.3f\n", n4a, n4b, n6, n8,
           p4, p8, off);
    CHECK(n4a < 0.03f && n4b < 0.03f && n6 < 0.03f && n8 < 0.03f);   // deep notches (< -30 dB)
    CHECK(p4 > 0.95f && p4 < 1.05f && p8 > 0.95f && p8 < 1.05f);     // unity between them
    CHECK(off > 0.2f && off < 0.95f);
    // feedback: the peak rises, held near unity by the compensation and the soft limit; the notch stays
    // (the feedback's one-sample delay moves the peak to 588 Hz; measured small, -34 dBFS: the loop runs 10 x the input
    // there, and the feedback's soft limit holds louder ones lower, as below)
    float pfb = ph_gain(0, 127, 588.0f, 0.02f), nfb = ph_gain(0, 127, 248.5f, 0.02f);
    float pld = ph_gain(0, 127, 588.0f, 0.5f);
    printf("test_ufx: PHASER FDBK 127: peak %.3f (model 1.778), notch %.4f; at -6 dBFS the peak %.3f\n", pfb, nfb, pld);
    CHECK(pfb > 1.6f && pfb < 1.95f && nfb < 0.03f);
    CHECK(pld < pfb && pld > 0.3f);
  }
  {   // sweeping, stereo: noise in, the spread puts R's notches elsewhere; then the worst case for numbers
    Run r(0, 50, 127, 100, 43 + 42);   // FREE ~2.3 Hz, full depth, 6ST, spread 180 degrees
    std::vector<float> x = noise(96000, 0.5f), L, R;
    r.go(x, &L, &R);
    CHECK(sane(L, 2.0f) && sane(R, 2.0f));
    double c = 0.0, el = 0.0, er = 0.0;
    for (size_t i = 48000; i < L.size(); ++i) c += (double)L[i] * R[i], el += (double)L[i] * L[i], er += (double)R[i] * R[i];
    float corr = (float)(c / std::sqrt(el * er));
    // a block of the left's level moves with the sweep: its min and max over 1 s of 20 ms windows
    float lo = 1e9f, hi = 0.0f;
    for (size_t a = 48000; a + 960 <= L.size(); a += 960) {
      float q = hf_share(L, a, a + 960);
      lo = q < lo ? q : lo, hi = q > hi ? q : hi;
    }
    printf("test_ufx: PHASER sweep: L/R correlation %.3f, the left's tilt %.3f .. %.3f\n", corr, lo, hi);
    CHECK(corr < 0.9f);          // the spread decorrelates the sides
    CHECK(hi > 1.3f * lo);       // the sweep moves the spectrum
    Run m(0, 63, 127, 127, 127);   // fastest, deepest, full feedback, 8 stages, full spread: loud noise, then silence
    std::vector<float> y = noise(48000 * 4, 1.0f, 7);
    y.resize(48000 * 10, 0.0f);    // 6 s of silence after: the tails decay to zero, never to denormals
    m.go(y, &L, &R);
    CHECK(sane(L, 2.0f) && sane(R, 2.0f));
    CHECK(rms(L, y.size() - 48000, y.size()) < 1e-6f);
    // a stage change mid-signal: no step (the taps crossfade)
    Run s(0, 20, 0, 0, 21);
    std::vector<float> a = sine(300.0f, 24000), L1, R1;
    s.go(a, &L1, &R1);
    s.knobs(20, 0, 0, 86 + 21);
    std::vector<float> b(a.begin(), a.end()), L2, R2;
    for (size_t i = 0; i < b.size(); ++i) b[i] = 0.5f * (float)std::sin(2.0 * M_PI * 300.0 * (double)(i + 24000) / 48000.0);
    s.go(b, &L2, &R2);
    float jump = std::fabs(L2[0] - L1.back()), step = 0.0f;
    for (size_t i = 1; i < L1.size(); ++i) step = std::fmax(step, std::fabs(L1[i] - L1[i - 1]));
    CHECK(jump < 1.5f * step);
  }
  {   // tempo sync: at 120 BPM (the transport's default) a 1BAR sweep is 2 s; the sweep's period in the tilt meter
    Run r(0, 64 + 3 * 8, 127, 0, 0);
    std::vector<float> x = noise(48000 * 6, 0.5f, 3), L, R;
    r.go(x, &L, &R);
    std::vector<float> tilt;
    for (size_t a = 0; a + 960 <= L.size(); a += 960) tilt.push_back(hf_share(L, a, a + 960));
    // the tilt's autocorrelation peaks at the period: 2 s = 100 windows (a sine sweep: also at its half, 1 s, it is
    // inverted, so the peak at 100 beats the one at 50)
    auto ac = [&](int lag) {
      double m = 0.0, s = 0.0;
      for (float t : tilt) m += t;
      m /= (double)tilt.size();
      for (size_t i = 0; i + (size_t)lag < tilt.size(); ++i) s += (tilt[i] - m) * (tilt[i + lag] - m);
      return s;
    };
    int best = 0;
    double bv = -1e30;
    for (int lag = 60; lag <= 140; ++lag)
      if (ac(lag) > bv) bv = ac(lag), best = lag;
    printf("test_ufx: PHASER 1BAR at 120 BPM: period %d ms\n", best * 20);
    CHECK(best >= 96 && best <= 104);
  }
  {   // locked to the song: Start at song position 0 and at 1 bar (16 sixteenths) give the same sweep (the phase is
      // song_tick % ticks, snapped on the START/JUMP); at half a bar it is half a cycle off
    std::vector<float> x = noise(48000 * 4, 0.5f, 21);
    auto at = [&](int spp, std::vector<float>* L) {
      lfo::clock(0xFC, 0);
      clock_ticks(24, 20833);   // the tempo: 120 BPM
      Run r(0, 64 + 3 * 8, 127, 0, 0);
      lfo::clock(0xFA, 0);
      if (spp) lfo::clock(0xF2, spp);
      clock_ticks(1, 20833);
      std::vector<float> R;
      r.go(x, L, &R, 1000);   // a clock tick each 1000 frames (20.833 ms at 48 kHz)
      lfo::clock(0xFC, 0);
    };
    std::vector<float> A, B, C;
    at(0, &A), at(16, &B), at(8, &C);
    double dab = 0.0, dac = 0.0, ea = 0.0;
    for (size_t i = 48000; i < A.size(); ++i)
      dab += (double)(A[i] - B[i]) * (A[i] - B[i]), dac += (double)(A[i] - C[i]) * (A[i] - C[i]), ea += (double)A[i] * A[i];
    printf("test_ufx: PHASER 1BAR locked: from 1 bar %.4f, from half a bar %.3f (the difference against the level)\n",
           std::sqrt(dab / ea), std::sqrt(dac / ea));
    CHECK(std::sqrt(dab / ea) < 0.02 && std::sqrt(dac / ea) > 0.2);
  }

  // ---- BBD: the echo at its time; darker and noisier when longer
  auto echo_at = [](const std::vector<float>& y, size_t from, size_t to) {
    size_t at = from;
    float m = 0.0f;
    for (size_t i = from; i < to && i < y.size(); ++i)
      if (std::fabs(y[i]) > m) m = std::fabs(y[i]), at = i;
    return at;
  };
  const size_t kAt = 2400;
  std::vector<float> clicks(48000 * 3, 0.0f);
  for (int k = 0; k < 24; ++k) clicks[kAt + k] = 0.8f * (float)std::sin(M_PI * k / 24.0);   // a short click
  float hf_short = 0.0f, hf_long = 0.0f, n_short = 0.0f, n_long = 0.0f;
  for (int tv : {12, 32, 52}) {   // FREE: 20 ms x 50^(v/63)
    Run r(1, tv, 0, 0, 127);
    std::vector<float> L, R;
    r.go(clicks, &L, &R);
    CHECK(sane(L, 2.5f) && sane(R, 2.5f));
    float ms = 20.0f * std::pow(50.0f, (float)tv / 63.0f);
    size_t want = kAt + 12 + (size_t)(ms * 48.0f);
    size_t at = echo_at(L, kAt + 100, L.size());
    float peak = std::fabs(L[at]);
    printf("test_ufx: BBD TIME %d: echo at %.1f ms (want %.1f), peak %.3f\n", tv, (at - kAt) / 48.0, ms, peak);
    CHECK((float)at > (float)want - 48.0f && (float)at < (float)want + 48.0f);   // within 1 ms (the filters' delay)
    CHECK(peak > 0.1f && peak < 2.0f);
    // the tilt of a noise burst's echo (20 ms of white noise at the click's place)
    std::vector<float> burst = noise(48000, 0.3f, 21);
    for (size_t i = 0; i < burst.size(); ++i) if (i < kAt || i >= kAt + 960) burst[i] = 0.0f;
    Run rb(1, tv, 0, 0, 127);
    rb.go(burst, &L, &R);
    size_t e0 = kAt + (size_t)(ms * 48.0f) + 96;
    float tilt = hf_share(L, e0, e0 + 768);
    printf("test_ufx: BBD TIME %d: a noise burst's echo tilt %.4f\n", tv, tilt);
    if (tv == 12) hf_short = tilt;
    if (tv == 52) hf_long = tilt;
  }
  CHECK(hf_long < 0.5f * hf_short);   // the slower clock: darker echoes
  for (int tv : {12, 52}) {           // the hiss under a steady tone, against the time
    Run r(1, tv, 0, 0, 127);
    std::vector<float> x = sine(250.0f, 48000 * 3, 0.1f), L, R;
    r.go(x, &L, &R);
    // the residual: the output less a least-squares fit of the tone and its first 8 harmonics over the last second
    // (the compander's and the soft clip's distortion), so what is left is the line's noise
    size_t a = 48000 * 2, b = L.size();
    std::vector<double> e(L.begin() + a, L.begin() + b);
    double lvl = 0.0;
    for (int h = 1; h <= 9; ++h) {
      double sc = 0.0, cc = 0.0;
      for (size_t i = a; i < b; ++i) {   // 250 Hz x h: whole cycles in 1 s, the fits are orthogonal
        double ph = 2.0 * M_PI * 250.0 * h * (double)i / 48000.0;
        sc += e[i - a] * std::sin(ph), cc += e[i - a] * std::cos(ph);
      }
      sc *= 2.0 / (double)(b - a), cc *= 2.0 / (double)(b - a);
      if (h == 1) lvl = std::sqrt((sc * sc + cc * cc) / 2.0);
      for (size_t i = a; i < b; ++i) {
        double ph = 2.0 * M_PI * 250.0 * h * (double)i / 48000.0;
        e[i - a] -= sc * std::sin(ph) + cc * std::cos(ph);
      }
    }
    double res = 0.0;
    for (double v : e) res += v * v;
    float nl = (float)std::sqrt(res / (double)e.size());
    float db = 20.0f * std::log10(nl / (float)std::fmax(lvl, 1e-9));
    printf("test_ufx: BBD TIME %d: the tone %.4f, the noise %.1f dB under it\n", tv, lvl, db);
    CHECK(lvl > 0.03f && db < -45.0f);
    (tv == 12 ? n_short : n_long) = db;
  }
  CHECK(n_long > n_short + 3.0f);   // the slower clock: more hiss
  {   // a note value at the tempo: 1/4 at 120 BPM (the default transport) = 500 ms
    Run r(1, 64 + 5 * 8, 0, 0, 127);
    std::vector<float> L, R;
    r.go(clicks, &L, &R);
    size_t at = echo_at(L, kAt + 100, L.size());
    printf("test_ufx: BBD 1/4 at 120 BPM: echo at %.1f ms\n", (at - kAt) / 48.0);
    CHECK(at > kAt + 24000 - 48 && at < kAt + 24000 + 96);
  }
  {   // FDBK at noon: the repeats decay; at the top: bounded for 20 s, never NaN or denormal; then silence decays
    Run r(1, 24, 64, 0, 127);
    std::vector<float> L, R;
    r.go(clicks, &L, &R);
    size_t d = (size_t)(20.0f * std::pow(50.0f, 24.0f / 63.0f) * 48.0f);
    float e1 = std::fabs(L[echo_at(L, kAt + d / 2, kAt + d + d / 2)]);
    float e3 = std::fabs(L[echo_at(L, kAt + 2 * d + d / 2, kAt + 3 * d + d / 2)]);
    printf("test_ufx: BBD FDBK 64: repeats %.3f then %.3f\n", e1, e3);
    CHECK(e3 < e1 && e3 > 0.05f * e1);
    Run m(1, 20, 127, 0, 127);   // the top, no wobble: the loop's gain past 1, a runaway held by the soft clips
    std::vector<float> x = noise(48000, 1.0f, 9);
    x.resize(48000 * 20, 0.0f);
    m.go(x, &L, &R);
    CHECK(sane(L, 2.5f) && sane(R, 2.5f));
    float last = rms(L, L.size() - 48000, L.size());
    printf("test_ufx: BBD FDBK 127: 19 s after the input, the level %.3f (a bounded runaway)\n", last);
    CHECK(last > 0.02f && last < 0.7f);
    Run m2(1, 20, 127, 127, 127);   // the top with full wobble: the loop's energy spreads, it fades
    m2.go(x, &L, &R);
    CHECK(sane(L, 2.5f) && sane(R, 2.5f));
    float last2 = rms(L, L.size() - 48000, L.size());
    printf("test_ufx: BBD FDBK 127 W/F 127: the last second %.2e\n", last2);
    CHECK(last2 < 0.5f * last);
    Run q(1, 40, 100, 64, 64);   // below the runaway: a loud burst, then 10 s of silence decays to nothing
    std::vector<float> y = noise(48000, 1.0f, 11);
    y.resize(48000 * 12, 0.0f);
    q.go(y, &L, &R);
    printf("test_ufx: BBD FDBK 100: after 10 s of silence %.2e (sane %d %d)\n", rms(L, y.size() - 48000, y.size()), sane(L, 2.5f), sane(R, 2.5f));
    CHECK(sane(L, 2.5f) && sane(R, 2.5f) && rms(L, y.size() - 48000, y.size()) < 1e-3f);
  }
  {   // W/F widens: with it the sides differ, without they are the same; TONE closes the top
    // (one Run at a time: they share the send FX's control and the effect's state)
    std::vector<float> x = noise(48000 * 2, 0.3f, 5), L, R, L2, R2;
    {
      Run a(1, 30, 40, 0, 127);
      a.go(x, &L, &R);
    }
    {
      Run b(1, 30, 40, 127, 127);
      b.go(x, &L2, &R2);
    }
    float diff0 = 0.0f, diff1 = 0.0f;
    for (size_t i = 48000; i < L.size(); ++i) diff0 = std::fmax(diff0, std::fabs(L[i] - R[i])), diff1 = std::fmax(diff1, std::fabs(L2[i] - R2[i]));
    printf("test_ufx: BBD W/F: L-R %.5f without, %.3f with\n", diff0, diff1);
    CHECK(diff0 < 1e-4f && diff1 > 0.05f);
    float dark, open;
    {
      Run t(1, 12, 0, 0, 0);
      t.go(x, &L, &R);
      dark = hf_share(L, 48000, L.size());
    }
    {
      Run o(1, 12, 0, 0, 127);
      o.go(x, &L2, &R2);
      open = hf_share(L2, 48000, L2.size());
    }
    printf("test_ufx: BBD TONE: tilt %.4f closed, %.4f open\n", dark, open);
    CHECK(dark < 0.3f * open);
  }

  // ---- the cost of one call (one send slot, 12 frames) on this Mac: both effects, busy settings
  for (int u = 0; u < 2; ++u) {
    Run r(u, 40, 127, 100, 127);
    std::vector<float> x = noise(12 * 4000, 0.5f, 13), L, R;
    r.go(x, &L, &R);   // warm
    auto t0 = std::chrono::steady_clock::now();
    r.go(x, &L, &R);
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 4000.0;
    printf("test_ufx: %s: %.2f us a 12-frame block of the whole send FX on this Mac (the effect and the host's mix)\n",
           u ? "BBD" : "PHASER", us);
    if (getenv("UFX_TIMING")) CHECK(us < 30.0f);   // (wall clock: only when asked, a busy machine would fail it)
  }
  cmd("um=fx:1:-");
  cmd("um=fx:2:-");
  CHECK(!fx::labels(fx::kFirstUser) && !fx::labels(fx::kFirstUser + 1));
  printf("test_ufx: PASS\n");
  return 0;
}
#else
int main() {
  printf("test_ufx: SKIP (the RMPA packages run in place on an arm64 host only)\n");
  return 0;
}
#endif
