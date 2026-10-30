//
// ob_usb.cpp - Rytm MKII Overbridge host for Circle (Raspberry Pi 4, xHCI/VL805).
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
//   * ResetFromHalted() exists in CXHCIEndpoint (no CLEAR_FEATURE(HALT) recovery here yet; the
//     endpoint state is printed so a halt shows).
//
#include "ob_usb.h"
#include "usbmidi_pack.h"
#include "sidecar_gadget.h"
#include "midibridge_pi.h"
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usb.h>
#include <circle/usb/xhciendpoint.h>
#include <circle/koptions.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/new.h>
#include <assert.h>

#include "ob_block.h"
#include "sendfx_pi.h"
#include "rec_pi.h"
#include "bridge.h"
#include "fx/out_measure.h"		// stereo=measure's read-back (MAIN L/R)
#include "shared.h"

static const char From[] = "rytmob";

static const unsigned kBlk = ob::block_bytes (ob::kChannels);			// 368
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

// The hook the patched CUSBDeviceFactory calls for "ven1935-b48" (weak reference there).
CUSBFunction *RytmObDeviceCreate (CUSBFunction *pParent)
{
	return new CObUsb (pParent);
}

// The hook the patched CXHCIEndpoint::TransferEvent calls for every transfer event (weak there).
// Only the Rytm is on the bus in this spike, so the endpoint ID alone picks 0x83 / 0x03.
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

CObUsb::CObUsb (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
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

	s_pThis = this;
}

CObUsb::~CObUsb (void)
{
	s_pThis = 0;
	m_bStreaming = FALSE;
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
			if (alt == ob::kAlt && intr && (e->wMaxPacketSize & 0x7FF) == kBlk)
			{
				if (iface == ob::kInIface && e->bEndpointAddress == ob::kInEp) m_pInDesc = e;
				if (iface == ob::kOutIface && e->bEndpointAddress == ob::kOutEp) m_pOutDesc = e;
			}
		}
		p += p[0];
	}

	return m_pInDesc != 0 && m_pOutDesc != 0;
}

boolean CObUsb::Configure (void)
{
	AllocBufs ();

	CLogger::Get ()->Write (From, LogNotice, "Analog Rytm MKII (1935:0b48) claimed");

	if (!ScanDescriptors ())
	{
		CLogger::Get ()->Write (From, LogError, "no interrupt endpoints 0x83/0x03 (368 B) at alt 3: "
					"not in OVERBRIDGE mode?");
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

	const unsigned nLen = m_nBlocks * kBlk;
	for (unsigned i = 0; i < m_nURBs; i++)
	{
		m_pInURB[i] = new CUSBRequest (m_pIn, s_InBuf[i], nLen);
		m_pInURB[i]->SetCompletionRoutine (InStub, (void *) (uintptr) i, this);
		m_pOutURB[i] = new CUSBRequest (m_pOut, s_OutBuf[i], nLen);
		m_pOutURB[i]->SetCompletionRoutine (OutStub, (void *) (uintptr) i, this);
	}

	static const char *Modes[] = {"paced by IN", "free-running", "off"};
	CLogger::Get ()->Write (From, LogNotice, "alt 3 selected: %u URBs x %u blocks (%u B) per endpoint, "
				"OUT %s (delay %u blk), OUT counter from %s", m_nURBs, m_nBlocks, nLen,
				Modes[m_OutMode], m_nOutDelay, m_bCounterFromIn ? "IN" : "0");

	// the completions run in the xHCI IRQ: start everything with it held off, so the
	// TRB order on each ring is the submit order and the counters stay consistent
	if (s_bActive) g_Output.Reset ();	// fade in again on every new stream (not while the TCP link owns g_Output)
	EnterCritical (IRQ_LEVEL);
	m_nStartUs = m_nLastInUs = m_nLastOutUs = m_nLastLogUs = CTimer::GetClockTicks ();
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

// host -> device blocks: header (0x07FF, counter +7 per block) + 7 frames x 12 channels from the output stage
// (output.h: the voices mapped, faded in, limited; silence when its FIFO is empty). Capture channel 2: OUT MAIN L
// as sent. The control core is woken to refill the FIFO (sev).
void CObUsb::FillOut (u8 *pBuf, unsigned nBlocks)
{
	for (unsigned b = 0; b < nBlocks; b++)
	{
		u8 *pBlock = pBuf + b * kBlk;
		ob::write_header (pBlock, m_nOutCounter);
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
			u8 *fr = pBlock + ob::kHeaderBytes + 4 * ob::kChannels * f;
			for (int c = 0; c < ob::kChannels; c++) ob::put_be32 (fr + 4 * c, F[f][c]);
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
	asm volatile ("dsb sy; sev");
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
	if (!ok) m_nInErrors++;
	else
	{
		if (len % kBlk != 0) m_nInShort++;
		for (unsigned b = 0; b < len / kBlk; b++)
		{
			const u8 *pBlk = pBuf + b * kBlk;
			if (!ob::block_from_device (pBlk)) { m_nInBadMagic++; continue; }

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

	if (m_bStreaming) SubmitIn (i);

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
		m_nOutBlocks += pURB->GetResultLength () / kBlk;
		if (m_nOut16Us == 0 && m_nOutBlocks >= 16) m_nOut16Us = now - m_nStartUs + 1;
	}
	else
	{
		m_nOutErrors++;
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
	while (m_bStreaming && m_nCredit >= m_nBlocks)
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
		if (!m_bInBusy[i] && SubmitIn (i)) nRequeued++;
	}
	if (m_OutMode == OutPaced) PumpOut ();
	else if (m_OutMode == OutFree)
	{
		for (unsigned i = 0; i < m_nURBs; i++)
		{
			if (!m_bOutBusy[i])
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
	// OUT rate/total, errors, ms since the last OUT completion, credit; URBs pending
	// (driver/xHCI FIFO), endpoint states (1 running, 2 halted, 4 error); xHCI events per
	// endpoint: success/short/missed-service/other(last code)/orphan
	pLogger->Write (From, LogNotice,
			"%ums IN %u/s %u gap %u lost %u max %u odd %u sh %u bad %u err %u dt %uus pk %d"
			" | OUT %u/s %u err %u age %ums cr %u drop %u"
			" | pend %u/%u q %u/%u ep %u/%u rq %u sf %u/%u"
			" | evI %u ok %u sh %u ms %u x %u(%u) o %u evO %u ok %u ms %u x %u(%u) o %u",
			(now - m_nStartUs) / 1000, (nIn - m_nPrevIn) * 1000 / nMs, nIn, (unsigned) m_nInGaps, (unsigned) m_nInLost,
			(unsigned) m_nInMaxGap, (unsigned) m_nInOddGap, (unsigned) m_nInShort,
			(unsigned) m_nInBadMagic, (unsigned) m_nInErrors, nInMaxDt, nPeak,
			(nOut - m_nPrevOut) * 1000 / nMs, nOut, (unsigned) m_nOutErrors, nOutAge, nCredit,
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
	bool send (const u8 *p, unsigned n) override		{ return pMIDI != 0 && pMIDI->SendEventPackets (p, n); }
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
	m_Merge.poll (Io, m_pMIDI != 0, m_bSend, midibridge_pi::Queue (), midibridge_pi::Pace (), CTimer::GetClockTicks ());
	midibridge_pi::SetRytmFail (m_Merge.bridge_failed ());
}
