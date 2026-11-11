// pi/engine/tests/test_spectrum.cpp - the spectrum (spectrum.h): a sine lands in its band at its level, silence is
// the floor, the tap's wrap, the reader's rate, hold and source switch; the engine's taps (spectrum_pi.cpp); the /ws
// topic (ws_session.h's strip, ws_topics.h's payload). Prints the analysis's cost on this machine.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "check.h"
#include "spectrum.h"
#include "spectrum_pi.h"
#include "ws_topics.h"

// the Pi's integer printf (core_port.h), the Mac's here (core/fx/out.cpp's value names)
#include <stdarg.h>
extern "C" int core_snprintf (char *out, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	int r = vsnprintf (out, n, fmt, ap);
	va_end (ap);
	return r;
}
namespace core {
uint32_t g_ms;
uint32_t now_ms() { return g_ms; }
}  // namespace core

static const double kPi = 3.14159265358979323846;

// the band f falls in
static int band_of (double f) { return (int) floor (log (f / spectrum::kLoHz) / log (spectrum::kHiHz / spectrum::kLoHz) * spectrum::kBands); }

static void sine (float *x, int n, double f, double a, double ph = 0.0)
{
	for (int i = 0; i < n; i++) x[i] = (float) (a * sin (2.0 * kPi * f * i / spectrum::kRate + ph));
}

static int argmax (const uint8_t *b) { int m = 0; for (int i = 1; i < spectrum::kBands; i++) if (b[i] > b[m]) m = i; return m; }

static void test_analyser ()
{
	static spectrum::Analyser an;
	static float x[spectrum::kN];
	uint8_t b[spectrum::kBands];
	memset (x, 0, sizeof x);
	an.run (x, b);
	for (int i = 0; i < spectrum::kBands; i++) CHECK (b[i] == 0);		// silence: the floor
	const double fs[] = {50, 100, 440, 1000, 3000, 9000, 15000};
	for (double f : fs)
	{
		sine (x, spectrum::kN, f, 1.0, 0.3);
		an.run (x, b);
		int k = argmax (b);
		CHECK (k >= band_of (f) - 1 && k <= band_of (f) + 1);		// (a sine near an edge: either side)
		double db = spectrum::Analyser::db (b[k]);
		CHECK (db > -3.0 && db <= 0.0);					// full scale: ~0 dB
		CHECK (spectrum::Analyser::db (b[band_of (f < 5000 ? f * 4 : f / 4)]) < -50.0);	// two octaves away: down
		sine (x, spectrum::kN, f, 0.1, 0.3);				// -20 dB
		an.run (x, b);
		db = spectrum::Analyser::db (b[argmax (b)]);
		CHECK (db > -23.0 && db < -17.0);
	}
	// the FFT against a plain DFT: a 2-tone mix, each band's power summed the same way
	{
		for (int i = 0; i < spectrum::kN; i++) x[i] = (float) (0.5 * sin (2 * kPi * 700 * i / 48000.0) + 0.25 * sin (2 * kPi * 5100 * i / 48000.0 + 1));
		an.run (x, b);
		std::vector<double> p (spectrum::kN / 2 + 1);
		for (int k = 0; k <= spectrum::kN / 2; k++)
		{
			double re = 0, im = 0;
			for (int i = 0; i < spectrum::kN; i++)
			{
				double w = 0.5 - 0.5 * cos (2 * kPi * i / spectrum::kN), v = x[i] * w;
				re += v * cos (2 * kPi * k * i / spectrum::kN);
				im -= v * sin (2 * kPi * k * i / spectrum::kN);
			}
			p[k] = re * re + im * im;
		}
		const double df = 48000.0 / spectrum::kN, norm = 1.0 / ((spectrum::kN / 4.0) * (spectrum::kN / 4.0));
		for (int band = 0; band < spectrum::kBands; band++)
		{
			double lo = 20 * pow (1000.0, (double) band / 40), hi = 20 * pow (1000.0, (double) (band + 1) / 40);
			int k0 = (int) ceil (lo / df), k1 = (int) ceil (hi / df);
			double s = 0;
			if (k1 > k0) for (int k = k0; k < k1; k++) s += p[k];
			else { double c = sqrt (lo * hi) / df; int k = (int) c; s = p[k] + (p[k + 1] - p[k]) * (c - k); }
			int want = spectrum::Analyser::quant ((float) (s * norm));
			CHECK (abs (want - b[band]) <= 1);
		}
	}
}

static void test_tap_reader ()
{
	static spectrum::Tap tap;
	static spectrum::Analyser an;
	static float x[spectrum::kN];
	float y[8];
	for (int i = 0; i < 8; i++) y[i] = (float) i;
	for (int k = 0; k < 1000; k++) tap.write (y, 7);		// wraps kSize
	float r[7];
	CHECK (tap.read (r, 7) == 7000 && r[0] == 0.0f && r[6] == 6.0f);

	spectrum::Reader rd;
	uint8_t b[spectrum::kBands];
	CHECK (!rd.read (tap, an, 0, b) && rd.wants (7) == -1);	// nothing selected: no tap
	rd.select (3, tap.written ());
	CHECK (rd.wants (7) == -1);					// selected, but nobody read yet
	CHECK (rd.read (tap, an, 0, b));
	CHECK (rd.wants (7) == 3);					// read: the audio side copies
	sine (x, spectrum::kN, 1000, 1.0);
	tap.write (x, spectrum::kN);
	CHECK (rd.read (tap, an, 10, b) && rd.runs () == 1);		// 10 ms: not due (the first read analysed? no: nothing new)
	unsigned n0 = rd.runs ();
	CHECK (rd.read (tap, an, 50, b) && rd.runs () == n0 + 1 && argmax (b) == band_of (1000));
	CHECK (rd.read (tap, an, 60, b) && rd.runs () == n0 + 1);	// shared: the same bands until the next 50 ms
	CHECK (rd.read (tap, an, 100, b) && b[argmax (b)] == 0);	// the tap did not move: silence
	// half a window of a new strip after a switch: the rest is silence, not the last strip's sound
	tap.write (x, spectrum::kN);
	rd.select (5, tap.written ());
	sine (x, spectrum::kN / 2, 1000, 0.1);
	tap.write (x, spectrum::kN / 2);
	CHECK (rd.read (tap, an, 200, b));
	double db = spectrum::Analyser::db (b[argmax (b)]);
	CHECK (argmax (b) == band_of (1000) && db < -20.5 && db > -30.0);	// -20 dB over half the window (the power: -3 dB)
	// the hold: a second of audio with no read stops the tap
	unsigned left = 0;
	while (rd.wants (7) == 5) left += 7;
	CHECK (left >= spectrum::Reader::kHoldFrames && left < spectrum::Reader::kHoldFrames + 7);
	CHECK (rd.read (tap, an, 300, b) && rd.wants (7) == 5);	// a read starts it again
}

// the engine's taps: a voice, a bus's own return, the master as sent, B3 from core 2
static void test_engine ()
{
	int16_t v[7][8];
	float sl[7][4];
	int32_t out[7][12];
	uint8_t b[spectrum_pi::kBands];
	int s;
	CHECK (!spectrum_pi::Read (b, &s) && s == -1);
	CHECK (spectrum_pi::StripIndex ("V3") == 2 && spectrum_pi::StripIndex ("B3") == 10 && spectrum_pi::StripIndex ("M") == 11);
	CHECK (spectrum_pi::StripIndex ("V9") == -1 && spectrum_pi::StripIndex ("") == -1 && spectrum_pi::StripName (11) && !spectrum_pi::StripName (12));
	struct Case { int strip; double f; };
	const Case cs[] = {{2, 500}, {9, 2000}, {11, 6000}, {10, 300}};
	uint32_t ph = 0;
	for (const Case &c : cs)
	{
		spectrum_pi::Select (c.strip);
		core::g_ms += 1000;
		CHECK (spectrum_pi::Read (b, &s) && s == c.strip);		// arms the tap
		for (int blk = 0; blk < 400; blk++)				// 2800 frames, 7 at a time (an OUT block)
		{
			float y[7];
			for (int i = 0; i < 7; i++, ph++)
			{
				double a = 0.5 * sin (2 * kPi * c.f * ph / 48000.0);
				for (int k = 0; k < 8; k++) v[i][k] = (int16_t) (k == c.strip ? a * 32767 : 0);
				for (int k = 0; k < 4; k++) sl[i][k] = (c.strip == 9 && k >= 2) ? (float) a : 0.0f;
				for (int k = 0; k < 12; k++) out[i][k] = 0;
				out[i][0] = out[i][1] = (int32_t) (c.strip == 11 ? a * 2147483647.0 : 0);
				y[i] = (float) a;
			}
			spectrum_pi::FromOut (v, sl, out, 7);
			if (spectrum_pi::WantBus3 ()) spectrum_pi::FromBus3 (y, 7);
		}
		core::g_ms += 100;
		CHECK (spectrum_pi::Read (b, &s) && s == c.strip);
		int k = argmax (b);
		CHECK (k >= band_of (c.f) - 1 && k <= band_of (c.f) + 1);
		double db = spectrum::Analyser::db (b[k]);
		CHECK (db > -9.0 && db < -3.0);					// -6 dB
		CHECK (spectrum_pi::WantBus3 () == (c.strip == 10));
	}
	spectrum_pi::Select (-1);
	CHECK (!spectrum_pi::Read (b, &s));
}

// the topic: a sub names the strip (Host::strip), the payload
struct Fake
{
	std::vector<std::string> sent;
	std::string strip;
};
static bool FSend (void *p, const char *s, size_t n) { ((Fake *) p)->sent.push_back (std::string (s, n)); return true; }
static bool FTok (void *, const char *) { return true; }
static size_t FBuild (void *, wsproto::Topic t, char *o, size_t m)
{
	uint8_t b[40];
	for (int i = 0; i < 40; i++) b[i] = (uint8_t) (i * 6);
	return t == wsproto::kSpectrum ? wstopics::spectrum ("V3", b, 40, 20, 20000, -90, o, m) : (o[0] = '{', o[1] = '}', 2);
}
static const char *FCmd (void *, const char *) { return 0; }
static bool FStrip (void *p, const char *s) { if (spectrum_pi::StripIndex (s) < 0) return false; ((Fake *) p)->strip = s; return true; }
static bool has (const std::string &s, const char *t) { return s.find (t) != std::string::npos; }

static uint16_t msg (wsproto::Session &s, const char *m) { return s.message (m, strlen (m), 0); }

static void test_topic ()
{
	Fake f;
	char buf[4096];
	wsproto::Host h = {&f, FSend, FTok, FBuild, FCmd, "0.5.0", 0, 1u << wsproto::kSpectrum, 0, FStrip};
	wsproto::Session s (h, buf, sizeof buf);
	s.open (0);
	CHECK (has (f.sent[0], "\"topics\":[\"status\",\"spectrum\"]") && has (f.sent[0], "\"spectrum\":[50,50]"));
	CHECK (msg (s, "{\"type\":\"sub\",\"id\":1,\"payload\":{\"topics\":[\"spectrum\"],\"strip\":\"V3\"}}") == 0);
	CHECK (f.strip == "V3" && has (f.sent.back (), "\"topics\":[\"spectrum\"]") && has (f.sent.back (), "\"ms\":50"));
	s.tick (10);
	CHECK (has (f.sent.back (), "\"topic\":\"spectrum\"") && has (f.sent.back (), "{\"strip\":\"V3\",\"hz\":[20,20000],\"db\":[-90,0],\"b\":[0,6,12,"));
	CHECK (wsjson::valid_object (f.sent.back ().data (), f.sent.back ().size ()));
	size_t n = f.sent.size ();
	CHECK (msg (s, "{\"type\":\"sub\",\"id\":2,\"payload\":{\"topics\":[\"spectrum\"],\"strip\":\"X9\"}}") == 0);
	CHECK (f.strip == "V3" && f.sent.size () == n + 1 && has (f.sent.back (), "\"code\":400"));
	CHECK (msg (s, "{\"type\":\"sub\",\"id\":3,\"payload\":{\"topics\":[\"spectrum\"],\"strip\":7}}") == 0);
	CHECK (has (f.sent.back (), "\"code\":400"));
	CHECK (msg (s, "{\"type\":\"sub\",\"id\":4,\"payload\":{\"topics\":[\"status\"],\"strip\":\"B1\"}}") == 0);
	CHECK (f.strip == "V3");					// a strip only means something with spectrum
	CHECK (msg (s, "{\"type\":\"sub\",\"id\":5,\"payload\":{\"topics\":[\"spectrum\"],\"strip\":\"M\"}}") == 0 && f.strip == "M");
	char o[400];
	uint8_t z[40] = {};
	CHECK (wstopics::spectrum (0, z, 40, 20, 20000, -90, o, sizeof o) > 0 && has (o, "{\"strip\":null,"));
	CHECK (wstopics::spectrum ("V1", z, 40, 20, 20000, -90, o, 60) == 0);	// does not fit: 0
	// a build without the topic: "later", and the strip is not asked
	Fake g;
	wsproto::Host h2 = {&g, FSend, FTok, FBuild, FCmd, "0.5.0", 0, 0, 0, FStrip};
	wsproto::Session s2 (h2, buf, sizeof buf);
	s2.open (0);
	CHECK (msg (s2, "{\"type\":\"sub\",\"id\":1,\"payload\":{\"topics\":[\"spectrum\"],\"strip\":\"V3\"}}") == 0);
	CHECK (g.strip.empty () && has (g.sent.back (), "\"later\":[\"spectrum\"]"));
}

// the cost of one analysis here (the Pi's A72 / A76 are slower: see the report)
static void bench ()
{
	static spectrum::Analyser an;
	static float x[spectrum::kN];
	uint8_t b[spectrum::kBands];
	for (int i = 0; i < spectrum::kN; i++) x[i] = (float) sin (i * 0.37) * 0.3f;
	const int n = 2000;
	timespec t0, t1;
	clock_gettime (CLOCK_MONOTONIC, &t0);
	unsigned sum = 0;
	for (int k = 0; k < n; k++) { x[k & (spectrum::kN - 1)] += 1e-6f; an.run (x, b); sum += b[k % spectrum::kBands]; }
	clock_gettime (CLOCK_MONOTONIC, &t1);
	double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / n;
	printf ("test_spectrum: one analysis (%d-point FFT, %d bands) %.1f us here, %.2f %% of a core at %d Hz (%u)\n",
		spectrum::kN, spectrum::kBands, us, us * spectrum::kRateHz / 1e4, spectrum::kRateHz, sum & 1);
}

int main ()
{
	test_analyser ();
	test_tap_reader ();
	test_engine ();
	test_topic ();
	bench ();
	printf ("test_spectrum: OK\n");
	return 0;
}
