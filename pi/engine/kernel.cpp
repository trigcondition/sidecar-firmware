// pi/engine/kernel.cpp - the Pi engine (docs/pi-engine.md). Core 0: USB (the Rytm's Overbridge stream in the xHCI
// interrupt, its USB MIDI read-only), the network (Circle's scheduler; the TCP link to rytm-emu, tcp_link.h, with
// link=tcp), the console, the stats, the clean stop before a reboot; without a Rytm, the self-test transport
// (selftest.h). Cores 1-3: cores.cpp.
#include "kernel.h"
#include "um_pi.h"
#include "user/um.h"
#include "sidecar_display.h"

#include <circle/alloc.h>
#include <circle/bcmpropertytags.h>
#include <circle/devicetreeblob.h>
#include <circle/machineinfo.h>
#include <circle/memory.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/version.h>
#include <fatfs/ff.h>

#include "bridge.h"
#include "shared.h"
#include "sendfx_pi.h"
#include "sdfiles.h"
#include "wifi.h"
#include "rec_pi.h"
#include "version.h"
#include <circle/new.h>

// The network options' defaults when cmdline.txt has none: build.sh writes hosted/net_defaults.h from PI_IP,
// PI_NETMASK and EMUHOST (its defaults: the ones below). cmdline.txt wins; it can be edited from the Mac with the card
// in the Pi (pi/circle/sdboot.py cmdline, sdfiles.h), but it also applies to every RAM kernel flashed later.
#if __has_include("hosted/net_defaults.h")
#include "hosted/net_defaults.h"
#endif
#ifndef GADGET_DEFAULT
#define GADGET_DEFAULT		"sidecar"
#endif
#ifndef NET_DEFAULT_IP
#define NET_DEFAULT_IP		"192.168.77.2"
#define NET_DEFAULT_NETMASK	"255.255.255.0"
#define NET_DEFAULT_EMUHOST	"192.168.77.1:7301"
#endif

static const char From[] = "engine";
static const char RebootMagic[] = "RYTM-REBOOT";
static volatile boolean s_bRebootRequested = FALSE;
static void RebootRequest (void) { s_bRebootRequested = TRUE; }   // UART IRQ: core 0's loop stops cleanly first
static unsigned s_nMidiInLost;		// the Rytm's USB MIDI bytes lost: a full g_MidiIn (the stats line's "lost")
static unsigned s_nLoopMax, s_nLoopN, s_nLoopSum;	// core 0's loop: its MsSleep (1) in us (/status "core0", reset when read)
static unsigned s_nToHostFull;		// engine bytes for the host lost: a full g_MidiOut (none expected) or a record > 4096 B

// "a.b.c.d" or "a.b.c.d:port" -> IP (and port if asked); FALSE if malformed, trailing junk included ("1.2.3.4x",
// ":7301abc", a port where none is asked).
static boolean CKernel_ParseIP (const char *p, u8 IP[4], u16 *pPort = 0)
{
	if (p == 0) return FALSE;
	for (int i = 0; i < 4; i++)
	{
		if (*p < '0' || *p > '9') return FALSE;
		unsigned v = 0;
		while (*p >= '0' && *p <= '9' && v <= 255) v = v * 10 + (*p++ - '0');
		if (v > 255) return FALSE;
		IP[i] = (u8) v;
		if (i < 3 && *p++ != '.') return FALSE;
	}
	if (*p == 0) return TRUE;
	if (pPort == 0 || *p++ != ':' || *p < '0' || *p > '9') return FALSE;
	unsigned n = 0;
	while (*p >= '0' && *p <= '9' && n <= 65535) n = n * 10 + (*p++ - '0');
	if (*p != 0 || n == 0 || n > 65535) return FALSE;
	*pPort = (u16) n;
	return TRUE;
}

static const u8 *NetOption (CKernelOptions *pOpt, const char *pName, const char *pDefault, u8 Out[4])
{
	if (!CKernel_ParseIP (pOpt->GetAppOptionString (pName, pDefault), Out)) CKernel_ParseIP (pDefault, Out);
	return Out;
}

CKernel::CKernel (void)
:	m_Serial (&m_Interrupt),
	m_LogTee (&m_Serial),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_CPUThrottle (CPUSpeedMaximum),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),		// TRUE: plug-and-play
	m_Net (*(CNetSubSystem *) m_NetStore),		// constructed in Initialize (Ethernet or Wi-Fi: wifi.h)
	m_Cores (CMemorySystem::Get ()),
	m_Console (&m_Serial),
	m_Tcp (&m_Net),
	m_pTransport (&m_Usb),
	m_pMIDI (0),
	m_pSidecar (0),
	m_pGadget (0),
	m_nLastStats (0),
	m_pGadgetMode ("0"),
	m_bNetOK (FALSE),
	m_pDisplay (0),
	m_bUiPending (FALSE),
	m_nUiAt (0),
	m_bWeb (FALSE),
	m_nSaverMin (60),
	m_nBright (100),
	m_bTouchHidden (FALSE),
	m_bSaveDisplay (FALSE),
	m_pWifi (0)
{
	memset (&m_Ui, 0, sizeof m_Ui);
	m_Ui.screensaver = -1;
	for (unsigned c = 0; c < SIDECAR_UI_CORES; c++) m_Ui.load[c] = -1;
	// no m_ActLED.Blink (3) (the skeleton's): it held the boot 2.1 s before the first log line (measured 2026-10-16);
	// the boot screen shows the start now
}

// Each message as a record, its length (u16 LE) and its bytes: the TCP link sends whole messages (the emulator's
// USB MIDI queue splits at message boundaries). A record that does not fit is dropped whole (counted), so the
// records stay in step (core 3 is the only producer: the room can only grow meanwhile).
void CKernel::ToHost (const u8 *pData, int nLength)
{
	if (nLength <= 0) return;
	if (nLength > 4096)				// longer than one 'M' (tcp_link.h's m_Msg): dropped, counted
	{
		s_nToHostFull += nLength;
		return;
	}
	if (g_MidiOut.size () + 2 + nLength > kMidiOutBytes)
	{
		s_nToHostFull += nLength;
		return;
	}
	g_MidiOut.push ((u8) nLength);
	g_MidiOut.push ((u8) (nLength >> 8));
	for (int i = 0; i < nLength; i++) g_MidiOut.push (pData[i]);
}

// One boot-time reservation (CKernel::Initialize): 64-byte aligned (a cache line), from Circle's heap; a failure is
// logged and halts the boot, nothing runs without its buffers.
static void *BootAlloc (size_t nBytes, const char *pWhat)
{
	if (nBytes == 0) return 0;
	void *p = memalign (64, nBytes);
	if (p == 0) CLogger::Get ()->Write (From, LogPanic, "boot: no memory for %s (%u KB)", pWhat, (unsigned) (nBytes >> 10));
	return p;
}

boolean CKernel::Initialize (void)
{
	boolean bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (921600);
	if (bOK) bOK = m_Logger.Initialize (&m_LogTee);	// the serial port, and the web page's copy
	if (bOK) bOK = m_Timer.Initialize ();
	if (!bOK) return FALSE;
	m_Logger.Write (From, LogNotice, "==== rytm-pi engine ==== Sidecar " SIDECAR_VERSION_FULL ", CPU %u MHz", m_CPUThrottle.GetClockRate () / 1000000);
	ReadPower (TRUE);				// the supply's state in the boot log (a weak one: Wi-Fi and the SD fail first)
	// the boot screen first (sidecar_splash.h; display=0: none), one bar step per stage below, LVGL's home screen last
	if (m_Options.GetAppOptionDecimal ("display", 1) != 0)
	{
		unsigned ts = m_Timer.GetClockTicks ();
		m_pDisplay = new CSidecarDisplay (m_Options.GetWidth (), m_Options.GetHeight ());
		if (!m_pDisplay->Splash ()) m_Logger.Write (From, LogNotice, "boot screen: no frame buffer");
		else m_Logger.Write (From, LogNotice, "boot screen: %ux%u in %u us", m_pDisplay->GetFrameWidth (),
				     m_pDisplay->GetFrameHeight (), m_Timer.GetClockTicks () - ts);
	}
	if (!m_EMMC.Initialize ()) m_Logger.Write (From, LogError, "SD: no card");
	BootStep (1);
	// the big fixed buffers, reserved once here from Circle's heap (cached normal memory, as .bss) rather than in .bss,
	// which counts toward KERNEL_MAX_SIZE: the send FX's 4.8 MB, the core's (bridge::heap_bytes). Never freed, never
	// allocated after this; a failure halts the boot. Built or zeroed here, before cores 1-3 start.
	size_t nHeap0 = CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_ANY);
	void *pFxMem = BootAlloc (sendfx_pi::Bytes (), "sendfx");
	void *pCoreMem = BootAlloc (bridge::heap_bytes (), "core buffers");
	bridge::heap_place (pCoreMem);
	m_Logger.Write (From, LogNotice, "boot: heap %u KB free, %u KB reserved (sendfx %u KB at %p, core %u KB at %p), %u KB left",
			(unsigned) (nHeap0 >> 10), (unsigned) ((sendfx_pi::Bytes () + bridge::heap_bytes ()) >> 10),
			(unsigned) (sendfx_pi::Bytes () >> 10), pFxMem, (unsigned) (bridge::heap_bytes () >> 10), pCoreMem,
			(unsigned) (CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_ANY) >> 10));
	unsigned t0 = m_Timer.GetClockTicks ();
	m_Logger.Write (From, LogNotice, "boot: sendfx init...");
	sendfx_pi::Init (pFxMem);
	m_Logger.Write (From, LogNotice, "boot: sendfx init done in %u us", m_Timer.GetClockTicks () - t0);
	BootStep (2);
	t0 = m_Timer.GetClockTicks ();
	bridge::selftest_content ();
	boolean bCard = bridge::begin (ToHost);
	m_Logger.Write (From, LogNotice, "engine: begin %s in %u ms (%d folders, project \"%s\", pool %d)",
			bCard ? "ok" : "WITHOUT a card", (m_Timer.GetClockTicks () - t0) / 1000,
			bridge::card_folders (), bridge::project (), bridge::pool_slots ());
	// the user machines (um_pi.h): the code arena, the card's /Plugins scanned by core 3 once it runs
	if (bCard && m_Options.GetAppOptionDecimal ("um", 1) != 0) CUserMachines::Initialize ();
	if (bCard) rec_pi::Init ();		// the recorder's memory (recbuf=MB) and the boot scan of /Recordings
	BootStep (3);
	m_Usb.SetSend (m_Options.GetAppOptionDecimal ("tohost", 1) != 0);	// tohost=0: the replies dropped, as before
	boolean bBench = m_Options.GetAppOptionDecimal ("bench", 1) != 0;
	if (bBench) CEngineCores::BenchCore (0);		// cores 1-3 bench as they start (LogBench)
	SelfCheck ();
	// boot defaults (the user's decision, 2026-10-16): out=tracks, the voices on their tracks' inputs, through the
	// Rytm's filters and FX (the mod build); MIDI notes off (notes=on is for a stock Rytm, with out=main); no drone
	// (only :drone=). cmdline.txt or the console override them.
	g_Output.SetMode (strcmp (m_Options.GetAppOptionString ("out", "tracks"), "main") == 0 ? Output::kMain
											      : Output::kTracks);
	// fifo=: the target for every link; without it each link's own (UseLink: FifoUsb, FifoTcp). split= 4 is the
	// plan's default (docs/pi-engine.md "Measurements")
	if (m_Options.GetAppOptionString ("fifo", 0) != 0) CEngineCores::FixFifoTarget ();
	CEngineCores::Configure (m_Options.GetAppOptionDecimal ("fifo", FifoUsb),
				 strcmp (m_Options.GetAppOptionString ("notes", "off"), "on") == 0
				 || strcmp (m_Options.GetAppOptionString ("notes", "off"), "1") == 0);
	CEngineCores::SetSplit (m_Options.GetAppOptionDecimal ("split", 4));	// voices 0-3 core 1, 4-7 core 2
	u64 nCntFrq;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (nCntFrq));
	unsigned nPct = m_Options.GetAppOptionDecimal ("voicebudget", 25);
	nPct = nPct < 5 ? 5 : nPct > 100 ? 100 : nPct;
	g_Guard.SetBudget ((unsigned) (nCntFrq * 12 / 48000 * nPct / 100));
	m_Logger.Write (From, LogNotice, "guard: voice budget %u us (%u %% of a 12-frame block)",
			(unsigned) (g_Guard.Budget () / (nCntFrq / 1000000)), nPct);
	m_Console.Initialize ();
	BootStep (4);
	CEngineCores::BenchStart (bBench);
	if (!m_Cores.Initialize ()) return FALSE;		// cores 1-3 start (cores.cpp)
	BootStep (5);
	if (bBench) LogBench ();
	BootStep (6);
	if (!m_USBHCI.Initialize ()) return FALSE;		// the Rytm is enumerated by plug-and-play in Run
	BootStep (7);
	// the HDMI display and its USB touch (docs/pi-display.md), driven from core 0's loop: display=0 turns it off.
	// LVGL's home screen replaces the boot screen at the first Update.
	if (m_pDisplay != 0)
	{
		BootStep (8);
		if (m_pDisplay->Initialize ()) m_pDisplay->SetCommandHandler (UiCommand, this);
		else m_pDisplay = 0;			// no frame buffer: left alone (no destructor)
	}
	LoadDisplaySettings (bCard);
	m_Console.SetCore0Handler (Core0Command, this);	// screensaver=, brightness= (and the Sidecar's commands)
	// USB-C, the computer's side: gadget=sidecar (default: UAC2 + USB MIDI, sidecar_gadget.h), midi (P1's MIDI
	// echo, gadget.h) or 0. The default can be built in (GADGET=midi pi/engine/build.sh): the serial bootloader
	// card's cmdline.txt is not edited from the Mac. A gadget that fails to start is left alone (no destructor).
#ifdef SIDECAR_NO_GADGET
	const char *pGadgetMode = "0";			// Pi 5: no gadget in Circle (docs/pi5.md); gadget= is ignored
	m_pGadgetMode = pGadgetMode;
	m_Logger.Write (From, LogNotice, "gadget: none on this board (Pi 5 build)");
	if (0)
#else
	const char *pGadgetMode = m_Options.GetAppOptionString ("gadget", GADGET_DEFAULT);
	m_pGadgetMode = pGadgetMode;
	if (strcmp (pGadgetMode, "sidecar") == 0)
#endif
	{
		CSidecarGadget *pSidecar = new CSidecarGadget (&m_Interrupt);
		if (pSidecar->Start ()) m_pSidecar = pSidecar;
		m_Logger.Write (From, m_pSidecar ? LogNotice : LogError, "gadget: USB-C Sidecar %s (%u in / %u out, 48 kHz, 24-bit, + MIDI)",
				m_pSidecar ? "up" : "failed to start", CSidecarGadget::ChIn, CSidecarGadget::ChOut);
	}
	else if (strcmp (pGadgetMode, "midi") == 0)
	{
		CSidecarMIDIGadget *pGadget = new CSidecarMIDIGadget (&m_Interrupt);
		if (pGadget->Start ()) m_pGadget = pGadget;
		m_Logger.Write (From, m_pGadget ? LogNotice : LogError, "gadget: USB-C MIDI device %s",
				m_pGadget ? "up (Sidecar)" : "failed to start");
	}
#ifndef SIDECAR_NO_GADGET
	else m_Logger.Write (From, LogNotice, "gadget: off");
#endif
	// the network: one stack on one device (Circle), Ethernet (static: the direct cable has no DHCP server) or, with
	// wifi=1 and a stored network, Wi-Fi (DHCP unless wifi.txt has ip=); mDNS: sidecar.local on either (web.h)
	m_pWifi = new CWifi;				// reads /.rytm/wifi.txt (the card is mounted by bridge::begin)
	CWifi::TMode WifiMode = m_pWifi->BootMode (&m_Options);
	if (WifiMode != CWifi::ModeOff && !m_pWifi->StartChip ()) WifiMode = CWifi::ModeOff;
	if (WifiMode == CWifi::ModeNet)
	{
		u8 GW[4], DNS[4];
		boolean bStatic = m_pWifi->StaticIP (m_IP, m_NetMask, GW, DNS);
		if (!bStatic) memset (m_IP, 0, sizeof m_IP);
		new (m_NetStore) CNetSubSystem (bStatic ? m_IP : 0, m_NetMask, bStatic ? GW : 0, bStatic ? DNS : 0, "sidecar",
						NetDeviceTypeWLAN);
	}
	else new (m_NetStore) CNetSubSystem (NetOption (&m_Options, "ip", NET_DEFAULT_IP, m_IP),
					     NetOption (&m_Options, "netmask", NET_DEFAULT_NETMASK, m_NetMask), 0, 0, "sidecar");
	m_bNetOK = m_Net.Initialize (FALSE);
	if (!m_bNetOK) m_Logger.Write (From, LogError, "network: init failed (link=tcp refused, usb only)");
	else if (WifiMode == CWifi::ModeNet)
	{
		m_Logger.Write (From, LogNotice, "network: Wi-Fi (%s; Ethernet unused, wifi=0 for Ethernet)", m_IP[0] ? "static" : "DHCP");
	}
	else m_Logger.Write (From, LogNotice, "network: %u.%u.%u.%u/%u.%u.%u.%u (Ethernet; link comes up by itself)%s",
			     m_IP[0], m_IP[1], m_IP[2], m_IP[3], m_NetMask[0], m_NetMask[1], m_NetMask[2], m_NetMask[3],
			     WifiMode == CWifi::ModeScan ? "; Wi-Fi chip up for scans" : "");
	m_pWifi->StartSupplicant (m_bNetOK);		// ModeNet: joins; any mode: a join's trial may judge from now on
	UseLink (m_Options.GetAppOptionString ("link", "usb"));
	// the web page (web.h): http://sidecar.local once the Ethernet link is up; web=0 leaves it off
	if (m_bNetOK && m_Options.GetAppOptionDecimal ("web", 1) != 0)
	{
		new CWebStart (&m_Net, &m_LogTee, &m_Console, WebStatus, this);
		m_bWeb = TRUE;
		m_Logger.Write (From, LogNotice, "web: on (sidecar.local once the Ethernet link is up; web=0 turns it off)");
	}
	else m_Logger.Write (From, LogNotice, "web: off");
	// the SD card's boot files on TCP 7310 (sdfiles.h, pi/circle/sdboot.py: install to SD); sdport=0 turns it off
	if (m_bNetOK && m_Options.GetAppOptionDecimal ("sdport", 1) != 0) new CSdFiles (&m_Net);
	new CSdFiles (0, &m_Serial);		// and on the serial port after the console's ":sdf"
	return TRUE;
}

// /status's members (core 0: a web task). Plain reads of state other cores write: a stale value at worst.
void CKernel::WebStatus (CString *pOut, void *pParam)
{
	CKernel *k = (CKernel *) pParam;
	CString s;
	u8 IP[4];
	memcpy (IP, k->m_IP, sizeof IP);
	if (k->m_bNetOK) k->m_Net.GetConfig ()->GetIPAddress ()->CopyTo (IP);	// DHCP on Wi-Fi: the lease's
	s.Format ("\"ip\":\"%u.%u.%u.%u\",\"link\":\"%s\",\"gadget\":\"%s\",\"sidecar\":%s,\"mac\":%s,"
		  "\"streaming\":%s,\"rytmMidi\":%s,\"temp\":%u,\"out\":\"%s\",\"notes\":%s,\"drone\":%d,\"fifoTarget\":%u,"
		  "\"underruns\":%u",
		  IP[0], IP[1], IP[2], IP[3], k->m_pTransport->Name (), k->m_pGadgetMode,
		  k->m_pSidecar ? "true" : "false", k->m_pSidecar ? (k->m_pSidecar->MacOn () ? "true" : "false") : "null",
		  CObUsb::Streaming () ? "true" : "false", k->m_pMIDI != 0 ? "true" : "false",
		  k->m_CPUThrottle.GetTemperature (), g_Output.GetMode () == Output::kMain ? "main" : "tracks",
		  CEngineCores::NotesOn () ? "true" : "false", CEngineCores::DroneVoice (), CEngineCores::FifoTarget (),
		  g_Output.Underflows ());
	pOut->Append (s);
	pOut->Append (",\"machines\":[");			// each voice's machine (the Sound tab shows the current one)
	for (unsigned v = 0; v < 8; v++)
	{
		s.Format ("%s\"%s\"", v ? "," : "", bridge::machine_name (v));
		pOut->Append (s);
	}
	pOut->Append ("]");
	sendfx_pi::View fx;				// the send FX tab: each FX's TYPE, PRE/POST and the 12 sends
	sendfx_pi::Read (&fx);
	pOut->Append (",\"fx\":[");
	for (unsigned f = 0; f < 2; f++)
	{
		char t[13];
		memcpy (t, fx.type[f], 12);
		t[12] = 0;
		for (char *c = t; *c; c++) if (*c == '"' || *c == '\\' || *c < 0x20 || *c > 0x7e) *c = '_';
		s.Format ("%s{\"type\":\"%s\",\"post\":%u,\"send\":[", f ? "," : "", t, fx.post[f]);
		pOut->Append (s);
		for (unsigned v = 0; v < 12; v++)
		{
			s.Format ("%s%u", v ? "," : "", fx.send[f][v]);
			pOut->Append (s);
		}
		pOut->Append ("]}");
	}
	s.Format ("],\"version\":\"" SIDECAR_VERSION "\",\"commit\":\"" SIDECAR_COMMIT "\",\"dirty\":%s,\"build\":\"%s %s\",\"circle\":\"%s %s\",\"screensaver\":%u,\"brightness\":%u,\"touchtest\":%u,\"display\":%s,\"blank\":%s",
		  SIDECAR_DIRTY ? "true" : "false", __DATE__, __TIME__, CIRCLE_NAME, CIRCLE_VERSION_STRING, k->m_nSaverMin, k->m_nBright, k->m_bTouchHidden ? 0u : 1u,
		  k->m_pDisplay != 0 ? "true" : "false", k->m_pDisplay != 0 && k->m_pDisplay->IsBlank () ? "true" : "false");
	pOut->Append (s);
	{
		char Power[400];		// the supply (power_state.h): the page's warning
		powerstate::json (k->m_Power, Power, sizeof Power);
		pOut->Append (",");
		pOut->Append (Power);
	}
	if (k->m_pWifi != 0) { pOut->Append (",\"wifi\":"); k->m_pWifi->Status (pOut); }
	rec_pi::Status (pOut);			// the Record tab
	{
		char Fx[96];			// IN L/R source: the setting and the Rytm's confirmed state (core/host_link.h)
		bridge::fxin_json (Fx, sizeof Fx);
		pOut->Append (",");
		pOut->Append (Fx);
	}
	{
		static char Lfo[2048];		// the LFO tab: every LFO's settings and phase (core/lfo.h; core 3's, plain reads)
		bridge::lfo_json (Lfo, sizeof Lfo);
		pOut->Append (",");
		pOut->Append (Lfo);
	}
	s.Format (",\"core0\":{\"sleep1msAvgUs\":%u,\"sleep1msMaxUs\":%u,\"loops\":%u}", s_nLoopN ? s_nLoopSum / s_nLoopN : 0,
		  s_nLoopMax, s_nLoopN);
	s_nLoopMax = s_nLoopN = s_nLoopSum = 0;
	pOut->Append (s);
}

// "usb", "tcp" or "tcp:IP:PORT" (default emuhost= from cmdline, else NET_DEFAULT_EMUHOST): the engine's host. One at a
// time: in tcp the USB stream (a real Rytm, if attached) gets silence and its MIDI is dropped.
// The switch relies on the xHCI interrupt staying on core 0, this loop's core: FillOut (g_Output's consumer with
// usb) and MIDIPacketHandler (g_MidiIn's producer) then never run while this task is inside the TCP link's Pull or
// push, so flipping CObUsb::SetActive here hands the single-consumer/single-producer queues over cleanly. Routing
// that IRQ to another core would need a handshake (the IRQ acknowledging the flag) before the TCP link touches them.
void CKernel::UseLink (const char *pSpec)
{
	if (pSpec != 0 && strncmp (pSpec, "tcp", 3) == 0 && (pSpec[3] == 0 || pSpec[3] == ':'))
	{
		if (!m_bNetOK)
		{
			m_Logger.Write (From, LogWarning, "link: tcp refused - the network did not initialize; staying %s",
					m_pTransport->Name ());
			return;
		}
		if (m_pTransport == &m_SelfTest) SetSelfTest (FALSE, "link=tcp");	// one consumer of g_Output
		u8 IP[4];
		u16 nPort = 7301;
		const char *pHost = pSpec[3] == ':' ? pSpec + 4 : m_Options.GetAppOptionString ("emuhost", NET_DEFAULT_EMUHOST);
		if (!CKernel_ParseIP (pHost, IP, &nPort))
		{
			m_Logger.Write (From, LogWarning, "link: bad address \"%s\"", pHost);
			return;
		}
		m_Tcp.SetPeer (IP, nPort);
		CObUsb::SetActive (FALSE);
		m_pTransport = &m_Tcp;
		CEngineCores::LinkFifoTarget (FifoTcp);
		m_Logger.Write (From, LogNotice, "link: tcp to %u.%u.%u.%u:%u (fifo target %u frames)", IP[0], IP[1], IP[2],
				IP[3], nPort, CEngineCores::FifoTarget ());
		return;
	}
	if (pSpec == 0 || strcmp (pSpec, "usb") != 0)
	{
		m_Logger.Write (From, LogWarning, "link: usb|tcp|tcp:IP:PORT, not \"%s\" - usb", pSpec ? pSpec : "");
	}
	m_Tcp.Stop ();
	if (m_pTransport == &m_Tcp)
	{
		// tcp -> usb: the frames the TCP link left go, and the USB stream (already running: no new-stream Reset in
		// ob_usb.cpp) fades in again instead of a click. No consumer meanwhile: the TCP link stopped, FillOut leaves
		// g_Output alone until SetActive (TRUE).
		g_Output.Reset ();
	}
	CObUsb::SetActive (TRUE);
	if (m_pTransport == &m_Tcp) m_pTransport = &m_Usb;
	CEngineCores::LinkFifoTarget (FifoUsb);
	m_Logger.Write (From, LogNotice, "link: %s", m_pTransport->Name ());
}

// Task 7's boot bench (cores.h): each machine on one voice for 1 s of audio, on cores 0-3 (the cores share the
// clock but not their L1 caches; the render cores are 1 and 2). Per core and machine: the mean cost of one voice in
// % of a core in real time, the longest 12-frame block in % of a block (the guard mutes a voice at voicebudget=
// percent, three blocks running), and how many such voices fit 80 % of a core. Then one line per machine, the
// worst core's numbers, in the plan's format (pi_run.py bench parses both).
void CKernel::LogBench (void)
{
	if (!CEngineCores::BenchWait (20000))
	{
		// that core never reaches its loop, so no block would ever render: fatal (core 0's loop stops cleanly and
		// reboots: a RAM kernel is gone then; an SD card that boots engine.img (sdboot) boots it again, each time
		// through sdboot's 1 s window, so flash.sh still gets in)
		m_Logger.Write (From, LogError, "bench: core %u did not finish its bench within 20 s (bench=0 in cmdline.txt skips it)",
				CEngineCores::BenchTurn ());
		g_Guard.Fatal ("the boot bench hung");
		return;
	}
	u64 nCntFrq;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (nCntFrq));
	const u64 nBlock = nCntFrq * bridge::kBlock / 48000;		// one block's ticks
	const unsigned nMHz = m_CPUThrottle.GetClockRate () / 1000000;
	for (unsigned m = 0; m < CEngineCores::BenchMachines; m++)
	{
		unsigned nWorstBp = 0, nWorstBlk = 0;
		for (unsigned c = 0; c < 4; c++)
		{
			const CEngineCores::TBench &r = CEngineCores::BenchResult (c, m);
			unsigned nBp = (unsigned) (r.nTicks * 10000 / nCntFrq);	// 1/100 % of a core (the bench is 1 s)
			unsigned nBlk = (unsigned) (r.nWorst * 1000 / nBlock);		// 1/10 % of a block
			if (nBp > nWorstBp) nWorstBp = nBp;
			if (nBlk > nWorstBlk) nWorstBlk = nBlk;
			m_Logger.Write (From, LogNotice, "bench c%u %s: %u.%02u %% of a core per voice, worst block %u.%u %% -> %u voices a core, peak %d",
					c, CEngineCores::BenchMachine[m], nBp / 100, nBp % 100, nBlk / 10, nBlk % 10,
					nBp ? 8000 / nBp : 999, r.nPeak);		// at 80 % of a core
			m_Timer.MsDelay (2);	// ~150 B a line takes 1.6 ms at 921600 baud: Circle's 2 KB TX buffer drops when full
		}
		m_Logger.Write (From, LogNotice, "bench %s: %u.%02u %% of a core per voice at %u MHz -> %u voices a core (worst of cores 0-3, worst block %u.%u %%, budget %u %%)",
				CEngineCores::BenchMachine[m], nWorstBp / 100, nWorstBp % 100, nMHz,
				nWorstBp ? 8000 / nWorstBp : 999, nWorstBlk / 10, nWorstBlk % 10,
				(unsigned) (g_Guard.Budget () * 100 / nBlock));
		m_Timer.MsDelay (2);
	}
}

// Task 3's boot self-check, before the cores start: one second of a PLAITS voice rendered offline (no USB), then
// the voices back to their boot state.
void CKernel::SelfCheck (void)
{
	static const u8 Trig[41] = {0xF0, 0x7D, 0x52, 0x54, 0x04, 0x00, 48, 100,
				    40, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 64, 0, 100, 0,
				    64, 0, 64, 0, 0, 0, 0, 0, 0, 0, 120, 0, 0, 0, 100, 0, 0xF7};
	bridge::sysex (Trig, sizeof Trig);
	static s16 Ring[bridge::kBlock][bridge::kVoices];
	u64 Mach[bridge::kMachines] = {0};
	unsigned nPeak = 0, nCycles = 0;
	for (unsigned b = 0; b < 4000; b++)
	{
		nCycles += bridge::render (Ring, 0, bridge::kVoices, Mach);
		for (unsigned s = 0; s < bridge::kBlock; s++)
		{
			int a = Ring[s][0] < 0 ? -Ring[s][0] : Ring[s][0];
			if ((unsigned) a > nPeak) nPeak = a;
		}
	}
	bridge::reinit_voices ();
	u64 nCntFrq;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (nCntFrq));   // the generic timer: core::cycles()' rate (54 MHz)
	unsigned nFreq = (unsigned) nCntFrq;
	m_Logger.Write (From, LogNotice, "core: 1 s of PLAITS VA rendered in %u us (%u.%u %% of real time), peak %u",
			nCycles / (nFreq / 1000000), nCycles / (nFreq / 1000) / 10, nCycles / (nFreq / 1000) % 10, nPeak);   // ms / 10 = %
}

// Core 0: the self-test transport on or off (the console's :selftest=, cmdline selftest=1). Never together with the
// Rytm's stream: both would consume the output stage's single-consumer FIFO.
void CKernel::SetSelfTest (boolean bOn, const char *pWhy)
{
	if (bOn)
	{
		if (m_pTransport == &m_SelfTest) return;
		if (m_pTransport == &m_Tcp)
		{
			m_Logger.Write (From, LogWarning, "selftest: refused - the TCP link is the host (:link=usb first)");
			return;
		}
		if (CObUsb::Streaming () || m_pMIDI != 0)	// one producer each for g_Output's consumer and g_MidiIn
		{
			m_Logger.Write (From, LogWarning, "selftest: refused - the Rytm is connected (%s)",
					m_pMIDI != 0 ? "its USB MIDI" : "its stream");
			return;
		}
		m_SelfTest.Start ();
		m_pTransport = &m_SelfTest;
	}
	else
	{
		if (m_pTransport != &m_SelfTest) return;
		if (CObUsb::CaptureActive ())
		{
			CObUsb::CaptureStop ();
			m_Logger.Write (From, LogWarning, "capture: abandoned (the self-test stopped)");
			m_Logger.Write (From, LogError, "WAV-ERROR selftest-off");
		}
		m_SelfTest.Halt ();
		m_pTransport = &m_Usb;
		m_Logger.Write (From, LogNotice, "selftest: off (%s)", pWhy);
	}
	m_Console.SetSelfTest (bOn);
}

boolean CKernel::Core0Command (const char *pCmd, void *pParam)
{
	CKernel *pThis = (CKernel *) pParam;
	boolean bSaver = strncmp (pCmd, "screensaver", 11) == 0 && (pCmd[11] == 0 || pCmd[11] == '=');
	boolean bBright = strncmp (pCmd, "brightness", 10) == 0 && (pCmd[10] == 0 || pCmd[10] == '=');
	if (strncmp (pCmd, "touchtest", 9) == 0 && (pCmd[9] == 0 || pCmd[9] == '='))
	{
		if (pCmd[9] == '=' && (pCmd[10] == '0' || pCmd[10] == '1') && pCmd[11] == 0)
		{
			pThis->m_bTouchHidden = pCmd[10] == '0';
			pThis->m_bSaveDisplay = TRUE;
			if (pThis->m_pDisplay != 0)
			{
				pThis->m_pDisplay->Activity ();
				pThis->PushUi ();
			}
		}
		else if (pCmd[9] == '=') pThis->m_Logger.Write (From, LogWarning, "touchtest=0|1 (0 hides the home page's touch test)");
		pThis->m_Logger.Write (From, LogNotice, "display: touch test %s", pThis->m_bTouchHidden ? "hidden" : "shown");
		return TRUE;
	}
	if (bSaver || bBright)
	{
		const char *v = strchr (pCmd, '=');
		if (v != 0)
		{
			unsigned n = 0;
			for (v++; *v >= '0' && *v <= '9'; v++) n = n * 10 + (*v - '0');
			if (*v || (bSaver && n > 1440) || (bBright && (n < 5 || n > 100)))
			{
				pThis->m_Logger.Write (From, LogWarning, bSaver ? "screensaver=MINUTES (0 never, at most 1440)"
										 : "brightness=5..100 (%%)");
				return TRUE;
			}
			if (bSaver) pThis->m_nSaverMin = n; else pThis->m_nBright = n;
			if (pThis->m_pDisplay != 0)
			{
				if (bSaver) pThis->m_pDisplay->SetScreensaver (n);
				else pThis->m_Ui.brightness = (int) n;
				pThis->m_pDisplay->Activity ();
				pThis->PushUi ();
			}
			pThis->m_bSaveDisplay = TRUE;
		}
		pThis->m_Logger.Write (From, LogNotice, "display: screensaver %u min%s, brightness %u %% (the picture dimmed)",
				       pThis->m_nSaverMin, pThis->m_nSaverMin ? "" : " (off)", pThis->m_nBright);
		return TRUE;
	}
	return pThis->m_pSidecar != 0 && pThis->m_pSidecar->Command (pCmd);
}

// /.rytm/display.txt: "screensaver=MIN", "brightness=PCT" and "touchtest=0|1" lines; cmdline.txt's same keys win
void CKernel::LoadDisplaySettings (boolean bCard)
{
	char Text[128] = "";
	if (bCard)
	{
		FIL f;
		UINT n = 0;
		if (f_open (&f, "SD:/.rytm/display.txt", FA_READ) == FR_OK)
		{
			f_read (&f, Text, sizeof Text - 1, &n);
			f_close (&f);
		}
		Text[n] = 0;
	}
	auto Value = [&] (const char *pKey, unsigned nNow, unsigned nMin, unsigned nMax) -> unsigned
	{
		unsigned v = nNow;
		const char *p = strstr (Text, pKey);
		if (p != 0 && p[strlen (pKey)] == '=')
		{
			unsigned n = 0;
			boolean bDigits = FALSE;
			for (p += strlen (pKey) + 1; *p >= '0' && *p <= '9'; p++) { n = n * 10 + (*p - '0'); bDigits = TRUE; }
			if (bDigits && n >= nMin && n <= nMax) v = n;
		}
		const char *o = m_Options.GetAppOptionString (pKey, 0);
		if (o != 0)
		{
			unsigned n = m_Options.GetAppOptionDecimal (pKey, v);
			if (n >= nMin && n <= nMax) v = n;
		}
		return v;
	};
	m_nSaverMin = Value ("screensaver", 60, 0, 1440);
	m_nBright = Value ("brightness", 100, 5, 100);
	m_bTouchHidden = Value ("touchtest", 1, 0, 1) == 0;
	m_Ui.screensaver = (int) m_nSaverMin;
	m_Ui.brightness = (int) m_nBright;
	m_Ui.touch_hidden = m_bTouchHidden;
	if (m_pDisplay != 0) m_pDisplay->SetScreensaver (m_nSaverMin);
	m_Logger.Write (From, LogNotice, "display: screensaver %u min, brightness %u %% (%s)", m_nSaverMin, m_nBright,
			Text[0] ? "/.rytm/display.txt" : "defaults");
}

void CKernel::SaveDisplaySettings (void)
{
	m_bSaveDisplay = FALSE;
	CString s;
	s.Format ("screensaver=%u\nbrightness=%u\ntouchtest=%u\n", m_nSaverMin, m_nBright, m_bTouchHidden ? 0u : 1u);
	boolean bOK = SdWriteWhole ("/.rytm/display.txt", (const char *) s, s.GetLength ());
	m_Logger.Write (From, bOK ? LogNotice : LogWarning, "/.rytm/display.txt %s", bOK ? "written" : "NOT written");
}

void CKernel::MIDIPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength)
{
	// the Rytm's MIDI to the computer (the Sidecar's filter drops the host link and MIDI-CI): Circle hands over each
	// event packet's MIDI bytes, its header byte is the one before them in the endpoint's buffer
	if (CSidecarGadget::Get () != 0) CSidecarGadget::Get ()->FromRytmMIDI (pPacket - 1);
	if (!CObUsb::Active ()) return;	// the TCP link is the host: a real Rytm's MIDI is not the engine's
	if (nLength >= 3 && (pPacket[0] & 0xF0) == 0x90 && pPacket[2] != 0) CObUsb::CaptureMarkMIDI ();
	for (unsigned i = 0; i < nLength; i++)
		if (!g_MidiIn.push (pPacket[i])) s_nMidiInLost++;	// a full queue: counted, as the TCP link's
	asm volatile ("dsb sy; sev");
}

void CKernel::MIDIRemoved (CDevice *pDevice, void *pContext)
{
	((CKernel *) pContext)->m_pMIDI = 0;
}

// The Rytm's USB MIDI interface (Circle's umidi1) once enumerated: read only.
void CKernel::UpdateMIDI (void)
{
	if (m_pMIDI != 0) return;
	// umidiN in Circle's numbering: the gadget's own MIDI device (USB-C, gadget.h) takes a number too, so the
	// Rytm's is the first one that is not the gadget's
	CUSBMIDIDevice *pMIDI = 0;
	for (unsigned n = 1; n <= 4 && pMIDI == 0; n++)
	{
		CString Name;
		Name.Format ("umidi%u", n);
		CDevice *p = CDeviceNameService::Get ()->GetDevice (Name, FALSE);
		if (p != 0 && !(m_pGadget != 0 && m_pGadget->IsMIDIDevice (p))) pMIDI = (CUSBMIDIDevice *) p;
	}
	if (pMIDI == 0) return;
	pMIDI->RegisterRemovedHandler (MIDIRemoved, this);
	pMIDI->RegisterPacketHandler (MIDIPacketHandler);
	m_Usb.SetMIDI (0);				// a replug's umidi1 may sit at the old one's address: the sender sees it as new
	m_pMIDI = pMIDI;
	m_Logger.Write (From, LogNotice, "midi: the Rytm's USB MIDI found, listening; the engine's replies %s",
			m_Options.GetAppOptionDecimal ("tohost", 1) != 0 ? "go to it (host link)" : "are dropped (tohost=0)");
}

// The display's state, read the way /status reads it (plain reads of what other cores write: a stale value at worst), so
// a change from the web page, the serial console or the display shows on all of them. Once a second (LogStats) and
// shortly after a display command.
void CKernel::PushUi (void)
{
	sidecar_ui_status_t &u = m_Ui;
	u.rytm = CObUsb::Streaming () || m_pMIDI != 0;
	u.underruns = g_Output.Underflows ();
	for (int v = 0; v < SIDECAR_UI_TRACKS; v++) u.track[v] = bridge::machine_name (v);
	u.note = m_pTransport == &m_Tcp ? "link: tcp" : m_pTransport == &m_SelfTest ? "link: self-test" : "link: usb";
	if (powerstate::short_warning (m_Power) != 0) u.note = powerstate::short_warning (m_Power);	// red on the display
	u.notes = CEngineCores::NotesOn ();
	u.out_tracks = g_Output.GetMode () == Output::kTracks;
	u.mac = m_pSidecar != 0 ? (m_pSidecar->MacOn () ? 1 : 0) : -1;
	u.screensaver = (int) m_nSaverMin;
	u.brightness = (int) m_nBright;
	u.touch_hidden = m_bTouchHidden;
	u.drone = CEngineCores::DroneVoice ();
	u.fifo = CEngineCores::FifoTarget ();
	u.web = m_bWeb;
	u.rec = rec_pi::UiState (&u.rec_secs);
	sendfx_pi::View fx;
	sendfx_pi::Read (&fx);
	memcpy (u.fx_type, fx.type, sizeof u.fx_type);
	memcpy (u.fx_post, fx.post, sizeof u.fx_post);
	memcpy (u.send, fx.send, sizeof u.send);
	um::UiView uv;				// the user machines: the project and U1..U5 (published by the control core)
	if (um::ui_view (&uv))
	{
		strncpy (u.project, uv.project, sizeof u.project - 1);
		for (int i = 0; i < 5; i++)
		{
			strncpy (u.uslot[i], uv.slot[i].name, sizeof u.uslot[i] - 1);
			u.uslot_state[i] = uv.slot[i].state;
		}
	}
	CString Text;
	Text.Format ("%u.%u.%u.%u", m_IP[0], m_IP[1], m_IP[2], m_IP[3]);
	strncpy (u.ip, (const char *) Text, sizeof u.ip - 1);
	if (CWebStart::Hostname ()[0])
	{
		Text.Format ("%s.local", CWebStart::Hostname ());
		strncpy (u.host, (const char *) Text, sizeof u.host - 1);
	}
	strncpy (u.link, m_pTransport->Name (), sizeof u.link - 1);
	strncpy (u.gadget, m_pGadgetMode, sizeof u.gadget - 1);
	strncpy (u.build, SIDECAR_VERSION_FULL, sizeof u.build - 1);
	Text.Format ("%s %s", CIRCLE_NAME, CIRCLE_VERSION_STRING);
	strncpy (u.circle, (const char *) Text, sizeof u.circle - 1);
	m_pDisplay->SetStatus (u);
}

// A display control (core 0, inside the display's Update): the serial console's parser, as the web page does. The
// commands only queue (core 3 applies them), so this returns at once; the state is re-read 150 ms later.
void CKernel::UiCommand (const char *pCmd, void *pParam)
{
	CKernel *k = (CKernel *) pParam;
	k->m_Logger.Write (From, LogNotice, "display: command \"%s\"", pCmd);
	if (k->m_pDisplay != 0) k->m_pDisplay->Activity ();
	k->m_Console.Command (pCmd);
	k->m_nUiAt = k->m_Timer.GetClockTicks () + 150000;
	k->m_bUiPending = TRUE;
}

void CKernel::LogStats (void)
{
	m_pTransport->LogStats (&m_Logger);
	CEngineCores::TStats s;
	CEngineCores::TakeStats (&s);
	auto pct = [&] (u64 t) { return (unsigned) (t * 1000 / s.nTicks); };	// tenths of a percent
	CString Mach;
	for (int k = 0; k < bridge::kMachines; k++)
	{
		unsigned p = pct (s.nMach[k]);
		if (p)
		{
			CString One;
			One.Format ("%s %u.%u ", bridge::stat_name (k), p / 10, p % 10);
			Mach.Append (One);
		}
	}
	CString Chans;		// the MIDI channels (1..16) that sent note-ons this second
	for (unsigned c = 0; c < 16; c++)
	{
		if (!(s.nNoteChans & (1u << c))) continue;
		CString One;
		One.Format (Chans.GetLength () ? ",%u" : " ch %u", c + 1);
		Chans.Append (One);
	}
	unsigned c1 = pct (s.nBusy[1]), c2 = pct (s.nBusy[2]), c3 = pct (s.nBusy[3]);
	bridge::RuiMore More = {};			// the remote UI's SIDECAR page (core 3 reads it)
	unsigned nRec = 0;
	int nRecState = rec_pi::UiState (&nRec);	// 0 off (idle), 1 armed, 2 recording
	More.rec = nRecState == 2 ? 3 : nRecState == 1 ? 2 : 1;
	More.rec_secs = nRec;
	boolean bWifi = m_pWifi != 0 && m_pWifi->Mode () == CWifi::ModeNet;
	int nNet = m_bNetOK && m_Net.IsRunning () ? 2 : 1;	// up / down only: never the Wi-Fi's name or key
	(bWifi ? More.wifi : More.eth) = nNet;
	if (nNet == 2) m_Net.GetConfig ()->GetIPAddress ()->CopyTo (More.ip);
	um::UiView Uv;
	if (um::ui_view (&Uv))
		for (int i = 0; i < 5; i++) strncpy (More.user[i], Uv.slot[i].name, sizeof More.user[i] - 1);
	bridge::rui_status (m_pTransport->Running () ? 1 : 0, g_Output.Underflows (), (int) (c1 + 5) / 10, (int) (c2 + 5) / 10,
			    (int) (c3 + 5) / 10, &More);
	if (m_pDisplay != 0)
	{
		static unsigned s_nBlocksTotal;
		s_nBlocksTotal += s.nBlocks;
		m_Ui.blocks = s_nBlocksTotal;
		m_Ui.load[0] = -1;				// core 0 is not measured
		m_Ui.load[1] = (int) (c1 + 5) / 10;
		m_Ui.load[2] = (int) (c2 + 5) / 10;
		m_Ui.load[3] = (int) (c3 + 5) / 10;
		PushUi ();
		static unsigned s_nDisplayLog;
		if (++s_nDisplayLog % 10 == 0)			// every 10 s: the redraw cost on this core
			m_Logger.Write (From, LogNotice, "display: %ux%u, input %s, longest update %u us%s", m_pDisplay->GetWidth (),
					m_pDisplay->GetHeight (), m_pDisplay->GetInput (), m_pDisplay->TakeMaxUpdateUs (),
					m_pDisplay->TakeTouchDiag ());
	}
	m_Logger.Write (From, LogNotice,
		"engine: fifo min %u max %u target %u under %u | blocks %u max %u us | cpu c1 %u.%u c2 %u.%u c3 %u.%u %% | "
		"pass max %u us | %s| midi %u B (lost %u) sysex %u notes %u%s | to-host sent %u B %u msgs fail %u dropped %u | peak %u/1000 | %u C",
		s.nFifoMin, s.nFifoMax, CEngineCores::FifoTarget (), g_Output.Underflows (), s.nBlocks, s.nMaxBlockUs,
		c1 / 10, c1 % 10, c2 / 10, c2 % 10, c3 / 10, c3 % 10, s.nMaxPassUs, (const char *) Mach, s.nMidiBytes, s_nMidiInLost, s.nSysEx, s.nNotes, (const char *) Chans,
		m_Usb.MIDIOutBytes (), m_Usb.MIDIOutMessages (), m_Usb.MIDIOutFailed (),
		m_Usb.MIDIOutDropped () + m_SelfTest.MIDIOutDropped () + m_Tcp.MIDIOutDropped () + s_nToHostFull + bridge::to_host_dropped (), (unsigned) (g_Output.TakePeak () * 1000),
		m_CPUThrottle.GetTemperature ());
	static unsigned s_nPowerTick;
	if (++s_nPowerTick % 10 == 0) ReadPower (FALSE);	// every 10 s (a mailbox call)
	if (s_nPowerTick % 60 == 0 && m_bWeb) CWebServer::LogStats (&m_Logger);	// the web page's connections, once a minute
	if (m_pGadget != 0) m_pGadget->LogStats (&m_Logger);
	if (m_pSidecar != 0) m_pSidecar->LogStats (&m_Logger);
	g_Guard.LogNews (&m_Logger);
	if (g_Guard.MutedMask ())
	{
		CString Muted;
		for (unsigned v = 0; v < CGuard::Voices; v++)
		{
			if (!g_Guard.Muted (v)) continue;
			CString One;
			One.Format ("!%s(%u) ", bridge::machine_name (v), v);
			Muted.Append (One);
		}
		m_Logger.Write (From, LogNotice, "engine: muted %s", (const char *) Muted);
	}
}

// The supply (power_state.h): get_throttled read without clearing (request value 0 keeps the sticky bits), and on the
// Pi 5 the firmware's /chosen/power/max_current (read once: the PD negotiation is done before the kernel starts).
// Logged at boot, at once when the flags change (a warning while a problem shows), and once a minute.
void CKernel::ReadPower (boolean bLog)
{
	static boolean s_bMaxRead = FALSE;
	static unsigned s_nLogged = ~0u, s_nCalls = 0;
	if (!s_bMaxRead)
	{
		s_bMaxRead = TRUE;
		m_Power.board = RASPPI >= 5 ? 5 : 4;	// the warning names this board's official supply
		const CDeviceTreeBlob *pDTB = CMachineInfo::Get ()->GetDTB ();
		const TDeviceTreeNode *pNode = pDTB != 0 ? pDTB->FindNode ("/chosen/power") : 0;
		const TDeviceTreeProperty *pProp = pNode != 0 ? pDTB->FindProperty (pNode, "max_current") : 0;
		if (pProp != 0 && pDTB->GetPropertyValueLength (pProp) == 4) m_Power.max_ma = (int) pDTB->GetPropertyValueWord (pProp, 0);
	}
	CBcmPropertyTags Tags;
	TPropertyTagSimple Throttled;
	Throttled.nValue = 0;
	m_Power.known = Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled, sizeof Throttled, 4);
	m_Power.throttled = m_Power.known ? Throttled.nValue : 0;
	unsigned nKey = m_Power.known ? m_Power.throttled : ~1u;
	boolean bChanged = nKey != s_nLogged;
	if (bLog || bChanged || ++s_nCalls % 6 == 0)
	{
		char Line[256];
		powerstate::line (m_Power, Line, sizeof Line);
		m_Logger.Write (From, powerstate::warning (m_Power) != 0 ? LogWarning : LogNotice, "power: %s", Line);
		s_nLogged = nKey;
	}
}

void CKernel::BootStep (unsigned nStep)
{
	if (m_pDisplay != 0) m_pDisplay->SplashStep (nStep);
}

void CKernel::StopAndReboot (const char *pWhy, boolean bTryboot)
{
	m_Logger.Write (From, LogNotice, "reboot: %s - stopping the %s stream first", pWhy, m_pTransport->Name ());
	m_pTransport->Stop ();
	if (m_pTransport != &m_Usb) m_Usb.Stop ();		// a Rytm that streams anyway (no-op without one)
	m_Timer.MsDelay (50);					// the log line out
	m_Watchdog.Stop ();
	if (bTryboot && !CSdFiles::SetTryboot ()) m_Logger.Write (From, LogWarning, "reboot: the tryboot flag was refused (a plain boot follows)");
	reboot ();
}

void CKernel::Run (void)
{
	m_Serial.RegisterMagicReceivedHandler (RebootMagic, RebootRequest);
	m_Logger.Write (From, LogNotice, "boot timestamp %u us; waiting for the Rytm (1935:0b48, OVERBRIDGE mode)",
			m_Timer.GetClockTicks ());
	m_nLastStats = m_Timer.GetTicks ();
	if (m_Options.GetAppOptionDecimal ("selftest", 0) == 1) SetSelfTest (TRUE, "cmdline");
	m_Watchdog.Start (15);
	for (;;)
	{
		m_Watchdog.Start (15);	// fed: a hang of this loop reboots in 15 s (the BCM watchdog's maximum: a TCP connect may block for seconds)
		if (m_Console.TakeWatchdogTest ())
		{
			m_Logger.Write (From, LogWarning, "watchdog test: core 0 hangs (the watchdog reboots in 15 s)");
			for (;;) asm volatile ("");	// interrupts still run: the log line goes out
		}
		boolean bPlugAndPlay = m_USBHCI.UpdatePlugAndPlay ();
		if (m_pGadget != 0) m_pGadget->Poll ();
		if (m_pSidecar != 0) m_pSidecar->Poll ();
		UpdateMIDI ();
		if (m_pTransport == &m_SelfTest && CObUsb::Streaming ()) SetSelfTest (FALSE, "the Rytm streams");
		if (m_pTransport == &m_SelfTest && m_pMIDI != 0) SetSelfTest (FALSE, "the Rytm's USB MIDI is up");
		int nSelfTest = m_Console.TakeSelfTest ();
		if (nSelfTest >= 0) SetSelfTest (nSelfTest == 1, "console");
		m_SelfTest.SetQuiet (m_Console.Dumping ());
		m_CPUThrottle.Update ();
		m_Usb.SetMIDI (m_pMIDI);			// umidi1 (or 0): the engine's replies to the Rytm
		m_pTransport->Poll ();
		if (m_pTransport != &m_Tcp) m_Tcp.Reap ();	// a connect abandoned by :link=usb is closed when it ends
		m_Console.Poll ();
		rec_pi::Poll ();			// the recorder's ring to the card: one 192 KB write a pass at most
		if (m_pDisplay != 0)
		{
			m_pDisplay->Update (bPlugAndPlay);	// a redraw at most every 10 ms, dirty areas only
			if (m_bUiPending && (int) (m_Timer.GetClockTicks () - m_nUiAt) >= 0)
			{
				m_bUiPending = FALSE;
				PushUi ();
			}
		}
		if (m_bSaveDisplay) SaveDisplaySettings ();
		char Link[40];
		if (m_Console.TakeLink (Link, sizeof Link)) UseLink (Link);
		if (s_bRebootRequested) StopAndReboot ("the flasher's magic");
		if (g_Guard.FatalWhy () != 0) StopAndReboot (g_Guard.FatalWhy ());
		if (m_pWifi->TakeReboot ()) StopAndReboot ("the Wi-Fi settings (a join, or a failed one's revert)");
		CSdFiles::TReboot SdReboot = CSdFiles::TakeReboot ();
		if (SdReboot != CSdFiles::RebootNone) StopAndReboot ("asked on the SD file port", SdReboot == CSdFiles::RebootTry);
		if (m_Timer.GetTicks () - m_nLastStats >= HZ || m_Console.TakeStatsNow ())
		{
			m_nLastStats = m_Timer.GetTicks ();
			if (m_Console.Dumping ()) m_Timer.MsDelay (25);	// the TX buffer drains: a log line is not cut
			LogStats ();
		}
		asm volatile ("dsb sy; sev");			// core 3's heartbeat: the drone and card work run without traffic
		// the self-test's pacer: 7 frames are 146 us. A busy wait never yields, so the network's tasks starve while
		// it runs - fine only because the self-test is refused under link=tcp (SetSelfTest)
		if (m_SelfTest.Running ()) m_Timer.usDelay (20);
		else
		{
			unsigned t = m_Timer.GetClockTicks ();
			m_Scheduler.MsSleep (1);		// the network's tasks run meanwhile
			t = m_Timer.GetClockTicks () - t;	// how late the loop wakes: core 0's tasks (Wi-Fi's polling) measured
			s_nLoopN++;
			s_nLoopSum += t;
			if (t > s_nLoopMax) s_nLoopMax = t;
		}
	}
}
