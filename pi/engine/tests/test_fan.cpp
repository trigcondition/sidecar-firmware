// pi/engine/tests/test_fan.cpp - fan.h: the Pi 5 fan's AUTO curve (Linux's trips with 5 C hysteresis), ON, OFF and
// its 80 C override, failed readings, the parser and the duty.
#include "check.h"
#include "fan.h"

using namespace fan;

int main ()
{
	// the curve going up: 50 / 60 / 67.5 / 75 C
	{
		State s;
		CHECK (!step (s, kAuto, 49) && s.level == 0);
		CHECK (step (s, kAuto, 50) && s.level == 1);
		CHECK (!step (s, kAuto, 59) && s.level == 1);
		CHECK (step (s, kAuto, 60) && s.level == 2);
		CHECK (!step (s, kAuto, 67) && s.level == 2);			// 67 < 67.5
		CHECK (step (s, kAuto, 68) && s.level == 3);
		CHECK (!step (s, kAuto, 74) && s.level == 3);
		CHECK (step (s, kAuto, 75) && s.level == 4);
		CHECK (!step (s, kAuto, 90) && s.level == 4);			// the top level is the last
		CHECK (s.changes == 4);
		// and down, each a full 5 C below its trip
		CHECK (!step (s, kAuto, 70) && s.level == 4);			// 75 - 5 = 70 still holds
		CHECK (step (s, kAuto, 69) && s.level == 3);
		CHECK (!step (s, kAuto, 63) && s.level == 3);			// 67.5 - 5 = 62.5: 63 holds
		CHECK (step (s, kAuto, 62) && s.level == 2);
		CHECK (!step (s, kAuto, 55) && s.level == 2);
		CHECK (step (s, kAuto, 54) && s.level == 1);
		CHECK (!step (s, kAuto, 45) && s.level == 1);
		CHECK (step (s, kAuto, 44) && s.level == 0);
	}
	// a jump crosses several trips in one sample, both ways
	{
		State s;
		CHECK (step (s, kAuto, 72) && s.level == 3 && s.changes == 1);
		CHECK (step (s, kAuto, 40) && s.level == 0 && s.changes == 2);
		CHECK (step (s, kAuto, 80) && s.level == 4);
		CHECK (step (s, kAuto, 58) && s.level == 2);			// below 70 and 62.5, not below 55
		CHECK (step (s, kAuto, 50) && s.level == 1);			// below 55, not below 45
	}
	// a unit that sits on a trip does not hunt: one change in 100 samples
	{
		State s;
		step (s, kAuto, 59);
		unsigned c = s.changes;
		for (int i = 0; i < 100; i++) step (s, kAuto, i % 2 ? 60 : 57);
		CHECK (s.changes == c + 1 && s.level == 2);
	}
	// a failed reading (0) changes nothing, in AUTO and OFF
	{
		State s;
		step (s, kAuto, 65);
		CHECK (!step (s, kAuto, 0) && s.level == 2 && s.last_c == 65);
		State o;
		step (o, kOff, 82);
		CHECK (o.guard && o.level == 4);
		CHECK (!step (o, kOff, 0) && o.guard && o.level == 4);
	}
	// ON: full whatever the reading, even none
	{
		State s;
		CHECK (step (s, kOn, 0) && s.level == 4);
		CHECK (!step (s, kOn, 30) && s.level == 4);
	}
	// OFF: off, and full from 80 C until below 75 C
	{
		State s;
		step (s, kAuto, 70);
		CHECK (s.level == 3);
		CHECK (step (s, kOff, s.last_c) && s.level == 0);		// a mode change steps at once with the last reading
		CHECK (!step (s, kOff, 79) && s.level == 0 && !s.guard);
		CHECK (step (s, kOff, 80) && s.level == 4 && s.guard);
		CHECK (!step (s, kOff, 76) && !step (s, kOff, 75) && s.level == 4);
		CHECK (step (s, kOff, 74) && s.level == 0 && !s.guard);
	}
	// the override comes on in AUTO already (at 80 C) and holds when OFF is chosen between 75 and 79 C
	{
		State s;
		step (s, kAuto, 81);
		CHECK (s.level == 4 && s.guard);
		step (s, kAuto, 77);
		CHECK (!step (s, kOff, s.last_c) && s.level == 4 && s.guard);	// still full: not yet below 75
		CHECK (step (s, kOff, 74) && s.level == 0);
	}
	// a mode change before any reading (last_c 0): AUTO changes nothing, ON and OFF still apply
	{
		State s;
		CHECK (!step (s, kAuto, s.last_c) && s.level == 0);
		CHECK (step (s, kOn, s.last_c) && s.level == 4);
		CHECK (!step (s, kAuto, s.last_c) && s.level == 4);		// AUTO without a reading keeps the level
		CHECK (step (s, kOff, s.last_c) && s.level == 0);
	}
	// back to AUTO from ON: the curve from the top level down, with its hysteresis
	{
		State s;
		step (s, kOn, 66);
		CHECK (s.level == 4);
		CHECK (step (s, kAuto, s.last_c) && s.level == 3);		// 66: below 70 (level 4 off), not below 62.5
	}
	// the parser
	{
		Mode m = kAuto;
		CHECK (parse ("on", &m) && m == kOn);
		CHECK (parse ("off\n", &m) && m == kOff);
		CHECK (parse ("auto\r\n", &m) && m == kAuto);
		CHECK (!parse ("onx", &m) && !parse ("", &m) && !parse ("of", &m) && !parse ("ON", &m) && !parse (0, &m));
		CHECK (name (kAuto)[0] == 'a' && name (kOn)[1] == 'n' && name (kOff)[1] == 'f');
	}
	// the duty: the device tree's levels of 255, in the 24 kHz PWM's 2078 counts
	{
		CHECK (duty (0) == 0);
		CHECK (duty (1) == (75 * 2078 + 127) / 255);			// 611
		CHECK (duty (4) == (250 * 2078 + 127) / 255);			// 2037
		CHECK (duty (4) < kRange && duty (9) == duty (4) && duty (-1) == 0);
		CHECK (duty (4, 255) == 250);
		CHECK ((unsigned long long) kRange * 1000000000ull / kClockHz == 41560);	// the period, ns (41566 asked)
	}
	printf ("test_fan: pass\n");
	return 0;
}
