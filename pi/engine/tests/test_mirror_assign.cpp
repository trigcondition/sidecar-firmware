// test_mirror_assign.cpp - pi/engine/mirror_assign.h: which box USB-C shows and which the Teensy relay shows, decided at
// run time (USB-C first, the preferred box when both are there, nobody moved away while there, the hold, the settle).
#include "../mirror_assign.h"
#include "check.h"
#include <cstdio>

using namespace mirror_assign;

static const uint64_t S = 1000000;	// a second in us

// the box of slot k comes up / leaves at t; then the assignment as ob_usb.cpp's PollAssign makes it
static void up (State &s, int k, uint64_t t)	{ s.up[k] = true, s.up_at[k] = t; }
static void gone (State &s, int k, uint64_t t)	{ s.up[k] = false, s.gone_at[k] = t; }
static int step (const Config &c, State &s, uint64_t t) { return s.first = decide (c, s, t); }

int main () {
  Config rytm;			// mirror=rytm (the default): settle 15 s, hold 10 s
  Config a4;
  a4.prefer = 1;
  Config first;
  first.prefer = kFirst;

  CHECK (parse_prefer (nullptr) == 0 && parse_prefer ("rytm") == 0 && parse_prefer ("a4") == 1);
  CHECK (parse_prefer ("first") == kFirst && parse_prefer ("A4") == 0 && parse_prefer ("a44") == 0);
  CHECK (other_slot (0) == 1 && other_slot (1) == 0 && other_slot (kNone) == kNone);
  CHECK (!parse_relay_first (nullptr) && !parse_relay_first ("usbc") && parse_relay_first ("relay") && !parse_relay_first ("relays"));
  {  // the routes: USB-C first (the default) or the relay first
    Config u, r;
    r.relay_first = true;
    CHECK (usbc_slot (u, 1) == 1 && relay_slot (u, 1) == 0 && usbc_slot (u, kNone) == kNone && relay_slot (u, kNone) == kNone);
    CHECK (usbc_slot (r, 1) == 0 && relay_slot (r, 1) == 1 && usbc_slot (r, kNone) == kNone && relay_slot (r, kNone) == kNone);
  }

  {  // nothing there: nothing shown
    State s;
    CHECK (step (rytm, s, 0) == kNone && step (rytm, s, 100 * S) == kNone);
  }
  {  // the Rytm alone (preferred): USB-C at once
    State s;
    up (s, 0, 3 * S);
    CHECK (step (rytm, s, 3 * S) == 0);
  }
  {  // the A4 alone, mirror=rytm: USB-C waits out the settle time for the Rytm, then the A4 takes it
    State s;
    up (s, 1, 4 * S);
    CHECK (step (rytm, s, 4 * S) == kNone);
    CHECK (step (rytm, s, 15 * S - 1) == kNone);
    CHECK (step (rytm, s, 15 * S) == 1);
  }
  {  // the A4 alone after the settle time (hotplug): USB-C at once
    State s;
    up (s, 1, 40 * S);
    CHECK (step (rytm, s, 40 * S) == 1);
  }
  {  // boot, mirror=rytm: the A4 first, the Rytm 6 s later, inside the settle time: Rytm USB-C, A4 the relay
    State s;
    up (s, 1, 5 * S);
    CHECK (step (rytm, s, 5 * S) == kNone);
    up (s, 0, 11 * S);
    CHECK (step (rytm, s, 11 * S) == 0 && other_slot (s.first) == 1);
  }
  {  // boot, mirror=a4: the Rytm first: it waits; the A4 comes: A4 USB-C, Rytm the relay
    State s;
    up (s, 0, 2 * S);
    CHECK (step (a4, s, 2 * S) == kNone);
    up (s, 1, 9 * S);
    CHECK (step (a4, s, 9 * S) == 1 && other_slot (s.first) == 0);
  }
  {  // the preferred box arrives after the other took USB-C: it goes to the relay, nobody is moved
    State s;
    up (s, 1, 30 * S);
    CHECK (step (rytm, s, 30 * S) == 1);
    up (s, 0, 50 * S);
    CHECK (step (rytm, s, 50 * S) == 1 && other_slot (s.first) == 0);
    CHECK (step (rytm, s, 500 * S) == 1);
  }
  {  // the USB-C box reboots inside the hold (an OS update): it keeps USB-C, the relay's box stays there
    State s;
    up (s, 0, 1 * S), up (s, 1, 2 * S);
    CHECK (step (rytm, s, 2 * S) == 0);
    gone (s, 0, 100 * S);
    CHECK (step (rytm, s, 100 * S) == 0 && step (rytm, s, 110 * S - 1) == 0);
    up (s, 0, 105 * S);
    CHECK (step (rytm, s, 105 * S) == 0 && step (rytm, s, 200 * S) == 0);
  }
  {  // the USB-C box stays away past the hold: the relay's box moves to USB-C; the first one back goes to the relay
    State s;
    up (s, 0, 1 * S), up (s, 1, 2 * S);
    CHECK (step (rytm, s, 2 * S) == 0);
    gone (s, 0, 100 * S);
    CHECK (step (rytm, s, 110 * S - 1) == 0);
    CHECK (step (rytm, s, 110 * S) == 1);
    up (s, 0, 130 * S);
    CHECK (step (rytm, s, 130 * S) == 1 && other_slot (s.first) == 0);
  }
  {  // the USB-C box leaves with nothing else there: held, then free; it (or the other) takes it again on arrival
    State s;
    up (s, 1, 1 * S);
    CHECK (step (a4, s, 1 * S) == 1);
    gone (s, 1, 60 * S);
    CHECK (step (a4, s, 65 * S) == 1 && step (a4, s, 70 * S) == kNone);
    up (s, 0, 80 * S);
    CHECK (step (a4, s, 80 * S) == 0);
  }
  {  // a longer hold (cmdline mirrorhold=60)
    Config c = rytm;
    c.hold_us = 60 * S;
    State s;
    up (s, 0, 1 * S), up (s, 1, 1 * S);
    CHECK (step (c, s, 1 * S) == 0);
    gone (s, 0, 100 * S);
    CHECK (step (c, s, 159 * S) == 0 && step (c, s, 160 * S) == 1);
  }
  {  // the relay's box leaves and comes back: USB-C untouched
    State s;
    up (s, 0, 1 * S), up (s, 1, 1 * S);
    CHECK (step (rytm, s, 1 * S) == 0);
    gone (s, 1, 20 * S);
    CHECK (step (rytm, s, 20 * S) == 0 && step (rytm, s, 200 * S) == 0);
    up (s, 1, 210 * S);
    CHECK (step (rytm, s, 210 * S) == 0);
  }
  {  // mirror=first: no settle wait; whichever came first; the same moment: the Rytm
    State s;
    up (s, 1, 2 * S);
    CHECK (step (first, s, 2 * S) == 1);
    up (s, 0, 3 * S);
    CHECK (step (first, s, 3 * S) == 1);
    State t;
    up (t, 0, 5 * S), up (t, 1, 4 * S);
    CHECK (step (first, t, 5 * S) == 1);
    State u;
    up (u, 0, 5 * S), up (u, 1, 5 * S);
    CHECK (step (first, u, 5 * S) == 0);
  }
  {  // both there and USB-C free (the held box came back after the hold ran out, with the other there): the preferred one
    State s;
    s.first = kNone;
    up (s, 0, 100 * S), up (s, 1, 90 * S);
    CHECK (step (rytm, s, 100 * S) == 0);
    CHECK (step (a4, s, 100 * S) == 0);	// (sticky once chosen: a4 preference does not move the Rytm off)
  }
  {  // mirrorfirst=relay (testing the relay): the lone box goes to the relay, the second one to USB-C, the same rules
    Config c;
    c.relay_first = true;
    State s;
    up (s, 1, 40 * S);
    CHECK (step (c, s, 40 * S) == 1 && relay_slot (c, s.first) == 1 && usbc_slot (c, s.first) == 0);
    up (s, 0, 50 * S);
    CHECK (step (c, s, 50 * S) == 1 && usbc_slot (c, s.first) == 0);	// the Rytm on USB-C, the A4 stays on the relay
    gone (s, 1, 60 * S);
    CHECK (step (c, s, 69 * S) == 1 && step (c, s, 70 * S) == 0 && relay_slot (c, s.first) == 0);	// the Rytm moves to the relay
  }
  std::printf ("test_mirror_assign: ok\n");
  return 0;
}
