// pi/engine/fan.h - the Pi 5 fan header's speed (portable, header only: tests/test_fan.cpp; the pin: fan_pi.h).
// Under Linux the Raspberry Pi 5's device tree runs the official Active Cooler (and the case fan) from the SoC
// temperature; under Sidecar nobody did, so a Pi 5 under load reached socmaxtemp and Circle lowered its clock.
// The mode (fan= in /.rytm/display.txt, cmdline fan= wins, the console's fan=):
//  - AUTO (the default): Linux's Pi 5 curve (bcm2712-rpi-5-b.dts): four trips at 50 / 60 / 67.5 / 75 C, each with 5 C
//    of hysteresis, step the fan through the levels 0 / 75 / 125 / 175 / 250 (of 255). A level goes up when the SoC
//    reaches its trip and down only once it is 5 C below it, so a unit that sits at a trip does not hunt.
//  - ON: level 4 (250, Linux's full speed) all the time.
//  - OFF: off, unless the SoC reaches the temperature warning (thermal.h, 80 C): then full until it is below 75 C.
//    The firmware's own limits still protect the chip; this keeps "off" from leaving it there.
// One sample a second, the stats line's reading (never its own mailbox call). A failed reading (0) changes nothing.
#ifndef _fan_h
#define _fan_h

#include "thermal.h"	// OFF's override: the temperature warning's own on / off pair (one source of truth)

namespace fan {

enum Mode : int { kAuto, kOn, kOff, kModes };
enum : int { kLevels = 5, kTrips = 4, kHystC10 = 50, kGuardOnC = thermal::kWarnC, kGuardOffBelowC = thermal::kClearBelowC };
static const int kTripC10[kTrips] = {500, 600, 675, 750};		// tenths of a degree C (67.5)
static const unsigned kPwm[kLevels] = {0, 75, 125, 175, 250};		// the device tree's cooling-levels, of 255
// the PWM: RP1 PWM1 at 50 MHz, the device tree's period 41566 ns (24 kHz) = 2078 counts
enum : unsigned { kClockHz = 50000000, kRange = 2078 };

inline const char *name (Mode m)
{
	return m == kOn ? "on" : m == kOff ? "off" : "auto";
}

// "auto" / "on" / "off", ended by the string's end or a line end (display.txt); false: none of them
inline bool parse (const char *p, Mode *pMode)
{
	if (p == 0) return false;
	static const char *const Names[kModes] = {"auto", "on", "off"};
	for (int m = 0; m < kModes; m++)
	{
		const char *a = Names[m], *b = p;
		while (*a && *a == *b) a++, b++;
		if (*a == 0 && (*b == 0 || *b == '\n' || *b == '\r'))
		{
			*pMode = (Mode) m;
			return true;
		}
	}
	return false;
}

struct State
{
	int level = 0;			// 0..kLevels-1, what the pin is set to
	bool guard = false;		// OFF's override: on at kGuardOnC, off below kGuardOffBelowC
	int last_c = 0;			// the last good reading (a mode change steps with it at once)
	unsigned changes = 0;		// level changes since boot
};

// AUTO's level for a reading, from the level it is at: up through every trip reached, down through every trip the
// SoC is a full hysteresis below
inline int curve (int level, int temp_c)
{
	const int t = temp_c * 10;
	while (level < kLevels - 1 && t >= kTripC10[level]) level++;
	while (level > 0 && t < kTripC10[level - 1] - kHystC10) level--;
	return level;
}

// One sample (or a mode change: temp_c = s.last_c). True when the level changed.
inline bool step (State &s, Mode m, int temp_c)
{
	if (temp_c > 0)
	{
		s.last_c = temp_c;
		if (temp_c >= kGuardOnC) s.guard = true;
		else if (temp_c < kGuardOffBelowC) s.guard = false;
	}
	int level = s.level;
	if (m == kOn) level = kLevels - 1;
	else if (m == kOff) level = s.guard ? kLevels - 1 : 0;
	else if (temp_c > 0) level = curve (s.level, temp_c);
	if (level == s.level) return false;
	s.level = level;
	s.changes++;
	return true;
}

// The duty for a level, in PWM counts of nRange (the fan's speed: the pin is inverted, fan_pi.cpp)
inline unsigned duty (int level, unsigned nRange = kRange)
{
	if (level < 0) level = 0;
	if (level >= kLevels) level = kLevels - 1;
	return (unsigned) (((unsigned long long) kPwm[level] * nRange + 127) / 255);
}

}	// namespace fan

#endif
