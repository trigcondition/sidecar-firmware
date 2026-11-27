// test_periodic_starve.cpp - pi/engine/periodic_starve.h: a box's OUT short with every URB at the controller, named
// after 3 s in a row and once over; k from the rate (984.4/s steps with 64-TRB rings, 996.1/s with 256, XV1/XR1).
#include "../periodic_starve.h"
#include "check.h"
#include <cstdio>

int main () {
  using P = PeriodicStarve;
  // short: more than 1 % under IN, every URB queued, IN running
  CHECK (P::Short (6860, 5906, true));            // k 6 (PD6)
  CHECK (!P::Short (6860, 5906, false));          // URBs free: the driver's doing, not the controller's
  CHECK (!P::Short (6860, 6860, true));           // full
  CHECK (!P::Short (6860, 6800, true));           // within 1 %
  CHECK (P::Short (6860, 6780, true));            // 1.2 % under
  CHECK (P::Short (6860, 0, true));               // XR1: 40 lines at 0/s
  CHECK (!P::Short (500, 0, true));               // IN not running: not judged
  // steady IN: within 1 % of the second before
  CHECK (P::Steady (6861, 6863) && P::Steady (6860, 6800) && !P::Steady (6863, 3809) && !P::Steady (6860, 0));
  { // IV2 T424 A1, the lines before the start-of-stream stall (OUT/IN): IN ramping, so never named
    P s;
    const unsigned in[] = {6860, 3809, 6863, 6861, 3128, 0}, out[] = {6860, 3755, 6700, 6631, 2946, 0};
    for (int i = 0; i < 6; i++) CHECK (s.Tick (in[i], out[i], true) == P::None);
    CHECK (s.events == 0);
  }
  { // IV2 S1, the A4 after a ramp: named on the 5th line (the ramp's line and the one after it are not steady)
    P s;
    CHECK (s.Tick (6860, 6860, true) == P::None);
    CHECK (s.Tick (3969, 2566, true) == P::None);
    CHECK (s.Tick (6860, 3933, true) == P::None);
    CHECK (s.Tick (6858, 3606, true) == P::None);
    CHECK (s.Tick (6861, 3940, true) == P::None);
    CHECK (s.Tick (6864, 3940, true) == P::Start);
  }
  { // 3 short seconds name it once; over on the first full second; again later counts a second event
    P s;
    s.Tick (6860, 6860, true);   // a steady second before
    CHECK (s.Tick (6860, 984, true) == P::None);
    CHECK (s.Tick (6860, 984, true) == P::None);
    CHECK (s.Tick (6860, 984, true) == P::Start && s.on && s.events == 1);
    CHECK (s.Tick (6860, 1968, true) == P::None);  // still short: no new line
    CHECK (s.Tick (6860, 6860, true) == P::Over && !s.on && s.lasted == 4);
    CHECK (s.Tick (6860, 6860, true) == P::None);
    for (int i = 0; i < 2; i++) CHECK (s.Tick (6860, 3938, true) == P::None);
    CHECK (s.Tick (6860, 3938, true) == P::Start && s.events == 2);
  }
  { // a full second between short ones starts the count again
    P s;
    s.Tick (6860, 6860, true);
    s.Tick (6860, 984, true); s.Tick (6860, 984, true);
    CHECK (s.Tick (6860, 6860, true) == P::None && s.run == 0);
    s.Tick (6860, 984, true); s.Tick (6860, 984, true);
    CHECK (s.Tick (6860, 984, true) == P::Start);
  }
  { // a second with a free URB breaks the run too, and so does an unsteady IN
    P s;
    s.Tick (6860, 6860, true);
    s.Tick (6860, 984, true); s.Tick (6860, 984, true);
    CHECK (s.Tick (5000, 984, true) == P::None && s.run == 0);
    s.Tick (6860, 984, true); s.Tick (6860, 984, true);
    CHECK (s.Tick (6860, 984, false) == P::None && s.run == 0);
  }
  // k: one transfer a frame less the link TRB's share
  CHECK (P::Steps (984, 64, 1) == 1 && P::Steps (3938, 64, 1) == 4 && P::Steps (5906, 64, 1) == 6);
  CHECK (P::Steps (6860, 64, 1) == 7 && P::Steps (0, 64, 1) == 0);
  CHECK (P::Steps (5977, 256, 1) == 6 && P::Steps (4980, 256, 1) == 5);   // XV1: 996.1/s steps
  CHECK (P::Steps (1968, 64, 2) == 1);                                      // obblocks=2: two blocks a transfer
  CHECK (P::Steps (100, 1, 1) == 0 && P::Steps (100, 64, 0) == 0);
  printf ("test_periodic_starve: PASS\n");
  return 0;
}
