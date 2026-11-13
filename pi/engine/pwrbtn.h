// pi/engine/pwrbtn.h - the Pi 5's power button (kernel.cpp, RASPPI 5 only): when a press counts, and how long the
// clean power-off waits for the card. Portable, header only: tests/test_pwrbtn.cpp.
#ifndef _pwrbtn_h
#define _pwrbtn_h

namespace pwrbtn {

const unsigned kPollUs = 20000;		// the button read every 20 ms
const unsigned kDrainCapUs = 3000000;	// the drain's longest wait: a hung card does not hold the power-off
const unsigned kSettledUs = 100000;	// idle that long in a row: the other core's saves are done

// A press counts only once the button was seen up since boot (the press that powered the board on does not count).
// bRaw: the GPIO reads pressed now; debounced (): the slow check, asked only then.
struct Button
{
	bool bReleased = false;
	template <class TDebounced>
	bool Poll (bool bRaw, TDebounced debounced)
	{
		if (!bRaw) { bReleased = true; return false; }
		return bReleased && debounced ();
	}
};

// The drain: Done (bIdle, now) once idle has held kSettledUs, or kDrainCapUs after the start. Times in us, wrapping.
struct Drain
{
	unsigned nStart, nIdleAt = 0;
	bool bIdle = false;
	explicit Drain (unsigned nNow) : nStart (nNow) {}
	bool TimedOut (unsigned nNow) const { return nNow - nStart >= kDrainCapUs; }
	bool Done (bool bIdleNow, unsigned nNow)
	{
		if (!bIdleNow) bIdle = false;
		else if (!bIdle) bIdle = true, nIdleAt = nNow;
		return (bIdle && nNow - nIdleAt >= kSettledUs) || TimedOut (nNow);
	}
};

}

#endif
