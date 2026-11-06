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
#include "midibridge_pi.h"
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbhcirootport.h>
#include <circle/usb/usbstandardhub.h>
#include <circle/usb/usb.h>
#include <circle/usb/xhciendpoint.h>
#include <circle/koptions.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/new.h>
#include <assert.h>

#include "ob_block.h"
#include "ob_follow.h"
#include "ob_speed.h"
#include "sendfx_pi.h"
#include "masterfx_pi.h"		// the master FX chain
#include "rec_pi.h"
#include "bridge.h"
#include "device/device.h"
#include "fx/out_measure.h"		// stereo=measure's read-back (MAIN L/R)
#include "shared.h"

static const char From[] = "rytmob";

static const unsigned kBlk = ob::block_bytes (ob::kChannels);			// 368, the biggest block either way
static const unsigned kBufSize = (CObUsb::MaxBlocks * kBlk + 63) & ~63u;	// 2944

static_assert (CObUsb::MaxURBs <= XHCI_EP_MAX_URBS, "Circle's per-endpoint URB FIFO");

// xHCI endpoint IDs (DCI): 2 * number + IN
static const unsigned kInEpID = (ob::kInEp & 0x0F) * 2 + 1;		// 7
static const unsigned kOutEpID = (ob::kOutEp & 0x0F) * 2;		// 6

// Circle's xHCI wants transfer buffers above MEM_KERNEL_END (asserted in xhciendpoint.cpp),
// so not static arrays in the kernel image: heap blocks, 64 B aligned, allocated once.
static u8 *s_InBuf[CObUsb::MaxURBs];
static u8 *s_OutBuf[CObUsb::MaxURBs];

static u8 *AllocDmaBuf (void)
{
	u8 *p = new u8[kBufSize + 64];
	return (u8 *) (((uintptr) p + 63) & ~(uintptr) 63);
}

static void AllocBufs (void)
{
	if (s_InBuf[0] != 0)
	{
		return;
	}
	for (unsigned i = 0; i < CObUsb::MaxURBs; i++)
	{
		s_InBuf[i] = AllocDmaBuf ();
		s_OutBuf[i] = AllocDmaBuf ();
	}
}

static unsigned Clamp (unsigned v, unsigned lo, unsigned hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

CObUsb *CObUsb::s_pThis = 0;
unsigned CObUsb::s_nGen = 0, CObUsb::s_nPipeResets = 0;
static obspeed::Retry s_Retry;				// a box refused below high speed (ob_speed.h)
static CUSBHCIRootPort *s_pRetryPort = 0;		// its root port: Circle's, it lives as long as the controller
static CUSBStandardHub *s_pRetryHub = 0;		// or the Pi 4's VL805 hub (BuiltInHub), which lives as long too
static u32 NowMs (void) { return (u32) (CTimer::GetClockTicks64 () / (CLOCKHZ / 1000)); }	// wraps at 2^32 ms
volatile boolean CObUsb::s_bActive = TRUE;
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

CUSBFunction *RytmObDeviceCreate (CUSBFunction *pParent)
{
	CUSBFunction *pFollow = CObFollow::Create (pParent);		// a4=1: an A4's stream beside the Rytm's (ob_follow.h)
	if (pFollow != 0) return pFollow;
	const TUSBDeviceDescriptor *pDesc = pParent->GetDevice ()->GetDeviceDescriptor ();
	const core::device::Profile *p = core::device::by_usb (pDesc->idVendor, pDesc->idProduct);
	if (p == 0) return 0;
	const core::out::Device *pAudio = p->audio;
	boolean bRytm = pAudio == &core::out::kRytmMk2;
	if (bRytm && CObUsb::s_bActive) core::device::rytm_on_usb (true);	// a stock Rytm's way back to /.rytm (core/device/device.h)
	if (CObUsb::s_pThis != 0)					// one stream: a second box keeps its MIDI, Circle goes on to its interfaces
	{
		CLogger::Get ()->Write (From, LogWarning, "%s: a box already streams, no audio from this one", pAudio->name);
		return 0;
	}
	if ((pAudio->in_channels != ob::kChannels && pAudio->in_channels != 8) || pAudio->channels < 2 || pAudio->channels > ob::kChannels)
	{
		CLogger::Get ()->Write (From, LogError, "%s: %u channels in, %u out: not a stream this driver serves", pAudio->name,
					(unsigned) pAudio->in_channels, (unsigned) pAudio->channels);
		return 0;
	}
	return new CObUsb (pParent, pAudio);
}

void CObUsb::PollReEnum (void)
{
	if (!obspeed::due (s_Retry, NowMs ()) || (s_pRetryPort == 0 && s_pRetryHub == 0)) return;
	CLogger::Get ()->Write (From, LogNotice, "enumerating the box again (try %u of %u)", s_Retry.tries, obspeed::kTries);
	// the hot-plug path: port reset, address, configure (this driver again). A hub resets each of its empty ports
	// that has something connected; a root port with a device already on it only rescans that device
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
void CObUsb::PollHalt (void)
{
	s_bResetDone = FALSE;
	CObUsb *p = s_pThis;
	if (p != 0 && p->m_bStreaming)
	{
		unsigned now = CTimer::GetClockTicks ();
		p->BoxWait (now) || p->Recover (TRUE, now) || p->Recover (FALSE, now);
	}
	if (!s_bResetDone) s_bResetDone = CObFollow::PollHalt ();	// an A4's stream beside it (ob_follow.h)
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
	CObUsb *p = s_pThis;
	if (p == 0) return;
	if (bHalted && !p->m_bMidiErred) p->m_nMidiFailUs = nNowUs;
	p->m_bMidiErred = bHalted;
}

boolean CObUsb::BoxWaits (unsigned nNowUs)
{
	CObUsb *p = s_pThis;
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
	else if (m_OutMode == OutPaced) PumpOut ();
	else if (m_OutMode == OutFree)
	{
		for (unsigned i = 0; i < m_nURBs; i++) if (!m_bOutBusy[i]) FillOut (s_OutBuf[i], m_nBlocks), SubmitOut (i);
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

void CObUsb::SetActive (boolean bActive)
{
	s_bActive = bActive;
	core::out::set_device (bActive && s_pThis != 0 ? s_pThis->m_pAudio : 0);	// the TCP link's host is 12 channels, the Rytm's
}

CObUsb::CObUsb (CUSBFunction *pFunction, const core::out::Device *pAudio)
:	CUSBFunction (pFunction),
	m_pAudio (pAudio), m_nOutCh (pAudio->channels), m_nOutBlk (ob::block_bytes (pAudio->channels)),
	m_nInCh (pAudio->in_channels), m_nInBlk (ob::block_bytes (pAudio->in_channels)),
	m_pInDesc (0), m_pOutDesc (0), m_pIn (0), m_pOut (0),
	m_pInURB {}, m_pOutURB {}, m_bInBusy {}, m_bOutBusy {},
	m_nInPending (0), m_nOutPending (0), m_bStreaming (FALSE),
	m_nOutCounter (0), m_bOutCounterSet (FALSE), m_nExpectIn (0), m_bHaveIn (FALSE), m_nPhase (0),
	m_nCredit (0),
	m_nInBlocks (0), m_nInShort (0), m_nInBadMagic (0), m_nInGaps (0), m_nInLost (0), m_nInMaxGap (0),
	m_nInOddGap (0), m_nInErrors (0), m_nInSubmitFail (0),
	m_nOutBlocks (0), m_nOutErrors (0), m_nOutSubmitFail (0), m_nCreditDrops (0),
	m_nInPeak (0), m_nInMaxDt (0),
	m_nStartUs (0), m_nLastInUs (0), m_nLastOutUs (0), m_nFirstInUs (0), m_nFirstOutUs (0), m_nOut16Us (0), m_nLastLogUs (0),
	m_bSaidStart (FALSE), m_nPrevIn (0), m_nPrevOut (0), m_nSeconds (0)
{
	CKernelOptions *pOpt = CKernelOptions::Get ();
	m_nURBs = Clamp (pOpt->GetAppOptionDecimal ("oburbs", 8), 1, MaxURBs);
	m_nBlocks = Clamp (pOpt->GetAppOptionDecimal ("obblocks", 1), 1, MaxBlocks);
	m_nOutDelay = pOpt->GetAppOptionDecimal ("oboutdelay", 0);
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
	m_nGen = ++s_nGen;

	s_pThis = this;
}

CObUsb::~CObUsb (void)
{
	s_pThis = 0;
	m_bStreaming = FALSE;
	core::out::set_device (0);
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bMirror && CSidecarGadget::Get () != 0) CSidecarGadget::Get ()->MirrorBoxUp (FALSE);
	for (unsigned i = 0; i < m_nPipes; i++)		// a URB in flight is leaked, as below
	{
		if (!m_Pipe[i].bBusy) delete m_Pipe[i].pURB;
		delete m_Pipe[i].pEP;
	}
#endif
	if (m_pAudio == &core::out::kRytmMk2) core::device::rytm_on_usb (false);
	// URBs still queued on the (gone) endpoint are leaked, not freed under the controller
	for (unsigned i = 0; i < MaxURBs; i++)
	{
		if (!m_bInBusy[i]) delete m_pInURB[i];
		if (!m_bOutBusy[i]) delete m_pOutURB[i];
	}
	delete m_pOut;
	delete m_pIn;
	m_pOut = m_pIn = 0;
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
	AllocBufs ();

	const TUSBDeviceDescriptor *pDevDesc = GetDevice ()->GetDeviceDescriptor ();
	CLogger::Get ()->Write (From, LogNotice, "%s (%04x:%04x) claimed", m_pAudio->name, (unsigned) pDevDesc->idVendor,
				(unsigned) pDevDesc->idProduct);

	// Overbridge needs high speed. Below it the stream cannot start ("cannot submit the first URBs"), so refuse at
	// once and have the root port enumerate the box again (ob_speed.h): a new port reset, a new chance at high speed
	if (GetDevice ()->GetSpeed () != USBSpeedHigh)
	{
		static const char *Speeds[] = {"LS", "FS", "HS", "SS", "?"};
		const char *pSpeed = Speeds[GetDevice ()->GetSpeed () <= USBSpeedUnknown ? GetDevice ()->GetSpeed () : 4];
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
			s_pRetryPort = pPort, s_pRetryHub = pHub;
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
		m_pInURB[i] = new CUSBRequest (m_pIn, s_InBuf[i], nLen);
		m_pInURB[i]->SetCompletionRoutine (InStub, (void *) (uintptr) i, this);
		m_pOutURB[i] = new CUSBRequest (m_pOut, s_OutBuf[i], nOutLen);
		m_pOutURB[i]->SetCompletionRoutine (OutStub, (void *) (uintptr) i, this);
	}

#ifdef CIRCLE_GADGET_MIRROR
	if (CSidecarGadget::Get () != 0) { if (CSidecarGadget::Get ()->Mirror ()) MirrorAttach (); }
	else m_bAttachDue = TRUE;			// the box came up before the gadget (at boot): MirrorAttachLate
#endif

	static const char *Modes[] = {"paced by IN", "free-running", "off"};
	CLogger::Get ()->Write (From, LogNotice, "alt 3 selected: %u URBs x %u blocks (%u B) per endpoint, "
				"OUT %s (delay %u blk), OUT counter from %s", m_nURBs, m_nBlocks, nLen,
				Modes[m_OutMode], m_nOutDelay, m_bCounterFromIn ? "IN" : "0");

	if (m_nOutCh != ob::kChannels)
	{
		CLogger::Get ()->Write (From, LogNotice, "OUT %u channels (%u B a block), %s", m_nOutCh, m_nOutBlk,
					m_pAudio->ret == core::out::kStereoMix ? "the voices as one stereo mix" : "per voice");
	}

	if (s_bActive) core::out::set_device (m_pAudio);	// the voices' channels and routes follow the box; not while the TCP link owns g_Output

	// the completions run in the xHCI IRQ: start everything with it held off, so the
	// TRB order on each ring is the submit order and the counters stay consistent
	if (s_bActive) g_Output.Reset ();	// fade in again on every new stream (not while the TCP link owns g_Output)
	if (s_bActive) masterfx_pi::Reset ();
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
			FillOut (s_OutBuf[i], m_nBlocks);
			bOK = SubmitOut (i);
		}
	}
	LeaveCritical ();

	if (!bOK)
	{
		CLogger::Get ()->Write (From, LogError, "cannot submit the first URBs");
		return FALSE;
	}

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
	CObUsb *p = s_pThis;
	if (p == 0 || !p->m_bAttachDue || CSidecarGadget::Get () == 0) return;
	p->m_bAttachDue = FALSE;
	if (!CSidecarGadget::Get ()->Mirror ()) return;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: the box was up before the gadget or the mode: read now");
	p->MirrorAttach ();
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
	CObUsb *p = s_pThis;
	if (p != 0 && p->m_bStreaming && !p->m_bMirror) p->m_bAttachDue = TRUE;
}

// Configure (task level): the box's descriptors to the gadget, its string descriptors read once, its pipes opened
void CObUsb::MirrorAttach (void)
{
	CSidecarGadget *pGadget = CSidecarGadget::Get ();
	const TUSBDeviceDescriptor *pDev = GetDevice ()->GetDeviceDescriptor ();
	const TUSBConfigurationDescriptor *pCfg = GetDevice ()->GetConfigurationDescriptor ();
	if (pDev == 0 || pCfg == 0) return;
	const u8 *pC = (const u8 *) pCfg;
	unsigned nCfg = pCfg->wTotalLength;
	if (!pGadget->MirrorSetDescriptors ((const u8 *) pDev, pC, nCfg)) return;
	m_bMirror = TRUE;

	if (m_pCtlBuf == 0) m_pCtlBuf = AllocDmaBuf ();
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
	if (!mirror::parse (pC, nCfg, ob::kInEp, ob::kOutEp, &l))
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
		TPipe &p = m_Pipe[m_nPipes];
		p.uchAddr = e.addr;
		p.nMps = (pDesc->wMaxPacketSize & 0x7FF) > 512 ? 512 : pDesc->wMaxPacketSize & 0x7FF;
		p.pEP = new CUSBEndpoint (GetDevice (), pDesc);
		p.pBuf = AllocDmaBuf ();
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
	EnterCritical (IRQ_LEVEL);
	for (unsigned i = 0; i < m_nPipes; i++) if (m_Pipe[i].uchAddr & 0x80) SubmitPipeIn (i);
	LeaveCritical ();
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %u strings (language %04x), %u pipes", nGot, (unsigned) nLang, m_nPipes);
	pGadget->MirrorBoxUp (TRUE);
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
	if (pThis != s_pThis || ParamGen (pParam) != pThis->m_nGen) return;
	TPipe &p = pThis->m_Pipe[ParamIndex (pParam)];
	p.bBusy = FALSE;
	if (pURB->GetStatus () == 0) { p.nErrors++; p.bStopped = TRUE; return; }	// halted: MirrorPipePoll clears it and resubmits
	p.nTries = 0;
	unsigned n = pURB->GetResultLength ();
	InvalidateDataCacheRange ((uintptr) p.pBuf, kBufSize);
	CSidecarGadget *pGadget = CSidecarGadget::Get ();
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
	CObUsb *pThis = s_pThis;
	if (pThis == 0) return;
	for (unsigned i = 0; i < pThis->m_nPipes; i++)
		if (pThis->m_Pipe[i].uchAddr == uchAddr && !pThis->m_Pipe[i].bBusy && !pThis->m_Pipe[i].bStopped) pThis->SubmitPipeIn (i);
}

boolean CObUsb::MirrorPipeOut (u8 uchAddr, const u8 *pData, unsigned nLength)
{
	CObUsb *pThis = s_pThis;
	if (pThis == 0 || nLength > 512) return FALSE;
	for (unsigned i = 0; i < pThis->m_nPipes; i++)
	{
		TPipe &p = pThis->m_Pipe[i];
		if (p.uchAddr != uchAddr) continue;
		if (p.bBusy || p.bStopped) return FALSE;	// halted: dropped until MirrorPipePoll has cleared it
		if (nLength) memcpy (p.pBuf, pData, nLength);
		CleanAndInvalidateDataCacheRange ((uintptr) p.pBuf, kBufSize);
		CUSBRequest *pURB = new CUSBRequest (p.pEP, p.pBuf, nLength);	// its length is this packet's (IRQ-safe heap)
		pURB->SetCompletionRoutine (PipeOutStub, Param (pThis->m_nGen, i), pThis);
		p.bBusy = TRUE;
		if (pThis->GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
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
	if (pThis == s_pThis && ParamGen (pParam) == pThis->m_nGen)
	{
		TPipe &p = pThis->m_Pipe[ParamIndex (pParam)];
		p.bBusy = FALSE;
		if (pURB->GetStatus () == 0) { p.nErrors++; p.bStopped = TRUE; }	// halted: MirrorPipePoll clears it
		else { p.nPackets++; p.nTries = 0; }
		if (CSidecarGadget::Get () != 0) CSidecarGadget::Get ()->MirrorPipeOutDone (p.uchAddr);	// the computer's next
	}
	delete pURB;
}

// core 0's loop: one class / vendor request of the computer to the box, as it came (setup and data); the answer in the
// completion. FALSE: no box, or one still in flight.
boolean CObUsb::MirrorControl (const TSetupData *pSetup, const u8 *pOutData, unsigned nSeq)
{
	CObUsb *pThis = s_pThis;
	if (pThis == 0 || !pThis->m_bMirror || pThis->m_bCtlBusy || pSetup->wLength > 512) return FALSE;
	if ((pSetup->bmRequestType & REQUEST_IN) && pSetup->wLength == 0) return FALSE;	// no data stage to read into: STALL
	TSetupData *pS = new TSetupData;
	memcpy (pS, pSetup, sizeof *pS);
	if (!(pSetup->bmRequestType & REQUEST_IN) && pSetup->wLength) memcpy (pThis->m_pCtlBuf, pOutData, pSetup->wLength);
	CleanAndInvalidateDataCacheRange ((uintptr) pThis->m_pCtlBuf, kBufSize);
	CUSBRequest *pURB = new CUSBRequest (pThis->GetEndpoint0 (), pThis->m_pCtlBuf, pSetup->wLength, pS);
	pURB->SetCompletionRoutine (MirrorCtlStub, Param (pThis->m_nGen, nSeq), pThis);
	pThis->m_bCtlBusy = TRUE;
	if (pThis->GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
	pThis->m_bCtlBusy = FALSE;
	delete pURB;
	delete pS;
	return FALSE;
}

void CObUsb::MirrorCtlStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CObUsb *pThis = (CObUsb *) pContext;
	TSetupData *pS = pURB->GetSetupData ();
	if (pThis == s_pThis && ParamGen (pParam) == pThis->m_nGen)
	{
		pThis->m_bCtlBusy = FALSE;
		int n = pURB->GetStatus () ? (int) pURB->GetResultLength () : -1;
		InvalidateDataCacheRange ((uintptr) pThis->m_pCtlBuf, kBufSize);
		if (CSidecarGadget::Get () != 0) CSidecarGadget::Get ()->MirrorControlDone (ParamIndex (pParam), pThis->m_pCtlBuf, n);
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
	CObUsb *pThis = s_pThis;
	if (pThis == 0 || !pThis->m_bMirror) return;
	if (CSidecarGadget::Get () == 0 || !CSidecarGadget::Get ()->MirrorShown ()) return;	// the Sidecar device shown: no EP0 traffic
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
	CObUsb *p = s_pThis;
	return p != 0 && p->m_bMirror && p->m_bCtlBusy;
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

#endif

// host -> device blocks: header (0x07FF, counter +7 per block) + 7 frames x 12 channels from the output stage
// (output.h: the voices mapped, faded in, limited; silence when its FIFO is empty). Capture channel 2: OUT MAIN L
// as sent. The control core is woken to refill the FIFO (sev).
void CObUsb::FillOut (u8 *pBuf, unsigned nBlocks)
{
	for (unsigned b = 0; b < nBlocks; b++)
	{
		u8 *pBlock = pBuf + b * m_nOutBlk;
		ob::write_header (pBlock, m_nOutCounter);
#ifdef CIRCLE_GADGET_MIRROR
		if (CSidecarGadget::Get () != 0) CSidecarGadget::Get ()->MirrorOutExtra (pBlock, m_nOutCounter);	// usb=mirror
#endif
		m_nOutCounter += ob::kFramesPerBlock;

		s32 F[ob::kFramesPerBlock][ob::kChannels];
		s16 V[ob::kFramesPerBlock][ob::kVoices];
		s16 LR[ob::kFramesPerBlock][2 * ob::kVoices];		// the voices' L and R planes (core/fx/out.h: PRE / POST in stereo)
		float FD[ob::kFramesPerBlock][ob::kVoices];		// the PRE / POST voices' stand-ins for their silent stems
		float PR[ob::kFramesPerBlock][4];			// the PRE and POST pairs as sent (the recorder's 23 / 24)
		Output::Taps T;
		T.lr = LR, T.feed = FD, T.pairs = PR;
		if (s_bActive) g_Output.Pull (F, ob::kFramesPerBlock, V, &T);
		else
		{
			memset (F, 0, sizeof F);
			memset (V, 0, sizeof V);
			memset (LR, 0, sizeof LR);
			memset (FD, 0, sizeof FD);
			memset (PR, 0, sizeof PR);
		}
		if (CSidecarGadget::Get () != 0)		// the computer's OUT added (UAC2 OUT 1-12), the voices kept
			CSidecarGadget::Get ()->ToRytm (F, V, ob::kFramesPerBlock, FALSE);
		float X[ob::kFramesPerBlock][4] = {};		// each FX's own return (the recorder's FX1/FX2)
		if (s_bActive) sendfx_pi::FromVoices (V, ob::kFramesPerBlock, FD);	// fx-ins: the voices' sends to the FX buses; PRE / POST voices' stems
		if (s_bActive) sendfx_pi::AddReturn (F, ob::kFramesPerBlock, X);	// the send FX's return on 2/3 or 0/1
		if (s_bActive) CObFollow::Mix (F, ob::kFramesPerBlock);		// a4=1: the A4's MAIN on 2/3 (ob_follow.h)
		if (s_bActive) masterfx_pi::Process (F, ob::kFramesPerBlock);	// the master chain over all that (masterfx_pi.h)
		g_Play.mix (F, ob::kFramesPerBlock);		// a take's playback onto MAIN L / R (rec_play.h; idle: nothing)
		{
			u8 nSt = 0;					// the voices on PRE / POST: their take files stereo
			for (int v = 0; v < ob::kVoices; v++) nSt |= core::out::control.route[v] != core::out::kTrk ? 1u << v : 0;
			g_Rec.set_stereo_voices (nSt);
		}
		if (g_Rec.wants_side ()) g_Rec.out_block (V, X, ob::kFramesPerBlock, LR, PR);	// the voices as rendered, the FX, the pairs
		if (bridge::samp_wants_voices ()) bridge::samp_voices (V, ob::kFramesPerBlock, LR, X);	// the sampler's Sidecar voice (X: its FX, CHOP)
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
	if (pContext != s_pThis) return;		// a late event after unplug: the device is gone
	((CObUsb *) pContext)->InDone ((unsigned) (uintptr) pParam, pURB);
}

void CObUsb::OutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	if (pContext != s_pThis) return;
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

	u8 *pBuf = s_InBuf[i];
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
			if (m_bHaveIn && c != m_nExpectIn)
			{
				u16 d = (u16) (c - m_nExpectIn);
				m_nInGaps++;
				if (s_bCapOn) s_nCapGaps++;
				if (d % ob::kFramesPerBlock != 0) m_nInOddGap++;
				else
				{
					unsigned lost = d / ob::kFramesPerBlock;
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
			if (CSidecarGadget::Get () != 0) CSidecarGadget::Get ()->FromRytmBlock (pBlk);	// UAC2 IN 1-12
			boolean bRecIn = g_Rec.wants_in (), bSampIn = bridge::samp_wants_in ();
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
			if (core::out::meas::active ())	// stereo=measure: MAIN L/R read back (core/fx/out_measure.h)
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					core::out::meas::in_frame (ob::block_sample (pBlk, ob::kChannels, f, 0), ob::block_sample (pBlk, ob::kChannels, f, 1));
			if (bridge::cal_on ())		// the sequencer's latency click on its way back (core/seq/seq.h cal_audio)
			{
				s32 C[ob::kFramesPerBlock][ob::kChannels];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int c = 0; c < ob::kChannels; c++) C[f][c] = ob::block_sample (pBlk, ob::kChannels, f, c);
				bridge::cal_in (&C[0][0], ob::kFramesPerBlock, ob::kChannels);
			}
			if (sendfx_pi::On ())		// the voice stems (IN 2..9) into the send FX
			{
				s32 S[ob::kFramesPerBlock][8];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int v = 0; v < 8; v++) S[f][v] = ob::block_sample (pBlk, ob::kChannels, f, 2 + v);
				sendfx_pi::FromRytm (S, ob::kFramesPerBlock);
			}

			if (masterfx_pi::WantStems ())	// SRC MIX: the stems onto 0/1 (core/fx/master.h)
			{
				s32 S[ob::kFramesPerBlock][8];
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					for (int v = 0; v < 8; v++) S[f][v] = ob::block_sample (pBlk, ob::kChannels, f, 2 + v);
				masterfx_pi::Stems (S, ob::kFramesPerBlock);
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
		PumpOut ();
	}
}

void CObUsb::OutDone (unsigned i, CUSBRequest *pURB)
{
	unsigned now = CTimer::GetClockTicks ();
	m_nLastOutUs = now;
	if (m_nFirstOutUs == 0) m_nFirstOutUs = now - m_nStartUs + 1;

	m_bOutBusy[i] = FALSE;
	m_nOutPending--;

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
		FillOut (s_OutBuf[i], m_nBlocks);
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
		FillOut (s_OutBuf[i], m_nBlocks);
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
	CObUsb *p = s_pThis;
	if (p == 0 || !p->m_bStreaming) return;
	p->Log (pLogger);
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
				FillOut (s_OutBuf[i], m_nBlocks);
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
		pLogger->Write (From, LogNotice, "start: first IN done at %u us, first OUT done at %u us, "
				"16th OUT block at %u us (0 = never)", m_nFirstInUs, m_nFirstOutUs, m_nOut16Us);
	}

	// one line: IN rate/total, gaps (blocks lost, largest), longest IN gap in time, peak;
	// OUT rate/total, errors, endpoint resets (PollHalt), ms since the last OUT completion, credit; URBs pending
	// (driver/xHCI FIFO), endpoint states (1 running, 2 halted, 4 error); xHCI events per
	// endpoint: success/short/missed-service/other(last code)/orphan
	pLogger->Write (From, LogNotice,
			"%ums IN %u/s %u gap %u lost %u max %u odd %u sh %u bad %u err %u rs %u dt %uus pk %d"
			" | OUT %u/s %u err %u rs %u age %ums cr %u drop %u"
			" | pend %u/%u q %u/%u ep %u/%u rq %u sf %u/%u"
			" | evI %u ok %u sh %u ms %u x %u(%u) o %u evO %u ok %u ms %u x %u(%u) o %u",
			(now - m_nStartUs) / 1000, (nIn - m_nPrevIn) * 1000 / nMs, nIn, (unsigned) m_nInGaps, (unsigned) m_nInLost,
			(unsigned) m_nInMaxGap, (unsigned) m_nInOddGap, (unsigned) m_nInShort,
			(unsigned) m_nInBadMagic, (unsigned) m_nInErrors, m_InHalt.resets, nInMaxDt, nPeak,
			(nOut - m_nPrevOut) * 1000 / nMs, nOut, (unsigned) m_nOutErrors, m_OutHalt.resets, nOutAge, nCredit,
			(unsigned) m_nCreditDrops,
			nInPend, nOutPend, nQIn, nQOut, nStIn, nStOut, nRequeued,
			(unsigned) m_nInSubmitFail, (unsigned) m_nOutSubmitFail,
			ei.nEvents, ei.nSuccess, ei.nShort, ei.nMissed, ei.nOther + ei.nRing, ei.nLastOther, ei.nOrphan,
			eo.nEvents, eo.nSuccess, eo.nMissed, eo.nOther + eo.nRing + eo.nShort, eo.nLastOther, eo.nOrphan);

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
	CObUsb *p = s_pThis;
	if (p == 0) return;
	p->m_bStreaming = FALSE;
	unsigned t0 = CTimer::GetClockTicks ();
	while ((p->m_nInPending || p->m_nOutPending) && CTimer::GetClockTicks () - t0 < 50000) {}
	unsigned nIn = p->m_nInPending, nOut = p->m_nOutPending;
	CUSBHostController *pHost = p->GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		pHost->ControlMessage (p->GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 0, nIface, 0, 0);
	}
	CLogger::Get ()->Write (From, LogNotice, "stream stopped cleanly: alt 0 (URBs still pending in %u out %u)", nIn, nOut);
}

// From an exception on core 0 (guard.cpp): only the flag - no more resubmits; nothing that waits or allocates.
void CObUsb::EmergencyStop (void)
{
	CObFollow::EmergencyStop ();
	if (s_pThis != 0) s_pThis->m_bStreaming = FALSE;
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
	bool send (const u8 *p, unsigned n) override
	{
		bool ok = pMIDI != 0 && pMIDI->SendEventPackets (p, n);
		if (!ok && !bFailed) bFailed = true, nFailUs = CTimer::GetClockTicks ();
		return ok;
	}
	bool key (u32 *w) override				{ return g_KeysToRytm.pop (w); }
	bool has_computer (void) override			{ return pSidecar != 0; }
	void computer_connected (bool bUp) override		{ pSidecar->RytmMIDIConnected (bUp); }
	bool computer (u32 *w) override				{ return pSidecar->ForRytm (w); }
	bool computer_in_sysex (void) override			{ return pSidecar != 0 && pSidecar->ForRytmInSysEx (); }
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
		if (Io.pSidecar != 0) Io.pSidecar->ComputerSendFailed ();
	}
	midibridge_pi::SetRytmFail (m_Merge.bridge_failed ());
	boolean bSending = m_pMIDI != 0 && m_bSend && !m_Merge.failed ();
	g_bRytmOff = !bSending;
	if (m_Resync.tick (m_Merge.dropped () + g_nToHostFull + bridge::to_host_dropped (), bSending, nNow))
		g_bRytmResync = true;
}
