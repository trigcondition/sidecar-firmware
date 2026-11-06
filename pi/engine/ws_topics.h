// pi/engine/ws_topics.h - the /ws topics' payloads that are not /status (docs/web-ws.md "Topics"): "levels" (the
// output's peaks, peak_fan.h) and "log" (the console log's tail). The sequencer's are seqview_json.h. Portable, header
// only (tests/test_ws.cpp).
#ifndef _ws_topics_h
#define _ws_topics_h

#include "ws_session.h"

namespace wstopics {

// a peak 0..1 with three decimals ("0.625")
inline void peak (wsproto::Writer &w, float p)
{
	if (!(p > 0.0f)) p = 0.0f;			// (NaN too)
	if (p > 1.0f) p = 1.0f;
	unsigned m = (unsigned) (p * 1000.0f + 0.5f);
	char b[8] = {(char) ('0' + m / 1000), '.', (char) ('0' + m / 100 % 10), (char) ('0' + m / 10 % 10), (char) ('0' + m % 10), 0};
	w.s (b);
}

// {"master":[L,R],"strips":[V1..V8, B1, B2, B3, M]}: a voice's strip is the largest of its OUT pair (L, R) and its own
// track (trk, 0 when the voice is not on a track); the buses' are bus (null: none, a build without send FX); the
// master is the whole output's peak (Output::TakePeak gives one value: L = R)
inline size_t levels (const float (*voices)[2], const float *trk, const float *bus, float master, char *pOut, size_t max)
{
	wsproto::Writer w (pOut, max);
	w.s ("{\"master\":[");
	peak (w, master); w.s (","); peak (w, master);
	w.s ("],\"strips\":[");
	for (int v = 0; v < 8; v++)
	{
		float p = voices[v][0] > voices[v][1] ? voices[v][0] : voices[v][1];
		if (trk && trk[v] > p) p = trk[v];
		peak (w, p); w.s (",");
	}
	for (int b = 0; b < 3; b++) { if (bus) peak (w, bus[b]); else w.s ("null"); w.s (","); }
	peak (w, master);
	w.s ("]}");
	return w.ok ? w.n : 0;
}

// {"from":N,"to":M,"text":"..."}: the log's bytes from..to (CLogTee::ReadSince's positions), escaped; "gap": bytes
// before from were lost (the ring overwrote them, or the client asked for more than one push holds)
inline size_t log (unsigned long long from, unsigned long long to, bool gap, const char *text, size_t n, char *pOut, size_t max)
{
	wsproto::Writer w (pOut, max);
	char b[24];
	auto num = [&] (unsigned long long v) { int k = 23; b[k] = 0; do { b[--k] = (char) ('0' + v % 10); v /= 10; } while (v); w.s (b + k); };
	w.s ("{\"from\":"); num (from);
	w.s (",\"to\":"); num (to);
	w.s (",\"gap\":").s (gap ? "true" : "false").s (",\"text\":\"");
	static const char kHex[] = "0123456789abcdef";
	for (size_t k = 0; k < n && w.ok; k++)
	{
		unsigned char c = (unsigned char) text[k];
		char e[7];
		if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char) c; e[2] = 0; }
		else if (c == '\n') { e[0] = '\\'; e[1] = 'n'; e[2] = 0; }
		else if (c < 0x20 || c == 0x7f) { e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = kHex[c >> 4]; e[5] = kHex[c & 15]; e[6] = 0; }
		else if (c >= 0x80) { e[0] = '?'; e[1] = 0; }	// the log is ASCII; anything else must not break the UTF-8 frame
		else { e[0] = (char) c; e[1] = 0; }
		w.s (e);
	}
	w.s ("\"}");
	return w.ok ? w.n : 0;
}

}	// namespace wstopics

#endif
