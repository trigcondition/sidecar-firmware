// pi/engine/web_ws.h - the Pi's WebSocket endpoint /ws (docs/web-ws.md): one upgraded HTTP connection served to its
// end in the HTTP worker's own task on core 0 (pi/circle/circle-http-ws.patch hands the socket over), polling the
// socket without blocking and sleeping 10 ms between rounds, like the rest of the web code. The protocol is
// ws_session.h's, the frames ws_frame.h's (both portable, tests/test_ws.cpp); this file is only the socket and the
// timers. web.cpp decides whether a connection may upgrade (the path, the web UI switch, the strict same-origin check,
// MaxWs) and supplies the device's side (status, the pairing gate, the console).
#ifndef _web_ws_h
#define _web_ws_h

#include <circle/net/socket.h>
#include <circle/types.h>
#include <stddef.h>
#include "ws_session.h"

namespace ws_pi {

static const unsigned kPingMs = 15000;		// a ping when nothing was sent for this long (keeps NATs and the client awake)
static const unsigned kIdleMs = 45000;		// nothing at all from the client for this long: closed (1001)
static const unsigned kCloseWaitMs = 1000;	// after our close frame: the client's, at most this long
static const unsigned kLoopMs = 10;		// the poll's sleep when nothing arrived
// The send queue (Circle's TCP queue, CSocket::GetBytesQueued, circle-http-ws.patch: sent and not acknowledged, and
// unsent). MSG_DONTWAIT never refuses, so these are the only bound for a client that acks pushes but stopped reading
// (or a tool that floods pings): above kTxSoft no push is built (it waits, coalesced), above kTxHard the connection
// is dropped.
static const size_t kTxSoft = 48 * 1024;
static const size_t kTxHard = 128 * 1024;
static const size_t kOutMax = 40 * 1024;	// (the largest: the "seq" topic, 1024 notes: under 40 KB, test_ws)	// one outgoing message (a status push: /status is ~12 KB on a unit today)

struct TDevice
{
	void *pParam;
	bool (*TokenOk) (void *pParam, const char *pToken);
	size_t (*Build) (void *pParam, wsproto::Topic Topic, char *pOut, size_t nMax);	// a topic's payload
	unsigned nTopics;			// the topics served (wsproto::Host::topics)
	const char *(*Command) (void *pParam, const char *pCmd);
	boolean (*StillOn) (void);		// the web UI switch (SIDECAR MENU > WEB UI): off ends the connection
	const char *pVersion;
};

struct TStats { unsigned nPushes, nCoalesced, nRefused, nBad; u16 nCode; };

// Sends the 101 answer with pAccept (ws_frame.h's accept_key), serves the connection, deletes pSocket. The stats of
// the connection into *pStats (for the log line).
void Serve (CSocket *pSocket, const char *pAccept, const TDevice &rDevice, TStats *pStats);

}	// namespace ws_pi

#endif
