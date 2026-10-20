// pi/engine/sidecar_gadget.h - P3 of the Overbridge pass-through (docs/superpowers/specs/2026-10-16-overbridge-passthrough-
// spike.md): "Sidecar" on the Pi 4's USB-C port, one composite device of our own descriptor (Circle has no composite
// framework): USB Audio Class 2 (12 in / 12 out, or 20 in with the build option SIDECAR_CH_IN=20) + USB MIDI.
// Needs the isochronous endpoints of pi/circle/circle-gadget.patch.
//
// Channel map (the Teensy T3/T4's, so DAW templates work for both):
//   IN  (to the computer)   1-2 the Rytm's MAIN L/R, 3-10 its tracks BD SD RS BT LT MT CH CY, 11-12 its AUDIO IN L/R,
//                           13-20 (20-in build) the Pi's 8 voices as rendered (voice 0..7)
//   OUT (from the computer) 1-2 the Rytm's MAIN L/R input, 3-4 MAIN FX L/R, 5-12 the tracks' inputs BD..CY: added
//                           (saturating) to what the engine sends on the same Overbridge OUT channels; :mac=off drops it
//
// Clock: the Rytm (its Overbridge IN frames). IN packets: 12 frames, 11/13 when the IN ring is more than 7 frames off
// its 40-frame target. Feedback: the Rytm's frames per microframe over 65536 microframes (8 s; they come 7 at a time),
// plus a correction toward an OUT ring level of 48 frames, 16.16 frames per packet (bInterval 2: macOS follows that).
// Without the Rytm, a stand-in: the self-test's output (generic timer) is fed as the Rytm's IN (FromEngine); with no
// source at all, the generic timer and silence.
//
// MIDI: the Rytm's USB MIDI to the computer and back, through midi_filter.h (the host link and MIDI-CI dropped both
// ways, Elektron OS updates never toward the Rytm). The computer's messages for the Rytm are sent by core 0's loop
// (ob_usb.cpp's CObUsbTransport::Poll, between the engine's own replies, never inside a SysEx). :midifake=on (no Rytm
// needed): what would go to the Rytm comes back as if the Rytm had sent it (the filters both ways tested from the Mac).
//
// Test source (:uac2test=on): IN ch 1..N-2 tones (TONE_PERIOD of teensy/uac2/uac2_test.py, -12 dBFS), IN N-1 the
// loopback of OUT ch 1, IN N the loopback of OUT ch 12, the generic timer the clock - the T2 check, bit-exact.
//
// Everything runs on core 0: the gadget's IRQ, the xHCI IRQ (FromRytmBlock, ToRytm, FromRytmMIDI) and core 0's loop
// (the self-test, Poll); task-level callers mask IRQs around the shared state.
#ifndef _sidecar_gadget_h
#define _sidecar_gadget_h

#ifdef SIDECAR_NO_GADGET
// Pi 5 (Makefile): no gadget in Circle there; a stand-in that is never made (Get () is 0; docs/pi5.md)
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/types.h>
class CSidecarGadget
{
public:
	static const unsigned ChIn = 12, ChOut = 12;
	CSidecarGadget (CInterruptSystem *) {}
	static CSidecarGadget *Get (void)		{ return 0; }
	boolean Start (void)				{ return FALSE; }
	void Poll (void)				{}
	void LogStats (CLogger *)			{}
	boolean MacOn (void) const			{ return FALSE; }
	boolean Command (const char *)			{ return FALSE; }
	void FromRytmBlock (const u8 *)			{}
	void FromEngine (const s32 (*)[12], unsigned)	{}
	void ToRytm (s32 (*)[12], const s16 (*)[8], unsigned, boolean) {}
	void FromRytmMIDI (const u8 *)			{}
	boolean ForRytm (u32 *)				{ return FALSE; }
	boolean ForRytmInSysEx (void) const		{ return FALSE; }
	void RytmMIDIConnected (boolean)		{}
};
#else
#include <circle/usb/gadget/dwusbgadget.h>
#include <circle/usb/gadget/dwusbgadgetendpoint.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/types.h>

#include "midi_filter.h"
#include "spsc.h"

#if __has_include("hosted/sidecar_config.h")
#include "hosted/sidecar_config.h"			// build.sh: SIDECAR_CH_IN=20 pi/engine/build.sh
#endif
#ifndef SIDECAR_CH_IN
#define SIDECAR_CH_IN	12
#endif
#ifndef SIDECAR_NAME
#define SIDECAR_NAME	"Sidecar | Expander (Pi)"	// the product, its audio and MIDI functions (the Teensy: "Sidecar | Expander (Mini)";
#endif						// both of manufacturer "Sidecar")

class CSidecarGadget;

class CSidecarIsoEP : public CDWUSBGadgetEndpoint
{
public:
	enum TRole { RoleOut, RoleFeedback, RoleIn };
	CSidecarIsoEP (const TUSBEndpointDescriptor *pDesc, CSidecarGadget *pGadget, TRole Role);
	void OnActivate (void) override {}		// started by SET_INTERFACE
	void OnDeactivate (void) override		{ Stop (); }
	void OnTransferComplete (boolean bIn, size_t nLength) override;
	void Start (void);
	void Stop (void);
	unsigned Incomplete (void) const		{ return GetIsoIncompleteCount (); }
	unsigned Late (void) const			{ return GetIsoLateCount (); }
	u64 LateStamp (void) const			{ return GetIsoLateStamp (); }

private:
	void Arm (void);
	CSidecarGadget *m_pSidecar;
	TRole m_Role;
	boolean m_bRunning;
	DMA_BUFFER (u8, m_Buffer, 1024);
};

class CSidecarMIDIEP : public CDWUSBGadgetEndpoint
{
public:
	CSidecarMIDIEP (const TUSBEndpointDescriptor *pDesc, CSidecarGadget *pGadget);
	void OnActivate (void) override;
	void OnDeactivate (void) override;
	void OnTransferComplete (boolean bIn, size_t nLength) override;
	void Kick (void);				// IN: start a transfer if idle and packets wait (IRQs masked)

private:
	CSidecarGadget *m_pSidecar;
	boolean m_bActive, m_bBusy;
	static const unsigned BufSize = 512, InMax = 508;	// an IN transfer ends short: P1's finding
	DMA_BUFFER (u8, m_Buffer, BufSize);
};

class CSidecarGadget : public CDWUSBGadget
{
public:
	static const unsigned ChIn = SIDECAR_CH_IN, ChOut = 12, Voices = 8, Subslot = 3;
	static const unsigned Nominal = 12;				// frames per 250 us packet
	static const unsigned MaxIn = (Nominal + 1) * ChIn * Subslot;	// 468 / 780
	static const unsigned MaxOut = (Nominal + 1) * ChOut * Subslot;	// 468
	static const unsigned Ring = 512, InTarget = 40, OutTarget = 48;
	static const unsigned MIDIQueue = 2048;			// event packets each way

	CSidecarGadget (CInterruptSystem *pInterrupt);
	static CSidecarGadget *Get (void)		{ return s_pThis; }

	boolean Start (void);
	void Poll (void);				// core 0's loop: plug-and-play, :midifake
	void LogStats (CLogger *pLogger);
	boolean MacOn (void) const			{ return m_bMac; }	// :mac= (the web page's status)
	boolean Command (const char *pCmd);		// a console command for the Sidecar (TRUE: it was one)

	// the engine's side (core 0)
	void FromRytmBlock (const u8 *pBlock);		// xHCI IRQ: one Overbridge IN block (7 frames x 12 ch)
	void FromEngine (const s32 (*pFrames)[12], unsigned nFrames);	// task: the self-test's output as the stand-in IN
	void ToRytm (s32 (*pFrames)[12], const s16 (*pVoices)[8], unsigned nFrames, boolean bTask);
	void FromRytmMIDI (const u8 *pPacket);		// xHCI IRQ: one event packet from the Rytm
	boolean ForRytm (u32 *pPacket);			// core 0's loop: the next packet for the Rytm
	boolean ForRytmInSysEx (void) const		{ return m_bToRytmSysEx; }
	void RytmMIDIConnected (boolean bOn)		{ m_bRytmMIDI = bOn; }

protected:
	const void *GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength) override;
	int OnClassOrVendorRequest (const TSetupData *pSetupData, u8 *pData) override;
	boolean OnSetInterface (u8 uchInterface, u8 uchAltSetting) override;
	int OnGetInterface (u8 uchInterface) override;

private:
	void AddEndpoints (void) override;
	void CreateDevice (void) override {}
	void OnSuspend (void) override;

	// the endpoints' work (IRQ level)
	unsigned FillIn (u8 *pBuffer);
	void InSent (void);
	void TakeOut (const u8 *pBuffer, size_t nLength);
	unsigned Feedback (unsigned nFrame);
	void MIDIFromHost (const u8 *pBuffer, size_t nLength);
	unsigned MIDIForHost (u8 *pBuffer, unsigned nMax);
	void ToHost (const u8 *pPacket);		// one packet into the computer's queue (IRQs masked)
	friend class CSidecarIsoEP;
	friend class CSidecarMIDIEP;

	u64 TimerQ16 (void);				// generic timer, device frames 16.16
	u64 Now (void);
	void InPush (const s32 *pFrame);		// one 12-ch frame of the Rytm (24-bit)
	void OutReset (void);
	const void *String (const char *pString, size_t *pLength);

	static CSidecarGadget *s_pThis;

	enum { EPOut = 1, EPFeedback = 2, EPIn = 3, EPMIDIOut = 4, EPMIDIIn = 5, NumEPs };
	CSidecarIsoEP *m_pIso[4];
	CSidecarMIDIEP *m_pMIDIOut, *m_pMIDIIn;

	u64 m_nCntFrq, m_nT0;
	u8 m_uchOutAlt, m_uchInAlt;
	boolean m_bMac, m_bTest, m_bFake, m_bRytmMIDI;

	// IN (to the computer): the Rytm's 12 channels, and the voices (20-in build)
	s32 m_InRing[Ring][12];
	unsigned m_nInHead, m_nInTail;
	s16 m_VoiceRing[Ring][8];
	unsigned m_nVoiceHead, m_nVoiceTail;
	u64 m_nSrcFrames;				// the source's frames (the Rytm's, or the stand-in's)
	u64 m_nSrcTick;					// the generic timer at the last ones
	boolean m_bSrcRytm;				// the last source was the Rytm (else the stand-in)
	u64 m_nInPos;					// test source / no source: device frames consumed by IN
	unsigned m_nPhase[20];				// test tones' positions
	s32 *m_pTone[20];
	boolean m_bInPrimed;
	boolean m_bInRunning;				// the computer has read an IN packet of this stream

	// OUT (from the computer)
	s32 m_OutRing[Ring][ChOut];
	unsigned m_nOutHead, m_nOutTail;
	boolean m_bOutPriming;
	s32 m_nLevelQ8;
	u64 m_nSinkPos, m_nLastConsume;
	s32 m_LoopRing[Ring][2];			// test source: OUT ch 1 and 12 for IN N-1, N
	unsigned m_nLoopHead, m_nLoopTail;
	boolean m_bLoopPriming;

	// feedback
	boolean m_bWinStarted, m_bWinRytm;
	unsigned m_nWinUFrames, m_nLastFrameNo;
	u64 m_nWinQ16;
	unsigned m_nFbBase;

	// MIDI
	MidiFilter m_ToRytmFilter, m_ToHostFilter;
	Spsc<u32, MIDIQueue> m_ToRytm;			// producer: the gadget IRQ; consumer: core 0's loop
	u32 m_ToHostQ[MIDIQueue];			// producers: IRQs and the loop (IRQs masked); consumer: the IRQ
	unsigned m_nToHostHead, m_nToHostTail;
	boolean m_bToRytmSysEx;

	struct TStats
	{
		unsigned nOutPkts, nOutFrames, nOutHist[3], nOutBad, nOutOver, nOutUnder;
		unsigned nInPkts, nInFrames, nInHist[3], nInUnder, nInOver, nVoiceUnder;
		unsigned nRingMin, nRingMax, nInMin, nInMax;
		unsigned nFbSent, nFb, nMacPeak;		// nMacPeak: the computer's largest |sample| mixed (24-bit)
		unsigned nReq, nBadReq;
		unsigned nMidiFromHost, nMidiToHost, nMidiToRytm, nMidiFull, nMidiNoRytm;
	} m_Stats;
	unsigned m_nLastIRQs;
	u64 m_nLastTicks, m_nLastStamp;
	u8 m_StringBuffer[80];
};

#endif	// SIDECAR_NO_GADGET

#endif
