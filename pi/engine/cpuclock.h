// pi/engine/cpuclock.h - the CPU clock option (Settings > System: 1500 / 1200 / 1000 MHz) and what follows from it
// (portable, header only: tests/test_cpuclock.cpp).
//  - the setting: 1500 (the default: the board's full clock, 1500 MHz on the Pi 4), 1200 or 1000
//  - the voice guard's budget: a fixed share of a 12-frame block's time (voicebudget=, 25 % by default: set when the
//    engine was measured at 1500 MHz). A voice's render takes proportionally longer at a lower clock, so the same time
//    would mean less work: the budget grows by 1500 / clock (25 % -> 31.25 % at 1200, 37.5 % at 1000, at most the whole
//    block), and never shrinks below the validated share on a board that runs faster than 1500. It follows the ARM
//    clock as read back once a second (follow_mhz), so Circle's own throttle to 600 MHz raises it too
//  - the boot bench's "voices a core" at the clock it ran at, rescaled to another (linear: an estimate, not a measurement)
#ifndef _cpuclock_h
#define _cpuclock_h

namespace cpuclock {

enum : unsigned { kRefMHz = 1500, kN = 3 };
static const unsigned kOptions[kN] = {1500, 1200, 1000};

// cmdline / display.txt value to a setting: anything but 1200 and 1000 is the default
inline unsigned normalize (unsigned nMHz)
{
	for (unsigned i = 0; i < kN; i++) if (kOptions[i] == nMHz) return nMHz;
	return kRefMHz;
}

// The ARM clock to ask the firmware for, in Hz: the default is the board's own maximum (the Pi 5's 2400 MHz stays);
// 1200 / 1000 within the board's range
inline unsigned target_hz (unsigned nSettingMHz, unsigned nMinHz, unsigned nMaxHz)
{
	nSettingMHz = normalize (nSettingMHz);
	if (nSettingMHz == kRefMHz) return nMaxHz;
	unsigned long long hz = (unsigned long long) nSettingMHz * 1000000ull;
	if (hz < nMinHz) hz = nMinHz;
	if (hz > nMaxHz) hz = nMaxHz;
	return (unsigned) hz;
}

// The guard's budget in counter ticks: nBasePct (5..100) of a 12-frame block at 48 kHz, scaled for the clock
inline unsigned budget_ticks (unsigned long long nCntFrq, unsigned nBasePct, unsigned nMHz)
{
	if (nBasePct < 5) nBasePct = 5;
	if (nBasePct > 100) nBasePct = 100;
	if (nMHz == 0 || nMHz > kRefMHz) nMHz = kRefMHz;		// unknown, or faster than the validated clock: no change
	unsigned long long nBlock = nCntFrq * 12 / 48000;
	unsigned long long n = nBlock * nBasePct * kRefMHz / (100ull * nMHz);
	return (unsigned) (n > nBlock ? nBlock : n);
}

// The ARM clock as read back (CCPUThrottle::GetClockRate, Hz) against the MHz the budget is for: the new MHz when it
// moved, 0 when it did not (to the MHz, rounded) or could not be read (0 Hz: the mailbox failed). The budget follows the
// clock the cores really run at, not the setting: at 1500 Circle's CCPUThrottle::Update drops the clock to its minimum
// (600 on the Pi 4) when the SoC passes its enforced temperature and raises it again when cool, behind the setting.
inline unsigned follow_mhz (unsigned nBudgetMHz, unsigned nReadHz)
{
	if (nReadHz == 0) return 0;
	unsigned nMHz = (unsigned) (((unsigned long long) nReadHz + 500000ull) / 1000000ull);
	return nMHz == nBudgetMHz ? 0 : nMHz;
}

// The bench's voices a core (or any figure that scales with the clock) rescaled from the clock it was measured at
inline unsigned voices_at (unsigned nVoices, unsigned nBenchMHz, unsigned nMHz)
{
	if (nBenchMHz == 0 || nMHz == 0) return nVoices;
	return (unsigned) ((unsigned long long) nVoices * nMHz / nBenchMHz);
}

}	// namespace cpuclock

#endif
