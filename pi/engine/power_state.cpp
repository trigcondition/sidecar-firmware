// pi/engine/power_state.cpp - see power_state.h (portable: no libc beyond the language)
#include "power_state.h"

namespace powerstate {

// The one warning the page, the display and the log show; 0 when the supply looks fine
const char *warning (const State &s)
{
	const bool p5 = s.board == 5;
	if (s.known && (s.throttled & kUvNow))
		return p5 ? "power supply too weak (under-voltage now): use the 27 W (5 V 5 A) adapter directly"
			  : "power supply too weak (under-voltage now): use the official 15 W (5.1 V 3 A) supply";
	if (s.max_ma > 0 && s.max_ma < 5000)			// Pi 5 only (the Pi 4 has no max_current)
		return "power supply too weak (no 5 A USB-PD: 3 A mode): use the 27 W (5 V 5 A) adapter directly";
	if (s.known && (s.throttled & kUvBoot))
		return p5 ? "power supply too weak (under-voltage since boot): use the 27 W (5 V 5 A) adapter directly"
			  : "power supply too weak (under-voltage since boot): use the official 15 W (5.1 V 3 A) supply";
	return 0;
}

// The display's short form (its bottom line is about 36 characters at 12 px)
const char *short_warning (const State &s)
{
	if (!warning (s)) return 0;
	return s.board == 5 ? "POWER TOO WEAK: use 27 W adapter" : "POWER TOO WEAK: use 15 W supply";
}

// The kernel links no printf family (newlib's libm only): a small appender, cut at nSize - 1, the full length counted
namespace {
struct Out
{
	char *p; unsigned n, len = 0;
	Out (char *pOut, unsigned nSize) : p (pOut), n (nSize) { if (n) p[0] = 0; }
	Out &s (const char *t) { for (; *t; t++, len++) if (len + 1 < n) { p[len] = *t; p[len + 1] = 0; } return *this; }
	Out &u (unsigned v) { char b[12]; int i = 11; b[i] = 0; do b[--i] = (char) ('0' + v % 10); while (v /= 10); return s (b + i); }
	Out &x (unsigned v) { char b[12]; int i = 11; b[i] = 0; do b[--i] = "0123456789ABCDEF"[v & 15]; while (v >>= 4); return s (b + i); }
	Out &b (bool v) { return s (v ? "true" : "false"); }
	Out &d (bool v) { return s (v ? "1" : "0"); }
};
}

// /status's member, without a leading comma: "power":{...}
int json (const State &st, char *pOut, unsigned nSize)
{
	const char *w = warning (st);
	unsigned t = st.throttled;
	Out o (pOut, nSize);
	o.s ("\"power\":{\"known\":").b (st.known).s (",\"throttled\":").u (t).s (",\"uvNow\":").b (t & kUvNow)
	 .s (",\"uvBoot\":").b (t & kUvBoot).s (",\"throttledNow\":").b (t & kThrNow).s (",\"throttledBoot\":").b (t & kThrBoot)
	 .s (",\"cappedNow\":").b (t & kCapNow).s (",\"cappedBoot\":").b (t & kCapBoot).s (",\"maxCurrent\":");
	if (st.max_ma >= 0) o.u ((unsigned) st.max_ma); else o.s ("null");
	o.s (",\"warn\":");
	if (w) o.s ("\"").s (w).s ("\""); else o.s ("null");	// the warnings hold no character JSON escapes
	o.s ("}");
	return (int) o.len;
}

// The serial log's line (after "power: ")
int line (const State &st, char *pOut, unsigned nSize)
{
	const char *w = warning (st);
	unsigned t = st.throttled;
	Out o (pOut, nSize);
	if (!st.known) o.s ("throttled unknown");
	else o.s ("throttled 0x").x (t).s (" (under-voltage now ").d (t & kUvNow).s (" boot ").d (t & kUvBoot)
	      .s (", throttled now ").d (t & kThrNow).s (" boot ").d (t & kThrBoot).s (", capped now ").d (t & kCapNow)
	      .s (" boot ").d (t & kCapBoot).s (")");
	o.s (" | max_current ");
	if (st.max_ma >= 0) o.u ((unsigned) st.max_ma).s (" mA"); else o.s ("n/a");
	if (w || st.known) o.s (" | ").s (w ? w : "ok");
	return (int) o.len;
}

}	// namespace powerstate
