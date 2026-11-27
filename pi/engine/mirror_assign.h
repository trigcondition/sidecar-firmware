//
// mirror_assign.h - usb=mirror with two boxes (slots by attach order, core/device/device.h; ob_usb.cpp runs it with prefer
// kFirst, attach order: cmdline mirror= retired 2026-11-21): which box the
// Pi's USB-C gadget shows and which one a Teensy relay shows (docs/usb-mirror.md "Which box goes where"). Decided at run
// time, so one build serves any mix of boxes:
//
//   - One route is filled first: USB-C (cmdline mirrorfirst=usbc, the default) or the Teensy relay (mirrorfirst=relay, to
//     test the relay with one box). Below, "the first route" is that one and "the second" the other.
//   - A box on its own always goes to the first route, whichever box it is.
//   - Both there: the one cmdline mirror= names takes the first route (mirror=rytm, the default, or mirror=a4), the other
//     the second. mirror=first: whichever box came first keeps the first route.
//   - A box on the first route is never moved away while it is there: one that arrives later goes to the second route,
//     even the preferred one (the computer's session with the box already shown is not cut).
//   - The first route's box left: its place is held for mirrorhold seconds (a box rebooting, as after an OS update, comes
//     back as itself); after that the first route is free, and the second route's box (if any) moves to it.
//   - At boot the first route waits up to mirrorsettle seconds (default 15) for the preferred box before it takes the
//     other one alone; the preferred box takes it at once.
//   - mirrorfirst=relay with no relay attached: the lone box is not shown on USB-C (it waits for the relay).
//
// Pure logic, no Circle: ob_usb.cpp feeds it the boxes it streams and the clock, tests/test_mirror_assign.cpp drives it.
//
#ifndef _mirror_assign_h
#define _mirror_assign_h

#include <stdint.h>

namespace mirror_assign {

const int kNone = -1;
const int kFirst = -1;			// Config::prefer: first come, first served

struct Config
{
	int prefer = 0;			// the slot that takes the first route when both are there (0 the Rytm, 1 the other box), or kFirst
	bool relay_first = false;	// mirrorfirst=relay: the Teensy relay is the first route, USB-C the second
	uint64_t settle_us = 15000000;	// from boot: how long a free first route waits for the preferred box
	uint64_t hold_us = 10000000;	// a first-route box that left keeps its place this long
};

struct State
{
	int first = kNone;		// the slot the first route shows (kNone: none yet, or free again)
	bool up[2] = {false, false};	// the box of the slot streams (it came up)
	uint64_t up_at[2] = {0, 0};	// when it came up
	uint64_t gone_at[2] = {0, 0};	// when it left
};

inline int parse_prefer (const char *p)
{
	if (p == 0) return 0;
	if (p[0] == 'a' && p[1] == '4' && p[2] == 0) return 1;
	if (p[0] == 'f' && p[1] == 'i' && p[2] == 'r' && p[3] == 's' && p[4] == 't' && p[5] == 0) return kFirst;
	return 0;			// rytm, or anything else: the Rytm as ever
}

inline const char *slot_name (int k)	{ return k == 0 ? "rytm" : k == 1 ? "a4" : "none"; }
inline const char *prefer_name (int p)	{ return p == kFirst ? "first" : slot_name (p); }
inline bool parse_relay_first (const char *p)	{ return p != 0 && p[0] == 'r' && p[1] == 'e' && p[2] == 'l' && p[3] == 'a' && p[4] == 'y' && p[5] == 0; }

// The slot the first route should show now (kNone: none). now_us counts from boot.
inline int decide (const Config &c, const State &s, uint64_t now_us)
{
	int g = s.first;
	if (g == 0 || g == 1)
	{
		if (s.up[g]) return g;					// there: it stays
		if (now_us - s.gone_at[g] < c.hold_us) return g;	// gone a moment: held for it
	}
	const bool r = s.up[0], a = s.up[1];
	if (!r && !a) return kNone;
	if (r && !a) g = 0;
	else if (a && !r) g = 1;
	else if (c.prefer == kFirst) g = s.up_at[1] < s.up_at[0] ? 1 : 0;	// (the same moment: the Rytm)
	else return c.prefer;
	if (c.prefer == kFirst || g == c.prefer) return g;
	// the other box alone: it waits for the preferred one until the boot's settle time is over
	return now_us >= c.settle_us ? g : kNone;
}

inline int other_slot (int k)	{ return k == 0 ? 1 : k == 1 ? 0 : kNone; }
// the slots USB-C and the relay show, from the first route's (either may have no box there)
inline int usbc_slot (const Config &c, int first)	{ return c.relay_first ? other_slot (first) : first; }
inline int relay_slot (const Config &c, int first)	{ return c.relay_first ? first : other_slot (first); }

}  // namespace mirror_assign

#endif
