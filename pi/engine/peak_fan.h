// pi/engine/peak_fan.h - the output's peaks for more than one reader. Output::TakeVoicePeaks / TakePeak reset what they
// return, so a second reader (the web's "levels" topic, docs/web-ws.md) would steal the display's meters. Whoever takes
// from the output feeds the result in here (every reader's accumulator gets its max), then takes its own share. Core 0
// only (the display loop, the stats line and the web task are all core 0's): no lock. Portable (tests/test_ws.cpp).
#ifndef _peak_fan_h
#define _peak_fan_h

namespace peakfan {

enum Reader { kDisplay, kStats, kWeb, kWeb2, kReaders };	// (two: web.h MaxWs)
static const int kVoices = 8;

class Fan
{
public:
	// what Output::TakeVoicePeaks just returned: into every reader's share
	void feed_voices (const float (*pk)[2])
	{
		for (int r = 0; r < kReaders; r++)
			for (int v = 0; v < kVoices; v++)
				for (int c = 0; c < 2; c++) if (pk[v][c] > m_V[r][v][c]) m_V[r][v][c] = pk[v][c];
	}
	// what Output::TakePeak just returned
	void feed_master (float p) { for (int r = 0; r < kReaders; r++) if (p > m_M[r]) m_M[r] = p; }

	// this reader's peaks since its last take (reset)
	void take_voices (Reader r, float (*pk)[2])
	{
		for (int v = 0; v < kVoices; v++)
			for (int c = 0; c < 2; c++) { pk[v][c] = m_V[r][v][c]; m_V[r][v][c] = 0.0f; }
	}
	float take_master (Reader r) { float p = m_M[r]; m_M[r] = 0.0f; return p; }

private:
	float m_V[kReaders][kVoices][2] = {};
	float m_M[kReaders] = {};
};

}	// namespace peakfan

#endif
