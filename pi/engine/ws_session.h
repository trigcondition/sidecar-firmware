// pi/engine/ws_session.h - one /ws connection's protocol (docs/web-ws.md): the hello, the requests (auth, sub, unsub,
// cmd, ping) and their replies, the pushes of the subscribed topics, and the flow control that keeps a slow client
// from piling data up on the Pi. Portable, header only, no allocation: the frames are ws_frame.h's, the device is
// behind Host's callbacks (web_ws.cpp on the Pi, tests/test_ws.cpp on the Mac).
//
// Flow control: every push carries a seq; the client acks the highest seq it has handled ({"type":"ack","seq":N}).
// At most kWindow pushes are unacked at a time; while the window is full nothing is built or sent, and when it opens
// the topic's newest state goes out - older states are never queued (coalesced, counted in Stats). So a client that
// stalls costs the Pi nothing but the open socket, and Circle's TCP queue (which MSG_DONTWAIT never refuses) holds at
// most kWindow pushes per client. Replies to requests are not windowed: the rate limit bounds them.
#ifndef _ws_session_h
#define _ws_session_h

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ws_json.h"

namespace wsproto {

static const unsigned kProto = 1;		// the protocol's version: a client refuses a major it does not know
static const unsigned kWindow = 4;		// unacked pushes per connection
static const unsigned kMinMs = 250;		// the status topic's fastest push interval a client can ask for
static const unsigned kDefaultMs = 1000;	// ... and the one it gets without asking (the old page polls /status at 1 s)
static const unsigned kMaxMs = 10000;		// the slowest of any topic
static const unsigned kMaxMsg = 4096;		// the longest message a client may send (ws_frame.h's Parser size)
static const unsigned kTokenMax = 64;		// a pairing session token (paircode.h: kTokenHex)
static const unsigned kCmdMax = 200;		// a console command, as /cmd's (web.cpp)
static const unsigned kBurst = 20;		// requests at once; then one per kRefillMs
static const unsigned kRefillMs = 50;		// (20 requests a second, sustained)
static const unsigned kMaxBad = 10;		// malformed messages before the connection is closed (1008)
static const unsigned kMaxAuthFail = 5;		// wrong tokens before the connection is closed (1008)

// The topics (docs/web-ws.md "Topics"). A build serves the ones its Host's mask names (the hello lists them).
enum Topic { kStatus, kLevels, kSeq, kSeqPos, kLog, kTopics };
static const char *const kTopicName[kTopics] = {"status", "levels", "seq", "seqpos", "log"};
struct Rate { unsigned min, def, gap; };	// ms: the fastest a client may ask, the default, the longest silence for an unchanged topic
static const Rate kRate[kTopics] = {
	{kMinMs, kDefaultMs, kMaxMs},	// status: /status
	{33, 40, 1000},			// levels: peaks, ~25 Hz
	{100, 250, 10000},		// seq: the sequencer view (pushed when it changed)
	{33, 50, 1000},			// seqpos: the play head
	{250, 500, 10000},		// log: the console log's new bytes (as GET /log)
};
// Names the protocol reserves for later topics: a sub to one of them is answered as "later", not "unknown", so a
// client can tell a newer server from a typo
static const char *const kLater[] = {"usbMode", "voices", "scenes"};

struct Host
{
	void *param;
	// one text message to the client; false: the connection failed (the session stops sending)
	bool (*send) (void *param, const char *p, size_t n);
	// the pairing gate's check of a session token (paircode.h's Gate::check)
	bool (*token_ok) (void *param, const char *tok);
	// a topic's payload (a JSON value, "{...}") into pOut, at most max bytes; its length, 0 if it does not fit
	size_t (*build) (void *param, Topic t, char *pOut, size_t max);
	// a console command, as POST /cmd runs it (its refusals included); 0 = taken, else why it was refused
	const char *(*command) (void *param, const char *c);
	const char *version;			// the firmware's, for the hello ("0.4.2")
	// false while the connection's send queue is too full for a push (the client stopped reading although it acks):
	// the push waits, as when the window is full. 0: always room
	bool (*can_push) (void *param);
	unsigned topics;			// the topics served: bit (1 << Topic); 0 = status only
};

struct Stats
{
	uint32_t requests = 0, pushes = 0, coalesced = 0, unchanged = 0, bad = 0, refused = 0;
};

// A message being written into a fixed buffer; ok false once anything did not fit
struct Writer
{
	char *p;
	size_t n, max;
	bool ok;
	Writer (char *b, size_t m) : p (b), n (0), max (m), ok (m > 0) { if (ok) p[0] = 0; }
	Writer &s (const char *t)
	{
		size_t k = strlen (t);
		if (!ok || n + k >= max) { ok = false; return *this; }
		memcpy (p + n, t, k);
		n += k;
		p[n] = 0;
		return *this;
	}
	Writer &u (uint32_t v)
	{
		char b[12];
		int i = 11;
		b[i] = 0;
		do { b[--i] = (char) ('0' + v % 10); v /= 10; } while (v);
		return s (b + i);
	}
	Writer &str (const char *t)		// a JSON string, quoted and escaped
	{
		char e[256];
		wsjson::escape (t ? t : "", e, sizeof e);
		return s ("\"").s (e).s ("\"");
	}
};

class Session
{
public:
	// pOut: the buffer every outgoing message is written into (a status push needs the status's size + 80)
	Session (const Host &h, char *pOut, size_t nOut) : m_Host (h), m_pOut (pOut), m_nOut (nOut)
	{
		memset (m_Tok, 0, sizeof m_Tok);
		for (unsigned t = 0; t < kTopics; t++) { m_Sub[t] = false; m_Ms[t] = kRate[t].def; m_At[t] = 0; m_Hash[t] = 0; m_bSent[t] = false; }
	}
	~Session () { wipe (); }

	// the connection is up: the hello
	void open (uint32_t now)
	{
		m_Bucket = kBurst;
		m_RefillAt = now;
		Writer w (m_pOut, m_nOut);
		w.s ("{\"type\":\"hello\",\"payload\":{\"proto\":").u (kProto).s (",\"server\":\"sidecar\",\"version\":")
		 .str (m_Host.version).s (",\"topics\":[");
		unsigned k = 0;
		for (unsigned t = 0; t < kTopics; t++) if (served ((Topic) t)) w.s (k++ ? "," : "").str (kTopicName[t]);
		w.s ("],\"later\":[");
		k = 0;
		for (unsigned t = 0; t < kTopics; t++) if (!served ((Topic) t)) w.s (k++ ? "," : "").str (kTopicName[t]);
		for (unsigned i = 0; i < sizeof kLater / sizeof kLater[0]; i++) w.s (k++ ? "," : "").str (kLater[i]);
		w.s ("],\"caps\":[\"auth\",\"sub\",\"unsub\",\"cmd\",\"ping\",\"ack\"],\"auth\":\"token\",\"window\":").u (kWindow)
		 .s (",\"maxMsg\":").u (kMaxMsg).s (",\"minMs\":").u (kMinMs).s (",\"maxMs\":").u (kMaxMs).s (",\"rates\":{");
		k = 0;
		for (unsigned t = 0; t < kTopics; t++)		// per topic: [fastest, default] in ms
			if (served ((Topic) t)) w.s (k++ ? "," : "").str (kTopicName[t]).s (":[").u (kRate[t].min).s (",").u (kRate[t].def).s ("]");
		w.s ("}}}");
		out (w);
	}

	// One whole text message from the client; 0, or the close code the connection must end with
	uint16_t message (const char *p, size_t n, uint32_t now)
	{
		if (m_bDead) return 1011;
		m_Stats.requests++;
		if (!wsjson::valid_object (p, n)) return bad (0, 400, "not a JSON object");
		char Type[16];
		wsjson::Span sp;
		if (!wsjson::find (p, n, "type", &sp) || !wsjson::str (sp, Type, sizeof Type)) return bad (0, 400, "no type");
		if (strcmp (Type, "ack") == 0)			// not a request: no id, no reply, not rate-limited
		{
			uint32_t seq;
			if (!wsjson::find (p, n, "seq", &sp) || !wsjson::u32 (sp, &seq)) return bad (0, 400, "ack: no seq");
			if (seq > m_Seq) return bad (0, 400, "ack: a seq not sent yet");
			if (seq > m_Acked) m_Acked = seq;
			return 0;
		}
		uint32_t id;
		if (!wsjson::find (p, n, "id", &sp) || !wsjson::u32 (sp, &id)) return bad (0, 400, "a request needs an id");
		refill (now);
		if (m_Bucket == 0) { m_Stats.refused++; error (id, 429, "too many requests"); return 0; }
		m_Bucket--;
		if (strcmp (Type, "ping") == 0)
		{
			Writer w (m_pOut, m_nOut);
			w.s ("{\"type\":\"reply\",\"id\":").u (id).s (",\"ok\":true,\"payload\":{\"t\":").u (now).s ("}}");
			out (w);
			return 0;
		}
		if (strcmp (Type, "auth") == 0) return auth (p, n, id);
		if (strcmp (Type, "sub") == 0 || strcmp (Type, "unsub") == 0) return sub (p, n, id, Type[0] == 's');
		if (strcmp (Type, "cmd") == 0) return cmd (p, n, id);
		return bad (id, 404, "unknown type");
	}

	// Pushes whatever is due (call it every few ms)
	void tick (uint32_t now)
	{
		if (m_bDead) return;
		for (unsigned t = 0; t < kTopics; t++)
		{
			if (!m_Sub[t] || (m_bSent[t] && now - m_At[t] < m_Ms[t])) continue;
			if (m_Seq - m_Acked >= kWindow || (m_Host.can_push && !m_Host.can_push (m_Host.param)))	// the newest goes out once the client acks
			{
				if (!m_bWaiting) { m_bWaiting = true; m_Stats.coalesced++; }
				return;
			}
			m_bWaiting = false;
			push ((Topic) t, now);
		}
	}

	// A control frame from the client (a ping, which costs a pong): charged to the request bucket like a request;
	// false when it is empty - the caller drops the pong and closes on repeats (a flood would fill the send queue)
	bool control (uint32_t now)
	{
		refill (now);
		if (m_Bucket == 0) { m_Stats.refused++; return false; }
		m_Bucket--;
		return true;
	}

	bool authed () const { return m_bAuthed; }
	bool subscribed (Topic t) const { return m_Sub[t]; }
	unsigned interval (Topic t) const { return m_Ms[t]; }
	uint32_t seq () const { return m_Seq; }
	uint32_t acked () const { return m_Acked; }
	bool dead () const { return m_bDead; }
	const Stats &stats () const { return m_Stats; }

private:
	void out (const Writer &w)
	{
		if (m_bDead) return;
		if (!w.ok) { m_bDead = true; return; }		// the buffer is too small for an answer: a bug, end it
		if (!m_Host.send (m_Host.param, w.p, w.n)) m_bDead = true;
	}
	void error (uint32_t id, unsigned code, const char *msg)
	{
		Writer w (m_pOut, m_nOut);
		if (id) w.s ("{\"type\":\"reply\",\"id\":").u (id).s (",\"ok\":false,\"error\":{\"code\":");
		else w.s ("{\"type\":\"error\",\"error\":{\"code\":");
		w.u (code).s (",\"msg\":").str (msg);
		if (code == 401) w.s (",\"pair\":true");
		w.s ("}}");
		out (w);
	}
	uint16_t bad (uint32_t id, unsigned code, const char *msg)
	{
		m_Stats.bad++;
		error (id, code, msg);
		return m_Stats.bad >= kMaxBad ? 1008 : 0;
	}
	void ok (uint32_t id, const char *payload)
	{
		Writer w (m_pOut, m_nOut);
		w.s ("{\"type\":\"reply\",\"id\":").u (id).s (",\"ok\":true,\"payload\":").s (payload).s ("}");
		out (w);
	}
	void refill (uint32_t now)
	{
		uint32_t n = (now - m_RefillAt) / kRefillMs;
		if (n == 0) return;
		m_RefillAt += n * kRefillMs;
		m_Bucket = m_Bucket + n > kBurst ? kBurst : m_Bucket + n;
	}
	void wipe () { memset (m_Tok, 0, sizeof m_Tok); m_bAuthed = false; }

	uint16_t auth (const char *p, size_t n, uint32_t id)
	{
		wsjson::Span sp;
		char Tok[kTokenMax + 2];
		bool bGot = wsjson::find (p, n, "payload.token", &sp) && wsjson::str (sp, Tok, sizeof Tok);
		if (bGot && m_Host.token_ok (m_Host.param, Tok))
		{
			memcpy (m_Tok, Tok, sizeof m_Tok);
			memset (Tok, 0, sizeof Tok);
			m_bAuthed = true;
			ok (id, "{\"paired\":true}");
			return 0;
		}
		memset (Tok, 0, sizeof Tok);
		wipe ();
		m_Stats.refused++;
		error (id, 401, "not paired: pair this browser first (the code is on the Rytm's screen)");
		return ++m_nAuthFail >= kMaxAuthFail ? 1008 : 0;
	}

	uint16_t sub (const char *p, size_t n, uint32_t id, bool bOn)
	{
		wsjson::Span sp, el;
		bool bList = wsjson::find (p, n, "payload.topics", &sp);
		if (bList && sp.p[0] != '[') return bad (id, 400, "topics: not an array");
		uint32_t ask = 0, ms = 0, msFirst = 0;		// ask 0: each topic's default; else clamped to each topic's range
		wsjson::Span sm;
		if (bOn && wsjson::find (p, n, "payload.ms", &sm) && !wsjson::u32 (sm, &ask)) return bad (id, 400, "ms: not an unsigned integer");
		if (bOn && !bList) return bad (id, 400, "sub: no topics");
		Writer ans (m_Ans, sizeof m_Ans), unk (m_Unk, sizeof m_Unk), lat (m_Lat, sizeof m_Lat);
		ans.s ("{\"topics\":[");
		size_t pos = 0;
		unsigned nOn = 0, nUnk = 0, nLat = 0;
		while (bList && wsjson::next_elem (sp, &pos, &el))
		{
			char Name[24];
			if (!wsjson::str (el, Name, sizeof Name)) return bad (id, 400, "topics: not strings");
			int t = topic (Name);
			if (t >= 0 && served ((Topic) t))
			{
				const Rate &r = kRate[t];
				ms = ask == 0 ? r.def : ask < r.min ? r.min : ask > kMaxMs ? kMaxMs : ask;
				if (nOn == 0) msFirst = ms;
				if (bOn && (!m_Sub[t] || m_Ms[t] != ms)) { m_bSent[t] = false; m_Hash[t] = 0; }	// send it now
				m_Sub[t] = bOn;
				if (bOn) m_Ms[t] = ms;
				ans.s (nOn++ ? "," : "").str (Name);
			}
			else if (t >= 0 || later (Name)) lat.s (nLat++ ? "," : "").str (Name);
			else unk.s (nUnk++ ? "," : "").str (Name);
		}
		if (!bOn && !bList) for (unsigned t = 0; t < kTopics; t++) m_Sub[t] = false;	// unsub with no list: all
		ans.s ("],\"later\":[").s (m_Lat).s ("],\"unknown\":[").s (m_Unk).s ("]");
		if (bOn && nOn) ans.s (",\"ms\":").u (msFirst);	// (the first topic's; each topic's range is in the hello)
		ans.s ("}");
		if (!ans.ok || !unk.ok || !lat.ok) return bad (id, 400, "topics: too many");
		ok (id, m_Ans);
		return 0;
	}

	uint16_t cmd (const char *p, size_t n, uint32_t id)
	{
		// the token is checked again: a session unpaired meanwhile (forget, revoke, a reboot of the gate) ends here
		if (!m_bAuthed || !m_Host.token_ok (m_Host.param, m_Tok))
		{
			wipe ();
			m_Stats.refused++;
			error (id, 401, "not paired: pair this browser first (the code is on the Rytm's screen)");
			return 0;
		}
		wsjson::Span sp;
		char Cmd[kCmdMax];
		if (!wsjson::find (p, n, "payload.c", &sp) || !wsjson::str (sp, Cmd, sizeof Cmd) || Cmd[0] == 0)
			return bad (id, 400, "cmd: no c (or longer than 199 bytes)");
		const char *pWhy = m_Host.command (m_Host.param, Cmd);
		memset (Cmd, 0, sizeof Cmd);			// it may have been wifi=join with its key
		if (pWhy != 0) { m_Stats.refused++; error (id, 403, pWhy); return 0; }
		ok (id, "{}");
		return 0;
	}

	void push (Topic t, uint32_t now)
	{
		Writer w (m_pOut, m_nOut);
		w.s ("{\"type\":\"pub\",\"topic\":").str (kTopicName[t]).s (",\"seq\":").u (m_Seq + 1).s (",\"payload\":");
		size_t nHead = w.n;
		size_t nBody = m_Host.build (m_Host.param, t, m_pOut + nHead, m_nOut > nHead + 2 ? m_nOut - nHead - 2 : 0);
		m_At[t] = now;
		if (nBody == 0) { error (0, 500, "a topic too large for the buffer (unsubscribed)"); m_Sub[t] = false; return; }
		w.n = nHead + nBody;
		uint32_t h = 2166136261u;			// FNV-1a of the body: unchanged -> nothing sent
		for (size_t i = nHead; i < w.n; i++) h = (h ^ (uint8_t) m_pOut[i]) * 16777619u;
		if (m_bSent[t] && h == m_Hash[t] && now - m_Sent[t] < kRate[t].gap) { m_Stats.unchanged++; return; }
		w.s ("}");
		m_Hash[t] = h;
		m_bSent[t] = true;
		m_Sent[t] = now;
		m_Seq++;
		m_Stats.pushes++;
		out (w);
	}

	bool served (Topic t) const { return t == kStatus || (m_Host.topics & (1u << t)) != 0; }
	static int topic (const char *p) { for (unsigned t = 0; t < kTopics; t++) if (strcmp (p, kTopicName[t]) == 0) return (int) t; return -1; }
	static bool later (const char *p) { for (unsigned i = 0; i < sizeof kLater / sizeof kLater[0]; i++) if (strcmp (p, kLater[i]) == 0) return true; return false; }

	Host m_Host;
	char *m_pOut;
	size_t m_nOut;
	char m_Tok[kTokenMax + 2];
	bool m_bAuthed = false, m_bDead = false, m_bWaiting = false;
	unsigned m_nAuthFail = 0;
	uint32_t m_Seq = 0, m_Acked = 0;
	uint32_t m_Bucket = kBurst, m_RefillAt = 0;
	bool m_Sub[kTopics];
	unsigned m_Ms[kTopics];
	uint32_t m_At[kTopics], m_Sent[kTopics] = {}, m_Hash[kTopics];
	bool m_bSent[kTopics];
	char m_Ans[512], m_Unk[256], m_Lat[256];
	Stats m_Stats;
};

}	// namespace wsproto

#endif
