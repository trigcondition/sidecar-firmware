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
inline boolean (*g_pSidecarKeysTap) (const u8 *pPacket) = 0;	// (no gadget: never called)

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
	boolean HostOn (void) const			{ return FALSE; }
	boolean Command (const char *)			{ return FALSE; }
	void FromRytmBlock (const u8 *)			{}
	void FromEngine (const s32 (*)[12], unsigned)	{}
	void ToRytm (s32 (*)[12], const s16 (*)[8], unsigned, boolean) {}
	void FromRytmMIDI (const u8 *)			{}
	void MIDIToComputer (u32)			{}
	boolean ForRytm (u32 *)				{ return FALSE; }
	boolean ForRytmInSysEx (void) const		{ return FALSE; }
	boolean EngineWaits (void) const		{ return FALSE; }
	void ComputerSendFailed (void)			{}
	unsigned OsAborts (void) const			{ return 0; }
	void RytmMIDIConnected (boolean)		{}
	boolean OsUpdate (void) const			{ return FALSE; }
	const char *OsUpdateName (void) const		{ return "off"; }
	boolean OsPass (void) const			{ return FALSE; }
	void SetLinkTcp (boolean)			{}
	boolean TakeReboot (void)			{ return FALSE; }
	boolean UsbReboot (void) const			{ return FALSE; }
	boolean Mirror (void) const			{ return FALSE; }
	static boolean MirrorBuilt (void)		{ return FALSE; }
	void SetMirror (boolean)			{}
	boolean MirrorShown (void) const		{ return FALSE; }
	boolean MirrorBoxThere (void) const		{ return FALSE; }
	boolean MirrorSwitching (void) const		{ return FALSE; }
};
#else
#include <circle/usb/gadget/dwusbgadget.h>
#include <circle/usb/gadget/dwusbgadgetendpoint.h>
#include <circle/usb/gadget/dwusbgadgetendpoint0.h>	// circle-gadget-stray: GetSetupAbortCount ()
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/types.h>

#include "midi_filter.h"
#include "reboot_sysex.h"
#include "usbmidi_os.h"
#include "usbmidi_os_path.h"
#include "spsc.h"
#include "tohost_q.h"
#include "mirror_desc.h"
#include "mirror_ctl.h"
#include "mirror_ep.h"

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
	void OnActivate (void) override;		// started by SET_INTERFACE
	void OnDeactivate (void) override		{ Stop (); }
	void OnSuspend (void) override			{ Stop (); }	// a reset after this never calls OnDeactivate
	void OnResume (void) override;			// a resume without a reset: the stream goes on, no SET_INTERFACE comes
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
	void OnSuspend (void) override			{ m_Wake.suspend (*this); }	// mirror_ep.h: back at the resume
	void OnResume (void) override			{ m_Wake.wake (*this); }	// (a resume without a reset)
	boolean Active (void) const			{ return m_bActive; }
	void OnTransferComplete (boolean bIn, size_t nLength) override;
	void Kick (void);				// IN: start a transfer if idle and packets wait (IRQs masked)
	boolean Held (void) const			{ return m_bHeld; }
	void Resume (void);				// OUT, usbmidi=os: re-arm a held endpoint (IRQs masked)
	static const unsigned BufSize = 512, InMax = 508;	// an IN transfer ends short: P1's finding

private:
	CSidecarGadget *m_pSidecar;
	mirror::EpWake m_Wake;				// a bus suspend and a resume without a reset (mirror_ep.h)
	boolean m_bActive, m_bBusy;
	boolean m_bHeld;				// OUT, usbmidi=os: not re-armed while the Rytm-bound queue is full
	DMA_BUFFER (u8, m_Buffer, BufSize);
};

#ifdef CIRCLE_GADGET_MIRROR
// usb=mirror (docs/usb-mirror.md): one of the box's audio or pipe endpoints on the computer's side (mirror_desc.h roles;
// MIDI keeps CSidecarMIDIEP). Interrupt or bulk, its size the largest of any alternate setting.
class CMirrorEP : public CDWUSBGadgetEndpoint
{
public:
	CMirrorEP (const mirror::Ep &rEp, CSidecarGadget *pGadget);
	void OnActivate (void) override;
	void OnDeactivate (void) override;
	void OnSuspend (void) override			{ m_Wake.suspend (*this); }	// mirror_ep.h
	void OnResume (void) override			{ m_Wake.wake (*this); }	// a resume without a reset
	void OnTransferComplete (boolean bIn, size_t nLength) override;
	void Kick (void);				// IN: send the next packet if idle (IRQ level, or IRQs masked)
	void Rearm (void);				// OUT: take the computer's next packet (the box took this one)
	boolean Send (const u8 *pData, unsigned nLength);	// pipe IN: one packet of the box (FALSE: inactive or busy)
	mirror::Role Role (void) const			{ return m_Role; }
	u8 Address (void) const				{ return m_uchAddr; }
	boolean Active (void) const			{ return m_bActive; }
	boolean Busy (void) const			{ return m_Role == mirror::kPipeOut ? m_Out.box : m_bBusy; }
	boolean Armed (void) const			{ return IsTransferActive (); }	// mirror::OutGate
	void ArmOut (void);
	boolean Iso (void) const			{ return GetType () == TypeIsochronous; }	// mirror::cancel ()
	void StopIso (void)				{ StopIsoTransfer (); }
	void Cancel (void)				{ CancelTransfer (); }

private:
	mirror::EpWake m_Wake;
	mirror::OutGate m_Out;				// audio and pipe OUT
	CSidecarGadget *m_pSidecar;
	mirror::Role m_Role;
	u8 m_uchAddr;
	unsigned m_nMps;
	boolean m_bActive, m_bBusy;
	DMA_BUFFER (u8, m_Buffer, 512);
};
#endif

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
	boolean HostOn (void) const			{ return m_ToHost.IsUp (); }	// a computer has configured the device (/status "usbHost")
	boolean Command (const char *pCmd);		// a console command for the Sidecar (TRUE: it was one)

	// the engine's side (core 0)
	void FromRytmBlock (const u8 *pBlock);		// xHCI IRQ: one Overbridge IN block (7 frames x 12 ch)
	void FromEngine (const s32 (*pFrames)[12], unsigned nFrames);	// task: the self-test's output as the stand-in IN
	void ToRytm (s32 (*pFrames)[12], const s16 (*pVoices)[8], unsigned nFrames, boolean bTask);
#ifdef CIRCLE_GADGET_MIRROR
	boolean MirrorOutExtra (u8 *pBlock, u16 nCounter);	// ob_usb FillOut: the computer's per-block bytes into the box's block
#endif
	void FromRytmMIDI (const u8 *pPacket);		// xHCI IRQ: one event packet from the Rytm
	void MIDIToComputer (u32 nPacket);		// core 0's loop: one event packet of the engine's own (the auto-sampler's notes)
	boolean ForRytm (u32 *pPacket);			// core 0's loop: the next packet for the Rytm
	boolean ForRytmInSysEx (void) const		{ return m_ToRytmPath.InSysEx (); }
	// the TCP link: the engine's records wait behind the computer's half-sent SysEx (usbmidi=os: on or emu only)
	boolean EngineWaits (void) const		{ return m_Gate.EngineWaits (m_ToRytmPath.InSysEx ()); }
	void RytmMIDIConnected (boolean bOn);		// core 0's loop: the USB merge or the TCP link can take the computer's MIDI
	void ComputerSendFailed (void);			// core 0's loop: a send of the computer's packets failed (they are lost)
	unsigned OsAborts (void) const			{ return m_Gate.Aborts (); }	// usbmidi=os: upgrades aborted (/status)
	// usbmidi=os:off|emu|on (cmdline, or the usbmidi= command; osupdate=off|emu|rytm the older name, usbmidi_os.h): off;
	// emu: OS upgrades pass only while the TCP link (rytm-emu) is the host; on: also to a real Rytm on USB. Any but off:
	// flow control and the emu relay
	boolean OsUpdate (void) const			{ return m_Gate.On (); }
	const char *OsUpdateName (void) const		{ return usbmidi_os::name (m_Gate.Mode ()); }
	boolean OsPass (void) const			{ return m_Gate.Pass (); }
	void SetLinkTcp (boolean bTcp);			// core 0's loop: the current host (kernel.cpp UseLink, at the switch)
	boolean UsbReboot (void) const			{ return m_bUsbReboot; }
	boolean TakeReboot (void);			// core 0's loop: the computer asked for a reboot (usbreboot=1, reboot_sysex.h)

	// usb=mirror, the USB-C mode OVERBRIDGE (usb_mode.h; docs/usb-mirror.md): the computer sees the attached box itself -
	// its device, configuration and string descriptors, read from it at run time (ob_usb.cpp) - and its audio, async
	// channel (pipes), MIDI and class / vendor requests are relayed. Until the box has been read, and after it has been
	// gone for mirrorhold= seconds (10), the computer sees the Sidecar device; each switch is a replug (SoftDisconnect,
	// 300 ms, SoftConnect). Built only with pi/circle/circle-gadget-mirror.patch and -softconnect.patch
	// (CIRCLE_GADGET_MIRROR); without them the mode is CLASS COMPLIANT whatever is set.
	static boolean MirrorBuilt (void);
	// core 0, task level: the mode set (kernel.cpp, before Start and at run time). At run time OFF while the box is shown
	// is a replug back as the Sidecar device; ON reads a box that is up (CObUsb::MirrorWanted) and MirrorPoll switches
	// to it as at a plug-in. Nothing else changes: the Pi's own stream with the box runs on.
	void SetMirror (boolean bOn);
	boolean Mirror (void) const			{ return m_bMirror; }
	boolean MirrorShown (void) const;		// the computer sees the box now
	boolean MirrorBoxThere (void) const;		// the box has been read and is up (it is shown, or will be)
	boolean MirrorSwitching (void) const;		// off the bus between the two
#ifdef CIRCLE_GADGET_MIRROR
	// the box's side (ob_usb.cpp): task level unless said
	boolean MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg);	// FALSE: not one the gadget can carry
	void MirrorSetString (u8 uchIndex, const u8 *pDesc, unsigned nLength);
	void MirrorBoxUp (boolean bUp);			// read completely / gone
	void MirrorControlDone (unsigned nSeq, const u8 *pData, int nLength);	// xHCI IRQ: the box's answer (< 0: STALL)
	boolean MirrorPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength);	// xHCI IRQ: FALSE: dropped (no computer)
	void MirrorPipeOutDone (u8 uchAddr);		// xHCI IRQ: the box took the computer's packet
#endif

protected:
	const void *GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength) override;
#ifdef CIRCLE_GADGET_MIRROR
	int OnControlRequest (const TSetupData *pSetupData, const u8 *pOutData) override;
	void OnControlAborted (void) override;
#endif
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
	boolean ToRytmRoom (void) const;		// usbmidi=os: room in the Rytm-bound queue for one more OUT transfer
							// (CSidecarMIDIEP::BufSize / 4 packets and what the filter holds)
	unsigned MIDIForHost (u8 *pBuffer, unsigned nMax);
	void ToHost (const u8 *pPacket);		// one packet into the computer's queue (IRQs masked)
	void HostUp (boolean bUp);			// the MIDI IN endpoint (de)activated (gadget IRQ), or torn down (OnSuspend)
	friend class CSidecarIsoEP;
	friend class CSidecarMIDIEP;
#ifdef CIRCLE_GADGET_MIRROR
	friend class CMirrorEP;

	void MirrorAddEndpoints (void);
	boolean MirrorSetInterface (u8 uchInterface, u8 uchAlt);
	void MirrorAudioIn (const u8 *pBlock);		// xHCI IRQ: one IN block of the box (12 channels)
	unsigned MirrorFillIn (u8 *pBuffer);		// gadget IRQ: the next block for the computer (0: none)
	void MirrorAudioOut (const u8 *pBlock, unsigned nLength);	// gadget IRQ: one OUT block of the computer
	void MirrorPipeOut (CMirrorEP *pEP, const u8 *pData, unsigned nLength);	// gadget IRQ
	void MirrorPipeInSent (u8 uchAddr);		// gadget IRQ
	void MirrorPoll (void);				// core 0's loop: the connect, the forwarded request
	boolean IsNameRequest (const TSetupData *pSetup, u8 uchType) const;
	void MirrorLogStats (CLogger *pLogger);
	CMirrorEP *MirrorEP (u8 uchAddr);

	u8 m_MirDev[18];
	u8 m_MirCfg[mirror::kMaxConfig];
	unsigned m_nMirCfg;
	mirror::Layout m_MirLayout;
	struct { u8 uchIndex, nLength; u8 Data[254]; } m_MirStr[mirror::kMaxStrings];
	unsigned m_nMirStr;
	CMirrorEP *m_pMirEP[mirror::kMaxEps];
	u8 m_MirAlt[mirror::kMaxIfaces];
	unsigned m_nMirInCh, m_nMirOutCh;		// channels of the computer's current alternate (0: no stream)
	volatile boolean m_bMirHave, m_bMirBoxUp;
	// which device the computer sees: the box (m_bShowMirror) or the Sidecar; a switch leaves the bus, waits, comes back
	volatile boolean m_bShowMirror;
	boolean m_bSwitching, m_bSwitchTo, m_bMirKeep;
	u64 m_nSwitchAt, m_nBoxGoneAt;
	mirror::ExtraRing m_MirExtra;
	unsigned m_nExtraMargin;			// mirrormargin=N blocks (8)
	u16 m_nExtraOff;				// the Pi's OUT counter minus the computer's, plus the margin
	volatile boolean m_bExtraOff;			// set at the computer's first OUT block of a stream			// the computer's per-block OUT bytes, for the box (MirrorOutExtra)
	char m_MirName[mirror::kNameLen];		// mirrorname=: the suffix on the box's Overbridge name (empty: off)
	unsigned m_nHoldSecs;				// mirrorhold=S: the box gone this long before the Sidecar comes back
	void MirrorSwitch (boolean bToMirror);		// task level: leave the bus
	// the box's IN blocks for the computer (xHCI IRQ -> gadget IRQ, both core 0)
	static const unsigned MirInDepth = 64;		// 8 ms: 8 (1 ms) dropped blocks while a plugin was open (U3 run 4 [hw])
	u8 m_MirIn[MirInDepth][384];
	unsigned m_nMirInHead, m_nMirInTail;
	// the forwarded class / vendor request (gadget IRQ -> core 0's loop -> the box -> xHCI IRQ)
	TSetupData m_CtlSetup;
	u8 m_CtlData[512];
	mirror::Ctl m_Ctl;				// pending, at the box, its sequence number (mirror_ctl.h)
	void MirrorBoxGone (void);			// IRQs masked: the box's half of every exchange ends (pipes, EP0)
	struct TMirStats
	{
		unsigned nInBlocks, nInSent, nInDrop, nOutBlocks, nOutBad, nOutExtra, nOutAnchor, nCtl, nCtlStall, nCtlAbort, nCtlLate;
		unsigned nPipeIn, nPipeInDrop, nPipeOut, nPipeOutDrop, nAltBad, nBoxGoneDrop;
	} m_MirStats;
#endif

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
	OsGate m_Gate;					// usbmidi=os: the setting and the host (usbmidi_os_path.h)
	boolean m_bMirror;				// usb=mirror: the USB-C mode is OVERBRIDGE (SetMirror)
	boolean m_bMirUsed;				// it has been on this boot: MirrorPoll runs (a switch back, the box's pipes)
	boolean m_bUsbReboot;				// usbreboot=1: the computer's reboot SysEx is obeyed
	RebootSysEx m_Reboot;				// the computer's packets (gadget IRQ) -> TakeReboot (core 0)
	void UpdateOsPass (boolean bCut, const char *pWhy);	// the filter's pass from m_Gate; bCut: drop what is on its
								// way to the Rytm first (usbmidi_os_path.h Cut)
	void OsLost (boolean bSendFailed, const char *pWhy);	// usbmidi=os: an upgrade lost data on the way: abort it

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
	ToHostQueue<MIDIQueue> m_ToHost;		// producers: IRQs and the loop (IRQs masked); consumer: the IRQ. Up only while the
							// computer has the MIDI IN endpoint configured (tohost_q.h): none there, nothing queued
	boolean m_bToRytmSysEx;
	ToRytmPath<MIDIQueue> m_ToRytmPath {m_ToRytmFilter, m_ToRytm, m_bToRytmSysEx};	// the three above (usbmidi_os_path.h)

	struct TStats
	{
		unsigned nOutPkts, nOutFrames, nOutHist[3], nOutBad, nOutOver, nOutUnder;
		unsigned nInPkts, nInFrames, nInHist[3], nInUnder, nInOver, nVoiceUnder;
		unsigned nRingMin, nRingMax, nInMin, nInMax;
		unsigned nFbSent, nFb, nMacPeak;		// nMacPeak: the computer's largest |sample| mixed (24-bit)
		unsigned nReq, nBadReq;
		unsigned nMidiFromHost, nMidiToHost, nMidiToRytm, nMidiFull, nMidiNoRytm;
#if RASPPI >= 5
		unsigned nOutCross[ChOut], nOutPeak[ChOut];	// test source: each OUT channel, for the log (no Rytm, no serial needed)
#endif
	} m_Stats;
#if RASPPI >= 5
	s32 m_nOutLast[ChOut];
#endif
	unsigned m_nQuietSecs;				// LogStats: seconds since the last line while no computer is there
	unsigned m_nLastIRQs;
	u64 m_nLastTicks, m_nLastStamp;
	u8 m_StringBuffer[80];
};

// The computer's MIDI for the Rytm passes this tap first (gadget IRQ, one event packet): TRUE when it took the packet
// (kernel.cpp: the notes for the Pi's own keyboard path, core/keys.h), else it goes to the Rytm as before. 0: none.
extern boolean (*g_pSidecarKeysTap) (const u8 *pPacket);

#endif	// SIDECAR_NO_GADGET

#endif
