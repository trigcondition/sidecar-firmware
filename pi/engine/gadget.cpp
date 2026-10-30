// pi/engine/gadget.cpp - gadget.h
#include "gadget.h"
#include "mbtrace.h"

#include <circle/bcmpropertytags.h>
#include <circle/usb/usb.h>

static const char From[] = "gadget";

// String 1 (manufacturer) and 2 (product); 0 (the language) and the descriptors stay Circle's.
const void *CSidecarMIDIGadget::GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength)
{
	if ((wValue >> 8) == DESCRIPTOR_STRING)
	{
		switch (wValue & 0xFF)
		{
		case 1:	return ToStringDescriptor ("Sidecar", pLength);
		case 2:	return ToStringDescriptor ("Sidecar", pLength);
		default: break;
		}
	}
	return CUSBMIDIGadget::GetDescriptor (wValue, wIndex, pLength);
}

boolean CSidecarMIDIGadget::Start (void)
{
	if (!Initialize ()) return FALSE;
	asm volatile ("mrs %0, cntpct_el0" : "=r" (m_nLastStamp));
	return TRUE;
}

// IRQ context (the OUT endpoint's completion, core 0). Circle hands over each event packet's 3 MIDI bytes; the packet's
// header byte (cable, CIN) is the byte before them in the endpoint's buffer, so the echo is the packet itself, bit for bit.
void CSidecarMIDIGadget::PacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength, unsigned nDevice, void *pParam)
{
	CSidecarMIDIGadget *pThis = (CSidecarMIDIGadget *) pParam;
	pThis->m_nInPackets++;
	CUSBMIDIDevice *pMIDI = pThis->GetMIDIDevice ();
	if (pMIDI == 0 || !pMIDI->SendEventPackets (pPacket - 1, CUSBMIDIDevice::EventPacketSize)) pThis->m_nEchoFail++;
}

void CSidecarMIDIGadget::Poll (void)
{
	UpdatePlugAndPlay ();
	CUSBMIDIDevice *pMIDI = GetMIDIDevice ();
	if (pMIDI != m_pBound)
	{
		m_pBound = pMIDI;
		if (pMIDI != 0)
		{
			pMIDI->RegisterPacketHandler (PacketHandler, this);
			CLogger::Get ()->Write (From, LogNotice, "configured by the computer (%s speed): MIDI echo on",
						GetNegotiatedUSBSpeed () == HighSpeed ? "high" : "full");
		}
	}
}

void CSidecarMIDIGadget::LogStats (CLogger *pLogger)
{
	unsigned nIRQs;
	u64 nTicks, nNow, nFrq;
	GetIRQStats (&nIRQs, &nTicks);
	asm volatile ("mrs %0, cntpct_el0" : "=r" (nNow));
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (nFrq));
	u64 nSpan = nNow - m_nLastStamp;
	if (nSpan == 0) return;
	unsigned nRate = (unsigned) ((u64) (nIRQs - m_nLastIRQs) * nFrq / nSpan);
	unsigned nPpm = (unsigned) ((nTicks - m_nLastTicks) * 1000000 / nSpan);	// of core 0's time
	// the firmware's throttled flags (USB-C also powers the Pi): bit 0 under-voltage now, bit 16 since boot
	// (read without clearing: the request value 0 leaves the sticky bits)
	CBcmPropertyTags Tags;
	TPropertyTagSimple Throttled;
	Throttled.nValue = 0;
	unsigned nPower;
	{ mbtrace::Scope Mb (6); nPower = Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled, sizeof Throttled, 4) ? Throttled.nValue : ~0u; }
	pLogger->Write (From, LogNotice, "%s | midi in %u packets, echo fail %u | irq %u/s %u.%02u %% of core 0 (total %u) | throttled 0x%X",
			m_pBound ? "configured" : "waiting", m_nInPackets, m_nEchoFail, nRate, nPpm / 10000, nPpm / 100 % 100, nIRQs,
			nPower);
	m_nLastIRQs = nIRQs;
	m_nLastTicks = nTicks;
	m_nLastStamp = nNow;
}
