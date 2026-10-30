// core/tests/timing.h - the timing tests' wall-clock checks, made load-robust (final review, 2026-10-26).
//
// A test's deterministic checks (the card time the card model charges the control core, step and callback counts)
// stay plain CHECKs: they fail loud whatever the Mac is doing. A check of measured time cannot: a pass preempted by
// another process, or a benchmark sharing the CPU with the parallel suite, reads as slow. Two rules, one per kind:
//   - a repeatable measured phase (a benchmark: N calls, their mean) runs up to kTimingTries times and fails only
//     when every try is over its bound (timing_retry; each try over is printed);
//   - a stateful run's passes (an export, a load: not repeatable) are timed on the thread's own CPU clock
//     (timing_cpu_us: a wait while preempted is not counted) and fail when more than kTimingSpikes passes are over
//     the bound - none: preemption is not in the CPU clock, so any pass over is the code's (final re-review: one
//     14 ms pass at a project switch is exactly the regression these tests exist for).
// The Makefile's TIMING_TESTS run one at a time after the parallel batch.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <time.h>

constexpr int kTimingTries = 3;
constexpr uint32_t kTimingSpikes = 0;   // a stateful run's passes over its bound, on the thread CPU clock

// the calling thread's CPU time in ns / us
inline uint64_t timing_cpu_ns() {
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
inline uint64_t timing_cpu_us() { return timing_cpu_ns() / 1000u; }

// try_once(): measures once, true when within its bound. true once a try is; false when all kTimingTries were over
template <class F>
bool timing_retry(const char* what, F try_once) {
  for (int k = 1; k <= kTimingTries; ++k) {
    if (try_once()) return true;
    printf("timing: %s over its bound (try %d of %d)\n", what, k, kTimingTries);
  }
  return false;
}
