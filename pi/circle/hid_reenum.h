// pi/circle/hid_reenum.h - when to give up on resetting a USB HID report endpoint and re-enumerate the whole device,
// for Circle's HID driver (pi/circle/circle-hid-reenum.patch; build.sh copies this file into the Circle tree as
// include/circle/usb/hidreenum.h) and the Mac's unit test (pi/engine/tests/test_hid_reenum.cpp).
//
// Why a separate header: the policy is a state machine spread over thousands of CUSBHIDDevice::Poll() passes and two
// object lifetimes, and the first version of it was wrong on hardware in a way that only a replay could show (see
// "The 2026-10-25 bug" below). It is kept free of Circle types so the timeline can be replayed on the Mac.
//
// What the escalation is for: an error on the panel's interrupt IN endpoint halts it in the xHC. circle-hid.patch
// resets it from halted and submits the request again from the main loop, and for an ordinary transfer error that
// works. The GeeekPi 7" case's touch screen (0416:c169) can get into a state where it does not: measured on the real
// device, 4201 endpoint resets and 4201 resubmits all reported success, the endpoint context stayed at state 2
// (halted), the xHCI completion code stayed 36 (split transaction error) and not one report was ever delivered. A
// cold power cycle of the Pi did not clear it; pulling the panel's USB cable out and putting it back did. So this
// escalates to the software equivalent of that replug: remove the device (the hub port is disabled and the xHCI slot
// freed) and have the parent enumerate it again with a fresh address and device context.
//
// The 2026-10-25 bug, which is what this header exists to stop coming back: the "has the recovery been failing long
// enough" clock used to be cleared whenever Poll() saw the endpoint in state 1 ("running"). But ResetFromHalted()
// plus the doorbell from the resubmit put a dead endpoint into state 1 for as long as the transfer is in flight, so
// that sighting is the recovery's own doing and says nothing about the device. Whenever the error took longer to come
// back than one main-loop pass, Poll() saw state 1, cleared the clock, and the escalation never reached the ten
// seconds it needed. On hardware that stalled the ladder at two attempts out of three with the error count climbing
// past 150, and the exhausted state was never reached, so nothing in the log said the ladder had stopped.
//
// The rule that follows: these clocks are driven by the error run and by new errors only. The endpoint state may say
// "do not escalate yet" (it is an input to armed-ness) but must never be able to rewind the clock on its own.
//
// No allocation, no library: freestanding C++ (stdint.h only, for the test's benefit).
#ifndef SIDECAR_HID_REENUM_H
#define SIDECAR_HID_REENUM_H

#include <stdint.h>

namespace hidreenum {

// How long the endpoint-level recovery must have been in its given-up state (the HID driver's
// MaxConsecutiveErrors reached, i.e. 50 consecutive transfer errors with no report in between) before the first
// escalation. Errors come back as fast as the transfers complete, so the count reaches 50 within a second and the
// recovery then retries once a second: 10 s means 50 immediate errors plus about ten reset-and-resubmit cycles that
// each reported success and still produced no report. That is far past any transient - a bus glitch or a device
// hiccup is over in one or two retries - and past the hub's 510 ms power-good and 100 ms port-reset delays, so a
// device that is merely still coming up is never touched. It is short enough that the screen is back before whoever
// is poking at it has given up.
static const unsigned HoldMs = 10000;

// The whole budget for one device, and how much of it is spent on the plain re-enumeration before the port power
// cycle is added. Measured 2026-10-25: a plain re-enumeration does revive this panel - counters to zero, endpoint
// state 1, a report pending, twice, with no cable touched - but it re-halts 5 to 10 s later every time. Two plain
// tries are therefore enough to learn that plain does not hold; tries 3 and 4 drop VBUS on the hub port as well,
// which is the part of a replug that a removal cannot imitate. Four in all: if two real power cycles do not hold
// either, it is the hardware and more resets will not help.
static const unsigned PlainTries = 2;
static const unsigned MaxTries = 4;

// A floor between escalations, held in the budget rather than on the device, so it survives the re-enumeration that
// destroys the device. Each try also has to earn its own given-up run and its own HoldMs of silence on the new
// object, so this is only a backstop - but it means the spacing no longer depends on the per-device clock at all.
static const unsigned ApartMs = 10000;

// The budget is handed back when the endpoint has been armed and has produced no new transfer error for this long,
// which is a real recovery rather than a device that re-halts on its first transfer. Deliberately not "a report
// arrived": an idle touch screen sends no reports at all (the measured healthy line is "rep +0 ... ep state 1
// pend 1"), so waiting for one would never forgive. 30 s is three times HoldMs, so a device that keeps dying cannot
// refund itself faster than it spends.
static const unsigned ForgiveMs = 30000;

enum Step
{
	StepNone = 0,		// nothing to do
	StepPlain,		// remove the device and have the parent enumerate it again
	StepPowerCycle		// the same, with VBUS dropped on the hub port in between
};

// The budget: one device at a time, keyed on its place in the USB tree (root hub port plus route string). That key
// is the physical topology, so it outlives a re-enumeration while the CUSBDevice and its functions do not - which
// is the whole reason the budget cannot live on the device.
struct Budget
{
	unsigned Key;		// 0: nobody holds the budget
	unsigned Tries;		// escalations spent on Key
	unsigned Total;		// escalations since boot, for the diagnostics
	unsigned LastHZ;	// tick of the last escalation, +1; 0: none yet
	bool	 Spent;		// Key has used its budget up: nothing more is tried
};

// The clocks, per device object. A re-enumeration destroys the device, so a new one starts these again - that is
// intended: every attempt has to prove on its own that it did not work.
struct Hold
{
	unsigned GaveUpHZ;	// first tick of the current given-up run, +1; 0: not given up
	unsigned HealthyHZ;	// first tick of the current healthy run, +1; 0: not healthy
	unsigned HealthyErrors;	// the device's total error count when HealthyHZ was taken
};

struct Input
{
	unsigned nowHZ;		// CTimer::GetTicks()
	unsigned hz;		// Circle's HZ (ticks per second)
	unsigned Key;		// the device's place in the USB tree; 0: unknown, never escalate
	unsigned Errors;	// the device's transfer errors so far (only ever increases)
	bool	 bGaveUp;	// the endpoint recovery has given up: MaxConsecutiveErrors consecutive errors
	bool	 bArmed;	// the endpoint is running with a request queued
};

inline unsigned Ticks (unsigned nMs, unsigned nHz)
{
	return (unsigned) ((uint64_t) nMs * nHz / 1000);
}

// How long the given-up run has held, for the diagnostics; 0 when no run is in progress.
inline unsigned HeldMs (const Hold &rHold, unsigned nowHZ, unsigned nHz)
{
	if (rHold.GaveUpHZ == 0 || nHz == 0)
	{
		return 0;
	}

	return (unsigned) ((uint64_t) (nowHZ + 1 - rHold.GaveUpHZ) * 1000 / nHz);
}

inline void Forget (Budget *pBudget)
{
	pBudget->Key = 0;
	pBudget->Tries = 0;
	pBudget->LastHZ = 0;
	pBudget->Spent = false;
}

// Advances the clocks and says what Poll() should do now. Pure: all the state is the two structs, and every
// comparison is an unsigned tick difference, so the tick counter's own wraparound is handled.
//
// Call this on every Poll() pass, before any other early return: the clocks must see every pass, and in particular
// the given-up clock must not depend on which branch of the endpoint recovery happens to run.
inline Step Decide (Hold *pHold, Budget *pBudget, const Input &rIn)
{
	// The given-up clock, driven by the error run and nothing else. No endpoint-state sighting may rewind it:
	// ResetFromHalted() plus the resubmit's doorbell put a dead endpoint into state 1, so "running" is our own
	// doing. bGaveUp only goes false again when a report got through (the driver clears its consecutive-error
	// count on success) or when two seconds passed with no error at all, and either of those is real news.
	if (!rIn.bGaveUp)
	{
		pHold->GaveUpHZ = 0;
	}
	else if (pHold->GaveUpHZ == 0)
	{
		pHold->GaveUpHZ = rIn.nowHZ + 1;		// +1: 0 means "no run in progress"
	}

	// The health clock: armed, not given up, and no new transfer error since it started.
	if (   !rIn.bArmed
	    || rIn.bGaveUp)
	{
		pHold->HealthyHZ = 0;
	}
	else if (   pHold->HealthyHZ == 0
		 || pHold->HealthyErrors != rIn.Errors)
	{
		pHold->HealthyHZ = rIn.nowHZ + 1;		// start, or start again on a new error
		pHold->HealthyErrors = rIn.Errors;
	}
	else if (   rIn.nowHZ + 1 - pHold->HealthyHZ >= Ticks (ForgiveMs, rIn.hz)
		 && rIn.Key != 0
		 && rIn.Key == pBudget->Key)
	{
		Forget (pBudget);				// a real recovery: the budget is handed back
	}

	if (   !rIn.bGaveUp
	    || rIn.Key == 0)
	{
		return StepNone;
	}

	if (rIn.nowHZ + 1 - pHold->GaveUpHZ < Ticks (HoldMs, rIn.hz))
	{
		return StepNone;				// not long enough yet
	}

	if (rIn.Key != pBudget->Key)
	{
		Forget (pBudget);				// another device: it gets its own budget
		pBudget->Key = rIn.Key;
	}

	if (pBudget->Spent)
	{
		return StepNone;				// given up for good; the reset loop carries on
	}

	if (   pBudget->LastHZ != 0
	    && rIn.nowHZ + 1 - pBudget->LastHZ < Ticks (ApartMs, rIn.hz))
	{
		return StepNone;				// too soon after the last one
	}

	Step TheStep = pBudget->Tries < PlainTries ? StepPlain : StepPowerCycle;

	pBudget->Tries++;
	pBudget->Total++;
	pBudget->LastHZ = rIn.nowHZ + 1;
	if (pBudget->Tries >= MaxTries)
	{
		pBudget->Spent = true;				// that was the last one
	}

	return TheStep;
}

}

#endif
