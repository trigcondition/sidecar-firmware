// core/tests/test_livein.cpp - LIVE IN (core/livein/livein.h, docs/LIVE-IN.md): the machine's declaration (names, mono
// only, the page and its labels' fit), the override, the signal path (an injected AUDIO IN through an INSERT AMP and a
// SPACE send), INPT's mono / stereo modes with the OUT route, GAIN, the gate and its hold, MON TRIG with sequencer and
// Rytm trigs, underruns, the feedback guard (a simulated loop muted, a swell left alone), the round-trip measure, no
// allocation and no card time on the render core, the CPU.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "fx/amp.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "inst.h"
#include "inst_alloc.h"
#include "livein/livein.h"
#include "machines.h"
#include "ui/page.h"
#include "voices.h"

using namespace core;

// ---- the allocation counter (the render path must not allocate)
static long g_news = 0;
void* operator new(size_t n) {
  ++g_news;
  void* p = malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) {
  ++g_news;
  void* p = malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

static const int kV = 2;   // the voice under test (LT on the Rytm)
static int16_t ring[kBlock][kVoices], ring_r[kBlock][kVoices];
static uint8_t st[kVoices];
static uint64_t mach[kMachines];
static std::vector<float> outl, outr;   // voice kV's output, -1..1 of the voice's return (int16 / 32767)

// One block: n input frames (l / r, null: none: an underrun) into the ring, then render_voices with the R ring
static void block(const float* l, const float* r, int nin = kBlock) {
  if (l && nin > 0) livein::in_frames(l, r, nin);
  render_voices(ring, kBlock, 0, 0, kVoices, mach, ring_r, st);
  for (int s = 0; s < kBlock; ++s) outl.push_back(ring[s][kV] / 32767.0f), outr.push_back(ring_r[s][kV] / 32767.0f);
}
// n blocks of a generator g(frame, &l, &r)
static long g_f = 0;   // the generators' frame (fresh() starts it again)
template <class G>
static void run(int blocks, G g) {
  for (int b = 0; b < blocks; ++b) {
    float l[kBlock], r[kBlock];
    for (int s = 0; s < kBlock; ++s) g(g_f + s, &l[s], &r[s]);
    g_f += kBlock;
    block(l, r);
  }
}
static double rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
  return b > a ? std::sqrt(s / (double)(b - a)) : 0;
}
static double peak(const std::vector<float>& x, size_t a, size_t b) {
  double p = 0;
  for (size_t i = a; i < b; ++i) p = std::max(p, (double)std::fabs(x[i]));
  return p;
}
// the share of x[a..b) at frequency hz (its amplitude)
static double tone(const std::vector<float>& x, size_t a, size_t b, double hz) {
  double c = 0, s = 0;
  for (size_t i = a; i < b; ++i) c += x[i] * std::cos(2 * M_PI * hz * i / 48000.0), s += x[i] * std::sin(2 * M_PI * hz * i / 48000.0);
  return 2 * std::sqrt(c * c + s * s) / (double)(b - a);
}
static void knobs(std::vector<float> p) {   // P1..P9 of voice kV (as a report leaves them; the render reads x.p)
  for (size_t k = 0; k < p.size(); ++k) v[kV].p[k] = p[k];
}
static std::string cmd(const char* c) {
  char b[1200];
  CHECK(livein::command(c, b, sizeof b));
  return b;
}
static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) m.push_back((uint8_t)note), m.push_back((uint8_t)vel);
  p.resize(16, 0);
  for (float x : p) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  m.push_back(0xf7);
  return m;
}
static void put(std::vector<uint8_t>& b, uint32_t v, int n) {
  for (int i = 0; i < n; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}
static std::vector<uint8_t> namb(const std::vector<float>& w) {   // as render_amp.cpp's
  const size_t cfg = 68, woff = (84 + cfg + 3) & ~(size_t)3, size = woff + 4 * w.size();
  std::vector<uint8_t> b(size, 0);
  auto p16 = [&](size_t a, uint16_t x) { b[a] = (uint8_t)x, b[a + 1] = (uint8_t)(x >> 8); };
  auto p32 = [&](size_t a, uint32_t x) { for (int i = 0; i < 4; ++i) b[a + i] = (uint8_t)(x >> (8 * i)); };
  double rate = 48000.0;
  p32(0, 0x4E414D42u), p16(4, 1), p32(8, (uint32_t)size), p32(12, (uint32_t)woff), p32(16, (uint32_t)w.size()), p32(20, 4 + cfg);
  memcpy(&b[36], &rate, 8);
  b[80] = 3, p16(82, (uint16_t)cfg), b[84] = 1, b[86] = 1;
  p16(88, 1), p16(90, 1), p16(92, 1), p16(94, 3), p16(96, 3), p16(98, 16), b[101] = 23;
  memcpy(&b[woff], w.data(), 4 * w.size());
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    if (i >= 24 && i < 28) continue;
    c ^= b[i];
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
  }
  p32(24, c ^ 0xFFFFFFFFu);
  return b;
}
// the default knobs: L+R, 0 dB, no gate, HOLD 50 ms, ALW, ATK 0.5 ms DEC 300 ms SUS 0 REL 300 ms
static const std::vector<float> kDef = {80, 64, 0, 28, 0, 0, 77, 0, 77};
static void fresh() {
  livein::silence(kV);
  ins::reset();
  out::set_route(kV, out::kTrk);
  for (int k = 0; k < 400; ++k) block(nullptr, nullptr, 0);   // the route's crossfade and the silence over
  knobs(kDef);
  outl.clear(), outr.clear();
  g_f = 0;
}

int main() {
  // ---- the card: a synthetic A2-Lite capture in /NAM/ for the INSERT AMP
  char tmpl[] = "/tmp/test_livein.XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  mkdir((card + "/NAM").c_str(), 0755);
  mkdir((card + "/Samples").c_str(), 0755);
  {
    std::vector<float> w(amp::kWeights);
    amp::synthetic_weights(w.data(), 1);
    auto b = namb(w);
    FILE* f = fopen((card + "/NAM/Synth.namb").c_str(), "wb");
    CHECK(f && fwrite(b.data(), 1, b.size(), f) == b.size());
    fclose(f);
  }
  host_set_card(tmpl);
  CHECK(engine_begin());
  ins::reset();
  for (int i = 0; i < 100; ++i) amp::step();
  CHECK(amp::models() == 1);

  // ---- the declaration: names, mono only (a poly config refuses more), the device's AUDIO IN in its table
  {
    CHECK(!strcmp(machine_short(kLiveMachine), "LIVEIN") && strlen(machine_short(kLiveMachine)) <= 6);
    CHECK(!strcmp(machine_long(kLiveMachine), "LIVE IN") && strlen(machine_long(kLiveMachine)) <= 10);
    const inst::Decl* d = inst::builtin(kLiveMachine);
    CHECK(d && d->machine == kLiveMachine && d->max_voices == 1 && d->def_voices == 1 && d->pitch == inst::kPitchNone);
    seq::PolyCfg c = {};
    c.voices = 4, c.unison = 2;
    inst::Resolved r = inst::resolve(c, *d);
    CHECK(r.instances == 1 && r.lanes == 1 && r.clamped);
    CHECK(out::device().audio_in[0] == 10 && out::device().audio_in[1] == 11 && out::device().loop_frames == 289);
    // the labels (the page's and the console's) and every knob's text: short enough for the Rytm's boxes
    const ui::Page* p = ui::page_for_machine(kLiveMachine);
    CHECK(p && p->machine == kLiveMachine && !strcmp(p->title, "LIVE IN"));
    for (int k = 0; k < 8; ++k) CHECK(strlen(p->label[k]) <= 4 && !strcmp(p->label[k], livein::label(k)));
    CHECK(strlen(livein::label(8)) <= 4);
    char t[16];
    for (int k = 0; k < 9; ++k)
      for (int x = 0; x < 128; ++x) {
        livein::value_text(k, (float)x, t, sizeof t);
        CHECK(strlen(t) >= 1 && strlen(t) <= 6);
      }
    livein::value_text(1, 64, t, sizeof t); CHECK(!strcmp(t, "0DB"));
    livein::value_text(1, 127.99f, t, sizeof t); CHECK(!strcmp(t, "+24DB"));
    livein::value_text(1, 0, t, sizeof t); CHECK(!strcmp(t, "-24DB"));
    livein::value_text(2, 0, t, sizeof t); CHECK(!strcmp(t, "OFF"));
    livein::value_text(2, 1, t, sizeof t); CHECK(!strcmp(t, "-80DB"));
    livein::value_text(2, 127, t, sizeof t); CHECK(!strcmp(t, "-20DB"));
    livein::value_text(0, 0, t, sizeof t); CHECK(!strcmp(t, "L"));
    livein::value_text(0, 127, t, sizeof t); CHECK(!strcmp(t, "STR"));
    livein::value_text(4, 100, t, sizeof t); CHECK(!strcmp(t, "TRIG"));
    // the page drawn (the text-fit check of check.h runs on every text it puts on the Rytm's screen)
    struct Src : ui::Source {
      int param(int k) const override { return (int)kDef[k < 9 ? k : 0]; }
    } src;
    ui::Canvas cv;
    ui::PageView pv;
    CHECK(pv.draw(cv, *p, src, "V3", 0, 1) >= p->ncells);
    printf("declaration: LIVE IN / LIVEIN, mono only (VOICES 4 x UNISON 2 -> 1 instance), page %s %s %s %s / %s %s %s %s\n",
           p->label[0], p->label[1], p->label[2], p->label[3], p->label[4], p->label[5], p->label[6], p->label[7]);
  }

  // ---- the override: livein=voice:kV plays it at once and keeps it whatever built-in machine the Rytm reports
  {
    std::string a = cmd("livein=voice:2");
    CHECK(livein::forced(kV) && v[kV].li && v[kV].live && livein::wanted());
    auto m = report(0x04, kPlaits | kV, 60, 100, kDef);
    voice_report(0x04, m.data(), (int)m.size());
    CHECK(v[kV].li && !v[kV].md && !v[kV].fm);
    CHECK(cmd("livein=voice:9").find("0..7") != std::string::npos);
    CHECK(cmd("livein").find("voice 2:") != std::string::npos);
    char b[64];
    CHECK(!livein::command("liveinx", b, sizeof b) && !livein::command("ins", b, sizeof b));
  }

  // ---- the signal path: a 220 Hz tone on AUDIO IN L+R comes out of voice kV at the same level (TRK: mono)
  fresh();
  {
    uint32_t u0 = livein::stats(kV).underruns;
    run(400, [](long f, float* l, float* r) { *l = *r = 0.25f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    double a = tone(outl, 2400, outl.size(), 220);   // the voice's return: 0.7 x the input (the voices' headroom)
    printf("signal: in 0.250 at 220 Hz -> out %.4f (x %.3f; 0.7 expected)\n", a, a / 0.25);
    CHECK(std::fabs(a / 0.25 - 0.7) < 0.01);
    CHECK(livein::stats(kV).underruns == u0 && livein::stats(kV).stream);
    // GAIN +12 dB / -12 dB
    knobs({80, 96});
    outl.clear();
    run(400, [](long f, float* l, float* r) { *l = *r = 0.05f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    double g = tone(outl, 2400, outl.size(), 220) / (0.05 * 0.7);
    knobs({80, 32});
    outl.clear();
    run(400, [](long f, float* l, float* r) { *l = *r = 0.25f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    double h = tone(outl, 2400, outl.size(), 220) / (0.25 * 0.7);
    printf("GAIN 96: %+.2f dB, GAIN 32: %+.2f dB\n", 20 * std::log10(g), 20 * std::log10(h));
    CHECK(std::fabs(20 * std::log10(g) - 12) < 0.2 && std::fabs(20 * std::log10(h) + 12) < 0.2);
    // CC 120 / a link reset: a live input is not a ringing sound - it passes on (no trig needed to bring it back)
    knobs(kDef);
    voice_silence(kV);
    outl.clear();
    run(400, [](long f, float* l, float* r) { *l = *r = 0.25f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    CHECK(std::fabs(tone(outl, 2400, outl.size(), 220) / 0.25 - 0.7) < 0.01);
  }

  // ---- through an INSERT AMP (slot A, the synthetic capture) and a SPACE send (send FX 1 TYPE 5 via bus 1)
  fresh();
  {
    auto sfx = std::make_unique<fx::SendFx>(48000.0f);
    memset((void*)&fx::control, 0, sizeof fx::control);
    fx::control.knob[0][0] = fx::kTape, fx::control.knob[0][2] = 25, fx::control.knob[0][3] = 50;   // SPACE: TIME 25, RPT 50
    ins::set_type(kV, 0, ins::kAmp), ins::set_param(kV, 0, 0, amp::knob_for_model(0));
    for (int i = 0; i < 400; ++i) amp::step(), amp::service(kV);
    ins::set_send(kV, 0, 127);
    static float stems[kBlock][fx::kStems];
    std::vector<float> fxo;
    double dry_on = 0, dry_off = 0;
    // a plucked line on AUDIO IN for 1 s, then silence for 1 s
    auto pluck = [](long f, float* l, float* r) {
      float t = (float)(f % 12000) / 48000.0f, ph = 110.0f * t;
      float x = f < 48000 ? 0.3f * std::exp(-6.0f * t) * (2.0f * (ph - std::floor(ph)) - 1.0f) : 0.0f;
      *l = *r = x;
    };
    for (int b = 0; b < 8000; ++b) {
      amp::step();
      run(1, pluck);
      float vs[kBlock][ins::kBuses];
      ins::voice_sends(ring, kBlock, vs);
      float o[kBlock][2], post[kBlock][2];
      sfx->Process(stems, kBlock, o, fx::control, post, nullptr, vs);
      for (int s = 0; s < kBlock; ++s) fxo.push_back(o[s][0]);
    }
    CHECK(amp::status(kV, 0) == amp::kPlaying);
    dry_on = rms(outl, 4800, 48000), dry_off = rms(outl, 60000, 96000);
    double fx_on = rms(fxo, 4800, 48000), fx_tail = rms(fxo, 50000, 72000);
    printf("AMP + SPACE: voice rms %.4f while the input plays, %.6f after; SPACE return %.4f, its tail %.4f\n", dry_on,
           dry_off, fx_on, fx_tail);
    CHECK(dry_on > 0.01 && dry_off < 1e-4);            // the voice follows its input
    CHECK(fx_on > 0.001 && fx_tail > 0.0005);          // SPACE hears the voice, and echoes on after the input stops
    // the AMP coloured it: not the dry input (the same pluck without the AMP)
    std::vector<float> amped(outl.begin(), outl.begin() + 48000);
    fresh();
    run(4000, pluck);
    double diff = 0;
    for (int i = 4800; i < 48000; ++i) diff += std::pow(outl[i] - amped[i], 2);
    printf("AMP vs dry: rms difference %.4f\n", std::sqrt(diff / 43200));
    CHECK(std::sqrt(diff / 43200) > 0.01);
    ins::set_send(kV, 0, 0);
    memset((void*)&fx::control, 0, sizeof fx::control);
  }

  // ---- INPT: L, R, L+R on TRK (mono), STR on POST (the pair) and on TRK (its mono sum)
  fresh();
  {
    auto two = [](long f, float* l, float* r) {
      *l = 0.2f * (float)std::sin(2 * M_PI * 300 * f / 48000.0);
      *r = 0.2f * (float)std::sin(2 * M_PI * 500 * f / 48000.0);
    };
    const float inp[4] = {0, 40, 80, 120};
    double t3[4][2], t5[4][2];
    for (int k = 0; k < 4; ++k) {
      knobs({inp[k]});
      outl.clear(), outr.clear();
      run(400, two);
      t3[k][0] = tone(outl, 2400, outl.size(), 300), t5[k][0] = tone(outl, 2400, outl.size(), 500);
    }
    printf("INPT on TRK: L %.3f/%.3f  R %.3f/%.3f  L+R %.3f/%.3f  STR %.3f/%.3f (300 Hz / 500 Hz)\n", t3[0][0], t5[0][0],
           t3[1][0], t5[1][0], t3[2][0], t5[2][0], t3[3][0], t5[3][0]);
    CHECK(t3[0][0] > 0.13 && t5[0][0] < 0.002);   // L: 0.2 x 0.7
    CHECK(t5[1][0] > 0.13 && t3[1][0] < 0.002);   // R
    CHECK(std::fabs(t3[2][0] - 0.07) < 0.003 && std::fabs(t5[2][0] - 0.07) < 0.003);   // L+R: (L + R) / 2
    CHECK(std::fabs(t3[3][0] - 0.07) < 0.003 && std::fabs(t5[3][0] - 0.07) < 0.003);   // STR on TRK: mono
    out::set_route(kV, out::kPost);
    for (int k = 0; k < 4; ++k) {
      knobs({inp[k]});
      outl.clear(), outr.clear();
      run(400, two);
      t3[k][0] = tone(outl, 2400, outl.size(), 300), t5[k][0] = tone(outl, 2400, outl.size(), 500);
      t3[k][1] = tone(outr, 2400, outr.size(), 300), t5[k][1] = tone(outr, 2400, outr.size(), 500);
    }
    printf("INPT on POST: STR L %.3f/%.3f R %.3f/%.3f; L+R L %.3f/%.3f R %.3f/%.3f\n", t3[3][0], t5[3][0], t3[3][1], t5[3][1],
           t3[2][0], t5[2][0], t3[2][1], t5[2][1]);
    CHECK(t3[3][0] > 0.13 && t5[3][0] < 0.002 && t5[3][1] > 0.13 && t3[3][1] < 0.002 && st[kV] == 1);   // the pair
    CHECK(std::fabs(t3[2][0] - t3[2][1]) < 1e-3 && std::fabs(t5[2][0] - t5[2][1]) < 1e-3);   // mono: L = R
    CHECK(t3[0][0] > 0.13 && t3[0][1] > 0.13 && t5[0][1] < 0.002);   // L on both sides
    // the stereo pair through the AMP: (L + R) / 2 into the capture, on both sides (insert.h's mono AMP)
    knobs({120});
    ins::set_type(kV, 0, ins::kAmp), ins::set_param(kV, 0, 0, amp::knob_for_model(0));
    for (int i = 0; i < 400; ++i) amp::step(), amp::service(kV);
    outl.clear(), outr.clear();
    run(800, two);
    double dlr = 0;
    for (size_t i = 4800; i < outl.size(); ++i) dlr += std::fabs(outl[i] - outr[i]);
    printf("STR on POST through AMP: |L - R| mean %.6f (the capture runs on the sum, both sides)\n", dlr / (outl.size() - 4800));
    CHECK(dlr / (outl.size() - 4800) < 1e-3 && rms(outl, 4800, outl.size()) > 0.005);
    ins::reset();
  }

  // ---- the gate: GATE -40 dBFS, HOLD ~100 ms: a -60 dBFS hiss stays out, a -20 dBFS tone passes, then the hold
  fresh();
  {
    knobs({80, 64, 1 + 40 * 126 / 60.0f, 36});   // -40 dB; HOLD 36 -> 80 ms
    char ht[16];
    livein::value_text(3, 36, ht, sizeof ht);
    uint32_t seed = 1;
    auto hiss = [&](float a) {
      seed = seed * 1664525u + 1013904223u;
      return a * ((float)(seed >> 8) / 8388608.0f - 1.0f);
    };
    // 0.5 s of hiss at -60 dBFS
    for (int b = 0; b < 2000; ++b) run(1, [&](long, float* l, float* r) { *l = *r = hiss(0.001f); });
    double closed = rms(outl, 12000, 24000);
    CHECK(!livein::stats(kV).gate_open);
    char sb[16];
    livein::status_short(kV, sb, sizeof sb);
    CHECK(!strcmp(sb, "LIVE GATED"));
    // a -20 dBFS tone for 0.25 s, then the hiss again
    size_t t0 = outl.size();
    run(1000, [](long f, float* l, float* r) { *l = *r = 0.1f * (float)std::sin(2 * M_PI * 440 * f / 48000.0); });
    double open = rms(outl, t0 + 2400, t0 + 12000);
    CHECK(livein::stats(kV).gate_open);
    size_t t1 = outl.size();
    run(4000, [&](long, float* l, float* r) { *l = *r = hiss(0.001f); });
    // after the tone: open for the hold (the hiss passes), then closed
    double held = rms(outl, t1 + 240, t1 + 2400), shut = rms(outl, t1 + 24000, t1 + 48000);
    int last_open = 0;
    for (size_t i = t1; i < outl.size(); ++i)
      if (std::fabs(outl[i]) > 0.0005f * 0.7f * 0.5f) last_open = (int)(i - t1);
    printf("gate (-40 dBFS, HOLD %s): hiss alone %.6f, the tone %.4f, the hiss in the hold %.6f, later %.6f; "
           "the hiss heard to %.0f ms after the tone\n", ht, closed, open, held, shut, last_open / 48.0);
    CHECK(closed < 1e-5 && open > 0.04 && held > 0.0002 && shut < 1e-5);
    CHECK(last_open > 48 * 70 && last_open < 48 * 160);   // the 80 ms hold, then its close (25 ms)
    CHECK(livein::status_short(kV, sb, sizeof sb) <= 10);
  }

  // ---- MON TRIG: silent until a trig; the sequencer's note (E0's mono path) opens ATK / DEC; SUS + its note-off
  fresh();
  {
    knobs({80, 64, 0, 0, 100, 0, 50, 0, 50});   // TRIG, ATK 0.5 ms, DEC 50 -> ~76 ms, SUS 0
    auto drone = [](long f, float* l, float* r) { *l = *r = 0.2f * (float)std::sin(2 * M_PI * 330 * f / 48000.0); };
    run(400, drone);
    CHECK(peak(outl, 0, outl.size()) < 1e-6);   // no trig: nothing
    size_t t0 = outl.size();
    seq::PolyCfg pc = {};
    pc.voices = 4;   // a poly config: LIVE IN takes it as one voice (the mono path), no instances
    CHECK(inst::seq_on(kV, 60, 100, pc) && inst::seq_mono(kV) && !inst::active(kV));
    run(1200, drone);
    double first = peak(outl, t0, t0 + 480), mid = peak(outl, t0 + 1440, t0 + 1920), late = peak(outl, t0 + 12000, t0 + 14400);
    // a Rytm trig the same way (0x04)
    size_t t1 = outl.size();
    auto m = report(0x04, kPlaits | kV, 60, 100, {80, 64, 0, 0, 100, 0, 50, 0, 50});
    voice_report(0x04, m.data(), (int)m.size());
    run(400, drone);
    double rytm = peak(outl, t1, t1 + 480);
    printf("MON TRIG: before %.4f, the seq note's first 10 ms %.4f, 30 ms %.4f, 250 ms %.6f; a Rytm trig %.4f\n",
           peak(outl, 0, t0), first, mid, late, rytm);
    CHECK(first > 0.12 && mid < first * 0.5 && mid > 0.001 && late < 1e-4 && rytm > 0.12);
    // SUS 64: held at half until the note-off, then REL
    knobs({80, 64, 0, 0, 100, 0, 30, 64, 40});
    CHECK(inst::seq_on(kV, 62, 100, pc));
    size_t t2 = outl.size();
    run(2000, drone);
    double sus = peak(outl, t2 + 12000, t2 + 24000);
    inst::seq_off(kV, 62);   // the sequencer's note-off
    size_t t3 = outl.size();
    run(2000, drone);
    double rel = peak(outl, t3 + 12000, t3 + 24000);
    printf("MON TRIG SUS 64: held %.4f (0.14 x 64/127 = %.4f), 0.25 s after the note-off %.6f\n", sus, 0.14 * 64 / 127, rel);
    CHECK(std::fabs(sus - 0.14 * 64 / 127) < 0.01 && rel < 0.002);
  }

  // ---- underruns: no stream -> silence and the counter; a half-fed stream; then the stream back
  fresh();
  {
    uint32_t u0 = livein::stats(kV).underruns;
    for (int b = 0; b < 2000; ++b) block(nullptr, nullptr, 0);   // 0.5 s: nothing comes
    CHECK(peak(outl, 0, outl.size()) == 0 && livein::stats(kV).underruns - u0 >= 1990);
    char sb[16];
    livein::status_short(kV, sb, sizeof sb);
    CHECK(!strcmp(sb, "LIVE NO IN") && !livein::stats(kV).stream);
    // 6 frames a block: every block short, no NaN, the frames that came still play
    uint32_t u1 = livein::stats(kV).underruns;
    float half[kBlock];
    for (int s = 0; s < kBlock; ++s) half[s] = 0.2f;
    for (int b = 0; b < 400; ++b) block(half, half, 6);
    bool finite = true;
    for (float x : outl) finite = finite && std::isfinite(x);
    CHECK(finite && livein::stats(kV).underruns - u1 >= 399 && peak(outl, outl.size() - 4800, outl.size()) > 0.1);
    // whole blocks again: no more underruns
    run(400, [](long f, float* l, float* r) { *l = *r = 0.1f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    uint32_t u2 = livein::stats(kV).underruns;
    run(400, [](long f, float* l, float* r) { *l = *r = 0.1f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    printf("underruns: 2000 empty blocks -> %u, half blocks -> %u more, whole blocks -> %u more (backlog %u)\n",
           (unsigned)(u1 - u0), (unsigned)(u2 - u1), (unsigned)(livein::stats(kV).underruns - u2),
           (unsigned)livein::stats(kV).backlog);
    CHECK(livein::stats(kV).underruns == u2);
    livein::status_short(kV, sb, sizeof sb);
    CHECK(!strncmp(sb, "LIVE -", 6));
    // a backlog (the render stalled: 40 blocks' input at once) is cut back within two seconds (the mean shown a second later)
    float big[kBlock * 40];
    for (int s = 0; s < kBlock * 40; ++s) big[s] = 0.1f * (float)std::sin(2 * M_PI * 220 * s / 48000.0);
    livein::in_frames(big, big, kBlock * 40);
    uint32_t k0 = livein::stats(kV).skips;
    run(12000, [](long f, float* l, float* r) { *l = *r = 0.1f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    printf("a 480-frame backlog: skips %u, backlog now %u frames\n", (unsigned)(livein::stats(kV).skips - k0),
           (unsigned)livein::stats(kV).backlog);
    CHECK(livein::stats(kV).skips - k0 == 1 && livein::stats(kV).backlog <= 1);
  }

  // ---- the feedback guard: the voice's output back into AUDIO IN (D frames late, x 2: a howl); a swell left alone
  fresh();
  {
    const int D = 420;
    std::vector<float> loop(4 * 48000 * 10, 0.0f);
    long f0 = 0;
    uint32_t seed = 7;
    uint32_t trips0 = livein::stats(kV).trips, probes0 = livein::stats(kV).probes, lim0 = livein::stats(kV).limited;
    int muted_at = -1;
    double pk_max = 0;
    for (int b = 0; b < 4000 * 6; ++b) {   // 6 s
      float l[kBlock];
      for (int s = 0; s < kBlock; ++s) {
        long f = f0 + s;
        seed = seed * 1664525u + 1013904223u;
        float noise = 0.002f * ((float)(seed >> 8) / 8388608.0f - 1.0f);
        l[s] = noise + (f >= D ? 2.0f * loop[(size_t)(f - D)] / 0.7f : 0.0f);
      }
      block(l, l);
      for (int s = 0; s < kBlock; ++s) loop[(size_t)(f0 + s)] = outl[outl.size() - kBlock + s];
      f0 += kBlock;
      if (muted_at < 0 && livein::stats(kV).muted) muted_at = (int)f0;
    }
    pk_max = peak(outl, 0, outl.size());
    livein::step();   // the log line
    printf("feedback (loop x 2, %d frames): probes %u, trips %u, muted at %.2f s, peak %.3f (the limiter's ceiling: 0.891 of the voice's return), limited blocks %u\n",
           D, (unsigned)(livein::stats(kV).probes - probes0), (unsigned)(livein::stats(kV).trips - trips0),
           muted_at / 48000.0, pk_max, (unsigned)(livein::stats(kV).limited - lim0));
    CHECK(livein::stats(kV).trips - trips0 >= 1 && muted_at > 0 && muted_at < 48000 * 3);
    CHECK(pk_max <= 0.8915);   // never past the limiter (-1 dBFS of the voice's return)
    // while muted: silence (after its 10 ms ramp)
    // a swell (not a loop): a tone rising 1 dB every 50 ms from -40 dBFS to -3 dBFS, then held 2 s
    fresh();
    uint32_t t1 = livein::stats(kV).trips, p1 = livein::stats(kV).probes;
    run(4000 * 5, [](long f, float* l, float* r) {
      double db = -40.0 + std::min(37.0, f / 2400.0);
      *l = *r = (float)(std::pow(10.0, db / 20.0) * std::sin(2 * M_PI * 196 * f / 48000.0));
    });
    printf("a swell -40 -> -3 dBFS (no loop): probes %u, trips %u\n", (unsigned)(livein::stats(kV).probes - p1),
           (unsigned)(livein::stats(kV).trips - t1));
    CHECK(livein::stats(kV).trips == t1);
    // a strummed rhythm near full scale (dips and rises of its own, no loop): no trip
    fresh();
    uint32_t t2 = livein::stats(kV).trips, p2 = livein::stats(kV).probes;
    run(4000 * 6, [](long f, float* l, float* r) {
      float t = (float)(f % 6000) / 48000.0f, ph = 147.0f * t;
      *l = *r = 0.9f * std::exp(-9.0f * t) * (2.0f * (ph - std::floor(ph)) - 1.0f);
    });
    printf("a strummed rhythm (8 a second, -1 dBFS, no loop): probes %u, trips %u\n",
           (unsigned)(livein::stats(kV).probes - p2), (unsigned)(livein::stats(kV).trips - t2));
    CHECK(livein::stats(kV).trips == t2);
  }

  // ---- the round-trip measure: the voice's clicks back on AUDIO IN D frames later (half level, some hiss)
  fresh();
  {
    const int D = 377;
    std::vector<float> loop(48000 * 8, 0.0f);
    long f0 = 0;
    uint32_t seed = 3;
    std::string a = cmd("livein=measure:2");
    CHECK(a.find("measuring voice 2") != std::string::npos);
    for (int b = 0; b < 4000 * 4 && livein::stats(kV).meas_state == 1; ++b) {
      float l[kBlock];
      for (int s = 0; s < kBlock; ++s) {
        long f = f0 + s;
        seed = seed * 1664525u + 1013904223u;
        l[s] = 0.0003f * ((float)(seed >> 8) / 8388608.0f - 1.0f) + (f >= D ? 0.5f * loop[(size_t)(f - D)] / 0.7f : 0.0f);
      }
      block(l, l);
      for (int s = 0; s < kBlock; ++s) loop[(size_t)(f0 + s)] = outl[outl.size() - kBlock + s];
      f0 += kBlock;
    }
    livein::step();
    livein::Stats s = livein::stats(kV);
    bool m = false;
    int ms10 = livein::latency_ms10(kV, &m);
    printf("measure: state %u, %d of 5 clicks, %d frames (%d.%d ms), the loop %d frames\n", s.meas_state, s.meas_found,
           s.measured, ms10 / 10, ms10 % 10, D);
    CHECK(s.meas_state == 2 && s.meas_found == 5 && std::abs(s.measured - D) <= 2 && m);
    CHECK(cmd("livein").find("measured") != std::string::npos);
    // nothing comes back: the measure fails (and says so)
    fresh();
    cmd("livein=measure:2");
    for (int b = 0; b < 4000 * 4 && livein::stats(kV).meas_state == 1; ++b) run(1, [](long, float* l, float* r) { *l = *r = 0; });
    CHECK(livein::stats(kV).meas_state == 3);
    CHECK(cmd("livein=measure:5").find("does not play") != std::string::npos);
  }

  // ---- the estimate without a measure: the device's loop + the FIFO + the backlog
  {
    livein::reset();
    cmd("livein=voice:2");
    livein::set_fifo(96);
    fresh();
    run(4000 * 2, [](long f, float* l, float* r) { *l = *r = 0.1f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    bool m = true;
    int ms10 = livein::latency_ms10(kV, &m);
    printf("estimate: %d.%d ms (loop 289 + fifo 96 + backlog %u frames)\n", ms10 / 10, ms10 % 10, (unsigned)livein::stats(kV).backlog);
    CHECK(!m && ms10 == (int)((289 + 96 + livein::stats(kV).backlog) * 10000 + 24000) / 48000);
    std::string a = cmd("livein");
    CHECK(a.find("est.: loop 289 + fifo 96") != std::string::npos);
  }

  // ---- the render core: no allocation, no card time; a stereo route and an AMP on the voice, the guard running
  fresh();
  {
    ins::set_type(kV, 0, ins::kDrive);
    out::set_route(kV, out::kPre);
    knobs({120, 70, 20, 30, 0});
    run(400, [](long f, float* l, float* r) { *l = *r = 0.2f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    host_card_model(true);
    host_card_core(1);
    uint64_t c0 = host_card_us(1);
    long n0 = g_news;
    outl.reserve(outl.size() + 4000 * kBlock), outr.reserve(outr.size() + 4000 * kBlock);
    run(4000, [](long f, float* l, float* r) { *l = *r = 0.2f * (float)std::sin(2 * M_PI * 220 * f / 48000.0); });
    long news = g_news - n0;
    uint64_t cu = host_card_us(1) - c0;
    host_card_core(3);
    host_card_model(false);
    printf("render core: %ld allocations, %llu us of card time over 4000 blocks\n", news, (unsigned long long)cu);
    CHECK(news == 0 && cu == 0);
    ins::reset();
    out::set_route(kV, out::kTrk);
  }

  // ---- the CPU: one LIVE IN voice (gate on, MON TRIG, the guard), 1 s of audio in 12-frame blocks; its machine alone
  {
    fresh();
    knobs({80, 70, 20, 30, 100, 10, 60, 60, 60});
    voice_play(kV, 60, 100);
    float in[kBlock];
    using clk = std::chrono::steady_clock;
    double best = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
      auto t0 = clk::now();
      for (int b = 0; b < 4000; ++b) {
        for (int s = 0; s < kBlock; ++s) in[s] = 0.2f * (float)std::sin(0.03 * (b * kBlock + s));
        livein::in_frames(in, in, kBlock);
        render_voices(ring, kBlock, 0, kV, kV + 1, mach, ring_r, st);
      }
      double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
      best = std::min(best, us);
    }
    double pct = best / 1e6 * 100.0;
    printf("CPU: LIVE IN %.3f %% of a Mac core per voice (1 s of audio in %.0f us); Pi 4 est. x 4.4: %.2f %%\n", pct, best,
           pct * 4.4);
    CHECK(pct < 1.0);
  }

  livein::reset();
  ins::reset();
  std::string rm = "rm -rf " + card;
  CHECK(system(rm.c_str()) == 0);
  printf("test_livein: ALL PASS\n");
  return 0;
}
