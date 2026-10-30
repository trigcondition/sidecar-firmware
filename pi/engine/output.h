// pi/engine/output.h - the engine's output toward the host. Blocks of the 8 voices (int16, as core::render_voices
// makes them: L and R planes, R = L for a voice rendered mono) go in on the control core; frames of the host's 12
// Overbridge channels (int32, full scale 2^31) come out where the transport needs them (the USB OUT completion, a TCP
// request). On the way: the channel map (MAIN: every TRK voice summed onto MAIN L/R, for a stock Rytm; TRACKS: each TRK
// voice on its track's channel, for the mod build), the OUT route (core/fx/out.h: a voice on PRE / POST goes, x LEV /
// PAN / WID and its mute gate, onto the PRE pair (2/3) or the POST pair (0/1) in stereo, in both modes; an OUT change
// crossfades over 8 ms), a 200 ms fade-in after each Reset (a new stream) and a hard -12 dBFS limit on every TRK
// sample (spike3's safety stage; the MAIN sum, fade and limit are core/main_out.h's, shared with the Teensy host
// build), the pairs soft-limited at the send FX returns' ceilings. An empty FIFO gives silence, never stale data, and
// is counted. With every voice on TRK the output is the old path, bit for bit.
#pragma once
#include <stdint.h>

#include "fx/out.h"
#include "main_out.h"

namespace core {
namespace out {
class Mixer;
}
}  // namespace core
#include "spsc.h"

class Output {
 public:
  enum Mode { kMain = 0, kTracks = 1 };
  static const int kVoices = 8, kChannels = 12;
  static const uint32_t kFifo = 2048;                   // frames
  static constexpr float kLimit = core::MainOut::kLimit;            // -12 dBFS
  static const uint32_t kFadeFrames = core::MainOut::kFadeFrames;   // 200 ms
  static constexpr float kMainGain = core::MainOut::kGain;          // MAIN: the voices' sum, before the limit
  static const uint8_t kVoiceToChannel[kVoices];        // ob_block.h's kVoiceToRytm

  void SetMode(Mode m) { mode_ = m; }
  Mode GetMode() const { return mode_; }
  // The OUT settings the output stage follows (core::out::control; the kernel sets it). Unset: its own defaults (every
  // voice TRK: the Mac tests of the output stage alone).
  void SetOutControl(const core::out::Control* c) { ctl_ = c ? c : &own_; }
  core::out::Control& OwnControl() { return own_; }   // tests: the default control's settings
  void Reset();                                         // transport (consumer) side only: a new stream drops the
                                                        // frames left from the last one and fades in again
  uint32_t Level() const { return fifo_.size(); }
  // control core: frames taken (FIFO room). r (optional): the R plane (null: R = L, every voice mono); st: the voices
  // whose pair is a real stereo pair in this block (render_voices' bits)
  uint32_t Push(const int16_t (*frames)[kVoices], uint32_t n, const int16_t (*r)[kVoices] = nullptr, uint8_t st = 0);
  // What the transport's other users take from a Pull, each optional (null: not wanted), one row a frame:
  //   lr     the voices as rendered, L plane [0..7] and R plane [8..15] (zeros on an underflow)
  //   feed   each voice's stand-in for its Rytm stem (a PRE / POST voice's mono sum x LEV x its gate; 0 on TRK) for
  //          the send FX (sendfx_pi FromVoices), full scale 1.0
  //   pairs  the PRE pair (L R) and the POST pair (L R) as added to 2/3 and 0/1 (the recorder's sources 23 / 24)
  struct Taps {
    int16_t (*lr)[2 * kVoices] = nullptr;
    float (*feed)[kVoices] = nullptr;
    float (*pairs)[4] = nullptr;
  };
  // transport: n frames; voices (if given): each frame's 8 voices as rendered, mono (a stereo voice's (L + R) / 2;
  // int16, before the map, the fade and the limit; zeros on an underflow) - the Sidecar's IN channels 13-20
  // (sidecar_gadget.h), the voices' bus sends
  void Pull(int32_t (*out)[kChannels], uint32_t n, int16_t (*voices)[kVoices] = nullptr, const Taps* taps = nullptr);
  // Underruns in two buckets. Underflows(): glitches while playing - an empty FIFO after it had reached its target since
  // the last Reset, or kRestartGrace frames after the Reset when it never did (a stream that never fills is starved, not
  // refilling). RestartUnderflows(): the burst each new stream produces while the control core refills (Reset empties
  // the FIFO), and anything before the first stream or without a target - not a glitch, but kept: a marginal PSU or a
  // throttling unit shows up here. Streams(): the Resets,
  // i.e. how often a stream (re)started; it climbing mid-session means the host is re-negotiating. Plain monotonic
  // counters: Reset and Pull are both the consumer's (the transport's), others only read.
  uint32_t Underflows() const { return under_; }
  uint32_t RestartUnderflows() const { return restart_under_; }
  uint32_t Streams() const { return streams_; }
  // the FIFO level the control core fills to (CEngineCores::FifoTarget); it fills to within a render block of it, so the
  // FIFO "has reached" it from target - kArmSlack. Unset (0): never armed - without a target a healthy buffer is
  // unknown, so nothing is called a glitch
  void SetFifoTarget(uint32_t frames) { target_ = frames; }
  // Where the counts fell, in frames pulled since the last Reset (audio time: 48 frames = 1 ms), kept so /status can say
  // when a count happened after the boot log is gone. kNone: did not happen.
  static const uint32_t kNone = 0xFFFFFFFFu;
  struct Diag {
    uint32_t pre_reset_pulled;   // frames pulled before the first Reset (something pulling before any stream)
    uint32_t reset_level;        // the FIFO's level when the last Reset came (before it drained)
    uint32_t first_pull_level;   // the level at the first Pull after the last Reset
    uint32_t arm_at, arm_level;  // when the FIFO first reached its target, and its level then
    uint32_t restart_first, restart_last;   // first / last empty-FIFO frame while disarmed
    uint32_t under_first, under_last;       // first / last while armed
    uint32_t since_reset;        // frames pulled since the last Reset
  };
  Diag Diagnostics() const { return d_; }
  uint32_t FramesSinceReset() const { return d_.since_reset; }
  static const uint32_t kArmSlack = 12;                  // one render block (bridge::kBlock)
  // How long after a Reset an empty FIFO may still be the refill's burst (frames pulled: 4800 = 100 ms). The control
  // core refills a target of a few ms in well under that, so a stream that has not reached its target by then is not
  // refilling but starved, and from then on its empty frames are counted as underruns (armed or not).
  static const uint32_t kRestartGrace = 4800;
  float TakePeak();                                     // the largest |sample| (0..1) sent since the last call
  // each PRE / POST voice's pair (L, R) as it went into its pair since the last call (0 for TRK voices); any core
  // (racy floats: a meter)
  void TakeVoicePeaks(float (*pk)[2]);
  Output();

 private:
  struct Frame {
    int16_t v[kVoices], r[kVoices];
    uint8_t st;
  };
  static float Limit(float x) { return x > kLimit ? kLimit : x < -kLimit ? -kLimit : x; }
  static int32_t To32(float x) { return (int32_t)(x * 2147483648.0f); }
  static int32_t Add(int32_t a, float x) {
    int64_t s = (int64_t)a + (int64_t)(x * 2147483648.0f);
    return s > 2147483647 ? 2147483647 : s < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)s;
  }
  Spsc<Frame, kFifo> fifo_;
  Mode mode_ = kMain;
  core::MainOut main_;                                  // the fade (both modes), MAIN's sum and limit
  core::out::Control own_ = [] { core::out::Control c; core::out::defaults(c); return c; }();
  const core::out::Control* ctl_ = &own_;
  // the OUT mixer (core/fx/out_mix.h: libm, hosted only), placed here by output.cpp's constructor
  alignas(16) unsigned char mix_mem_[1024];
  core::out::Mixer* mix_;
  uint32_t under_ = 0, restart_under_ = 0, streams_ = 0, target_ = 0;
  Diag d_ = {0, 0, kNone, kNone, 0, kNone, kNone, kNone, kNone, 0};
  bool armed_ = false;                                  // the FIFO reached its target since the last Reset
  float peak_ = 0.0f;
  volatile float vpk_[kVoices][2] = {};
};
