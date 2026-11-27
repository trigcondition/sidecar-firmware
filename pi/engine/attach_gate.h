// pi/engine/attach_gate.h - usb=mirror: a box is shown to the computer only once its audio runs both ways, and a box
// whose stream the USB host controller starves is restarted on the Pi, for ob_usb.cpp and the Mac's unit test
// (pi/engine/tests/test_attach_gate.cpp). design/ob-latency-fault-plan.md (project files), decided 2026-11-23.
//
// Why: Overbridge Engine measures a device's latency once, as soon as its helper process streams: a marker on OUT, the
// pulse expected back on IN within 1 s, given up on the 11th miss (~13 s), and the result (LatencyMeasurementTimeout,
// "Fault") latched until the device is attached again. On the LT9 restore boot (2026-11-23) the VL805 never served the
// Rytm's audio OUT (OUT 0/s from the first second, every URB at the controller, 5 periodic iv1 endpoints busy - the
// same count as the good boots), the relay's ring for the computer's OUT overran on every frame, the pulse never
// reached the box and the Engine faulted. An Engine restart measured into the same dead OUT; a Pi reboot cleared it.
//
// The gate (while a box is held, once a second from CObUsb::Log):
//   - its IN alive (kMinIn/s or more) and its OUT keeping up with it (kKeepPct % of IN or more; not judged with
//     obout=off) for kNeedSeconds in a row: Show. Judged against IN, not a fixed rate: the controller's starvation is
//     OUT short of IN with IN full (LT9: IN 6,860/s, OUT 0), while a stream that runs slower as a whole (the emulator's
//     ~6,000/s) still carries the Engine's pulse back.
//   - not full for kRestartAfter seconds: Restart (once per hold): its audio interfaces to alt 0 and back, and its
//     audio endpoints dropped and added again on the controller (new transfer rings, a new place in its schedule).
//   - still not full kReEnumAfter seconds after that: ReEnum (once per hold, only if allowed): the box removed and
//     enumerated again by its hub (the freeze path, ob_freeze.h). The hold goes on through it: the next attach of the
//     slot continues this one (begin), so the limit below counts from the first attach.
//   - hold_ms after the hold began: ShowLate (shown anyway, with a warning; the Engine may fault).
// A box already shown (or any box without a computer: usb=sidecar, class mode) whose OUT starves (periodic_starve.h's
// Start) is restarted the same way, at most kShownRestarts times a stream; never re-enumerated (the other box's stream
// stalls about 1 s with one, SR5 T483).
//
// Freestanding C++ (stdint.h only).
#ifndef SIDECAR_ATTACH_GATE_H
#define SIDECAR_ATTACH_GATE_H

#include <stdint.h>

namespace obgate {

static const unsigned kMinIn = 3000;		// blocks/s: a box streams ~6,860 (48 kHz / 7 frames); a starved endpoint runs in
						// steps of ~984/s (k services a ms), so k <= 3 is caught on IN as well
static const unsigned kKeepPct = 98;		// OUT at least this share of IN
static const unsigned kNeedSeconds = 2;
static const unsigned kRestartAfter = 3;	// seconds not full while held (periodic_starve.h names it after 3 too)
static const unsigned kReEnumAfter = 3;		// seconds not full after the restart
static const uint32_t kHoldMs = 12000;		// default obgate= (0: the gate off, a box shown at once as before). 12 s, not 10:
						// on the unit the first tick comes ~1 s into the hold, so a forced recovery shows at
						// ~9.1 s (GT2, 2026-11-23); 12 s leaves ~3 s for a boot-time stall inside the hold
static const unsigned kShownRestarts = 2;	// per stream, for a box already shown

enum Act { Wait, Show, ShowLate, Restart, ReEnum };

inline const char *act_name (Act a)
{
	return a == Show ? "show" : a == ShowLate ? "show-late" : a == Restart ? "restart" : a == ReEnum ? "reenum" : "wait";
}

// per slot: outlives the CObUsb a re-enumeration deletes
struct Gate
{
	bool held = false;		// descriptors given to the target, not shown yet
	uint32_t since_ms = 0;		// the hold began (the first attach of this episode)
	unsigned good = 0, bad = 0;	// full / not-full seconds in a row
	unsigned restarts = 0;		// this hold's
	bool reenumed = false;		// this hold re-enumerated the box
	bool reenum_due = false;	// it did, and the box is not back yet: its next attach continues the hold
	// since boot (/status)
	unsigned holds = 0, shown_late = 0, total_restarts = 0, total_reenums = 0;
	uint32_t last_wait_ms = 0;	// the last hold's length (shown or shown late)
};

// a box's descriptors went to its target: the hold begins (or goes on, after the gate's own re-enumeration)
inline void begin (Gate &g, uint32_t now_ms, uint32_t hold_ms = kHoldMs)
{
	// (a box that did not come back from the gate's re-enumeration until much later starts a hold of its own)
	if (g.reenum_due && (now_ms - g.since_ms) > 2 * hold_ms + 10000) g.reenum_due = false;
	if (!g.reenum_due)
	{
		g.since_ms = now_ms;
		g.restarts = 0;
		g.reenumed = false;
		g.holds++;
	}
	g.reenum_due = false;
	g.held = true;
	g.good = g.bad = 0;
}

// the box left while held (not by the gate's re-enumeration): the hold ends with nothing shown
inline void gone (Gate &g)
{
	if (!g.reenum_due) g.held = false;
}

inline bool full (unsigned in_rate, unsigned out_rate, bool out_judged)
{
	return in_rate >= kMinIn && (!out_judged || (unsigned long long) out_rate * 100 >= (unsigned long long) in_rate * kKeepPct);
}

// once a second while held: the second's IN and OUT rates. allow_reenum: obgatereenum (default on)
inline Act tick (Gate &g, unsigned in_rate, unsigned out_rate, bool out_judged, uint32_t now_ms, uint32_t hold_ms,
		 bool allow_reenum)
{
	if (!g.held) return Wait;
	if (full (in_rate, out_rate, out_judged))
	{
		g.bad = 0;
		if (++g.good >= kNeedSeconds)
		{
			g.held = false;
			g.last_wait_ms = now_ms - g.since_ms;
			return Show;
		}
	}
	else
	{
		g.good = 0;
		g.bad++;
	}
	if ((int32_t) (now_ms - g.since_ms) >= (int32_t) hold_ms)
	{
		g.held = false;
		g.shown_late++;
		g.last_wait_ms = now_ms - g.since_ms;
		return ShowLate;
	}
	if (g.restarts == 0 && g.bad >= kRestartAfter)
	{
		g.restarts++, g.total_restarts++;
		g.bad = 0;
		return Restart;
	}
	if (g.restarts > 0 && !g.reenumed && allow_reenum && g.bad >= kReEnumAfter)
	{
		g.reenumed = true;
		g.reenum_due = true;
		g.total_reenums++;
		g.bad = 0;
		return ReEnum;
	}
	return Wait;
}

// a stream that is not held (shown, or no computer): its OUT starvation named (PeriodicStarve::Start). TRUE: restart it
inline bool starved (unsigned &stream_restarts)
{
	if (stream_restarts >= kShownRestarts) return false;
	stream_restarts++;
	return true;
}

} // namespace obgate

#endif
