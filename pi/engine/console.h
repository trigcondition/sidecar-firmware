// pi/engine/console.h - the serial console (921600 baud): 'w' a 10 s capture (spike3's: IN MAIN L/R, OUT MAIN L,
// note-on markers; dumped as base64 for pi/circle/capture.py), 's' the stats now, ':<command>\n' a command for the
// control core (cores.cpp: notemach=V:NAME drone=V|all|off notes=0|1 out=main|tracks fifo=N; the guard's unmute faulttest=V overtest=V hangtest=V timedtest),
// except ':selftest=on|off', which core 0 runs (selftest.h; while it is on, 'w' captures its output), ':wdtest'
// (core 0's loop hangs: the watchdog test, kernel.cpp) and ':link=usb|tcp|tcp:IP:PORT' (the engine's host, kernel.cpp).
#ifndef _console_h
#define _console_h
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/types.h>

// True while a capture dumps: the engine core's log lines (platform_pi.cpp log(), core 3's trig lines) are held
// back, because another core's bytes written into a dump line garble it (measured: 311 of 134000 lines with the
// Rytm's notes at 8 a second). Counted in g_nQuietLogDropped and reported at the dump's end.
extern volatile boolean g_bQuietLog;
extern volatile unsigned g_nQuietLogDropped;

class CConsole
{
public:
	CConsole (CSerialDevice *pSerial) : m_pSerial (pSerial) {}
	void Initialize (void);		// the capture buffer (1.9 MB)
	void Poll (void);		// core 0's loop
	void Command (const char *pText);	// a command line without the ':' (core 0: the serial console, the web page)
	boolean Dumping (void) const	{ return m_bDumping; }
	boolean TakeStatsNow (void)	{ boolean b = m_bStatsNow; m_bStatsNow = FALSE; return b; }
	int TakeSelfTest (void)		{ int n = m_nSelfTest; m_nSelfTest = -1; return n; }	// -1 none, 0 off, 1 on
	boolean TakeWatchdogTest (void)	{ boolean b = m_bWatchdogTest; m_bWatchdogTest = FALSE; return b; }
	void SetSelfTest (boolean bOn)	{ m_bSelfTest = bOn; }	// 'w' captures the self-test's output
	boolean TakeLink (char *pOut, unsigned nSize);		// a ':link=' command's value (core 0's)
	// core 0's own commands (the Sidecar's :mac= etc., kernel.cpp): TRUE when the handler took it
	void SetCore0Handler (boolean (*pHandler) (const char *, void *), void *pParam)
					{ m_pCore0 = pHandler; m_pCore0Param = pParam; }

private:
	void StartDump (void);
	boolean DumpStep (void);

	CSerialDevice *m_pSerial;
	char m_Cmd[128];	// wifi=join:SSID:PASS is up to 106 characters
	int m_nCmd = -1;		// -1: not in a ':' line
	boolean m_bCmdLong = FALSE;	// the line overflowed m_Cmd: dropped at its end
	boolean m_bStatsNow = FALSE;
	boolean m_bWatchdogTest = FALSE;
	int m_nSelfTest = -1;
	char m_Link[40];
	boolean m_bLink = FALSE;
	boolean (*m_pCore0) (const char *, void *) = 0;
	void *m_pCore0Param = 0;
	boolean m_bSelfTest = FALSE;
	unsigned m_nCaptureStart = 0;
	boolean m_bDumping = FALSE;
	const u8 *m_pDump = 0;
	unsigned m_nDumpLen = 0, m_nDumpPos = 0;
	u32 m_nCrc = 0;
	boolean m_bDumpEnd = FALSE;
	char m_Line[96];
	unsigned m_nLineLen = 0, m_nLineOff = 0;
};

#endif
