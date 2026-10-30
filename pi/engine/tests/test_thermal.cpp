// pi/engine/tests/test_thermal.cpp - thermal.h: the warning comes on at 80 C, goes off below 75 C, once per crossing;
// the clock Circle lowered (clock_low_mhz) and the Rytm popup's text (popup_text).
#include "check.h"
#include "thermal.h"
#include <string.h>

using namespace thermal;

int main ()
{
	State s;
	CHECK (!s.warn && s.crossings == 0);
	CHECK (update (s, 74) == kNone && update (s, 79) == kNone && !s.warn);		// below the threshold: quiet
	CHECK (update (s, 80) == kWarned && s.warn && s.crossings == 1);		// 80 up
	CHECK (update (s, 80) == kNone && update (s, 81) == kNone && s.warn);		// no second event while it stays
	CHECK (update (s, 79) == kNone && s.warn);					// 79 stays warned
	CHECK (update (s, 77) == kNone && update (s, 75) == kNone && s.warn);		// 75 stays warned too
	CHECK (update (s, 80) == kNone && s.warn && s.crossings == 1);			// back at 80: the same crossing
	CHECK (update (s, 74) == kCleared && !s.warn);					// 74 clears
	CHECK (s.peak_c == 81);								// the crossing's peak
	CHECK (update (s, 74) == kNone && update (s, 78) == kNone && !s.warn);		// 78 after a clear: not warned again
	CHECK (update (s, 85) == kWarned && s.warn && s.crossings == 2 && s.peak_c == 85);	// a new crossing, a new event

	// a sitting unit at 79..80 over and over: one warning, no flicker
	State t;
	unsigned events = 0;
	for (int i = 0; i < 100; i++) events += update (t, i % 2 ? 79 : 80) != kNone;
	CHECK (events == 1 && t.warn);

	// a failed read (0) changes nothing, warned or not
	State u;
	CHECK (update (u, 0) == kNone && !u.warn);
	update (u, 82);
	CHECK (update (u, 0) == kNone && u.warn);
	// down by degrees: stays warned until below 75
	for (int c = 82; c >= 75; c--) CHECK (update (u, c) == kNone && u.warn);
	CHECK (update (u, 74) == kCleared);

	// the Rytm's popup: once per crossing, offered each second until it is shown, never after the warning cleared
	{
		State w;
		Popup p;
		unsigned shown = 0, offers = 0;
		auto step = [&] (int c, bool rytm_can) {		// one second: the sample, then the kernel's offer
			Event e = update (w, c);
			if (popup_due (p, w, e, shown)) { offers++; if (rytm_can) shown++; }
		};
		step (75, true);  CHECK (offers == 0);			// not warned: no popup
		step (80, true);  CHECK (offers == 1 && shown == 1);	// the crossing: offered, shown
		step (81, true);  CHECK (offers == 1);			// shown: not again while it lasts
		step (79, true);  step (77, true);  CHECK (offers == 1);
		step (74, true);  CHECK (offers == 1 && !p.owed);		// cleared
		step (80, true);  CHECK (offers == 2 && shown == 2);	// a new crossing: a new popup (once per crossing)
		step (74, true);
		// the Rytm cannot take it (a page is open): offered every second, and shown the first second it can
		step (82, false); step (83, false); step (82, false);  CHECK (offers == 5 && shown == 2 && p.owed);
		step (82, true);  CHECK (offers == 6 && shown == 3);
		step (82, true);  CHECK (offers == 6);			// then quiet
		step (74, true);
		// the warning clears before the Rytm could take it: dropped, not shown late
		step (80, false); step (79, false); step (74, false);  CHECK (!p.owed);
		unsigned o = offers;
		step (74, true);  step (76, true);  CHECK (offers == o && shown == 3);
	}
	// the clock Circle lowered (socmaxtemp 78: 600 MHz): at the full setting, read back well below it, the SoC near the limit
	CHECK (clock_low_mhz (600, 1500, 79) == 600);			// between the throttle and the warning: lowered (quiet)
	CHECK (clock_low_mhz (600, 1500, 81) == 600);			// warned and lowered
	CHECK (clock_low_mhz (600, 1500, 75) == 600 && clock_low_mhz (600, 1500, 74) == 0);	// Circle raises it 3 C below
	CHECK (clock_low_mhz (1500, 1500, 81) == 0);			// hot, the clock still full: not lowered
	CHECK (clock_low_mhz (1000, 1000, 81) == 0 && clock_low_mhz (1199, 1200, 81) == 0);	// a lower setting is not "lowered"
	CHECK (clock_low_mhz (0, 1500, 81) == 0 && clock_low_mhz (600, 0, 81) == 0 && clock_low_mhz (600, 1500, 0) == 0);	// unknowns

	// the Rytm's popup: honest about the clock
	{
		char a[33], b[33];
		popup_text (a, b, 80, 0);
		CHECK (!strcmp (a, "SIDECAR RUNNING HOT") && !strcmp (b, "SEE STATUS (UP+T10)"));
		popup_text (a, b, 80, 600);
		CHECK (!strcmp (a, "SOC 80C CLOCK LOWERED") && !strcmp (b, "TO 600 MHZ (UP+T10)"));
		CHECK (strlen (a) <= 21 && strlen (b) <= 21);			// the Rytm's screen
		popup_text (a, b, 123456789, 1234567890);			// never past 32
		CHECK (strlen (a) <= 32 && strlen (b) <= 32);
	}
	puts ("thermal: PASS");
	return 0;
}
