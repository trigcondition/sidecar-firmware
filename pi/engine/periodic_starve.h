// pi/engine/periodic_starve.h - a box's audio OUT starved by the USB host controller, named in the log.
//
// On the Pi 4's VL805, once about 6 interrupt endpoints polled every microframe (bInterval 1) are busy, one of them is
// served only k = 0..7 times a millisecond: a box's OUT then runs in whole steps of one transfer a frame (984.4/s with
// Circle's 64-TRB rings, 1000 x 63/64) while every OUT URB sits at the controller (XV1 T421, XR1 T422,
// diagnosis-periodic-starvation.md). The driver is not holding anything back then, so the shortfall is the controller's.
//
// CObUsb::Log (once a second) calls Tick with the second's IN and OUT rates (blocks/s) and whether every OUT URB was
// queued at the controller (driver pending and xHCI pending both at the URB count). Short = OUT below IN by more than
// 1 % with IN alive (>= kMinIn/s) and steady (within 1 % of the second before: IV2 T424 A1 fired at 5 endpoints in
// the ramp before a start-of-stream stall, IN 3809 -> 6863 -> 6861, OUT 6631 - not the controller's starvation; after
// a ramp the first two lines are not steady, so a real one is named up to 2 s later). After kSeconds short seconds in
// a row Tick says Start (one "periodic starvation" line); the first second that is not short after that says Over
// (one line, with how long it lasted). Nothing between: the per-second rate line already shows each second.
//
// Steps(): k, the services a millisecond the OUT got, from its rate, the ring size and blocks per transfer.
// Portable: tests/test_periodic_starve.cpp.
#pragma once

struct PeriodicStarve {
  static constexpr unsigned kSeconds = 3;      // short seconds in a row before it is named
  static constexpr unsigned kMinIn = 1000;     // IN blocks/s below this: the stream is not running, nothing judged
  enum Say { None, Start, Over };

  bool on = false;          // named and not over yet
  unsigned run = 0;         // short seconds in a row
  unsigned events = 0;      // times named since the stream started
  unsigned lasted = 0;      // Over: the short seconds of the event that just ended
  unsigned prev_in = 0;     // the second before's IN rate (0: none yet)

  static bool Short(unsigned in_rate, unsigned out_rate, bool all_queued) {
    return all_queued && in_rate >= kMinIn && (unsigned long long)out_rate * 100 < (unsigned long long)in_rate * 99;
  }

  static bool Steady(unsigned in_rate, unsigned prev) {
    unsigned d = in_rate > prev ? in_rate - prev : prev - in_rate;
    return prev > 0 && (unsigned long long)d * 100 <= prev;
  }

  Say Tick(unsigned in_rate, unsigned out_rate, bool all_queued) {
    bool steady = Steady(in_rate, prev_in);
    prev_in = in_rate;
    if (steady && Short(in_rate, out_rate, all_queued)) {
      run++;
      if (!on && run >= kSeconds) { on = true; events++; return Start; }
      return None;
    }
    Say s = None;
    if (on) { on = false; lasted = run; s = Over; }
    run = 0;
    return s;
  }

  // services a millisecond: rate / (one transfer a frame, less the link TRB's share) / blocks a transfer, rounded
  static unsigned Steps(unsigned out_rate, unsigned ring_trbs, unsigned blocks) {
    if (ring_trbs < 2 || blocks == 0) return 0;
    unsigned long long step = 1000ull * (ring_trbs - 1) * blocks;          // x ring_trbs
    return (unsigned)(((unsigned long long)out_rate * ring_trbs + step / 2) / step);
  }
};
