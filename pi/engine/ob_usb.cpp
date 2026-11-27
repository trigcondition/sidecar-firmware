//
// ob_usb.cpp - Rytm MKII Overbridge host for Circle (Raspberry Pi 4, xHCI/VL805), and any box whose device profile has an
// audio row: only the block size of OUT and its channel count differ (IN is 12 channels for every box so far).
//
// What it does:
//   1. logs the whole configuration descriptor (interfaces, alternate settings, endpoints);
//   2. SET_INTERFACE alt 3 on interface 1 (interrupt IN 0x83) and interface 2 (interrupt OUT 0x03),
//      the same sequence as RytmHost.cpp (setup_[0], setup_[1]);
//   3. creates the two CUSBEndpoint objects and keeps `oburbs` URBs of `obblocks` 368-byte blocks
//      in flight on IN (a fixed pool, reused: no allocation while streaming);
//   4. OUT: by default paced by IN, one OUT block per IN block received (Overwitch, rytm-emu's
//      simulated host and the Teensy's RytmHost all do this: the Rytm is the clock); the frames come
//      from the engine's output stage (output.h, shared.h);
//   5. counts blocks, frame-counter gaps (+7 per block) and the blocks they lost, bad headers,
//      errors, every xHCI transfer event's completion code, and prints one line a second.
//
// Run-time options (cmdline.txt): see ob_usb.h.
//
// First hardware run (2026-10-14, stock Circle 2 URBs, OUT free-running): IN ~6809 blocks/s of
// 6857, ~14 counter gaps/s; OUT 16 blocks, then both OUT URBs pending forever with no error.
// See docs/pi-circle-spike.md for the analysis.
//
// Circle (build/circle, tag Step51.1.1, USE_XHCI_INTERNAL on RASPPI=4) needs two patches,
// both applied by build.sh:
//   * circle-usbdevicefactory.patch: the factory hook for "ven1935-b48" (RytmObDeviceCreate);
//   * circle-xhci-urbqueue.patch: CXHCIEndpoint keeps up to 16 URBs per endpoint (stock: 2, the
//     second asserted empty), holds its lock from the URB FIFO slot to the doorbell, a barrier
//     before a TRB's cycle bit, GetEPState() and the transfer-event hook used here.
//
// Other notes from Circle's source:
//   * Each URB is ONE Normal TRB with IOC. With obblocks > 1 the TRB spans several packets
//     (one per microframe) and completes after the last (or a short packet).
//   * Endpoint context: MaxBurst 0, MaxESITPayload = wMaxPacketSize (368 fits a HS interrupt
//     endpoint). Interval: bInterval 1 -> 0 -> every microframe.
//   * Interrupt moderation XHCI_CONFIG_IMODI 500 (125 us): completions may be reported up to a
//     microframe late, so a 2-deep queue has under 125 us to be refilled.
//   * A transfer error is logged by xhciendpoint.cpp per event (flooding risk on a bad link).
//   * Buffers must be cache-line aligned DMA memory above MEM_KERNEL_END: heap blocks.
//   * A transfer error halts the endpoint: PollHalt clears it on the box and resets it on the host (ep_halt.h);
//     the endpoint state is printed so a halt shows.
//
#include "ob_usb.h"
#include "simd.h"
#include "usbmidi_pack.h"
#include "sidecar_gadget.h"
#include "teensy_target.h"	// the Rytm on the Teensy relay: its computer's MIDI (TCircleIo)
#include "midibridge_pi.h"
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbhcirootport.h>
#include <circle/usb/usbstandardhub.h>
#include <circle/usb/usb.h>
#include <circle/usb/xhciendpoint.h>
#if RASPPI >= 4
#include <circle/usb/xhciconfig.h>
#endif
#ifndef XHCI_CONFIG_TRANSFER_RING_SIZE
#define XHCI_CONFIG_TRANSFER_RING_SIZE	64		// Circle's (the portable tests' shim has none)
#endif
#include <circle/koptions.h>
#if RASPPI == 4 || defined (OB_USB_TEST)
#include <circle/bcmpciehostbridge.h>	// circle-pcie-config.patch: the VL805 ASPM bits kept off (PollAspm)
#define OB_VL805_ASPM 1
#endif
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/new.h>
#include <assert.h>

#include "ob_block.h"
#include "ob_follow.h"
#include "ob_speed.h"
#include "mirror_assign.h"
#include "sendfx_pi.h"
#include "masterfx_pi.h"		// the master FX chain
#include "spectrum_pi.h"
#include "rec_pi.h"
#include "loop_pi.h"
#include "bridge.h"
#include "device/device.h"
#include "device/a4.h"
#include "device/digitakt.h"
#include "fx/out_measure.h"		// stereo=measure's read-back (MAIN L/R)
#include "samp/preview.h"		// the sample browser's preview (POST)
#include "shared.h"
#include "side_out.h"		// a box beside the Rytm: its own voices (slot 1)

static const char From[] = "rytmob";

static const unsigned kBlk = ob::block_bytes (ob::kChannels);			// 368, the biggest block either way
static const unsigned kBufSize = (CObUsb::MaxBlocks * kBlk + 63) & ~63u;	// 2944

static_assert (CObUsb::MaxURBs <= XHCI_EP_MAX_URBS, "Circle's per-endpoint URB FIFO");

// xHCI endpoint IDs (DCI): 2 * number + IN
static const unsigned kInEpID = (ob::kInEp & 0x0F) * 2 + 1;		// 7
static const unsigned kOutEpID = (ob::kOutEp & 0x0F) * 2;		// 6

// Circle's xHCI wants transfer buffers above MEM_KERNEL_END (asserted in xhciendpoint.cpp),
// so not static arrays in the kernel image: heap blocks, 64 B aligned, allocated once.
static u8 *s_InBuf[CObUsb::Slots][CObUsb::MaxURBs];	// each slot's box its own (two stream at once)
static u8 *s_OutBuf[CObUsb::Slots][CObUsb::MaxURBs];

static u8 *AllocDmaBuf (void)
{
	u8 *p = new u8[kBufSize + 64];
	return (u8 *) (((uintptr) p + 63) & ~(uintptr) 63);
}

static void AllocBufs (unsigned nSlot)
{
	if (s_InBuf[nSlot][0] != 0)
	{
		return;
	}
	for (unsigned i = 0; i < CObUsb::MaxURBs; i++)
	{
		s_InBuf[nSlot][i] = AllocDmaBuf ();
		s_OutBuf[nSlot][i] = AllocDmaBuf ();
	}
}

static unsigned Clamp (unsigned v, unsigned lo, unsigned hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

CObUsb *CObUsb::s_pThis = 0;
volatile boolean g_bObPowerAsk = FALSE;
CObUsb *CObUsb::s_pBox[CObUsb::Slots] = {};
volatile int CObUsb::s_nMidiSlot[2] = {-1, -1};
static IMirrorTarget *s_pRelay;				// the Teensy relay's target, besides the gadget (0: none)
static CObUsb *s_pRelayBox;				// the box it was given (SetBox)

// usb=mirror with two boxes: who goes where (mirror_assign.h). Core 0 (task level and its loop) alone writes these.
static mirror_assign::State s_Assign;
static mirror_assign::Config s_AssignCfg;
static boolean s_bAssignCfg;

static const mirror_assign::Config &AssignCfg (void)
{
	if (!s_bAssignCfg)
	{
		CKernelOptions *pOpt = CKernelOptions::Get ();
		s_AssignCfg.prefer = mirror_assign::kFirst;	// attach order (design decided 2026-11-21): the first box keeps USB-C; mirror= retired
		s_AssignCfg.relay_first = mirror_assign::parse_relay_first (pOpt->GetAppOptionString ("mirrorfirst", "usbc"));
		s_AssignCfg.settle_us = (u64) pOpt->GetAppOptionDecimal ("mirrorsettle", 15) * 1000000;
		s_AssignCfg.hold_us = (u64) pOpt->GetAppOptionDecimal ("mirrorhold", 10) * 1000000;	// the gadget's and the relay's
		s_bAssignCfg = TRUE;
	}
	return s_AssignCfg;
}

static u64 NowUs (void) { return CTimer::GetClockTicks64 (); }
#ifdef OB_USB_TEST
void ObUsbTestRelayFirst (bool bOn) { AssignCfg (); s_AssignCfg.relay_first = bOn; }	// (the shim's cmdline has no strings)
#endif

void CObUsb::Rebind (void)
{
	s_pThis = s_pBox[0] != 0 ? s_pBox[0] : s_pBox[1];
}

// A MIDI device is a slot's box when it is part of the same USB device as the slot's stream (circle-midi-usbdevice.patch:
// CUSBMIDIDevice::GetUSBDevice, the USB MIDI host function's device). Two boxes of one type carry the same vendor, product
// and serial strings (every Elektron box says serial 000000000001), so the strings below are only the fallback for a MIDI
// device that names no USB device: the same three strings, the same box, as before two boxes of one type.
static boolean SameStrings (const CDevice *pA, const CDevice *pB)
{
	static const CDevice::TProperty P[] = {CDevice::PropertyVendor, CDevice::PropertyProduct, CDevice::PropertySerialNumber};
	for (CDevice::TProperty x : P)
	{
		const char *a = pA->GetProperty (x), *b = pB->GetProperty (x);
		if (strcmp (a != 0 ? a : "", b != 0 ? b : "") != 0) return FALSE;
	}
	return TRUE;
}

void CObUsb::SetMidiDevices (CUSBMIDIDevice *pHostLink, CUSBMIDIDevice *pLink1)
{
	CUSBMIDIDevice *const pMidi[2] = {pHostLink, pLink1};
	for (unsigned t = 0; t < 2; t++)
	{
		int nSlot = -1;
		const CUSBDevice *pUsb = pMidi[t] != 0 ? pMidi[t]->GetUSBDevice () : 0;
		for (unsigned k = 0; k < Slots && pMidi[t] != 0; k++)
			if (s_pBox[k] != 0 && (pUsb != 0 ? s_pBox[k]->GetDevice () == pUsb : SameStrings (s_pBox[k], pMidi[t]))) nSlot = (int) k;
		core::device::set_midi_slot ((int) t, nSlot);
		if (nSlot == s_nMidiSlot[t]) continue;
		s_nMidiSlot[t] = nSlot;
		if (nSlot < 0) CLogger::Get ()->Write (From, LogNotice, "midi: %s is no slot's box", t == 0 ? "the host link" : "link1");
		else CLogger::Get ()->Write (From, LogNotice, "midi: %s is the box of slot %d (%s)", t == 0 ? "the host link" : "link1",
					     nSlot, SlotName (nSlot));
	}
}

int CObUsb::UsbcSlot (void)	{ return mirror_assign::usbc_slot (AssignCfg (), s_Assign.first); }
int CObUsb::RelaySlot (void)	{ return mirror_assign::relay_slot (AssignCfg (), s_Assign.first); }
int CObUsb::MirrorPrefer (void)	{ return AssignCfg ().prefer; }
boolean CObUsb::MirrorRelayFirst (void)	{ return AssignCfg ().relay_first; }

unsigned CObUsb::GadgetSlot (void)
{
	return UsbcSlot () == 1 ? 1 : 0;
}

// The boxes USB-C and the relay show under the assignment now (0: none chosen, or that box is not here)
static CObUsb *UsbcBox (void)
{
	int k = CObUsb::UsbcSlot ();
	return k < 0 ? 0 : CObUsb::Box ((unsigned) k);
}
static CObUsb *RelayBox (void)
{
	int k = CObUsb::RelaySlot ();
	return k < 0 ? 0 : CObUsb::Box ((unsigned) k);
}

// The relay told which box it shows, if that changed. bMoved: the old one went to USB-C (it did not leave): the relay goes
// off the computer's bus at once, not after mirrorhold
static void RelayFollow (boolean bMoved)
{
	CObUsb *pBox = RelayBox ();
	if (s_pRelay == 0 || pBox == s_pRelayBox) return;
	if (bMoved && s_pRelayBox != 0) s_pRelay->MirrorMoved ();
	s_pRelayBox = pBox;
	s_pRelay->SetBox (pBox);
	if (pBox != 0) pBox->Reattach ();			// read for the relay at its next MirrorPoll (AttachLate)
}

void CObUsb::SetRelayTarget (IMirrorTarget *pTarget)
{
	s_pRelay = pTarget;
	s_pRelayBox = 0;
	RelayFollow (FALSE);
}

static void GadgetBoxMoved (void);			// (below: the gadget's adapter)

// core 0's loop (and a box's Configure): the boxes there now against the assignment; a change moves a box between the
// gadget and the relay (either way: mirrorfirst=relay moves USB-C's box to the relay). A box with a forwarded request of
// the computer's on its EP0 is not moved until it has its answer.
// Attach order (core/device/device.h): a slot holds whichever box came first; the routes go by attach order too (the box
// that came first keeps the first route, AssignCfg), and the logs name the box, not the slot.
static boolean IsRytm (const CObUsb *p)	{ return p != 0 && p->Audio () == &core::out::kRytmMk2; }
static const char *BoxName (int k)	// mirror_assign::slot_name's words, by the box in slot k
{
	CObUsb *p = k >= 0 && k < (int) CObUsb::Slots ? CObUsb::Box ((unsigned) k) : 0;
	if (p == 0) return "none";
	if (IsRytm (p)) return "rytm";
	return p->Audio () == core::device::kDigitakt.audio ? "digitakt" : "a4";	// (the A4, or a box with no word of its own)
}
const char *CObUsb::SlotName (int nSlot)	{ return BoxName (nSlot); }

// The route line, in mirror mode only (usb=sidecar, the class mode, has no routes to name). The first assignment comes
// before the gadget is up (about 0.6 s on the unit, AO3), so a line not yet printed waits for the gadget's mirror mode.
static boolean s_bRouteUnlogged = FALSE;
static void LogRoutes (const char *pWhy)
{
	if (!s_bRouteUnlogged || CSidecarGadget::Get () == 0 || !CSidecarGadget::Get ()->Mirror ()) return;
	s_bRouteUnlogged = FALSE;
	CObUsb *pUsbc = UsbcBox ();
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: USB-C shows %s, the relay %s (%s, mirrorfirst=%s)%s",
				pUsbc != 0 ? BoxName (CObUsb::UsbcSlot ()) : "nothing",
				RelayBox () != 0 ? BoxName (CObUsb::RelaySlot ()) : "nothing",
				AssignCfg ().prefer == mirror_assign::kFirst ? "attach order" : mirror_assign::prefer_name (AssignCfg ().prefer),
				AssignCfg ().relay_first ? "relay" : "usbc", pWhy);
}

void CObUsb::PollAssign (void)
{
	LogRoutes ("");
	const mirror_assign::Config &c = AssignCfg ();
	for (unsigned k = 0; k < Slots; k++) s_Assign.up[k] = s_pBox[k] != 0 && s_pBox[k]->m_bUp;
	const int nOld = s_Assign.first, nNew = mirror_assign::decide (c, s_Assign, NowUs ());
	if (nNew == nOld) return;
	for (unsigned k = 0; k < Slots; k++)
		if (s_pBox[k] != 0 && s_pBox[k]->m_bCtlBusy) return;
	CObUsb *pOldUsbc = UsbcBox ();
	s_Assign.first = nNew;
	CObUsb *pNewUsbc = UsbcBox ();
	const boolean bToUsbc = pNewUsbc != 0 && pNewUsbc == s_pRelayBox;	// the relay's box moves to USB-C
	const boolean bToRelay = pOldUsbc != 0 && pOldUsbc == RelayBox ();	// USB-C's box moves to the relay
	s_bRouteUnlogged = TRUE;
	LogRoutes (bToUsbc ? ": moved from the relay" : bToRelay ? ": moved from USB-C" : "");
	if (bToRelay && pOldUsbc->m_bMirror) GadgetBoxMoved ();
	RelayFollow (bToUsbc);
	if (bToRelay) pOldUsbc->KickPipes ();		// (RelayFollow had it read for the relay)
	if (pNewUsbc != 0 && pNewUsbc != pOldUsbc)
	{
		if (bToUsbc) pNewUsbc->KickPipes ();
		pNewUsbc->Reattach ();				// read for the gadget at its next MirrorPoll (MirrorAttachLate)
	}
}

// The USB-C gadget as a mirror target (mirror_target.h): the CSidecarGadget calls as they were.
namespace {
class CGadgetTarget : public IMirrorTarget
{
public:
	void SetBox (CObUsb *) override {}
	boolean Mirror (void) const override			{ CSidecarGadget *g = CSidecarGadget::Get (); return g != 0 && g->Mirror (); }
	boolean MirrorShown (void) const override		{ CSidecarGadget *g = CSidecarGadget::Get (); return g != 0 && g->MirrorShown (); }
#ifdef CIRCLE_GADGET_MIRROR
	boolean MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg) override
								{ return CSidecarGadget::Get ()->MirrorSetDescriptors (pDev, pCfg, nCfg); }
	void MirrorSetString (u8 i, const u8 *pDesc, unsigned n) override	{ CSidecarGadget::Get ()->MirrorSetString (i, pDesc, n); }
	void MirrorBoxUp (boolean bUp) override			{ CSidecarGadget::Get ()->MirrorBoxUp (bUp); }
	void MirrorControlDone (unsigned nSeq, const u8 *pData, int n) override	{ CSidecarGadget::Get ()->MirrorControlDone (nSeq, pData, n); }
	boolean MirrorPipeIn (u8 a, const u8 *pData, unsigned n) override	{ return CSidecarGadget::Get ()->MirrorPipeIn (a, pData, n); }
	void MirrorPipeOutDone (u8 a) override			{ CSidecarGadget::Get ()->MirrorPipeOutDone (a); }
	boolean MirrorOutExtra (u8 *pBlock, u16 nCounter) override	{ return CSidecarGadget::Get ()->MirrorOutExtra (pBlock, nCounter); }
#else
	boolean MirrorSetDescriptors (const u8 *, const u8 *, unsigned) override	{ return FALSE; }
	void MirrorSetString (u8, const u8 *, unsigned) override	{}
	void MirrorBoxUp (boolean) override			{}
	void MirrorControlDone (unsigned, const u8 *, int) override	{}
	boolean MirrorPipeIn (u8, const u8 *, unsigned) override	{ return FALSE; }
	void MirrorPipeOutDone (u8) override			{}
	boolean MirrorOutExtra (u8 *, u16) override		{ return FALSE; }
#endif
	void FromBoxBlock (const u8 *pBlock) override		{ CSidecarGadget::Get ()->FromRytmBlock (pBlock); }
	void ToBox (s32 (*pF)[12], const s16 (*pV)[8], unsigned n) override	{ CSidecarGadget::Get ()->ToRytm (pF, pV, n, FALSE); }
};
CGadgetTarget s_Gadget;
}

// PollAssign, mirrorfirst=relay: USB-C's box moved to the relay (it did not leave): the gadget shows it gone (requests
// STALLed), then the Sidecar device after mirrorhold, as after a box that left
static void GadgetBoxMoved (void)
{
	s_Gadget.MirrorBoxUp (FALSE);
}

namespace {

// The box the gadget serves: with usb=mirror on, the box assigned to USB-C (mirror_assign.h; 0 while none is chosen or it
// is away); else the primary stream (the Sidecar device's audio follows it, as it did with one stream)
CObUsb *GadgetBox (void)
{
	CSidecarGadget *g = CSidecarGadget::Get ();
	if (g == 0) return 0;
	if (!g->Mirror ()) return CObUsb::s_pThis;
	return UsbcBox ();
}
}

IMirrorTarget *CObUsb::Target (void)
{
	if (this == GadgetBox ()) return &s_Gadget;
	return s_pRelay != 0 && this == RelayBox () ? s_pRelay : 0;
}

void CObUsb::PollTargets (void)
{
	if (s_pRelay != 0) s_pRelay->MirrorPoll ();
}
unsigned CObUsb::s_nGen = 0, CObUsb::s_nPipeResets = 0;
u8 *CObUsb::s_PipeBuf[CObUsb::Slots][CObUsb::MaxPipes] = {}, *CObUsb::s_pCtlBuf[CObUsb::Slots] = {};	// usb=mirror's: allocated once per slot, as s_InBuf (two boxes can be mirrored at once)
unsigned CObUsb::s_nDetaches = 0, CObUsb::s_nFreedInFlight = 0, CObUsb::s_nKeptInFlight = 0;
static obspeed::Retry s_Retry;				// a box refused below high speed (ob_speed.h)
static CUSBHCIRootPort *s_pRetryPort = 0;		// its root port: Circle's, it lives as long as the controller
static CUSBStandardHub *s_pRetryHub = 0;		// or the Pi 4's VL805 hub (BuiltInHub), which lives as long too
static unsigned s_nRetryHubPort = 0;			// the box's 0-based port on that hub
static const char *s_pRetryName = "?";			// the box (a static table) and its slot, for the hub's no-answer lines
static unsigned s_nRetrySlot = 0;
static u32 NowMs (void) { return (u32) (CTimer::GetClockTicks64 () / (CLOCKHZ / 1000)); }	// wraps at 2^32 ms
volatile boolean CObUsb::s_bActive = TRUE;
#if RASPPI >= 5
volatile boolean CObUsb::s_bFading = FALSE;
volatile unsigned CObUsb::s_nFadeLeft = 0;
#endif
s32 *CObUsb::s_pCapBuf = 0;
volatile unsigned CObUsb::s_nCapPos = 0;
volatile unsigned CObUsb::s_nCapOutPos = 0;
unsigned CObUsb::s_nCapVoiceCh = CObUsb::CaptureMarkers;
volatile boolean CObUsb::s_bCapOn = FALSE, CObUsb::s_bCapFull = FALSE;
volatile unsigned CObUsb::s_nCapGaps = 0, CObUsb::s_nCapLost = 0;

boolean CObUsb::CaptureAlloc (void)
{
	if (s_pCapBuf == 0)
	{
		s_pCapBuf = new s32[CaptureFrames * CaptureChannels];
		if (s_pCapBuf) memset (s_pCapBuf, 0, CaptureBytes);
	}
	// capch=N: IN channel N; default (and anything >= 12): note markers (CaptureMarker*)
	s_nCapVoiceCh = CKernelOptions::Get ()->GetAppOptionDecimal ("capch", CaptureMarkers);
	return s_pCapBuf != 0;
}

boolean CObUsb::CaptureStart (boolean bSelfTest)
{
	CObUsb *p = s_pThis;
	if (s_pCapBuf == 0 || s_bCapOn) return FALSE;
	if (!bSelfTest && (p == 0 || !p->m_bStreaming)) return FALSE;

	for (unsigned i = 0; i < CaptureFrames; i++) s_pCapBuf[i * CaptureChannels + 3] = 0;	// markers

	EnterCritical (IRQ_LEVEL);
	s_nCapPos = 0;
	// OUT starts where IN does: the OUT blocks are filled in the IN completion (paced)
	s_nCapOutPos = 0;
	s_nCapGaps = 0;
	s_nCapLost = 0;
	s_bCapFull = FALSE;
	s_bCapOn = TRUE;		// from the next IN block on
	LeaveCritical ();
	return TRUE;
}

// IRQ (the MIDI completion): a note-on arrived; marks the next IN frame of the capture
void CObUsb::CaptureMarkMIDI (void)
{
	if (s_bCapOn && s_pCapBuf != 0 && s_nCapVoiceCh >= (unsigned) ob::kChannels && s_nCapPos < CaptureFrames)
	{
		s_pCapBuf[s_nCapPos * CaptureChannels + 3] |= CaptureMarkerMIDI;
	}
}

void CObUsb::CaptureStop (void)
{
	s_bCapOn = FALSE;
}

// The Rytm-less self-test (selftest.cpp, core 0 task level; never while the Rytm streams): OUT MAIN L as pulled
// from the output stage into channel 2, the IN channels 0 and 1 silent, channel 3's markers kept.
void CObUsb::CaptureFeed (const s32 *pOutL, unsigned nFrames)
{
	if (!s_bCapOn) return;
	unsigned pos = s_nCapPos;
	for (unsigned f = 0; f < nFrames && pos < CaptureFrames; f++, pos++)
	{
		s32 *pOut = s_pCapBuf + pos * CaptureChannels;
		pOut[0] = pOut[1] = 0;
		pOut[2] = pOutL[f];
	}
	s_nCapPos = s_nCapOutPos = pos;
	if (pos >= CaptureFrames)
	{
		s_bCapOn = FALSE;
		s_bCapFull = TRUE;
	}
}

CObUsb::TEvStats CObUsb::s_EvIn, CObUsb::s_EvOut;

// The hook the patched CUSBDeviceFactory calls for every "ven1935-..." name (weak reference there): a box with an audio row
// and the stream shape this driver serves, else 0 and Circle goes on to the device's interfaces (a Digitakt in USB MIDI mode,
// 1935:000d: Circle's MIDI driver takes it).
// The hub a box hangs off when that hub is the Pi 4's own: the VL805's USB 2 hub (2109:3431) right on a root port,
// part of the board, so a pointer to it stays good for as long as the controller. Any other hub can be unplugged
// while a retry waits, so it gets none (0) - also an external hub with the same generic VIA id, so Pi 4 builds only.
static CUSBStandardHub *BuiltInHub (CUSBDevice *pDevice)
{
#if RASPPI != 4 && !defined (OB_USB_TEST)
	return 0;
#endif
	CUSBStandardHub *pHub = pDevice->GetParentHub ();
	if (pHub == 0) return 0;
	CUSBDevice *pHubDevice = pHub->GetDevice ();
	const TUSBDeviceDescriptor *pDesc = pHubDevice->GetDeviceDescriptor ();
	if (pHubDevice->GetParentRootPort () == 0 || pDesc->idVendor != 0x2109 || pDesc->idProduct != 0x3431) return 0;
	return pHub;
}

// The box's DeviceUID by the vendor request Overbridge sends (IOCTL_OB_PROPERTIES_GET_UID: bmRequestType 0xC0, bRequest 0,
// 8 bytes, a word 1 then the UID, both big-endian as the box stores them; Rytm 1.74 FUN_40003be6/FUN_40004294; the units
// answer 00 00 00 01 then the UID MSB first, step 0 2026-11-26), on EP0 before anything
// else: the same 32-bit value as the MIDI RPC 0x03 (design 2026-11-26, findings). 0: no answer (the core asks by MIDI).
u32 ObReadUid (CUSBFunction *pParent)
{
	static u8 *Buf = 0;			// (DMA: a heap block, not the kernel image - the xHCI asserts so; once, 64 B
	if (Buf == 0) Buf = AllocDmaBuf ();	// aligned; the factory runs on core 0, one device at a time)
	memset (Buf, 0, 64);
	CUSBHostController *pHost = pParent->GetHost ();
	const int n = pHost != 0 ? pHost->ControlMessage (pParent->GetEndpoint0 (), 0xC0, 0, 0, 0, Buf, 8) : -1;
	const u32 nUid = n == 8 ? (u32) Buf[4] << 24 | (u32) Buf[5] << 16 | (u32) Buf[6] << 8 | (u32) Buf[7] : 0;
	(void) n;
	return nUid;
}

CUSBFunction *RytmObDeviceCreate (CUSBFunction *pParent)
{
	CUSBFunction *pFollow = CObFollow::Create (pParent);		// a4=1: an A4's stream beside the Rytm's (ob_follow.h)
	if (pFollow != 0) return pFollow;
	const TUSBDeviceDescriptor *pDesc = pParent->GetDevice ()->GetDeviceDescriptor ();
	// any known box is claimed by its USB ids, with the mirror or without, a stock one too (attach order: its USB is its
	// hello; core/device/device.cpp by_usb no longer reads the flag, the tests' shim still does)
	boolean bMirror = strcmp (CKernelOptions::Get ()->GetAppOptionString ("usb", "sidecar"), "mirror") == 0;
	const core::device::Profile *p = core::device::by_usb (pDesc->idVendor, pDesc->idProduct, bMirror);
	if (p == 0) return 0;
	const core::out::Device *pAudio = p->audio;
	// attach order (core/device/device.h): its slot already, else the lowest free one, reserved for it now; the control core
	// makes it the box's at its next pass (a stock box, a stock Rytm too: its USB is its hello)
	const u32 nUid = ObReadUid (pParent);		// two boxes of one type: which one this is (design 2026-11-26)
	const int nSlot = core::device::usb_attach (p, nUid);
	if (nSlot < 0 || nSlot >= (int) CObUsb::Slots || CObUsb::Box ((unsigned) nSlot) != 0)	// one box a slot: a third keeps its MIDI, Circle goes on to its interfaces
	{
		CLogger::Get ()->Write (From, LogWarning, "%s: no slot free (two boxes stream already), no audio from this one", pAudio->name);
		return 0;
	}
	if ((pAudio->in_channels != ob::kChannels && pAudio->in_channels != 8) || pAudio->channels < 2 || pAudio->channels > ob::kChannels)
	{
		CLogger::Get ()->Write (From, LogError, "%s: %u channels in, %u out: not a stream this driver serves", pAudio->name,
					(unsigned) pAudio->in_channels, (unsigned) pAudio->channels);
		return 0;
	}
	CObUsb *pBox = new CObUsb (pParent, pAudio, (unsigned) nSlot);
	pBox->SetUid (nUid);
	return pBox;
}

void CObUsb::PollReEnum (void)
{
	if (!obspeed::due (s_Retry, NowMs ()) || (s_pRetryPort == 0 && s_pRetryHub == 0)) return;
	CLogger::Get ()->Write (From, LogNotice, "enumerating the box again (try %u of %u)", s_Retry.tries, obspeed::kTries);
	// The refused box is still on its port: Configure failed only this driver, and its other functions (its USB MIDI)
	// keep the device configured there (SR4, T482: "Port 4: Device configured" after the refusal). A hub resets only
	// its empty ports, and a root port with a device on it only rescans that device, so the rescan alone did nothing
	// and the box stayed off until a reboot. So the hot-unplug path first (the port disabled, the device and its
	// functions deleted; nothing there: only the port disabled), then the hot-plug path: port reset, address,
	// configure (this driver again)
	if (s_pRetryHub != 0)
	{
		CUSBStandardHub *pHub = s_pRetryHub;
		const char *pName = s_pRetryName;
		const unsigned nSlot = s_nRetrySlot;
		if (obfreeze::quiet (s_Hub, NowMs ()))		// it did not answer a moment ago: nothing sent to it (as ReEnumerate)
		{
			CLogger::Get ()->Write (From, LogError, "%s (slot %u): the Pi's USB hub did not answer %u s ago: nothing sent "
						"to it, the box stays off; unplug it and plug it in again, or power the Pi off and on",
						pName, nSlot, (unsigned) ((NowMs () - s_Hub.dead_ms) / 1000));
			return;
		}
		if (s_nForceNoAnswer == (int) nSlot)		// obhubnoanswer=<slot> (a test): the port disable not sent, taken
		{						// as Circle's 3 s timeout
			s_nForceNoAnswer = -1;
			CLogger::Get ()->Write (From, LogWarning, "obhubnoanswer: slot %u's port disable not sent, taken as not "
						"answered (a test)", nSlot);
			HubNoAnswer (pHub, pName, nSlot, 3000000);
			return;
		}
		// The disable is a request to the hub (CLEAR_FEATURE PORT_ENABLE): one that waits out its timeout is a hub
		// that does not answer (RL1, ob_freeze.h), handled as a frozen box's is: the hub reset at its root port, which
		// enumerates this box again with the rest; a quick refusal only stops this retry
		const unsigned nStartUs = CTimer::GetClockTicks ();
		if (!pHub->RemoveDeviceAt (s_nRetryHubPort))
		{
			const unsigned nTookUs = CTimer::GetClockTicks () - nStartUs;
			if (obfreeze::no_answer (nTookUs))
			{
				HubNoAnswer (pHub, pName, nSlot, nTookUs);
				return;
			}
			CLogger::Get ()->Write (From, LogWarning, "%s (slot %u): its hub refused to disable the port (%u ms): the box "
						"stays as it is", pName, nSlot, nTookUs / 1000);
			return;
		}
	}
	else
		s_pRetryPort->RemoveDevice ();			// (a root port always disables)
	if (!(s_pRetryPort != 0 ? s_pRetryPort->ReScanDevices () : s_pRetryHub->ReScanDevices ()))
	{
		CLogger::Get ()->Write (From, LogWarning, "the port did not enumerate it (unplugged, or something else "
					"is on it now)");
	}
}

// The hook the patched CXHCIEndpoint::TransferEvent calls for every transfer event (weak there).
// The endpoint ID alone picks 0x83 / 0x03: with a4=1 the A4's events are counted in these two as well.
void RytmXHCITransferEventHook (unsigned nSlotID, unsigned nEndpointID, unsigned nCompletionCode,
				unsigned nResidue, unsigned nPending)
{
	CObUsb::XHCIEvent (nEndpointID, nCompletionCode, nResidue, nPending);
}

void CObUsb::XHCIEvent (unsigned nEndpointID, unsigned nCode, unsigned nResidue, unsigned nPending)
{
	TEvStats *s = nEndpointID == kInEpID ? &s_EvIn : nEndpointID == kOutEpID ? &s_EvOut : 0;
	if (s == 0) return;

	s->nEvents++;
	if (nPending == 0) s->nOrphan++;
	switch (nCode)
	{
	case 1:		s->nSuccess++;	break;
	case 13:	s->nShort++;	break;
	case 14:
	case 15:	s->nRing++;	break;
	case 23:	s->nMissed++;	break;
	default:	s->nOther++; s->nLastOther = nCode;	break;
	}
}

unsigned CObUsb::s_nHaltResets[2], CObUsb::s_nHaltRefused, CObUsb::s_nHaltHeld;

boolean CObUsb::s_bResetDone;

// At most one endpoint is reset a main-loop pass, the Rytm MIDI's included (CObUsbTransport::Poll asks ResetDone): its
// waits are Circle's, 3 s at most each (EP0's CLEAR_FEATURE, Reset Endpoint, Set TR Dequeue), so one pass stays well under
// the 15 s watchdog.
#ifdef OB_VL805_ASPM
// The Pi 4's VL805 turns ASPM (L0s+L1) on in its own PCIe Link Control about 9 s after boot, with no host write, then
// toggles it on a fixed 10 s timetable. While it was on the root complex logged correctable link errors and the
// controller's whole periodic schedule stopped for one MFINDEX wrap, 2.048 s, with every box quiet (VP1, PP1). Written
// back to off at once (every 10 ms), it was set once a boot and never again, and the stalls were gone: PW1 (2026-11-24)
// 2 stalls in 2 boots as found, 0 in 3 boots kept off. obaspmoff=0 leaves it as the VL805 sets it. The status half of
// the dword is written 0: LNKSTA's two write-1-to-clear bits are left alone, the rest are read-only.
static void PollAspm (void)
{
	typedef CBcmPCIeHostBridge B;
	static boolean s_bInit, s_bOn;
	static unsigned s_nLnk, s_nWrites, s_nFailed;
	static u32 s_nLastUs;
	if (!s_bInit)
	{
		s_bInit = TRUE;
		s_bOn = CKernelOptions::Get ()->GetAppOptionDecimal ("obaspmoff", 1) != 0;
		if (!s_bOn || B::ConfigRead (1, 0, 0, 0) == ~0U) { s_bOn = FALSE; return; }
		const unsigned nExp = B::FindCapability (1, 0, 0, 0x10);	// PCI_CAP_ID_EXP
		if (nExp == 0) { s_bOn = FALSE; return; }
		s_nLnk = nExp + 0x10;
		CLogger::Get ()->Write ("rytmob", LogNotice, "VL805 ASPM kept off (obaspmoff=1): Link Control/Status %08x",
					B::ConfigRead (1, 0, 0, s_nLnk));
	}
	if (!s_bOn) return;
	const u32 nNow = CTimer::GetClockTicks ();
	if (nNow - s_nLastUs < 10000) return;
	s_nLastUs = nNow;
	const u32 v = B::ConfigRead (1, 0, 0, s_nLnk);
	if (v == ~0U || (v & 3) == 0) return;
	const boolean bOK = B::ConfigWrite (1, 0, 0, s_nLnk, v & 0xfffc);
	const u32 nAfter = B::ConfigRead (1, 0, 0, s_nLnk);
	++s_nWrites;
	if (!bOK || (nAfter & 3) != 0) ++s_nFailed;
	if (s_nWrites <= 5 || s_nWrites % 100 == 0)
		CLogger::Get ()->Write ("rytmob", s_nFailed != 0 ? LogWarning : LogNotice,
					"VL805 ASPM was on (Link Control/Status %08x): set off, reads %08x%s (%u times, %u did not take)",
					v, nAfter, (nAfter & 3) != 0 ? " - DID NOT TAKE" : "", s_nWrites, s_nFailed);
}
#else
static void PollAspm (void) {}
#endif

void CObUsb::PollHalt (void)
{
	PollAspm ();
	s_bResetDone = FALSE;
	if (PollFreeze ())				// a frozen box enumerated again: its object is gone, the pass is spent
	{
		s_bResetDone = TRUE;
		return;
	}
	if (PollGate ())				// the attach gate showed, restarted or re-enumerated a box (attach_gate.h)
	{
		s_bResetDone = TRUE;
		return;
	}
	for (unsigned k = 0; k < Slots && !s_bResetDone; k++)	// (one reset a pass, every box's together)
	{
		CObUsb *p = s_pBox[k];
		if (p == 0 || !p->m_bStreaming) continue;
		unsigned now = CTimer::GetClockTicks ();
		p->BoxWait (now) || p->Recover (TRUE, now) || p->Recover (FALSE, now);
	}
	if (!s_bResetDone) s_bResetDone = CObFollow::PollHalt ();	// an A4's stream beside it (ob_follow.h)
}

obfreeze::Budget CObUsb::s_FreezeBudget[2];
int CObUsb::s_nForceReEnum = -1;
unsigned CObUsb::s_nFreezeReEnums;

// TRUE: a box was removed and its hub asked to enumerate it again (ob_freeze.h). At most one a pass.
boolean CObUsb::PollFreeze (void)
{
	if (PollForcedHubReset ()) return TRUE;	// obhubreset! (a test)
	const u32 nNowMs = NowMs ();
	for (unsigned k = 0; k < Slots && k < 2; k++)
	{
		CObUsb *p = s_pBox[k];
		const boolean bForce = s_nForceReEnum == (int) k;
		if (bForce) s_nForceReEnum = -1;
		if (p == 0 || !p->m_bStreaming)
		{
			if (bForce) CLogger::Get ()->Write (From, LogWarning, "obreenum: no box streams in slot %u", k);
			continue;
		}
		const unsigned nIn = p->m_InHalt.fails, nOut = p->m_OutHalt.fails;
		if (!bForce && !obfreeze::pass (p->m_Freeze, s_FreezeBudget[k], nIn > nOut ? nIn : nOut, nNowMs))
		{
			if (obfreeze::spent_now (p->m_Freeze, s_FreezeBudget[k], nNowMs))
				CLogger::Get ()->Write (From, LogError, "%s (slot %u): its stream is frozen again after %u re-enumerations: "
							"unplug it at the box and plug it in again", p->m_pAudio->name, k, obfreeze::kTries);
			continue;
		}
		boolean bSent = TRUE;
		if (p->ReEnumerate (bForce ? "asked (obreenum)" : "its stream froze with the box still on the bus", &bSent)) return TRUE;
		if (!bSent && !bForce && s_FreezeBudget[k].tries > 0) s_FreezeBudget[k].tries--;	// given back: nothing happened to the box
	}
	return FALSE;
}

// The software replug (ob_freeze.h): CUSBDevice::RemoveDevice is Circle's hot-unplug path (the hub port disabled, the
// xHCI slot freed, this driver and the box's other functions deleted: its MIDI too), and the parent's ReScanDevices its
// hot-plug path (the port reset, a fresh slot, address and Configure). Main loop only. Nothing of this object is used
// once RemoveDevice is called. On a hub port the removal is a request to the hub (CLEAR_FEATURE PORT_ENABLE), and a
// hub that does not answer it removes nothing (RL1, ob_freeze.h "The hub that does not answer"): no rescan then.
boolean CObUsb::ReEnumerate (const char *pWhy, boolean *pSent)
{
	CUSBDevice *pDevice = GetDevice ();
	CUSBHCIRootPort *pPort = pDevice->GetParentRootPort ();
	CUSBStandardHub *pHub = pPort == 0 ? BuiltInHub (pDevice) : 0;
	const char *pName = m_pAudio->name;		// (a static table: still good after the delete)
	const unsigned nSlot = m_nSlot;
	if (pSent != 0) *pSent = TRUE;			// FALSE only where the hub did not answer (a try given back)
	if (pPort == 0 && pHub == 0)
	{
		CLogger::Get ()->Write (From, LogError, "%s (slot %u): %s, but it hangs off another hub: no re-enumeration; unplug "
					"it at the box and plug it in again", pName, nSlot, pWhy);
		return FALSE;
	}
	if (pHub != 0 && obfreeze::quiet (s_Hub, NowMs ()))
	{
		if (!s_Hub.told)
		{
			s_Hub.told = TRUE;
			CLogger::Get ()->Write (From, LogError, "%s (slot %u): %s, but the Pi's USB hub did not answer %u s ago: nothing "
						"sent to it; unplug the box and plug it in again, or power the Pi off and on", pName,
						nSlot, pWhy, (unsigned) ((NowMs () - s_Hub.dead_ms) / 1000));
		}
		if (s_nForceNoAnswer == (int) nSlot)
		{
			s_nForceNoAnswer = -1;
			CLogger::Get ()->Write (From, LogNotice, "obhubnoanswer: not used: the hub did not answer %u s ago, nothing "
						"is sent to it (slot %u)", (unsigned) ((NowMs () - s_Hub.dead_ms) / 1000), nSlot);
		}
		if (pSent != 0) *pSent = FALSE;
		return FALSE;
	}
	s_nFreezeReEnums++;
	CLogger::Get ()->Write (From, LogWarning, "%s (slot %u): %s: enumerating it again (try %u of %u, %u since boot)", pName,
				nSlot, pWhy, nSlot < 2 ? s_FreezeBudget[nSlot].tries : 0, obfreeze::kTries, s_nFreezeReEnums);
	if (pHub != 0 && s_nForceNoAnswer == (int) nSlot)	// obhubnoanswer=<slot> (a test): nothing is sent to the hub; the
	{							// port disable is taken as Circle's 3 s timeout
		s_nForceNoAnswer = -1;
		CLogger::Get ()->Write (From, LogWarning, "obhubnoanswer: slot %u's port disable not sent, taken as not answered (a test)",
					nSlot);
		if (pSent != 0) *pSent = FALSE;
		return HubNoAnswer (pHub, pName, nSlot, 3000000);
	}
	const unsigned nStartUs = CTimer::GetClockTicks ();
	if (!pDevice->RemoveDevice ())			// deletes this - unless its hub did not disable the port (a root port always does)
	{
		const unsigned nTookUs = CTimer::GetClockTicks () - nStartUs;
		if (pHub != 0 && obfreeze::no_answer (nTookUs))
		{
			if (pSent != 0) *pSent = FALSE;		// (only the hub was asked: the try is given back)
			return HubNoAnswer (pHub, pName, nSlot, nTookUs);
		}
		CLogger::Get ()->Write (From, LogWarning, "%s (slot %u): its hub refused to disable the port (%u ms): nothing "
					"removed, not enumerated again", pName, nSlot, nTookUs / 1000);
		return FALSE;
	}
	if (!(pPort != 0 ? pPort->ReScanDevices () : pHub->ReScanDevices ()))
		CLogger::Get ()->Write (From, LogWarning, "the port did not enumerate it again (unplugged meanwhile?)");
	return TRUE;
}

obfreeze::Hub CObUsb::s_Hub;

namespace teensy_flash { void HubGone (void) __attribute__ ((weak)); }	// a Teensy write's hub pointer (teensy_flash.cpp)

// The Pi's own hub (BuiltInHub) did not answer a port disable (nTookUs: the request's 3 s timeout). Nothing more is sent
// to it: its root port is reset instead (obhubreset=1, the default; ob_freeze.h Hub), which the xHC does by itself and
// Circle takes as an unplug and a replug of the hub: the hub, every device behind it (this box, the other one, the relay)
// deleted, then the hub enumerated again with all of them. TRUE: done (this object is gone); FALSE: not now (nothing
// goes to that hub for obfreeze::kDeadMs).
boolean CObUsb::HubNoAnswer (CUSBStandardHub *pHub, const char *pName, unsigned nSlot, unsigned nTookUs)
{
	CUSBDevice *pHubDevice = pHub->GetDevice ();
	CUSBHCIRootPort *pRoot = pHubDevice != 0 ? pHubDevice->GetParentRootPort () : 0;	// (BuiltInHub: on one)
	const boolean bAllowed = pRoot != 0 && CKernelOptions::Get ()->GetAppOptionDecimal ("obhubreset", 1) != 0;
	if (!obfreeze::dead_now (s_Hub, NowMs (), bAllowed))
	{
		CLogger::Get ()->Write (From, LogError, "%s (slot %u): the Pi's USB hub did not answer (the port disable waited %u ms): "
					"nothing removed, nothing sent to the hub for %u s (%s); unplug the box and plug it in again, "
					"or power the Pi off and on", pName, nSlot, nTookUs / 1000, (unsigned) (obfreeze::kDeadMs / 1000),
					!bAllowed ? "obhubreset=0" : s_Hub.resets >= obfreeze::kHubResets ? "its resets for this boot "
					"spent" : "its last reset was less than a minute ago");
		return FALSE;
	}
	CLogger::Get ()->Write (From, LogError, "%s (slot %u): the Pi's USB hub did not answer (the port disable waited %u ms): "
				"resetting it at its root port (reset %u of %u since boot): every box and the relay on it leave and "
				"are enumerated again", pName, nSlot, nTookUs / 1000, s_Hub.resets, obfreeze::kHubResets);
	ResetHub (pRoot);
	return TRUE;
}

// The hub on pRoot, and every device behind it, deleted (Circle's unplug of the hub: no request goes to it, each device's
// slot disabled by a command to the xHC), then the root port reset by the xHC (PORTSC) and the hub enumerated again with
// its ports. Main loop only; every CObUsb behind the hub is gone afterwards.
// Worst case (not bounded here): a hub that comes back half-alive can add 3 s request timeouts in this same pass on top
// of the port disable's 3 s; past the 15 s watchdog the Pi reboots, which resets the VL805 too.
void CObUsb::ResetHub (CUSBHCIRootPort *pRoot)
{
	s_pRetryPort = 0, s_pRetryHub = 0;		// a full-speed retry's hub goes with it (PollReEnum: nothing due)
	if (teensy_flash::HubGone != 0) teensy_flash::HubGone ();
	const unsigned nStartUs = CTimer::GetClockTicks ();
	pRoot->RemoveDevice ();				// deletes the hub and everything behind it
	if (pRoot->ReScanDevices ())
		CLogger::Get ()->Write (From, LogNotice, "the Pi's USB hub is back after its reset (%u ms)",
					(CTimer::GetClockTicks () - nStartUs) / 1000);
	else
		CLogger::Get ()->Write (From, LogError, "the Pi's USB hub did not come back after its reset: power the Pi off and on");
}

// obhubreset!: the root-port reset above at the next pass, of the Pi's hub that a streaming box hangs off, the hub
// answering or not (a test of the teardown and the re-enumeration; not counted in the hub's reset budget)
boolean CObUsb::s_bForceHubReset;
int CObUsb::s_nForceNoAnswer = -1;
int CObUsb::s_nForceFs = -1;
unsigned CObUsb::s_nForcedHubResets;

boolean CObUsb::PollForcedHubReset (void)
{
	if (!s_bForceHubReset) return FALSE;
	s_bForceHubReset = FALSE;
	for (unsigned k = 0; k < Slots && k < 2; k++)
	{
		CObUsb *p = s_pBox[k];
		if (p == 0) continue;
		CUSBStandardHub *pHub = BuiltInHub (p->GetDevice ());
		CUSBDevice *pHubDevice = pHub != 0 ? pHub->GetDevice () : 0;
		CUSBHCIRootPort *pRoot = pHubDevice != 0 ? pHubDevice->GetParentRootPort () : 0;
		if (pRoot == 0) continue;
		s_nForcedHubResets++;
		CLogger::Get ()->Write (From, LogWarning, "obhubreset!: resetting the Pi's USB hub at its root port (%s in slot %u "
					"on it; %u asked since boot)", p->m_pAudio->name, k, s_nForcedHubResets);
		ResetHub (pRoot);
		return TRUE;
	}
	CLogger::Get ()->Write (From, LogWarning, "obhubreset!: no box on the Pi's own hub");
	return FALSE;
}

// ---- the attach gate (attach_gate.h) ------------------------------------------------------------------------------

obgate::Gate CObUsb::s_Gate[2];
int CObUsb::s_nForceRestart = -1;
int CObUsb::s_nGateTest = -2;			// -2: cmdline not read yet
unsigned CObUsb::s_nStreamRestarts;
static boolean s_bGateTestDone[2];		// obgatetest: the gate re-enumerated that slot once (the faking ends)

uint32_t CObUsb::GateHoldMs (void)
{
	return CKernelOptions::Get ()->GetAppOptionDecimal ("obgate", obgate::kHoldMs);
}

// Main loop: what Log decided for each box (and obrestart=). TRUE: something was done this pass (a show, a restart, or
// a re-enumeration, after which that box's object is gone).
boolean CObUsb::PollGate (void)
{
	for (unsigned k = 0; k < Slots && k < 2; k++)
	{
		CObUsb *p = s_pBox[k];
		const boolean bForce = s_nForceRestart == (int) k;
		if (bForce) s_nForceRestart = -1;
		if (p == 0 || !p->m_bStreaming)
		{
			if (bForce) CLogger::Get ()->Write (From, LogWarning, "obrestart: no box streams in slot %u", k);
			continue;
		}
		if (bForce)
		{
			p->RestartStream ("asked (obrestart)");
			return TRUE;
		}
		const int a = p->m_nGateAct;
		if (a == obgate::Wait) continue;
		switch (a)
		{
		case obgate::Show:
		case obgate::ShowLate:
			p->m_nGateAct = obgate::Wait;
			p->Show (a == obgate::ShowLate);
			return TRUE;
		case obgate::Restart:
			if (!p->RestartStream (p->m_bShowDue ? "held from the computer, its audio not running both ways"
							      : "its OUT starved by the host controller")) continue;	// (EP0 busy: next pass)
			p->m_nGateAct = obgate::Wait;
			return TRUE;
		case obgate::ReEnum:
			p->m_nGateAct = obgate::Wait;
			if (s_nGateTest == (int) k) s_bGateTestDone[k] = TRUE;
			if (p->ReEnumerate ("held from the computer, still starved after a stream restart")) return TRUE;
			s_Gate[k].reenum_due = false;			// (not removed: the hold goes on to its limit)
			continue;
		default:
			p->m_nGateAct = obgate::Wait;
			continue;
		}
	}
	return FALSE;
}

// The held box to its computer (the target's MirrorBoxUp): the Engine's latency measurement starts now
void CObUsb::Show (boolean bLate)
{
	const obgate::Gate &g = s_Gate[m_nSlot < 2 ? m_nSlot : 1];
	m_bShowDue = FALSE;
	IMirrorTarget *pT = Target ();
	if (pT == 0 || !m_bMirror) return;		// (moved off its target meanwhile: the next MirrorAttach holds it again)
	m_bShown = TRUE;
	if (bLate)
		CLogger::Get ()->Write (From, LogWarning, "usb=mirror: %s shown to the computer after %u ms although its audio "
					"does not run both ways (the hold's limit, obgate=%u): Overbridge may fail its latency "
					"measurement (%u restarts, %u re-enumerations since boot)", m_pAudio->name, g.last_wait_ms,
					GateHoldMs (), g.total_restarts, g.total_reenums);
	else
		CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %s shown to the computer after %u ms, its audio running "
					"both ways", m_pAudio->name, g.last_wait_ms);
	pT->MirrorBoxUp (TRUE);
}

// The stream again on the same device (TRUE: done, FALSE: not now, EP0 busy): the controller lets go of every queued transfer (Stop Endpoint, the URBs
// dropped), the box's audio interfaces to alt 0 and back to alt 3 (its stream engine off and on, as Configure), then
// new endpoint objects: each one's Configure Endpoint drops and adds that endpoint on the xHC with a new transfer ring
// (a new place in the controller's periodic schedule, which a resubmit or a Reset Endpoint does not give: LT9, T426).
// Then the URBs go in as at Configure. Main loop only, the box's pipes and MIDI untouched.
boolean CObUsb::RestartStream (const char *pWhy)
{
	if (m_bMirror && m_bCtlBusy) return FALSE;	// a forwarded request of the computer's holds EP0
	s_nStreamRestarts++;
	CLogger::Get ()->Write (From, LogWarning, "%s (slot %u): %s: its audio stream restarted on the Pi (alt 0 and back, "
				"its endpoints added again; %u since boot)", m_pAudio->name, m_nSlot, pWhy, s_nStreamRestarts);
	EnterCritical (IRQ_LEVEL);
	m_bStreaming = FALSE;				// no completion resubmits from here
	LeaveCritical ();
	CXHCIEndpoint *pXIn = m_pIn->GetXHCIEndpoint (), *pXOut = m_pOut->GetXHCIEndpoint ();
	const unsigned nDropIn = pXIn->StopAndDrop (), nDropOut = pXOut->StopAndDrop ();
	EnterCritical (IRQ_LEVEL);
	for (unsigned i = 0; i < m_nURBs; i++) m_bInBusy[i] = m_bOutBusy[i] = FALSE;
	m_nInPending = m_nOutPending = 0;
	LeaveCritical ();

	CUSBHostController *pHost = GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
		if (pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 0, nIface, 0, 0) < 0)
			CLogger::Get ()->Write (From, LogWarning, "restart: SET_INTERFACE %u alt 0 failed", (unsigned) nIface);
	CTimer::Get ()->MsDelay (100);
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
		if (pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, ob::kAlt, nIface, 0, 0) < 0)
			CLogger::Get ()->Write (From, LogError, "restart: SET_INTERFACE %u alt %u failed", (unsigned) nIface,
						(unsigned) ob::kAlt);

	CUSBEndpoint *pNewIn = new CUSBEndpoint (GetDevice (), m_pInDesc);
	CUSBEndpoint *pNewOut = new CUSBEndpoint (GetDevice (), m_pOutDesc);
	const boolean bNew = pNewIn->GetXHCIEndpoint ()->IsValid () && pNewOut->GetXHCIEndpoint ()->IsValid ();
	if (bNew)
	{
		CUSBEndpoint *pOldIn = m_pIn, *pOldOut = m_pOut;
		m_pIn = pNewIn, m_pOut = pNewOut;
		const unsigned nLen = m_nBlocks * m_nInBlk, nOutLen = m_nBlocks * m_nOutBlk;
		for (unsigned i = 0; i < m_nURBs; i++)
		{
			delete m_pInURB[i];
			delete m_pOutURB[i];
			m_pInURB[i] = new CUSBRequest (m_pIn, m_pInBuf[i], nLen);
			m_pInURB[i]->SetCompletionRoutine (InStub, (void *) (uintptr) i, this);
			m_pOutURB[i] = new CUSBRequest (m_pOut, m_pOutBuf[i], nOutLen);
			m_pOutURB[i]->SetCompletionRoutine (OutStub, (void *) (uintptr) i, this);
		}
		delete pOldIn;					// (the device's endpoint table points at the new ones now)
		delete pOldOut;
	}
	else
	{
		// refused (the new endpoint's Configure Endpoint failed: it registered nothing): the old ones stay; their
		// doorbells (the submits below) run them again from where Stop Endpoint left them
		CLogger::Get ()->Write (From, LogError, "restart: the controller refused the new endpoints: the old ones go on");
		delete pNewIn;
		delete pNewOut;
	}

	// a new stream: its counters from 0 (the box's too, after alt 0), the recoveries' state cleared
	m_nOutCounter = 0;
	m_bOutCounterSet = FALSE;
	m_bHaveIn = FALSE;
	m_nExpectIn = 0;
	m_nCredit = 0;
	m_bLeadDue = TRUE;
	m_nLeadGiven = 0;
	m_InHalt.halted = m_InHalt.erred = m_OutHalt.halted = m_OutHalt.erred = false;
	m_InHalt.fails = m_OutHalt.fails = 0;
	m_Freeze = obfreeze::Watch ();
	m_Starve = PeriodicStarve ();
	if (m_bShown && !m_bShowDue)			// shown: its target's counter offset and rings start again (as a box back)
		if (IMirrorTarget *pT = Target ()) pT->MirrorBoxUp (TRUE);

	EnterCritical (IRQ_LEVEL);
	m_nLastInUs = m_nLastOutUs = CTimer::GetClockTicks ();
	m_bStreaming = TRUE;
	boolean bOK = TRUE;
	for (unsigned i = 0; i < m_nURBs && bOK; i++) bOK = SubmitIn (i);
	if (bOK && m_OutMode == OutFree)
		for (unsigned i = 0; i < m_nURBs && bOK; i++)
		{
			FillOut (m_pOutBuf[i], m_nBlocks);
			bOK = SubmitOut (i);
		}
	LeaveCritical ();
	CLogger::Get ()->Write (From, bOK ? LogNotice : LogError, "restart: %s (%u IN and %u OUT transfers dropped at the "
				"controller)%s", bNew ? "new endpoints" : "old endpoints", nDropIn, nDropOut,
				bOK ? "" : ": cannot submit the first URBs");
	return TRUE;
}

// obreenum=<slot>: the re-enumeration above at the next main-loop pass, whatever the stream does (a test of the path)
boolean CObUsb::Command (const char *pText)
{
	if (strncmp (pText, "obrestart", 9) == 0)
	{
		if (pText[9] == '=' && (pText[10] == '0' || pText[10] == '1') && pText[11] == 0)
		{
			s_nForceRestart = pText[10] - '0';
			CLogger::Get ()->Write (From, LogNotice, "obrestart: slot %d at the next pass", s_nForceRestart);
		}
		else
			CLogger::Get ()->Write (From, LogNotice, "obrestart=0|1 (restart that slot's audio stream; %u restarts "
						"since boot)", s_nStreamRestarts);
		return TRUE;
	}
	if (strncmp (pText, "obhubnoanswer=", 14) == 0 && (pText[14] == '0' || pText[14] == '1') && pText[15] == 0)
	{
		s_nForceNoAnswer = pText[14] - '0';
		CLogger::Get ()->Write (From, LogNotice, "obhubnoanswer: slot %d's next re-enumeration takes the hub as not answering",
					s_nForceNoAnswer);
		return TRUE;
	}
	if (strncmp (pText, "obfsnext=", 9) == 0 && (pText[9] == '0' || pText[9] == '1') && pText[10] == 0)
	{
		s_nForceFs = pText[9] - '0';
		CLogger::Get ()->Write (From, LogNotice, "obfsnext: slot %d's next attach is refused as full speed, then "
					"enumerated again; obreenum=%d to replug it now", s_nForceFs, s_nForceFs);
		return TRUE;
	}
	if (strcmp (pText, "obhubreset!") == 0)
	{
		s_bForceHubReset = TRUE;
		CLogger::Get ()->Write (From, LogNotice, "obhubreset!: the Pi's USB hub at the next pass");
		return TRUE;
	}
	if (strncmp (pText, "obreenum", 8) != 0) return FALSE;
	if (pText[8] == '=' && (pText[9] == '0' || pText[9] == '1') && pText[10] == 0)
	{
		s_nForceReEnum = pText[9] - '0';
		CLogger::Get ()->Write (From, LogNotice, "obreenum: slot %d at the next pass", s_nForceReEnum);
	}
	else
	{
		CLogger::Get ()->Write (From, LogNotice, "obreenum=0|1 (re-enumerate that slot's box; frozen-stream re-enumerations "
					"since boot %u, budget slot 0 %u/%u, slot 1 %u/%u; the Pi's hub reset %u/%u%s)", s_nFreezeReEnums,
					s_FreezeBudget[0].tries, obfreeze::kTries, s_FreezeBudget[1].tries, obfreeze::kTries,
					s_Hub.resets, obfreeze::kHubResets, s_Hub.dead ? ", it did not answer" : "");
	}
	return TRUE;
}

// TRUE: none of the Rytm's endpoints is reset now: one failed a moment ago (another may follow), or several failed
// together, a likely unplug, held for the hub's disconnect (EpBox, ep_halt.h). Removed meanwhile, this object is gone
// with its state (Circle's removal runs in the main loop before PollHalt: s_pThis 0), and nothing was sent
boolean CObUsb::BoxWait (unsigned nNowUs)
{
	EpFails f;
	EnterCritical (IRQ_LEVEL);			// the IRQ writes erred and fail_us: both read as one
	boolean bIn = m_InHalt.erred, bOut = m_OutHalt.erred;
	f.Add (bIn, m_InHalt.fail_us);
	f.Add (bOut, m_OutHalt.fail_us);
	LeaveCritical ();
	f.Add (m_bMidiErred, m_nMidiFailUs);
	int w = m_Box.Wait (nNowUs, f);
	if (w == 3)
	{
		s_nHaltHeld++;
		g_bObPowerAsk = TRUE;			// the supply read and logged at the next second (kernel.cpp: a drop's cause, power or not)
		if (m_Box.Log (nNowUs))
			CLogger::Get ()->Write (From, LogWarning, "endpoints failed together (IN %u, OUT %u, MIDI OUT %u): a likely "
						"unplug, their resets held up to %u ms for the hub (held %u; %u lines left out)",
						(unsigned) bIn, (unsigned) bOut, (unsigned) m_bMidiErred, EpBox::kGraceUs / 1000,
						m_Box.held, m_Box.TakeQuiet ());
	}
	return w != 0;
}

void CObUsb::MidiHalted (boolean bHalted, unsigned nNowUs)
{
	const int k = MidiSlot (0);				// the host link's MIDI OUT is its box's (any slot)
	CObUsb *p = k < 0 ? 0 : s_pBox[k];
	if (p == 0) return;
	if (bHalted && !p->m_bMidiErred) p->m_nMidiFailUs = nNowUs;
	p->m_bMidiErred = bHalted;
}

boolean CObUsb::BoxWaits (unsigned nNowUs)
{
	const int k = MidiSlot (0);				// the host link's box, whichever slot it holds
	CObUsb *p = k < 0 ? 0 : s_pBox[k];
	return p != 0 && p->m_bStreaming && p->BoxWait (nNowUs);
}

// TRUE: a reset was sent (the pass's one)
boolean CObUsb::Recover (boolean bIn, unsigned nNowUs)
{
	EpHalt<CXHCIEndpoint> &h = bIn ? m_InHalt : m_OutHalt;
	CXHCIEndpoint *pX = (bIn ? m_pIn : m_pOut)->GetXHCIEndpoint ();
	const char *pDir = bIn ? "IN" : "OUT";
	EnterCritical (IRQ_LEVEL);			// gone quiet with no error: URBs pending, none completed for 50 ms
	if (!h.halted && (bIn ? m_nInPending : m_nOutPending) != 0
	    && EpHalt<CXHCIEndpoint>::Quiet (nNowUs, bIn ? m_nLastInUs : m_nLastOutUs)) h.Stall ();
	LeaveCritical ();
	if (!h.Due (nNowUs)) return FALSE;
	if (m_bMirror && m_bCtlBusy) return FALSE;	// usb=mirror: the computer's request holds EP0 (the clear would queue behind it)
	const u8 uchAddr = bIn ? ob::kInEp : ob::kOutEp;
	int r = h.Reset (pX, nNowUs, [this, uchAddr] {
		GetHost ()->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_ENDPOINT, CLEAR_FEATURE, ENDPOINT_HALT,
					    uchAddr, 0, 0);
	});
	if (r == 0) return FALSE;
	s_bResetDone = r != 2;
	if (r < 0)
	{
		s_nHaltRefused++;
		if (h.Log (nNowUs))
			CLogger::Get ()->Write (From, LogWarning, "%s endpoint halted (state %u, transfer error %u): reset refused "
						"(%u in a row; %u lines left out)", pDir, h.state, pX->GetLastErrorCode (),
						h.fails, h.TakeQuiet ());
		return TRUE;
	}
	if (r == 1) s_nHaltResets[bIn ? 0 : 1]++;
	EnterCritical (IRQ_LEVEL);
	if (h.Finish (pX))				// every TD still on the ring was skipped: its URB never completes
	{
		for (unsigned i = 0; i < m_nURBs; i++) (bIn ? m_bInBusy : m_bOutBusy)[i] = FALSE;
		(bIn ? m_nInPending : m_nOutPending) = 0;
	}
	(bIn ? m_nLastInUs : m_nLastOutUs) = nNowUs;	// (the stall clock starts again)
	if (bIn)
	{
		for (unsigned i = 0; i < m_nURBs; i++) if (!m_bInBusy[i]) SubmitIn (i);
	}
	else if (m_OutMode == OutPaced)
	{
		m_bLeadDue = TRUE;			// the blocks queued for the box were skipped: its lead again with the next IN
		PumpOut ();
	}
	else if (m_OutMode == OutFree)
	{
		for (unsigned i = 0; i < m_nURBs; i++) if (!m_bOutBusy[i]) FillOut (m_pOutBuf[i], m_nBlocks), SubmitOut (i);
	}
	LeaveCritical ();
	if (h.Log (nNowUs))
	{
		if (r == 2)
			CLogger::Get ()->Write (From, LogWarning, "%s endpoint quiet or failed but not halted (state %u, %u stalls): "
						"resubmitted (%u in a row; %u lines left out)", pDir, h.state, h.stalls, h.fails,
						h.TakeQuiet ());
		else
			CLogger::Get ()->Write (From, LogWarning, "%s endpoint halted (state %u, transfer error %u): reset %u, "
						"%u URBs dropped, %s goes on (%u in a row; %u lines left out)", pDir, h.state,
						pX->GetLastErrorCode (), h.resets, h.dropped, pDir, h.fails, h.TakeQuiet ());
	}
	return s_bResetDone;
}

unsigned CObUsb::UsbRequestsLimit (void)
{
#if RASPPI >= 4
	return XHCI_CONFIG_MAX_REQUESTS;
#else
	return 0;				// the DWHCI host: no such limit to report here
#endif
}

void CObUsb::SetActive (boolean bActive)
{
	s_bActive = bActive;
	core::out::set_device (bActive && s_pThis != 0 ? s_pThis->m_pAudio : 0);	// the TCP link's host is 12 channels, the Rytm's
}

CObUsb::CObUsb (CUSBFunction *pFunction, const core::out::Device *pAudio, unsigned nSlot)
:	CUSBFunction (pFunction),
	m_pAudio (pAudio), m_nOutCh (pAudio->channels), m_nOutBlk (ob::block_bytes (pAudio->channels)),
	m_nInCh (pAudio->in_channels), m_nInBlk (ob::block_bytes (pAudio->in_channels)),
	m_pInDesc (0), m_pOutDesc (0), m_pIn (0), m_pOut (0),
	m_pInURB {}, m_pOutURB {}, m_bInBusy {}, m_bOutBusy {},
	m_nInPending (0), m_nOutPending (0), m_bStreaming (FALSE),
	m_nOutCounter (0), m_bOutCounterSet (FALSE), m_nExpectIn (0), m_bHaveIn (FALSE), m_nPhase (0),
	m_nCredit (0), m_bLeadDue (TRUE), m_nLeadGiven (0),
	m_nInBlocks (0), m_nInShort (0), m_nInBadMagic (0), m_nInGaps (0), m_nInLost (0), m_nInMaxGap (0),
	m_nInOddGap (0), m_nInErrors (0), m_nInSubmitFail (0),
	m_nOutBlocks (0), m_nOutErrors (0), m_nOutSubmitFail (0), m_nCreditDrops (0), m_nOutMaxDt (0), m_nOutPendMin (~0u),
	m_nInPeak (0), m_nInMaxDt (0),
	m_nStartUs (0), m_nLastInUs (0), m_nLastOutUs (0), m_nFirstInUs (0), m_nFirstOutUs (0), m_nOut16Us (0), m_nLastLogUs (0),
	m_bSaidStart (FALSE), m_nPrevIn (0), m_nPrevOut (0), m_nSeconds (0)
{
	CKernelOptions *pOpt = CKernelOptions::Get ();
	m_nURBs = Clamp (pOpt->GetAppOptionDecimal ("oburbs", 8), 1, MaxURBs);
	m_nBlocks = Clamp (pOpt->GetAppOptionDecimal ("obblocks", 1), 1, MaxBlocks);
	m_nOutDelay = pOpt->GetAppOptionDecimal ("oboutdelay", 0);
	m_nOutLead = Clamp (pOpt->GetAppOptionDecimal ("oboutlead", 0), 0, m_nURBs * m_nBlocks);
	m_nRelayLead = Clamp (pOpt->GetAppOptionDecimal ("obrelaylead", 7), 0, m_nURBs * m_nBlocks);
	const char *pMode = pOpt->GetAppOptionString ("obout", "paced");
	m_OutMode = strcmp (pMode, "free") == 0 ? OutFree : strcmp (pMode, "off") == 0 ? OutOff : OutPaced;
	const char *pCtr = pOpt->GetAppOptionString ("obctr", "zero");
	m_bCounterFromIn = strcmp (pCtr, "in") == 0;

	memset (&s_EvIn, 0, sizeof s_EvIn);
	memset (&s_EvOut, 0, sizeof s_EvOut);

	memset ((void *) m_Pipe, 0, sizeof m_Pipe);	// usb=mirror
	m_nPipes = 0;
	m_bMirror = FALSE;
	m_bAttachDue = FALSE;
	m_bMidiErred = FALSE;
	m_nMidiFailUs = 0;
	m_pCtlBuf = 0;
	m_bCtlBusy = FALSE;
	m_pCtlURB = 0;
	m_pCtlSetup = 0;
	m_nGen = ++s_nGen;
	m_bUp = FALSE;
	m_bConfigured = FALSE;
	m_bShowDue = FALSE;
	m_bShown = FALSE;
	m_nStarveRestarts = 0;
	m_nGateAct = obgate::Wait;

	m_nSlot = nSlot < Slots ? nSlot : Slots - 1;	// its core slot (attach order): its buffers, its side stage, its FX
	m_pInBuf = s_InBuf[m_nSlot];
	m_pOutBuf = s_OutBuf[m_nSlot];
	s_pBox[m_nSlot] = this;
	Rebind ();
}

// The box left (Circle's ~CUSBDevice, task level), or Configure failed (CUSBDevice::Configure deletes the function).
// Left: ~CXHCIUSBDevice ran first (the derived class's destructor) and disabled and freed the xHC slot, so no transfer
// event reaches these endpoints any more (CXHCISlotManager::TransferEvent drops a freed slot's): the URBs still queued
// are never completed, and each is freed here, in flight or not. A failed Configure leaves the slot running, so an URB
// in flight there is left allocated (its completion may still come), as every one was before.
CObUsb::~CObUsb (void)
{
	const boolean bWasPrimary = Primary ();
	IMirrorTarget *pTarget = m_bMirror ? Target () : 0;	// (asked while this box is still its slot's)
	(void) pTarget;					// (no mirror in this build)
	if (s_pBox[m_nSlot] == this) s_pBox[m_nSlot] = 0;
	Rebind ();
	if (m_bShowDue && m_nSlot < 2) obgate::gone (s_Gate[m_nSlot]);	// (the gate's own re-enumeration: the hold goes on)
	if (m_bUp) { s_Assign.up[m_nSlot] = false; s_Assign.gone_at[m_nSlot] = NowUs (); }	// USB-C's place held (mirror_assign.h)
	if (this == s_pRelayBox)			// gone, not moved: the relay holds it for mirrorhold, as ever
	{
		s_pRelayBox = 0;
		if (s_pRelay != 0) s_pRelay->SetBox (0);
	}
	m_bStreaming = FALSE;
	if (bWasPrimary) core::out::set_device (s_bActive && s_pThis != 0 ? s_pThis->m_pAudio : 0);	// the other box's, if one streams
	const boolean bGone = m_bConfigured;
	unsigned nFreed = 0, nKept = 0;
	auto Free = [&] (CUSBRequest *pURB, boolean bBusy)
	{
		if (pURB == 0) return;
		if (!bBusy) { delete pURB; return; }
		if (bGone) { delete pURB; nFreed++; }
		else nKept++;
	};
#ifdef CIRCLE_GADGET_MIRROR
	if (pTarget != 0) pTarget->MirrorBoxUp (FALSE);
	for (unsigned i = 0; i < m_nPipes; i++)
	{
		Free (m_Pipe[i].pURB, m_Pipe[i].bBusy);
		m_Pipe[i].pURB = 0;
		delete m_Pipe[i].pEP;
		m_Pipe[i].pEP = 0;
	}
	if (m_pCtlURB != 0)				// a forwarded request in flight (on EP0, which ~CUSBDevice deletes next)
	{
		Free (m_pCtlURB, TRUE);
		if (bGone) delete m_pCtlSetup;
		m_pCtlURB = 0;
		m_pCtlSetup = 0;
	}
#endif
	core::device::usb_detach ((int) m_nSlot);	// its slot freed (the control core's next pass): the other box stays
	for (unsigned i = 0; i < MaxURBs; i++)
	{
		Free (m_pInURB[i], m_bInBusy[i]);
		Free (m_pOutURB[i], m_bOutBusy[i]);
		m_pInURB[i] = m_pOutURB[i] = 0;
	}
	delete m_pOut;
	delete m_pIn;
	m_pOut = m_pIn = 0;
	CountDetach (bGone, nFreed, nKept);
	CLogger::Get ()->Write (From, LogNotice, "%s: %u URBs in flight freed, %u kept (USB requests now %u)",
				bGone ? "box gone" : "not configured", nFreed, nKept, UsbRequests ());
}

// Log every interface/alternate/endpoint of the default configuration; find alt 3's endpoints.
boolean CObUsb::ScanDescriptors (void)
{
	const TUSBConfigurationDescriptor *pCfg = GetDevice ()->GetConfigurationDescriptor ();
	if (pCfg == 0) return FALSE;

	const u8 *p = (const u8 *) pCfg;
	const u8 *end = p + pCfg->wTotalLength;
	static const char *Speeds[] = {"LS", "FS", "HS", "SS"};
	CLogger::Get ()->Write (From, LogNotice, "speed %s, config %u: %u interfaces, total %u B",
				Speeds[GetDevice ()->GetSpeed ()], (unsigned) pCfg->bConfigurationValue,
				(unsigned) pCfg->bNumInterfaces, (unsigned) pCfg->wTotalLength);

	int iface = -1, alt = -1;
	while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end)
	{
		if (p[1] == DESCRIPTOR_INTERFACE && p[0] >= sizeof (TUSBInterfaceDescriptor))
		{
			const TUSBInterfaceDescriptor *d = (const TUSBInterfaceDescriptor *) p;
			iface = d->bInterfaceNumber;
			alt = d->bAlternateSetting;
			CLogger::Get ()->Write (From, LogNotice, "  iface %d alt %d: class %02x/%02x/%02x, %u endpoints",
						iface, alt, (unsigned) d->bInterfaceClass, (unsigned) d->bInterfaceSubClass,
						(unsigned) d->bInterfaceProtocol, (unsigned) d->bNumEndpoints);
		}
		else if (p[1] == DESCRIPTOR_ENDPOINT && p[0] >= sizeof (TUSBEndpointDescriptor))
		{
			const TUSBEndpointDescriptor *e = (const TUSBEndpointDescriptor *) p;
			CLogger::Get ()->Write (From, LogNotice, "    ep 0x%02x attr 0x%02x mps %u interval %u",
						(unsigned) e->bEndpointAddress, (unsigned) e->bmAttributes,
						(unsigned) e->wMaxPacketSize, (unsigned) e->bInterval);

			bool intr = (e->bmAttributes & 3) == 3;
			if (alt == ob::kAlt && intr)
			{
				unsigned nMps = e->wMaxPacketSize & 0x7FF;
				if (iface == ob::kInIface && e->bEndpointAddress == ob::kInEp && nMps == m_nInBlk) m_pInDesc = e;
				if (iface == ob::kOutIface && e->bEndpointAddress == ob::kOutEp && nMps == m_nOutBlk) m_pOutDesc = e;
			}
		}
		p += p[0];
	}

	return m_pInDesc != 0 && m_pOutDesc != 0;
}

boolean CObUsb::Configure (void)
{
	AllocBufs (m_nSlot);

	const TUSBDeviceDescriptor *pDevDesc = GetDevice ()->GetDeviceDescriptor ();
	CLogger::Get ()->Write (From, LogNotice, "%s (%04x:%04x) claimed", m_pAudio->name, (unsigned) pDevDesc->idVendor,
				(unsigned) pDevDesc->idProduct);
	if (m_nUid != 0) CLogger::Get ()->Write (From, LogNotice, "slot %u: UID %08x (vendor request 0)", m_nSlot, (unsigned) m_nUid);
	else CLogger::Get ()->Write (From, LogNotice, "slot %u: no UID from the vendor request (the core asks by MIDI)", m_nSlot);

	// Overbridge needs high speed. Below it the stream cannot start ("cannot submit the first URBs"), so refuse at
	// once and have the root port enumerate the box again (ob_speed.h): a new port reset, a new chance at high speed
	// obfsnext=<slot> (a test): the next box in that slot refused as if it came up at full speed. Its other functions
	// keep the device on the port as in SR4, so the retry below has to remove it before the rescan
	const boolean bForceFs = s_nForceFs == (int) m_nSlot;
	if (bForceFs)
	{
		s_nForceFs = -1;
		CLogger::Get ()->Write (From, LogWarning, "obfsnext: slot %u taken as full speed (a test; it is at %s)", m_nSlot,
					GetDevice ()->GetSpeed () == USBSpeedHigh ? "HS" : "not HS");
	}
	if (GetDevice ()->GetSpeed () != USBSpeedHigh || bForceFs)
	{
		static const char *Speeds[] = {"LS", "FS", "HS", "SS", "?"};
		const char *pSpeed = bForceFs ? "FS" : Speeds[GetDevice ()->GetSpeed () <= USBSpeedUnknown ? GetDevice ()->GetSpeed () : 4];
		unsigned nNow = NowMs ();
		CUSBHCIRootPort *pPort = GetDevice ()->GetParentRootPort ();
		CUSBStandardHub *pHub = pPort == 0 ? BuiltInHub (GetDevice ()) : 0;
		if (pPort == 0 && pHub == 0)
		{
			CLogger::Get ()->Write (From, LogError, "%s came up at %s, Overbridge needs HS: behind a hub, so no retry; "
						"unplug it and plug it in again", m_pAudio->name, pSpeed);
		}
		else if (obspeed::refused (s_Retry, nNow))
		{
			s_pRetryPort = pPort, s_pRetryHub = pHub, s_nRetryHubPort = GetDevice ()->GetParentHubPortIndex ();
			s_pRetryName = m_pAudio->name, s_nRetrySlot = m_nSlot;
			CLogger::Get ()->Write (From, LogWarning, "%s came up at %s, Overbridge needs HS: enumerating it again "
						"in %u s (try %u of %u)", m_pAudio->name, pSpeed,
						obspeed::kWaitMs[s_Retry.tries] / 1000, s_Retry.tries + 1, obspeed::kTries);
		}
		else
		{
			CLogger::Get ()->Write (From, LogError, "%s still at %s after %u tries: unplug it and plug it in again, "
						"or try another cable or port", m_pAudio->name, pSpeed, obspeed::kTries);
		}
		return FALSE;
	}
	obspeed::high_speed (s_Retry);

	if (!ScanDescriptors ())
	{
		CLogger::Get ()->Write (From, LogError, "no interrupt endpoints 0x83/0x03 (%u/%u B) at alt 3: "
					"not in OVERBRIDGE mode?", m_nInBlk, m_nOutBlk);
		return FALSE;
	}

	// SET_INTERFACE 1 and 2 alt 0 first (stream off), then alt 3 (same order as RytmHost.cpp).
	// spike3, 2026-10-14: after a host crashed mid-stream the Rytm kept answering alt 3 with
	// silent IN blocks (pk 0) and NAKed OUT after 16 blocks, through Pi reboots (USB resets)
	// and with spike2 too; alt 0 before alt 3 is the attempt to restart its stream engine.
	CUSBHostController *pHost = GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		if (pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE,
					   0, nIface, 0, 0) < 0)
		{
			CLogger::Get ()->Write (From, LogWarning, "SET_INTERFACE %u alt 0 failed", (unsigned) nIface);
		}
	}
	CTimer::Get ()->MsDelay (100);
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		if (pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE,
					   ob::kAlt, nIface, 0, 0) < 0)
		{
			CLogger::Get ()->Write (From, LogError, "SET_INTERFACE %u alt %u failed", (unsigned) nIface,
						(unsigned) ob::kAlt);
			return FALSE;
		}
	}

	// creates the xHCI endpoint contexts (after the alternate setting is selected)
	m_pIn = new CUSBEndpoint (GetDevice (), m_pInDesc);
	m_pOut = new CUSBEndpoint (GetDevice (), m_pOutDesc);

	const unsigned nLen = m_nBlocks * m_nInBlk, nOutLen = m_nBlocks * m_nOutBlk;
	for (unsigned i = 0; i < m_nURBs; i++)
	{
		m_pInURB[i] = new CUSBRequest (m_pIn, m_pInBuf[i], nLen);
		m_pInURB[i]->SetCompletionRoutine (InStub, (void *) (uintptr) i, this);
		m_pOutURB[i] = new CUSBRequest (m_pOut, m_pOutBuf[i], nOutLen);
		m_pOutURB[i]->SetCompletionRoutine (OutStub, (void *) (uintptr) i, this);
	}

	// up: who goes where now (mirror_assign.h); the box USB-C or the relay takes is read at once, else when assigned
	m_bUp = TRUE;
	s_Assign.up_at[m_nSlot] = NowUs ();
	PollAssign ();
	if (s_pRelay != 0 && this == RelayBox () && s_pRelayBox != this) { s_pRelayBox = this; s_pRelay->SetBox (this); }	// (no change: a box for the relay as it is)
#ifdef CIRCLE_GADGET_MIRROR
	if (IMirrorTarget *pT = Target ()) { if (pT->Mirror ()) MirrorAttach (); }
	else m_bAttachDue = TRUE;			// the box came up before its target (at boot: the gadget): MirrorAttachLate
#endif

	static const char *Modes[] = {"paced by IN", "free-running", "off"};
	CLogger::Get ()->Write (From, LogNotice, "alt 3 selected: %u URBs x %u blocks (%u B) per endpoint, "
				"OUT %s (delay %u blk, lead %u blk, %u through a relay), OUT counter from %s", m_nURBs, m_nBlocks, nLen,
				Modes[m_OutMode], m_nOutDelay, m_nOutLead, m_nRelayLead, m_bCounterFromIn ? "IN" : "0");

	if (m_nOutCh != ob::kChannels)
	{
		CLogger::Get ()->Write (From, LogNotice, "OUT %u channels (%u B a block), %s", m_nOutCh, m_nOutBlk,
					m_pAudio->ret == core::out::kStereoMix ? "the voices as one stereo mix" : "per voice");
	}

	if (s_bActive && Primary ()) core::out::set_device (m_pAudio);	// the voices' channels and routes follow the box; not while the TCP link owns g_Output
	loop_pi::SetBox (m_nSlot, m_pAudio);	// its looper's returns and make-ups: this box's (core/loop)

	// the completions run in the xHCI IRQ: start everything with it held off, so the
	// TRB order on each ring is the submit order and the counters stay consistent
	if (s_bActive && Primary ()) g_Output.Reset ();	// fade in again on every new stream (not while the TCP link owns g_Output)
	if (s_bActive && Primary ()) masterfx_pi::Reset (core::out::g_base_slot);	// the base box's master chain
	if (s_bActive && !Primary ()) masterfx_pi::Reset (m_nSlot);	// a side box's own (phase 3(d))
	if (!Primary ()) side_out::Restart ();	// its own voices fade in again (side_out.h)
	if (!Primary ()) CLogger::Get ()->Write (From, LogNotice, "%s streams beside %s: its own clock, its own voices", m_pAudio->name,
						s_pThis != 0 ? s_pThis->m_pAudio->name : "?");
	EnterCritical (IRQ_LEVEL);
	m_nStartUs = m_nLastInUs = m_nLastOutUs = m_nLastLogUs = CTimer::GetClockTicks ();
	m_InHalt.halted = m_OutHalt.halted = false;	// (a stream starts with nothing to recover)
	m_bStreaming = TRUE;
	boolean bOK = TRUE;
	for (unsigned i = 0; i < m_nURBs && bOK; i++)
	{
		bOK = SubmitIn (i);
	}
	if (bOK && m_OutMode == OutFree)
	{
		for (unsigned i = 0; i < m_nURBs && bOK; i++)
		{
			FillOut (m_pOutBuf[i], m_nBlocks);
			bOK = SubmitOut (i);
		}
	}
	LeaveCritical ();

	if (!bOK)
	{
		CLogger::Get ()->Write (From, LogError, "cannot submit the first URBs");
		return FALSE;
	}

	m_bConfigured = TRUE;
	return TRUE;
}

// ---- usb=mirror: the box's side (docs/usb-mirror.md, sidecar_gadget.h) -------------------------------------------
#ifdef CIRCLE_GADGET_MIRROR

// A completion's parameter: this attach's generation and an index. A box that went away and came back can get a new
// CObUsb at the old one's address, so the pointer alone does not tell an old URB's completion from a new one's.
static inline void *Param (unsigned nGen, unsigned nIndex)	{ return (void *) ((uintptr) nGen << 32 | nIndex); }
static inline unsigned ParamGen (void *pParam)			{ return (unsigned) ((uintptr) pParam >> 32); }
static inline unsigned ParamIndex (void *pParam)		{ return (unsigned) ((uintptr) pParam & 0xFFFFFFFF); }

// core 0's loop (the gadget's MirrorPoll): a box configured before the gadget existed is attached now
void CObUsb::MirrorAttachLate (void)
{
	for (unsigned k = 0; k < Slots; k++)
		if (s_pBox[k] != 0 && s_pBox[k] == GadgetBox ()) s_pBox[k]->AttachLate ();
}

void CObUsb::AttachLate (void)
{
	IMirrorTarget *pT = Target ();
	if (!m_bAttachDue || pT == 0) return;
	m_bAttachDue = FALSE;
	if (!pT->Mirror ()) return;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %s was up before its target or the mode: read now", m_pAudio->name);
	MirrorAttach ();
}

static volatile unsigned s_nParked;		// the box's pipe IN packets while the computer saw the Sidecar device

// the mirror line's "parked": pipe IN packets of the box that came while the computer saw the Sidecar device (CLASS
// COMPLIANT, or the time until the switch to the box): drained, as no endpoint of the box is there to take them
unsigned CObUsb::MirrorParked (void)
{
	return s_nParked;
}

// core 0's loop (the gadget's SetMirror): the USB-C mode set to OVERBRIDGE at run time. A box that streams and was not read
// (the mode was CLASS COMPLIANT at its Configure) is read by the next MirrorAttachLate, as one configured before the gadget
void CObUsb::MirrorWanted (void)
{
	if (CObUsb *p = GadgetBox ()) p->Wanted ();
}

void CObUsb::Wanted (void)
{
	if (m_bStreaming && !m_bMirror) m_bAttachDue = TRUE;
}

// SetMirrorTarget: a new target for a box that already streams (a Teensy relay plugged in or replugged while the box is
// up) gets its descriptors and strings at the next AttachLate; pipes already open stay as they are (MirrorAttach)
void CObUsb::Reattach (void)
{
	if (m_bStreaming) m_bAttachDue = TRUE;
}

// PollAssign: this box moved from the relay to USB-C. A pipe IN packet the relay took waits for its computer's
// MirrorPipeInNext, which never comes now: each IN pipe not in flight and not halted asks the box again
void CObUsb::KickPipes (void)
{
	EnterCritical (IRQ_LEVEL);
	for (unsigned i = 0; i < m_nPipes; i++)
		if ((m_Pipe[i].uchAddr & 0x80) && !m_Pipe[i].bBusy && !m_Pipe[i].bStopped) SubmitPipeIn (i);
	LeaveCritical ();
}

static char s_PipeInfo[2][192];

const char *CObUsb::PipeInfo (unsigned nSlot) { return nSlot < 2 ? s_PipeInfo[nSlot] : ""; }

static u8 s_BoxCfg[2][512];
static unsigned s_nBoxCfg[2];

const u8 *CObUsb::BoxConfig (unsigned nSlot, unsigned *pLen) { *pLen = nSlot < 2 ? s_nBoxCfg[nSlot] : 0; return nSlot < 2 ? s_BoxCfg[nSlot] : 0; }

// Configure (task level): the box's descriptors to the gadget, its string descriptors read once, its pipes opened
void CObUsb::MirrorAttach (void)
{
	IMirrorTarget *pGadget = Target ();		// (the gadget's adapter, or this slot's own target)
	if (pGadget == 0) return;
	const TUSBDeviceDescriptor *pDev = GetDevice ()->GetDeviceDescriptor ();
	const TUSBConfigurationDescriptor *pCfg = GetDevice ()->GetConfigurationDescriptor ();
	if (pDev == 0 || pCfg == 0) return;
	const u8 *pC = (const u8 *) pCfg;
	unsigned nCfg = pCfg->wTotalLength;
	if (m_nSlot < 2)
	{
		s_nBoxCfg[m_nSlot] = nCfg < sizeof s_BoxCfg[0] ? nCfg : sizeof s_BoxCfg[0];
		memcpy (s_BoxCfg[m_nSlot], pC, s_nBoxCfg[m_nSlot]);
	}
	if (!pGadget->MirrorSetDescriptors ((const u8 *) pDev, pC, nCfg)) return;
	const boolean bPipesOpen = m_bMirror;		// read before (Reattach: a new target): its pipes run already
	m_bMirror = TRUE;

	if (s_pCtlBuf[m_nSlot] == 0) s_pCtlBuf[m_nSlot] = AllocDmaBuf ();
	m_pCtlBuf = s_pCtlBuf[m_nSlot];
	CUSBHostController *pHost = GetHost ();
	u8 *pBuf = m_pCtlBuf;
	u16 nLang = 0x0409;
	int n = pHost->GetDescriptor (GetEndpoint0 (), DESCRIPTOR_STRING, 0, pBuf, 255, REQUEST_IN, 0);
	if (n >= 4 && pBuf[1] == DESCRIPTOR_STRING)
	{
		pGadget->MirrorSetString (0, pBuf, n);
		nLang = (u16) (pBuf[2] | pBuf[3] << 8);
	}
	u8 Index[mirror::kMaxStrings];
	unsigned nIdx = mirror::string_indexes ((const u8 *) pDev, pC, nCfg, Index, mirror::kMaxStrings - 1);
	unsigned nGot = 0;
	for (unsigned k = 0; k < nIdx; k++)
	{
		n = pHost->GetDescriptor (GetEndpoint0 (), DESCRIPTOR_STRING, Index[k], pBuf, 255, REQUEST_IN, nLang);
		if (n >= 2 && pBuf[1] == DESCRIPTOR_STRING && pBuf[0] <= n) { pGadget->MirrorSetString (Index[k], pBuf, pBuf[0]); nGot++; }
	}

	// the pipes: vendor-class endpoints other than the audio stream's, at the alternate they are in (SET_INTERFACE when
	// that is not 0). Never on an interface of the audio stream: the Pi's own stream stays at the alternate the engine
	// opened (ob::kAlt), so a pipe there is taken only at that alternate.
	mirror::Layout l;
	if (!bPipesOpen && m_nSlot < 2) s_PipeInfo[m_nSlot][0] = 0;
	if (bPipesOpen) l.n = 0;
	else if (!mirror::parse (pC, nCfg, ob::kInEp, ob::kOutEp, &l))
	{
		CLogger::Get ()->Write (From, LogWarning, "usb=mirror: the box's configuration does not parse: no pipes");
		l.n = 0;
	}
	boolean bAudioIface[mirror::kMaxIfaces] = {FALSE};
	for (unsigned k = 0; k < l.n; k++)
		if (l.ep[k].role == mirror::kAudioIn || l.ep[k].role == mirror::kAudioOut) bAudioIface[l.ep[k].iface] = TRUE;
	for (unsigned k = 0; k < l.n && m_nPipes < MaxPipes; k++)
	{
		const mirror::Ep &e = l.ep[k];
		if (e.role != mirror::kPipeIn && e.role != mirror::kPipeOut) continue;
		const boolean bAudio = bAudioIface[e.iface];
		const TUSBEndpointDescriptor *pDesc = 0;
		int iface = -1, alt = -1;
		for (unsigned o = 0; o + 2 <= nCfg && pC[o] >= 2 && o + pC[o] <= nCfg && pDesc == 0; o += pC[o])
		{
			if (pC[o + 1] == DESCRIPTOR_INTERFACE) iface = pC[o + 2], alt = pC[o + 3];
			else if (pC[o + 1] == DESCRIPTOR_ENDPOINT && pC[o + 2] == e.addr && (!bAudio || alt == ob::kAlt))
				pDesc = (const TUSBEndpointDescriptor *) (pC + o);
		}
		if (pDesc == 0)
		{
			if (bAudio) CLogger::Get ()->Write (From, LogWarning, "usb=mirror: pipe 0x%02x is on the audio interface %u "
							    "but not at alt %u: not relayed", e.addr, (unsigned) e.iface, (unsigned) ob::kAlt);
			continue;
		}
		if (!bAudio && alt > 0 && pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, alt, iface, 0, 0) < 0)
		{
			CLogger::Get ()->Write (From, LogWarning, "usb=mirror: SET_INTERFACE %d alt %d for pipe 0x%02x failed", iface, alt, e.addr);
			continue;
		}
		if (m_nSlot < 2)			// TR2c: the pipes open beside the relay's link (its throughput fell to ~1 block/ms)
		{
			char *s = s_PipeInfo[m_nSlot];
			unsigned len = strlen (s);
			if (len + 40 < sizeof s_PipeInfo[0])
			{
				// "0xAA tT mps 0xMMMM iv I" without CString (the host-test shim has none)
				static const char H[] = "0123456789abcdef";
				char *o = s + len;
				const unsigned nMps = pDesc->wMaxPacketSize, nIv = pDesc->bInterval;
				if (len) *o++ = ',', *o++ = ' ';
				*o++ = '0', *o++ = 'x', *o++ = H[pDesc->bEndpointAddress >> 4], *o++ = H[pDesc->bEndpointAddress & 15];
				*o++ = ' ', *o++ = 't', *o++ = (char) ('0' + (pDesc->bmAttributes & 3));
				*o++ = ' ', *o++ = 'm', *o++ = 'p', *o++ = 's', *o++ = ' ', *o++ = '0', *o++ = 'x';
				for (int sh = 12; sh >= 0; sh -= 4) *o++ = H[(nMps >> sh) & 15];
				*o++ = ' ', *o++ = 'i', *o++ = 'v', *o++ = ' ';
				if (nIv >= 10) *o++ = (char) ('0' + nIv / 10 % 10);
				*o++ = (char) ('0' + nIv % 10);
				*o = 0;
			}
		}
		TPipe &p = m_Pipe[m_nPipes];
		p.uchAddr = e.addr;
		p.nMps = (pDesc->wMaxPacketSize & 0x7FF) > 512 ? 512 : pDesc->wMaxPacketSize & 0x7FF;
		// An interrupt pipe of the box the Teensy relay shows is polled every 1 ms at most (bInterval 4 at high speed):
		// the A4's async IN 0x82 asks for every 125 us, and polled that often beside the relay's link the Pi's host
		// controller sent the link only ~1 block/ms (T330); every 1 ms the link carried the full rate and the Overbridge
		// Engine's latency check passed (T335). Every box but the Rytm (the A4) does so on USB-C too: with the Rytm on the relay beside it
		// (the default route) both boxes' async pipes at 125 us made 6 every-microframe interrupt endpoints on the VL805,
		// one more than it serves in full, and one box's OUT ran short (PD6/PD6b, T416/T417; diagnosis-periodic-starvation).
		// The Rytm keeps its own interval wherever it is: with its pipe at 1 ms both boxes' IN collapsed in stalls (PV1;
		// PA1, T353: its stream died). A box moved later (PollAssign) keeps the interval its pipes were opened with.
		// mirrorpipeiv=N sets it for every box (0: the box's own)
		static TUSBEndpointDescriptor s_Desc[2][MaxPipes];
		unsigned nIv = CKernelOptions::Get ()->GetAppOptionDecimal ("mirrorpipeiv", m_pAudio != &core::out::kRytmMk2 ? 4 : 0);
		if (nIv > 16) nIv = 16;
		if (m_nSlot < 2 && (pDesc->bmAttributes & 3) == 3 && nIv > pDesc->bInterval)
		{
			TUSBEndpointDescriptor &d = s_Desc[m_nSlot][m_nPipes];
			memcpy (&d, pDesc, sizeof d);
			d.bInterval = (u8) nIv;
			CLogger::Get ()->Write (From, LogNotice, "usb=mirror: pipe 0x%02x polled at bInterval %u (the box asks %u)", e.addr, nIv, (unsigned) pDesc->bInterval);
			pDesc = &d;
		}
		p.pEP = new CUSBEndpoint (GetDevice (), pDesc);
		if (s_PipeBuf[m_nSlot][m_nPipes] == 0) s_PipeBuf[m_nSlot][m_nPipes] = AllocDmaBuf ();
		p.pBuf = s_PipeBuf[m_nSlot][m_nPipes];
		p.bBusy = p.bStopped = FALSE;
		p.nPackets = p.nErrors = p.nTries = p.nLastTry = 0;
		p.bRefused = FALSE;
		p.pURB = 0;
		if (e.addr & 0x80)
		{
			p.pURB = new CUSBRequest (p.pEP, p.pBuf, p.nMps);
			p.pURB->SetCompletionRoutine (PipeInStub, Param (m_nGen, m_nPipes), this);
		}
		m_nPipes++;
	}
	// mirrorpipepoll=0 (TR2d, a test): the pipes opened, but no IN request kept on them, so the box's interrupt IN pipe
	// is not polled every microframe beside the relay's link (with it the link ran at ~1 block/ms, T329/T330)
	if (!bPipesOpen && CKernelOptions::Get ()->GetAppOptionDecimal ("mirrorpipepoll", 1) != 0)
	{
		EnterCritical (IRQ_LEVEL);
		for (unsigned i = 0; i < m_nPipes; i++) if (m_Pipe[i].uchAddr & 0x80) SubmitPipeIn (i);
		LeaveCritical ();
	}
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %u strings (language %04x), %u pipes (%s)", nGot, (unsigned) nLang, m_nPipes,
				m_nSlot < 2 ? s_PipeInfo[m_nSlot] : "");
	// the attach gate (attach_gate.h): shown to the computer once its audio runs both ways. A box already shown (moved
	// to another target, PollAssign) is shown at once
	if (m_bShown || GateHoldMs () == 0 || m_nSlot >= 2)
	{
		m_bShowDue = FALSE;
		m_bShown = TRUE;
		pGadget->MirrorBoxUp (TRUE);
		return;
	}
	if (m_bShowDue) return;				// held already (a new target meanwhile): the same hold
	const boolean bGoesOn = s_Gate[m_nSlot].reenum_due;
	m_bShowDue = TRUE;
	m_nGateAct = obgate::Wait;
	obgate::begin (s_Gate[m_nSlot], NowMs (), GateHoldMs ());
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %s held from the computer until its audio runs both ways "
				"(obgate=%u ms%s)", m_pAudio->name, GateHoldMs (),
				bGoesOn && s_Gate[m_nSlot].held && s_Gate[m_nSlot].holds > 0 ? ", the hold goes on after the gate's "
				"re-enumeration" : "");
}

boolean CObUsb::SubmitPipeIn (unsigned i)
{
	TPipe &p = m_Pipe[i];
	p.pURB->SetStatus (0);
	p.pURB->SetResultLen (0);
	p.bBusy = TRUE;
	if (GetHost ()->SubmitAsyncRequest (p.pURB)) return TRUE;
	p.bBusy = FALSE;
	p.bStopped = TRUE;
	p.nErrors++;
	return FALSE;
}

// xHCI IRQ: the box's packet to the computer; the next request goes out when the computer has it (MirrorPipeInNext)
void CObUsb::PipeInStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CObUsb *pThis = (CObUsb *) pContext;
	if (!Live (pThis) || ParamGen (pParam) != pThis->m_nGen) return;
	TPipe &p = pThis->m_Pipe[ParamIndex (pParam)];
	p.bBusy = FALSE;
	if (pURB->GetStatus () == 0) { p.nErrors++; p.bStopped = TRUE; return; }	// halted: MirrorPipePoll clears it and resubmits
	p.nTries = 0;
	unsigned n = pURB->GetResultLength ();
	InvalidateDataCacheRange ((uintptr) p.pBuf, kBufSize);
	IMirrorTarget *pGadget = pThis->Target ();
	if (pGadget != 0 && !pGadget->MirrorShown ())	// the Sidecar device is shown: drained, not a drop (always renewed,
	{						// so no packet of the box waits in it for the computer)
		s_nParked++;
		pThis->SubmitPipeIn (ParamIndex (pParam));
		return;
	}
	p.nPackets++;
	if (pGadget == 0 || !pGadget->MirrorPipeIn (p.uchAddr, p.pBuf, n)) pThis->SubmitPipeIn (ParamIndex (pParam));
}

void CObUsb::MirrorPipeInNext (u8 uchAddr)
{
	if (CObUsb *p = GadgetBox ()) p->PipeInNext (uchAddr);
}

void CObUsb::PipeInNext (u8 uchAddr)
{
	for (unsigned i = 0; i < m_nPipes; i++)
		if (m_Pipe[i].uchAddr == uchAddr && !m_Pipe[i].bBusy && !m_Pipe[i].bStopped) SubmitPipeIn (i);
}

boolean CObUsb::MirrorPipeOut (u8 uchAddr, const u8 *pData, unsigned nLength)
{
	CObUsb *p = GadgetBox ();
	return p != 0 && p->PipeOut (uchAddr, pData, nLength);
}

boolean CObUsb::PipeOut (u8 uchAddr, const u8 *pData, unsigned nLength)
{
	CObUsb *pThis = this;
	if (nLength > 512) return FALSE;
	for (unsigned i = 0; i < pThis->m_nPipes; i++)
	{
		TPipe &p = pThis->m_Pipe[i];
		if (p.uchAddr != uchAddr) continue;
		if (p.bBusy || p.bStopped) return FALSE;	// halted: dropped until MirrorPipePoll has cleared it
		if (nLength) memcpy (p.pBuf, pData, nLength);
		CleanAndInvalidateDataCacheRange ((uintptr) p.pBuf, kBufSize);
		CUSBRequest *pURB = new CUSBRequest (p.pEP, nLength ? p.pBuf : 0, nLength);	// its length is this packet's (IRQ-safe heap); a zero-length one has no buffer (Circle asserts nBufLen > 0 on one)
		pURB->SetCompletionRoutine (PipeOutStub, Param (pThis->m_nGen, i), pThis);
		p.bBusy = TRUE;
		p.pURB = pURB;					// ~CObUsb frees it if the box leaves first
		if (pThis->GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
		p.pURB = 0;
		p.bBusy = FALSE;
		p.nErrors++;
		delete pURB;
		return FALSE;
	}
	return FALSE;
}

void CObUsb::PipeOutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CObUsb *pThis = (CObUsb *) pContext;
	if (Live (pThis) && ParamGen (pParam) == pThis->m_nGen)
	{
		TPipe &p = pThis->m_Pipe[ParamIndex (pParam)];
		p.pURB = 0;
		p.bBusy = FALSE;
		if (pURB->GetStatus () == 0) { p.nErrors++; p.bStopped = TRUE; }	// halted: MirrorPipePoll clears it
		else { p.nPackets++; p.nTries = 0; }
		if (IMirrorTarget *pT = pThis->Target ()) pT->MirrorPipeOutDone (p.uchAddr);	// the computer's next
	}
	delete pURB;
}

// core 0's loop: one class / vendor request of the computer to the box, as it came (setup and data); the answer in the
// completion. FALSE: no box, or one still in flight.
boolean CObUsb::MirrorControl (const TSetupData *pSetup, const u8 *pOutData, unsigned nSeq)
{
	CObUsb *p = GadgetBox ();
	return p != 0 && p->Control (pSetup, pOutData, nSeq);
}

boolean CObUsb::Control (const TSetupData *pSetup, const u8 *pOutData, unsigned nSeq)
{
	CObUsb *pThis = this;
	if (!pThis->m_bMirror || pThis->m_bCtlBusy || pSetup->wLength > 512) return FALSE;
	if ((pSetup->bmRequestType & REQUEST_IN) && pSetup->wLength == 0) return FALSE;	// no data stage to read into: STALL
	TSetupData *pS = new TSetupData;
	memcpy (pS, pSetup, sizeof *pS);
	if (!(pSetup->bmRequestType & REQUEST_IN) && pSetup->wLength) memcpy (pThis->m_pCtlBuf, pOutData, pSetup->wLength);
	CleanAndInvalidateDataCacheRange ((uintptr) pThis->m_pCtlBuf, kBufSize);
	CUSBRequest *pURB = new CUSBRequest (pThis->GetEndpoint0 (), pSetup->wLength ? pThis->m_pCtlBuf : 0, pSetup->wLength, pS);	// no data stage: no buffer (Circle asserts nBufLen > 0 on one)
	pURB->SetCompletionRoutine (MirrorCtlStub, Param (pThis->m_nGen, nSeq), pThis);
	pThis->m_bCtlBusy = TRUE;
	pThis->m_pCtlSetup = pS;
	pThis->m_pCtlURB = pURB;			// ~CObUsb frees it if the box leaves first
	if (pThis->GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
	pThis->m_pCtlURB = 0;
	pThis->m_pCtlSetup = 0;
	pThis->m_bCtlBusy = FALSE;
	delete pURB;
	delete pS;
	return FALSE;
}

void CObUsb::MirrorCtlStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CObUsb *pThis = (CObUsb *) pContext;
	TSetupData *pS = pURB->GetSetupData ();
	if (Live (pThis) && ParamGen (pParam) == pThis->m_nGen)
	{
		pThis->m_pCtlURB = 0;
		pThis->m_pCtlSetup = 0;
		pThis->m_bCtlBusy = FALSE;
		int n = pURB->GetStatus () ? (int) pURB->GetResultLength () : -1;
		InvalidateDataCacheRange ((uintptr) pThis->m_pCtlBuf, kBufSize);
		if (IMirrorTarget *pT = pThis->Target ()) pT->MirrorControlDone (ParamIndex (pParam), pThis->m_pCtlBuf, n);
	}
	delete pURB;
	delete pS;
}

// core 0's loop: a pipe whose transfer failed (the box STALLed it, or the bus had an error) halts on the xHC and on
// the box. Both are cleared - CLEAR_FEATURE(ENDPOINT_HALT) to the box, Reset Endpoint on the host - and an IN pipe is
// resubmitted. Every 100 ms for the first tries, then once a second, so a box that keeps failing costs little.
// Circle's ResetFromHalted gives TRUE for a refused command too (it returns the completion code as a boolean), and a
// reset that took leaves the endpoint Stopped (3): so a state of 2 or 4 after it is a refusal (as ep_halt.h reads it),
// and the pipe stays stopped for the next try. Resubmitted into a ring still halted, the URB would never complete and
// the pipe would stay busy for good.
void CObUsb::MirrorPipePoll (void)
{
	if (CObUsb *p = GadgetBox ()) p->PipePoll ();
}

void CObUsb::PipePoll (void)
{
	CObUsb *pThis = this;
	if (!pThis->m_bMirror) return;
	IMirrorTarget *pT = Target ();
	if (pT == 0 || !pT->MirrorShown ()) return;	// the Sidecar device shown: no EP0 traffic
	const unsigned nNow = CTimer::Get ()->GetTicks ();
	for (unsigned i = 0; i < pThis->m_nPipes; i++)
	{
		TPipe &p = pThis->m_Pipe[i];
		if (!p.bStopped || p.bBusy) continue;
		if (nNow - p.nLastTry < (p.nTries < 5 ? HZ / 10 : HZ)) continue;
		if (pThis->m_bCtlBusy) return;		// EP0 has a forwarded request: next time
		p.nLastTry = nNow;
		p.nTries++;
		s_nPipeResets++;
		pThis->GetHost ()->ControlMessage (pThis->GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_ENDPOINT, CLEAR_FEATURE,
						   ENDPOINT_HALT, p.uchAddr, 0, 0);
#if RASPPI >= 4
		CXHCIEndpoint *pXHCI = p.pEP->GetXHCIEndpoint ();
		if (pXHCI->GetEPState () >= 2)		// halted, stopped or error
		{
			unsigned nAfter = pXHCI->ResetFromHalted () ? pXHCI->GetEPState () : 4;
			if (nAfter == 2 || nAfter == 4)
			{
				if (!p.bRefused)
					CLogger::Get ()->Write (From, LogWarning, "usb=mirror: pipe 0x%02x halted: the host refused its "
								"reset (state %u), trying again", p.uchAddr, nAfter);
				p.bRefused = TRUE;
				continue;
			}
		}
#endif
		EnterCritical (IRQ_LEVEL);
		p.bStopped = FALSE;
		if (p.uchAddr & 0x80) pThis->SubmitPipeIn (i);
		LeaveCritical ();
		if (p.nTries == 1 || p.bRefused)
			CLogger::Get ()->Write (From, LogNotice, "usb=mirror: pipe 0x%02x halted: cleared, %s", p.uchAddr,
						p.bStopped ? "cannot resubmit" : "running again");
		p.bRefused = FALSE;
	}
}

unsigned CObUsb::MirrorPipeResets (void)
{
	return s_nPipeResets;
}

boolean CObUsb::MirrorCtlBusy (void)
{
	CObUsb *p = GadgetBox ();
	return p != 0 && p->CtlBusy ();
}

#else	// a gadget without circle-gadget-mirror.patch (a Pi 5 built with PI5_GADGET=0, the host tests): no mirror

boolean CObUsb::MirrorControl (const TSetupData *, const u8 *, unsigned)	{ return FALSE; }
boolean CObUsb::MirrorPipeOut (u8, const u8 *, unsigned)		{ return FALSE; }
void CObUsb::MirrorAttachLate (void)					{}
unsigned CObUsb::MirrorParked (void)					{ return 0; }
boolean CObUsb::MirrorCtlBusy (void)					{ return FALSE; }
void CObUsb::MirrorWanted (void)					{}
void CObUsb::MirrorPipeInNext (u8)					{}
void CObUsb::MirrorPipePoll (void)					{}
unsigned CObUsb::MirrorPipeResets (void)				{ return 0; }
boolean CObUsb::Control (const TSetupData *, const u8 *, unsigned)	{ return FALSE; }
boolean CObUsb::PipeOut (u8, const u8 *, unsigned)			{ return FALSE; }
void CObUsb::PipeInNext (u8)						{}
void CObUsb::PipePoll (void)						{}
void CObUsb::AttachLate (void)						{}
void CObUsb::Wanted (void)						{}
void CObUsb::Reattach (void)						{}
void CObUsb::KickPipes (void)						{}

#endif

// host -> device blocks: header (0x07FF, counter +7 per block) + 7 frames x 12 channels from the output stage
// (output.h: the voices mapped, faded in, limited; silence when its FIFO is empty). Capture channel 2: OUT MAIN L
// as sent. The control core is woken to refill the FIFO (sev).
u16 CObUsb::OutCounter (void)
{
	CObUsb *p = GadgetBox ();
	return p != 0 ? p->m_nOutCounter : 0;
}

void CObUsb::FillOut (u8 *pBuf, unsigned nBlocks)
{
	IMirrorTarget *pTarget = Target ();
	for (unsigned b = 0; b < nBlocks; b++)
	{
		u8 *pBlock = pBuf + b * m_nOutBlk;
		ob::write_header (pBlock, m_nOutCounter);
		if (pTarget != 0) pTarget->MirrorOutExtra (pBlock, m_nOutCounter);	// usb=mirror
		m_nOutCounter += ob::kFramesPerBlock;
		if (!Primary ())				// a box beside the primary stream: its own voices (side_out.h), no FX
		{
			s32 F[ob::kFramesPerBlock][ob::kChannels] = {};
			s16 V[ob::kFramesPerBlock][ob::kVoices] = {};
			float FD[ob::kFramesPerBlock][ob::kVoices] = {};	// the Rytm's PRE / POST voices' stand-ins for their stems (a box's: unused)
			const uint64_t nRender = core::device::side_slot () == (int) m_nSlot ? g_OutputSide.PulledFrame () : g_Output.PulledFrame ();	// its pass's render frame (its looper's playback)
			// the recorder records this box (rec_pi::Slot, decided 2026-11-26: either box alike): its voices' L / R planes
			// and pairs too, as the primary stream gives them
			const boolean bRec = rec_pi::Slot () == (int) m_nSlot && g_Rec.wants_side ();
			s16 LR[ob::kFramesPerBlock][2 * ob::kVoices] = {};
			float PR[ob::kFramesPerBlock][4] = {};
			float X[ob::kFramesPerBlock][4] = {};		// each FX's own return (the recorder's FX1/FX2)
			if (s_bActive) side_out::Pull (m_nSlot, m_pAudio, F, ob::kFramesPerBlock, V, FD, bRec ? LR : 0, bRec ? PR : 0);
			if (pTarget != 0) pTarget->ToBox (F, V, ob::kFramesPerBlock);	// the computer's OUT, when it mirrors this box
			if (s_bActive && side_out::g_on)	// its own send FX and master chain (phase 3(d): sendfx_pi.h, masterfx_pi.h)
			{
				sendfx_pi::FromVoices (m_nSlot, *m_pAudio, V, ob::kFramesPerBlock, FD);	// the Rytm here: paced by its stems
				sendfx_pi::AddReturn (m_nSlot, *m_pAudio, F, ob::kFramesPerBlock, X);
				masterfx_pi::Process (m_nSlot, *m_pAudio, F, ob::kFramesPerBlock);
			}
			if (s_bActive) loop_pi::OutMix (m_nSlot, F, ob::kFramesPerBlock, nRender);	// its own looper's loop onto its returns, after its master FX (core/loop)
			if (bRec && side_out::g_on)
			{
				u8 nSt = 0;				// its voices on PRE / POST: their take files stereo
				for (int v = 0; v < ob::kVoices; v++) nSt |= core::out::control_of (m_nSlot).route[v] != core::out::kTrk ? 1u << v : 0;
				g_Rec.set_stereo_voices (nSt);
				g_Rec.out_block (V, X, ob::kFramesPerBlock, LR, PR);
			}
			for (int f = 0; f < ob::kFramesPerBlock; f++)
				core::simd::be32 (pBlock + ob::kHeaderBytes + 4 * m_nOutCh * f, F[f], m_nOutCh);
			continue;
		}

		s32 F[ob::kFramesPerBlock][ob::kChannels];
		s16 V[ob::kFramesPerBlock][ob::kVoices];
		s16 LR[ob::kFramesPerBlock][2 * ob::kVoices];		// the voices' L and R planes (core/fx/out.h: PRE / POST in stereo)
		float FD[ob::kFramesPerBlock][ob::kVoices];		// the PRE / POST voices' stand-ins for their silent stems
		float PR[ob::kFramesPerBlock][4];			// the PRE and POST pairs as sent (the recorder's 23 / 24)
		Output::Taps T;
		T.lr = LR, T.feed = FD, T.pairs = PR;
		const uint64_t nRender = g_Output.PulledFrame ();	// this block's first frame's render frame (the looper's playback, core/loop)
		if (s_bActive) g_Output.Pull (F, ob::kFramesPerBlock, V, &T);
		else
		{
			memset (F, 0, sizeof F);
			memset (V, 0, sizeof V);
			memset (LR, 0, sizeof LR);
			memset (FD, 0, sizeof FD);
			memset (PR, 0, sizeof PR);
		}
		if (pTarget != 0)				// the computer's OUT added (UAC2 OUT 1-12), the voices kept
			pTarget->ToBox (F, V, ob::kFramesPerBlock);
		float X[ob::kFramesPerBlock][4] = {};		// each FX's own return (the recorder's FX1/FX2)
		const int nBase = core::out::g_base_slot;	// the base box's send FX and master chain (phase 3(d))
		if (s_bActive) sendfx_pi::FromVoices (nBase, core::out::device (), V, ob::kFramesPerBlock, FD);	// fx-ins: the voices' sends to the FX buses; PRE / POST voices' stems (the Rytm's: paced by its stems, another box's by its voices)
		if (s_bActive) sendfx_pi::AddReturn (nBase, core::out::device (), F, ob::kFramesPerBlock, X);	// the send FX's return on 2/3 or 0/1
		if (s_bActive) CObFollow::Mix (F, ob::kFramesPerBlock);		// a4=1: the A4's MAIN on 2/3 (ob_follow.h)
		if (s_bActive) masterfx_pi::Process (nBase, core::out::device (), F, ob::kFramesPerBlock);	// the master chain over all that (masterfx_pi.h)
		if (s_bActive) loop_pi::OutMix (m_nSlot, F, ob::kFramesPerBlock, nRender);	// this box's looper's loop onto its returns, after the master FX (core/loop, spec-050 3.5)
		spectrum_pi::FromOut (V, X, F, ob::kFramesPerBlock);	// the spectrum's strip, while a reader wants it (a copy)
		g_Play.mix (F, ob::kFramesPerBlock);		// a take's playback onto MAIN L / R (rec_play.h; idle: nothing)
#if CORE_MSAMP
		core::preview::mix (F, ob::kFramesPerBlock, core::out::device ().post[0], core::out::device ().post[1]);	// the browser's preview on POST (core/samp/preview.h; idle: nothing)
#endif
		{
			u8 nSt = 0;					// the voices on PRE / POST: their take files stereo
			for (int v = 0; v < ob::kVoices; v++) nSt |= core::out::control_of (nBase).route[v] != core::out::kTrk ? 1u << v : 0;
			if (rec_pi::Slot () == nBase) g_Rec.set_stereo_voices (nSt);
		}
		if (rec_pi::Slot () == nBase && g_Rec.wants_side ())	// the recorder's box (rec_pi::Slot): the base box here
			g_Rec.out_block (V, X, ob::kFramesPerBlock, LR, PR);	// the voices as rendered, the FX, the pairs
		if (bridge::samp_wants_voices ()) bridge::samp_voices (V, ob::kFramesPerBlock, LR, X);	// the sampler's Sidecar voice (X: its FX, CHOP)
#if RASPPI >= 5
		if (s_bFading)					// FadeOut: every channel, a frame at a time
			for (int f = 0; f < ob::kFramesPerBlock; f++)
			{
				float g = (float) s_nFadeLeft / FadeFrames;
				if (s_nFadeLeft) s_nFadeLeft--;
				for (int c = 0; c < ob::kChannels; c++) F[f][c] = (s32) (F[f][c] * g);
			}
#endif
		for (int f = 0; f < ob::kFramesPerBlock; f++)
		{
			core::simd::be32 (pBlock + ob::kHeaderBytes + 4 * m_nOutCh * f, F[f], m_nOutCh);
		}

		if (s_bCapOn)
		{
			unsigned pos = s_nCapOutPos;
			for (int f = 0; f < ob::kFramesPerBlock && pos < CaptureFrames; f++, pos++)
			{
				s_pCapBuf[pos * CaptureChannels + 2] = F[f][0];
			}
			s_nCapOutPos = pos;
		}
	}
#if defined (__aarch64__)
	asm volatile ("dsb sy; sev");
#endif
}

boolean CObUsb::SubmitIn (unsigned i)
{
	CUSBRequest *pURB = m_pInURB[i];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	m_bInBusy[i] = TRUE;
	m_nInPending++;
	if (!GetHost ()->SubmitAsyncRequest (pURB))
	{
		m_bInBusy[i] = FALSE;
		m_nInPending--;
		m_nInSubmitFail++;
		return FALSE;
	}
	return TRUE;
}

boolean CObUsb::SubmitOut (unsigned i)
{
	CUSBRequest *pURB = m_pOutURB[i];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	m_bOutBusy[i] = TRUE;
	m_nOutPending++;
	if (!GetHost ()->SubmitAsyncRequest (pURB))
	{
		m_bOutBusy[i] = FALSE;
		m_nOutPending--;
		m_nOutSubmitFail++;
		return FALSE;
	}
	return TRUE;
}

void CObUsb::InStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	if (!Live (pContext)) return;		// a late event after unplug: the device is gone
	((CObUsb *) pContext)->InDone ((unsigned) (uintptr) pParam, pURB);
}

void CObUsb::OutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	if (!Live (pContext)) return;
	((CObUsb *) pContext)->OutDone ((unsigned) (uintptr) pParam, pURB);
}

// An A4's IN block as the Rytm's layout (core/device/a4.cpp): MAIN and the tracks where they are, its two inputs on the AUDIO IN channels, the tracks it does not
// have silent. Audio words are copied as they are: nothing here reads their value.
void CObUsb::WidenIn (const u8 *pIn, u8 *pWide, unsigned nCh)
{
	memcpy (pWide, pIn, ob::kHeaderBytes);
	for (int f = 0; f < ob::kFramesPerBlock; f++)
	{
		const u8 *s = pIn + ob::kHeaderBytes + 4 * nCh * f;
		u8 *d = pWide + ob::kHeaderBytes + 4 * ob::kChannels * f;
		memcpy (d, s, 4 * (nCh - 2));
		memset (d + 4 * (nCh - 2), 0, 4 * (ob::kChannels - nCh));
		memcpy (d + 4 * (ob::kChannels - 2), s + 4 * (nCh - 2), 8);
	}
}

// Runs in the xHCI interrupt.
void CObUsb::InDone (unsigned i, CUSBRequest *pURB)
{
	unsigned now = CTimer::GetClockTicks ();
	unsigned dt = now - m_nLastInUs;
	if (dt > m_nInMaxDt) m_nInMaxDt = dt;
	m_nLastInUs = now;
	if (m_nFirstInUs == 0) m_nFirstInUs = now - m_nStartUs + 1;

	m_bInBusy[i] = FALSE;
	m_nInPending--;

	u8 *pBuf = m_pInBuf[i];
	boolean ok = pURB->GetStatus () != 0;
	unsigned len = ok ? pURB->GetResultLength () : 0;
	InvalidateDataCacheRange ((uintptr) pBuf, kBufSize);	// the CPU must not see stale cache lines

	unsigned good = 0;
	if (!ok)
	{
		m_nInErrors++;
		m_InHalt.Fail (now);		// the endpoint halts on an error: no resubmit until PollHalt reset it
	}
	else
	{
		m_InHalt.Ok ();
		if (len % m_nInBlk != 0) m_nInShort++;
		for (unsigned b = 0; b < len / m_nInBlk; b++)
		{
			const u8 *pBlk = pBuf + b * m_nInBlk;
			if (!ob::block_from_device (pBlk)) { m_nInBadMagic++; continue; }
			u8 Wide[kBlk];
			if (m_nInCh != ob::kChannels)	// a box with 8 channels: the 12-channel block every consumer below takes
			{
				WidenIn (pBlk, Wide, m_nInCh);
				pBlk = Wide;
			}

			u16 c = ob::block_counter (pBlk);
			unsigned nLostFrames = 0;		// frames the counter says never came before this block (the looper's ring: silence)
			if (m_bHaveIn && c != m_nExpectIn)
			{
				u16 d = (u16) (c - m_nExpectIn);
				m_nInGaps++;
				if (s_bCapOn) s_nCapGaps++;
				if (d % ob::kFramesPerBlock != 0) m_nInOddGap++;
				else
				{
					unsigned lost = d / ob::kFramesPerBlock;
					nLostFrames = lost * ob::kFramesPerBlock;
					m_nInLost += lost;
					if (lost > m_nInMaxGap) m_nInMaxGap = lost;
					if (s_bCapOn) s_nCapLost += lost;
				}
			}
			if (!m_bHaveIn && m_bCounterFromIn && !m_bOutCounterSet)
			{
				m_nOutCounter = c;
				m_bOutCounterSet = TRUE;
			}
			m_bHaveIn = TRUE;
			m_nExpectIn = c + ob::kFramesPerBlock;
			m_nInBlocks++;
			good++;
			if (IMirrorTarget *pT = Target ()) pT->FromBoxBlock (pBlk);	// UAC2 IN 1-12, or its mirror
			if (m_pAudio == &core::out::kRytmMk2)	// the Rytm's voice stems (IN 2..9), whichever slot it holds (a box's IN 2..9 are no stems)
			{
				const boolean bFx = sendfx_pi::On (m_nSlot), bMix = masterfx_pi::WantStems (m_nSlot);	// its send FX; SRC MIX onto 0/1 (core/fx/master.h)
				if (bFx || bMix)
				{
					s32 S[ob::kFramesPerBlock][8];
					for (int f = 0; f < ob::kFramesPerBlock; f++)
						for (int v = 0; v < 8; v++) S[f][v] = ob::block_sample (pBlk, ob::kChannels, f, 2 + v);
					if (bFx) sendfx_pi::FromRytm (m_nSlot, S, ob::kFramesPerBlock);
					if (bMix) masterfx_pi::Stems (m_nSlot, S, ob::kFramesPerBlock);
				}
			}
			if (loop_pi::WantsIn (m_nSlot))	// the box's own looper's ring: its MAIN L / R (IN 0 / 1 on every box: the Rytm, the A4 widened, the Digitakt)
			{
				int32_t LR[2 * ob::kFramesPerBlock];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					LR[2 * f] = ob::block_sample (pBlk, ob::kChannels, f, 0), LR[2 * f + 1] = ob::block_sample (pBlk, ob::kChannels, f, 1);
				loop_pi::InBlock (m_nSlot, LR, ob::kFramesPerBlock, nLostFrames, bridge::render_frames ((int) m_nSlot));
			}
			if (bridge::sidechain_on ((int) m_nSlot))	// SIDECHAIN (core/fx/sidechain.h): this box's stems and AUDIO IN into its slot's key bus
			{
				s32 K[ob::kFramesPerBlock][ob::kChannels];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int c = 0; c < ob::kChannels; c++) K[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
				bridge::sidechain_in ((int) m_nSlot, m_pAudio, &K[0][0], ob::kFramesPerBlock, ob::kChannels);
			}
			if (!Primary ())			// a box beside the primary stream: its audio is not the engine's (yet)
			{
				int32_t s = ob::block_sample (pBlk, ob::kChannels, 0, 0) >> 8;
				if (s < 0) s = -s;
				if (s > m_nInPeak) m_nInPeak = s;
				// While it is the side box (both attached), what is this box's takes its IN here, either box alike (decided
				// 2026-11-26): the recorder's box (rec_pi::Slot), the sampler's take's box, stereo=measure of this box, the
				// latency click of the Rytm here
				if (core::device::side_slot () != (int) m_nSlot) continue;
				const boolean bRec = rec_pi::Slot () == (int) m_nSlot && g_Rec.wants_in ();
				const boolean bSamp = bridge::samp_slot () == (int) m_nSlot && bridge::samp_wants_in ();
				const boolean bCal = bridge::cal_slot () == (int) m_nSlot && bridge::cal_on ();
				if (bRec || bSamp || bCal)
				{
					s32 I[ob::kFramesPerBlock][ob::kChannels];
					for (int f = 0; f < ob::kFramesPerBlock; f++)
						for (int c = 0; c < ob::kChannels; c++) I[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
					if (bRec) g_Rec.in_block (I, ob::kFramesPerBlock);
					if (bSamp) bridge::samp_in (&I[0][0], ob::kFramesPerBlock, ob::kChannels);
					if (bCal) bridge::cal_in (&I[0][0], ob::kFramesPerBlock, ob::kChannels);
				}
				if (core::out::meas::active () && core::out::meas::slot () == (int) m_nSlot)
					for (int f = 0; f < ob::kFramesPerBlock; f++)
						core::out::meas::in_frame (ob::block_sample (pBlk, ob::kChannels, f, 0), ob::block_sample (pBlk, ob::kChannels, f, 1));
				continue;
			}
			const boolean bRecHere = rec_pi::Slot () == core::out::g_base_slot;	// the recorder's box is the base box
			boolean bRecIn = bRecHere && g_Rec.wants_in ();
			boolean bSampIn = bridge::samp_wants_in () && (bridge::samp_slot () == core::out::g_base_slot || core::device::side_slot () != bridge::samp_slot ());
			if (bRecIn || bSampIn)		// the recorder: the selected channels into its ring (rec_pi.h); the sampler's (core/samp)
			{
				s32 I[ob::kFramesPerBlock][ob::kChannels];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int c = 0; c < ob::kChannels; c++) I[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
				if (bRecIn) g_Rec.in_block (I, ob::kFramesPerBlock);
				if (bSampIn) bridge::samp_in (&I[0][0], ob::kFramesPerBlock, ob::kChannels);
			}
			if (bridge::tap_on ())		// the remote UI's SCOPE / meters (core/ui/extra.h): compares and stores only
			{
				s32 T[ob::kFramesPerBlock][ob::kChannels];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int c = 0; c < ob::kChannels; c++) T[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
				bridge::tap_in (&T[0][0], ob::kFramesPerBlock, ob::kChannels);
			}
			if (bridge::gr_live_on ())	// GRANULAR's live sources (core/granular/granular.h): into its capture ring
			{
				s32 G[ob::kFramesPerBlock][ob::kChannels];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int c = 0; c < ob::kChannels; c++) G[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
				bridge::gr_live_in (&G[0][0], ob::kFramesPerBlock, ob::kChannels);
			}
			if (bridge::livein_on ())	// LIVE IN's AUDIO IN (core/livein/livein.h): into its ring, as it comes
			{
				s32 L[ob::kFramesPerBlock][ob::kChannels];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int c = 0; c < ob::kChannels; c++) L[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
				bridge::livein_in (&L[0][0], ob::kFramesPerBlock, ob::kChannels);
			}
			if (core::out::meas::active () && core::out::meas::slot () == core::out::g_base_slot)	// stereo=measure: MAIN L/R read back (core/fx/out_measure.h), the base box's
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					core::out::meas::in_frame (ob::block_sample (pBlk, ob::kChannels, f, 0), ob::block_sample (pBlk, ob::kChannels, f, 1));
			if (bridge::cal_on () && (bridge::cal_slot () == core::out::g_base_slot || core::device::side_slot () != bridge::cal_slot ()))	// the sequencer's latency click on its way back (core/seq/seq.h cal_audio): the base box's, unless the side box's stream takes it
			{
				s32 C[ob::kFramesPerBlock][ob::kChannels];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int c = 0; c < ob::kChannels; c++) C[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
				bridge::cal_in (&C[0][0], ob::kFramesPerBlock, ob::kChannels);
			}
			int32_t s = ob::block_sample (pBlk, ob::kChannels, 0, 0) >> 8;
			if (s < 0) s = -s;
			if (s > m_nInPeak) m_nInPeak = s;

			// capture: MAIN L/R (Rytm -> host channels 0, 1). The samples are int32 full
			// Stored raw (int32): the Rytm's on-wire scale is checked from this capture.
			if (s_bCapOn)
			{
				unsigned pos = s_nCapPos;
				s32 *pOut = s_pCapBuf + pos * CaptureChannels;
				for (int f = 0; f < ob::kFramesPerBlock && pos < CaptureFrames; f++, pos++)
				{
					pOut[0] = ob::block_sample (pBlk, ob::kChannels, f, 0);
					pOut[1] = ob::block_sample (pBlk, ob::kChannels, f, 1);
					if (s_nCapVoiceCh < (unsigned) ob::kChannels)
					{
						pOut[3] = ob::block_sample (pBlk, ob::kChannels, f, s_nCapVoiceCh);
					}
					pOut += CaptureChannels;
				}
				s_nCapPos = pos;
				if (pos >= CaptureFrames)
				{
					s_bCapOn = FALSE;
					s_bCapFull = TRUE;
				}
			}
		}
	}

	if (m_bStreaming && !m_InHalt.halted) SubmitIn (i);

	if (m_OutMode == OutPaced && m_nInBlocks > m_nOutDelay)
	{
		m_nCredit += good;
		if (good != 0)				// oboutlead / obrelaylead: N blocks ahead of the one-for-one, once a stream
		{					// (or OUT reset), topped up if the box's route now wants more
			if (m_bLeadDue) m_nLeadGiven = 0, m_bLeadDue = FALSE;
			IMirrorTarget *pT = Target ();
			const unsigned nLead = pT != 0 && pT != &s_Gadget ? m_nRelayLead : m_nOutLead;
			if (nLead > m_nLeadGiven)
			{
				m_nCredit += nLead - m_nLeadGiven;
				m_nLeadGiven = nLead;
			}
		}
		PumpOut ();
	}
}

void CObUsb::OutDone (unsigned i, CUSBRequest *pURB)
{
	unsigned now = CTimer::GetClockTicks ();
	if (m_nFirstOutUs != 0 && now - m_nLastOutUs > m_nOutMaxDt) m_nOutMaxDt = now - m_nLastOutUs;
	m_nLastOutUs = now;
	if (m_nFirstOutUs == 0) m_nFirstOutUs = now - m_nStartUs + 1;

	m_bOutBusy[i] = FALSE;
	m_nOutPending--;
	if (m_nOutPending < m_nOutPendMin) m_nOutPendMin = m_nOutPending;	// before this completion's refill

	if (pURB->GetStatus () != 0)
	{
		m_OutHalt.Ok ();
		m_nOutBlocks += pURB->GetResultLength () / m_nOutBlk;
		if (m_nOut16Us == 0 && m_nOutBlocks >= 16) m_nOut16Us = now - m_nStartUs + 1;
	}
	else
	{
		m_nOutErrors++;
		m_OutHalt.Fail (now);		// the endpoint halts on an error: no resubmit until PollHalt reset it
		return;
	}

	if (!m_bStreaming) return;
	if (m_OutMode == OutFree)
	{
		FillOut (m_pOutBuf[i], m_nBlocks);
		SubmitOut (i);
	}
	else if (m_OutMode == OutPaced)
	{
		PumpOut ();
	}
}

// Paced: as many OUT blocks as IN blocks came, m_nBlocks per URB, at most m_nURBs in flight.
void CObUsb::PumpOut (void)
{
	const unsigned cap = 2 * m_nURBs * m_nBlocks;
	if (m_nCredit > cap)
	{
		m_nCreditDrops += m_nCredit - cap;
		m_nCredit = cap;
	}
	while (m_bStreaming && !m_OutHalt.halted && m_nCredit >= m_nBlocks)
	{
		unsigned i = 0;
		while (i < m_nURBs && m_bOutBusy[i]) i++;
		if (i == m_nURBs) break;

		u16 nCounter = m_nOutCounter;
		FillOut (m_pOutBuf[i], m_nBlocks);
		if (!SubmitOut (i))
		{
			m_nOutCounter = nCounter;		// as if not sent
			break;
		}
		m_nCredit -= m_nBlocks;
	}
}

void CObUsb::LogAllStats (CLogger *pLogger)
{
	CObFollow::LogStats (pLogger);
	for (unsigned k = 0; k < Slots; k++)
		if (s_pBox[k] != 0 && s_pBox[k]->m_bStreaming) s_pBox[k]->Log (pLogger);
}

void CObUsb::Log (CLogger *pLogger)
{
	// snapshot with the xHCI IRQ held off; resubmit IN URBs a failed submit left idle
	EnterCritical (IRQ_LEVEL);
	unsigned now = CTimer::GetClockTicks ();
	unsigned nRequeued = 0;
	for (unsigned i = 0; i < m_nURBs; i++)
	{
		if (!m_InHalt.halted && !m_bInBusy[i] && SubmitIn (i)) nRequeued++;
	}
	if (m_OutMode == OutPaced) PumpOut ();
	else if (m_OutMode == OutFree)
	{
		for (unsigned i = 0; i < m_nURBs; i++)
		{
			if (!m_OutHalt.halted && !m_bOutBusy[i])
			{
				FillOut (m_pOutBuf[i], m_nBlocks);
				if (SubmitOut (i)) nRequeued++;
			}
		}
	}

	unsigned nIn = m_nInBlocks, nOut = m_nOutBlocks;
	unsigned nInMaxDt = m_nInMaxDt;
	int nPeak = m_nInPeak;
	m_nInMaxDt = 0;
	m_nInPeak = 0;
	unsigned nOutAge = (now - m_nLastOutUs) / 1000;
	unsigned nInPend = m_nInPending, nOutPend = m_nOutPending, nCredit = m_nCredit;
	unsigned nOutMaxDt = m_nOutMaxDt, nOutPendMin = m_nOutPendMin == ~0u ? nOutPend : m_nOutPendMin;
	m_nOutMaxDt = 0;
	m_nOutPendMin = ~0u;
	TEvStats ei = s_EvIn, eo = s_EvOut;
	unsigned nQIn = m_pIn->GetXHCIEndpoint ()->GetPendingURBs ();
	unsigned nQOut = m_pOut->GetXHCIEndpoint ()->GetPendingURBs ();
	LeaveCritical ();

	unsigned nStIn = m_pIn->GetXHCIEndpoint ()->GetEPState ();
	unsigned nStOut = m_pOut->GetXHCIEndpoint ()->GetEPState ();

	m_nSeconds++;
	unsigned nMs = (now - m_nLastLogUs) / 1000;		// measured, not assumed 1000
	if (nMs == 0) nMs = 1;
	m_nLastLogUs = now;
	if (!m_bSaidStart && m_nSeconds >= 2)
	{
		m_bSaidStart = TRUE;
		char Iv1[96];
		unsigned nIv1Busy = 0, nIv1 = CXHCIEndpoint::GetPeriodicIv1 (&nIv1Busy, Iv1, sizeof Iv1);
		pLogger->Write (m_nSlot ? "rytmob2" : From, LogNotice, "start: first IN done at %u us, first OUT done at %u us, "
				"16th OUT block at %u us (0 = never); periodic iv1 %u, %u busy: %s", m_nFirstInUs, m_nFirstOutUs,
				m_nOut16Us, nIv1, nIv1Busy, Iv1);
	}

	// one line: IN rate/total, gaps (blocks lost, largest), longest IN gap in time, peak;
	// OUT rate/total, errors, endpoint resets (PollHalt), ms since the last OUT completion, credit; URBs pending
	// (driver/xHCI FIFO), endpoint states (1 running, 2 halted, 4 error); xHCI events per
	// endpoint: success/short/missed-service/other(last code)/orphan
	pLogger->Write (m_nSlot ? "rytmob2" : From, LogNotice,	// (rytmob2: the box of slot 1; the xHCI event counts are both boxes')
			"%ums IN %u/s %u gap %u lost %u max %u odd %u sh %u bad %u err %u rs %u dt %uus pk %d"
			" | OUT %u/s %u err %u rs %u age %ums cr %u drop %u odt %uus pmin %u"
			" | pend %u/%u q %u/%u ep %u/%u rq %u sf %u/%u"
			" | evI %u ok %u sh %u ms %u x %u(%u) o %u evO %u ok %u ms %u x %u(%u) o %u",
			(now - m_nStartUs) / 1000, (nIn - m_nPrevIn) * 1000 / nMs, nIn, (unsigned) m_nInGaps, (unsigned) m_nInLost,
			(unsigned) m_nInMaxGap, (unsigned) m_nInOddGap, (unsigned) m_nInShort,
			(unsigned) m_nInBadMagic, (unsigned) m_nInErrors, m_InHalt.resets, nInMaxDt, nPeak,
			(nOut - m_nPrevOut) * 1000 / nMs, nOut, (unsigned) m_nOutErrors, m_OutHalt.resets, nOutAge, nCredit,
			(unsigned) m_nCreditDrops, nOutMaxDt, nOutPendMin,
			nInPend, nOutPend, nQIn, nQOut, nStIn, nStOut, nRequeued,
			(unsigned) m_nInSubmitFail, (unsigned) m_nOutSubmitFail,
			ei.nEvents, ei.nSuccess, ei.nShort, ei.nMissed, ei.nOther + ei.nRing, ei.nLastOther, ei.nOrphan,
			eo.nEvents, eo.nSuccess, eo.nMissed, eo.nOther + eo.nRing + eo.nShort, eo.nLastOther, eo.nOrphan);

	// the controller starving this box's OUT (periodic_starve.h): every OUT URB queued at the xHC and OUT still short of IN
	unsigned nInRate = (nIn - m_nPrevIn) * 1000 / nMs, nOutRate = (nOut - m_nPrevOut) * 1000 / nMs;
	boolean bAllQueued = nOutPend >= m_nURBs && nQOut >= m_nURBs;
	const PeriodicStarve::Say Say = m_OutMode == OutOff ? PeriodicStarve::None : m_Starve.Tick (nInRate, nOutRate, bAllQueued);
	switch (Say)
	{
	case PeriodicStarve::Start:
		{
			char Iv1[96];
			unsigned nIv1Busy = 0, nIv1 = CXHCIEndpoint::GetPeriodicIv1 (&nIv1Busy, Iv1, sizeof Iv1);
			pLogger->Write (m_nSlot ? "rytmob2" : From, LogWarning, "periodic starvation: OUT %u/s under IN %u/s for %u s "
					"with all %u OUT URBs at the controller (k %u services a ms); periodic iv1 %u, %u busy: %s "
					"(the VL805 has starved one at 6 busy)", nOutRate, nInRate, m_Starve.run, m_nURBs,
					PeriodicStarve::Steps (nOutRate, XHCI_CONFIG_TRANSFER_RING_SIZE, m_nBlocks), nIv1, nIv1Busy, Iv1);
		}
		break;
	case PeriodicStarve::Over:
		pLogger->Write (m_nSlot ? "rytmob2" : From, LogNotice, "periodic starvation over after %u s: OUT %u/s, IN %u/s "
				"(%u times this stream)", m_Starve.lasted, nOutRate, nInRate, m_Starve.events);
		break;
	default:
		break;
	}

	// the attach gate (attach_gate.h): a held box shown, restarted or re-enumerated; a box not held whose OUT starves
	// restarted (PollGate acts in the main loop)
	if (m_nGateAct == obgate::Wait && m_nSlot < 2)
	{
		if (s_nGateTest == -2) s_nGateTest = (int) CKernelOptions::Get ()->GetAppOptionDecimal ("obgatetest", (unsigned) -1);
		obgate::Gate &g = s_Gate[m_nSlot];
		if (m_bShowDue && g.held)
		{
			const boolean bFake = s_nGateTest == (int) m_nSlot && !s_bGateTestDone[m_nSlot];
			const boolean bReEnum = CKernelOptions::Get ()->GetAppOptionDecimal ("obgatereenum", 1) != 0;
			const obgate::Act a = obgate::tick (g, nInRate, bFake ? 0 : nOutRate, m_OutMode != OutOff, NowMs (),
							   GateHoldMs (), bReEnum);
			if (a != obgate::Wait)
			{
				pLogger->Write (m_nSlot ? "rytmob2" : From, a == obgate::Show ? LogNotice : LogWarning, "gate: %s (IN %u/s, "
						"OUT %u/s%s, held %u ms)", obgate::act_name (a), nInRate, nOutRate,
						bFake ? " read as 0: obgatetest" : "", NowMs () - g.since_ms);
				m_nGateAct = a;
			}
		}
		else if (Say == PeriodicStarve::Start && obgate::starved (m_nStarveRestarts))
			m_nGateAct = obgate::Restart;
	}

	m_nPrevIn = nIn;
	m_nPrevOut = nOut;
}

// Before a reboot (task level, core 0): no more resubmits, the URBs in flight complete (IN and OUT, a few
// microframes), then SET_INTERFACE alt 0 on interfaces 1 and 2, so the Rytm's stream engine is off when the next
// host comes (spike3: a Pi that died mid-stream left the Rytm streaming silence and NAKing OUT until it was
// power-cycled).
void CObUsb::Stop (void)
{
	CObFollow::Stop ();
	for (unsigned k = 0; k < Slots; k++)
	{
	CObUsb *p = s_pBox[k];
	if (p == 0) continue;
	p->m_bStreaming = FALSE;
	unsigned t0 = CTimer::GetClockTicks ();
	while ((p->m_nInPending || p->m_nOutPending) && CTimer::GetClockTicks () - t0 < 50000) {}
	unsigned nIn = p->m_nInPending, nOut = p->m_nOutPending;
	CUSBHostController *pHost = p->GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		pHost->ControlMessage (p->GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 0, nIface, 0, 0);
	}
	CLogger::Get ()->Write (From, LogNotice, "%s: stream stopped cleanly: alt 0 (URBs still pending in %u out %u)", p->m_pAudio->name, nIn, nOut);
	}
}

// From an exception on core 0 (guard.cpp): only the flag - no more resubmits; nothing that waits or allocates.
void CObUsb::EmergencyStop (void)
{
	CObFollow::EmergencyStop ();
	for (unsigned k = 0; k < Slots; k++)
		if (s_pBox[k] != 0) s_pBox[k]->m_bStreaming = FALSE;
}

void CObUsbTransport::SetMIDI (CUSBMIDIDevice *pMIDI)
{
	if (pMIDI == m_pMIDI) return;
	CLogger::Get ()->Write (From, LogNotice, "to-host: the Rytm's USB MIDI %s%s", pMIDI != 0 ? "up: sending" : "gone",
				m_Merge.failed () ? " (sending was off after a failed send)" : "");
	m_Merge.rytm_new ();				// a new umidi1 (or none): sending again when there is one
	m_pMIDI = pMIDI;
}

// usbmidi_merge.h's Io on Circle: the endpoint (Circle's bulk Transfer blocks core 0's loop until the Rytm takes it,
// 3 s at most, then it fails - the merge then turns the sending off until umidi1 comes again), the keys' queue, the
// Sidecar gadget and g_ToHost
namespace {
struct TCircleIo : UsbMidiMerge::Io
{
	CUSBMIDIDevice *pMIDI;
	CSidecarGadget *pSidecar;
	bool bFailed = false;				// a send failed in this Poll, first at nFailUs (EpBox: the MIDI OUT's error time)
	unsigned nFailUs = 0;
	unsigned nSendUs = 0;				// how long the last send () blocked (lagdiag.h TxDiag: the Rytm taking its input)
	bool send (const u8 *p, unsigned n) override
	{
		unsigned t0 = CTimer::GetClockTicks ();
		bool ok = pMIDI != 0 && pMIDI->SendEventPackets (p, n);
		nSendUs = CTimer::GetClockTicks () - t0;
		if (!ok && !bFailed) bFailed = true, nFailUs = CTimer::GetClockTicks ();
		return ok;
	}
	uint32_t last_send_us (void) override			{ return nSendUs; }
	unsigned engine_pending (void) override			{ return g_ToHost.Waiting (); }
	bool key (u32 *w) override				{ return g_KeysToRytm.pop (w); }
	// the host link's computer: USB-C's gadget, or the Teensy relay's when the relay shows the host link's box (any slot;
	// USB-C then shows the other box)
	static bool RelayHasRytm (void)				{ return CTeensyTarget::ShowsRytm (); }
	bool has_computer (void) override			{ return pSidecar != 0 || RelayHasRytm (); }
	void computer_connected (bool bUp) override
	{
		if (RelayHasRytm ()) CTeensyTarget::RytmMidiConnected (bUp);
		else if (pSidecar != 0) pSidecar->RytmMIDIConnected (bUp);
	}
	bool computer (u32 *w) override
	{
		if (RelayHasRytm ()) return CTeensyTarget::ForRytm (w);
		return pSidecar != 0 && !(pSidecar->Mirror () && !CObUsb::UsbcShowsHostLink ()) && pSidecar->ForRytm (w);
	}
	bool computer_in_sysex (void) override
	{
		if (RelayHasRytm ()) return CTeensyTarget::ForRytmInSysEx ();
		return pSidecar != 0 && pSidecar->ForRytmInSysEx ();
	}
	const u8 *engine_next (int *pLen) override		{ return g_ToHost.Next (pLen); }
	unsigned engine_drain (void) override			{ return g_ToHost.Drain (); }
	void warn (const char *p) override			{ CLogger::Get ()->Write (From, LogWarning, "%s", p); }
};
}

// The keys, the computer's MIDI, the engine's records and the Rytm MIDI bridge's, in that order (usbmidi_merge.h)
void CObUsbTransport::Poll (void)
{
	TCircleIo Io;
	Io.pMIDI = m_pMIDI;
	Io.pSidecar = CSidecarGadget::Get ();
	unsigned nNow = CTimer::GetClockTicks ();
	m_Merge.poll (Io, m_pMIDI != 0, m_bSend, midibridge_pi::Queue (), midibridge_pi::Pace (), nNow);
	unsigned nFail = m_Merge.failures () + m_Merge.mac_failed () + m_Merge.bridge_failed ();
	if (m_Merge.msgs () != m_nMsgsSeen) m_nMsgsSeen = m_Merge.msgs (), m_nMidiInRow = 0;	// a good send: the run ends
	// a send failed (its message dropped, counted): a halted endpoint is reset, one reset a main-loop pass in all
	// (CObUsb::PollHalt's first); not halted (a Rytm only slow): the merge's back-off stays
	// a failed send on a halted endpoint is the Rytm box's third endpoint (EpBox, ep_halt.h): its reset waits a moment
	// for another to fail, and is held with the Overbridge ones when they failed together (a likely unplug); the
	// failure stays unseen until then, so a later pass resets it. Only while the Rytm streams Overbridge (its EpBox): a
	// Rytm on USB MIDI alone has no box, and its MIDI OUT is reset at once, as before (one endpoint: never together)
	boolean bHalted = nFail != m_nFailSeen && m_pMIDI != 0 && m_pMIDI->OutHalted ();
	if (nFail != m_nFailSeen) CObUsb::MidiHalted (bHalted, Io.bFailed ? Io.nFailUs : nNow);	// (the send's time)
	// usb=mirror: a forwarded request on the box's EP0 (the Rytm's MIDI is the box's own interfaces): the reset's synchronous
	// CLEAR_FEATURE would queue behind it, and a request the box never answers would leave it on the ring after Circle's
	// 3 s timeout, to complete later into a dead stack URB (xHCI IRQ). The failure stays unseen: a later Poll resets it.
	boolean bWait = bHalted && (CObUsb::BoxWaits (nNow) || CObUsb::MirrorCtlBusy ());
	if (bHalted && !CObUsb::ResetDone () && !bWait)
	{
		m_nFailSeen = nFail;
		m_nMidiInRow++;
		if (m_pMIDI->ResetOutHalted ())
		{
			CObUsb::MidiHalted (FALSE, nNow);
			m_nMidiResets++;
			// sending again from the next Poll, unless it keeps failing: from the third reset in a row with no
			// good send between, the merge's back-off (5 s, doubling to 60 s) paces it
			if (m_nMidiInRow < 3) m_Merge.rytm_new ();
			if (m_nMidiInRow <= 3)
				CLogger::Get ()->Write (From, LogWarning, "to-host: the Rytm's USB MIDI OUT endpoint was halted by a "
							"transfer error: reset %u (%u in a row), %s", m_nMidiResets, m_nMidiInRow,
							m_nMidiInRow < 3 ? "sending again" : "sending after the back-off");
		}
		else
		{
			m_nMidiRefused++;
			if (m_nMidiInRow <= 3)
				CLogger::Get ()->Write (From, LogWarning, "to-host: the Rytm's USB MIDI OUT endpoint is halted: the "
							"reset was refused (%u in a row), tried at the next failed send", m_nMidiInRow);
		}
	}
	else if (nFail != m_nFailSeen && !CObUsb::ResetDone () && !bWait) m_nFailSeen = nFail;	// not halted: nothing to reset
	if (m_Merge.mac_failed () != m_nMacFailedSeen)	// the computer's packets in that send are lost (usbmidi=os: aborts an upgrade)
	{
		m_nMacFailedSeen = m_Merge.mac_failed ();
		if (TCircleIo::RelayHasRytm ()) CTeensyTarget::RytmSendFailed ();	// the relay's computer sent them
		else if (Io.pSidecar != 0) Io.pSidecar->ComputerSendFailed ();
	}
	midibridge_pi::SetRytmFail (m_Merge.bridge_failed ());
	boolean bSending = m_pMIDI != 0 && m_bSend && !m_Merge.failed ();
	g_bRytmOff = !bSending;
	if (m_Resync.tick (m_Merge.dropped () + g_nToHostFull + bridge::to_host_dropped (), bSending, nNow))
		g_bRytmResync = true;
}
