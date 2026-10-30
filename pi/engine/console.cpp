// pi/engine/console.cpp - see console.h. The capture's dump is spike3's (kernel.cpp: CRC32, base64 lines, between
// which log lines may fall), unchanged.
#include "rec_pi.h"
#include "stor_pi.h"
#include "bundle_pi.h"
#include "lib_pi.h"
#include "console.h"
#include "wifi.h"
#include "paircode_pi.h"
#include "midibridge_pi.h"
#include "sdfiles.h"

#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>

#include "ob_usb.h"
#include "shared.h"
#include "version.h"

volatile boolean g_bQuietLog = FALSE;
volatile unsigned g_nQuietLogDropped = 0;

static const char From[] = "console";
static const unsigned CaptureTimeoutSecs = CObUsb::CaptureSecs + 5;
static const unsigned B64LineBytes = 57;		// 76 characters
static u32 s_CrcTable[256];

static u32 Crc32 (u32 nCrc, const u8 *p, unsigned n)
{
	nCrc = ~nCrc;
	while (n--) nCrc = s_CrcTable[(nCrc ^ *p++) & 0xFF] ^ (nCrc >> 8);
	return ~nCrc;
}

static unsigned Base64 (char *pOut, const u8 *p, unsigned n)
{
	static const char Tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *q = pOut;
	for (; n >= 3; n -= 3, p += 3)
	{
		*q++ = Tab[p[0] >> 2];
		*q++ = Tab[(p[0] & 3) << 4 | p[1] >> 4];
		*q++ = Tab[(p[1] & 15) << 2 | p[2] >> 6];
		*q++ = Tab[p[2] & 63];
	}
	if (n > 0)
	{
		*q++ = Tab[p[0] >> 2];
		if (n == 1)
		{
			*q++ = Tab[(p[0] & 3) << 4];
			*q++ = '=';
		}
		else
		{
			*q++ = Tab[(p[0] & 3) << 4 | p[1] >> 4];
			*q++ = Tab[(p[1] & 15) << 2];
		}
		*q++ = '=';
	}
	return q - pOut;
}

void CConsole::Initialize (void)
{
	for (u32 i = 0; i < 256; i++)
	{
		u32 c = i;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		s_CrcTable[i] = c;
	}
	if (!CObUsb::CaptureAlloc ())
	{
		CLogger::Get ()->Write (From, LogError, "capture: cannot allocate %u B", CObUsb::CaptureBytes);
	}
}

void CConsole::Poll (void)
{
	CLogger *L = CLogger::Get ();
	if (CSdFiles::SerialActive ()) return;		// ":sdf": the port is the SD file task's (sdfiles.h)
	char Buf[64];
	int n = m_pSerial->Read (Buf, sizeof Buf);
	for (int i = 0; i < n; i++)
	{
		char c = Buf[i];
		if (m_nCmd >= 0)		// inside a ':' line
		{
			if (c == '\n' || c == '\r')
			{
				TCmd Cmd;
				memcpy (Cmd.text, m_Cmd, m_nCmd);
				Cmd.text[m_nCmd] = 0;
				m_nCmd = -1;
				if (m_bCmdLong)
				{
					m_bCmdLong = FALSE;
					L->Write (From, LogWarning, "command too long (max %u characters): \"%s...\" dropped",
						  (unsigned) sizeof m_Cmd - 1, CWifi::IsSecret (Cmd.text) ? "wifi=join:..." : Cmd.text);
					continue;
				}
				Command (Cmd.text);
			}
			else if (m_nCmd < (int) sizeof m_Cmd - 1)
			{
				m_Cmd[m_nCmd++] = c;
			}
			else
			{
				m_bCmdLong = TRUE;
			}
			continue;
		}
		if (c == ':') m_nCmd = 0;
		else if (c == 's') m_bStatsNow = TRUE;
		else if (c == 'w')
		{
			if (m_bDumping || CObUsb::CaptureActive ())
			{
				L->Write (From, LogWarning, "capture: busy, 'w' ignored");
			}
			else if (!CObUsb::CaptureStart (m_bSelfTest))
			{
				L->Write (From, LogError, "capture: no Overbridge stream and no self-test (or no buffer)");
				L->Write (From, LogError, "WAV-ERROR no-stream");
			}
			else
			{
				m_nCaptureStart = CTimer::Get ()->GetTicks ();
				L->Write (From, LogNotice, "capture: started, %u s at %u Hz", CObUsb::CaptureSecs, CObUsb::CaptureRate);
			}
		}
	}
	if (CObUsb::CaptureActive () && CTimer::Get ()->GetTicks () - m_nCaptureStart >= CaptureTimeoutSecs * HZ)
	{
		CObUsb::CaptureStop ();
		L->Write (From, LogError, "capture: timed out at %u of %u frames (stream stopped?)", CObUsb::CaptureFramesDone (),
			  CObUsb::CaptureFrames);
		L->Write (From, LogError, "WAV-ERROR timeout");
	}
	if (CObUsb::CaptureFull () && !m_bDumping)
	{
		CObUsb::CaptureClearFull ();
		L->Write (From, LogNotice, "capture: done, %u gaps (%u blocks lost); dumping", CObUsb::CaptureGaps (),
			  CObUsb::CaptureLost ());
		StartDump ();
	}
	if (m_bDumping && DumpStep ())
	{
		g_bQuietLog = FALSE;
		L->Write (From, LogNotice, "capture: dump sent (%u core log lines held back)", g_nQuietLogDropped);
	}
}

// One command line (without the ':'): core 0's own (wdtest, link=, selftest=, the Sidecar's) here, the rest queued for
// the control core (cores.cpp). Core 0 only: the serial console and the web page (web.cpp, a task on core 0).
void CConsole::Command (const char *pText)
{
	CLogger *L = CLogger::Get ();
	if (CWifi::Get () != 0 && CWifi::Get ()->Command (pText)) return;	// core 0's (wifi.h): before the length check, join is long
	if (rec_pi::Command (pText)) return;				// core 0's (rec_pi.h): rec=ch: lists are long too
	if (stor_pi::Command (pText)) return;				// core 0's (stor_pi.h): the volumes, the bench
	if (bundle_pi::command (pText)) return;				// core 0's (bundle_pi.h): bundle=... (the PROJECT pages)
	if (paircode_pi::Command (pText)) return;			// core 0's (paircode_pi.h): pair, pair=show, pair=revoke
	if (midibridge_pi::Command (pText)) return;			// core 0's (midibridge_pi.h): midibridge, midibridge=on|off|os:on|os:off
	if (lib_pi::command (pText)) return;				// core 0's (lib_pi.h): lib=status|rescan|verify|bench (srclib's browse= / lib=changed:, pool= below)
	if (strlen (pText) >= sizeof (TCmd::text))
	{
		L->Write (From, LogWarning, "command too long (max %u characters): dropped", (unsigned) sizeof (TCmd::text) - 1);
		return;
	}
	TCmd Cmd;
	strcpy (Cmd.text, pText);
	if (strcmp (Cmd.text, "sdf") == 0)			// the SD file commands on the serial port (sdfiles.h)
	{
		CSdFiles::RequestSerial ();
		return;
	}
	if (strcmp (Cmd.text, "version") == 0)			// core 0's: the Sidecar's version and commit (docs/versioning.md)
	{
		L->Write (From, LogNotice, "version: Sidecar " SIDECAR_VERSION_FULL);
		return;
	}
	if (strcmp (Cmd.text, "wdtest") == 0)			// core 0's (kernel.cpp)
	{
		m_bWatchdogTest = TRUE;
		return;
	}
	if (strncmp (Cmd.text, "link=", 5) == 0)		// core 0's (kernel.cpp)
	{
		strncpy (m_Link, Cmd.text + 5, sizeof m_Link - 1);
		m_Link[sizeof m_Link - 1] = 0;
		m_bLink = TRUE;
		return;
	}
	if (strncmp (Cmd.text, "selftest=", 9) == 0)		// core 0's (kernel.cpp)
	{
		const char *v = Cmd.text + 9;
		m_nSelfTest = strcmp (v, "on") == 0 || strcmp (v, "1") == 0 ? 1
			    : strcmp (v, "off") == 0 || strcmp (v, "0") == 0 ? 0 : -1;
		if (m_nSelfTest < 0) L->Write (From, LogWarning, "selftest=on|off, not \"%s\"", v);
		return;
	}
	if (m_pCore0 != 0 && (*m_pCore0) (Cmd.text, m_pCore0Param)) return;	// core 0's (kernel.cpp)
	if (!g_Cmds.push (Cmd)) L->Write (From, LogWarning, "command queue full: \"%s\" dropped", Cmd.text);
	asm volatile ("dsb sy; sev");
}

boolean CConsole::TakeLink (char *pOut, unsigned nSize)
{
	if (!m_bLink) return FALSE;
	strncpy (pOut, m_Link, nSize - 1);
	pOut[nSize - 1] = 0;
	m_bLink = FALSE;
	return TRUE;
}

void CConsole::StartDump (void)
{
	m_pDump = CObUsb::CaptureData ();
	m_nDumpLen = CObUsb::CaptureBytes;
	m_nDumpPos = 0;
	m_nCrc = 0;
	m_bDumpEnd = FALSE;
	m_bDumping = TRUE;
	g_nQuietLogDropped = 0;
	g_bQuietLog = TRUE;
	asm volatile ("dmb ish");
	CString Line;
	Line.Format ("WAV-BEGIN %u %u %u %u\n", m_nDumpLen, CObUsb::CaptureRate, CObUsb::CaptureChannels,
		     CObUsb::CaptureGaps ());
	m_nLineLen = Line.GetLength ();
	memcpy (m_Line, (const char *) Line, m_nLineLen);
	m_nLineOff = 0;
}

// Whole lines into the serial TX buffer until it is full (or the dump ends); a line is always finished before the
// loop logs anything, so log lines fall between lines. It returns once a line had to wait for room: the TX
// interrupt drains the buffer while it waits, so "full at a line's start" alone may never come, and core 0's loop
// (the watchdog's feed) would not run again before the dump ends (measured: the watchdog reset 4.2 s into a dump).
boolean CConsole::DumpStep (void)
{
	for (;;)
	{
		boolean bWaited = FALSE;
		while (m_nLineOff < m_nLineLen)
		{
			int n = m_pSerial->Write (m_Line + m_nLineOff, m_nLineLen - m_nLineOff);
			if (n < 0) n = 0;
			m_nLineOff += n;
			if (m_nLineOff < m_nLineLen)
			{
				if (m_nLineOff == 0) return FALSE;
				CTimer::Get ()->MsDelay (1);
				bWaited = TRUE;
			}
		}
		if (bWaited) return FALSE;	// the line is whole; the next call goes on with the next one
		if (m_nDumpPos < m_nDumpLen)
		{
			unsigned n = m_nDumpLen - m_nDumpPos;
			if (n > B64LineBytes) n = B64LineBytes;
			m_nCrc = Crc32 (m_nCrc, m_pDump + m_nDumpPos, n);
			m_nLineLen = Base64 (m_Line, m_pDump + m_nDumpPos, n);
			m_Line[m_nLineLen++] = '\n';
			m_nLineOff = 0;
			m_nDumpPos += n;
		}
		else if (!m_bDumpEnd)
		{
			CString Line;
			Line.Format ("WAV-END %08x\n", m_nCrc);
			m_nLineLen = Line.GetLength ();
			memcpy (m_Line, (const char *) Line, m_nLineLen);
			m_nLineOff = 0;
			m_bDumpEnd = TRUE;
		}
		else
		{
			m_bDumping = FALSE;
			return TRUE;
		}
	}
}
