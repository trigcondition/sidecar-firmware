// pi/engine/teensy_target.h - usb=mirror through a Teensy 4.1 (docs/teensy-mirror.md): the Pi's end of the link
// (relay_proto.h). The Teensy enumerates on a Pi USB-A port as 1209:0002; this driver claims it and becomes the mirror
// target (mirror_target.h) of the box the USB-C gadget does not mirror (ob_usb.cpp PollAssign, mirror_assign.h), so both boxes
// are shown to computers at once, each on its own cable.
//
// What runs where (all on core 0):
//   xHCI IRQ    the link's completions: events, the computer's audio OUT blocks (mirror_stream.h), packets the
//               computer sent to the box's pipes; the box's IN blocks and pipe packets going out to the Teensy
//   core 0 loop MirrorPoll: the forwarded control requests (CtlData / the box / CtlAnswer), credits, the box's late
//               attach, its pipes' halt recovery, the hold after the box went, a stats line every 10 s
//   task level  Configure, and the descriptor upload when the box is read (MirrorSetDescriptors .. MirrorBoxUp)
#ifndef _teensy_target_h
#define _teensy_target_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/usb/xhciendpoint.h>
#include <circle/logger.h>
#include <circle/types.h>
#include "mirror_target.h"
#include "mirror_stream.h"
#include "relay_proto.h"
#include "midi_filter.h"
#include "usbmidi_os_path.h"
#include "reboot_sysex.h"
#include "slip_detect.h"
#include "ep_halt.h"

class CObUsb;

class CTeensyTarget : public CUSBFunction, public IMirrorTarget
{
public:
	CTeensyTarget (CUSBFunction *pFunction);
	~CTeensyTarget (void);
	boolean Configure (void) override;

	static CTeensyTarget *Get (void)	{ return s_pThis; }
	static boolean TakeReboot (void);	// core 0's loop: the relay's computer asked for a reboot (usbreboot=1, reboot_sysex.h)
	static void LogStats (CLogger *pLogger);
	// teensy_flash.h (docs/teensy-flash.md): requests MirrorPoll carries out on the link's EP0, once started.
	// RequestLog: the Teensy's log since its last read into a 2 KB buffer (LogReads counts the reads done; Log copies it)
	static void RequestBootloader (void);
	static void RequestLog (void);
	static unsigned LogReads (void);
	static unsigned Log (char *pOut, unsigned nCap);

	// IMirrorTarget
	void SetBox (CObUsb *pBox) override;
	boolean Mirror (void) const override;
	boolean MirrorShown (void) const override;
	boolean MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg) override;
	void MirrorSetString (u8 uchIndex, const u8 *pDesc, unsigned nLength) override;
	void MirrorBoxUp (boolean bUp) override;
	void MirrorControlDone (unsigned nSeq, const u8 *pData, int nLength) override;
	boolean MirrorPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength) override;
	void MirrorPipeOutDone (u8 uchAddr) override;
	boolean MirrorOutExtra (u8 *pBlock, u16 nCounter) override;
	void FromBoxBlock (const u8 *pBlock) override;
	void ToBox (s32 (*pFrames)[12], const s16 (*pVoices)[8], unsigned nFrames) override;
	void MirrorPoll (void) override;
	void MirrorMoved (void) override;

	static void MidiTap (const u8 *pEventPacket);	// xHCI IRQ (link1::Tap): the box's USB MIDI, for the computer
	static boolean Relaying (void);			// a relay is here and started (not relaytest=1)
	// the Rytm (slot 0, the host link) on the relay: its USB MIDI both ways, as the gadget does for it on USB-C
	static boolean ShowsRytm (void);		// the relay is started and shows the host link's box (any slot)
	static void RytmTap (const u8 *pEventPacket);	// xHCI IRQ (MIDIPacketHandler): the Rytm's packet, for the computer
	static boolean ForRytm (u32 *pPacket);		// core 0's loop (the USB merge): the computer's next packet for the Rytm
	static boolean ForRytmInSysEx (void);		// a SysEx of the computer's is half sent to the Rytm
	// usbmidi=os: for the Rytm on the relay, as the gadget's on USB-C (core 0's loop): an upgrade that loses data is
	// aborted - the path cut, OS messages dropped until the computer stops sending them - rather than let a hole through
	static void RytmSendFailed (void);		// the USB merge: a send of the computer's packets failed
	static void RytmMidiConnected (boolean bOn);	// the USB merge: whether the Rytm takes MIDI
	static void MidiToComputer (u32 nPacket);	// core 0's loop: the engine's own packet (port 1) to the relay's computer

private:
	static void EvStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static void AudioInStub (CUSBRequest *pURB, void *pParam, void *pContext);	// the computer's OUT blocks (link 0x82)
	static void AudioOutStub (CUSBRequest *pURB, void *pParam, void *pContext);	// an IN block sent to the Teensy (0x02)
	static void ChanInStub (CUSBRequest *pURB, void *pParam, void *pContext);	// a packet the computer sent to the box
	static void ChanOutStub (CUSBRequest *pURB, void *pParam, void *pContext);	// a box packet sent to the Teensy

	void OnEvent (const relay::Event &e);
	int Vendor (boolean bIn, u8 uchRequest, u16 wValue, u16 wIndex, const void *pData, unsigned nLength);	// task / loop
	boolean Upload (void);			// the box's descriptors and strings to the Teensy, then kReqConnect
	void Disconnect (void);
	void ControlPoll (void);
	boolean IsNameRequest (const u8 *pSetup, u8 uchType) const;
	unsigned ChanOfOut (u8 uchAddr) const;	// the link channel a mirrored OUT endpoint rides (kChannels: none)
	unsigned ChanOfIn (u8 uchAddr) const;
	void Resubmit (CUSBRequest *pURB);
	boolean Start (void);			// endpoints, buffers, URBs (MirrorPoll, relaydelay= s after boot)
	void Step (const char *pFormat, ...);	// relaydebug=1: a log line, written to the black box at once
	// relaytest=1 (cmdline; docs/teensy-mirror.md "Pi bench"): no box; a made-up one (pid.codes 1209:0003, the bench's shape)
	// with a synthetic IN stream at the box rate, pipe packets echoed, vendor requests answered with a pattern
	void TestPoll (void);
	void EvWatch (void);			// the event pipe silent after the relay was shown: resubmit, then show it again (T411)
	void EvArm (void);			// (from each kReqConnect 1: the Teensy answers it with kEvHello)
	void TestBlock (u8 *pBlk, u32 nSeq);	// the made-up box's block nSeq in the 12-channel layout
	void SendBoxBlock (const u8 *pBlock);	// FromBoxBlock's work (xHCI IRQ or TestPoll in a critical section)
	boolean TestControl (const u8 *pSetup, const u8 *pData, int *pAnswer);
	void TestOutBlock (const u8 *pBlock, unsigned nLength);
	boolean TestExtras (void);		// the made-up box's BOS + MS OS 2.0 set (WinUSB on the bench computer)
	void FlushMidiIn (void);			// xHCI IRQ: queued box MIDI out to the Teensy when its channel is free
	unsigned MidiInChan (void) const;		// the link channel of the box's MIDI IN endpoint (kChannels: none)

	static CTeensyTarget *s_pThis;
	static volatile boolean s_bBootDue, s_bLogDue, s_bLeaving;
	static u64 s_nLeavingAt;
	static const unsigned kLeaveSecs = 20;	// bootloader request -> the link back if the relay is still here (teensyflash: 15 s)
	static volatile unsigned s_nLogReads;
	static char s_Log[2048];
	static unsigned s_nLog;

	CUSBEndpoint *m_pEv, *m_pAudioIn, *m_pAudioOut;
	CUSBEndpoint *m_pChanIn[relay::kChannels], *m_pChanOut[relay::kChannels];
	static const unsigned EvURBs = 2, AudioInURBs = 4, AudioOutBufs = 16, ChanInURBs = 2;
	u8 *m_pEvBuf[EvURBs];
	CUSBRequest *m_pEvURB[EvURBs], *m_pAudioInURB[AudioInURBs], *m_pChanInURB[relay::kChannels][ChanInURBs];
	CUSBRequest *m_pAudioOutURB[AudioOutBufs];	// kept while its length (the computer's channels) stays
	unsigned m_nAudioOutLen[AudioOutBufs];
	volatile unsigned m_nAtBox[relay::kChannels];	// computer packets at the box, not yet taken (a box that goes: credited back)
	u8 *m_pAudioInBuf[AudioInURBs];
	u8 *m_pAudioOutBuf[AudioOutBufs];
	volatile boolean m_bAudioOutBusy[AudioOutBufs];
	volatile u64 m_nAudioOutAt[AudioOutBufs];
	u64 m_nCompLast;				// the last computer OUT block in (link 0x82), us
	unsigned m_nCompGapMax, m_nCompGap1, m_nCompGap2;	// gaps between them: longest, >= 1 ms, >= 2 ms (T342)
	unsigned m_nUnderSeen, m_nOverSeen;
	unsigned m_EvMs[8], m_nEv;			// uptime ms of the last 8 OUT ring under/over events
	volatile unsigned m_nPipeInZlp;		// box pipe zero-length packets not sent on (Circle cannot take one, T409)
	// the event pipe's safety net (T411: on some boots no event ever came, so the computer's first forwarded request was
	// never answered and it gave up on the relay). The Teensy posts kEvHello ~300 ms after each kReqConnect 1: none
	// within kEvSilentSecs, the event URBs are reset and resubmitted; still none kEvSilentSecs later, the relay is shown
	// again (kReqConnect 0, 1: the computer enumerates it again), at most kReshowMax times for one upload
	static const unsigned kEvSilentSecs = 5, kReshowMax = 3;
	u64 m_nEvShownAt;			// the last kReqConnect 1 (0: not watching)
	unsigned m_nEvAtShow;			// m_Stats.nEvents then
	unsigned m_nEvStep;			// 0: waiting; 1: URBs resubmitted
	unsigned m_nEvReshowTry;		// re-shows since the last Upload
	unsigned m_nEvSilent, m_nEvReshows;	// the relay line's counts: silent pipes seen, re-shows sent
	EpHalt<CXHCIEndpoint> m_EvHalt;
	char m_EvKind[8];
	SlipDetect m_SlipOut, m_SlipIn;		// the loop's test sine on channel 0: the computer's OUT as it arrives, the box's IN (T344)	// submit time (us) of the block on link 0x02
	u8 *m_pChanInBuf[relay::kChannels][ChanInURBs];
	u8 *m_pChanOutBuf[relay::kChannels];
	volatile boolean m_bChanOutBusy[relay::kChannels];
	u8 *m_pCtlBuf;				// the link's EP0 (DMA)
	u16 m_nVersion;

	CObUsb *volatile m_pBox;
	// the box's descriptors as read (MirrorSetDescriptors / MirrorSetString)
	u8 m_Dev[18];
	u8 m_Cfg[mirror::kMaxConfig];
	unsigned m_nCfg;
	struct TStr { u8 uchIndex, nLength; u8 Data[254]; };
	TStr m_Str[mirror::kMaxStrings];
	unsigned m_nStr;
	relay::Map m_Map;
	boolean m_bHave, m_bKeep;		// read; the same box again (its strings kept)
	volatile boolean m_bBoxUp, m_bConnected, m_bPcConfigured, m_bUploadDue;
	u64 m_nBoxGoneAt;
	unsigned m_nHoldSecs;
	char m_Name[24];			// mirrorname= suffix, as the gadget's

	mirror::Stream m_Stream;

	// control: one request at a time, as on the Teensy (relay::CtlSlot there)
	enum TCtlState { CtlIdle, CtlNew, CtlAtBox, CtlAnswered };
	volatile TCtlState m_CtlState;
	u8 m_CtlSeq;
	u8 m_CtlSetup[8];
	u8 m_CtlData[512];
	int m_nCtlAnswer;			// the box's answer length (< 0: STALL)

	volatile unsigned m_nCreditDue[relay::kChannels];
	volatile unsigned m_nMidiHeld[relay::kChannels];	// MIDI credits held back until a whole packet more fits the box's queue
	volatile unsigned m_nMidiOut[relay::kChannels];	// MIDI credits granted whose packets have not come yet (at most 128 events each)
	boolean MidiCreditFits (unsigned c) const;	// one more MIDI credit: every packet it and the granted ones bring fits
	void RebootFeed (const u8 *pEventPacket);	// xHCI IRQ: usbreboot=1's matcher
	boolean m_bUsbReboot;				// usbreboot=1: the reboot SysEx from the relay's computer is obeyed
	MidiFilter m_ToBoxFilter;			// the computer's MIDI to the box: usbmidi=os: as the gadget's (no OS update by default)
	// slot 0 (the Rytm) on the relay: the computer's MIDI waits here for the USB merge (ob_usb.cpp TCircleIo), filtered
	// as the gadget's (host-link SysEx dropped, OS updates only with usbmidi=os:on); the Rytm's MIDI to the computer
	// goes through m_ToPcFilter (host link and MIDI-CI dropped), as the gadget's m_ToHostFilter
	static const unsigned RytmQueue = 2048;
	OwnedToRytmPath<RytmQueue> m_ToRytmPath;
	MidiFilter m_ToPcFilter;
	// usbmidi=os: on the relay, one gate per path (usbmidi_os_path.h OsGate), as the gadget's on USB-C: slot 0 (the
	// Rytm, m_ToRytmPath) and slot 1 (the A4, m_ToBoxFilter into link1's queue). OsLost: data lost while an upgrade is
	// under way -> the path cut and OS messages dropped until the computer has sent none for OsGate::kQuietUs
	OsGate m_Gate;					// slot 0
	OsGate m_BoxGate;				// slot 1
	boolean m_bRytmMidi;				// the Rytm takes MIDI (the USB merge)
	unsigned m_nLink1LostSeen;			// link1::ComputerLost at the last look
	unsigned m_nToBoxFullSeen;			// m_Stats.nMidiToBoxFull at the last look
	unsigned m_nNoBoxSeen;				// m_Stats.nChanInMidi (the computer's MIDI dropped, no box up) at the last look
	void OsLost (boolean bSendFailed, const char *pWhy);		// slot 0
	void BoxOsLost (boolean bSendFailed, const char *pWhy);	// slot 1
	unsigned ApplyBoxGate (boolean bCut);		// slot 1's ApplyGate (IRQs masked by the caller); the packets cut
	void OsGatePoll (void);				// core 0's loop: losses, the box going, the quiet after an abort
	volatile boolean m_bCutRytm;			// the box went: MirrorPoll cuts m_ToRytmPath
	unsigned MidiRoom (void) const;			// packets the box-bound MIDI queue (link1's, or the Rytm's) takes now
	void QueueToPc (const u8 *pEventPacket);	// IRQ level: one packet onto m_MidiIn, flushed to the Teensy
	// T463/T464: 512 packets (2 KB) dropped part of a Data RPC reply when the computer read late; the box is never held
	// back for it (a Rytm made to wait overruns its own transmit buffer), so the ring holds the largest reply whole:
	// 37457 SysEx bytes, 12486 packets
	static const unsigned MidiInRing = 16384;
	u32 m_MidiIn[MidiInRing];			// the box's event packets for the computer
	volatile unsigned m_nMidiInHead, m_nMidiInTail;
	volatile unsigned m_nTakenDue;

	struct TStats {
		unsigned nEvents, nSetups, nAborts, nAnswers, nStalls, nLate, nAudioOutDrop, nChanInDrop, nChanInMidi;
		unsigned nPipeOut, nPipeIn, nPipeInDrop, nCredits, nVendorFail, nConnects;
		unsigned nMidiToBox, nMidiToBoxFull, nMidiToPc, nMidiToPcDrop, nMidiToKeys;
		// link 0x02 (IN blocks to the Teensy, TR2: the link carried ~18 %): each URB's submit -> completion time
		unsigned nLinkErr, nLinkDone, nLinkMaxUs, nLinkHist[8];	// < 125, 250, 500 us, 1, 2, 4, 8 ms, more
		unsigned nDropRun, nDropRunMax, nBusyMax, nLinkErrStatus;	// drops in a row; most URBs busy at a submit
	} m_Stats;
	u64 m_nLastLog;
	boolean m_bTest;
	u64 m_nTestStart;
	u32 m_nTestSent, m_nTestOutNext, m_nTestOutLost, m_nTestOutDup, m_nTestOutBad, m_nTestOut, m_nTestEcho, m_nTestEchoDrop;
	boolean m_bTestOutSeen;
	boolean m_bTestFeeding;
	boolean m_bBoxPipes;			// relayboxpipes=1 (relaytest): the real box's pipes opened as without relaytest (TR2c)
	unsigned m_nInfoLines;
	volatile u8 m_uchTestParked;		// relayboxpipes: a real box pipe IN packet dropped; its next request from MirrorPoll
	u64 m_nTestParkedAt;			// LogStats: the endpoint lines repeated for the first ~2 minutes (the log ring is ~1 min)
	boolean SendPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength);	// MirrorPipeIn's work (relaytest's echo too)
	boolean m_bFeedIrq;			// relayfeed=irq (relaytest): the test blocks go out from the real box's IN
					// completions (xHCI IRQ, its pace), as a box's own blocks do, not from TestPoll's loop
	boolean m_bStarted, m_bLeaveNow, m_bDebug;	// m_bLeaveNow: MirrorMoved, off the bus without mirrorhold
	unsigned m_nDelay;
	unsigned m_nDebugVendor;
	unsigned m_nVariant;			// relayvariant= (TR1g bisect): 2 a link request before the URBs, 3 a standard request after
					// them, 4 link requests as async URBs waited for
	volatile boolean m_bCtlDone;
	volatile int m_nCtlResult;
	static void CtlStub (CUSBRequest *pURB, void *pParam, void *pContext);
	const TUSBEndpointDescriptor *m_pEpDesc[16];
	unsigned m_nEpDesc;			// TestPoll is the caller (a real box of the slot is ignored in test mode)
};

#endif
