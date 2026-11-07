// pi/engine/tests/test_pwrbtn.cpp - pwrbtn.h: the released-once latch, the debounce only on a raw press, the drain's cap
#include "check.h"
#include "pwrbtn.h"

static void TestButton (void)
{
	pwrbtn::Button b;
	unsigned nDeb = 0;
	auto yes = [&] { nDeb++; return true; };
	auto no = [&] { nDeb++; return false; };
	CHECK (!b.Poll (true, yes) && nDeb == 0);	// held since boot: no press, no debounce
	CHECK (!b.Poll (false, yes) && nDeb == 0);	// released: armed, nothing read slowly
	CHECK (!b.Poll (true, no) && nDeb == 1);	// a glitch: the debounce says no
	CHECK (b.Poll (true, yes) && nDeb == 2);	// a real press
	CHECK (!b.Poll (false, yes) && nDeb == 2);
}

static void TestDrain (void)
{
	{	// idle at once: done after kSettledUs, not before
		pwrbtn::Drain d (1000);
		CHECK (!d.Done (true, 1000));
		CHECK (!d.Done (true, 1000 + pwrbtn::kSettledUs - 1));
		CHECK (d.Done (true, 1000 + pwrbtn::kSettledUs));
	}
	{	// busy again restarts the settle time
		pwrbtn::Drain d (0);
		CHECK (!d.Done (true, 0));
		CHECK (!d.Done (false, 50000));
		CHECK (!d.Done (true, 60000));
		CHECK (!d.Done (true, 60000 + pwrbtn::kSettledUs - 1));
		CHECK (d.Done (true, 60000 + pwrbtn::kSettledUs));
	}
	{	// never idle: the cap ends it, across the clock's wrap
		unsigned t0 = 0xFFFFFFFFu - 1000;
		pwrbtn::Drain d (t0);
		CHECK (!d.Done (false, t0 + pwrbtn::kDrainCapUs - 1));
		CHECK (d.TimedOut (t0 + pwrbtn::kDrainCapUs));
		CHECK (d.Done (false, t0 + pwrbtn::kDrainCapUs));
	}
}

int main (void)
{
	TestButton ();
	TestDrain ();
	printf ("pwrbtn: OK\n");
	return 0;
}
