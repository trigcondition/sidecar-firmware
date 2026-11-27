// pi/engine/ob_freeze.h - when to re-enumerate an Overbridge box whose stream froze while the box stayed on the bus,
// for ob_usb.cpp (CObUsb::PollHalt) and the Mac's unit test (pi/engine/tests/test_ob_freeze.cpp).
//
// Two boxes on the Pi 4's USB-A ports (the VL805 hub): unplugging one can freeze the other (KB T469 SR1, T472 SR2,
// 2026-11-22, either box, the default build df2f029 too). The box that stays is still enumerated, but its isochronous
// IN / OUT stop at the same millisecond (0/s), its control and MIDI transfers time out, the engine renders nothing,
// and the endpoint loop's resubmits ("quiet or failed but not halted: resubmitted", ep_halt.h) never bring it back:
// 41 in a row over 75 s in SR1. The hub did not reset its port. When it does (AO1, T464: the port re-enumerated 6 s
// after the other box's unplug), or when the unplugged box is plugged in again (SR2: the hub re-enumerated the frozen
// box's port), the box streams again at once. So the escalation here is the software version of that: remove the
// device (the hub port disabled, the xHCI slot freed, this driver deleted, as for an unplug) and have its hub
// enumerate it again (port reset, a fresh slot and address). The attach-order slot hold (5 s, core/device) keeps its
// slot meanwhile.
//
// When: the stream's endpoint loop has been resetting or resubmitting with no good completion between (EpHalt::fails
// > 0, the larger of IN and OUT) for kHoldMs. The known short stall two boxes show 4 to 8 s after they start (about
// 846 ms) is well under it, and a box being unplugged is removed by Circle's own hot-plug path long before (its
// endpoints fail together and EpBox holds their resets for the hub). kTries per slot; a slot that streams for
// kForgetMs after its last try has the whole budget again. A spent budget leaves the endpoint loop going, and a
// replug by hand.
//
// The hub that does not answer (RL1, KB T516, 2026-11-26): the frozen-stream path fired on a unit for the first time
// after a plug-in knock. Seen: the A4 went silent at 00:16:45.84 (a lone IN error, then its CLEAR_FEATURE timed out), and
// the first request to the Pi's own USB 2 hub after that (the VL805's 2109:3431), this path's port disable 7 s later,
// got no answer. Inferred, not seen: the hub stopped at about 45.84 too (the Rytm plugged into its port 4 was never
// reported; earlier cycles reported it within about 1 s). Why it stopped is not known. The port disable timed out (3 s), and
// the removal was ignored: nothing was deleted, the rescan that followed waited 3 s on each of three more requests to
// the same hub, and the next two tries did the same (12 s of the main loop each, 15 s with the relay's poll after it).
// Circle also leaves a timed-out control transfer on the endpoint's ring (CXHCIEndpoint::Transfer only drops the URB),
// so every later request to that hub waits behind it. Now (Hub below): a removal that fails after about the request
// timeout is the hub not answering. Nothing more is sent to it, and its own root port is reset instead (obhubreset=1,
// the default): the xHC drives that reset itself (PORTSC, no request to the hub), Circle deletes the hub with every
// device behind it - the other box and the relay too, each held its 5 s slot - and enumerates it again (a freeze try
// spent: the box comes back with the hub). kHubResets a boot, kHubGapMs apart. Spent or off: no request goes to that
// hub for kDeadMs, said once, and the freeze tries meanwhile are given back (nothing happened to the box). A quick
// refusal (the hub answered no) is a try spent, without the rescan. obhubreset! runs the root-port reset at once (a
// test of the teardown and the re-enumeration, the hub answering or not). Not known: whether that reset brings a hub
// in RL1's state back (RL1 needed two Pi power cycles).
//
// Freestanding C++ (stdint.h only), so the timeline can be replayed on the Mac.
#ifndef SIDECAR_OB_FREEZE_H
#define SIDECAR_OB_FREEZE_H

#include <stdint.h>

namespace obfreeze {

static const uint32_t kHoldMs = 4000;
static const uint32_t kForgetMs = 60000;
static const unsigned kTries = 3;

// per stream (CObUsb object): the current failing run
struct Watch
{
	bool run = false;	// a run of resets / resubmits with no good completion is going on
	uint32_t run_ms = 0;	// since when
};

// per slot, outlives the CObUsb that a re-enumeration deletes
struct Budget
{
	unsigned tries = 0;
	bool seen = false;	// a try happened (last_ms is valid)
	uint32_t last_ms = 0;	// the last try
	bool told = false;	// "spent" logged for this run of tries
};

// The main loop, every pass, for a streaming box: fails = its endpoints' resets in a row with no good completion
// (the larger of IN and OUT). true: re-enumerate it now (a try spent; w and b updated).
inline bool pass (Watch &w, Budget &b, unsigned fails, uint32_t now_ms)
{
	if (fails == 0)
	{
		w.run = false;
		if (b.seen && now_ms - b.last_ms > kForgetMs) b = Budget ();
		return false;
	}
	if (!w.run)
	{
		w.run = true;
		w.run_ms = now_ms;
		return false;
	}
	if ((int32_t) (now_ms - w.run_ms) < (int32_t) kHoldMs || b.tries >= kTries) return false;
	b.tries++;
	b.seen = true;
	b.last_ms = now_ms;
	w.run = false;
	return true;
}

// the budget is spent and the freeze still goes on: true once (the caller logs that a replug by hand is needed)
inline bool spent_now (const Watch &w, Budget &b, uint32_t now_ms)
{
	if (b.tries < kTries || b.told || !w.run || (int32_t) (now_ms - w.run_ms) < (int32_t) kHoldMs) return false;
	b.told = true;
	return true;
}

// ---- the hub that does not answer (the header above) ----

static const uint32_t kNoAnswerUs = 2500000;	// a failed removal this slow is Circle's 3 s request timeout, not a "no"
static const uint32_t kDeadMs = 60000;		// no request to a hub that did not answer for this long
static const unsigned kHubResets = 2;		// root-port resets of the hub a boot
static const uint32_t kHubGapMs = 60000;	// at least this far apart

struct Hub
{
	bool dead = false;	// did not answer (dead_ms valid)
	uint32_t dead_ms = 0;
	bool told = false;	// a "nothing sent" line logged for this dead spell
	unsigned resets = 0;	// root-port resets since boot
	uint32_t reset_ms = 0;	// the last one (resets > 0)
};

// What a failed removal that took took_us says: true, the hub did not answer
inline bool no_answer (uint32_t took_us) { return took_us >= kNoAnswerUs; }

// Before any request to the hub: true, it did not answer less than kDeadMs ago (send nothing). Past it, it may be asked
// again (one more 3 s wait at most).
inline bool quiet (Hub &h, uint32_t now_ms)
{
	if (!h.dead) return false;
	if (now_ms - h.dead_ms < kDeadMs) return true;
	h.dead = false;
	h.told = false;
	return false;
}

// The hub did not answer at now_ms. true: reset its root port now (a reset spent); false: marked dead (quiet() above)
inline bool dead_now (Hub &h, uint32_t now_ms, bool allowed)
{
	if (allowed && h.resets < kHubResets && (h.resets == 0 || now_ms - h.reset_ms >= kHubGapMs))
	{
		h.resets++;
		h.reset_ms = now_ms;
		h.dead = false;		// a fresh hub after the reset
		h.told = false;
		return true;
	}
	h.dead = true;
	h.dead_ms = now_ms;
	h.told = true;			// the caller says so now
	return false;
}

}	// namespace obfreeze

#endif
