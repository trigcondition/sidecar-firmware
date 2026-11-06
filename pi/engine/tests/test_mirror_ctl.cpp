// pi/engine/tests/test_mirror_ctl.cpp - usb=mirror's forwarded request and the box going away (mirror_ctl.h), driven
// in the order sidecar_gadget.cpp's handlers run: OnControlRequest (gadget IRQ), MirrorPoll (core 0's loop),
// MirrorControlDone (xHCI IRQ), OnControlAborted and the switch (end), MirrorBoxGone (box_gone, drop_pipe_outs).
#include "mirror_ctl.h"
#include "check.h"

#include <cstdio>

using mirror::Ctl;

// what MirrorPoll does with a pending request: forwarded when the box is up and the submit works, else STALLed
// unless the computer gave up meanwhile. Returns 'F' forwarded, 'S' STALLed, '-' nothing.
static char poll (Ctl &c, bool box_up, bool submit_ok, unsigned *pSeq)
{
	if (!c.pending) return '-';
	if (!c.take (pSeq)) return '-';
	if (c.forward (box_up, *pSeq) && submit_ok) return 'F';
	return c.not_forwarded (*pSeq) ? 'S' : '-';
}

struct FakeEP
{
	mirror::Role role;
	bool active, busy;
	int rearms = 0;
	mirror::Role Role (void) const	{ return role; }
	bool Active (void) const	{ return active; }
	bool Busy (void) const		{ return busy; }
	void Rearm (void)		{ ++rearms; busy = false; }
};

int main ()
{
	unsigned s;

	{	// a request forwarded and answered
		Ctl c;
		CHECK (c.request ());
		CHECK (poll (c, true, true, &s) == 'F' && c.at_box && !c.pending);
		CHECK (c.answer (s) && !c.at_box);
		CHECK (!c.box_gone ());			// nothing at the box: nothing to STALL
	}
	{	// one at a time: a second SETUP while one is pending is STALLed by the gadget, the first unchanged
		Ctl c;
		CHECK (c.request ());
		unsigned seq = c.seq;
		CHECK (!c.request () && c.seq == seq && c.pending);
	}
	{	// no box (gone, inside mirrorhold): STALLed, never at the box
		Ctl c;
		CHECK (c.request ());
		CHECK (poll (c, false, true, &s) == 'S' && !c.at_box);
	}
	{	// the submit fails: STALLed
		Ctl c;
		CHECK (c.request ());
		CHECK (poll (c, true, false, &s) == 'S' && !c.at_box);
	}
	{	// the box goes away with the request at it (m_bCtlAtBox): STALLed at once, and the old box's answer if it
		// ever comes is late
		Ctl c;
		CHECK (c.request ());
		CHECK (poll (c, true, true, &s) == 'F');
		CHECK (c.box_gone () && !c.at_box);
		CHECK (!c.box_gone ());			// once
		CHECK (!c.answer (s));
		// the computer's next request goes on as usual (here the box is still gone: STALLed by MirrorPoll)
		CHECK (c.request ());
		CHECK (poll (c, false, true, &s) == 'S');
	}
	{	// the box goes away with the request still pending (not taken by core 0's loop yet): not box_gone's, it is
		// MirrorPoll's to STALL, as the box is not up
		Ctl c;
		CHECK (c.request ());
		CHECK (!c.box_gone () && c.pending);
		CHECK (poll (c, false, true, &s) == 'S');
	}
	{	// the computer aborts the request at the box (a new SETUP, a reset): its answer is late, and box_gone has
		// nothing left to STALL
		Ctl c;
		CHECK (c.request ());
		CHECK (poll (c, true, true, &s) == 'F');
		c.end ();
		CHECK (!c.at_box && !c.pending);
		CHECK (!c.box_gone ());
		CHECK (!c.answer (s));
	}
	{	// aborted between take and the STALL (no box): no STALL to a request the computer gave up on
		Ctl c;
		CHECK (c.request ());
		CHECK (c.take (&s));
		c.end ();
		CHECK (!c.forward (true, s));
		CHECK (!c.not_forwarded (s));
	}
	{	// the answer comes in the xHCI IRQ before MirrorPoll's submit returns: at_box was set first, so it is cleared
		Ctl c;
		CHECK (c.request ());
		CHECK (c.take (&s) && c.forward (true, s));
		CHECK (c.answer (s) && !c.at_box);
		CHECK (!c.box_gone ());
	}
	{	// the gadget's switch (off the bus and back) ends whatever was going
		Ctl c;
		CHECK (c.request ());
		CHECK (poll (c, true, true, &s) == 'F');
		c.end ();
		CHECK (!c.answer (s) && !c.box_gone ());
	}

	{	// the pipes: a busy pipe OUT endpoint is dropped and re-armed (an inactive one too: its packet is the box's
		// until then, mirror_ep.h OutGate, and it arms at its activation)
		FakeEP po {mirror::kPipeOut, true, true}, idle {mirror::kPipeOut, true, false}, off {mirror::kPipeOut, false, true};
		FakeEP pi {mirror::kPipeIn, true, true}, au {mirror::kAudioOut, true, true};
		FakeEP *eps[mirror::kMaxEps] = {&po, nullptr, &idle, &off, &pi, &au};
		CHECK (mirror::drop_pipe_outs (eps, mirror::kMaxEps) == 2);
		CHECK (po.rearms == 1 && !po.busy && idle.rearms == 0 && off.rearms == 1 && !off.busy && pi.rearms == 0 && au.rearms == 0);
		CHECK (mirror::drop_pipe_outs (eps, mirror::kMaxEps) == 0);
	}

	puts ("test_mirror_ctl: ok");
	return 0;
}
