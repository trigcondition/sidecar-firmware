// pi/engine/sidecar_gadget.cpp - sidecar_gadget.h
#include "sidecar_gadget.h"

#include <circle/usb/usb.h>
#include <circle/util.h>

#include "ob_block.h"

void sidecar_tone_table (int period, double amp, int32_t *out);	// tones.cpp (hosted: libm)

static const char From[] = "sidecar";

CSidecarGadget *CSidecarGadget::s_pThis = 0;

// ---- descriptors -----------------------------------------------------------------------------------------------

enum { StrManufacturer = 1, StrProduct, StrSerial, StrAudio, StrMIDI, StrOutCh = 16, StrInCh = 32 };
static const char *const OutNames[12] =
	{"MAIN L", "MAIN R", "MAIN FX L", "MAIN FX R", "BD", "SD", "RS", "BT", "LT", "MT", "CH", "CY"};
static const char *const InNames[20] =
	{"MAIN L", "MAIN R", "BD", "SD", "RS", "BT", "LT", "MT", "CH", "CY", "AUDIO IN L", "AUDIO IN R",
	 "VOICE 1", "VOICE 2", "VOICE 3", "VOICE 4", "VOICE 5", "VOICE 6", "VOICE 7", "VOICE 8"};

static TUSBDeviceDescriptor s_Device =
{
	sizeof (TUSBDeviceDescriptor), DESCRIPTOR_DEVICE, 0x200,
	0xEF, 0x02, 0x01,				// interface association
	64, USB_GADGET_VENDOR_ID, USB_GADGET_DEVICE_ID_BASE + 3,	// 1209:0004 (P1 MIDI 0001, P2 probe 0002; 0003 a P3 build under a test name: CoreMIDI keeps a device's first name)
	0x0300, StrManufacturer, StrProduct, StrSerial, 1
};

static u8 s_Config[400];
static unsigned s_nConfig, s_nOffOut, s_nOffFb, s_nOffIn, s_nOffMIDIOut, s_nOffMIDIIn;

static void Put (const u8 *p, unsigned n)	{ memcpy (s_Config + s_nConfig, p, n); s_nConfig += n; }
#define D(...)	do { const u8 d_[] = {__VA_ARGS__}; Put (d_, sizeof d_); } while (0)
#define U16(v)	(u8) ((v) & 0xFF), (u8) (((v) >> 8) & 0xFF)
#define U32(v)	(u8) ((v) & 0xFF), (u8) (((v) >> 8) & 0xFF), (u8) (((v) >> 16) & 0xFF), (u8) (((v) >> 24) & 0xFF)

// UAC2 (Audio Device Class 2.0 4.7-4.10, the Teensy T1 layout): interfaces 0 AC, 1 AS OUT (EP 0x01, feedback 0x82),
// 2 AS IN (EP 0x83); one internal fixed 48 kHz clock 0x10; IT 0x01 (USB) -> OT 0x02, IT 0x03 -> OT 0x04 (USB); raw
// channels (bmChannelConfig 0) named by strings. USB MIDI 1.0: interfaces 3 AC, 4 MS (bulk 0x04 / 0x85), one cable.
static void BuildConfig (unsigned nIn, unsigned nOut, unsigned nMaxIn, unsigned nMaxOut)
{
	const unsigned Clock = 0x10, AcCs = 9 + 8 + 17 + 12 + 17 + 12, MsCs = 7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5;
	s_nConfig = 0;
	D (9, DESCRIPTOR_CONFIGURATION, 0, 0, 5, 1, 0, 0x80, 250);
	D (8, 11, 0, 3, 0x01, 0x00, 0x20, StrAudio);
	D (9, DESCRIPTOR_INTERFACE, 0, 0, 0, 0x01, 0x01, 0x20, 0);
	D (9, 0x24, 0x01, U16 (0x0200), 0x08, U16 (AcCs), 0x00);
	D (8, 0x24, 0x0A, Clock, 0x01, 0x05, 0, 0);
	D (17, 0x24, 0x02, 0x01, U16 (0x0101), 0, Clock, (u8) nOut, U32 (0), StrOutCh, U16 (0), 0);
	D (12, 0x24, 0x03, 0x02, U16 (0x0301), 0, 0x01, Clock, U16 (0), 0);
	D (17, 0x24, 0x02, 0x03, U16 (0x0201), 0, Clock, (u8) nIn, U32 (0), StrInCh, U16 (0), 0);
	D (12, 0x24, 0x03, 0x04, U16 (0x0101), 0, 0x03, Clock, U16 (0), 0);
	D (9, DESCRIPTOR_INTERFACE, 1, 0, 0, 0x01, 0x02, 0x20, 0);
	D (9, DESCRIPTOR_INTERFACE, 1, 1, 2, 0x01, 0x02, 0x20, 0);
	D (16, 0x24, 0x01, 0x01, 0x00, 0x01, U32 (1), (u8) nOut, U32 (0), StrOutCh);
	D (6, 0x24, 0x02, 0x01, 3, 24);
	s_nOffOut = s_nConfig;
	D (7, DESCRIPTOR_ENDPOINT, 0x01, 0x05, U16 (nMaxOut), 2);
	D (8, 0x25, 0x01, 0, 0, 0, 0, 0);
	s_nOffFb = s_nConfig;
	D (7, DESCRIPTOR_ENDPOINT, 0x82, 0x11, U16 (4), 4);
	D (9, DESCRIPTOR_INTERFACE, 2, 0, 0, 0x01, 0x02, 0x20, 0);
	D (9, DESCRIPTOR_INTERFACE, 2, 1, 1, 0x01, 0x02, 0x20, 0);
	D (16, 0x24, 0x01, 0x04, 0x00, 0x01, U32 (1), (u8) nIn, U32 (0), StrInCh);
	D (6, 0x24, 0x02, 0x01, 3, 24);
	s_nOffIn = s_nConfig;
	D (7, DESCRIPTOR_ENDPOINT, 0x83, 0x05, U16 (nMaxIn), 2);
	D (8, 0x25, 0x01, 0, 0, 0, 0, 0);
	// MIDI
	D (8, 11, 3, 2, 0x01, 0x03, 0x00, StrMIDI);
	D (9, DESCRIPTOR_INTERFACE, 3, 0, 0, 0x01, 0x01, 0x00, 0);
	D (9, 0x24, 0x01, U16 (0x0100), U16 (9), 1, 4);
	D (9, DESCRIPTOR_INTERFACE, 4, 0, 2, 0x01, 0x03, 0x00, 0);
	D (7, 0x24, 0x01, U16 (0x0100), U16 (MsCs));
	D (6, 0x24, 0x02, 0x01, 1, StrMIDI);		// IN jack, embedded (from the computer)
	D (6, 0x24, 0x02, 0x02, 2, 0);			// IN jack, external
	D (9, 0x24, 0x03, 0x01, 3, 1, 2, 1, StrMIDI);	// OUT jack, embedded (to the computer), from jack 2
	D (9, 0x24, 0x03, 0x02, 4, 1, 1, 1, 0);		// OUT jack, external, from jack 1
	s_nOffMIDIOut = s_nConfig;
	D (9, DESCRIPTOR_ENDPOINT, 0x04, 0x02, U16 (512), 0, 0, 0);
	D (5, 0x25, 0x01, 1, 1);
	s_nOffMIDIIn = s_nConfig;
	D (9, DESCRIPTOR_ENDPOINT, 0x85, 0x02, U16 (512), 0, 0, 0);
	D (5, 0x25, 0x01, 1, 3);
	s_Config[2] = s_nConfig & 0xFF;
	s_Config[3] = s_nConfig >> 8;
}

const void *CSidecarGadget::String (const char *p, size_t *pLength)
{
	size_t n = 2;
	for (u8 *q = m_StringBuffer + 2; *p && n < sizeof m_StringBuffer - 1; p++, n += 2) { *q++ = (u8) *p; *q++ = 0; }
	m_StringBuffer[0] = (u8) n;
	m_StringBuffer[1] = DESCRIPTOR_STRING;
	*pLength = n;
	return m_StringBuffer;
}

const void *CSidecarGadget::GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength)
{
	unsigned i = wValue & 0xFF;
	switch (wValue >> 8)
	{
	case DESCRIPTOR_DEVICE:
		if (i) break;
		*pLength = sizeof s_Device;
		return &s_Device;
	case DESCRIPTOR_CONFIGURATION:
		if (i) break;
		*pLength = s_nConfig;
		return s_Config;
	case DESCRIPTOR_STRING:
		if (i == 0)
		{
			static const u8 Lang[] = {4, DESCRIPTOR_STRING, 0x09, 0x04};
			*pLength = sizeof Lang;
			return Lang;
		}
		// "Sidecar | Expander (Pi)" (the Teensy: "Sidecar | Expander (Mini)"): CoreAudio and CoreMIDI name the device by it, so the
		// test tools pick it by its exact name (teensy/uac2/coreaudio_pick.py, UAC2_NAME)
		if (i == StrManufacturer) return String ("Sidecar", pLength);
		if (i == StrProduct || i == StrAudio || i == StrMIDI) return String (SIDECAR_NAME, pLength);
		if (i == StrSerial) return String ("PI4", pLength);
		if (i >= StrOutCh && i < StrOutCh + ChOut) return String (OutNames[i - StrOutCh], pLength);
		if (i >= StrInCh && i < StrInCh + ChIn) return String (InNames[i - StrInCh], pLength);
		break;
	default:
		break;
	}
	return nullptr;
}

// ---- endpoints -------------------------------------------------------------------------------------------------

CSidecarIsoEP::CSidecarIsoEP (const TUSBEndpointDescriptor *pDesc, CSidecarGadget *pGadget, TRole Role)
:	CDWUSBGadgetEndpoint (pDesc, pGadget), m_pSidecar (pGadget), m_Role (Role), m_bRunning (FALSE)
{
}

void CSidecarIsoEP::Arm (void)
{
	switch (m_Role)
	{
	case RoleOut:
		BeginTransfer (TransferDataOut, m_Buffer, (CSidecarGadget::MaxOut + 3) & ~3u);	// whole words for the DMA
		break;
	case RoleIn:
		BeginTransfer (TransferDataIn, m_Buffer, m_pSidecar->FillIn (m_Buffer));
		break;
	case RoleFeedback: {
		u32 v = m_pSidecar->Feedback (GetFrameNumber ());
		m_Buffer[0] = v; m_Buffer[1] = v >> 8; m_Buffer[2] = v >> 16; m_Buffer[3] = v >> 24;
		BeginTransfer (TransferDataIn, m_Buffer, 4);
		} break;
	}
}

void CSidecarIsoEP::Start (void)
{
	if (m_bRunning) return;
	m_bRunning = TRUE;
	Arm ();
}

void CSidecarIsoEP::Stop (void)
{
	if (!m_bRunning) return;
	m_bRunning = FALSE;
	StopIsoTransfer ();
}

void CSidecarIsoEP::OnTransferComplete (boolean bIn, size_t nLength)
{
	if (m_Role == RoleOut) m_pSidecar->TakeOut (m_Buffer, nLength);
	else if (m_Role == RoleIn) m_pSidecar->InSent ();
	else m_pSidecar->m_Stats.nFbSent++;
	if (m_bRunning) Arm ();
}

CSidecarMIDIEP::CSidecarMIDIEP (const TUSBEndpointDescriptor *pDesc, CSidecarGadget *pGadget)
:	CDWUSBGadgetEndpoint (pDesc, pGadget), m_pSidecar (pGadget), m_bActive (FALSE), m_bBusy (FALSE)
{
}

void CSidecarMIDIEP::OnActivate (void)
{
	m_bActive = TRUE;
	m_bBusy = FALSE;
	if (GetDirection () == DirectionIn) m_pSidecar->HostUp (TRUE);	// the computer configured us: the queue starts empty
	if (GetDirection () == DirectionOut) BeginTransfer (TransferDataOut, m_Buffer, BufSize);
	else Kick ();
}

void CSidecarMIDIEP::OnDeactivate (void)
{
	m_bActive = FALSE;
	m_bBusy = FALSE;
	if (GetDirection () == DirectionIn) m_pSidecar->HostUp (FALSE);	// reset or deconfigured: idle again
	CancelTransfer ();
}

void CSidecarMIDIEP::Kick (void)
{
	if (!m_bActive || m_bBusy || GetDirection () != DirectionIn) return;
	unsigned n = m_pSidecar->MIDIForHost (m_Buffer, InMax);
	if (n == 0) return;
	m_bBusy = TRUE;
	BeginTransfer (TransferDataIn, m_Buffer, n);
}

void CSidecarMIDIEP::OnTransferComplete (boolean bIn, size_t nLength)
{
	if (!bIn)
	{
		m_pSidecar->MIDIFromHost (m_Buffer, nLength);
		if (m_bActive) BeginTransfer (TransferDataOut, m_Buffer, BufSize);
		return;
	}
	m_bBusy = FALSE;
	Kick ();
}

// ---- the gadget ------------------------------------------------------------------------------------------------

static const unsigned TonePeriod[18] = {480, 400, 320, 300, 240, 200, 192, 160, 150, 128, 120, 100, 96, 80, 75, 64, 60, 50};

CSidecarGadget::CSidecarGadget (CInterruptSystem *pInterrupt)
:	CDWUSBGadget (pInterrupt, HighSpeed),
	m_pIso {0, 0, 0, 0}, m_pMIDIOut (0), m_pMIDIIn (0),
	m_uchOutAlt (0), m_uchInAlt (0), m_bMac (TRUE), m_bTest (FALSE), m_bFake (FALSE), m_bRytmMIDI (FALSE),
	m_nInHead (0), m_nInTail (0), m_nVoiceHead (0), m_nVoiceTail (0), m_nSrcFrames (0), m_nSrcTick (0),
	m_bSrcRytm (FALSE), m_nInPos (0), m_bInPrimed (FALSE), m_bInRunning (FALSE),
	m_nOutHead (0), m_nOutTail (0), m_bOutPriming (TRUE), m_nLevelQ8 (0), m_nSinkPos (0), m_nLastConsume (0),
	m_nLoopHead (0), m_nLoopTail (0), m_bLoopPriming (TRUE),
	m_bWinStarted (FALSE), m_bWinRytm (FALSE), m_nWinUFrames (0), m_nLastFrameNo (0), m_nWinQ16 (0),
	m_nFbBase (6 << 16),
	m_ToRytmFilter (true), m_ToHostFilter (false), m_bToRytmSysEx (FALSE),
	m_nLastIRQs (0), m_nLastTicks (0), m_nLastStamp (0)
{
	s_pThis = this;
	memset (&m_Stats, 0, sizeof m_Stats);
	m_Stats.nRingMin = m_Stats.nInMin = ~0u;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (m_nCntFrq));
	asm volatile ("mrs %0, cntpct_el0" : "=r" (m_nT0));
	s_Device.idVendor = USB_GADGET_VENDOR_ID;
	BuildConfig (ChIn, ChOut, MaxIn, MaxOut);
	for (unsigned c = 0; c < 20; c++)
	{
		m_nPhase[c] = 0;
		m_pTone[c] = 0;
	}
	for (unsigned c = 0; c + 2 < ChIn && c < 18; c++)
	{
		m_pTone[c] = new s32[TonePeriod[c]];
		sidecar_tone_table (TonePeriod[c], 0.25, m_pTone[c]);
	}
}

boolean CSidecarGadget::Start (void)
{
	if (!Initialize ()) return FALSE;
	m_nLastStamp = Now ();
	return TRUE;
}

u64 CSidecarGadget::Now (void)
{
	u64 t;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t));
	return t;
}

u64 CSidecarGadget::TimerQ16 (void)
{
	return (u64) ((unsigned __int128) (Now () - m_nT0) * (48000u << 16) / m_nCntFrq);
}

void CSidecarGadget::AddEndpoints (void)
{
	m_pIso[EPOut] = new CSidecarIsoEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffOut], this, CSidecarIsoEP::RoleOut);
	m_pIso[EPFeedback] = new CSidecarIsoEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffFb], this,
					       CSidecarIsoEP::RoleFeedback);
	m_pIso[EPIn] = new CSidecarIsoEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffIn], this, CSidecarIsoEP::RoleIn);
	m_pMIDIOut = new CSidecarMIDIEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffMIDIOut], this);
	m_pMIDIIn = new CSidecarMIDIEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffMIDIIn], this);
}

void CSidecarGadget::OnSuspend (void)
{
	EnterCritical ();				// task level (UpdatePlugAndPlay): the endpoints go, so does the computer
	HostUp (FALSE);
	LeaveCritical ();
	for (unsigned i = 1; i < 4; i++)
	{
		delete m_pIso[i];
		m_pIso[i] = 0;
	}
	delete m_pMIDIOut;
	delete m_pMIDIIn;
	m_pMIDIOut = m_pMIDIIn = 0;
}

int CSidecarGadget::OnClassOrVendorRequest (const TSetupData *pSetup, u8 *pData)
{
	if (!(pSetup->bmRequestType & REQUEST_IN)) return 0;		// SET CUR: one fixed rate, accepted
	unsigned n = 0;
	if ((pSetup->wIndex & 0xFF) == 0 && (pSetup->wIndex >> 8) == 0x10)
	{
		u8 uchCS = pSetup->wValue >> 8;
		if (pSetup->bRequest == 1 && uchCS == 1) { const u8 v[] = {U32 (48000)}; memcpy (pData, v, n = 4); }
		else if (pSetup->bRequest == 1 && uchCS == 2) { pData[0] = 1; n = 1; }
		else if (pSetup->bRequest == 2 && uchCS == 1)
		{
			const u8 v[] = {U16 (1), U32 (48000), U32 (48000), U32 (0)};
			memcpy (pData, v, n = 14);
		}
	}
	if (n == 0) { m_Stats.nBadReq++; return -1; }
	m_Stats.nReq++;
	return n > pSetup->wLength ? pSetup->wLength : n;
}

void CSidecarGadget::OutReset (void)
{
	m_nOutHead = m_nOutTail = 0;
	m_bOutPriming = TRUE;
	m_nLevelQ8 = 0;
	m_nLoopHead = m_nLoopTail = 0;
	m_bLoopPriming = TRUE;
}

boolean CSidecarGadget::OnSetInterface (u8 uchInterface, u8 uchAlt)
{
	if (uchInterface == 0 || uchInterface == 3 || uchInterface == 4) return uchAlt == 0;
	if (uchAlt > 1) return FALSE;
	if (uchInterface == 1)
	{
		m_uchOutAlt = uchAlt;
		OutReset ();
		m_bWinStarted = FALSE;
		if (uchAlt) { m_pIso[EPOut]->Start (); m_pIso[EPFeedback]->Start (); }
		else { m_pIso[EPOut]->Stop (); m_pIso[EPFeedback]->Stop (); }
		return TRUE;
	}
	if (uchInterface == 2)
	{
		m_uchInAlt = uchAlt;
		if (uchAlt)
		{
			// the IN ring trimmed to its target: the stream starts with a short delay, not the backlog
			while (m_nInHead - m_nInTail > InTarget) m_nInTail++;
			while (m_nVoiceHead - m_nVoiceTail > InTarget) m_nVoiceTail++;
			m_bInPrimed = FALSE;
			m_bInRunning = FALSE;
			m_nInPos = (TimerQ16 () >> 16) - Nominal;
			m_pIso[EPIn]->Start ();
		}
		else m_pIso[EPIn]->Stop ();
		return TRUE;
	}
	return FALSE;
}

int CSidecarGadget::OnGetInterface (u8 uchInterface)
{
	if (uchInterface == 1) return m_uchOutAlt;
	if (uchInterface == 2) return m_uchInAlt;
	return uchInterface <= 4 ? 0 : -1;
}

// ---- audio: the engine's side ----------------------------------------------------------------------------------

void CSidecarGadget::InPush (const s32 *pFrame)
{
	if (m_nInHead - m_nInTail >= Ring)
	{
		if (m_uchInAlt && m_bInRunning && !m_bTest) m_Stats.nInOver++;	// lost while the computer reads them
		m_nInTail++;					// the oldest goes (no reader yet: InSent trims)
	}
	memcpy (m_InRing[m_nInHead % Ring], pFrame, sizeof m_InRing[0]);
	m_nInHead++;
}

void CSidecarGadget::FromRytmBlock (const u8 *pBlock)
{
	for (int f = 0; f < ob::kFramesPerBlock; f++)
	{
		s32 Frame[12];
		for (int c = 0; c < 12; c++) Frame[c] = ob::block_sample (pBlock, ob::kChannels, f, c) >> 8;	// 24-bit
		InPush (Frame);
	}
	m_nSrcFrames += ob::kFramesPerBlock;
	m_nSrcTick = Now ();
	m_bSrcRytm = TRUE;
}

void CSidecarGadget::FromEngine (const s32 (*pFrames)[12], unsigned n)
{
	EnterCritical ();
	if (Now () - m_nSrcTick > m_nCntFrq / 50 || !m_bSrcRytm)	// the Rytm, when it streams, wins
	{
		for (unsigned f = 0; f < n; f++)
		{
			s32 Frame[12];
			for (int c = 0; c < 12; c++) Frame[c] = pFrames[f][c] >> 8;
			InPush (Frame);
		}
		m_nSrcFrames += n;
		m_nSrcTick = Now ();
		m_bSrcRytm = FALSE;
	}
	LeaveCritical ();
}

// One OUT block toward the Rytm (or the self-test): the computer's frames added, the voices kept for IN 13-20.
void CSidecarGadget::ToRytm (s32 (*F)[12], const s16 (*V)[8], unsigned n, boolean bTask)
{
	if (bTask) EnterCritical ();
	for (unsigned f = 0; f < n; f++)
	{
		if (ChIn > 12 && V != 0)
		{
			if (m_nVoiceHead - m_nVoiceTail >= Ring) m_nVoiceTail++;
			memcpy (m_VoiceRing[m_nVoiceHead % Ring], V[f], sizeof m_VoiceRing[0]);
			m_nVoiceHead++;
		}
		if (m_bTest || !m_uchOutAlt) continue;
		unsigned nLevel = m_nOutHead - m_nOutTail;
		if (m_bOutPriming)
		{
			if (nLevel < OutTarget) continue;
			m_bOutPriming = FALSE;
		}
		if (nLevel == 0)
		{
			m_Stats.nOutUnder++;
			m_bOutPriming = TRUE;
			continue;
		}
		const s32 *pMac = m_OutRing[m_nOutTail++ % Ring];
		if (!m_bMac) continue;
		for (unsigned c = 0; c < ChOut; c++)
		{
			s32 a = pMac[c] < 0 ? -pMac[c] : pMac[c];
			if ((unsigned) a > m_Stats.nMacPeak) m_Stats.nMacPeak = a;
			s64 v = (s64) F[f][c] + ((s64) pMac[c] << 8);
			F[f][c] = v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFFLL - 1 ? (s32) 0x80000000 : (s32) v;
		}
	}
	m_nLastConsume = Now ();
	if (bTask) LeaveCritical ();
}

// ---- audio: the computer's side (gadget IRQ) -------------------------------------------------------------------

void CSidecarGadget::TakeOut (const u8 *p, size_t nLength)
{
	m_Stats.nOutPkts++;
	if (nLength % (ChOut * 3)) m_Stats.nOutBad++;
	unsigned n = nLength / (ChOut * 3);
	if (n + 1 == Nominal) m_Stats.nOutHist[0]++;
	else if (n == Nominal) m_Stats.nOutHist[1]++;
	else if (n == Nominal + 1) m_Stats.nOutHist[2]++;
	m_Stats.nOutFrames += n;
	for (unsigned f = 0; f < n; f++, p += ChOut * 3)
	{
		s32 Frame[ChOut];
		for (unsigned c = 0; c < ChOut; c++)
			Frame[c] = (s32) ((p[3 * c] << 8) | (p[3 * c + 1] << 16) | (p[3 * c + 2] << 24)) >> 8;
		if (m_bTest)
		{
			if (m_nLoopHead - m_nLoopTail >= Ring) { m_Stats.nOutOver++; continue; }
			m_LoopRing[m_nLoopHead % Ring][0] = Frame[0];
			m_LoopRing[m_nLoopHead % Ring][1] = Frame[ChOut - 1];
			m_nLoopHead++;
			continue;
		}
		if (m_nOutHead - m_nOutTail >= Ring) { m_Stats.nOutOver++; continue; }
		memcpy (m_OutRing[m_nOutHead % Ring], Frame, sizeof Frame);
		m_nOutHead++;
	}
}

// IN packet sent (gadget IRQ). The first one of a stream: the computer polls IN some 10-20 ms after SET_INTERFACE,
// and the Rytm's frames meanwhile fill the ring (512 frames, then the oldest go). The ring is trimmed to its target
// here, once, so the stream goes on from InTarget frames behind the Rytm, not from a full ring that overflows on
// every block until 13-frame packets drain it (~120 ms of drops, counted as IN over, and 10 ms of extra latency).
void CSidecarGadget::InSent (void)
{
	m_Stats.nInPkts++;
	if (m_bInRunning) return;
	m_bInRunning = TRUE;
	while (m_nInHead - m_nInTail > InTarget) m_nInTail++;
	while (m_nVoiceHead - m_nVoiceTail > InTarget) m_nVoiceTail++;
}

static inline void Put24 (u8 *&p, s32 v)	{ *p++ = v; *p++ = v >> 8; *p++ = v >> 16; }

unsigned CSidecarGadget::FillIn (u8 *p)
{
	const boolean bSource = !m_bTest && Now () - m_nSrcTick < m_nCntFrq / 50;	// a source within 20 ms
	unsigned n = Nominal;
	if (bSource)
	{
		unsigned nLevel = m_nInHead - m_nInTail;
		if (nLevel < m_Stats.nInMin) m_Stats.nInMin = nLevel;
		if (nLevel > m_Stats.nInMax) m_Stats.nInMax = nLevel;
		if (!m_bInPrimed && nLevel >= InTarget) m_bInPrimed = TRUE;
		if (nLevel > InTarget + 7) n = Nominal + 1;
		else if (nLevel < InTarget - 7) n = Nominal - 1;
		m_nInPos = (TimerQ16 () >> 16) - Nominal;		// ready for a fall-back to the timer
	}
	else
	{
		// the generic timer: the frames it made due, 11..13
		u64 nDue = TimerQ16 () >> 16;
		s64 d = (s64) (nDue - m_nInPos);
		if (d > (s64) (8 * Nominal) || d < -(s64) (8 * Nominal)) { m_nInPos = nDue - Nominal; d = Nominal; }
		n = d < (s64) Nominal - 1 ? Nominal - 1 : d > (s64) Nominal + 1 ? Nominal + 1 : (unsigned) d;
		m_nInPos += n;
	}
	m_Stats.nInHist[n - (Nominal - 1)]++;
	m_Stats.nInFrames += n;

	for (unsigned f = 0; f < n; f++)
	{
		if (m_bTest)
		{
			// tones on 1..N-2, the loopbacks of OUT 1 and OUT 12 on N-1, N
			for (unsigned c = 0; c + 2 < ChIn; c++)
			{
				Put24 (p, m_pTone[c] ? m_pTone[c][m_nPhase[c]] : 0);
				if (m_pTone[c] && ++m_nPhase[c] == TonePeriod[c]) m_nPhase[c] = 0;
			}
			s32 a = 0, b = 0;
			unsigned nLevel = m_nLoopHead - m_nLoopTail;
			if (m_bLoopPriming && m_uchOutAlt && nLevel >= OutTarget) m_bLoopPriming = FALSE;
			if (!m_bLoopPriming)
			{
				if (nLevel == 0) { m_Stats.nOutUnder++; m_bLoopPriming = TRUE; }
				else
				{
					a = m_LoopRing[m_nLoopTail % Ring][0];
					b = m_LoopRing[m_nLoopTail % Ring][1];
					m_nLoopTail++;
				}
			}
			Put24 (p, a);
			Put24 (p, b);
			continue;
		}
		const s32 *pFrame = 0;
		if (bSource && m_bInPrimed)
		{
			if (m_nInHead != m_nInTail) pFrame = m_InRing[m_nInTail++ % Ring];
			else { m_Stats.nInUnder++; m_bInPrimed = FALSE; }
		}
		for (unsigned c = 0; c < 12; c++) Put24 (p, pFrame ? pFrame[c] : 0);
		if (ChIn > 12)
		{
			const s16 *pV = 0;
			if (pFrame)
			{
				if (m_nVoiceHead != m_nVoiceTail) pV = m_VoiceRing[m_nVoiceTail++ % Ring];
				else m_Stats.nVoiceUnder++;
			}
			for (unsigned v = 0; v < Voices; v++) Put24 (p, pV ? (s32) pV[v] << 8 : 0);
		}
	}
	return n * ChIn * 3;
}

// Every feedback packet (1 ms): the source's frames (the Rytm's, the stand-in's, else the generic timer's) against
// the host's microframes, and the OUT ring's level. Without a consumer of OUT the ring drains at the timer's rate.
unsigned CSidecarGadget::Feedback (unsigned nFrame)
{
	u64 nNow = Now ();
	const boolean bSource = !m_bTest && nNow - m_nSrcTick < m_nCntFrq / 50;
	u64 q = bSource ? m_nSrcFrames << 16 : TimerQ16 ();
	const unsigned nWindow = bSource ? 65536 : 8192;
	if (!m_bWinStarted || m_bWinRytm != bSource)
	{
		m_bWinStarted = TRUE;
		m_bWinRytm = bSource;
		m_nWinQ16 = q;
		m_nWinUFrames = 0;
	}
	else
	{
		m_nWinUFrames += (nFrame - m_nLastFrameNo) & 0x3FFF;
		if (m_nWinUFrames >= nWindow)
		{
			m_nFbBase = (unsigned) ((q - m_nWinQ16) / m_nWinUFrames);
			m_nWinQ16 = q;
			m_nWinUFrames = 0;
		}
	}
	m_nLastFrameNo = nFrame;

	// the ring this feedback serves: the loopback ring (test) or the OUT ring
	unsigned *pHead = m_bTest ? &m_nLoopHead : &m_nOutHead, *pTail = m_bTest ? &m_nLoopTail : &m_nOutTail;
	boolean *pPriming = m_bTest ? &m_bLoopPriming : &m_bOutPriming;
	u64 nDue = TimerQ16 () >> 16;
	boolean bConsumer = m_bTest ? m_uchInAlt : nNow - m_nLastConsume < m_nCntFrq / 50;
	if (!bConsumer)
	{
		s64 d = (s64) (nDue - m_nSinkPos);
		if (d < 0 || d > 4096) d = 0;
		for (s64 i = 0; i < d; i++)
		{
			unsigned nLevel = *pHead - *pTail;
			if (*pPriming && nLevel >= OutTarget) *pPriming = FALSE;
			if (*pPriming) break;
			if (nLevel == 0) { *pPriming = TRUE; break; }
			(*pTail)++;
		}
	}
	m_nSinkPos = nDue;

	unsigned nLevel = *pHead - *pTail;
	if (nLevel < m_Stats.nRingMin) m_Stats.nRingMin = nLevel;
	if (nLevel > m_Stats.nRingMax) m_Stats.nRingMax = nLevel;
	m_nLevelQ8 += ((s32) (nLevel << 8) - m_nLevelQ8) >> 3;
	s32 nCorr = 0;
	if (!*pPriming)
	{
		nCorr = (s32) (((s64) ((s32) OutTarget * 256 - m_nLevelQ8) * 65536) / (8000 * 256));
		s32 nLim = m_nFbBase / 200;
		if (nCorr > nLim) nCorr = nLim;
		if (nCorr < -nLim) nCorr = -nLim;
	}
	unsigned v = (m_nFbBase + nCorr) << 1;			// per packet (2 microframes)
	m_Stats.nFb = v;
	return v;
}

// ---- MIDI ------------------------------------------------------------------------------------------------------

void CSidecarGadget::ToHost (const u8 *p)
{
	switch (m_ToHost.Push (p))
	{
	case ToHostQueue<MIDIQueue>::kQueued:	m_Stats.nMidiToHost++; break;
	case ToHostQueue<MIDIQueue>::kFull:	m_Stats.nMidiFull++; break;	// a computer is there and not reading
	default:				break;				// no computer: not queued, not counted
	}
}

// the computer configured (or lost) the MIDI IN endpoint: IRQ level, or OnSuspend's task level with IRQs masked.
// Up empties the queue (no backlog of old messages for a new computer), Down too (and back to idle)
void CSidecarGadget::HostUp (boolean bUp)
{
	if (bUp) m_ToHost.Up ();
	else m_ToHost.Down ();
}

unsigned CSidecarGadget::MIDIForHost (u8 *pBuffer, unsigned nMax)
{
	return m_ToHost.Pop (pBuffer, nMax);
}

boolean (*g_pSidecarKeysTap) (const u8 *pPacket) = 0;

// the computer's packets (gadget IRQ) -> the keys tap (its notes) -> the Rytm-bound filter -> the queue core 0's loop
// sends from
void CSidecarGadget::MIDIFromHost (const u8 *pBuffer, size_t nLength)
{
	for (size_t i = 0; i + 4 <= nLength; i += 4)
	{
		if (pBuffer[i] == 0) continue;				// padding
		m_Stats.nMidiFromHost++;
		if (g_pSidecarKeysTap != 0 && (*g_pSidecarKeysTap) (pBuffer + i)) continue;
		m_ToRytmFilter.Feed (pBuffer + i, [this] (const u8 *p)
		{
			u32 w;
			memcpy (&w, p, 4);
			if (!m_ToRytm.push (w)) m_Stats.nMidiFull++;
		});
	}
}

// the Rytm's packets (xHCI IRQ, or the fake loop with IRQs masked) -> the computer-bound filter -> the IN queue
void CSidecarGadget::FromRytmMIDI (const u8 *pPacket)
{
	if (!m_ToHost.IsUp () && !m_ToHostFilter.InSysEx ()) return;	// no computer: nothing to filter, queue or kick
	m_ToHostFilter.Feed (pPacket, [this] (const u8 *p) { ToHost (p); });
	if (m_pMIDIIn) m_pMIDIIn->Kick ();
}

void CSidecarGadget::MIDIToComputer (u32 nPacket)
{
	if (!m_ToHost.IsUp ()) return;			// no computer (a plain read: the flag only changes at IRQ level)
	u8 p[4];
	memcpy (p, &nPacket, 4);
	EnterCritical ();				// the queue's other producer is the xHCI IRQ (FromRytmMIDI)
	ToHost (p);
	LeaveCritical ();
}

boolean CSidecarGadget::ForRytm (u32 *pPacket)
{
	if (!m_ToRytm.pop (pPacket)) return FALSE;
	u8 cin = *pPacket & 0x0F, b1 = (*pPacket >> 8) & 0xFF;
	if (cin == 0x4 && b1 == 0xF0) m_bToRytmSysEx = TRUE;
	else if (cin == 0x5 || cin == 0x6 || cin == 0x7) { if (m_bToRytmSysEx || b1 == 0xF7) m_bToRytmSysEx = FALSE; }
	m_Stats.nMidiToRytm++;
	return TRUE;
}

void CSidecarGadget::Poll (void)
{
	UpdatePlugAndPlay ();
	// :midifake=on without the Rytm's MIDI: what would go to the Rytm comes back as if the Rytm had sent it
	if (m_bFake && !m_bRytmMIDI)
	{
		u32 w;
		for (unsigned k = 0; k < 256 && ForRytm (&w); k++)
		{
			u8 p[4];
			memcpy (p, &w, 4);
			EnterCritical ();
			FromRytmMIDI (p);
			LeaveCritical ();
		}
	}
	else if (!m_bRytmMIDI)
	{
		u32 w;
		while (ForRytm (&w)) m_Stats.nMidiNoRytm++;		// nowhere to go: counted
	}
}

boolean CSidecarGadget::Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	if (strcmp (p, "mac=on") == 0 || strcmp (p, "mac=off") == 0) m_bMac = p[5] == 'n';
	else if (strcmp (p, "uac2test=on") == 0 || strcmp (p, "uac2test=off") == 0)
	{
		EnterCritical ();
		m_bTest = p[10] == 'n';
		OutReset ();
		m_bWinStarted = FALSE;
		LeaveCritical ();
	}
	else if (strcmp (p, "midifake=on") == 0 || strcmp (p, "midifake=off") == 0) m_bFake = p[10] == 'n';
	else if (strcmp (p, "mac") != 0 && strcmp (p, "sidecar") != 0) return FALSE;
	L->Write (From, LogNotice, "mac %s (the computer's OUT %s the Rytm), uac2test %s, midifake %s, %u in / %u out",
		  m_bMac ? "on" : "off", m_bMac ? "added to what goes to" : "dropped before", m_bTest ? "on" : "off",
		  m_bFake ? "on" : "off", ChIn, ChOut);
	return TRUE;
}

void CSidecarGadget::LogStats (CLogger *pLogger)
{
	unsigned nIRQs;
	u64 nTicks, nNow = Now ();
	GetIRQStats (&nIRQs, &nTicks);
	u64 nSpan = nNow - m_nLastStamp;
	if (nSpan == 0) return;
	unsigned nRate = (unsigned) ((u64) (nIRQs - m_nLastIRQs) * m_nCntFrq / nSpan);
	unsigned nPpm = (unsigned) ((nTicks - m_nLastTicks) * 1000000 / nSpan);
	m_nLastIRQs = nIRQs;
	m_nLastTicks = nTicks;
	m_nLastStamp = nNow;
	u64 nMaxGap, nMaxHandler;
	unsigned nLong;
	TakeIRQGaps (&nMaxGap, &nMaxHandler, &nLong, m_nCntFrq * 300 / 1000000);	// gaps over 300 us
	u64 nLate = m_pIso[EPIn] ? m_pIso[EPIn]->LateStamp () : 0;
	unsigned nLateAgo = nLate ? (unsigned) ((nNow - nLate) * 1000 / m_nCntFrq) : 0;	// ms
	EnterCritical ();
	TStats s = m_Stats;
	m_Stats.nRingMin = m_Stats.nInMin = ~0u;
	m_Stats.nRingMax = m_Stats.nInMax = 0;
	m_Stats.nMacPeak = 0;
	LeaveCritical ();
	const boolean bSource = (s64) (nNow - m_nSrcTick) < (s64) (m_nCntFrq / 50);	// the IRQ may set it after nNow
	unsigned nFbmHz = (unsigned) ((u64) s.nFb * 4000 * 1000 / 65536);
	pLogger->Write (From, LogNotice,
		"%s alt %u/%u mac %s pk %u | out %u pkts (11/12/13 %u/%u/%u) bad %u over %u under %u ring %u..%u | "
		"in %u pkts (11/12/13 %u/%u/%u) ring %u..%u under %u over %u vunder %u | fb %u.%03u | late %u/%u/%u incompl %u/%u/%u | "
		"midi from %u to-mac %u to-rytm %u full %u norytm %u drop %u/%u mac-midi %s (up %u) | irq %u/s %u.%02u %% gap max %u us handler max %u us >300us %u, last late %u ms ago",
		m_bTest ? "test" : bSource ? (m_bSrcRytm ? "rytm" : "standin") : "timer", m_uchOutAlt, m_uchInAlt,
		m_bMac ? "on" : "off", s.nMacPeak,
		s.nOutPkts, s.nOutHist[0], s.nOutHist[1], s.nOutHist[2], s.nOutBad, s.nOutOver, s.nOutUnder,
		s.nRingMin == ~0u ? 0 : s.nRingMin, s.nRingMax,
		s.nInPkts, s.nInHist[0], s.nInHist[1], s.nInHist[2], s.nInMin == ~0u ? 0 : s.nInMin, s.nInMax, s.nInUnder, s.nInOver,
		s.nVoiceUnder, nFbmHz / 1000, nFbmHz % 1000,
		m_pIso[EPOut] ? m_pIso[EPOut]->Late () : 0, m_pIso[EPFeedback] ? m_pIso[EPFeedback]->Late () : 0,
		m_pIso[EPIn] ? m_pIso[EPIn]->Late () : 0,
		m_pIso[EPOut] ? m_pIso[EPOut]->Incomplete () : 0, m_pIso[EPFeedback] ? m_pIso[EPFeedback]->Incomplete () : 0,
		m_pIso[EPIn] ? m_pIso[EPIn]->Incomplete () : 0,
		s.nMidiFromHost, s.nMidiToHost, s.nMidiToRytm, s.nMidiFull, s.nMidiNoRytm,
		m_ToRytmFilter.DroppedMessages (), m_ToHostFilter.DroppedMessages (),
		m_ToHost.IsUp () ? "up" : "none", m_ToHost.Ups (), nRate, nPpm / 10000, nPpm / 100 % 100,
		(unsigned) (nMaxGap * 1000000 / m_nCntFrq), (unsigned) (nMaxHandler * 1000000 / m_nCntFrq), nLong, nLateAgo);
}
