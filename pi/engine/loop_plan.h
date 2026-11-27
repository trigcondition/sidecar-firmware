// pi/engine/loop_plan.h - the master looper's RAM by board (spec-050 3.10, owner decisions 20 and 23, 2026-11-05): the ring
// is 32 MB on every board, the grab store 96 / 192 / 256 MB by RAM (2 GB / 4 GB / 8 GB and up); a 1 GB board has the looper
// OFF by default (the ring and the store take 80 MB from the pool there) and `loopram=MB` turns it on (0..1024). Portable.
#pragma once
#include <stdint.h>

namespace loop_pi {

constexpr uint32_t kRingFrames = 4194304;   // 4,194,304 stereo int32 frames = 32 MB = 87.38 s at 48 kHz
constexpr uint64_t kRingBytes = (uint64_t)kRingFrames * 8;
constexpr int kLoopramMax = 1024;

// the grab store in MB for a board with mem_mb of RAM; opt: the cmdline's loopram=MB, -1 not given. 0: the looper is off
inline unsigned StoreMB(uint64_t mem_mb, int opt) {
  // at most a quarter of the board's RAM (and 1024): loopram=1024 on a 2 GB board would starve the pool
  unsigned cap = mem_mb / 4 < (uint64_t)kLoopramMax ? (unsigned)(mem_mb / 4) : (unsigned)kLoopramMax;
  if (opt >= 0) return (unsigned)opt > cap ? cap : (unsigned)opt;
  return mem_mb >= 6000 ? 256 : mem_mb >= 3000 ? 192 : mem_mb >= 1500 ? 96 : 0;
}

}  // namespace loop_pi
