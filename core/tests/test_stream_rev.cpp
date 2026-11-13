// core/tests/test_stream_rev.cpp - SAMPLER's stream backwards (core/engines/stream_voice.h prepare's rev, 0.5.0: PLAY
// REV / RLP, or END before STRT, on a sample not in the sample RAM): the head ends at the start frame, the ring fills
// downwards. At unity every frame of the file comes out in reverse order, bit for bit as the forward stream gave it
// (16-bit and 24-bit, mono and stereo: both channels), from the file's end and from the middle down to the region's low
// end, then silent; a loop goes back to the start frame each time, and reads nothing below the region (review L3). With
// a card core (requests 0.25 .. 10 ms late), at unity, +7, +12 and -12 semitones, and a retrigger while a ring request
// is under way: the same output as without, no underruns. Then the engine (no sample RAM: the Teensy's and
// sampleram=0's path): SAMPLER's PLAY REV streams the sample backwards, RLP loops, FWD forwards.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "engines/stream_voice.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "pool/card_index.h"
#include "pool/pool.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;
using SV = rytm::StreamVoice;
constexpr int kB = 12;
static card::Index idx;
static uint8_t scratch[256 * 1024];
static uint8_t head[SV::kHead], ring[SV::kRingBytes];
static SV sv;
static void lg(const char*, ...) {}
static constexpr int kFrames = 5 * 48000;

static int g_smp;
static bool find(const char* name, int* fo, card::FileRec* r) {
  for (int f = 0; f < idx.folders(); ++f)
    for (int i = 0; idx.file(f, i, r); ++i)
      if (!strcmp(r->name, name)) return *fo = f, g_smp = i, true;
  return false;
}
struct Play {
  float semis, start, end;
  bool loop;
  int blocks;
  int retrig = -1;   // the block before which the note is triggered again (-1 none)
};
// L then R of each block (R: the render's outr, both channels of a stereo file)
static void block(const Play& p, std::vector<float>& o) {
  float l[kB], r[kB];
  sv.render(p.semis, p.start, p.end, p.loop, 1.0f, l, kB, r);
  o.insert(o.end(), l, l + kB), o.insert(o.end(), r, r + kB);
}
static uint64_t g_read;   // bytes the last sync_way's refills read
static std::vector<float> sync_way(const char* name, const Play& p, bool rev) {   // no card core: the reads here
  int fo;
  card::FileRec r;
  static File f;
  CHECK(find(name, &fo, &r) && idx.open(fo, r, &f));
  CHECK(sv.prepare(r, f, fo, g_smp, p.start, rev));
  sv.trigger();
  std::vector<float> o;
  g_read = 0;
  for (int b = 0; b < p.blocks; ++b) {
    if (b == p.retrig) sv.trigger();
    while (uint32_t n = sv.refill()) g_read += n;
    block(p, o);
  }
  return o;
}
static bool serve() {
  host_card_core(0);
  bool r = cardq::service() != nullptr;
  host_card_core(3);
  return r;
}
static std::vector<float> queued(const char* name, const Play& p, int late) {
  int fo;
  card::FileRec r;
  static File f;
  CHECK(find(name, &fo, &r) && idx.open(fo, r, &f));
  for (int i = 0;; ++i) {
    CHECK(i < 100);
    SV::Prep s = sv.prepare_q(r, f, fo, g_smp, p.start, false, true);
    if (s == SV::kPrepOk) break;
    CHECK(s == SV::kPrepWait);
    serve();
    sv.step(now_us());
  }
  sv.trigger();
  std::vector<float> o;
  for (int b = 0; b < p.blocks; ++b) {
    if (b == p.retrig) sv.trigger();   // (a ring request may be under way: it is stale now)
    uint64_t c = host_card_us(3);
    sv.step(now_us());
    while (sv.refill()) {}
    CHECK(host_card_us(3) == c);   // no card time on the control core
    block(p, o);
    if ((b + 1) % late == 0) while (serve()) sv.step(now_us());
  }
  sv.clear();
  for (int i = 0; i < 6; ++i) {
    sv.step(now_us());
    while (serve()) {}
  }
  return o;
}
// frame k's (L, R) in a forward render's output
static float at(const std::vector<float>& o, int k, int c) { return o[(k / kB) * 2 * kB + c * kB + k % kB]; }

static void t_unit(const char* name) {
  const int blocks = kFrames / kB + 4;
  std::vector<float> fwd = sync_way(name, {0, 0, 1, false, blocks}, false);
  CHECK(sv.underruns() == 0);
  // from the end down to frame 0: every frame, reversed, bit for bit; then silent
  std::vector<float> rev = sync_way(name, {0, 1, 0, false, blocks}, true);
  CHECK(sv.underruns() == 0);
  for (int i = 0; i < kFrames; ++i)
    for (int c = 0; c < 2; ++c) CHECK(at(rev, i, c) == at(fwd, kFrames - 1 - i, c));
  for (int i = kFrames + kB; i < blocks * kB; ++i) CHECK(at(rev, i, 0) == 0.0f);
  CHECK(!sv.playing());
  // from the middle (STA 0.5 as the start, END 0.1 the low end)
  const int s0 = (int)(0.5f * (kFrames - 1)), lo = (int)ceilf(0.1f * (kFrames - 1));
  std::vector<float> mid = sync_way(name, {0, 0.5f, 0.1f, false, blocks}, true);
  for (int i = 0; i <= s0 - lo; ++i) CHECK(at(mid, i, 0) == at(fwd, s0 - i, 0) && at(mid, i, 1) == at(fwd, s0 - i, 1));
  for (int i = s0 - lo + kB; i < s0 - lo + 4 * kB; ++i) CHECK(at(mid, i, 0) == 0.0f);
  // a loop: back to the start frame at the low end, over and over (the ring restarts below the head)
  const float lo2 = 0.45f;
  const int l2 = (int)ceilf(lo2 * (kFrames - 1)), len = s0 - l2 + 1;
  std::vector<float> lp = sync_way(name, {0, 0.5f, lo2, true, 3 * len / kB + 8}, true);
  CHECK(sv.playing() && sv.underruns() == 0);
  for (int i = 0; i < 2 * len; ++i) CHECK(at(lp, i, 0) == at(fwd, s0 - i % len, 0));
  // L3: a short loop reads nothing below its low end - each pass at most the region and a chunk (the ring holds more)
  {
    const float lo3 = 0.495f;
    const int l3 = (int)ceilf(lo3 * (kFrames - 1)), len3 = s0 - l3 + 1, passes = 12;
    std::vector<float> sl = sync_way(name, {0, 0.5f, lo3, true, passes * len3 / kB}, true);
    CHECK(sv.playing() && sv.underruns() == 0);
    for (int i = 0; i < 2 * len3; ++i) CHECK(at(sl, i, 0) == at(fwd, s0 - i % len3, 0));
    const uint64_t bpf = (uint64_t)(name[0] == 's' ? 2 : 1) * (strstr(name, "24") ? 3 : 2);   // st24: 6 bytes a frame
    CHECK(g_read <= (uint64_t)(passes + 1) * (len3 * bpf + 2 * SV::kRevChunk));
    CHECK(g_read < (uint64_t)passes * SV::kRing / 2);
  }
  // with a card core: the same, at unity and an octave up, the core late
  cardq::set_worker(true);
  for (const Play& p : {Play{0, 1, 0, false, blocks}, Play{12, 1, 0, false, blocks / 2 + 4}, Play{7, 0.8f, 0.1f, false, blocks / 2},
                        Play{-12, 0.6f, 0.2f, false, blocks / 3}, Play{0, 0.5f, lo2, true, 3 * len / kB},
                        Play{0, 0.9f, 0.1f, false, blocks / 3, 300}})
    for (int late : {1, 8, 40}) {
      cardq::set_worker(false);
      std::vector<float> want = sync_way(name, p, true);
      cardq::set_worker(true);
      std::vector<float> got = queued(name, p, late);
      CHECK(got == want && sv.underruns() == 0);
      bool any = false;
      for (float x : got) any = any || x != 0.0f;
      CHECK(any);
    }
  cardq::set_worker(false);
}

// ---- the engine without the sample RAM: DSP SAMPLE streams
static float P[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100};
static void send(int cmd) {   // SAMPLER: DSP SAMPLE's flag with its tag (machines.h kSmplTag)
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(kDspSample | 5)};
  if (cmd == 0x04) m.push_back(60), m.push_back(100);
  for (float x : P) m.push_back((uint8_t)x), m.push_back(0);
  m.push_back((uint8_t)(kSmplTag >> 7)), m.push_back((uint8_t)(kSmplTag & 0x7f));
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static std::vector<double> play(int frames) {
  send(0x04);
  while (idle_prepare()) {}
  static int16_t rg[kB][kVoices];
  uint64_t mach[kMachines] = {0};
  std::vector<double> o;
  for (int b = 0; b < frames / kB; ++b) {
    for (int k = 0; k < 4; ++k) idle_stream();
    render_voices(rg, kB, 0, 0, kVoices, mach);
    for (int i = 0; i < kB; ++i) o.push_back(atanh(rg[i][5] / (0.7 * 32767.0)));
  }
  return o;
}
static double ramp(double f) { return 0.1 + 0.5 * f / kFrames; }
static void t_engine() {
  CHECK(!psram);
  P[11] = 15;   // REV: from the end, down
  std::vector<double> o = play(48000);
  CHECK(std::fabs(o[0] - ramp(kFrames - 1)) < 0.003 && std::fabs(o[24000] - ramp(kFrames - 1 - 24000)) < 0.003);
  CHECK(streams[5].underruns() == 0);
  P[11] = 43, P[12] = 0, P[13] = 6;   // RLP over STRT 0 .. END 6 (12000 frames): END down to STRT, again
  o = play(36000);
  CHECK(std::fabs(o[0] - ramp(0.05 * (kFrames - 1))) < 0.003 && std::fabs(o[12100] - ramp(0.05 * (kFrames - 1) - 100)) < 0.003);
  CHECK(streams[5].playing());
  P[11] = 0, P[13] = 120;   // FWD
  o = play(4800);
  CHECK(std::fabs(o[0] - ramp(0)) < 0.003 && o[2400] > o[0]);
  P[12] = 120, P[13] = 0;   // FWD with END before STRT: backwards (DSP SAMPLE's is unchanged: test_dsps_golden)
  o = play(4800);
  CHECK(std::fabs(o[0] - ramp(kFrames - 1)) < 0.003 && o[2400] < o[0]);
  CHECK(v[5].smpl);
}

int main() {
  char tmpl[] = "/tmp/core-streamrev-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Samples/S") && fs.mkdir("/.rytm/projects/SMP"));
  auto noise = [](uint32_t seed) {
    return [seed](int i, int c) {
      uint32_t x = ((uint32_t)i + (uint32_t)c * 977u + seed) * 2654435761u;
      x ^= x >> 13;
      return (double)(x * 2654435761u >> 8) / 16777216.0 - 0.5;
    };
  };
  std::string card = tmpl;
  wg::write_file(card + "/Samples/S/mo16.wav", wg::wav_bytes(kFrames, 1, 16, 48000, 1, "", noise(1)));
  wg::write_file(card + "/Samples/S/st24.wav", wg::wav_bytes(kFrames, 2, 24, 48000, 1, "", noise(2)));
  wg::write_file(card + "/Samples/S/mo24.wav", wg::wav_bytes(kFrames, 1, 24, 48000, 1, "", noise(3)));
  wg::write_file(card + "/Samples/S/st16.wav", wg::wav_bytes(kFrames, 2, 16, 48000, 1, "", noise(4)));
  wg::write_file(card + "/Samples/S/ramp.wav", wg::wav_bytes(kFrames, 1, 16, 48000, 1, "", [](int i, int) { return ramp(i); }));
  CHECK(idx.begin(lg, scratch, sizeof scratch));
  sv.attach(head, ring);
  host_card_model(true);
  t_unit("mo16.wav");
  t_unit("st24.wav");
  t_unit("mo24.wav");
  t_unit("st16.wav");
  host_card_model(false);

  FILE* f = fopen((card + "/.rytm/project").c_str(), "wb");
  fputs("SMP", f);
  fclose(f);
  f = fopen((card + "/.rytm/projects/SMP/samples.pool.txt").c_str(), "wb");   // SMP 1 = A001
  fprintf(f, "#sidecar samplepool 1\nA001 sample - %d /Samples/S/ramp.wav\tramp\n", 44 + 2 * kFrames);
  fclose(f);
  unsetenv("CORE_SAMPLE_RAM_MB");
  CHECK(engine_begin());
  for (int i = 0; i < 2000 && npool == 0; ++i) idle_stream();
  CHECK(npool == 1 && pool[0].folder >= 0);
  t_engine();
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_stream_rev: PASS\n");
  return 0;
}
