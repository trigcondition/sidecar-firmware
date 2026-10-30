// core/tests/test_engine.cpp - the engine's API: boot on a card (the last project's SAMPLES pool, bank A: DSP SAMPLE), the host's bytes,
// blocks, and the work between requests (a DSP SAMPLE prepare, the project's name saved).
#include <cmath>
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "engine.h"
#include "wav.h"

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

int main() {
  using namespace core;
  char tmpl[] = "/tmp/core-engine-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/S") && fs.mkdir("/.rytm/projects/SMP"));
  write_wav("/Samples/S/tone.wav", 48000, 1, 16, 48000,
            [](int i, int) { return (int32_t)(10000 * std::sin(2 * M_PI * 440.0 * i / 48000)); });
  FILE* f = fopen(host_path("/.rytm/project").c_str(), "wb");
  fputs("SMP", f);
  fclose(f);
  f = fopen(host_path("/.rytm/projects/SMP/samples.pool.txt").c_str(), "wb");   // DSP SAMPLE's SMP 1 = A001
  fputs("#sidecar samplepool 1\nA001 sample - 96044 /Samples/S/tone.wav\ttone\n", f);
  fclose(f);
  hooks.to_rytm = out;
  CHECK(engine_begin());
  for (int i = 0; i < 2000 && npool == 0; ++i) idle_stream();   // the project's pool lists read (the card core's job)
  CHECK(!strcmp(project, "SMP") && npool == 1 && pool[0].folder >= 0);
  // DSP SAMPLE on voice 5, SMP 1, MIX 0 (the sample alone)
  std::vector<uint8_t> t = {0xf0, 0x7d, 0x52, 0x54, 0x04, 0x20 | 5, 60, 100};
  const float p[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100};
  for (float x : p) { t.push_back((uint8_t)x); t.push_back(0); }
  t.push_back(0xf7);
  engine_sysex(t.data(), (int)t.size());
  CHECK(prep_pending[5] && trig_pending[5]);   // the trig waits for its sample's head
  CHECK(idle_prepare() && !prep_pending[5] && !idle_prepare());
  static int16_t ring[kBlock * 100][kVoices];
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < 100; ++b) {
    idle_stream();
    render_voices(ring, kBlock * 100, b * kBlock, 0, kVoices, mach);
  }
  double s = 0;
  for (int i = 0; i < kBlock * 100; ++i) s += (double)ring[i][5] * ring[i][5];
  CHECK(std::sqrt(s / (kBlock * 100)) > 1000 && mach[kMDsps] > 0);
  std::vector<uint8_t> pr = {0xf0, 0x7d, 0x52, 0x54, 0x10, 'N', 'E', 'W', 0xf7};
  engine_sysex(pr.data(), (int)pr.size());
  CHECK(idle_save() && !idle_save());
  printf("test_engine: PASS\n");
  return 0;
}
