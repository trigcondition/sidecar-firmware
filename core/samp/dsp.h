// core/samp/dsp.h - the analysis the sampler (sampler.h) and the auto-sampler (autosamp.h) share (docs/SAMPLING.md):
// levels, silence trim, transient detection and grid slices, a loop finder for sustained sounds, the WAV header (24 /
// 16-bit PCM, an optional smpl loop) and note names. Buffers are float, -1..1 full scale, interleaved when ch == 2.
// Portable, allocation-free, cold (never on the render path).
#pragma once
#include <math.h>
#include <stdint.h>

namespace core {
namespace samp {

constexpr uint32_t kRate = 48000;

// The largest |x| over n frames of ch channels.
float peak(const float* x, int ch, uint32_t n);
// The RMS over frames [a, b) (all channels).
float rms(const float* x, int ch, uint32_t a, uint32_t b);
// The first frame >= from where any channel's |x| > thr (n: none).
uint32_t first_above(const float* x, int ch, uint32_t n, float thr, uint32_t from = 0);
// One past the last frame where any channel's |x| > thr (0: none).
uint32_t last_above(const float* x, int ch, uint32_t n, float thr);
inline float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }

// Silence trim (docs/MULTISAMPLE.md §7 "Silence detection / trim"): the floor is the RMS of [0, floor_n) (a
// pre-roll; 0: none) + 12 dB, at least min_db (-50 dBFS by default); the start is the first frame above it less
// 2 ms (not before 0), the end one past the last frame above it plus 10 ms (a fade over those 10 ms is the writer's).
// False: nothing above the floor ("no sound").
struct Trim {
  uint32_t start, end;
  float floor;
};
bool trim(const float* x, int ch, uint32_t n, uint32_t floor_n, Trim* out, float min_db = -50.0f);

// Transients: an energy envelope in hops of 256 frames (5.3 ms); a hop whose energy jumps over sens times the mean of
// the 8 before it (and over the floor) starts a slice, refined to the first frame in that hop above a quarter of its
// peak; at least min_gap_ms apart. sens 1.5 .. 8 (2.5: drums). The slice starts, ascending, the first at 0; the count.
int transients(const float* x, int ch, uint32_t n, uint32_t* out, int max, float sens = 2.5f,
               uint32_t min_gap_ms = 60);
// Grid: n frames in `slices` equal parts (a bar in 16: slices 16), each start moved to the nearest zero crossing
// within 1 ms. The count.
int grid(const float* x, int ch, uint32_t n, int slices, uint32_t* out);

// The loop finder (docs/MULTISAMPLE.md §7 "Loops"): in a sustained part [a, b) of a mono sound, the period by
// autocorrelation (a window of 4096 frames at the part's middle, 30..2000 Hz; the first peak within 10 % of the
// highest: no octave errors), then a loop of a whole number of periods (at least min_ms long), its start at a rising
// zero crossing in the part's first half, its length tried +-4 frames around k periods: the seam score is the
// squared difference of the 32 frames before the end and before the start (the waveform across the seam), over the
// part's power, plus the two halves' RMS mismatch. ok when the score is under max_score. s, e: the loop's first frame
// and one past its last (SFZ loop_end = e - 1).
struct Loop {
  bool ok;
  uint32_t s, e;
  float period;   // frames (0: none found)
  float score;
};
Loop find_loop(const float* x, uint32_t a, uint32_t b, uint32_t min_ms = 100, float max_score = 0.02f);
// A crossfade loop (§7 "optional crossfade loop"): the xf frames before the loop's end blended into the xf frames
// before its start (equal power), so that playing on from e - 1 into s is seamless whatever the seam score was.
// Rewrites x in place; needs s >= xf.
void crossfade_loop(float* x, uint32_t s, uint32_t e, uint32_t xf);
// Is [a, b) sustained (a loop is worth finding): its RMS in 8 parts within 3 dB of their mean, and audible.
bool sustained(const float* x, uint32_t a, uint32_t b);

// The WAV header for ch channels, bits 16 or 24, rate, frames, and (loop) a smpl chunk with one forward loop
// [loop_s, loop_e] (inclusive, as SFZ's) at root key root: the bytes up to the data (at most kWavHeaderMax). The data
// is frames * ch * bits / 8 bytes, then a pad byte when that is odd (wav_tail).
constexpr int kWavHeaderMax = 44 + 68;
int wav_header(uint8_t* out, int ch, int bits, uint32_t rate, uint32_t frames, bool loop = false, uint32_t loop_s = 0,
               uint32_t loop_e = 0, int root = 60);
inline uint32_t wav_data_bytes(int ch, int bits, uint32_t frames) { return frames * (uint32_t)ch * (uint32_t)(bits / 8); }
// One sample to 24-bit little endian (clipped).
inline void put24(uint8_t* p, float v) {
  float y = v * 8388608.0f;
  int32_t s = y >= 8388607.0f ? 8388607 : y <= -8388608.0f ? -8388608 : (int32_t)(y < 0 ? y - 0.5f : y + 0.5f);
  p[0] = (uint8_t)s, p[1] = (uint8_t)(s >> 8), p[2] = (uint8_t)(s >> 16);
}

// SFZ note names (C4 = 60): "C4", "C#4", "C-1"; out at least 5 bytes.
void note_name(int note, char* out);

}  // namespace samp
}  // namespace core
