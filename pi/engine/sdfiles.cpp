// pi/engine/sdfiles.cpp - the SD card's boot files over TCP (sdfiles.h; docs/pi-engine.md, "Boot from the SD card").
#include "sdfiles.h"
#include "bridge.h"
#include "paircode_pi.h"
#include "lib/library.h"

#include <circle/bcmpropertytags.h>
#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/sched/scheduler.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include <stdarg.h>

static const char From[] = "sdfiles";
static const unsigned BufSize = 16 * 1024;
static const unsigned ScratchSize = 64 * 1024;

volatile int CSdFiles::s_nReboot = CSdFiles::RebootNone;

// CRC-32 (IEEE, zlib's crc32 and Python's binascii.crc32)
static u32 s_CrcTable[256];
static void CrcInit (void)
{
	for (u32 n = 0; n < 256; n++)
	{
		u32 c = n;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
		s_CrcTable[n] = c;
	}
}
static u32 Crc (u32 crc, const u8 *p, unsigned n)
{
	crc = ~crc;
	while (n--) crc = s_CrcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static boolean NameOK (const char *p)
{
	unsigned n = 0;
	for (; *p; p++, n++)
	{
		char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
			return FALSE;
	}
	return n > 0 && n <= 64;
}

static boolean EndsWith (const char *p, const char *s)
{
	size_t a = strlen (p), b = strlen (s);
	return a >= b && strcmp (p + a - b, s) == 0;
}

static void Path (char *pOut, const char *pName, const char *pSuffix = "")
{
	strcpy (pOut, "SD:/");
	strcat (pOut, pName);
	strcat (pOut, pSuffix);
}

static boolean Exists (const char *pPath)
{
	FILINFO fi;
	return f_stat (pPath, &fi) == FR_OK;
}

// The file's size and CRC-32 (FALSE: unreadable)
static boolean FileCrc (const char *pPath, u8 *pBuf, unsigned *pSize, u32 *pCrc)
{
	FIL f;
	if (f_open (&f, pPath, FA_READ) != FR_OK) return FALSE;
	u32 crc = 0;
	unsigned nTotal = 0;
	for (;;)
	{
		UINT n = 0;
		if (f_read (&f, pBuf, BufSize, &n) != FR_OK) { f_close (&f); return FALSE; }
		if (n == 0) break;
		crc = Crc (crc, pBuf, n);
		nTotal += n;
		CScheduler::Get ()->Yield ();
	}
	f_close (&f);
	*pSize = nTotal;
	*pCrc = crc;
	return TRUE;
}

CSdFiles::CSdFiles (CNetSubSystem *pNet, CSerialDevice *pSerial)
:	m_pNet (pNet), m_pSerial (pSerial), m_pConn (0), m_pBuf (new u8[BufSize]), m_nRxPos (0), m_nRxLen (0),
	m_nDecPos (0), m_nDecLen (0), m_bAuth (FALSE), m_bToldUnpaired (FALSE), m_nPeer (0), m_nAcceptAt (0)
{
	SetName (pSerial ? "sdfiles-serial" : "sdfiles");
	CrcInit ();
}

volatile boolean CSdFiles::s_bSerial = FALSE;

void CSdFiles::RequestSerial (void)
{
	s_bSerial = TRUE;		// the console stops reading the serial port now
}

// base64 (RFC 4648, with padding)
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static unsigned B64Encode (const u8 *p, unsigned n, char *pOut)
{
	char *q = pOut;
	for (unsigned i = 0; i < n; i += 3)
	{
		u32 v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		*q++ = B64[v >> 18 & 63];
		*q++ = B64[v >> 12 & 63];
		*q++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
		*q++ = i + 2 < n ? B64[v & 63] : '=';
	}
	*q = 0;
	return q - pOut;
}
static int B64Val (char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
static int B64Decode (const char *p, u8 *pOut, unsigned nMax)	// -1: malformed
{
	unsigned n = 0;
	u32 v = 0;
	int k = 0;
	for (; *p && *p != '='; p++)
	{
		int d = B64Val (*p);
		if (d < 0) return -1;
		v = v << 6 | d;
		if (++k == 4)
		{
			if (n + 3 > nMax) return -1;
			pOut[n++] = v >> 16; pOut[n++] = v >> 8; pOut[n++] = v;
			v = 0; k = 0;
		}
	}
	if (k == 1) return -1;
	if (n + (k ? k - 1 : 0) > nMax) return -1;
	if (k == 2) pOut[n++] = v >> 4;
	if (k == 3) { pOut[n++] = v >> 10; pOut[n++] = v >> 2; }
	return n;
}

CSdFiles::TReboot CSdFiles::TakeReboot (void)
{
	int n = s_nReboot;
	s_nReboot = RebootNone;
	return (TReboot) n;
}

boolean CSdFiles::SetTryboot (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagSimple Tag;
	Tag.nValue = 1;					// the reboot flags: bit 0 tryboot
	return Tags.GetTag (0x00038064, &Tag, sizeof Tag, 4);	// SET_REBOOT_FLAGS (Linux: RPI_FIRMWARE_SET_REBOOT_FLAGS)
}

void CSdFiles::Run (void)
{
	CScheduler *S = CScheduler::Get ();
	if (m_pSerial)				// the console's ":sdf": the same commands on the serial port
	{
		for (;;)
		{
			while (!s_bSerial) S->MsSleep (50);
			m_nRxPos = m_nRxLen = m_nDecPos = m_nDecLen = 0;
			m_bAuth = TRUE;		// the serial port: physical access, and the way back in when the network is locked
			Reply ("READY (the SD file commands; BYE or 10 s idle returns to the console)");
			Serve ();
			Reply ("BYE");
			s_bSerial = FALSE;
		}
	}
	while (!m_pNet->IsRunning ()) S->MsSleep (100);
	CSocket Listen (m_pNet, IPPROTO_TCP);
	if (Listen.Bind (Port) < 0 || Listen.Listen (1) < 0)
	{
		CLogger::Get ()->Write (From, LogError, "cannot listen on port %u", Port);
		return;
	}
	CLogger::Get ()->Write (From, LogNotice, "the SD card's boot files on TCP port %u (pi/circle/sdboot.py)%s", Port,
				paircode_pi::Gate ().gated () ? ": a connection must PAIR with the code on the screen first"
							      : ": NOT GATED (cmdline paircode=0) - anyone on this network can replace the firmware");
	for (;;)
	{
		CIPAddress IP;
		u16 nPort;
		m_pConn = Listen.Accept (&IP, &nPort);
		if (m_pConn == 0) { S->MsSleep (100); continue; }
		m_nRxPos = m_nRxLen = 0;
		m_bAuth = !paircode_pi::Gate ().gated ();	// the pairing code: nothing is served until PAIR / AUTH
		m_nPeer = (u32) IP;				// its rate-limit key (paircode.h: per peer)
		// One connection at a time, so an idle one held the port - and every install - for good (review M2:
		// `nc sidecar.local 7310` and walk away). 10 s of silence before PAIR / AUTH ends it, 60 s after.
		m_pConn->SetOptionReceiveTimeout (m_bAuth ? kAuthedTimeoutUs : kUnauthTimeoutUs);
		m_nAcceptAt = CTimer::GetClockTicks ();	// and 20 s in all to authenticate (a byte every 9 s is no way round it)
		m_bToldUnpaired = FALSE;
		Serve ();
		delete m_pConn;
		m_pConn = 0;
	}
}

// Fills m_Rx (FALSE: the connection closed, or 10 s without a byte on the serial port)
boolean CSdFiles::Fill (void)
{
	m_nRxPos = 0;
	m_nRxLen = 0;
	if (!m_pSerial)
	{
		if (!m_bAuth && CTimer::GetClockTicks () - m_nAcceptAt > kUnauthMaxUs) return FALSE;
		int n = m_pConn->Receive (m_Rx, sizeof m_Rx, 0);
		if (n <= 0) return FALSE;
		m_nRxLen = n;
		return TRUE;
	}
	for (unsigned t = 0; t < 10000; t++)
	{
		int n = m_pSerial->Read (m_Rx, sizeof m_Rx);
		if (n > 0) { m_nRxLen = n; return TRUE; }
		CScheduler::Get ()->MsSleep (1);
	}
	return FALSE;
}

// Sends all of it (the serial port's ring drops what does not fit: the rest goes after a pause)
void CSdFiles::SendRaw (const void *pData, unsigned nLen)
{
	if (!m_pSerial) { m_pConn->Send (pData, nLen, 0); return; }
	const u8 *p = (const u8 *) pData;
	for (unsigned nTry = 0; nLen && nTry < 2000; nTry++)
	{
		int n = m_pSerial->Write (p, nLen);
		if (n > 0) { p += n; nLen -= n; }
		if (nLen) CScheduler::Get ()->MsSleep (1);
	}
}
int CSdFiles::RecvByte (void)
{
	if (m_nRxPos == m_nRxLen && !Fill ()) return -1;
	return m_Rx[m_nRxPos++];
}
boolean CSdFiles::ReadLine (char *pLine, unsigned nSize)
{
	unsigned n = 0;
	for (;;)
	{
		int c = RecvByte ();
		if (c < 0) return FALSE;
		if (c == '\n') break;
		if (c == '\r') continue;
		if (n + 1 < nSize) pLine[n++] = (char) c;
	}
	pLine[n] = 0;
	return TRUE;
}

boolean CSdFiles::RecvExact (u8 *pBuf, unsigned nLen)
{
	while (nLen)
	{
		const u8 *pSrc;
		unsigned k;
		if (m_pSerial)				// "D <base64>" lines, each answered "SDF K" once taken
		{
			if (m_nDecPos == m_nDecLen)
			{
				if (!ReadLine (m_Line, sizeof m_Line) || m_Line[0] != 'D' || m_Line[1] != ' ') return FALSE;
				int n = B64Decode (m_Line + 2, m_Dec, sizeof m_Dec);
				if (n <= 0) return FALSE;
				m_nDecPos = 0;
				m_nDecLen = n;
				Reply ("K");
			}
			pSrc = m_Dec + m_nDecPos;
			k = m_nDecLen - m_nDecPos;
			if (k > nLen) k = nLen;
			m_nDecPos += k;
		}
		else
		{
			if (m_nRxPos == m_nRxLen && !Fill ()) return FALSE;
			pSrc = m_Rx + m_nRxPos;
			k = m_nRxLen - m_nRxPos;
			if (k > nLen) k = nLen;
			m_nRxPos += k;
		}
		memcpy (pBuf, pSrc, k);
		pBuf += k;
		nLen -= k;
	}
	return TRUE;
}
void CSdFiles::Reply (const char *pFormat, ...)
{
	va_list Args;
	va_start (Args, pFormat);
	CString s;
	s.FormatV (pFormat, Args);
	va_end (Args);
	CString Line (m_pSerial ? "SDF " : "");	// on the serial port among the log lines
	Line.Append (s);
	Line.Append ("\n");
	SendRaw ((const char *) Line, Line.GetLength ());
}

void CSdFiles::Serve (void)
{
	char Line[160];
	while (ReadLine (Line, sizeof Line))
	{
		// the Rytm's SIDECAR MENU > WEB UI off: the network side refuses every line (as unauthenticated, so
		// sdboot.py's probe takes the gated path and says why); the serial :sdf is physical access and stays
		if (m_pSerial == 0 && !paircode_pi::WebOn ())
		{
			if (strcmp (Line, "BYE") == 0) return;
			memset (Line, 0, sizeof Line);
			Reply ("ERR unauthenticated: %s", paircode_pi::kWebOff);
			continue;
		}
		if (!m_bAuth)		// the pairing code (paircode.h): PAIR, AUTH or BYE, nothing else
		{
			if (strcmp (Line, "BYE") == 0) return;
			if (Line[0] == 0) continue;
			char R[400];
			boolean bPair = strncmp (Line, "PAIR ", 5) == 0;
			m_bAuth = paircode_pi::Gate ().tcp_auth (Line, R, sizeof R, m_nPeer) ? TRUE : FALSE;
			memset (Line, 0, sizeof Line);		// a PAIR line carries the code: not left on the stack
			Reply ("%s", R);
			if (m_bAuth)
			{
				m_pConn->SetOptionReceiveTimeout (kAuthedTimeoutUs);	// (Serve is TCP only before auth)
				paircode_pi::Save ();		// a PAIR remembers this client on the card
				if (bPair) paircode_pi::Hide ();	// paired with the code: off the Rytm's screen
				CLogger::Get ()->Write (From, LogNotice, "a client on port %u is paired: the SD file commands "
							"are open to it for this connection", Port);
			}
			// one line a connection, so a client hammering the port cannot push the log's tail out
			else if (!m_bToldUnpaired)
			{
				m_bToldUnpaired = TRUE;
				CLogger::Get ()->Write (From, LogWarning, "a client on port %u is not paired: %s", Port, R);
			}
			memset (R, 0, sizeof R);		// a PAIR answer carries the new client's secret
			continue;
		}
		char *pArg = strchr (Line, ' ');
		if (pArg) *pArg++ = 0;
		if (strcmp (Line, "LS") == 0) List ();
		else if (strcmp (Line, "GET") == 0 && pArg && NameOK (pArg)) Get (pArg);
		else if (strcmp (Line, "PUT") == 0 && pArg)
		{
			char *pSize = strchr (pArg, ' ');
			char *pCrc = pSize ? strchr (pSize + 1, ' ') : 0;
			if (!pCrc) { Reply ("ERR PUT <name> <size> <crc32>"); return; }
			*pSize++ = 0;
			*pCrc++ = 0;
			if (!NameOK (pArg)) { Reply ("ERR name"); return; }
			Put (pArg, strtoul (pSize, 0, 10), strtoul (pCrc, 0, 16));
		}
		else if (strcmp (Line, "SCRATCH") == 0) Scratch ();
		else if (strcmp (Line, "DEL") == 0 && pArg && NameOK (pArg))
		{
			if (!(strncmp (pArg, "tryboot.txt", 11) == 0 || strncmp (pArg, "engine.img", 10) == 0
			      || strncmp (pArg, "sdboot", 6) == 0 || EndsWith (pArg, ".tmp") || EndsWith (pArg, ".new")))
				{ Reply ("ERR DEL: only engine.img*, sdboot*, tryboot.txt*, *.tmp, *.new"); continue; }
			char P[80];
			Path (P, pArg);
			FRESULT r = f_unlink (P);
			if (r == FR_OK) CLogger::Get ()->Write (From, LogNotice, "deleted %s", pArg);
			Reply (r == FR_OK || r == FR_NO_FILE ? "OK" : "ERR unlink %d", r);
		}
		else if (strcmp (Line, "BYE") == 0) return;
		else if (strcmp (Line, "REBOOT") == 0 || strcmp (Line, "TRYBOOT") == 0)
		{
			boolean bTry = Line[0] == 'T';
			CLogger::Get ()->Write (From, LogNotice, "%s asked for", bTry ? "tryboot" : "reboot");
			Reply ("OK");
			CScheduler::Get ()->MsSleep (100);	// the reply out before the stop
			s_nReboot = bTry ? RebootTry : RebootPlain;
			return;
		}
		else if (Line[0] == 0) continue;
		else { Reply ("ERR unknown command"); if (!m_pSerial) return; }
	}
}

void CSdFiles::List (void)
{
	DIR d;
	FILINFO fi;
	if (f_opendir (&d, "SD:/") != FR_OK) { Reply ("ERR opendir"); return; }
	CString All;
	unsigned n = 0;
	while (f_readdir (&d, &fi) == FR_OK && fi.fname[0] != 0)
	{
		CString One;
		One.Format ("%s%u %s%s\n", m_pSerial ? "SDF " : "", (unsigned) fi.fsize, fi.fname, fi.fattrib & AM_DIR ? "/" : "");
		All.Append (One);
		n++;
	}
	f_closedir (&d);
	Reply ("OK %u", n);
	SendRaw ((const char *) All, All.GetLength ());
}

void CSdFiles::Get (const char *pName)
{
	char P[80];
	Path (P, pName);
	unsigned nSize;
	u32 nCrc;
	if (!FileCrc (P, m_pBuf, &nSize, &nCrc)) { Reply ("ERR no such file"); return; }
	FIL f;
	if (f_open (&f, P, FA_READ) != FR_OK) { Reply ("ERR open"); return; }
	Reply ("OK %u %08x", nSize, nCrc);
	for (;;)
	{
		UINT n = 0;
		if (f_read (&f, m_pBuf, m_pSerial ? 96 : 1400, &n) != FR_OK || n == 0) break;
		if (!m_pSerial) { if (m_pConn->Send (m_pBuf, n, 0) < 0) break; continue; }
		char b[140] = "SDF D ";				// short lines, paced: whole lines among the log's
		unsigned k = 6 + B64Encode (m_pBuf, n, b + 6);
		b[k++] = '\n';
		SendRaw (b, k);
		CScheduler::Get ()->MsSleep (2);
	}
	f_close (&f);
}

void CSdFiles::Put (const char *pName, unsigned nSize, u32 nCRC)
{
	char PNew[80], P[80], PBak[80], PPrev[80];
	Path (PNew, pName, ".new");
	Path (P, pName);
	Path (PBak, pName, ".bak");
	Path (PPrev, pName, ".prev");
	CLogger *L = CLogger::Get ();
	if (nSize > 32 * 1024 * 1024) { Reply ("ERR size"); return; }
	FIL f;
	FRESULT r = f_open (&f, PNew, FA_WRITE | FA_CREATE_ALWAYS);
	if (r != FR_OK) { Reply ("ERR create %d", r); return; }
	u32 crc = 0;
	unsigned nLeft = nSize;
	boolean bOK = TRUE;
	while (nLeft && bOK)
	{
		unsigned k = nLeft < BufSize ? nLeft : BufSize;
		if (!RecvExact (m_pBuf, k)) { bOK = FALSE; break; }
		crc = Crc (crc, m_pBuf, k);
		for (unsigned t0 = CTimer::GetClockTicks (); bridge::card_busy () && CTimer::GetClockTicks () - t0 < 200000; )
			CScheduler::Get ()->MsSleep (1);	// uploads last: the streams first (core/msamp/cardq.h)
		UINT w = 0;
		if (f_write (&f, m_pBuf, k, &w) != FR_OK || w != k) bOK = FALSE;
		nLeft -= k;
	}
	if (f_close (&f) != FR_OK) bOK = FALSE;
	if (!bOK || crc != nCRC)
	{
		f_unlink (PNew);
		L->Write (From, LogWarning, "%s: %s, %s left as it was", pName, bOK ? "CRC mismatch" : "receive/write failed", pName);
		Reply ("ERR %s", bOK ? "crc" : "receive/write");
		return;
	}
	unsigned nBack;
	u32 nBackCrc;
	if (!FileCrc (PNew, m_pBuf, &nBack, &nBackCrc) || nBack != nSize || nBackCrc != nCRC)
	{
		f_unlink (PNew);
		L->Write (From, LogWarning, "%s: the read-back differs, %s left as it was", pName, pName);
		Reply ("ERR readback");
		return;
	}
	const char *pKept = "";
	if (strcmp (pName, "tryboot.txt") == 0) f_unlink (P);	// sdboot.py writes it fresh: nothing to keep
	if (Exists (P))
	{
		// the user's own files keep their original once (FatFs: the new name has no drive)
		boolean bOriginal = strcmp (pName, "config.txt") == 0 || strcmp (pName, "cmdline.txt") == 0;
		if (bOriginal && !Exists (PBak)) { r = f_rename (P, PBak + 3); pKept = ".bak"; }
		else { f_unlink (PPrev); r = f_rename (P, PPrev + 3); pKept = ".prev"; }
		if (r != FR_OK) { f_unlink (PNew); Reply ("ERR keep %d", r); return; }
	}
	r = f_rename (PNew, P + 3);
	if (r != FR_OK) { Reply ("ERR rename %d (the new file is %s.new)", r, pName); return; }
	core::lib::touched (P + 3, 0, 0);	// the sample library (a library type only: the boot files are not)
	L->Write (From, LogNotice, "%s: %u bytes, crc %08x, written and checked%s%s%s", pName, nSize, nCRC,
		  *pKept ? " (the old one: " : "", *pKept ? pName : "", *pKept ? (pKept[1] == 'b' ? ".bak)" : ".prev)") : "");
	Reply ("OK %u %08x%s%s", nSize, nCRC, *pKept ? " kept " : "", pKept);
}

void CSdFiles::Scratch (void)
{
	static const char P[] = "SD:/sdtest.tmp";
	FIL f;
	FRESULT r = f_open (&f, P, FA_WRITE | FA_CREATE_ALWAYS);
	if (r != FR_OK) { Reply ("ERR create %d", r); return; }
	u32 crc = 0;
	boolean bOK = TRUE;
	for (unsigned nDone = 0; nDone < ScratchSize && bOK; nDone += BufSize)
	{
		for (unsigned i = 0; i < BufSize; i++) m_pBuf[i] = (u8) ((nDone + i) * 7 + (nDone + i) / 251);
		crc = Crc (crc, m_pBuf, BufSize);
		UINT w = 0;
		if (f_write (&f, m_pBuf, BufSize, &w) != FR_OK || w != BufSize) bOK = FALSE;
	}
	if (f_close (&f) != FR_OK) bOK = FALSE;
	unsigned nBack = 0;
	u32 nBackCrc = 0;
	boolean bBack = bOK && FileCrc (P, m_pBuf, &nBack, &nBackCrc);
	r = f_unlink (P);
	boolean bPass = bBack && nBack == ScratchSize && nBackCrc == crc && r == FR_OK;
	CLogger::Get ()->Write (From, LogNotice, "scratch: %u bytes written, read back %u crc %08x/%08x, deleted %d: %s",
				ScratchSize, nBack, crc, nBackCrc, r, bPass ? "PASS" : "FAIL");
	Reply (bPass ? "OK %u %08x" : "ERR scratch %u %08x", nBack, nBackCrc);
}
