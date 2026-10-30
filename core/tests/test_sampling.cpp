// core/tests/test_sampling.cpp - SAMPLING and the AUTO-SAMPLER (core/samp, docs/SAMPLING.md) and the card queue
// (core/msamp/cardq.h): the analysis (trim, transients, grid, the loop finder, the WAV header); the card queue's order
// and cancel; a take from IN L/R on a threshold with its pre-roll, trimmed, normalised, sliced, written, its
// instruments listed and played by MULTISAMPLE; a take of 2 bars at the Rytm's clock from its next bar; a Sidecar
// voice's take; the auto-sampler on a 6-OP FM patch and a JUNO patch (offline, faster than real time: zones, layers,
// loops, one gain) and on a Rytm track (a fake Rytm answering the notes 6 ms late: the latency measured, the takes cut).
// SAMP_DEMO=out.wav: the JUNO instrument it made plays chords into out.wav (the demo).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "lib/pools.h"
#include "engine.h"
#include "host_link.h"
#include "msamp/cardq.h"
#include "msamp/msamp.h"
#include "samp/autosamp.h"
#include "samp/dsp.h"
#include "samp/sampler.h"
#include "voices.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 1000000;
static void advance(uint32_t us) {
  g_us += us;
  host_fake_time(true, g_us);
}
static bool exists(const std::string& p) {
  struct stat s;
  return stat((g_card + p).c_str(), &s) == 0;
}
static long fsize(const std::string& p) {
  struct stat s;
  return stat((g_card + p).c_str(), &s) == 0 ? (long)s.st_size : -1;
}
static std::string slurp(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "";
  std::string t;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n);
  fclose(f);
  return t;
}
static int count(const std::string& t, const char* s) {
  int n = 0;
  for (size_t at = t.find(s); at != std::string::npos; at = t.find(s, at + 1)) ++n;
  return n;
}
static uint32_t rd32(const std::string& t, size_t at) {
  return (uint8_t)t[at] | (uint8_t)t[at + 1] << 8 | (uint8_t)t[at + 2] << 16 | (uint32_t)(uint8_t)t[at + 3] << 24;
}

// ---- the analysis
static void test_dsp() {
  using namespace samp;
  const uint32_t R = kRate;
  uint32_t rnd = 1;
  auto noise = [&]() { rnd = rnd * 1664525u + 1013904223u; return ((rnd >> 8) / 8388608.0f - 1.0f); };
  {  // trim: 0.1 s of a -80 dB floor, 0.5 s of a tone, 0.2 s of floor
    std::vector<float> x(R * 8 / 10);
    for (uint32_t i = 0; i < x.size(); ++i) x[i] = 1e-4f * noise();
    for (uint32_t i = R / 10; i < R * 6 / 10; ++i) x[i] += 0.5f * sinf(2 * (float)M_PI * 440 * i / R);
    Trim t;
    CHECK(trim(x.data(), 1, (uint32_t)x.size(), R / 10, &t));
    CHECK(t.start + R / 500 >= R / 10 - 2 && t.start <= R / 10);
    CHECK(t.end >= R * 6 / 10 && t.end <= R * 6 / 10 + R / 100 + 2);
    std::vector<float> q(R, 1e-5f);
    CHECK(!trim(q.data(), 1, R, R / 10, &t));   // nothing above the floor
  }
  {  // transients: 8 hits 0.25 s apart (decaying noise bursts over a quiet pad), each found within 3 ms
    std::vector<float> x(R * 2);
    for (uint32_t i = 0; i < x.size(); ++i) x[i] = 0.01f * sinf(2 * (float)M_PI * 110 * i / R);
    for (int h = 0; h < 8; ++h)
      for (uint32_t i = 0; i < R / 8; ++i) x[h * R / 4 + 300 + i] += (h % 2 ? 0.4f : 0.8f) * noise() * expf(-(float)i / (R / 60));
    uint32_t s[32];
    int n = transients(x.data(), 1, (uint32_t)x.size(), s, 32);
    CHECK(n == 8 && s[0] == 0);   // the first slice at 0 (its hit 6 ms in: closer than the minimum gap), then 7 hits
    for (int h = 1; h < 8; ++h) CHECK(std::abs((int)s[h] - (int)(h * R / 4 + 300)) < (int)R * 3 / 1000);
    n = grid(x.data(), 1, (uint32_t)x.size(), 16, s);
    CHECK(n == 16 && s[0] == 0);
    for (int k = 1; k < 16; ++k) CHECK(std::abs((int)s[k] - (int)(R * 2 * k / 16)) <= (int)R / 1000 + 1);
  }
  {  // the loop finder: a sine of 220.5 Hz (217.69 frames a period) and a two-partial tone: loops of whole periods
    for (int kind = 0; kind < 2; ++kind) {
      std::vector<float> x(R * 2);
      double f = 220.5;
      for (uint32_t i = 0; i < x.size(); ++i)
        x[i] = 0.5f * sinf(2 * (float)M_PI * f * i / R) + (kind ? 0.2f * sinf(2 * (float)M_PI * 3 * f * i / R + 0.3f) : 0);
      CHECK(sustained(x.data(), R / 4, R * 7 / 4));
      Loop L = find_loop(x.data(), R / 4, R * 7 / 4);
      double per = R / f;
      double k = (L.e - L.s) / per;
      printf("test_sampling: loop (%s): %u..%u, %.2f periods (period %.2f, found %.2f), score %.5f\n", kind ? "2 partials" : "sine",
             L.s, L.e, k, per, L.period, L.score);
      CHECK(L.ok && std::fabs(L.period - per) < 0.5 && std::fabs(k - std::round(k)) < 0.03 && L.e - L.s >= R / 10 &&
            L.score < 0.01f);
    }
    std::vector<float> d(R * 2);   // decaying: not sustained
    for (uint32_t i = 0; i < d.size(); ++i) d[i] = 0.5f * expf(-(float)i / (R / 4)) * sinf(2 * (float)M_PI * 300 * i / R);
    CHECK(!sustained(d.data(), R / 4, R * 7 / 4));
  }
  {  // the WAV header: 24-bit mono, a smpl loop
    uint8_t h[kWavHeaderMax];
    int n = wav_header(h, 1, 24, 48000, 1001, true, 100, 900, 64);
    std::string t((char*)h, n);
    CHECK(n == 44 + 68 && t.substr(0, 4) == "RIFF" && t.substr(8, 4) == "WAVE" && t.substr(36, 4) == "smpl");
    CHECK(rd32(t, 36 + 8 + 12) == 64 && rd32(t, 36 + 8 + 28) == 1 && rd32(t, 36 + 8 + 44) == 100 && rd32(t, 36 + 8 + 48) == 900);
    CHECK(t.substr(n - 8, 4) == "data" && rd32(t, n - 4) == 3003 && rd32(t, 4) == (uint32_t)(n - 8 + 3003 + 1));
    char nn[8];
    note_name(60, nn); CHECK(!strcmp(nn, "C4"));
    note_name(61, nn); CHECK(!strcmp(nn, "C#4"));
    note_name(0, nn); CHECK(!strcmp(nn, "C-1"));
  }
  printf("test_sampling: analysis PASS\n");
}

// ---- the card queue
static void test_cardq() {
  cardq::reset();
  cardq::set_worker(true);
  File f;
  cardq::Req a, b, c;
  for (cardq::Req* r : {&a, &b, &c}) {
    memset(r, 0, sizeof *r);
    r->kind = cardq::kOpenReq;
    r->f = &f;
    strcpy(r->path, "/nothing-here.wav");
  }
  CHECK(cardq::post(&a) && cardq::post(&b) && cardq::post(&c) && cardq::pending() == 3 && cardq::busy());
  CHECK(!cardq::idle(&a) && cardq::cancel(&b) && cardq::idle(&b));
  CHECK(cardq::service() == &a && a.state == cardq::kDone && a.got == 0);   // (no such file: not opened)
  CHECK(cardq::service() == &c && cardq::service() == nullptr && cardq::pending() == 0);
  CHECK(!cardq::cancel(&c));   // done: too late
  cardq::set_need(40, 150);    // a stream 40 ms ahead, its low mark 150: the recorder and uploads hold off
  CHECK(cardq::busy());
  cardq::set_need(400, 150);
  CHECK(!cardq::busy());
  CHECK(cardq::post(&b) && cardq::take_back(&b) && cardq::idle(&b) && cardq::stats().taken_back == 1);   // the card core held up
  CHECK(cardq::service() == nullptr);   // (its entry passed over)
  cardq::reset();   // (no worker: post runs at once)
  CHECK(cardq::post(&a) && cardq::idle(&a) && cardq::pending() == 0);
  printf("test_sampling: card queue PASS\n");
}

// ---- the engine around the sampler: blocks of 12 frames (250 us)
static int16_t g_ring[48000 * 8][kVoices];
static int g_w = 0;
static void block(bool render = true) {
  if (render) {
    uint64_t mach[kMachines] = {0};
    render_voices(g_ring, 48000 * 8, g_w, 0, kVoices, mach);
    g_w = (g_w + kBlock) % (48000 * 8);
  }
  for (int i = 0; i < 64 && msamp::step(); ++i) {}
  for (int i = 0; i < 8 && pools::step(); ++i) {}   // the pool lists (a take's row, an instrument's .sfz read)
  samp::tick();
  samp::asamp_tick();
  for (int i = 0; i < 4 && samp::poll(); ++i) {}
  for (int i = 0; i < 4 && samp::asamp_poll(); ++i) {}
  advance(250);
}
static void in_frames(const std::vector<float>& l, const std::vector<float>& r, uint32_t a, uint32_t n, int ch0 = 10) {
  int32_t f[kBlock * 12];
  for (uint32_t i = 0; i < n; i += kBlock) {
    int m = n - i < (uint32_t)kBlock ? (int)(n - i) : kBlock;
    memset(f, 0, sizeof f);
    for (int k = 0; k < m; ++k) {
      f[k * 12 + ch0] = (int32_t)(l[a + i + k] * 2147483000.0f);
      f[k * 12 + ch0 + 1] = (int32_t)(r[a + i + k] * 2147483000.0f);
    }
    samp::in_block(f, m, 12);
    block(false);
  }
}
static int entry(const char* name) {
  for (int k = 0; k < msamp::kList; ++k) {
    const char* p = msamp::list_path(k);
    if (*p && strstr(p, name)) return k;
  }
  return -1;
}
static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) m.push_back(note), m.push_back(vel);
  p.resize(16, 0);
  for (float x : p) {
    int i = (int)x;
    m.push_back(i), m.push_back((int)((x - i) * 128) & 0x7f);
  }
  m.push_back(0xf7);
  return m;
}
static void send(const std::vector<uint8_t>& m) { host_sysex(m.data(), (int)m.size()); }
static constexpr int kFmB = 0x48;   // a 6-OP FM voice byte (msamp=voice:V makes it MULTISAMPLE)
static std::vector<float> ms(int inst) { return {(float)inst, 127, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0}; }
static double rms_ring(int voice, int a, int n) {
  double s = 0;
  for (int i = 0; i < n; ++i) {
    double v = g_ring[(a + i) % (48000 * 8)][voice];
    s += v * v;
  }
  return std::sqrt(s / n);
}
// MULTISAMPLE plays list entry e on voice vi: its first trig's output (0.25 s)
static double play_entry(int vi, int e, int note) {
  char o[200];
  msamp::command(("msamp=voice:" + std::to_string(vi)).c_str(), o, sizeof o);
  send(report(0x03, kFmB | vi, -1, 0, ms(e)));
  for (int i = 0; i < 200; ++i) block();
  int w0 = g_w;
  send(report(0x04, kFmB | vi, note, 120, ms(e)));
  for (int i = 0; i < 1000; ++i) block();
  double r = rms_ring(vi, w0, 12000);
  send({0xf0, 0x7d, 0x52, 0x54, 0x11, (uint8_t)vi, 0xf7});
  for (int i = 0; i < 4000; ++i) block();
  msamp::silence(vi);
  msamp::command(("msamp=voice:" + std::to_string(vi) + ":off").c_str(), o, sizeof o);
  return r;
}

static void wait_written() {
  for (int i = 0; i < 400000; ++i) {
    samp::Status s;
    samp::status(&s);
    if (s.state == samp::kDone || s.state == samp::kFailed) break;
    block(false);
  }
  for (int i = 0; i < 400; ++i) block(false);   // the rescan, the list
}

// IN L/R, threshold -30 dB, 100 ms pre-roll: a 2 s loop of 8 hits after 0.5 s of floor; trimmed, normalised, 8
// slices, the WAV and three .sfz written, MULTISAMPLE plays the single zone and the slices
static void test_sampler_threshold() {
  using namespace samp;
  uint32_t R = kRate, rnd = 7;
  auto noise = [&]() { rnd = rnd * 1664525u + 1013904223u; return ((rnd >> 8) / 8388608.0f - 1.0f); };
  std::vector<float> l(R * 4), r(R * 4);
  for (uint32_t i = 0; i < l.size(); ++i) l[i] = r[i] = 3e-4f * noise();
  for (int h = 0; h < 8; ++h)
    for (uint32_t i = 0; i < R / 6; ++i) {
      float v = (h % 2 ? 0.25f : 0.5f) * noise() * expf(-(float)i / (R / 50)) + 0.3f * sinf(2 * (float)M_PI * 60 * i / R) * expf(-(float)i / (R / 20));
      l[R / 2 + h * R / 4 + i] += v, r[R / 2 + h * R / 4 + i] += 0.8f * v;
    }
  char o[400];
  CHECK(command("samp=src:in", o, sizeof o));
  CHECK(command("samp=start:thr:30", o, sizeof o) && config().start == kThreshold && config().thr_db == -30);
  command("samp=pre:100", o, sizeof o);
  command("samp=len:0", o, sizeof o);
  command("samp=slices:trans", o, sizeof o);
  command("samp=arm", o, sizeof o);
  printf("test_sampling: %s\n", o);
  Status s;
  status(&s);
  CHECK(s.state == kArmed && wants_in() && !wants_voices());
  in_frames(l, r, 0, R * 3);   // the floor, the hits, the tail
  status(&s);
  CHECK(s.state == kRecording && s.frames > R * 2);
  command("samp=stop", o, sizeof o);
  in_frames(l, r, R * 3, 240);
  wait_written();
  status(&s);
  printf("test_sampling: %s\n", (command("samp", o, sizeof o), o));
  CHECK(s.state == kDone && !strcmp(s.wav, "/Samples/Sidecar/SMP_001.wav") && s.slices == 8);
  uint32_t n;
  int ch;
  const float* x = take_audio(&n, &ch);
  CHECK(ch == 2 && std::fabs(peak(x, 2, n) * db_to_lin(s.gain_db) - 0.891f) < 0.01f);   // (the gain is the writer's)
  // trimmed: from 2 ms before the first hit to 10 ms after the last frame over the floor
  CHECK(n > R * 2 - R / 4 && n < R * 2 + R / 20);
  const uint32_t* sl;
  CHECK(take_slices(&sl) == 8);
  for (int h = 1; h < 8; ++h) CHECK(std::abs((int)(sl[h] - sl[0]) - (int)(h * R / 4)) < (int)R * 3 / 1000);
  long sz = fsize("/Samples/Sidecar/SMP_001.wav");
  CHECK(sz == (long)(44 + n * 6));
  std::string sfz = slurp("/Samples/Sidecar/SMP_001.sfz");
  CHECK(count(sfz, "<region>") == 8 && sfz.find("sample=SMP_001.wav key=36") != std::string::npos);
  CHECK(exists("/MULTISAMPLE/Sidecar/SMP_001.sfz") && exists("/MULTISAMPLE/Sidecar/SMP_001-slices.sfz"));
  // the pools design 3.7: the take into the SAMPLES pool (the first free row of the bank last shown: A001), its slice
  // map into the INSTRUMENTS pool (played: key 37, the second hit); the one-zone .sfz stays on the card only
  for (int i = 0; i < 400; ++i) block();
  status(&s);
  CHECK(!strcmp(s.row, "A001") && pools::at(pools::kSamples, 1).type == pools::kSample &&
        !strcmp(pools::at(pools::kSamples, 1).path, "/Samples/Sidecar/SMP_001.wav"));
  int e1 = entry("Sidecar/SMP_001.sfz"), e2 = entry("SMP_001-slices.sfz");
  CHECK(e1 < 0 && e2 == 1);
  double b = play_entry(0, e2, 37);
  printf("test_sampling: the take's slices on MULTISAMPLE: rms %.0f (slice 2)\n", b);
  CHECK(b > 200);
  printf("test_sampling: sampler (threshold, slices) PASS\n");
}

// MAIN, 2 bars at the Rytm's clock (125 bpm: 24 ticks a beat), from its next bar: 2 x 4 x 0.48 s = 3.84 s + the pre-roll
static void test_sampler_bars() {
  using namespace samp;
  uint32_t R = kRate;
  std::vector<float> l(R * 8), r(R * 8);
  for (uint32_t i = 0; i < l.size(); ++i) l[i] = r[i] = 0.2f * sinf(2 * (float)M_PI * 220 * i / R);
  char o[400];
  command("samp=src:main", o, sizeof o);
  command("samp=start:bar", o, sizeof o);
  command("samp=len:2", o, sizeof o);
  command("samp=pre:0", o, sizeof o);
  command("samp=slices:grid:8", o, sizeof o);
  command("samp=name:loop", o, sizeof o);
  CHECK(command("samp=arm", o, sizeof o));
  // the Rytm plays: FA, then F8 every 20 ms (125 bpm); IN frames meanwhile (MAIN = IN 0/1)
  transport(0xFA);
  uint32_t pos = 0, tick_us = 20000, since = 0;
  Status s;
  int started_at_tick = -1, ticks = 0;
  for (int b = 0; b < 4 * 2000 * 6 && pos + 12 < l.size(); ++b) {
    int32_t f[12 * 12] = {0};
    for (int k = 0; k < 12; ++k) f[k * 12] = (int32_t)(l[pos + k] * 2147483000.0f), f[k * 12 + 1] = (int32_t)(r[pos + k] * 2147483000.0f);
    samp::in_block(f, 12, 12);
    pos += 12;
    since += 250;
    if (since >= tick_us) {
      since -= tick_us;
      transport(0xF8);
      ++ticks;
    }
    status(&s);
    if (started_at_tick < 0 && s.state == kRecording) started_at_tick = ticks;
    if (s.state == kCaptured || s.state == kWriting || s.state == kDone) break;
    block(false);
  }
  wait_written();
  status(&s);
  printf("test_sampling: %s\n", (command("samp", o, sizeof o), o));
  uint32_t n;
  int ch;
  take_audio(&n, &ch);
  // PLAY starts bar 1: the take starts with it; 2 bars at the measured 125 bpm (the clock's first tick is 20 ms in)
  CHECK(s.state == kDone && started_at_tick == 0 && std::fabs(s.bpm - 125) < 0.5f && s.slices == 8);
  CHECK(std::abs((int)n - (int)(R * 384 / 100)) < 48);
  CHECK(!strcmp(s.wav, "/Samples/Sidecar/loop.wav") && exists("/MULTISAMPLE/Sidecar/loop-slices.sfz"));
  command("samp=name:auto", o, sizeof o);
  printf("test_sampling: sampler (bars at the clock) PASS\n");
}

// A Sidecar voice as rendered (voice 1: a JUNO note), free, started by hand
static void test_sampler_voice() {
  using namespace samp;
  char o[400];
  command("samp=src:voice:2", o, sizeof o);
  command("samp=start:now", o, sizeof o);
  command("samp=len:0", o, sizeof o);
  command("samp=pre:0", o, sizeof o);
  command("samp=slices:off", o, sizeof o);
  CHECK(command("samp=arm", o, sizeof o) && wants_voices() && !wants_in());
  std::vector<float> juno = {0, 64, 64, 64, 64, 64, 64, 127, 0, 0, 0, 0, 0, 0, 0, 0};
  send(report(0x04, kJuno | 1, 60, 110, juno));
  for (int b = 0; b < 4000; ++b) {   // 1 s
    int w = g_w;
    block();
    int16_t v[kBlock][8];
    for (int k = 0; k < kBlock; ++k) memcpy(v[k], g_ring[(w + k) % (48000 * 8)], sizeof v[k]);
    voice_block(v, kBlock);
  }
  command("samp=stop", o, sizeof o);
  int16_t z[kBlock][8] = {};
  voice_block(z, kBlock);
  wait_written();
  Status s;
  status(&s);
  uint32_t n;
  int ch;
  const float* x = take_audio(&n, &ch);
  CHECK(s.state == kDone && ch == 1 && n > kRate / 2 && rms(x, 1, 0, n) > 0.005f && !strcmp(s.wav, "/Samples/Sidecar/SMP_002.wav"));
  printf("test_sampling: sampler (a Sidecar voice) PASS\n");
}

static samp::AStatus run_asamp(const char* const* cmds) {
  char o[600];
  auto w0 = std::chrono::steady_clock::now();
  for (const char* const* c = cmds; *c; ++c) CHECK(samp::asamp_command(*c, o, sizeof o));
  printf("test_sampling: %s\n", o);
  samp::AStatus s;
  for (int i = 0; i < 4000000; ++i) {
    samp::asamp_status(&s);
    if (s.state == samp::kADone || s.state == samp::kAFailed || s.state == samp::kACancelled) break;
    block(false);
  }
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
  double audio = 0;
  for (int i = 0; i < s.total; ++i) audio += (samp::asamp_plan().hold_ms + samp::asamp_plan().tail_ms) / 1000.0;
  for (int i = 0; i < 400; ++i) block(false);
  samp::asamp_command("asamp", o, sizeof o);
  printf("test_sampling: %s\n  (the Mac: %.2f s for at most %.0f s of takes, rendered, trimmed, looped and written)\n", o, wall, audio);
  return s;
}

// The auto-sampler on 6-OP FM (ROM 1, offline): 5 notes x 2 velocities: 10 samples, the zones split halfway, the
// layers split between 64 and 120, one gain; MULTISAMPLE plays it
static void test_asamp_fm() {
  const char* cmds[] = {"asamp=src:fm:0", "asamp=notes:c2:c6:12", "asamp=vel:64,120", "asamp=rr:1", "asamp=hold:600",
                        "asamp=tail:1500", "asamp=loop:on", "asamp=name:FMtest", "asamp=start", nullptr};
  samp::AStatus s = run_asamp(cmds);
  CHECK(s.state == samp::kADone && s.total == 10 && s.written == 10 && s.speed > 1);
  std::string t = slurp("/MULTISAMPLE/FMtest/FMtest.sfz");
  CHECK(count(t, "<region>") == 10 && t.find("default_path=samples/") != std::string::npos);
  CHECK(t.find("sample=FMtest_C2_v64_rr1.wav lokey=0 hikey=42 pitch_keycenter=36 lovel=1 hivel=92") != std::string::npos);
  CHECK(t.find("sample=FMtest_C4_v120_rr1.wav lokey=55 hikey=66 pitch_keycenter=60 lovel=93 hivel=127") != std::string::npos);
  CHECK(t.find("hikey=127 pitch_keycenter=84") != std::string::npos);
  CHECK(fsize("/MULTISAMPLE/FMtest/samples/FMtest_C4_v120_rr1.wav") > 44 + 3 * 48000 / 2);
  int e = entry("FMtest.sfz");
  CHECK(e >= 0);
  double a = play_entry(3, e, 62);
  printf("test_sampling: FM instrument on MULTISAMPLE: rms %.0f; offline x%.0f real time\n", a, s.speed);
  CHECK(a > 200);
  printf("test_sampling: auto-sampler (6-OP FM) PASS\n");
}

// JUNO "A15 Moving Strings" (sustained): loops found on every note; the demo's instrument
static void test_asamp_juno() {
  const char* cmds[] = {"asamp=src:juno:4", "asamp=notes:c2:c6:6", "asamp=vel:100", "asamp=hold:1800", "asamp=tail:2500",
                        "asamp=loop:on", "asamp=name:JunoStrings", "asamp=start", nullptr};
  samp::AStatus s = run_asamp(cmds);
  CHECK(s.state == samp::kADone && s.total == 9 && s.written == 9);
  std::string t = slurp("/MULTISAMPLE/JunoStrings/JunoStrings.sfz");
  printf("test_sampling: JUNO: %d loops of 9\n", s.loops);
  CHECK(count(t, "<region>") == 9 && s.loops >= 6 && count(t, "loop_mode=loop_sustain") == s.loops);
  // the loop is in the WAV's smpl chunk too
  std::string w = slurp("/MULTISAMPLE/JunoStrings/samples/JunoStrings_C4_v100_rr1.wav");
  CHECK(w.size() > 112 && w.substr(36, 4) == "smpl" && t.find("JunoStrings_C4_v100_rr1.wav lokey=58 hikey=63 pitch_keycenter=60 "
                                                               "lovel=1 hivel=127 loop_mode=loop_sustain") != std::string::npos);
  printf("test_sampling: auto-sampler (JUNO, loops) PASS\n");
}

// The demo: the auto-sampled JUNO strings play chords held 3 s (longer than the 1.8 s sampled: the loops carry them),
// then the 6-OP FM instrument plays short chords, on MULTISAMPLE, into SAMP_DEMO (16-bit mono 48 kHz)
static void demo(const char* path) {
  std::vector<int16_t> out;
  auto play = [&](const char* inst, int vi, const int* roots, const int* chd, int n, int hold_blocks, int len_blocks) {
    int e = entry(inst);
    CHECK(e >= 0);
    char o[200];
    msamp::command(("msamp=voice:" + std::to_string(vi)).c_str(), o, sizeof o);
    send(report(0x03, kFmB | vi, -1, 0, ms(e)));
    for (int i = 0; i < 400; ++i) block();
    for (int c = 0; c < n; ++c) {
      std::vector<float> p = {(float)e, 127, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, (float)chd[c], (float)(c % 2), 0, 0};
      send(report(0x04, kFmB | vi, roots[c], 110, p));
      for (int b = 0; b < len_blocks; ++b) {
        int w = g_w;
        block();
        for (int k = 0; k < kBlock; ++k) out.push_back(g_ring[(w + k) % (48000 * 8)][vi]);
        if (b == hold_blocks) send({0xf0, 0x7d, 0x52, 0x54, 0x11, (uint8_t)vi, 0xf7});
      }
    }
    msamp::silence(vi);
    msamp::command(("msamp=voice:" + std::to_string(vi) + ":off").c_str(), o, sizeof o);
  };
  const int r1[4] = {48, 45, 50, 43}, c1[4] = {6, 7, 4, 1};   // Cmaj7 Am7 Dsus4 G
  play("JunoStrings.sfz", 4, r1, c1, 4, 4000 * 3, 4000 * 4);
  const int r2[8] = {60, 57, 62, 55, 60, 57, 65, 67}, c2[8] = {6, 7, 7, 1, 6, 7, 1, 5};
  play("FMtest.sfz", 5, r2, c2, 8, 4000 * 4 / 10, 4000 / 2);
  FILE* f = fopen(path, "wb");
  CHECK(f);
  uint8_t h[64];
  int n = samp::wav_header(h, 1, 16, 48000, (uint32_t)out.size());   // (16-bit: the header's sizes hold for any width)
  fwrite(h, 1, n, f);
  fwrite(out.data(), 2, out.size(), f);
  fclose(f);
  double s = 0;
  for (int16_t v : out) s += (double)v * v;
  printf("test_sampling: demo: %zu frames into %s, rms %.0f\n", out.size(), path, std::sqrt(s / out.size()));
}

// ---- a Rytm track: the notes go out on its channel; a fake Rytm sounds the track 6 ms later on IN 2 + track
struct FakeNote {
  bool on;
  int note, vel;
  uint64_t at_us, off_us;
};
static FakeNote g_fake = {false, 0, 0, 0, 0};
static int g_notes_on = 0, g_notes_off = 0, g_note_ch = -1;
static void fake_rytm(const uint8_t* m, int n) {
  if (n != 3) return;
  if ((m[0] & 0xF0) == 0x90 && m[2]) {
    g_fake = {true, m[1], m[2], g_us + 6000, 0};
    ++g_notes_on, g_note_ch = m[0] & 15;
  } else if ((m[0] & 0xF0) == 0x80 || (m[0] & 0xF0) == 0x90) {
    g_fake.off_us = g_us + 6000;
    ++g_notes_off;
  }
}
static void test_asamp_rytm() {
  hooks.to_rytm = fake_rytm;
  char o[600];
  const char* cmds[] = {"asamp=src:track:2:11", "asamp=vel:60,127", "asamp=rr:2", "asamp=hold:300", "asamp=tail:400",
                        "asamp=gap:50", "asamp=lat:auto", "asamp=loop:off", "asamp=name:auto", "asamp=start", nullptr};
  for (const char* const* c = cmds; *c; ++c) CHECK(samp::asamp_command(*c, o, sizeof o));
  printf("test_sampling: %s\n", o);
  samp::AStatus s;
  uint32_t rnd = 3;
  double ph = 0;
  for (int b = 0; b < 4 * 1000 * 20; ++b) {
    samp::asamp_status(&s);
    if (s.state == samp::kADone || s.state == samp::kAFailed) break;
    int32_t f[kBlock * 12] = {0};
    for (int k = 0; k < kBlock; ++k) {   // the track (SD: IN 3): a 180 Hz tone, the velocity's level, a 80 ms decay after its off
      uint64_t t = g_us + k * 1000000ull / 48000;
      rnd = rnd * 1664525u + 1013904223u;
      double v = ((rnd >> 8) / 8388608.0 - 1) * 2e-4;
      if (g_fake.on && t >= g_fake.at_us) {
        double env = g_fake.off_us && t > g_fake.off_us ? std::exp(-(double)(t - g_fake.off_us) / 80000.0) : 1.0;
        ph += 2 * M_PI * 180 / 48000;
        v += 0.4 * g_fake.vel / 127.0 * env * std::sin(ph);
      }
      f[k * 12 + 3] = (int32_t)(v * 2147483000.0);
    }
    if (samp::asamp_wants_in()) samp::asamp_in_block(f, kBlock, 12);
    block(false);
  }
  for (int i = 0; i < 400; ++i) block(false);
  samp::asamp_command("asamp", o, sizeof o);
  printf("test_sampling: %s; latency %.1f ms\n", o, s.lat_ms);
  CHECK(s.state == samp::kADone && s.total == 4 && s.written == 4 && g_notes_on == 4 && g_notes_off == 4 && g_note_ch == 10);
  CHECK(std::fabs(s.lat_ms - 6.0f) < 1.0f);
  std::string t = slurp("/MULTISAMPLE/RYTM_SD/RYTM_SD.sfz");
  CHECK(count(t, "<region>") == 4 && count(t, "seq_length=2") == 4 && t.find("lovel=94 hivel=127") != std::string::npos);
  printf("test_sampling: auto-sampler (a Rytm track) PASS\n");
}

int main() {
  test_dsp();
  test_cardq();
  char tmpl[] = "/tmp/core_sampling_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples' '" + g_card + "/MULTISAMPLE'").c_str()) == 0);
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  for (int i = 0; i < 4000 && msamp::step(); ++i) {}
  test_sampler_threshold();
  test_sampler_bars();
  test_sampler_voice();
  test_asamp_fm();
  test_asamp_juno();
  const char* d = getenv("SAMP_DEMO");
  if (d && *d) demo(d);
  test_asamp_rytm();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_sampling: PASS\n");
  return 0;
}
