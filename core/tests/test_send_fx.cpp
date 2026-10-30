// core/tests/test_send_fx.cpp - the send FX (core/fx/send_fx.*): 0x20 parsing (sends, TYPE + params, tags,
// malformed), the console commands, 0x21 / 0x22 through host_sysex (0x23, a TYPE change, a user-FX slot from the
// project's fxslots file), the mix math (per-voice sends, shared voices, the squared level), chorus and comb impulse
// and sine responses, the TYPE change fade, the limits, no NaN / subnormals, and the CPU cost per block (the Mac as
// a proxy for a Pi core).
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "timing.h"
#include "engine.h"
#include "fx/send_fx.h"

using namespace core;
using namespace core::fx;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static std::vector<uint8_t> msg(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static bool take(const std::vector<uint8_t>& m) { return take_sendfx(m.data(), (int)m.size()); }
static void link(const std::vector<uint8_t>& m) { host_sysex(m.data(), (int)m.size()); }
static int count(uint8_t cmd) {
  int n = 0;
  for (auto& m : g_out) n += m[4] == cmd;
  return n;
}
static void clear_control() {
  memset((void*)&control, 0, sizeof control);
  kit_tag[0] = kit_tag[1] = 0;
}

static const float kSr = 48000.0f;
// n frames: stem v carries x(i), the rest 0; returns L/R
static std::vector<std::array<float, 2>> run(SendFx& fx, int n, int v, float (*x)(int), int t0 = 0) {
  std::vector<std::array<float, 2>> y(n);
  float st[SendFx::kMaxFrames][kStems], o[SendFx::kMaxFrames][2];
  for (int i = 0; i < n; i += 12) {
    int k = n - i < 12 ? n - i : 12;
    for (int j = 0; j < k; ++j) {
      for (int s = 0; s < kStems; ++s) st[j][s] = 0.0f;
      if (v >= 0) st[j][v] = x(t0 + i + j);
    }
    fx.Process(st, k, o, control);
    for (int j = 0; j < k; ++j) y[i + j] = {o[j][0], o[j][1]};
  }
  return y;
}
static float rms_v(const std::vector<float>& y, int from) {
  double s = 0;
  for (size_t i = from; i < y.size(); ++i) s += y[i] * y[i];
  return (float)std::sqrt(s / (y.size() - from));
}
static float rms(const std::vector<std::array<float, 2>>& y, int from) {
  double s = 0;
  for (size_t i = from; i < y.size(); ++i) s += y[i][0] * y[i][0];
  return (float)std::sqrt(s / (y.size() - from));
}
static float zero(int) { return 0.0f; }
static float impulse_at_0(int i) { return i == 0 ? 0.1f : 0.0f; }
static float g_f = 0.0f, g_a = 0.01f;
static float sine(int i) { return g_a * std::sin(6.283185307f * g_f * i / kSr); }
static int g_stop = 0;
static float sine_burst(int i) { return i < g_stop ? sine(i) : 0.0f; }
static uint32_t g_rng = 1;
static float noise(int) { return g_a * ((float)((g_rng = g_rng * 1664525u + 1013904223u) >> 8) / 8388608.0f - 1.0f); }
// a drum-ish send: every 250 ms a 55 Hz kick (30 ms decay) and a noise burst (6 ms decay), g_a x 0.3 / 0.15
static float drums(int i) {
  int t = i % 12000;
  float kick = t < 4800 ? std::sin(6.283185307f * 55.0f * t / kSr) * std::exp(-t / 1500.0f) : 0.0f;
  float a = g_a;
  g_a = 1.0f;
  float snare = t < 2400 ? noise(0) * std::exp(-t / 300.0f) : 0.0f;
  g_a = a;
  return g_a * (0.3f * kick + 0.15f * snare);
}

// the power of x[from..] at f (Goertzel)
static double goertzel(const std::vector<float>& x, size_t from, double f) {
  double w = 2 * M_PI * f / kSr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = from; i < x.size(); ++i) {
    double s0 = x[i] + c * s1 - s2;
    s2 = s1, s1 = s0;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;
}
// the strongest frequency of x[from..] in lo..hi (0.25 % steps)
static double peak_freq(const std::vector<float>& x, size_t from, double lo, double hi) {
  double best = lo, bp = -1;
  for (double f = lo; f <= hi; f *= 1.0025) {
    double p = goertzel(x, from, f);
    if (p > bp) bp = p, best = f;
  }
  return best;
}
// T60 from Schroeder's energy decay curve of x: the slope between -5 and -25 dB (T20 x 3)
static double t60_edc(const std::vector<float>& x) {
  std::vector<double> e(x.size() + 1, 0.0);
  for (size_t i = x.size(); i-- > 0;) e[i] = e[i + 1] + (double)x[i] * x[i];
  if (e[0] <= 0) return 0;
  size_t a = 0, b = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    double db = 10 * std::log10(e[i] / e[0] + 1e-30);
    if (!a && db < -5) a = i;
    if (!b && db < -25) { b = i; break; }
  }
  if (!a || !b) return 1e9;   // decays too slowly for the capture
  return 3.0 * (double)(b - a) / kSr;
}
// T60 of a pulse train / periodic decay: the energy of each period, a least-squares line through their dB (to -50 dB)
static double t60_periods(const std::vector<float>& x, double period) {
  std::vector<double> t, db;
  double top = 0;
  for (size_t w = 0; (w + 1) * period < x.size(); ++w) {
    double m = 0;
    for (size_t i = (size_t)(w * period); i < (size_t)((w + 1) * period); ++i) m += (double)x[i] * x[i];
    if (w == 1) top = m;
    if (w >= 1 && m > 0 && m > top * 1e-5) t.push_back((w + 0.5) * period / kSr), db.push_back(10 * std::log10(m));
  }
  double n = (double)t.size(), st = 0, sd = 0, stt = 0, std_ = 0;
  for (size_t i = 0; i < t.size(); ++i) st += t[i], sd += db[i], stt += t[i] * t[i], std_ += t[i] * db[i];
  double slope = (n * std_ - st * sd) / (n * stt - st * st);   // dB per second
  return slope < 0 ? -60.0 / slope : 1e9;
}
// a mono effect run directly: in (n) -> L + R; k = P1..P5
template <class Fx>
static std::vector<float> run_mono(Fx& fx, const std::vector<float>& in, const uint8_t* k, std::vector<float>* r = nullptr) {
  std::vector<float> y(in.size());
  if (r) r->assign(in.size(), 0.0f);
  float o[12][2];
  for (size_t i = 0; i < in.size(); i += 12) {
    int n = (int)std::min<size_t>(12, in.size() - i);
    for (int j = 0; j < n; ++j) o[j][0] = o[j][1] = 0;
    fx.Process(&in[i], n, o, k, kSr, 0.0f);
    for (int j = 0; j < n; ++j) {
      y[i + j] = o[j][0] + o[j][1];
      if (r) (*r)[i + j] = o[j][1];
    }
  }
  return y;
}
static std::vector<float> impulse(size_t n, float a = 0.5f, size_t at = 0) {
  std::vector<float> x(n, 0.0f);
  x[at] = a;
  return x;
}
static std::vector<float> chan(const std::vector<std::array<float, 2>>& y, int c) {
  std::vector<float> v(y.size());
  for (size_t i = 0; i < y.size(); ++i) v[i] = y[i][c];
  return v;
}

static bool clean(const std::vector<std::array<float, 2>>& y, float lim) {
  for (auto& f : y)
    for (float s : f)
      if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL || std::fabs(s) > lim) {
        fprintf(stderr, "unclean sample %g (limit %g)\n", s, lim);
        return false;
      }
  return true;
}

int main() {
  // ---- 0x20 parsing
  clear_control();
  CHECK(take(msg({0x20, 1, 0, 0x40, 0x7f})) && control.send[0][0] == 0x40 && control.send[0][1] == 0x7f);
  CHECK(take(msg({0x20, 1, 11, 1, 2})) && control.send[11][0] == 1 && control.send[11][1] == 2);
  // track 12: words 0, 29, 38, 39, 40, 41 -> FX1 = 29 38 39, FX2 = 0 40 41 (hi, lo)
  CHECK(take(msg({0x20, 1, 12, 2, 10, 1, 11, 12, 13, 14, 15, 16, 17, 18, 19})));
  const uint8_t k1[6] = {1, 11, 12, 13, 14, 15}, k2[6] = {2, 10, 16, 17, 18, 19};
  for (int j = 0; j < 6; ++j) CHECK(control.knob[0][j] == k1[j] && control.knob[1][j] == k2[j]);
  CHECK(take(msg({0x20, 1, 13, 0x12, 0x34, 0x7f, 0x7f})) && kit_tag[0] == (0x12 << 7 | 0x34) && kit_tag[1] == 0x3fff);
  uint32_t n0 = control.messages;
  CHECK(!take(msg({0x20, 2, 0, 1, 1})));                 // version
  CHECK(!take(msg({0x20, 1, 14, 1, 1})));                // track
  CHECK(!take(msg({0x20, 1, 0, 1})));                    // length
  CHECK(!take(msg({0x20, 1, 12, 1, 1})));
  CHECK(!take(msg({0x20, 1, 0, 0x80, 1})));              // a data byte above 0x7f
  CHECK(control.messages == n0 && control.send[0][0] == 0x40);

  // ---- names and ids
  CHECK(type_from_name("chorus") == 1 && type_from_name("COMB") == 2 && type_from_name("off") == 0);
  CHECK(type_from_name("rings") == 6 && type_from_name("64") == 64 && type_from_name("x") == -1);
  CHECK(builtin(16) && builtin(17) && type_from_name("crush") == 16 && type_from_name("ringmod") == 17);
  CHECK(builtin(1) && builtin(2) && builtin(6) && builtin(8) && builtin(15) && !builtin(7) && !builtin(64));
  CHECK(type_from_name("cloudseed") == 8 && strcmp(type_name(8), "CLOUDSEED") == 0);
  CHECK(strcmp(type_name(6), "RINGS") == 0 && strcmp(type_name(64), "UFX1") == 0);
  CHECK(fx_tag("x") != 0 && fx_tag("x") <= 0x3fff && fx_tag("DUSTY") != fx_tag("DUSTX"));

  // ---- console
  char r[400];
  clear_control();
  CHECK(command("sendfx=1:chorus", r, sizeof r) && control.knob[0][0] == kChorus && strstr(r, "fx1 CHORUS (1)"));
  CHECK(command("sendfx=2:rings", r, sizeof r) && control.knob[1][0] == 6 && state(1) == kStReady && !strstr(r, "MISSING"));
  CHECK(command("sendfx=2:cloudseed", r, sizeof r) && control.knob[1][0] == 8 && state(1) == kStReady);
  CHECK(command("sendfx=2:s4 reso", r, sizeof r) && control.knob[1][0] == 7 && state(1) == kStMissing && strstr(r, "MISSING"));
  CHECK(command("sendfx=2:9", r, sizeof r) && control.knob[1][0] == 9);
  CHECK(command("sendfx=3:comb", r, sizeof r) && strncmp(r, "sendfx=K", 8) == 0);   // usage
  card_dirty = false;
  CHECK(command("sendfxout=main", r, sizeof r) && control.route == kRouteMain && card_dirty && strstr(r, "forced main"));
  CHECK(command("sendfxout=fx", r, sizeof r) && control.route == kRouteFx);
  CHECK(command("sendfxout=slot", r, sizeof r) && control.route == kRouteSlot && strstr(r, "per slot"));
  CHECK(command("sendlvl=1:all:100", r, sizeof r) && control.send[5][0] == 100 && control.send[5][1] == 0);
  CHECK(command("sendlvl=2:3:7", r, sizeof r) && control.send[3][1] == 7 && control.send[2][1] == 0);
  CHECK(command("sendlvl=2:12:7", r, sizeof r) && strncmp(r, "sendlvl=K", 9) == 0);
  CHECK(command("sendknob=2:5:127", r, sizeof r) && control.knob[1][5] == 127);
  CHECK(command("sendknob=2:6:1", r, sizeof r) && strncmp(r, "sendknob=K", 10) == 0);
  CHECK(command("sendfx=off", r, sizeof r) && !any_on());
  CHECK(command("sendfx", r, sizeof r) && strstr(r, "fx1 OFF (0)"));
  CHECK(!command("out=main", r, sizeof r));

  // ---- the host link: 0x23, a TYPE change, the project's user-FX slots
  char tmpl[] = "/tmp/core-sendfx-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  {
    Fs fs;
    CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.rytm/fxslots"));
    File f;
    CHECK(f.open("/.rytm/fxslots/DEMO 1.txt", kWrite));
    const char* t = "\n1a2b DUSTY\n";   // UFX1 empty, UFX2 = DUSTY (tag 0x1a2b)
    f.write(t, strlen(t));
    f.close();
    CHECK(f.open("/.rytm/sendfx.txt", kWrite));
    f.write("out=main\n", 9);
    f.close();
  }
  hooks.to_rytm = out;
  clear_control();
  CHECK(engine_begin());
  CHECK(control.route == kRouteMain);
  g_out.clear();
  link(msg({0x10, 'D', 'E', 'M', 'O', ' ', '1'}));
  CHECK(user[1].tag == 0x1a2b && strcmp(user[1].name, "DUSTY") == 0 && user[0].tag == 0);
  CHECK(count(0x21) == 2 && count(0x22) == 2);
  CHECK(count(0x24) == 8);   // 9 labelled TYPEs (CHORUS COMB SPACE RINGS CLOUDSEED STUTTER CRUSH RINGMOD HALO), the Rytm keeps 8:
  // the slots' TYPEs first (both OFF here), then by id: HALO (18) left out; one 24 each, after the 21, before the 22s
  {
    int first24 = -1, last24 = -1, first22 = -1, first21 = -1;
    for (int i = 0; i < (int)g_out.size(); ++i) {
      if (g_out[i][4] == 0x21 && first21 < 0) first21 = i;
      if (g_out[i][4] == 0x24 && first24 < 0) first24 = i;
      if (g_out[i][4] == 0x24) last24 = i;
      if (g_out[i][4] == 0x22 && first22 < 0) first22 = i;
    }
    CHECK(first21 >= 0 && first21 < first24 && last24 < first22);
    for (auto& m : g_out)
      if (m[4] == 0x24) {
        CHECK(m.size() <= 128 && (m.size() - 24) % 8 == 0 && m.back() == 0xF7);
        if (m[6] == kRings) CHECK(memcmp(&m[7 + 4], "MODL", 4) == 0 && m[23] == (1 | 4) && memcmp(&m[26], "MODAL", 5) == 0);
        if (m[6] == kStutter) CHECK(m.size() == 24 + 12 * 8);
      }
  }
  {
    uint8_t m[160];
    CHECK(sysex_labels(m, kOff) == 0 && sysex_labels(m, 7) == 0 && sysex_labels(m, 65) == 0);   // none: not played
    CHECK(labels(kChorus) && labels(kChorus)->nzones == 0 && !labels(3));
  }
  {   // F0 7D 52 54 21 01 <replace> <total> {<id> <13>} F7, 8 a message: OFF CHORUS COMB SPACE RINGS CLOUDSEED
      // STUTTER CRUSH, then RINGMOD HALO DUSTY(65)
    std::vector<std::vector<uint8_t>> nm;
    for (auto& m : g_out)
      if (m[4] == 0x21) nm.push_back(m);
    CHECK(nm.size() == 2);
    const uint8_t want[11] = {0, 1, 2, 5, 6, 8, 15, 16, 17, 18, 65};
    const char* names[11] = {"OFF", "CHORUS", "COMB", "SPACE", "RINGS", "CLOUDSEED", "STUTTER", "CRUSH", "RINGMOD", "HALO",
                             "DUSTY"};
    if (nm.size() == 2) {
      CHECK(nm[0][6] == 1 && nm[0][7] == 11 && nm[0].size() == 9 + 8 * 14);
      CHECK(nm[1][6] == 0 && nm[1][7] == 11 && nm[1].size() == 9 + 3 * 14);
      for (int e = 0; e < 11; ++e) {
        const std::vector<uint8_t>& m = nm[e / 8];
        int o = 8 + (e % 8) * 14;
        CHECK(m[o] == want[e] && memcmp(&m[o + 1], names[e], strlen(names[e])) == 0 && m[o + 1 + strlen(names[e])] == 0);
      }
    }
  }
  link(msg({0x23, 1}));   // an ASK first: no unasked names/labels burst on the first 0x20 below
  g_out.clear();
  link(msg({0x20, 1, 12, 0, 0, 65, 0, 0, 0, 0, 0, 0, 0, 0, 0}));   // FX1 TYPE 65 (word 29 hi)
  CHECK(control.knob[0][0] == 65 && count(0x22) == 1);
  // 22 <ver> <fx> <id> <state> <name 10>
  CHECK(g_out[0].size() == 20 && g_out[0][6] == 0 && g_out[0][7] == 65 && g_out[0][8] == kStMissing &&
        memcmp(&g_out[0][9], "DUSTY", 5) == 0);
  g_out.clear();
  link(msg({0x20, 1, 12, 0, 0, 65, 3, 0, 0, 0, 0, 0, 0, 0, 0}));   // P1 only: no new state
  CHECK(count(0x22) == 0);
  link(msg({0x20, 1, 0, 1, 2}));                                     // a track's sends: none
  CHECK(count(0x22) == 0 && control.send[0][1] == 2);
  link(msg({0x23, 1}));
  CHECK(count(0x21) == 2 && count(0x22) == 2 && count(0x24) == 8);
  {   // FX 1 to HALO, whose labels did not fit the Rytm's 8 tables: the list and the labels again, HALO's first
    for (auto& m : g_out) CHECK(m[4] != 0x24 || m[6] != kHalo);
    g_out.clear();
    link(msg({0x20, 1, 12, 0, 0, kHalo, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(count(0x21) == 2 && count(0x24) == 8 && count(0x22) == 2);
    int first24 = -1;
    for (int i = 0; i < (int)g_out.size() && first24 < 0; ++i)
      if (g_out[i][4] == 0x24) first24 = i;
    CHECK(first24 >= 0 && g_out[first24][6] == kHalo);
    g_out.clear();
    link(msg({0x20, 1, 12, 0, 0, kHalo, 5, 0, 0, 0, 0, 0, 0, 0, 0}));   // HALO stays (P1 moved): nothing again
    CHECK(count(0x21) == 0 && count(0x24) == 0);
    link(msg({0x20, 1, 12, 0, 0, kTape, 0, 0, 0, 0, 0, 0, 0, 0, 0}));   // to SPACE: sent this time, a 22 only
    CHECK(count(0x21) == 0 && count(0x24) == 0 && count(0x22) == 1);
    link(msg({0x20, 1, 12, 0, 0, 65, 0, 0, 0, 0, 0, 0, 0, 0, 0}));     // back as before
  }
  {   // fix1049: a rui1049 Rytm (its hello's caps bit 1) keeps 16 label tables: all 9 labelled TYPEs go, HALO too; an
      // older hello (caps 1) the 8 again
    g_out.clear();
    link(msg({0x35, 1, 0, 100, 3}));   // the hello itself: the labels again at once, the 16 tables filled
    CHECK(count(0x24) == 9);
    g_out.clear();
    link(msg({0x23, 1}));
    bool halo = false;
    for (auto& m : g_out) halo = halo || (m[4] == 0x24 && m[6] == kHalo);
    CHECK(count(0x24) == 9 && halo);
    link(msg({0x35, 1, 0, 100, 1}));
    g_out.clear();
    link(msg({0x23, 1}));
    CHECK(count(0x24) == 8);
  }
  command("sendfxout=fx", r, sizeof r);
  CHECK(idle_save() && !card_dirty);
  {
    File f;
    char b[16] = {0};
    CHECK(f.open("/.rytm/sendfx.txt", kRead) && f.read(b, 15) == 7 && strcmp(b, "out=fx\n") == 0);
  }

  // ---- the mix math (comb: linear below the limit's knee; P3 0 -> T60 0.2 s)
  clear_control();
  control.knob[0][0] = kComb;
  control.knob[0][2] = 48;   // P2 tune: 55 * 2^2 = 220 Hz
  g_f = 220.0f;
  g_a = 0.001f;
  float lvl[kStems];
  for (int v = 0; v < kStems; ++v) {   // each stem through its track's SEND FX 1 = 127, the others 0
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;
    clear_control();
    control.knob[0][0] = kComb;
    control.knob[0][2] = 48;
    control.send[kStemTracks[v][0]][0] = 127;
    auto y = run(fx, 24000, v, sine);
    lvl[v] = rms(y, 12000);
    CHECK(lvl[v] > 0.0005f);
    if (v) CHECK(std::fabs(lvl[v] - lvl[0]) < 1e-6f);   // every voice the same path
  }
  {
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;   // a shared voice (stem 2 = RS/CP) takes the louder track: CP's
    clear_control();
    control.knob[0][0] = kComb, control.knob[0][2] = 48, control.send[3][0] = 127;
    CHECK(std::fabs(rms(run(fx, 24000, 2, sine), 12000) - lvl[0]) < 1e-6f);
    auto fx2_u = std::make_unique<SendFx>(kSr);
    SendFx& fx2 = *fx2_u;   // send 2 does not feed FX 1
    control.send[3][0] = 0, control.send[3][1] = 127;
    CHECK(rms(run(fx2, 24000, 2, sine), 12000) == 0.0f);
    auto fx3_u = std::make_unique<SendFx>(kSr);
    SendFx& fx3 = *fx3_u;   // the level squared: 64 -> (64/127)^2
    control.send[3][0] = 64;
    float q = rms(run(fx3, 24000, 2, sine), 12000) / lvl[0];
    CHECK(std::fabs(q - (64.0f / 127) * (64.0f / 127)) < 0.002f);
  }

  // ---- comb: impulse = a pulse train at the tuned period; resonance against an off-tune sine
  {
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;
    clear_control();
    control.knob[0][0] = kComb, control.knob[0][2] = 0, control.knob[0][3] = 63;   // 55 Hz, T60 8 s, no damping
    control.send[0][0] = 127;
    run(fx, 4800, 0, zero);   // the smoothers settle
    auto y = run(fx, 9600, 0, impulse_at_0);
    float period = kSr / 55.0f;   // 872.7
    int peaks[3] = {0, 0, 0};
    for (int k = 1; k <= 3; ++k) {
      int best = 0;
      for (int i = (int)(k * period) - 3; i <= (int)(k * period) + 3; ++i)
        if (std::fabs(y[i][0]) > std::fabs(y[best][0])) best = i;
      peaks[k - 1] = best;
      CHECK(std::fabs(y[best][0]) > 0.0005f);
    }
    CHECK(std::abs(peaks[1] - peaks[0] - (int)std::lround(period)) <= 1);
    CHECK(std::fabs(y[peaks[0] / 2][0]) < 1e-6f);   // nothing between the pulses
    printf("test_send_fx: comb impulse peaks %d %d %d (period %.1f)\n", peaks[0], peaks[1], peaks[2], period);
    auto a_u = std::make_unique<SendFx>(kSr), b_u = std::make_unique<SendFx>(kSr);
    SendFx &a = *a_u, &b = *b_u;
    g_a = 0.001f, g_f = 55.0f;
    float on = rms(run(a, 48000, 0, sine), 24000);
    g_f = 55.0f * 1.5f;
    float off = rms(run(b, 48000, 0, sine), 24000);
    printf("test_send_fx: comb 55 Hz sine on/off tune %.1f dB\n", 20 * std::log10(on / off));
    CHECK(on > 10.0f * off);
  }

  // ---- comb: P3 is the decay time; the ring is as long at 55 Hz as at ~550 Hz (T60 from the impulse's decay curve)
  for (int tune : {0, 80}) {   // 55 Hz, 554 Hz
    for (int dec : {0, 32, 60, 96}) {   // T60 0.2, 1.30, 6.6 s; 96 = 1.30 s negative (an octave down: f0 / 2)
      auto fx_u = std::make_unique<SendFx>(kSr);
      SendFx& fx = *fx_u;
      clear_control();
      control.knob[0][0] = kComb, control.knob[0][2] = (uint8_t)tune, control.knob[0][3] = (uint8_t)dec;
      control.send[0][0] = 127;
      run(fx, 4800, 0, zero);
      double want = 0.2 * std::pow(40.0, (dec & 63) / 63.0), f0 = 55 * std::exp2(tune / 24.0) / (dec >= 64 ? 2 : 1);
      // the free ring of the comb's fundamental after a sine at it (an impulse also loses its highs in the
      // linear-interpolated loop, a fraction of a dB a pass)
      g_a = 0.01f, g_f = (float)f0, g_stop = (int)kSr;
      auto all = chan(run(fx, (int)(kSr * (want * 1.2 + 1.2)), 0, sine_burst), 0);
      std::vector<float> y(all.begin() + g_stop, all.end());
      double t60 = t60_periods(y, kSr / f0);
      printf("test_send_fx: comb %.1f Hz P3 %d: T60 %.3f s (want %.3f)\n", f0, dec, t60, want);
      CHECK(std::fabs(t60 / want - 1) < 0.05);
    }
  }

  // ---- RINGS: an impulse rings at P2's pitch (each model); a strum moves to the next voice, the old one rings on
  {
    struct M { uint8_t p1; const char* name; } models[] = {{0, "modal"}, {32, "modal mono"}, {64, "strings"}, {96, "chord"}};
    for (auto& m : models)
      for (int note : {45, 57, 69}) {   // A2 A3 A4
        auto r_u = std::make_unique<RingsFx>();
        RingsFx& r = *r_u;
        uint8_t k[4] = {(uint8_t)(2 * (note - 36)), m.p1, 0, 0};   // P2 pitch, P3 model zone (structure 0)
        double f0 = 440.0 * std::exp2((note - 69) / 12.0);
        auto y = run_mono(r, impulse(24000), k);
        double pk = peak_freq(y, 2400, f0 * 0.8, f0 * 1.25);
        double on = goertzel(y, 2400, f0), off = goertzel(y, 2400, f0 * std::exp2(1.5 / 12));
        float mx = 0;
        for (float v : y) mx = std::fmax(mx, std::fabs(v));
        printf("test_send_fx: rings %s note %d: peak %.1f Hz (want %.1f), on/off %.1f dB, max %.3f\n", m.name, note, pk,
               f0, 10 * std::log10(on / off), mx);
        CHECK(std::fabs(pk / f0 - 1) < 0.015 && on > 4 * off && mx > 0.002f && mx < 1.0f && r.Strums() == 1);
      }
    // modal, 2 voices: A3 hit, P2 to A4, a second hit: both ring
    auto r_u = std::make_unique<RingsFx>();
    RingsFx& r = *r_u;
    uint8_t k[4] = {2 * (57 - 36), 0, 0, 64};
    run_mono(r, impulse(14400), k);
    k[0] = 2 * (69 - 36);
    auto y = run_mono(r, impulse(24000), k);
    double a3 = goertzel(y, 2400, 220), a4 = goertzel(y, 2400, 440), mid = goertzel(y, 2400, 311);
    printf("test_send_fx: rings two strums: %u, A3 %.1f dB, A4 %.1f dB over D#4\n", r.Strums(), 10 * std::log10(a3 / mid),
           10 * std::log10(a4 / mid));
    CHECK(r.Strums() == 2 && a3 > 4 * mid && a4 > 4 * mid);
    // a steady sine (no onsets after the first) keeps exciting the same voice: one strum
    auto s_u = std::make_unique<RingsFx>();
    std::vector<float> sn(48000);
    for (size_t i = 0; i < sn.size(); ++i) sn[i] = 0.1f * std::sin(6.283185307f * 220.0f * i / kSr);
    uint8_t k0[4] = {2 * (57 - 36), 0, 0, 0};
    auto z = run_mono(*s_u, sn, k0);
    printf("test_send_fx: rings, a steady 220 Hz sine: %u strum(s), rms %.4f\n", s_u->Strums(), rms_v(z, 24000));
    CHECK(s_u->Strums() <= 1 && rms_v(z, 24000) > 0.001f);
  }

  // ---- CLOUDSEED: the decay follows P3 (x10^(P3/127) of the preset's T60) in every preset; every knob at 0 plays
  {
    const char* names[4] = {"room", "hall", "dark plate", "ambience"};
    const double base[4] = {0.75, 2.4, 3.6, 7.0};
    for (int p = 0; p < 4; ++p) {
      double t[2];
      int p3s[2] = {0, 40};
      for (int j = 0; j < 2; ++j) {
        auto c_u = std::make_unique<CloudSeedFx>();
        uint8_t k[4] = {(uint8_t)(32 * p), (uint8_t)p3s[j], 0, 0};   // P2 character zone (size x1), P3 decay
        double want = base[p] * std::pow(10.0, p3s[j] / 127.0);
        auto y = run_mono(*c_u, impulse((size_t)(kSr * (want * 0.6 + 0.5))), k);
        t[j] = t60_edc(y);
        float mx = 0;
        for (float v : y) mx = std::fmax(mx, std::fabs(v));
        printf("test_send_fx: cloudseed %s P3 %d: T60 %.2f s (preset x P3: %.2f), max %.4f\n", names[p], p3s[j], t[j],
               want, mx);
        CHECK(t[j] > 0.5 * want && t[j] < 1.5 * want && mx > 1e-3f && mx < 0.5f);
      }
      double ratio = t[1] / t[0], want = std::pow(10.0, 40 / 127.0);
      CHECK(std::fabs(ratio / want - 1) < 0.35);
    }
    // stereo: L and R decorrelated (Cloud Seed's cross seed)
    auto c_u = std::make_unique<CloudSeedFx>();
    uint8_t k[4] = {32, 0, 0, 0};
    std::vector<float> rr;
    auto y = run_mono(*c_u, impulse(48000), k, &rr);
    double lr = 0, ll = 0, r2 = 0;
    for (size_t i = 4800; i < y.size(); ++i) {
      double l = y[i] - rr[i];
      lr += l * rr[i], ll += l * l, r2 += (double)rr[i] * rr[i];
    }
    printf("test_send_fx: cloudseed L/R correlation %.3f; %zu B a slot\n", lr / std::sqrt(ll * r2), sizeof(CloudSeedFx));
    CHECK(std::fabs(lr / std::sqrt(ll * r2)) < 0.3);
  }

  // ---- the new TYPEs through SendFx: READY, fades both ways, a left CLOUDSEED cleared over many blocks (never in one),
  // a switch back waits for the clear and starts clean, a P1 change fades like a TYPE change
  {
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;
    clear_control();
    control.send[1][0] = 127;
    g_a = 0.05f, g_f = 300.0f;
    const uint8_t seq[] = {kChorus, kRings, kCloudSeed, kComb, kRings, kChorus};
    int t0 = 0;
    auto prev = run(fx, 2400, 1, sine, t0);
    t0 += 2400;
    for (uint8_t t : seq) {
      control.knob[0][0] = t;
      CHECK(state(0) == kStReady);
      auto b = run(fx, 9600, 1, sine, t0);
      t0 += 9600;
      float maxstep = 0, minabs = 1, p = prev.back()[0];
      for (int i = 0; i < 9600; ++i) {
        maxstep = std::fmax(maxstep, std::fabs(b[i][0] - p));
        p = b[i][0];
        if (i > 300 && i < 700) minabs = std::fmin(minabs, std::fabs(b[i][0]));
      }
      printf("test_send_fx: TYPE -> %s: max step %.4f, min |y| at the switch %.6f, rms after %.4f\n", type_name(t),
             maxstep, minabs, rms(b, 4800));
      CHECK(maxstep < 0.02f && minabs < 2e-4f && rms(b, 4800) > 0.0005f);   // minabs: a 300 Hz sine sampled near a crossing (CHORUS from OFF: 1.03e-4 with the block ramps)
      prev = b;
    }
    // CLOUDSEED with a loud input, then COMB (no input from here): the reverb is cleared in pieces
    control.knob[0][0] = kCloudSeed;
    g_a = 0.2f;
    run(fx, 48000, 1, noise);
    control.knob[0][0] = kComb;
    float st[12][kStems] = {}, o[12][2];
    int blocks_clearing = 0;
    double worst = 0, typical = 0;
    for (int b = 0; b < 400; ++b) {
      auto c0 = std::chrono::steady_clock::now();
      fx.Process(st, 12, o, control);
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - c0).count();
      if (fx.Clearing(0)) ++blocks_clearing, worst = std::fmax(worst, us);
      else if (b > 200) typical = std::fmax(typical, us);
    }
    printf("test_send_fx: CLOUDSEED left: cleared over %d blocks (%d B a 12-frame block), worst block %.2f us "
           "(comb alone %.2f us)\n",
           blocks_clearing, 12 * kClearBytesPerFrame, worst, typical);
    CHECK(blocks_clearing >= 40 && blocks_clearing < 120 && !fx.Clearing(0));
    // back and forth fast: CLOUDSEED (loud), COMB, CLOUDSEED again while it is still being cleared: silence, then
    // CLOUDSEED from a clean state (no input: exactly silent, no old tail)
    control.knob[0][0] = kCloudSeed;
    run(fx, 48000, 1, noise);
    control.knob[0][0] = kComb;
    run(fx, 600, -1, zero);
    CHECK(fx.Clearing(0));
    control.knob[0][0] = kCloudSeed;
    auto z = run(fx, 24000, -1, zero);
    float mz = 0;
    for (auto& f : z) mz = std::fmax(mz, std::fmax(std::fabs(f[0]), std::fabs(f[1])));
    printf("test_send_fx: CLOUDSEED back while clearing: max |y| %.6f after the comb's fade\n", mz);
    float tail = 0;
    for (int i = 600; i < 24000; ++i) tail = std::fmax(tail, std::fabs(z[i][0]));
    CHECK(tail == 0.0f && !fx.Clearing(0));
    // a structure zone change (RINGS P3: MODAL -> STRINGS, p-lockable) fades out and in; P1 (ROUTE) changes nothing
    control.knob[0][0] = kRings, control.knob[0][3] = 0;
    g_a = 0.05f;
    auto a = run(fx, 24000, 1, sine, t0);
    control.knob[0][3] = 64;
    auto b = run(fx, 4800, 1, sine, t0 + 24000);
    float maxstep = 0, minabs = 1, p = a.back()[0];
    for (int i = 0; i < 4800; ++i) {
      maxstep = std::fmax(maxstep, std::fabs(b[i][0] - p));
      p = b[i][0];
      if (i > 300 && i < 1200) minabs = std::fmin(minabs, std::fabs(b[i][0]));
    }
    printf("test_send_fx: RINGS P3 modal -> strings: max step %.4f, min |y| %.6f\n", maxstep, minabs);
    CHECK(maxstep < 0.02f && minabs < 1e-4f && rms(b, 2400) > 0.0005f);
    // CLOUDSEED's character (P2 zone, p-lockable): HALL -> AMBIENCE fades out, waits for the clear, fades in
    control.knob[0][0] = kCloudSeed, control.knob[0][2] = 32;
    a = run(fx, 24000, 1, sine, t0 + 28800);
    control.knob[0][2] = 96;
    b = run(fx, 9600, 1, sine, t0 + 52800);
    maxstep = 0, minabs = 1, p = a.back()[0];
    for (int i = 0; i < 9600; ++i) {
      maxstep = std::fmax(maxstep, std::fabs(b[i][0] - p));
      p = b[i][0];
      if (i > 300 && i < 700) minabs = std::fmin(minabs, std::fabs(b[i][0]));
    }
    printf("test_send_fx: CLOUDSEED P2 hall -> ambience: max step %.4f, min |y| %.6f\n", maxstep, minabs);
    CHECK(maxstep < 0.02f && minabs < 1e-4f && rms(b, 4800) > 0.0005f);
  }

  // ---- chorus: the impulse arrives after delay - depth, both sides, never before; a sine stays a sine-ish level
  {
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;
    clear_control();
    control.knob[0][0] = kChorus;   // P all 0: 0.2 Hz, 25 % of 6 ms, 8 ms
    control.send[1][0] = 127;
    run(fx, 4800, 1, zero);
    auto y = run(fx, 4800, 1, impulse_at_0);
    int first_l = -1, first_r = -1;
    for (int i = 0; i < 4800; ++i) {
      if (first_l < 0 && std::fabs(y[i][0]) > 1e-6f) first_l = i;
      if (first_r < 0 && std::fabs(y[i][1]) > 1e-6f) first_r = i;
    }
    printf("test_send_fx: chorus impulse L %d R %d samples\n", first_l, first_r);
    CHECK(first_l >= 384 - 72 - 2 && first_l <= 384 + 72 + 2 && first_r >= 384 - 74 && first_r <= 384 + 74);
    auto s_u = std::make_unique<SendFx>(kSr);
    SendFx& s = *s_u;
    g_a = 0.01f, g_f = 440.0f;
    float g = rms(run(s, 48000, 1, sine), 24000) / (0.01f / std::sqrt(2.0f));
    CHECK(g > 0.5f && g < 1.5f);   // wet only, unity level (P5 0)
  }

  // ---- the TYPE change: fades out, the new effect from silence, fades in; no step
  {
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;
    clear_control();
    control.knob[0][0] = kChorus;
    control.send[1][0] = 127;
    g_a = 0.05f, g_f = 300.0f;
    auto a = run(fx, 24000, 1, sine);
    control.knob[0][0] = kComb;
    auto b = run(fx, 4800, 1, sine, 24000);
    float maxstep = 0, minabs = 1;
    float prev = a.back()[0];
    for (int i = 0; i < 4800; ++i) {
      maxstep = std::fmax(maxstep, std::fabs(b[i][0] - prev));
      prev = b[i][0];
      if (i > 300 && i < 700) minabs = std::fmin(minabs, std::fabs(b[i][0]));
    }
    printf("test_send_fx: TYPE change max step %.4f, min |y| at the switch %.6f\n", maxstep, minabs);
    CHECK(maxstep < 0.01f && minabs < 1e-4f && rms(b, 2400) > 0.001f);
  }

  // ---- the TYPE change on the Pi's (12-frame) and the Teensy's (42-frame) blocks, and the sim's 28 / 35: with the Rytm's
  // knob values (CHORUS -> COMB), the largest step through the fade is no larger than the steady CHORUS's
  for (int bs : {12, 42, 28, 35}) {
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;
    clear_control();
    const uint8_t kn[6] = {1, 0, 16, 8, 24, 0};
    for (int j = 0; j < 6; ++j) control.knob[0][j] = kn[j];
    control.send[1][0] = 112;
    g_a = 0.2f, g_f = 220.0f;
    std::vector<float> y;
    float st[SendFx::kMaxFrames][kStems], o[SendFx::kMaxFrames][2];
    auto blocks = [&](int n) {
      for (int done = 0, t = (int)y.size(); done < n; done += bs) {
        int k = std::min(bs, n - done);
        for (int j = 0; j < k; ++j) {
          for (int s = 0; s < kStems; ++s) st[j][s] = 0.0f;
          st[j][1] = sine(t + done + j);
        }
        fx.Process(st, k, o, control);
        for (int j = 0; j < k; ++j) y.push_back(o[j][1]);
      }
    };
    blocks(24000);
    float steady = 0;
    for (size_t i = 12000; i < 23000; ++i) steady = std::fmax(steady, std::fabs(y[i + 1] - y[i]));
    control.knob[0][0] = kComb;
    size_t sw = y.size();
    blocks(9600);
    float maxstep = 0, minabs = 1;
    for (size_t i = sw - 1; i < sw + 4000; ++i) maxstep = std::fmax(maxstep, std::fabs(y[i + 1] - y[i]));
    for (size_t i = sw + 600; i < sw + 700; ++i) minabs = std::fmin(minabs, std::fabs(y[i]));
    printf("test_send_fx: TYPE change at %d-frame blocks: max step %.5f (steady CHORUS %.5f), min |y| %.6f\n", bs,
           maxstep, steady, minabs);
    CHECK(maxstep <= 1.05f * steady && minabs < 1e-4f);
  }

  // ---- limits, NaN, subnormals: loud noise, then a long silence (every pair of effects, every knob at 127)
  const uint8_t pairs[][2] = {{1, 2}, {2, 1}, {6, 8}, {8, 6}, {8, 8}, {6, 6}};
  for (int route = kRouteFx; route <= kRouteMain; ++route)
    for (auto& pr : pairs) {
      auto fx_u = std::make_unique<SendFx>(kSr);
      SendFx& fx = *fx_u;
      clear_control();
      control.route = (uint8_t)route;
      control.knob[0][0] = pr[0], control.knob[1][0] = pr[1];
      for (int j = 1; j < 6; ++j) control.knob[0][j] = control.knob[1][j] = 127;
      for (int tr = 0; tr < kTracks; ++tr) control.send[tr][0] = control.send[tr][1] = 127;
      g_a = 50.0f;
      float lim = route == kRouteMain ? kLimitMain : kLimitFx;
      run(fx, 480, -1, zero);   // the pair crossfade (8 ms) settles on a forced route first
      auto y = run(fx, 48000, 0, noise);
      CHECK(clean(y, lim));
      float peak = 0;
      for (auto& f : y) peak = std::fmax(peak, std::fabs(f[0]));
      // the comb and chorus reach the limit; RINGS stops below it (its own limiter, ~0.4) and CLOUDSEED spreads
      printf("test_send_fx: limits %s + %s route %d: peak %.3f (limit %.2f)\n", type_name(pr[0]), type_name(pr[1]), route,
             peak, lim);
      CHECK(peak > (pr[0] <= 2 ? 0.9f : 0.3f) * lim);
      auto z = run(fx, 48000 * 20, -1, zero);   // RINGS and CLOUDSEED at P5 / P3 127 ring for minutes: no 0 check
      CHECK(clean(z, lim) && fx.Faults() == 0);
      if (pr[0] <= 2 && pr[1] <= 2) CHECK(z.back()[0] == 0.0f);
    }
  {   // every knob at 0: each effect plays a usable level (wet, unity-ish for a sine)
    for (uint8_t t : {kChorus, kComb, kRings, kCloudSeed}) {
      auto fx_u = std::make_unique<SendFx>(kSr);
      clear_control();
      control.knob[0][0] = t, control.send[0][0] = 127;
      g_a = 1.0f;
      std::vector<float> d(96000);
      for (int i = 0; i < 96000; ++i) d[i] = drums(i);
      g_rng = 1;
      float g = rms(run(*fx_u, 96000, 0, drums), 48000) / rms_v(d, 48000);
      printf("test_send_fx: %s, knobs 0: gain %.2f for a drum send (rms out / in)\n", type_name(t), g);
      CHECK(g > 0.1f && g < 4.0f);
    }
  }

  // ---- CPU per block (12 frames, all sends up): the Mac as a proxy for a Pi 4 core. M2 measured chorus + comb at
  // 0.28 us here and 3.7 us on the Pi (2026-10-16 hardware test): x13.2
  {
    const double kPiRatio = 3.7 / 0.28, kBlockUs = 12 / kSr * 1e6;
    struct C { uint8_t t1, p1a, t2, p1b; const char* name; } cases[] = {   // p1a / p1b: the structure knob
        {kChorus, 0, kComb, 0, "chorus + comb"},
        {kRings, 0, kOff, 0, "rings modal"},
        {kRings, 32, kOff, 0, "rings modal mono"},
        {kRings, 64, kOff, 0, "rings strings"},
        {kCloudSeed, 0, kOff, 0, "cloudseed room"},
        {kCloudSeed, 32, kOff, 0, "cloudseed hall"},
        {kCloudSeed, 64, kOff, 0, "cloudseed dark plate"},
        {kCloudSeed, 96, kOff, 0, "cloudseed ambience"},
        {kRings, 64, kRings, 64, "rings strings x2"},
        {kCloudSeed, 96, kCloudSeed, 96, "cloudseed ambience x2"},
        {kCloudSeed, 96, kRings, 64, "cloudseed ambience + rings strings"},
    };
    for (auto& cs : cases) {
      auto fx_u = std::make_unique<SendFx>(kSr);
      SendFx& fx = *fx_u;
      clear_control();
      control.knob[0][0] = cs.t1, control.knob[1][0] = cs.t2;
      control.knob[0][structure_knob(cs.t1) ? structure_knob(cs.t1) : 1] = cs.p1a;
      control.knob[1][structure_knob(cs.t2) ? structure_knob(cs.t2) : 1] = cs.p1b;
      for (int tr = 0; tr < kTracks; ++tr) control.send[tr][0] = control.send[tr][1] = 100;
      float st[12][kStems], o[12][2];
      const int blocks = 40000;   // 10 s of audio
      float acc = 0;
      for (int b = 0; b < 2000; ++b) {   // warm (the fade in, the caches)
        for (int j = 0; j < 12; ++j)
          for (int s2 = 0; s2 < kStems; ++s2) st[j][s2] = 0.01f * (float)(((b * 12 + j) * 7 + s2 * 3) % 11 - 5);
        fx.Process(st, 12, o, control);
      }
      CHECK(timing_retry(cs.name, [&] {   // (wall-clock: up to 3 tries - tests/timing.h)
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) {
          for (int j = 0; j < 12; ++j)
            for (int s2 = 0; s2 < kStems; ++s2) st[j][s2] = 0.01f * (float)(((b * 12 + j) * 7 + s2 * 3) % 11 - 5);
          fx.Process(st, 12, o, control);
          acc += o[5][1];
        }
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
        printf("test_send_fx: CPU %-36s %6.0f ns a 12-frame block (Mac %.2f %%; Pi 4 est. %.1f us, %.1f %% of a core) [%g]\n",
               cs.name, us * 1000, 100 * us / kBlockUs, us * kPiRatio, 100 * us * kPiRatio / kBlockUs, acc * 0);
        return us < 50.0;
      }));
    }
  }
  {   // a p-lock on every block (P2..P5 of CLOUDSEED moving): the reconfiguration's cost a block
    auto fx_u = std::make_unique<SendFx>(kSr);
    clear_control();
    control.knob[0][0] = kCloudSeed, control.knob[0][2] = 96;
    for (int tr = 0; tr < kTracks; ++tr) control.send[tr][0] = 100;
    float st[12][kStems] = {}, o[12][2];
    st[0][0] = 0.1f;
    for (int b = 0; b < 200; ++b) fx_u->Process(st, 12, o, control);
    CHECK(timing_retry("cloudseed, a knob moved every block", [&] {   // (wall-clock: up to 3 tries - tests/timing.h)
      auto t0 = std::chrono::steady_clock::now();
      for (int b = 0; b < 4000; ++b) {
        if (b % 4 == 0) control.knob[0][2] = (uint8_t)(96 | (b * 7 & 31));   // size within AMBIENCE
        else control.knob[0][3 + b % 3] = (uint8_t)(b * 37 % 128);
        fx_u->Process(st, 12, o, control);
      }
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 4000;
      printf("test_send_fx: CPU cloudseed ambience, a knob moved every block: %.0f ns a block\n", us * 1000);
      return us < 50.0;
    }));
  }
  {   // CC 120 (All Sound Off): a burst of 13 during a CLOUDSEED tail: one request, silence within 10 ms, no step, the
      // TYPE and params kept, the next input sounds
    auto fx_u = std::make_unique<SendFx>(kSr);
    SendFx& fx = *fx_u;
    clear_control();
    control.send[1][0] = 127, control.knob[0][0] = kCloudSeed, control.knob[0][2] = 32;
    g_a = 0.2f;
    auto pre = run(fx, 48000, 1, noise);
    auto tail = run(fx, 4800, -1, zero);
    CHECK(rms(tail, 0) > 0.001f);   // ringing
    uint8_t k0 = control.kill;
    int made = 0;
    for (int ch = 0; ch < 13; ++ch) made += all_sound_off();
    CHECK(made == 1 && (uint8_t)(control.kill - k0) == 1);
    auto b = run(fx, 4800, -1, zero);
    float natural = 0, pk = 0;   // the tail's own largest step (it is loud noise): the fade adds no step beyond it
    for (size_t i = 1; i < tail.size(); ++i)
      natural = std::fmax(natural, std::fabs(tail[i][0] - tail[i - 1][0])), pk = std::fmax(pk, std::fabs(tail[i][0]));
    float maxstep = 0, p = tail.back()[0];
    for (auto& f : b) maxstep = std::fmax(maxstep, std::fabs(f[0] - p)), p = f[0];
    float late = 0;
    for (int i = 480; i < 4800; ++i) late = std::fmax(late, std::fmax(std::fabs(b[i][0]), std::fabs(b[i][1])));
    printf("test_send_fx: CC 120 burst: max step %.4f (tail's own %.4f), max |y| after 10 ms %.6f\n", maxstep, natural, late);
    CHECK(maxstep <= natural + pk / 240.0f && late == 0.0f);
    CHECK(control.knob[0][0] == kCloudSeed && control.knob[0][2] == 32);
    auto z = run(fx, 24000, -1, zero);   // cleared: nothing comes back
    float mz = 0;
    for (auto& f : z) mz = std::fmax(mz, std::fabs(f[0]));
    CHECK(mz == 0.0f && !fx.Clearing(0));
    auto again = run(fx, 24000, 1, noise);   // ready for the next note
    CHECK(rms(again, 12000) > 0.001f);
  }
  {   // ---- P1 = ROUTE: 0..63 PRE (the 2/3 pair), 64..127 POST (0/1), every TYPE incl. STUTTER; the crossfade on a change
    const uint8_t types[] = {kChorus, kComb, kRings, kCloudSeed, kStutter};
    for (int ty : types)
      for (int p1 : {0, 63, 64, 127}) {
        auto fx_u = std::make_unique<SendFx>(kSr);
        SendFx& fx = *fx_u;
        clear_control();
        control.knob[0][0] = (uint8_t)ty, control.knob[0][1] = (uint8_t)p1;
        for (int tr = 0; tr < kTracks; ++tr) control.send[tr][0] = 127;
        g_a = 0.2f;
        float st[SendFx::kMaxFrames][kStems], pre[SendFx::kMaxFrames][2], post[SendFx::kMaxFrames][2], ep = 0, eq = 0;
        for (int i = 0; i < 24000; i += 12) {
          for (int j = 0; j < 12; ++j) {
            for (int v = 0; v < kStems; ++v) st[j][v] = 0.0f;
            st[j][0] = noise(i + j);
          }
          fx.Process(st, 12, pre, control, post);
          if (i >= 12000)
            for (int j = 0; j < 12; ++j)
              ep += pre[j][0] * pre[j][0] + pre[j][1] * pre[j][1], eq += post[j][0] * post[j][0] + post[j][1] * post[j][1];
        }
        bool want_post = p1 >= 64;
        if (ep + eq > 0.0f) CHECK((want_post ? ep : eq) == 0.0f);   // nothing on the other pair, exactly
        CHECK(want_post ? eq > 1e-3f : ep > 1e-3f);
      }
    // the override wins: forced fx / main ignore P1
    for (int forced : {kRouteFx, kRouteMain}) {
      auto fx_u = std::make_unique<SendFx>(kSr);
      clear_control();
      control.route = (uint8_t)forced;
      control.knob[0][0] = kStutter, control.knob[0][1] = forced == kRouteFx ? 127 : 0;
      float st[SendFx::kMaxFrames][kStems] = {}, pre[SendFx::kMaxFrames][2], post[SendFx::kMaxFrames][2];
      float e[2] = {0, 0};
      for (int i = 0; i < 4800; i += 12) {
        for (int j = 0; j < 12; ++j) st[j][0] = 0.1f;
        fx_u->Process(st, 12, pre, control, post);
        if (i >= 2400) e[0] += pre[0][0] * pre[0][0], e[1] += post[0][0] * post[0][0];
      }
      CHECK(forced == kRouteFx ? (e[0] > 0 && e[1] == 0) : (e[1] > 0 && e[0] == 0));
    }
    // a P1 change while STUTTER is disengaged (a DC mix): exact passthrough on either pair, the pair sum constant,
    // each pair's per-frame step no larger than the fade limit (1 / kPanFrames of the level)
    for (int dir = 0; dir < 2; ++dir) {
      auto fx_u = std::make_unique<SendFx>(kSr);
      SendFx& fx = *fx_u;
      clear_control();
      control.knob[0][0] = kStutter, control.knob[0][1] = dir ? 127 : 0;
      float st[SendFx::kMaxFrames][kStems] = {}, pre[SendFx::kMaxFrames][2], post[SendFx::kMaxFrames][2];
      for (int j = 0; j < 12; ++j) st[j][0] = 0.1f;
      for (int i = 0; i < 4800; i += 12) fx.Process(st, 12, pre, control, post);
      CHECK(pre[0][0] == (dir ? 0.0f : 0.1f) && post[0][1] == (dir ? 0.1f : 0.0f));   // exact on whichever pair
      control.knob[0][1] = dir ? 0 : 127;
      float lp = pre[11][0], lq = post[11][0], maxstep = 0;
      int mid = 0;
      for (int i = 0; i < 1200; i += 12) {
        fx.Process(st, 12, pre, control, post);
        for (int j = 0; j < 12; ++j) {
          maxstep = std::fmax(maxstep, std::fmax(std::fabs(pre[j][0] - lp), std::fabs(post[j][0] - lq)));
          lp = pre[j][0], lq = post[j][0];
          CHECK(std::fabs(pre[j][0] + post[j][0] - 0.1f) < 1e-6f);
          mid += pre[j][0] > 0.01f && post[j][0] > 0.01f;
        }
      }
      printf("test_send_fx: ROUTE crossfade dir %d: max step %.6f (limit %.6f), %d frames on both pairs\n", dir, maxstep,
             0.1f / kPanFrames, mid);
      CHECK(maxstep <= 0.1f / kPanFrames * 1.01f && mid > kPanFrames - 90);
      CHECK(pre[11][0] == (dir ? 0.1f : 0.0f) && post[11][0] == (dir ? 0.0f : 0.1f));
    }
  }
  printf("test_send_fx: SendFx %zu bytes\n", sizeof(SendFx));
  printf("test_send_fx: PASS\n");
  return 0;
}
