// pi/engine/tests/test_midi_rxqueue.cpp - the USB MIDI host's IN requests (pi/circle/circle-midi-rxqueue.patch): Circle's
// lib/usb/usbmidihost.cpp with the Pi engine's patch stack (pi/engine/build.sh's order), copied into build/umh and patched
// there, on a fake host controller, timer and MIDI interface (everything it calls is defined below).
//
// T464: a Rytm's Data RPC reply came back with one event packet replaced by the one 1008 later. The Rytm's own USB MIDI
// transmit buffer (4 KB, wrapped at 4032 bytes) overran while the Pi did not read: Circle kept one IN request and, after an
// empty completion, asked again only 10 ms later. On the xHCI (RASPPI 4) both buffers are now in flight and an empty
// completion asks again at once; the DWHCI (RASPPI 3) is unchanged.
#include <circle/usb/usbmidihost.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbaudio.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbendpoint.h>
#if RASPPI >= 4
#include <circle/usb/xhciendpoint.h>
#endif
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/ptrlist.h>
#include <circle/spinlock.h>
#include <circle/string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <vector>
#include "check.h"

// ---- the fake world -------------------------------------------------------------------------------------------------

static std::deque<CUSBRequest *> g_Flight;	// submitted, not completed: the xHCI's transfer ring order
static std::vector<u32> g_Got;			// event packets the interface handed on, in order
static int g_nSubmits;
static bool g_bRefuse;				// SubmitAsyncRequest fails
static TKernelTimerHandler *g_pTimer;		// the one kernel timer armed (0: none)
static void *g_pTimerParam, *g_pTimerContext;
static unsigned g_nTimerDelay;
static int g_nRequestsOut;			// CUSBRequest objects alive

class CFakeHost : public CUSBHostController
{
public:
	CFakeHost (void) : CUSBHostController (FALSE) {}
	boolean Initialize (boolean) override { return TRUE; }
	boolean SubmitBlockingRequest (CUSBRequest *, unsigned) override { return FALSE; }
	boolean SubmitAsyncRequest (CUSBRequest *pURB, unsigned) override
	{
		if (g_bRefuse) return FALSE;
		g_nSubmits++;
		g_Flight.push_back (pURB);
		return TRUE;
	}
};
alignas (CFakeHost) static unsigned char g_HostMem[sizeof (CFakeHost)];
static CFakeHost *g_pHost;

static TUSBDeviceDescriptor g_DevDesc;
static unsigned char g_Config[64];		// endpoint IN + its class-specific endpoint, endpoint OUT + its own
static unsigned g_nConfigAt;

// Circle's own classes, reduced to what usbmidihost.cpp calls
CUSBHostController::CUSBHostController (boolean) {}
CUSBHostController::~CUSBHostController (void) {}
boolean CUSBHostController::UpdatePlugAndPlay (void) { return FALSE; }
int CUSBHostController::ControlMessage (CUSBEndpoint *, u8, u8, u16, u16, void *, u16) { return 0; }
int CUSBHostController::Transfer (CUSBEndpoint *, void *, unsigned, unsigned) { return 0; }
CPtrList::CPtrList (void) {}
CString::CString (void) {}
CString::~CString (void) {}
CPtrList::~CPtrList (void) {}

CDevice::CDevice (CDevice *) {}
CDevice::~CDevice (void) {}
int CDevice::Read (void *, size_t) { return -1; }
int CDevice::Write (const void *, size_t) { return -1; }
u64 CDevice::Seek (u64) { return 0; }
u64 CDevice::GetSize (void) const { return 0; }
int CDevice::IOCtl (unsigned long, void *) { return -1; }
boolean CDevice::RemoveDevice (void) { return FALSE; }

CUSBFunction::CUSBFunction (CUSBFunction *) {}
CUSBFunction::~CUSBFunction (void) {}
boolean CUSBFunction::Initialize (void) { return TRUE; }
boolean CUSBFunction::Configure (void) { return TRUE; }
boolean CUSBFunction::ReScanDevices (void) { return FALSE; }
boolean CUSBFunction::RemoveDevice (void) { return FALSE; }
alignas (16) static unsigned char g_DevMem[sizeof (CUSBDevice)];	// GetDeviceDescriptor (below) is all it is asked
CUSBDevice *CUSBFunction::GetDevice (void) const { return (CUSBDevice *) g_DevMem; }
CUSBHostController *CUSBFunction::GetHost (void) const { return g_pHost; }
CUSBEndpoint *CUSBFunction::GetEndpoint0 (void) const { return 0; }
u8 CUSBFunction::GetNumEndpoints (void) const { return 2; }
void CUSBFunction::ConfigurationError (const char *) const { fprintf (stderr, "configuration error\n"); }
boolean CUSBFunction::SelectInterfaceByClass (u8, u8, u8, unsigned) { return TRUE; }
const TUSBDescriptor *CUSBFunction::GetDescriptor (u8 ucType)
{
	while (g_nConfigAt < sizeof g_Config && g_Config[g_nConfigAt] != 0)
	{
		const TUSBDescriptor *p = (const TUSBDescriptor *) (g_Config + g_nConfigAt);
		g_nConfigAt += g_Config[g_nConfigAt];
		if (p->Header.bDescriptorType == ucType) return p;
	}
	return 0;
}
const TUSBDeviceDescriptor *CUSBDevice::GetDeviceDescriptor (void) const { return &g_DevDesc; }

CUSBEndpoint::CUSBEndpoint (CUSBDevice *, const TUSBEndpointDescriptor *) {}
CUSBEndpoint::~CUSBEndpoint (void) {}
u8 CUSBEndpoint::GetNumber (void) const { return 1; }
#if RASPPI >= 4
CXHCIEndpoint *CUSBEndpoint::GetXHCIEndpoint (void) { return 0; }
unsigned CXHCIEndpoint::GetEPState (void) { return 1; }
boolean CXHCIEndpoint::ResetFromHalted (void) { return FALSE; }
unsigned CXHCIEndpoint::DropURBs (void) { return 0; }
#endif

CUSBRequest::CUSBRequest (CUSBEndpoint *pEndpoint, void *pBuffer, u32 nBufLen, TSetupData *)
:	m_pEndpoint (pEndpoint), m_pSetupData (0), m_pBuffer (pBuffer), m_nBufLen (nBufLen), m_bStatus (0), m_nResultLen (0),
	m_USBError (USBErrorUnknown), m_nNumIsoPackets (0), m_pCompletionRoutine (0), m_pCompletionParam (0),
	m_pCompletionContext (0), m_bCompleteOnNAK (FALSE), m_bCompleteImmediately (FALSE)
{
	g_nRequestsOut++;
}
CUSBRequest::~CUSBRequest (void) { g_nRequestsOut--; }
void *CUSBRequest::operator new (size_t n) { return malloc (n); }
void CUSBRequest::operator delete (void *p, size_t) { free (p); }
void CUSBRequest::SetStatus (int b) { m_bStatus = b; }
void CUSBRequest::SetResultLen (u32 n) { m_nResultLen = n; }
int CUSBRequest::GetStatus (void) const { return m_bStatus; }
u32 CUSBRequest::GetResultLength (void) const { return m_nResultLen; }
TUSBError CUSBRequest::GetUSBError (void) const { return m_USBError; }
void *CUSBRequest::GetBuffer (void) { return m_pBuffer; }
u32 CUSBRequest::GetBufLen (void) const { return m_nBufLen; }
void CUSBRequest::SetCompleteOnNAK (void) { m_bCompleteOnNAK = TRUE; }
void CUSBRequest::SetCompletionRoutine (TURBCompletionRoutine *r, void *p, void *c)
{
	m_pCompletionRoutine = r, m_pCompletionParam = p, m_pCompletionContext = c;
}
void CUSBRequest::CallCompletionRoutine (void) { (*m_pCompletionRoutine) (this, m_pCompletionParam, m_pCompletionContext); }

CUSBMIDIDevice::CUSBMIDIDevice (CDevice *) : m_pPacketHandler (0), m_pPacketHandlerParam (0), m_pSendEventsHandler (0),
	m_pSendEventsParam (0), m_bAllSoundOff (FALSE), m_nDeviceNumber (0) {}
CUSBMIDIDevice::~CUSBMIDIDevice (void) {}
boolean CUSBMIDIDevice::CallPacketHandler (u8 *pData, unsigned nLength)	// Circle's: TRUE when any packet was not padding
{
	boolean bResult = FALSE;
	for (unsigned i = 0; i + 4 <= nLength; i += 4)
	{
		if (pData[i] == 0) continue;
		u32 w;
		memcpy (&w, pData + i, 4);
		g_Got.push_back (w);
		bResult = TRUE;
	}
	return bResult;
}
void CUSBMIDIDevice::RegisterSendEventsHandler (TSendEventsHandler *h, void *p) { m_pSendEventsHandler = h, m_pSendEventsParam = p; }
void CUSBMIDIDevice::RegisterSendEventsAsyncHandler (TSendEventsHandler *h, TSendStateHandler *s, void *p)
{
	m_pSendAsyncHandler = h, m_pSendStateHandler = s, m_pSendAsyncParam = p;
}
void CUSBMIDIDevice::RegisterResetHandler (TResetHandler *h, void *p) { m_pResetHandler = h, m_pResetParam = p; }
boolean CUSBMIDIDevice::GetAllSoundOffOnUSBError (void) const { return FALSE; }

alignas (CKernelOptions) static unsigned char g_OptMem[sizeof (CKernelOptions)];
CKernelOptions *CKernelOptions::Get (void) { return (CKernelOptions *) g_OptMem; }
unsigned CKernelOptions::GetUSBBoost (void) const { return USB_MIDI_BOOST_NONE; }

alignas (CTimer) static unsigned char g_TimerMem[sizeof (CTimer)];
CTimer *CTimer::Get (void) { return (CTimer *) g_TimerMem; }
TKernelTimerHandle CTimer::StartKernelTimer (unsigned nDelay, TKernelTimerHandler *pHandler, void *pParam, void *pContext)
{
	CHECK (g_pTimer == 0);			// one at a time
	g_pTimer = pHandler, g_pTimerParam = pParam, g_pTimerContext = pContext, g_nTimerDelay = nDelay;
	return 1;
}
void CTimer::CancelKernelTimer (TKernelTimerHandle) { g_pTimer = 0; }

alignas (CLogger) static unsigned char g_LogMem[sizeof (CLogger)];
CLogger *CLogger::Get (void) { return (CLogger *) g_LogMem; }
void CLogger::Write (const char *, TLogSeverity, const char *, ...) {}

extern "C" void assertion_failed (const char *pExpr, const char *pFile, unsigned nLine)
{
	fprintf (stderr, "assertion failed: %s (%s:%u)\n", pExpr, pFile, nLine);
	abort ();
}

// ---- helpers --------------------------------------------------------------------------------------------------------

static unsigned g_nNext = 1;			// the next event packet the device sends (a counter in its 3 data bytes)

// the oldest request completes with n event packets (0: a zero-length packet), or with an error
static void Complete (unsigned n, bool bError = false)
{
	CHECK (!g_Flight.empty ());
	CUSBRequest *p = g_Flight.front ();
	g_Flight.pop_front ();
	u8 *b = (u8 *) p->GetBuffer ();
	CHECK (n * 4 <= p->GetBufLen ());
	for (unsigned i = 0; i < n; i++, g_nNext++)
	{
		u32 w = 0x04 | (g_nNext & 0xFFFFFF) << 8;		// CIN 4: SysEx continues, 3 bytes
		memcpy (b + 4 * i, &w, 4);
	}
	p->SetStatus (bError ? 0 : 1);
	p->SetResultLen (bError ? 0 : n * 4);
	p->CallCompletionRoutine ();
}

static void FireTimer (void)
{
	CHECK (g_pTimer != 0);
	TKernelTimerHandler *h = g_pTimer;
	g_pTimer = 0;
	(*h) (1, g_pTimerParam, g_pTimerContext);
}

static bool InOrder (void)
{
	for (size_t i = 0; i < g_Got.size (); i++)
		if (g_Got[i] >> 8 != i + 1) { fprintf (stderr, "packet %zu: %u\n", i, g_Got[i] >> 8); return false; }
	return true;
}

static CUSBMIDIHostDevice *NewDevice (void)
{
	g_Flight.clear (), g_Got.clear (), g_nSubmits = 0, g_bRefuse = false, g_pTimer = 0, g_nNext = 1;
	memset (&g_DevDesc, 0, sizeof g_DevDesc);
	g_DevDesc.idVendor = 0x1935, g_DevDesc.idProduct = 0x0010;	// (no Roland special case)
	memset (g_Config, 0, sizeof g_Config);
	unsigned char cfg[] = { 7, DESCRIPTOR_ENDPOINT, 0x81, 0x02, 0x00, 0x02, 0,	// bulk IN 512
				5, DESCRIPTOR_CS_ENDPOINT, 0x01, 1, 1,
				7, DESCRIPTOR_ENDPOINT, 0x01, 0x02, 0x00, 0x02, 0,	// bulk OUT 512
				5, DESCRIPTOR_CS_ENDPOINT, 0x01, 1, 1 };
	memcpy (g_Config, cfg, sizeof cfg);
	g_nConfigAt = 0;
	CUSBFunction *pFunction = 0;
	CUSBMIDIHostDevice *p = new CUSBMIDIHostDevice ((CUSBFunction *) &pFunction);
	CHECK (p->Configure ());
	return p;
}

// ---- tests ----------------------------------------------------------------------------------------------------------

#if RASPPI >= 4
static const size_t kDepth = 2;
#else
static const size_t kDepth = 1;
#endif

static void test_configure_fills_depth (void)
{
	CUSBMIDIHostDevice *p = NewDevice ();
	CHECK ((g_Flight.size ()) == (kDepth));
	if (kDepth == 2) CHECK (g_Flight[0]->GetBuffer () != g_Flight[1]->GetBuffer ());	// each its own buffer
	delete p;
	CHECK ((g_nRequestsOut) == (0));			// the ones in flight freed with the device (circle-usb-urb-free)
}

static void test_stream_in_order (void)
{
	CUSBMIDIHostDevice *p = NewDevice ();
	for (unsigned k = 0; k < 2000; k++)
	{
		Complete (k % 3 == 0 ? 128 : 1 + k % 128);
		CHECK ((g_Flight.size ()) == (kDepth));	// always topped up again
		CHECK (g_pTimer == 0);
		if (kDepth == 2) CHECK (g_Flight[0]->GetBuffer () != g_Flight[1]->GetBuffer ());
	}
	CHECK (InOrder ());
	delete p;
	CHECK ((g_nRequestsOut) == (0));
}

// a zero-length packet: the xHCI asks again at once (it never completes on NAK, so this is the device's own ZLP); the
// DWHCI keeps Circle's 10 ms pause (its NAK completions)
static void test_zero_length (void)
{
	CUSBMIDIHostDevice *p = NewDevice ();
	Complete (128);
	Complete (0);
#if RASPPI >= 4
	CHECK (g_pTimer == 0);
	CHECK ((g_Flight.size ()) == (kDepth));
#else
	CHECK (g_pTimer != 0);
	CHECK ((g_nTimerDelay) == ((unsigned) MSEC2HZ (10)));
	CHECK ((g_Flight.size ()) == ((size_t) 0));
	FireTimer ();
	CHECK ((g_Flight.size ()) == (kDepth));
#endif
	Complete (5);
	CHECK (InOrder ());
	CHECK ((g_Got.size ()) == ((size_t) 133));
	delete p;
	CHECK ((g_nRequestsOut) == (0));
}

// padding only (header 0 in every packet): a completion with data, not a NAK
static void test_padding_only (void)
{
	CUSBMIDIHostDevice *p = NewDevice ();
	CHECK (!g_Flight.empty ());
	CUSBRequest *r = g_Flight.front ();
	g_Flight.pop_front ();
	memset (r->GetBuffer (), 0, 64);
	r->SetStatus (1);
	r->SetResultLen (64);
	r->CallCompletionRoutine ();
#if RASPPI >= 4
	CHECK (g_pTimer == 0);
	CHECK ((g_Flight.size ()) == (kDepth));
#else
	CHECK (g_pTimer != 0);
	FireTimer ();
#endif
	delete p;
	CHECK ((g_nRequestsOut) == (0));
}

// an error: one 10 ms pause for all of them; the other request's data meanwhile is still handed on, and the pause refills
// every buffer when it ends
static void test_error_pause (void)
{
	CUSBMIDIHostDevice *p = NewDevice ();
	Complete (10);
	Complete (0, true);
	CHECK (g_pTimer != 0);
	CHECK ((g_Flight.size ()) == (kDepth - 1));
	if (kDepth == 2)
	{
		Complete (7);			// arrives during the pause: kept, nothing asked yet
		CHECK ((g_Flight.size ()) == ((size_t) 0));
		CHECK (g_pTimer != 0);
	}
	FireTimer ();
	CHECK ((g_Flight.size ()) == (kDepth));
	Complete (3);
	CHECK (InOrder ());
	CHECK ((g_Got.size ()) == ((size_t) (kDepth == 2 ? 20 : 13)));
	delete p;
	CHECK ((g_nRequestsOut) == (0));
}

// a refused submission leaves a buffer free; the next completion brings it back
static void test_refused_submit (void)
{
	CUSBMIDIHostDevice *p = NewDevice ();
	g_bRefuse = true;
	Complete (4);
	CHECK ((g_Flight.size ()) == (kDepth - 1));
	g_bRefuse = false;
	if (kDepth == 2)
	{
		Complete (4);
		CHECK ((g_Flight.size ()) == (kDepth));
	}
	CHECK (InOrder ());
	delete p;
	CHECK ((g_nRequestsOut) == (0));
}

// the device gone with a pause armed: the timer is cancelled, nothing leaks
static void test_delete_during_pause (void)
{
	CUSBMIDIHostDevice *p = NewDevice ();
	Complete (0, true);
	CHECK (g_pTimer != 0);
	delete p;
	CHECK (g_pTimer == 0);
	CHECK ((g_nRequestsOut) == (0));
}

int main (void)
{
	g_pHost = new (g_HostMem) CFakeHost;
	test_configure_fills_depth ();
	test_stream_in_order ();
	test_zero_length ();
	test_padding_only ();
	test_error_pause ();
	test_refused_submit ();
	test_delete_during_pause ();
	printf ("test_midi_rxqueue (RASPPI %d): OK\n", RASPPI);
	return 0;
}
