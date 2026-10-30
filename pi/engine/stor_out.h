// pi/engine/stor_out.h - a small text appender for the storage layer (stor_vol.cpp, stor_bench.cpp): the kernel links
// no printf family (newlib's libm only; power_state.cpp has the same idea). Cut at cap - 1, the full length counted.
#pragma once
#include <stdint.h>

namespace stor {

struct Out
{
	char *p;
	int cap, len = 0;
	Out (char *pOut, int nCap) : p (pOut), cap (nCap) { if (cap > 0) p[0] = 0; }
	Out &c (char ch)
	{
		if (len + 1 < cap) { p[len] = ch; p[len + 1] = 0; }
		len++;
		return *this;
	}
	Out &s (const char *t) { if (t) for (; *t; t++) c (*t); return *this; }
	Out &u (uint64_t v)
	{
		char b[24];
		int i = 23;
		b[i] = 0;
		do b[--i] = (char) ('0' + v % 10); while (v /= 10);
		return s (b + i);
	}
	Out &i (int64_t v) { if (v < 0) { c ('-'); v = -v; } return u ((uint64_t) v); }
	// v / 10^dec with dec decimals ("12.34" from 1234, 2)
	Out &fix (uint64_t v, int dec)
	{
		uint64_t m = 1;
		for (int k = 0; k < dec; k++) m *= 10;
		u (v / m);
		if (dec <= 0) return *this;
		c ('.');
		uint64_t f = v % m;
		for (uint64_t d = m / 10; d; d /= 10) c ((char) ('0' + f / d % 10));
		return *this;
	}
	// t right-aligned in w columns
	Out &pad (const char *t, int w)
	{
		int n = 0;
		while (t[n]) n++;
		for (; n < w; n++) c (' ');
		return s (t);
	}
	Out &b (bool v) { return s (v ? "true" : "false"); }
};

}	// namespace stor
