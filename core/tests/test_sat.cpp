// core/tests/test_sat.cpp - INSERT TYPE SAT (core/fx/sat_fx.h, docs/INSERT-FX.md): the ids around it, the names, bit-exact
// bypass, the latency and the dry path, aliasing against the plain curve, the level across DRV, no click, and the input
// nobody wants (denormals, NaN, full scale, zero). The master chain's every-TYPE test is test_master.cpp's.
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/sat_fx.h"
#include "timing.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;
using ins::Engine;

static const float kSr = 48000.0f;
static const int kTape = 0, kCons = 64, kSoft = 127;   // the CHAR knob, one value in each zone
static const int kChar[3] = {kTape, kCons, kSoft};
static const char* const kCharName[3] = {"TAPE", "CONS", "SOFT"};

static std::vector<float> g_mem((size_t)ins::kLines * ins::kLine);
static Engine* g_e;
static Engine& fresh() {
  static Engine* e = new Engine(kSr);
  std::fill(g_mem.begin(), g_mem.end(), 0.0f);
  e->Place((float(*)[ins::kLine])g_mem.data());
  e->Reset();
  memset((void*)&ins::control, 0, sizeof ins::control);
  g_e = e;
  return *e;
}
static void set(int chain, int slot, int t, int c, int drv, int tone, int mix) {
  ins::control.type[chain][slot] = (uint8_t)t;
  const int p[4] = {c, drv, tone, mix};
  for (int k = 0; k < 4; ++k) ins::control.play[chain][slot][k] = (uint8_t)p[k];
}
static std::vector<float> mono(Engine& e, std::vector<float> y, int chain = 0) {
  for (size_t i = 0; i < y.size(); i += 12) e.ProcessMono(chain, &y[i], (int)std::min<size_t>(12, y.size() - i), ins::control);
  return y;
}
static std::vector<float> sine(float hz, float amp, int n) {
  std::vector<float> x(n);
  for (int i = 0; i < n; ++i) x[i] = amp * (float)std::sin(2 * M_PI * hz * i / kSr);
  return x;
}
static std::vector<float> noise(float amp, int n) {
  std::vector<float> x(n);
  uint32_t r = 12345;
  for (float& s : x) r = r * 1664525u + 1013904223u, s = amp * ((float)(r >> 8) / 8388608.0f - 1.0f);
  return x;
}
static float rms(const std::vector<float>& x, size_t from = 0) {
  double s = 0;
  for (size_t i = from; i < x.size(); ++i) s += x[i] * x[i];
  return (float)std::sqrt(s / (double)(x.size() - from));
}
static float peak(const std::vector<float>& x, size_t from = 0) {
  float p = 0;
  for (size_t i = from; i < x.size(); ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}
static float worst_step(const std::vector<float>& y, size_t from = 1) {
  float w = 0;
  for (size_t i = std::max<size_t>(from, 1); i < y.size(); ++i) w = std::max(w, std::fabs(y[i] - y[i - 1]));
  return w;
}
static float db(float g) { return 20.0f * std::log10(std::max(g, 1e-12f)); }

static void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t b = n >> 1;
    for (; j & b; b >>= 1) j ^= b;
    j ^= b;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t l = 2; l <= n; l <<= 1) {
    const double t = -2 * M_PI / (double)l;
    for (size_t i = 0; i < n; i += l)
      for (size_t k = 0; k < l / 2; ++k) {
        std::complex<double> w = std::polar(1.0, t * (double)k), u = a[i + k], v = w * a[i + k + l / 2];
        a[i + k] = u + v, a[i + k + l / 2] = u - v;
      }
  }
}
// the loudest line that is no harmonic of the sine at bin, below 20 kHz, in dB against the fundamental
constexpr int kFft = 1 << 15;
static float worst_alias(const std::vector<float>& y, size_t from, int bin) {
  std::vector<std::complex<double>> a(kFft);
  for (int i = 0; i < kFft; ++i) a[i] = y[from + i];
  fft(a);
  const double fund = std::abs(a[bin]);
  double w = 0;
  for (int k = 1; k < (int)(20000.0 / 48000.0 * kFft); ++k)
    if (k % bin) w = std::max(w, std::abs(a[k]));
  return db((float)(w / fund));
}

static void ids_and_names() {
  // every id the releases know stays; SAT comes after AMP, the master's send TYPEs after it
  static const char* const kOld[] = {"OFF", "FILTER", "CRUSH", "DRIVE", "EQ", "COMP", "DELAY", "TREMOLO", "RINGMOD", "AMP"};
  for (int t = 0; t < 10; ++t) CHECK(!strcmp(ins::type_name(t), kOld[t]) && ins::type_from_name(kOld[t]) == t);
  CHECK(ins::kSat == 10 && ins::kTypes == 11 && ins::kMChorus == 11 && ins::kAllTypes == 17);
  static const char* const kSend[] = {"CHORUS", "COMB", "SPACE", "RINGS", "CLOUD", "HALO"};
  for (int t = 0; t < 6; ++t) CHECK(ins::type_from_name(kSend[t]) == ins::kMChorus + t);
  CHECK(ins::type_from_name("SAT") == ins::kSat && ins::type_from_name("sat") == ins::kSat);
  const ins::Labels& L = ins::labels(ins::kSat);
  CHECK(!strcmp(L.name, "SAT") && !strcmp(L.label[0], "CHAR") && !strcmp(L.label[1], "DRV") && !strcmp(L.label[2], "TONE") &&
        !strcmp(L.label[3], "MIX"));
  char b[16];
  int seen[3] = {0, 0, 0};
  for (int v = 0; v < 128; ++v) {
    int c = satfx::char_of(v);
    CHECK(c >= 0 && c < satfx::kChars);
    ++seen[c];
    ins::value_text(ins::kSat, 0, v, b, sizeof b);
    CHECK(!strcmp(b, kCharName[c]));
    CHECK(strlen(b) <= 7);
  }
  CHECK(seen[0] > 40 && seen[1] > 40 && seen[2] > 40);
  ins::value_text(ins::kSat, 1, 127, b, sizeof b);
  CHECK(!strcmp(b, "+24DB"));
  ins::value_text(ins::kSat, 1, 0, b, sizeof b);
  CHECK(!strcmp(b, "+0DB"));
  ins::value_text(ins::kSat, 2, 64, b, sizeof b);
  CHECK(!strcmp(b, "FLAT"));
  ins::value_text(ins::kSat, 2, 127, b, sizeof b);
  CHECK(!strcmp(b, "+6DB"));
  ins::value_text(ins::kSat, 2, 0, b, sizeof b);
  CHECK(!strcmp(b, "-6DB"));
  // every chain takes it: voices, buses, the master
  ins::reset();
  for (int c : {0, ins::kBus0, ins::kBus0 + 2, ins::kM0, ins::kM0 + 1}) {
    ins::set_type(c, 0, ins::kSat);
    CHECK(ins::control.type[c][0] == ins::kSat);
    CHECK(ins::param(c, 0, 3) == 127);
  }
  CHECK(master::next_type(0, ins::kAmp, 1) == ins::kSat);
  char o[200];
  CHECK(ins::command("ins=V2:B:type:sat", o, sizeof o) && strstr(o, "SAT") && strstr(o, "CHAR") && strstr(o, "TAPE"));
  CHECK(ins::command("ins=V2:B:p1:64", o, sizeof o) && strstr(o, "CONS"));
  ins::reset();
  printf("test_sat: ids kept, SAT = %d, the names, every chain takes it\n", ins::kSat);
}

// the INSERT page's TYPE key on a bus steps over AMP (which a bus refuses) to SAT and back
static void bus_page_reaches_sat() {
  ins::reset();
  const ui::ExtraPage* p = nullptr;
  for (int k = 0; k < ui::extra_pages(); ++k)
    if (!strcmp(ui::extra_page(k).title, "INSERT")) p = &ui::extra_page(k);
  CHECK(p);
  CHECK(p->key(0x44));   // YES: the BUS view
  ins::set_type(ins::kBus0, 0, ins::kRing);
  p->enc(5, 1, false);
  CHECK(ins::control.type[ins::kBus0][0] == ins::kSat);
  p->enc(5, -1, false);
  CHECK(ins::control.type[ins::kBus0][0] == ins::kRing);
  // 0.5.0: past SAT a bus's list runs on into the send TYPEs (core/fx/master.h); past its end (HALO) nothing happens: a
  // tuned slot keeps its knobs, the card file stays clean
  ins::set_type(ins::kBus0, 0, ins::kSat);
  p->enc(5, 1, false);
  CHECK(ins::control.type[ins::kBus0][0] == ins::kMChorus);
  ins::set_type(ins::kBus0, 0, ins::kMHalo);
  ins::set_param(ins::kBus0, 0, 1, 99);
  ins::settings_clean();
  p->enc(5, 1, false);
  CHECK(ins::control.type[ins::kBus0][0] == ins::kMHalo && ins::param(ins::kBus0, 0, 1) == 99 && !ins::settings_dirty(nullptr));
  ins::set_type(ins::kBus0, 0, ins::kOff);
  ins::settings_clean();
  p->enc(5, -1, false);
  CHECK(ins::control.type[ins::kBus0][0] == ins::kOff && !ins::settings_dirty(nullptr));
  p->key(0x45);          // back
  p->key(0x45);
  ins::reset();
  printf("test_sat: a bus's TYPE key steps over AMP, and stops at the ends without touching the slot\n");
}

static void bit_exact() {
  auto x = noise(0.7f, 9600);
  const int cases[][2] = {{0, 0}, {0, 64}, {0, 127}, {1, 0}, {60, 0}, {127, 0}};
  for (int c : kChar)
    for (auto& q : cases) {
      Engine& e = fresh();
      set(0, 0, ins::kSat, c, q[0], 90, q[1]);
      auto y = mono(e, x);
      CHECK(!memcmp(y.data(), x.data(), x.size() * sizeof(float)));
      // the stereo paths
      Engine& e2 = fresh();
      set(0, 0, ins::kSat, c, q[0], 90, q[1]);
      set(ins::kBus0, 0, ins::kSat, c, q[0], 90, q[1]);
      auto l = x, r = x;
      std::reverse(r.begin(), r.end());
      auto r0 = r;
      std::vector<float> by(2 * x.size());
      for (size_t i = 0; i < x.size(); ++i) by[2 * i] = l[i], by[2 * i + 1] = r[i];
      for (size_t i = 0; i < x.size(); i += 12) {
        e2.ProcessVoiceStereo(0, &l[i], &r[i], 12, ins::control);
        e2.ProcessStereo(ins::kBus0, (float(*)[2])&by[2 * i], 12, ins::control);
      }
      CHECK(!memcmp(l.data(), x.data(), x.size() * sizeof(float)) && !memcmp(r.data(), r0.data(), x.size() * sizeof(float)));
      for (size_t i = 0; i < x.size(); ++i) CHECK(by[2 * i] == x[i] && by[2 * i + 1] == r0[i]);
    }
  // back to bypass from a playing slot: bit for bit again once the 3 ms smoothing and the 5 ms dry fade are over
  for (int c : kChar) {
    Engine& e = fresh();
    set(0, 0, ins::kSat, c, 80, 64, 100);
    auto y = x;
    size_t i = 0;
    for (; i < 4800; i += 12) e.ProcessMono(0, &y[i], 12, ins::control);
    set(0, 0, ins::kSat, c, 80, 64, 0);
    x.resize(36000), y.resize(36000);
    for (size_t j = 9600; j < x.size(); ++j) x[j] = x[j - 9600];
    for (size_t j = 9600; j < y.size(); ++j) y[j] = x[j];
    for (; i < y.size(); i += 12) e.ProcessMono(0, &y[i], 12, ins::control);
    const size_t idle = 4800 + 2400 + satfx::kHold + satfx::kFade;   // the knob's smoothing, the hold, the fade
    CHECK(!memcmp(y.data() + idle, x.data() + idle, (x.size() - idle) * sizeof(float)));
    CHECK(e.Faults() == 0);
    x.resize(9600);
  }
  printf("test_sat: DRV 0 or MIX 0 bit for bit, mono and both stereo paths, from the first sample and back from a playing slot\n");
}

static void latency_and_dry() {
  for (int ci = 0; ci < 3; ++ci) {
    Engine& e = fresh();
    set(0, 0, ins::kSat, kChar[ci], 30, 64, 127);
    auto x = sine(997, 0.01f, 12000);
    auto y = mono(e, x);
    int best = -1;
    double bv = -1;
    for (int lag = 0; lag < 64; ++lag) {
      double s = 0;
      for (int i = 4800; i < 11000; ++i) s += y[i + lag] * x[i];
      if (s > bv) bv = s, best = lag;
    }
    CHECK(best == satfx::kLatency);
    // MIX halfway at 12 kHz: the dry path is delayed as the wet is, no comb
    Engine& f = fresh();
    set(0, 0, ins::kSat, kChar[ci], 30, 64, 64);
    auto u = sine(12000, 0.01f, 12000);
    auto v = mono(f, u);
    float g = db(rms(v, 4800) / rms(u, 4800));
    printf("test_sat: %s lag %d, MIX 64 at 12 kHz %+.2f dB\n", kCharName[ci], best, g);
    CHECK(std::fabs(g) < 1.0f);
  }
  printf("test_sat: latency %d samples (%.2f ms), the dry path with it\n", satfx::kLatency, satfx::kLatency * 1000.0f / kSr);
}

// a p-lock or an LFO takes DRV or MIX through 0 on a sustained tone: the wet ends, the delay stays, no dip. 1.6 kHz is the
// worst tone: 15 frames are half its period
static void through_zero() {
  auto x = sine(1600, 0.25f, 48000);
  auto low = [&](const std::vector<float>& y, size_t from) {   // the lowest peak over one period
    float w = 9;
    for (size_t i = from; i + 30 < y.size(); i += 6) {
      float p = 0;
      for (size_t j = i; j < i + 30; ++j) p = std::max(p, std::fabs(y[j]));
      w = std::min(w, p);
    }
    return w;
  };
  for (int ci = 0; ci < 3; ++ci)
    for (int knob = 0; knob < 2; ++knob) {   // DRV, MIX
      Engine& e = fresh();
      set(0, 0, ins::kSat, kChar[ci], 30, 64, 127);
      auto y = x;
      for (size_t i = 0; i < y.size(); i += 12) {
        bool zero = (i / 2400) % 4 == 3 && i >= 9600;   // 50 ms at 0 every 200 ms
        set(0, 0, ins::kSat, kChar[ci], knob == 0 && zero ? 0 : 30, 64, knob == 1 && zero ? 0 : 127);
        e.ProcessMono(0, &y[i], 12, ins::control);
      }
      float w = low(y, 4800);
      printf("test_sat: %s %s through 0 at 1.6 kHz: lowest peak %.3f (the tone 0.25)\n", kCharName[ci], knob ? "MIX" : "DRV", w);
      CHECK(w > 0.2f);
    }
  // idle: long at 0 the path is x itself again, and still no step on the way
  {
    Engine& e = fresh();
    set(0, 0, ins::kSat, kSoft, 30, 64, 127);
    auto y = x;
    size_t i = 0;
    for (; i < 4800; i += 12) e.ProcessMono(0, &y[i], 12, ins::control);
    set(0, 0, ins::kSat, kSoft, 0, 64, 127);
    for (; i < y.size(); i += 12) e.ProcessMono(0, &y[i], 12, ins::control);
    const size_t idle = 4800 + 2400 + satfx::kHold + satfx::kFade;   // the knob's smoothing, the hold, the fade
    CHECK(!memcmp(y.data() + idle, x.data() + idle, (x.size() - idle) * sizeof(float)));
  }
}

static void aliasing() {
  // the curve alone, 1x, against the slot (4x): the worst line that is no harmonic of the input, below 20 kHz
  const int fs[] = {1000, 5000, 10000, 15000};
  float worst[3][4][2];
  for (int ci = 0; ci < 3; ++ci)
    for (int fi = 0; fi < 4; ++fi)
      for (int a = 0; a < 2; ++a) {
        const float amp = a ? 0.25f : 0.5f;
        const int bin = (int)std::lround(fs[fi] / 48000.0 * kFft / 64) * 64;
        std::vector<float> x(kFft + 4800);
        for (size_t i = 0; i < x.size(); ++i) x[i] = amp * (float)std::sin(2 * M_PI * bin * (double)i / kFft);
        Engine& e = fresh();
        set(0, 0, ins::kSat, kChar[ci], 127, 64, 127);
        auto y = mono(e, x);
        const float os = worst_alias(y, 4800, bin);
        const float g = exp2f(4.0f);
        std::vector<float> n(x.size());
        for (size_t i = 0; i < x.size(); ++i) n[i] = satfx::curve(ci, x[i] * g);
        const float plain = worst_alias(n, 4800, bin);
        worst[ci][fi][a] = os;
        printf("aliasing %s DRV +24 dB, %5d Hz at %.2f: plain %6.1f dB, SAT %6.1f dB\n", kCharName[ci], fs[fi], amp, plain, os);
        if (fi < 3) CHECK(os < plain - 12.0f);
      }
  for (int ci = 0; ci < 3; ++ci) {
    CHECK(worst[ci][0][0] < -70.0f && worst[ci][0][1] < -80.0f);   // 1 kHz
    CHECK(worst[ci][1][0] < -40.0f && worst[ci][1][1] < -50.0f);   // 5 kHz
    CHECK(worst[ci][2][0] < -25.0f && worst[ci][2][1] < -38.0f);   // 10 kHz (TAPE: its shelf adds 6 dB up there)
  }
}

static void level() {
  for (int ci = 0; ci < 3; ++ci) {
    float lo = 99, hi = -99;
    auto x = sine(220, 0.25f, 19200);
    for (int d = 0; d < 128; d += (d < 8 ? 1 : 8)) {
      Engine& e = fresh();
      set(0, 0, ins::kSat, kChar[ci], d, 64, 127);
      auto y = mono(e, x);
      float g = db(rms(y, 9600) / rms(x, 9600));
      lo = std::min(lo, g), hi = std::max(hi, g);
    }
    Engine& e = fresh();
    set(0, 0, ins::kSat, kChar[ci], 127, 64, 127);
    auto n = noise(0.5f, 19200);
    auto y = mono(e, n);
    printf("level %s: a -12 dBFS sine across DRV %+.2f .. %+.2f dB; DRV 127 on noise peak 0.5 %+.2f dB\n", kCharName[ci], lo, hi,
           db(rms(y, 9600) / rms(n, 9600)));
    CHECK(lo > -1.5f && hi < 2.5f);
  }
}

static void no_click() {
  auto x = sine(220, 0.5f, 48000);
  // what a playing slot steps by on its own
  float own = 0;
  for (int c : kChar) {
    Engine& e = fresh();
    set(0, 0, ins::kSat, c, 60, 64, 127);
    own = std::max(own, worst_step(mono(e, x), 4800));
  }
  // CHAR among the three, every 37 ms
  {
    Engine& e = fresh();
    auto y = x;
    int n = 0;
    for (size_t i = 0; i < y.size(); i += 12) {
      if (i % 1776 == 0) set(0, 0, ins::kSat, kChar[n++ % 3], 60, 64, 127);
      e.ProcessMono(0, &y[i], 12, ins::control);
    }
    float w = worst_step(y, 1776);
    printf("test_sat: CHAR every 37 ms: largest step %.4f (a playing slot's own %.4f)\n", w, own);
    CHECK(w < own * 1.3f);
    CHECK(e.Faults() == 0);
  }
  // TAPE <-> SOFT, the far ends of the knob, as a p-lock jumps
  {
    Engine& e = fresh();
    auto y = x;
    for (size_t i = 0; i < y.size(); i += 12) {
      if (i % 480 == 0) set(0, 0, ins::kSat, (i / 480) & 1 ? kSoft : kTape, 60, 64, 127);
      e.ProcessMono(0, &y[i], 12, ins::control);
    }
    float w = worst_step(y, 480);
    printf("test_sat: CHAR 0 <-> 127 every 10 ms: largest step %.4f\n", w);
    CHECK(w < own * 1.3f);
  }
  // TYPE in and out, among OFF and DRIVE
  {
    Engine& e = fresh();
    const int seq[] = {ins::kOff, ins::kSat, ins::kDrive, ins::kSat, ins::kFilter};
    auto y = x;
    int n = 0;
    for (size_t i = 0; i < y.size(); i += 12) {
      if (i % 1776 == 0) {
        int t = seq[n++ % 5];
        set(0, 0, t, t == ins::kSat ? kTape : 20, t == ins::kSat ? 30 : 64, 64, 127);
      }
      e.ProcessMono(0, &y[i], 12, ins::control);
    }
    float w = worst_step(y, 1);
    printf("test_sat: TYPE every 37 ms: largest step %.4f (the sine's own 0.0144)\n", w);
    CHECK(w < 0.06f);
  }
  // MIX and DRV jumps every 10 ms
  {
    Engine& e = fresh();
    auto y = x;
    for (size_t i = 0; i < y.size(); i += 12) {
      if (i % 480 == 0) set(0, 0, ins::kSat, kCons, 40, 64, (i / 480) & 1 ? 127 : 0);
      e.ProcessMono(0, &y[i], 12, ins::control);
    }
    float a = worst_step(y, 480);
    Engine& f = fresh();
    y = x;
    for (size_t i = 0; i < y.size(); i += 12) {
      if (i % 480 == 0) set(0, 0, ins::kSat, kSoft, (i / 480) & 1 ? 127 : 0, 64, 127);
      f.ProcessMono(0, &y[i], 12, ins::control);
    }
    float b = worst_step(y, 480);
    printf("test_sat: MIX 0 <-> 127 every 10 ms: largest step %.4f; DRV 0 <-> 127: %.4f\n", a, b);
    CHECK(a < 0.08f && b < 0.2f);
  }
}

static void hostile_input() {
  // zero in, zero out, and nothing denormal left after silence
  for (int c : kChar) {
    Engine& e = fresh();
    set(0, 0, ins::kSat, c, 127, 127, 127);
    std::vector<float> z(9600, 0.0f);
    auto y = mono(e, z);
    for (float v : y) CHECK(v == 0.0f);
    auto x = sine(300, 0.5f, 4800);
    x.resize(4800 + 96000, 0.0f);
    y = mono(e, x);
    CHECK(peak(y, y.size() - 4800) == 0.0f);
    const satfx::Ch& s = e.slot(0, 0).d[0].sat;
    CHECK(s.pre == 0.0f && s.de == 0.0f && s.b1 == 0.0f && s.b2 == 0.0f && s.tl == 0.0f);
    // a denormal input: finite, and no denormal comes out of the wet path past its filters
    std::vector<float> d(9600, 1e-40f);
    y = mono(e, d);
    for (float v : y) CHECK(std::isfinite(v));
    CHECK(s.pre == 0.0f || std::fpclassify(s.pre) == FP_NORMAL);
    CHECK(s.b1 == 0.0f || std::fpclassify(s.b1) == FP_NORMAL);
    CHECK(e.Faults() == 0);
  }
  // NaN and infinity: the slot's guard clears it, and it plays again
  for (float bad : {NAN, INFINITY}) {
    for (int mix : {127, 0}) {
      Engine& e = fresh();
      set(0, 0, ins::kSat, kTape, 60, 64, mix);
      auto x = sine(300, 0.3f, 48000);
      for (int i = 2400; i < 2412; ++i) x[i] = bad;
      auto y = mono(e, x);
      CHECK(e.Faults() >= 1);
      for (size_t i = 7200; i < y.size(); ++i) CHECK(std::isfinite(y[i]));
      CHECK(rms(y, 24000) > 0.1f);
    }
  }
  // full scale: noise and a square at every corner of the knobs
  {
    float worst = 0;
    auto n = noise(1.0f, 48000);
    std::vector<float> q(48000);
    for (size_t i = 0; i < q.size(); ++i) q[i] = (i / 50) & 1 ? 1.0f : -1.0f;
    for (int c : kChar)
      for (int d : {1, 64, 127})
        for (int t : {0, 64, 127})
          for (int m : {1, 64, 127})
            for (auto* x : {&n, &q}) {
              Engine& e = fresh();
              set(0, 0, ins::kSat, c, d, t, m);
              auto y = mono(e, *x);
              for (float v : y) CHECK(std::isfinite(v));
              worst = std::max(worst, peak(y));
              CHECK(e.Faults() == 0);
            }
    printf("test_sat: full-scale noise and square at the corners: peak %.2f, no fault\n", worst);
    CHECK(worst < 4.0f);
  }
  // knobs thrown around under noise
  {
    Engine& e = fresh();
    auto y = noise(0.8f, 96000);
    uint32_t r = 99;
    for (size_t i = 0; i < y.size(); i += 12) {
      if (i % 96 == 0) {
        r = r * 1664525u + 1013904223u;
        set(0, 0, ins::kSat, (r >> 8) & 127, (r >> 16) & 127, (r >> 24) & 127, (r >> 4) & 127);
      }
      e.ProcessMono(0, &y[i], 12, ins::control);
    }
    for (float v : y) CHECK(std::isfinite(v));
    CHECK(e.Faults() == 0);
  }
  printf("test_sat: zero, denormals, NaN, infinity, full scale\n");
}

static void stereo() {
  for (int c : kChar) {
    auto l = noise(0.4f, 24000), r = sine(440, 0.3f, 24000);
    Engine& e = fresh();
    set(0, 0, ins::kSat, c, 70, 40, 100);
    auto m = mono(e, l);
    Engine& f = fresh();
    set(0, 0, ins::kSat, c, 70, 40, 100);
    auto a = l, b = r;
    for (size_t i = 0; i < a.size(); i += 12) f.ProcessVoiceStereo(0, &a[i], &b[i], 12, ins::control);
    CHECK(!memcmp(a.data(), m.data(), a.size() * sizeof(float)));   // L = the mono chain
    // a side stays a side: R silent stays silent
    Engine& g = fresh();
    set(ins::kBus0, 0, ins::kSat, c, 127, 64, 127);
    std::vector<float> by(2 * l.size(), 0.0f);
    for (size_t i = 0; i < l.size(); ++i) by[2 * i] = l[i];
    for (size_t i = 0; i < l.size(); i += 12) g.ProcessStereo(ins::kBus0, (float(*)[2])&by[2 * i], 12, ins::control);
    for (size_t i = 0; i < l.size(); ++i) CHECK(by[2 * i + 1] == 0.0f);
  }
  // the characters are three sounds: the same drive gives three different results
  auto x = sine(1000, 0.25f, 24000);
  std::vector<float> o[3];
  for (int ci = 0; ci < 3; ++ci) {
    Engine& e = fresh();
    set(0, 0, ins::kSat, kChar[ci], 100, 64, 127);
    o[ci] = mono(e, x);
  }
  for (int a = 0; a < 3; ++a) {
    std::vector<float> d(o[a].size());
    for (size_t i = 0; i < d.size(); ++i) d[i] = o[a][i] - o[(a + 1) % 3][i];
    printf("test_sat: %s against %s: difference %.1f dB\n", kCharName[a], kCharName[(a + 1) % 3], db(rms(d, 9600) / rms(o[a], 9600)));
    CHECK(rms(d, 9600) > 0.01f * rms(o[a], 9600));
  }
  printf("test_sat: L = the mono chain, a side stays a side\n");
}

static void cpu() {
  // two SATs a voice, the budget's case: 12 frames, one voice; then the same in stereo, and during a CHAR fade
  auto x = noise(0.4f, 12);
  float a[12], b[12];
  const int blocks = 40000;
  auto bench = [&](const char* nm, bool st, bool fade) {
    Engine& e = fresh();
    set(0, 0, ins::kSat, kTape, 90, 80, 100), set(0, 1, ins::kSat, kCons, 90, 80, 100);
    double us = 0;
    CHECK(timing_retry("SAT bench", [&] {
      auto t0 = std::chrono::steady_clock::now();
      for (int n = 0; n < blocks; ++n) {
        if (fade && n % 4 == 0) ins::control.play[0][0][0] = ins::control.play[0][1][0] = (uint8_t)kChar[(n / 4) % 3];
        memcpy(a, x.data(), sizeof a), memcpy(b, x.data(), sizeof b);
        if (st) e.ProcessVoiceStereo(0, a, b, 12, ins::control);
        else e.ProcessMono(0, a, 12, ins::control);
      }
      us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
      return us < 125.0 / 4.4;
    }));
    printf("CPU (Mac): %s: %.2f us a 12-frame block; the Pi 4 by the boot bench's ratio (x 4.4) ~%.1f us of its 125\n", nm, us, us * 4.4);
  };
  bench("a voice, SAT + SAT, mono", false, false);
  bench("a voice, SAT + SAT, stereo", true, false);
  bench("a voice, SAT + SAT, mono, CHAR fading", false, true);
  // core 2's worst: every slot of the buses and the master on SAT, stereo
  {
    Engine& e = fresh();
    for (int c = ins::kBus0; c < ins::kChains; ++c)
      for (int s = 0; s < ins::kSlots; ++s) set(c, s, ins::kSat, kTape, 90, 80, 100);
    float y[12][2];
    double us = 0;
    CHECK(timing_retry("SAT bus bench", [&] {
      auto t0 = std::chrono::steady_clock::now();
      for (int n = 0; n < blocks; ++n)
        for (int c = ins::kBus0; c < ins::kChains; ++c) {
          for (int i = 0; i < 12; ++i) y[i][0] = y[i][1] = x[i];
          e.ProcessStereo(c, y, 12, ins::control);
        }
      us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
      return us < 250.0 / 4.4 / 2;
    }));
    printf("CPU (Mac): every bus and master slot on SAT (10): %.2f us a 12-frame block, %.1f %% of a core; x 4.4 ~%.1f us of 250\n", us, us / 2.5, us * 4.4);
  }
}

int main() {
  ids_and_names();
  bus_page_reaches_sat();
  bit_exact();
  latency_and_dry();
  through_zero();
  aliasing();
  level();
  no_click();
  hostile_input();
  stereo();
  cpu();
  printf("test_sat: PASS\n");
  return 0;
}
