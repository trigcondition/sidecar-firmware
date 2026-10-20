// pi/engine/web.cpp - see web.h.
#include "web.h"
#include "wifi.h"

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/mdnsdaemon.h>
#include <circle/net/mdnspublisher.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/util.h>

#include <fatfs/ff.h>

#include "console.h"
#include "fsweb.h"
#include "wifi_conf.h"
#include "hosted/web_page.h"	// build.sh: www/index.html as WebPage[] (a raw string)
#include "hosted/web_page_gz.h"	// build.sh: the same, gzip -9n, as WebPageGz[] (what "/" sends)
#include "user/lists.h"
#include "user/um.h"

static const char From[] = "web";

// A line that names an SSID (wpa_supplicant's own, at the debug level) has the name blanked, on the serial port too
// (wifi_conf.h's redact_ssids): no network's name in the log
static boolean MaySsid (const char *p, size_t n)
{
	for (size_t i = 0; i + 4 <= n; i++)
		if ((p[i] | 0x20) == 's' && (p[i + 1] | 0x20) == 's' && (p[i + 2] | 0x20) == 'i' && (p[i + 3] | 0x20) == 'd') return TRUE;
	return FALSE;
}

int CLogTee::Write (const void *pBuffer, size_t nCount)
{
	char Line[512];
	size_t nAsked = nCount;
	if (MaySsid ((const char *) pBuffer, nCount))
	{
		size_t n = nCount < sizeof Line ? nCount : sizeof Line - 1;	// longer: cut (one log line is shorter)
		memcpy (Line, pBuffer, n);
		nCount = wificonf::redact_ssids (Line, n);
		pBuffer = Line;
	}
	int nResult = m_pTarget->Write (pBuffer, nCount);
	if (nResult == (int) nCount) nResult = (int) nAsked;
	const char *p = (const char *) pBuffer;
	m_Lock.Acquire ();
	u64 n = m_nTotal;
	for (size_t i = 0; i < nCount; i++)
	{
		if (p[i] == '\r') continue;
		m_Ring[n++ & (Size - 1)] = p[i];
	}
	m_nTotal = n;
	m_Lock.Release ();
	return nResult;
}

unsigned CLogTee::ReadSince (u64 nSince, char *pOut, unsigned nMax, u64 *pStart, u64 *pEnd)
{
	m_Lock.Acquire ();
	u64 nEnd = m_nTotal;
	u64 nOldest = nEnd > Size ? nEnd - Size : 0;
	if (nSince < nOldest || nSince > nEnd) nSince = nOldest;
	if (nEnd - nSince > nMax) nSince = nEnd - nMax;		// the newest nMax bytes
	unsigned n = 0;
	for (u64 i = nSince; i < nEnd; i++) pOut[n++] = m_Ring[i & (Size - 1)];
	m_Lock.Release ();
	*pStart = nSince;
	*pEnd = nEnd;
	return n;
}

unsigned CWebServer::s_nWorkers = 0, CWebServer::s_nRefused = 0, CWebServer::s_nIdle = 0, CWebServer::s_nMaxLifeMs = 0;
unsigned CWebServer::s_nRequests, CWebServer::s_nMaxUs, CWebServer::s_nCommands;
CWebServer *CWebServer::s_pWorker[MaxWorkers];

CWebServer::CWebServer (CNetSubSystem *pNet, CLogTee *pLog, CConsole *pConsole, TWebStatus *pStatus, void *pParam,
			CSocket *pSocket)
:	CHTTPDaemon (pNet, pSocket, MaxContent, 80, MaxMultipart, Timeout),	// a request that stalls Timeout s is dropped
	m_pNet (pNet), m_pLog (pLog), m_pConsole (pConsole), m_pStatus (pStatus), m_pParam (pParam), m_bWorker (pSocket != 0),
	m_nStart (CTimer::GetClockTicks ()), m_bServed (FALSE)
{
	if (!m_bWorker) return;
	s_nWorkers++;
	for (unsigned i = 0; i < MaxWorkers; i++) if (s_pWorker[i] == 0) { s_pWorker[i] = this; break; }
}

CWebServer::~CWebServer (void)
{
	if (!m_bWorker) return;
	s_nWorkers--;
	for (unsigned i = 0; i < MaxWorkers; i++) if (s_pWorker[i] == this) s_pWorker[i] = 0;
	unsigned ms = (CTimer::GetClockTicks () - m_nStart) / 1000;
	if (ms > s_nMaxLifeMs) s_nMaxLifeMs = ms;
	if (!m_bServed) s_nIdle++;		// ended in ParseRequest: Circle's "Receive failed", or closed before a request
}

void CWebServer::LogStats (CLogger *pLogger)
{
	static unsigned s_nReq, s_nRef, s_nIdl;
	if (s_nRequests == s_nReq && s_nRefused == s_nRef && s_nIdle == s_nIdl) return;
	pLogger->Write ("web", LogNotice, "web: %u requests, %u refused (503), %u closed without a request, longest connection %u ms, "
			"%u open (in the last minute: +%u, +%u, +%u)", s_nRequests, s_nRefused, s_nIdle, s_nMaxLifeMs, s_nWorkers,
			s_nRequests - s_nReq, s_nRefused - s_nRef, s_nIdle - s_nIdl);
	s_nReq = s_nRequests;
	s_nRef = s_nRefused;
	s_nIdl = s_nIdle;
	s_nMaxLifeMs = 0;
}

// At most MaxWorkers connections at once (each is one of Circle's tasks): with Wi-Fi's tasks, Circle's own limit of 10
// clients passed MAX_TASKS (20) and panicked ("System limit of tasks exceeded") when a browser's requests, piled up
// while the Pi was away, all arrived at once (measured 2026-10-17, 12 s after a boot). One more gets a fixed 503 with
// Retry-After (one non-blocking send, no task) and is closed: the page retries it (www/index.html, fsRetry).
CHTTPDaemon *CWebServer::CreateWorker (CNetSubSystem *pNet, CSocket *pSocket)
{
	if (s_nWorkers >= MaxWorkers)
	{
		static const char k503[] = "HTTP/1.0 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Type: text/plain\r\n"
					   "Content-Length: 9\r\nConnection: close\r\n\r\n503 busy\n";
		s_nRefused++;
		static unsigned s_nWarnAt = 0;		// the busy line at most every 10 s: the oldest connection's age
		unsigned now = CTimer::GetClockTicks (), nOldest = 0;
		for (unsigned i = 0; i < MaxWorkers; i++)
			if (s_pWorker[i] != 0 && now - s_pWorker[i]->m_nStart > nOldest) nOldest = now - s_pWorker[i]->m_nStart;
		if (s_nWarnAt == 0 || now - s_nWarnAt > 10000000)
		{
			s_nWarnAt = now | 1;
			CLogger::Get ()->Write (From, LogWarning, "busy: %u connections open (the oldest %u ms): 503 (%u so far)",
						s_nWorkers, nOldest / 1000, s_nRefused);
		}
		pSocket->Send (k503, sizeof k503 - 1, MSG_DONTWAIT);
		delete pSocket;
		return 0;
	}
	return new CWebServer (pNet, m_pLog, m_pConsole, m_pStatus, m_pParam, pSocket);
}

static u64 ParseU64 (const char *p)
{
	u64 n = 0;
	while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
	return n;
}

static int Hex (char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// "a=1&c=notes%3D1" -> the value of pName, URL-decoded (+ is a space); FALSE if absent or too long
static boolean FormValue (const char *pForm, const char *pName, char *pOut, unsigned nSize)
{
	unsigned nName = strlen (pName);
	for (const char *p = pForm; *p; )
	{
		if (strncmp (p, pName, nName) == 0 && p[nName] == '=')
		{
			p += nName + 1;
			unsigned n = 0;
			while (*p && *p != '&')
			{
				char c = *p++;
				if (c == '+') c = ' ';
				else if (c == '%' && Hex (p[0]) >= 0 && Hex (p[1]) >= 0)
				{
					c = (char) (Hex (p[0]) << 4 | Hex (p[1]));
					p += 2;
				}
				if (n + 1 >= nSize) return FALSE;
				pOut[n++] = c;
			}
			pOut[n] = 0;
			return TRUE;
		}
		while (*p && *p != '&') p++;
		if (*p == '&') p++;
	}
	return FALSE;
}

// ---- the file browser's card (fsweb.h's Backend) on FatFs: "SD:" + the path. One upload's .part stays open between
// its 4 KB pieces (closed before anything else touches a file); everything here runs in core 0's web tasks.
static CConsole *s_pConsole = 0;
static FIL s_File;
static char s_FilePath[fsweb::kPath + 8] = "";
static boolean s_bFileOpen = FALSE;
static void SdPath (char *pOut, const char *p) { strcpy (pOut, "SD:"); strcat (pOut, p); }
static void FileClose (void)
{
	if (s_bFileOpen) f_close (&s_File);
	s_bFileOpen = FALSE;
	s_FilePath[0] = 0;
}
static boolean FileOpen (const char *p, boolean bCreate)
{
	if (s_bFileOpen && !bCreate && strcmp (s_FilePath, p) == 0) return TRUE;
	FileClose ();
	char sd[fsweb::kPath + 8];
	SdPath (sd, p);
	if (f_open (&s_File, sd, FA_READ | FA_WRITE | (bCreate ? FA_CREATE_ALWAYS : FA_OPEN_EXISTING)) != FR_OK) return FALSE;
	strcpy (s_FilePath, p);
	s_bFileOpen = TRUE;
	return TRUE;
}
static bool BStat (const char *p, u64 *pSize, bool *pDir)
{
	FileClose ();
	if (strcmp (p, "/") == 0) { *pSize = 0; *pDir = true; return true; }
	char sd[fsweb::kPath + 8];
	SdPath (sd, p);
	FILINFO fi;
	if (f_stat (sd, &fi) != FR_OK) return false;
	*pSize = fi.fsize;
	*pDir = (fi.fattrib & AM_DIR) != 0;
	return true;
}
static int BList (const char *p, fsweb::Entry *pOut, int nMax, bool *pMore)
{
	FileClose ();
	char sd[fsweb::kPath + 8];
	SdPath (sd, p);
	DIR d;
	FILINFO fi;
	if (f_opendir (&d, sd) != FR_OK) return -1;
	int n = 0;
	*pMore = false;
	while (f_readdir (&d, &fi) == FR_OK && fi.fname[0])
	{
		if (n == nMax) { *pMore = true; break; }
		strncpy (pOut[n].name, fi.fname, sizeof pOut[n].name - 1);
		pOut[n].name[sizeof pOut[n].name - 1] = 0;
		pOut[n].size = fi.fsize;
		pOut[n].dir = (fi.fattrib & AM_DIR) != 0;
		n++;
	}
	f_closedir (&d);
	return n;
}
static bool BRead (const char *p, u64 nOff, void *pBuf, u32 n, u32 *pGot)
{
	*pGot = 0;
	if (s_bFileOpen && strcmp (s_FilePath, p) == 0)		// the upload's own file (its read-back)
	{
		UINT got = 0;
		if (f_sync (&s_File) != FR_OK || f_lseek (&s_File, nOff) != FR_OK || f_read (&s_File, pBuf, n, &got) != FR_OK) return false;
		*pGot = got;
		return true;
	}
	FileClose ();
	char sd[fsweb::kPath + 8];
	SdPath (sd, p);
	FIL f;
	if (f_open (&f, sd, FA_READ) != FR_OK) return false;
	UINT got = 0;
	bool bOK = f_lseek (&f, nOff) == FR_OK && f_read (&f, pBuf, n, &got) == FR_OK;
	f_close (&f);
	*pGot = got;
	return bOK;
}
static bool BWrite (const char *p, u64 nOff, const void *pBuf, u32 n, bool bCreate)
{
	if (!FileOpen (p, bCreate)) return false;
	UINT w = 0;
	return f_lseek (&s_File, nOff) == FR_OK && f_write (&s_File, pBuf, n, &w) == FR_OK && w == n;
}
static bool BRemove (const char *p) { FileClose (); char sd[fsweb::kPath + 8]; SdPath (sd, p); return f_unlink (sd) == FR_OK; }
static bool BRename (const char *a, const char *b)
{
	FileClose ();
	char sa[fsweb::kPath + 8];
	SdPath (sa, a);
	return f_rename (sa, b) == FR_OK;		// the new name without its drive (FatFs)
}
static bool BMkdir (const char *p) { FileClose (); char sd[fsweb::kPath + 8]; SdPath (sd, p); return f_mkdir (sd) == FR_OK; }
static bool BSpace (u64 *pFree, u64 *pTotal)
{
	static u64 s_nFree, s_nTotal;
	static unsigned s_nAt = 0;
	unsigned now = CTimer::Get ()->GetUptime ();
	if (s_nTotal == 0 || now - s_nAt >= 10)			// f_getfree once in 10 s (a FAT scan when FSInfo is stale)
	{
		DWORD nFree;
		FATFS *pFs;
		if (f_getfree ("SD:", &nFree, &pFs) != FR_OK) return false;
		s_nFree = (u64) nFree * pFs->csize * 512;
		s_nTotal = (u64) (pFs->n_fatent - 2) * pFs->csize * 512;
		s_nAt = now;
	}
	*pFree = s_nFree;
	*pTotal = s_nTotal;
	return true;
}
static void BYield (void) { CScheduler::Get ()->Yield (); }
static void BChanged (const char *p)
{
	if (strncasecmp (p, "/Plugins", 8) == 0 && s_pConsole) s_pConsole->Command ("um=rescan");	// the control core's
	if (strncasecmp (p, "/PRESETS", 8) == 0 && s_pConsole) s_pConsole->Command ("um=presets");	// the PRE lists again
}
static const fsweb::Backend s_Card = {BStat, BList, BRead, BWrite, BRemove, BRename, BMkdir, BSpace, BYield, BChanged};

// The Wi-Fi settings' changes come from this page only (wifi_conf.h's same_origin; pi/circle/circle-http-origin.patch
// keeps the headers): the Host one of the Pi's names (sidecar.local, its IP), the Origin (else the Referer) that host.
// bStrict FALSE (/cmd): a request with neither header (a tool such as curl) passes.
static boolean FromPage (CNetSubSystem *pNet, const char *pHost, const char *pOrigin, const char *pReferer, boolean bStrict)
{
	char Name[80];
	strncpy (Name, CWebStart::Hostname (), sizeof Name - 8);
	Name[sizeof Name - 8] = 0;
	if (Name[0]) strcat (Name, ".local");
	CString Ip;
	pNet->GetConfig ()->GetIPAddress ()->Format (&Ip);
	const char *Names[] = {Name, (const char *) Ip};
	return bStrict ? wificonf::same_origin (pHost, pOrigin, pReferer, Names, 2)
		       : wificonf::not_cross_site (pHost, pOrigin, pReferer, Names, 2);
}

// {"ok":true} or {"ok":false,"code":N,"error":"..."}: the Wi-Fi settings' answers (always HTTP 200, as Reply)
static void WifiReply (const char *pErr, unsigned nCode, u8 *pBuffer, unsigned *pLength, const char **ppContentType)
{
	CString s;
	if (pErr == 0) s = "{\"ok\":true}";
	else
	{
		char e[400];
		wificonf::json_escape (pErr, e, sizeof e);
		s.Format ("{\"ok\":false,\"code\":%u,\"error\":\"%s\"}", nCode, e);
	}
	unsigned n = s.GetLength () < *pLength ? s.GetLength () : *pLength;
	memcpy (pBuffer, (const char *) s, n);
	*pLength = n;
	*ppContentType = "application/json";
}

// "<code> <message>" as the body (always HTTP 200: Circle's error pages carry no message); the page reads the code.
static void Reply (const fsweb::Result &r, u8 *pBuffer, unsigned *pLength, const char **ppContentType)
{
	CString s;
	s.Format ("%d %s\n", r.status, r.msg);
	unsigned n = s.GetLength () < *pLength ? s.GetLength () : *pLength;
	memcpy (pBuffer, (const char *) s, n);
	*pLength = n;
	*ppContentType = "text/plain; charset=utf-8";
}

THTTPStatus CWebServer::GetContent (const char *pPath, const char *pParams, const char *pFormData, u8 *pBuffer,
				    unsigned *pLength, const char **ppContentType)
{
	unsigned t0 = CTimer::GetClockTicks ();
	THTTPStatus Status = HTTPOK;
	s_nRequests++;
	m_bServed = TRUE;
	if (strcmp (pPath, "/") == 0 || strcmp (pPath, "/index.html") == 0)
	{
		// gzip (about a quarter of the 120 KB over Wi-Fi; every browser takes it). Circle's daemon writes only
		// Content-Type, so the other headers ride in its value (each line ends in CRLF there). no-cache: a reload asks
		// again (Circle answers no 304), so a new kernel's page is never stale. ?raw: plain, for tools (curl without
		// --compressed).
		boolean bRaw = strstr (pParams, "raw") != 0;
		unsigned n = bRaw ? sizeof WebPage - 1 : sizeof WebPageGz;
		if (n > *pLength) return HTTPInternalServerError;
		memcpy (pBuffer, bRaw ? (const void *) WebPage : (const void *) WebPageGz, n);
		*pLength = n;
		*ppContentType = bRaw ? "text/html; charset=utf-8\r\nCache-Control: no-store"
				      : "text/html; charset=utf-8\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nCache-Control: no-cache";
	}
	else if (strcmp (pPath, "/status") == 0)
	{
		CString s;
		s.Format ("{\"host\":\"%s\",\"uptime\":%u,\"web\":{\"requests\":%u,\"maxus\":%u,\"commands\":%u,\"refused\":%u,"
			  "\"idle\":%u,\"open\":%u},",
			  CWebStart::Hostname (), CTimer::Get ()->GetUptime (), s_nRequests, s_nMaxUs, s_nCommands, s_nRefused,
			  s_nIdle, s_nWorkers);
		(*m_pStatus) (&s, m_pParam);
		s.Append ("}");
		unsigned n = s.GetLength ();
		if (n > *pLength) return HTTPInternalServerError;
		memcpy (pBuffer, (const char *) s, n);
		*pLength = n;
		*ppContentType = "application/json";
	}
	else if (strcmp (pPath, "/log") == 0)
	{
		// "since=N": the first line is "START END", then the log's bytes START..END (the client asks since=END next)
		const char *p = strstr (pParams, "since=");
		u64 nSince = p != 0 ? ParseU64 (p + 6) : 0;
		const unsigned Head = 48;
		u64 nStart, nEnd;
		unsigned n = m_pLog->ReadSince (nSince, (char *) pBuffer + Head, *pLength - Head, &nStart, &nEnd);
		CString h;
		h.Format ("%lu %lu\n", (unsigned long) nStart, (unsigned long) nEnd);
		unsigned nh = h.GetLength ();
		memmove (pBuffer + nh, pBuffer + Head, n);
		memcpy (pBuffer, (const char *) h, nh);
		*pLength = nh + n;
		*ppContentType = "text/plain; charset=utf-8";
	}
	else if (strcmp (pPath, "/cmd") == 0)
	{
		char Cmd[200];		// wifi=join:SSID:PASS is up to 106 characters, pool=S:<path> up to 191
		boolean bForm = FormValue (pFormData, "c", Cmd, sizeof Cmd);
		if (!bForm && !FormValue (pParams, "c", Cmd, sizeof Cmd)) return HTTPBadRequest;
		// another site's page cannot post commands (its Origin is not ours); wifi= only in a POST's body (never a URL)
		if (!FromPage (m_pNet, GetRequestHost (), GetRequestOrigin (), GetRequestReferer (), FALSE)
		    || (!bForm && strncmp (Cmd, "wifi=", 5) == 0))
		{
			memset (Cmd, 0, sizeof Cmd);
			return HTTPBadRequest;
		}
		// wdtest hangs core 0 on purpose: the watchdog's reboot ends a RAM kernel (needs a reflash, or sdboot's engine.img)
		if (Cmd[0] == 0 || strcmp (Cmd, "wdtest") == 0) return HTTPBadRequest;
		s_nCommands++;
		CLogger::Get ()->Write (From, LogNotice, "command :%s", CWifi::IsSecret (Cmd) ? "wifi=join:..." : Cmd);
		m_pConsole->Command (Cmd);
		memset (Cmd, 0, sizeof Cmd);
		memcpy (pBuffer, "ok\n", 3);
		*pLength = 3;
		*ppContentType = "text/plain";
	}
	else if (strncmp (pPath, "/wifi/", 6) == 0)	// the Wi-Fi settings (wifi.h, docs/pi-engine.md "Wi-Fi settings")
	{
		CWifi *w = CWifi::Get ();
		const char *op = pPath + 6;
		if (w == 0) return HTTPNotFound;
		if (strcmp (op, "status") == 0)	// the only answer with an SSID in it (the stored one, the scan's)
		{
			CString s;
			w->Settings (&s);
			unsigned n = s.GetLength ();
			if (n > *pLength) return HTTPInternalServerError;
			memcpy (pBuffer, (const char *) s, n);
			*pLength = n;
			*ppContentType = "application/json";
		}
		else if (strcmp (op, "scan") == 0) WifiReply (w->Scan (), 409, pBuffer, pLength, ppContentType);
		else if (strcmp (op, "join") == 0 || strcmp (op, "forget") == 0 || strcmp (op, "power") == 0)
		{
			if (!FromPage (m_pNet, GetRequestHost (), GetRequestOrigin (), GetRequestReferer (), TRUE))
				WifiReply ("refused: not from this page (the same-origin check)", 403, pBuffer, pLength, ppContentType);
			else if (op[0] == 'j')		// the body only: ssid=, psk=, open=1 (never the URL)
			{
				char SSID[wificonf::kSsidMax + 2], PSK[66], Open[4];
				boolean bSSID = FormValue (pFormData, "ssid", SSID, sizeof SSID);
				boolean bPSK = FormValue (pFormData, "psk", PSK, sizeof PSK);
				boolean bOpen = FormValue (pFormData, "open", Open, sizeof Open) && Open[0] == '1';
				if (!bPSK) PSK[0] = 0;
				const char *pErr = !bSSID ? wificonf::err_text (wificonf::kSsid)
					: !bPSK && !bOpen && strstr (pFormData, "psk=") ? wificonf::err_text (wificonf::kPsk)
					: w->Join (SSID, strlen (SSID), PSK, bOpen);
				memset (PSK, 0, sizeof PSK);
				memset ((char *) pFormData, 0, strlen (pFormData));	// the daemon's copy of the body
				WifiReply (pErr, 400, pBuffer, pLength, ppContentType);
			}
			else if (op[0] == 'f') WifiReply (w->Forget (), 409, pBuffer, pLength, ppContentType);
			else
			{
				char On[4];
				if (!FormValue (pFormData, "on", On, sizeof On) || (On[0] != '0' && On[0] != '1') || On[1]) return HTTPBadRequest;
				WifiReply (w->Power (On[0] == '1'), 409, pBuffer, pLength, ppContentType);
			}
		}
		else return HTTPNotFound;
	}
	else if (strcmp (pPath, "/um") == 0)		// the user machines (core/user/um.h's JSON) and the card's space
	{
		int n = um::json_copy ((char *) pBuffer, *pLength - 128);
		if (n <= 0) { memcpy (pBuffer, "[]", 2); n = 2; }
		u64 nFree = 0, nTotal = 0;
		if (n > 2 && BSpace (&nFree, &nTotal))
		{
			CString s;
			s.Format (",{\"t\":\"mem\",\"k\":\"sd\",\"size\":%lu,\"used\":%lu}]", (unsigned long) nTotal,
				  (unsigned long) (nTotal - nFree));
			memcpy (pBuffer + n - 1, (const char *) s, s.GetLength ());	// over the closing ']'
			n += s.GetLength () - 1;
		}
		*pLength = n;
		*ppContentType = "application/json";
	}
	else if (strcmp (pPath, "/lists") == 0)		// the pool and the JUNO / FM lists (core/user/lists.h's JSON)
	{
		int n = lists::json_copy ((char *) pBuffer, *pLength);
		if (n <= 0) { memcpy (pBuffer, "{}", 2); n = 2; }
		*pLength = n;
		*ppContentType = "application/json";
	}
	else if (strncmp (pPath, "/fs/", 4) == 0)		// the file browser (fsweb.h)
	{
		s_pConsole = m_pConsole;
		char Path[fsweb::kPath + 1], To[fsweb::kPath + 1], Num[24];
		fsweb::Result r = {400, "bad request"};
		if (!FormValue (pParams, "path", Path, sizeof Path)) Path[0] = 0;
		const char *op = pPath + 4;
		if (strcmp (op, "ls") == 0)
		{
			int n = fsweb::list_json (s_Card, Path[0] ? Path : "/", (char *) pBuffer, *pLength, &r);
			if (n <= 0) Reply (r, pBuffer, pLength, ppContentType);
			else { *pLength = n; *ppContentType = "application/json"; }
		}
		else if (strcmp (op, "get") == 0)		// a range: off=, len= (at most 64 KB)
		{
			u64 nOff = FormValue (pParams, "off", Num, sizeof Num) ? ParseU64 (Num) : 0;
			unsigned nLen = FormValue (pParams, "len", Num, sizeof Num) ? (unsigned) ParseU64 (Num) : 65536;
			if (nLen > 65536) nLen = 65536;
			if (nLen > *pLength) nLen = *pLength;
			u32 got = 0;
			if (!fsweb::path_ok (Path) || !BRead (Path, nOff, pBuffer, nLen, &got)) return HTTPNotFound;
			*pLength = got;
			*ppContentType = "application/octet-stream";
		}
		else if (strcmp (op, "put") == 0)		// off=, total=, crc= ; the chunk as the multipart's one part
		{
			const char *pHead;
			const u8 *pData = 0;
			unsigned nData = 0;
			if (!GetMultipartFormPart (&pHead, &pData, &nData)) { pData = (const u8 *) ""; nData = 0; }
			u64 nOff = FormValue (pParams, "off", Num, sizeof Num) ? ParseU64 (Num) : 0;
			u64 nTotal = FormValue (pParams, "total", Num, sizeof Num) ? ParseU64 (Num) : 0;
			u32 nCrc = FormValue (pParams, "crc", Num, sizeof Num) ? (u32) ParseU64 (Num) : 0;
			fsweb::put (s_Card, Path, nOff, nTotal, nCrc, pData, nData, &r);
			if (!(r.status == 200 && strncmp (r.msg, "ok", 2) == 0)) FileClose ();
			Reply (r, pBuffer, pLength, ppContentType);
		}
		else if (strcmp (op, "del") == 0) { fsweb::remove (s_Card, Path, &r); Reply (r, pBuffer, pLength, ppContentType); }
		else if (strcmp (op, "mkdir") == 0)		// p=1: every missing folder on the way (a dropped folder)
		{
			if (FormValue (pParams, "p", Num, sizeof Num) && Num[0] == '1') fsweb::mkdirs (s_Card, Path, &r);
			else fsweb::mkdir (s_Card, Path, &r);
			Reply (r, pBuffer, pLength, ppContentType);
		}
		else if (strcmp (op, "mv") == 0)
		{
			if (!FormValue (pParams, "to", To, sizeof To)) To[0] = 0;
			fsweb::rename (s_Card, Path, To, &r);
			Reply (r, pBuffer, pLength, ppContentType);
		}
		else return HTTPNotFound;
	}
	else Status = HTTPNotFound;
	unsigned us = CTimer::GetClockTicks () - t0;
	if (us > s_nMaxUs) s_nMaxUs = us;
	return Status;
}

char CWebStart::s_Hostname[64] = "";

CWebStart::CWebStart (CNetSubSystem *pNet, CLogTee *pLog, CConsole *pConsole, TWebStatus *pStatus, void *pParam)
:	m_pNet (pNet), m_pLog (pLog), m_pConsole (pConsole), m_pStatus (pStatus), m_pParam (pParam)
{
	SetName ("webstart");
}

void CWebStart::Run (void)
{
	CScheduler *S = CScheduler::Get ();
	while (!m_pNet->IsRunning ()) S->MsSleep (100);		// the Ethernet link is up
	new CWebServer (m_pNet, m_pLog, m_pConsole, m_pStatus, m_pParam);	// the listener (port 80)
	CmDNSPublisher *pPub = new CmDNSPublisher (m_pNet);	// starts Circle's mDNS daemon: sidecar.local
	static const char *Text[] = {"path=/", nullptr};
	while (!pPub->PublishService ("Sidecar | Expander", "_http._tcp", 80, Text)) S->MsSleep (100);
	CString h = CmDNSDaemon::Get ()->GetHostname ();
	strncpy (s_Hostname, (const char *) h, sizeof s_Hostname - 1);
	CLogger::Get ()->Write (From, LogNotice, "http://%s.local/ (port 80, _http._tcp \"Sidecar | Expander\")", s_Hostname);
	// done: the daemons are tasks of their own; this one ends
}
