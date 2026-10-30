// core/tests/test_insert.cpp - the INSERT effects (core/fx/insert.h, docs/INSERT-FX.md): each effect's response,
// stability under noise at every extreme, the CPU, click-free TYPE switching and param jumps, the control side (knob /
// lock / LFO -> play), the console and the card file, the voice hook in render_voices, the buses (a voice's send through
// send FX 1 and its bus chain, bus 3's dry bus), and that nothing changes while every chain is off.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>   // mkdtemp

#include "check.h"
#include "timing.h"
#include "rui_png.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "fmt/fmt.h"
#include "lfo.h"
#include "msamp/cardq.h"
#include "store/store.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using ins::Engine;

static const float kSr = 48000.0f;

// one mono chain (voice 0) over x with slot A = type t, params p
static std::vector<float> run(Engine& e, int t, std::vector<int> p, const std::vector<float>& x, int slot = 0, int chain = 0) {
  ins::control.type[chain][slot] = (uint8_t)t;
  for (int k = 0; k < 4; ++k) ins::control.play[chain][slot][k] = (uint8_t)p[k];
  std::vector<float> y = x;
  for (size_t i = 0; i < y.size(); i += 12) e.ProcessMono(chain, &y[i], (int)std::min<size_t>(12, y.size() - i), ins::control);
  return y;
}
static std::vector<float> sine(float hz, float amp, int n) {
  std::vector<float> x(n);
  for (int i = 0; i < n; ++i) x[i] = amp * std::sin(2 * M_PI * hz * i / kSr);
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
static float db(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }
// a fresh engine, its lines on the heap
static std::vector<float> g_mem((size_t)ins::kLines * ins::kLine);
static Engine fresh() {
  Engine e(kSr);
  std::fill(g_mem.begin(), g_mem.end(), 0.0f);
  e.Place((float(*)[ins::kLine])g_mem.data());
  e.Reset();
  memset((void*)&ins::control, 0, sizeof ins::control);
  return e;
}
// the gain of a sine through the type (after 0.3 s settling)
static float gain(int t, std::vector<int> p, float hz) {
  Engine e = fresh();
  auto x = sine(hz, 0.25f, 24000);
  auto y = run(e, t, p, x);
  return rms(y, 14400) / rms(x, 14400);
}

// ---- the RAM store (2026-10-26, phase 1): the card core's turn, a card file, the project's insert files into the store
static void card_core_turn() {
  host_card_core(0);
  while (cardq::service()) {
  }
  host_card_core(3);
}
static std::string rd_file(const std::string& p) {
  std::string s;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void wr_file(const std::string& p, const char* text) {
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}
// a project load (0x10, the boot): the store filled from the idle pass, then the inserts loaded from it (Task 4)
static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
static bool fill_store() {
  store::clear();
  ins::want_files();
  store::fill_begin();
  for (int i = 0; i < 100000; ++i)
    if (store::fill_step()) return store::ready();
  return false;
}
// a kit change reads the store, never the card: kit 6's own file, kit 7's (none) the project's, kit 6's again - and the
// control core is charged no card time for it
static void kit_change_reads_ram_only(const std::string& card) {
  wr_file(card + "/.rytm/ins/STO/k006.txt", "# Sidecar inserts\nV2 A EQ 64 64 100 64\nSEND V2 0 0 55\n");
  wr_file(card + "/.rytm/ins/STO.txt", "# Sidecar inserts\nV4 B RING 10 20 30 40\n");
  uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'T', 'O', 0xF7};
  engine_sysex(m, (int)sizeof m);   // project STO: its files into the store, then its inserts from there
  loaded();
  CHECK(store::ready());
  host_card_model(true);
  uint64_t ctl = host_card_us(3);
  ins::kit_changed(5);   // kit 6: its own file
  CHECK(ins::kit() == 5 && ins::control.type[1][0] == ins::kEq && ins::param(1, 0, 2) == 100 &&
        ins::control.send[1][2] == 55 && !ins::control.type[3][1]);
  ins::kit_changed(6);   // kit 7: none of its own, the project's
  CHECK(ins::kit() == 6 && ins::control.type[3][1] == ins::kRing && !ins::control.type[1][0] && !ins::control.send[1][2]);
  ins::kit_changed(5);
  CHECK(ins::control.type[1][0] == ins::kEq && ins::control.send[1][2] == 55 && !ins::control.type[3][1]);
  CHECK(host_card_us(3) == ctl);   // RAM only: no open, no exists, no recover on the control core
  host_card_model(false);
  printf("test_insert: a kit change reads the RAM store only (no card time on the control core)\n");
}
// the kit's edit survives a kit change away and back before its card write lands (the store has its text at once; the
// write is posted, or queued behind one in flight)
static void edit_then_switch_back(const std::string& card) {
  const std::string f6 = card + "/.rytm/ins/STO/k006.txt";
  cardq::reset();
  cardq::set_worker(true);   // a card core: nothing reaches the card until its turn
  CHECK(ins::kit() == 5);
  ins::set_param(1, 0, 2, 33);
  ins::kit_changed(6);
  CHECK(rd_file(f6).find("EQ 64 64 33") == std::string::npos);   // not on the card yet
  ins::kit_changed(5);
  CHECK(ins::control.type[1][0] == ins::kEq && ins::param(1, 0, 2) == 33);   // from the store
  card_core_turn();
  for (int i = 0; i < 8; ++i) idle_save(), card_core_turn();
  CHECK(rd_file(f6).find("EQ 64 64 33") != std::string::npos && !fmt::post_busy());

  char* t = fmt::post_begin();   // the post slot busy (another settings file in flight) when the kit changes
  CHECK(t);
  memcpy(t, "x\n", 2);
  CHECK(fmt::post_end("/.rytm/other.txt", 2) && fmt::post_busy());
  ins::set_param(1, 0, 2, 44);
  ins::kit_changed(6);
  CHECK(!ins::settings_dirty(nullptr) && fmt::post_busy());   // kit 6's write queued behind it, not lost
  ins::kit_changed(5);
  CHECK(ins::param(1, 0, 2) == 44);   // from the store, before any card write
  CHECK(rd_file(f6).find("EQ 64 64 44") == std::string::npos);
  for (int i = 0; i < 16; ++i) card_core_turn(), idle_save();
  card_core_turn();
  CHECK(rd_file(f6).find("EQ 64 64 44") != std::string::npos && !fmt::post_busy());
  cardq::set_worker(false);
  cardq::reset();
  printf("test_insert: an edit survives a kit change away and back before its card write lands\n");
}

// a kit without its own file whose default (the project's file) cannot be read: all off, and the kit is NOT locked (its
// own file overwrites nothing, as the card path); a kit's own unreadable file still locks it
static void unreadable_default_does_not_lock_the_kit(const std::string& card, uint64_t& us) {
  const std::string pf = card + "/.rytm/ins/STO.txt", f10 = card + "/.rytm/ins/STO/k010.txt";
  const std::string f6 = card + "/.rytm/ins/STO/k006.txt";
  const std::string p0 = rd_file(pf), k6 = rd_file(f6);
  host_fail_open("/.rytm/ins/STO.txt", true);   // (one path at a time)
  CHECK(fill_store());
  const uint8_t* d;
  uint32_t n;
  CHECK(store::get("/.rytm/ins/STO.txt", &d, &n) == store::kLocked);
  ins::kit_changed(9);   // kit 10: none of its own, the project's cannot be read
  CHECK(ins::kit() == 9 && !ins::any_on());
  ins::set_type(5, 0, ins::kRing);
  host_fake_time(true, us += 1100000);
  for (int i = 0; i < 64; ++i) idle_save();
  CHECK(rd_file(f10).find("V6 A RING") != std::string::npos && rd_file(pf) == p0);   // its own file, not the default
  host_fail_open("/.rytm/ins/STO.txt", false);
  host_fail_open("/.rytm/ins/STO/k006.txt", true);
  CHECK(fill_store());
  CHECK(store::get("/.rytm/ins/STO/k006.txt", &d, &n) == store::kLocked);
  ins::kit_changed(5);   // kit 6: its own file there but unreadable - locked, never written over
  ins::set_type(6, 1, ins::kRing);
  host_fake_time(true, us += 1100000);
  for (int i = 0; i < 64; ++i) idle_save();
  CHECK(rd_file(f6) == k6);
  host_fail_open("/.rytm/ins/STO/k006.txt", false);
  project_fill_begin();   // the project's store as the card has it again
  loaded();
  printf("test_insert: an unreadable default does not lock a kit without its own file; an unreadable own file does\n");
}

int main() {
  // ---- FILTER: LP at ~1 kHz (CUT 72: 20 x 2^(10 x 72/127) = 1.02 kHz), Q 0.5 .. resonant; HP; BP
  {
    float lo = gain(ins::kFilter, {72, 0, 0, 0}, 100), hi = gain(ins::kFilter, {72, 0, 0, 0}, 8000);
    printf("FILTER LP 1k: 100 Hz %.2f dB, 8 kHz %.1f dB\n", db(lo), db(hi));
    CHECK(db(lo) > -1.0f && db(hi) < -30.0f);
    float hlo = gain(ins::kFilter, {72, 0, 127, 0}, 100), hhi = gain(ins::kFilter, {72, 0, 127, 0}, 8000);
    printf("FILTER HP 1k: 100 Hz %.1f dB, 8 kHz %.2f dB\n", db(hlo), db(hhi));
    CHECK(db(hlo) < -30.0f && db(hhi) > -1.0f);
    float bc = gain(ins::kFilter, {72, 60, 64, 0}, 1020), bl = gain(ins::kFilter, {72, 60, 64, 0}, 100);
    printf("FILTER BP 1k: at 1 kHz %.2f dB, 100 Hz %.1f dB\n", db(bc), db(bl));
    CHECK(std::fabs(db(bc)) < 1.5f && db(bl) < -12.0f);
    float rp = gain(ins::kFilter, {72, 127, 0, 0}, 1020);   // the resonance's peak at the cutoff
    printf("FILTER LP RES 127 at the cutoff: %+.1f dB\n", db(rp));
    CHECK(db(rp) > 20.0f);
  }
  // ---- CRUSH: 2 bits -> at most a handful of levels; RATE: held samples
  {
    Engine e = fresh();
    auto x = sine(220, 0.5f, 4800);
    auto y = run(e, ins::kCrush, {0, 127, 0, 127}, x);
    std::vector<float> lv;
    for (size_t i = 960; i < y.size(); ++i) {
      bool seen = false;
      for (float l : lv) seen |= std::fabs(l - y[i]) < 1e-5f;
      if (!seen) lv.push_back(y[i]);
    }
    printf("CRUSH 2 bits: %zu levels\n", lv.size());
    CHECK(lv.size() <= 5);
    e = fresh();
    y = run(e, ins::kCrush, {127, 0, 0, 127}, x);   // hold 64
    int runs = 0;
    for (size_t i = 1001; i < 3000; ++i) runs += y[i] != y[i - 1];
    printf("CRUSH RATE 127: %d changes in 2000 samples (hold 64: ~31)\n", runs);
    CHECK(runs >= 25 && runs <= 40);
  }
  // ---- DRIVE: bounded, harmonics, the level about kept for a -12 dBFS input
  {
    Engine e = fresh();
    auto x = sine(200, 0.25f, 24000);
    auto y = run(e, ins::kDrive, {127, 0, 127, 127}, x);   // max drive, tone open-ish? (TONE 127 = 1 kHz)
    printf("DRIVE max: peak %.2f, rms %.1f dB re the input\n", peak(y, 2400), db(rms(y, 2400) / rms(x, 2400)));
    CHECK(peak(y) < 1.2f && std::fabs(db(rms(y, 2400) / rms(x, 2400))) < 6.0f);
    e = fresh();
    y = run(e, ins::kDrive, {80, 0, 0, 127}, x);
    // the 3rd harmonic's share (a soft clip is odd)
    double c3 = 0, s3 = 0, c1 = 0, s1 = 0;
    for (int i = 4800; i < 24000; ++i) {
      double ph = 2 * M_PI * 200 * i / kSr;
      c1 += y[i] * std::cos(ph), s1 += y[i] * std::sin(ph), c3 += y[i] * std::cos(3 * ph), s3 += y[i] * std::sin(3 * ph);
    }
    double h3 = std::sqrt(c3 * c3 + s3 * s3) / std::sqrt(c1 * c1 + s1 * s1);
    printf("DRIVE 80: 3rd harmonic %.1f dB\n", 20 * std::log10(h3));
    CHECK(h3 > 0.03);
  }
  // ---- EQ: all 64 = transparent; each band +-15 dB where it acts
  {
    Engine e = fresh();
    auto x = sine(440, 0.3f, 9600);
    auto y = run(e, ins::kEq, {64, 64, 64, 64}, x);
    float md = 0;
    for (size_t i = 0; i < x.size(); ++i) md = std::max(md, std::fabs(y[i] - x[i]));
    printf("EQ flat: max diff %.2e\n", md);
    CHECK(md < 1e-4f);
    float l = gain(ins::kEq, {127, 64, 64, 64}, 40), h = gain(ins::kEq, {64, 64, 0, 64}, 15000);
    // the bell at FREQ 64 (200 x 2^(5.32 x 64/127) = 1.28 kHz)
    float m = gain(ins::kEq, {64, 127, 64, 64}, 1280), m0 = gain(ins::kEq, {64, 127, 64, 64}, 100);
    printf("EQ: LOW +15 at 40 Hz %+.1f dB, HIGH -15 at 15 kHz %+.1f dB, MID +15 at 1.28 kHz %+.1f dB (100 Hz %+.1f)\n",
           db(l), db(h), db(m), db(m0));
    CHECK(std::fabs(db(l) - 15) < 1.0f && std::fabs(db(h) + 15) < 1.5f && std::fabs(db(m) - 15) < 1.0f && db(m0) < 2.0f);
  }
  // ---- COMP: THR -20 dB, RAT 20:1: a -1 dBFS sine down by ~(19 - 19/20) dB; a -40 dB one untouched
  {
    float loud = gain(ins::kComp, {64, 127, 20, 0}, 300);
    Engine e = fresh();
    auto xq = sine(300, 0.01f, 24000);
    auto yq = run(e, ins::kComp, {64, 127, 20, 0}, xq);
    float quiet = rms(yq, 14400) / rms(xq, 14400);
    // the 0.25 sine of gain(): -12 dBFS peak; threshold -20.2 dB: ~8 dB over -> ~7.7 dB down (peak detector)
    printf("COMP -20 dB 20:1: -12 dBFS in %+.1f dB, -40 dBFS in %+.2f dB\n", db(loud), db(quiet));
    CHECK(db(loud) < -5.0f && db(loud) > -10.0f && std::fabs(db(quiet)) < 0.3f);
    float mk = gain(ins::kComp, {0, 0, 20, 127}, 300);   // no compression, +24 dB makeup
    CHECK(std::fabs(db(mk) - 24) < 0.5f);
  }
  // ---- DELAY: an impulse comes back after TIME; FDBK repeats it, decaying
  {
    Engine e = fresh();
    std::vector<float> x(9600, 0.0f);
    x[1000] = 1.0f;   // after the 5 ms fade-in
    int tv = 100;   // 80^(100/127) = 31.4 ms = 1508 samples (+1)
    auto y = run(e, ins::kDelay, {tv, 64, 0, 127}, x);
    int at = -1;
    for (int i = 1100; i < 9600; ++i)
      if (std::fabs(y[i]) > 0.3f) { at = i - 1000; break; }
    float want = 0.001f * std::pow(80.0f, tv / 127.0f) * kSr + 1;
    int at2 = -1;
    for (int i = 1000 + at + 150; i < 9600; ++i)
      if (std::fabs(y[i]) > 0.1f) { at2 = i - 1000; break; }
    printf("DELAY TIME 100: echo at %d samples (want %.0f), the 2nd at %d (%.2f)\n", at, want, at2, at2 > 0 ? y[at2 + 1000] : 0);
    CHECK(std::fabs(at - want) <= 2 && std::abs(at2 - 2 * at) <= 3 && std::fabs(y[at2 + 1000]) < std::fabs(y[at + 1000]));
  }
  // ---- TREMOLO: DPTH 127, the envelope down to ~0 and up to 1
  {
    Engine e = fresh();
    std::vector<float> x(48000, 0.5f);
    auto y = run(e, ins::kTrem, {64, 127, 0, 0}, x);
    float mn = 1, mx = 0;
    for (size_t i = 4800; i < y.size(); ++i) mn = std::min(mn, y[i]), mx = std::max(mx, y[i]);
    printf("TREMOLO DPTH 127: %.3f .. %.3f\n", mn, mx);
    CHECK(mn < 0.02f && mx > 0.48f);
    // a mono chain with PAN 127: L and R in antiphase sum to a constant
    e = fresh();
    y = run(e, ins::kTrem, {64, 127, 0, 127}, x);
    mn = 1, mx = 0;
    for (size_t i = 4800; i < y.size(); ++i) mn = std::min(mn, y[i]), mx = std::max(mx, y[i]);
    printf("TREMOLO PAN 127 on a mono chain: %.3f .. %.3f (summed)\n", mn, mx);
    CHECK(mx - mn < 0.02f);
  }
  // ---- RINGMOD: DC in -> the carrier out at FREQ
  {
    Engine e = fresh();
    std::vector<float> x(48000, 0.5f);
    auto y = run(e, ins::kRing, {64, 0, 0, 127}, x);   // 10 x 500^(64/127) = 229 Hz
    int zc = 0;
    for (size_t i = 1; i < y.size(); ++i) zc += (y[i - 1] < 0) != (y[i] < 0);
    printf("RINGMOD FREQ 64: %d zero crossings a second (%.0f Hz)\n", zc, zc / 2.0);
    CHECK(std::fabs(zc / 2.0 - 229) < 4);
  }
  // ---- stability: white noise at full scale through every type at every corner (0 / 127 each param) and random
  // params, 2 s each: finite, bounded, no fault
  {
    uint32_t r = 12345;
    auto rnd = [&] { r = r * 1664525u + 1013904223u; return (float)(r >> 8) / 8388608.0f - 1.0f; };
    std::vector<float> x(96000);
    for (float& s : x) s = rnd();
    float worst = 0;
    int runs = 0;
    for (int t = 1; t < ins::kTypes; ++t)
      for (int corner = 0; corner < 20; ++corner) {
        std::vector<int> p(4);
        for (int k = 0; k < 4; ++k) p[k] = corner < 16 ? ((corner >> k) & 1) * 127 : (int)((rnd() + 1) * 63.5f);
        Engine e = fresh();
        auto y = run(e, t, p, x);
        for (float v : y) CHECK(std::isfinite(v));
        worst = std::max(worst, peak(y));
        CHECK(e.Faults() == 0);
        ++runs;
      }
    printf("stability: %d runs of 2 s full-scale noise, peak %.2f, no fault\n", runs, worst);
    CHECK(worst < 40.0f);   // (EQ +15 dB x3 and COMP +24 dB makeup on full-scale noise: big but bounded)
  }
  // ---- click-free TYPE switching: a 220 Hz sine at 0.5 through slot A switched every 37 ms among the smooth types
  // and OFF; the largest sample step stays near the sine's own (2 pi 220 / 48000 x 0.5 = 0.0144)
  {
    Engine e = fresh();
    const int seq[] = {ins::kOff, ins::kFilter, ins::kEq, ins::kComp, ins::kTrem, ins::kDelay, ins::kOff, ins::kDrive,
                       ins::kFilter, ins::kOff};
    // params near unity gain (a click is a step against the signal's own level, not a louder signal)
    std::vector<int> par[ins::kTypes] = {{0, 0, 0, 0},   {70, 60, 0, 0},  {0, 0, 0, 0},  {20, 0, 0, 127},
                                         {64, 70, 60, 64}, {40, 60, 30, 10}, {80, 0, 30, 64}, {64, 60, 0, 0},
                                         {0, 0, 0, 0}};
    auto x = sine(220, 0.5f, 48000);
    std::vector<float> y = x;
    float worst = 0;
    int t = 0;
    for (size_t i = 0; i < y.size(); i += 12) {
      if (i % 1776 == 0) {   // 37 ms
        int ty = seq[t++ % 10];
        ins::control.type[0][0] = (uint8_t)ty;
        for (int k = 0; k < 4; ++k) ins::control.play[0][0][k] = (uint8_t)par[ty][k];
      }
      e.ProcessMono(0, &y[i], 12, ins::control);
    }
    size_t wi = 0;
    for (size_t i = 1; i < y.size(); ++i)
      if (std::fabs(y[i] - y[i - 1]) > worst) worst = std::fabs(y[i] - y[i - 1]), wi = i;
    printf("TYPE switching every 37 ms: largest step %.4f at %zu (switch %zu + %zu) (the sine's own 0.0144)\n", worst, wi,
           wi / 1776, wi % 1776);
    CHECK(worst < 0.06f);
    // a p-lock's jump: FILTER CUT 0 -> 127 -> 0 every 10 ms (no zipper, no click: the 3 ms smoothing)
    e = fresh();
    y = x;
    for (size_t i = 0; i < y.size(); i += 12) {
      ins::control.type[0][0] = ins::kFilter;
      ins::control.play[0][0][0] = (uint8_t)((i / 480) & 1 ? 127 : 20);
      ins::control.play[0][0][1] = 40;
      e.ProcessMono(0, &y[i], 12, ins::control);
    }
    worst = 0;
    for (size_t i = 2400; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - y[i - 1]));
    printf("FILTER CUT jumps 20 <-> 127 every 10 ms: largest step %.4f\n", worst);
    CHECK(worst < 0.06f);
  }
  // ---- ins1112, the POST EQ: where it is in the chain, what it does, and that it costs nothing while flat
  {
    Engine e = fresh();
    const int ch = 0;
    auto peq = [&](int a, int b, int c2, int d) {
      ins::control.peq[ch][0] = (int8_t)ins::eq_store(0, a), ins::control.peq[ch][1] = (int8_t)ins::eq_store(1, b);
      ins::control.peq[ch][2] = (int8_t)ins::eq_store(2, c2), ins::control.peq[ch][3] = (int8_t)ins::eq_store(3, d);
    };
    // half a second of a sine through chain 0, the settled RMS out (the file's `gain` helper, chain 0 not reset)
    auto through = [&](float hz) {
      std::vector<float> x = sine(hz, 0.3f, 24000), y = x;
      for (size_t i = 0; i < y.size(); i += 12) e.ProcessMono(ch, &y[i], 12, ins::control);
      return rms(y, 14400);
    };
    // flat: bit for bit the input, and the engine says there is nothing to do
    memset((void*)&ins::control, 0, sizeof ins::control);
    peq(0, 0, 64, 64);
    CHECK(!ins::peq_set(ins::control, ch) && !e.Active(ch, ins::control));
    {
      auto x = sine(100, 0.3f, 12);
      float b[12];
      memcpy(b, x.data(), sizeof b);
      e.ProcessMono(ch, b, 12, ins::control);
      CHECK(!memcmp(b, x.data(), sizeof b));   // untouched, every bit
    }
    // THE ORDERING CLAIM. Slot B is a RINGMOD at ~2 kHz on a 100 Hz sine: everything it makes is up at 1.9 / 2.1 kHz.
    // A post EQ LPF down at ~200 Hz can only silence that if it runs AFTER the slot. Were it before, the 100 Hz input
    // would pass it untouched and the ring modulator would make the same 2 kHz anyway. AMP is slot type 9, so
    // "after slot B" is "after the inserts, AMP included".
    for (int slot = 0; slot < 2; ++slot) {
      e = fresh();
      memset((void*)&ins::control, 0, sizeof ins::control);
      ins::control.type[ch][slot] = ins::kRing;
      ins::control.play[ch][slot][0] = 100;   // FREQ well above the input
      ins::control.play[ch][slot][3] = 127;   // MIX: wet only
      peq(0, 0, 64, 64);
      float open_ = through(100);
      peq(0, 110, 64, 64);   // LPF down at ~50 Hz
      float shut = through(100);
      printf("ins1112: RINGMOD in slot %c, 100 Hz in: EQ flat %.4f, EQ LPF down %.4f (%.1f dB)\n", 'A' + slot, open_,
             shut, db(shut / (open_ + 1e-12f)));
      CHECK(open_ > 0.02f);
      CHECK(shut < open_ * 0.05f);   // the slot's own 2 kHz output is gone: the EQ is after it
    }
    // ... and the other way round: a post EQ HPF cannot stop a RINGMOD making its sum and difference, because it is
    // after it. (A filter before the slot would take the 100 Hz input away and silence the slot.)
    {
      e = fresh();
      memset((void*)&ins::control, 0, sizeof ins::control);
      ins::control.type[ch][1] = ins::kRing;
      ins::control.play[ch][1][0] = 100;
      ins::control.play[ch][1][3] = 127;
      peq(0, 0, 64, 64);
      float open_ = through(100);
      peq(70, 0, 64, 64);   // an HPF at ~900 Hz: it would cut the 100 Hz input by ~38 dB if it ran first
      float still = through(100);
      printf("ins1112: the same with the EQ's HPF right up: %.4f (%.1f dB)\n", still, db(still / (open_ + 1e-12f)));
      CHECK(still > open_ * 0.5f);   // the slot's 2 kHz output sails through it
    }
    // the four stages do what they say: HPF, LPF, and the shelves at 250 Hz / 4 kHz
    {
      e = fresh();
      memset((void*)&ins::control, 0, sizeof ins::control);
      auto gain_at = [&](float hz) {
        std::vector<float> x = sine(hz, 0.3f, 24000), y = x;
        for (size_t i = 0; i < y.size(); i += 12) e.ProcessMono(ch, &y[i], 12, ins::control);
        return rms(y, 14400) / rms(x, 14400);
      };
      peq(90, 0, 64, 64);   // HPF ~5 kHz: 100 Hz gone, 10 kHz through
      float lo = gain_at(100), hi = gain_at(10000);
      CHECK(lo < 0.1f && hi > 0.8f);
      e = fresh();
      peq(0, 90, 64, 64);   // LPF: the other way round
      float lo2 = gain_at(100), hi2 = gain_at(10000);
      CHECK(lo2 > 0.8f && hi2 < 0.1f);
      e = fresh();
      peq(0, 0, 127, 64);   // the low shelf right up: +15 dB at 50 Hz, flat at 10 kHz
      float sl = gain_at(50), sh = gain_at(10000);
      printf("ins1112: low shelf up: 50 Hz %.1f dB, 10 kHz %.1f dB\n", db(sl), db(sh));
      CHECK(db(sl) > 12.0f && db(sl) < 17.0f && std::fabs(db(sh)) < 0.6f);
      e = fresh();
      peq(0, 0, 64, 1);   // the high shelf right down: -15 dB at 15 kHz, flat at 50 Hz
      float hl = gain_at(50), hh = gain_at(15000);
      printf("ins1112: high shelf down: 50 Hz %.1f dB, 15 kHz %.1f dB\n", db(hl), db(hh));
      CHECK(db(hh) < -12.0f && db(hh) > -17.0f && std::fabs(db(hl)) < 0.6f);
      e = fresh();
      peq(0, 0, 64, 64);
      CHECK(!ins::peq_set(ins::control, ch) && !e.Active(ch, ins::control));   // flat: the stage is not there at all
    }
    // stability: white noise at full scale through every corner of the four knobs, and random settings
    {
      e = fresh();
      memset((void*)&ins::control, 0, sizeof ins::control);
      uint32_t r = 1;
      auto rnd = [&] { r = r * 1664525u + 1013904223u; return (int)((r >> 16) & 127); };
      float worst = 0;
      for (int it = 0; it < 80; ++it) {
        if (it < 16) peq(it & 1 ? 127 : 0, it & 2 ? 127 : 0, it & 4 ? 127 : 0, it & 8 ? 127 : 0);
        else peq(rnd(), rnd(), rnd(), rnd());
        float b[12];
        for (int k = 0; k < 60; ++k) {
          for (int i = 0; i < 12; ++i) b[i] = (float)(rnd() - 64) / 64.0f;
          e.ProcessMono(ch, b, 12, ins::control);
          for (int i = 0; i < 12; ++i) {
            CHECK(std::isfinite(b[i]));
            worst = std::max(worst, std::fabs(b[i]));
          }
        }
      }
      printf("ins1112: noise through every corner: worst %.1f, faults %u\n", worst, e.Faults());
      CHECK(e.Faults() == 0 && worst < 40.0f);
    }
    // click-free: the HPF knob jumped between off and right up every ~10 ms (the engage / release fade)
    {
      e = fresh();
      memset((void*)&ins::control, 0, sizeof ins::control);
      auto x = sine(220, 0.5f, 48000);
      std::vector<float> y = x;
      for (size_t i = 0; i < y.size(); i += 12) {
        if (i % 480 == 0) peq((i / 480) & 1 ? 127 : 0, 0, 64, 64);   // off <-> right up every 10 ms
        e.ProcessMono(ch, &y[i], 12, ins::control);
      }
      float worst = 0;
      for (size_t i = 2400; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - y[i - 1]));
      printf("ins1112: the HPF knob 0 <-> 127 every 10 ms: largest step %.4f (the sine's own 0.0144)\n", worst);
      CHECK(worst < 0.06f);
    }
    // ... and it settles back to a true bypass, so the chain is bit for bit again
    {
      e = fresh();
      memset((void*)&ins::control, 0, sizeof ins::control);
      peq(100, 0, 64, 64);
      float b[12];
      for (int k = 0; k < 200; ++k) {
        for (int i = 0; i < 12; ++i) b[i] = 0.2f;
        e.ProcessMono(ch, b, 12, ins::control);
      }
      peq(0, 0, 64, 64);
      for (int k = 0; k < 200; ++k) {
        for (int i = 0; i < 12; ++i) b[i] = 0.2f;
        e.ProcessMono(ch, b, 12, ins::control);
      }
      CHECK(!e.Active(ch, ins::control));
      for (int i = 0; i < 12; ++i) b[i] = 0.2f;
      e.ProcessMono(ch, b, 12, ins::control);
      for (int i = 0; i < 12; ++i) CHECK(b[i] == 0.2f);
    }
    memset((void*)&ins::control, 0, sizeof ins::control);
  }
  // ---- ins1112's CPU: the post EQ on its own, and on top of the costliest pair
  {
    auto bench = [&](const char* what, int a, int b, bool eq) {
      Engine e = fresh();
      for (int v = 0; v < 8; ++v) {
        for (int s = 0; s < 2; ++s) {
          ins::control.type[v][s] = (uint8_t)(s ? b : a);
          for (int k = 0; k < 4; ++k) ins::control.play[v][s][k] = (uint8_t)(40 + 20 * k);
        }
        for (int k = 0; k < ins::kEqParams; ++k)
          ins::control.peq[v][k] = (int8_t)(eq ? ins::eq_store(k, k < 2 ? 70 : 100) : ins::eq_store(k, ins::kEqFlat(k)));
      }
      auto x = sine(330, 0.3f, 12);
      float bf[12];
      const int blocks = 20000;
      auto t0 = std::chrono::steady_clock::now();
      for (int n = 0; n < blocks; ++n)
        for (int v = 0; v < 8; ++v) {
          memcpy(bf, x.data(), sizeof bf);
          e.ProcessMono(v, bf, 12, ins::control);
        }
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
      printf("CPU (Mac) ins1112: 8 voices x %s: %.2f us a 12-frame block (%.2f %% of its 250 us)\n", what, us,
             us / 250 * 100);
      memset((void*)&ins::control, 0, sizeof ins::control);
      return us;
    };
    // (wall-clock: up to 3 tries - tests/timing.h)
    CHECK(timing_retry("ins1112 post EQ bench", [&] {
      double flat = bench("both slots OFF, the post EQ flat", ins::kOff, ins::kOff, false);
      double only = bench("both slots OFF, the post EQ on (all four stages)", ins::kOff, ins::kOff, true);
      double pair = bench("COMP + EQ, the post EQ flat", ins::kComp, ins::kEq, false);
      double both = bench("COMP + EQ + the post EQ on", ins::kComp, ins::kEq, true);
      printf("CPU (Mac) ins1112: the post EQ costs %.2f us a block over 8 voices (%.2f %% of a core); on top of the "
             "costliest pair %.2f us (%.2f %%)\n",
             only - flat, (only - flat) / 250 * 100, both - pair, (both - pair) / 250 * 100);
      return both < 50 && flat < only;   // (flat really is the cheap path)
    }));
  }
  // ---- CPU: 8 voices x 2 slots of the costliest pair, as render_voices calls it (12 frames), and a bus pair
  {
    const int pairs[][2] = {{ins::kFilter, ins::kDelay}, {ins::kComp, ins::kEq}, {ins::kDrive, ins::kTrem}, {ins::kCrush, ins::kRing}};
    for (auto& pr : pairs) {
      Engine e = fresh();
      for (int v = 0; v < 8; ++v)
        for (int s = 0; s < 2; ++s) {
          ins::control.type[v][s] = (uint8_t)pr[s];
          for (int k = 0; k < 4; ++k) ins::control.play[v][s][k] = (uint8_t)(40 + 20 * k);
        }
      auto x = sine(330, 0.3f, 12);
      float b[12];
      const int blocks = 20000;
      CHECK(timing_retry("the pair bench", [&] {   // (wall-clock: up to 3 tries - tests/timing.h)
        auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < blocks; ++n)
          for (int v = 0; v < 8; ++v) {
            memcpy(b, x.data(), sizeof b);
            e.ProcessMono(v, b, 12, ins::control);
          }
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
        printf("CPU (Mac): 8 voices x %s + %s: %.2f us a 12-frame block (%.2f %% of its 250 us)\n", ins::type_name(pr[0]),
               ins::type_name(pr[1]), us, us / 250 * 100);
        return us < 50;
      }));
    }
  }
  // ---- the control side: knob / lock / LFO -> play, the defaults, the names, the console
  {
    ins::reset();
    ins::set_type(2, 0, ins::kFilter);
    CHECK(ins::control.type[2][0] == ins::kFilter && ins::control.play[2][0][0] == 80 && ins::param(2, 0, 0) == 80);
    ins::lock(2, 0, 0, 30);
    CHECK(ins::control.play[2][0][0] == 30 && ins::locked(2, 0, 0) == 30);
    ins::lfo_offset(2, 0, 0, 10);
    CHECK(ins::control.play[2][0][0] == 40);
    ins::lock(2, 0, 0, -1);
    CHECK(ins::control.play[2][0][0] == 90);
    ins::lfo_offset(2, 0, 0, 100);
    CHECK(ins::control.play[2][0][0] == 127);
    ins::lfo_offset(2, 0, 0, 0);
    char b[400], t[16];
    ins::value_text(ins::kFilter, 0, 72, t, sizeof t);
    CHECK(!strcmp(t, "1.0K"));
    ins::value_text(ins::kEq, 0, 127, t, sizeof t);
    CHECK(!strcmp(t, "+15.0DB"));
    ins::value_text(ins::kEq, 1, 64, t, sizeof t);
    CHECK(!strcmp(t, "0DB"));
    ins::value_text(ins::kDelay, 0, 100, t, sizeof t);
    printf("value_text DELAY TIME 100: %s\n", t);
    CHECK(!strcmp(t, "32MS"));
    CHECK(ins::command("ins=V1:A:type:delay", b, sizeof b) && strstr(b, "V1 A DELAY TIME"));
    CHECK(ins::control.type[0][0] == ins::kDelay && ins::control.play[0][0][0] == 90);
    CHECK(ins::command("ins=v1:a:p1:100", b, sizeof b) && strstr(b, "32MS"));
    CHECK(ins::command("ins=V1:send:3:90", b, sizeof b) && ins::control.send[0][2] == 90);
    CHECK(ins::command("ins=B3:A:type:comp", b, sizeof b) && ins::control.type[ins::kBus0 + 2][0] == ins::kComp);
    CHECK(ins::command("ins=b3:route:post", b, sizeof b) && ins::control.route3 == 1);
    CHECK(ins::command("ins=V9:A:type:eq", b, sizeof b) && strstr(b, "ins=C:S"));
    CHECK(ins::command("ins=V1:B:type:nope", b, sizeof b) && strstr(b, "no type"));
    CHECK(ins::command("ins", b, sizeof b) && strstr(b, "V1 DELAY/OFF send 0/0/90") && strstr(b, "B3 COMP/OFF"));
    printf("console: %s\n", b);
    CHECK(!ins::command("insx", b, sizeof b));
    static char j[4096];
    int n = ins::json(j, sizeof j);
    printf("json: %d bytes\n", n);
    CHECK(n > 0 && n < (int)sizeof j && strstr(j, "\"ins\":{") && strstr(j, "\"t\":\"DELAY\""));
    CHECK(ins::command("ins=reset", b, sizeof b) && !ins::any_on() && ins::control.send[0][2] == 0);
  }
  // ---- the LFOs on an insert (I3: voice 3's chain; B1: bus 1's)
  {
    ins::reset();
    lfo::reset();
    ins::set_type(2, 1, ins::kTrem);   // slot B
    char o[300];
    CHECK(lfo::command("lfo=1:tgt:i3", o, sizeof o) && lfo::get(0).target == lfo::kTIns + 2);
    CHECK(lfo::command("lfo=1:par:5", o, sizeof o) && strstr(o, "BDPTH"));   // slot B P2 (DPTH)
    CHECK(lfo::command("lfo=1:dep:30", o, sizeof o));
    CHECK(lfo::command("lfo=1:rate:127", o, sizeof o));   // 20 Hz: a cycle in 50 ms
    uint32_t us = 1000000;
    int lo = 127, hi = 0;
    for (int i = 0; i < 400; ++i) {
      us += 1000;
      lfo::tick(us);
      int pv = ins::control.play[2][1][1];
      lo = std::min(lo, pv), hi = std::max(hi, pv);
    }
    printf("LFO on I3 B DPTH (knob 90, depth 30): %d .. %d\n", lo, hi);
    CHECK(lo <= 62 && hi == 120);
    CHECK(lfo::command("lfo=1:off", o, sizeof o));
    for (int i = 0; i < 5; ++i) us += 1000, lfo::tick(us);
    CHECK(ins::control.play[2][1][1] == 90);   // the LFO gone: the knob again
    CHECK(lfo::command("lfo=2:tgt:b1", o, sizeof o) && lfo::get(1).target == lfo::kTIns + ins::kBus0);
    // O3: voice 3's OUT route, param 1 PAN (core/fx/out.h): an offset on the knob, gone with the LFO
    lfo::reset();
    out::reset();
    CHECK(lfo::command("lfo=1:tgt:o3", o, sizeof o) && lfo::get(0).target == lfo::kTOut + 2);
    CHECK(lfo::command("lfo=1:par:1", o, sizeof o) && strstr(o, "PAN"));
    CHECK(lfo::command("lfo=1:dep:40", o, sizeof o) && lfo::command("lfo=1:rate:127", o, sizeof o));
    lo = 127, hi = 0;
    for (int i = 0; i < 400; ++i) {
      us += 1000;
      lfo::tick(us);
      int pv = out::control.pan[2];
      lo = std::min(lo, pv), hi = std::max(hi, pv);
    }
    printf("LFO on O3 PAN (knob 64, depth 40): %d .. %d\n", lo, hi);
    CHECK(lo < 40 && hi > 88 && out::param(2, 1) == 64);
    CHECK(lfo::command("lfo=1:off", o, sizeof o));
    for (int i = 0; i < 5; ++i) us += 1000, lfo::tick(us);
    CHECK(out::control.pan[2] == 64);
    lfo::reset();
    out::reset();
  }
  // ---- the voice hook: a voice's chain in render_voices (nothing while off: the voices' output bit for bit)
  {
    ins::reset();
    voices_init();
    static int16_t ring[12][8], ref[12][8];
    uint64_t mach[kMachines] = {0};
    for (int k = 0; k < 8; ++k) v[k].live = true;
    int16_t a[12];
    for (int s = 0; s < 12; ++s) {
      a[s] = (int16_t)(10000 * std::sin(s * 0.5));
      for (int k = 0; k < 8; ++k) ref[s][k] = a[s];
    }
    int row[12];
    for (int s = 0; s < 12; ++s) row[s] = s;
    memcpy(ring, ref, sizeof ring);
    for (int k = 0; k < 8; ++k) ins::voice_out(k, ring, row, 12);   // all off: untouched
    CHECK(!memcmp(ring, ref, sizeof ring) && ins::stats().voice_blocks == 0);
    ins::set_type(3, 0, ins::kTrem);
    ins::set_param(3, 0, 1, 127);
    for (int i = 0; i < 100; ++i) ins::voice_out(3, ring, row, 12);   // fading in, then the tremolo
    CHECK(ins::stats().voice_blocks == 100 && memcmp(ring[0], ref[0], 6) == 0);   // voices 1..3 untouched
    (void)mach;
    ins::reset();
  }
  // ---- the buses: voice 1 sends to bus 1 (send FX 1 CHORUS, its bus chain a FILTER LP at 300 Hz) and to bus 3 (dry,
  // its chain a TREMOLO, PRE): SendFx::Process with the voice sends, bus3() onto its pair
  {
    ins::reset();
    memset((void*)&fx::control, 0, sizeof fx::control);
    static fx::SendFx sfx(48000.0f);
    fx::control.knob[0][0] = fx::kChorus;
    ins::set_type(ins::kBus0, 0, ins::kFilter);
    ins::set_param(ins::kBus0, 0, 0, 37);   // 20 x 2^(10 x 37/127) = 151 Hz
    ins::set_send(0, 0, 127);
    ins::set_send(0, 2, 127);
    ins::set_type(ins::kBus0 + 2, 0, ins::kTrem);
    CHECK(ins::sends_on() && ins::bus3_on());
    static float stems[96][8];
    float out[96][2], post[96][2], vs[96][3];
    double e_lo = 0, e_hi = 0, e3 = 0;
    for (int blk = 0; blk < 1000; ++blk) {
      int16_t V[96][8] = {};
      for (int i = 0; i < 96; ++i) {
        int64_t f = (int64_t)blk * 96 + i;
        V[i][0] = (int16_t)(8000 * (std::sin(2 * M_PI * 100 * f / 48000.0) + std::sin(2 * M_PI * 5000 * f / 48000.0)));
      }
      ins::voice_sends(V, 96, vs);
      sfx.Process(stems, 96, out, fx::control, post, nullptr, vs);
      if (blk > 200) {
        // the bus-1 return: 100 Hz kept, 5 kHz gone (the bus chain's LP)
        for (int i = 0; i < 96; ++i) {
          int64_t f = (int64_t)blk * 96 + i;
          e_lo += out[i][0] * std::sin(2 * M_PI * 100 * f / 48000.0);
          e_hi += out[i][0] * std::sin(2 * M_PI * 5000 * f / 48000.0);
        }
      }
      float pre3[96][2] = {}, mono[96];
      for (int i = 0; i < 96; ++i) mono[i] = vs[i][2];
      ins::bus3(mono, 96, pre3, nullptr);
      for (int i = 0; i < 96; ++i) e3 += pre3[i][0] * pre3[i][0];
    }
    printf("bus 1 (CHORUS + FILTER LP): |100 Hz| %.1f, |5 kHz| %.1f; bus 3 (TREMOLO) energy %.1f\n", std::fabs(e_lo),
           std::fabs(e_hi), e3);
    CHECK(std::fabs(e_hi) < 0.1 * std::fabs(e_lo) && e3 > 100 && ins::stats().bus_blocks > 1000);
    // the existing routing untouched without inserts: the same SendFx output with the bus chain off and no voice sends
    ins::reset();
    static fx::SendFx a(48000.0f), b2(48000.0f);
    for (int i = 0; i < 96; ++i)
      for (int k = 0; k < 8; ++k) stems[i][k] = 0.1f * std::sin(i * 0.1 * (k + 1));
    fx::control.send[0][0] = 100;
    float o1[96][2], o2[96][2], p1[96][2], p2[96][2];
    float zero[96][3] = {};
    for (int blk = 0; blk < 50; ++blk) {
      a.Process(stems, 96, o1, fx::control, p1);
      b2.Process(stems, 96, o2, fx::control, p2, nullptr, zero);
      CHECK(!memcmp(o1, o2, sizeof o1) && !memcmp(p1, p2, sizeof p1));
    }
    memset((void*)&fx::control, 0, sizeof fx::control);
  }
  // ---- ins1112: the send FX's RETURN filter. Bus 1 IS send FX 1 (docs/INSERT-FX.md), so bus 1's post EQ is the
  // HPF + LPF on the return - what stock's DELAY and REVERB have. The send FX here is CHORUS with no bus chain at
  // all, so anything the filter removes it removed on the return itself, not inside an insert.
  {
    ins::reset();
    memset((void*)&fx::control, 0, sizeof fx::control);
    fx::control.knob[0][0] = fx::kChorus;
    fx::control.send[0][0] = 127;
    static float stems2[96][8];
    // one tone at a time, the return's settled RMS: CHORUS keeps the level but smears the phase, so a correlation
    // against the input would say nothing useful here
    auto energy = [&](int hz) {
      static fx::SendFx sfx(48000.0f);
      sfx.Reset();
      float out[96][2], post[96][2];
      double e = 0;
      int n = 0;
      for (int blk = 0; blk < 600; ++blk) {
        for (int i = 0; i < 96; ++i) {
          int64_t f = (int64_t)blk * 96 + i;
          for (int k = 0; k < 8; ++k) stems2[i][k] = 0.0f;
          stems2[i][0] = 0.25f * (float)std::sin(2 * M_PI * hz * f / 48000.0);
        }
        sfx.Process(stems2, 96, out, fx::control, post);
        if (blk > 200)
          for (int i = 0; i < 96; ++i) e += (double)out[i][0] * out[i][0], ++n;
      }
      return std::sqrt(e / (double)(n ? n : 1));
    };
    double lo0 = energy(120), hi0 = energy(6000);
    CHECK(ins::eq_param(ins::kBus0, 1) == 0);
    ins::set_eq(ins::kBus0, 1, 55);   // send FX 1's return LPF, down at ~1 kHz
    double lo1 = energy(120), hi1 = energy(6000);
    ins::set_eq(ins::kBus0, 1, 0);
    ins::set_eq(ins::kBus0, 0, 90);   // and its return HPF, up at ~2.7 kHz
    double lo2 = energy(120), hi2 = energy(6000);
    printf("ins1112: send FX 1's return RMS: flat 120 Hz %.4f 6 kHz %.4f; LPF %.4f / %.4f; HPF %.4f / %.4f\n", lo0,
           hi0, lo1, hi1, lo2, hi2);
    CHECK(lo0 > 0.01 && hi0 > 0.01);
    CHECK(hi1 < 0.2 * hi0 && lo1 > 0.5 * lo0);   // the LPF took the 6 kHz off the return, kept the 120 Hz
    CHECK(lo2 < 0.2 * lo0 && hi2 > 0.5 * hi0);   // the HPF the other way round
    ins::reset();
    memset((void*)&fx::control, 0, sizeof fx::control);
  }
  // ---- ins1112: the control side, the console, the LFO targets and /status
  {
    ins::reset();
    const ins::Labels& L = ins::eq_labels();
    CHECK(!strcmp(L.label[0], "HPF") && !strcmp(L.label[1], "LPF") && !strcmp(L.label[2], "LOW") && !strcmp(L.label[3], "HIGH"));
    for (int k = 0; k < ins::kEqParams; ++k) CHECK(ins::eq_param(2, k) == ins::kEqFlat(k));
    CHECK(!ins::eq_on(2) && !ins::eq_any_on() && !ins::any_on());
    ins::set_eq(2, 0, 90);
    CHECK(ins::eq_on(2) && ins::eq_any_on() && ins::any_on() && ins::eq_playing(2, 0) == 90);
    CHECK(ins::control.peq[2][0] == 90);
    ins::eq_lfo_offset(2, 0, -20);
    CHECK(ins::eq_param(2, 0) == 90 && ins::eq_playing(2, 0) == 70 && ins::control.peq[2][0] == 70);
    ins::eq_lfo_offset(2, 0, 0);
    ins::set_eq(2, 2, 100);   // a shelf: stored as the knob less 64, so flat is a zero
    CHECK(ins::control.peq[2][2] == 36 && ins::eq_param(2, 2) == 100);
    ins::set_eq(2, 2, 64);
    CHECK(ins::control.peq[2][2] == 0);
    char t[16];
    ins::eq_value_text(0, 0, t, sizeof t); CHECK(!strcmp(t, "OFF"));
    ins::eq_value_text(1, 0, t, sizeof t); CHECK(!strcmp(t, "OFF"));
    ins::eq_value_text(2, 64, t, sizeof t); CHECK(!strcmp(t, "FLAT"));
    ins::eq_value_text(3, 127, t, sizeof t); CHECK(!strcmp(t, "+15.0DB"));
    ins::eq_value_text(2, 1, t, sizeof t); CHECK(!strcmp(t, "-14.8DB"));
    ins::eq_value_text(0, 127, t, sizeof t); CHECK(strlen(t) <= 7);
    for (int k = 0; k < ins::kEqParams; ++k)   // every value prints, and fits the Rytm's cell
      for (int v = 0; v < 128; ++v) {
        ins::eq_value_text(k, v, t, sizeof t);
        CHECK(t[0] && strlen(t) <= 7);
      }
    char o[400];
    CHECK(ins::command("ins=V3:eq:hpf:40", o, sizeof o));
    CHECK(ins::eq_param(2, 0) == 40 && strstr(o, "POST EQ") && strstr(o, "HPF"));
    CHECK(ins::command("ins=b1:eq:lpf:90", o, sizeof o));
    CHECK(ins::eq_param(ins::kBus0, 1) == 90);
    CHECK(ins::command("ins=V3:eq:low:100", o, sizeof o) && ins::eq_param(2, 2) == 100);
    CHECK(ins::command("ins=V3:eq:high:10", o, sizeof o) && ins::eq_param(2, 3) == 10);
    CHECK(ins::command("ins=V3:eq:off", o, sizeof o));
    for (int k = 0; k < ins::kEqParams; ++k) CHECK(ins::eq_param(2, k) == ins::kEqFlat(k));
    CHECK(ins::command("ins=V3:eq:nope:1", o, sizeof o) && strstr(o, "ins=") && ins::eq_param(2, 0) == 0);
    CHECK(ins::command("ins", o, sizeof o));
    CHECK(strstr(o, "B1") && strstr(o, "/EQ"));   // the bare dump says which chains have one
    // the LFO: targets I1..I8 / B1..B3 now carry 12 params, 8..11 the post EQ
    CHECK(lfo::params_of(lfo::kTIns + 2) == 8 + ins::kEqParams);
    char nb[16];
    CHECK(!strcmp(lfo::param_name(lfo::kTIns + 2, 8, nb, sizeof nb), "QHPF"));
    CHECK(!strcmp(lfo::param_name(lfo::kTIns + 2, 11, nb, sizeof nb), "QHIG"));
    CHECK(lfo::command("lfo=1:tgt:I3", o, sizeof o) && lfo::command("lfo=1:par:9", o, sizeof o));
    CHECK(lfo::command("lfo=1:par:12", o, sizeof o));   // clamped to the target's last
    CHECK(lfo::get(0).param == 8 + ins::kEqParams - 1);
    CHECK(lfo::command("lfo=1:off", o, sizeof o));
    // /status: "peq" (set) and "peql" (playing) per chain
    ins::set_eq(0, 0, 55);
    static char js[8192];
    int n = ins::json(js, sizeof js);
    CHECK(n > 0 && n < (int)sizeof js);
    CHECK(strstr(js, "\"peq\":[55,0,64,64]") && strstr(js, "\"peql\":[55,0,64,64]"));
    ins::reset();
  }
  // ---- the remote INSERT page: listed once a chain is set (or the Rytm names a Sidecar track), drawn from the values
  {
    ins::reset();
    int k = -1;
    for (int i = 0; i < ui::extra_pages(); ++i)
      if (!strcmp(ui::extra_page(i).title, "INSERT")) k = i;
    CHECK(k >= 0 && !ui::extra_page(k).shown());
    ins::set_type(0, 0, ins::kFilter);
    CHECK(ui::extra_page(k).shown());
    ui::ins_page_set(false, 0, 0, 0);
    ui::Canvas c;
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::extra_page(k).enc(0, 5, false);   // A: CUT +5
    CHECK(ins::param(0, 0, 0) == 85);
    ui::extra_page(k).enc(5, 1, false);   // F: TYPE +1 -> CRUSH (its defaults)
    CHECK(ins::control.type[0][0] == ins::kCrush && ins::param(0, 0, 0) == 40);
    ui::extra_page(k).enc(7, 10, false);   // H: the send to bus 1
    CHECK(ins::control.send[0][0] == 10);
    CHECK(ui::extra_page(k).key(0x44));   // YES: the BUS view
    ui::extra_page(k).enc(6, 2, false);   // G: bus 3
    ui::extra_page(k).enc(7, 1, false);   // H: ROUTE POST
    CHECK(ins::control.route3 == 1);
    ui::extra_page(k).enc(5, 4, false);   // F: bus 3 slot A -> EQ
    CHECK(ins::control.type[ins::kBus0 + 2][0] == ins::kEq);
    CHECK(ui::extra_page(k).key(0x45) && !ui::extra_page(k).key(0x45));   // NO: back to TRACK, then not the page's
    // the OUT view (YES twice): A OUT -> POST, B LEV, C PAN; drawn (the text fits); the TRACK view's OUT box; NO back
    CHECK(ui::extra_page(k).key(0x44) && ui::extra_page(k).key(0x44));
    ui::extra_page(k).enc(0, 2, false);
    CHECK(out::control.route[0] == out::kPost);
    ui::extra_page(k).enc(1, -10, false), ui::extra_page(k).enc(2, 20, false);
    CHECK(out::param(0, 0) == 90 && out::control.pan[0] == 84);
    ui::extra_page(k).enc(4, 5, false);   // E DEC: only for DSP SAMPLE (none here): unchanged
    CHECK(out::param(0, 3) == 0);
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    const std::string pd = rui_png_dir("RUI_PAGES", "build/rui_pages");   // rui1046: the docs' renders
    uint8_t fb[ui::kBytes];
    ui::preview(c, fb); CHECK(rui_png(pd, "out_view.png", fb));
    CHECK(ui::extra_page(k).key(0x45));
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "out_track_view.png", fb));
    // rui1047, UP + AMP (30 page 3): the page opens on the OUT view of the Rytm's selected track, from any view; page 2
    // (UP + FX) and an older host's unknown page stay as they were
    for (int from = 0; from < 3; ++from) {
      out::reset();
      ui::ins_page_set(from == 1, 0, 0, 3, false);   // TRACK view of track 4 (a FUNC + G pick), BUS view, TRACK view
      ui::remote_reset();
      const uint8_t m3[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, from == 2 ? (uint8_t)2 : (uint8_t)3, 1, 0, 0xF7};
      ui::remote_sysex(m3, (int)sizeof m3);
      CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "INSERT"));
      ui::extra_page(k).enc(0, 2, false);   // A: in the OUT view the voice's route; on the TRACK / BUS views a slot's param
      if (from < 2) CHECK(out::control.route[0] == out::kPost);   // (track 0, the Rytm's selection, not the picked 4)
      else CHECK(out::control.route[0] == 0);                      // page 2: the TRACK view
      CHECK(ui::extra_page(k).key(0x45) == (from < 2));            // NO leaves the OUT view, then not the page's
    }
    out::reset();
    ins::reset();
  }
  // ---- ins1112: the EQ view, RIGHT on the INSERT page (LEFT / NO back, YES the voice's EQ <-> the bus's); rendered
  {
    ins::reset();
    int k = -1;
    for (int i = 0; i < ui::extra_pages(); ++i)
      if (!strcmp(ui::extra_page(i).title, "INSERT")) k = i;
    CHECK(k >= 0);
    ins::set_type(0, 0, ins::kFilter);
    ins::set_type(0, 1, ins::kDrive);
    ui::ins_page_set(false, 0, 0, 0);
    ui::Canvas c;
    uint8_t fb[ui::kBytes];
    const std::string pd = rui_png_dir("RUI_PAGES", "build/rui_pages");
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "ins1112_1_track_view.png", fb));
    CHECK(ui::extra_page(k).key(0x41));   // RIGHT: the EQ view
    ui::extra_page(k).enc(0, 40, false);
    ui::extra_page(k).enc(2, 30, false);
    CHECK(ins::eq_param(0, 0) == 40 && ins::eq_param(0, 2) == 94);
    ui::extra_page(k).enc(1, 10, true);   // FUNC: by 8
    CHECK(ins::eq_param(0, 1) == 80);
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "ins1112_2_eq_view.png", fb));
    CHECK(ins::eq_on(0));
    CHECK(fb[(7 - 13 / 8) + 8 * 50] >> (13 % 8) & 1);   // the strip's POST EQ box is filled while the EQ is on
    CHECK(!ui::extra_page(k).key(0x41));   // RIGHT again: the page list's, as always
    CHECK(ui::extra_page(k).key(0x44));    // YES: the bus's EQ
    ui::extra_page(k).enc(6, 1, false);    // G: bus 2
    ui::extra_page(k).enc(1, 90, false);
    CHECK(ins::eq_param(ins::kBus0 + 1, 1) == 90);
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "ins1112_3_eq_bus_view.png", fb));
    CHECK(ui::extra_page(k).key(0x40));    // LEFT: back out of the EQ view (into the BUS view)
    CHECK(!ui::extra_page(k).key(0x40));   // LEFT again: the page list's
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "ins1112_4_bus_view.png", fb));
    CHECK(ui::extra_page(k).key(0x41) && ui::extra_page(k).key(0x45));   // RIGHT then NO: back to the TRACK view
    CHECK(!ui::extra_page(k).key(0x45));
    // the EQ view is listed and drawn even with no slot set at all (a post EQ alone)
    ins::reset();
    CHECK(!ui::extra_page(k).shown());
    ins::set_eq(0, 0, 60);
    CHECK(ui::extra_page(k).shown());
    ui::ins_page_set(false, 0, 0, 0, false, true);
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "ins1112_5_eq_no_slots.png", fb));
    // the bus EQ view untouched (its footer says where the stage is), and the voice's: both drawn, text-fit checked
    ins::set_eq(ins::kBus0, 0, 70);
    ui::ins_page_set(true, 0, 0, 0, false, true);
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "ins1112_6_eq_bus_untouched.png", fb));
    ui::ins_page_set(true, 0, 2, 0, false, true);   // bus 3: no send FX of its own
    CHECK(ui::remote_render(6 + k, 0, nullptr, c));
    ui::preview(c, fb); CHECK(rui_png(pd, "ins1112_7_eq_bus3.png", fb));
    ui::ins_page_set(false, 0, 0, 0);
    ins::reset();
  }
  // ---- ux1027 (the user, 2026-10-27): UP + FX / UP + AMP's page replaces the Rytm's screen, as the SRC view does - no
  // LEFT / RIGHT siblings (RIGHT the EQ view, LEFT back out of it, nothing more); the same UP + key again leaves it
  {
    static int closes = 0;
    hooks.to_rytm = [](const uint8_t* d, int len) {
      if (len >= 9 && d[4] == 0x3A && d[7] == 0) ++closes;
    };
    ins::reset();
    ins::set_type(0, 0, ins::kFilter);
    ui::ins_page_set(false, 0, 0, 0);
    ui::remote_reset();
    auto rytm = [](uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
      const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
      ui::remote_sysex(m, 10);
    };
    auto key = [&](int code) { rytm(0x31, 0, (uint8_t)code, 1), rytm(0x31, 0, (uint8_t)code, 0); };
    auto on = [](const char* t) { return ui::remote_page_title() && !strcmp(ui::remote_page_title(), t); };
    rytm(0x30, 2, 1, 0);   // UP + FX on the stock screen
    CHECK(on("INSERT"));
    for (int c : {0x40, 0x40, 0x41, 0x41, 0x41, 0x43, 0x84, 0x40, 0x40}) {   // RIGHT: EQ; again nothing; LEFT: out
      key(c);
      CHECK(on("INSERT"));
    }
    closes = 0;
    rytm(0x30, 2, 1, 0);   // UP + FX again: leaves (where it came from: the stock screen)
    CHECK(closes == 1);
    rytm(0x30, 0, 0, 3);
    closes = 0;
    rytm(0x30, 3, 1, 0);   // UP + AMP: the OUT view
    CHECK(on("INSERT"));
    key(0x41), key(0x40), key(0x41);
    CHECK(on("INSERT"));
    rytm(0x30, 2, 1, 0);   // UP + FX on UP + AMP's page: the page stays (another key's), no leave
    CHECK(on("INSERT") && closes == 0);
    rytm(0x30, 2, 1, 0);   // and again: leaves
    CHECK(closes == 1);
    rytm(0x30, 0, 0, 3);
    closes = 0;
    rytm(0x30, 3, 1, 0);   // UP + AMP: in
    ui::ins_page_set(false, 0, 0, 3, false);   // FUNC + G picked track 4 meanwhile (TRACK view)
    rytm(0x30, 3, 1, 0);   // review 4: UP + AMP again re-aims the OUT view at the Rytm's selected track (rui1047), no leave
    CHECK(on("INSERT") && closes == 0);
    {
      int k = -1;
      for (int i = 0; i < ui::extra_pages(); ++i)
        if (!strcmp(ui::extra_page(i).title, "INSERT")) k = i;
      out::reset();
      ui::extra_page(k).enc(0, 2, false);   // A on the OUT view: the selected track's (0) route
      CHECK(out::control.route[0] == out::kPost);
      out::reset();
    }
    key(0x45), key(0x45);   // NO: back to TRACK, then where UP + AMP came from (the stock screen)
    CHECK(closes == 1);
    rytm(0x30, 0, 0, 3);
    // review 3: the page byte is forgotten at any other page change - UP + FX, UP + LFO over it, NO back to INSERT:
    // UP + FX there goes to INSERT (it is there), no leave; again: leaves
    rytm(0x30, 2, 1, 0), rytm(0x30, 1, 1, 0);
    CHECK(on("LFO"));
    key(0x45);
    CHECK(on("INSERT"));
    closes = 0;
    rytm(0x30, 2, 1, 0);
    CHECK(on("INSERT") && closes == 0);
    rytm(0x30, 2, 1, 0);
    CHECK(closes == 1);
    rytm(0x30, 0, 0, 3);
    hooks.to_rytm = nullptr;
    ui::remote_reset();
    ui::ins_page_set(false, 0, 0, 0);
    ins::reset();
    printf("insert: ux1027 UP + FX / UP + AMP: no LEFT / RIGHT siblings, UP + FX again leaves, UP + AMP again re-aims\n");
  }
  // ---- the card: per kit (/.rytm/ins/<project>/kNNN.txt), the project's file the default; a switch saves the old kit's
  // unsaved edits first
  {
    char tmpl[] = "/tmp/core_ins_XXXXXX";
    CHECK(mkdtemp(tmpl));
    CHECK(mkdir((std::string(tmpl) + "/Samples").c_str(), 0755) == 0);   // a card (the index wants its /Samples)
    host_set_card(tmpl);
    uint64_t us = 5000000;
    host_fake_time(true, us);
    engine_begin();
    loaded();
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'I', 'N', 'S', 0xF7};
    engine_sysex(m, (int)sizeof m);
    loaded();
    ins::kit_changed(4);   // kit 5
    CHECK(ins::kit() == 4 && !ins::any_on());
    ins::set_type(1, 0, ins::kEq);
    ins::set_param(1, 0, 2, 100);
    ins::set_type(ins::kBus0 + 2, 1, ins::kDelay);
    ins::set_send(1, 2, 77);
    ins::set_route3(1);
    CHECK(!ins::save());   // not yet: a second after the last change
    host_fake_time(true, us += 1100000);
    for (int i = 0; i < 64; ++i) idle_save();
    CHECK(access((std::string(tmpl) + "/.rytm/ins/INS/k005.txt").c_str(), 0) == 0);
    ins::kit_changed(5);   // kit 6: none of its own, no project file: all off
    CHECK(ins::kit() == 5 && !ins::any_on() && ins::control.send[1][2] == 0);
    ins::set_type(7, 1, ins::kRing);   // kit 6's edit, unsaved: the switch back writes it first
    ins::kit_changed(4);
    CHECK(ins::control.type[1][0] == ins::kEq && ins::param(1, 0, 2) == 100 && ins::param(1, 0, 0) == 64 &&
          ins::control.type[ins::kBus0 + 2][1] == ins::kDelay && ins::control.send[1][2] == 77 && ins::control.route3 == 1 &&
          !ins::control.type[7][1]);
    ins::kit_changed(5);
    CHECK(ins::control.type[7][1] == ins::kRing && !ins::control.type[1][0]);
    printf("card: kit files %s/.rytm/ins/INS/k005.txt, k006.txt\n", tmpl);
    // 1042 data safety: a kit file with a line this build does not read (a newer Sidecar's FX type) is never written
    // over: the settings change in memory only
    {
      const char bad[] = "V1 A EQ 64 64 100\nV2 B FUTUREFX 1 2 3\n";
      std::string fn = std::string(tmpl) + "/.rytm/ins/INS/k008.txt";
      FILE* fp = fopen(fn.c_str(), "wb");
      fwrite(bad, 1, sizeof bad - 1, fp);
      fclose(fp);
      project_fill_begin();   // the project loaded anew, with that file (the store holds what the card had at the load)
      loaded();
      ins::kit_changed(7);   // kit 8: that file
      ins::set_type(3, 0, ins::kRing);
      host_fake_time(true, us += 1100000);
      for (int i = 0; i < 64; ++i) idle_save();
      char got[128] = {};
      fp = fopen(fn.c_str(), "rb");
      size_t n = fp ? fread(got, 1, sizeof got - 1, fp) : 0;
      if (fp) fclose(fp);
      CHECK(n == sizeof bad - 1 && !memcmp(got, bad, n));
      printf("card: 1042: an unreadable line locks its file (not written over)\n");
    }
    // ins1112: the post EQ in the kit's file ("PEQ V1 40 0 64 70"), read back; a flat chain no line
    {
      ins::kit_changed(11);   // kit 12: a file of its own
      ins::set_eq(0, 0, 40);
      ins::set_eq(0, 3, 70);
      ins::set_eq(ins::kBus0, 1, 90);
      host_fake_time(true, us += 1100000);
      for (int i = 0; i < 64; ++i) idle_save();
      std::string fn = std::string(tmpl) + "/.rytm/ins/INS/k012.txt";
      char got[800] = {};
      FILE* fp = fopen(fn.c_str(), "rb");
      size_t n = fp ? fread(got, 1, sizeof got - 1, fp) : 0;
      if (fp) fclose(fp);
      CHECK(n > 0 && strstr(got, "PEQ V1 40 0 64 70\n") && strstr(got, "PEQ B1 0 90 64 64\n") && !strstr(got, "PEQ V2"));
      ins::reset();
      CHECK(!ins::eq_any_on());
      ins::kit_changed(4), ins::kit_changed(11);   // away and back
      CHECK(ins::eq_param(0, 0) == 40 && ins::eq_param(0, 3) == 70 && ins::eq_param(ins::kBus0, 1) == 90 &&
            !ins::eq_on(1));
      printf("card: the PEQ line round trip (%s)\n", fn.c_str());
      ins::reset();
    }
    // the OUT route (core/fx/out.h) in the kit's file: "OUT V3 POST 90 20 100 30", read back; a default voice no line
    {
      ins::kit_changed(9);   // kit 10: a file of its own
      out::set_route(2, out::kPost);
      out::set_param(2, 0, 90), out::set_param(2, 1, 20), out::set_param(2, 2, 100), out::set_param(2, 3, 30);
      host_fake_time(true, us += 1100000);
      for (int i = 0; i < 64; ++i) idle_save();
      std::string fn = std::string(tmpl) + "/.rytm/ins/INS/k010.txt";
      char got[512] = {};
      FILE* fp = fopen(fn.c_str(), "rb");
      size_t n = fp ? fread(got, 1, sizeof got - 1, fp) : 0;
      if (fp) fclose(fp);
      CHECK(n > 0 && strstr(got, "OUT V3 POST 90 20 100 30\n") && !strstr(got, "OUT V1"));
      out::reset();
      CHECK(out::control.route[2] == out::kTrk);
      ins::kit_changed(4), ins::kit_changed(9);   // away and back: loaded again
      CHECK(out::control.route[2] == out::kPost && out::param(2, 0) == 90 && out::control.pan[2] == 20 &&
            out::control.wid[2] == 100 && out::control.dec[2] == 30);
      char o[300];
      CHECK(out::command("out=V3:pre", o, sizeof o) && out::control.route[2] == out::kPre && strstr(o, "V3 PRE"));
      CHECK(out::command("out=V3:dec:off", o, sizeof o) && out::control.dec[2] == 0);
      CHECK(out::command("out=V2:lev:200", o, sizeof o) && strstr(o, "out=Vn"));   // out of range: the help
      CHECK(!out::command("out=main", o, sizeof o) && !out::command("out=tracks", o, sizeof o));   // the output stage's
      CHECK(out::command("out=list", o, sizeof o) && strstr(o, "V3 PRE LEV 90 PAN L44"));
      ins::json(o, sizeof o);
      out::reset();
      printf("card: the OUT line round trip, the console\n");
      // the mute gate's tracks (the device table, voice order BD BT SD LT CH/OH MT/HT CY/CB RS/CP): BT muted closes
      // voice 2 only; CH alone leaves CH/OH open (its trig's track unknown), CH and OH close it; a known track wins
      out::set_mutes(1u << 4);
      for (int v = 0; v < 8; ++v) CHECK(out::control.open[v] == (v == 1 ? 0 : 1));
      out::set_mutes(1u << 8);
      CHECK(out::control.open[4] == 1);
      out::set_mutes(1u << 8 | 1u << 9);
      CHECK(out::control.open[4] == 0 && out::control.open[2] == 1);
      out::set_mutes(1u << 8);
      out::trig_track(4, 8);
      CHECK(out::control.open[4] == 0);
      out::set_mutes(0);
    }
    ins::reset();
    std::string rm = std::string("rm -rf ") + tmpl;
    CHECK(system(rm.c_str()) == 0);
  }
  // ---- a voice's chain in stereo (OUT PRE / POST) = the bus chain on the same input, every TYPE (DELAY's R a quarter
  // cycle behind, TREMOLO's PAN real); the mono chain of a TRK voice unchanged by it
  {
    std::vector<float> xl = sine(220, 0.2f, 9600), xr = sine(330, 0.15f, 9600);
    for (int t = 1; t < ins::kTypes; ++t) {
      Engine e = fresh();
      for (int c : {0, ins::kBus0}) {
        ins::control.type[c][0] = (uint8_t)t;
        for (int k = 0; k < 4; ++k) ins::control.play[c][0][k] = (uint8_t)(ins::labels(t).def[k] + 7 * k + 3);
      }
      std::vector<float> vl = xl, vr = xr;
      std::vector<float> by(2 * xl.size());
      for (size_t i = 0; i < xl.size(); ++i) by[2 * i] = xl[i], by[2 * i + 1] = xr[i];
      for (size_t i = 0; i < xl.size(); i += 12) {
        e.ProcessVoiceStereo(0, &vl[i], &vr[i], 12, ins::control);
        e.ProcessStereo(ins::kBus0, (float(*)[2])&by[2 * i], 12, ins::control);
      }
      for (size_t i = 0; i < xl.size(); ++i) CHECK(vl[i] == by[2 * i] && vr[i] == by[2 * i + 1]);
    }
    printf("test_insert: a voice's stereo chain = the bus chain, every TYPE\n");
  }
  {
    char tmpl[] = "/tmp/core_ins_store_XXXXXX";
    CHECK(mkdtemp(tmpl));
    CHECK(mkdir((std::string(tmpl) + "/Samples").c_str(), 0755) == 0);
    host_set_card(tmpl);
    host_fake_time(true, 90000000);
    engine_begin();
    loaded();
    kit_change_reads_ram_only(tmpl);
    host_fake_time(true, 92000000);
    edit_then_switch_back(tmpl);
    uint64_t us = 94000000;
    unreadable_default_does_not_lock_the_kit(tmpl, us);
    ins::reset();
    CHECK(system((std::string("rm -rf ") + tmpl).c_str()) == 0);
  }
  printf("test_insert: PASS\n");
  return 0;
}
