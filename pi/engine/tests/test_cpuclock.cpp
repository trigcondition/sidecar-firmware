// pi/engine/tests/test_cpuclock.cpp - cpuclock.h: the setting, the clock asked for, and the voice budget that follows it.
#include "check.h"
#include "cpuclock.h"

using namespace cpuclock;

int main ()
{
	CHECK (normalize (1500) == 1500 && normalize (1200) == 1200 && normalize (1000) == 1000);
	CHECK (normalize (0) == 1500 && normalize (900) == 1500 && normalize (2400) == 1500);	// anything else: the default

	// the Pi 4: 600..1500 MHz
	CHECK (target_hz (1500, 600000000, 1500000000) == 1500000000u);
	CHECK (target_hz (1200, 600000000, 1500000000) == 1200000000u);
	CHECK (target_hz (1000, 600000000, 1500000000) == 1000000000u);
	CHECK (target_hz (777, 600000000, 1500000000) == 1500000000u);				// not an option: the default
	// the Pi 5 (1500..2400): the default stays at its own full clock; 1000 cannot go under the board's minimum
	CHECK (target_hz (1500, 1500000000, 2400000000u) == 2400000000u);
	CHECK (target_hz (1000, 1500000000, 2400000000u) == 1500000000u);

	// the guard's budget: the counter is 54 MHz on the Pi 4, a 12-frame block is 13500 ticks (250 us); 25 % = 3375 ticks
	const unsigned long long f = 54000000ull;
	CHECK (budget_ticks (f, 25, 1500) == 3375);					// the validated figure (guard.h's default)
	CHECK (budget_ticks (f, 25, 1200) == 4218);					// 31.25 %: 78 us, not 62.5 us
	CHECK (budget_ticks (f, 25, 1000) == 5062);					// 37.5 %: 93.75 us
	CHECK (budget_ticks (f, 25, 1000) > budget_ticks (f, 25, 1200) && budget_ticks (f, 25, 1200) > budget_ticks (f, 25, 1500));
	CHECK (budget_ticks (f, 25, 2400) == 3375);					// a faster board: never tighter than validated
	CHECK (budget_ticks (f, 25, 0) == 3375);					// clock unknown: unchanged
	CHECK (budget_ticks (f, 80, 1000) == 13500);					// at most the whole block
	CHECK (budget_ticks (f, 100, 1200) == 13500);
	CHECK (budget_ticks (f, 1, 1500) == 675);					// voicebudget= 5 % is the floor
	// the work a budget allows stays the same: ticks * clock is constant (the same work-at-1500 threshold)
	CHECK (budget_ticks (f, 25, 1000) * 1000ull / 1500 == 3374 || budget_ticks (f, 25, 1000) * 1000ull / 1500 == 3375);

	// the bench's voices a core, rescaled
	CHECK (voices_at (49, 1500, 1000) == 32 && voices_at (49, 1500, 1200) == 39 && voices_at (49, 1500, 1500) == 49);
	CHECK (voices_at (49, 1000, 1500) == 73);						// a bench taken at 1000, now at 1500
	CHECK (voices_at (49, 0, 1000) == 49);							// no bench clock known
	// the budget follows the ARM clock as read back (Circle's CCPUThrottle drops 1500 to its minimum when hot, behind
	// the setting's back): the new MHz when it moved, 0 when it did not or the read failed
	CHECK (follow_mhz (1500, 600000000) == 600);					// throttled
	CHECK (follow_mhz (600, 1500000000) == 1500);					// and back
	CHECK (follow_mhz (1500, 1500000000) == 0);					// no change: nothing to do
	CHECK (follow_mhz (1500, 1499999999) == 0 && follow_mhz (1500, 1500400000) == 0);	// rounds to the same MHz
	CHECK (follow_mhz (1500, 0) == 0);						// the mailbox failed: the budget stays
	CHECK (follow_mhz (0, 600000000) == 600);					// not known yet
	CHECK (follow_mhz (1200, 1200000000) == 0 && follow_mhz (1200, 600000000) == 600);	// a capped setting too
	puts ("cpuclock: PASS");
	return 0;
}
