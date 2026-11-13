// pi/engine/spectrum_pi.cpp - see spectrum_pi.h.
#include "spectrum_pi.h"

#include "fx/out.h"
#include "platform.h"
#include "spectrum.h"

namespace spectrum_pi {

static_assert(kBands == spectrum::kBands && kLoHz == (int)spectrum::kLoHz && kHiHz == (int)spectrum::kHiHz &&
                  kFloorDb == spectrum::kFloorDb, "spectrum_pi.h's copy of spectrum.h");
static const int kB3 = 10, kM = 11;
static spectrum::Tap s_Out, s_Bus3;		// written on core 0 (the OUT fill) and on core 2 (B3)
static spectrum::Reader s_Reader;
static spectrum::Analyser* s_pAn = nullptr;	// ~30 KB, made on the first read (core 0)

static spectrum::Tap& TapOf(int strip) { return strip == kB3 ? s_Bus3 : s_Out; }

void Select(int strip) {
  if (strip < -1 || strip >= spectrum::kStrips) strip = -1;
  s_Reader.select(strip, strip < 0 ? 0 : TapOf(strip).written());
}

bool Read(uint8_t* bands, int* strip) {
  if (!s_pAn) s_pAn = new spectrum::Analyser;
  const int s = s_Reader.strip();
  if (strip) *strip = s;
  if (s < 0 || !s_pAn) return false;
  return s_Reader.read(TapOf(s), *s_pAn, core::now_ms(), bands);
}

void FromOut(const int16_t (*voices)[8], const float (*slots)[4], const int32_t (*out)[12], unsigned n) {
  if (s_Reader.strip() == kB3) return;   // (core 2's: its hold is not run down twice)
  const int s = s_Reader.wants(n);
  if (s < 0 || s == kB3) return;
  const uint8_t* post = core::out::device().post;   // MAIN L / R
  float m[16];
  for (unsigned i = 0; i < n;) {
    unsigned k = 0;
    for (; k < 16 && i < n; ++k, ++i)
      m[k] = s < 8    ? voices[i][s] * (1.0f / 32768.0f)
             : s < kM ? 0.5f * (slots[i][2 * (s - 8)] + slots[i][2 * (s - 8) + 1])
                      : 0.5f / 2147483648.0f * ((float)out[i][post[0]] + (float)out[i][post[1]]);
    s_Out.write(m, k);
  }
}

bool WantBus3() { return s_Reader.strip() == kB3; }

void FromBus3(const float* y, unsigned n) {
  if (s_Reader.wants(n) != kB3) return;
  s_Bus3.write(y, n);
}

int StripIndex(const char* name) { return spectrum::strip_index(name); }
const char* StripName(int strip) { return strip >= 0 && strip < spectrum::kStrips ? spectrum::kStripName[strip] : nullptr; }

unsigned Runs() { return s_Reader.runs(); }

}  // namespace spectrum_pi
