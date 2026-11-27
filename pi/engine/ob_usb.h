//
// ob_usb.h - Circle USB function driver for an Elektron box in OVERBRIDGE mode: the Analog Rytm MKII (VID 0x1935, PID 0x0b48)
// and any box whose device profile has an audio row (core/device, docs/device-profiles.md "Audio"): alt 3, interrupt
// IN 0x83 / OUT 0x03, each direction's block size from the profile (the Rytm 368 / 368, the Digitakt 368 / 88, an A4 256 / 200), the descriptor
// checked against it. The log's prefix stays rytmob for every box.
//
// Run-time options (cmdline.txt on the SD card, one line, space separated; all optional):
//   oburbs=N       URBs in flight per endpoint, 1..16 (default 8; stock Circle allowed 2)
//   obblocks=N     368-byte blocks per URB (one TRB, IOC at its end), 1..8 (default 1)
//   obout=MODE     paced (default): one OUT block per IN block received, as Overwitch,
//                  rytm-emu's host and the Teensy's RytmHost do; free: every OUT URB is
//                  resubmitted on completion (the first hardware run's behaviour); off: no OUT
//   oboutdelay=N   paced: IN blocks to receive before the first OUT block (default 0)
//   oboutlead=N    paced: OUT blocks sent ahead of IN, once, with the first credit and again after an OUT endpoint
//                  reset (default 0, at most oburbs x obblocks): the box then has N blocks queued for it beyond
//                  the one-for-one, so a late IN completion on the Pi no longer leaves it without OUT (T346-T351)
//   obrelaylead=N  paced: the lead instead while the box's mirror goes to its own target (a Teensy relay), not the USB-C
//                  gadget (default 7, at most oburbs x obblocks): through the relay the IN credit trails the box's OUT use
//                  by a few blocks more, and lead 4 ran out before each slip (T369); 7 had none (T371). Follows the
//                  routing at run time: a target that comes while the box streams tops the lead up with the next IN
//   obctr=MODE     zero (default): the OUT frame counter starts at 0; in: at the first IN
//                  block's counter
//   obgate=MS      usb=mirror: a box is shown to the computer only once its IN runs and its OUT keeps up with it for 2 s,
//                  held at most MS (default 12000, then shown anyway with a warning; 0: shown at once, as before).
//                  Overbridge Engine measures a box's latency once, as soon as it streams, and latches a Fault when
//                  the pulse never comes back (attach_gate.h). A held box starved for 3 s is restarted on the Pi (its
//                  audio interfaces to alt 0 and back, its endpoints added again), then re-enumerated
//   obgatereenum=0 the held box is not re-enumerated (default 1: once a hold, only while the computer cannot see it)
//   obgatetest=S   a test: slot S's OUT read as 0/s by the gate until the gate has re-enumerated it (restart and
//                  re-enumeration both exercised); the console's obrestart=S restarts slot S's stream at once
//
#ifndef _ob_usb_h
#define _ob_usb_h

#include "periodic_starve.h"
#include "ep_halt.h"
#include "ob_freeze.h"
#include "attach_gate.h"
#include <circle/usb/xhciendpoint.h>
#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/usb/usbmidi.h>		// SetMidiDevices (circle-midi-usbdevice.patch: GetUSBDevice)
#include <circle/usb/usb.h>
#include <circle/logger.h>
#include <circle/types.h>
#include "fx/out.h"
#include "mirror_target.h"

class CUSBStandardHub;
class CUSBHCIRootPort;

// One CObUsb per box streaming (two at most): the Rytm's (slot 0) and another box's (slot 1, its profile's audio row). The
// primary stream (the Rytm's while it is there, else the other box's) is the one the engine's output stage and the Sidecar
// device follow, as the one stream did; the other streams in step with its own box (its own clock), its OUT silent until the
// engine renders voices of its own for it. Each box's usb=mirror goes to its own target (mirror_target.h).
extern volatile boolean g_bObPowerAsk;		// a box's endpoints failed together: the kernel reads the supply's flags at its next second

class CObUsb : public CUSBFunction
{
public:
	static const unsigned MaxURBs = 16;		// <= XHCI_EP_MAX_URBS (circle-xhci-urbqueue.patch)
	static const unsigned MaxBlocks = 8;

	CObUsb (CUSBFunction *pFunction, const core::out::Device *pAudio, unsigned nSlot);	// nSlot: core::device::usb_attach's
	~CObUsb (void);

	boolean Configure (void);

	// called once a second from the main loop (counters are updated in IRQ context)
	static void LogAllStats (CLogger *pLogger);
	static boolean Streaming (void)		{ return s_pThis != 0 && s_pThis->m_bStreaming; }	// the primary stream
	static const unsigned Slots = 2;
	static CObUsb *Box (unsigned nSlot)	{ return nSlot < Slots ? s_pBox[nSlot] : 0; }
	static boolean Streaming (unsigned nSlot)	{ return Box (nSlot) != 0 && Box (nSlot)->m_bStreaming; }
	unsigned Slot (void) const		{ return m_nSlot; }
	u32 Uid (void) const			{ return m_nUid; }	// its DeviceUID (ObReadUid at enumeration), 0 none
	void SetUid (u32 nUid)			{ m_nUid = nUid; }
	// slot nSlot's stream is in a failing run (ob_freeze.h): no MIDI to its box (core::device::g_frozen, the UID asked by MIDI)
	static boolean Frozen (unsigned nSlot)	{ return Box (nSlot) != 0 && Box (nSlot)->m_Freeze.run; }
	boolean Primary (void) const		{ return this == s_pThis; }
	const core::out::Device *Audio (void) const	{ return m_pAudio; }
	// usb=mirror with two boxes (mirror_assign.h): which box USB-C shows and which a Teensy relay shows is decided at run
	// time, USB-C first (mirrorfirst=relay: the relay first); the box that came first takes the first
	// route when both are there.
	static void SetRelayTarget (IMirrorTarget *pTarget);	// the Teensy relay came (0: went). Task level
	static unsigned GadgetSlot (void);		// the box the USB-C gadget mirrors (0, the Rytm's, while none is chosen)
	static int UsbcSlot (void);			// the same, -1 while none is chosen (/status)
	static const char *SlotName (int nSlot);	// the box in slot nSlot (rytm, a4, digitakt; none)
	static int RelaySlot (void);			// the box the relay shows: the other one, -1 none
	// A box's MIDI by its transport, not its slot (slots by attach order: the Rytm, on the host link, may be slot 1 and an
	// A4, on link1, slot 0). Transport 0 is the host link (the kernel's m_pMIDI: a Rytm or a Digitakt), 1 link1 (link1.h:
	// an A4, or a second host-type box). MidiSlot: the slot whose box that MIDI device is part of (the same USB device as the
	// slot's stream: circle-midi-usbdevice.patch; two boxes of one type have the same vendor, product and serial strings, so
	// those are only the fallback for a MIDI device that names no USB device), -1 none (no such MIDI, or its box has no stream).
	// SetMidiDevices: core 0's loop, each pass (0: none); the slots are worked out there, read anywhere (IRQs too), and told to
	// the core (core::device::set_midi_slot: a hello or a Rytm's 35 on that transport is that slot's box, design 2026-11-26).
	static void SetMidiDevices (CUSBMIDIDevice *pHostLink, CUSBMIDIDevice *pLink1);
	static int MidiSlot (unsigned nTransport)	{ return nTransport < 2 ? s_nMidiSlot[nTransport] : -1; }
	// usb=mirror: USB-C shows the host link's box (or none is chosen: the host link's MIDI goes to USB-C, as before two
	// boxes); USB-C shows link1's box; the relay shows the host link's box. A transport's MIDI goes to the computer of the
	// port that shows its box, the computer's MIDI there to it.
	static boolean UsbcShowsHostLink (void)		{ return UsbcSlot () < 0 || UsbcSlot () == MidiSlot (0); }
	static boolean UsbcShowsLink1 (void)		{ return UsbcSlot () >= 0 && UsbcSlot () == MidiSlot (1); }
	static boolean RelayShowsHostLink (void)	{ return RelaySlot () >= 0 && RelaySlot () == MidiSlot (0); }
	static boolean RelayShowsLink1 (void)		{ return RelaySlot () >= 0 && RelaySlot () == MidiSlot (1); }
	boolean OnTransport (unsigned nTransport) const	{ return (int) m_nSlot == MidiSlot (nTransport); }
	static int MirrorPrefer (void);			// -1 (mirror_assign::kFirst): attach order (mirror= retired 2026-11-21)
	static boolean MirrorRelayFirst (void);		// cmdline mirrorfirst=relay: the relay filled first (testing it), not USB-C
	static void PollAssign (void);			// core 0's loop: a box came or went: who goes where (before PollTargets)
	static const char *PipeInfo (unsigned nSlot);
	static const u8 *BoxConfig (unsigned nSlot, unsigned *pLen);	// the configuration descriptor MirrorAttach read	// the pipes MirrorAttach opened: address, type, wMaxPacketSize, bInterval
	static void SetActive (boolean bActive);	// FALSE: silence out, MIDI in dropped (the TCP link is the host)
	static boolean Active (void)		{ return s_bActive; }
#if RASPPI >= 5
	static void FadeOut (void)		{ s_nFadeLeft = FadeFrames; s_bFading = TRUE; }	// the power-off: OUT to silence in 20 ms, then silent
	static const unsigned FadeFrames = 960;
#endif

	static void WidenIn (const u8 *pIn, u8 *pWide, unsigned nCh);	// an 8-channel IN block as the 12-channel layout
	// main loop, after UpdatePlugAndPlay: a box Configure refused below high speed is enumerated again on its root
	// port when ob_speed.h says so (2, 5, 10 s later; then it stays off and the log says to replug it)
	static void PollReEnum (void);
	// main loop, task level (core 0): an IN or OUT transfer error halts that xHCI endpoint (code 4, a USB transaction
	// error: the box did not answer three tries; 6, a STALL), or it goes quiet (URBs pending, none completed for 50 ms),
	// and the URBs behind it never complete; reset it, drop the URBs the reset skipped and the stream goes on, with a
	// back-off while it keeps failing (ep_halt.h). Also the A4 follower's. At most one endpoint reset a pass.
	static void PollHalt (void);
	// a stream frozen with its box still on the bus (ob_freeze.h: kHoldMs of failing resets / resubmits) is enumerated
	// again, as a replug would (PollHalt's first step); obreenum=<slot> asks for it at once (a test)
	static boolean Command (const char *pText);
	static unsigned FreezeReEnums (void)	{ return s_nFreezeReEnums; }
	static unsigned HubResets (void)	{ return s_Hub.resets; }	// the Pi's hub reset at its root port (ob_freeze.h Hub)
#ifdef OB_USB_TEST
	static void ForgetHub (void)		{ s_Hub = obfreeze::Hub (); }	// a fresh boot's hub state (the unit test)
#endif
	// usb=mirror's attach gate (attach_gate.h): a slot's gate (holds, restarts, re-enumerations since boot), whether its
	// box is held from the computer now, and the stream restarts since boot (the gate's and a starved shown box's)
	static const obgate::Gate &AttachGate (unsigned nSlot)	{ return s_Gate[nSlot < 2 ? nSlot : 1]; }
	static boolean GateHolds (unsigned nSlot)	{ return Box (nSlot) != 0 && Box (nSlot)->m_bShowDue; }
	static unsigned StreamRestarts (void)		{ return s_nStreamRestarts; }
	static boolean ResetDone (void)		{ return s_bResetDone; }	// this pass's reset was sent (the MIDI path waits)
	// since boot (every stream, the Rytm's and a box's): endpoint resets after a halt, IN and OUT, and refused resets
	static unsigned HaltResets (boolean bIn)	{ return s_nHaltResets[bIn ? 0 : 1]; }
	static unsigned HaltRefused (void)		{ return s_nHaltRefused; }
	static unsigned HaltHeld (void)			{ return s_nHaltHeld; }	// resets held: endpoints failed together (EpBox)
	// the transport's USB MIDI OUT, the Rytm's third endpoint in its EpBox: seen halted after a failed send (or not)
	static void MidiHalted (boolean bHalted, unsigned nNowUs);
	static boolean BoxWaits (unsigned nNowUs);	// TRUE: the Rytm's resets wait (EpBox::Wait), the MIDI OUT's too

	static void Stop (void);			// task level: the stream stopped cleanly (alt 0)
	static void EmergencyStop (void);		// exception context: no more resubmits

	// The URBs of a box that left: ~CObUsb (and ~CObFollow) free every one, those still in flight too. Circle deletes a
	// configured function from ~CUSBDevice, which runs after ~CXHCIUSBDevice has disabled and freed the xHC slot, so the
	// controller completes none of them any more. They used to be left allocated; with Circle's 256 CUSBRequest
	// instances (a LogPanic past that) ~17 a Rytm restart rebooted the Pi after about 15 restarts on one boot.
	static unsigned Detaches (void)		{ return s_nDetaches; }
	static unsigned FreedInFlight (void)	{ return s_nFreedInFlight; }	// URBs in flight freed at detaches, all told
	static unsigned KeptInFlight (void)	{ return s_nKeptInFlight; }	// left allocated: a Configure that failed
	static void CountDetach (boolean bGone, unsigned nFreed, unsigned nKept)
	{
		if (bGone) s_nDetaches++;
		s_nFreedInFlight += nFreed;
		s_nKeptInFlight += nKept;
	}
	// Circle's CUSBRequest instances on the heap, every driver's (circle-usb-urb-free.patch): now, the most since boot,
	// and the allocator's limit (XHCI_CONFIG_MAX_REQUESTS on the Pi 4 and 5; past it a LogPanic)
	static unsigned UsbRequests (void)	{ return CUSBRequest::GetHeapInstances (); }
	static unsigned UsbRequestsPeak (void)	{ return CUSBRequest::GetHeapInstancesPeak (); }
	static unsigned UsbRequestsLimit (void);

	// 10 s capture of IN MAIN L/R (channels 0, 1) as the raw int32 samples (the on-wire scale is not yet measured on the Rytm side: MAIN peaks of ~3.9e6 with a pattern playing would be -55 dBFS as int32 full scale, so nothing is cut off),
	// interleaved little-endian, i.e. WAV PCM data. Task level: CaptureAlloc once (1.92 MB on the
	// heap), CaptureStart arms it; the IN completion fills it (no allocation, no logging) and
	// sets CaptureFull. Gaps/lost: IN frame-counter gaps (and blocks lost) while capturing.
	// spike3: 4 channels: IN MAIN L, IN MAIN R, OUT MAIN L as sent (at the IN position of its
	// send time), and channel 3: IN channel `capch` (cmdline) or, by default, note markers:
	// CaptureMarkerMIDI at the IN frame after a note-on arrived, CaptureMarkerTrig at the OUT
	// frame where the voice's trigger takes effect (OR-ed)
	static const unsigned CaptureRate = 48000, CaptureSecs = 10, CaptureChannels = 4;
	static const unsigned CaptureFrames = CaptureRate * CaptureSecs;
	static const unsigned CaptureBytes = CaptureFrames * CaptureChannels * 4;
	static const unsigned CaptureMarkers = 99;
	static const s32 CaptureMarkerMIDI = 0x10000000, CaptureMarkerTrig = 0x20000000;
	static void CaptureMarkMIDI (void);
	static boolean CaptureAlloc (void);
	static boolean CaptureStart (boolean bSelfTest = FALSE);	// FALSE: no buffer, no stream, or busy
	static void CaptureFeed (const s32 *pOutL, unsigned nFrames);	// the self-test's OUT MAIN L (no stream)
	static void CaptureStop (void);			// abandon a capture (stream gone)
	static boolean CaptureActive (void)	{ return s_bCapOn; }
	static boolean CaptureFull (void)	{ return s_bCapFull; }
	static void CaptureClearFull (void)	{ s_bCapFull = FALSE; }
	static unsigned CaptureFramesDone (void) { return s_nCapPos; }
	static unsigned CaptureGaps (void)	{ return s_nCapGaps; }
	static unsigned CaptureLost (void)	{ return s_nCapLost; }
	static const u8 *CaptureData (void)	{ return (const u8 *) s_pCapBuf; }

	// usb=mirror (docs/usb-mirror.md, sidecar_gadget.h): the box's side. At Configure the box's device, configuration and
	// string descriptors go to the gadget, and the other vendor-class endpoints (pipes: the async channel) are opened.
	// FALSE: no box (or busy): the caller drops or STALLs.
	// The statics are the USB-C gadget's: they act on the box it mirrors (GadgetSlot). A target of its own (mirror_target.h)
	// calls the members on its box instead.
	static boolean MirrorControl (const TSetupData *pSetup, const u8 *pOutData, unsigned nSeq);	// core 0's loop
	static boolean MirrorPipeOut (u8 uchAddr, const u8 *pData, unsigned nLength);	// gadget IRQ: one packet to the box
	static void MirrorAttachLate (void);
	static unsigned MirrorParked (void);		// pipe IN packets drained while the Sidecar device was shown (the mirror line)
	static void MirrorWanted (void);		// core 0's loop: the USB-C mode turned to OVERBRIDGE: a box up and not yet read is read (MirrorAttachLate)
	static u16 OutCounter (void);			// the next block's of the gadget's box
	static void MirrorPipeInNext (u8 uchAddr);	// gadget IRQ: the computer has the last packet; the box's next one
	static void MirrorPipePoll (void);		// core 0's loop: a halted pipe is cleared on the box and the host, then resubmitted
	static unsigned MirrorPipeResets (void);	// those clears so far (the mirror line)
	// a forwarded request of the computer's is on the box's EP0 (async, its answer not back): no synchronous EP0 transfer to
	// the box meanwhile (Circle's 3 s timeout would drop the forwarded URB's slot and leave the caller's on the ring)
	static boolean MirrorCtlBusy (void);
	// the same, on this box (a target's own calls)
	boolean Control (const TSetupData *pSetup, const u8 *pOutData, unsigned nSeq);
	boolean PipeOut (u8 uchAddr, const u8 *pData, unsigned nLength);
	void PipeInNext (u8 uchAddr);
	void PipePoll (void);
	void AttachLate (void);
	void Wanted (void);
	void Reattach (void);				// a new mirror target for this box (PollAssign, SetRelayTarget): read again at AttachLate
	void KickPipes (void);				// moved to another target: each IN pipe waiting for the old one's computer asks again
	u16 NextOutCounter (void) const		{ return m_nOutCounter; }
	boolean CtlBusy (void) const		{ return m_bMirror && m_bCtlBusy; }
	static void PollTargets (void);			// core 0's loop: each target's MirrorPoll (the gadget's runs in its own Poll)

	// every xHCI transfer event, from the patched CXHCIEndpoint::TransferEvent (IRQ)
	static void XHCIEvent (unsigned nEndpointID, unsigned nCode, unsigned nResidue, unsigned nPending);

	enum TOutMode { OutPaced, OutFree, OutOff };

	struct TEvStats				// xHCI transfer events of one endpoint
	{
		unsigned nEvents;
		unsigned nSuccess;		// completion code 1
		unsigned nShort;		// 13
		unsigned nRing;			// 14 ring underrun, 15 overrun
		unsigned nMissed;		// 23 missed service
		unsigned nOther;		// anything else (the last one in nLastOther)
		unsigned nLastOther;
		unsigned nOrphan;		// no URB pending when the event came
	};

	static CObUsb *s_pThis;				// the primary stream: s_pBox[0], else s_pBox[1]
	static CObUsb *s_pBox[Slots];
	static volatile int s_nMidiSlot[2];		// SetMidiDevices: each transport's slot, -1 none
	static volatile boolean s_bActive;
#if RASPPI >= 5
	static volatile boolean s_bFading;
	static volatile unsigned s_nFadeLeft;
#endif

private:
	boolean ScanDescriptors (void);			// log everything, locate alt 3 endpoints
	boolean SubmitIn (unsigned i);
	boolean SubmitOut (unsigned i);
	void InDone (unsigned i, CUSBRequest *pURB);
	void OutDone (unsigned i, CUSBRequest *pURB);
	void PumpOut (void);
	void FillOut (u8 *pBuf, unsigned nBlocks);
	void Log (CLogger *pLogger);
	boolean Recover (boolean bIn, unsigned nNowUs);	// PollHalt, one endpoint: TRUE a reset was sent
	boolean BoxWait (unsigned nNowUs);		// EpBox::Wait over IN, OUT and the MIDI OUT: TRUE no reset now
	static void InStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static void OutStub (CUSBRequest *pURB, void *pParam, void *pContext);

	static boolean Live (const void *p)	{ return p != 0 && (p == s_pBox[0] || p == s_pBox[1]); }	// a completion of a box still here
	static void Rebind (void);			// s_pThis from s_pBox
	IMirrorTarget *Target (void);			// this box's mirror target now (the gadget's adapter, a slot's own, or 0)
	unsigned m_nSlot;
	u32 m_nUid = 0;					// the box's DeviceUID (same type twice, design 2026-11-26), 0 none
	u8 **m_pInBuf, **m_pOutBuf;			// this slot's DMA buffers (AllocBufs)

	// usb=mirror
	void MirrorAttach (void);
	boolean SubmitPipeIn (unsigned i);
	static void MirrorCtlStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static void PipeInStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static void PipeOutStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static const unsigned MaxPipes = 6;
	struct TPipe
	{
		u8 uchAddr;
		unsigned nMps;
		CUSBEndpoint *pEP;
		CUSBRequest *pURB;			// IN: its one URB, reused; OUT: a new one each packet, here while in flight
		u8 *pBuf;				// DMA, 512 B (s_PipeBuf: allocated once per slot, reused by every attach)
		volatile boolean bBusy;
		volatile boolean bStopped;		// an error: halted until MirrorPipePoll clears it (IN: then resubmitted)
		volatile unsigned nPackets, nErrors;
		unsigned nTries;			// clears in a row that did not bring it back (slows the retries down)
		unsigned nLastTry;			// CTimer ticks
		boolean bRefused;			// the host refused the last reset: the next one that takes is logged
	} m_Pipe[MaxPipes];
	unsigned m_nPipes;
	boolean m_bMirror;
	volatile boolean m_bAttachDue;			// configured while CSidecarGadget::Get () was 0
	u8 *m_pCtlBuf;					// DMA, 512 B: the forwarded request's data (s_pCtlBuf, allocated once per slot)
	volatile boolean m_bCtlBusy;
	CUSBRequest *volatile m_pCtlURB;		// the forwarded request in flight, and its setup packet
	TSetupData *volatile m_pCtlSetup;
	unsigned m_nGen;				// this box's attach: a completion of an earlier one is ignored
	static unsigned s_nGen, s_nPipeResets;
	boolean m_bUp;					// came up far enough to be mirrored (mirror_assign.h counts it there)
	boolean m_bConfigured;				// Configure returned TRUE: only ~CUSBDevice deletes this (the slot is gone)
	static unsigned s_nDetaches, s_nFreedInFlight, s_nKeptInFlight;
	// usb=mirror's DMA buffers (pipes, forwarded requests): allocated once and reused by every attach (one box at a time,
	// and an old attach's transfers are over before a new one exists: ~CObUsb); they were allocated anew each attach
	static u8 *s_PipeBuf[Slots][MaxPipes], *s_pCtlBuf[Slots];	// per slot: the gadget's box and a relay's box at once

private:
	const core::out::Device *m_pAudio;		// the box's row: channels, what it is called
	const unsigned m_nOutCh, m_nOutBlk;		// host -> device: channels a frame, bytes a block
	const unsigned m_nInCh, m_nInBlk;		// device -> host

	// options
	unsigned m_nURBs, m_nBlocks, m_nOutDelay, m_nOutLead, m_nRelayLead;
	TOutMode m_OutMode;
	boolean m_bCounterFromIn;

	// endpoint descriptors found at interface 1 / 2 alt 3 (point into the device's config descriptor)
	const TUSBEndpointDescriptor *m_pInDesc;
	const TUSBEndpointDescriptor *m_pOutDesc;
	CUSBEndpoint *m_pIn;
	CUSBEndpoint *m_pOut;

	// fixed URB pool (no allocation while streaming), one buffer each
	CUSBRequest *m_pInURB[MaxURBs], *m_pOutURB[MaxURBs];
	volatile boolean m_bInBusy[MaxURBs], m_bOutBusy[MaxURBs];
	volatile unsigned m_nInPending, m_nOutPending;
	volatile boolean m_bStreaming;

	u16 m_nOutCounter;
	boolean m_bOutCounterSet;
	u16 m_nExpectIn;
	boolean m_bHaveIn;
	u32 m_nPhase;
	volatile unsigned m_nCredit;			// OUT blocks owed (paced)
	volatile boolean m_bLeadDue;			// oboutlead: the lead is added with the next credit
	volatile unsigned m_nLeadGiven;			// the lead given since then (topped up when the route's is larger)

	// statistics, written from the xHCI interrupt, read by LogAllStats
	volatile unsigned m_nInBlocks, m_nInShort, m_nInBadMagic, m_nInGaps, m_nInLost, m_nInMaxGap,
			  m_nInOddGap, m_nInErrors, m_nInSubmitFail;
	volatile unsigned m_nOutBlocks, m_nOutErrors, m_nOutSubmitFail, m_nCreditDrops;
	volatile unsigned m_nOutMaxDt, m_nOutPendMin;	// per log line: longest gap between OUT completions (us), fewest OUT URBs left in flight
	EpHalt<CXHCIEndpoint> m_InHalt, m_OutHalt;	// a URB failed: none submitted on it until PollHalt reset it
	obfreeze::Watch m_Freeze;			// the failing run PollFreeze watches (ob_freeze.h)
	static obfreeze::Budget s_FreezeBudget[2];	// per slot: outlives this object, which a re-enumeration deletes
	static int s_nForceReEnum;			// obreenum=<slot>: -1 none
	static unsigned s_nFreezeReEnums;		// since boot
	static boolean PollFreeze (void);
	// TRUE: removed (this object is deleted; after a hub reset the other box's too). FALSE: still here; *pSent FALSE only
	// when the hub did not answer, now or less than obfreeze::kDeadMs ago (a freeze try given back)
	boolean ReEnumerate (const char *pWhy, boolean *pSent = 0);
	static obfreeze::Hub s_Hub;			// the Pi 4's own hub (BuiltInHub): did it answer, its root-port resets
	static boolean HubNoAnswer (CUSBStandardHub *pHub, const char *pName, unsigned nSlot, unsigned nTookUs);
	static void ResetHub (CUSBHCIRootPort *pRoot);	// the hub on that root port reset and enumerated again
	static boolean s_bForceHubReset;		// obhubreset!: at the next pass (a test)
	static int s_nForceNoAnswer;			// obhubnoanswer=<slot>: that slot's next re-enumeration takes the hub as
							// not answering, nothing sent to it (a test of HubNoAnswer); -1 none
	static int s_nForceFs;				// obfsnext=<slot>: that slot's next Configure refuses it as full speed
							// (a test of the full-speed retry, PollReEnum); -1 none
	static unsigned s_nForcedHubResets;		// since boot
	static boolean PollForcedHubReset (void);	// TRUE: done (every box on the hub is gone)
	// the attach gate (attach_gate.h): Log decides once a second (m_nGateAct), PollGate acts in the main loop
	static obgate::Gate s_Gate[2];			// per slot: outlives this object, which a re-enumeration deletes
	static int s_nForceRestart;			// obrestart=<slot>: -1 none
	static int s_nGateTest;				// obgatetest=<slot>: -1 none
	static unsigned s_nStreamRestarts;		// since boot
	boolean m_bShowDue;				// descriptors at the target, held from its computer (MirrorAttach)
	boolean m_bShown;				// shown to a computer (a later target shows it at once)
	unsigned m_nStarveRestarts;			// this stream's restarts for a starvation named while not held
	volatile int m_nGateAct;			// obgate::Act for PollGate
	static boolean PollGate (void);
	static uint32_t GateHoldMs (void);
	void Show (boolean bLate);
	boolean RestartStream (const char *pWhy);	// FALSE: not now (EP0 busy), or the new endpoints were refused
	EpBox m_Box;					// IN, OUT and the MIDI OUT failed together: a likely unplug
	boolean m_bMidiErred;				// the MIDI OUT seen halted, not reset yet (MidiHalted)
	unsigned m_nMidiFailUs;
	static unsigned s_nHaltResets[2], s_nHaltRefused, s_nHaltHeld;
	static boolean s_bResetDone;
	volatile int m_nInPeak;				// largest |sample| seen on MAIN L (channel 0), per second
	volatile unsigned m_nInMaxDt;			// longest time between IN completions (us), per second

	// times (CTimer::GetClockTicks, us)
	unsigned m_nStartUs, m_nLastInUs, m_nLastOutUs, m_nFirstInUs, m_nFirstOutUs, m_nOut16Us, m_nLastLogUs;
	boolean m_bSaidStart;
	unsigned m_nPrevIn, m_nPrevOut, m_nSeconds;
	PeriodicStarve m_Starve;			// OUT short with every URB at the controller (Log, periodic_starve.h)

	static s32 *s_pCapBuf;
	static volatile unsigned s_nCapPos;		// frames written
	static volatile unsigned s_nCapOutPos;		// OUT frames written (channel 2)
	static unsigned s_nCapVoiceCh;
	static volatile boolean s_bCapOn, s_bCapFull;
	static volatile unsigned s_nCapGaps, s_nCapLost;

public:
	static TEvStats s_EvIn, s_EvOut;
};

#include "transport.h"
#include <circle/usb/usbmidi.h>
#include "usbmidi_merge.h"

// The Rytm over USB (OVERBRIDGE mode) as the engine's host. The engine's SysEx for the Rytm (the host-link replies:
// 1D slots, 1A EMPTY, 0E listings, 14 pool, 16/17 names, 15 JUNO sliders) goes out on the Rytm's USB MIDI OUT
// endpoint (Circle's umidi1, cable 0), as the Teensy's host_to_rytm does; nothing else is ever sent there. Without
// umidi1, with tohost=off (cmdline), or after a send that failed or timed out (Circle's 3 s bulk limit; the
// sending stays off until umidi1 comes again), the records are dropped whole and counted.
class CObUsbTransport : public CTransport
{
public:
	const char *Name (void) const override		{ return "usb"; }
	boolean Running (void) const override		{ return CObUsb::Streaming (); }
	void Poll (void) override;
	void Stop (void) override			{ CObUsb::Stop (); }
	void LogStats (CLogger *pLogger) override	{ CObUsb::LogAllStats (pLogger); }
	unsigned MIDIOutDropped (void) const override	{ return m_Merge.dropped (); }

	void SetMIDI (CUSBMIDIDevice *pMIDI);		// core 0's loop, before Poll: umidi1 or 0
	void SetSend (boolean bSend)			{ m_bSend = bSend; }
	lagdiag::TxDiag &Tx (void)			{ return m_Merge.tx (); }	// core 0: the engine's records sent, by type (lagdiag.h)
	unsigned MIDIOutBytes (void) const		{ return m_Merge.sent (); }
	unsigned MIDIOutMessages (void) const		{ return m_Merge.msgs (); }
	unsigned MIDIOutFailed (void) const		{ return m_Merge.failures (); }
	unsigned MacMIDIFailed (void) const		{ return m_Merge.mac_failed (); }	// the computer's, for the Rytm
	// a failed send on a halted bulk OUT (a transfer error: every later send fails too) reset it (circle-midi-reset.patch)
	// and the sending went on at once instead of after the back-off; since boot
	unsigned MIDIOutResets (void) const		{ return m_nMidiResets; }
	unsigned MIDIOutRefused (void) const		{ return m_nMidiRefused; }	// resets the controller refused

private:
	// the order, the pacing and the failure rule: usbmidi_merge.h (portable, Mac-tested); Poll is its Circle side
	UsbMidiMerge m_Merge;
	DropResync m_Resync;
	unsigned m_nMacFailedSeen = 0;			// m_Merge.mac_failed () told to the Sidecar gadget (usbmidi=os:)
	CUSBMIDIDevice *m_pMIDI = 0;
	boolean m_bSend = TRUE;
	unsigned m_nFailSeen = 0;			// the merge's failed sends (engine, keys / computer, bridge) seen
	unsigned m_nMidiResets = 0, m_nMidiRefused = 0, m_nMidiInRow = 0, m_nMsgsSeen = 0;
};

#endif
