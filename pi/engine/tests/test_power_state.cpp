// pi/engine/tests/test_power_state.cpp - power_state.cpp: get_throttled's bits and the Pi 5's max_current to the
// warning, the /status JSON and the log line.
#include <string.h>

#include "check.h"
#include "power_state.h"

using namespace powerstate;

int main ()
{
	char b[512];
	State s;
	CHECK (warning (s) == 0 && short_warning (s) == 0);		// nothing known: no warning
	CHECK (json (s, b, sizeof b) > 0 && strstr (b, "\"known\":false") && strstr (b, "\"maxCurrent\":null") && strstr (b, "\"warn\":null}"));
	line (s, b, sizeof b);
	CHECK (strstr (b, "throttled unknown") != 0);

	CHECK (s.board == 4);						// the default: the Pi 4
	s.known = true;
	s.throttled = 0;
	CHECK (warning (s) == 0);
	line (s, b, sizeof b);
	CHECK (strstr (b, "throttled 0x0 ") && strstr (b, "| ok"));

	s.throttled = 0x50000;						// under-voltage and throttling since boot, not now
	CHECK (warning (s) && strstr (warning (s), "since boot") && short_warning (s));
	json (s, b, sizeof b);
	CHECK (strstr (b, "\"uvNow\":false") && strstr (b, "\"uvBoot\":true") && strstr (b, "\"throttledBoot\":true"));
	CHECK (strstr (b, "\"warn\":\"power supply too weak"));

	CHECK (strstr (warning (s), "official 15 W (5.1 V 3 A) supply") && !strstr (warning (s), "27 W"));	// Pi 4's supply
	CHECK (strcmp (short_warning (s), "POWER TOO WEAK: use 15 W supply") == 0);
	s.board = 5;							// the same flags on a Pi 5: its adapter
	CHECK (strstr (warning (s), "since boot") && strstr (warning (s), "27 W (5 V 5 A) adapter directly") && !strstr (warning (s), "15 W"));
	CHECK (strcmp (short_warning (s), "POWER TOO WEAK: use 27 W adapter") == 0);
	json (s, b, sizeof b);
	CHECK (strstr (b, "27 W (5 V 5 A) adapter directly"));
	line (s, b, sizeof b);
	CHECK (strstr (b, "| power supply too weak (under-voltage since boot): use the 27 W (5 V 5 A) adapter directly"));
	s.board = 4;
	line (s, b, sizeof b);
	CHECK (strstr (b, "use the official 15 W (5.1 V 3 A) supply"));
	for (int bd = 4; bd <= 5; bd++)					// every short form fits the display's bottom line (36)
	{
		s.board = bd;
		CHECK (strlen (short_warning (s)) <= 36);
	}
	s.board = 4;

	s.throttled = 0x50005;						// under-voltage now wins
	CHECK (strstr (warning (s), "now"));
	json (s, b, sizeof b);
	CHECK (strstr (b, "\"uvNow\":true") && strstr (b, "\"throttledNow\":true") && strstr (b, "\"throttled\":327685"));

	s.throttled = 0;
	s.max_ma = 3000;						// Pi 5 without the 5 A PD negotiation
	s.board = 5;
	CHECK (warning (s) && strstr (warning (s), "3 A mode") && strstr (warning (s), "27 W (5 V 5 A)"));
	json (s, b, sizeof b);
	CHECK (strstr (b, "\"maxCurrent\":3000"));
	line (s, b, sizeof b);
	CHECK (strstr (b, "max_current 3000 mA"));
	s.max_ma = 5000;
	CHECK (warning (s) == 0);

	CHECK (json (s, b, 16) > 16 && strlen (b) == 15);		// cut, still terminated
	printf ("power_state: OK\n");
	return 0;
}
