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

// sedit-050: the markers' zero-crossing snap. The frames [lo, hi] (within 1..frames - 1) searched for the place nearest
// f: stereo first a frame where both channels are within kNearZero, else (and mono) a crossing of the channels' sum (a
// sign change between frame i - 1 and i, or an exact 0: the one of the two nearer 0). True and *out: found.
constexpr float kNearZero = 0.003f;   // -50 dBFS
constexpr uint32_t kSnapReach = 256;  // frames either side of the asked place (5.3 ms at 48 kHz)
bool zero_near(const float* x, int ch, uint32_t frames, uint32_t f, uint32_t lo, uint32_t hi, uint32_t* out);

// sedit-050: the transients (FUNC + A / B): energy onsets on hops of kOnsetHop frames of the channels' sum. Hop k is an
// onset when its mean square is over kOnsetFloor and at least kOnsetRise times the larger of the two hops before it,
// and kOnsetGap after the last onset; its place is the hop's first frame over half the hop's RMS. Fed in pieces (any
// size; one pass in frame order); off the audio path (the editor's step).
constexpr uint32_t kOnsetHop = 256;          // 5.3 ms at 48 kHz
constexpr float kOnsetRise = 4.0f;           // +6 dB in energy
constexpr float kOnsetFloor = 2.5e-5f;       // the hop's RMS over 0.005 (-46 dBFS)
constexpr uint32_t kOnsetGapMs = 50;
constexpr int kOnsetsMax = 2048;            // 8 KB (uint32 each) in the editor's state
struct Onsets {
  uint32_t at[kOnsetsMax];
  int n = 0;
  void begin(uint32_t rate);
  // frames [from, to) of x (from == where the last call stopped); the last call ends at the sample's end
  void feed(const float* x, int ch, uint32_t from, uint32_t to);
  void end(const float* x, int ch);   // the last part hop
 private:
  void hop_done(const float* x, int ch);
  uint32_t gap_ = 0, hop0_ = 0, last_ = 0;
  double acc_ = 0;
  uint32_t cnt_ = 0;
  float e1_ = 0, e2_ = 0;
  bool any_ = false;
};

}  // namespace sedit
}  // namespace core
