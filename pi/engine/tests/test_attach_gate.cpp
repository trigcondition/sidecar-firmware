// pi/engine/tests/test_attach_gate.cpp - attach_gate.h: a box held from the computer until its audio runs both ways,
// restarted and then re-enumerated while starved, shown late at the hold limit; a shown box's starvation restarts it
// at most kShownRestarts times a stream. LT9's restore boot (2026-11-23: Rytm OUT 0/s from the start) is replayed.
#include <cstdio>

#include "../attach_gate.h"
#include "check.h"

using namespace obgate;

static const unsigned kIn = 6860;

int main ()
{
	{	// a good boot: shown after kNeedSeconds full seconds
		Gate g;
		begin (g, 8000);
		CHECK (g.held && g.holds == 1);
		CHECK (tick (g, kIn, kIn, true, 9000, kHoldMs, true) == Wait);
		CHECK (tick (g, kIn, kIn, true, 10000, kHoldMs, true) == Show);
		CHECK (!g.held && g.last_wait_ms == 2000 && g.total_restarts == 0);
		CHECK (tick (g, 0, 0, true, 11000, kHoldMs, true) == Wait);	// not held: nothing
	}
	{	// the first line after the stream starts is partial (IN ramping): it does not count, nor does it restart
		Gate g;
		begin (g, 0);
		CHECK (tick (g, 3809, 3700, true, 1000, kHoldMs, true) == Wait);
		CHECK (tick (g, kIn, kIn, true, 2000, kHoldMs, true) == Wait);
		CHECK (tick (g, kIn, kIn, true, 3000, kHoldMs, true) == Show);
	}
	{	// LT9: OUT 0/s - restart at 3 s; still dead - re-enumerate 3 s later; the box back (a new object) and full: shown
		Gate g;
		begin (g, 8670);
		uint32_t t = 8670;
		Act a = Wait;
		int n = 0;
		while ((a = tick (g, kIn, 0, true, t += 1000, kHoldMs, true)) == Wait) n++;
		CHECK (a == Restart && n == 2 && g.restarts == 1 && g.held);
		while ((a = tick (g, kIn, 0, true, t += 1000, kHoldMs, true)) == Wait) {}
		CHECK (a == ReEnum && g.reenum_due && g.total_reenums == 1);
		uint32_t first = g.since_ms;
		begin (g, t + 800);			// the box enumerated again: the same hold, the same limit
		CHECK (g.held && !g.reenum_due && g.since_ms == first && g.holds == 1 && g.restarts == 1);
		CHECK (tick (g, kIn, kIn, true, t + 1800, kHoldMs, true) == Wait);
		CHECK (tick (g, kIn, kIn, true, t + 2800, kHoldMs, true) == Show);
		CHECK (g.last_wait_ms == t + 2800 - first && g.last_wait_ms <= kHoldMs);
	}
	{	// never recovers: restart, re-enumeration, then shown late at the hold limit (not later), once
		Gate g;
		begin (g, 0);
		uint32_t t = 0;
		Act a;
		unsigned restarts = 0, reenums = 0;
		while ((a = tick (g, kIn, 0, true, t += 1000, kHoldMs, true)) != ShowLate)
		{
			if (a == Restart) restarts++;
			if (a == ReEnum) { reenums++; begin (g, t + 500); }
			CHECK (t < 20000);
		}
		CHECK (restarts == 1 && reenums == 1 && t == kHoldMs && g.shown_late == 1 && !g.held);
	}
	{	// obgatereenum=0: the restart only, then shown late
		Gate g;
		begin (g, 0);
		uint32_t t = 0;
		Act a;
		while ((a = tick (g, kIn, 0, true, t += 1000, kHoldMs, false)) != ShowLate) CHECK (a != ReEnum && t < 20000);
		CHECK (g.total_restarts == 1 && g.total_reenums == 0);
	}
	{	// obout=off: OUT is not judged
		Gate g;
		begin (g, 0);
		CHECK (tick (g, kIn, 0, false, 1000, kHoldMs, true) == Wait);
		CHECK (tick (g, kIn, 0, false, 2000, kHoldMs, true) == Show);
	}
	{	// a short OUT (k = 6 of 7 services a ms, 5,906/s) is not full either
		CHECK (!full (kIn, 5906, true) && full (kIn, 6851, true) && !full (0, 0, false));
		CHECK (full (6000, 5995, true) && full (4461, 4459, true));	// the emulator's slower stream: OUT keeps up
		CHECK (!full (2952, 2952, true) && !full (kIn, 0, true));	// IN served 3 times a ms; LT9's OUT
	}
	{	// a whole stream slower than a box's (the emulator): shown, not restarted
		Gate g;
		begin (g, 0);
		CHECK (tick (g, 6014, 6015, true, 1000, kHoldMs, true) == Wait);
		CHECK (tick (g, 5940, 5936, true, 2000, kHoldMs, true) == Show);
	}
	{	// unplugged while held: the hold ends; the next attach is a hold of its own
		Gate g;
		begin (g, 0);
		tick (g, kIn, 0, true, 1000, kHoldMs, true);
		gone (g);
		CHECK (!g.held);
		begin (g, 30000);
		CHECK (g.since_ms == 30000 && g.holds == 2 && g.restarts == 0);
	}
	{	// re-enumerated by the gate but back only much later: a hold of its own, not shown late at once
		Gate g;
		begin (g, 0);
		g.reenum_due = true;
		gone (g);
		CHECK (g.held);				// (the gate's own removal: the hold goes on)
		begin (g, 60000);
		CHECK (g.since_ms == 60000 && g.holds == 2);
	}
	{	// a shown box's starvation: at most kShownRestarts restarts a stream
		unsigned n = 0;
		CHECK (starved (n) && starved (n) && !starved (n) && n == kShownRestarts);
	}
	CHECK (act_name (ReEnum)[0] == 'r' && act_name (Wait)[0] == 'w');
	printf ("test_attach_gate: PASS\n");
	return 0;
}
