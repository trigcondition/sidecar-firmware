//
// ob_follow.h - a second Overbridge stream, read for its audio only: the Analog Four MKII's MAIN goes into the Rytm's PRE pair (host channels 2 / 3), so
// through the Rytm's distortion and compressor, with no audio cable. The Rytm stays the clock: the A4's stream runs on its own crystal, and drift_src.h
// follows it in the Rytm's OUT fill (a4_in.h). Stock firmware on the A4, OVERBRIDGE mode; no mod and no vendor request. Its OUT gets silence, block for block
// with its IN. Everything else is CObUsb's, which does not change: the streams share no state but the resampler's ring.
//
// Off unless asked for (cmdline.txt):
//   a4=1           claim an A4 in OVERBRIDGE mode (default 0: it stays a USB MIDI device left alone, usb_box.h)
//   a4ch=N         the IN pair to use, its left channel (default 0: MAIN)
//   a4att=N        attenuation in dB before the PRE pair (default 6: PRE is 8.9 dB hotter than POST, and it feeds the compressor)
//
#ifndef _ob_follow_h
#define _ob_follow_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/logger.h>
#include <circle/types.h>
#include "a4_in.h"
#include "ep_halt.h"
#include <circle/usb/xhciendpoint.h>

class CObFollow : public CUSBFunction
{
public:
	static const unsigned URBs = 8;

	// the factory hook: a driver for this device, or 0 (not an A4, a4=0, one streams already)
	static CUSBFunction *Create (CUSBFunction *pParent);

	CObFollow (CUSBFunction *pFunction);
	~CObFollow (void);

	boolean Configure (void);

	// The Rytm's OUT fill (xHCI IRQ, core 0): nFrames (at most 7) of the A4 onto the device's PRE pair. One load when a4=0.
	static void Mix (s32 (*pFrames)[12], unsigned nFrames)	{ if (s_bOn) a4in::Mix (pFrames, nFrames); }

	static boolean Streaming (void)	{ return s_pThis != 0 && s_pThis->m_bStreaming; }
	static void LogStats (CLogger *pLogger);	// once a second, core 0's loop: a line every 10 s
	static void Stop (void);			// task level: alt 0, as CObUsb::Stop
	static void EmergencyStop (void);		// exception context: no more resubmits
	// main loop, task level (CObUsb::PollHalt): an IN or OUT endpoint halted by a transfer error reset (ep_halt.h)
	static boolean PollHalt (void);			// TRUE: a reset was sent (the pass's one)
	static unsigned HaltResets (boolean bIn)	{ return s_nHaltResets[bIn ? 0 : 1]; }	// since boot
	static unsigned HaltRefused (void)		{ return s_nHaltRefused; }
	static unsigned HaltHeld (void)			{ return s_nHaltHeld; }	// resets held: IN and OUT failed together

private:
	boolean SubmitIn (unsigned i);
	boolean SubmitOut (unsigned i);
	void InDone (unsigned i, CUSBRequest *pURB);
	void OutDone (unsigned i, CUSBRequest *pURB);
	void PumpOut (void);
	boolean Recover (boolean bIn, unsigned nNowUs);
	static void InStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static void OutStub (CUSBRequest *pURB, void *pParam, void *pContext);

private:
	const TUSBEndpointDescriptor *m_pInDesc, *m_pOutDesc;
	CUSBEndpoint *m_pIn, *m_pOut;
	CUSBRequest *m_pInURB[URBs], *m_pOutURB[URBs];
	volatile boolean m_bInBusy[URBs], m_bOutBusy[URBs];
	volatile unsigned m_nInPending, m_nOutPending;
	volatile boolean m_bStreaming;
	unsigned m_nCredit;
	u16 m_nOutCounter, m_nExpectIn;
	boolean m_bHaveIn;
	volatile unsigned m_nInBlocks, m_nInGaps, m_nInLost, m_nInBad, m_nInErrors, m_nOutErrors, m_nSubmitFail;
	unsigned m_nPrevIn, m_nSecs;
	volatile unsigned m_nLastInUs, m_nLastOutUs;	// the last completion of each (CTimer::GetClockTicks): the stall check
	EpHalt<CXHCIEndpoint> m_InHalt, m_OutHalt;	// a URB failed: none submitted on it until PollHalt reset it
	EpBox m_Box;					// both failed together: a likely unplug (ep_halt.h)
	static unsigned s_nHaltResets[2], s_nHaltRefused, s_nHaltHeld;

	static CObFollow *s_pThis;
	static volatile boolean s_bOn;			// a4=1 and an A4 met this boot; stays on: the ring fades out by itself when the box goes
};

#endif
