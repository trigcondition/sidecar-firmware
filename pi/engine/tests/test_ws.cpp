// pi/engine/tests/test_ws.cpp - the /ws server's portable parts (docs/web-ws.md): ws_frame.h (handshake key, frames),
// ws_json.h (the client's messages), ws_session.h (the protocol, flow control) and web_cmd.h (the command policy).
#include "check.h"
#include "../ws_frame.h"
#include "../ws_json.h"
#include "../ws_session.h"
#include "../web_cmd.h"
#include "../seqview_json.h"
#include "../peak_fan.h"
#include "../ws_topics.h"
#include "../voiceview_json.h"
#include "../webnext_path.h"

#include <string>
#include <vector>

using namespace wsframe;

static std::string hex (const uint8_t *p, size_t n)
{
	static const char k[] = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < n; i++) { s += k[p[i] >> 4]; s += k[p[i] & 15]; }
	return s;
}

static std::string sha1 (const std::string &s)
{
	Sha1 h;
	h.update (s.data (), s.size ());
	uint8_t d[20];
	h.final (d);
	return hex (d, 20);
}

// a client's frame: masked with a fixed key
static std::vector<uint8_t> frame (int op, const std::string &pay, bool fin = true, bool mask = true, int rsv = 0)
{
	std::vector<uint8_t> f;
	f.push_back ((fin ? 0x80 : 0) | rsv | op);
	size_t n = pay.size ();
	uint8_t m = mask ? 0x80 : 0;
	if (n < 126) f.push_back (m | n);
	else if (n < 65536) { f.push_back (m | 126); f.push_back (n >> 8); f.push_back (n & 255); }
	else { f.push_back (m | 127); for (int i = 0; i < 8; i++) f.push_back ((uint64_t) n >> (56 - 8 * i)); }
	const uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
	if (mask) for (int i = 0; i < 4; i++) f.push_back (key[i]);
	for (size_t i = 0; i < n; i++) f.push_back ((uint8_t) pay[i] ^ (mask ? key[i & 3] : 0));
	return f;
}

typedef Parser<4096 + 14, 4096> P;

static void test_handshake ()
{
	CHECK (sha1 ("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK (sha1 ("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
	CHECK (sha1 ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
	CHECK (sha1 (std::string (1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
	char b[16];
	base64 ((const uint8_t *) "f", 1, b); CHECK (std::string (b) == "Zg==");
	base64 ((const uint8_t *) "fo", 2, b); CHECK (std::string (b) == "Zm8=");
	base64 ((const uint8_t *) "foo", 3, b); CHECK (std::string (b) == "Zm9v");
	char a[29];
	CHECK (accept_key ("dGhlIHNhbXBsZSBub25jZQ==", a));			// RFC 6455 1.3
	CHECK (std::string (a) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	CHECK (!accept_key ("dGhlIHNhbXBsZSBub25jZQ=", a));
	CHECK (!accept_key ("dGhlIHNhbXBsZSBub25jZQ!=", a));
	CHECK (!accept_key ("dGhlIHNhbXBsZSBub2\r\nZQ==", a));
	CHECK (!accept_key (0, a));
	CHECK (is_websocket ("websocket") && is_websocket ("WebSocket") && !is_websocket ("websockets") && !is_websocket ("h2c") && !is_websocket (""));
}

static void test_frames ()
{
	uint8_t h[10];
	CHECK (header (h, kText, 5) == 2 && h[0] == 0x81 && h[1] == 5);
	CHECK (header (h, kText, 126) == 4 && h[1] == 126 && h[2] == 0 && h[3] == 126);
	CHECK (header (h, kText, 70000) == 10 && h[1] == 127 && h[7] == 1 && h[8] == 0x11 && h[9] == 0x70);
	CHECK (close_payload (h, 1001, 0) == 2 && h[0] == 3 && h[1] == 0xe9);

	Event e;
	{	// RFC 6455 5.7: a masked "Hello", fed a byte at a time
		static const uint8_t k[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
		P *p = new P;
		for (size_t i = 0; i < sizeof k - 1; i++) { CHECK (p->push (k + i, 1)); CHECK (!p->next (&e)); }
		CHECK (p->push (k + 10, 1) && p->next (&e) && e.kind == Event::kMessage && e.len == 5 && memcmp (e.data, "Hello", 5) == 0);
		CHECK (!p->next (&e));
		delete p;
	}
	{	// fragments with a ping between them; two frames in one push
		P *p = new P;
		auto a = frame (kText, "Hel", false), b = frame (kPing, "x"), c = frame (kCont, "lo");
		a.insert (a.end (), b.begin (), b.end ());
		a.insert (a.end (), c.begin (), c.end ());
		CHECK (p->push (a.data (), a.size ()));
		CHECK (p->next (&e) && e.kind == Event::kPing && e.len == 1 && e.data[0] == 'x');
		CHECK (p->next (&e) && e.kind == Event::kMessage && std::string ((const char *) e.data, e.len) == "Hello");
		delete p;
	}
	{	// 16-bit length, close with a code and a reason
		P *p = new P;
		std::string big (300, 'z');
		auto a = frame (kText, big);
		CHECK (p->push (a.data (), a.size ()) && p->next (&e) && e.kind == Event::kMessage && e.len == 300);
		auto c = frame (kClose, std::string ("\x03\xe8" "bye", 5));
		CHECK (p->push (c.data (), c.size ()) && p->next (&e) && e.kind == Event::kClose && e.code == 1000 && e.len == 3);
		auto c0 = frame (kClose, "");
		CHECK (p->push (c0.data (), c0.size ()) && p->next (&e) && e.kind == Event::kClose && e.code == 1005);
		delete p;
	}
	struct { std::vector<uint8_t> f; uint16_t code; } bad[] = {
		{frame (kText, "hi", true, false), kProtocol},			// not masked
		{frame (kText, "hi", true, true, 0x40), kProtocol},		// RSV1 without an extension
		{frame (kBinary, "hi"), kUnsupported},
		{frame (kCont, "hi"), kProtocol},				// a continuation of nothing
		{frame (3, "hi"), kProtocol},					// a reserved data opcode
		{frame (0xB, "hi"), kProtocol},					// a reserved control opcode
		{frame (kPing, "hi", false), kProtocol},			// a fragmented control frame
		{frame (kPing, std::string (126, 'p')), kProtocol},		// a control frame over 125
		{frame (kText, "\xc3\x28"), kBadData},				// invalid UTF-8
		{frame (kText, "\xed\xa0\x80"), kBadData},			// a surrogate
		{frame (kText, "\xc0\xaf"), kBadData},				// an overlong
		{frame (kText, "\xe2\x82"), kBadData},				// cut short
		{frame (kText, std::string (4097, 'a')), kTooBig},
		{frame (kClose, "\x03"), kProtocol},				// a close of one byte
	};
	for (auto &b : bad)
	{
		P *p = new P;
		p->push (b.f.data (), b.f.size ());
		CHECK (p->next (&e) && e.kind == Event::kError && e.code == b.code);
		CHECK (!p->next (&e) && p->error ());
		delete p;
	}
	{	// a new text frame while one is fragmented; a 64-bit length that fits 16 bits
		P *p = new P;
		auto a = frame (kText, "a", false), b = frame (kText, "b");
		a.insert (a.end (), b.begin (), b.end ());
		p->push (a.data (), a.size ());
		CHECK (p->next (&e) && e.kind == Event::kError && e.code == kProtocol);
		delete p;
		p = new P;
		const uint8_t k[] = {0x81, 0xFF, 0, 0, 0, 0, 0, 0, 0, 5};
		p->push (k, sizeof k);
		CHECK (p->next (&e) && e.kind == Event::kError && e.code == kProtocol);
		delete p;
	}
	{	// fragments adding up past the limit; more bytes than the input buffer
		P *p = new P;
		auto a = frame (kText, std::string (3000, 'a'), false), b = frame (kCont, std::string (1200, 'b'));
		p->push (a.data (), a.size ());
		CHECK (!p->next (&e));
		p->push (b.data (), b.size ());
		CHECK (p->next (&e) && e.kind == Event::kError && e.code == kTooBig);
		delete p;
		p = new P;
		std::vector<uint8_t> junk (5000, 0x81);
		CHECK (!p->push (junk.data (), junk.size ()));
		CHECK (p->next (&e) && e.kind == Event::kError && e.code == kTooBig);
		delete p;
	}
	{	// many empty fragments: a loop, not a recursion
		P *p = new P;
		std::vector<uint8_t> all = frame (kText, "", false);
		for (int i = 0; i < 600; i++) { auto f = frame (kCont, "", false); all.insert (all.end (), f.begin (), f.end ()); }
		auto end = frame (kCont, "x");
		all.insert (all.end (), end.begin (), end.end ());
		CHECK (p->push (all.data (), all.size ()) && p->next (&e) && e.kind == Event::kMessage && e.len == 1);
		p->wipe ();
		delete p;
	}
	CHECK (utf8_ok ((const uint8_t *) "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\xb5", 10));
	CHECK (!utf8_ok ((const uint8_t *) "\xf4\x90\x80\x80", 4));		// past U+10FFFF
}

static void test_json ()
{
	using namespace wsjson;
	const char *ok[] = {"{}", " {\"a\":1} ", "{\"a\":[1,2,{\"b\":null}],\"c\":\"x\\\"y\",\"d\":-1.5e3,\"e\":true}"};
	for (auto s : ok) CHECK (valid_object (s, strlen (s)));
	const char *no[] = {"", "[]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":01}", "{a:1}", "{\"a\":1} x",
			    "{\"a\":\"\x01\"}", "{\"a\":\"\\x\"}", "{\"a\":\"\\u12\"}", "{\"a\":tru}",
			    "{\"a\":[[[[[[[[[[1]]]]]]]]]]}"};
	for (auto s : no) CHECK (!valid_object (s, strlen (s)));

	const char *m = "{ \"type\" : \"cmd\", \"id\":42, \"payload\":{\"x\":{\"c\":\"no\"},\"c\":\"notes=1\\u00e9\\ud83c\\udfb5\",\"topics\":[\"status\", \"voices\"]}}";
	size_t n = strlen (m);
	CHECK (valid_object (m, n));
	Span s;
	char b[64];
	CHECK (find (m, n, "type", &s) && str (s, b, sizeof b) && strcmp (b, "cmd") == 0);
	uint32_t v;
	CHECK (find (m, n, "id", &s) && u32 (s, &v) && v == 42);
	CHECK (find (m, n, "payload.c", &s) && str (s, b, sizeof b) && strcmp (b, "notes=1\xc3\xa9\xf0\x9f\x8e\xb5") == 0);
	CHECK (!find (m, n, "c", &s) && !find (m, n, "payload.y", &s) && !find (m, n, "id.x", &s));
	CHECK (find (m, n, "payload.topics", &s));
	size_t pos = 0;
	Span el;
	CHECK (next_elem (s, &pos, &el) && str (el, b, sizeof b) && strcmp (b, "status") == 0);
	CHECK (next_elem (s, &pos, &el) && str (el, b, sizeof b) && strcmp (b, "voices") == 0);
	CHECK (!next_elem (s, &pos, &el));
	Span e0 = {"[]", 2};
	pos = 0;
	CHECK (!next_elem (e0, &pos, &el));
	CHECK (!str (Span {"\"abcdef\"", 8}, b, 4));				// too long
	CHECK (!str (Span {"\"\\u0000\"", 8}, b, sizeof b));			// a NUL
	CHECK (!str (Span {"\"\\udc00\"", 8}, b, sizeof b));			// a lone low surrogate
	CHECK (!str (Span {"12", 2}, b, sizeof b));
	CHECK (u32 (Span {"4294967295", 10}, &v) && v == 4294967295u);
	CHECK (!u32 (Span {"4294967296", 10}, &v) && !u32 (Span {"-1", 2}, &v) && !u32 (Span {"1.0", 3}, &v) && !u32 (Span {"01", 2}, &v));
	escape ("a\"b\\c\n", b, sizeof b);
	CHECK (strcmp (b, "a\\\"b\\\\c\\u000a") == 0);
	escape ("ab\xc3\xa9", b, 4);					// cut before the é, never inside it
	CHECK (strcmp (b, "ab") == 0);
	// a key with an escape never matches, a duplicate key: the first wins
	const char *d = "{\"t\\u0079pe\":\"x\",\"type\":\"a\",\"type\":\"b\"}";
	CHECK (valid_object (d, strlen (d)) && find (d, strlen (d), "type", &s) && str (s, b, sizeof b) && strcmp (b, "a") == 0);
}

// ---- the session against a fake device
struct Fake
{
	std::vector<std::string> sent;
	std::string status = "{\"uptime\":1}";
	std::string token = "00112233445566778899aabbccddeeff";
	std::vector<std::string> cmds;
	bool fail = false;
	bool room = true;			// can_push
	std::string other = "{\"x\":1}";	// every topic but status
	unsigned topics = 0;
};
static bool FRoom (void *p) { return ((Fake *) p)->room; }
static bool FSend (void *p, const char *s, size_t n) { Fake *f = (Fake *) p; if (f->fail) return false; f->sent.push_back (std::string (s, n)); return true; }
static bool FTok (void *p, const char *t) { return !((Fake *) p)->token.empty () && ((Fake *) p)->token == t; }
static size_t FStatus (void *p, wsproto::Topic t, char *o, size_t max)
{
	Fake *f = (Fake *) p;
	const std::string &s = t == wsproto::kStatus ? f->status : f->other;
	if (s.size () > max) return 0;
	memcpy (o, s.data (), s.size ());
	return s.size ();
}
static const char *FCmd (void *p, const char *c)
{
	((Fake *) p)->cmds.push_back (c);
	return webcmd::refused (c, true, false);
}

static bool has (const std::string &s, const char *t) { return s.find (t) != std::string::npos; }

struct Rig
{
	Fake f;
	char buf[8192];
	wsproto::Session *s;
	Rig (size_t nBuf = 8192, unsigned topics = 0)
	{
		wsproto::Host h = {&f, FSend, FTok, FStatus, FCmd, "0.4.2", FRoom, topics};
		s = new wsproto::Session (h, buf, nBuf);
		s->open (0);
	}
	~Rig () { delete s; }
	uint16_t msg (const std::string &m, uint32_t now = 0) { return s->message (m.data (), m.size (), now); }
	const std::string &last () { return f.sent.back (); }
};

static void test_session ()
{
	{
		Rig r;
		CHECK (r.f.sent.size () == 1 && has (r.last (), "\"type\":\"hello\"") && has (r.last (), "\"proto\":1")
		       && has (r.last (), "\"version\":\"0.4.2\"") && has (r.last (), "\"window\":4"));
		CHECK (wsjson::valid_object (r.last ().data (), r.last ().size ()));
		// a ping, an unknown type, a request without an id
		CHECK (r.msg ("{\"type\":\"ping\",\"id\":1}", 77) == 0 && has (r.last (), "\"id\":1,\"ok\":true,\"payload\":{\"t\":77}"));
		CHECK (r.msg ("{\"type\":\"nope\",\"id\":2}") == 0 && has (r.last (), "\"code\":404"));
		CHECK (r.msg ("{\"type\":\"ping\"}") == 0 && has (r.last (), "\"type\":\"error\""));
		// sub: status now, later names, unknown names; ms clamped
		CHECK (r.msg ("{\"type\":\"sub\",\"id\":3,\"payload\":{\"topics\":[\"status\",\"scenes\",\"zzz\"],\"ms\":10}}") == 0);
		CHECK (has (r.last (), "\"topics\":[\"status\"],\"later\":[\"scenes\"],\"unknown\":[\"zzz\"],\"ms\":250"));
		CHECK (r.s->subscribed (wsproto::kStatus) && r.s->interval (wsproto::kStatus) == 250);
		CHECK (wsjson::valid_object (r.last ().data (), r.last ().size ()));
		size_t n0 = r.f.sent.size ();
		r.s->tick (1000);					// due at once
		CHECK (r.f.sent.size () == n0 + 1 && r.last () == "{\"type\":\"pub\",\"topic\":\"status\",\"seq\":1,\"payload\":{\"uptime\":1}}");
		r.s->tick (1100);					// not due
		CHECK (r.f.sent.size () == n0 + 1);
		r.s->tick (1250);					// due, unchanged: nothing sent
		CHECK (r.f.sent.size () == n0 + 1 && r.s->stats ().unchanged == 1);
		r.f.status = "{\"uptime\":2}";
		r.s->tick (1400);					// changed, but only 150 ms since the last build
		CHECK (r.f.sent.size () == n0 + 1);
		r.s->tick (1500);
		CHECK (r.f.sent.size () == n0 + 2 && has (r.last (), "\"seq\":2,\"payload\":{\"uptime\":2}"));
		// unchanged for kMaxMs: sent anyway (a heartbeat)
		for (uint32_t t = 1750; t < 11500; t += 250) r.s->tick (t);
		CHECK (r.f.sent.size () == n0 + 2);
		r.s->tick (11500);
		CHECK (r.f.sent.size () == n0 + 3 && has (r.last (), "\"seq\":3"));
		// the window: 4 unacked, then nothing; the ack lets the newest out
		r.f.status = "{\"uptime\":4}";
		r.s->tick (11750);
		CHECK (r.s->seq () == 4);
		r.f.status = "{\"uptime\":5}";
		r.s->tick (12000);
		r.f.status = "{\"uptime\":6}";
		r.s->tick (12250);
		CHECK (r.s->seq () == 4 && r.s->stats ().coalesced == 1);
		CHECK (r.msg ("{\"type\":\"ack\",\"seq\":9}") == 0 && has (r.last (), "a seq not sent yet"));
		CHECK (r.msg ("{\"type\":\"ack\",\"seq\":2}") == 0 && r.s->acked () == 2);
		r.s->tick (12300);
		CHECK (r.s->seq () == 5 && has (r.last (), "\"seq\":5,\"payload\":{\"uptime\":6}"));
		CHECK (r.msg ("{\"type\":\"ack\",\"seq\":1}") == 0 && r.s->acked () == 2);	// never backwards
		// unsub everything
		CHECK (r.msg ("{\"type\":\"unsub\",\"id\":4}") == 0 && !r.s->subscribed (wsproto::kStatus));
		size_t n1 = r.f.sent.size ();
		r.s->tick (20000);
		CHECK (r.f.sent.size () == n1);
	}
	{	// commands need the token, and it is checked again for each
		Rig r;
		CHECK (r.msg ("{\"type\":\"cmd\",\"id\":1,\"payload\":{\"c\":\"notes=1\"}}") == 0 && has (r.last (), "\"code\":401") && has (r.last (), "\"pair\":true"));
		CHECK (r.f.cmds.empty ());
		CHECK (r.msg ("{\"type\":\"auth\",\"id\":2,\"payload\":{\"token\":\"bad\"}}") == 0 && has (r.last (), "\"code\":401") && !r.s->authed ());
		CHECK (r.msg ("{\"type\":\"auth\",\"id\":3,\"payload\":{\"token\":\"00112233445566778899aabbccddeeff\"}}") == 0
		       && has (r.last (), "\"id\":3,\"ok\":true") && r.s->authed ());
		CHECK (!has (r.last (), "0011223344"));		// the token is never echoed
		CHECK (r.msg ("{\"type\":\"cmd\",\"id\":4,\"payload\":{\"c\":\"notes=1\"}}") == 0 && has (r.last (), "\"id\":4,\"ok\":true"));
		CHECK (r.f.cmds.size () == 1 && r.f.cmds[0] == "notes=1");
		CHECK (r.msg ("{\"type\":\"cmd\",\"id\":5,\"payload\":{\"c\":\"midibridge=on\"}}") == 0 && has (r.last (), "\"code\":403"));
		CHECK (r.msg ("{\"type\":\"cmd\",\"id\":6,\"payload\":{}}") == 0 && has (r.last (), "\"code\":400"));
		CHECK (r.msg ("{\"type\":\"cmd\",\"id\":7,\"payload\":{\"c\":\"" + std::string (200, 'x') + "\"}}") == 0 && has (r.last (), "\"code\":400"));
		r.f.token = "";					// unpaired elsewhere (revoke)
		CHECK (r.msg ("{\"type\":\"cmd\",\"id\":8,\"payload\":{\"c\":\"notes=0\"}}") == 0 && has (r.last (), "\"code\":401") && !r.s->authed ());
		CHECK (r.f.cmds.size () == 2);
	}
	{	// wrong tokens close the connection; so do malformed messages; the rate limit
		Rig r;
		uint16_t c = 0;
		for (int i = 0; i < 5; i++) c = r.msg ("{\"type\":\"auth\",\"id\":1,\"payload\":{\"token\":\"x\"}}");
		CHECK (c == 1008);
		Rig b;
		for (int i = 0; i < 9; i++) CHECK (b.msg ("{nope") == 0);
		CHECK (b.msg ("[1]") == 1008);
		Rig q;
		int nOk = 0, nBusy = 0;
		for (int i = 0; i < 25; i++)
		{
			q.msg ("{\"type\":\"ping\",\"id\":1}", 1000);
			if (has (q.last (), "\"ok\":true")) nOk++;
			if (has (q.last (), "\"code\":429")) nBusy++;
		}
		CHECK (nOk == 20 && nBusy == 5);
		q.msg ("{\"type\":\"ping\",\"id\":1}", 1050);
		CHECK (has (q.last (), "\"ok\":true"));
		q.msg ("{\"type\":\"ping\",\"id\":1}", 1050);
		CHECK (has (q.last (), "\"code\":429"));
		q.msg ("{\"type\":\"ack\",\"seq\":0}", 1050);	// acks are not rate-limited
		CHECK (has (q.last (), "\"code\":429"));
	}
	{	// topics: only the served ones in the hello, the rest "later"; each topic's own range and silence gap
		Rig r (8192, 1u << wsproto::kLevels);
		CHECK (has (r.f.sent[0], "\"topics\":[\"status\",\"levels\"]") && has (r.f.sent[0], "\"later\":[\"seq\",\"seqpos\",\"log\",\"voices\",\"usbMode\""));
		CHECK (has (r.f.sent[0], "\"rates\":{\"status\":[250,1000],\"levels\":[33,40]}"));
		CHECK (wsjson::valid_object (r.f.sent[0].data (), r.f.sent[0].size ()));
		r.msg ("{\"type\":\"sub\",\"id\":1,\"payload\":{\"topics\":[\"levels\",\"seq\"],\"ms\":1}}");
		CHECK (has (r.last (), "\"topics\":[\"levels\"],\"later\":[\"seq\"],\"unknown\":[],\"ms\":33"));
		CHECK (r.s->interval (wsproto::kLevels) == 33);
		r.s->tick (0);
		CHECK (has (r.last (), "\"topic\":\"levels\",\"seq\":1,\"payload\":{\"x\":1}"));
		r.msg ("{\"type\":\"ack\",\"seq\":1}");
		for (uint32_t t = 33; t < 1000; t += 33) r.s->tick (t);	// unchanged: silent up to its 1 s gap
		CHECK (r.s->seq () == 1);
		r.s->tick (1030);
		CHECK (r.s->seq () == 2);
		r.msg ("{\"type\":\"sub\",\"id\":2,\"payload\":{\"topics\":[\"levels\"]}}");
		CHECK (r.s->interval (wsproto::kLevels) == 40);			// no ms: the topic's default
	}
	{	// a full send queue holds pushes back like a full window; pings are charged to the bucket
		Rig r;
		r.msg ("{\"type\":\"sub\",\"id\":1,\"payload\":{\"topics\":[\"status\"]}}");
		r.f.room = false;
		r.s->tick (0);
		CHECK (r.s->seq () == 0 && r.s->stats ().coalesced == 1);
		r.f.room = true;
		r.s->tick (5);
		CHECK (r.s->seq () == 1);
		int nOk = 0;
		for (int i = 0; i < 30; i++) nOk += r.s->control (100);
		CHECK (nOk == 20 && !r.s->control (100) && r.s->control (150));	// a full bucket, then one per 50 ms
	}
	{	// a status too large for the buffer: an error, the topic dropped; a failed send ends the session
		Rig r (600);
		r.f.status = "{\"x\":\"" + std::string (700, 'y') + "\"}";
		r.msg ("{\"type\":\"sub\",\"id\":1,\"payload\":{\"topics\":[\"status\"]}}");
		r.s->tick (0);
		CHECK (has (r.last (), "\"code\":500") && !r.s->subscribed (wsproto::kStatus) && !r.s->dead ());
		r.f.fail = true;
		r.msg ("{\"type\":\"ping\",\"id\":1}");
		CHECK (r.s->dead () && r.msg ("{\"type\":\"ping\",\"id\":1}") == 1011);
	}
}

static void test_cmd_policy ()
{
	CHECK (webcmd::refused ("notes=1", false, false) == 0);
	CHECK (webcmd::refused ("wifi=join:a:b", true, false) == 0);
	CHECK (webcmd::refused ("wifi=join:a:b", false, false) != 0);
	CHECK (webcmd::refused ("midibridge=on", true, false) != 0);
	CHECK (webcmd::refused ("usbmidi=os:on", true, false) != 0);
	CHECK (webcmd::refused ("osupdate=1", true, false) != 0);
	CHECK (webcmd::refused ("link=tcp:1.2.3.4:5", true, false) == 0);
	CHECK (webcmd::refused ("link=tcp:1.2.3.4:5", true, true) != 0);
	CHECK (webcmd::refused ("link=tcp", true, true) == 0);
	CHECK (webcmd::refused ("wdtest", true, false) != 0);
	CHECK (webcmd::refused ("", true, false) != 0);
	CHECK (webcmd::refused ("usbmode=overbridge", true, false) == 0);	// web.cpp's strict origin check, not this list
}

static void test_topics ()
{
	// the sequencer view: valid JSON, notes as arrays, only busy steps listed
	sidecar_seq_view_t *v = new sidecar_seq_view_t ();
	char *buf = new char[64 * 1024];
	CHECK (seqjson::view (*v, buf, 64 * 1024) > 0 && std::string (buf) == "{\"valid\":false}");
	v->valid = 1; v->track = 8; v->tracks[0] = 8; v->tracks[1] = 9; v->tracks[2] = v->tracks[3] = -1;
	v->pattern = 3; strcpy (v->project, "SHARED \"SET\""); strcpy (v->machine, "6-OP FM");
	v->length = 32; v->npages = 2; v->transport = 1; v->bpm10 = 1200; v->gen = 7;
	strcpy (v->chord[0], "OFF"); strcpy (v->chord[1], "MAJ");
	v->n = 2;
	v->note[0] = {0, -12, 60, 100, 0, 96};
	v->note[1] = {5, 0, 64, 90, 2, 48};
	v->locks[5] = 3; v->fx_val[9] = -4;
	size_t n = seqjson::view (*v, buf, 64 * 1024);
	CHECK (n > 0 && wsjson::valid_object (buf, n));
	std::string s (buf, n);
	CHECK (has (s, "\"project\":\"SHARED \\\"SET\\\"\""));
	CHECK (has (s, "\"notes\":[[0,-12,60,100,96,0],[5,0,64,90,48,2]]"));
	CHECK (has (s, "\"steps\":[[5,3,0,0,0,0,0,0,0,0],[9,0,0,0,0,0,0,-4,0,0]]"));
	CHECK (has (s, "\"tracks\":[8,9,-1,-1]") && has (s, "\"bpm10\":1200"));
	// the worst case fits the connection's buffer (web_ws.h kOutMax)
	v->n = SEQED_NOTES; v->length = SEQED_STEPS;
	for (int k = 0; k < SEQED_NOTES; k++) v->note[k] = {(unsigned short) (k % 128), -96, 127, 127, 3, 65535};
	for (int k = 0; k < SEQED_STEPS; k++) { v->locks[k] = 12; v->cond[k] = 1; v->xstep[k] = 31; v->rt_cnt[k] = 8; v->rt_mode[k] = 3; v->fx_cmd[k] = 15; v->fx_val[k] = -128; v->lcond[k] = 64; v->arpl[k] = 65535; }
	n = seqjson::view (*v, buf, 40 * 1024);
	CHECK (n > 0 && wsjson::valid_object (buf, n));
	CHECK (seqjson::pos (12345, 8, 1, 3, 1200, buf, 100) > 0 && std::string (buf) == "{\"pos\":12345,\"track\":8,\"transport\":1,\"pattern\":3,\"bpm10\":1200}");
	CHECK (seqjson::pos (-1, -1, 0, -1, 0, buf, 100) > 0 && std::string (buf) == "{\"pos\":-1,\"track\":-1,\"transport\":0,\"pattern\":-1,\"bpm10\":0}");
	CHECK (seqjson::view (*v, buf, 100) == 0);		// does not fit: 0
	delete v;

	// the peak fan: each reader gets every peak once
	peakfan::Fan *f = new peakfan::Fan ();
	float pk[8][2] = {}, out[8][2];
	pk[2][1] = 0.5f;
	f->feed_voices (pk);
	f->feed_master (0.25f);
	f->take_voices (peakfan::kDisplay, out);
	CHECK (out[2][1] == 0.5f);
	f->take_voices (peakfan::kDisplay, out);
	CHECK (out[2][1] == 0.0f);				// taken
	f->take_voices (peakfan::kWeb, out);
	CHECK (out[2][1] == 0.5f);				// the web's share was not stolen
	pk[2][1] = 0.1f;
	f->feed_voices (pk);
	pk[2][1] = 0.3f;
	f->feed_voices (pk);
	f->take_voices (peakfan::kWeb, out);
	CHECK (out[2][1] == 0.3f);				// the max since the last take
	CHECK (f->take_master (peakfan::kStats) == 0.25f && f->take_master (peakfan::kStats) == 0.0f && f->take_master (peakfan::kWeb) == 0.25f);
	delete f;

	// levels and log payloads
	float vp[8][2] = {{0.1f, 0.2f}, {1.5f, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0.0005f, 0}};
	n = wstopics::levels (vp, nullptr, nullptr, 0.625f, buf, 1000);
	CHECK (n > 0 && wsjson::valid_object (buf, n));
	CHECK (std::string (buf) == "{\"master\":[0.625,0.625],\"strips\":[0.200,1.000,0.000,0.000,0.000,0.000,0.000,0.001,null,null,null,0.625]}");
	{
		const float trk[8] = {0.5f, 0, 0.25f, 0, 0, 0, 0, 0}, bus[3] = {0.125f, 2.0f, 0};
		n = wstopics::levels (vp, trk, bus, 0.625f, buf, 1000);
		CHECK (std::string (buf) == "{\"master\":[0.625,0.625],\"strips\":[0.500,1.000,0.250,0.000,0.000,0.000,0.000,0.001,0.125,1.000,0.000,0.625]}");
		peakfan::Fan *g = new peakfan::Fan;
		const float x1[11] = {0.1f, 0, 0, 0, 0, 0, 0, 0, 0.3f, 0, 0}, x2[3] = {0.2f, 0, 0};
		g->feed_extra (x1, 0, 11);
		g->feed_extra (x2, 8, 3);
		float o[11];
		g->take_extra (peakfan::kWeb, o);
		CHECK (o[0] == 0.1f && o[8] == 0.3f);
		g->take_extra (peakfan::kWeb, o);
		CHECK (o[0] == 0.0f && o[8] == 0.0f);
		g->take_extra (peakfan::kWeb2, o);
		CHECK (o[8] == 0.3f);			// the other connection's share is its own
		delete g;
	}
	const char t[] = "a \"b\"\\\n\x01\xe9z";
	n = wstopics::log (100, 109, true, t, sizeof t - 1, buf, 1000);
	CHECK (n > 0 && wsjson::valid_object (buf, n));
	CHECK (std::string (buf) == "{\"from\":100,\"to\":109,\"gap\":true,\"text\":\"a \\\"b\\\"\\\\\\n\\u0001?z\"}");
	CHECK (wstopics::log (0, 5, false, "hello", 5, buf, 20) == 0);	// does not fit
	delete[] buf;
}


static void test_voices_and_next ()
{
	char buf[4096];
	sidecar_voice_view_t *v = new sidecar_voice_view_t;
	memset (v, 0, sizeof *v);
	size_t n = voicejson::view (*v, buf, sizeof buf);
	CHECK (std::string (buf, n) == "{\"valid\":false}");
	v->valid = 1; v->track = 2; v->machine = -1; v->sidecar = 1;
	memcpy (v->name, "FM \"X\"         Z", 16);			// not terminated: 16 characters at most
	for (int k = 0; k < 8; k++) v->val[k] = (unsigned char) (k * 10);
	memcpy (v->label[0], "LEV", 4); memcpy (v->label[1], "TUNE!", 5);
	v->pattern = 3; v->steps = 2; v->locks[1] = 4; v->trig[0] = 1;
	v->mode = SVV_FM; v->fm_alg = 4; v->fm_fb = 7; v->fm_car = 5; v->fm_fb_src = -1; v->fm_col[0] = -2;
	memcpy (v->fm_freq[0], "1.00", 5);
	v->gen = 99;
	n = voicejson::view (*v, buf, sizeof buf);
	CHECK (n > 0 && wsjson::valid_object (buf, n));
	CHECK (has (buf, "\"track\":2,\"machine\":-1,\"sidecar\":true,\"name\":\"FM \\\"X\\\"         Z\""));
	CHECK (has (buf, "\"val\":[0,10,20,30,40,50,60,70],\"label\":[\"LEV\",\"TUNE!\",\"\""));
	CHECK (has (buf, "\"pattern\":3,\"locks\":[0,4],\"trig\":[1,0],\"mode\":\"fm\",\"fm\":{\"name\":\"\",\"alg\":4,\"fb\":7"));
	CHECK (has (buf, "\"col\":[-2,0,0,0,0,0]") && has (buf, "\"fbSrc\":-1") && has (buf, "\"freq\":[\"1.00\",\"\""));
	CHECK (has (buf, "\"gen\":99}"));
	v->mode = SVV_MULTI; v->ms_nz = 99; v->steps = 999;		// clamped
	n = voicejson::view (*v, buf, sizeof buf);
	CHECK (n > 0 && wsjson::valid_object (buf, n) && has (buf, "\"multi\":{\"inst\":\"\",\"ready\":false"));
	v->mode = SVV_DAISY; v->dz_n = 2; v->dz_ratio[1] = 271;
	n = voicejson::view (*v, buf, sizeof buf);
	CHECK (has (buf, "\"daisy\":{\"model\":0,\"ratio\":[0,271],\"ring\":[0,0]}"));
	CHECK (voicejson::view (*v, buf, 40) == 0);			// does not fit
	delete v;

	// /next from the card
	char p[160];
	CHECK (webnext::path ("/next", p, sizeof p) && std::string (p) == "SD:/.rytm/www-next/index.html");
	CHECK (webnext::path ("/next/", p, sizeof p) && std::string (p) == "SD:/.rytm/www-next/index.html");
	CHECK (webnext::path ("/next/assets/index-Ab_9.js", p, sizeof p) && std::string (p) == "SD:/.rytm/www-next/assets/index-Ab_9.js");
	const char *bad[] = {"/nextx", "/nex", "/next/../x", "/next/a/../b", "/next/.hidden", "/next/a//b", "/next/a/", "/next//a",
			     "/next/a b", "/next/a%2e", "/next/a?x=1", "/next/a\\b", "/fs/x", "/"};
	for (const char *b : bad) CHECK (!webnext::path (b, p, sizeof p));
	std::string longp = "/next/" + std::string (121, 'a');
	CHECK (!webnext::path (longp.c_str (), p, sizeof p));
	CHECK (!webnext::path ("/next/index.html", p, 20));		// does not fit the buffer
	CHECK (std::string (webnext::type ("a/b.js")) == "text/javascript; charset=utf-8");
	CHECK (std::string (webnext::type ("x.woff2")) == "font/woff2" && std::string (webnext::type ("x")) == "application/octet-stream");
	CHECK (std::string (webnext::type ("x.exe")) == "application/octet-stream");
	CHECK (webnext::immutable ("/next/assets/x.js") && !webnext::immutable ("/next/index.html"));
	CHECK (std::string (webnext::headers ("a/index.html", false, false)) == "text/html; charset=utf-8\r\nCache-Control: no-cache");
	CHECK (std::string (webnext::headers ("x.js", true, true)) == "text/javascript; charset=utf-8\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nCache-Control: public, max-age=31536000, immutable");
	CHECK (std::string (webnext::headers ("noext", false, true)) == "application/octet-stream\r\nCache-Control: public, max-age=31536000, immutable");
	CHECK (std::string (webnext::headers ("x.exe", true, false)).rfind ("application/octet-stream\r\nContent-Encoding: gzip", 0) == 0);
}

int main ()
{
	test_handshake ();
	test_frames ();
	test_json ();
	test_session ();
	test_cmd_policy ();
	test_topics ();
	test_voices_and_next ();
	printf ("test_ws: OK\n");
	return 0;
}
