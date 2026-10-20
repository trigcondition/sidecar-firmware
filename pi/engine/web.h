// pi/engine/web.h - the Pi's own web page (docs/pi-engine.md "Web page"): http://sidecar.local over the on-board
// Ethernet. Circle's mDNS daemon answers sidecar.local and publishes an _http._tcp service; Circle's HTTP daemon
// serves one page (www/index.html, built in), /status (JSON), /log?since=N (the console log's tail from a ring
// buffer), POST /cmd (c=<command>: the serial console's parser, CConsole::Command), /um (the user machines' JSON),
// /lists (the pool and JUNO / FM lists' JSON, core/user/lists.h), /fs/* (fsweb.h) and /wifi/* (wifi.h: status, scan,
// and the POSTs join, forget, power, accepted from this page only: wifi_conf.h's same_origin). Everything runs as Circle
// tasks on core 0 (Circle's scheduler and network stack are core 0's), between the loop's 1 ms sleeps; the
// Overbridge stream and the USB-C gadget run in core 0's interrupts, which the tasks never mask.
// cmdline.txt web=0: no HTTP daemon and no mDNS (the network itself stays up for link=tcp).
#ifndef _web_h
#define _web_h

#include <circle/device.h>
#include <circle/logger.h>
#include <circle/net/httpdaemon.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/task.h>
#include <circle/spinlock.h>
#include <circle/string.h>
#include <circle/types.h>

class CConsole;

// The logger's target: everything to the serial port as before, and a copy into a ring buffer (64 KB) the page
// reads. Any core may log: one spin lock (IRQs off while a line is copied, a few hundred bytes).
class CLogTee : public CDevice
{
public:
	static const unsigned Size = 65536;		// a power of two
	CLogTee (CDevice *pTarget) : m_pTarget (pTarget) {}
	int Write (const void *pBuffer, size_t nCount) override;
	// The log from byte position nSince (counted from the boot) to now, at most nMax bytes, into pOut; the oldest
	// still held if nSince has been overwritten (or is in the future). *pStart: where the copy begins.
	unsigned ReadSince (u64 nSince, char *pOut, unsigned nMax, u64 *pStart, u64 *pEnd);
private:
	CDevice *m_pTarget;
	char m_Ring[Size];
	volatile u64 m_nTotal = 0;
	CSpinLock m_Lock;
};

// The page's /status fields the kernel knows (kernel.cpp), as JSON members without the braces
typedef void TWebStatus (CString *pOut, void *pParam);

class CWebServer : public CHTTPDaemon
{
public:
	static const unsigned MaxContent = 128 * 1024;	// the page (73 KB), 64 KB of /fs/get, the log, the /lists JSON (48 KB)
	static const unsigned MaxMultipart = 66 * 1024;	// an upload chunk (fsweb::kChunkMax) and its multipart framing
	CWebServer (CNetSubSystem *pNet, CLogTee *pLog, CConsole *pConsole, TWebStatus *pStatus, void *pParam,
		    CSocket *pSocket = 0);
	CHTTPDaemon *CreateWorker (CNetSubSystem *pNet, CSocket *pSocket) override;
	THTTPStatus GetContent (const char *pPath, const char *pParams, const char *pFormData, u8 *pBuffer,
				unsigned *pLength, const char **ppContentType) override;
	void WriteAccessLog (const CIPAddress &rRemoteIP, THTTPRequestMethod Method, const char *pURI,
			     THTTPStatus Status, unsigned nContentLength) override {}	// quiet: the page polls

	static unsigned Requests (void)		{ return s_nRequests; }
	static unsigned MaxUs (void)		{ return s_nMaxUs; }	// the longest GetContent, in us
	static unsigned Commands (void)		{ return s_nCommands; }
	// One log line (kernel.cpp, once a minute): requests, refused (503), connections closed without a request
	// (Circle's "httpd: Receive failed (-12)": nothing arrived within Timeout s, e.g. a browser's spare connection), the
	// longest connection. Nothing when nothing happened since the last line.
	static void LogStats (CLogger *pLogger);
	~CWebServer (void);
	static const unsigned MaxWorkers = 4;	// connections served at once (CreateWorker)
	// Circle's receive timeout per Receive (seconds): a connection that sends nothing for this long ends (its worker
	// with it), so a browser's idle spare connections, or the ones of a client gone with a link change, free their
	// slot after it. A request trickling a byte at a time could hold a slot longer: none of the page's do.
	static const unsigned Timeout = 3;
private:
	static unsigned s_nWorkers, s_nRefused, s_nIdle, s_nMaxLifeMs;
	static CWebServer *s_pWorker[MaxWorkers];	// the live workers (the oldest one's age in the busy line)
	CNetSubSystem *m_pNet;
	CLogTee *m_pLog;
	CConsole *m_pConsole;
	TWebStatus *m_pStatus;
	void *m_pParam;
	boolean m_bWorker;
	unsigned m_nStart;			// the connection's start, CTimer::GetClockTicks
	boolean m_bServed;			// GetContent ran (FALSE at the end: closed without a request)
	static unsigned s_nRequests, s_nMaxUs, s_nCommands;
};

// Starts the web page once the network is up (the link may come later than the boot): mDNS (blocks until its
// hostname is claimed: a task of its own, not core 0's loop), the service record, the HTTP daemon.
class CWebStart : public CTask
{
public:
	CWebStart (CNetSubSystem *pNet, CLogTee *pLog, CConsole *pConsole, TWebStatus *pStatus, void *pParam);
	void Run (void) override;
	static const char *Hostname (void)	{ return s_Hostname; }	// "" until mDNS runs
private:
	CNetSubSystem *m_pNet;
	CLogTee *m_pLog;
	CConsole *m_pConsole;
	TWebStatus *m_pStatus;
	void *m_pParam;
	static char s_Hostname[64];
};

#endif
