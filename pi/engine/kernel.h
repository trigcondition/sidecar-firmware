// pi/engine/kernel.h - the Pi engine's kernel (docs/pi-engine.md): init order and core 0's loop.
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/bcmwatchdog.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbmidi.h>
#include <SDCard/emmc.h>

#include "console.h"
#include "cores.h"
#include "gadget.h"
#include "sidecar_gadget.h"
#include "guard.h"
#include "ob_usb.h"
#include "selftest.h"
#include "tcp_link.h"
#include "web.h"
#include "power_state.h"
#include "thermal.h"
#include "cpuclock.h"
#include "touch_cal.h"		// pi/ui: the touch calibration (touchcal=)
#include "sidecar_ui.h"

class CWifi;				// wifi.h: Wi-Fi (off by default)
class CSidecarDisplay;			// pi/ui/sidecar_display.h: the HDMI display (docs/pi-display.md)

class CKernel
{
public:
	CKernel (void);
	boolean Initialize (void);
	void Run (void);

private:
	// The FIFO target per link, in frames, when no fifo= is given (docs/pi-engine.md "FIFO target"). USB: the
	// Rytm's latency (2 ms), measured on the stock Rytm: core 3's card work blocks up to 1.2 ms (a DSP SAMPLE
	// refill from the card) and 1.3-1.7 ms (a prepare), so 72 left 6 frames and 48 underflowed. TCP: rytm-emu asks in bursts on a Mac (its frame requests are not paced like the
	// Rytm's USB IN stream), so the emulator link holds more; the emulator is a test harness, latency is free.
	static const unsigned FifoUsb = 96;
	static const unsigned FifoTcp = 288;

	void UpdateMIDI (void);
	static void MIDIPacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength);
	static void MIDIRemoved (CDevice *pDevice, void *pContext);
	static boolean Core0Command (const char *pCmd, void *pParam);	// the console's, for the Sidecar
	static void ToHost (const u8 *pData, int nLength);	// the engine's SysEx for the host (control core)
	void SelfCheck (void);
	void LogBench (void);
	void SetSelfTest (boolean bOn, const char *pWhy);
	void LogStats (void);
	void LoadClock (boolean bCard);				// cpumhz= (display.txt, cmdline): at boot, before the bench
	void SetClock (unsigned nSettingMHz);			// the CPU clock option (cpuclock.h): the clock, the voice budget, the log
	void FollowClock (void);				// the stats tick: the budget follows the ARM clock read back
	void ApplyBudget (unsigned nMHz, boolean bLog);		// the voice guard's budget for that clock (voicebudget= scaled by it)
	void LogClockRescale (void);				// the boot bench's voices a core, rescaled to the clock now (not re-measured)
	void ThermalStep (unsigned nTempC);			// the temperature warning (thermal.h): once a second, from LogStats
	void ReadPower (boolean bLog);				// the supply (power_state.h): at boot, every 10 s, logged on a change and once a minute
	void PushUi (void);					// the display's state, read back from the engine (docs/pi-display.md)
	static void UiCommand (const char *pCmd, void *pParam);	// a display control: the console's parser, then a re-read
	void StopAndReboot (const char *pWhy, boolean bTryboot = FALSE);
	void BootStep (unsigned nStep);			// the boot screen's bar (1..CSidecarDisplay::SplashSteps)	// bTryboot: the firmware reads tryboot.txt once
	void UseLink (const char *pSpec);			// usb | tcp | tcp:IP:PORT
	static void WebStatus (CString *pOut, void *pParam);
	static void WebHealth (CString *pOut, void *pParam);	// GET /health: the open one (paircode.h's open set)	// the web page's /status (web.h)

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CLogTee			m_LogTee;		// the logger's target: m_Serial and the web page's log (web.h)
	CEngineExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CScheduler		m_Scheduler;		// the network's tasks (core 0)
	CCPUThrottle		m_CPUThrottle;
	CEMMCDevice		m_EMMC;
	CUSBHCIDevice		m_USBHCI;
	u8			m_IP[4], m_NetMask[4];	// cmdline ip= netmask= (default 192.168.77.2/24): before m_Net
	alignas (CNetSubSystem) u8 m_NetStore[sizeof (CNetSubSystem)];	// built in Initialize: Ethernet, or Wi-Fi (wifi.h)
	CNetSubSystem		&m_Net;
	CEngineCores		m_Cores;
	CConsole		m_Console;
	CObUsbTransport		m_Usb;
	CTcpTransport		m_Tcp;
	CBcmWatchdog		m_Watchdog;
	CSelfTest		m_SelfTest;
	CTransport		*m_pTransport;
	CUSBMIDIDevice *volatile m_pMIDI;
	CSidecarGadget		*m_pSidecar;		// USB-C: "Sidecar", UAC2 + MIDI (sidecar_gadget.h), gadget=sidecar
	CSidecarMIDIGadget	*m_pGadget;		// or P1's MIDI echo (gadget.h), gadget=midi; both 0 with gadget=0
	unsigned		m_nLastStats;
	const char		*m_pGadgetMode;		// gadget= (sidecar, midi, 0)
	boolean			m_bNetOK;		// m_Net.Initialize succeeded: link=tcp refused without it
	CSidecarDisplay		*m_pDisplay;		// display=1 (default) and a frame buffer; else 0
	powerstate::State	m_Power;		// the supply as the firmware reports it (ReadPower)
	// the SoC temperature: LogStats reads it once a second (a mailbox call) and everything else uses that reading
	// (the STATUS page, /status, the warning); thermal.h's warning, and the Rytm's popup owed until it was shown
	thermal::State		m_Thermal;
	volatile unsigned	m_nTempC = 0;		// the last reading, 0: none yet (plain reads from the web task)
	thermal::Popup		m_ThermalPopup;
	// the CPU clock option (cpuclock.h): the setting (1500 = the board's full clock), the clock the cores run at (read back
	// when it was set, then after each CCPUThrottle::Update and once a second: FollowClock), and the voice budget for it; capped: below the full clock, so Circle's Update does not run (it
	// would raise the clock again when the SoC is cool)
	unsigned		m_nClockSetting = 1500;
	volatile unsigned	m_nClockMHz = 0;
	volatile unsigned	m_nBudgetUs = 0;
	boolean			m_bClockCapped = FALSE;
	unsigned		m_nThrottleAt = 0;	// CTimer::GetClockTicks () of the last CCPUThrottle::Update (main loop)
	unsigned		m_nBenchMHz = 0;			// the clock the boot bench ran at, and its worst figure
	unsigned		m_nBenchBp[CEngineCores::BenchMachines];	// per machine (1/100 % of a core a voice), for the rescale
	sidecar_ui_status_t	m_Ui;			// what the display shows (LogStats: the load and counters; PushUi: the rest)
	boolean			m_bUiPending;		// a display command was sent: re-read at m_nUiAt (us, m_Timer.GetClockTicks)
	unsigned		m_nUiAt;
	boolean			m_bWeb;			// web=1 and the network is up
	// the display's screensaver and brightness (docs/pi-display.md): /.rytm/display.txt, cmdline screensaver= /
	// brightness= override it at boot; screensaver=MIN and brightness=PCT on the console (web page, display) change it
	unsigned		m_nSaverMin;
	unsigned		m_nBright;
	volatile boolean	m_bTouchHidden;		// touchtest=0: the home page's touch test is hidden
	boolean			m_bTouchCal;		// touchcal=X0,X1,Y0,Y1,SWAP (pi/ui/touch_cal.h): the 4-point calibration
	touch_cal_t		m_TouchCal;
	unsigned		m_nUiStyle;		// uistyle=oled|classic: the display's theme (sidecar_theme.h), 0 OLED
	volatile boolean	m_bSaveDisplay;		// written by core 0's loop (tmp + rename)
	unsigned		m_nResW, m_nResH;	// res=800x480|1024x600 (display.txt / cmdline): the UI's size, 0: automatic
	unsigned		m_nResFileW, m_nResFileH;	// display.txt's res= alone (kept when the file is written)
	void LoadDisplayRes (boolean bCard);
	void LoadDisplaySettings (boolean bCard);
	void SaveDisplaySettings (void);
	CWifi			*m_pWifi;		// wifi.h: always made (its console commands), the chip only with wifi=1|scan
};

#endif
