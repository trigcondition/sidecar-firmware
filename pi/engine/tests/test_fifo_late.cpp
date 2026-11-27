// fifo_late.h: the FIFO's lateness for the measured FX guard (cores.cpp).
#include <stdio.h>

#include "check.h"
#include "fifo_late.h"

static const uint32_t kBlock = 12, kTarget = 96;
static void warm(fifo_late::State& s) {
  for (unsigned i = 0; i < fifo_late::State::kGrace + fifo_late::State::kHold; ++i) s.step(kTarget, 0, kBlock, true);
}

int main() {
  {  // after a stream start: no lateness for the grace, even from an empty FIFO
    fifo_late::State s;
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) CHECK(!s.step(0, 0, kBlock, true));
    CHECK(!s.step(0, 1, kBlock, true));   // a new stream: the grace again
  }
  {  // LT12h P5's shape: each control pass renders 8 blocks, the first from a FIFO near empty, the rest refilled.
     // From the second pass every block counts late (4 in a row: the late rule acts)
    fifo_late::State s;
    warm(s);
    unsigned run = 0, best = 0, first = 0, n = 0;
    for (int pass = 0; pass < 10; ++pass)
      for (uint32_t b = 0; b < 8; ++b, ++n) {
        const bool late = s.step(3 + b * kBlock, 0, kBlock, true);
        if (late && !first) first = n;
        run = late ? run + 1 : 0;
        if (run > best) best = run;
      }
    CHECK(best >= 4 && first == 8);
    printf("test_fifo_late: a FIFO short at every pass: late from block %u, %u in a row\n", first, best);
  }
  {  // one long control pass in normal play: the FIFO dips once (2 low blocks while it refills), never late
    fifo_late::State s;
    warm(s);
    for (int i = 0; i < 1000; ++i) CHECK(!s.step(60 + i % 36, 0, kBlock, true));
    bool dip = false;
    CHECK(!s.step(5, 0, kBlock, true, &dip) && dip);
    CHECK(!s.step(17, 0, kBlock, true, &dip) && !dip);   // (the same low run: not a second dip)
    for (int i = 0; i < 200; ++i) CHECK(!s.step(29 + i % 60, 0, kBlock, true));
    // ... and another long pass well after it: still one dip in the window
    CHECK(!s.step(10, 0, kBlock, true, &dip) && dip);
    for (int i = 0; i < 40; ++i) CHECK(!s.step(60, 0, kBlock, true));
  }
  {  // the mark: 2 blocks (LT12i P5): 23 frames is low, 24 is not
    fifo_late::State s;
    warm(s);
    CHECK(!s.step(24, 0, kBlock, true) && !s.step(60, 0, kBlock, true) && !s.step(24, 0, kBlock, true));
    bool dip = false;
    s.step(23, 0, kBlock, true, &dip);
    CHECK(dip);
    s.step(60, 0, kBlock, true);
    CHECK(!s.step(23, 0, kBlock, true));   // the second dip within the window, but both shallow: not late (LT14 P6)
    s.step(60, 0, kBlock, true);
    CHECK(s.step(11, 0, kBlock, true));   // ... a third, under a block, with the second in the window: late
    for (unsigned i = 1; i < fifo_late::State::kHold; ++i) CHECK(s.step(90, 0, kBlock, true));
    CHECK(!s.step(90, 0, kBlock, true));
  }
  {  // fxguard=fifo:off: never, and the hold ends at once
    fifo_late::State s;
    warm(s);
    s.step(0, 0, kBlock, true), s.step(60, 0, kBlock, true);
    CHECK(s.step(0, 0, kBlock, true));
    CHECK(!s.step(0, 0, kBlock, false));
    CHECK(!s.step(90, 0, kBlock, true));
  }
  {  // held(): the hold seen from the other FIFO's passes, never in the grace
    fifo_late::State s;
    CHECK(!s.held());
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) s.step(90, 0, kBlock, true);
    CHECK(!s.held());
    s.step(0, 0, kBlock, true), s.step(90, 0, kBlock, true);
    CHECK(!s.held());
    CHECK(s.step(0, 0, kBlock, true) && s.held());   // the second dip: held for the other passes too
    for (unsigned i = 1; i < fifo_late::State::kHold; ++i) s.step(90, 0, kBlock, true);
    CHECK(!s.held());
    printf("test_fifo_late: held() for the other FIFO's passes\n");
  }
  {  // rearm() after a late mute: the hold ends, the next needs two new dips (LT12k)
    fifo_late::State s;
    warm(s);
    s.step(0, 0, kBlock, true), s.step(90, 0, kBlock, true);
    CHECK(s.step(0, 0, kBlock, true));   // held
    s.rearm();
    CHECK(!s.held() && !s.step(0, 0, kBlock, true));   // still low: no new dip
    CHECK(!s.step(90, 0, kBlock, true) && !s.step(0, 0, kBlock, true));   // one new dip: not yet
    CHECK(!s.step(90, 0, kBlock, true) && s.step(0, 0, kBlock, true));    // the second: late again
    printf("test_fifo_late: rearm() takes two new dips\n");
  }
  {  // LT13 P5: an overload the control loop cannot refill: the FIFO low at every pass, one dip, yet late once a low run
     // gains nothing over kArm passes, and late while it stays so
    fifo_late::State s;
    warm(s);
    unsigned first = 0;
    for (unsigned i = 1; i <= 200; ++i)
      if (s.step(3 + i % 5, 0, kBlock, true) && !first) first = i;
    CHECK(first > 0 && first <= 2 * fifo_late::State::kArm);
    for (unsigned i = 0; i < 100; ++i) CHECK(s.step(2, 0, kBlock, true));
    printf("test_fifo_late: a FIFO pinned low: late from pass %u of the low run\n", first);
  }
  {  // ... but a FIFO climbing back slowly after one long control pass near a full load (a frame a pass) never is
    fifo_late::State s;
    warm(s);
    for (unsigned i = 0; i < 2 * kBlock; ++i) CHECK(!s.step(i, 0, kBlock, true));
    for (unsigned i = 0; i < 200; ++i) CHECK(!s.step(60, 0, kBlock, true));
  }
  {  // Pair: a side pass late while the base FIFO's hold is on; rearm ends both
    fifo_late::Pair p;
    for (unsigned i = 0; i < fifo_late::State::kGrace + fifo_late::State::kHold; ++i)
      p.step(false, kTarget, 0, kBlock, true, true), p.step(true, kTarget, 0, kBlock, true, true);
    for (unsigned i = 0; i < fifo_late::State::kArm; ++i) p.step(false, 1, 0, kBlock, true, true), p.step(true, kTarget, 0, kBlock, true, true);
    CHECK(p.step(true, kTarget, 0, kBlock, true, true));
    p.rearm();
    CHECK(!p.step(true, kTarget, 0, kBlock, true, true));
    CHECK(fifo_late::blame(90, 160, 120, 70) == 1 && fifo_late::blame(90, 0, 120, 0) == 2);
  }
  {  // LT14 P6 (T511): shallow dips at 1200 MHz (FIFO min 30 a second, low runs of 2 passes, no underrun): never late
    fifo_late::State s;
    warm(s);
    for (int i = 0; i < 2000; ++i) CHECK(!s.step(i % 20 < 2 ? 14 + i % 7 : 40 + i % 40, 0, kBlock, true));
    // ... but the same with one dip under a block is (the second of the pair deep)
    s.step(20, 0, kBlock, true), s.step(60, 0, kBlock, true);
    CHECK(s.step(4, 0, kBlock, true));
    printf("test_fifo_late: shallow dips never late, a deep second dip is\n");
  }
  {  // Load: a core's time a block (its last base + side pass) over a block, a running mean
    fifo_late::Load l;
    const uint64_t block = 1000;
    CHECK(l.bp(block) == 0);
    for (int i = 0; i < 400; ++i) l.step(740);   // VA1 O1 P1: core 1 at 74 %
    CHECK(l.bp(block) >= 7350 && l.bp(block) <= 7400);
    CHECK(!fifo_late::counts(l.bp(block), 8000, false));
    unsigned n = 0;   // LT12g P5: two slots slower, 83 %: over the limit within a few ms of passes
    while (!fifo_late::counts(l.bp(block), 8000, false) && n < 1000) l.step(830), ++n;
    CHECK(n > 0 && n <= 64);
    for (int i = 0; i < 400; ++i) l.step(830);
    CHECK(l.bp(block) >= 8250 && l.bp(block) <= 8300);
    printf("test_fifo_late: Load: 74 %% fits, 83 %% over the limit after %u passes\n", n);
  }
  {  // Under: an underflow counts for kRecent passes, then no more; the first reading is not an underrun
    fifo_late::Under u;
    u.step(2568);
    CHECK(!u.recent());
    for (int i = 0; i < 100; ++i) u.step(2568);
    CHECK(!u.recent());
    u.step(2572);   // VA1 O1 P1: +4
    CHECK(u.recent());
    for (unsigned i = 1; i < fifo_late::Under::kRecent; ++i) {
      u.step(2572);
      CHECK(u.recent());
    }
    u.step(2572);
    CHECK(!u.recent());
    CHECK(fifo_late::counts(7400, 8000, true) && fifo_late::counts(8000, 8000, false) && !fifo_late::counts(7999, 8000, false));
    printf("test_fifo_late: Under: an underrun counts for %u passes\n", fifo_late::Under::kRecent);
  }
  printf("test_fifo_late: PASS\n");
  return 0;
}
