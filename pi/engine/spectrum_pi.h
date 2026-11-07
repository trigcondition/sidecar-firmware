// pi/engine/spectrum_pi.h - the engine's spectrum analyser (spectrum.h): one strip at a time, its sound after its POST
// EQ. Readers (the web's "spectrum" topic; later the touchscreen) pick a strip and read the newest bands, all on core 0;
// the analysis runs inside Read when it is due (spectrum::kRateHz), shared by every reader. The audio side copies
// samples only while someone read in the last second. Taps: the voices (each its chain's output, mono, before the OUT
// route's LEV / PAN), B1 / B2 (each send FX's own return after its bus chain) and M (MAIN L + R as sent, after the master
// chain: the Rytm's own sound is not in it) from the OUT fill on core 0; B3 (the dry bus's chain output) from render
// core 2. Hosted (no Circle headers): ob_usb.cpp, sendfx_pi.cpp and web.cpp call it.
#pragma once
#include <stdint.h>

namespace spectrum_pi {
// spectrum.h's, for the callers built without libm's headers (web.cpp)
const int kBands = 40, kLoHz = 20, kHiHz = 20000, kFloorDb = -90;
int StripIndex(const char* name);   // "V1".."V8", "B1".."B3", "M" -> 0..11; -1: not a strip
const char* StripName(int strip);   // (null: not a strip)
// core 0: the strip to analyse (spectrum::kStripName: V1..V8 0..7, B1..B3 8..10, M 11; -1 none). One for all readers:
// the last pick wins
void Select(int strip);
// core 0: the selected strip's newest bands (kBands bytes, 0..255 = kFloorDb..0 dB), analysed when due; keeps
// the taps running. false: no strip selected. strip (optional): which one they are
bool Read(uint8_t* bands, int* strip = nullptr);
// core 0, the OUT fill: n frames of the voices (Output::Pull's), each FX's own return (AddReturn's slots: FX 1 L R, FX 2
// L R) and the channels as sent (after the master chain). A copy, and only while wanted
void FromOut(const int16_t (*voices)[8], const float (*slots)[4], const int32_t (*out)[12], unsigned n);
// core 2, sendfx_pi::Render: B3's chain output (mono) wanted? Then FromBus3 with it
bool WantBus3();
void FromBus3(const float* y, unsigned n);
unsigned Runs();   // analyses so far (the tests, a stats line)
}  // namespace spectrum_pi
