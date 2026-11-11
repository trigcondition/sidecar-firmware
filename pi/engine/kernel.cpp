// pi/engine/kernel.cpp - the Pi engine (docs/pi-engine.md). Core 0: USB (the Rytm's Overbridge stream in the xHCI
// interrupt, its USB MIDI read-only), the network (Circle's scheduler; the TCP link to rytm-emu, tcp_link.h, with
// link=tcp), the console, the stats, the clean stop before a reboot; without a Rytm, the self-test transport
// (selftest.h). Cores 1-3: cores.cpp.
#include "kernel.h"
#include "idlewait.h"
#include "ob_follow.h"	// CObFollow::HaltResets (/status "usbhalt")
#include "mbtrace.h"
#include "blackbox_pi.h"	// the black box (2026-10-30): the whole log appended to /.rytm/blackbox.log
#include "crumb.h"		// the crash breadcrumb (2026-10-29): kept over a watchdog reset, reported here at the next boot

static volatile unsigned s_nTempC;	// the SoC temperature as the stats line last read it (once a second); /status and the UI show this
#include "um_pi.h"
#include "user/um.h"
#include "sidecar_display.h"
#include "sidecar_splash.h"	// the boot screen's look (uistyle= on the cmdline)
#include "sidecar_seqed.h"
#include "wgview_pi.h"		// the display's WAVETABLE / GRANULAR view
#include "preset_pi.h"		// the display's JUNO / 6-OP FM preset pages
#include "chopview_pi.h"		// the display's CHOP page
#include "seditview_pi.h"		// the display's SAMPLE EDIT page (core/samp/sedit.h)
#include "song_pi.h"		// the display's SONG page (core/song)
#include "morph_pi.h"		// 0.5.0: the display's MORPH page (core/mod/scene.h)
#include "voice_pi.h"		// the display's VOICE page, the Rytm's UP + T1..T5 (rui1036b)
#include "insfx_pi.h"		// the display's INSERT FX page (fx-ins)
#include "scope_pi.h"		// touch2: the display's SCOPE page (UP + T9)
#include "status_pi.h"		// touch2: the display's SIDECAR STATUS page (UP + T10)
#include "../ui/sidecar_scope.h"
#include "../ui/sidecar_dot.h"		// sidecar_dot_look (UX review L1)
#include "../ui/sidecar_status.h"
#include "../ui/sidecar_project.h"
#if SIDECAR_SEQED
#include "seqed_pi.h"
#include "rytmpat_pi.h"		// the Rytm pattern view (its SysEx dump)
#include "../ui/sidecar_rytmpat.h"
static rytm_pattern_t s_RytmPat;	// the newest decoded Rytm pattern: the PAT page's, VOICE's p-locks
static int s_nRytmPatIndex = -1;
#endif

#include <circle/alloc.h>
#include <circle/bcmpropertytags.h>
#include <circle/devicetreeblob.h>
#include <circle/machineinfo.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/memory.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <circle/util.h>
#include <circle/version.h>
#if RASPPI >= 5
#include <circle/bcm2712.h>		// the power button's GPIO
#include <circle/macros.h>
#endif
#include <fatfs/ff.h>

#include "bridge.h"
#include "shared.h"
#include "midiout_q.h"	// the MIDI machine's USB-A queues
#include "usb_box.h"	// which USB MIDI device is the host link
#include "hello_gate.h"
#include "box_list.h"
#include "device/a4.h"
#include "card_pass.h"	// MIDI between the card requests
#include "sendfx_pi.h"
#include "paircode_pi.h"	// the pairing code that gates the network (paircode.h)
#include "usbc_pi.h"		// the USB-C mode for the SIDECAR MENU > USB-C (usb_mode.h)
#include "midibridge_pi.h"	// the Rytm MIDI bridge (midibridge.h): off at every boot
#include "sdfiles.h"
#include "wifi.h"
#include "usbnet_pi.h"		// the USB-C network (docs/usb-network.md)
#include "rec_pi.h"
#include "bundle_pi.h"		// project bundles (core/bundle, docs/BUNDLES.md)
#include "lib_pi.h"		// the sample library (core/lib, docs/LIBRARY.md)
#include "fmt/fmt.h"		// the format registry: display.txt keeps the keys this build does not read (class S)
#include "stor_pi.h"
#include "version.h"
#include <circle/new.h>

// why OVERBRIDGE is not in this build (the boot line for usb=mirror, usbmode=overbridge refused): the Circle tree has no
// mirror patches (pi/engine/build.sh puts them on the Pi 4 and the Pi 5 trees)
static const char NoMirrorWhy[] = "no circle-gadget-mirror.patch";

// The Pi 5 engine test mode (PI5_TESTMODE=1 builds only; docs/pi5.md "Engine test mode"): testsecs=, its watchdog and
// reboot, /usbtest.log, testcard=1. A plain PI5_GADGET=1 build has the gadget and none of this, so a release card takes it.
#ifdef SIDECAR_PI5_TESTMODE
#if RASPPI < 5 || defined (SIDECAR_NO_GADGET)
#error "SIDECAR_PI5_TESTMODE: a Pi 5 gadget build only (PI5_GADGET=1 PI5_TESTMODE=1 RASPPI=5)"
#endif
extern bool g_bCardOff;			// platform_pi.cpp: testsecs=
extern bool g_bCardGuard;			// card_guard.cpp: testcard=1
#endif

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
// BenchChains' results (core 0, before cores 1-3), LogBench's rows: [0] the master chain, [1..] the send TYPE insert sets
static const int ChainBenches = 3;
static struct { u64 nTicks; u32 nWorst; } s_ChainBench[ChainBenches];
static const char RebootMagic[] = "RYTM-REBOOT";
static volatile boolean s_bRebootRequested = FALSE;
static void RebootRequest (void) { s_bRebootRequested = TRUE; }   // UART IRQ: core 0's loop stops cleanly first
static unsigned s_nMidiInLost;		// the Rytm's USB MIDI bytes lost: a full g_MidiIn (the stats line's "lost")
// the bundle job's rescans (console commands, as the web page's uploads send them; core 0)
static CConsole *s_pBundleConsole = 0;
static void BundleCommand (const char *pCmd)
{
	if (s_pBundleConsole != 0) s_pBundleConsole->Command (pCmd);
}

static unsigned s_nLoopMax, s_nLoopN, s_nLoopSum;	// core 0's loop: its MsSleep (1) in us (/status "core0", reset when read)

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
	m_bTouchCal (FALSE),
	m_TouchCal (),
	m_nUiStyle (0),
	m_bSaveDisplay (FALSE),
	m_nResW (0),
	m_nResH (0),
	m_nResFileW (0),
	m_nResFileH (0),
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
		g_nToHostFull += nLength;
		return;
	}
	if (g_MidiOut.size () + 2 + nLength > kMidiOutBytes)
	{
		g_nToHostFull += nLength;
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

static void CrumbPanic (void) { crumb::Why ("a panic (the log's last line says which)"); }
static u32 s_nPmRsts;			// PM_RSTS as this boot found it (crumb.h's report)

// The previous boot's breadcrumb on the card, /.rytm/prevboot.txt: the report's lines, then its whole log tail. Once,
// 60 s into the boot (a card that hangs a write would not hang every boot at once: no reboot loop), in its own section
// (41): skipped when the previous boot's core 0 died there. A planned reboot's (an update, the page's reboot) keeps the
// file there: the last crash's.
static void CrumbToCard (CLogger *pLogger)
{
	const char *From = "crumb";
	if (!crumb::had_prev ()) return;
	if (crumb::PrevPlanned ())
	{
		pLogger->Write (From, LogNotice, "crumb: the previous boot ended with a planned reboot: /.rytm/prevboot.txt kept (the last crash's)");
		return;
	}
	if (crumb::PrevDiedIn (41))
	{
		pLogger->Write (From, LogWarning, "crumb: the previous boot hung writing /.rytm/prevboot.txt: not written again");
		return;
	}
	crumb::Section Cs (41);
	static char s_Text[crumb::kLog + 8192];
	unsigned n = 0;
	crumb::Report ([&n] (const char *pLine) {
		while (*pLine && n < 8192 - 2) s_Text[n++] = *pLine++;
		s_Text[n++] = '\n';
	}, s_nPmRsts, 0);
	n += (unsigned) core_snprintf (s_Text + n, 128, "---- the previous boot's log, its last %u KB ----\n", crumb::kLog / 1024);
	n += crumb::PrevLog (s_Text + n, sizeof s_Text - n);
	boolean bOK = SdWriteWhole ("/.rytm/prevboot.txt", s_Text, n);
	pLogger->Write (From, bOK ? LogNotice : LogWarning, "crumb: the previous boot's report and log (%u bytes) %s /.rytm/prevboot.txt",
			n, bOK ? "in" : "NOT written to");
}

boolean CKernel::Initialize (void)
{
	crumb::Begin ();				// first: the previous boot's breadcrumb aside, before anything logs (crumb.h)
	boolean bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (921600);
	if (bOK) bOK = m_Logger.Initialize (&m_LogTee);	// the serial port, and the web page's copy
	if (bOK) bOK = m_Timer.Initialize ();
	if (!bOK) return FALSE;
	m_Logger.Write (From, LogNotice, "==== rytm-pi engine ==== Sidecar " SIDECAR_VERSION_FULL ", CPU %u MHz", m_CPUThrottle.GetClockRate () / 1000000);
#ifdef SIDECAR_PI5_TESTMODE
	// testsecs=S (docs/pi5.md "Engine test mode"): a run on a card that is not Sidecar's, a tryboot on Raspberry Pi OS. The
	// card is never mounted for the engine, so nothing is written to that system's boot partition; the watchdog runs
	// from here on, and at S seconds the Pi reboots, which ends the tryboot. testcard=1 with it: the engine uses the card
	// as usual, projects, presets and plugins on a tryboot, but changes nothing outside Sidecar's folders (card_guard.h)
	m_nTestSecs = m_Options.GetAppOptionDecimal ("testsecs", 0);
	if (m_nTestSecs != 0)
	{
		m_bTestCard = m_Options.GetAppOptionDecimal ("testcard", 0) != 0;
		g_bCardOff = !m_bTestCard;
		g_bCardGuard = m_bTestCard;
		m_Watchdog.Start (15);
		m_Logger.Write (From, LogNotice, "testsecs=%u: %s, the watchdog on, a reboot at the end", m_nTestSecs,
				m_bTestCard ? "testcard=1: the card in use, Sidecar's folders only" : "no card for the engine");
	}
#endif
	m_Logger.RegisterPanicHandler (CrumbPanic);
	s_nPmRsts = read32 (ARM_PM_RSTS);
	crumb::Report ([this] (const char *pLine) { m_Logger.Write ("crumb", LogNotice, "%s", pLine); }, s_nPmRsts, 6144);
	ReadPower (TRUE);				// the supply's state in the boot log (a weak one: Wi-Fi and the SD fail first)
	// the boot screen first (sidecar_splash.h; display=0: none), one bar step per stage below, LVGL's home screen last
	if (m_Options.GetAppOptionDecimal ("display", 1) != 0)
	{
		unsigned ts = m_Timer.GetClockTicks ();
		m_pDisplay = new CSidecarDisplay (m_Options.GetWidth (), m_Options.GetHeight ());
		const char *pStyle = m_Options.GetAppOptionString ("uistyle", 0);	// the card is not read yet: cmdline only
		sidecar_splash_style (pStyle != 0 && (strcmp (pStyle, "sidecar") == 0 || strcmp (pStyle, "classic") == 0) ? 1 : 0);
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
	s_pBundleConsole = &m_Console;
	bundle_pi::init (BundleCommand);	// project bundles: the job on this core's loop (bridge::begin ran the boot recovery)
	lib_pi::begin (bCard);			// the sample library: its index read and the card walked, a piece a pass (lib_pi.h)
	// the card queue (core/msamp/cardq.h): the streams' reads on this core, before the recorder's writes; cardq=0: on
	// core 3, as before
	bridge::card_worker (m_Options.GetAppOptionDecimal ("cardq", 1) != 0);
	BootStep (3);
	m_Usb.SetSend (m_Options.GetAppOptionDecimal ("tohost", 1) != 0);	// tohost=0: the replies dropped, as before
	boolean bBench = m_Options.GetAppOptionDecimal ("bench", 1) != 0;
	LoadClock (bCard);				// the CPU clock option, before the bench: the bench measures the clock the engine plays at
	if (bBench) CEngineCores::BenchCore (0);		// cores 1-3 bench as they start (LogBench)
	if (bBench) BenchChains ();			// the master / send TYPE insert rows: before cores 1-3 start (LogBench)
	SelfCheck ();
	// boot defaults (the user's decision, 2026-10-16): out=tracks, the voices on their tracks' inputs, through the
	// Rytm's filters and FX (the mod build); MIDI notes off (notes=on is for a stock Rytm, with out=main); no drone
	// (only :drone=). cmdline.txt or the console override them.
	g_Output.SetMode (strcmp (m_Options.GetAppOptionString ("out", "tracks"), "main") == 0 ? Output::kMain
											      : Output::kTracks);
	g_Output.SetOutControl (&core::out::control);	// the voices' OUT routes (core/fx/out.h, docs/STEREO.md)
	// poolmono=1: stereo files summed at load (half the pool RAM for them; PRE / POST voices then play them centred)
	bridge::pool_mono (m_Options.GetAppOptionDecimal ("poolmono", 0) != 0);
	// trimpre= / trimpost= (tenths of a dB, may be negative): the PRE / POST pairs' calibration from stereo=measure
	// (docs/STEREO.md "Measurements"); the console's stereo=trim:pre|post:DB10 for the session
	bridge::stereo_trim (0, atoi (m_Options.GetAppOptionString ("trimpre", "0")));
	bridge::stereo_trim (1, atoi (m_Options.GetAppOptionString ("trimpost", "0")));
	// fifo=: the target for every link; without it each link's own (UseLink: FifoUsb, FifoTcp). split= 4 is the
	// plan's default (docs/pi-engine.md "Measurements")
	if (m_Options.GetAppOptionString ("fifo", 0) != 0) CEngineCores::FixFifoTarget ();
	CEngineCores::Configure (m_Options.GetAppOptionDecimal ("fifo", FifoUsb),
				 strcmp (m_Options.GetAppOptionString ("notes", "off"), "on") == 0
				 || strcmp (m_Options.GetAppOptionString ("notes", "off"), "1") == 0);
	CEngineCores::SetSplit (m_Options.GetAppOptionDecimal ("split", 4));	// voices 0-3 core 1, 4-7 core 2
	CEngineCores::SetIdleWfe (m_Options.GetAppOptionDecimal ("idlewfe", 1) != 0);	// docs/pi-engine.md "Idle sleep"
	ApplyBudget (m_nClockMHz, TRUE);			// the voice guard's budget, for the clock the engine runs at
	m_Console.Initialize ();
	BootStep (4);
	CEngineCores::BenchStart (bBench);
	if (!m_Cores.Initialize ()) return FALSE;		// cores 1-3 start (cores.cpp)
	BootStep (5);
	if (bBench) LogBench ();
	BootStep (6);
	if (!m_USBHCI.Initialize ()) return FALSE;		// the Rytm is enumerated by plug-and-play in Run
	// the volumes besides the card (stor_pi.h, docs/STORAGE.md): USB drives as they come (core 0's loop); the Pi 5's
	// NVMe SSD now if one answers, only with nvme=1 in cmdline.txt: the probe with the Rytm already on USB has been
	// followed by the Rytm dropping off (issue 11; cause not proven)
	stor_pi::Init (&m_USBHCI, &m_Interrupt, m_Options.GetAppOptionDecimal ("nvme", 0) != 0);
	BootStep (7);
	// the HDMI display and its USB touch (docs/pi-display.md), driven from core 0's loop: display=0 turns it off.
	// LVGL's home screen replaces the boot screen at the first Update.
	if (m_pDisplay != 0)
	{
		BootStep (8);
		LoadDisplayRes (bCard);			// res=: the UI's size (800x480 / 1024x600 / 1280x720), before LVGL starts
		m_pDisplay->SetUiSize (m_nResW, m_nResH);
		if (m_pDisplay->Initialize ())
		{
			m_pDisplay->SetCommandHandler (UiCommand, this);
			voice_pi::OwnPages ();		// UP + T7: the touchscreen's REC page (and the signal that a panel is up)
		}
		else m_pDisplay = 0;			// no frame buffer: left alone (no destructor)
	}
	LoadDisplaySettings (bCard);
	m_Console.SetCore0Handler (Core0Command, this);	// screensaver=, brightness= (and the Sidecar's commands)
	// USB-C, the computer's side: gadget=sidecar (default: UAC2 + USB MIDI, sidecar_gadget.h), midi (P1's MIDI
	// echo, gadget.h) or 0. The default can be built in (GADGET=midi pi/engine/build.sh): the serial bootloader
	// card's cmdline.txt is not edited from the Mac. A gadget that fails to start is left alone (no destructor).
#if RASPPI >= 5
	// what pi/circle/sdboot.py and make-sd-dual.sh tell a Pi 5 kernel by, gadget or not (older ones: the line below)
	m_Logger.Write (From, LogNotice, "board: Raspberry Pi 5 build (kernel_2712.img)");
#endif
	const unsigned nGadgetT0 = CTimer::GetClockTicks ();	// net=auto's wait counts from here (UsbNetProbe)
#ifdef SIDECAR_NO_GADGET
	const char *pGadgetMode = "0";			// Pi 5 without PI5_GADGET=1 (docs/pi5.md); gadget= is ignored
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
		m_nUsbModes = usb_mode::modes (CSidecarGadget::MirrorBuilt (), true);
		ApplyUsbMode ();				// the USB-C mode (usb_mode.h): display.txt's usbmode=, else usb=
		if (pSidecar->Start ()) m_pSidecar = pSidecar;
		else m_nUsbModes = 0;
		const boolean bNoMirror = m_UsbMode == usb_mode::kMirror && !CSidecarGadget::MirrorBuilt ();
		m_Logger.Write (From, LogNotice, "gadget: USB-C mode %s (%s)%s%s%s", m_UsbMode == usb_mode::kMirror ? "OVERBRIDGE" : "CLASS COMPLIANT",
				usb_mode::from_name (m_UsbFrom), bNoMirror ? ": not in this build (" : "", bNoMirror ? NoMirrorWhy : "",
				bNoMirror ? "), CLASS COMPLIANT runs" : "");
		if (m_pSidecar != 0 && m_pSidecar->Mirror ())	// OVERBRIDGE, usb=mirror (docs/usb-mirror.md)
			m_Logger.Write (From, LogWarning, "gadget: OVERBRIDGE (usb=mirror): USB-C is the Sidecar device until the box has been read, "
					"then the box itself (the Sidecar device again after the box has been gone mirrorhold s)");
		m_Logger.Write (From, m_pSidecar ? LogNotice : LogError, "gadget: USB-C Sidecar %s (%u in / %u out, 48 kHz, 24-bit, + MIDI)",
					m_pSidecar ? "up" : "failed to start", CSidecarGadget::ChIn, CSidecarGadget::ChOut);
		if (m_pSidecar != 0 && m_pSidecar->OsUpdate ())
			m_Logger.Write (From, LogWarning, "gadget: usbmidi=os:%s: Elektron OS upgrades from the computer pass to %s",
					m_pSidecar->OsUpdateName (), m_pSidecar->OsPass () ? "the Rytm on USB too" : "the emulator only");
		if (m_pSidecar != 0 && m_pSidecar->UsbReboot ())
			m_Logger.Write (From, LogWarning, "gadget: usbreboot=1: the computer's reboot SysEx on the MIDI port reboots the Pi");
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
	if (m_pSidecar == 0) ApplyUsbMode ();		// no Sidecar gadget: the setting read (/status, the pages), none runs
#ifdef SIDECAR_PI5_TESTMODE
	if (m_nTestSecs != 0)
	{
		m_Watchdog.Start (15);
		if (m_pSidecar != 0 && m_Options.GetAppOptionDecimal ("uac2test", 0) != 0) m_pSidecar->Command ("uac2test=on");
		if (m_pSidecar != 0 && m_Options.GetAppOptionDecimal ("midifake", 0) != 0) m_pSidecar->Command ("midifake=on");
		blackbox_pi::TestLog (m_bTestCard);		// the boot's log, should the rest not get far
	}
#endif
	// the network: one stack on one device (Circle), Ethernet (static: the direct cable has no DHCP server) or, with
	// wifi=1 and a stored network, Wi-Fi (DHCP unless wifi.txt has ip=); mDNS: sidecar.local on either (web.h).
	// Or the USB-C network (usbnet_pi.h, docs/usb-network.md): net=auto (the default) takes it when a computer on
	// USB-C starts its network driver in time, net=usb always, net=wifi|lan never; a Sidecar with no computer on USB-C
	// comes up as before. On USB-C the Wi-Fi chip stays up for scans (Wi-Fi can be set up from the page) and joins nothing.
	m_pWifi = new CWifi;				// reads /.rytm/wifi.txt (the card is mounted by bridge::begin)
	CWifi::TMode WifiMode = m_pWifi->BootMode (&m_Options);
	const usbnet::Options UsbOpt = usbnet::options (m_Options.GetAppOptionString ("net", 0),
		m_Options.GetAppOptionString ("usbnet", 0), m_Options.GetAppOptionString ("usbip", 0),
		m_Options.GetAppOptionString ("usbwait", 0), m_Options.GetAppOptionString ("usbquiet", 0),
		m_Options.GetAppOptionString ("usbtrust", 0));
	if (UsbOpt.bad_net) m_Logger.Write (From, LogWarning, "network: net= is auto, usb, wifi or lan: auto used");
	if (UsbOpt.bad_ip) m_Logger.Write (From, LogWarning, "network: usbip= is not a usable address: 172.31.77.1 used");
	usbnet::Inputs UsbIn;
	UsbIn.net = UsbOpt.net;
	UsbIn.function = m_pSidecar != 0 && m_pSidecar->NetInDevice ();
	UsbIn.wifi_trial = WifiMode == CWifi::ModeNet && m_pWifi->TrialPending ();
	const char *pLinkOpt = m_Options.GetAppOptionString ("link", "usb");
	UsbIn.link_tcp = strncmp (pLinkOpt, "tcp", 3) == 0 && (pLinkOpt[3] == 0 || pLinkOpt[3] == ':');
	const char *pUsbWhy = "";
	const usbnet::Plan UsbPlan = usbnet::plan (UsbIn, &pUsbWhy);
	if (WifiMode != CWifi::ModeOff && !m_pWifi->StartChip ()) WifiMode = CWifi::ModeOff;	// (net=auto's wait runs meanwhile)
	boolean bUsbNet = UsbPlan == usbnet::kForced;
	if (UsbPlan == usbnet::kProbe) bUsbNet = UsbNetProbe (UsbOpt, nGadgetT0, &pUsbWhy);
	if (bUsbNet || UsbOpt.net == usbnet::kLan)
	{
		m_pWifi->OtherNet (bUsbNet ? "usb" : "ethernet");
		WifiMode = m_pWifi->Mode ();
	}
	if (!bUsbNet && m_pSidecar != 0 && m_pSidecar->NetInDevice ()) m_pSidecar->NetDetach ();	// told "not connected"
	usbnet_pi::Gadget (m_pSidecar);			// its late link: the page offers a reboot onto USB-C
	u8 UsbClient[4];
	usbnet::client_ip (UsbOpt.ip, UsbClient);
	if (bUsbNet)
	{
		new CUsbNetDevice (m_pSidecar);		// before the stack: it picks the device by its type (Unknown)
		memcpy (m_IP, UsbOpt.ip, sizeof m_IP);
		memcpy (m_NetMask, usbnet::kMask, sizeof m_NetMask);
		new (m_NetStore) CNetSubSystem (m_IP, m_NetMask, 0, 0, "sidecar", NetDeviceTypeUnknown);
	}
	else if (WifiMode == CWifi::ModeNet)
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
	else if (bUsbNet)
	{
		m_Logger.Write (From, LogNotice, "network: USB-C %u.%u.%u.%u/24, the computer %u.%u.%u.%u by DHCP (%s; Wi-Fi and Ethernet "
				"unused this boot, net=wifi or net=lan for them)%s", m_IP[0], m_IP[1], m_IP[2], m_IP[3], UsbClient[0],
				UsbClient[1], UsbClient[2], UsbClient[3], pUsbWhy,
				UsbOpt.trust ? "; the cable is trusted (no pairing code on it, usbtrust=0 asks for it)" : "");
		new CUsbDhcpd (&m_Net, m_IP, UsbClient);
	}
	else if (WifiMode == CWifi::ModeNet)
	{
		m_Logger.Write (From, LogNotice, "network: Wi-Fi (%s; Ethernet unused, wifi=0 for Ethernet)", m_IP[0] ? "static" : "DHCP");
	}
	else m_Logger.Write (From, LogNotice, "network: %u.%u.%u.%u/%u.%u.%u.%u (Ethernet; link comes up by itself)%s",
			     m_IP[0], m_IP[1], m_IP[2], m_IP[3], m_NetMask[0], m_NetMask[1], m_NetMask[2], m_NetMask[3],
			     WifiMode == CWifi::ModeScan ? "; Wi-Fi chip up for scans" : "");
	m_pWifi->StartSupplicant (m_bNetOK);		// ModeNet: joins; any mode: a join's trial may judge from now on
	usbnet_pi::Use (bUsbNet ? usbnet::kIfUsb : WifiMode == CWifi::ModeNet ? usbnet::kIfWifi : usbnet::kIfEthernet, UsbOpt,
			pUsbWhy);
	UseLink (m_Options.GetAppOptionString ("link", "usb"));
	// the pairing code (paircode.h, docs/pi-engine.md "Pairing code"): before anything on the network is served, so
	// no request can arrive ungated. cmdline paircode=0 turns the gate off (loudly), paircode=<8 digits> fixes the code.
	paircode_pi::Init (m_Options.GetAppOptionString ("paircode", 0), m_pDisplay != 0,
			   m_Options.GetAppOptionDecimal ("display", 1) != 0);
	// the web page (web.h): http://sidecar.local once the Ethernet link is up; web=0 leaves it off
	if (m_bNetOK && m_Options.GetAppOptionDecimal ("web", 1) != 0)
	{
		new CWebStart (&m_Net, &m_LogTee, &m_Console, WebStatus, WebHealth, this);
		m_bWeb = TRUE;
		m_Logger.Write (From, LogNotice, "web: on (sidecar.local once the Ethernet link is up; web=0 turns it off)");
	}
	else m_Logger.Write (From, LogNotice, "web: off");
	// the SD card's boot files on TCP 7310 (sdfiles.h, pi/circle/sdboot.py: install to SD); sdport=0 turns it off
	if (m_bNetOK && m_Options.GetAppOptionDecimal ("sdport", 1) != 0) new CSdFiles (&m_Net);
	new CSdFiles (0, &m_Serial);		// and on the serial port after the console's ":sdf"
	// the Rytm's USB MIDI over TCP 7330 for hardware testing (midibridge_pi.h): listening, but OFF until turned on.
	// Not on the USB-C network: the computer has the Rytm's MIDI on that cable already (USB MIDI)
	if (m_bNetOK && !bUsbNet) midibridge_pi::Start (&m_Net);
	return TRUE;
}

// GET /health's members (core 0: a web task), the one answer that needs no pairing code besides the page and /log:
// is this unit alive and well? The build, how long it has been up, the temperature, whether the Rytm and the stream
// are there, whether a screen is attached, the supply's flags and the underruns. Deliberately nothing else - no host
// name, no address, no settings, no machine names, no counts of paired devices - and it is a function of its own, not
// a filtered /status, so a field added to /status can never appear here by accident.
void CKernel::WebHealth (CString *pOut, void *pParam)
{
	CKernel *k = (CKernel *) pParam;
	CString s;		// temp: the stats line's reading (about 1 s old), as /status: this endpoint is open, so no firmware call per request
	s.Format ("\"version\":\"" SIDECAR_VERSION "\",\"commit\":\"" SIDECAR_COMMIT "\",\"dirty\":%s,\"uptime\":%u,"
		  "\"temp\":%u,\"streaming\":%s,\"rytmMidi\":%s,\"display\":%s,\"underruns\":%u,\"pair\":\"%s\"",
		  SIDECAR_DIRTY ? "true" : "false", CTimer::Get ()->GetUptime (), (unsigned) s_nTempC,
		  CObUsb::Streaming () ? "true" : "false", k->m_pMIDI != 0 ? "true" : "false",
		  k->m_pDisplay != 0 ? "true" : "false", g_Output.Underflows (),
		  paircode_pi::Gate ().mode () == paircode::kOff ? "off"
		  : !paircode_pi::Gate ().rng_ok () ? "rng-failed"	// refusing everything: paircode.h
		  : paircode_pi::Gate ().mode () == paircode::kFixed ? "fixed" : "on");
	pOut->Append (s);
	char Power[400];		// the supply's flags and its warning (power_state.h): a unit nobody can see
	powerstate::json (k->m_Power, Power, sizeof Power);
	pOut->Append (",");
	pOut->Append (Power);
}

static BoxList s_Boxes;						// /status "boxes": written and read by core 0's tasks

static bool BoxCapsOf (u16 nId, BoxCaps *pCaps)
{
	const core::device::Profile *p = core::device::by_id (nId);
	if (p == 0) return false;
	*pCaps = {p->tracks, p->voices, p->params, p->features, p->state_dir};
	return true;
}

// /status's members (core 0: a web task). Plain reads of state other cores write: a stale value at worst.
void CKernel::WebStatus (CString *pOut, void *pParam)
{
	CKernel *k = (CKernel *) pParam;
	CString s;
	char Boxes[1536];
	if (s_Boxes.json (Boxes, sizeof Boxes, core::device::active ().id, core::device::active ().name, BoxCapsOf) != 0)
	{
		pOut->Append (Boxes + 1);		// (past its comma: the first member here)
		pOut->Append (",");
	}
	unsigned nTempWeb = s_nTempC;			// the stats line's last read (about 1 s old): no firmware call per request
	const Output::Diag D = g_Output.Diagnostics ();
	const CEngineCores::TStall ST = CEngineCores::Stall ();
	u8 IP[4];
	memcpy (IP, k->m_IP, sizeof IP);
	if (k->m_bNetOK) k->m_Net.GetConfig ()->GetIPAddress ()->CopyTo (IP);	// DHCP on Wi-Fi: the lease's
	s.Format ("\"ip\":\"%u.%u.%u.%u\",\"link\":\"%s\",\"gadget\":\"%s\",\"sidecar\":%s,\"mac\":%s,\"usbHost\":%s,"
		  "\"streaming\":%s,\"rytmMidi\":%s,\"temp\":%u,\"tempWarn\":%s,\"clockLow\":%d,\"out\":\"%s\",\"notes\":%s,\"drone\":%d,\"fifoTarget\":%u,"
		  "\"underruns\":%u,\"restartUnderruns\":%u,\"streams\":%u,"
		  "\"outDiag\":{\"preResetPulled\":%u,\"resetLevel\":%u,\"firstPullLevel\":%d,\"armAt\":%d,\"armLevel\":%u,"
		  "\"restartFirst\":%d,\"restartLast\":%d,\"underFirst\":%d,\"underLast\":%d,\"sinceReset\":%u},"
		  "\"stall\":{\"gapUs\":%u,\"atFrames\":%u,\"over2ms\":%u,\"midi\":%u,\"keys\":%u,\"cmds\":%u,\"tick\":%u,\"render\":%u,\"idle\":%u,"
		  "\"step\":\"%s\",\"stepUs\":%u,\"namedUs\":%u,\"cardHereUs\":%u}",
		  IP[0], IP[1], IP[2], IP[3], k->m_pTransport->Name (), k->m_pGadgetMode,
		  k->m_pSidecar ? "true" : "false", k->m_pSidecar ? (k->m_pSidecar->MacOn () ? "true" : "false") : "null",
		  k->m_pSidecar ? (k->m_pSidecar->HostOn () ? "true" : "false") : "null",	// a computer has configured USB-C's Sidecar
		  CObUsb::Streaming () ? "true" : "false", k->m_pMIDI != 0 ? "true" : "false",
		  nTempWeb, k->m_Thermal.warn ? "true" : "false",
		  thermal::clock_low_mhz ((int) k->m_nClockMHz, (int) k->AskedMHz (), (int) nTempWeb),	// MHz, 0: not lowered
		  g_Output.GetMode () == Output::kMain ? "main" : "tracks",
		  CEngineCores::NotesOn () ? "true" : "false", CEngineCores::DroneVoice (), CEngineCores::FifoTarget (),
		  g_Output.Underflows (), g_Output.RestartUnderflows (), g_Output.Streams (),
		  D.pre_reset_pulled, D.reset_level, (int) D.first_pull_level, (int) D.arm_at, D.arm_level, (int) D.restart_first,
		  (int) D.restart_last, (int) D.under_first, (int) D.under_last, D.since_reset,
		  ST.gap_us, ST.at_frames, ST.gaps_over_2ms, ST.sec_us[0], ST.sec_us[1], ST.sec_us[2], ST.sec_us[3], ST.sec_us[4],
		  ST.sec_us[5], CEngineCores::StallStep (), ST.step_us, ST.step_sum, ST.here_us);	// -1: did not happen
	pOut->Append (s);
	usbnet_pi::Status (pOut, k->m_pSidecar != 0 && k->m_pSidecar->NetInDevice (), k->m_pSidecar != 0 && k->m_pSidecar->NetUp (),
			   k->m_pSidecar != 0 && k->m_pSidecar->NetLate ());	// late: reboot to use the cable
	{	// the USB-C mode (usb_mode.h): set (chosen), from (card, cmdline, default), runs (set, or class where the unit cannot
		// show it), modes (what this unit can show; [] no Sidecar gadget), shown (what the computer sees now: sidecar, box,
		// switching, none), boxUp (the box has been read and is up: shown, or about to be) and board (4, 5)
		static const char *const Shown[] = {"none", "sidecar", "box", "switching"};
		const usb_mode::Mode Runs = usb_mode::effective (k->m_UsbMode, k->m_nUsbModes);
		s.Format (",\"usbMode\":{\"set\":\"%s\",\"from\":\"%s\",\"runs\":\"%s\",\"modes\":[%s%s%s],\"shown\":\"%s\",\"boxUp\":%s,\"board\":%d}",
			  usb_mode::name (k->m_UsbMode), usb_mode::from_name (k->m_UsbFrom), usb_mode::name (Runs),
			  k->m_nUsbModes & 1u << usb_mode::kSidecar ? "\"class\"" : "",
			  (k->m_nUsbModes & 1u << usb_mode::kSidecar) && (k->m_nUsbModes & 1u << usb_mode::kMirror) ? "," : "",
			  k->m_nUsbModes & 1u << usb_mode::kMirror ? "\"overbridge\"" : "", Shown[k->UsbShown () + 1],
			  k->m_pSidecar != 0 && k->m_pSidecar->MirrorBoxThere () ? "true" : "false", RASPPI >= 5 ? 5 : 4);
		pOut->Append (s);
	}
	s.Format (",\"usbmidi\":{\"os\":\"%s\",\"osUpgrade\":%s,\"aborted\":%u}",	// usbmidi=os: (sidecar_gadget.h); osUpgrade: they
		  k->m_pSidecar ? k->m_pSidecar->OsUpdateName () : "off",	// pass now; aborted: upgrades cut after a loss
		  k->m_pSidecar && k->m_pSidecar->OsPass () ? "true" : "false", k->m_pSidecar ? k->m_pSidecar->OsAborts () : 0);
	pOut->Append (s);
	{	// the control loop's recent gaps over 2 ms: [gap us, frames pulled at the end, wfe us, cntkctl, DAIF, the pass's
		// longest section (0 midi .. 5 idle, as "stall"), its us, its slowest step and us, the named steps' us, the card's on core 3]
		CEngineCores::TGap Gp[CEngineCores::kGaps];
		unsigned nG = CEngineCores::Gaps (Gp);
		pOut->Append (CEngineCores::WaitPolled () ? ",\"ctlwait\":\"poll\",\"gaps\":[" : ",\"ctlwait\":\"wfe\",\"gaps\":[");
		for (unsigned i = 0; i < nG; i++)
		{
			s.Format ("%s[%u,%u,%u,%u,%u,%u,%u,\"%s\",%u,%u,%u]", i ? "," : "", Gp[i].gap_us, Gp[i].at_frames, Gp[i].wfe_us, Gp[i].kctl, Gp[i].daif,
				  Gp[i].sec, Gp[i].sec_us, Gp[i].step, Gp[i].step_us, Gp[i].step_sum, Gp[i].here_us);
			pOut->Append (s);
		}
		pOut->Append ("]");
		CEngineCores::TSlowSx Sx[CEngineCores::kSlowSx];	// the SysEx messages that took core 3 over 1 ms
		unsigned nSx = CEngineCores::SlowSysex (Sx);
		pOut->Append (",\"slowSx\":[");
		for (unsigned i = 0; i < nSx; i++)
		{
			s.Format ("%s[%u,%u,%u,%u]", i ? "," : "", Sx[i].cmd, Sx[i].arg, Sx[i].len, Sx[i].us);
			pOut->Append (s);
		}
		pOut->Append ("]");
	}
	{	// the firmware calls core 0 made recently: [id, frames pulled since the Reset at the start, us]; see mbtrace.h
		unsigned n = mbtrace::count (), k = n < mbtrace::kN ? n : mbtrace::kN;
		pOut->Append (",\"mb\":[");
		for (unsigned i = 0; i < k; i++)
		{
			const mbtrace::Rec &r = mbtrace::ring ()[(n - k + i) % mbtrace::kN];
			s.Format ("%s[%u,%u,%u]", i ? "," : "", r.id, r.at, r.dur_us);
			pOut->Append (s);
		}
		pOut->Append ("]");
		unsigned nB = mbtrace::bootCount ();	// the first 10 s of the current stream, frozen: readable whenever the web is up
		pOut->Append (",\"mbBoot\":[");
		for (unsigned i = 0; i < nB && i < mbtrace::kB; i++)
		{
			const mbtrace::Rec &r = mbtrace::boot ()[i];
			s.Format ("%s[%u,%u,%u]", i ? "," : "", r.id, r.at, r.dur_us);
			pOut->Append (s);
		}
		pOut->Append ("]");
	}
	// setting: cpumhz=, where 1500 means the board's full clock; mhz: what the firmware says it set; fullMhz, minMhz: its range;
	// measuredMhz: what it measured (MeasureClock, up to 10 s old, 0: none yet); socMaxC: Circle's throttle (socmaxtemp)
	s.Format (",\"cpu\":{\"setting\":%u,\"mhz\":%u,\"budgetUs\":%u,\"askedMhz\":%u,\"fullMhz\":%u,\"minMhz\":%u,\"measuredMhz\":%u,\"socMaxC\":%u,\"capped\":%s}",
		  k->m_nClockSetting, k->m_nClockMHz, k->m_nBudgetUs, k->AskedMHz (), k->m_CPUThrottle.GetMaxClockRate () / 1000000,
		  k->m_CPUThrottle.GetMinClockRate () / 1000000, k->m_nMeasuredMHz, k->m_Options.GetSoCMaxTemp (),
		  k->m_bClockCapped ? "true" : "false");
	pOut->Append (s);
	s.Format (",\"idle\":{\"wfe\":%s,\"asleepPct\":[%u,%u,%u,%u],\"wakes\":[%u,%u,%u,%u]}", CEngineCores::IdleWfe () ? "true" : "false",
		  k->m_nAsleepPct[0], k->m_nAsleepPct[1], k->m_nAsleepPct[2], k->m_nAsleepPct[3],
		  k->m_nWakes[0], k->m_nWakes[1], k->m_nWakes[2], k->m_nWakes[3]);
	pOut->Append (s);
	pOut->Append (",\"machines\":[");			// each voice's machine (the Sound tab shows the current one)
	for (unsigned v = 0; v < 8; v++)
	{
		s.Format ("%s\"%s\"", v ? "," : "", bridge::machine_name (v));
		pOut->Append (s);
	}
	pOut->Append ("]");
	pOut->Append (",\"muted\":[");			// the guard's muted voices and why (guard.h; :unmute or a machine change frees one)
	{
		u64 nCntFrq;					// the guard's ticks: the generic timer's
		asm volatile ("mrs %0, cntfrq_el0" : "=r" (nCntFrq));
		const unsigned nPerUs = (unsigned) (nCntFrq / 1000000);
		boolean bFirst = TRUE;
		for (unsigned v = 0; v < CGuard::Voices; v++)
		{
			if (!g_Guard.Muted (v)) continue;
			s.Format ("%s{\"v\":%u,\"m\":\"%s\",\"why\":\"%s\",\"maxUs\":%u}", bFirst ? "" : ",", v,
				  bridge::machine_name (v), g_Guard.Why (v) ? g_Guard.Why (v) : "", nPerUs ? g_Guard.MaxTicks (v) / nPerUs : 0);
			pOut->Append (s);
			bFirst = FALSE;
		}
	}
	pOut->Append ("]");
	{
		char Src[320];		// 1025: each track's SRC machine as the Rytm reports it (bridge.h track_src_json)
		if (bridge::track_src_json (Src, sizeof Src) > 0) { pOut->Append (","); pOut->Append (Src); }
		char Store[240];	// 2026-10-26: the RAM store and the writes still on their way to the card (bridge.h store_json)
		unsigned long nHeapFree = (unsigned long) CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW);	// new's heap
		if (bridge::store_json (Store, sizeof Store, nHeapFree) > 0) { pOut->Append (","); pOut->Append (Store); }
	}
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
		char Bundle[256];		// the project bundle job (core/bundle): its phase, pauses and pieces
		if (bundle_pi::status_json (Bundle, sizeof Bundle) > 0) pOut->Append (Bundle);
	}
	{
		char Lib[400];			// the sample library (core/lib): files, hashed, pending, duplicates, missing
		if (lib_pi::status_json (Lib, sizeof Lib) > 0) pOut->Append (Lib);
	}
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
	{
		char Mo[160];			// the modulation layer: live targets, the tick's worst us and counts (core/mod/mod.h)
		if (bridge::mod_json (Mo, sizeof Mo) > 0)
		{
			pOut->Append (",");
			pOut->Append (Mo);
		}
	}
	{
		static char Sc[16384];		// the scenes: A / B, the fader, every lock (core/mod/scene.h; core 3's, plain reads)
		int n = bridge::scene_json (Sc, sizeof Sc);
		if (n > 0 && n < (int) sizeof Sc)
		{
			pOut->Append (",");
			pOut->Append (Sc);
		}
	}
	{
		char Bb[128];			// the black box's counts (blackbox_pi.h)
		blackbox_pi::Status (Bb, sizeof Bb);
		pOut->Append (",\"blackbox\":");
		pOut->Append (Bb);
	}
	{	// the USB endpoints a transfer error halted and the engine reset since boot (ep_halt.h, circle-midi-reset.patch): the Rytm's
		// Overbridge IN / OUT, an A4's, the Rytm's USB MIDI OUT; refused: resets the controller refused (tried again 1 s later);
		// held: a box's endpoints failed together, a likely unplug, its resets held for the hub's disconnect (EpBox)
		CString Halt;
		Halt.Format (",\"usbhalt\":{\"ob_in\":%u,\"ob_out\":%u,\"a4_in\":%u,\"a4_out\":%u,\"midi_out\":%u,\"refused\":%u,\"held\":%u}",
			     CObUsb::HaltResets (TRUE), CObUsb::HaltResets (FALSE), CObFollow::HaltResets (TRUE),
			     CObFollow::HaltResets (FALSE), k->m_Usb.MIDIOutResets (),
			     CObUsb::HaltRefused () + CObFollow::HaltRefused () + k->m_Usb.MIDIOutRefused (),
			     CObUsb::HaltHeld () + CObFollow::HaltHeld ());
		pOut->Append (Halt);
	}
	{
		static char Poly[3072];		// E0: the seq tracks' poly configs and the instances (core/seq, core/inst.h; plain reads)
		if (bridge::poly_json (Poly, sizeof Poly) > 0)
		{
			pOut->Append (",");
			pOut->Append (Poly);
		}
	}
	{
		static char Seqx[24576];		// seqtop: the pattern's key / scale / seed and the seq tracks' seqtop settings (core/seq; plain reads)
		if (bridge::seqx_json (Seqx, sizeof Seqx) > 0)
		{
			pOut->Append (",");
			pOut->Append (Seqx);
		}
	}
	{
		static char Ins[8192];		// fx-ins (+ INSERT AMP's slots): the INSERT chains, sends, bus 3's route, the OUT routes (core/fx/insert.h, out.h), the streaming budget
		bridge::ins_json (Ins, sizeof Ins);
		pOut->Append (",");
		pOut->Append (Ins);
	}
	{
		char Sd[480];			// the SD card test (core/msamp/sdtest.h; core 3's, plain reads)
		pOut->Append (",\"sdtest\":");
		bridge::sdtest_json (Sd, sizeof Sd);
		pOut->Append (Sd);
	}
	{
		static char Sa[640];		// SAMPLING and the AUTO-SAMPLER (core/samp; plain reads)
		bridge::samp_json (Sa, sizeof Sa);
		pOut->Append (Sa);
	}
	stor_pi::Status (pOut);			// the volumes and the bench (docs/STORAGE.md)
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
// net=auto (usbnet_cfg.h's BootProbe, docs/usb-network.md): after the Wi-Fi chip's start, timed from the gadget's, so
// the two overlap. No start-of-frame on USB-C within usbquiet ms: no computer (a power supply), the network of today.
// A computer that has not started a USB network driver within usbwait s (Windows 10 without the NCM driver): today's
// too. Its driver selecting the data interface: the USB-C network. The gadget is polled meanwhile (its enumeration).
boolean CKernel::UsbNetProbe (const usbnet::Options &rOpt, unsigned nGadgetT0, const char **ppWhy)
{
	usbnet::BootProbe Probe (rOpt.quiet_ms, rOpt.wait_s);
	const unsigned nFrame0 = m_pSidecar->BusFrame ();
	usbnet::Probe r;
	do
	{
		m_Timer.MsDelay (2);			// a frame is 1 ms: two reads this far apart differ while a computer drives the bus
		m_pSidecar->Poll ();
		r = Probe.step ((CTimer::GetClockTicks () - nGadgetT0) / 1000, m_pSidecar->BusFrame () != nFrame0,
				m_pSidecar->NetUp ());
	}
	while (r == usbnet::kWaiting);
	const unsigned nMs = (CTimer::GetClockTicks () - nGadgetT0) / 1000;
	switch (r)
	{
	case usbnet::kUsbUp:
		*ppWhy = "a computer on USB-C started its network driver";
		break;
	case usbnet::kNoDriver:
		*ppWhy = "a computer on USB-C, but no USB network driver started (Windows 10: docs/usb-network.md)";
		break;
	default:
		*ppWhy = "no computer on USB-C";
		break;
	}
	m_Logger.Write (From, LogNotice, "network: net=auto: %s (%u ms after the gadget's start)", *ppWhy, nMs);
	return r == usbnet::kUsbUp;
}

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
		if (m_pSidecar != 0) m_pSidecar->SetLinkTcp (TRUE);	// usbmidi=os: in the same step (the queue is cut)
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
	// usbmidi=os: in the same step, before the USB merge's next Poll: what waited for the emulator (an OS image under
	// emu) is cut, never sent to the real Rytm
	if (m_pSidecar != 0) m_pSidecar->SetLinkTcp (m_pTransport == &m_Tcp);
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
			m_Logger.Write (From, LogNotice, "bench c%u %s: %u.%02u %% of a core per voice, worst block %u.%u %% -> %u voices a core, peak %d, hash %08X",
					c, CEngineCores::BenchMachine[m], nBp / 100, nBp % 100, nBlk / 10, nBlk % 10,
					nBp ? 8000 / nBp : 999, r.nPeak, r.nHash);	// at 80 % of a core
			m_Timer.MsDelay (2);	// ~150 B a line takes 1.6 ms at 921600 baud: Circle's 2 KB TX buffer drops when full
		}
		m_Logger.Write (From, LogNotice, "bench %s: %u.%02u %% of a core per voice at %u MHz -> %u voices a core (worst of cores 0-3, worst block %u.%u %%, budget %u %%)",
				CEngineCores::BenchMachine[m], nWorstBp / 100, nWorstBp % 100, nMHz,
				nWorstBp ? 8000 / nWorstBp : 999, nWorstBlk / 10, nWorstBlk % 10,
				(unsigned) (g_Guard.Budget () * 100 / nBlock));
		m_nBenchBp[m] = nWorstBp;			// for the clock option's rescale (LogClockRescale)
		m_Timer.MsDelay (2);
	}
	m_nBenchMHz = nMHz;
	// fx-ins (core/fx/insert.h): 8 voices' INSERT chains, two slots each, on core 0 (docs/INSERT-FX.md "CPU")
	// FILTER+DELAY COMP+EQ DRIVE+TREMOLO CRUSH+RINGMOD SAT+SAT, then OFF+OFF = ins1112's POST EQ on its own (bridge.cpp turns
	// the post EQ on only for that row; the four pairs are measured with it flat, i.e. skipped, so their numbers
	// stay comparable with the ones in docs/INSERT-FX.md)
	static const int Pairs[6][2] = {{1, 6}, {5, 4}, {3, 7}, {2, 8}, {10, 10}, {0, 0}};
	for (unsigned p = 0; p < 6; p++)
	{
		u32 nWorst = 0;
		u32 nHash = 0;
		u64 nTicks = bridge::ins_bench (Pairs[p][0], Pairs[p][1], &nWorst, &nHash);
		unsigned nBp = (unsigned) (nTicks * 10000 / nCntFrq), nBlk = (unsigned) (nWorst * 1000 / nBlock);
		m_Logger.Write (From, LogNotice, "bench ins %s+%s: 8 voices x 2 slots %u.%02u %% of a core, worst block %u.%u %%, hash %08X",
				bridge::ins_bench_name (Pairs[p][0]), bridge::ins_bench_name (Pairs[p][1]), nBp / 100, nBp % 100,
				nBlk / 10, nBlk % 10, nHash);
		m_Timer.MsDelay (2);
	}
	{	// SAT on every slot of the buses and the master: what it can add to core 2
		u32 nWorst = 0;
		u64 nTicks = bridge::ins_bus_bench (&nWorst);
		unsigned nBp = (unsigned) (nTicks * 10000 / nCntFrq), nBlk = (unsigned) (nWorst * 1000 / nBlock);
		m_Logger.Write (From, LogNotice, "bench ins SAT buses+master: 10 stereo slots %u.%02u %% of a core, worst block %u.%u %%",
				nBp / 100, nBp % 100, nBlk / 10, nBlk % 10);
		m_Timer.MsDelay (2);
	}
	// the NEON kernels against their C on this CPU (neon_bench.cpp)
	{
		u64 nC, nN;
		for (unsigned r = 0; const char *pName = bridge::neon_bench (r, &nC, &nN); r++)
		{
			m_Logger.Write (From, LogNotice, "bench neon %s: C %u us, NEON %u us (%u %%)", pName, (unsigned) (nC * 1000000 / nCntFrq),
					(unsigned) (nN * 1000000 / nCntFrq), nC ? (unsigned) (nN * 100 / nC) : 0);
			m_Timer.MsDelay (2);
		}
	}
	// INSERT AMP (core/fx/amp.h): one NAM A2-Lite instance, a synthetic capture (docs/INSERT-FX.md "AMP")
	{
		u32 nWorst = 0;
		u64 nTicks = bridge::amp_bench (&nWorst);
		unsigned nBp = (unsigned) (nTicks * 10000 / nCntFrq), nBlk = (unsigned) (nWorst * 1000 / nBlock);
		m_Logger.Write (From, LogNotice, "bench amp A2-LITE: 1 instance %u.%02u %% of a core, worst block %u.%u %% -> cap 6 = %u.%02u %%",
				nBp / 100, nBp % 100, nBlk / 10, nBlk % 10, nBp * 6 / 100, nBp * 6 % 100);	// the cap: core/fx/amp.h kMaxRunning
		m_Timer.MsDelay (2);
	}
	// the master chain (core/fx/master.h): add 2 x the AMP line for an L/R pair (measured by BenchChains, logged here)
	if (s_ChainBench[0].nTicks != 0)
	{
		u32 nWorst = s_ChainBench[0].nWorst;
		u64 nTicks = s_ChainBench[0].nTicks;
		unsigned nBp = (unsigned) (nTicks * 10000 / nCntFrq), nBlk = (unsigned) (nWorst * 1000 / nBlock);
		m_Logger.Write (From, LogNotice, "bench mfx COMP EQ SPACE CLOUD POSTEQ: %u.%02u %% of a core, worst block %u.%u %%",
				nBp / 100, nBp % 100, nBlk / 10, nBlk % 10);
		m_Timer.MsDelay (2);
	}
	// 0.5.0: 4 send TYPE inserts at once (B1 A, B2 A, B3 A, M1 A) through core 2's paths (BenchChains)
	for (int nSet = 0; const char *pName = bridge::sfxins_bench_name (nSet); nSet++)
	{
		if (nSet + 1 >= ChainBenches || s_ChainBench[nSet + 1].nTicks == 0) break;
		u32 nWorst = s_ChainBench[nSet + 1].nWorst;
		u64 nTicks = s_ChainBench[nSet + 1].nTicks;
		unsigned nBp = (unsigned) (nTicks * 10000 / nCntFrq), nBlk = (unsigned) (nWorst * 1000 / nBlock);
		m_Logger.Write (From, LogNotice, "bench sfxins %s: %u.%02u %% of a core, worst block %u.%u %%",
				pName, nBp / 100, nBp % 100, nBlk / 10, nBlk % 10);
		m_Timer.MsDelay (2);
	}
}

// The master chain's and the send TYPE inserts' benches (core/fx/master.h bench, sfx_bench), run on core 0 before cores
// 1-3 start: then nothing else touches the insert and master controls. Core 3's idle pass steps the project's store fill
// and applies its inserts once it runs; a bench at the same time could book a room the kit wants, or the kit's load land
// mid-bench. Logged later by LogBench, in the rows' usual place.
void CKernel::BenchChains (void)
{
	s_ChainBench[0].nTicks = bridge::mfx_bench (&s_ChainBench[0].nWorst);
	for (int nSet = 0; nSet + 1 < ChainBenches && bridge::sfxins_bench_name (nSet); nSet++)
		s_ChainBench[nSet + 1].nTicks = bridge::sfxins_bench (nSet, &s_ChainBench[nSet + 1].nWorst);
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

static int s_nUiLook;			// UX review L1: the OLED look (0 studio, 1 stage), /.rytm/display.txt's uilook=

boolean CKernel::Core0Command (const char *pCmd, void *pParam)
{
	CKernel *pThis = (CKernel *) pParam;
	if (pThis->UsbCommand (pCmd)) return TRUE;			// usbmode= (the USB-C mode)
	if (strncmp (pCmd, "poweroff", 8) == 0 && (pCmd[8] == 0 || pCmd[8] == '='))	// a clean power-off (docs/pi5.md)
	{
#if RASPPI >= 5
		if (strcmp (pCmd + 8, "=1") == 0) pThis->m_bPowerOff = TRUE;
		else pThis->m_Logger.Write (From, LogWarning, "poweroff=1: a clean power-off, as the power button");
#else
		pThis->m_Logger.Write (From, LogWarning, "poweroff: refused, the Pi 5 only (this board has no power button)");
#endif
		return TRUE;
	}
	boolean bSaver = strncmp (pCmd, "screensaver", 11) == 0 && (pCmd[11] == 0 || pCmd[11] == '=');
	boolean bBright = strncmp (pCmd, "brightness", 10) == 0 && (pCmd[10] == 0 || pCmd[10] == '=');
	if (strncmp (pCmd, "uistyle", 7) == 0 && (pCmd[7] == 0 || pCmd[7] == '='))	// the display's theme
	{
		if (pCmd[7] == '=')
		{
			int n = strcmp (pCmd + 8, "oled") == 0 || strcmp (pCmd + 8, "0") == 0 ? 0
				: strcmp (pCmd + 8, "sidecar") == 0 || strcmp (pCmd + 8, "classic") == 0 || strcmp (pCmd + 8, "1") == 0 ? 1 : -1;
			if (n < 0) pThis->m_Logger.Write (From, LogWarning, "uistyle=oled|sidecar");
			else
			{
				pThis->m_nUiStyle = (unsigned) n;
				pThis->m_bSaveDisplay = TRUE;
				if (pThis->m_pDisplay != 0)
				{
					pThis->m_pDisplay->Activity ();
					pThis->PushUi ();
				}
			}
		}
		pThis->m_Logger.Write (From, LogNotice, "display: UI style %s", pThis->m_nUiStyle ? "sidecar" : "oled");
		return TRUE;
	}
	if (strncmp (pCmd, "uilook", 6) == 0 && (pCmd[6] == 0 || pCmd[6] == '='))	// UX review L1: the OLED look
	{
		if (pCmd[6] == '=')
		{
			int n = strcmp (pCmd + 7, "studio") == 0 ? 0 : strcmp (pCmd + 7, "stage") == 0 ? 1 : -1;
			if (n < 0) pThis->m_Logger.Write (From, LogWarning, "uilook=studio|stage");
			else
			{
				s_nUiLook = n;
				sidecar_dot_look (n);
				pThis->m_bSaveDisplay = TRUE;
				if (pThis->m_pDisplay != 0) { pThis->m_pDisplay->Activity (); pThis->PushUi (); }
			}
		}
		pThis->m_Logger.Write (From, LogNotice, "display: look %s", s_nUiLook ? "stage" : "studio");
		return TRUE;
	}
	if (strncmp (pCmd, "cpumhz", 6) == 0 && (pCmd[6] == 0 || pCmd[6] == '='))	// Settings > System: the CPU clock option
	{
		if (pCmd[6] == '=')
		{
			unsigned n = 0;
			const char *v = pCmd + 7;
			boolean bDigits = *v >= '0' && *v <= '9';
			for (; *v >= '0' && *v <= '9' && n < 100000; v++) n = n * 10 + (*v - '0');
			if (!bDigits || *v || cpuclock::normalize (n) != n)
			{
				pThis->m_Logger.Write (From, LogWarning, "cpumhz=1500|1200|1000 (1500: the board's full clock)");
				return TRUE;
			}
			pThis->SetClock (n);
			pThis->m_bSaveDisplay = TRUE;	// into display.txt as cpumhz= (audit 2026-10-26: it was never saved)
			if (pThis->m_pDisplay != 0)
			{
				pThis->m_pDisplay->Activity ();
				pThis->PushUi ();
			}
		}
		pThis->m_Logger.Write (From, LogNotice, "cpu: setting %u MHz, ARM clock %u MHz, voice budget %u us", pThis->m_nClockSetting,
				       pThis->m_nClockMHz, pThis->m_nBudgetUs);
		return TRUE;
	}
	if (strncmp (pCmd, "voicepick=", 10) == 0)				// a HOME tile: VOICE shows that track (UX review T3)
	{
		voice_pi::Pick (atoi (pCmd + 10));
		return TRUE;
	}
	if (strncmp (pCmd, "touchcal", 8) == 0 && (pCmd[8] == 0 || pCmd[8] == '='))	// Settings > Display > Calibrate
	{
		if (pCmd[8] == '=')
		{
			touch_cal_t Cal;
			if (touch_cal_parse (pCmd + 9, &Cal)) { pThis->m_TouchCal = Cal; pThis->m_bTouchCal = TRUE; }
			else if (strcmp (pCmd + 9, "off") == 0 || pCmd[9] == 0) pThis->m_bTouchCal = FALSE;
			else
			{
				pThis->m_Logger.Write (From, LogWarning, "touchcal=X0,X1,Y0,Y1,SWAP (raw at the edges) or touchcal=off");
				return TRUE;
			}
			pThis->m_bSaveDisplay = TRUE;
			pThis->m_Ui.touch_cal = pThis->m_bTouchCal;
			if (pThis->m_pDisplay != 0)
			{
				pThis->m_pDisplay->SetTouchCal (pThis->m_bTouchCal ? &pThis->m_TouchCal : 0);
				pThis->m_pDisplay->Activity ();
				pThis->PushUi ();
			}
		}
		char b[64] = "off";
		if (pThis->m_bTouchCal) touch_cal_format (&pThis->m_TouchCal, b, sizeof b);
		pThis->m_Logger.Write (From, LogNotice, "display: touchcal=%s", b);
		return TRUE;
	}
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

// res= in /.rytm/display.txt or cmdline.txt (cmdline wins): "800x480", "1024x600" or "1280x720" fixes the UI's size, "auto" (or
// none) lets the display choose (the frame buffer's size if it is one of them, else 800x480 centred; docs/pi-display.md
// "Screen sizes"). Read once, before the display's Initialize; a change takes a reboot. Kept when display.txt is written.
void CKernel::LoadDisplayRes (boolean bCard)
{
	char Text[160] = "";
	if (bCard)
	{
		FIL f;
		UINT n = 0;
		// 0.3.0: the first read of display.txt after a boot - a cut between the atomic write's renames left the
		// settings in display.txt.prev, and a cut during it left a display.txt.tmp (core/fmt/fmt.h recover)
		core::fmt::recover ("/.rytm/display.txt");
		if (f_open (&f, "SD:/.rytm/display.txt", FA_READ) == FR_OK)
		{
			f_read (&f, Text, sizeof Text - 1, &n);
			f_close (&f);
		}
		Text[n] = 0;
	}
	auto Parse = [&] (const char *p, const char *pFrom, unsigned &rW, unsigned &rH) -> void
	{
		if (p == 0) return;
		unsigned w = 0, h = 0;
		while (*p >= '0' && *p <= '9') w = w * 10 + (unsigned) (*p++ - '0');
		if (*p == 'x' || *p == 'X') for (p++; *p >= '0' && *p <= '9'; p++) h = h * 10 + (unsigned) (*p - '0');
		if ((w == 800 && h == 480) || (w == 1024 && h == 600) || (w == 1280 && h == 720)) { rW = w; rH = h; }
		else if (strncmp (p, "auto", 4) == 0) rW = rH = 0;
		else m_Logger.Write (From, LogWarning, "display: res= in %s: 800x480, 1024x600, 1280x720 or auto", pFrom);
	};
	const char *ps = strstr (Text, "\nres=");
	ps = strncmp (Text, "res=", 4) == 0 ? Text + 4 : ps ? ps + 5 : 0;
	Parse (ps, "/.rytm/display.txt", m_nResFileW, m_nResFileH);
	m_nResW = m_nResFileW, m_nResH = m_nResFileH;
	Parse (m_Options.GetAppOptionString ("res", 0), "cmdline.txt", m_nResW, m_nResH);
	if (m_nResW) m_Logger.Write (From, LogNotice, "display: res=%ux%u", m_nResW, m_nResH);
}

// /.rytm/display.txt: "screensaver=MIN", "brightness=PCT", "touchtest=0|1" and "uistyle=oled|sidecar" lines
// (SIDECAR replaced the CLASSIC theme under its id 1, so an older card's "classic" is read as "sidecar" and
// written back as "sidecar" at the next save); cmdline.txt's same keys win
static core::fmt::KvKeep s_DisplayKeep;		// display.txt's lines a newer build wrote: written back unchanged
void CKernel::LoadDisplaySettings (boolean bCard)
{
	static char Text[1536];
	Text[0] = 0;
	s_DisplayKeep.n = 0;
	if (bCard)
	{
		FIL f;
		UINT n = 0;
		core::fmt::recover ("/.rytm/display.txt");	// 0.3.0: as LoadDisplayRes (which display=0 skips), idempotent
		if (f_open (&f, "SD:/.rytm/display.txt", FA_READ) == FR_OK)
		{
			f_read (&f, Text, sizeof Text - 1, &n);
			f_close (&f);
		}
		Text[n] = 0;
		static const char *const Known[] = {"screensaver", "brightness", "touchtest", "uistyle", "uilook", "res", "touchcal", "cpumhz",
						    "usbmode", 0};
		core::fmt::kv_keep (Text, n, Known, &s_DisplayKeep);
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
	auto Style = [&] (const char *p) -> int		// "oled" / "sidecar" (older cards: "classic"; or 0 / 1), -1: neither
	{
		if (p == 0) return -1;
		auto End = [] (char c) { return c == 0 || c == '\n' || c == '\r'; };
		if ((strncmp (p, "oled", 4) == 0 && End (p[4])) || (*p == '0' && End (p[1]))) return 0;
		if ((strncmp (p, "sidecar", 7) == 0 && End (p[7])) || (strncmp (p, "classic", 7) == 0 && End (p[7])) || (*p == '1' && End (p[1])))
			return 1;
		return -1;
	};
	const char *ps = strstr (Text, "uistyle=");
	int nStyle = Style (ps ? ps + 8 : 0), nCmd = Style (m_Options.GetAppOptionString ("uistyle", 0));
	m_nUiStyle = nCmd >= 0 ? (unsigned) nCmd : nStyle >= 0 ? (unsigned) nStyle : 0;
	m_Ui.ui_style = (int) m_nUiStyle;
	{
		const char *pl = strstr (Text, "uilook=");
		s_nUiLook = pl && strncmp (pl + 7, "stage", 5) == 0 ? 1 : 0;
		sidecar_dot_look (s_nUiLook);
		m_Ui.ui_look = s_nUiLook;
	}
	m_Ui.screensaver = (int) m_nSaverMin;
	m_Ui.brightness = (int) m_nBright;
	m_Ui.touch_hidden = m_bTouchHidden;
	{
		const char *pc = strstr (Text, "touchcal=");				// the 4-point calibration
		m_bTouchCal = pc != 0 && (pc == Text || pc[-1] == '\n') && touch_cal_parse (pc + 9, &m_TouchCal);
		m_Ui.touch_cal = m_bTouchCal;
		if (m_pDisplay != 0 && m_bTouchCal) m_pDisplay->SetTouchCal (&m_TouchCal);
	}
	{
		// the USB-C mode (usb_mode.h): stored, it wins over cmdline.txt's usb= (ApplyUsbMode); a word it does not know is
		// dropped at the next write, with a warning now
		bool bFound;
		m_bUsbStored = usb_mode::from_text (Text, &m_UsbStored, &bFound);
		if (bFound && !m_bUsbStored) m_Logger.Write (From, LogWarning, "usb: usbmode= in /.rytm/display.txt: class or overbridge");
	}
	if (m_pDisplay != 0) m_pDisplay->SetScreensaver (m_nSaverMin);
	m_Logger.Write (From, LogNotice, "display: screensaver %u min, brightness %u %%, UI style %s (%s)", m_nSaverMin, m_nBright,
			m_nUiStyle ? "sidecar" : "oled", Text[0] ? "/.rytm/display.txt" : "defaults");
}

void CKernel::SaveDisplaySettings (void)
{
	m_bSaveDisplay = FALSE;
	CString s;
	s.Format ("screensaver=%u\nbrightness=%u\ntouchtest=%u\nuistyle=%s\nuilook=%s\n", m_nSaverMin, m_nBright, m_bTouchHidden ? 0u : 1u,
		  m_nUiStyle ? "sidecar" : "oled", s_nUiLook ? "stage" : "studio");
	if (m_bTouchCal)
	{
		char b[64];
		touch_cal_format (&m_TouchCal, b, sizeof b);
		CString c;
		c.Format ("touchcal=%s\n", b);
		s.Append (c);
	}
	if (m_nClockSetting != 1500)				// the CPU clock option (cpuclock.h); the default is not written
	{
		CString c;
		c.Format ("cpumhz=%u\n", m_nClockSetting);
		s.Append (c);
	}
	if (m_bUsbStored)					// the USB-C mode (usb_mode.h): only once chosen, so cmdline usb= holds until then
	{
		CString c;
		c.Format ("usbmode=%s\n", usb_mode::name (m_UsbStored));
		s.Append (c);
	}
	if (m_nResFileW)					// the file's res= kept (set by hand: docs/pi-display.md)
	{
		CString r;
		r.Format ("res=%ux%u\n", m_nResFileW, m_nResFileH);
		s.Append (r);
	}
	if (s_DisplayKeep.n) s.Append (s_DisplayKeep.text);	// (class S: a newer build's keys carried, fmt.h)
	boolean bOK = SdWriteWhole ("/.rytm/display.txt", (const char *) s, s.GetLength ());
	m_Logger.Write (From, bOK ? LogNotice : LogWarning, "/.rytm/display.txt %s", bOK ? "written" : "NOT written");
}

void CKernel::MIDIPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength)
{
	// the Rytm's MIDI to the computer (the Sidecar's filter drops the host link and MIDI-CI): Circle hands over each
	// event packet's MIDI bytes, its header byte is the one before them in the endpoint's buffer
	// (usbmidi=os: on or emu: only while USB is the host; under link=tcp the emulator's MIDI goes to the computer, tcp_link.cpp)
	CSidecarGadget *pSidecar = CSidecarGadget::Get ();
	if (pSidecar != 0 && (!pSidecar->OsUpdate () || CObUsb::Active ())) pSidecar->FromRytmMIDI (pPacket - 1);
	midibridge_pi::FromRytm (pPacket, nLength);	// and to the Rytm MIDI bridge's client, if one is connected
	if (!CObUsb::Active ()) return;	// the TCP link is the host: a real Rytm's MIDI is not the engine's
	if (nLength >= 3 && (pPacket[0] & 0xF0) == 0x90 && pPacket[2] != 0) CObUsb::CaptureMarkMIDI ();
	for (unsigned i = 0; i < nLength; i++)
		if (!g_MidiIn.push (pPacket[i])) s_nMidiInLost++;	// a full queue: counted, as the TCP link's
	asm volatile ("dsb sy; sev");
}

// An A4's USB MIDI before it is the box in use: its hello reaches the engine while no Rytm has the link, nothing else does, and nothing goes back (usb_box.h).
static CDevice *volatile s_pHelloBox;
static volatile boolean s_bHelloHeard;				// it has the mod: its hello came, taken or not (box_list.h)
static volatile boolean s_bLinkUp;				// a host link is up
static HelloGate s_HelloGate;
static void HelloPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength)
{
	for (unsigned i = 0; i < nLength; i++)
	{
		unsigned n = s_HelloGate.feed (pPacket[i]);
		if (n != 0) s_bHelloHeard = TRUE;
		if (n == 0 || s_bLinkUp || !CObUsb::Active ()) continue;
		for (unsigned k = 0; k < n; k++)
			if (!g_MidiIn.push (s_HelloGate.data ()[k])) s_nMidiInLost++;
		asm volatile ("dsb sy; sev");
	}
}
static void HelloRemoved (CDevice *pDevice, void *pContext)
{
	s_pHelloBox = 0;
	s_bHelloHeard = FALSE;
}

void CKernel::MIDIRemoved (CDevice *pDevice, void *pContext)
{
	((CKernel *) pContext)->m_pMIDI = 0;
	g_bRytmGone = true;				// the MIDI machine's USB notes: no hello will end them (the control core's)
}

// The Pi's own keyboards (core/keys.h): every other USB MIDI device in the USB-A ports, up to four, as sources 0..3 (in
// plug order), their channel messages to the control core (g_KeysIn); and the computer's notes from the Sidecar gadget
// (source 4). Gone on unplug (Circle's removed handler, from UpdatePlugAndPlay on core 0's loop: IRQs masked for the
// queue, whose other producers are the IRQs).
static const unsigned MaxKeyboards = 4;
static CDevice *volatile s_pKeyboard[MaxKeyboards];
static unsigned s_nKeysLost;			// keyboard messages lost: a full g_KeysIn
static void KeysPush (u32 w)
{
	if (!g_KeysIn.push (w)) s_nKeysLost++;
	asm volatile ("dsb sy; sev");
}
static void KeysPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength, unsigned nDevice, void *pParam)
{
	unsigned i = (unsigned) (uintptr) pParam;
	if (nLength < 2 || pPacket[0] < 0x80 || pPacket[0] >= 0xF0 || i >= MaxKeyboards) return;	// channel messages only
	KeysPush ((u32) i << 24 | (u32) pPacket[0] << 16 | (u32) pPacket[1] << 8 | (nLength > 2 ? pPacket[2] : 0));
}
static void KeysRemoved (CDevice *pDevice, void *pContext)
{
	unsigned i = (unsigned) (uintptr) pContext;
	if (i >= MaxKeyboards) return;
	s_pKeyboard[i] = 0;
	EnterCritical ();
	KeysPush ((u32) i << 24 | 0xF4u << 16);
	LeaveCritical ();
}
// The auto-sampler's notes for an external synth and the MIDI machine's (g_AsampMidi, shared.h): port 1 the computer's (the
// Sidecar gadget's MIDI IN), 2 the first USB-A device's (a synth or an interface there), 3 every USB-A device's (the MIDI machine,
// core/midim/midim.h); none there: dropped. USB-A never waits: each device has a queue (midiout_q.h) and one transfer in flight
// (circle-midi-async.patch), so a device that does not drain costs only its own queue; what it cannot take is shed, note-offs last.
// A failed transfer's note-offs go again, 100 ms later.
static MidiOutQ s_OutQ[MaxKeyboards];
static void AsampMidiSend (CSidecarGadget *pSidecar)
{
	u32 w;
	while (g_AsampMidi.pop (&w))
	{
		unsigned nPort = (w >> 4) & 0x0F;
		w &= ~0xF0u;				// cable 0
		if (nPort == 1)
		{
			if (pSidecar != 0) pSidecar->MIDIToComputer (w);
		}
		else if (nPort == 2 || nPort == 3)
			for (unsigned i = 0; i < MaxKeyboards; i++)
			{
				if (s_pKeyboard[i] == 0) continue;
				s_OutQ[i].push (w);
				if (nPort == 2) break;
			}
	}
	for (unsigned i = 0; i < MaxKeyboards; i++)
	{
		CUSBMIDIDevice *p = (CUSBMIDIDevice *) s_pKeyboard[i];
		if (p == 0)
		{
			s_OutQ[i].clear ();
			continue;
		}
		unsigned nState = p->GetSendState ();
		if (nState == 1) continue;
		unsigned nNow = CTimer::GetClockTicks ();	// (1 MHz)
		if (nState == 2)
		{
			s_OutQ[i].failed ();
			s_OutQ[i].hold (nNow);
		}
		if (s_OutQ[i].held (nNow)) continue;
		u32 Batch[MidiOutQ::kBatch];
		unsigned n = s_OutQ[i].pop (Batch);
		if (n != 0 && !p->SendEventPacketsAsync ((const u8 *) Batch, n * 4))
		{
			s_OutQ[i].failed ();
			s_OutQ[i].hold (nNow);
		}
	}
}

// The computer's packet (gadget IRQ): its notes, sustain and all-notes-off go to core::keys (which plays them here or
// passes them on to the Rytm); everything else to the Rytm as before
static boolean KeysTap (const u8 *p)
{
	u8 cin = p[0] & 0x0F;
	if (!(cin == 0x8 || cin == 0x9 || (cin == 0xB && (p[2] == 64 || p[2] == 120 || p[2] == 123)))) return FALSE;
	if (!bridge::keys_computer_on ()) return FALSE;
	KeysPush (4u << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3]);
	return TRUE;
}
static TUsbBox BoxOf (CDevice *p)
{
	return usb_box (p->GetProperty (CDevice::PropertyVendor), p->GetProperty (CDevice::PropertyProduct));
}

// The Rytm's USB MIDI interface (Circle's umidiN) once enumerated: read only. Another Elektron box: left alone (usb_box.h). Every other USB MIDI device: a keyboard.
// While the Rytm is missing every pass, else four times a second (a keyboard plugged in).
void CKernel::UpdateMIDI (void)
{
	static unsigned s_nScanAt;
	unsigned nNow = m_Timer.GetClockTicks ();
	if (m_pMIDI != 0 && nNow - s_nScanAt < 250000) return;
	s_nScanAt = nNow;
	g_pSidecarKeysTap = KeysTap;
	s_bLinkUp = m_pMIDI != 0;
	// umidiN in Circle's numbering: the gadget's own MIDI device (USB-C, gadget.h) takes a number too
	for (unsigned n = 1; n <= 8; n++)
	{
		CString Name;
		Name.Format ("umidi%u", n);
		CDevice *p = CDeviceNameService::Get ()->GetDevice (Name, FALSE);
		if (p == 0 || p == m_pMIDI || (m_pGadget != 0 && m_pGadget->IsMIDIDevice (p))) continue;
		boolean bKnown = FALSE;
		for (unsigned i = 0; i < MaxKeyboards; i++) bKnown |= s_pKeyboard[i] == p;
		if (bKnown) continue;
		TUsbBox Box = BoxOf (p);
		if (Box == UsbBoxHello)
		{
			if (core::device::active ().id == core::device::kIdA4) Box = UsbBoxHost;	// it chose this boot's folder: it is the box in use
			else if (s_pHelloBox != 0) continue;		// one is heard at a time: a second A4 waits for that one to go
			else
			{
				s_pHelloBox = p;
				((CUSBMIDIDevice *) p)->RegisterRemovedHandler (HelloRemoved, this);
				((CUSBMIDIDevice *) p)->RegisterPacketHandler (HelloPacketHandler);
				m_Logger.Write (From, LogNotice, "midi: an Analog Four MKII (%s): nothing goes to it, listening for its mod's hello", (const char *) Name);
				continue;
			}
		}
		if (Box == UsbBoxOther)
		{
			static u32 s_nSaid;				// umidiN already logged
			if (!(s_nSaid & 1u << n))
			{
				const char *pName = p->GetProperty (CDevice::PropertyProduct);
				m_Logger.Write (From, LogNotice, "midi: %s (%s) is not the host link: nothing goes to it", pName != 0 && *pName ? pName : "an Elektron box", (const char *) Name);
			}
			s_nSaid |= 1u << n;
			continue;
		}
		if (Box == UsbBoxHost)
		{
			if (m_pMIDI != 0) continue;
			CUSBMIDIDevice *pMIDI = (CUSBMIDIDevice *) p;
			pMIDI->RegisterRemovedHandler (MIDIRemoved, this);
			pMIDI->RegisterPacketHandler (MIDIPacketHandler);
			m_Usb.SetMIDI (0);			// a replug's umidi may sit at the old one's address: the sender sees it as new
			m_pMIDI = pMIDI;
			const char *pBox = p->GetProperty (CDevice::PropertyProduct);	// the box's own name: the profile follows its hello, later
			m_Logger.Write (From, LogNotice, "midi: %s's USB MIDI found (%s), listening; the engine's replies %s",
					pBox != 0 && *pBox ? pBox : "the host box", (const char *) Name,
					m_Options.GetAppOptionDecimal ("tohost", 1) != 0 ? "go to it (host link)" : "are dropped (tohost=0)");
			continue;
		}
		unsigned i = 0;
		while (i < MaxKeyboards && s_pKeyboard[i] != 0) i++;
		static CDevice *s_pRefused;
		if (i == MaxKeyboards)
		{
			if (s_pRefused != p) m_Logger.Write (From, LogWarning, "keys: %s not used (%u keyboards at most)", (const char *) Name, MaxKeyboards);
			s_pRefused = p;
			continue;
		}
		const char *pProduct = p->GetProperty (CDevice::PropertyProduct);
		strncpy (g_KeyNames[i], pProduct != 0 && *pProduct ? pProduct : "USB MIDI", sizeof g_KeyNames[i] - 1);
		g_KeyNames[i][sizeof g_KeyNames[i] - 1] = 0;
		s_pKeyboard[i] = p;
		p->RegisterRemovedHandler (KeysRemoved, (void *) (uintptr) i);
		((CUSBMIDIDevice *) p)->RegisterPacketHandler (KeysPacketHandler, (void *) (uintptr) i);
		EnterCritical ();
		KeysPush ((u32) i << 24 | 0xF4u << 16 | 1u << 8);
		LeaveCritical ();
		m_Logger.Write (From, LogNotice, "keys: keyboard %u: %s (%s), its notes play the %s", i + 1, g_KeyNames[i],
				(const char *) Name, "Rytm's selected track (core/keys.h)");
	}
	s_Boxes.clear ();
	for (unsigned n = 1; n <= 8; n++)
	{
		CString Name;
		Name.Format ("umidi%u", n);
		CDevice *p = CDeviceNameService::Get ()->GetDevice (Name, FALSE);
		if (p == 0 || (m_pGadget != 0 && m_pGadget->IsMIDIDevice (p))) continue;
		if (p != m_pMIDI && BoxOf (p) == UsbBoxKeyboard) continue;
		const char *pProduct = p->GetProperty (CDevice::PropertyProduct);
		u16 nId = p == m_pMIDI ? core::device::active ().id : usb_box_id (pProduct);
		TBoxRole Role = p == m_pMIDI ? BoxHost : p == s_pHelloBox ? (s_bHelloHeard ? BoxWaiting : BoxListening) : nId != 0 ? BoxSpare : BoxOther;
		s_Boxes.add (nId, n, Role, pProduct);
	}
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
	const core::device::Profile &Box = core::device::active ();	// the box in use: its word and its voices' names
	u.box = core::device::short_name (Box);
	for (int v = 0; v < SIDECAR_UI_TRACKS; v++) u.voice[v] = Box.screen ? Box.screen->names[v] : 0;
	u.poly = bridge::poly_voices ();	// UX review S1: the POLY badges
	u.pattern = bridge::rytm_pattern (&u.transport);	// T1: HOME's NOW panel
	u.note = m_pTransport == &m_Tcp ? "link: tcp" : m_pTransport == &m_SelfTest ? "link: self-test" : "link: usb";
	static char s_Note[64];				// a keyboard on the Pi (core/keys.h): where its notes go, after the link
	char Keys[40];
	if (bridge::keys_line (Keys, sizeof Keys) != 0)
	{
		CString n;
		n.Format ("%s | %s", u.note, Keys);
		strncpy (s_Note, (const char *) n, sizeof s_Note - 1);
		u.note = s_Note;
	}
	if (powerstate::short_warning (m_Power) != 0) u.note = powerstate::short_warning (m_Power);	// red on the display
	u.notes = CEngineCores::NotesOn ();
	u.out_tracks = g_Output.GetMode () == Output::kTracks;
	u.mac = m_pSidecar != 0 ? (m_pSidecar->MacOn () ? 1 : 0) : -1;
	u.screensaver = (int) m_nSaverMin;
	u.brightness = (int) m_nBright;
	u.touch_hidden = m_bTouchHidden;
	u.ui_style = (int) m_nUiStyle;
	u.ui_look = s_nUiLook;
	u.temp_c = (int) m_nTempC;		// the temperature warning's banner (thermal.h)
	u.temp_warn = m_Thermal.warn ? 1 : 0;
	u.clock_low_mhz = thermal::clock_low_mhz ((int) m_nClockMHz, (int) AskedMHz (), (int) m_nTempC);	// Circle's throttle
	u.cpu_mhz = (int) m_nClockSetting;
	u.usb_mode = (int) m_UsbMode;		// the USB-C mode (usb_mode.h): Settings > Sound
	u.usb_modes = (int) m_nUsbModes;
	u.usb_shown = UsbShown ();
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
	u8 IP[4] = {m_IP[0], m_IP[1], m_IP[2], m_IP[3]};	// cmdline's ip=, the fallback
	if (m_bNetOK) m_Net.GetConfig ()->GetIPAddress ()->CopyTo (IP);	// DHCP on Wi-Fi: the lease's, as WebStatus does
	Text.Format ("%u.%u.%u.%u", IP[0], IP[1], IP[2], IP[3]);
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

// ---- the USB-C mode (usb_mode.h; docs/usb-mirror.md "The setting")

// The mode chosen (display.txt's usbmode=, else cmdline usb=, else CLASS COMPLIANT) on the gadget: before its Start at
// boot, live after (CSidecarGadget::SetMirror: a replug of USB-C when what the computer sees changes)
void CKernel::ApplyUsbMode (void)
{
	m_UsbMode = usb_mode::resolve (m_bUsbStored, m_UsbStored, m_Options.GetAppOptionString ("usb", 0), &m_UsbFrom);
	CSidecarGadget *p = CSidecarGadget::Get ();		// (made, if not yet started, at boot)
	if (p != 0) p->SetMirror (usb_mode::effective (m_UsbMode, m_nUsbModes) == usb_mode::kMirror);
}

int CKernel::UsbShown (void) const
{
	if (m_pSidecar == 0) return -1;
	if (m_pSidecar->MirrorSwitching ()) return 2;
	return m_pSidecar->MirrorShown () ? 1 : 0;
}

void CKernel::PublishUsb (void)
{
	const int nShown = UsbShown ();
	const int nKey = (int) m_UsbMode | (int) m_nUsbModes << 4 | (nShown + 1) << 8;
	if (nKey == m_nUsbPub) return;
	m_nUsbPub = nKey;
	usbc_pi::Publish ((int) m_UsbMode, m_nUsbModes, nShown);
	if (m_pDisplay != 0 && !m_bUiPending)			// the touchscreen's Settings > Sound shows it soon
	{
		m_nUiAt = m_Timer.GetClockTicks () + 50000;
		m_bUiPending = TRUE;
	}
}

// usbmode: the state; usbmode=class|overbridge: stored in display.txt (it then wins over cmdline usb=) and applied at
// once by Run's loop (m_bUsbApply); usbmode=cmdline: the stored one dropped, cmdline usb= (or the default) again. A mode this unit cannot show is
// refused (the build has no mirror, or USB-C is not the Sidecar gadget: cmdline gadget=midi / 0)
boolean CKernel::UsbCommand (const char *pCmd)
{
	if (strncmp (pCmd, "usbmode", 7) != 0 || (pCmd[7] != 0 && pCmd[7] != '=')) return FALSE;
	if (pCmd[7] == '=')
	{
		const char *v = pCmd + 8;
		usb_mode::Mode m = usb_mode::kSidecar;
		if (strcmp (v, "cmdline") == 0)
		{
			if (m_bUsbStored) m_bSaveDisplay = TRUE;
			m_bUsbStored = FALSE;
		}
		else if (!usb_mode::parse (v, &m))
		{
			m_Logger.Write (From, LogWarning, "usbmode=class|overbridge|cmdline");
			return TRUE;
		}
		else if (!(m_nUsbModes & 1u << m))
		{
			m_Logger.Write (From, LogWarning, "usbmode=%s: not here (%s)", usb_mode::name (m),
					m_nUsbModes == 0 ? "USB-C is not the Sidecar gadget: cmdline gadget=" : NoMirrorWhy);
			return TRUE;
		}
		else
		{
			if (!m_bUsbStored || m_UsbStored != m) m_bSaveDisplay = TRUE;
			m_bUsbStored = TRUE;
			m_UsbStored = m;
		}
		// the state now (/status, the pages); the switch itself (a replug of USB-C) is Run's, at one point of its loop
		// (ApplyUsbMode), whichever task asked: the web server, the display, the serial console or the Rytm's page
		const usb_mode::Mode Was = m_UsbMode;
		m_UsbMode = usb_mode::resolve (m_bUsbStored, m_UsbStored, m_Options.GetAppOptionString ("usb", 0), &m_UsbFrom);
		m_bUsbApply = TRUE;
		if (m_UsbMode == usb_mode::kMirror && Was != usb_mode::kMirror && m_pSidecar != 0 && m_pSidecar->OsUpdate ())
			m_Logger.Write (From, LogWarning, "usb: OVERBRIDGE with usbmidi=os:%s: the OS-update filter covers MIDI only; the box's "
					"vendor requests and async channel are relayed unfiltered (docs/usb-mirror.md)", m_pSidecar->OsUpdateName ());
	}
	static const char *const Shown[] = {"no Sidecar gadget", "the Sidecar device", "the box", "off the bus (a replug)"};
	m_Logger.Write (From, LogNotice, "usb: USB-C mode %s (%s; cmdline usb= %s), the computer sees %s%s%s",
			m_UsbMode == usb_mode::kMirror ? "OVERBRIDGE" : "CLASS COMPLIANT", usb_mode::from_name (m_UsbFrom),
			m_bUsbStored ? "ignored while one is stored" : "used", Shown[UsbShown () + 1], m_bUsbApply ? " (a switch is due)" : "",
			m_UsbMode == usb_mode::kMirror ? ": the box's requests and async channel pass unfiltered" : "");
	return TRUE;
}

// touch2: the STATUS page's numbers: LogStats' second (the loads, the longest block and pass, the FIFO), the rest when
// the page is filled (kernel's state, status_pi's core part)
static sidecar_status_view_t s_StatusView;

// the output's peak since the last stats line, shared with the web's "levels" (peak_fan.h)
static float StatsPeak (void)
{
	g_PeakFan.feed_master (g_Output.TakePeak ());
	return g_PeakFan.take_master (peakfan::kStats);
}

void CKernel::LogStats (void)
{
	m_pTransport->LogStats (&m_Logger);
	CEngineCores::TStats s;
	CEngineCores::TakeStats (&s);
	bridge::livein_fifo (CEngineCores::FifoTarget ());	// LIVE IN's latency estimate (the FIFO the link holds)
	unsigned nTemp;		// the one reading a second (a mailbox call, traced): stats line, STATUS, /status, the warning
	{ mbtrace::Scope Mb (3); nTemp = m_CPUThrottle.GetTemperature (); }
	s_nTempC = m_nTempC = nTemp;
	ThermalStep (nTemp);
	FollowClock ();
	auto pct = [&] (u64 t) { return (unsigned) (t * 1000 / s.nTicks); };	// tenths of a percent
	for (int c = 0; c < 4; c++)
	{
		m_nAsleepPct[c] = (pct (s.nSleep[c]) + 5) / 10;
		m_nWakes[c] = s.nWakes[c];
	}
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
		sidecar_status_view_t &sv = s_StatusView;		// touch2: the STATUS page
		sv.valid = 1;
		for (int v = 0; v < SST_VOICES; v++)
		{
			sv.vload[v] = (int) (s.nVoice[v] * 1000 / s.nTicks);	// tenths of a percent of one render core
			sv.muted[v] = g_Guard.Muted ((unsigned) v) ? 1 : 0;
		}
		for (int c = 0; c < SST_CORES; c++) sv.load[c] = m_Ui.load[c];
		sv.block_max_us = s.nMaxBlockUs, sv.block_budget_us = bridge::kBlock * 1000000 / 48000;
		sv.pass_max_us = s.nMaxPassUs, sv.fifo_min = s.nFifoMin, sv.fifo_target = CEngineCores::FifoTarget ();
		sv.temp_c = (int) nTemp;
		sv.temp_warn = m_Thermal.warn ? 1 : 0;
		sv.clock_low_mhz = thermal::clock_low_mhz ((int) m_nClockMHz, (int) AskedMHz (), (int) nTemp);
		sv.cpu_mhz = (int) m_nClockMHz;
		sv.blocks = s_nBlocksTotal;
		sv.ob = m_pTransport->Running () ? 1 : 0;
		sv.net = nNet, sv.wifi = bWifi ? 1 : 0;
		PushUi ();
		static unsigned s_nDisplayLog;
		if (++s_nDisplayLog % 10 == 0)			// every 10 s: the redraw cost on this core
			m_Logger.Write (From, LogNotice, "display: %ux%u, input %s, longest update %u us%s%s", m_pDisplay->GetWidth (),
					m_pDisplay->GetHeight (), m_pDisplay->GetInput (), m_pDisplay->TakeMaxUpdateUs (),
					m_pDisplay->TakeRotateDiag (), m_pDisplay->TakeTouchDiag ());
	}
	m_Logger.Write (From, LogNotice,
		"engine: fifo min %u max %u target %u under %u (restart %u, streams %u) | blocks %u max %u us | cpu c1 %u.%u c2 %u.%u c3 %u.%u %% | "
		"pass max %u us | %s| midi %u B (lost %u) sysex %u notes %u%s | to-host sent %u B %u msgs fail %u dropped %u | peak %u/1000 | %u C",
		s.nFifoMin, s.nFifoMax, CEngineCores::FifoTarget (), g_Output.Underflows (), g_Output.RestartUnderflows (), g_Output.Streams (), s.nBlocks, s.nMaxBlockUs,
		c1 / 10, c1 % 10, c2 / 10, c2 % 10, c3 / 10, c3 % 10, s.nMaxPassUs, (const char *) Mach, s.nMidiBytes, s_nMidiInLost, s.nSysEx, s.nNotes, (const char *) Chans,
		m_Usb.MIDIOutBytes (), m_Usb.MIDIOutMessages (), m_Usb.MIDIOutFailed (),
		m_Usb.MIDIOutDropped () + m_SelfTest.MIDIOutDropped () + m_Tcp.MIDIOutDropped () + g_nToHostFull + bridge::to_host_dropped (), (unsigned) (StatsPeak () * 1000),
		nTemp);
	static unsigned s_nPowerTick;
	if (++s_nPowerTick % 10 == 0) { mbtrace::Scope Mb (1); ReadPower (FALSE); }	// every 10 s (a mailbox call)
	if (s_nPowerTick % 10 == 5)				// every 10 s: the heat's figures, in the black box as well
	{
		MeasureClock ();
		m_Logger.Write (From, LogNotice, "idle: asleep %u %u %u %u %%, wakes %u %u %u %u a second (cores 0-3), wfe %s, ARM clock %u MHz, measured %u, SoC %u C",
				m_nAsleepPct[0], m_nAsleepPct[1], m_nAsleepPct[2], m_nAsleepPct[3], m_nWakes[0], m_nWakes[1], m_nWakes[2], m_nWakes[3],
				CEngineCores::IdleWfe () ? "on" : "off", m_nClockMHz, m_nMeasuredMHz, nTemp);
	}
	if (s_nPowerTick % 60 == 0 && m_bWeb) CWebServer::LogStats (&m_Logger);	// the web page's connections, once a minute
	if (s_nPowerTick % 60 == 0) paircode_pi::WarnTick ();	// and the loud line while the pairing code is off or fixed
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

// The CPU clock option (cpuclock.h; Settings > System: 1500 / 1200 / 1000 MHz). cpumhz= in /.rytm/display.txt, cmdline.txt's
// wins; read before the boot bench. 1500 is the board's full clock (the Pi 4's 1500; a Pi 5 keeps its own maximum).
void CKernel::LoadClock (boolean bCard)
{
	unsigned nMHz = 1500;
	if (bCard)
	{
		static char Text[1536];
		FIL f;
		UINT n = 0;
		Text[0] = 0;
		if (f_open (&f, "SD:/.rytm/display.txt", FA_READ) == FR_OK)
		{
			f_read (&f, Text, sizeof Text - 1, &n);
			f_close (&f);
		}
		Text[n] = 0;
		const char *p = strstr (Text, "cpumhz=");
		if (p != 0 && (p == Text || p[-1] == '\n'))
		{
			unsigned v = 0;
			for (p += 7; *p >= '0' && *p <= '9' && v < 100000; p++) v = v * 10 + (*p - '0');
			nMHz = cpuclock::normalize (v);
		}
	}
	if (m_Options.GetAppOptionString ("cpumhz", 0) != 0) nMHz = cpuclock::normalize (m_Options.GetAppOptionDecimal ("cpumhz", nMHz));
	m_nClockSetting = nMHz;
	if (nMHz == 1500) m_nClockMHz = (m_CPUThrottle.GetClockRate () + 500000) / 1000000;	// as Circle set it (its maximum): no change
	else SetClock (nMHz);
	m_Logger.Write (From, LogNotice, "cpu: setting %u MHz, ARM clock %u MHz (asks for %u; the firmware's range %u-%u, Circle lowers it above %u C)",
			m_nClockSetting, m_nClockMHz, AskedMHz (), m_CPUThrottle.GetMinClockRate () / 1000000, m_CPUThrottle.GetMaxClockRate () / 1000000,
			m_Options.GetSoCMaxTemp ());
}

unsigned CKernel::AskedMHz (void) const
{
	return cpuclock::asked_mhz (m_nClockSetting, m_CPUThrottle.GetMinClockRate (), m_CPUThrottle.GetMaxClockRate ());
}

// Runtime, not config.txt's arm_freq: Settings > System can set it, it takes a tap and no reboot, and a boot file the file
// API cannot write is not a setting. Below the full clock the ARM clock is set through the mailbox (Circle's CCPUThrottle
// only knows its minimum and maximum), and CCPUThrottle::Update is no longer called: it would put the maximum back
// whenever the SoC is cool. The firmware's own limits (80 C soft, 85 C hard) stay. Back at 1500: CCPUThrottle's SetSpeed
// (maximum) and its Update again. One mailbox call to set it and one to read it back, on core 0, when the setting changes:
// a tap during playback can be heard once. The voice budget follows (ApplyBudget): raised before the clock drops, lowered
// after it rises, so a voice is never judged against a budget for the wrong clock.
void CKernel::SetClock (unsigned nSettingMHz)
{
	nSettingMHz = cpuclock::normalize (nSettingMHz);
	m_nClockSetting = nSettingMHz;
	if (!m_CPUThrottle.IsDynamic ())
	{
		m_Logger.Write (From, LogWarning, "cpu: the firmware gave no clock range, the CPU clock is left as it is");
		return;
	}
	const unsigned nHz = cpuclock::target_hz (nSettingMHz, m_CPUThrottle.GetMinClockRate (), m_CPUThrottle.GetMaxClockRate ());
	const boolean bFull = nHz >= m_CPUThrottle.GetMaxClockRate ();
	if (m_nClockMHz != 0 && nHz / 1000000 < m_nClockMHz) ApplyBudget (nHz / 1000000, FALSE);	// lowering: the budget first
	if (bFull)
	{
		m_CPUThrottle.SetSpeed (CPUSpeedMaximum, FALSE);
		m_bClockCapped = FALSE;
	}
	else
	{
		CBcmPropertyTags Tags;
		TPropertyTagSetClockRate Tag;
		Tag.nClockId = CLOCK_ID_ARM;
		Tag.nRate = nHz;
		Tag.nSkipSettingTurbo = 0;
		if (!Tags.GetTag (PROPTAG_SET_CLOCK_RATE, &Tag, sizeof Tag, 12))
			m_Logger.Write (From, LogWarning, "cpu: the firmware refused %u MHz", nHz / 1000000);
		m_bClockCapped = TRUE;
	}
	CTimer::SimpleusDelay (355);				// (the clock's transition: Circle waits as long)
	m_nClockMHz = (m_CPUThrottle.GetClockRate () + 500000) / 1000000;
	if (m_nClockMHz == 0) m_nClockMHz = nHz / 1000000;
	ApplyBudget (m_nClockMHz, TRUE);
	LogClockRescale ();
}

// The guard's budget (guard.h): voicebudget= (50 % of a 12-frame block by default, the share measured at 1500 MHz) scaled
// by 1500 / the clock (cpuclock.h budget_ticks), so the same work is allowed at any clock. At the full 1500 MHz the line
// is the one test-on-pi.sh expects.
void CKernel::ApplyBudget (unsigned nMHz, boolean bLog)
{
	u64 nCntFrq;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (nCntFrq));
	unsigned nPct = m_Options.GetAppOptionDecimal ("voicebudget", 50);
	nPct = nPct < 5 ? 5 : nPct > 100 ? 100 : nPct;
	const unsigned nTicks = cpuclock::budget_ticks (nCntFrq, nPct, nMHz);
	g_Guard.SetBudget (nTicks);
	g_Guard.SetBlock ((unsigned) (nCntFrq * 12 / 48000));
	m_nBudgetUs = (unsigned) (nTicks / (nCntFrq / 1000000));
	if (!bLog) return;
	const unsigned nBlock = (unsigned) (nCntFrq * 12 / 48000);
	if (nTicks == cpuclock::budget_ticks (nCntFrq, nPct, cpuclock::kRefMHz))
		m_Logger.Write (From, LogNotice, "guard: voice budget %u us (%u %% of a 12-frame block)", m_nBudgetUs, nPct);
	else
		m_Logger.Write (From, LogNotice, "guard: voice budget %u us (%u %% of a 12-frame block: voicebudget %u %% scaled for %u MHz)",
				m_nBudgetUs, (unsigned) ((u64) nTicks * 100 / nBlock), nPct, nMHz);
}

// The boot bench ran at m_nBenchMHz; the figures scale with the clock, so a change of the setting says what they come to
// now (a linear estimate: the memory-bound parts scale less, and it is not re-measured: the bench loads every core).
void CKernel::LogClockRescale (void)
{
	if (m_nBenchMHz == 0 || m_nClockMHz == 0) return;
	CString Line;
	for (unsigned m = 0; m < CEngineCores::BenchMachines; m++)
	{
		unsigned nVoices = m_nBenchBp[m] ? 8000 / m_nBenchBp[m] : 999;
		CString One;
		One.Format (" %s %u->%u", CEngineCores::BenchMachine[m], nVoices, cpuclock::voices_at (nVoices, m_nBenchMHz, m_nClockMHz));
		Line.Append (One);
		if (m == CEngineCores::BenchMachines / 2 - 1 || m == CEngineCores::BenchMachines - 1)
		{
			m_Logger.Write (From, LogNotice, "bench rescaled: voices a core at 80 %% (%u -> %u MHz, linear, not re-measured):%s",
					m_nBenchMHz, m_nClockMHz, (const char *) Line);
			m_Timer.MsDelay (2);
			Line = "";
		}
	}
}

// The temperature warning (thermal.h): on at 80 C, off below 75 C, from the second's one reading. Logged once per
// crossing (the clock the unit runs at then: one mailbox call, at the crossing only). The Rytm's popup is owed once
// per crossing: sent each second until the Rytm's remote UI has shown it (it cannot while a page is open, the connect
// splash runs, or the Rytm does not understand popups), dropped when the warning clears first. It never overrides a page.
// Its text says so when Circle's throttle has lowered the clock (socmaxtemp 78: by 80 C it usually has).
void CKernel::ThermalStep (unsigned nTempC)
{
	thermal::Event e = thermal::update (m_Thermal, (int) nTempC);
	if (e == thermal::kWarned)
		m_Logger.Write (From, LogWarning, "thermal: SoC %u C, warning on (from %d C, off below %d C), warning %u this boot, ARM clock %u MHz",
				nTempC, (int) thermal::kWarnC, (int) thermal::kClearBelowC, m_Thermal.crossings,
				m_CPUThrottle.GetClockRate () / 1000000);
	else if (e == thermal::kCleared)
		m_Logger.Write (From, LogNotice, "thermal: SoC %u C, warning off (peak %d C)", nTempC, m_Thermal.peak_c);
	if (thermal::popup_due (m_ThermalPopup, m_Thermal, e, bridge::rui_popups ()))
	{
		char L1[33], L2[33];
		thermal::popup_text (L1, L2, (int) nTempC, thermal::clock_low_mhz ((int) m_nClockMHz, (int) AskedMHz (), (int) nTempC));
		bridge::rui_popup (L1, L2);
	}
}

// The voice budget follows the ARM clock the cores really run at, read back once a second here (one mailbox call, as
// the temperature above). The setting is not enough: at 1500 Circle's CCPUThrottle::Update (the main loop) drops the
// clock to its minimum - 600 MHz on the Pi 4 - when the SoC passes its enforced temperature and raises it again 3 C
// below, and nothing told the budget, so every voice was judged against the 1500 MHz budget at 600 and the guard muted
// voices that were only slow because the clock was. Called right after each CCPUThrottle::Update that can act (the main
// loop) and once a second here, so the budget follows a throttle at once and anything else within a second.
void CKernel::FollowClock (void)
{
	unsigned nHz;
	{ mbtrace::Scope Mb (4); nHz = m_CPUThrottle.GetClockRate (); }
	const unsigned nMHz = cpuclock::follow_mhz (m_nClockMHz, nHz);
	if (nMHz == 0) return;
	m_Logger.Write (From, LogWarning, "cpu: ARM clock %u MHz (was %u, setting %u, SoC %u C): the voice budget follows",
			nMHz, m_nClockMHz, m_nClockSetting, m_nTempC);
	m_nClockMHz = nMHz;
	ApplyBudget (nMHz, TRUE);
}

// What the firmware measures, not what it says it set: GET_CLOCK_RATE repeats the rate asked for, so the firmware's own
// throttle (80 C soft, 85 C hard, undervoltage) is invisible to FollowClock. One mailbox call, every 10 s, not per /status
// request; logged when the two part and when they agree again. Only reported: the voice budget stays on FollowClock's.
void CKernel::MeasureClock (void)
{
	static boolean s_bOff = FALSE;
	CBcmPropertyTags Tags;
	TPropertyTagClockRate Tag;
	Tag.nClockId = CLOCK_ID_ARM;
	boolean bOK;
	{ mbtrace::Scope Mb (5); bOK = Tags.GetTag (PROPTAG_GET_CLOCK_RATE_MEASURED, &Tag, sizeof Tag, 4); }
	if (!bOK) return;
	m_nMeasuredMHz = (Tag.nRate + 500000) / 1000000;
	boolean bOff = cpuclock::measured_off (m_nClockMHz, m_nMeasuredMHz);
	if (bOff == s_bOff) return;
	s_bOff = bOff;
	m_Logger.Write (From, bOff ? LogWarning : LogNotice, "cpu: measured %u MHz, reported %u MHz (SoC %u C)", m_nMeasuredMHz, m_nClockMHz, m_nTempC);
}

void CKernel::BootStep (unsigned nStep)
{
#ifdef SIDECAR_PI5_TESTMODE
	if (m_nTestSecs != 0) m_Watchdog.Start (15);	// fed through the boot
#endif
	if (m_pDisplay != 0) m_pDisplay->SplashStep (nStep);
}

void CKernel::StopAndReboot (const char *pWhy, boolean bTryboot)
{
	m_Logger.Write (From, LogNotice, "reboot: %s - stopping the %s stream first", pWhy, m_pTransport->Name ());
	{
		char Why[68];
		core_snprintf (Why, sizeof Why, "a planned reboot: %s", pWhy);
		crumb::Why (Why);
	}
	m_pTransport->Stop ();
	if (m_pTransport != &m_Usb) m_Usb.Stop ();		// a Rytm that streams anyway (no-op without one)
	m_Timer.MsDelay (50);					// the log line out
	if (bTryboot && !CSdFiles::SetTryboot ()) m_Logger.Write (From, LogWarning, "reboot: the tryboot flag was refused (a plain boot follows)");
	blackbox_pi::Flush ();					// and onto the card (the black box; after the flag: a slow card cannot lose it)
	m_Watchdog.Stop ();
	reboot ();
}

#if RASPPI >= 5
void CKernel::StopAndPowerOff (const char *pWhy)
{
	m_Logger.Write (From, LogNotice, "power-off: %s - the audio faded, the card's work drained, then off", pWhy);
	{
		char Why[68];
		core_snprintf (Why, sizeof Why, "a planned power-off: %s", pWhy);
		crumb::Why (Why);
	}
	if (m_pDisplay != 0) m_pDisplay->PowerOff ();		// dark at once: the press was seen
	CObUsb::FadeOut ();
	m_Timer.MsDelay (30);					// the 20 ms fade out to the Rytm: no click
	g_Rec.stop ();						// a take ends: its files closed in the drain
	m_pTransport->Stop ();
	if (m_pTransport != &m_Usb) m_Usb.Stop ();
	if (m_bSaveDisplay) SaveDisplaySettings ();
	bridge::save_flush (true);				// core 3's idle saves: the settle waits skipped
	// the drain: the card queue (core 3's posted writes), the recorder's ring and close, until all idle for 100 ms
	pwrbtn::Drain Drain (m_Timer.GetClockTicks ());
	rec::Status s;
	boolean bDone;
	do
	{
		bridge::card_service (8);
		rec_pi::Poll ();
		g_Rec.status (&s, false);
		asm volatile ("dsb sy; sev");			// core 3 out of its wfe: its idle saves run
		bDone = Drain.Done (s.state == rec::Recorder::kIdle && bridge::save_settled (), m_Timer.GetClockTicks ());
	}
	while (!bDone);
	if (Drain.TimedOut (m_Timer.GetClockTicks ()))
		m_Logger.Write (From, LogWarning, "power-off: the card's work did not finish in %u s (recorder %d, saves %s): off anyway",
				pwrbtn::kDrainCapUs / 1000000, s.state, bridge::save_settled () ? "done" : "pending");
	else
		m_Logger.Write (From, LogNotice, "power-off: the card's work done in %u ms",
				(m_Timer.GetClockTicks () - Drain.nStart) / 1000);
	m_Timer.MsDelay (50);					// the log line out
	blackbox_pi::Flush ();
	m_Watchdog.Stop ();
	poweroff ();
}

void CKernel::PowerButtonPoll (void)
{
	unsigned nNow = m_Timer.GetClockTicks ();
	if (nNow - m_nButtonAt < pwrbtn::kPollUs) return;
	m_nButtonAt = nNow;
	// active low; the 50 ms debounce (Circle's) only once it reads pressed
	if (m_Button.Poll (!(read32 (ARM_GPIO1_DATA0) & BIT (20)), [] { return is_power_button_pressed () != FALSE; }))
		StopAndPowerOff ("the power button");
}
#endif

#ifdef SIDECAR_PI5_TESTMODE
void CKernel::UsbTestTick (void)
{
	static unsigned s_nLastLog;
	unsigned nNow = m_Timer.GetUptime ();
	if (nNow >= m_nTestSecs)
	{
		m_Logger.Write (From, LogNotice, "testsecs: %u s are up", m_nTestSecs);
		blackbox_pi::TestLog (m_bTestCard);
		StopAndReboot ("testsecs");
	}
	if (nNow >= s_nLastLog + 10)
	{
		s_nLastLog = nNow;
		blackbox_pi::TestLog (m_bTestCard);
	}
}
#endif

void CKernel::Run (void)
{
	m_Serial.RegisterMagicReceivedHandler (RebootMagic, RebootRequest);
	m_Logger.Write (From, LogNotice, "boot timestamp %u us; waiting for the Rytm (1935:0b48, OVERBRIDGE mode)",
			m_Timer.GetClockTicks ());
	m_nLastStats = m_Timer.GetTicks ();
	if (m_Options.GetAppOptionDecimal ("selftest", 0) == 1) SetSelfTest (TRUE, "cmdline");
	m_Watchdog.Start (15);
	CEngineCores::EventStream ();		// core 0's wfe at the loop's end: 152 us at most (idlewait.h)
	m_Logger.Write (From, LogNotice, "idle: wfe %s (idlewfe=0 turns it off)", CEngineCores::IdleWfe () ? "on" : "off");
	u64 nLoopT = 0;
	boolean bCrumbCard = FALSE;
	for (;;)
	{
		crumb::Beat (0);
		if (!bCrumbCard && m_Timer.GetUptime () >= 60)	// the previous boot's breadcrumb on the card (CrumbToCard)
		{
			bCrumbCard = TRUE;
			CrumbToCard (&m_Logger);
		}
		{	// a whole iteration of this loop (top to top): what the wrapped sections below do not cover (the display, IRQs)
			u64 t, f;
			asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t));
			asm volatile ("mrs %0, cntfrq_el0" : "=r" (f));
			if (nLoopT) mbtrace::Note (30, g_Output.FramesSinceReset (), (unsigned) ((t - nLoopT) * 1000000 / f), 1000);
			nLoopT = t;
		}
		m_Watchdog.Start (15);	// fed: a hang of this loop reboots in 15 s (the BCM watchdog's maximum: a TCP connect may block for seconds)
#ifdef SIDECAR_PI5_TESTMODE
		if (m_nTestSecs != 0) UsbTestTick ();
#endif
		if (m_Console.TakeWatchdogTest ())
		{
			m_Logger.Write (From, LogWarning, "watchdog test: core 0 hangs (the watchdog reboots in 15 s)");
			for (;;) asm volatile ("");	// interrupts still run: the log line goes out
		}
		boolean bPlugAndPlay;
		{ mbtrace::Scope Mb (10, 200); bPlugAndPlay = m_USBHCI.UpdatePlugAndPlay (); }
		CObUsb::PollReEnum ();		// a box that came up below high speed (ob_speed.h)
		{ mbtrace::Scope Mb (18, 200); CObUsb::PollHalt (); }	// an Overbridge endpoint (IN / OUT, the A4's too) halted or quiet: reset (ep_halt.h)
		{ mbtrace::Scope Mb (11, 200); if (m_pGadget != 0) m_pGadget->Poll (); }
		{ mbtrace::Scope Mb (12, 200); if (m_pSidecar != 0) m_pSidecar->Poll (); }
		{ mbtrace::Scope Mb (13, 200); UpdateMIDI (); }
		g_bComputerMidi = m_pSidecar != 0;
		AsampMidiSend (m_pSidecar);
		if (m_pTransport == &m_SelfTest && CObUsb::Streaming ()) SetSelfTest (FALSE, "the Rytm streams");
		if (m_pTransport == &m_SelfTest && m_pMIDI != 0) SetSelfTest (FALSE, "the Rytm's USB MIDI is up");
		int nSelfTest = m_Console.TakeSelfTest ();
		if (nSelfTest >= 0) SetSelfTest (nSelfTest == 1, "console");
		m_SelfTest.SetQuiet (m_Console.Dumping ());
		// (a capped clock is held: Update would raise it again when cool.) CCPUThrottle::Update acts once every 4 s
		// and is a no-op in between; it is called on that cadence here (4 s + 10 ms since our last call, so its own
		// 4 s gate is open every time) so that the clock it may just have changed is read back at once and the voice
		// budget follows within microseconds instead of at the next stats tick (FollowClock, also there)
		if (!m_bClockCapped && m_Timer.GetClockTicks () - m_nThrottleAt >= 4 * CLOCKHZ + CLOCKHZ / 100)
		{
			m_nThrottleAt = m_Timer.GetClockTicks ();
			{ mbtrace::Scope Mb (2, 200); m_CPUThrottle.Update (); }
			FollowClock ();
		}
		m_Usb.SetMIDI (m_pMIDI);			// umidi1 (or 0): the engine's replies to the Rytm
		{ mbtrace::Scope Mb (14, 200); m_pTransport->Poll (); }
		if (m_pTransport != &m_Tcp) m_Tcp.Reap ();	// a connect abandoned by :link=usb is closed when it ends
		{ mbtrace::Scope Mb (15, 200); m_Console.Poll (); }
		// the card's turns, MIDI between the requests (card_pass.h): only the USB transport's Poll, which is the merge (TCP's may connect)
		auto CardPass = [this] ()
		{
			card_pass (8, [] { mbtrace::Scope Mb (16, 200); return bridge::card_service (1); },
				   [this]
				   {
					AsampMidiSend (m_pSidecar);
					if (m_pTransport == &m_Usb) { mbtrace::Scope Mb (14, 200); m_Usb.Poll (); }
				   });
		};
		CardPass ();								// the streams' card requests first (core/msamp/cardq.h)
		{ mbtrace::Scope Mb (17, 200); rec_pi::Poll (); }			// the recorder's ring to the card: one 64 KB piece a pass at most
		CardPass ();
		{					// a project bundle's step: one piece, only while the card queue is idle
			unsigned nRecSecs;
			boolean bBundle;
			{ crumb::Section Cs (21); bBundle = bundle_pi::poll (rec_pi::UiState (&nRecSecs)); }
			if (!bBundle)
			{
				CardPass ();
				crumb::Section Cs (22);
				lib_pi::poll ();	// the sample library's step: one piece, the same rules, never during a bundle job
			}
		}
		CardPass ();
		{ mbtrace::Scope Mb (19, 200); stor_pi::Poll (bPlugAndPlay); }		// USB drives in and out, the bench's step (stor_pi.h)
#ifdef SIDECAR_PI5_TESTMODE
		if (m_nTestSecs == 0 || m_bTestCard)
#endif
		blackbox_pi::Poll ();						// the log to /.rytm/blackbox.log (its own crumb section, 42)
		{ mbtrace::Scope Mb (20, 200); bridge::samp_poll (); }			// SAMPLING / the AUTO-SAMPLER: a take's step (core/samp)
		if (m_pDisplay != 0)
		{
			crumb::Section Cs (23);
#if SIDECAR_SEQED
			{					// the SEQ page: core 3's published view (when new) and the play head
				static sidecar_seq_view_t s_SeqView;
				static unsigned s_nSeqSeen;
				if (seqed_pi::View (&s_SeqView, &s_nSeqSeen))
				{
					strncpy (s_SeqView.project, m_Ui.project, sizeof s_SeqView.project - 1);
					sidecar_seqed_view (&s_SeqView);
					static unsigned s_nShowReq;
					if (s_SeqView.show_req != s_nShowReq)	// seq-ux: TRK + TRIG on the Rytm (rui1037; rui1036 FUNC + LFO): the SEQ page
					{
						s_nShowReq = s_SeqView.show_req;
						sidecar_ui_show (4);
					}
				}
				sidecar_seqed_playhead (seqed_pi::Playhead ());
			}
			{					// the Rytm pattern view: fetched while open, its age / fetching shown
				static unsigned s_nPatSeen, s_nPatAt;
				int nPage = sidecar_rytmpat_shown () ? 1 : sidecar_voice_shown () ? 2 : 0;	// (VOICE: the track's p-locks)
				int bOpen = nPage != 0;
				rytmpat_pi::Want (nPage);
				int nIndex;
				if (rytmpat_pi::Pattern (&s_RytmPat, &nIndex, &s_nPatSeen))
				{
					s_nRytmPatIndex = nIndex;
					sidecar_rytmpat_set (&s_RytmPat, nIndex, m_Ui.project, "", "from the Rytm");
				}
				unsigned nNow = m_Timer.GetClockTicks ();
				if (bOpen && nNow - s_nPatAt >= 250000)
				{
					s_nPatAt = nNow;
					rytmpat_pi::State St = rytmpat_pi::Get ();
					CString Where;
					if (St.age_ms < 0) Where = St.pending ? "fetching..." : St.lost ? "no answer from the Rytm" : "waiting";
					else if (St.pending) Where = "fetching...";
					else if (St.lost) Where.Format ("no answer, %d s old", St.age_ms / 1000);
					else if (St.stale == 2) Where = "edited: updates after STOP";
					else if (St.stale) Where = "edited: updates when edits settle";
					else if (St.age_ms < 2000) Where = "from the Rytm";
					else Where.Format ("from the Rytm %d s ago", St.age_ms / 1000);
					sidecar_rytmpat_where ((const char *) Where, St.pending || St.lost || St.stale);
				}
			}
#endif
			{					// rui1036b: the Rytm's UP + T1..T5 (the page), VOICE following its track
				static uint32_t s_nReqSeen, s_nTrkSeen;
				static int s_bReqInit;
				if (!s_bReqInit)
				{
					s_bReqInit = 1;
					voice_pi::Seen (&s_nReqSeen, &s_nTrkSeen);	// (requests before the display: not acted on)
				}
				int nCur = sidecar_ui_page ();
				static int s_nHeldPg = -1;
				// (CHOP too: it follows the track as VOICE does; SAMPLE EDIT never: it is not in FollowsTrack)
				int nPg = voice_pi::Request (&s_nReqSeen, voice_pi::FollowsTrack (nCur), &s_nTrkSeen);
				// a marker of CHOP or SAMPLE EDIT under the finger: the page asked for waits for the release
				int bDrag = (nCur == 17 && sidecar_chop_dragging ()) || (nCur == seditview_pi::kPage && sidecar_sedit_dragging ());
				if (nPg >= 0)
				{
					if (bDrag) { s_nHeldPg = nPg; nPg = -1; }	// a marker under the finger: its release is sent first
					else s_nHeldPg = -1;
				}
				else if (s_nHeldPg >= 0 && !bDrag)
				{
					if (nCur == 17 || nCur == seditview_pi::kPage) nPg = s_nHeldPg;
					s_nHeldPg = -1;
				}
				if (nPg >= 0 && nPg != nCur) sidecar_ui_show (nPg);
				sidecar_ui_combo_map (voice_pi::ComboMap ());	// UX review N3: UP held on the Rytm: the map here too
			}
			if (sidecar_voice_shown ())		// the VOICE page: ~10 frames a second, redrawn only on a change
			{
				static sidecar_voice_view_t s_VoiceView;
				static unsigned s_nVoiceAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nVoiceAt >= 100000)
				{
					s_nVoiceAt = nNow;
#if SIDECAR_SEQED
					voice_pi::Fill (&s_VoiceView, s_RytmPat.valid ? &s_RytmPat : 0, s_nRytmPatIndex);
#else
					voice_pi::Fill (&s_VoiceView, 0, -1);
#endif
					sidecar_voice_set (&s_VoiceView);
				}
			}
			{					// the SONG page: core 3's view (~10 a second while it shows), a replay to confirm shows it
				song_pi::Want (sidecar_song_shown ());
				static sidecar_song_view_t s_SongView;
				static unsigned s_nSongSeen, s_nSongReq;
				if (song_pi::View (&s_SongView, &s_nSongSeen))
				{
					sidecar_song_set (&s_SongView);
					if (s_SongView.show_req != s_nSongReq)
					{
						if (s_nSongReq || s_SongView.show_req) sidecar_ui_show (10);
						s_nSongReq = s_SongView.show_req;
					}
				}
			}
			{					// the REC page: the recorder ~25 times a second while it shows (its meters only then)
				static bool s_bRecWas;
				static unsigned s_nRecAt;
				bool bRec = sidecar_rec_shown () != 0;
				if (bRec != s_bRecWas) rec_pi::UiWant (s_bRecWas = bRec);
				unsigned nNow = m_Timer.GetClockTicks ();
				if (bRec && nNow - s_nRecAt >= 40000)
				{
					static sidecar_rec_view_t s_RecView;
					s_nRecAt = nNow;
					rec_pi::UiFill (&s_RecView);
					sidecar_rec_set (&s_RecView);
				}
			}
			{					// touch2: SCOPE, ~15 traces a second while it shows (the recorder's meters on then)
				static bool s_bScopeWas;
				static unsigned s_nScopeAt;
				bool bScope = sidecar_scope_shown () != 0;
				if (bScope != s_bScopeWas)
				{
					rec_pi::ScopeWant (s_bScopeWas = bScope);
					if (!bScope) scope_pi::Leave ();
				}
				unsigned nNow = m_Timer.GetClockTicks ();
				if (bScope && nNow - s_nScopeAt >= 66000)
				{
					static sidecar_scope_view_t s_ScopeView;
					uint32_t pk[SSC_CH];
					s_nScopeAt = nNow;
					rec_pi::ScopePeaks (pk);
					scope_pi::Fill (&s_ScopeView, sidecar_scope_source (), sidecar_scope_timebase (), pk);
					sidecar_scope_set (&s_ScopeView);
				}
			}
			if (sidecar_status_shown ())		// touch2: SIDECAR STATUS, twice a second (labels redrawn only on a change)
			{
				static unsigned s_nStatusAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nStatusAt >= 500000)
				{
					s_nStatusAt = nNow;
					sidecar_status_view_t &sv = s_StatusView;
					status_pi::Core (&sv);
					lib_pi::ui_fill (&sv);
					sv.underruns = m_Ui.underruns, sv.rytm = m_Ui.rytm, sv.mac = m_Ui.mac;
					memcpy (sv.fx_type, m_Ui.fx_type, sizeof sv.fx_type);
					memcpy (sv.fx_post, m_Ui.fx_post, sizeof sv.fx_post);
					memcpy (sv.ip, m_Ui.ip, sizeof sv.ip);
					memcpy (sv.host, m_Ui.host, sizeof sv.host);
					memcpy (sv.link, m_Ui.link, sizeof sv.link);
					memcpy (sv.gadget, m_Ui.gadget, sizeof sv.gadget);
					if (!strcmp (sv.gadget, "0")) sv.gadget[0] = 0;
					memcpy (sv.build, m_Ui.build, sizeof sv.build);
					memcpy (sv.circle, m_Ui.circle, sizeof sv.circle);
					rec_pi::CardSpace (&sv.card_free_mb, &sv.card_total_mb);
					stor_pi::UiFill (&sv);	// the USB drive, the NVMe SSD
					paircode_pi::UiFill (&sv);	// the PAIR CODE row (the code is on the screen, not in a log)
					sv.ui_max_us = m_pDisplay->PeekMaxUpdateUs ();
					u32 h = 2166136261u;
					const u8 *b = (const u8 *) &sv;
					for (unsigned i = 0; i < __builtin_offsetof (sidecar_status_view_t, gen); i++) h = (h ^ b[i]) * 16777619u;
					sv.gen = h;
					sidecar_status_set (&sv);
				}
			}
			if (sidecar_project_shown ())		// PROJECT (bundles): the feed five times a second (labels redrawn only on a change)
			{
				static sidecar_project_view_t s_ProjView;
				static unsigned s_nProjAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nProjAt >= 200000)
				{
					s_nProjAt = nNow;
					bundle_pi::ui_fill (&s_ProjView);
					sidecar_project_set (&s_ProjView);
				}
			}
			if (sidecar_insfx_shown ())		// fx-ins: the INSERT FX page, ~10 frames a second, redrawn only on a change
			{
				static sidecar_insfx_view_t s_InsView;
				static unsigned s_nInsAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nInsAt >= 100000)
				{
					s_nInsAt = nNow;
					float Pk[8][2];
					g_Output.TakeVoicePeaks (Pk);	// the OUT route's meters (PRE / POST voices),
					g_PeakFan.feed_voices (Pk);	// shared with the web's "levels" (peak_fan.h)
					g_PeakFan.take_voices (peakfan::kDisplay, Pk);
					insfx_pi::Fill (&s_InsView, Pk);
					sidecar_insfx_set (&s_InsView);
				}
			}
			if (sidecar_mfx_shown ())		// the MASTER FX page, the same way
			{
				static sidecar_mfx_view_t s_MfxView;
				static unsigned s_nMfxAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nMfxAt >= 100000)
				{
					s_nMfxAt = nNow;
					insfx_pi::FillMaster (&s_MfxView);
					sidecar_mfx_set (&s_MfxView);
				}
			}
			{	// 0.5.0: the MORPH page, its view built on core 3 (morph_pi), the newest taken here
				static sidecar_morph_view_t s_MorphView;
				static unsigned s_nMorphSeen;
				morph_pi::Want (sidecar_morph_shown (), sidecar_morph_top ());
				if (sidecar_morph_shown () && morph_pi::View (&s_MorphView, &s_nMorphSeen))
					sidecar_morph_set (&s_MorphView);
			}
			if (sidecar_wgview_shown ())		// the machine view: ~10 frames a second, redrawn only on a change
			{
				static sidecar_wg_view_t s_WgView;
				static unsigned s_nWgAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nWgAt >= 100000)
				{
					s_nWgAt = nNow;
					wgview_pi::Fill (&s_WgView, sidecar_wgview_wanted ());
					sidecar_wgview_set (&s_WgView);
				}
			}
			if (sidecar_preset_shown () || sidecar_wgview_pool_page_shown ())	// the preset pages (and the POOL page's JUNO / 6-OP tabs' counts)
			{
				static sidecar_preset_view_t s_PreView;
				static unsigned s_nPreAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nPreAt >= 100000)
				{
					s_nPreAt = nNow;
					preset_pi::Fill (&s_PreView);
					sidecar_preset_set (&s_PreView);
				}
			}
			if (sidecar_chop_shown ())		// the CHOP page: ~10 frames a second, redrawn only on a change
			{
				static sidecar_chop_view_t s_ChopView;
				static unsigned s_nChopAt;
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nChopAt >= 100000)
				{
					s_nChopAt = nNow;
					chopview_pi::Fill (&s_ChopView, sidecar_chop_wanted ());
					sidecar_chop_set (&s_ChopView);
				}
			}
			{					// SAMPLE EDIT: shown while an edit is open (the Rytm's page, the console, the web),
				static sidecar_sedit_view_t s_SeditView;	// back to the page the display was on when it closes; ~10
				static unsigned s_nSeditAt;		// frames a second, redrawn only on a change
				static int s_nSeditBack = -1;		// the page to go back to (-1: not shown for an open edit yet)
				unsigned nNow = m_Timer.GetClockTicks ();
				if (nNow - s_nSeditAt >= 100000)
				{
					s_nSeditAt = nNow;
					seditview_pi::Fill (&s_SeditView);
					int nCur = sidecar_ui_page ();
					if (s_SeditView.valid && s_nSeditBack < 0 && !(nCur == 17 && sidecar_chop_dragging ()))
					{					// (a CHOP marker under the finger: after its release)
						s_nSeditBack = nCur == seditview_pi::kPage ? 0 : nCur;
						sidecar_ui_show (seditview_pi::kPage);
					}
					else if (!s_SeditView.valid && s_nSeditBack >= 0)
					{
						if (nCur == seditview_pi::kPage) sidecar_ui_show (s_nSeditBack);	// (left by hand: stays there)
						s_nSeditBack = -1;
					}
					sidecar_sedit_set (&s_SeditView);	// (shown or not: the VOICE key comes back to an open edit)
				}
			}
			m_pDisplay->Update (bPlugAndPlay);	// a redraw at most every 10 ms, dirty areas only
			if (m_bUiPending && (int) (m_Timer.GetClockTicks () - m_nUiAt) >= 0)
			{
				m_bUiPending = FALSE;
				PushUi ();
			}
		}
		if (m_bSaveDisplay) SaveDisplaySettings ();
		paircode_pi::Poll ();			// the Rytm's SIDECAR MENU > WEB UI asks: a new code, forget, web on / off
		{
			int nUsbAsk = usbc_pi::TakeAsk ();	// the Rytm's SIDECAR MENU > USB-C asks: as the console's usbmode=
			if (nUsbAsk >= 0) UsbCommand (nUsbAsk == usb_mode::kMirror ? "usbmode=overbridge" : "usbmode=class");
			if (m_bUsbApply)			// a usbmode= from any task: the switch happens here
			{
				m_bUsbApply = FALSE;
				ApplyUsbMode ();
			}
			PublishUsb ();
		}
		char Link[40];
		if (m_Console.TakeLink (Link, sizeof Link)) UseLink (Link);
		if (m_pSidecar != 0) m_pSidecar->SetLinkTcp (m_pTransport == &m_Tcp);	// usbmidi=os:emu follows the host
		if (s_bRebootRequested) StopAndReboot ("the flasher's magic");
		if (m_pSidecar != 0 && m_pSidecar->TakeReboot ()) StopAndReboot ("asked on the USB-C MIDI port (usbreboot=1)");
		if (g_Guard.FatalWhy () != 0) StopAndReboot (g_Guard.FatalWhy ());
		if (m_pWifi->TakeReboot ()) StopAndReboot ("the Wi-Fi settings (a join, or a failed one's revert)");
		if (usbnet_pi::TakeReboot ()) StopAndReboot ("the page's reboot onto USB-C (a computer started its USB-C network late)");
		if (bridge::take_restart ()) StopAndReboot ("another device's state folder");
		CSdFiles::TReboot SdReboot = CSdFiles::TakeReboot ();
		if (SdReboot != CSdFiles::RebootNone) StopAndReboot ("asked on the SD file port", SdReboot == CSdFiles::RebootTry);
#if RASPPI >= 5
		if (m_bPowerOff) StopAndPowerOff ("asked on the console (poweroff=1)");
		PowerButtonPoll ();
#endif
		if (m_Timer.GetTicks () - m_nLastStats >= HZ || m_Console.TakeStatsNow ())
		{
			m_nLastStats = m_Timer.GetTicks ();
			if (m_Console.Dumping ()) m_Timer.MsDelay (25);	// the TX buffer drains: a log line is not cut
			crumb::Section Cs (24);
			LogStats ();
		}
		asm volatile ("dsb sy; sev");			// core 3's heartbeat: the drone and card work run without traffic
		// the self-test's pacer: 7 frames are 146 us. A busy wait never yields, so the network's tasks starve while
		// it runs - fine only because the self-test is refused under link=tcp (SetSelfTest)
		if (m_SelfTest.Running ()) m_Timer.usDelay (20);
		else
		{
			unsigned t = m_Timer.GetClockTicks ();
			{
				crumb::Section Cs (40);
				if (CEngineCores::IdleWfe ())			// the network's tasks run in the Yields (idlewait.h)
					idlewait::spend (t, 1000, [&] { return m_Timer.GetClockTicks (); }, [&] { m_Scheduler.Yield (); },
							 [] { CEngineCores::Sleep (); });
				else m_Scheduler.MsSleep (1);			// the network's tasks run meanwhile
			}
			t = m_Timer.GetClockTicks () - t;	// how late the loop wakes: core 0's tasks (Wi-Fi's polling) measured
			s_nLoopN++;
			s_nLoopSum += t;
			if (t > s_nLoopMax) s_nLoopMax = t;
			stor_pi::NoteLoop (t);		// (the bench's busy phases: their own max, not reset by /status)
		}
	}
}
