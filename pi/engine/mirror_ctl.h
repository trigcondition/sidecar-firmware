// pi/engine/mirror_ctl.h - usb=mirror (docs/usb-mirror.md): the state of a class / vendor request the computer sent
// the box through the Sidecar, and what the box going away does to it. Portable (no Circle): sidecar_gadget.cpp keeps
// one Ctl, tests/test_mirror_ctl.cpp drives it as the gadget IRQ, core 0's loop and the xHCI IRQ would.
//
// One request at a time:
//   gadget IRQ      request ()          the computer's setup taken (false: one is pending already, STALL this one)
//   core 0's loop   take ()             to forward it; forward (box up) just before CObUsb::MirrorControl submits it
//                   not_forwarded ()    no box or the submit failed: STALL it, unless the computer gave up meanwhile
//   xHCI IRQ        answer (seq)        the box's answer: false when it is for an older request (late, dropped)
//   gadget IRQ      end ()              the computer aborted it (a new SETUP, a reset) or the gadget left the bus
//   IRQs masked     box_gone ()         the box went away with the request at it: true means STALL it now, as its
//                                       answer never comes (EP0 would NAK until the computer gave up)
// Each request has its sequence number; end () and box_gone () move it on, so an answer still on its way is dropped.
//
// drop_pipe_outs (): the other half of the box going away, on the pipes. A pipe OUT packet the computer sent and the
// box never took leaves its gadget endpoint busy (it would NAK for good): each one is dropped and re-armed (re-armed
// when active; an inactive one, as across a bus suspend, keeps no packet of the box and arms at its activation).
#ifndef _mirror_ctl_h
#define _mirror_ctl_h

#include "mirror_desc.h"

namespace mirror {

struct Ctl
{
	volatile unsigned seq = 0;
	volatile bool pending = false;		// taken by the gadget IRQ, not yet by core 0's loop
	volatile bool at_box = false;		// forwarded to the box, its answer not back yet

	bool request (void)			{ if (pending) return false; seq = seq + 1; pending = true; return true; }
	void end (void)				{ seq = seq + 1; pending = false; at_box = false; }
	bool take (unsigned *pSeq)		{ *pSeq = seq; bool b = pending; pending = false; return b; }
	bool forward (bool box_up, unsigned s)	{ at_box = box_up && s == seq; return at_box; }
	bool not_forwarded (unsigned s)		{ at_box = false; return s == seq; }
	bool answer (unsigned s)		{ if (s != seq) return false; at_box = false; return true; }
	bool box_gone (void)			{ if (!at_box) return false; at_box = false; seq = seq + 1; return true; }
};

// EP: Role (), Busy (), Rearm () (sidecar_gadget.h CMirrorEP). The packets dropped.
template <class EP>
unsigned drop_pipe_outs (EP *const *ppEP, unsigned n)
{
	unsigned nDropped = 0;
	for (unsigned k = 0; k < n; k++)
	{
		EP *p = ppEP[k];
		if (p != 0 && p->Role () == kPipeOut && p->Busy ())
		{
			nDropped++;
			p->Rearm ();
		}
	}
	return nDropped;
}

}	// namespace mirror

#endif
