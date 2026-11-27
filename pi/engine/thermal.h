// pi/engine/thermal.h - the SoC temperature warning's state (portable, header only: tests/test_thermal.cpp).
// One sample a second (the stats line's reading: never its own mailbox call). The warning comes on at 80 C and goes off
// only below 75 C, so a unit that sits at 80 does not flicker: 79 stays warned, 75 stays warned, 74 clears. (75 / 70
// until 2026-10-29, then 78 / 73; the user: "increase the temp warning to 80C. the unit doesnt feel hot at all".) A
// reading of 0 (the mailbox failed: CCPUThrottle::GetTemperature returns 0) changes nothing. update() returns what
// happened this sample, so the log line and the Rytm's popup come once per crossing, not once per sample.
// Circle's own throttle acts first: above socmaxtemp (78 on the card, Circle's maximum) CCPUThrottle drops the ARM clock
// to its minimum (600 MHz on the Pi 4, 1500 on the Pi 5) and raises it 3 C below. clock_low_mhz() says so: the warning then says the
// clock was lowered, and between 78 and 80 a quiet status line does (STATUS, /status: no banner, no popup).
#ifndef _thermal_h
#define _thermal_h

namespace thermal {

enum : int { kWarnC = 80, kClearBelowC = 75, kThrottleC = 78 };	// kThrottleC: socmaxtemp on the card

enum Event { kNone, kWarned, kCleared };

struct State
{
	bool warn = false;
	int peak_c = 0;			// the highest reading since this crossing began (kept after it ends, until the next)
	unsigned crossings = 0;		// warnings since boot
};

inline Event update (State &s, int temp_c)
{
	if (temp_c <= 0) return kNone;
	if (!s.warn)
	{
		if (temp_c < kWarnC) return kNone;
		s.warn = true, s.peak_c = temp_c, s.crossings++;
		return kWarned;
	}
	if (temp_c > s.peak_c) s.peak_c = temp_c;
	if (temp_c >= kClearBelowC) return kNone;
	s.warn = false;
	return kCleared;
}

// The Rytm's popup is owed once per crossing (not once per boot: a unit that cooled and heated again is news again).
// The Rytm cannot always take one (a page open, its connect splash, an old mod), so it is offered each sample until the
// remote UI's count of shown popups moves, and dropped when the warning clears first. It never overrides a page.
struct Popup
{
	bool owed = false;
	unsigned base = 0;		// the shown-popups count when it was first offered
};

// After update(): true when the popup should be (re)offered now
inline bool popup_due (Popup &p, const State &s, Event e, unsigned shown)
{
	if (e == kWarned) { p.owed = true, p.base = shown; return true; }
	if (!s.warn) { p.owed = false; return false; }
	if (!p.owed) return false;
	if (shown != p.base) { p.owed = false; return false; }	// it was shown
	return true;
}

// True while the Rytm's popup is still owed: another popup source (the web update's, each second) stays quiet then, as
// each popup it draws would count as the thermal one "shown" (the remote UI's count is shared)
inline bool popup_owed (const Popup &p) { return p.owed; }

// The ARM clock the heat lowered (MHz), 0: not lowered. Lowered: the clock read back (kernel FollowClock) well below the
// clock the setting asks for (cpuclock::asked_mhz: 2400 at a Pi 5's default; a lower setting is not "lowered"), with the SoC near Circle's limit (it raises the clock
// again 3 C below; a failed reading, 0, is not hot).
inline int clock_low_mhz (int clock_mhz, int asked_mhz, int temp_c)
{
	if (clock_mhz <= 0 || asked_mhz <= 0 || clock_mhz + 50 >= asked_mhz) return 0;
	return temp_c >= kThrottleC - 3 ? clock_mhz : 0;
}

// The Rytm's popup's two lines (core::ui::kTextLen 32 each; the Rytm's screen fits about 21 characters), without libc (any
// build). The clock lowered: it says so, honestly ("SOC 80C CLOCK LOWERED" / "TO 600 MHZ
// (UP+T10)"); otherwise as before.
inline void popup_text (char (&l1)[33], char (&l2)[33], int temp_c, int low_mhz)
{
	struct W
	{
		char *b; int n;
		void s (const char *t) { while (*t && n < 32) b[n++] = *t++; b[n] = 0; }
		void i (int v) { char d[12]; int k = 0; if (v < 0) v = 0; do d[k++] = (char) ('0' + v % 10), v /= 10; while (v && k < 11);
				 while (k && n < 32) b[n++] = d[--k]; b[n] = 0; }
	};
	W a = {l1, 0}, b = {l2, 0};
	a.s (""), b.s ("");
	if (low_mhz <= 0) { a.s ("SIDECAR RUNNING HOT"); b.s ("SEE STATUS (UP+T10)"); return; }
	a.s ("SOC "), a.i (temp_c), a.s ("C CLOCK LOWERED");
	b.s ("TO "), b.i (low_mhz), b.s (" MHZ (UP+T10)");
}

// The step-down for a Pi without cooling (decided 2026-11-22 after LT4: a bare Pi 4 under the full two-box load reached
// 77 C in ~15 min and Circle's throttle dropped the ARM clock from 1500 to 600 MHz at once: core 1 99 %, the FIFO empty,
// ~150,000 underruns in 4 s). The engine lowers the clock itself, in steps that the load can still carry, before that
// cliff: at kStep1C a cap of 1200 MHz, at kStep2C 1000, and only at kThrottleC the board's minimum (600 on the Pi 4, as
// Circle did). A cap is raised one step when the SoC has stayed at or below kStepUpC for kStepUpSecs readings in a row.
// What no longer fits at the lower clock (CPU admission: AMPs, INSERT types) plays dry and shows NO CPU until it rises.
// The cap is never above the setting (a setting of 1000 is already the lowest step). A reading of 0 changes nothing.
enum : int { kStep1C = 74, kStep2C = 76, kStepUpC = 70, kStepUpSecs = 30 };

struct Step
{
	int cap_mhz = 0;		// the clock the heat caps the setting to, 0: no cap
	int cool_secs = 0;		// readings in a row at or below kStepUpC
	unsigned steps = 0;		// caps lowered since boot
};

// One reading a second. setting_mhz: the clock the setting asks for (cpuclock::asked_mhz); min_mhz: the board's minimum.
// Returns true when the cap moved: the clock to run at is then cap_mhz, or the setting's when it is 0.
inline bool step (Step &s, int temp_c, int setting_mhz, int min_mhz)
{
	if (temp_c <= 0 || setting_mhz <= 0) return false;
	const int want = temp_c >= kThrottleC ? min_mhz : temp_c >= kStep2C ? 1000 : temp_c >= kStep1C ? 1200 : 0;
	const int now = s.cap_mhz ? s.cap_mhz : setting_mhz;
	if (want && want < now)
	{
		s.cap_mhz = want, s.cool_secs = 0, s.steps++;
		return true;
	}
	if (!s.cap_mhz) return false;
	if (temp_c > kStepUpC) { s.cool_secs = 0; return false; }
	if (++s.cool_secs < kStepUpSecs) return false;
	s.cool_secs = 0;
	const int up = s.cap_mhz < 1000 ? 1000 : s.cap_mhz < 1200 ? 1200 : 0;
	s.cap_mhz = up && up < setting_mhz ? up : 0;
	return true;
}

}	// namespace thermal

#endif
