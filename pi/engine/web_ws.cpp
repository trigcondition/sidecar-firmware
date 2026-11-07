// pi/engine/web_ws.cpp - see web_ws.h.
#include "web_ws.h"
#include "ws_frame.h"
#include "ws_session.h"

#include <circle/net/in.h>
#include <circle/net/netbuffer.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/util.h>

namespace ws_pi {

// One connection's buffers: on the heap (the worker task's stack is Circle's TASK_STACK_SIZE)
struct TConn
{
	CSocket *pSocket;
	const TDevice *pDevice;
	boolean bFailed;
	boolean bStalled;			// dropped for a full send queue (kTxHard)
	u32 nLastTx;				// ms: the last frame sent (for the ping)
	u32 nNow;
	wsframe::Parser<wsproto::kMaxMsg + 14, wsproto::kMaxMsg> In;
	char Out[kOutMax];
	u8 Rx[FRAME_BUFFER_SIZE];
};

// One frame: the header with MSG_MORE, then the payload. MSG_DONTWAIT never blocks and Circle's TCP queues whatever
// it is given, so the session's window (ws_session.h) is what bounds the queue, not this.
static boolean SendFrame (TConn *c, wsframe::Op op, const void *p, size_t n)
{
	if (c->bFailed) return FALSE;
	if (c->pSocket->GetBytesQueued () + n > kTxHard)	// the client does not read: drop it, nothing more queued
	{
		c->bFailed = TRUE;
		c->bStalled = TRUE;
		return FALSE;
	}
	u8 h[10];
	size_t nh = wsframe::header (h, op, n);
	if (   c->pSocket->Send (h, nh, MSG_DONTWAIT | (n > 0 ? MSG_MORE : 0)) < 0
	    || (n > 0 && c->pSocket->Send (p, n, MSG_DONTWAIT) < 0))
	{
		c->bFailed = TRUE;
		return FALSE;
	}
	c->nLastTx = c->nNow;
	return TRUE;
}

static bool HSend (void *p, const char *s, size_t n)	{ return SendFrame ((TConn *) p, wsframe::kText, s, n); }
static bool HToken (void *p, const char *t)		{ const TDevice *d = ((TConn *) p)->pDevice; return d->TokenOk (d->pParam, t); }
static size_t HBuild (void *p, wsproto::Topic t, char *o, size_t m)	{ const TDevice *d = ((TConn *) p)->pDevice; return d->Build (d->pParam, t, o, m); }
static const char *HCmd (void *p, const char *c)	{ const TDevice *d = ((TConn *) p)->pDevice; return d->Command (d->pParam, c); }
static unsigned HChanges (void *p)			{ const TDevice *d = ((TConn *) p)->pDevice; return d->Changes ? d->Changes () : 0; }
static bool HStrip (void *p, const char *s)		{ const TDevice *d = ((TConn *) p)->pDevice; return d->Strip ? d->Strip (d->pParam, s) : true; }
static bool HCanPush (void *p)				{ return ((TConn *) p)->pSocket->GetBytesQueued () < kTxSoft; }

// A close code a peer may send (RFC 6455 7.4): echoed back; anything else is answered with 1002
static u16 EchoCode (u16 n)
{
	if (n == 1005) return 1000;			// no code: a plain close
	if ((n >= 1000 && n <= 1003) || (n >= 1007 && n <= 1011) || (n >= 3000 && n <= 4999)) return n;
	return 1002;
}

// A ms clock from the µs ticks that does not jump when they wrap (every ~71 minutes)
struct TClock
{
	u32 nTicks, nMs;
	TClock () : nTicks (CTimer::GetClockTicks ()), nMs (0) {}
	u32 Now (void)
	{
		u32 d = (CTimer::GetClockTicks () - nTicks) / 1000;
		nTicks += d * 1000;
		nMs += d;
		return nMs;
	}
};

void Serve (CSocket *pSocket, const char *pAccept, const TDevice &rDevice, TStats *pStats)
{
	memset (pStats, 0, sizeof *pStats);
	CString Head;
	Head.Format ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		     "Sec-WebSocket-Accept: %s\r\n\r\n", pAccept);
	TConn *c = new TConn;
	if (c == 0 || pSocket->Send ((const char *) Head, Head.GetLength (), MSG_DONTWAIT) < 0)
	{
		delete c;
		delete pSocket;
		pStats->nCode = 1011;
		return;
	}
	c->pSocket = pSocket;
	c->pDevice = &rDevice;
	c->bFailed = FALSE;
	c->bStalled = FALSE;
	TClock Clock;
	c->nNow = Clock.Now ();
	c->nLastTx = c->nNow;
	wsproto::Host Host = {c, HSend, HToken, HBuild, HCmd, rDevice.pVersion, HCanPush, rDevice.nTopics, HChanges, HStrip};
	wsproto::Session *pSession = new wsproto::Session (Host, c->Out, sizeof c->Out);
	pSession->open (c->nNow);

	u32 nLastRx = c->nNow, nClosingAt = 0;
	boolean bClosing = FALSE;
	u16 nCode = 0;
	auto Close = [&] (u16 nWhy, const char *pReason)	// our close frame; then wait for the client's
	{
		if (bClosing) return;
		u8 b[125];
		size_t n = wsframe::close_payload (b, nWhy, pReason);
		SendFrame (c, wsframe::kClose, b, n);
		bClosing = TRUE;
		nClosingAt = c->nNow;
		nCode = nWhy;
	};

	while (!c->bFailed)
	{
		c->nNow = Clock.Now ();
		int n = pSocket->Receive (c->Rx, sizeof c->Rx, MSG_DONTWAIT);
		if (n < 0) { if (!nCode) nCode = 1006; break; }		// gone (reset, or closed after our close)
		if (n > 0)
		{
			nLastRx = c->nNow;
			c->In.push (c->Rx, (size_t) n);			// a refusal shows as a kTooBig event below
			wsframe::Event e;
			boolean bDone = FALSE;
			while (!bDone && c->In.next (&e))
			{
				switch (e.kind)
				{
				case wsframe::Event::kMessage:
					if (!bClosing)
					{
						u16 nWhy = pSession->message ((const char *) e.data, e.len, c->nNow);
						if (nWhy) Close (nWhy, nWhy == 1008 ? "too many bad requests" : "internal error");
					}
					break;
				case wsframe::Event::kPing:		// charged like a request: a flood of pings is not a flood of pongs
					if (bClosing) break;
					if (pSession->control (c->nNow)) SendFrame (c, wsframe::kPong, e.data, e.len);
					else Close (1008, "too many pings");
					break;
				case wsframe::Event::kPong:
					break;
				case wsframe::Event::kClose:		// the client's: answered with its code (unless we began)
					if (!bClosing)
					{
						u16 nEcho = EchoCode (e.code);
						u8 b[2] = {(u8) (nEcho >> 8), (u8) nEcho};
						SendFrame (c, wsframe::kClose, b, 2);
						nCode = e.code;
					}
					bDone = TRUE;
					bClosing = TRUE;
					nClosingAt = 0;			// both sides have closed: end now
					break;
				case wsframe::Event::kError:
					Close (e.code, "protocol error");
					break;
				default:
					break;
				}
			}
			if (bDone) break;
		}
		if (pSession->dead ()) { if (!bClosing) Close (1011, "internal error"); }
		if (!bClosing)
		{
			pSession->tick (c->nNow);
			if (!rDevice.StillOn ()) Close (1001, "the web UI was turned off");
			else if (c->nNow - nLastRx > kIdleMs) Close (1001, "idle");
			else if (c->nNow - c->nLastTx > kPingMs) SendFrame (c, wsframe::kPing, 0, 0);
		}
		else if (c->nNow - nClosingAt > kCloseWaitMs) break;
		if (n > 0) CScheduler::Get ()->Yield ();
		else CScheduler::Get ()->MsSleep (kLoopMs);
	}
	if (c->bStalled) nCode = 1008;			// (logged: the client stopped reading)
	if (c->bFailed && !nCode) nCode = 1006;

	const wsproto::Stats &s = pSession->stats ();
	pStats->nPushes = s.pushes;
	pStats->nCoalesced = s.coalesced;
	pStats->nRefused = s.refused;
	pStats->nBad = s.bad;
	pStats->nCode = nCode;
	delete pSession;				// wipes the token it held
	c->In.wipe ();					// and what passed through: the auth message, a wifi= key
	memset (c->Rx, 0, sizeof c->Rx);
	delete c;
	delete pSocket;					// closes the connection
}

}	// namespace ws_pi
