// core/tests/test_wavetable.cpp - WAVETABLE (core/wavetable): the WAV reader (formats, 'clm ' hints, extensible),
// frame size detection and thinning, the harmonics and mip levels (no energy above a level's top harmonic), then the
// machine through the engine's own entry (host_sysex with the 0x28 flag, and wt=voice:V): the card's list, a trig
// waiting for its table, frames loaded as written, aliasing at a high note (mip levels against a level-0 read), the
// morph's continuity across a POS sweep, unison detune, warps, chords, live notes, the headroom; golden renders
// (features within tolerance) and the CPU a note costs.
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>   // chmod

#include "arp/arp.h"
#include "check.h"
#include "lfo.h"
#include "ui/remote.h"
#include "poly.h"
#include "voices.h"
#include "wavetable/tables.h"
#include "wavetable/wav_read.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"
#include "pool_util.h"
#include "pool/pool.h"

using namespace core;
namespace core { namespace ui { const char* voice_label(int vi, int k, char* b, int cap); } }   // page_lfo.cpp

static constexpr int kW = kWaveMachine;
// P1..P16: TBL POS WARP AMT UNI SUB CUT RES ATK DEC SUS ENV TBLB POSB MIX FTYPE
static std::vector<float> P(float tbl, float pos, float warp = 0, float amt = 0, float uni = 0, float sub = 0,
                            float cut = 64, float res = 0, float atk = 0, float dec = 127, float sus = 127, float env = 64,
                            float tblb = 0, float posb = 0, float mix = 0, float ftype = 0) {
  return {tbl, pos, warp, amt, uni, sub, cut, res, atk, dec, sus, env, tblb, posb, mix, ftype};
}
// the knob value in the middle of warp zone z (OFF SYNC PWM FM FORMANT QUANT BEND SQUEEZE RM) and of filter type zone z
// (DJ LP12 LP24 BP12 BP24 HP12 HP24 LADDER)
static float wz(int z) { return (float)((z * 128 + 64) / 9); }
static float fz(int z) { return (float)(z * 16 + 8); }
enum { kWOff, kWSync, kWPwm, kWFm, kWFormant, kWQuant, kWBend, kWSqueeze, kWRm, kWarps };
enum { kFDj, kFLp12, kFLp24, kFBp12, kFBp24, kFHp12, kFHp24, kFLadder, kFTypes };
static const float kSawPos = 84 + 85 / 128.0f;   // BASIC's saw frame (42 of 64)

static uint64_t g_us = 1000000;
static void advance_ms(int ms) {   // the control core's clock (the B fade out waits on it)
  g_us += (uint64_t)ms * 1000;
  host_fake_time(true, g_us);
}
static void idle() {
  for (int i = 0; i < 4000 && wt::step(); ++i) {}
}
static std::vector<float> render(int vi, int frames) {   // the machine alone (no limiter): -1..1
  std::vector<float> o(frames, 0.0f);
  for (int s = 0; s < frames; s += kBlock) {
    idle();
    wt::render_add(vi, o.data() + s, frames - s < kBlock ? frames - s : kBlock);
  }
  return o;
}

// ---- the reader
static void test_wav_read() {
  using wavread::Info;
  auto sine = [](int i, int c) { return 0.5 * std::sin(2 * M_PI * i / 64.0) * (c ? -1 : 1); };
  struct Case { int bits, fmt; bool ext; int ch; };
  const Case cases[] = {{16, 1, false, 1}, {24, 1, false, 2}, {32, 1, false, 1}, {8, 1, false, 1}, {32, 3, false, 2},
                        {24, 1, true, 1}, {32, 3, true, 1}};
  for (const Case& c : cases) {
    std::vector<uint8_t> b = wg::wav_bytes(256, c.ch, c.bits, 44100, c.fmt, "", sine, c.ext);
    Info a;
    CHECK(wavread::parse(b.data(), (uint32_t)b.size(), &a));
    CHECK(a.ch == c.ch && a.bits == c.bits && a.rate == 44100 && a.frames == 256 && a.format == c.fmt && a.clm_frame == 0);
    std::vector<float> m(256);
    wavread::to_mono(a, b.data() + a.data_off, 256, m.data());
    double err = 0;
    for (int i = 0; i < 256; ++i) err = std::max(err, std::fabs(m[i] - (c.ch == 2 ? 0.0 : sine(i, 0))));
    CHECK(err < (c.bits == 8 ? 0.01 : 1e-3));   // stereo: L and -L summed to nothing
  }
  {   // a 'clm ' chunk (Serum's): its frame size; a data chunk longer than the file: cut
    std::vector<uint8_t> b = wg::wav_bytes(1024, 1, 16, 48000, 1, "<!>256 10000000 wavetable (www.xferrecords.com)", sine);
    Info a;
    CHECK(wavread::parse(b.data(), (uint32_t)b.size(), &a) && a.clm_frame == 256 && a.frames == 1024);
    std::vector<uint8_t> cut(b.begin(), b.end() - 100);
    CHECK(wavread::parse(cut.data(), (uint32_t)cut.size(), &a) && a.frames == 1024 - 50);
  }
  {   // not WAVs
    Info a;
    const uint8_t junk[64] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'I', 'F', 'F'};
    CHECK(!wavread::parse(junk, sizeof junk, &a));
    std::vector<uint8_t> b = wg::wav_bytes(16, 1, 12, 48000, 1, "", sine);   // 12-bit: refused
    CHECK(!wavread::parse(b.data(), (uint32_t)b.size(), &a));
  }
  CHECK(wavread::clm_size((const uint8_t*)"<!>2048 abc", 11) == 2048 && wavread::clm_size((const uint8_t*)"none", 4) == 0);
}

static void test_frames() {
  CHECK(wt::detect_frame_size(2048 * 10, 0) == 2048);
  CHECK(wt::detect_frame_size(1024 * 3, 0) == 1024 && wt::detect_frame_size(512 * 5, 0) == 512);
  CHECK(wt::detect_frame_size(256 * 7, 0) == 256);
  CHECK(wt::detect_frame_size(4096, 1024) == 1024);   // the hint over the 2048 default
  CHECK(wt::detect_frame_size(4096, 3000) == 2048);   // a hint that does not divide: ignored
  CHECK(wt::detect_frame_size(600, 0) == 600);        // one single cycle (AKWF's 600 samples)
  CHECK(wt::detect_frame_size(100000, 0) == 2048);
  CHECK(wt::frame_count(2048 * 300, 2048) == 256 && wt::source_frame(2048 * 300, 2048, 255) == 299);
  CHECK(wt::source_frame(2048 * 300, 2048, 0) == 0 && wt::frame_count(2048 * 3, 2048) == 3);
  CHECK(wt::source_frame(2048 * 3, 2048, 2) == 2);
}

// harmonics and levels: a saw's series back; no level holds energy over its top harmonic; every level the same wave
static void test_levels() {
  std::vector<float> x(2048), c(wt::kSpec), sc(4 * 8192), fr(wt::kFrameFloats);
  for (int i = 0; i < 2048; ++i) x[i] = (float)(1.0 - 2.0 * i / 2048.0);   // a naive saw (falling)
  wt::analyze(x.data(), 2048, c.data(), sc.data());
  for (int h = 1; h < 40; ++h) {
    double mag = std::hypot(c[2 * h], c[2 * h + 1]);
    CHECK(std::fabs(mag - 2.0 / (M_PI * h)) < 0.002);
  }
  std::vector<float> y(600);   // a 600-sample cycle (the DFT path): the same series
  for (int i = 0; i < 600; ++i) y[i] = (float)(1.0 - 2.0 * i / 600.0);
  std::vector<float> c2(wt::kSpec);
  wt::analyze(y.data(), 600, c2.data(), sc.data());
  for (int h = 1; h < 40; ++h) CHECK(std::fabs(std::hypot(c2[2 * h], c2[2 * h + 1]) - 2.0 / (M_PI * h)) < 0.003);
  wt::synth_frame(c.data(), fr.data(), sc.data());
  for (int k = 0; k < wt::kLevels; ++k) {
    const float* l = wt::level(fr.data(), k);
    int n = wt::kLevelSize[k];
    CHECK(l[-1] == l[n - 1] && l[n] == l[0] && l[n + 1] == l[1]);   // the wrap guards
    std::vector<float> xl(l, l + n), cl(wt::kSpec);
    wt::analyze(xl.data(), n, cl.data(), sc.data());
    for (int h = 1; h <= (n - 1) / 2 && h <= wt::kHarm; ++h) {
      double mag = std::hypot(cl[2 * h], cl[2 * h + 1]);
      if (h > wt::kLevelHarm[k]) CHECK(mag < 1e-4);                    // nothing over the level's top
      else CHECK(std::fabs(mag - 2.0 / (M_PI * h)) < 0.002);           // the rest kept
    }
  }
  CHECK(wt::pick_level(440.0f / 48000) == 3);   // A4: 0.6 / inc = 65 -> 64 harmonics (to 28 kHz)
  CHECK(wt::pick_level(20.0f / 48000) == 0 && wt::pick_level(0.7f) == wt::kLevels);
}

// ---- the machine
static std::string g_card;
static void setup_card() {
  char tmpl[] = "/tmp/wt-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  std::string W = g_card + "/WAVETABLES";
  CHECK(system(("mkdir -p '" + W + "/sub' '" + W + "/.hidden'").c_str()) == 0);
  // 8 frames of 2048, frame f a sine of harmonic f + 1 (16-bit)
  wg::write_file(W + "/harm.wav", wg::wav_bytes(8 * 2048, 1, 16, 44100, 1, "", [](int i, int) {
                   int f = i / 2048;
                   return 0.8 * std::sin(2 * M_PI * (f + 1) * (i % 2048) / 2048.0);
                 }));
  // Serum's 'clm ': 4 frames of 256 (a saw, a square, a triangle, a sine), 32-bit float stereo
  wg::write_file(W + "/sub/clm256.wav", wg::wav_bytes(4 * 256, 2, 32, 48000, 3, "<!>256 00000000 wavetable", [](int i, int) {
                   int f = i / 256;
                   double t = (i % 256) / 256.0;
                   return f == 0 ? 1 - 2 * t : f == 1 ? (t < 0.5 ? 0.7 : -0.7) : f == 2 ? 1 - 4 * std::fabs(t - 0.5)
                                                                                           : std::sin(2 * M_PI * t);
                 }));
  // one 600-sample cycle, 24-bit
  wg::write_file(W + "/akwf600.wav", wg::wav_bytes(600, 1, 24, 44100, 1, "", [](int i, int) { return std::sin(2 * M_PI * i / 600.0); }));
  wg::write_file(W + "/broken.wav", std::vector<uint8_t>(100, 7));
  wg::write_file(W + "/.hidden/no.wav", wg::wav_bytes(2048, 1, 16, 48000, 1, "", [](int, int) { return 0.0; }));
  wg::write_file(W + "/notes.txt", {'x'});
  host_set_card(g_card.c_str());
}
static int card_entries() {   // the rows that hold a table (the WAVETABLES pool's)
  int n = 0;
  for (int e = 0; e < wt::entries(); ++e) n += wt::entry_lib(e) == srclib::kLibNone;
  return n;
}
static int entry(const char* name) {
  for (int e = 0; e < wt::entries(); ++e)
    if (!strcmp(wt::entry_name(e), name)) return e;
  return -1;
}

static void test_list_and_load() {
  // the template's built-ins A001..A006, then the card's (put in A007..A010 here; the pool takes any WAV)
  CHECK(card_entries() == wt::kBuiltins + 4);
  CHECK(!strcmp(wt::entry_name(0), "OFF") && !strcmp(wt::entry_name(1), "BASIC") && !strcmp(wt::entry_name(6), "SYNC"));
  CHECK(entry("akwf600") == 7 && entry("broken") == 8 && entry("harm") == 9 && entry("clm256") == 10 && entry("no") < 0);
  int e = entry("harm");
  wg::params(kW, 0, P((float)e, 0));
  CHECK(wt::voice_entry(0) == 1);   // BASIC (A001) until it loads
  wg::trig(kW, 0, 60, 100, P((float)e, 0));
  CHECK(wt::stats().waits == 1 && wt::voice_notes(0) == 0);
  idle();
  CHECK(wt::voice_entry(0) == e && wt::voice_notes(0) == 1 && wt::stats().trigs == 1);
  CHECK(wt::table_frames(e) == 8 && wt::stats().last_frame_size == 2048);
  for (int f = 0; f < 8; ++f) {   // each frame's level 0: its sine, at 0.95 of the table's peak
    const float* l = wt::level(wt::table_frame(e, f), 0);
    double err = 0;
    for (int i = 0; i < 2048; ++i) err = std::max(err, std::fabs(l[i] - 0.95 * std::sin(2 * M_PI * (f + 1) * i / 2048.0)));
    CHECK(err < 2e-3);
  }
  int c = entry("clm256");
  wg::trig(kW, 1, 60, 100, P((float)c, 0));
  idle();
  CHECK(wt::table_frames(c) == 4 && wt::stats().last_frame_size == 256);
  int a = entry("akwf600");
  wg::trig(kW, 2, 60, 100, P((float)a, 0));
  idle();
  CHECK(wt::table_frames(a) == 1 && wt::stats().last_frame_size == 600);
  uint32_t fails = wt::stats().load_fails;
  wg::trig(kW, 3, 60, 100, P((float)entry("broken"), 0));
  idle();
  CHECK(wt::stats().load_fails == fails + 1 && wt::voice_entry(3) == 1);   // refused: BASIC plays on
  // the slots: four card tables at most; a fifth replaces the least recently used one not in use
  for (int vi = 0; vi < 4; ++vi) wt::silence(vi), wg::off(vi);
  render(0, 4800);
  char o[400];
  CHECK(wt::command("wt", o, sizeof o) && strstr(o, "3/4 card tables resident"));
  wt::reset();
}

// owner 2026-11-15 (hardware): WAVETABLE knew its table only once a note had played with it. The voice is idle: its first
// report (the machine chosen, TBL OFF), then the pick in the Rytm's list reaches the host as the SRC view's 2A (P1) only.
// The table loads and the voice shows it (the SRC view draws voice_entry / viz_frame) with no note.
static void test_idle_pick() {
  const int t = 0, vi = ui::remote_track_voice(t);   // BD: voice 0
  CHECK(vi == 0);
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 46, (uint8_t)(kW | vi), 0xF7});   // the machine chosen
  wg::params(kW, vi, P(0, 0));   // its first report: TBL OFF
  idle();
  CHECK(wt::voice_entry(vi) != entry("clm256"));
  uint32_t trigs = wt::stats().trigs;
  int e = entry("clm256");
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 1, 0xF7});   // its SRC view
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 0, 1, (uint8_t)(e & 127), (uint8_t)srclib::lo_of(e), 0xF7});   // the pick
  idle();
  CHECK(wt::voice_entry(vi) == e && wt::table_frames(e) == 4);
  int nf = 0;
  float y[64];
  wt::viz_pos(vi, &nf);
  CHECK(nf == 4 && wt::viz_frame(vi, 0, 64, y) == 64);
  CHECK(wt::stats().trigs == trigs);   // (no note played)
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 0, 0x7F, 0xF7});   // not ours again
  wt::reset();
  printf("test_wavetable: a table picked on an idle voice (2A) loads and shows with no note\n");
}

// frames morph smoothly: a POS sweep over the harmonic table, no step beyond what the waves themselves have
static void test_morph() {
  int e = entry("harm");
  wg::trig(kW, 0, 48, 127, P((float)e, 0));
  render(0, 2400);
  std::vector<float> all;
  double maxd = 0;
  for (int s = 0; s <= 127; ++s) {   // POS 0 -> 127 in 1 s
    wg::params(kW, 0, P((float)e, (float)s));
    std::vector<float> o = render(0, 376);
    all.insert(all.end(), o.begin(), o.end());
  }
  for (size_t i = 1; i < all.size(); ++i) maxd = std::max(maxd, (double)std::fabs(all[i] - all[i - 1]));
  // the steepest a static frame here can be: harmonic 8 at 130.8 Hz, the note's level
  wg::params(kW, 0, P((float)e, 127));
  std::vector<float> top = render(0, 4800);
  double maxs = 0;
  for (size_t i = 1; i < top.size(); ++i) maxs = std::max(maxs, (double)std::fabs(top[i] - top[i - 1]));
  printf("test_wavetable: POS sweep max step %.4f, the top frame's own %.4f\n", maxd, maxs);
  CHECK(maxd < maxs * 1.2 && wg::rms(all) > 0.05);
  // the morph's middle: halfway between frames 0 and 1 at POS 127/14 (fp 0.5): both harmonics at half
  wg::params(kW, 0, P((float)e, 127.0f / 14));
  std::vector<float> mid = render(0, 9600);
  int n;
  std::vector<double> sp = wg::spectrum(std::vector<float>(mid.begin() + 4800, mid.end()), &n);
  double f0 = 440 * std::pow(2, (48 - 69) / 12.0), b1 = 0, b2 = 0;
  for (int k = -3; k <= 3; ++k) b1 += sp[(int)std::round(f0 * n / 48000) + k], b2 += sp[(int)std::round(2 * f0 * n / 48000) + k];
  CHECK(std::fabs(10 * std::log10(b1 / b2)) < 1.0);
  wt::reset();
}

// aliasing: a saw at C7 through the mip levels against the same read of level 0 (all 512 harmonics)
static void test_alias() {
  int key = 96;
  double f0 = 440 * std::pow(2, (key - 69) / 12.0);
  wg::trig(kW, 0, key, 127, P(1, kSawPos));
  render(0, 4800);
  std::vector<float> o = render(0, 16384);
  double mip = wg::alias_db(o, f0, 20000);
  // level 0 read the same way (Hermite, the same rate): what a table without mip levels plays
  const float* fr = wt::table_frame(1, 41);
  const float* l0 = wt::level(fr, 0);
  std::vector<float> naive(16384);
  double ph = 0, inc = f0 / 48000;
  for (int i = 0; i < 16384; ++i, ph += inc) {
    ph -= std::floor(ph);
    double x = ph * 2048;
    int j = (int)x;
    float t = (float)(x - j), xm1 = l0[j - 1], x0 = l0[j], x1 = l0[j + 1], x2 = l0[j + 2];
    float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, b = c + w + a;
    naive[i] = ((a * t - b) * t + c) * t + x0;
  }
  double nv = wg::alias_db(naive, f0, 20000);
  printf("test_wavetable: C7 saw: aliases %.1f dB under the harmonics with the mip levels, %.1f dB with level 0 only\n", mip, nv);
  CHECK(mip < -60 && nv > mip + 30);
  // every key from C2 to C8: the aliases stay down
  double worst = -200;
  for (int k = 36; k <= 108; k += 6) {
    wt::silence(0);
    render(0, 240);
    wg::trig(kW, 0, k, 127, P(1, kSawPos));
    render(0, 2400);
    std::vector<float> y = render(0, 16384);
    double a = wg::alias_db(y, 440 * std::pow(2, (k - 69) / 12.0), 18000);
    printf("  key %d: %.1f dB\n", k, a);
    worst = std::max(worst, a);
  }
  printf("test_wavetable: saw C2..C8 worst alias (to 18 kHz) %.1f dB\n", worst);
  CHECK(worst < -60);
  // the warps: SYNC, PWM, FM sound (and differ from OFF)
  double r0 = 0;
  for (int w = 0; w < 4; ++w) {
    wt::silence(0);
    render(0, 240);
    wg::trig(kW, 0, 48, 127, P(1, 0, wz(w), 100));
    render(0, 2400);
    std::vector<float> y = render(0, 9600);
    int n;
    std::vector<double> sp = wg::spectrum(y, &n);
    double hi = 0, all = 0;
    for (size_t k = 0; k < sp.size(); ++k) all += sp[k], hi += k * 48000.0 / n > 200 ? sp[k] : 0;
    double bright = hi / all;
    printf("test_wavetable: warp %d on a sine (131 Hz): %.3f of the energy above 200 Hz\n", w, bright);
    if (w == 0) {
      r0 = bright;
      CHECK(bright < 0.01);
    } else {
      CHECK(bright > r0 + 0.05);
    }
  }
  wt::reset();
}

// unison: the fundamental's energy spreads over the detune; the level stays near one oscillator's
static void test_unison() {
  double width[2], level[2];
  for (int u = 0; u < 2; ++u) {
    wt::silence(0);
    render(0, 240);
    wg::trig(kW, 0, 57, 127, P(1, 0, 0, 0, u ? 127.0f : 0.0f));
    render(0, 4800);
    std::vector<float> y = render(0, 65536);
    int n;
    std::vector<double> sp = wg::spectrum(y, &n);
    double df = 48000.0 / n, f0 = 220, in = 0, near = 0;
    for (size_t k = 0; k < sp.size(); ++k) {
      double f = k * df;
      if (std::fabs(f - f0) < 1.0) in += sp[k];
      if (std::fabs(f - f0) < 12.0) near += sp[k];
    }
    width[u] = 1 - in / near;   // the share of the energy around f0 that is off it
    level[u] = wg::rms(y);
  }
  printf("test_wavetable: unison: %.2f / %.2f of the energy off the fundamental, rms %.3f / %.3f\n", width[0], width[1], level[0], level[1]);
  CHECK(width[0] < 0.1 && width[1] > 0.5);
  CHECK(level[1] > level[0] * 0.6 && level[1] < level[0] * 1.6);
  wt::reset();
}

// chords come from a Sidecar poly track (P13..P16 are TBL B, POS B, MIX, FTYPE: a trig is one note), the headroom, a
// 0x11's release, live notes and the sequencer's
static void test_chords_live() {
  // an old project's chord word (CHD 1 = MAJ, now TBL B 1 = BASIC): one note, as a trig always is
  wg::trig(kW, 4, 60, 100, P(1, 30, 0, 0, 0, 0, 64, 0, 0, 40, 100, 64, 1, 0, 0));
  render(4, 2400);
  CHECK(wt::voice_notes(4) == 1);
  wg::trig(kW, 4, 62, 100, P(1, 30, 0, 0, 0, 0, 64, 0, 0, 40, 100, 64));   // the earlier trig note's tail goes on
  render(4, 240);
  CHECK(wt::voice_notes(4) == 2);
  wg::off(4);
  render(4, 48000);
  CHECK(wt::voice_notes(4) == 0);
  // a poly track's chord (poly::seq_on: the sequencer's notes; poly::key_on: the Pi's keyboards): a MAJ and a MAJ7 at
  // once, each note its own, the headroom as for any chord
  const poly::Chord maj = {1, 0, 0, 0}, maj7 = {6, 0, 0, 0};
  uint8_t ns[poly::kMaxNotes];
  int n = poly::expand(60, maj, ns);
  CHECK(n == 3);
  for (int i = 0; i < n; ++i) CHECK(poly::seq_on(4, ns[i], 100));
  render(4, 2400);
  CHECK(wt::voice_notes(4) == 3 && std::fabs(wt::headroom(4) - 1 / std::sqrt(3.0 * 100 / 127)) < 0.08);
  n = poly::expand(62, maj7, ns);
  CHECK(n == 4);
  CHECK(poly::plays_poly(4));
  for (int i = 0; i < n; ++i) CHECK(poly::key_on(4, ns[i], 100, poly::kKeysCh0));
  render(4, 240);
  CHECK(wt::voice_notes(4) == 7);
  poly::seq_off(4, -1);
  poly::key_off(4, -1, poly::kKeysCh0);
  render(4, 48000);
  CHECK(wt::voice_notes(4) == 0);
  // live pad notes: held through a trig's release, released by their own note-off
  wt::live_on(4, 64, 100, 3, 64);
  wt::live_on(4, 67, 100, 3, 67);
  CHECK(wt::voice_notes(4) == 2);
  wt::note_off(4);
  render(4, 9600);
  CHECK(wt::voice_notes(4) == 2);
  wt::live_off(4, 3, 64);
  render(4, 48000);
  CHECK(wt::voice_notes(4) == 1);
  wt::live_off(4, 3, -1);
  render(4, 48000);
  CHECK(wt::voice_notes(4) == 0);
  // through poly's sequencer entry and wt=voice:V on another machine's reports
  char o[200];
  CHECK(wt::command("wt=voice:5", o, sizeof o) && wt::forced(5) && strstr(o, "voice 5 plays WAVETABLE"));
  wg::params(kFm, 5, P(1, 20));
  CHECK(v[5].wt && !v[5].fm);
  CHECK(poly::seq_on(5, 60, 100) && wt::voice_notes(5) == 1);
  poly::seq_off(5, 60);
  render(5, 48000);
  CHECK(wt::voice_notes(5) == 0);
  CHECK(wt::command("wt=voice:5:off", o, sizeof o) && !wt::forced(5));
  wg::params(kFm, 5, P(1, 20));
  CHECK(!v[5].wt && v[5].fm);
  CHECK(wt::command("wt=list", o, sizeof o) && !wt::command("wtx", o, sizeof o) && !wt::command("wt=nope", o, sizeof o));
  wt::reset();
}

// golden renders: a pluck, a pad, a sync lead and a vocal, their level and brightness as the engine rendered them before
// TBL B and the new warps and filters (docs/WAVETABLE.md); WG_RENDER_DIR=... writes them as WAVs
static void test_golden() {
  // The patches as before this change (TBL B OFF, MIX 0, FTYPE DJ: P13..P16 = 0; their WARP value in the new zones). The
  // references are what the engine at edc5886 (before TBL B, the new warps and filters) rendered for the same notes from
  // the same random start (wt::reset): with B off and the DJ filter this engine's renders are those, sample for sample
  // (compared as 32-bit floats when this was written, nine patches; the tolerances here are for another compiler's
  // rounding). The pad is four live notes (MIN7), as a chord is played now.
  struct G { const char* name; int key; std::vector<float> p; int chord; double rms, avg, zc; };
  G gs[] = {
      {"pluck", 48, P(2, 20, 0, 0, 40, 0, 40, 30, 0, 75, 0, 100), 0, 0, 0, 0},
      {"pad", 60, P(1, 60, 0, 0, 110, 40, 50, 10, 60, 90, 110, 80), 1, 0, 0, 0},
      {"sync", 55, P(6, 90, wz(kWSync), 70, 30, 20, 58, 50, 0, 70, 90, 40), 0, 0, 0, 0},
      {"vocal", 52, P(3, 0, wz(kWFm), 40, 60, 0, 64, 0, 10, 110, 120, 127), 0, 0, 0, 0},
  };
  const double kRms[] = {0.022498, 0.108225, 0.118213, 0.059837}, kAvg[] = {0.004275, 0.076944, 0.081906, 0.042108};
  const double kZc[] = {252, 554, 5916, 2257};   // [measured: the engine at edc5886, 2026-11-17]
  const char* dir = getenv("WG_RENDER_DIR");
  for (size_t i = 0; i < sizeof gs / sizeof gs[0]; ++i) {
    G& g = gs[i];
    wt::silence(6);
    render(6, 480);
    if (i == 0) wt::reset();   // the unison's random start phases from the start the references had
    if (g.chord) {
      wg::params(kW, 6, g.p);
      const int mi[4] = {0, 3, 7, 10};   // MIN7
      for (int k = 0; k < 4; ++k) wt::live_on(6, g.key + mi[k], 110, 3, g.key + mi[k]);
    } else {
      wg::trig(kW, 6, g.key, 110, g.p);
    }
    std::vector<float> y = render(6, 48000);
    if (g.chord) wt::live_off(6, 3, -1);
    else wg::off(6);
    std::vector<float> t = render(6, 24000);
    y.insert(y.end(), t.begin(), t.end());
    g.rms = wg::rms(y), g.zc = wg::zc_freq(y);
    g.avg = 0;
    for (float x : y) g.avg += std::fabs(x);
    g.avg /= (double)y.size();
    printf("test_wavetable: golden %s: rms %.6f, mean |x| %.6f, crossings %.0f a second (golden %.6f, %.6f, %.0f)\n", g.name, g.rms, g.avg, g.zc, kRms[i], kAvg[i], kZc[i]);
    if (dir) wg::write_wav16(std::string(dir) + "/wt_" + g.name + ".wav", y);
    CHECK(std::fabs(g.rms / kRms[i] - 1) < 1e-4 && std::fabs(g.avg / kAvg[i] - 1) < 1e-4 && std::fabs(g.zc / kZc[i] - 1) < 1e-3);
  }
  wt::reset();
}


// ---- table B, the warps and the filters

// the spectrum's energy within +-w Hz of f
static double band_energy(const std::vector<double>& sp, int n, double f, double w = 8) {
  double e = 0, df = 48000.0 / n;
  for (size_t k = 0; k < sp.size(); ++k)
    if (std::fabs(k * df - f) <= w) e += sp[k];
  return e;
}
static double frame_pos(int frame, int frames) { return 127.0 * frame / (frames - 1); }   // POS (0..127) of a frame

// TBL B: the A / B mix is equal power, both POS (and the ENV of the mod envelope) move, B is as free of aliases as A,
// B off is no B, B shares the unison
static void test_table_b() {
  const int h = entry("harm");   // 8 frames: frame f a sine of harmonic f + 1
  CHECK(h > 0);
  const double f0 = 440 * std::pow(2.0, (48 - 69) / 12.0);
  auto spec = [&](const std::vector<float>& y, double* e1, double* e2, double* e8) {
    int n;
    std::vector<double> sp = wg::spectrum(y, &n);
    *e1 = band_energy(sp, n, f0), *e2 = band_energy(sp, n, 2 * f0), *e8 = band_energy(sp, n, 8 * f0);
  };
  auto note = [&](int vi, const std::vector<float>& p) {
    wt::silence(vi);
    render(vi, 480);
    wg::trig(kW, vi, 48, 127, p);
    render(vi, 4800);
    return render(vi, 24000);
  };
  // B OFF (row 000, the default P13 = 0): A alone, as ever
  double a1, a2, a8, b1, b2, b8;
  std::vector<float> y = note(0, P((float)h, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, frame_pos(1, 8), 127));
  CHECK(wt::voice_entry_b(0) == -1);
  spec(y, &a1, &a2, &a8);
  CHECK(a1 > 1e3 * a2);   // MIX 127 with no B is A: its 1st harmonic only
  // B = harm at its 2nd frame: MIX 0 only A (1st harmonic), 127 only B (2nd), equal power between
  double rms0 = 0;
  for (int m : {0, 32, 64, 96, 127}) {
    y = note(0, P((float)h, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)h, (float)frame_pos(1, 8), (float)m));
    CHECK(wt::voice_entry_b(0) == (m ? h : -1));   // (B is wanted while it can be heard)
    spec(y, &b1, &b2, &b8);
    double ang = m / 127.0 * M_PI / 2, share = b1 / (b1 + b2), r = wg::rms(y);
    printf("test_wavetable: MIX %3d: A's share of the energy %.3f (law %.3f), rms %.4f\n", m, share, std::cos(ang) * std::cos(ang), r);
    CHECK(std::fabs(share - std::cos(ang) * std::cos(ang)) < 0.02);
    if (m == 0) rms0 = r;
    CHECK(std::fabs(r / rms0 - 1) < 0.03);   // equal power: the level stays
  }
  // POS B: B's own position (A stays on its first frame), the 4th frame's harmonic
  y = note(0, P((float)h, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)h, (float)frame_pos(3, 8), 127));
  int n;
  std::vector<double> sp = wg::spectrum(y, &n);
  CHECK(band_energy(sp, n, 4 * f0) > 1e3 * band_energy(sp, n, f0));
  // ENV: the mod envelope moves both POS (ATK 10 = 2 ms, DEC 127: it stays up): either table alone ends on its last
  // frame (the 8th harmonic)
  for (int m : {0, 127}) {
    y = note(0, P((float)h, 0, 0, 0, 0, 0, 64, 0, 10, 127, 127, 127, (float)h, 0, (float)m));
    spec(y, &b1, &b2, &b8);
    printf("test_wavetable: ENV +, MIX %d (%s only): 8th harmonic %.0f x the 1st\n", m, m ? "B" : "A", b8 / (b1 + 1e-30));
    CHECK(b8 > 100 * b1);
  }
  // B's aliases are as low as A's (a saw at the keys C2..C8, B alone and A + B)
  double worst = -200;
  for (int m : {64, 127})
    for (int k = 36; k <= 108; k += 12) {
      wt::silence(0);
      render(0, 240);
      wg::trig(kW, 0, k, 127, P(1, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 1, kSawPos, (float)m));
      render(0, 2400);
      std::vector<float> o = render(0, 16384);
      worst = std::max(worst, wg::alias_db(o, 440 * std::pow(2.0, (k - 69) / 12.0), 18000));
    }
  printf("test_wavetable: table B (saw, alone and mixed) C2..C8 worst alias (to 18 kHz) %.1f dB\n", worst);
  CHECK(worst < -60);
  // B shares the note's unison: the energy spreads off the fundamental as A's does (0.86 above)
  wt::silence(0);
  render(0, 240);
  wg::trig(kW, 0, 57, 127, P(1, 0, 0, 0, 127, 0, 64, 0, 0, 127, 127, 64, 1, 0, 127));
  render(0, 4800);
  y = render(0, 65536);
  sp = wg::spectrum(y, &n);
  double df = 48000.0 / n, in = 0, near = 0;
  for (size_t k = 0; k < sp.size(); ++k) {
    double f = k * df;
    if (std::fabs(f - 220) < 1.0) in += sp[k];
    if (std::fabs(f - 220) < 12.0) near += sp[k];
  }
  printf("test_wavetable: B's unison: %.2f of the energy off the fundamental\n", 1 - in / near);
  CHECK(1 - in / near > 0.5);
  wt::reset();
}

// the warps: the new ones change a sine, a mid AMT of BEND and SQUEEZE leaves it as it is, FM and RM take B as the
// modulator when it is on (a sine otherwise); then every warp x every filter type at the knobs' ends stays finite and
// goes silent after the release
static void test_warps_filters() {
  const float tbl = 1;
  auto note = [&](int vi, std::vector<float> p, int key = 48, int frames = 9600) {
    wt::silence(vi);
    render(vi, 480);
    wg::trig(kW, vi, key, 127, p);
    render(vi, 2400);
    return render(vi, frames);
  };
  std::vector<float> off = note(0, P(tbl, kSawPos));
  const char* names[] = {"OFF", "SYNC", "PWM", "FM", "FORMANT", "QUANT", "BEND", "SQUEEZE", "RM"};
  const float amt_of[kWarps] = {0, 0, 0, 0, 110, 100, 20, 110, 100};
  for (int w = kWFormant; w <= kWRm; ++w) {
    std::vector<float> y = note(0, P(tbl, kSawPos, wz(w), amt_of[w]));
    double d = 0, e = 0;
    for (size_t i = 0; i < y.size(); ++i) d += (y[i] - off[i]) * (y[i] - off[i]), e += off[i] * off[i];
    printf("test_wavetable: warp %s at AMT %.0f on a saw: %.2f of the unwarped energy off it, rms %.3f\n", names[w], amt_of[w], d / e, wg::rms(y));
    CHECK(d / e > 0.05 && std::isfinite(wg::rms(y)) && wg::rms(y) > 0.01);
  }
  // identity amounts: BEND 64 (d 0.5) and SQUEEZE 63.5 (d 1) read the phase as it is
  std::vector<float> sine = note(0, P(tbl, 0));   // (a sine: the warp's wider mip level costs it nothing)
  for (int w : {kWBend, kWSqueeze}) {
    std::vector<float> y = note(0, P(tbl, 0, wz(w), 63.5f));
    double d = 0, e = 0;
    for (size_t i = 0; i < y.size(); ++i) d += (y[i] - sine[i]) * (y[i] - sine[i]), e += sine[i] * sine[i];
    printf("test_wavetable: warp %s at its centre: %.5f of the energy off the unwarped\n", names[w], d / e);
    CHECK(d / e < 0.001);
  }
  // FM by B / by the sine: B on and the sine differ; RM likewise; B's own output is plain under FM / RM (MIX 127)
  for (int w : {kWFm, kWRm}) {
    std::vector<float> a = note(0, P(tbl, kSawPos, wz(w), 80));
    std::vector<float> b = note(0, P(tbl, kSawPos, wz(w), 80, 0, 0, 64, 0, 0, 127, 127, 64, 3, 20, 0));
    std::vector<float> c = note(0, P(tbl, kSawPos, wz(w), 80, 0, 0, 64, 0, 0, 127, 127, 64, 3, 20, 127));
    std::vector<float> p = note(0, P(3, 20));   // B's table plain
    double d = 0, e = 0;
    for (size_t i = 0; i < a.size(); ++i) d += (a[i] - b[i]) * (a[i] - b[i]), e += a[i] * a[i];
    double f = wg::rms(c) / wg::rms(p);   // (B has phases of its own: its level, not its waveform)
    printf("test_wavetable: %s by B against by a sine: %.2f of the energy differs; B alone under it: %.3f of plain B's level\n", names[w], d / e, f);
    CHECK(d / e > 0.1 && std::fabs(f - 1) < 0.03 && wg::rms(b) > 0.01);
  }
  // every warp x every filter at the knobs' ends: finite, bounded, silent after the release (B on at the upper end)
  int cases = 0;
  for (int w = 0; w < kWarps; ++w)
    for (int ft = 0; ft < kFTypes; ++ft)
      for (int end = 0; end < 2; ++end) {
        const float e = end ? 127.0f : 0.0f;
        for (int bon = 0; bon < 2; ++bon) {
          std::vector<float> p = P(tbl, end ? kSawPos : 0, wz(w), e, e, 40, e, e, 0, 20, 100, end ? 127.0f : 0.0f, bon ? 6.0f : 0.0f,
                                   e, bon ? 64.0f : 0.0f, fz(ft));
          wt::silence(7);
          render(7, 480);
          wt::set_fenv(7, end ? 1.0f : 0.0f);
          wg::trig(kW, 7, end ? 84 : 36, 127, p);
          std::vector<float> y = render(7, 9600);
          bool ok = true;
          double pk = 0;
          for (float x : y) ok = ok && std::isfinite(x), pk = std::max(pk, (double)std::fabs(x));
          wg::off(7);
          std::vector<float> t = render(7, 48000);
          for (float x : t) ok = ok && std::isfinite(x);
          if (!ok || pk > 4 || wt::voice_notes(7) != 0 || wg::rms(t, 40000, t.size()) > 1e-4)
            printf("  FAIL warp %s filter %d ends %d B %d: finite %d peak %.2f notes %d tail %.2g\n", names[w], ft, end, bon, (int)ok, pk,
                   wt::voice_notes(7), wg::rms(t, 40000, t.size()));
          CHECK(ok && pk < 4 && wt::voice_notes(7) == 0 && wg::rms(t, 40000, t.size()) < 1e-4);
          ++cases;
        }
      }
  printf("test_wavetable: %d warp x filter x end x B cases finite and silent after the release\n", cases);
  wt::set_fenv(7, 0);
  wt::reset();
}

// the new filters' response: a sine at each key through the filter at a fixed CUT against the digital response the design
// says (the SVF's analog prototype 1 / (1 + k s + s^2) at the prewarped s, the 24 dB types' first stage at k = 1, the
// blends as Vital's), and the slopes
static double svf_gain(int ft, double res, double f, double fc) {   // |H| for the types LP12 LP24 BP12 BP24 HP12 HP24
  using C = std::complex<double>;
  const double fs = 48000, w = std::tan(M_PI * f / fs) / std::tan(M_PI * fc / fs);
  const int bl = ft == kFLp12 || ft == kFLp24 ? -1 : ft == kFBp12 || ft == kFBp24 ? 0 : 1;
  const bool two = ft == kFLp24 || ft == kFBp24 || ft == kFHp24;
  const double ra = res * res * res, q = 0.5 + 15.5 * ra, k = 1 / q, drv = 1 / (ra * 2 + 1);
  const double band = std::sqrt(1.0 - bl * bl), low = bl < 0 ? 1 : 0, high = bl > 0 ? 1 : 0;
  const double b0 = high, b1 = band - k * high, b2 = low - high;
  const C s(0, w);
  auto stage = [&](double kk) {   // b0 + b1 s / D + b2 / D, D = 1 + kk s + s^2
    C d = 1.0 + kk * s + s * s;
    return b0 + b1 * s / d + b2 / d;
  };
  C h = drv * stage(two ? 1.0 : k);
  if (two) h *= stage(k);
  return std::abs(h);
}
static void test_filters() {
  const float cut = 40;
  auto tone = [](int key, float ft, float res, float c) {
    wt::silence(7);
    render(7, 480);
    wg::trig(kW, 7, key, 127, P(1, 0, 0, 0, 0, 0, c, res, 0, 127, 127, 64, 0, 0, 0, ft));   // BASIC at POS 0: a sine
    render(7, 9600);
    return wg::rms(render(7, 24000));
  };
  auto fc_of = [&](int key, float c) {
    double semis = 8 + c * 128.0 / 127 + 0.5 * (key - 60);
    return std::min(8.1757989 * std::pow(2.0, semis / 12), 0.45 * 48000);
  };
  struct T { int ft; const char* name; int lo, hi; };
  const T ts[] = {{kFLp12, "LP12", 24, 120}, {kFLp24, "LP24", 24, 120}, {kFBp12, "BP12", 24, 120},
                  {kFBp24, "BP24", 24, 120}, {kFHp12, "HP12", 24, 120}, {kFHp24, "HP24", 24, 120}};
  for (const T& t : ts) {
    // HP wants a cutoff the keys' range crosses: CUT 60 (fc 0.4 .. 4 kHz over the keys) instead of 40
    const float c = t.ft == kFHp12 || t.ft == kFHp24 ? 60.0f : cut;
    double worst = 0;
    int nm = 0;
    for (int key = t.lo; key <= t.hi; key += 4) {
      double ref = tone(key, fz(kFDj), 0, 64);   // the DJ filter open (bypassed): the table as it is
      double got = tone(key, fz(t.ft), 0, c);
      double f = 440 * std::pow(2.0, (key - 69) / 12.0), fc = fc_of(key, c);
      double want = svf_gain(t.ft, 0, f, fc);
      double gdb = 20 * std::log10(got / ref + 1e-12), wdb = 20 * std::log10(want + 1e-12);
      if (wdb > -50) {
        worst = std::max(worst, std::fabs(gdb - wdb));
        ++nm;
      }
    }
    printf("test_wavetable: %s at CUT %.0f: %d keys, the response within %.2f dB of the design's\n", t.name, c, nm, worst);
    CHECK(nm > 10 && worst < 1.0);
  }
  // the slopes in the stopband, dB per octave of f / fc: the 12 dB types' 12, the 24 dB LP's 24; the 24 dB HP's first
  // stage is the blend's band-pass (Vital's: its stage 1 has the high-pass's -RES x band term at resonance 1, not a
  // high-pass), so it falls 18
  struct S { int ft; const char* name; int k1, k2; double slope; };
  const S ss[] = {{kFLp12, "LP12", 84, 100, -12}, {kFLp24, "LP24", 84, 100, -24}, {kFHp12, "HP12", 24, 48, 12}, {kFHp24, "HP24", 24, 36, 18}};
  for (const S& s : ss) {
    const float c = s.slope > 0 ? 80.0f : cut;
    double g1 = tone(s.k1, fz(s.ft), 0, c), g2 = tone(s.k2, fz(s.ft), 0, c);
    double r1 = 440 * std::pow(2.0, (s.k1 - 69) / 12.0) / fc_of(s.k1, c), r2 = 440 * std::pow(2.0, (s.k2 - 69) / 12.0) / fc_of(s.k2, c);
    double slope = 20 * std::log10(g2 / g1) / std::log2(r2 / r1);
    double want = 20 * std::log10(svf_gain(s.ft, 0, 440 * std::pow(2.0, (s.k2 - 69) / 12.0), fc_of(s.k2, c)) /
                                  svf_gain(s.ft, 0, 440 * std::pow(2.0, (s.k1 - 69) / 12.0), fc_of(s.k1, c))) / std::log2(r2 / r1);
    printf("test_wavetable: %s slope %.1f dB / octave (design %.1f; f/fc %.3f .. %.3f)\n", s.name, slope, want, r1, r2);
    CHECK(std::fabs(slope - want) < 0.5 && std::fabs(slope - s.slope) < 2.5);
  }
  // the band-passes peak at the cutoff
  for (int ft : {kFBp12, kFBp24}) {
    double best = 0, at = 0;
    for (int key = 24; key <= 120; key += 2) {
      double g = tone(key, fz(ft), 0, cut), r = 440 * std::pow(2.0, (key - 69) / 12.0) / fc_of(key, cut);
      if (g > best) best = g, at = r;
    }
    printf("test_wavetable: %s peaks at f / fc = %.2f\n", ft == kFBp12 ? "BP12" : "BP24", at);
    CHECK(at > 0.8 && at < 1.25);
  }
  // RES lifts the cutoff's neighbourhood; at RES 0 the LP's gain at fc is 1/2 (Q 1/2)
  {
    int key = 72;   // 440 * 2^(3/12) = 523 Hz; CUT chosen so that fc is that
    double c = (std::log2(523.25 / 8.1757989) * 12 - 8 - 0.5 * (key - 60)) * 127 / 128.0;
    double ref = tone(key, fz(kFDj), 0, 64), lo = tone(key, fz(kFLp12), 0, (float)c), hi = tone(key, fz(kFLp12), 90, (float)c);
    printf("test_wavetable: LP12 at its cutoff: RES 0 %.2f of the level, RES 90 %.2f\n", lo / ref, hi / ref);
    CHECK(std::fabs(lo / ref - 0.5) < 0.04 && hi / ref > 1.5 * lo / ref);
  }
  // the ladder: 24 dB / octave in the stopband, open far below the cutoff, RES = 0 gives no peak
  {
    double g1 = tone(84, fz(kFLadder), 0, cut), g2 = tone(100, fz(kFLadder), 0, cut), ref1 = tone(84, fz(kFDj), 0, 64), ref2 = tone(100, fz(kFDj), 0, 64);
    double r1 = 440 * std::pow(2.0, (84 - 69) / 12.0) / fc_of(84, cut), r2 = 440 * std::pow(2.0, (100 - 69) / 12.0) / fc_of(100, cut);
    double slope = 20 * std::log10((g2 / ref2) / (g1 / ref1)) / std::log2(r2 / r1);
    double open = tone(24, fz(kFLadder), 0, 80) / tone(24, fz(kFDj), 0, 64);
    printf("test_wavetable: LADDER slope %.1f dB / octave, %.2f of the level well below its cutoff\n", slope, open);
    CHECK(std::fabs(slope + 24) < 3 && open > 0.7 && open < 1.2);
  }
  // the filter envelope (the mod envelope x FENV onto CUT): a saw through LP12 at a low CUT is bright at its start with
  // FENV +1, and as dull at the start as at the end with FENV 0
  char co[120];
  CHECK(wt::command("wt=fenv:7:-40", co, sizeof co) && strstr(co, "-40 %") && std::fabs(wt::fenv_get(7) + 0.4f) < 1e-6);
  CHECK(wt::command("wt=fenv:9:10", co, sizeof co) && strstr(co, "fenv:V:PERCENT") && std::fabs(wt::fenv_get(7) + 0.4f) < 1e-6);
  CHECK(wt::command("wt=fenv:7:101", co, sizeof co) && strstr(co, "fenv:V:PERCENT") && wt::command("wt=fenv:7:x", co, sizeof co) && strstr(co, "fenv:V:PERCENT"));
  for (float fe : {0.0f, 1.0f}) {
    char cmd[40];
    snprintf(cmd, sizeof cmd, "wt=fenv:7:%d", (int)(fe * 100));
    CHECK(wt::command(cmd, co, sizeof co) && std::fabs(wt::fenv_get(7) - fe) < 1e-6);
    wt::silence(7);
    render(7, 480);
    wg::trig(kW, 7, 48, 127, P(1, kSawPos, 0, 0, 0, 0, 30, 0, 0, 60, 127, 64, 0, 0, 0, fz(kFLp12)));   // ATK 0 DEC 60 (80 ms)
    std::vector<float> a = render(7, 2400), b = render(7, 24000);
    std::vector<float> c(b.end() - 9600, b.end());
    double ra = wg::rms(a, 480, a.size()), rc = wg::rms(c);
    printf("test_wavetable: FENV %+.0f: level at 10..50 ms %.4f, at 0.5 s %.4f\n", fe, ra, rc);
    if (fe == 0) CHECK(std::fabs(ra / rc - 1) < 0.05);
    else CHECK(ra > 3 * rc);
  }
  wt::set_fenv(7, 0);
  wt::reset();
}

// two tables hold two slots: the one a voice's B reads is not reloaded under it (the hazard slot rules, B's) and goes
// back to the others once B leaves it
static void test_b_slots() {
  const int t5 = entry("t5"), harm = entry("harm"), clm = entry("clm256"), akwf = entry("akwf600"), late = entry("late");
  CHECK(t5 > 0 && harm > 0 && clm > 0 && akwf > 0 && late > 0);
  for (int vi = 0; vi < 8; ++vi) wg::params(kPlaits, vi, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});   // every selection dropped
  idle();
  wg::params(kW, 6, P(1, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)t5, 0, 64));
  idle();
  CHECK(wt::table_frames(t5) > 0 && wt::voice_entry(6) == 1 && wt::voice_entry_b(6) == t5);
  const int three[3] = {harm, clm, akwf};
  for (int vi = 0; vi < 3; ++vi) wg::params(kW, vi, P((float)three[vi], 0));
  idle();
  for (int vi = 0; vi < 3; ++vi) CHECK(wt::table_frames(three[vi]) > 0);
  wg::params(kW, 3, P((float)late, 0));   // a fifth table: every slot is read by a voice, so it waits
  idle();
  CHECK(wt::table_frames(late) == 0 && wt::table_frames(t5) > 0 && wt::voice_entry_b(6) == t5);
  wg::trig(kW, 6, 60, 100, P(1, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)t5, 0, 64));
  render(6, 4800);
  CHECK(wt::voice_notes(6) == 1 && wt::table_frames(t5) > 0);
  wg::params(kW, 6, P(1, 0));   // B off: its slot goes back (after its fade out), the waiting table loads into it
  render(6, 4800);
  advance_ms(100);
  idle();
  CHECK(wt::voice_entry_b(6) == -1 && wt::table_frames(late) > 0 && wt::voice_entry(3) == late);
  // B on the built-in rows (no slot) and again OFF
  wg::params(kW, 6, P(1, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 3, 0, 64));
  CHECK(wt::voice_entry_b(6) == 3);
  wg::params(kW, 6, P(1, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, 0, 64));
  CHECK(wt::voice_entry_b(6) == -1);
  for (int vi = 0; vi < 8; ++vi) wg::off(vi);
  wt::reset();
}

// review fixes: B claims no slot while it cannot be heard, B fades in and out, the arpeggiator and the LFO treat P13..P16 as
// TBL B ... FTYPE, the new filters' memories hold no subnormals
static void test_review() {
  const int t5 = entry("t5"), t6 = entry("t6"), harm = entry("harm"), clm = entry("clm256"), akwf = entry("akwf600"), late = entry("late");
  // old projects: card rows in P13 at MIX 0 (chord words read as TBL B): four voices' card A tables all load and play
  for (int vi = 0; vi < 8; ++vi) wg::params(kPlaits, vi, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  advance_ms(100);
  idle();
  const int as[4] = {harm, clm, akwf, t5}, bs[4] = {t6, late, t6, late};
  for (int vi = 0; vi < 4; ++vi) wg::params(kW, vi, P((float)as[vi], 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)bs[vi], 0, 0));
  idle();
  for (int vi = 0; vi < 4; ++vi) CHECK(wt::table_frames(as[vi]) > 0 && wt::voice_entry(vi) == as[vi] && wt::voice_entry_b(vi) == -1);
  wt::reset();
  for (int vi = 0; vi < 4; ++vi) render(vi, 480);
  for (int vi = 0; vi < 4; ++vi) wg::trig(kW, vi, 60, 100, P((float)as[vi], 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)bs[vi], 0, 0));
  for (int vi = 0; vi < 4; ++vi) CHECK(wt::voice_notes(vi) == 1);
  // MIX up: B is wanted now, and loads (after the A tables, in a slot not read by a voice)
  wg::params(kW, 0, P((float)as[0], 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)bs[0], 0, 64));
  for (int vi = 0; vi < 8; ++vi) wg::off(vi), wt::silence(vi);
  idle();
  CHECK(wt::voice_entry_b(0) == -1 || wt::voice_entry_b(0) == bs[0]);
  for (int vi = 0; vi < 8; ++vi) wg::params(kPlaits, vi, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  advance_ms(100);
  idle();
  wt::reset();

  // B off at MIX 127 mid-note, and B arriving: no step above a bound (a sine's own is 0.02; a dropped B would be 0.3)
  auto steps = [&](const std::vector<float>& p1, const std::vector<float>& p2, int frames) {
    wt::silence(6);
    render(6, 480);
    wg::trig(kW, 6, 48, 127, p1);
    render(6, 9600);
    wg::params(kW, 6, p2);
    std::vector<float> y = render(6, frames);
    double mx = 0;
    for (size_t i = 1; i < y.size(); ++i) mx = std::max(mx, (double)std::fabs(y[i] - y[i - 1]));
    return mx;
  };
  const float p2 = (float)frame_pos(1, 8);
  double down = steps(P((float)harm, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)harm, p2, 127), P((float)harm, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, p2, 127), 4800);
  double up = steps(P((float)harm, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, p2, 127), P((float)harm, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)harm, p2, 127), 4800);
  double slide = steps(P((float)harm, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)harm, p2, 0), P((float)harm, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)harm, p2, 127), 4800);
  double ref = steps(P((float)harm, 0), P((float)harm, 0), 4800);
  printf("test_wavetable: largest step: a sine %.4f, B off at MIX 127 %.4f, B arriving %.4f, MIX 0 > 127 %.4f\n", ref, down, up, slide);
  CHECK(down < 0.06 && up < 0.06 && slide < 0.06);
  wt::reset();
  advance_ms(100);

  // the arpeggiator: one note a trig on a WAVETABLE voice whatever P13 holds (it read the chord words before)
  {
    wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 46, (uint8_t)(kW | 0), 0xF7});   // BD plays WAVETABLE
    wg::params(kW, 0, P(1, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 5, 0, 0));
    arp::Cfg up = arp::decode(0);
    up.mode = arp::kUp;
    arp::set(0, up);
    CHECK(arp::on(0));
    float pw[16] = {1, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 5, 0, 0, 0};
    CHECK(arp::trig(0, 60, 100, pw) && arp::held(0) == 1);
    arp::trig_off(0);
    arp::set(0, arp::decode(0));
    arp::end(0);
  }
  // the LFO's targets: P13..P16 of a WAVETABLE voice are not among them, and the labels say what they are
  {
    wg::params(kW, 3, P(1, 0));
    char b[8];
    CHECK(!strcmp(ui::voice_label(3, 12, b, sizeof b), "TBLB") && !strcmp(ui::voice_label(3, 13, b, sizeof b), "POSB") &&
          !strcmp(ui::voice_label(3, 14, b, sizeof b), "MIX") && !strcmp(ui::voice_label(3, 15, b, sizeof b), "FTYP"));
    lfo::Settings st = {};
    st.target = (uint8_t)(lfo::kTVoice + 3);
    int n = lfo::param_count(st);
    printf("test_wavetable: the LFO's targets on a WAVETABLE voice: %d parameters\n", n);
    CHECK(n == 12);
    wg::params(kPlaits, 3, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  }

  // no subnormal in the new filters' memories: a flat input (QUANT's end: the phase stops) decays them through the range
  for (int ft : {kFLp12, kFBp24, kFLadder}) {
    wt::silence(7);
    render(7, 480);
    wg::trig(kW, 7, 48, 127, P(1, kSawPos, wz(kWQuant), 127, 0, 0, 90, 20, 0, 127, 127, 64, 0, 0, 0, fz(ft)));
    bool seen = false;
    for (int b = 0; b < 12000; ++b) {
      render(7, kBlock);
      seen = seen || wt::denormal_state(7);
    }
    printf("test_wavetable: filter type %d held on a flat input for 3 s: subnormal state seen %d\n", ft, (int)seen);
    CHECK(!seen);
    wg::off(7);
  }
  wt::reset();
}

// the CPU a note costs on this Mac: one note plain, with 7 unison oscillators and a warp, a 6-note chord of those
static void test_cpu() {
  struct C { const char* name; std::vector<float> p; int notes; };
  const float ent_b = 6;   // SYNC as table B
  C cs[] = {{"1 note, 1 osc", P(1, 60), 1},
            {"1 note, 7 osc + FM warp + filter", P(1, 60, wz(kWFm), 60, 127, 60, 40, 60), 1},
            {"6-note chord, 7 osc each + filter", P(1, 60, wz(kWFm), 60, 127, 60, 40, 60), 6},
            {"1 note, 7 osc, B on, FM by B, LP24", P(1, 60, wz(kWFm), 60, 127, 60, 40, 60, 0, 127, 127, 64, ent_b, 60, 64, fz(kFLp24)), 1},
            {"1 note, 7 osc, B on, BEND, ladder", P(1, 60, wz(kWBend), 60, 127, 60, 60, 60, 0, 127, 127, 64, ent_b, 60, 64, fz(kFLadder)), 1},
            {"6-note chord, 7 osc each, B on, BEND, ladder", P(1, 60, wz(kWBend), 60, 127, 60, 60, 60, 0, 127, 127, 64, ent_b, 60, 64, fz(kFLadder)), 6}};
  for (C& c : cs) {
    wt::silence(7);
    render(7, 480);
    if (c.notes == 1) {
      wg::trig(kW, 7, 48, 100, c.p);
    } else {   // a chord from a poly track: its notes one by one
      wg::params(kW, 7, c.p);
      const int ch[6] = {0, 4, 7, 11, 14, 16};   // ADD9 and a 13th
      for (int k = 0; k < c.notes; ++k) CHECK(poly::seq_on(7, 48 + ch[k], 100));
    }
    render(7, 480);
    CHECK(wt::voice_notes(7) == c.notes);
    std::vector<float> o(kBlock);
    const int blocks = 40000;
    double t0 = wg::now_s();
    for (int b = 0; b < blocks; ++b) wt::render_add(7, o.data(), kBlock);
    double ns = (wg::now_s() - t0) / blocks * 1e9;
    printf("test_wavetable: CPU %s: %.0f ns a 12-frame block = %.2f %% of a core (Mac)\n", c.name, ns, ns / 250000 * 100);
  }
  wt::reset();
}

// a table that failed to load (here: a card file it could not open) is tried again when TBL picks it again; a voice
// that leaves WAVETABLE gives its card table back (with four card slots, four voices that left must not keep a fifth
// table from loading)
static void test_retry_release() {
  std::string W = g_card + "/WAVETABLES";
  auto tone = [](int h) {
    return wg::wav_bytes(4 * 2048, 1, 16, 48000, 1, "", [h](int i, int) { return 0.5 * std::sin(2 * M_PI * h * (i % 2048) / 2048.0); });
  };
  wg::write_file(W + "/t5.wav", tone(3));
  wg::write_file(W + "/t6.wav", tone(5));
  wg::write_file(W + "/late.wav", tone(7));
  CHECK(pu::put(pools::kTables, "A011", "/WAVETABLES/t5.wav") == pools::kOk);
  CHECK(pu::put(pools::kTables, "A012", "/WAVETABLES/t6.wav") == pools::kOk);
  idle();
  const int t5 = entry("t5"), t6 = entry("t6");
  CHECK(t5 > 0 && t6 > 0);
  // four voices on four card tables: every slot selected
  const int four[4] = {entry("harm"), entry("clm256"), entry("akwf600"), t5};
  for (int vi = 0; vi < 4; ++vi) wg::params(kW, vi, P((float)four[vi], 0));
  idle();
  for (int vi = 0; vi < 4; ++vi) CHECK(wt::table_frames(four[vi]) > 0 && wt::voice_entry(vi) == four[vi]);
  // they leave WAVETABLE (PLAITS): a fifth table on voice 4 loads into a slot they gave back
  for (int vi = 0; vi < 4; ++vi) wg::params(kPlaits, vi, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  // a pool change after they left (the list composed again, every voice's selection redone) does not take them back
  CHECK(pu::put(pools::kTables, "A013", "/WAVETABLES/late.wav") == pools::kOk);
  idle();
  const int late = entry("late");
  CHECK(late > 0);
  for (int vi = 0; vi < 4; ++vi) CHECK(wt::voice_entry(vi) == -1);
  wg::params(kW, 4, P((float)t6, 0));
  idle();
  CHECK(wt::table_frames(t6) > 0 && wt::voice_entry(4) == t6);
  // a load that fails (the file cannot be opened), the log says so, the old table plays on (root opens any file:
  // the case needs a user)
  if (geteuid() == 0) {
    wt::reset();
    return;
  }
  CHECK(chmod((W + "/late.wav").c_str(), 0) == 0);
  uint32_t fails = wt::stats().load_fails;
  wg::params(kW, 5, P((float)late, 0));
  idle();
  CHECK(wt::stats().load_fails == fails + 1 && wt::entry_state(late) == 5 && wt::table_frames(late) == 0);
  CHECK(chmod((W + "/late.wav").c_str(), 0644) == 0);
  wg::params(kW, 5, P((float)late, 0));   // the same TBL again (a report, a trig): no new load
  idle();
  CHECK(wt::stats().load_fails == fails + 1 && wt::entry_state(late) == 5);
  wg::params(kW, 5, P(1, 0));             // TBL away and back: tried again, and in
  wg::params(kW, 5, P((float)late, 0));
  idle();
  CHECK(wt::table_frames(late) > 0 && wt::voice_entry(5) == late && wt::entry_state(late) == 3);
  for (int vi = 0; vi < 6; ++vi) wg::off(vi);
  wt::reset();
}

// 2026-11-24 TBLB (the pool browser for table B, P13). pick_b is pick for P13: 48 with k = 12 (never k = 0), vp[12], select_b; A is
// untouched, and a pick of A is as before. B is silent at MIX 0 unless the warp is FM or RM (select_b): b_silent says so (the
// touchscreen's note). The Rytm's own pick of P13 on an idle voice (2A, k = 12) is kit_src_b: the table loads with no note.
static std::vector<std::vector<uint8_t>> g_tx;
static void test_pick_b() {
  const int t = 0, vi = 0;
  const int ea = entry("harm"), eb = entry("clm256");
  CHECK(ea > 0 && eb > 0);
  void (*old_tx)(const uint8_t*, int) = hooks.to_rytm;
  hooks.to_rytm = [](const uint8_t* d, int n) { g_tx.emplace_back(d, d + n); };
  auto w48 = [&](int k) {   // 48 <track> <k> sent since the last clear
    for (auto& m : g_tx)
      if (m.size() >= 10 && m[4] == 0x48 && m[6] == t && m[7] == k) return true;   // (10 bytes: no bank byte in bank A)
    return false;
  };
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 46, (uint8_t)(kW | vi), 0xF7});   // BD on WAVETABLE
  wg::params(kW, vi, P((float)ea, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, 0, 64));          // MIX 64, B OFF
  idle();
  CHECK(wt::voice_entry(vi) == ea && wt::voice_entry_b(vi) == -1 && !wt::b_silent(vi));
  g_tx.clear();
  CHECK(wt::pick_b(vi, eb));
  idle();
  CHECK(wt::voice_entry_b(vi) == eb && wt::voice_entry(vi) == ea);   // B in, A as it was
  CHECK(w48(12) && !w48(0));                                         // P13 written, never P1
  CHECK(!wt::b_silent(vi));
  // a pick of A: P1 only, B stays (the same as before this change)
  g_tx.clear();
  CHECK(wt::pick(vi, eb) && w48(0) && !w48(12));
  idle();
  CHECK(wt::voice_entry(vi) == eb && wt::voice_entry_b(vi) == eb);
  CHECK(wt::pick(vi, ea));
  idle();
  // MIX 0: B is picked and silent (no table wanted, nothing playing from it); FM / RM use it as the modulator
  wg::params(kW, vi, P((float)ea, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)eb, 0, 0));
  idle();
  CHECK(wt::voice_entry_b(vi) == -1 && wt::b_silent(vi));
  CHECK(wt::pick_b(vi, eb) && wt::b_silent(vi) && wt::voice_entry_b(vi) == -1);
  wg::params(kW, vi, P((float)ea, 0, wz(kWFm), 64, 0, 0, 64, 0, 0, 127, 127, 64, (float)eb, 0, 0));
  idle();
  CHECK(!wt::b_silent(vi) && wt::voice_entry_b(vi) == eb);
  wg::params(kW, vi, P((float)ea, 0, wz(kWRm), 64, 0, 0, 64, 0, 0, 127, 127, 64, (float)eb, 0, 0));
  idle();
  CHECK(!wt::b_silent(vi) && wt::voice_entry_b(vi) == eb);
  wg::params(kW, vi, P((float)ea, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, (float)eb, 0, 64));
  idle();
  CHECK(!wt::b_silent(vi) && wt::voice_entry_b(vi) == eb);
  // OFF (row 000): B off, and that is not "silent by MIX"
  CHECK(wt::pick_b(vi, 0));
  idle();
  CHECK(wt::voice_entry_b(vi) == -1 && !wt::b_silent(vi));
  g_tx.clear();
  CHECK(!wt::pick_b(vi, wt::entries()) && !wt::pick_b(vi, -1) && g_tx.empty());   // no such entry: nothing written
  // the console: wt=pickb:V:ROW
  char o[200];
  g_tx.clear();
  CHECK(wt::command("wt=pickb:0:A010", o, sizeof o));
  idle();
  CHECK(wt::voice_entry_b(vi) == 10 && w48(12) && !w48(0) && strstr(o, "TBLB"));
  CHECK(wt::command("wt=pickb:9:A010", o, sizeof o) && strstr(o, "pickb:V:ROW"));
  // the Rytm's own pick of P13 on an idle voice (the SRC view's 2A, k = 12): kit_src_b, no note, no write back
  CHECK(wt::pick_b(vi, 0));
  idle();
  CHECK(wt::voice_entry_b(vi) == -1);
  uint32_t trigs = wt::stats().trigs;
  g_tx.clear();
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 1, 0xF7});   // its SRC view
  g_tx.clear();
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 12, 1, (uint8_t)(eb & 127), (uint8_t)srclib::lo_of(eb), 0xF7});
  idle();
  CHECK(wt::voice_entry_b(vi) == eb && wt::voice_entry(vi) == ea && wt::stats().trigs == trigs && !w48(12));
  wt::kit_src_b(vi, 0);   // OFF via the same path
  idle();
  CHECK(wt::voice_entry_b(vi) == -1);
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 0, 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 0, 0x7F, 0xF7});   // not ours again
  wt::reset();
  hooks.to_rytm = old_tx;
  printf("test_wavetable: pick_b writes P13 only (48 k = 12), A as before, silent at MIX 0 (not FM / RM), OFF, wt=pickb, 2A's P13 on an idle voice\n");
}

// The Rytm's value text names one list's bank (47's bank byte): the bank of the SRC view's P1, now of whichever of P1 / P13 was
// touched last (TBL and TBLB share list 1 and so one slot); a list open for a P13 follows its own bank.
static void test_names_bank() {
  const int t = 0, vi = 0;
  void (*old_tx)(const uint8_t*, int) = hooks.to_rytm;
  hooks.to_rytm = [](const uint8_t* d, int n) { g_tx.emplace_back(d, d + n); };
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 46, (uint8_t)(kW | vi), 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 1, 0xF7});   // the SRC view on
  uint32_t ms = now_ms() + 100000;
  auto bank47 = [&]() {   // the bank byte of the last 47 for list 1 (TBL) in g_tx, -1 none
    int b = -1;
    for (auto& m : g_tx)
      if (m.size() >= 11 && m[4] == 0x47 && m[6] == 1) b = m[9];
    return b;
  };
  auto touch = [&](int k, int row, int bank) {   // a single 2A value, then the host's tick
    g_tx.clear();
    wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, (uint8_t)k, 1, (uint8_t)row, (uint8_t)(bank << 4), 0xF7});
    for (int i = 0; i < 3; ++i) ui::remote_tick(ms += 100);
  };
  touch(0, 3, 0);
  CHECK(bank47() == 0);   // P1 in bank A: A's names
  touch(12, 2, 5);
  CHECK(bank47() == 5);   // P13 touched last, in bank F: F's names (the knob in hand reads right)
  touch(0, 4, 2);
  CHECK(bank47() == 2);   // P1 again (bank C)
  touch(0, 4, 3);         // P1 by itself: its own bank
  CHECK(bank47() == 3);
  touch(12, 6, 7);
  CHECK(bank47() == 7);
  g_tx.clear();
  for (int i = 0; i < 3; ++i) ui::remote_tick(ms += 100);
  CHECK(bank47() == -1);   // (nothing changed: no resend)
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 0, 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 0, 0x7F, 0xF7});
  wt::reset();
  hooks.to_rytm = old_tx;
  printf("test_wavetable: the names bank follows the knob last touched (P1 or P13)\n");
}

// Fix round 1 (review M1, L1, L2, L3). An idle voice's 2A of P13: the FIRST sight of a track's P13 (a kit load, a report) is no
// change, so an old kit's stale P13 loads no table B and marks no row "used"; a later real change selects B only while B can be
// heard (MIX above 0 or FM / RM, by the voice's values), and marks used only then. P1 and P13 changed in one frame: P1 first (the
// names bank is P1's). A write of P13 is mirrored at once (the names bank), and only on a WAVETABLE track.
static void test_p13_first_sight() {
  const int t = 1, vi = 2;
  const int ea = entry("harm"), eb = 4;   // (the built-in ORGAN / FM: never reported or picked by the tests before)
  void (*old_tx)(const uint8_t*, int) = hooks.to_rytm;
  hooks.to_rytm = [](const uint8_t* d, int n) { g_tx.emplace_back(d, d + n); };
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 46, (uint8_t)(kW | vi), 0xF7});
  wg::params(kW, vi, P((float)ea, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, 0, 64));   // MIX 64 (stale: a previous sound's), B OFF
  idle();
  CHECK(wt::voice_entry(vi) == ea && wt::voice_entry_b(vi) == -1 && !pools::used(pools::kTables, eb));
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 1, 0xF7});
  // the first 2A of P13 (an old kit with a nonzero P13, MIX audible by the stale values): no load, no used mark
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 12, 1, (uint8_t)(eb & 127), (uint8_t)srclib::lo_of(eb), 0xF7});
  idle();
  CHECK(wt::voice_entry_b(vi) == -1 && !pools::used(pools::kTables, eb));
  // a real change after that selects B (MIX audible) and marks it used
  const int ec = 5;
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 12, 1, (uint8_t)(ec & 127), (uint8_t)srclib::lo_of(ec), 0xF7});
  idle();
  CHECK(wt::voice_entry_b(vi) == ec && pools::used(pools::kTables, ec) && wt::voice_entry(vi) == ea);
  // a real change while B cannot be heard (MIX 0, by the voice's values): no load, no mark
  wg::params(kW, vi, P((float)ea, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, 0, 0));   // MIX 0
  idle();
  CHECK(wt::voice_entry_b(vi) == -1);
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 12, 1, (uint8_t)(eb & 127), (uint8_t)srclib::lo_of(eb), 0xF7});
  idle();
  CHECK(wt::voice_entry_b(vi) == -1 && !pools::used(pools::kTables, eb));
  // a frame of several values (a kit's load, with P13 changed): no selection either
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 11, 2, 0, 0, 1, (uint8_t)(ec & 127), 0xF7});
  wg::params(kW, vi, P((float)ea, 0, 0, 0, 0, 0, 64, 0, 0, 127, 127, 64, 0, 0, 64));
  idle();
  CHECK(wt::voice_entry_b(vi) == -1);
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 0, 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 0, 0x7F, 0xF7});
  wt::reset();
  hooks.to_rytm = old_tx;
  printf("test_wavetable: P13's first sight is no change (no load, no used mark), a real change selects B only while heard\n");
}

static void test_names_bank_rules() {
  const int t = 0, vi = 0;
  void (*old_tx)(const uint8_t*, int) = hooks.to_rytm;
  hooks.to_rytm = [](const uint8_t* d, int n) { g_tx.emplace_back(d, d + n); };
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 46, (uint8_t)(kW | vi), 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 1, 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 0, 1, 2, 0, 0xF7});   // P1 touched: the names are P1's
  CHECK(!ui::remote_track_names_p13(t));
  // L1: P1 and P13 change in one frame: P1 first (the names are P1's), even when P13 was the knob touched last
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 12, 1, 5, 0x20, 0xF7});
  CHECK(ui::remote_track_names_p13(t));
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 0, 2, 3, 0, 4, 0, 0xF7});   // P1 and P2
  CHECK(!ui::remote_track_names_p13(t));
  std::vector<uint8_t> both = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 0, 13};
  for (int i = 0; i < 13; ++i) both.push_back((uint8_t)(i == 0 ? 9 : i == 12 ? 6 : 1)), both.push_back(0x10);
  both.push_back(0xF7);
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 12, 1, 7, 0x30, 0xF7});
  CHECK(ui::remote_track_names_p13(t));
  wg::send(both);   // P1 and P13 both new
  CHECK(!ui::remote_track_names_p13(t));
  // L3: a written P13 (the touchscreen) is the track's P13 word at once, and the names follow it
  CHECK(ui::remote_track_write(t, 12, 4, 0x20, 1));
  CHECK(ui::remote_track_names_p13(t) && ui::remote_track_src_word(t, 12) == (4 << 7 | 0x20));
  // L2: not on a track whose machine is not WAVETABLE
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 0, 0x7F, 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 0, 1, 1, 0, 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 46, (uint8_t)(kGranMachine | vi), 0xF7});   // GRANULAR (16 params too)
  CHECK(!ui::remote_track_names_p13(t));
  ui::remote_track_write(t, 12, 4, 0x20, 1);
  CHECK(!ui::remote_track_names_p13(t));
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, 0, 0xF7});
  wg::send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 0, 0x7F, 0xF7});
  wt::reset();
  hooks.to_rytm = old_tx;
  printf("test_wavetable: P1 first when both change, a written P13 mirrored at once, p13_last only on WAVETABLE\n");
}

int main() {
  test_wav_read();
  test_frames();
  test_levels();
  setup_card();
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, g_us);
  voices_init();
  card_ok = true;
  srclib::begin(true);
  pools::begin(true);
  wt::begin(true);
  pu::steps();
  CHECK(pu::put(pools::kTables, "A007", "/WAVETABLES/akwf600.wav") == pools::kOk);
  CHECK(pu::put(pools::kTables, "A008", "/WAVETABLES/broken.wav") == pools::kOk);
  CHECK(pu::put(pools::kTables, "A009", "/WAVETABLES/harm.wav") == pools::kOk);
  CHECK(pu::put(pools::kTables, "A010", "/WAVETABLES/sub/clm256.wav") == pools::kOk);
  idle();
  test_list_and_load();
  test_idle_pick();
  test_pick_b();
  test_names_bank();
  test_p13_first_sight();
  test_names_bank_rules();
  test_morph();
  test_alias();
  test_unison();
  test_chords_live();
  test_golden();
  test_table_b();
  test_warps_filters();
  test_filters();
  test_cpu();
  test_retry_release();
  test_b_slots();
  test_review();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_wavetable: PASS\n");
  return 0;
}
