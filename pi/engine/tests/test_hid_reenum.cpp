// test_hid_reenum.cpp - pi/circle/hid_reenum.h (when to escalate from resetting a halted USB HID report endpoint to
// re-enumerating the device, pi/circle/circle-hid-reenum.patch) on the Mac.
//
// The first test is the one that matters: it replays the timeline measured on the real GeeekPi panel on 2026-10-25,
// where the escalation stalled at two attempts out of three because Poll() kept seeing the endpoint in state 1
// ("running") just after its own ResetFromHalted() and treated that as health, rewinding the given-up clock.
#include "../../circle/hid_reenum.h"
#include "check.h"
#include <cstdio>
#include <cstring>

using namespace hidreenum;

static const unsigned HZ = 100;			// Circle's tick rate
static const unsigned KeyA = 0x100001;		// root hub port 1, route string 1 (a device on a hub)
static const unsigned KeyB = 0x100002;

// A device being driven by the recovery loop, as CUSBHIDDevice::Poll() sees it.
struct Dev
{
	Hold	 hold;
	unsigned errors;			// total transfer errors (only increases)
	unsigned consecutive;			// the driver's m_nErrorCount
	bool	 armed;				// endpoint state 1 with a request queued

	Dev () { reset (); }

	// A re-enumeration destroys the device and makes a new one: the clocks go, the budget does not.
	void reset ()
	{
		memset (&hold, 0, sizeof hold);
		errors = 0;
		consecutive = 0;
		armed = true;
	}

	Input in (unsigned nowHZ, unsigned key = KeyA) const
	{
		Input i;
		i.nowHZ = nowHZ;
		i.hz = HZ;
		i.Key = key;
		i.Errors = errors;
		i.bGaveUp = consecutive >= 50;		// the driver's MaxConsecutiveErrors
		i.bArmed = armed;
		return i;
	}
};

// Run the recovery for nMs with the device dead: errors pile up, and once the driver has given up, Poll() resets the
// endpoint and resubmits once a second - which leaves the endpoint "running" for a while each time. bSeeRunning
// replays that sighting, which is what broke the first version.
// Returns the first escalation Decide() asked for, and the tick it asked for it at.
struct Run
{
	Step	 step;
	unsigned atHZ;
	unsigned steps;				// how many escalations in all over the run
	Step	 seen[8];			// each one, in order (up to 8)
	unsigned apartHZ;			// the smallest gap between two of them
};

static Run drive_dead (Dev *d, Budget *b, unsigned startHZ, unsigned nMs, bool bSeeRunning)
{
	Run r;
	memset (&r, 0, sizeof r);
	r.step = StepNone;
	r.apartHZ = ~0u;
	unsigned prevHZ = 0;
	unsigned endHZ = startHZ + Ticks (nMs, HZ);
	unsigned lastResubmitHZ = startHZ;

	for (unsigned t = startHZ; t != endHZ; t++)	// one pass per tick: Poll() runs on every main-loop pass
	{
		if (d->consecutive < 50)
		{
			d->errors++;			// the fast burst: the loop resubmits every pass
			d->consecutive++;
			d->armed = false;
		}
		else if (t - lastResubmitHZ >= HZ)	// given up: the loop is throttled to once a second
		{
			lastResubmitHZ = t;
			d->errors++;
			d->consecutive++;
			// the reset and resubmit leave the endpoint running until the error comes back; Poll() can
			// look in exactly that window, and the first version believed it
			d->armed = bSeeRunning;
		}
		else
		{
			d->armed = false;
		}

		Step s = Decide (&d->hold, b, d->in (t));
		if (s != StepNone)
		{
			if (r.step == StepNone) { r.step = s; r.atHZ = t; }
			else if (t - prevHZ < r.apartHZ) r.apartHZ = t - prevHZ;
			prevHZ = t;
			if (r.steps < 8) r.seen[r.steps] = s;
			r.steps++;
			d->reset ();			// the escalation remade the device
		}
	}

	return r;
}

// 1. The measured timeline: a panel that re-halts a few seconds after every re-enumeration must walk the whole
//    ladder - two plain tries, two with the port power cycle - and then say it is spent. This is the regression:
//    with bSeeRunning the first version stalled after two attempts and never set Spent.
static void test_measured_timeline (bool bSeeRunning)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;

	// 2 minutes of a device that is dead and stays dead
	Run r = drive_dead (&d, &b, 1000, 120000, bSeeRunning);

	CHECK (r.step == StepPlain);			// the first escalation is a plain re-enumeration
	CHECK (r.steps == MaxTries);			// all four tries are reached, whatever the endpoint state says
	CHECK (b.Tries == MaxTries);
	CHECK (b.Total == MaxTries);
	CHECK (b.Spent);				// and REENUM-EXHAUSTED becomes visible
	CHECK (b.Key == KeyA);

	// the ladder's shape: plain while there is plain budget left, then the port power cycle
	CHECK (r.seen[0] == StepPlain);
	CHECK (r.seen[1] == StepPlain);
	CHECK (r.seen[2] == StepPowerCycle);
	CHECK (r.seen[3] == StepPowerCycle);

	// and no two of them closer together than the floor
	CHECK (r.apartHZ >= Ticks (ApartMs, HZ));

	// the first try may not come before the given-up run has held HoldMs
	CHECK (r.atHZ >= 1000 + Ticks (HoldMs, HZ));
	// ... and not much later than that: 50 errors at one per pass, then the hold
	CHECK (r.atHZ <= 1000 + 50 + Ticks (HoldMs, HZ) + 2);

	// nothing more, ever, however long it stays broken
	Run after = drive_dead (&d, &b, 1000 + Ticks (120000, HZ), 120000, bSeeRunning);
	CHECK (after.steps == 0);
	CHECK (b.Total == MaxTries);
}

// 2. The whole ladder takes a bounded time: four tries, each needing its own given-up run and hold plus the floor
//    between them, so the device is left alone for good well inside a minute of being broken.
static void test_ladder_is_bounded (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;

	Run r = drive_dead (&d, &b, 1000, 60000, true);
	CHECK (r.steps == MaxTries);
	CHECK (b.Spent);
}

// 3. A transient must never escalate: the driver's consecutive count never reaches 50, so bGaveUp is never true.
static void test_transient_never_escalates (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Hold h;
	memset (&h, 0, sizeof h);
	Input i;
	i.hz = HZ;
	i.Key = KeyA;
	i.bArmed = true;
	i.Errors = 0;

	for (unsigned t = 0; t < Ticks (300000, HZ); t++)
	{
		if (t % 500 == 0) i.Errors++;		// one error every 5 s, forever
		i.nowHZ = t;
		i.bGaveUp = false;			// far apart, so the driver's run never builds up
		CHECK (Decide (&h, &b, i) == StepNone);
	}

	CHECK (b.Total == 0);
	CHECK (!b.Spent);
}

// 4. Given up for just under HoldMs and then a report gets through: no escalation.
static void test_recovered_before_the_hold_expires (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;
	d.consecutive = 50;				// straight into the given-up state
	d.errors = 50;
	d.armed = false;

	unsigned t = 1000;
	for (; t < 1000 + Ticks (HoldMs, HZ) - 1; t++)
	{
		CHECK (Decide (&d.hold, &b, d.in (t)) == StepNone);
	}

	d.consecutive = 0;				// a report got through
	d.armed = true;
	for (; t < 1000 + Ticks (60000, HZ); t++)
	{
		CHECK (Decide (&d.hold, &b, d.in (t)) == StepNone);
	}

	CHECK (b.Total == 0);
}

// 5. Forgiveness without a single report: an idle panel sends none, so health is "armed and no new error".
static void test_forgiveness_needs_no_reports (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;

	Run r = drive_dead (&d, &b, 1000, 30000, true);
	CHECK (r.steps >= 1);
	CHECK (b.Tries >= 1);

	// the new device is healthy: armed, and not one report or new error for ForgiveMs
	unsigned t = 1000 + Ticks (30000, HZ);
	d.reset ();
	unsigned endHZ = t + Ticks (ForgiveMs + 1000, HZ);
	for (; t != endHZ; t++)
	{
		CHECK (Decide (&d.hold, &b, d.in (t)) == StepNone);
	}

	CHECK (b.Key == 0);				// handed back
	CHECK (b.Tries == 0);
	CHECK (!b.Spent);

	// and a later episode gets the whole ladder again
	Run again = drive_dead (&d, &b, endHZ, 120000, true);
	CHECK (again.step == StepPlain);			// a plain try again, not a power cycle
	CHECK (again.steps == MaxTries);
	CHECK (b.Spent);
}

// 6. A new error inside the forgiveness window restarts it: no refund.
static void test_new_error_restarts_the_health_clock (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	b.Key = KeyA;
	b.Tries = 3;
	Dev d;

	unsigned t = 1000;
	unsigned endHZ = t + Ticks (ForgiveMs, HZ) - 100;
	for (; t != endHZ; t++) CHECK (Decide (&d.hold, &b, d.in (t)) == StepNone);
	d.errors++;					// one error just before the window closes
	for (unsigned n = 0; n < Ticks (ForgiveMs, HZ) - 200; n++, t++)
	{
		CHECK (Decide (&d.hold, &b, d.in (t)) == StepNone);
	}

	CHECK (b.Tries == 3);				// not refunded: the clock started again
	CHECK (b.Key == KeyA);

	for (unsigned n = 0; n < 400; n++, t++) Decide (&d.hold, &b, d.in (t));
	CHECK (b.Tries == 0);				// now the full window has passed
}

// 7. An unarmed endpoint is never healthy, so a device that is dead but quiet is not forgiven.
static void test_unarmed_is_not_healthy (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	b.Key = KeyA;
	b.Tries = 2;
	Dev d;
	d.armed = false;

	for (unsigned t = 1000; t < 1000 + Ticks (3 * ForgiveMs, HZ); t++)
	{
		Decide (&d.hold, &b, d.in (t));
	}

	CHECK (b.Tries == 2);
}

// 8. The spacing floor lives in the budget, so it survives the re-enumeration that destroys the device: a new object
//    whose own clock is already satisfied still cannot escalate inside ApartMs of the last one.
static void test_spacing_floor_survives_the_device (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;
	d.consecutive = 50;
	d.errors = 50;
	d.armed = false;

	unsigned t = 1000;
	while (Decide (&d.hold, &b, d.in (t)) == StepNone) t++;
	CHECK (b.Tries == 1);
	unsigned firstHZ = t;

	// a brand-new device, already given up and already past its own hold: the floor must still hold it off
	d.reset ();
	d.consecutive = 50;
	d.errors = 50;
	d.armed = false;
	d.hold.GaveUpHZ = t + 1 - Ticks (HoldMs, HZ) - 10;	// its own clock says "go"

	for (t++; t < firstHZ + Ticks (ApartMs, HZ); t++)
	{
		CHECK (Decide (&d.hold, &b, d.in (t)) == StepNone);
		CHECK (b.Tries == 1);
	}

	CHECK (Decide (&d.hold, &b, d.in (t)) == StepPlain);
	CHECK (b.Tries == 2);
}

// 9. No topology key (GetRootHubPortID() unknown): never escalate, because nothing could bound the budget.
static void test_no_key_never_escalates (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;
	d.consecutive = 50;
	d.errors = 50;
	d.armed = false;

	for (unsigned t = 1000; t < 1000 + Ticks (120000, HZ); t++)
	{
		CHECK (Decide (&d.hold, &b, d.in (t, 0)) == StepNone);
	}

	CHECK (b.Total == 0);
	CHECK (b.Key == 0);
}

// 10. Another device at another place in the tree gets its own budget, and taking it over does not inherit the
//     exhausted state.
static void test_another_device_gets_its_own_budget (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	b.Key = KeyA;
	b.Tries = MaxTries;
	b.Total = MaxTries;
	b.Spent = true;
	b.LastHZ = 1;

	Dev d;
	d.consecutive = 50;
	d.errors = 50;
	d.armed = false;

	unsigned t = 100000;
	while (Decide (&d.hold, &b, d.in (t, KeyB)) == StepNone)
	{
		t++;
		CHECK (t < 100000 + Ticks (60000, HZ));	// it must get its turn
	}

	CHECK (b.Key == KeyB);
	CHECK (b.Tries == 1);
	CHECK (!b.Spent);
	CHECK (b.Total == MaxTries + 1);
}

// 11. The tick counter wraps (unsigned, HZ ticks since boot): every comparison is a difference, so the ladder has to
//     work straight across the wrap.
static void test_tick_wraparound (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;

	Run r = drive_dead (&d, &b, 0u - Ticks (5000, HZ), 120000, true);
	CHECK (r.step == StepPlain);
	CHECK (r.steps == MaxTries);
	CHECK (b.Spent);
}

// 12. HeldMs is the diagnostic that would have found the 2026-10-25 bug in one log line: it has to climb while the
//     run holds, and read 0 when there is no run.
static void test_held_ms_reports_the_clock (void)
{
	Budget b;
	memset (&b, 0, sizeof b);
	Dev d;
	d.armed = true;

	CHECK (HeldMs (d.hold, 1000, HZ) == 0);		// no run in progress

	d.consecutive = 50;
	d.errors = 50;
	d.armed = false;
	Decide (&d.hold, &b, d.in (1000));
	CHECK (HeldMs (d.hold, 1000, HZ) == 0);
	CHECK (HeldMs (d.hold, 1000 + Ticks (4000, HZ), HZ) == 4000);

	// ... and the sighting that used to rewind it does not
	d.armed = true;
	Decide (&d.hold, &b, d.in (1000 + Ticks (4000, HZ)));
	CHECK (HeldMs (d.hold, 1000 + Ticks (4000, HZ), HZ) == 4000);

	d.consecutive = 0;				// a report got through: now it resets
	Decide (&d.hold, &b, d.in (1000 + Ticks (5000, HZ)));
	CHECK (HeldMs (d.hold, 1000 + Ticks (5000, HZ), HZ) == 0);
}

int main (void)
{
	test_measured_timeline (true);			// with the state-1 sightings: the regression
	test_measured_timeline (false);			// and without them
	test_ladder_is_bounded ();
	test_transient_never_escalates ();
	test_recovered_before_the_hold_expires ();
	test_forgiveness_needs_no_reports ();
	test_new_error_restarts_the_health_clock ();
	test_unarmed_is_not_healthy ();
	test_spacing_floor_survives_the_device ();
	test_no_key_never_escalates ();
	test_another_device_gets_its_own_budget ();
	test_tick_wraparound ();
	test_held_ms_reports_the_clock ();
	printf ("test_hid_reenum: OK\n");
	return 0;
}
