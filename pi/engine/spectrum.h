// pi/engine/spectrum.h - a strip's spectrum, for the web's "spectrum" topic (docs/web-ws.md) and, later, an EQ page on
// the touchscreen. The audio side only copies samples (mono) into a Tap; a reader on another core takes the newest kN
// of them and an Analyser turns them into kBands log-spaced bands in dB, quantised to bytes. Portable, header only, no
// allocation (tests/test_spectrum.cpp, with a benchmark). The engine's instance is spectrum_pi.h.
#ifndef _spectrum_h
#define _spectrum_h

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace spectrum {

static const int kN = 2048;			// the FFT: 23.4 Hz bins at 48 kHz, 43 ms of audio
static const int kBands = 40;			// log-spaced across the EQ curve's axis (pi/../web-next src/lib/mix.ts eqPath)
static const int kRateHz = 20;			// analyses a second (5 is still fine on the page: it interpolates); the readers
						// share them, however many there are
constexpr float kLoHz = 20.0f, kHiHz = 20000.0f;
static const int kFloorDb = -90;		// byte 0; byte 255 = 0 dBFS (a full-scale sine in one band)
constexpr float kRate = 48000.0f;

// The strips, as the web's mixer names them (ws_topics.h levels): V1..V8 the voices, B1..B3 the buses, M the master
static const int kStrips = 12;
static const char *const kStripName[kStrips] = {"V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "B1", "B2", "B3", "M"};
inline int strip_index (const char *p)
{
	for (int i = 0; p && i < kStrips; i++) if (strcmp (p, kStripName[i]) == 0) return i;
	return -1;
}

// The newest samples of one source. One writer (the audio side: write, a copy), one reader (read: the newest n, a copy).
// Nothing waits and nothing is refused: the writer overwrites the oldest. The reader takes kN of kSize, so the writer
// would have to write kSize - kN samples (43 ms) during one copy to tear it.
class Tap
{
public:
	static const unsigned kSize = 2 * kN;	// a power of 2
	void write (const float *p, unsigned n)
	{
		uint32_t w = m_W;
		for (unsigned i = 0; i < n; i++) m_Buf[(w + i) & (kSize - 1)] = p[i];
		__atomic_store_n (&m_W, w + n, __ATOMIC_RELEASE);
	}
	// the newest n (<= kSize) samples, oldest first; returns how many were ever written (the reader's clock)
	uint32_t read (float *out, unsigned n) const
	{
		uint32_t w = __atomic_load_n (&m_W, __ATOMIC_ACQUIRE);
		for (unsigned i = 0; i < n; i++) out[i] = m_Buf[(w - n + i) & (kSize - 1)];
		return w;
	}
	uint32_t written () const { return __atomic_load_n (&m_W, __ATOMIC_ACQUIRE); }

private:
	float m_Buf[kSize] = {};
	uint32_t m_W = 0;
};

// kN samples -> kBands bytes. A Hann window, a real FFT (a kN / 2 complex one and the split), each band the sum of the
// bins' power inside it (a band narrower than a bin: the power at its centre, interpolated), in dB re a full-scale sine.
class Analyser
{
public:
	Analyser ()
	{
		const double pi = 3.14159265358979323846;
		for (int i = 0; i < kN; i++) m_Win[i] = (float) (0.5 - 0.5 * cos (2.0 * pi * i / kN));
		for (int k = 0; k < kN / 2; k++) { m_Cos[k] = (float) cos (2.0 * pi * k / kN); m_Sin[k] = (float) sin (2.0 * pi * k / kN); }
		for (int i = 0, j = 0; i < kH; i++)
		{
			m_Rev[i] = (uint16_t) j;
			int b = kH >> 1;
			while (j & b) { j ^= b; b >>= 1; }
			j |= b;
		}
		const double df = kRate / kN;
		for (int b = 0; b < kBands; b++)
		{
			double lo = kLoHz * pow (kHiHz / kLoHz, (double) b / kBands), hi = kLoHz * pow (kHiHz / kLoHz, (double) (b + 1) / kBands);
			int k0 = (int) ceil (lo / df), k1 = (int) ceil (hi / df);
			if (k1 > kN / 2) k1 = kN / 2;
			double c = sqrt (lo * hi) / df;		// the centre, for a band with no bin of its own
			m_K0[b] = (uint16_t) k0;
			m_K1[b] = (uint16_t) (k1 > k0 ? k1 : 0);
			m_Ci[b] = (uint16_t) c;
			m_Cf[b] = (float) (c - floor (c));
		}
	}

	void run (const float *x, uint8_t *bands)
	{
		for (int i = 0; i < kH; i++)			// even samples real, odd imaginary, in bit-reversed order
		{
			int j = m_Rev[i];
			m_Re[j] = x[2 * i] * m_Win[2 * i];
			m_Im[j] = x[2 * i + 1] * m_Win[2 * i + 1];
		}
		for (int len = 2; len <= kH; len <<= 1)		// radix 2, in place
		{
			int half = len >> 1, step = kN / len;	// the twiddles of kN / 2 points are every other one of kN's
			for (int s = 0; s < kH; s += len)
				for (int k = 0; k < half; k++)
				{
					float wr = m_Cos[k * step], wi = -m_Sin[k * step];
					int a = s + k, b = a + half;
					float tr = m_Re[b] * wr - m_Im[b] * wi, ti = m_Re[b] * wi + m_Im[b] * wr;
					m_Re[b] = m_Re[a] - tr; m_Im[b] = m_Im[a] - ti;
					m_Re[a] += tr; m_Im[a] += ti;
				}
		}
		for (int k = 0; k <= kH; k++)			// the split: the kN-point spectrum's power, bins 0..kN/2
		{
			int a = k & (kH - 1), b = (kH - k) & (kH - 1);
			float er = 0.5f * (m_Re[a] + m_Re[b]), ei = 0.5f * (m_Im[a] - m_Im[b]);
			float orr = 0.5f * (m_Im[a] + m_Im[b]), oi = -0.5f * (m_Re[a] - m_Re[b]);
			float wr = k < kH ? m_Cos[k] : -1.0f, wi = k < kH ? -m_Sin[k] : 0.0f;
			float xr = er + orr * wr - oi * wi, xi = ei + orr * wi + oi * wr;
			m_Pow[k] = xr * xr + xi * xi;
		}
		const float norm = 1.0f / ((kN / 4.0f) * (kN / 4.0f));	// a full-scale sine's peak bin: kN / 4 (Hann)
		for (int b = 0; b < kBands; b++)
		{
			float p;
			if (m_K1[b]) { p = 0.0f; for (int k = m_K0[b]; k < m_K1[b]; k++) p += m_Pow[k]; }
			else { int k = m_Ci[b]; p = m_Pow[k] + (m_Pow[k + 1] - m_Pow[k]) * m_Cf[b]; }
			bands[b] = quant (p * norm);
		}
	}

	// a power (1 = 0 dB) as a byte: kFloorDb..0 dB on 0..255
	static uint8_t quant (float p)
	{
		if (!(p > 1e-12f)) return 0;
		float db = 10.0f * log10f (p);
		float v = (db - kFloorDb) * (255.0f / -kFloorDb) + 0.5f;
		return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t) v;
	}
	static float db (uint8_t v) { return (v - 255) * (-kFloorDb / 255.0f); }

private:
	static const int kH = kN / 2;
	float m_Win[kN], m_Cos[kN / 2], m_Sin[kN / 2];
	float m_Re[kH], m_Im[kH], m_Pow[kH + 1];
	uint16_t m_Rev[kH];
	uint16_t m_K0[kBands], m_K1[kBands], m_Ci[kBands];
	float m_Cf[kBands];
};

// The reader's side: the selected strip, the newest bands, when to analyse again. Time is the caller's (ms). The audio
// side asks wants (budget) whether to copy: it runs only while someone read in the last kHoldFrames of audio.
class Reader
{
public:
	static const uint32_t kHoldFrames = 48000;	// 1 s: nobody read for this long, the tap stops

	// strip 0..kStrips-1 (-1: none). Samples written before this are not used (the window starts in silence)
	void select (int strip, uint32_t written)
	{
		if (strip == m_Strip) return;
		m_Strip = strip;
		m_SelAt = written;
		m_bHave = false;
		memset (m_Bands, 0, sizeof m_Bands);
	}
	int strip () const { return m_Strip; }

	// the newest bands; analyses when 1000 / kRateHz ms have passed (tap: the selected strip's). A tap that did not
	// move since the last analysis (no stream, a bus that does not run): silence. false: no strip selected
	bool read (Tap &tap, Analyser &an, uint32_t now, uint8_t *bands)
	{
		if (m_Strip < 0) return false;
		m_Budget = kHoldFrames;
		if (!m_bHave || now - m_At >= 1000u / kRateHz)
		{
			m_At = now;
			m_bHave = true;
			uint32_t w = tap.read (m_X, kN);
			uint32_t fresh = w - m_SelAt;		// samples of this strip in the window
			if (w == m_Last) memset (m_Bands, 0, sizeof m_Bands);
			else
			{
				if (fresh < (uint32_t) kN) memset (m_X, 0, sizeof (float) * (kN - fresh));
				an.run (m_X, m_Bands);
				m_nRuns++;
			}
			m_Last = w;
		}
		memcpy (bands, m_Bands, kBands);
		return true;
	}

	// the audio side: the strip to copy (-1: none), n frames about to be copied (the hold runs down by them)
	int wants (unsigned n)
	{
		uint32_t b = m_Budget;
		if (b == 0) return -1;
		m_Budget = b > n ? b - n : 0;
		return m_Strip;
	}
	unsigned runs () const { return m_nRuns; }

private:
	volatile int m_Strip = -1;
	volatile uint32_t m_Budget = 0;		// racy (reader sets, audio side runs it down): a hold, not a count
	uint32_t m_SelAt = 0, m_Last = 0, m_At = 0;
	bool m_bHave = false;
	unsigned m_nRuns = 0;
	uint8_t m_Bands[kBands] = {};
	float m_X[kN];
};

}	// namespace spectrum

#endif
