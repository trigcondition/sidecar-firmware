// pi/engine/teensy_target.cpp - see teensy_target.h, relay_proto.h and docs/teensy-mirror.md
#include "teensy_target.h"
#include <stddef.h>
#include "ob_usb.h"
#include "sidecar_gadget.h"
#include "link1.h"
#include "blackbox_pi.h"
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/koptions.h>
#include <circle/util.h>
#include <circle/string.h>
#include <stdarg.h>

static const char From[] = "teensy";

#ifdef CIRCLE_GADGET_MIRROR

using namespace relay;

CTeensyTarget *CTeensyTarget::s_pThis = 0;

// usbreboot=1: the reboot SysEx from the relay's computer reboots the Pi too, as on the USB-C port (sidecar_gadget.cpp).
// Fed from the xHCI IRQ (the computer's MIDI packets), taken by core 0's loop; static, so a Teensy that went and came
// back starts a fresh match. The request is a host-link SysEx, which m_ToBoxFilter drops: it never reaches the box.
static RebootSysEx s_Reboot;
static volatile boolean s_bRebootAsked;

boolean CTeensyTarget::TakeReboot (void)
{
	if (!s_bRebootAsked) return FALSE;
	s_bRebootAsked = FALSE;
	return TRUE;
}
volatile boolean CTeensyTarget::s_bBootDue = FALSE, CTeensyTarget::s_bLogDue = FALSE;
volatile boolean CTeensyTarget::s_bLeaving = FALSE;	// set with the bootloader request: the link is going, nothing more is sent on it
u64 CTeensyTarget::s_nLeavingAt = 0;			// when (clock ticks): the relay still here kLeaveSecs later clears it (MirrorPoll)
volatile unsigned CTeensyTarget::s_nLogReads = 0;
char CTeensyTarget::s_Log[2048];
unsigned CTeensyTarget::s_nLog = 0;

void CTeensyTarget::RequestBootloader (void)	{ s_bBootDue = TRUE; }
void CTeensyTarget::RequestLog (void)		{ s_nLog = 0; s_bLogDue = TRUE; }
unsigned CTeensyTarget::LogReads (void)		{ return s_nLogReads; }
unsigned CTeensyTarget::Log (char *pOut, unsigned nCap)
{
	unsigned n = s_nLog < nCap ? s_nLog : nCap;
	memcpy (pOut, s_Log, n);
	return n;
}

static const unsigned kDmaSize = 576;			// a link packet (512) and EP0's answers, 64-byte lines
static u8 *DmaBuf (void)
{
	u8 *p = new u8[kDmaSize + 64];
	return (u8 *) (((uintptr) p + 63) & ~(uintptr) 63);
}

static u64 NowTicks (void) { return CTimer::GetClockTicks64 (); }

// The hook the patched CUSBDeviceFactory calls for "ven1209-2" (circle-usbdevicefactory3.patch, weak reference there)
CUSBFunction *SidecarRelayCreate (CUSBFunction *pParent)
{
	if (CTeensyTarget::Get () != 0) return 0;		// one relay at a time
	return new CTeensyTarget (pParent);
}

CTeensyTarget::CTeensyTarget (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pEv (0), m_pAudioIn (0), m_pAudioOut (0), m_pCtlBuf (0), m_nVersion (0), m_pBox (0), m_nCfg (0), m_nStr (0),
	m_bHave (FALSE), m_bKeep (FALSE), m_bBoxUp (FALSE), m_bConnected (FALSE), m_bPcConfigured (FALSE), m_bUploadDue (FALSE),
	m_nBoxGoneAt (0), m_CtlState (CtlIdle), m_CtlSeq (0), m_nCtlAnswer (0), m_ToBoxFilter (true), m_ToPcFilter (false), m_bCutRytm (FALSE), m_bRytmMidi (FALSE), m_nLink1LostSeen (0), m_nToBoxFullSeen (0), m_nNoBoxSeen (0), m_nMidiInHead (0),
	m_nMidiInTail (0), m_nTakenDue (0), m_nLastLog (0), m_bTest (FALSE), m_nTestStart (0), m_nTestSent (0), m_nTestOutNext (0),
	m_nTestOutLost (0), m_nTestOutDup (0), m_nTestOutBad (0), m_nTestOut (0), m_nTestEcho (0), m_nTestEchoDrop (0), m_bTestOutSeen (FALSE), m_bTestFeeding (FALSE), m_bBoxPipes (FALSE), m_nInfoLines (0), m_nCompLast (0), m_nCompGapMax (0), m_nCompGap1 (0), m_nCompGap2 (0), m_nUnderSeen (0), m_nOverSeen (0), m_nEv (0), m_nPipeInZlp (0), m_nEvShownAt (0), m_nEvAtShow (0), m_nEvStep (0), m_nEvReshowTry (0), m_nEvSilent (0), m_nEvReshows (0), m_uchTestParked (0), m_nTestParkedAt (0), m_bFeedIrq (FALSE), m_bStarted (FALSE), m_bLeaveNow (FALSE), m_bDebug (FALSE), m_nDelay (0), m_nDebugVendor (0), m_nVariant (0), m_bCtlDone (FALSE), m_nCtlResult (0), m_nEpDesc (0)
{
	for (unsigned c = 0; c < kChannels; c++)
	{
		m_pChanIn[c] = m_pChanOut[c] = 0;
		m_pChanOutBuf[c] = 0;
		m_bChanOutBusy[c] = FALSE;
		m_nCreditDue[c] = 0;
		m_nAtBox[c] = 0;
		m_nMidiHeld[c] = 0;
		m_nMidiOut[c] = 0;
		for (unsigned k = 0; k < ChanInURBs; k++) { m_pChanInBuf[c][k] = 0; m_pChanInURB[c][k] = 0; }
	}
	for (unsigned k = 0; k < AudioOutBufs; k++) { m_pAudioOutBuf[k] = 0; m_bAudioOutBusy[k] = FALSE; m_nAudioOutAt[k] = 0; m_pAudioOutURB[k] = 0; m_nAudioOutLen[k] = 0; }
	for (unsigned k = 0; k < EvURBs; k++) m_pEvURB[k] = 0;
	for (unsigned k = 0; k < AudioInURBs; k++) m_pAudioInURB[k] = 0;
	memset (&m_Map, 0, sizeof m_Map);
	memset (&m_Stats, 0, sizeof m_Stats);
	m_Stream.init (CKernelOptions::Get ()->GetAppOptionDecimal ("mirrormargin", 8));
	m_nHoldSecs = CKernelOptions::Get ()->GetAppOptionDecimal ("mirrorhold", 10);
	// OS updates to the box over the relay's MIDI pass only with usbmidi=os:on (the gadget's default lets them pass for
	// the Rytm; here the box is the second one, so the safer default)
	const char *pMidi = CKernelOptions::Get ()->GetAppOptionString ("usbmidi", "");
	// the Rytm on the relay: the gadget's setting and gate (usbmidi=os:on|off|emu; emu never passes here: no TCP host)
	m_Gate.SetMode (usbmidi_os::from_cmdline (pMidi, CKernelOptions::Get ()->GetAppOptionString ("osupdate")));
	ApplyGate (m_ToRytmPath, m_Gate, FALSE);		// no IRQ yet
	m_BoxGate.SetMode (usbmidi_os::from_cmdline (pMidi, CKernelOptions::Get ()->GetAppOptionString ("osupdate")));
	ApplyBoxGate (FALSE);
	m_nLink1LostSeen = link1::ComputerLost ();
	m_bUsbReboot = CKernelOptions::Get ()->GetAppOptionDecimal ("usbreboot", 0) == 1;
	// mirrorname=off | TEXT (an underscore for a space; a space comes first): the default " (SC)", as the gadget's
	const char *pName = CKernelOptions::Get ()->GetAppOptionString ("mirrorname", 0);
	if (pName == 0) strcpy (m_Name, " (SC)");
	else if (strcmp (pName, "off") == 0) m_Name[0] = 0;
	else
	{
		unsigned n = 0;
		m_Name[n++] = ' ';
		for (; *pName && n < sizeof m_Name - 1; pName++) m_Name[n++] = *pName == '_' ? ' ' : *pName;
		m_Name[n] = 0;
	}
}

CTeensyTarget::~CTeensyTarget (void)
{
	if (s_pThis == this)
	{
		CObUsb::SetRelayTarget (0);
		link1::SetTap (0);				// (kernel.cpp's loop sets it again for USB-C's box, if that is the A4)
		s_pThis = 0;			// completions still on their way see it gone (they test s_pThis)
		CLogger::Get ()->Write (From, LogNotice, "the mirror relay went away: the box it showed is not shown");
	}
	delete m_pEv;
	delete m_pAudioIn;
	delete m_pAudioOut;
	for (unsigned c = 0; c < kChannels; c++) { delete m_pChanIn[c]; delete m_pChanOut[c]; }
	// the DMA buffers stay allocated: a URB Circle has not handed back may still point at them
}

boolean CTeensyTarget::Configure (void)
{
	const TUSBDeviceDescriptor *pDev = GetDevice ()->GetDeviceDescriptor ();
	m_nVersion = pDev->bcdDevice;
	if (GetDevice ()->GetSpeed () != USBSpeedHigh)
	{
		CLogger::Get ()->Write (From, LogError, "the mirror relay came up below high speed: not used (another cable or port?)");
		return FALSE;
	}
	if ((m_nVersion >> 8) != (kVersion >> 8))
	{
		CLogger::Get ()->Write (From, LogError, "the mirror relay speaks %u.%02u, this engine %u.%02u: not used",
					m_nVersion >> 8, m_nVersion & 0xFF, kVersion >> 8, kVersion & 0xFF);
		return FALSE;
	}
	const TUSBConfigurationDescriptor *pCfg = GetDevice ()->GetConfigurationDescriptor ();
	if (pCfg == 0) return FALSE;
	const u8 *p = (const u8 *) pCfg, *end = p + pCfg->wTotalLength;
	boolean bIface = FALSE;
	for (; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0])
	{
		if (p[1] == DESCRIPTOR_INTERFACE && p[0] >= 9)
			bIface = p[2] == 0 && p[3] == 0 && p[5] == kIfaceClass && p[6] == kIfaceSub && p[7] == kIfaceProto;
		else if (p[1] == DESCRIPTOR_ENDPOINT && p[0] >= 7 && bIface && m_nEpDesc < 16)
			m_pEpDesc[m_nEpDesc++] = (const TUSBEndpointDescriptor *) p;
	}
	m_bDebug = CKernelOptions::Get ()->GetAppOptionDecimal ("relaydebug", 0) != 0;
	m_nDelay = CKernelOptions::Get ()->GetAppOptionDecimal ("relaydelay", 0);
	m_nVariant = CKernelOptions::Get ()->GetAppOptionDecimal ("relayvariant", 1);
	m_bTest = CKernelOptions::Get ()->GetAppOptionDecimal ("relaytest", 0) != 0;
	{
		const char *pFeed = CKernelOptions::Get ()->GetAppOptionString ("relayfeed", "loop");
		m_bFeedIrq = m_bTest && strcmp (pFeed, "irq") == 0;
		m_bBoxPipes = m_bTest && CKernelOptions::Get ()->GetAppOptionDecimal ("relayboxpipes", 0) != 0;
		// the computer's audio for the box waits 2 ms in the OUT ring, not the gadget's 1 ms: through the Teensy both ways the
		// computer's blocks come with more jitter (T336: one underrun in 60 s at 48 frames). relayouttarget= frames
		unsigned nT = CKernelOptions::Get ()->GetAppOptionDecimal ("relayouttarget", 96);
		m_Stream.out_target = nT < 8 ? 8 : nT > mirror::Stream::kRing / 2 ? mirror::Stream::kRing / 2 : nT;
	}
	s_pThis = this;
	CLogger::Get ()->Write (From, LogNotice, "mirror relay %u.%02u claimed (%u endpoints): starts %u s after boot%s", m_nVersion >> 8,
				m_nVersion & 0xFF, m_nEpDesc, m_nDelay, m_bFeedIrq ? ", relaytest=1 relayfeed=irq" : m_bTest ? ", relaytest=1" : "");
	Step ("configured; registering as a mirror target");
	CObUsb::SetRelayTarget (this);		// the box USB-C does not show (ob_usb.cpp PollAssign); its MirrorPoll starts the relay (Start)
	Step ("registered");
	return TRUE;
}

void CTeensyTarget::Step (const char *pFormat, ...)
{
	if (!m_bDebug) return;
	va_list v;
	va_start (v, pFormat);
	CString s;
	s.FormatV (pFormat, v);
	va_end (v);
	CLogger::Get ()->Write (From, LogNotice, "relaydebug: %s", (const char *) s);
	blackbox_pi::Flush ();
}

boolean CTeensyTarget::Start (void)
{
	Step ("start: endpoints");
	s_bLeaving = FALSE;		// a new link (after a flash the relay enumerates again)
	for (unsigned i = 0; i < m_nEpDesc; i++)
	{
		const TUSBEndpointDescriptor *e = m_pEpDesc[i];
		u8 a = e->bEndpointAddress;
		CUSBEndpoint *pEP = new CUSBEndpoint (GetDevice (), e);
		if (a == kEvEp) m_pEv = pEP;
		else if (a == kAudioToPiEp) m_pAudioIn = pEP;
		else if (a == kAudioFromPiEp) m_pAudioOut = pEP;
		else if ((a & 0x7F) >= kChanFirstEp && (a & 0x7F) < kChanFirstEp + kChannels)
			((a & 0x80) ? m_pChanIn : m_pChanOut)[(a & 0x7F) - kChanFirstEp] = pEP;
		else delete pEP;
		Step ("endpoint 0x%02x made", a);
		if (a == kAudioToPiEp || a == kAudioFromPiEp || a == kEvEp)	// 1.01 makes the audio pair bulk (T406/T407), 1.02 the events (T411)
			CLogger::Get ()->Write (From, LogNotice, "link %s 0x%02x: %s, %u B", a == kEvEp ? "events" : "audio", a,
						(e->bmAttributes & 3) == 2 ? "bulk" : (e->bmAttributes & 3) == 3 ? "interrupt" : "other",
						(unsigned) e->wMaxPacketSize);
	}
	boolean bAll = m_pEv && m_pAudioIn && m_pAudioOut;
	for (unsigned c = 0; c < kChannels; c++) bAll = bAll && m_pChanIn[c] && m_pChanOut[c];
	if (!bAll)
	{
		CLogger::Get ()->Write (From, LogError, "the mirror relay's interface is not the link relay_proto.h describes");
		return FALSE;
	}
	Step ("start: buffers");
	m_pCtlBuf = DmaBuf ();
	for (unsigned k = 0; k < EvURBs; k++) m_pEvBuf[k] = DmaBuf ();
	for (unsigned k = 0; k < AudioInURBs; k++) m_pAudioInBuf[k] = DmaBuf ();
	for (unsigned k = 0; k < AudioOutBufs; k++) m_pAudioOutBuf[k] = DmaBuf ();
	for (unsigned c = 0; c < kChannels; c++)
	{
		m_pChanOutBuf[c] = DmaBuf ();
		for (unsigned k = 0; k < ChanInURBs; k++) m_pChanInBuf[c][k] = DmaBuf ();
	}
	s_pThis = this;
	if (m_nVariant == 2)
	{
		Step ("variant 2: a link request before any URB");
		int n = Vendor (TRUE, kReqStats, 0, 0, 0, sizeof (Stats));
		Step ("variant 2: returned %d", n);
	}
	Step ("start: URBs");
	// the IN URBs: a fixed pool, resubmitted from their completions (no allocation while streaming, as ob_usb.cpp's)
	for (unsigned k = 0; k < EvURBs; k++)
	{
		m_pEvURB[k] = new CUSBRequest (m_pEv, m_pEvBuf[k], kEvMps);
		m_pEvURB[k]->SetCompletionRoutine (EvStub, (void *) (uintptr) k, this);
		Resubmit (m_pEvURB[k]);
	}
	for (unsigned k = 0; k < AudioInURBs; k++)
	{
		m_pAudioInURB[k] = new CUSBRequest (m_pAudioIn, m_pAudioInBuf[k], kLinkMps);
		m_pAudioInURB[k]->SetCompletionRoutine (AudioInStub, (void *) (uintptr) k, this);
		Resubmit (m_pAudioInURB[k]);
	}
	for (unsigned c = 0; c < kChannels; c++)
		for (unsigned k = 0; k < ChanInURBs; k++)
		{
			m_pChanInURB[c][k] = new CUSBRequest (m_pChanIn[c], m_pChanInBuf[c][k], kLinkMps);
			m_pChanInURB[c][k]->SetCompletionRoutine (ChanInStub, (void *) (uintptr) (c * ChanInURBs + k), this);
			Resubmit (m_pChanInURB[c][k]);
		}
	Step ("start: URBs submitted");
	if (m_nVariant == 3)
	{
		Step ("variant 3: GET_DESCRIPTOR device through the same sync path");
		int n = GetHost ()->GetDescriptor (GetEndpoint0 (), DESCRIPTOR_DEVICE, DESCRIPTOR_INDEX_DEFAULT, m_pCtlBuf, 18, REQUEST_IN);
		Step ("variant 3: returned %d", n);
	}
	// (the A4's USB MIDI to this computer: kernel.cpp's loop sets link1's tap to MidiTap while the relay shows the A4)
	m_bStarted = TRUE;
	Step ("started");
	return TRUE;
}

void CTeensyTarget::Resubmit (CUSBRequest *pURB)
{
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	if (!GetHost ()->SubmitAsyncRequest (pURB)) m_Stats.nVendorFail++;	// the link is going away
}

int CTeensyTarget::Vendor (boolean bIn, u8 uchRequest, u16 wValue, u16 wIndex, const void *pData, unsigned nLength)
{
	if (nLength > kDmaSize || m_pCtlBuf == 0) return -1;	// (not started yet: no EP0 buffer)
	if (!bIn && nLength) memcpy (m_pCtlBuf, pData, nLength);
	if (m_nDebugVendor < 40) Step ("vendor %s %02x value %04x index %04x length %u: sent", bIn ? "IN" : "OUT", uchRequest, wValue, wIndex, nLength);
	int n;
	if (m_nVariant == 4)		// as CObUsb::Control: the request as an async URB on EP0, waited for here (1 s)
	{
		TSetupData *pS = new TSetupData;
		pS->bmRequestType = (bIn ? REQUEST_IN : REQUEST_OUT) | REQUEST_VENDOR;
		pS->bRequest = uchRequest, pS->wValue = wValue, pS->wIndex = wIndex, pS->wLength = (u16) nLength;
		CleanAndInvalidateDataCacheRange ((uintptr) m_pCtlBuf, kDmaSize);
		CUSBRequest *pURB = new CUSBRequest (GetEndpoint0 (), nLength ? m_pCtlBuf : 0, nLength, pS);
		pURB->SetCompletionRoutine (CtlStub, 0, this);
		m_bCtlDone = FALSE;
		n = -1;
		if (GetHost ()->SubmitAsyncRequest (pURB))
		{
			unsigned t0 = CTimer::Get ()->GetTicks ();
			while (!m_bCtlDone && CTimer::Get ()->GetTicks () - t0 < HZ) {}
			if (m_bCtlDone)
			{
				n = m_nCtlResult;
				InvalidateDataCacheRange ((uintptr) m_pCtlBuf, kDmaSize);
			}
			// (not done in 1 s: the URB stays with the controller; a debug variant only)
		}
		else { delete pURB; delete pS; }
	}
	else
		// no buffer for a request with no data stage: Circle's xHCI asserts nBufLen > 0 on a buffer at completion
		// (xhciendpoint.cpp: TR1g 2026-11-14, every reset of TR1 to TR1g)
		n = GetHost ()->ControlMessage (GetEndpoint0 (), (bIn ? REQUEST_IN : REQUEST_OUT) | REQUEST_VENDOR, uchRequest,
						wValue, wIndex, nLength ? m_pCtlBuf : 0, (u16) nLength);
	if (m_nDebugVendor < 40) { m_nDebugVendor++; Step ("vendor %02x: returned %d", uchRequest, n); }
	if (n < 0) m_Stats.nVendorFail++;
	return n;
}

// ---- IMirrorTarget: the box's side ---------------------------------------------------------------------------------

void CTeensyTarget::CtlStub (CUSBRequest *pURB, void *, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	TSetupData *pS = pURB->GetSetupData ();
	if (p == s_pThis)
	{
		p->m_nCtlResult = pURB->GetStatus () ? (int) pURB->GetResultLength () : -1;
		p->m_bCtlDone = TRUE;
	}
	delete pURB;
	delete pS;
}

// xHCI IRQ: one event packet of the computer's MIDI to the relay: usbreboot=1's reboot SysEx
void CTeensyTarget::RebootFeed (const u8 *pEventPacket)
{
	if (!m_bUsbReboot || pEventPacket[0] == 0) return;
	s_Reboot.Feed (pEventPacket);
	if (s_Reboot.Matched ()) { s_Reboot.Clear (); s_bRebootAsked = TRUE; }
}

boolean CTeensyTarget::Relaying (void)
{
	return s_pThis != 0 && s_pThis->m_bStarted && !s_pThis->m_bTest && !s_bLeaving;
}

void CTeensyTarget::SetBox (CObUsb *pBox)
{
	if (m_bTest) return;
	m_pBox = pBox;
	if (pBox == 0 && m_bBoxUp) MirrorBoxUp (FALSE);
}

// ob_usb.cpp PollAssign: the box moved to USB-C (it did not leave): off the computer's bus at once, not after mirrorhold
void CTeensyTarget::MirrorMoved (void)
{
	if (m_bTest) return;
	if (m_bBoxUp) MirrorBoxUp (FALSE);
	m_bLeaveNow = TRUE;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: the relay's box moved to USB-C: the relay leaves the computer's bus");
}

boolean CTeensyTarget::Mirror (void) const
{
	if (m_bTest) return TRUE;
	// opt-in as the gadget's: the USB-C mode OVERBRIDGE (or usb=mirror on the card) shows a box maker's IDs; a relay
	// alone does not turn it on
	CSidecarGadget *g = CSidecarGadget::Get ();
	if (g != 0) return g->Mirror ();
	return strcmp (CKernelOptions::Get ()->GetAppOptionString ("usb", "sidecar"), "mirror") == 0;
}

boolean CTeensyTarget::MirrorShown (void) const { return m_bConnected && m_bPcConfigured && !s_bLeaving; }

boolean CTeensyTarget::MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg)
{
	// relaytest: not a box of this Pi; relayboxpipes=1 lets its MirrorAttach go on (its pipes opened, polled, their
	// packets dropped below) so the bus carries what it does without relaytest (TR2c: the link ran at ~1 block/ms then)
	if (m_bTest && !m_bTestFeeding) return m_bBoxPipes;
	Map m;
	if (pDev[0] != 18 || pDev[1] != DESCRIPTOR_DEVICE || pDev[7] != 64 || !make_map (pCfg, nCfg, ob::kInEp, ob::kOutEp, &m))
	{
		CLogger::Get ()->Write (From, LogError, "usb=mirror: the box's descriptors are not ones the relay can carry");
		return FALSE;
	}
	m_bKeep = m_bHave && nCfg == m_nCfg && memcmp (pCfg, m_Cfg, nCfg) == 0 && memcmp (pDev, m_Dev, 18) == 0;
	if (m_bKeep) return TRUE;
	if (m_bConnected) Disconnect ();
	memcpy (m_Dev, pDev, 18);
	memcpy (m_Cfg, pCfg, nCfg);
	m_nCfg = nCfg;
	m_Map = m;
	m_nStr = 0;
	m_bHave = FALSE;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %04x:%04x through the relay, %u B configuration, %u endpoints, "
				"channels %u in / %u out", (unsigned) (pDev[8] | pDev[9] << 8), (unsigned) (pDev[10] | pDev[11] << 8),
				nCfg, m.l.n, m.n_out, m.n_in);
	return TRUE;
}

void CTeensyTarget::MirrorSetString (u8 uchIndex, const u8 *pDesc, unsigned nLength)
{
	if (m_bTest && !m_bTestFeeding) return;
	if (m_bKeep || m_nStr == mirror::kMaxStrings || nLength < 2 || nLength > sizeof m_Str[0].Data) return;
	m_Str[m_nStr].uchIndex = uchIndex;
	m_Str[m_nStr].nLength = (u8) nLength;
	memcpy (m_Str[m_nStr].Data, pDesc, nLength);
	m_nStr++;
}

void CTeensyTarget::MirrorBoxUp (boolean bUp)
{
	if (m_bTest && !m_bTestFeeding) return;
	EnterCritical ();
	m_bBoxUp = bUp && m_nCfg != 0;
	m_Stream.box_restart ();
	if (!m_bBoxUp && (m_CtlState == CtlAtBox || m_CtlState == CtlNew))	// a request at the box that went: STALL
	{
		m_nCtlAnswer = -1;
		m_CtlState = CtlAnswered;
	}
	if (!m_bBoxUp) m_bCutRytm = TRUE;		// the box went: MirrorPoll drops what waits for the Rytm (core 0's loop, the queue's consumer)
	for (unsigned c = 0; c < kChannels; c++)	// a computer packet at the box that went is never taken: credit it back
	{
		m_nCreditDue[c] += m_nAtBox[c];
		m_nAtBox[c] = 0;
	}
	LeaveCritical ();
	if (bUp && m_nCfg)
	{
		m_bHave = TRUE;
		if (!m_bConnected) m_bUploadDue = TRUE;	// MirrorPoll uploads and connects (core 0's loop, not inside Configure)
	}
	else m_nBoxGoneAt = NowTicks ();
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: box %s", m_bBoxUp ? "up" : m_bConnected ? "gone (requests are STALLed; "
				"off the computer's bus after mirrorhold)" : "gone");
}

boolean CTeensyTarget::Upload (void)
{
	if (m_nVariant == 5) { Step ("variant 5: no link request at all (URBs only)"); m_bUploadDue = FALSE; return TRUE; }
	Step ("upload: %u B configuration, %u strings", m_nCfg, m_nStr);
	for (unsigned i = 0; i < m_Map.l.n; i++)		// the box's endpoints as the Teensy shows them (TR2: the A4's shape)
	{
		const mirror::Ep &e = m_Map.l.ep[i];
		CLogger::Get ()->Write (From, LogNotice, "relay ep 0x%02x iface %u role %u type %u mps %u interval %u desc %02x %02x %02x",
					e.addr, e.iface, e.role, e.attr & 3, e.mps, e.interval, e.desc[4], e.desc[5], e.desc[6]);
	}
	if (Vendor (FALSE, kReqConnect, 0, 0, 0, 0) < 0) return FALSE;
	if (Vendor (FALSE, kReqSetDevice, 0, 0, m_Dev, 18) < 0) return FALSE;
	if (Vendor (FALSE, kReqSetConfig, 0, 0, m_Cfg, m_nCfg) < 0) return FALSE;
	for (unsigned k = 0; k < m_nStr; k++)
		if (Vendor (FALSE, kReqSetString, m_Str[k].uchIndex, 0, m_Str[k].Data, m_Str[k].nLength) < 0) return FALSE;
	if (m_bTest && !TestExtras ()) return FALSE;
	m_Stream.in_ch = m_Stream.out_ch = 0;
	m_bPcConfigured = FALSE;
	if (Vendor (FALSE, kReqConnect, 1, 0, 0, 0) < 0) return FALSE;
	m_bConnected = TRUE;
	m_Stats.nConnects++;
	m_nEvReshowTry = 0;
	EvArm ();
	Step ("upload: connected");
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: the relay shows the box to its computer (%u strings)", m_nStr);
	return TRUE;
}

void CTeensyTarget::Disconnect (void)
{
	Vendor (FALSE, kReqConnect, 0, 0, 0, 0);
	m_bConnected = FALSE;
	m_nEvShownAt = 0;
	m_bPcConfigured = FALSE;
	m_Stream.in_ch = m_Stream.out_ch = 0;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: the relay's computer side left the bus");
}

void CTeensyTarget::EvArm (void)
{
	m_nEvAtShow = m_Stats.nEvents;
	m_nEvStep = 0;
	m_nEvShownAt = NowTicks ();
	if (m_nEvShownAt == 0) m_nEvShownAt = 1;
}

// MirrorPoll (task level, IRQs on: the xHCI commands wait for their events), while the relay is shown
void CTeensyTarget::EvWatch (void)
{
	if (m_nEvShownAt == 0 || m_pEv == 0) return;
	if (m_Stats.nEvents != m_nEvAtShow)		// the kEvHello (or anything) came: the pipe works
	{
		if (m_nEvStep != 0 || m_nEvReshowTry != 0)
			CLogger::Get ()->Write (From, LogNotice, "relay event pipe answered again (%s, %u re-shows)",
						m_nEvStep ? "after its URBs were resubmitted" : "after the relay was shown again", m_nEvReshowTry);
		m_nEvShownAt = 0;
		return;
	}
	u64 nNow = NowTicks ();
	if (nNow - m_nEvShownAt < (u64) kEvSilentSecs * (m_nEvStep + 1) * CLOCKHZ) return;
	unsigned nSecs = (unsigned) ((nNow - m_nEvShownAt) / CLOCKHZ);
	if (m_nEvStep == 0)
	{
		// step 1: the URBs. A halted (or erred) endpoint is reset and every queued URB dropped, then all are submitted
		// again; a running one keeps its URBs (Circle cannot cancel them there), so only its state is told
		m_nEvStep = 1;
		m_nEvSilent++;
		CXHCIEndpoint *pX = m_pEv->GetXHCIEndpoint ();
		unsigned nPending = pX->GetPendingURBs ();
		unsigned nNowUs = (unsigned) nNow;
		m_EvHalt.Stall ();
		int r = m_EvHalt.Reset (pX, nNowUs, [this] {
			GetHost ()->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_ENDPOINT, CLEAR_FEATURE, ENDPOINT_HALT,
						    kEvEp, 0, 0);
		});
		unsigned nResubmitted = 0;
		if (r > 0)
		{
			EnterCritical ();
			if (m_EvHalt.Finish (pX))
				for (unsigned k = 0; k < EvURBs; k++) { Resubmit (m_pEvURB[k]); nResubmitted++; }
			LeaveCritical ();
		}
		m_EvHalt.halted = m_EvHalt.erred = false;	// (a refused reset: tried again only with the next silence)
		CLogger::Get ()->Write (From, LogWarning, "relay event pipe silent %u s after the relay was shown (ev %u, endpoint state %u, "
					"%u URBs pending): %s; shown again in %u s if still silent", nSecs, m_Stats.nEvents, m_EvHalt.state,
					nPending, r < 0 ? "reset refused" : nResubmitted ? "reset, URBs resubmitted" :
					"running, URBs left queued", kEvSilentSecs);
		return;
	}
	// step 2: show the relay again, so that the computer enumerates it from the start
	if (!m_bBoxUp) return;				// the box went: Disconnect follows after mirrorhold, nothing to show
	if (m_nEvReshowTry >= kReshowMax)
	{
		CLogger::Get ()->Write (From, LogError, "relay event pipe still silent after %u re-shows: left as it is (the computer's "
					"requests go unanswered until the relay or the box comes back)", m_nEvReshowTry);
		m_nEvShownAt = 0;
		return;
	}
	m_nEvReshowTry++;
	m_nEvReshows++;
	EnterCritical ();
	if (m_CtlState != CtlIdle) m_CtlState = CtlIdle;	// the request was the old enumeration's: an answer is for nobody
	LeaveCritical ();
	m_bPcConfigured = FALSE;
	m_Stream.in_ch = m_Stream.out_ch = 0;
	int n0 = Vendor (FALSE, kReqConnect, 0, 0, 0, 0);
	int n1 = Vendor (FALSE, kReqConnect, 1, 0, 0, 0);
	m_Stats.nConnects++;
	CLogger::Get ()->Write (From, LogWarning, "relay event pipe still silent %u s after the relay was shown: shown again (%u of %u; "
				"connect 0 %d, 1 %d)", nSecs, m_nEvReshowTry, kReshowMax, n0, n1);
	EvArm ();
}

// xHCI IRQ: the box's answer to a forwarded request
void CTeensyTarget::MirrorControlDone (unsigned nSeq, const u8 *pData, int nLength)
{
	if (m_CtlState != CtlAtBox || (u8) nSeq != m_CtlSeq) { m_Stats.nLate++; return; }
	m_nCtlAnswer = nLength;
	if (nLength > 0) memcpy (m_CtlData, pData, (unsigned) nLength <= sizeof m_CtlData ? nLength : sizeof m_CtlData);
	m_CtlState = CtlAnswered;
}

// xHCI IRQ: one IN packet of a box pipe for the computer
boolean CTeensyTarget::MirrorPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength)
{
	if (m_bTest)		// relaytest (relayboxpipes=1): the real box's packet dropped and held as the real path holds it until the
	{			// computer took it; MirrorPoll asks for the next one 1 ms later (T331: "not taken" re-requested at once: a storm)
		m_Stats.nPipeInDrop++;
		m_uchTestParked = uchAddr;
		return TRUE;
	}
	return SendPipeIn (uchAddr, pData, nLength);
}

boolean CTeensyTarget::SendPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength)
{
	unsigned c = ChanOfIn (uchAddr);
	if (c >= kChannels || !MirrorShown () || nLength > kLinkMps || m_bChanOutBusy[c]) { m_Stats.nPipeInDrop++; return FALSE; }
	// a zero-length packet is not sent on: Circle's xHCI asserts pBuffer != 0 and nBufLen > 0 on a bulk or interrupt
	// transfer (xhciendpoint.cpp), which reset the Pi in BL1 (T409, 2026-11-17: an A4 pipe packet as the relay left
	// for its bootloader). Dropped and counted; the box's next IN request goes out at once (the caller resubmits).
	if (nLength == 0 || m_pChanOut[c] == 0 || m_pChanOutBuf[c] == 0) { m_Stats.nPipeInDrop++; m_nPipeInZlp++; return FALSE; }
	memcpy (m_pChanOutBuf[c], pData, nLength);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pChanOutBuf[c], kDmaSize);
	CUSBRequest *pURB = new CUSBRequest (m_pChanOut[c], m_pChanOutBuf[c], nLength);
	pURB->SetCompletionRoutine (ChanOutStub, (void *) (uintptr) c, this);
	m_bChanOutBusy[c] = TRUE;
	if (!GetHost ()->SubmitAsyncRequest (pURB)) { m_bChanOutBusy[c] = FALSE; delete pURB; m_Stats.nPipeInDrop++; return FALSE; }
	m_Stats.nPipeIn++;
	return TRUE;			// the box's next IN request waits for kEvInTaken (the computer has this one)
}

// xHCI IRQ: the box took the computer's pipe packet: the computer may send the next (one credit)
void CTeensyTarget::MirrorPipeOutDone (u8 uchAddr)
{
	unsigned c = ChanOfOut (uchAddr);
	if (c >= kChannels) return;
	if (m_nAtBox[c]) m_nAtBox[c]--;
	m_nCreditDue[c]++;
}

boolean CTeensyTarget::MirrorOutExtra (u8 *pBlock, u16 nCounter)
{
	if (m_bTest) return FALSE;
	return MirrorShown () && m_Stream.out_extra (pBlock, nCounter);
}

// xHCI IRQ: an IN block of the box (12-channel layout), cut to the computer's alternate and sent on link 0x02
void CTeensyTarget::FromBoxBlock (const u8 *pBlock)
{
	if (!m_bTest && MirrorShown ())
	{
		uint32_t ms = (uint32_t) (NowTicks () / 1000);
		for (int f = 0; f < ob::kFramesPerBlock; f++) m_SlipIn.feed (ob::block_sample (pBlock, ob::kChannels, f, 0), ms);
	}
	if (m_bTest)					// relaytest: a real box's blocks never reach the computer (TestPoll uses SendBoxBlock)
	{
		// relayfeed=irq: the box's block only paces the test stream: a test block goes out in its place, from the same
		// xHCI completion, so the link sees a real box's timing (TR2: ~2-3 blocks per completion, every ~380 us)
		if (m_bFeedIrq && MirrorShown () && m_Stream.in_ch == 8 && m_nTestStart != 0)
		{
			u8 Blk[ob::block_bytes (12)];
			TestBlock (Blk, m_nTestSent++);
			SendBoxBlock (Blk);
		}
		return;
	}
	SendBoxBlock (pBlock);
}

// FromBoxBlock's work; relaytest's TestPoll calls it directly (a shared "feeding" flag let a few real blocks through, TR1i)
void CTeensyTarget::SendBoxBlock (const u8 *pBlock)
{
	if (!MirrorShown ()) return;
	unsigned k = AudioOutBufs, nBusy = 0;
	for (unsigned i = 0; i < AudioOutBufs; i++)
	{
		if (m_bAudioOutBusy[i]) nBusy++;
		else if (k == AudioOutBufs) k = i;
	}
	if (nBusy > m_Stats.nBusyMax) m_Stats.nBusyMax = nBusy;
	if (k == AudioOutBufs)				// the link is behind by 16 blocks (2.3 ms)
	{
		m_Stats.nAudioOutDrop++;
		if (++m_Stats.nDropRun > m_Stats.nDropRunMax) m_Stats.nDropRunMax = m_Stats.nDropRun;
		return;
	}
	m_Stats.nDropRun = 0;
	unsigned n = m_Stream.in_block (pBlock, m_pAudioOutBuf[k]);
	if (n == 0) return;
	CleanAndInvalidateDataCacheRange ((uintptr) m_pAudioOutBuf[k], kDmaSize);
	if (m_pAudioOutURB[k] == 0 || m_nAudioOutLen[k] != n)	// the computer's channels changed (its SET_INTERFACE): once
	{
		delete m_pAudioOutURB[k];
		m_pAudioOutURB[k] = new CUSBRequest (m_pAudioOut, m_pAudioOutBuf[k], n);
		m_pAudioOutURB[k]->SetCompletionRoutine (AudioOutStub, (void *) (uintptr) k, this);
		m_nAudioOutLen[k] = n;
	}
	m_bAudioOutBusy[k] = TRUE;
	m_nAudioOutAt[k] = NowTicks ();
	CUSBRequest *pURB = m_pAudioOutURB[k];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	if (!GetHost ()->SubmitAsyncRequest (pURB)) { m_bAudioOutBusy[k] = FALSE; m_Stats.nAudioOutDrop++; }
}

void CTeensyTarget::ToBox (s32 (*pFrames)[12], const s16 (*)[8], unsigned nFrames)
{
	if (m_bTest) return;
	if (!MirrorShown ()) return;
	m_Stream.to_box (pFrames, nFrames);
	const mirror::StreamStats &a = m_Stream.st;	// each OUT ring under/over event's time, for a loop's breaks (T342)
	if (a.out_under != m_nUnderSeen || a.out_over != m_nOverSeen)
	{
		char c = a.out_under != m_nUnderSeen ? 'U' : 'O';
		m_nUnderSeen = a.out_under, m_nOverSeen = a.out_over;
		m_EvMs[m_nEv % 8] = (unsigned) (NowTicks () / 1000);
		m_EvKind[m_nEv % 8] = c;
		m_nEv++;
	}
}

// ---- the link's completions (xHCI IRQ) -----------------------------------------------------------------------------

void CTeensyTarget::EvStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	unsigned k = (unsigned) (uintptr) pParam;
	if (p != s_pThis) return;			// the relay went: Circle frees its URBs with the device
	if (p->m_bDebug && p->m_Stats.nEvents < 8)	// (IRQ: logged only, the black box writes it with the next step)
		CLogger::Get ()->Write (From, LogNotice, "relaydebug: event URB status %d length %u", pURB->GetStatus (), pURB->GetResultLength ());
	if (pURB->GetStatus () && pURB->GetResultLength () >= sizeof (Event))
	{
		InvalidateDataCacheRange ((uintptr) p->m_pEvBuf[k], kDmaSize);
		Event e;
		memcpy (&e, p->m_pEvBuf[k], sizeof e);
		p->OnEvent (e);
	}
	p->Resubmit (pURB);
}

void CTeensyTarget::OnEvent (const Event &e)
{
	m_Stats.nEvents++;
	switch (e.type)
	{
	case kEvBus:
		if (e.a == kBusReset || e.a == kBusOff)
		{
			m_bPcConfigured = FALSE;
			m_Stream.in_ch = m_Stream.out_ch = 0;
		}
		break;
	case kEvConfig:
		m_bPcConfigured = e.a != 0;
		if (m_bDebug) CLogger::Get ()->Write (From, LogNotice, "relaydebug: the computer configured %u", e.a);
		m_Stream.in_ch = m_Stream.out_ch = 0;
		for (unsigned c = 0; c < m_Map.n_out; c++)	// each OUT pipe takes one packet at a time; MIDI a few, as they fit
		{
			const mirror::Ep &ep = m_Map.l.ep[m_Map.ep_of_out[c]];
			if (ep.role == mirror::kPipeOut) { m_nCreditDue[c]++; continue; }
			// MIDI: the Teensy holds none now (bus reset, kReqConnect); MirrorPoll grants these while a whole packet each fits
			EnterCritical ();
			m_nMidiOut[c] = 0;
			m_nCreditDue[c] = 0;
			m_nMidiHeld[c] = kMaxCredits;
			LeaveCritical ();
		}
		break;
	case kEvAlt:
		m_Stream.set_alt (m_Map.l, m_Cfg, m_nCfg, e.a, e.b);
		break;
	case kEvSetup:
		m_Stats.nSetups++;
		m_CtlSeq = e.a;
		memcpy (m_CtlSetup, e.data, 8);
		m_CtlState = CtlNew;			// MirrorPoll forwards it (its OUT data stage read there)
		break;
	case kEvAbort:
		m_Stats.nAborts++;
		if (e.a == m_CtlSeq) m_CtlState = CtlIdle;	// a late answer from the box is for nobody (MirrorControlDone)
		break;
	case kEvInTaken:
		if (m_pBox != 0) for (unsigned i = 0; i < e.b; i++) m_pBox->PipeInNext (e.a);
		break;
	default:
		break;
	}
}

void CTeensyTarget::AudioInStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	unsigned k = (unsigned) (uintptr) pParam;
	if (p != s_pThis) return;
	if (pURB->GetStatus ())
	{
		InvalidateDataCacheRange ((uintptr) p->m_pAudioInBuf[k], kDmaSize);
		CObUsb *pBox = p->m_pBox;
		if (p->m_bTest) p->TestOutBlock (p->m_pAudioInBuf[k], pURB->GetResultLength ());
		else if (pBox != 0 && p->MirrorShown ())
		{
			u64 nNow = NowTicks ();
			if (p->m_nCompLast != 0 && nNow - p->m_nCompLast < 1000000)
			{
				unsigned us = (unsigned) (nNow - p->m_nCompLast);
				if (us > p->m_nCompGapMax) p->m_nCompGapMax = us;
				if (us >= 1000) p->m_nCompGap1++;
				if (us >= 2000) p->m_nCompGap2++;
			}
			p->m_nCompLast = nNow;
			unsigned nCh = p->m_Stream.out_ch;
			if (nCh != 0 && pURB->GetResultLength () == (unsigned) ob::block_bytes (nCh))
				for (int f = 0; f < ob::kFramesPerBlock; f++)
					p->m_SlipOut.feed (ob::block_sample (p->m_pAudioInBuf[k], nCh, f, 0), (uint32_t) (nNow / 1000));
			p->m_Stream.out_block (p->m_pAudioInBuf[k], pURB->GetResultLength (), pBox->NextOutCounter ());
		}
	}
	p->Resubmit (pURB);
}

void CTeensyTarget::AudioOutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	if (p != s_pThis) return;
	unsigned k = (unsigned) (uintptr) pParam;
	TStats &s = p->m_Stats;
	u64 us = NowTicks () - p->m_nAudioOutAt[k];
	if (us > s.nLinkMaxUs) s.nLinkMaxUs = (unsigned) us;
	unsigned b = 0;
	for (u64 lim = 125; b < 7 && us >= lim; lim <<= 1) b++;
	s.nLinkHist[b]++;
	s.nLinkDone++;
	if (pURB->GetStatus () == 0) { s.nLinkErr++; s.nLinkErrStatus = (unsigned) pURB->GetUSBError (); }
	p->m_bAudioOutBusy[k] = FALSE;			// its URB kept for the next block
}

void CTeensyTarget::ChanInStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	unsigned c = (unsigned) (uintptr) pParam / ChanInURBs, k = (unsigned) (uintptr) pParam % ChanInURBs;
	if (p != s_pThis) return;
	if (pURB->GetStatus () && p->m_Map.ep_of_out[c] >= 0)
	{
		InvalidateDataCacheRange ((uintptr) p->m_pChanInBuf[c][k], kDmaSize);
		const mirror::Ep &ep = p->m_Map.l.ep[p->m_Map.ep_of_out[c]];
		if (ep.role == mirror::kMidiOut && p->m_nMidiOut[c]) p->m_nMidiOut[c]--;	// the credit's packet came
		CObUsb *pBox = p->m_pBox;
		if (p->m_bTest && ep.role == mirror::kPipeOut)
		{
			// the made-up box echoes a pipe packet on its IN pipe of the same number (0x02 -> 0x82); the computer's
			// next packet once the echo is out (its credit), as a box that took it
			if (p->SendPipeIn ((u8) (ep.addr | 0x80), p->m_pChanInBuf[c][k], pURB->GetResultLength ())) p->m_nTestEcho++;
			else p->m_nTestEchoDrop++;
			p->m_nCreditDue[c]++;
		}
		else if (ep.role == mirror::kMidiOut && pBox != 0 && pBox->OnTransport (1) && link1::Device () != 0)
		{
			const u8 *q = p->m_pChanInBuf[c][k];
			unsigned n = pURB->GetResultLength () / 4;
			for (unsigned i = 0; i < n; i++, q += 4)
			{
				p->RebootFeed (q);
				p->m_ToBoxFilter.Feed (q, [p] (const uint8_t *e)
				{
					if (link1::FromComputer (e)) p->m_Stats.nMidiToBox++;
					else p->m_Stats.nMidiToBoxFull++;
				});
			}
			// the next packet from the computer once it fits beside every packet already granted; else MirrorPoll grants it
			if (p->MidiCreditFits (c)) p->m_nCreditDue[c]++;
			else p->m_nMidiHeld[c]++;
		}
		else if (ep.role == mirror::kMidiOut && pBox != 0 && pBox->OnTransport (0) && p->m_bBoxUp)
		{
			// the Rytm on the relay: the computer's MIDI to it through the USB merge (ob_usb.cpp), as from USB-C
			const u8 *q = p->m_pChanInBuf[c][k];
			unsigned n = pURB->GetResultLength () / 4;
			for (unsigned i = 0; i < n; i++, q += 4)
			{
				if (q[0] == 0) continue;			// padding
				p->RebootFeed (q);
				// the keys tap first, as MIDIFromHost on USB-C (the gadget's tap is off then: USB-C shows the A4 or nothing)
				if (g_pSidecarKeysTap != 0 && (*g_pSidecarKeysTap) (q)) { p->m_Stats.nMidiToKeys++; continue; }
				if (p->m_ToRytmPath.FromHost (q)) p->m_Stats.nMidiToBox++;
				else p->m_Stats.nMidiToBoxFull++;
			}
			if (p->MidiCreditFits (c)) p->m_nCreditDue[c]++;
			else p->m_nMidiHeld[c]++;
		}
		else if (ep.role == mirror::kPipeOut && pBox != 0 && p->m_bBoxUp
		    && pBox->PipeOut (ep.addr, p->m_pChanInBuf[c][k], pURB->GetResultLength ()))
		{
			p->m_Stats.nPipeOut++;			// its credit comes back with MirrorPipeOutDone
			p->m_nAtBox[c]++;
		}
		else
		{
			// dropped: no box, a halted pipe, or USB MIDI with no box MIDI (link1; a Rytm not up); the computer may send
			// the next one. The reboot SysEx is obeyed whichever box the relay shows
			if (ep.role == mirror::kMidiOut)
			{
				const u8 *q = p->m_pChanInBuf[c][k];
				for (unsigned i = 0, n = pURB->GetResultLength () / 4; i < n; i++, q += 4) p->RebootFeed (q);
				p->m_Stats.nChanInMidi++;
			}
			else p->m_Stats.nChanInDrop++;
			p->m_nCreditDue[c]++;
		}
	}
	p->Resubmit (pURB);
}

void CTeensyTarget::ChanOutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	if (p == s_pThis)
	{
		p->m_bChanOutBusy[(unsigned) (uintptr) pParam] = FALSE;
		if ((unsigned) (uintptr) pParam == p->MidiInChan ()) p->FlushMidiIn ();
	}
	delete pURB;
}

void CTeensyTarget::MidiTap (const u8 *pEventPacket)
{
	CTeensyTarget *p = s_pThis;
	if (p == 0 || !p->MirrorShown ()) return;
	p->QueueToPc (pEventPacket);
}

void CTeensyTarget::QueueToPc (const u8 *pEventPacket)
{
	if (m_nMidiInHead - m_nMidiInTail >= MidiInRing) { m_Stats.nMidiToPcDrop++; return; }
	u32 w;
	memcpy (&w, pEventPacket, 4);
	m_MidiIn[m_nMidiInHead % MidiInRing] = w;
	m_nMidiInHead = m_nMidiInHead + 1;
	FlushMidiIn ();
}

boolean CTeensyTarget::ShowsRytm (void)
{
	return Relaying () && CObUsb::RelayShowsHostLink ();
}

void CTeensyTarget::RytmTap (const u8 *pEventPacket)
{
	CTeensyTarget *p = s_pThis;
	if (p == 0 || !ShowsRytm () || (!p->MirrorShown () && !p->m_ToPcFilter.InSysEx ())) return;
	if (p->m_Gate.On () && !CObUsb::Active ()) return;	// usbmidi=os: on or emu: only while USB is the host (as the gadget's)
	p->m_ToPcFilter.Feed (pEventPacket, [p] (const u8 *q) { if (p->MirrorShown ()) p->QueueToPc (q); });
}

boolean CTeensyTarget::ForRytm (u32 *pPacket)
{
	CTeensyTarget *p = s_pThis;
	return p != 0 && p->m_ToRytmPath.Pop (pPacket);
}

boolean CTeensyTarget::ForRytmInSysEx (void)
{
	CTeensyTarget *p = s_pThis;
	return p != 0 && p->m_ToRytmPath.InSysEx ();
}

void CTeensyTarget::OsLost (boolean bSendFailed, const char *pWhy)
{
	if (!m_Gate.Lost (bSendFailed, m_ToRytmPath.Busy (), m_ToRytmPath.Filter ().OsMessages (), CTimer::GetClockTicks ()))
		return;
	EnterCritical ();				// the filter and the queue's producer run in the xHCI IRQ (ChanInStub)
	unsigned n = ApplyGate (m_ToRytmPath, m_Gate, TRUE);
	LeaveCritical ();
	CLogger::Get ()->Write (From, LogError, "usb=mirror: usbmidi=os: OS upgrade of the Rytm on the relay ABORTED: %s during it, "
				"part of the image is lost (%u packets dropped). Start the upgrade again (OS messages are dropped until "
				"the computer stops sending them for %u s)", pWhy, n, OsGate::kQuietUs / 1000000);
}

void CTeensyTarget::BoxOsLost (boolean bSendFailed, const char *pWhy)
{
	if (!m_BoxGate.Lost (bSendFailed, link1::ComputerBusy () || m_ToBoxFilter.InSysEx (), m_ToBoxFilter.OsMessages (),
			     CTimer::GetClockTicks ()))
		return;
	EnterCritical ();				// the filter runs in the xHCI IRQ (ChanInStub)
	unsigned n = ApplyBoxGate (TRUE);
	LeaveCritical ();
	CLogger::Get ()->Write (From, LogError, "usb=mirror: usbmidi=os: OS upgrade of the box on the relay ABORTED: %s during it, "
				"part of the image is lost (%u packets dropped). Start the upgrade again (OS messages are dropped until "
				"the computer stops sending them for %u s)", pWhy, n, OsGate::kQuietUs / 1000000);
}

unsigned CTeensyTarget::ApplyBoxGate (boolean bCut)
{
	unsigned n = 0;
	if (bCut)
	{
		n = link1::CutFromComputer ();
		m_ToBoxFilter.Cut ();
	}
	boolean bPass = m_BoxGate.Pass ();
	if (bPass && !m_ToBoxFilter.PassOsUpdate ()) m_ToBoxFilter.AwaitStart (true);	// a new upgrade's start first
	m_ToBoxFilter.SetPassOsUpdate (bPass);
	return n;
}

void CTeensyTarget::OsGatePoll (void)
{
	if (m_bCutRytm)					// the box went: nothing of the computer's waits for the next one
	{
		m_bCutRytm = FALSE;
		OsLost (FALSE, "the box went");		// an upgrade under way: aborted, loudly
		BoxOsLost (FALSE, "the box went");
		EnterCritical ();
		unsigned n = m_ToRytmPath.Cut ();
		LeaveCritical ();
		if (n) CLogger::Get ()->Write (From, LogNotice, "usb=mirror: the box went: %u MIDI packets for the Rytm dropped", n);
	}
	unsigned nLost = link1::ComputerLost ();	// slot 1: the computer's packets lost on link1's way to the box
	if (nLost != m_nLink1LostSeen)
	{
		m_nLink1LostSeen = nLost;
		if (m_pBox != 0 && m_pBox->OnTransport (1)) BoxOsLost (TRUE, "a send to the box failed");
	}
	if (m_Stats.nChanInMidi != m_nNoBoxSeen)		// the computer's MIDI dropped with no box up (ChanInStub's last branch):
	{							// a box gone mid-image and back within mirrorhold must not get the rest
		m_nNoBoxSeen = m_Stats.nChanInMidi;
		OsLost (TRUE, "MIDI for the box was dropped while it was gone");
		BoxOsLost (TRUE, "MIDI for the box was dropped while it was gone");
	}
	if (m_Stats.nMidiToBoxFull != m_nToBoxFullSeen)	// either slot: the computer's packets dropped at a full queue
	{
		m_nToBoxFullSeen = m_Stats.nMidiToBoxFull;
		if (m_pBox != 0 && m_pBox->OnTransport (0)) OsLost (TRUE, "the queue to the Rytm was full");
		else BoxOsLost (TRUE, "the queue to the box was full");
	}
	unsigned nNow = CTimer::GetClockTicks ();
	if (m_Gate.Quiet (m_ToRytmPath.Filter ().OsMessages (), nNow))	// an aborted upgrade's sender stopped
	{
		EnterCritical ();
		ApplyGate (m_ToRytmPath, m_Gate, FALSE);	// the pass on again, from a new start
		LeaveCritical ();
		CLogger::Get ()->Write (From, LogNotice, "usb=mirror: usbmidi=os: OS upgrades to the Rytm on the relay %s again "
					"after the aborted one, from a new start", m_Gate.Pass () ? "pass" : "are dropped");
	}
	if (m_BoxGate.Quiet (m_ToBoxFilter.OsMessages (), nNow))
	{
		EnterCritical ();
		ApplyBoxGate (FALSE);
		LeaveCritical ();
		CLogger::Get ()->Write (From, LogNotice, "usb=mirror: usbmidi=os: OS upgrades to the box on the relay %s again "
					"after the aborted one, from a new start", m_BoxGate.Pass () ? "pass" : "are dropped");
	}
}

void CTeensyTarget::RytmSendFailed (void)
{
	if (s_pThis != 0) s_pThis->OsLost (TRUE, "a send to the Rytm failed");
}

void CTeensyTarget::RytmMidiConnected (boolean bOn)
{
	CTeensyTarget *p = s_pThis;
	if (p == 0) return;
	if (p->m_bRytmMidi && !bOn) p->OsLost (FALSE, "the Rytm stopped taking MIDI");
	p->m_bRytmMidi = bOn;
}

void CTeensyTarget::MidiToComputer (u32 nPacket)
{
	CTeensyTarget *p = s_pThis;
	if (p == 0 || !ShowsRytm () || !p->MirrorShown ()) return;
	u8 q[4];
	memcpy (q, &nPacket, 4);
	EnterCritical ();				// the ring's other producer is the xHCI IRQ (RytmTap)
	p->QueueToPc (q);
	LeaveCritical ();
}

unsigned CTeensyTarget::MidiRoom (void) const
{
	if (m_pBox != 0 && m_pBox->OnTransport (0))
	{
		unsigned n = m_ToRytmPath.Size () + MidiFilter::kHoldPackets;
		return n >= RytmQueue ? 0 : RytmQueue - n;
	}
	return link1::FromComputerRoom ();
}

// A MIDI credit lets the computer send one link packet of up to 128 event packets. Credits not yet used, those due and
// the one asked for may all come at once, so each needs a whole packet's room: one per 128 the queue takes now
// (RF1, 2026-11-18: a credit per packet at room >= 128 kept 4 in flight and overflowed link1's queue during an OS upgrade)
boolean CTeensyTarget::MidiCreditFits (unsigned c) const
{
	return MidiRoom () >= 128 * (m_nMidiOut[c] + m_nCreditDue[c] + 1);
}

unsigned CTeensyTarget::MidiInChan (void) const
{
	for (unsigned c = 0; c < m_Map.n_in; c++) if (m_Map.l.ep[m_Map.ep_of_in[c]].role == mirror::kMidiIn) return c;
	return kChannels;
}

void CTeensyTarget::FlushMidiIn (void)
{
	unsigned c = MidiInChan ();
	if (c >= kChannels || m_bChanOutBusy[c] || m_nMidiInHead == m_nMidiInTail) return;
	unsigned mps = m_Map.l.ep[m_Map.ep_of_in[c]].mps;
	unsigned n = 0;
	while (m_nMidiInTail != m_nMidiInHead && n + 4 <= mps)
	{
		memcpy (m_pChanOutBuf[c] + n, &m_MidiIn[m_nMidiInTail % MidiInRing], 4);
		m_nMidiInTail = m_nMidiInTail + 1;
		n += 4;
	}
	CleanAndInvalidateDataCacheRange ((uintptr) m_pChanOutBuf[c], kDmaSize);
	CUSBRequest *pURB = new CUSBRequest (m_pChanOut[c], m_pChanOutBuf[c], n);
	pURB->SetCompletionRoutine (ChanOutStub, (void *) (uintptr) c, this);
	m_bChanOutBusy[c] = TRUE;
	if (!GetHost ()->SubmitAsyncRequest (pURB)) { m_bChanOutBusy[c] = FALSE; delete pURB; m_Stats.nMidiToPcDrop += n / 4; return; }
	m_Stats.nMidiToPc += n / 4;
}

unsigned CTeensyTarget::ChanOfOut (u8 uchAddr) const
{
	for (unsigned c = 0; c < m_Map.n_out; c++) if (m_Map.l.ep[m_Map.ep_of_out[c]].addr == uchAddr) return c;
	return kChannels;
}

unsigned CTeensyTarget::ChanOfIn (u8 uchAddr) const
{
	for (unsigned c = 0; c < m_Map.n_in; c++) if (m_Map.l.ep[m_Map.ep_of_in[c]].addr == uchAddr) return c;
	return kChannels;
}

// ---- core 0's loop ----------------------------------------------------------------------------------------------

boolean CTeensyTarget::IsNameRequest (const u8 *s, u8 uchType) const
{
	return m_Name[0] != 0 && mirror::le16 (m_Dev + 8) == mirror::kNameVendor && s[0] == uchType && s[1] == mirror::kNameRequest
		&& mirror::le16 (s + 2) == 0 && mirror::le16 (s + 4) == 0;
}

void CTeensyTarget::ControlPoll (void)
{
	if (m_CtlState == CtlNew)
	{
		u8 Setup[8];
		EnterCritical ();
		memcpy (Setup, m_CtlSetup, 8);
		u8 nSeq = m_CtlSeq;
		LeaveCritical ();
		u16 wLength = (u16) mirror::le16 (Setup + 6);
		u8 Data[512];
		boolean bOK = (m_pBox != 0 || m_bTest) && m_bBoxUp && wLength <= sizeof Data;
		if (bOK && !(Setup[0] & 0x80) && wLength)
		{
			int n = Vendor (TRUE, kReqCtlData, nSeq, 0, 0, wLength);
			bOK = n == (int) wLength;
			if (bOK) memcpy (Data, m_pCtlBuf, wLength);
			if (bOK && IsNameRequest (Setup, 0x40) && wLength == mirror::kNameLen)
				mirror::name_store (Data, mirror::kNameLen, m_Name);	// the box keeps its plain name
		}
		EnterCritical ();
		if (m_CtlState != CtlNew || m_CtlSeq != nSeq) bOK = FALSE;	// aborted meanwhile, or a newer one
		else if (bOK) m_CtlState = CtlAtBox;	// before the submit: the answer can come in the xHCI IRQ at once
		LeaveCritical ();
		if (bOK && m_bTest)
		{
			int nAnswer;
			boolean bAns = TestControl (Setup, Data, &nAnswer);
			EnterCritical ();
			if (m_CtlState == CtlAtBox && m_CtlSeq == nSeq)
			{
				m_nCtlAnswer = bAns ? nAnswer : -1;
				if (nAnswer > 0) memcpy (m_CtlData, Data, (unsigned) nAnswer);
				m_CtlState = CtlAnswered;
			}
			LeaveCritical ();
		}
		else if (bOK)
		{
			TSetupData S;
			memcpy (&S, Setup, sizeof S);
			if (m_pBox->Control (&S, Data, nSeq)) return;
		}
		EnterCritical ();
		if (!m_bTest && m_CtlSeq == nSeq && (m_CtlState == CtlNew || m_CtlState == CtlAtBox)) { m_nCtlAnswer = -1; m_CtlState = CtlAnswered; }
		else if (m_bTest && !bOK && m_CtlSeq == nSeq && m_CtlState == CtlNew) { m_nCtlAnswer = -1; m_CtlState = CtlAnswered; }
		LeaveCritical ();
	}
	if (m_CtlState == CtlAnswered)
	{
		u8 Setup[8], Data[512];
		EnterCritical ();
		u8 nSeq = m_CtlSeq;
		int n = m_nCtlAnswer;
		memcpy (Setup, m_CtlSetup, 8);
		if (n > 0) memcpy (Data, m_CtlData, (unsigned) n);
		m_CtlState = CtlIdle;
		LeaveCritical ();
		if (n > 0 && IsNameRequest (Setup, 0xC0))
		{
			u8 Name[mirror::kNameLen];		// the computer sees the name with the suffix
			unsigned l = (unsigned) n < sizeof Name ? (unsigned) n : sizeof Name;
			memset (Name, 0, sizeof Name);
			memcpy (Name, Data, l);
			mirror::name_show (Name, l, m_Name);
			memcpy (Data, Name, l);
		}
		if (n < 0) m_Stats.nStalls++;
		m_Stats.nAnswers++;
		Vendor (FALSE, kReqCtlAnswer, nSeq, n < 0 ? kAnsStall : 0, Data, n > 0 ? (unsigned) n : 0);
	}
}

void CTeensyTarget::MirrorPoll (void)
{
	if (!m_bStarted)
	{
		if (CTimer::Get ()->GetUptime () < m_nDelay) return;
		if (!Start ()) { m_nDelay = 0xFFFFFFFF; return; }	// not the link: stays idle
	}
	// the Pi flashing the Teensy (teensy_flash.h, docs/teensy-flash.md): its log (the start line names the build), and
	// the bootloader request. EP0 and m_pCtlBuf stay this poll's; after kReqBootloader the link goes within ~50 ms.
	if (s_bLogDue)
	{
		s_bLogDue = FALSE;
		for (unsigned k = 0; k < 8; k++)
		{
			int n = Vendor (TRUE, kReqLog, 0, 0, 0, 512);
			if (n <= 0) break;
			unsigned nCopy = s_nLog + (unsigned) n <= sizeof s_Log ? (unsigned) n : sizeof s_Log - s_nLog;
			memcpy (s_Log + s_nLog, m_pCtlBuf, nCopy);
			s_nLog += nCopy;
		}
		s_nLogReads++;
	}
	if (s_bBootDue)
	{
		s_bBootDue = FALSE;
		CLogger::Get ()->Write (From, LogNotice, "the relay is told to start its bootloader (teensyflash): link traffic stops");
		s_bLeaving = TRUE;		// MirrorShown/Relaying false from here: the boxes' packets are parked, not sent (T409)
		s_nLeavingAt = NowTicks ();
		// the answer may never come (the Teensy resets): a failure here is logged, the flag still waits for kLeaveSecs
		if (CKernelOptions::Get ()->GetAppOptionDecimal ("relayblignore", 0) != 0)
		{	// test only (relayblignore=1): the request is not sent, as if the relay had ignored it; the link comes back after kLeaveSecs
			CLogger::Get ()->Write (From, LogWarning, "relayblignore=1: the bootloader request is not sent (test of the %u s resume)", kLeaveSecs);
			return;
		}
		int n = Vendor (FALSE, kReqBootloader, 0x4C42, 0, 0, 0);
		CLogger::Get ()->Write (From, LogNotice, "bootloader request: %s (%d)", n < 0 ? "not answered" : "answered", n);
		return;
	}
	if (s_bLeaving)
	{
		// the relay did not leave (BL3, T411: no removal and no bootloader device seen): the link comes back rather than
		// staying hidden until the Pi restarts. teensyflash gives up at 15 s (kEnterSecs); a relay that does go is a new
		// CTeensyTarget, whose Start clears the flag. The box is shown again from the start (its computer may have given up
		// on requests that went unanswered meanwhile).
		if (NowTicks () - s_nLeavingAt < (u64) kLeaveSecs * CLOCKHZ) return;
		s_bLeaving = FALSE;
		CLogger::Get ()->Write (From, LogWarning, "the relay was still here %u s after the bootloader request: link traffic resumes%s",
					kLeaveSecs, m_bConnected && m_bBoxUp ? ", the box is shown again" : "");
		if (m_bConnected)
		{
			Disconnect ();
			if (m_bBoxUp) m_bUploadDue = TRUE;		// below: Upload and connect, as when the box comes up
		}
	}
	if (m_bTest) TestPoll ();
	CObUsb *pBox = m_pBox;
	if (m_uchTestParked != 0 && pBox != 0)
	{
		u64 nNow = NowTicks ();
		if (m_nTestParkedAt == 0) m_nTestParkedAt = nNow;
		else if (nNow - m_nTestParkedAt >= 1000)
		{
			u8 a = m_uchTestParked;
			m_uchTestParked = 0, m_nTestParkedAt = 0;
			pBox->PipeInNext (a);
		}
	}
	if (pBox != 0 && Mirror ())			// the box was up before the relay or the mode, or was given to it: read it now
	{
		if (!m_bHave) pBox->Wanted ();
		pBox->AttachLate ();			// (nothing unless it is due: Reattach, Wanted, Configure before a target)
	}
	if (m_bUploadDue && m_bHave && Mirror ())
	{
		m_bUploadDue = FALSE;
		if (!Upload ()) CLogger::Get ()->Write (From, LogError, "usb=mirror: the relay refused the box's descriptors");
	}
	OsGatePoll ();					// usbmidi=os: (also while the box is gone and the relay still shows it)
	if (m_bConnected && !Mirror ()) Disconnect ();			// the mode turned to CLASS COMPLIANT
	if (m_bConnected && !m_bBoxUp && (m_bLeaveNow || NowTicks () - m_nBoxGoneAt >= (u64) m_nHoldSecs * CLOCKHZ)) Disconnect ();
	if (!m_bConnected || m_bBoxUp) m_bLeaveNow = FALSE;
	if (!m_bConnected) return;
	EvWatch ();
	if (!m_bConnected) return;
	ControlPoll ();
	for (unsigned c = 0; c < m_Map.n_out; c++)
		while (m_nMidiHeld[c])
		{
			EnterCritical ();
			boolean bFits = m_nMidiHeld[c] && MidiCreditFits (c);
			if (bFits) { m_nCreditDue[c]++; m_nMidiHeld[c]--; }
			LeaveCritical ();
			if (!bFits) break;
		}
	for (unsigned c = 0; c < m_Map.n_out; c++)
	{
		// taken from the due ones (and counted as granted) before the request, so their packets can't come first
		const boolean bMidi = m_Map.l.ep[m_Map.ep_of_out[c]].role == mirror::kMidiOut;
		EnterCritical ();
		unsigned n = m_nCreditDue[c];
		if (n > kMaxCredits) n = kMaxCredits;
		m_nCreditDue[c] -= n;
		if (bMidi) m_nMidiOut[c] += n;
		LeaveCritical ();
		if (n == 0) continue;
		if (Vendor (FALSE, kReqCredit, m_Map.l.ep[m_Map.ep_of_out[c]].addr, n, 0, 0) >= 0) m_Stats.nCredits += n;
		else
		{
			EnterCritical ();			// not granted: due again
			m_nCreditDue[c] += n;
			if (bMidi) m_nMidiOut[c] -= m_nMidiOut[c] < n ? m_nMidiOut[c] : n;
			LeaveCritical ();
		}
	}
	if (pBox != 0) pBox->PipePoll ();				// a halted pipe: cleared and resubmitted
	u64 nNow = NowTicks ();
	if (nNow - m_nLastLog >= 10 * (u64) CLOCKHZ)
	{
		m_nLastLog = nNow;
		LogStats (CLogger::Get ());
		// the Teensy's own counters: how long the box's blocks waited there for the computer (the margin is 8 blocks,
		// about 1.2 ms), and its drops
		Stats ts;
		memset (&ts, 0, sizeof ts);
		int n = Vendor (TRUE, kReqStats, 0, 0, 0, sizeof ts);
		if (n >= (int) offsetof (Stats, isr1_max_us))	// a Teensy build before TR2c answers without the fields after isr2
		{
			memcpy (&ts, m_pCtlBuf, (unsigned) n);
			unsigned over1ms = 0, total = 0;
			for (unsigned i = 0; i < kHistBins; i++) { total += ts.hist_to_pc[i]; if (i >= 8) over1ms += ts.hist_to_pc[i]; }
			CLogger::Get ()->Write (From, LogNotice, "relay teensy: to-pc %u drop %u empty %u qmax %u worst %u us, >=1 ms %u of %u "
						"| to-pi %u drop %u worst %u us | ctl fwd %u ans %u late %u abort %u | resets pc %u link %u",
						ts.audio_to_pc, ts.audio_to_pc_drop, ts.audio_to_pc_empty, ts.q_to_pc_max, ts.worst_to_pc_us,
						over1ms, total, ts.audio_to_pi, ts.audio_to_pi_drop, ts.worst_to_pi_us, ts.ctl_forwarded,
						ts.ctl_answered, ts.ctl_late, ts.ctl_aborted, ts.pc_resets, ts.link_resets);
			if (n >= (int) offsetof (Stats, pc_in_gap))	// a Teensy build with the TR2c fields (older ones answer shorter)
				CLogger::Get ()->Write (From, LogNotice, "relay teensy: isr max link %u us pc %u us | link gap max %u us, "
							"hist <125 %u <250 %u <375 %u <500 %u <1ms %u <2ms %u more %u | dry %u nak polls %u | pc audio err %u",
							ts.isr1_max_us, ts.isr2_max_us, ts.link_gap_max_us, ts.link_gap_hist[0], ts.link_gap_hist[1],
							ts.link_gap_hist[2], ts.link_gap_hist[3],
							ts.link_gap_hist[4] + ts.link_gap_hist[5] + ts.link_gap_hist[6] + ts.link_gap_hist[7],
							ts.link_gap_hist[8] + ts.link_gap_hist[9] + ts.link_gap_hist[10] + ts.link_gap_hist[11] +
							ts.link_gap_hist[12] + ts.link_gap_hist[13] + ts.link_gap_hist[14], ts.link_gap_hist[15],
							ts.link_dry, ts.link_nak_polls, ts.pc_audio_err);
			if (n >= (int) offsetof (Stats, pc_idle_enter))	// a Teensy build with the TR5d fields
				CLogger::Get ()->Write (From, LogNotice, "relay teensy: pc gaps in max %u us <.25 %u <.5 %u <1 %u <2 %u <4 %u <8 %u <16 %u more %u"
							" | out max %u us <.25 %u <.5 %u <1 %u <2 %u <4 %u <8 %u <16 %u more %u",
							ts.pc_in_gap_max_us, ts.pc_in_gap[0], ts.pc_in_gap[1], ts.pc_in_gap[2], ts.pc_in_gap[3],
							ts.pc_in_gap[4], ts.pc_in_gap[5], ts.pc_in_gap[6], ts.pc_in_gap[7], ts.pc_out_gap_max_us,
							ts.pc_out_gap[0], ts.pc_out_gap[1], ts.pc_out_gap[2], ts.pc_out_gap[3], ts.pc_out_gap[4],
							ts.pc_out_gap[5], ts.pc_out_gap[6], ts.pc_out_gap[7]);
			if (n >= (int) offsetof (Stats, pc_idle_enter) + 8)
				CLogger::Get ()->Write (From, LogNotice, "relay teensy: computer IN idle entered %u, left %u | paced %u held %u empty %u",
							ts.pc_idle_enter, ts.pc_idle_leave, ts.pace_primed, ts.pace_held, ts.pace_empty);
		}
	}
}

void CTeensyTarget::LogStats (CLogger *pLogger)
{
	CTeensyTarget *p = s_pThis;
	if (p == 0) return;
	const TStats &s = p->m_Stats;
	const mirror::StreamStats &a = p->m_Stream.st;
	if (p->m_bTest)
		pLogger->Write (From, LogNotice, "relaytest: in sent %u | out from computer %u lost %u dup %u bad %u | pipe echo %u drop %u",
				p->m_nTestSent, p->m_nTestOut, p->m_nTestOutLost, p->m_nTestOutDup, p->m_nTestOutBad, p->m_nTestEcho,
				p->m_nTestEchoDrop);
	pLogger->Write (From, LogNotice, "relay %s box %s pc %s | in ch %u blocks %u sent %u drop %u | out ch %u blocks %u bad %u "
			"under %u over %u extra %u lost %u late %u anchor %u | ctl %u answered %u stall %u abort %u late %u | "
			"pipe in %u drop %u out %u drop %u credits %u | midi to-box %u full %u filtered %u to-pc %u drop %u keys %u nobox %u | ev %u silent %u reshow %u vendor fail %u connects %u pipe zlp %u",
			p->m_bConnected ? "shown" : "off", p->m_bBoxUp ? "up" : "none", p->m_bPcConfigured ? "configured" : "-",
			p->m_Stream.in_ch, a.in_blocks, a.in_sent, s.nAudioOutDrop, p->m_Stream.out_ch, a.out_blocks, a.out_bad,
			a.out_under, a.out_over, a.out_extra, p->m_Stream.extra.dropped, p->m_Stream.extra.late, a.anchor,
			s.nSetups, s.nAnswers, s.nStalls, s.nAborts, s.nLate, s.nPipeIn, s.nPipeInDrop, s.nPipeOut, s.nChanInDrop,
			s.nCredits, s.nMidiToBox, s.nMidiToBoxFull, (unsigned) (p->m_ToBoxFilter.DroppedPackets () + p->m_ToRytmPath.Filter ().DroppedPackets ()),
			s.nMidiToPc, s.nMidiToPcDrop, s.nMidiToKeys, s.nChanInMidi, s.nEvents, p->m_nEvSilent, p->m_nEvReshows, s.nVendorFail, s.nConnects, p->m_nPipeInZlp);
	if (p->m_nInfoLines < 12)		// the first ~2 minutes: what the link shares the bus with (the log ring holds ~1 min)
	{
		p->m_nInfoLines++;
		for (unsigned i = 0; i < p->m_Map.l.n; i++)
		{
			const mirror::Ep &e = p->m_Map.l.ep[i];
			pLogger->Write (From, LogNotice, "relay ep 0x%02x iface %u role %u type %u mps %u interval %u",
					e.addr, e.iface, e.role, e.attr & 3, e.mps, e.interval);
		}
		pLogger->Write (From, LogNotice, "relay box pipes: slot 0 (%s) slot 1 (%s)%s", CObUsb::PipeInfo (0),
				CObUsb::PipeInfo (1), p->m_bBoxPipes ? ", relayboxpipes=1" : "");
		unsigned nCfg = 0;
		const int nRelaySlot = CObUsb::RelaySlot ();
		const u8 *pCfg = nRelaySlot < 0 ? 0 : CObUsb::BoxConfig ((unsigned) nRelaySlot, &nCfg);
		for (unsigned o = 0; pCfg != 0 && o < nCfg; o += 64)	// the relay's box's configuration descriptor, 64 B a line
		{
			char Hex[129];
			unsigned n = nCfg - o < 64 ? nCfg - o : 64;
			for (unsigned i = 0; i < n; i++)
			{
				static const char D[] = "0123456789abcdef";
				Hex[2 * i] = D[pCfg[o + i] >> 4], Hex[2 * i + 1] = D[pCfg[o + i] & 15];
			}
			Hex[2 * n] = 0;
			pLogger->Write (From, LogNotice, "relay box cfg %03u: %s", o, Hex);
		}
	}
	{
		CString e;
		for (unsigned i = p->m_nEv > 8 ? p->m_nEv - 8 : 0; i < p->m_nEv; i++)
		{
			CString t;
			t.Format (" %c%u", p->m_EvKind[i % 8], p->m_EvMs[i % 8]);
			e.Append (t);
		}
		pLogger->Write (From, LogNotice, "relay out ring: level min %u max %u frames | computer OUT gaps max %u us, >=1 ms %u, >=2 ms %u"
				" | events (U under, O over, uptime ms):%s", a.lvl_min, a.lvl_max, p->m_nCompGapMax, p->m_nCompGap1,
				p->m_nCompGap2, (const char *) e);
		p->m_Stream.st.lvl_min = ~0u, p->m_Stream.st.lvl_max = 0;
		const SlipDetect *D[2] = {&p->m_SlipOut, &p->m_SlipIn};
		CString l[2];
		for (unsigned j = 0; j < 2; j++)
		{
			const SlipDetect &d = *D[j];
			l[j].Format ("crossings %u slips %u lost %u", d.crossings, d.slips, d.lost);
			for (unsigned i = d.ev_n > SlipDetect::kEvents ? d.ev_n - SlipDetect::kEvents : 0; i < d.ev_n; i++)
			{
				CString t;
				t.Format (" %d@%u", (int) d.ev_size[i % SlipDetect::kEvents], (unsigned) d.ev_ms[i % SlipDetect::kEvents]);
				l[j].Append (t);
			}
		}
		pLogger->Write (From, LogNotice, "relay slips (1/100 sample @ uptime ms): computer OUT %s | box IN %s",
				(const char *) l[0], (const char *) l[1]);
	}
	pLogger->Write (From, LogNotice, "relay link 0x02: done %u err %u (usb %u) max %u us, hist <125 %u <250 %u <500 %u <1ms %u "
			"<2ms %u <4ms %u <8ms %u more %u | busy max %u, drops in a row max %u",
			s.nLinkDone, s.nLinkErr, s.nLinkErrStatus, s.nLinkMaxUs, s.nLinkHist[0], s.nLinkHist[1], s.nLinkHist[2],
			s.nLinkHist[3], s.nLinkHist[4], s.nLinkHist[5], s.nLinkHist[6], s.nLinkHist[7], s.nBusyMax, s.nDropRunMax);
}

#endif	// CIRCLE_GADGET_MIRROR

#ifdef CIRCLE_GADGET_MIRROR
// ---- relaytest=1 -------------------------------------------------------------------------------------------------

static const u8 kTestDev[18] = {18, 1, 0x10, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x03, 0x00, 0x00, 0x01, 1, 2, 0, 1};

static unsigned TestConfig (u8 *c)
{
	unsigned n = 0;
	auto put = [&] (const u8 *b, unsigned len) { memcpy (c + n, b, len); n += len; };
	static const u8 Cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0xC0, 0};
	put (Cfg, sizeof Cfg);
	static const unsigned Mps[4] = {0, 32, 88, 256};
	static const u8 Pipes[4] = {0x02, 0x82, 0x01, 0x81};
	for (unsigned alt = 0; alt < 4; alt++)
	{
		const u8 If[9] = {9, 4, 0, (u8) alt, (u8) (alt ? 6 : 4), 0xFF, 0, 0, 0};
		put (If, sizeof If);
		for (unsigned k = 0; k < 4; k++) { const u8 E[7] = {7, 5, Pipes[k], 2, 0x00, 0x02, 0}; put (E, 7); }
		if (alt)
		{
			const u8 In[7] = {7, 5, 0x83, 3, (u8) (Mps[alt] & 0xFF), (u8) (Mps[alt] >> 8), 1};
			const u8 Out[7] = {7, 5, 0x03, 3, (u8) (Mps[alt] & 0xFF), (u8) (Mps[alt] >> 8), 1};
			put (In, 7);
			put (Out, 7);
		}
	}
	c[2] = (u8) n, c[3] = (u8) (n >> 8);
	return n;
}

static unsigned TestString (u8 *b, const char *s)
{
	unsigned n = 2;
	for (; *s; s++) { b[n++] = (u8) *s; b[n++] = 0; }
	b[0] = (u8) n, b[1] = 3;
	return n;
}

// the bench's block (relay_bench.py block()): u32 seq, then bytes (seq + i) & 0xFF, 256 bytes in all
static void TestPattern (u8 *b, u32 seq, unsigned n)
{
	memcpy (b, &seq, 4);
	for (unsigned i = 4; i < n; i++) b[i] = (u8) (seq + i);
}

// the 8-channel block the computer should get (seq nSeq), spread into the 12-channel layout FromBoxBlock cuts from
void CTeensyTarget::TestBlock (u8 *pBlk, u32 nSeq)
{
	u8 Want[256];
	TestPattern (Want, nSeq, sizeof Want);
	memset (pBlk, 0, ob::block_bytes (12));
	memcpy (pBlk, Want, ob::kHeaderBytes);
	for (int f = 0; f < ob::kFramesPerBlock; f++)
		memcpy (pBlk + ob::kHeaderBytes + 4 * 12 * f, Want + ob::kHeaderBytes + 4 * 8 * f, 4 * 8);
}

void CTeensyTarget::TestPoll (void)
{
	m_bTestFeeding = TRUE;
	if (!m_bHave)
	{
		u8 Cfg[mirror::kMaxConfig], Str[64];
		unsigned n = TestConfig (Cfg);
		if (!MirrorSetDescriptors (kTestDev, Cfg, n)) { m_bTestFeeding = FALSE; return; }
		static const u8 Lang[4] = {4, 3, 0x09, 0x04};
		MirrorSetString (0, Lang, 4);
		n = TestString (Str, "Sidecar bench"); MirrorSetString (1, Str, n);
		n = TestString (Str, "Relay bench box"); MirrorSetString (2, Str, n);
		MirrorBoxUp (TRUE);
		m_bTestFeeding = FALSE;
		return;
	}
	m_bTestFeeding = FALSE;
	if (!MirrorShown () || m_Stream.in_ch != 8) { m_nTestStart = 0; return; }
	u64 nNow = NowTicks ();
	if (m_nTestStart == 0) { m_nTestStart = nNow; m_nTestSent = 0; Step ("relaytest: the computer took alt 3, stream starts"); }
	if (m_bFeedIrq) return;				// relayfeed=irq: FromBoxBlock sends them
	u64 nDue = (nNow - m_nTestStart) * 48000 / 7 / CLOCKHZ;
	unsigned nBurst = 0;
	while (m_nTestSent < nDue && nBurst++ < 16)
	{
		u8 Blk[ob::block_bytes (12)];
		TestBlock (Blk, m_nTestSent);
		// not through FromBoxBlock: in test mode it drops the real box's blocks (TR1h: ~5 % of the computer's blocks
		// were the A4's audio, rytmob2 on this Pi; a flag set around this call still let a few through, TR1i)
		EnterCritical (IRQ_LEVEL);
		SendBoxBlock (Blk);
		LeaveCritical ();
		m_nTestSent++;
	}
	if (m_nTestSent + 64 < nDue) m_nTestSent = (u32) nDue;	// the loop was held: skip, the computer counts it as lost
	m_bTestFeeding = FALSE;
}

// the bench's control pattern: IN answers (wValue + i), OUT data must be (wValue * 3 + i)
boolean CTeensyTarget::TestControl (const u8 *s, const u8 *pData, int *pAnswer)
{
	u16 wValue = (u16) mirror::le16 (s + 2), wLength = (u16) mirror::le16 (s + 6);
	*pAnswer = 0;
	if (s[0] == 0x40 && s[1] == 0x7D)		// the Teensy's counters from zero (the bench, after its warm-up)
		return Vendor (FALSE, kReqStatsReset, 0, 0, 0, 0) >= 0;
	if (s[0] == 0xC0 && s[1] == 0x7E)		// the Teensy's own counters (relay::Stats), for a computer with no Pi network
	{
		int n = Vendor (TRUE, kReqStats, 0, 0, 0, sizeof (Stats) < wLength ? sizeof (Stats) : wLength);
		if (n <= 0) return FALSE;
		memcpy ((u8 *) pData, m_pCtlBuf, (unsigned) n);
		*pAnswer = n;
		return TRUE;
	}
	if (s[0] & 0x80)
	{
		u8 *a = (u8 *) pData;			// ControlPoll's Data buffer, copied into m_CtlData by the caller
		for (unsigned i = 0; i < wLength; i++) a[i] = (u8) (wValue + i);
		*pAnswer = wLength;
		return TRUE;
	}
	for (unsigned i = 0; i < wLength; i++) if (pData[i] != (u8) (wValue * 3 + i)) return FALSE;
	return TRUE;
}

void CTeensyTarget::TestOutBlock (const u8 *b, unsigned n)
{
	if (n != 256) { m_nTestOutBad++; return; }
	u32 seq;
	memcpy (&seq, b, 4);
	if (b[4] != (u8) (seq + 4) || b[255] != (u8) (seq + 255)) { m_nTestOutBad++; return; }
	m_nTestOut++;
	if (m_bTestOutSeen && seq < 16 && m_nTestOutNext > 1000) m_bTestOutSeen = FALSE;	// the computer's stream started again
	if (m_bTestOutSeen)
	{
		s32 d = (s32) (seq - m_nTestOutNext);
		if (d < 0) { m_nTestOutDup++; return; }
		m_nTestOutLost += (u32) d;
	}
	m_bTestOutSeen = TRUE;
	m_nTestOutNext = seq + 1;
}
#endif

#ifdef CIRCLE_GADGET_MIRROR
// relaytest: the made-up box's BOS and MS OS 2.0 set (relay_bench.py made_up_device: its bcdUSB 2.10 says a BOS is there),
// so the bench computer binds WinUSB to it with no INF; a real box has neither and gets none
boolean CTeensyTarget::TestExtras (void)
{
	static const char Guid[] = "{9E3D51A7-0B2C-4F86-A1D4-7C5E2B90F163}";
	static const char Name[] = "DeviceInterfaceGUIDs";
	u8 M[256];
	unsigned n = 10;
	memset (M, 0, sizeof M);
	M[0] = 10; M[4] = 0x00; M[5] = 0x00; M[6] = 0x03; M[7] = 0x06;	// set header, Windows 8.1+
	const u8 Compat[20] = {20, 0, 3, 0, 'W', 'I', 'N', 'U', 'S', 'B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	memcpy (M + n, Compat, 20); n += 20;
	unsigned start = n; n += 2;
	M[n++] = 4; M[n++] = 0; M[n++] = 7; M[n++] = 0;			// registry property, REG_MULTI_SZ
	unsigned nl = 2 * (sizeof Name);				// UTF-16 with one NUL
	M[n++] = (u8) nl; M[n++] = 0;
	for (unsigned i = 0; i < sizeof Name; i++) { M[n++] = (u8) Name[i]; M[n++] = 0; }
	unsigned dl = 2 * (sizeof Guid) + 2;				// UTF-16 with two NULs
	M[n++] = (u8) dl; M[n++] = 0;
	for (unsigned i = 0; i < sizeof Guid; i++) { M[n++] = (u8) Guid[i]; M[n++] = 0; }
	M[n++] = 0; M[n++] = 0;
	M[start] = (u8) (n - start); M[start + 1] = (u8) ((n - start) >> 8);
	M[8] = (u8) n; M[9] = (u8) (n >> 8);
	const u8 kCode = 0x22;
	const u8 Bos[33] = {5, 0x0F, 33, 0, 1, 28, 0x10, 5, 0, 0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65,
			    0x9D, 0x9E, 0x64, 0x8A, 0x9F, 0, 0, 3, 6, (u8) n, (u8) (n >> 8), kCode, 0};
	if (Vendor (FALSE, kReqSetExtra, 15, 0, Bos, sizeof Bos) < 0) return FALSE;
	return Vendor (FALSE, kReqSetExtra, 0xEE, kCode, M, n) >= 0;
}
#endif
