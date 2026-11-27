// core/tests/mc/test_render_cores.cpp - two cores rendering disjoint voice ranges at once (the Pi engine's split,
// pi/engine/cores.cpp), on two threads, against one thread rendering the same two halves in turn (as core 0 and
// core 1: the per-core state the same). Each half: two 6-OP FM voices, all reloading in the same blocks
// (fm_relist, as a project change or LOAD TO PROJ does), a PLAITS PARTICLE voice (stmlib's Random) and a
// DAISYSP STRING voice (rand()). Bit-identical, or a shared state raced: with one FM stage and bank pointer a wrong
// DX7 voice or a torn bank; with one RNG state each core's noise depends on the other's timing.
// Built with RYTM_RENDER_CORES=3 (core/Makefile's build/mc: engines and core alike); rytm_render_core() is the
// thread's.
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "../check.h"
#include "voices.h"

static thread_local int t_core = 0;
extern "C" int rytm_render_core() { return t_core; }

static void sink(const uint8_t*, int) {}
static void quiet(const char*, int) {}   // the trig lines

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back(note); m.push_back(vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}

constexpr int kBlocks = 600;
constexpr int kRows = core::kBlock * kBlocks;
using Ring = int16_t[kRows][core::kVoices];

static const int kFmVoices[4] = {0, 1, 4, 5};
// Each slot's box (16 voices: slot 1's 8..15 too, voice_report_at its engine voices), the same set a few notes up
static void trigs(int round) {   // per half: two FM voices (each its own ROM voice), PARTICLE, DAISYSP
  for (int sl = 0; sl < core::kVoiceSlots; ++sl) {
    const int b = sl * core::kBoxVoices, up = 5 * sl;
    for (int k = 0; k < 4; ++k) {
      auto t = report(0x04, core::kFm | kFmVoices[k], 48 + 3 * k + up, 100,
                      {(float)(7 * k + round % 5 + sl), 64, 64, 100, 64, 64, 120, 0});
      core::voice_report_at(b + kFmVoices[k], 0x04, t.data(), (int)t.size());
    }
    for (int h = 0; h < 2; ++h) {
      // P1 106: zone 14 of 17, PARTICLE (Plaits' engine 18); DAISYSP STRING (P1 64: its noise burst and dispersion)
      auto p = report(0x04, core::kPlaits | (2 + 4 * h), 60 + h + up, 100, {106, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
      core::voice_report_at(b + 2 + 4 * h, 0x04, p.data(), (int)p.size());
      auto d = report(0x04, core::kDaisy | (3 + 4 * h), 57 + h + up, 100, {64, 64, 64, 64, 0, 64, 64, 100});
      core::voice_report_at(b + 3 + 4 * h, 0x04, d.data(), (int)d.size());
    }
  }
}
// Core c's voices, as the Pi splits them (split=4): 4c..4c+3 of each slot's eight
static void render_core(Ring& ring, int g, int c, uint64_t* mach) {
  for (int sl = 0; sl < core::kVoiceSlots; ++sl) {
    const int v0 = sl * core::kBoxVoices + 4 * c;
    core::render_voices(ring, kRows, (g - 1) * core::kBlock, v0, v0 + 4, mach);
  }
}

// Every block: the control core (this thread, core 2: the Pi's core 3) first, then the render: two threads (0..3
// as core 0, 4..7 as core 1) or this one, the two halves in turn with the same core indices
static void run(bool two, Ring& ring) {
  t_core = 2;
  core::hooks.to_rytm = sink;
  core::voices_init();
  std::atomic<int> gen{0}, done{0};
  uint64_t mach[2][core::kMachines] = {};
  auto worker = [&](int c) {
    t_core = c;
    for (int g = 1; g <= kBlocks; ++g) {
      while (gen.load(std::memory_order_acquire) != g) {}
      render_core(ring, g, c, mach[c]);
      done.fetch_add(1, std::memory_order_acq_rel);
    }
  };
  std::thread a, b;
  if (two) { a = std::thread(worker, 0); b = std::thread(worker, 1); }
  for (int g = 1; g <= kBlocks; ++g) {
    if (g % 40 == 1) trigs(g / 40);
    if (g % 3 == 0) core::fm_relist();   // every FM voice reloads in the next block, in both halves at once
    if (two) {
      gen.store(g, std::memory_order_release);
      while (done.load(std::memory_order_acquire) != 2 * g) {}
    } else {
      for (int c = 0; c < 2; ++c) {
        t_core = c;
        render_core(ring, g, c, mach[c]);
      }
      t_core = 2;
    }
  }
  if (two) { a.join(); b.join(); }
}

int main() {
  core::host_set_log_sink(quiet);
  // the reference in a child (fork before any thread: both start from the same state)
  auto* ref = (Ring*)mmap(nullptr, sizeof(Ring), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  CHECK(ref != MAP_FAILED);
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) { run(false, *ref); _exit(0); }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  static Ring two;
  run(true, two);
  CHECK(core::v[2].patch.engine == 18 && core::v[6].patch.engine == 18 && core::v[3].daisy && core::v[7].daisy && core::daisy_model(core::v[3]) == rytm::DaisyEngine::STRING);
  for (int vi = 0; vi < core::kVoices; ++vi) {
    double s = 0;
    for (int r = 0; r < kRows; ++r) s += (double)(*ref)[r][vi] * (*ref)[r][vi];
    if (std::sqrt(s / kRows) <= 100) fprintf(stderr, "voice %d: rms %.1f\n", vi, std::sqrt(s / kRows));
    CHECK(std::sqrt(s / kRows) > 100);   // every voice sounds
    int first = -1;
    for (int r = 0; r < kRows && first < 0; ++r)
      if ((*ref)[r][vi] != two[r][vi]) first = r;
    if (first >= 0) fprintf(stderr, "voice %d: two cores differ from one from frame %d\n", vi, first);
    CHECK(first < 0);
  }
  printf("test_render_cores: PASS (%d voices on two threads - FM reloading %d times, PARTICLE, DAISYSP - "
         "bit-identical to one)\n", core::kVoices, kBlocks / 3);
  return 0;
}
