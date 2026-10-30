// core/tests/test_granular.cpp - GRANULAR (core/granular): the source list (live, built-in, the card's), a card sample
// loaded whole (any format, its rate folded into the pitch) and a trig waiting for it; grain scheduling (DENS grains a
// second a note, chords' notes each), the live ring (the IN stream's frames as sent, wrapping), freeze (the ring holds,
// the grains play on), feedback, reverse, the grain cap; golden renders and the CPU a grain costs.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "fx/out.h"
#include "granular/granular.h"
#include "pool/pool.h"
#include "pool_util.h"
#include "poly.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;

static constexpr int kG = kGranMachine;
// P1..P16: SRC POS SPRY SIZE DENS PRND REV WIN ATK REL FRZ FB CHD INV SPR VOI
static std::vector<float> P(float src, float pos = 0, float spry = 0, float size = 64, float dens = 64, float prnd = 0,
                            float rev = 0, float win = 0, float atk = 0, float rel = 40, float frz = 0, float fb = 0,
                            float chd = 0) {
  return {src, pos, spry, size, dens, prnd, rev, win, atk, rel, frz, fb, chd, 0, 0, 0};
}
// knob values for a size in seconds, a density in grains a second
static float size_v(double s) { return (float)(127 * std::log(s / 0.005) / std::log(200.0)); }
static float dens_v(double d) { return (float)(127 * std::log(d) / std::log(200.0)); }

static uint32_t g_phase = 0;   // the live input's test signal: a ramp counter on IN L/R, a sine on MAIN
static void feed(int frames, bool on = true) {
  int32_t f[7 * 12];
  for (int done = 0; done < frames;) {
    int n = frames - done < 7 ? frames - done : 7;
    for (int i = 0; i < n; ++i) {
      uint32_t t = g_phase++;
      int32_t ramp = on ? (int32_t)((t % 1000) * 2000000) - 1000000000 : 0;   // -0.47 .. 0.46
      int32_t s = on ? (int32_t)(1.0e9 * std::sin(2 * M_PI * 220.0 * t / 48000)) : 0;
      for (int c = 0; c < 12; ++c) f[i * 12 + c] = c >= 10 ? ramp : c < 2 ? s : 0;
    }
    gr::live_in(f, n, 12);
    done += n;
  }
}
static void idle() {
  for (int i = 0; i < 4000 && gr::step(); ++i) {}
}
// the machine alone: -1..1 (live: kBlock frames of input fed before each block, as the IN stream keeps time)
static std::vector<float> render(int vi, int frames, bool live_feed = false) {
  std::vector<float> o(frames, 0.0f);
  for (int s = 0; s < frames; s += kBlock) {
    idle();
    if (live_feed) feed(kBlock);
    gr::render_add(vi, o.data() + s, frames - s < kBlock ? frames - s : kBlock);
  }
  return o;
}

static std::string g_card;
static void setup_card() {
  char tmpl[] = "/tmp/gr-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  std::string G = g_card + "/GRANULAR";
  CHECK(system(("mkdir -p '" + G + "/sub'").c_str()) == 0);
  // 3 s of a 441 Hz sine at 44.1 kHz, 24-bit stereo (L and R in phase)
  wg::write_file(G + "/sine441.wav", wg::wav_bytes(3 * 44100, 2, 24, 44100, 1, "", [](int i, int) {
                   return 0.5 * std::sin(2 * M_PI * 441.0 * i / 44100);
                 }));
  // 2 s at 48 kHz of noise bursts (16-bit), for the golden
  wg::write_file(G + "/sub/bursts.wav", wg::wav_bytes(96000, 1, 16, 48000, 1, "", [](int i, int) {
                   uint32_t r = (uint32_t)i * 2654435761u;
                   double env = std::exp(-((i % 12000) / 2000.0));
                   return env * ((double)(r >> 8) / 16777216.0 - 0.5);
                 }));
  wg::write_file(G + "/bad.wav", std::vector<uint8_t>(60, 1));
  // 1.013 s of a 220 Hz sine at 48 kHz: its end mid-cycle (a grain across the end would read a step), for the clicks
  wg::write_file(G + "/wrap220.wav", wg::wav_bytes(48624, 1, 16, 48000, 1, "", [](int i, int) {
                   return 0.5 * std::sin(2 * M_PI * 220.0 * i / 48000);
                 }));
  host_set_card(g_card.c_str());
}
// the rows that hold something (the SAMPLES pool's)
static int card_entries() {
  int n = 0;
  for (int e = 0; e < gr::entries(); ++e) n += gr::entry_lib(e) == srclib::kLibNone;
  return n;
}
// the old list's layout in the SAMPLES pool (the tests' P1 values): the inputs A001..A010, PAD BELLS A011 A012, the
// card's files from A013 by path
static void pool_layout() {
  pu::clear(pools::kSamples);
  for (int i = 0; i < pools::kInputs; ++i) {
    char r[8], n[16];
    snprintf(r, sizeof r, "A%03d", i + 1), snprintf(n, sizeof n, "@%s", pools::input_name(i));
    CHECK(pu::put(pools::kSamples, r, n) == pools::kOk);
  }
  CHECK(pu::put(pools::kSamples, "A011", "@PAD") == pools::kOk && pu::put(pools::kSamples, "A012", "@BELLS") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A013", "/GRANULAR/bad.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A014", "/GRANULAR/sine441.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A015", "/GRANULAR/sub/bursts.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A016", "/GRANULAR/wrap220.wav") == pools::kOk);
}
static int entry(const char* name) {
  for (int e = 0; e < gr::entries(); ++e)
    if (!strcmp(gr::entry_name(e), name)) return e;
  return -1;
}

static void test_list_and_load() {
  // the SAMPLES pool: A000 OFF, the inputs, PAD BELLS, then the card's (pool_layout; the template's rows cleared)
  CHECK(card_entries() == gr::kLive + gr::kBuiltins + 4);
  CHECK(!strcmp(gr::entry_name(0), "OFF") && gr::entry_lib(0) == srclib::kLibOff);
  CHECK(!strcmp(gr::entry_name(1), "IN") && !strcmp(gr::entry_name(2), "MAIN") && !strcmp(gr::entry_name(10), "CY-CB"));
  CHECK(!strcmp(gr::entry_name(11), "PAD") && !strcmp(gr::entry_name(12), "BELLS") && !strcmp(gr::entry_group(1), "LIVE"));
  CHECK(entry("bad") == 13 && entry("sine441") == 14 && entry("bursts") == 15 && entry("wrap220") == 16);
  CHECK(!strcmp(gr::entry_name(7 * 128 + 1), "---") && !strcmp(gr::entry_group(14), "BANK A"));
  int e = entry("sine441");
  gr::silence(0);
  // a trig before its sample is in: waits, then plays; 100 ms grains, 40 a second, FRZ on (no scan), key 60: 441 Hz
  wg::trig(kG, 0, 60, 127, P((float)e, 64, 0, size_v(0.1), dens_v(40), 0, 0, 0, 0, 40, 127));
  CHECK(gr::stats().waits == 1 && gr::voice_notes(0) == 0);
  idle();
  CHECK(gr::voice_entry(0) == e && gr::voice_notes(0) == 1 && gr::stats().loads == 1);
  std::vector<float> o = render(0, 48000);
  double f = wg::zc_freq(o, 9600, 48000);
  printf("test_granular: sine441 at key 60: %.1f Hz, rms %.3f\n", f, wg::rms(o, 9600, 48000));
  CHECK(std::fabs(f - 441) < 3 && wg::rms(o, 9600, 48000) > 0.1);
  wg::trig(kG, 0, 72, 127, P((float)e, 64, 0, size_v(0.1), dens_v(40), 0, 0, 0, 0, 40, 127));   // an octave up
  o = render(0, 48000);
  f = wg::zc_freq(o, 9600, 48000);
  CHECK(std::fabs(f - 882) < 6);
  uint32_t fails = gr::stats().load_fails;
  wg::trig(kG, 1, 60, 100, P((float)entry("bad")));
  idle();
  CHECK(gr::stats().load_fails == fails + 1);
  gr::reset();
}

// scheduling: DENS grains a second a note; a chord's notes each their own stream; size sets each grain's length
static void test_density() {
  for (double d : {5.0, 20.0, 80.0}) {
    gr::silence(2);
    render(2, 4800);
    uint32_t g0 = gr::stats().grains;
    wg::trig(kG, 2, 60, 100, P(11, 30, 0, size_v(0.02), dens_v(d)));
    render(2, 48000);
    uint32_t n = gr::stats().grains - g0;
    printf("test_granular: DENS %.0f a second: %u grains in 1 s\n", d, n);
    CHECK(std::fabs((double)n - d) <= d * 0.05 + 1);
  }
  gr::silence(2);
  render(2, 4800);
  uint32_t g0 = gr::stats().grains;
  wg::trig(kG, 2, 60, 100, P(11, 30, 0, size_v(0.02), dens_v(20), 0, 0, 0, 0, 40, 0, 0, 2 /*MIN*/));
  CHECK(gr::voice_notes(2) == 3);
  render(2, 48000);
  CHECK(std::fabs((double)(gr::stats().grains - g0) - 60) <= 4);
  // overlap: 50 grains a second of 200 ms: about 10 sounding at once
  wg::trig(kG, 2, 60, 100, P(11, 30, 0, size_v(0.2), dens_v(50)));
  render(2, 24000);
  int now = gr::voice_grains(2);
  printf("test_granular: 50 a second of 200 ms: %d grains sounding\n", now);
  CHECK(now >= 8 && now <= 12);
  // spray: the timing jitters (no fixed period), the count stays
  g0 = gr::stats().grains;
  wg::trig(kG, 2, 60, 100, P(11, 30, 127, size_v(0.02), dens_v(40)));
  render(2, 48000);
  CHECK(std::fabs((double)(gr::stats().grains - g0) - 40) <= 8);
  gr::reset();
}

// the live ring: the IN stream's frames as sent (IN = L + R halves summed), across its wrap; freeze; feedback
static void test_live_ring() {
  gr::silence(3);
  wg::trig(kG, 3, 60, 100, P(1 /*IN: A001*/, 0, 0, size_v(0.05), dens_v(30), 0, 0, 0, 0, 40));
  render(3, 480, true);   // the first block takes the source (no backlog)
  uint32_t w0 = gr::ring_written(3), p0 = g_phase;
  render(3, (int)gr::kRing + 48000, true);   // past the ring's wrap
  uint32_t w1 = gr::ring_written(3);
  CHECK(w1 - w0 == g_phase - p0);
  int bad = 0;
  for (uint32_t k = 1; k < 20000; ++k) {   // the newest 20000 frames: the ramp as fed (int16 in the capture)
    uint32_t t = g_phase - k;
    float want = (float)((int32_t)((t % 1000) * 2000000) - 1000000000) / 2147483648.0f;
    if (std::fabs(gr::ring_at(3, w1 - k) - want) > 1e-4f) ++bad;
  }
  CHECK(bad == 0);
  std::vector<float> o = render(3, 9600, true);
  CHECK(wg::rms(o) > 0.02);   // the grains play the input
  // freeze: the ring stops (the input goes on), the grains play on what it holds
  wg::params(kG, 3, P(1, 0, 0, size_v(0.05), dens_v(30), 0, 0, 0, 0, 40, 127));
  render(3, 240, true);
  uint32_t wf = gr::ring_written(3);
  o = render(3, 48000, true);
  CHECK(gr::ring_written(3) == wf && wg::rms(o) > 0.02);
  feed(0);
  o = render(3, 24000, false);   // no input at all now: frozen, it still sounds
  CHECK(wg::rms(o) > 0.02);
  wg::params(kG, 3, P(1, 0, 0, size_v(0.05), dens_v(30), 0, 0, 0, 0, 40, 0));   // thawed: recording again
  render(3, 4800, true);
  CHECK(gr::ring_written(3) > wf);
  // feedback (HOLD below FREEZE on a live source): an input that stops; with it the ring keeps sounding, without it the ring is silent
  double keep[2];
  for (int fb = 0; fb < 2; ++fb) {
    gr::silence(3);
    render(3, 480);
    wg::trig(kG, 3, 60, 127, P(1, 0, 0, size_v(0.08), dens_v(40), 0, 0, 0, 0, 40, fb ? 113.0f : 0.0f));
    render(3, 24000, true);
    for (int b = 0; b < 400; ++b) {   // silence in for 0.1 s .. then the ring's newest frames
      feed(kBlock, false);
      float x[kBlock] = {0};
      gr::render_add(3, x, kBlock);
    }
    double e = 0;
    uint32_t w = gr::ring_written(3);
    for (uint32_t k = 1; k < 2400; ++k) e += std::fabs(gr::ring_at(3, w - k));
    keep[fb] = e / 2400;
  }
  printf("test_granular: feedback: the ring's newest 50 ms after the input stopped: %.4f without, %.4f with\n", keep[0], keep[1]);
  CHECK(keep[0] < 1e-6 && keep[1] > 0.005);
  gr::reset();
}

// reverse: a rising ramp read backwards falls; the grain cap holds
static void test_reverse_cap() {
  double slope[2];
  for (int r = 0; r < 2; ++r) {
    gr::silence(4);
    wg::trig(kG, 4, 60, 127, P(1, 20, 0, size_v(0.01), dens_v(10), 0, r ? 127.0f : 0.0f, 80 /*TUKEY*/, 0, 40));
    render(4, 48000, true);
    std::vector<float> o = render(4, 48000, true);
    double s = 0;   // the ramp's own steps (0.00093 a frame x the grain gain 1.6) in the window's flat part: up or down
    for (size_t i = 1; i < o.size(); ++i) {
      double d = o[i] - o[i - 1];
      s += d > 0.001 && d < 0.002 ? 1 : d < -0.001 && d > -0.002 ? -1 : 0;
    }
    slope[r] = s / (10 * 240);   // per flat frame: 10 grains a second, 240 flat frames each
  }
  printf("test_granular: the ramp's slope through the grains: %.2f forward, %.2f reversed\n", slope[0], slope[1]);
  CHECK(slope[0] > 0.8 && slope[1] < -0.8);
  gr::set_cap(8);
  uint32_t c0 = gr::stats().capped;
  wg::trig(kG, 4, 60, 127, P(11, 30, 0, size_v(0.5), dens_v(200)));
  int most = 0;
  for (int b = 0; b < 200; ++b) {
    render(4, kBlock);
    most = std::max(most, gr::voice_grains(4));
  }
  printf("test_granular: cap 8: at most %d grains, %u left out\n", most, gr::stats().capped - c0);
  CHECK(most <= 8 && gr::stats().capped > c0);
  char o[300];
  CHECK(gr::command("gr=grains:500", o, sizeof o) && gr::cap() == gr::kMaxGrains);
  CHECK(gr::command("gr=grains:64", o, sizeof o) && gr::cap() == 64);
  gr::reset();
}

// live pads, the sequencer's notes, gr=voice:V
static void test_live_notes() {
  gr::live_on(5, 60, 100, 2, 60);
  gr::live_on(5, 64, 100, 2, 64);
  CHECK(gr::voice_notes(5) == 2);
  gr::note_off(5);   // a trig's release leaves the pads' notes
  render(5, 4800);
  CHECK(gr::voice_notes(5) == 2);
  gr::live_off(5, 2, -1);
  render(5, 48000);
  CHECK(gr::voice_notes(5) == 0);
  char o[200];
  CHECK(gr::command("gr=voice:6", o, sizeof o) && gr::forced(6));
  wg::params(kFm, 6, P(11));
  CHECK(v[6].gr && !v[6].fm);
  CHECK(poly::seq_on(6, 62, 100) && gr::voice_notes(6) == 1);
  poly::seq_off(6, 62);
  render(6, 48000);
  CHECK(gr::voice_notes(6) == 0);
  CHECK(gr::command("gr=voice:6:off", o, sizeof o));
  wg::params(kFm, 6, P(11));
  CHECK(!v[6].gr);
  CHECK(gr::command("gr", o, sizeof o) && strstr(o, "sources") && !gr::command("grx", o, sizeof o));
  gr::reset();
}

// Clicks: a step in the signal (or in its slope) shows in the second difference |x[i] - 2 x[i-1] + x[i-2]|, which for
// a band-limited signal of peak A up to f Hz is at most A (2 pi f / 48000)^2 (windows and envelopes add a little). A
// sample over kClick times that bound is a click (counted once in 1 ms). Every case plays sines (441 / 220 Hz) so a
// cut grain, a window's edge, a steal or a seam in the ring stands out.
static constexpr double kClick = 4.0;
struct ClickRes { int n; double worst; };
static ClickRes clicks(const std::vector<float>& x, double fmax) {
  double pk = 0;
  for (float y : x) pk = std::max(pk, (double)std::fabs(y));
  double w = 2 * M_PI * fmax / 48000, bound = pk * w * w + 1e-9;
  ClickRes r = {0, 0};
  for (size_t i = 2; i < x.size(); ++i) {
    double q = std::fabs((double)x[i] - 2.0 * x[i - 1] + x[i - 2]) / bound;
    r.worst = std::max(r.worst, q);
    if (q > kClick) ++r.n, i += 48;
  }
  return r;
}
static void click_case(const char* name, const std::vector<float>& y, double fmax, int* bad) {
  ClickRes r = clicks(y, fmax);
  printf("test_granular: clicks %-14s %3d (worst %5.2f x the smooth bound, rms %.3f)\n", name, r.n, r.worst, wg::rms(y));
  CHECK(wg::rms(y) > 0.005);   // it sounds
  if (r.n) ++*bad;
}
static void test_clicks() {
  const int vi = 1, sine = entry("sine441"), wrap = entry("wrap220");
  int bad = 0;
  auto fresh = [&] { gr::silence(vi); render(vi, 960); };
  // both card samples resident (two slots)
  wg::trig(kG, vi, 60, 100, P((float)wrap));
  idle();
  wg::trig(kG, vi, 60, 100, P((float)sine));
  idle();
  // a. a sparse cloud of short PERC grains (10 ms, 20 a second), held
  fresh();
  wg::trig(kG, vi, 60, 120, P((float)sine, 40, 0, size_v(0.01), dens_v(20), 0, 0, 112 /*PERC*/, 20, 40, 127));
  click_case("perc-sparse", render(vi, 48000), 441 * 1.1, &bad);
  // b. a dense HANN cloud (80 ms, 100 a second), scanning, reversed half the time
  fresh();
  wg::trig(kG, vi, 60, 120, P((float)sine, 20, 10, size_v(0.08), dens_v(100), 0, 64, 0, 20, 40));
  click_case("hann-dense", render(vi, 48000), 441 * 1.1, &bad);
  // c. TUKEY grains across the end of a sample that ends mid-cycle (POS at the end, frozen there)
  fresh();
  wg::trig(kG, vi, 60, 120, P((float)wrap, 127, 0, size_v(0.1), dens_v(30), 0, 0, 80 /*TUKEY*/, 20, 40, 127));
  click_case("card-end", render(vi, 48000), 220 * 1.1, &bad);
  // d. steals: seven held keys on a voice of six notes (the seventh steals the oldest while it sounds)
  fresh();
  wg::params(kG, vi, P((float)sine, 30, 0, size_v(0.06), dens_v(60), 0, 0, 0, 20, 40, 127));
  std::vector<float> y;
  for (int k = 0; k < 9; ++k) {
    gr::live_on(vi, 48 + k, 100, 3, 48 + k);
    std::vector<float> t = render(vi, 4800);
    y.insert(y.end(), t.begin(), t.end());
  }
  click_case("steal", y, 441 * 1.1, &bad);
  gr::live_off(vi, 3, -1);
  // e. silence (CC 120, a machine change) in the middle of a cloud
  fresh();
  wg::trig(kG, vi, 60, 120, P((float)sine, 30, 0, size_v(0.06), dens_v(60), 0, 0, 0, 20, 40, 127));
  y = render(vi, 24000);
  gr::silence(vi);
  std::vector<float> t = render(vi, 4800);
  y.insert(y.end(), t.begin(), t.end());
  click_case("silence", y, 441 * 1.1, &bad);
  // f. SRC switched under a held note (sine441 -> wrap220)
  fresh();
  wg::trig(kG, vi, 60, 120, P((float)sine, 30, 0, size_v(0.06), dens_v(60), 0, 0, 0, 20, 40, 127));
  y = render(vi, 24000);
  wg::params(kG, vi, P((float)wrap, 30, 0, size_v(0.06), dens_v(60), 0, 0, 0, 20, 40, 127));
  t = render(vi, 24000);
  y.insert(y.end(), t.begin(), t.end());
  click_case("src-switch", y, 441 * 1.1, &bad);
  // g. live (MAIN: a 220 Hz sine): frozen, then thawed while the input runs on (the ring's seam), then a source switch
  fresh();
  wg::trig(kG, vi, 60, 120, P(2 /*MAIN: A002*/, 0, 0, size_v(0.06), dens_v(60), 0, 0, 0, 20, 40));
  render(vi, 24000, true);
  wg::params(kG, vi, P(2, 0, 0, size_v(0.06), dens_v(60), 0, 0, 0, 20, 40, 127));
  y = render(vi, 12000, true);
  g_phase += 333;   // the input moves on while frozen (any phase)
  wg::params(kG, vi, P(2, 0, 0, size_v(0.06), dens_v(60), 0, 0, 0, 20, 40, 0));
  t = render(vi, 24000, true);
  y.insert(y.end(), t.begin(), t.end());
  click_case("live-freeze", y, 220 * 1.1, &bad);
  // h. at the grain cap (8): grains left out, none cut
  gr::set_cap(8);
  fresh();
  wg::trig(kG, vi, 60, 120, P((float)sine, 30, 20, size_v(0.2), dens_v(200), 0, 0, 0, 20, 40, 127));
  click_case("cap", render(vi, 48000), 441 * 1.1, &bad);
  gr::set_cap(gr::kDefaultCap);
  // i. a released note (REL short), the next trig's attack (ATK 0)
  fresh();
  wg::trig(kG, vi, 60, 120, P((float)sine, 30, 0, size_v(0.06), dens_v(60), 0, 0, 0, 0, 0, 127));
  y = render(vi, 12000);
  wg::off(vi);
  t = render(vi, 12000);
  y.insert(y.end(), t.begin(), t.end());
  click_case("env-short", y, 441 * 1.1, &bad);
  printf("test_granular: clicks: %d of 9 cases click\n", bad);
  CHECK(bad == 0);
  gr::reset();
}

// ANIM: the head moves on the clock (here its own, 120 BPM) by the depth, none at 0; HOLD slows the scan; the texts;
// the display's view (the grains where they play)
static void test_anim() {
  const int vi = 2, sine = entry("sine441");
  char b[16];
  CHECK(!strcmp(gr::anim_text(0, b, sizeof b), "OFF") && !strcmp(gr::anim_text(1, b, sizeof b), "8BAR") &&
        !strcmp(gr::anim_text(64, b, sizeof b), "1/2") && !strcmp(gr::anim_text(127, b, sizeof b), "1/16"));
  CHECK(!strcmp(gr::hold_text(0, b, sizeof b), "0") && !strcmp(gr::hold_text(119, b, sizeof b), "99") &&
        !strcmp(gr::hold_text(120, b, sizeof b), "FRZ"));
  double range[2];
  for (int a = 0; a < 2; ++a) {
    gr::silence(vi);
    render(vi, 960);
    // frozen at POS 64 (no scan): ANIM 64 = a 2-beat cycle (1 s at 120 BPM), depth 0.5
    wg::trig(kG, vi, 60, 110, P((float)sine, 64, 0, size_v(0.05), dens_v(60), 0, 0, 0, 20, 40, 127, a ? 64.0f : 0.0f));
    idle();
    double lo = 1, hi = 0;
    std::vector<float> y;
    for (int t = 0; t < 40; ++t) {   // 2 s
      std::vector<float> o = render(vi, 2400);
      y.insert(y.end(), o.begin(), o.end());
      gr::Viz v;
      CHECK(gr::viz(vi, &v) && !v.live && v.entry == sine);
      lo = std::min(lo, (double)v.head), hi = std::max(hi, (double)v.head);
      if (t == 20) {
        CHECK(v.n > 0 && std::abs(v.n - gr::voice_grains(vi)) <= 1);
        for (int i = 0; i < v.n; ++i) CHECK(v.pos[i] >= 0 && v.pos[i] <= 1 && v.amp[i] >= 0 && v.amp[i] <= 1);
      }
    }
    range[a] = hi - lo;
    int bad = 0;
    click_case(a ? "anim-64" : "anim-off", y, 441 * 1.2, &bad);
    CHECK(bad == 0);
  }
  printf("test_granular: ANIM: the head's range %.3f off, %.3f at 64\n", range[0], range[1]);
  CHECK(range[0] < 1e-6 && range[1] > 0.15 && range[1] < 0.6);
  float mn[64], mx[64];
  CHECK(gr::viz_wave(vi, 64, mn, mx) == 64 && mx[10] > 0.4f && mn[10] < -0.4f);   // the sine's outline
  // HOLD: the playhead's speed (the scan over 1 s: 1x at 0, about half at 60, none at FRZ)
  double moved[3];
  const float hv[3] = {0, 60, 127};
  for (int h = 0; h < 3; ++h) {
    gr::silence(vi);
    render(vi, 960);
    wg::trig(kG, vi, 60, 110, P((float)sine, 0, 0, size_v(0.05), dens_v(60), 0, 0, 0, 20, 40, hv[h]));
    render(vi, 4800);
    gr::Viz v0, v1;
    gr::viz(vi, &v0);
    render(vi, 48000);
    gr::viz(vi, &v1);
    moved[h] = (v1.head - v0.head) * 3.0;   // the sample is 3 s: seconds of it a second
  }
  printf("test_granular: HOLD: the playhead moves %.2f / %.2f / %.2f s a second (HOLD 0, 60, FRZ)\n", moved[0], moved[1], moved[2]);
  CHECK(std::fabs(moved[0] - 1.0) < 0.05 && std::fabs(moved[1] - 0.5) < 0.05 && std::fabs(moved[2]) < 1e-6);
  gr::reset();
}

// golden renders: features within 10 % of what they were when this machine was written; WG_RENDER_DIR writes WAVs
static void test_golden() {
  struct Gd { const char* name; int key; std::vector<float> p; };
  Gd gs[] = {
      {"pad_cloud", 60, P(11, 20, 40, size_v(0.15), dens_v(40), 8, 10, 0, 40, 70, 0, 0, 7 /*MIN7*/)},
      {"bells_frozen", 67, P(12, 30, 20, size_v(0.06), dens_v(60), 0, 30, 112 /*PERC*/, 0, 60, 127)},
      {"bursts_scan", 55, P((float)entry("bursts"), 0, 10, size_v(0.03), dens_v(90), 24, 50, 48 /*TRI*/, 10, 50)},
  };
  // (re-measured in integ-0.3 with the click-free edges, the sample-end clamp and the smoothed envelope: was
  // 0.2386 / 0.0609 / 0.0336 rms, 710 / 480 / 12079 crossings)
  const double kRms[] = {0.2471, 0.0610, 0.0373}, kZc[] = {671, 475, 11909};
  const char* dir = getenv("WG_RENDER_DIR");
  for (size_t i = 0; i < sizeof gs / sizeof gs[0]; ++i) {
    gr::silence(7);
    render(7, 480);
    wg::trig(kG, 7, gs[i].key, 110, gs[i].p);
    std::vector<float> y = render(7, 72000);
    wg::off(7);
    std::vector<float> t = render(7, 48000);
    y.insert(y.end(), t.begin(), t.end());
    double r = wg::rms(y), z = wg::zc_freq(y);
    printf("test_granular: golden %s: rms %.4f, crossings %.0f a second (golden %.4f, %.0f)\n", gs[i].name, r, z, kRms[i], kZc[i]);
    CHECK(std::fabs(r / kRms[i] - 1) < 0.1 && std::fabs(z / kZc[i] - 1) < 0.1);
    if (dir) wg::write_wav16(std::string(dir) + "/gr_" + gs[i].name + ".wav", y);
  }
  gr::reset();
}

// the CPU: a cloud of N grains on this Mac, per grain
static void test_cpu() {
  for (int n : {16, 64}) {
    gr::set_cap(n);
    gr::silence(7);
    render(7, 480);
    // grains of 1 s at 200 a second: the cap always full
    wg::trig(kG, 7, 60, 100, P(11, 30, 30, 127, 127, 5));
    render(7, 48000);
    CHECK(gr::voice_grains(7) >= n - 2 && gr::voice_grains(7) <= n);
    std::vector<float> o(kBlock);
    const int blocks = 20000;
    double t0 = wg::now_s();
    for (int b = 0; b < blocks; ++b) gr::render_add(7, o.data(), kBlock);
    double ns = (wg::now_s() - t0) / blocks * 1e9;
    printf("test_granular: CPU %d grains: %.0f ns a 12-frame block = %.2f %% of a core, %.1f ns a grain-sample (Mac)\n", n,
           ns, ns / 250000 * 100, ns / n / kBlock);
  }
  gr::set_cap(gr::kDefaultCap);
  gr::reset();
}

// Rows of other banks and folders (the pool takes any file), the notes (a file longer than a source holds), a rescan
// keeping what is in RAM, OFF / empty / missing rows silent, a choice writing SRC (48), a preview.
static std::vector<std::vector<uint8_t>> g_sent;
static void test_sources_and_picks() {
  uint64_t us = 50000000;
  auto tick = [&](uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      us += 10000;
      host_fake_time(true, us);
      idle();
    }
  };
  host_fake_time(true, us);
  CHECK(system(("mkdir -p '" + g_card + "/Samples/Sidecar' '" + g_card + "/Samples/Loops'").c_str()) == 0);
  auto tone = [](double hz) {
    return [hz](int i, int) { return 0.5 * std::sin(2 * M_PI * hz * i / 48000); };
  };
  wg::write_file(g_card + "/Samples/Sidecar/TAKE1.wav", wg::wav_bytes(48000, 1, 16, 48000, 1, "", tone(300)));
  wg::write_file(g_card + "/Samples/Loops/beat.wav", wg::wav_bytes(24000, 1, 16, 48000, 1, "", tone(200)));
  // 90 s at 48 kHz: more than a source holds (kSlotFrames, 87.4 s)
  wg::write_file(g_card + "/Samples/Sidecar/LONG.wav", wg::wav_bytes(90 * 48000, 1, 16, 48000, 1, "", tone(100)));
  CHECK(pu::put(pools::kSamples, "B001", "/Samples/Loops/beat.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "B002", "/Samples/Sidecar/TAKE1.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "C010", "/Samples/Sidecar/LONG.wav") == pools::kOk);
  const int beat = 128 + 1, take = 128 + 2, lng = 2 * 128 + 10;
  CHECK(!strcmp(gr::entry_name(beat), "beat") && !strcmp(gr::entry_group(beat), "BANK B"));
  CHECK(!strcmp(gr::entry_path(take), "/Samples/Sidecar/TAKE1.wav") && !gr::entry_path(3)[0]);
  CHECK(gr::entry_state(3) == gr::kStateRam && gr::entry_state(beat) == gr::kStateCard);
  CHECK(gr::entry_lib(128) == srclib::kLibOff && gr::entry_lib(128 + 3) == srclib::kLibHole);
  char b[32];
  // the long one: its first 87 s, a note says so
  wg::params(kG, 3, P(10 + 2 * 16 / 128.0f));   // C010
  CHECK(gr::entry_state(lng) == gr::kStateLoading && !strcmp(gr::entry_note(lng, b, sizeof b), "LOADING"));
  idle();
  CHECK(gr::entry_state(lng) == gr::kStateCut);
  printf("test_granular: LONG.wav: %s\n", gr::entry_note(lng, b, sizeof b));
  CHECK(!strcmp(gr::entry_note(lng, b, sizeof b), "TOO LONG: FIRST 87 S"));
  CHECK(!strcmp(gr::entry_note(13, b, sizeof b), "NOT A WAV IT READS") || gr::entry_state(13) != gr::kStateFailed);
  // a rescan keeps a sample in RAM (same path, same size): no load again
  wg::params(kG, 2, P(2 + 16 / 128.0f));   // B002
  idle();
  CHECK(gr::voice_entry(2) == take);
  uint32_t loads = gr::stats().loads;
  gr::rescan();
  idle();
  CHECK(gr::stats().loads == loads && gr::voice_entry(2) == take && gr::entry_state(take) == gr::kStateRam);
  // an OFF / empty row: a trig sounds nothing
  gr::silence(2);
  render(2, 4800);
  wg::trig(kG, 2, 60, 100, P(16 / 128.0f));   // B000 OFF
  CHECK(gr::voice_notes(2) == 0);
  wg::trig(kG, 2, 60, 100, P(3 + 16 / 128.0f));   // B003 empty
  CHECK(gr::voice_notes(2) == 0);
  // a choice (the touchscreen's): SRC written on the Rytm (48 <track> 0 <index> <bank << 4>), the voice on it at once
  g_sent.clear();
  CHECK(gr::pick(2, beat) && !gr::voice_picked(2) && gr::src_entry(2, beat) == beat);
  idle();
  CHECK(gr::voice_entry(2) == beat);
  // a preview: a short note of the entry on the voice once it is in RAM, released after ~0.9 s
  gr::silence(2);
  render(2, 4800);
  CHECK(gr::voice_notes(2) == 0);
  gr::preview(2, beat);
  tick(20);
  CHECK(gr::voice_notes(2) == 1 && gr::voice_entry(2) == beat);
  render(2, 4800);
  tick(1200);
  render(2, 48000);
  CHECK(gr::voice_notes(2) == 0);
  char out[160];
  CHECK(gr::command("gr=pick:2:B002", out, sizeof out) && strstr(out, "TAKE1"));
  CHECK(gr::command("gr=pick:2:9999", out, sizeof out) && strstr(out, "no entry 9999"));
  // the file gone from the row (unloaded): the row is silent, its sample leaves RAM when no voice holds it
  CHECK(pu::put(pools::kSamples, "B001", "") == pools::kOk);
  CHECK(gr::entry_lib(beat) == srclib::kLibHole);
  gr::reset();
}

// Stereo (docs/STEREO.md): on a voice routed OUT PRE / POST (render_add2) a stereo source's L and R stay apart (a 440 Hz
// L, a 660 Hz R), each grain panned at random within SPRY x WID (a mono source spreads: L != R; WID 0: L = R); the mono
// render (TRK) of the stereo source is its sum; a TRK voice's grains draw from the same random stream as before.
static void test_stereo() {
  wg::write_file(g_card + "/GRANULAR/lr.wav", wg::wav_bytes(96000, 2, 16, 48000, 1, "", [](int i, int c) {
                   return 0.4 * std::sin(2 * M_PI * (c ? 660.0 : 440.0) * i / 48000);
                 }));
  CHECK(pu::put(pools::kSamples, "A017", "/GRANULAR/lr.wav") == pools::kOk);
  pu::steps();
  idle();
  int e = entry("lr");
  CHECK(e >= 0);
  auto both = [](int vi, int frames, std::vector<float>* l, std::vector<float>* r) {
    l->assign(frames, 0.0f), r->assign(frames, 0.0f);
    for (int s = 0; s < frames; s += kBlock) {
      idle();
      gr::render_add2(vi, l->data() + s, r->data() + s, frames - s < kBlock ? frames - s : kBlock);
    }
  };
  // 100 ms grains, 20 a second (a whole number of both pitches' cycles apart: the grains add in phase), no spray, FRZ:
  // L 440 Hz, R 660 Hz
  wg::trig(kG, 2, 60, 127, P((float)e, 32, 0, size_v(0.1), dens_v(20), 0, 0, 0, 0, 40, 127));
  idle();
  std::vector<float> l, r;
  auto c0 = std::chrono::steady_clock::now();
  both(2, 48000, &l, &r);
  double t_st = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
  auto mag = [](const std::vector<float>& x, double hz) {   // |DFT| at hz over 0.2..1 s
    double c = 0, q = 0;
    for (int i = 9600; i < 48000; ++i) c += x[i] * std::cos(2 * M_PI * hz * i / 48000), q += x[i] * std::sin(2 * M_PI * hz * i / 48000);
    return std::sqrt(c * c + q * q) / 38400;
  };
  double l4 = mag(l, 440), l6 = mag(l, 660), r4 = mag(r, 440), r6 = mag(r, 660);
  printf("test_granular: stereo source on POST: L 440 Hz %.3f / 660 Hz %.3f, R 440 Hz %.3f / 660 Hz %.3f\n", l4, l6, r4, r6);
  CHECK(l4 > 0.05 && r6 > 0.05 && l6 < l4 / 30 && r4 < r6 / 30);
  gr::silence(2);
  // the mono render of the same source: the sum (both pitches), as the summed load gave it
  wg::trig(kG, 2, 60, 127, P((float)e, 32, 0, size_v(0.1), dens_v(20), 0, 0, 0, 0, 40, 127));
  idle();
  c0 = std::chrono::steady_clock::now();
  std::vector<float> m = render(2, 48000);
  double t_mono = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
  printf("test_granular: a stereo source, 1 s of the cloud (20 grains/s, 100 ms): stereo %.2f ms, mono %.2f ms (Mac)\n",
         t_st * 1000, t_mono * 1000);
  CHECK(mag(m, 440) > 0.02 && mag(m, 660) > 0.02);   // both channels in the mono sum
  gr::silence(2);
  // a mono source with SPRY: the grains spread (L != R); WID 0: no spread (L = R)
  int w = entry("wrap220");
  for (int wid : {64, 0}) {
    out::control.wid[2] = (uint8_t)wid;
    wg::trig(kG, 2, 60, 127, P((float)w, 64, 90, size_v(0.05), dens_v(60), 0, 0, 0, 0, 40, 127));
    idle();
    both(2, 24000, &l, &r);
    double d = 0;
    for (int i = 4800; i < 24000; ++i) d += std::fabs(l[i] - r[i]);
    printf("test_granular: mono source, SPRY 90, WID %d: mean |L - R| %.4f\n", wid, d / 19200);
    CHECK(wid ? d / 19200 > 0.01 : d == 0);
    gr::silence(2);
  }
  out::control.wid[2] = 64;
  gr::reset();
}

int main() {
  setup_card();
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, 1000000);
  voices_init();
  card_ok = true;
  srclib::begin(true);
  pools::begin(true);
  gr::begin(true);
  pu::steps();
  pool_layout();
  idle();
  test_list_and_load();
  test_density();
  test_live_ring();
  test_reverse_cap();
  test_live_notes();
  test_clicks();
  test_anim();
  test_golden();
  test_cpu();
  test_sources_and_picks();
  test_stereo();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_granular: PASS\n");
  return 0;
}
