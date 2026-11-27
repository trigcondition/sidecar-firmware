// pi/engine/tests/test_ob_freeze.cpp - ob_freeze.h's timeline: a frozen box is re-enumerated after kHoldMs of failing
// resubmits, the known short stall is not, the budget holds three tries and comes back after kForgetMs of streaming.
#include <cstdio>

#include "../ob_freeze.h"
#include "check.h"

using namespace obfreeze;

int main ()
{
	{	// the short stall two boxes show (about 846 ms of resubmits, then good completions): nothing
		Watch w;
		Budget b;
		uint32_t t = 1000;
		for (int i = 0; i < 9; ++i, t += 100) CHECK (!pass (w, b, (unsigned) i + 1, t));
		CHECK (!pass (w, b, 0, t) && b.tries == 0 && !w.run);
	}
	{	// SR1: resubmits from 00:05:15.75 for 75 s: one re-enumeration at kHoldMs, then (the new object) again
		Watch w;
		Budget b;
		uint32_t t = 315750;
		int fired = -1;
		for (int ms = 0; ms <= 10000; ms += 50)
			if (pass (w, b, 1 + ms / 1000, t + (uint32_t) ms)) { fired = ms; break; }
		CHECK (fired >= (int) kHoldMs && fired < (int) kHoldMs + 100 && b.tries == 1 && !w.run);
		// the box comes back but freezes again: a fresh Watch (a new CObUsb), the same Budget
		for (unsigned k = 2; k <= kTries; ++k)
		{
			Watch w2;
			uint32_t s = t + 20000 * k;
			CHECK (!pass (w2, b, 1, s));
			CHECK (!pass (w2, b, 3, s + kHoldMs - 1));
			CHECK (pass (w2, b, 5, s + kHoldMs) && b.tries == k);
		}
		Watch w3;
		uint32_t s = t + 100000;
		CHECK (!pass (w3, b, 1, s) && !spent_now (w3, b, s));
		CHECK (!pass (w3, b, 9, s + kHoldMs) && b.tries == kTries);	// spent: no fourth
		CHECK (spent_now (w3, b, s + kHoldMs) && !spent_now (w3, b, s + kHoldMs + 1000));	// told once
		// it streams again (a replug by hand) for kForgetMs: the whole budget back
		CHECK (!pass (w3, b, 0, s + 10000) && b.tries == kTries);
		CHECK (!pass (w3, b, 0, b.last_ms + kForgetMs + 1) && b.tries == 0 && !b.told);
	}
	{	// a run interrupted by one good completion starts its clock again
		Watch w;
		Budget b;
		CHECK (!pass (w, b, 1, 0));
		CHECK (!pass (w, b, 4, 3000));
		CHECK (!pass (w, b, 0, 3100));
		CHECK (!pass (w, b, 1, 3200));
		CHECK (!pass (w, b, 6, 3200 + kHoldMs - 1));
		CHECK (pass (w, b, 7, 3200 + kHoldMs));
	}
	{	// the clock wraps (uint32 ms since boot): still kHoldMs
		Watch w;
		Budget b;
		const uint32_t t = 0xFFFFF000u;
		CHECK (!pass (w, b, 1, t));
		CHECK (!pass (w, b, 2, t + kHoldMs - 1));
		CHECK (pass (w, b, 3, t + kHoldMs));
	}
	printf ("test_ob_freeze: PASS\n");
	return 0;
}
