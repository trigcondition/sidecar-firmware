// core/wavetable/tables.h - WAVETABLE's band-limited tables (docs/WAVETABLE.md): a frame (one single cycle) as its
// harmonics, and from them kLevels mip levels, one an octave: level k holds harmonics 1..kLevelHarm[k] (512 >> k) at
// kLevelSize[k] samples (at least 4 samples a harmonic, so the 4-point Hermite read stays clean), DC removed. A note
// reads the level whose top harmonic stays under kAliasLimit of the sample rate (an alias of what is over Nyquist
// lands above 0.4 fs, out of hearing), so no level aliases audibly at any pitch. Frames of 256 / 512 / 1024 / 2048
// samples (FFT) or any other length (a DFT: AKWF's 600) are taken; a 2048 frame keeps its first 512 harmonics.
// The cold side only (building); the render core reads the levels through level().
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core {
namespace wt {

constexpr int kFrame = 2048;        // the open format's frame (Serum, Vital, ...)
constexpr int kHarm = 512;          // harmonics a frame keeps
constexpr int kLevels = 10;
constexpr int kMaxFrames = 256;
constexpr float kAliasLimit = 0.6f; // top harmonic x f / fs at most (its alias, if any, lands above 0.4 fs)
// (at least 512 samples: from level 3 up, where the notes' strong low harmonics sit near the level's top, the read
// takes 8 samples a harmonic or more, so its images sit near its nulls: the aliases stay under -60 dB)
constexpr int kLevelSize[kLevels] = {2048, 1024, 512, 512, 512, 512, 512, 512, 512, 512};
constexpr int kLevelHarm[kLevels] = {512, 256, 128, 64, 32, 16, 8, 4, 2, 1};
// A frame's floats: each level x[-1], x[0..N-1], x[N], x[N+1] (the wrap guards the Hermite read needs)
constexpr int level_off(int k) { return k == 0 ? 1 : level_off(k - 1) + kLevelSize[k - 1] + 3; }
constexpr int kFrameFloats = (level_off(kLevels) - 1 + 3) & ~3;   // 7200 (28.1 KB)

// A frame's harmonics: c[2h], c[2h+1] = re, im of c_h (h = 1..kHarm), x(t) = sum Re(c_h e^(2 pi i h t)); c[0..1] DC.
constexpr int kSpec = 2 * (kHarm + 1);
// x: n samples of one cycle (n >= 4); c: kSpec floats. scratch: 4 * max(n, 2048) floats.
void analyze(const float* x, int n, float* c, float* scratch);
// Every level of a frame into fr (kFrameFloats) from its harmonics. scratch: 2 * kFrame floats.
void synth_frame(const float* c, float* fr, float* scratch);
inline const float* level(const float* frame, int k) { return frame + level_off(k); }
float frame_peak(const float* fr);   // level 0's largest |x|
// The level for a fundamental of inc cycles a sample (the warp's top rate folded in); kLevels: too high to play.
inline int pick_level(float inc) {
  if (inc <= 0) return 0;
  float h = kAliasLimit / inc;
  for (int k = 0; k < kLevels; ++k)
    if ((float)kLevelHarm[k] <= h) return k;
  return kLevels;
}
// The frame size of a file of total mono samples: the 'clm ' hint (clm, if it divides total), else 2048 if it divides,
// else 1024 / 512 / 256 if one does, else (total <= 8192) one frame of all of it, else 2048 (the rest left out).
int detect_frame_size(uint32_t total, uint32_t clm);
// How many frames a file of total samples gives at frame size fs (at most kMaxFrames: more are thinned evenly) and
// which file frame table frame f reads.
int frame_count(uint32_t total, int fs);
uint32_t source_frame(uint32_t total, int fs, int f);

// The built-in tables (generated here, nothing third-party): 64 frames each.
constexpr int kBuiltins = 6, kBuiltinFrames = 64;
const char* builtin_name(int b);   // BASIC PWM VOCAL ORGAN FM SYNC
// frame f's harmonics (c: kSpec floats); scratch: 4 * kFrame floats
void builtin_frame(int b, int f, float* c, float* scratch);

}  // namespace wt
}  // namespace core
