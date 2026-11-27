// pi/engine/link1.cpp - see link1.h.
#include "link1.h"

#include <circle/usb/usbmidi.h>
#include <circle/util.h>

#include "bridge.h"
#include "device/device.h"
#include "usbmidi_pack.h"

namespace link1 {

Spsc<uint8_t, kInBytes> g_In;
Spsc<uint8_t, kOutBytes> g_Out;

static RecordReader<Spsc<uint8_t, kOutBytes>> s_Reader (&g_Out);
static CUSBMIDIDevice *volatile s_pDevice;	// core 0 writes, the control core reads (Poll: its sender while there)
static TStats s_Stats;
static SysExAsm<4096> s_Asm;			// the control core's
static RealtimeAsm s_Rt;			// the control core's: the box's clock (3(c) step 10)
static Cc120Asm s_Cc;			// ... and its CC 120 (All Sound Off): its own send FX cleared (3(c))
static TTap *volatile s_pTap;
static Spsc<uint32_t, kFromComputerPackets> s_FromComputer;	// xHCI IRQ -> core 0's loop (Send), whole event packets
static boolean s_bComputerSysEx;		// the computer's SysEx is part way out: the engine's records wait

const TStats &Stats (void) { return s_Stats; }
CUSBMIDIDevice *Device (void) { return s_pDevice; }

void Packet (unsigned nCable, uint8_t *pPacket, unsigned nLength)
{
	for (unsigned i = 0; i < nLength; i++)
		if (!g_In.push (pPacket[i])) s_Stats.nInLost++;
	asm volatile ("dsb sy; sev");
}

void SetTap (TTap *pTap) { s_pTap = pTap; }

void Tap (const uint8_t *pEventPacket)
{
	TTap *p = s_pTap;
	if (p != 0) p (pEventPacket);
}

bool FromComputer (const uint8_t *pEventPacket)
{
	uint32_t w;
	memcpy (&w, pEventPacket, 4);
	return s_FromComputer.push (w);
}

unsigned FromComputerRoom (void) { return kFromComputerPackets - s_FromComputer.size (); }

// usbmidi=os: for a box on the Teensy relay (teensy_target.cpp): the computer's packets lost on the way, what still waits,
// and a cut. The counter and the flag move in core 0's loop (Send); CutFromComputer is called there too (MirrorPoll)
static volatile unsigned s_nComputerLost;
static boolean s_bLastComputer;			// the transfer in flight holds the computer's packets
unsigned ComputerLost (void) { return s_nComputerLost; }
bool ComputerBusy (void) { return s_FromComputer.size () != 0 || s_bComputerSysEx; }
unsigned CutFromComputer (void)
{
	uint32_t w;
	unsigned n = 0;
	while (s_FromComputer.pop (&w)) n++;
	s_bComputerSysEx = FALSE;
	return n;
}

void SetDevice (CUSBMIDIDevice *pDevice)
{
	s_pDevice = pDevice;
	asm volatile ("dsb sy; sev");
}

// One record at a time, one transfer in flight (circle-midi-async.patch): the box never holds core 0's loop. Each transfer
// is at most kTxBytes, the async send's TxBufferSize: a longer one is refused and lost (an A4 OS upgrade through the mirror
// stopped at offset 0, T358, when link1 sent more than the 64 bytes it took then). 512 bytes (circle-midi-async512.patch,
// 128 event packets, one high-speed bulk packet): at 64 an A4 OS image through the relay ran at 28.5 KiB/s (RF2).
// A record is sent in kTxBytes pieces, one per call.
static const unsigned kTxBytes = 512;

void Send (void)
{
	static uint8_t s_Packets[4 * ((4096 + 2) / 3)];
	static unsigned s_nRec, s_nRecOff;		// the engine's record being sent: its packets' bytes, how far
	CUSBMIDIDevice *p = s_pDevice;
	if (p == 0)
	{
		s_Reader.Drain ();			// no box: the engine's records for it go nowhere (whole)
		uint32_t w;
		while (s_FromComputer.pop (&w)) s_nComputerLost++;	// nor the relay's computer's packets (lost: counted)
		s_bComputerSysEx = FALSE;
		s_nRec = s_nRecOff = 0;
		return;
	}
	unsigned nState = p->GetSendState ();
	if (nState == 1) return;			// the last transfer still in flight
	if (nState == 2)				// it failed: that message is lost (counted), the next goes
	{
		s_Stats.nSendFail++;
		if (s_bLastComputer) s_nComputerLost++;	// the computer's packets in it (usbmidi=os: aborts an upgrade)
	}
	s_bLastComputer = FALSE;
	// the rest of an engine record first: nothing lands inside it
	if (s_nRecOff < s_nRec)
	{
		unsigned n = s_nRec - s_nRecOff < kTxBytes ? s_nRec - s_nRecOff : kTxBytes;
		if (p->SendEventPacketsAsync (s_Packets + s_nRecOff, n))
		{
			s_nRecOff += n;
			if (s_nRecOff == s_nRec) s_Stats.nSent++;
		}
		else
		{
			s_Stats.nSendFail++;
			s_nRec = s_nRecOff = 0;		// the rest dropped: the box sees a cut SysEx, the next record goes
		}
		return;
	}
	// the computer's next (a relay's or the USB-C mirror's, up to 16 packets a transfer); while its SysEx is part way
	// out, the engine's records wait so they never land inside it
	static uint32_t s_Batch[kTxBytes / 4];
	unsigned nBatch = 0;
	uint32_t w;
	while (nBatch < kTxBytes / 4 && s_FromComputer.pop (&w))
	{
		s_Batch[nBatch++] = w;
		unsigned cin = w & 0x0F;
		s_bComputerSysEx = cin == 0x4 || (s_bComputerSysEx && cin != 0x5 && cin != 0x6 && cin != 0x7);
	}
	if (nBatch != 0)
	{
		if (!p->SendEventPacketsAsync ((const uint8_t *) s_Batch, nBatch * 4)) s_Stats.nSendFail++, s_nComputerLost++;
		else s_bLastComputer = TRUE;
		return;
	}
	if (s_bComputerSysEx) return;
	int nLen;
	const uint8_t *pRec = s_Reader.Next (&nLen);
	if (pRec == 0) return;
	int n = usbmidi_pack_sysex (pRec, nLen, s_Packets);
	if (n == 0) n = usbmidi_pack_piece (pRec, nLen, s_Packets);
	if (n == 0) return;				// (the engine sends whole SysEx, or pieces of one)
	s_nRec = (unsigned) n;
	s_nRecOff = 0;
	unsigned k = s_nRec < kTxBytes ? s_nRec : kTxBytes;
	if (p->SendEventPacketsAsync (s_Packets, k))
	{
		s_nRecOff = k;
		if (s_nRecOff == s_nRec) s_Stats.nSent++;
	}
	else
	{
		s_Stats.nSendFail++;
		s_nRec = s_nRecOff = 0;			// lost whole (as before)
	}
}

void ToBox (const uint8_t *pData, int nLength)
{
	if (nLength <= 0) return;
	if (nLength > 4096 || g_Out.size () + 2 + nLength > kOutBytes)
	{
		s_Stats.nOutFull += (unsigned) nLength;
		return;
	}
	g_Out.push ((uint8_t) nLength);
	g_Out.push ((uint8_t) (nLength >> 8));
	for (int i = 0; i < nLength; i++) g_Out.push (pData[i]);
}

void Poll (TRealtime *pRealtime, TAllSoundOff *pAllSoundOff, TByte *pByte)
{
	static boolean s_bUp;
	boolean bUp = s_pDevice != 0;
	if (bUp != s_bUp)
	{
		s_bUp = bUp;
		core::device::set_out (1, bUp ? ToBox : nullptr);
	}
	uint8_t b;
	while (g_In.pop (&b))
	{
		bridge::link_byte (1, b);	// SONG RECORD (core/song): the box's whole stream, its CC / NRPN too, as its slot's (step 10c)
		bridge::rytm_byte (1, b);	// the Rytm's pattern dumps and FILL's answers while it is on this transport (bridge.h)
		if (s_Rt.feed (b))
		{
			s_Stats.nRealtime++;
			if (pRealtime != 0) pRealtime (1, s_Rt.status (), s_Rt.spp ());	// transport 1's: its box's clock
		}
		if (s_Cc.feed (b) && pAllSoundOff != 0) pAllSoundOff (1);
		if (pByte != 0) pByte (b);
		unsigned n = s_Asm.feed (b);
		if (n == 0) continue;
		s_Stats.nSysEx++;
		bridge::sysex_link (1, s_Asm.data (), (int) n);
	}
	s_Stats.nBadSysEx = s_Asm.bad ();
}

}  // namespace link1
