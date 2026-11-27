// core/tests/test_loop_threads.cpp - the looper's two cores at once (core 0: the IN / OUT hooks; core 3: the control tick, the
// grabs' copies and reads), many iterations, real threads: the ring and its frame map, the gap list, copy_idx against a writer,
// and the snapshot seqlock (a loop started, swapped, stopped while out_mix runs). Every frame a reader gets is either the
// frame's own value (index + 1) or silence inside a recorded gap; nothing torn, nothing stale.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "check.h"
#include "loop/loop.h"
#include "loop/loop_ring.h"

using namespace core;

static const uint32_t kCap = 1u << 16;

static void ring_vs_copy() {
  static std::vector<int32_t> mem(2 * (size_t)kCap);
  loop::Ring r;
  r.init(mem.data(), kCap);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bad{0}, copies{0};
  const unsigned kLostEvery = 1500, kLost = 40;
  std::thread writer([&] {   // core 0: 7-frame blocks, every kLostEvery-th preceded by kLost lost frames
    uint64_t k = 0;
    int32_t b[14];
    for (uint64_t blk = 0; blk < 400000; ++blk) {
      unsigned lost = blk && blk % kLostEvery == 0 ? kLost : 0;
      k += lost;
      for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(k + i + 1), b[2 * i + 1] = -(int32_t)(k + i + 1);
      r.write(b, 7, 1000 + k + 6, lost);
      k += 7;
    }
    stop = true;
  });
  std::thread reader([&] {   // core 3: copies of recent ranges, checked frame by frame
    std::vector<int32_t> out(2 * 4096);
    uint32_t rng = 1;
    while (!stop) {
      uint64_t w = r.written();
      if (w < 8192) continue;
      rng = rng * 1103515245u + 12345u;
      uint32_t n = 64 + (rng >> 8) % 4000;
      uint64_t k = w - n - (rng >> 4) % 20000;   // somewhere in the recent past (the ring holds 65536)
      uint32_t got = r.copy_idx(k, n, out.data());
      if (!got) continue;
      ++copies;
      for (uint32_t i = 0; i < got; ++i) {
        int32_t v = out[2 * i];
        bool ok = v == (int32_t)(k + i + 1) && out[2 * i + 1] == -v;
        if (!ok && v == 0 && out[2 * i + 1] == 0) ok = true;   // a gap: silence
        if (!ok) ++bad;
      }
    }
  });
  writer.join();
  reader.join();
  printf("  threads: %llu copies against the writer, %llu bad frames\n", (unsigned long long)copies.load(), (unsigned long long)bad.load());
  CHECK(bad == 0 && copies > 100);
}

static std::atomic<uint64_t> g_now{0};
static bool beat(double* o, double* p, int* l) { *o = 10000, *p = 6000, *l = 384; return true; }
static uint64_t frames() { return g_now; }
static uint16_t muted() { return 0; }

static void loop_vs_hooks() {
  static loop::Env env;
  env.beat_map = beat, env.frames = frames, env.muted = muted;
  loop::set_env(&env);
  const uint32_t ring = 1u << 20;
  std::vector<int32_t> rmem(2 * (size_t)ring);
  std::vector<uint8_t> store(24u << 20);
  loop::init(rmem.data(), ring, store.data(), store.size());
  loop::reset();
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> loud{0};
  std::thread audio([&] {   // core 0: the IN and OUT hooks block by block, the host's frame counter running
    uint64_t k = 0;
    int32_t F[7][12];
    while (!stop) {
      int32_t b[14];
      for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(100000 + (k + i) % 1000), b[2 * i + 1] = b[2 * i];
      loop::in_block(b, 7, k + 6 + 1000, 0);
      memset(F, 0, sizeof F);
      loop::out_mix(F, 7, k);
      for (int f = 0; f < 7; ++f) {
        if (F[f][0] > 2000000 || F[f][0] < -2000000) ++loud;   // 1000 + 100000 at most (LEV 100, BAL 64): never a stray big value
      }
      k += 7;
      g_now = k;
      if (k > 90000000ull) break;
    }
  });
  std::thread control([&] {   // core 3: the tick, grabs (resamples), taps, stops, level moves
    uint32_t rng = 7;
    for (int i = 0; i < 20000 && !stop; ++i) {
      loop::tick(0);
      rng = rng * 1103515245u + 12345u;
      switch ((rng >> 8) % 40) {
        case 0: loop::grab(loop::k1); break;
        case 1: loop::grab(loop::kHalf); break;
        case 2: loop::play((int)((rng >> 16) % 4)); break;
        case 3: loop::stop((rng >> 12) & 1, (rng >> 13) & 1); break;
        case 4: loop::set_bal((int)((rng >> 16) % 128)); break;
        case 5: loop::set_lev((int)((rng >> 16) % 128)); break;
        default: break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    stop = true;
  });
  control.join();
  audio.join();
  printf("  threads: the loop under a running audio side: %llu out-of-range samples, %d grabs, %u resamples\n", (unsigned long long)loud.load(), loop::grabs(), loop::stats().resamples);
  CHECK(loud == 0 && loop::stats().grabs > 0);
  loop::init(nullptr, 0, nullptr, 0);
}

int main() {
  ring_vs_copy();
  loop_vs_hooks();
  printf("test_loop_threads: OK\n");
  return 0;
}
