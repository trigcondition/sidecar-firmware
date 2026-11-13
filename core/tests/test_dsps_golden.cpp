// core/tests/test_dsps_golden.cpp - DSP SAMPLE plays as it did before 0.5.0's SAMPLER (docs/SAMPLER.md), sample for
// sample: the resonator (MIX 0 and above, its three models), ONE / LOP / LOOP / SLICE, END before STA, TUN / FIN, a mono
// and a stereo file on OUT TRK and PRE, each from the card (no sample RAM: the Teensy's path) and from the sample RAM
// (the Pi's). Voice 0's rows (L, and R on PRE) are hashed (FNV-1a 64) and compared with
// tests/golden/dsps/v1/render.txt, which the host before SAMPLER (f9330b8^, the 0.4.x engine) wrote with
// DSPS_GOLDEN_WRITE=1. The same source builds against both hosts: it uses no 0.5.0 name and sends no sound tag.
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "core_port.h"   // host_fake_time
#include "engine.h"
#include "fx/out.h"
#include "machines.h"
#include "pool/pool.h"
#include "voices.h"
#include "wav.h"

using namespace core;

static constexpr int kN = 48000;   // frames of each file (1 s)
static uint64_t g_us = 1000000;

struct Case {
  const char* name;
  int smp;          // 1 mono 16-bit, 2 stereo 16-bit
  bool pre;         // OUT PRE (the stereo pair's path) else TRK
  int note;
  float p[16];      // ENG STR BRI DMP MIX FB TUN LEV / TUN FIN SMP MOD STA END LOP LEV
};
// MOD values: 0 ONE, 25 LOOP, 40 SLICE (rytm::SampleModes' 7 zones)
static const Case kCases[] = {
    {"one_mix0", 1, false, 60, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100}},
    {"one_mix64_modal", 1, false, 60, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100}},
    {"one_mix100_string", 1, false, 60, {64, 120, 30, 90, 100, 64, 70, 110, 64, 64, 1, 0, 0, 120, 0, 100}},
    {"one_mix64_reson", 1, false, 67, {127, 40, 100, 20, 64, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100}},
    {"lop_mix30", 1, false, 60, {0, 64, 64, 64, 30, 64, 64, 100, 64, 64, 1, 0, 0, 12, 1, 100}},
    {"loop_mix64", 1, false, 60, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 1, 25, 10, 30, 0, 100}},
    {"end_before_sta", 1, false, 60, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 90, 30, 0, 100}},
    {"end_before_sta_mix64", 1, false, 60, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 1, 0, 90, 30, 0, 100}},
    {"slice", 1, false, 60, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 40, 0, 120, 3, 100}},
    {"tun_fin", 1, false, 60, {0, 64, 64, 64, 20, 64, 64, 100, 76, 80, 1, 0, 0, 120, 0, 100}},
    {"st_trk_mix0", 2, false, 60, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 2, 0, 0, 120, 0, 100}},
    {"st_trk_mix64", 2, false, 60, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 2, 0, 0, 120, 0, 100}},
    {"st_pre_mix0", 2, true, 60, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 2, 0, 0, 120, 0, 100}},
    {"st_pre_mix64", 2, true, 60, {0, 64, 64, 64, 64, 64, 64, 100, 64, 64, 2, 0, 0, 120, 0, 100}},
    {"mono_pre_mix80", 1, true, 60, {64, 90, 70, 50, 80, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100}},
};

static void send(int cmd, const Case& c) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(kDspSample | 0)};
  if (cmd == 0x04) m.push_back((uint8_t)c.note), m.push_back(100);
  for (float x : c.p) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void tick() {
  idle_prepare();
  for (int k = 0; k < 4; ++k) idle_stream();
  g_us += 250, host_fake_time(true, g_us);
}
static uint64_t fnv(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
// The case's trig and 9600 frames of voice 0: L, and R when on PRE
static uint64_t play(const Case& c, bool ram) {
  out::control.route[0] = c.pre ? out::kPre : out::kTrk;
  send(0x03, c);
  if (ram) {   // the sample in the sample RAM, voice 0 playing from it (as the Pi plays it)
    for (int i = 0; i < 20000 && !from_ram[0]; ++i) {
      if (i % 50 == 0) send(0x04, c);
      tick();
    }
    CHECK(from_ram[0]);
  }
  send(0x04, c);
  for (int i = 0; i < 8; ++i) tick();
  static int16_t ring[kBlock][kVoices], ring_r[kBlock][kVoices];
  uint64_t mach[kMachines] = {0};
  uint64_t h = 1469598103934665603ull;
  for (int b = 0; b < 9600 / kBlock; ++b) {
    memset(ring, 0, sizeof ring), memset(ring_r, 0, sizeof ring_r);
    tick();
    render_voices(ring, kBlock, 0, 0, kVoices, mach, ring_r);
    for (int i = 0; i < kBlock; ++i) {
      h = fnv(h, &ring[i][0], 2);
      if (c.pre) h = fnv(h, &ring_r[i][0], 2);
    }
  }
  return h;
}

static std::map<std::string, std::string> read_golden(const char* path) {
  std::map<std::string, std::string> w;
  FILE* f = fopen(path, "r");
  if (!f) return w;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char k[128], v[64];
    if (sscanf(line, "%127s %63s", k, v) == 2) w[k] = v;
  }
  fclose(f);
  return w;
}

// One phase in this process: the engine with (ram) or without the sample RAM; "phase/case hash" lines to out
static void phase(bool ram, FILE* out) {
  if (ram) setenv("CORE_SAMPLE_RAM_MB", "16", 1);
  else unsetenv("CORE_SAMPLE_RAM_MB");
  setenv("CORE_POOL_RAM_MB", "64", 1);
  CHECK(engine_begin() && card_ok);
  CHECK(ram == (psram != nullptr));
  for (int i = 0; i < 4000 && npool < 2; ++i) tick();
  CHECK(npool == 2 && pool[0].folder >= 0 && pool[1].folder >= 0);
  for (const Case& c : kCases) fprintf(out, "%s/%s %016llx\n", ram ? "ram" : "card", c.name, (unsigned long long)play(c, ram));
}

int main() {
  char tmpl[] = "/tmp/core-dsps-golden-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/S") && fs.mkdir("/.rytm/projects/SMP"));
  // A001: a decaying 220 Hz tone with a click (mono); A002: two tones (stereo)
  write_wav("/Samples/S/mono.wav", kN, 1, 16, 48000, [](int i, int) {
    return (int32_t)std::lround(24000.0 * std::exp(-3.0 * i / kN) * std::sin(2 * M_PI * 220.0 * i / 48000.0) + (i < 64 ? 6000 : 0));
  });
  write_wav("/Samples/S/stereo.wav", kN, 2, 16, 44100, [](int i, int c) {
    return (int32_t)std::lround(20000.0 * std::exp(-2.0 * i / kN) * std::sin(2 * M_PI * (c ? 330.0 : 440.0) * i / 44100.0));
  });
  FILE* f = fopen(host_path("/.rytm/project").c_str(), "wb");
  fputs("SMP", f);
  fclose(f);
  f = fopen(host_path("/.rytm/projects/SMP/samples.pool.txt").c_str(), "wb");
  fprintf(f, "#sidecar samplepool 1\nA001 sample - %d /Samples/S/mono.wav\tmono\nA002 sample - %d /Samples/S/stereo.wav\tstereo\n",
          44 + 2 * kN, 44 + 4 * kN);
  fclose(f);

  // the card phase in a child (an engine without the sample RAM), the RAM phase here
  std::string cardf = std::string(tmpl) + "-card.txt";
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    FILE* o = fopen(cardf.c_str(), "w");
    phase(false, o);
    fclose(o);
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  std::string all;
  {
    FILE* c = fopen(cardf.c_str(), "r");
    CHECK(c);
    char line[256];
    while (fgets(line, sizeof line, c)) all += line;
    fclose(c);
  }
  {
    char* buf = nullptr;
    size_t len = 0;
    FILE* m = open_memstream(&buf, &len);
    phase(true, m);
    fclose(m);
    all += std::string(buf, len);
    free(buf);
  }
  const char* golden = "tests/golden/dsps/v1/render.txt";
  if (getenv("DSPS_GOLDEN_WRITE")) {
    FILE* g = fopen(golden, "w");
    CHECK(g);
    fputs("# DSP SAMPLE's voice 0, 9600 frames a case (core/tests/test_dsps_golden.cpp), written by the 0.4.x engine\n", g);
    fputs(all.c_str(), g);
    fclose(g);
    printf("test_dsps_golden: wrote %s\n", golden);
  } else {
    std::map<std::string, std::string> want = read_golden(golden);
    CHECK(!want.empty());
    int n = 0;
    size_t at = 0;
    while (at < all.size()) {
      size_t e = all.find('\n', at);
      std::string line = all.substr(at, e - at);
      at = e + 1;
      char k[128], v[64];
      CHECK(sscanf(line.c_str(), "%127s %63s", k, v) == 2);
      if (want[k] != v) printf("test_dsps_golden: %s differs: %s, golden %s\n", k, v, want[k].c_str());
      CHECK(want[k] == v);
      ++n;
    }
    CHECK(n == (int)want.size());
  }
  CHECK(system((std::string("rm -rf '") + tmpl + "' '" + cardf + "'").c_str()) == 0);
  printf("test_dsps_golden: PASS\n");
  return 0;
}
