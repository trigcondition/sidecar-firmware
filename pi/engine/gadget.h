// pi/engine/gadget.h - "Sidecar" on the Pi 4's USB-C port (Circle's DWC2 gadget, device mode), beside the xHCI host on
// USB-A (the Rytm). P1 of the Overbridge pass-through (docs/superpowers/specs/2026-10-16-overbridge-passthrough-spike.md):
// Circle's USB MIDI gadget with our strings; the computer's MIDI is echoed back to it (a loopback for the Mac-side
// test, pi/engine/tests/gadget_midi_test.py) and counted. cmdline/console: gadget=0 leaves the port off.
#ifndef _gadget_h
#define _gadget_h

#ifdef SIDECAR_NO_GADGET
// Pi 5 (Makefile): no gadget in Circle there; a stand-in that never starts (docs/pi5.md)
#include <circle/device.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/types.h>
class CSidecarMIDIGadget
{
public:
	CSidecarMIDIGadget (CInterruptSystem *) {}
	boolean Start (void)				{ return FALSE; }
	void Poll (void)				{}
	boolean IsMIDIDevice (const CDevice *) const	{ return FALSE; }
	void LogStats (CLogger *)			{}
};
#else
#include <circle/usb/gadget/usbmidigadget.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/types.h>

class CSidecarMIDIGadget : public CUSBMIDIGadget
{
public:
	CSidecarMIDIGadget (CInterruptSystem *pInterrupt) : CUSBMIDIGadget (pInterrupt) {}

	boolean Start (void);			// Initialize (power, core, EP0): the computer enumerates it from here
	void Poll (void);			// core 0's loop: plug-and-play, the MIDI device's handler
	boolean IsMIDIDevice (const CDevice *p) const	{ return p != 0 && p == GetMIDIDevice (); }
	void LogStats (CLogger *pLogger);	// once a second: messages, bytes, IRQs and their time

protected:
	const void *GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength) override;

private:
	static void PacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength, unsigned nDevice, void *pParam);

	CUSBMIDIDevice *m_pBound = 0;
	volatile unsigned m_nInPackets = 0, m_nEchoFail = 0;
	unsigned m_nLastIRQs = 0;
	u64 m_nLastTicks = 0, m_nLastStamp = 0;
};
#endif

#endif
