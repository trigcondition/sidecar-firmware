// core/samp/sedit_dsp.h - the sample editor's edit functions on interleaved float (1 or 2 channels). No state: every
// op works on a frame range [a, b), so the driver can run it piece by piece. Portable, allocation-free, cold.
#pragma once
#include <stdint.h>

#include "../ui/ui.h"

namespace core {
namespace sedit {

enum Fn : uint8_t { kCrop = 0, kNorm, kTrim, kFadeIn, kFadeOut, kReverse, kGainUp, kGainDown, kFns };
extern UI_ROM const char* const kFnNames[kFns];

constexpr float kNormPeak = 0.98855f;    // -0.1 dBFS
constexpr float kGainUpF = 1.41254f;     // +3 dB
constexpr float kGainDownF = 0.70795f;   // -3 dB

// The largest |x| over frames [a, b).
float peak_range(const float* x, int ch, uint32_t a, uint32_t b);
// x *= g over frames [a, b).
void gain_range(float* x, int ch, uint32_t a, uint32_t b, float g);
// A linear fade over the region [r0, r1), applied to the frames [a, b) within it. The gain at frame f is
// (f - r0) / (r1 - r0 - 1) for a fade in, 1 less that for a fade out; a 1-frame region: 1 for in, 0 for out.
void fade_range(float* x, int ch, uint32_t a, uint32_t b, uint32_t r0, uint32_t r1, bool in);
// Swaps frame r0 + i with r1 - 1 - i for i in [p0, p1), p1 <= (r1 - r0) / 2 (an odd length keeps its middle frame).
void reverse_pairs(float* x, int ch, uint32_t r0, uint32_t r1, uint32_t p0, uint32_t p1);
// Copies src frames [a, b) to dst at dst_off + (f - a): crop's mover and the undo copy.
void copy_frames(float* dst, const float* src, int ch, uint32_t a, uint32_t b, uint32_t dst_off);
// samp::trim over the whole sample (floor from the first rate/20 frames, -50 dBFS): the new [a, b). False: all silent.
bool trim_bounds(const float* x, int ch, uint32_t n, uint32_t rate, uint32_t* a, uint32_t* b);
// Makes 0 <= a < b <= frames: swaps when b < a, widens an empty region by one frame; frames == 0 gives 0, 0.
void clamp_region(uint32_t frames, int64_t* a, int64_t* b);

}  // namespace sedit
}  // namespace core
