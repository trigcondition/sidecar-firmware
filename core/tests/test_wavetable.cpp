// core/tests/test_wavetable.cpp - WAVETABLE (core/wavetable): the WAV reader (formats, 'clm ' hints, extensible),
// frame size detection and thinning, the harmonics and mip levels (no energy above a level's top harmonic), then the
// machine through the engine's own entry (host_sysex with the 0x28 flag, and wt=voice:V): the card's list, a trig
// waiting for its table, frames loaded as written, aliasing at a high note (mip levels against a level-0 read), the
// morph's continuity across a POS sweep, unison detune, warps, chords, live notes, the headroom; golden renders
// (features within tolerance) and the CPU a note costs.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "poly.h"
#include "voices.h"
#include "wavetable/tables.h"
#include "wavetable/wav_read.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"
#include "pool_util.h"
#include "pool/pool.h"

using namespace core;

static constexpr int kW = kWaveMachine;
// P1..P16: TBL POS WARP AMT UNI SUB CUT RES ATK DEC SUS ENV CHD INV SPR VOI
static std::vector<float> P(float tbl, float pos, float warp = 0, float amt = 0, float uni = 0, float sub = 0,
                            float cut = 64, float res = 0, float atk = 0, float dec = 127, float sus = 127, float env = 64,
                            float chd = 0, float voi = 0) {
  return {tbl, pos, warp, amt, uni, sub, cut, res, atk, dec, sus, env, chd, 0, 0, voi};
}
static const float kSawPos = 84 + 85 / 128.0f;   // BASIC's saw frame (42 of 64)

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
    wg::trig(kW, 0, 48, 127, P(1, 0, (float)(w * 32 + 1), 100));
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

// chords (P13..P16), the headroom, a 0x11's release, live notes and the sequencer's
static void test_chords_live() {
  wg::trig(kW, 4, 60, 100, P(1, 30, 0, 0, 0, 0, 64, 0, 0, 40, 100, 64, 1 /*MAJ*/));
  render(4, 2400);
  CHECK(wt::voice_notes(4) == 3 && std::fabs(wt::headroom(4) - 1 / std::sqrt(3.0 * 100 / 127)) < 0.08);
  wg::trig(kW, 4, 62, 100, P(1, 30, 0, 0, 0, 0, 64, 0, 0, 40, 100, 64, 6 /*MAJ7*/));   // the old chord's tails go on
  render(4, 240);
  CHECK(wt::voice_notes(4) == 7);
  wg::off(4);
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

// golden renders: a pluck, a pad and a sync lead, their level and brightness within 10 % of what they were
// when this machine was written (docs/WAVETABLE.md); WG_RENDER_DIR=... writes them as WAVs
static void test_golden() {
  struct G { const char* name; int key; std::vector<float> p; double rms, zc; };
  G gs[] = {
      {"pluck", 48, P(2, 20, 0, 0, 40, 0, 40, 30, 0, 75, 0, 100), 0, 0},
      {"pad", 60, P(1, 60, 0, 0, 110, 40, 50, 10, 60, 90, 110, 80, 7 /*MIN7*/), 0, 0},
      {"sync", 55, P(6, 90, 32, 70, 30, 20, 58, 50, 0, 70, 90, 40), 0, 0},
      {"vocal", 52, P(3, 0, 96, 40, 60, 0, 64, 0, 10, 110, 120, 127), 0, 0},
  };
  const double kRms[] = {0.0177, 0.1777, 0.1207, 0.0582}, kZc[] = {265, 558, 5830, 2291};
  const char* dir = getenv("WG_RENDER_DIR");
  for (size_t i = 0; i < sizeof gs / sizeof gs[0]; ++i) {
    G& g = gs[i];
    wt::silence(6);
    render(6, 480);
    wg::trig(kW, 6, g.key, 110, g.p);
    std::vector<float> y = render(6, 48000);
    wg::off(6);
    std::vector<float> t = render(6, 24000);
    y.insert(y.end(), t.begin(), t.end());
    g.rms = wg::rms(y), g.zc = wg::zc_freq(y);
    printf("test_wavetable: golden %s: rms %.4f, crossings %.0f a second (golden %.4f, %.0f)\n", g.name, g.rms, g.zc, kRms[i], kZc[i]);
    CHECK(std::fabs(g.rms / kRms[i] - 1) < 0.1 && std::fabs(g.zc / kZc[i] - 1) < 0.1);
    if (dir) wg::write_wav16(std::string(dir) + "/wt_" + g.name + ".wav", y);
  }
  wt::reset();
}

// the CPU a note costs on this Mac: one note plain, with 7 unison oscillators and a warp, a 6-note chord of those
static void test_cpu() {
  struct C { const char* name; std::vector<float> p; int notes; };
  C cs[] = {{"1 note, 1 osc", P(1, 60), 1},
            {"1 note, 7 osc + FM warp + filter", P(1, 60, 100, 60, 127, 60, 40, 60), 1},
            {"6-note chord, 7 osc each + filter", P(1, 60, 100, 60, 127, 60, 40, 60, 0, 127, 127, 64, 13 /*ADD9*/, 6), 6}};
  for (C& c : cs) {
    wt::silence(7);
    render(7, 480);
    wg::trig(kW, 7, 48, 100, c.p);
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

int main() {
  test_wav_read();
  test_frames();
  test_levels();
  setup_card();
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, 1000000);
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
  test_morph();
  test_alias();
  test_unison();
  test_chords_live();
  test_golden();
  test_cpu();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_wavetable: PASS\n");
  return 0;
}
