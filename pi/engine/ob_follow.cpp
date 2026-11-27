//
// ob_follow.cpp - the A4's Overbridge stream beside the Rytm's: see ob_follow.h. The sequence on the wire is CObUsb's (alt 0, then alt 3 on interfaces 1
// and 2, OUT paced by IN), which is also what the A4's firmware took from a simulated host in an emulator, with no vendor request.
//
#include "ob_follow.h"
#include "ob_usb.h"			// CountDetach, UsbRequests
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usb.h>
#include <circle/usb/xhciendpoint.h>
#include <circle/koptions.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/new.h>

#include "ob_block.h"
#include "a4_in.h"
#include "device/a4.h"

using core::device::kA4InChannels;
using core::device::kA4OutChannels;

static const char From[] = "a4ob";

static const unsigned kInBlk = ob::block_bytes (kA4InChannels);		// 256
static const unsigned kOutBlk = ob::block_bytes (kA4OutChannels);	// 200
static const unsigned kBuf = (kInBlk > kOutBlk ? kInBlk : kOutBlk) + 64;

static u8 *s_InBuf[CObFollow::URBs], *s_OutBuf[CObFollow::URBs];	// DMA, above MEM_KERNEL_END: heap blocks, 64 B aligned, once (ob_usb.cpp)

CObFollow *CObFollow::s_pThis = 0;
volatile boolean CObFollow::s_bOn = FALSE;

CUSBFunction *CObFollow::Create (CUSBFunction *pParent)
{
	const TUSBDeviceDescriptor *pDesc = pParent->GetDevice ()->GetDeviceDescriptor ();
	if (pDesc->idVendor != core::device::kA4Vid || pDesc->idProduct != core::device::kA4Pid) return 0;
	if (CKernelOptions::Get ()->GetAppOptionDecimal ("a4", 0) == 0) return 0;
	if (core::device::active ().id == core::device::kIdA4) return 0;	// the A4 is the box in use: CObUsb's
	if (s_pThis != 0)
	{
		CLogger::Get ()->Write (From, LogWarning, "an A4 already streams");
		return 0;
	}
	return new CObFollow (pParent);
}

CObFollow::CObFollow (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pInDesc (0), m_pOutDesc (0), m_pIn (0), m_pOut (0), m_pInURB {}, m_pOutURB {}, m_bInBusy {}, m_bOutBusy {},
	m_nInPending (0), m_nOutPending (0), m_bStreaming (FALSE), m_nCredit (0), m_nOutCounter (0), m_nExpectIn (0), m_bHaveIn (FALSE),
	m_nInBlocks (0), m_nInGaps (0), m_nInLost (0), m_nInBad (0), m_nInErrors (0), m_nOutErrors (0), m_nSubmitFail (0),
	m_nPrevIn (0), m_nSecs (0), m_nLastInUs (0), m_nLastOutUs (0), m_bConfigured (FALSE)
{
	s_pThis = this;
}

// As ~CObUsb: a box that left has its xHC slot freed already (~CXHCIUSBDevice ran first), so its URBs in flight are never
// completed and are freed here; after a failed Configure (the slot still runs) one in flight is left allocated.
CObFollow::~CObFollow (void)
{
	s_pThis = 0;
	m_bStreaming = FALSE;
	unsigned nFreed = 0, nKept = 0;
	for (unsigned i = 0; i < URBs; i++)
	{
		CUSBRequest *pURB[2] = {m_pInURB[i], m_pOutURB[i]};
		const boolean bBusy[2] = {m_bInBusy[i], m_bOutBusy[i]};
		for (unsigned d = 0; d < 2; d++)
		{
			if (pURB[d] == 0) continue;
			if (!bBusy[d] || m_bConfigured) { delete pURB[d]; nFreed += bBusy[d]; }
			else nKept++;
		}
		m_pInURB[i] = m_pOutURB[i] = 0;
	}
	delete m_pOut;
	delete m_pIn;
	m_pOut = m_pIn = 0;
	CObUsb::CountDetach (m_bConfigured, nFreed, nKept);
	CLogger::Get ()->Write (From, LogNotice, "%s: %u URBs in flight freed, %u kept (USB requests now %u)",
				m_bConfigured ? "box gone" : "not configured", nFreed, nKept, CObUsb::UsbRequests ());
}

boolean CObFollow::Configure (void)
{
	CLogger::Get ()->Write (From, LogNotice, "Analog Four MKII (1935:0b47) claimed");
	const TUSBConfigurationDescriptor *pCfg = GetDevice ()->GetConfigurationDescriptor ();
	if (pCfg != 0)
	{
		const u8 *p = (const u8 *) pCfg, *pEnd = p + pCfg->wTotalLength;
		int nIface = -1, nAlt = -1;
		for (; p + 2 <= pEnd && p[0] >= 2 && p + p[0] <= pEnd; p += p[0])
		{
			if (p[1] == DESCRIPTOR_INTERFACE && p[0] >= sizeof (TUSBInterfaceDescriptor))
			{
				nIface = ((const TUSBInterfaceDescriptor *) p)->bInterfaceNumber;
				nAlt = ((const TUSBInterfaceDescriptor *) p)->bAlternateSetting;
			}
			else if (p[1] == DESCRIPTOR_ENDPOINT && p[0] >= sizeof (TUSBEndpointDescriptor) && nAlt == ob::kAlt)
			{
				const TUSBEndpointDescriptor *e = (const TUSBEndpointDescriptor *) p;
				unsigned nMps = e->wMaxPacketSize & 0x7FF;
				if ((e->bmAttributes & 3) != 3) continue;
				if (nIface == ob::kInIface && e->bEndpointAddress == ob::kInEp && nMps == kInBlk) m_pInDesc = e;
				if (nIface == ob::kOutIface && e->bEndpointAddress == ob::kOutEp && nMps == kOutBlk) m_pOutDesc = e;
			}
		}
	}
	if (m_pInDesc == 0 || m_pOutDesc == 0)
	{
		CLogger::Get ()->Write (From, LogError, "no interrupt endpoints 0x83/0x03 (%u/%u B) at alt 3", kInBlk, kOutBlk);
		return FALSE;
	}

	CUSBHostController *pHost = GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)		// alt 0 first, as CObUsb::Configure
	{
		pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 0, nIface, 0, 0);
	}
	CTimer::Get ()->MsDelay (100);
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		if (pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, ob::kAlt, nIface, 0, 0) < 0)
		{
			CLogger::Get ()->Write (From, LogError, "SET_INTERFACE %u alt %u failed", (unsigned) nIface, (unsigned) ob::kAlt);
			return FALSE;
		}
	}

	m_pIn = new CUSBEndpoint (GetDevice (), m_pInDesc);
	m_pOut = new CUSBEndpoint (GetDevice (), m_pOutDesc);
	if (!m_pIn->GetXHCIEndpoint ()->IsValid () || !m_pOut->GetXHCIEndpoint ()->IsValid ())	// the controller's answer on the second stream's bandwidth
	{
		CLogger::Get ()->Write (From, LogError, "the host controller refused the endpoints: no second stream");
		return FALSE;
	}

	if (s_InBuf[0] == 0)
	{
		for (unsigned i = 0; i < URBs; i++)
		{
			s_InBuf[i] = (u8 *) (((uintptr) new u8[kBuf + 64] + 63) & ~(uintptr) 63);
			s_OutBuf[i] = (u8 *) (((uintptr) new u8[kBuf + 64] + 63) & ~(uintptr) 63);
			memset (s_OutBuf[i], 0, kOutBlk);
		}
		CKernelOptions *pOpt = CKernelOptions::Get ();
		a4in::Init (pOpt->GetAppOptionDecimal ("a4ch", core::device::kA4Main), pOpt->GetAppOptionDecimal ("a4att", 6));
	}
	for (unsigned i = 0; i < URBs; i++)
	{
		m_pInURB[i] = new CUSBRequest (m_pIn, s_InBuf[i], kInBlk);
		m_pInURB[i]->SetCompletionRoutine (InStub, (void *) (uintptr) i, this);
		m_pOutURB[i] = new CUSBRequest (m_pOut, s_OutBuf[i], kOutBlk);
		m_pOutURB[i]->SetCompletionRoutine (OutStub, (void *) (uintptr) i, this);
	}

	EnterCritical (IRQ_LEVEL);
	s_bOn = TRUE;
	m_nLastInUs = m_nLastOutUs = CTimer::GetClockTicks ();
	m_InHalt.halted = m_OutHalt.halted = false;
	m_bStreaming = TRUE;
	boolean bOK = TRUE;
	for (unsigned i = 0; i < URBs && bOK; i++) bOK = SubmitIn (i);
	LeaveCritical ();
	if (!bOK)
	{
		CLogger::Get ()->Write (From, LogError, "cannot submit the first URBs");
		return FALSE;
	}
	CLogger::Get ()->Write (From, LogNotice, "alt 3 selected: its MAIN into the PRE pair, its OUT silent");
	m_bConfigured = TRUE;
	return TRUE;
}

boolean CObFollow::SubmitIn (unsigned i)
{
	CUSBRequest *pURB = m_pInURB[i];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	m_bInBusy[i] = TRUE;
	m_nInPending++;
	if (GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
	m_bInBusy[i] = FALSE;
	m_nInPending--;
	m_nSubmitFail++;
	return FALSE;
}

boolean CObFollow::SubmitOut (unsigned i)
{
	CUSBRequest *pURB = m_pOutURB[i];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	m_bOutBusy[i] = TRUE;
	m_nOutPending++;
	if (GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
	m_bOutBusy[i] = FALSE;
	m_nOutPending--;
	m_nSubmitFail++;
	return FALSE;
}

void CObFollow::InStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	if (pContext != s_pThis) return;		// a late event after unplug
	((CObFollow *) pContext)->InDone ((unsigned) (uintptr) pParam, pURB);
}

void CObFollow::OutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	if (pContext != s_pThis) return;
	((CObFollow *) pContext)->OutDone ((unsigned) (uintptr) pParam, pURB);
}

// The xHCI interrupt, core 0.
void CObFollow::InDone (unsigned i, CUSBRequest *pURB)
{
	m_nLastInUs = CTimer::GetClockTicks ();
	m_bInBusy[i] = FALSE;
	m_nInPending--;
	const u8 *pBlk = s_InBuf[i];
	boolean bOK = pURB->GetStatus () != 0;
	unsigned nLen = bOK ? pURB->GetResultLength () : 0;
	InvalidateDataCacheRange ((uintptr) pBlk, kBuf);
	if (bOK) m_InHalt.Ok ();
	if (!bOK) m_nInErrors++, m_InHalt.Fail (m_nLastInUs);	// the endpoint halts on an error: no resubmit until PollHalt
	else if (nLen < kInBlk || !ob::block_from_device (pBlk)) m_nInBad++;
	else
	{
		u16 c = ob::block_counter (pBlk);
		if (m_bHaveIn && c != m_nExpectIn)		// a lost block: the loop takes it up, as it takes up the crystals
		{
			m_nInGaps++;
			m_nInLost += (u16) (c - m_nExpectIn) / ob::kFramesPerBlock;
		}
		m_bHaveIn = TRUE;
		m_nExpectIn = c + ob::kFramesPerBlock;
		m_nInBlocks++;
		a4in::In (pBlk);
		m_nCredit++;
	}
	if (m_bStreaming && !m_InHalt.halted) SubmitIn (i);
	PumpOut ();
}

void CObFollow::OutDone (unsigned i, CUSBRequest *pURB)
{
	m_nLastOutUs = CTimer::GetClockTicks ();
	m_bOutBusy[i] = FALSE;
	m_nOutPending--;
	if (pURB->GetStatus () == 0)
	{
		m_nOutErrors++;
		m_OutHalt.Fail (m_nLastOutUs);
		return;
	}
	m_OutHalt.Ok ();
	PumpOut ();
}

// One silent block for each block that came in
void CObFollow::PumpOut (void)
{
	if (m_nCredit > 2 * URBs) m_nCredit = 2 * URBs;
	while (m_bStreaming && !m_OutHalt.halted && m_nCredit != 0)
	{
		unsigned i = 0;
		while (i < URBs && m_bOutBusy[i]) i++;
		if (i == URBs) break;
		ob::write_header (s_OutBuf[i], m_nOutCounter);
		if (!SubmitOut (i)) break;
		m_nOutCounter += ob::kFramesPerBlock;
		m_nCredit--;
	}
}

void CObFollow::LogStats (CLogger *pLogger)
{
	CObFollow *p = s_pThis;
	if (p == 0 || !p->m_bStreaming) return;
	EnterCritical (IRQ_LEVEL);					// IN URBs a failed submit left idle, as CObUsb::Log
	for (unsigned i = 0; i < URBs; i++)
	{
		if (!p->m_InHalt.halted && !p->m_bInBusy[i]) p->SubmitIn (i);
	}
	p->PumpOut ();
	LeaveCritical ();
	if (++p->m_nSecs < 10) return;
	unsigned nIn = p->m_nInBlocks;
	a4in::Stats s = a4in::Read ();
	pLogger->Write (From, LogNotice, "IN %u/s %u gap %u lost %u bad %u err %u rs %u | OUT err %u rs %u sf %u | fill %d follow %d ppm under %u low %u high %u full %u",
			(nIn - p->m_nPrevIn) / p->m_nSecs, nIn, (unsigned) p->m_nInGaps, (unsigned) p->m_nInLost, (unsigned) p->m_nInBad, (unsigned) p->m_nInErrors, p->m_InHalt.resets,
			(unsigned) p->m_nOutErrors, p->m_OutHalt.resets, (unsigned) p->m_nSubmitFail, s.fill, s.ppm, s.under, s.low, s.high, s.full);
	p->m_nPrevIn = nIn;
	p->m_nSecs = 0;
}

unsigned CObFollow::s_nHaltResets[2], CObFollow::s_nHaltRefused, CObFollow::s_nHaltHeld;

boolean CObFollow::PollHalt (void)
{
	CObFollow *p = s_pThis;
	if (p == 0 || !p->m_bStreaming) return FALSE;
	unsigned now = CTimer::GetClockTicks ();
	EpFails f;					// IN and OUT failed together: a likely unplug, held (EpBox, as CObUsb::BoxWait)
	EnterCritical (IRQ_LEVEL);			// the IRQ writes erred and fail_us: both read as one
	f.Add (p->m_InHalt.erred, p->m_InHalt.fail_us);
	f.Add (p->m_OutHalt.erred, p->m_OutHalt.fail_us);
	LeaveCritical ();
	int w = p->m_Box.Wait (now, f);
	if (w == 3)
	{
		s_nHaltHeld++;
		if (p->m_Box.Log (now))
			CLogger::Get ()->Write (From, LogWarning, "IN and OUT failed together: a likely unplug, their resets "
						"held up to %u ms for the hub (held %u; %u lines left out)",
						EpBox::kGraceUs / 1000, p->m_Box.held, p->m_Box.TakeQuiet ());
	}
	if (w != 0) return FALSE;
	return p->Recover (TRUE, now) || p->Recover (FALSE, now);
}

// as CObUsb::Recover (no mirror here); TRUE: a reset was sent
boolean CObFollow::Recover (boolean bIn, unsigned nNowUs)
{
	EpHalt<CXHCIEndpoint> &h = bIn ? m_InHalt : m_OutHalt;
	CXHCIEndpoint *pX = (bIn ? m_pIn : m_pOut)->GetXHCIEndpoint ();
	const char *pDir = bIn ? "IN" : "OUT";
	EnterCritical (IRQ_LEVEL);
	if (!h.halted && (bIn ? m_nInPending : m_nOutPending) != 0
	    && EpHalt<CXHCIEndpoint>::Quiet (nNowUs, bIn ? m_nLastInUs : m_nLastOutUs)) h.Stall ();
	LeaveCritical ();
	const u8 uchAddr = bIn ? ob::kInEp : ob::kOutEp;
	int r = h.Reset (pX, nNowUs, [this, uchAddr] {
		GetHost ()->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_ENDPOINT, CLEAR_FEATURE, ENDPOINT_HALT,
					    uchAddr, 0, 0);
	});
	if (r == 0) return FALSE;
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
	if (h.Finish (pX))
	{
		for (unsigned i = 0; i < URBs; i++) (bIn ? m_bInBusy : m_bOutBusy)[i] = FALSE;
		(bIn ? m_nInPending : m_nOutPending) = 0;
	}
	(bIn ? m_nLastInUs : m_nLastOutUs) = nNowUs;
	if (bIn)
	{
		for (unsigned i = 0; i < URBs; i++) if (!m_bInBusy[i]) SubmitIn (i);
	}
	else PumpOut ();
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
	return r != 2;
}

void CObFollow::Stop (void)
{
	CObFollow *p = s_pThis;
	if (p == 0) return;
	p->m_bStreaming = FALSE;
	unsigned t0 = CTimer::GetClockTicks ();
	while ((p->m_nInPending || p->m_nOutPending) && CTimer::GetClockTicks () - t0 < 50000) {}
	CUSBHostController *pHost = p->GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		pHost->ControlMessage (p->GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 0, nIface, 0, 0);
	}
}

void CObFollow::EmergencyStop (void)
{
	if (s_pThis != 0) s_pThis->m_bStreaming = FALSE;
}
