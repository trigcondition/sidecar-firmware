// pi/engine/ws_frame.h - the WebSocket wire format (RFC 6455) for the Pi's /ws (docs/web-ws.md): the opening
// handshake's accept key (SHA-1 + base64), the server's frame header, and an incremental frame parser for the client's
// frames (masking, fragmentation, ping/pong/close between fragments, UTF-8 check, size limits). Portable, header only,
// no allocation: the caller owns the buffers (tests/test_ws.cpp). Text frames only: a binary frame closes with 1003.
#ifndef _ws_frame_h
#define _ws_frame_h

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wsframe {

// ---- SHA-1 (FIPS 180-4), only for the handshake's accept key: not used for anything secret
struct Sha1
{
	uint32_t h[5];
	uint64_t len;
	uint8_t blk[64];
	unsigned n;
	Sha1 () { h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE; h[3] = 0x10325476; h[4] = 0xC3D2E1F0; len = 0; n = 0; }
	static uint32_t rol (uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
	void block (const uint8_t *p)
	{
		uint32_t w[80];
		for (int i = 0; i < 16; i++) w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16 | (uint32_t) p[4 * i + 2] << 8 | p[4 * i + 3];
		for (int i = 16; i < 80; i++) w[i] = rol (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++)
		{
			uint32_t f, k;
			if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			uint32_t t = rol (a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol (b, 30); b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	void update (const void *pData, size_t nLen)
	{
		const uint8_t *p = (const uint8_t *) pData;
		len += nLen;
		while (nLen--)
		{
			blk[n++] = *p++;
			if (n == 64) { block (blk); n = 0; }
		}
	}
	void final (uint8_t out[20])
	{
		uint64_t bits = len * 8;
		uint8_t pad = 0x80;
		update (&pad, 1);
		pad = 0;
		while (n != 56) update (&pad, 1);
		uint8_t l[8];
		for (int i = 0; i < 8; i++) l[i] = (uint8_t) (bits >> (56 - 8 * i));
		update (l, 8);
		for (int i = 0; i < 5; i++) { out[4 * i] = h[i] >> 24; out[4 * i + 1] = h[i] >> 16; out[4 * i + 2] = h[i] >> 8; out[4 * i + 3] = h[i]; }
	}
};

// base64 (RFC 4648, with padding) of n bytes into pOut (4 * ceil(n / 3) + 1 bytes); the length written
inline size_t base64 (const uint8_t *p, size_t n, char *pOut)
{
	static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3)
	{
		uint32_t v = (uint32_t) p[i] << 16 | (i + 1 < n ? (uint32_t) p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		pOut[o++] = k[v >> 18 & 63];
		pOut[o++] = k[v >> 12 & 63];
		pOut[o++] = i + 1 < n ? k[v >> 6 & 63] : '=';
		pOut[o++] = i + 2 < n ? k[v & 63] : '=';
	}
	pOut[o] = 0;
	return o;
}

// The client's Sec-WebSocket-Key: 24 base64 characters (16 random bytes), the last two "==" (RFC 6455 4.1)
inline bool key_ok (const char *pKey)
{
	if (pKey == 0 || strlen (pKey) != 24 || pKey[22] != '=' || pKey[23] != '=') return false;
	for (int i = 0; i < 22; i++)
	{
		char c = pKey[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) return false;
	}
	return true;
}

// Sec-WebSocket-Accept for a key: base64 (SHA-1 (key + the RFC's GUID)), 28 characters + the terminator
inline bool accept_key (const char *pKey, char pOut[29])
{
	if (!key_ok (pKey)) return false;
	static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	Sha1 s;
	s.update (pKey, 24);
	s.update (kGuid, sizeof kGuid - 1);
	uint8_t d[20];
	s.final (d);
	base64 (d, 20, pOut);
	return true;
}

// "websocket" in any case (the Upgrade header's value; a list such as "websocket, foo" is not accepted)
inline bool is_websocket (const char *p)
{
	static const char k[] = "websocket";
	if (p == 0) return false;
	for (int i = 0; i < 9; i++) if ((p[i] | 0x20) != k[i]) return false;
	return p[9] == 0;
}

enum Op { kCont = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

// Close codes (RFC 6455 7.4.1)
enum Code { kNormal = 1000, kGoingAway = 1001, kProtocol = 1002, kUnsupported = 1003, kBadData = 1007,
	    kPolicy = 1008, kTooBig = 1009, kInternal = 1011 };

// The server's frame header (FIN set, not masked) for a payload of n bytes into pOut (at most 10 bytes); its length
inline size_t header (uint8_t *pOut, Op op, size_t n)
{
	pOut[0] = 0x80 | (uint8_t) op;
	if (n < 126) { pOut[1] = (uint8_t) n; return 2; }
	if (n < 65536) { pOut[1] = 126; pOut[2] = (uint8_t) (n >> 8); pOut[3] = (uint8_t) n; return 4; }
	pOut[1] = 127;
	for (int i = 0; i < 8; i++) pOut[2 + i] = (uint8_t) ((uint64_t) n >> (56 - 8 * i));
	return 10;
}

// A close frame's payload: the code (big endian) and a short reason (cut to fit a control frame's 125 bytes)
inline size_t close_payload (uint8_t *pOut, uint16_t code, const char *pReason)
{
	pOut[0] = (uint8_t) (code >> 8);
	pOut[1] = (uint8_t) code;
	size_t n = pReason ? strlen (pReason) : 0;
	if (n > 123) n = 123;
	if (n) memcpy (pOut + 2, pReason, n);
	return 2 + n;
}

// Valid UTF-8 (no overlongs, no surrogates, at most U+10FFFF): a text message must be (RFC 6455 8.1, close 1007)
inline bool utf8_ok (const uint8_t *p, size_t n)
{
	size_t i = 0;
	while (i < n)
	{
		uint8_t c = p[i];
		if (c < 0x80) { i++; continue; }
		unsigned need;
		uint32_t cp;
		if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; if (c < 0xC2) return false; }
		else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; if (c > 0xF4) return false; }
		else return false;
		if (i + need >= n) return false;			// cut short
		for (unsigned k = 1; k <= need; k++)
		{
			if ((p[i + k] & 0xC0) != 0x80) return false;
			cp = cp << 6 | (p[i + k] & 0x3F);
		}
		if (need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
		if (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
		i += need + 1;
	}
	return true;
}

// What the parser hands out, one at a time
struct Event
{
	enum Kind { kNone, kMessage, kPing, kPong, kClose, kError } kind;
	const uint8_t *data;		// the message / the control frame's payload (valid until the next push or next)
	size_t len;
	uint16_t code;			// kClose: the peer's code (1005 when none); kError: the code to close with
};

// The client's frames, fed as the bytes arrive (Push), taken apart with Next. One buffer of In bytes for frames not
// yet complete (a frame's header + payload must fit: In >= Msg + 14), one of Msg for a message being put together
// from fragments. Anything wrong -> one kError event with the close code; the parser stays in error after it.
template <size_t In, size_t Msg>
class Parser
{
public:
	Parser () : m_nIn (0), m_nMsg (0), m_bFrag (false), m_bError (false) {}

	// false: no room (the peer sent more than one frame's worth before it was taken): Next reports kTooBig
	bool push (const void *p, size_t n)
	{
		if (m_bError) return false;
		if (n > In - m_nIn) { m_bOverflow = true; return false; }
		memcpy (m_In + m_nIn, p, n);
		m_nIn += n;
		return true;
	}

	bool next (Event *e)
	{
		for (;;)					// a loop, not a recursion, over a message's fragments
		{
			int r = one (e);
			if (r >= 0) return r != 0;
		}
	}

	bool error () const { return m_bError; }

	// wipe what passed through (a token, a wifi= key), before the buffers are freed
	void wipe () { memset (m_In, 0, sizeof m_In); memset (m_Msg, 0, sizeof m_Msg); memset (m_Ctl, 0, sizeof m_Ctl); m_nIn = m_nMsg = 0; }

private:
	// one frame: 1 an event, 0 nothing (yet), -1 a fragment was taken, go on
	int one (Event *e)
	{
		e->kind = Event::kNone; e->data = 0; e->len = 0; e->code = 0;
		if (m_bError) return 0;
		if (m_bOverflow) return (int) fail (e, kTooBig);
		if (m_nIn < 2) return 0;
		uint8_t b0 = m_In[0], b1 = m_In[1];
		bool fin = b0 & 0x80;
		unsigned op = b0 & 0x0F;
		if (b0 & 0x70) return (int) fail (e, kProtocol);		// no extension was agreed
		if (!(b1 & 0x80)) return (int) fail (e, kProtocol);		// a client's frames are masked (RFC 6455 5.1)
		uint64_t len = b1 & 0x7F;
		size_t h = 2;
		if (len == 126)
		{
			if (m_nIn < 4) return 0;
			len = (uint64_t) m_In[2] << 8 | m_In[3];
			h = 4;
			if (len < 126) return (int) fail (e, kProtocol);	// not the shortest form
		}
		else if (len == 127)
		{
			if (m_nIn < 10) return 0;
			len = 0;
			for (int i = 0; i < 8; i++) len = len << 8 | m_In[2 + i];
			h = 10;
			if (len < 65536 || (len >> 63)) return (int) fail (e, kProtocol);
		}
		bool control = op & 0x8;
		if (control && (!fin || len > 125)) return (int) fail (e, kProtocol);
		if (!control && op != kCont && op != kText && op != kBinary) return (int) fail (e, kProtocol);
		if (control && op != kClose && op != kPing && op != kPong) return (int) fail (e, kProtocol);
		if (len > Msg) return (int) fail (e, kTooBig);
		if (h + 4 + len > In) return (int) fail (e, kTooBig);
		if (m_nIn < h + 4 + len) return 0;			// not all here yet
		const uint8_t *mask = m_In + h;
		uint8_t *pay = m_In + h + 4;
		for (size_t i = 0; i < len; i++) pay[i] ^= mask[i & 3];
		size_t frame = h + 4 + (size_t) len;
		if (control)
		{
			memcpy (m_Ctl, pay, (size_t) len);
			consume (frame);
			e->data = m_Ctl;
			e->len = (size_t) len;
			if (op == kPing) e->kind = Event::kPing;
			else if (op == kPong) e->kind = Event::kPong;
			else
			{
				e->kind = Event::kClose;
				e->code = 1005;
				if (len == 1) return (int) fail (e, kProtocol);
				if (len >= 2)
				{
					e->code = (uint16_t) (m_Ctl[0] << 8 | m_Ctl[1]);
					if (!utf8_ok (m_Ctl + 2, (size_t) len - 2)) return (int) fail (e, kBadData);
					e->data = m_Ctl + 2;
					e->len = (size_t) len - 2;
				}
			}
			return 1;
		}
		if (op == kBinary) return (int) fail (e, kUnsupported);
		if (op == kCont && !m_bFrag) return (int) fail (e, kProtocol);
		if (op == kText && m_bFrag) return (int) fail (e, kProtocol);
		if (op == kText) m_nMsg = 0;
		if (m_nMsg + len > Msg) return (int) fail (e, kTooBig);
		memcpy (m_Msg + m_nMsg, pay, (size_t) len);
		m_nMsg += (size_t) len;
		consume (frame);
		if (!fin) { m_bFrag = true; return -1; }
		m_bFrag = false;
		if (!utf8_ok (m_Msg, m_nMsg)) return (int) fail (e, kBadData);
		m_Msg[m_nMsg] = 0;					// a terminator, for the JSON reader's convenience
		e->kind = Event::kMessage;
		e->data = m_Msg;
		e->len = m_nMsg;
		return 1;
	}

	bool fail (Event *e, Code c) { m_bError = true; e->kind = Event::kError; e->code = (uint16_t) c; return true; }
	void consume (size_t n) { memmove (m_In, m_In + n, m_nIn - n); m_nIn -= n; }

	uint8_t m_In[In];
	size_t m_nIn;
	uint8_t m_Msg[Msg + 1];
	size_t m_nMsg;
	uint8_t m_Ctl[125];
	bool m_bFrag, m_bError, m_bOverflow = false;
};

}	// namespace wsframe

#endif
