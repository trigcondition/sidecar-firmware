// pi/engine/ws_json.h - just enough JSON for the /ws messages a client sends (docs/web-ws.md "Envelope"): validate a
// whole message, find a member by a dotted path ("payload.c"), read a string (unescaped, \u included), an unsigned
// number, the strings of an array; and escape a string for an answer. Portable, header only, no allocation, a fixed
// nesting limit (tests/test_ws.cpp). Not a general JSON library: numbers are only read as unsigned integers.
#ifndef _ws_json_h
#define _ws_json_h

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wsjson {

static const int kDepth = 8;

struct Span { const char *p; size_t n; };		// a value's text, quotes included for a string

inline const char *ws (const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
	return p;
}

inline int hex (char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// past one string (p at its opening quote); 0 if malformed
inline const char *skip_string (const char *p, const char *e)
{
	if (p >= e || *p != '"') return 0;
	for (p++; p < e; p++)
	{
		unsigned char c = (unsigned char) *p;
		if (c == '"') return p + 1;
		if (c < 0x20) return 0;
		if (c == '\\')
		{
			if (++p >= e) return 0;
			if (*p == 'u')
			{
				if (e - p < 5) return 0;
				for (int i = 1; i <= 4; i++) if (hex (p[i]) < 0) return 0;
				p += 4;
			}
			else if (!strchr ("\"\\/bfnrt", *p)) return 0;
		}
	}
	return 0;
}

inline const char *skip_value (const char *p, const char *e, int depth);

inline const char *skip_number (const char *p, const char *e)
{
	const char *s = p;
	if (p < e && *p == '-') p++;
	if (p >= e) return 0;
	if (*p == '0') p++;
	else if (*p >= '1' && *p <= '9') while (p < e && *p >= '0' && *p <= '9') p++;
	else return 0;
	if (p < e && *p == '.')
	{
		p++;
		if (p >= e || *p < '0' || *p > '9') return 0;
		while (p < e && *p >= '0' && *p <= '9') p++;
	}
	if (p < e && (*p == 'e' || *p == 'E'))
	{
		p++;
		if (p < e && (*p == '+' || *p == '-')) p++;
		if (p >= e || *p < '0' || *p > '9') return 0;
		while (p < e && *p >= '0' && *p <= '9') p++;
	}
	return p > s ? p : 0;
}

inline const char *skip_value (const char *p, const char *e, int depth)
{
	p = ws (p, e);
	if (p >= e) return 0;
	if (*p == '"') return skip_string (p, e);
	if (*p == '{' || *p == '[')
	{
		if (depth >= kDepth) return 0;
		char close = *p == '{' ? '}' : ']';
		bool obj = *p == '{';
		p = ws (p + 1, e);
		if (p < e && *p == close) return p + 1;
		for (;;)
		{
			if (obj)
			{
				p = skip_string (ws (p, e), e);
				if (!p) return 0;
				p = ws (p, e);
				if (p >= e || *p != ':') return 0;
				p++;
			}
			p = skip_value (p, e, depth + 1);
			if (!p) return 0;
			p = ws (p, e);
			if (p >= e) return 0;
			if (*p == close) return p + 1;
			if (*p != ',') return 0;
			p++;
		}
	}
	if (e - p >= 4 && memcmp (p, "true", 4) == 0) return p + 4;
	if (e - p >= 5 && memcmp (p, "false", 5) == 0) return p + 5;
	if (e - p >= 4 && memcmp (p, "null", 4) == 0) return p + 4;
	return skip_number (p, e);
}

// The whole text is one JSON object and nothing else
inline bool valid_object (const char *p, size_t n)
{
	const char *e = p + n, *s = ws (p, e);
	if (s >= e || *s != '{') return false;
	const char *q = skip_value (s, e, 0);
	return q != 0 && ws (q, e) == e;
}

// a key (raw, between its quotes) equals k[0..kn) - keys with escapes never match
inline bool key_is (const char *q, const char *qe, const char *k, size_t kn)
{
	return (size_t) (qe - q) == kn && memcmp (q, k, kn) == 0 && !memchr (q, '\\', kn);
}

// The member at a dotted path of an object (valid_object first); false if absent
inline bool find (const char *p, size_t n, const char *path, Span *out)
{
	const char *e = p + n;
	p = ws (p, e);
	while (*path)
	{
		const char *dot = strchr (path, '.');
		size_t kn = dot ? (size_t) (dot - path) : strlen (path);
		if (p >= e || *p != '{') return false;
		p = ws (p + 1, e);
		bool found = false;
		while (p < e && *p != '}')
		{
			const char *ks = p + 1, *ke = skip_string (p, e);
			if (!ke) return false;
			p = ws (ke, e) + 1;			// past ':'
			p = ws (p, e);
			const char *v = p, *ve = skip_value (p, e, 0);
			if (!ve) return false;
			if (key_is (ks, ke - 1, path, kn))
			{
				p = v;
				out->p = v;
				out->n = (size_t) (ve - v);
				found = true;
				break;			// the first one wins
			}
			p = ws (ve, e);
			if (p < e && *p == ',') p = ws (p + 1, e);
		}
		if (!found) return false;
		path += kn;
		if (*path == '.') path++;
	}
	return true;
}

inline void put_utf8 (uint32_t cp, char *o, size_t *k, size_t max, bool *ok)
{
	char b[4];
	size_t n;
	if (cp < 0x80) { b[0] = (char) cp; n = 1; }
	else if (cp < 0x800) { b[0] = (char) (0xC0 | cp >> 6); b[1] = (char) (0x80 | (cp & 63)); n = 2; }
	else if (cp < 0x10000) { b[0] = (char) (0xE0 | cp >> 12); b[1] = (char) (0x80 | (cp >> 6 & 63)); b[2] = (char) (0x80 | (cp & 63)); n = 3; }
	else { b[0] = (char) (0xF0 | cp >> 18); b[1] = (char) (0x80 | (cp >> 12 & 63)); b[2] = (char) (0x80 | (cp >> 6 & 63)); b[3] = (char) (0x80 | (cp & 63)); n = 4; }
	if (*k + n >= max) { *ok = false; return; }
	memcpy (o + *k, b, n);
	*k += n;
}

// A string value, unescaped, into pOut (max bytes with the terminator); false if not a string, too long, or it holds
// a NUL or a lone surrogate
inline bool str (const Span &s, char *pOut, size_t max)
{
	if (s.n < 2 || s.p[0] != '"' || max == 0) return false;
	const char *p = s.p + 1, *e = s.p + s.n - 1;
	size_t k = 0;
	bool ok = true;
	while (p < e && ok)
	{
		char c = *p++;
		if (c != '\\')
		{
			if (k + 1 >= max) return false;
			pOut[k++] = c;
			continue;
		}
		c = *p++;
		switch (c)
		{
		case 'b': c = '\b'; break;
		case 'f': c = '\f'; break;
		case 'n': c = '\n'; break;
		case 'r': c = '\r'; break;
		case 't': c = '\t'; break;
		case 'u':
		{
			uint32_t cp = (uint32_t) (hex (p[0]) << 12 | hex (p[1]) << 8 | hex (p[2]) << 4 | hex (p[3]));
			p += 4;
			if (cp >= 0xD800 && cp <= 0xDBFF)
			{
				if (e - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
				uint32_t lo = (uint32_t) (hex (p[2]) << 12 | hex (p[3]) << 8 | hex (p[4]) << 4 | hex (p[5]));
				if (lo < 0xDC00 || lo > 0xDFFF) return false;
				p += 6;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			}
			else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
			if (cp == 0) return false;
			put_utf8 (cp, pOut, &k, max, &ok);
			continue;
		}
		default: break;		// " \ /
		}
		if (k + 1 >= max) return false;
		pOut[k++] = c;
	}
	if (!ok) return false;
	pOut[k] = 0;
	return true;
}

// An unsigned integer value (no sign, fraction or exponent) up to 2^32 - 1
inline bool u32 (const Span &s, uint32_t *pOut)
{
	if (s.n == 0 || s.n > 10) return false;
	uint64_t v = 0;
	for (size_t i = 0; i < s.n; i++)
	{
		if (s.p[i] < '0' || s.p[i] > '9') return false;
		v = v * 10 + (uint64_t) (s.p[i] - '0');
	}
	if (v > 0xFFFFFFFFu || (s.n > 1 && s.p[0] == '0')) return false;
	*pOut = (uint32_t) v;
	return true;
}

// The elements of an array one at a time: *pPos 0 to start; false at its end (or if s is not an array)
inline bool next_elem (const Span &s, size_t *pPos, Span *out)
{
	if (s.n < 2 || s.p[0] != '[') return false;
	const char *e = s.p + s.n - 1;			// at the ']'
	const char *p = ws (s.p + (*pPos ? *pPos : 1), e);
	if (p < e && *p == ',') p = ws (p + 1, e);
	if (p >= e) return false;
	const char *ve = skip_value (p, e, 0);
	if (!ve) return false;
	out->p = p;
	out->n = (size_t) (ve - p);
	*pPos = (size_t) (ve - s.p);
	return true;
}

// pIn as a JSON string's inside (no quotes) into pOut (max with the terminator); control characters as \u00XX,
// " and \ escaped; cut (at a whole UTF-8 sequence) if it does not fit. The length written
inline size_t escape (const char *pIn, char *pOut, size_t max)
{
	size_t k = 0;
	if (max == 0) return 0;
	for (const unsigned char *p = (const unsigned char *) pIn; *p; p++)
	{
		char b[7];
		size_t n = 1;
		if (*p == '"' || *p == '\\') { b[0] = '\\'; b[1] = (char) *p; n = 2; }
		else if (*p < 0x20)
		{
			static const char kHex[] = "0123456789abcdef";
			b[0] = '\\'; b[1] = 'u'; b[2] = '0'; b[3] = '0'; b[4] = kHex[*p >> 4]; b[5] = kHex[*p & 15]; n = 6;
		}
		else b[0] = (char) *p;
		if (k + n >= max)
		{
			while (k > 0 && ((unsigned char) pOut[k - 1] & 0xC0) == 0x80) k--;	// no half a sequence
			if (k > 0 && ((unsigned char) pOut[k - 1] & 0xC0) == 0xC0) k--;
			break;
		}
		memcpy (pOut + k, b, n);
		k += n;
	}
	pOut[k] = 0;
	return k;
}

}	// namespace wsjson

#endif
