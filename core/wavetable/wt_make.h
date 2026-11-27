// core/wavetable/wt_make.h - a wavetable made from a sample (SAMPLE EDIT's MAKE WT; docs/SAMPLE-EDIT.md, docs/WAVETABLE.md):
// N frames of 2048 samples, each one cycle, ready for WAVETABLE's reader (wav_read.h, tables.h).
//
// PITCHED (a clear pitch in most of the selection): the period is found at each frame's place by YIN on a decimated
// copy (the first dip of the normalised difference under 0.15, parabolic) and refined at full rate; the track is kept
// in step with its neighbours (an octave jump is mended toward the previous frame's period), so vibrato and glides are
// followed. Each frame's harmonics are then measured over three periods (a Hann window, harmonic h at h / T cycles a
// sample, to the source's own Nyquist or 512), which is the cycle band-limited and resampled to 2048 at once, DC left
// out. Frame 0 is turned so its fundamental is a sine (the cycle starts at a rising zero crossing); every other frame
// is turned (a fractional rotation of all the phases) to the place where it best matches the frame before it, so a POS
// morph neither phase-cancels nor sweeps a comb.
// SPECTRAL (no pitch: noise, a hit, a chord of nothing in particular, or fewer than 1.5 periods): each frame is a
// window of the selection (2048 samples, or four fifths of a short selection) made periodic by a crossfade of its end
// into its start (equal power), DC out, resampled to 2048 as a harmonic series when it was shorter.
// Then one gain for the whole table (its loudest sample 0.95): the frames keep their own levels.
//
// A Maker runs in bounded pieces (step(): at most about 400 000 multiply-adds, one 65 536-frame scan, or one
// 2048-point FFT) so it never holds up the control core. Pi and Mac only, cold.
#pragma once
#include <stdint.h>

#include "../platform.h"

#if CORE_MSAMP

namespace core {
namespace wt {
namespace make {

constexpr int kFrameLen = 2048;
constexpr int kMaxFrames = 256;
constexpr int kMinSelection = 64;   // frames of the selection at least
enum Method : uint8_t { kNone, kPitched, kSpectral };
enum : int { kErrShort = -1, kErrSilent = -2, kErrArg = -3 };
// 16 32 64 128 256 (the choice's list)
constexpr int kChoices[5] = {16, 32, 64, 128, 256};

class Maker {
 public:
  // x: the selection's n frames of ch (1 or 2) interleaved channels (a stereo pair is summed), at rate; out: nframes x
  // kFrameLen floats the caller owns (until step returns 0 or < 0). 0 started; kErrShort / kErrArg refused.
  int begin(const float* x, int ch, uint32_t n, uint32_t rate, int nframes, float* out);
  // 1 more to do, 0 done (out holds the table), kErrSilent: nothing in the selection to make a table of
  int step();
  int progress() const;       // 0..100
  Method method() const { return method_; }
  float period() const { return period_; }       // pitched: the median period in samples of the source, else 0
  float voiced() const { return voiced_; }       // the fraction of frames with a clear pitch (0..1)
  float frame_period(int f) const { return f >= 0 && f < nf_ ? T_[f] : 0; }

 private:
  enum Stage : uint8_t { kIdle, kPeak, kDetect, kDecide, kBuild, kNorm, kDone };
  float mono(uint32_t i) const { return ch_ == 1 ? x_[i] : 0.5f * (x_[2 * (size_t)i] + x_[2 * (size_t)i + 1]); }
  void detect(int f);
  void decide();
  bool build_step();
  void win_pitched(int f);
  void win_spectral(int f);
  void finish_frame(int f);
  int harm_pass(int budget);

  const float* x_ = nullptr;
  float* out_ = nullptr;
  int ch_ = 1, nf_ = 0;
  uint32_t n_ = 0, rate_ = 0, pos_ = 0;
  Stage st_ = kIdle;
  Method method_ = kNone;
  float peak_ = 0, period_ = 0, voiced_ = 0;
  int f_ = 0;                // the frame in hand (detect, build)
  float T_[kMaxFrames] = {};     // the period at each frame's place (0: none)
  float conf_[kMaxFrames] = {};
  // the frame in hand
  bool open_ = false;        // its window is set and its harmonics are under way
  int h_ = 0, hmax_ = 0, len_ = 0;
  uint32_t start_ = 0;
  double per_ = 0, sumw_ = 0;
  float c_[2 * 513] = {};    // its harmonics (re, im; [0..1] unused)
  float prev_[2 * 513] = {}; // the frame before's, as aligned
  float xw_[8192];           // the windowed samples (detect: the full-rate segment of the refinement)
  float dec_[2200], cmnd_[1100];   // detect: the decimated window, YIN's normalised difference
  bool direct_ = false;      // the frame in hand is a 2048-sample cycle already (spectral): no harmonics
  float gain_ = 1;
  float emax_ = 0;           // the largest harmonic energy seen (alignment's reference is kept from loud frames)
  float cs_[kFrameLen], sn_[kFrameLen];   // cos / sin of 2 pi m / 2048
  float re_[kFrameLen], im_[kFrameLen];
};

// The header of the table's WAV (mono float32, fact, `clm ` 2048, data) for `frames` samples: the bytes up to the data.
constexpr int kTableHeaderMax = 104;
int table_wav_header(uint8_t* out, uint32_t rate, uint32_t frames);

// The whole table at once (tests, tools): 0 ok, else step's error. method / period read out when not null.
int make_table(const float* x, int ch, uint32_t n, uint32_t rate, int nframes, float* out, Method* method = nullptr,
               float* period = nullptr);

}  // namespace make
}  // namespace wt
}  // namespace core

#endif
