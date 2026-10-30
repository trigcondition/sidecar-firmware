// core/seq/rng.h - E1 (docs/SEQ-IDEAS.md "E1 and E2"): the sequencer's one seeded random generator and its step map.
// Pure and header-only (no state, no card, no clock): any core, the tests, the planner.
//
// The RNG is a hash, not a stream: rng_key(seed, track, step, loop, purpose, k) gives the same 32 bits for the same
// inputs however often a step is planned (a replan after an edit, a predicted pattern switch, a reload). Each random
// decision names its purpose (kRng*), so the probability of a step and a later random note on the same step never
// share a draw. k separates several draws of one purpose on one step (a chord's n random notes).
//
// The step map: step_for(len, abs) -> {index, loop}, the one place that turns the Rytm's linear step count since PLAY
// (or a pattern switch) into the step that plays and the loop it is in. Forward only today (index = abs mod len, loop =
// abs / len: exactly what the planner, the playhead, the recorder and A:B counted before); play directions (P1), step
// loops (P3) and jumps (V3) are later changes of this function only. Constraint 1 of SEQ-IDEAS: it changes which step
// plays at a tick, never when the tick comes.
#pragma once
#include <stdint.h>

namespace core {
namespace seq {

// What a draw is for (the hash's "purpose" input). Fixed numbers: a saved seed must give the same results in a later
// build, so a purpose is never renumbered (new ones are appended).
enum : uint8_t { kRngProb = 1, kRngNote = 2, kRngJump = 3, kRngDir = 4, kRngMutate = 5, kRngHuman = 6, kRngMorph = 7,
                 kRngLock = 8, kRngArp = 9, kRngFx = 10,     // seqtop: V1's lock chances, M4's RND arp, H1's FX column
                 kRngShuffle = 11, kRngNbr = 12 };            // CHOP (core/chop): SHUFFLE's order, NEIGHBOUR CHANCE

inline uint64_t rng_mix(uint64_t z) {   // splitmix64's finaliser: every input bit moves every output bit
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
// 32 random bits for (seed, track, step, loop, purpose, k). loop < 0: the loop is left out ("same every loop").
inline uint32_t rng_key(uint32_t seed, int track, int step, int64_t loop, int purpose, int k = 0) {
  uint64_t h = rng_mix(0x5EC5EED0ull ^ seed);
  h = rng_mix(h ^ ((uint64_t)(uint8_t)track << 56 | (uint64_t)(uint16_t)step << 40 | (uint64_t)(uint8_t)purpose << 32 |
                   (uint32_t)k));
  h = rng_mix(h ^ (uint64_t)(loop < 0 ? -1 : loop));
  return (uint32_t)(h >> 32);
}
// A draw in 0..n-1 (n >= 1), without the modulo's bias toward the low values that matters at these sizes
inline int rng_below(uint32_t r, int n) { return n <= 1 ? 0 : (int)(((uint64_t)r * (uint32_t)n) >> 32); }
// A percent test: true with probability pct / 100 (pct 0..100)
inline bool rng_chance(uint32_t r, int pct) { return rng_below(r, 100) < pct; }

struct StepAt {
  int index;      // the step that plays, 0..len-1
  int64_t loop;   // the loop it is in (what A:B and 1ST count), 0 the first
};
inline StepAt step_for(int len, int64_t abs) {
  if (len < 1) len = 1;
  if (abs < 0) return {0, 0};
  return {(int)(abs % len), abs / len};
}

// seqtop P1: the play directions (a track's; the ids are the file's, never renumbered). The order changes, never the time
// (constraint 1): step_for maps the track's step count to the step that plays.
//   FWD 0 1 2 3 | REV 3 2 1 0 | PEND 0 1 2 3 2 1 (the ends once; a loop is 2 len - 2) | PING 0 1 2 3 3 2 1 0 (the ends
//   twice; 2 len) | RND any step, a keyed draw per position | BROWN a random walk from step 0 each loop (+1 mostly,
//   sometimes 0, -1 or +2), so it keeps the riff's local shape.
// RND and BROWN draw from the keyed RNG (seed, track, the position in the loop, the loop: kRngDir), so the same count
// always gives the same step (a replan, a predicted switch, a second PLAY with a fixed seed); same: the loop left out (every
// loop the same order). The loop (what A:B and 1ST count) is one cycle of the direction.
enum : uint8_t { kDirFwd = 0, kDirRev, kDirPend, kDirPing, kDirRnd, kDirBrown, kDirs };
inline int dir_cycle(int len, int dir) {
  if (len < 1) len = 1;
  return dir == kDirPend ? (len > 1 ? 2 * len - 2 : 1) : dir == kDirPing ? 2 * len : len;
}
inline int brown_delta(uint32_t r) {   // +1 50 %, 0 15 %, -1 20 %, +2 15 %
  int x = rng_below(r, 100);
  return x < 50 ? 1 : x < 65 ? 0 : x < 85 ? -1 : 2;
}
inline StepAt step_for(int len, int64_t abs, int dir, uint32_t seed, int track, bool same) {
  if (len < 1) len = 1;
  if (abs < 0) abs = 0;
  if (dir <= kDirFwd || dir >= kDirs) return step_for(len, abs);
  int cyc = dir_cycle(len, dir);
  int pos = (int)(abs % cyc);
  int64_t loop = abs / cyc, key = same ? -1 : loop;
  switch (dir) {
    case kDirRev: return {len - 1 - pos, loop};
    case kDirPend: return {pos < len ? pos : cyc - pos, loop};
    case kDirPing: return {pos < len ? pos : cyc - 1 - pos, loop};
    case kDirRnd: return {rng_below(rng_key(seed, track, pos, key, kRngDir), len), loop};
    default: {   // kDirBrown: walked from step 0 at the loop's start (at most len - 1 draws)
      int idx = 0;
      for (int i = 1; i <= pos; ++i) idx = ((idx + brown_delta(rng_key(seed, track, i, key, kRngDir))) % len + len) % len;
      return {idx, loop};
    }
  }
}

}  // namespace seq
}  // namespace core
