// core/fx/stutter_fx.h - STUTTER (TYPE 15): a beat repeat that is a master insert, not a send (send_fx.h, the spec's
// "STUTTER" section). Its input is the plain sum of the 8 voice stems (the send levels ignored; the stems are mono, so
// the sum is mono, on both sides), its output goes to the MAIN return (host -> Rytm 0/1) unlimited: disengaged it is
// that sum at unity, sample for sample (the user takes every track off ROUTE TO MAIN, so the whole mix goes through
// here); engaged it repeats a tempo-synced slice of what it captured.
//
// Knobs (k[0..3] = P2..P5, all p-lockable; all at 0 = off, the dry sum):
//   P2 ENGAGE   0..63 off, 64..127 on. Engaging repeats the slice that ENDED at the latest LENGTH grid point (the
//               last 1/16 before this step, say), at the phase the clock is at now: so the first repeat starts at
//               once and is in time. A grid point less than 10 ms ahead (the step report a little early) is waited
//               for. Once fired it plays its first repeat to the end (a single locked step fires one); after that it
//               returns to dry (5 ms) as soon as P2 is below 64.
//   P3 LENGTH   7 zones of 128/7: 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar (4/4, 96 ticks; bars counted from Start /
//               SPP). Read at every wrap. A slice longer than half the ring halves until it fits (1 bar < 44 BPM).
//   P4 DECAY / PITCH  0 none; 1..63 volume decay per repeat, (v/63) x 12 dB (repeat n at -n x that); 64..127 pitch
//               down per repeat, 0.25 + (v-64)/63 x 11.75 semitones (repeat n at 2^(-n st/12) speed, floor 1/16: a
//               tape stop in steps). The repeat keeps its grid length: a slower repeat plays less of the slice. Not
//               "centre = none": every Sidecar knob's 0 is its default, and a kit's 0 must be no change.
//   P5 GATE     0 the whole repeat; 1..127 each repeat sounds for 100 % .. 10 % of its length (1 - 0.9 v/127), then
//               silence (a 2 ms fade) to the next. No wet/dry: an engaged repeat replaces the mix (that is the point).
// Repeat 0 (the first) is the captured slice as it was (no pitch, no decay): read at speed 1 it is the captured
// samples exactly.
//
// Clock (Clock below): the Rytm's 24 ppqn F8 ticks, FA / FB / FC and SPP (MIDI in, the control side) pack into one
// word; the audio side stamps each new tick with its frame count (+ lag) and takes the tempo from the last 24 ticks.
// The grid is the song position while running; with no tick for 250 ms, or stopped, it runs free: the last tempo (120
// BPM if none), the grid anchored at the engage. All Sound Off (CC 120): the repeat drops to dry (5 ms) and stays
// dry until P2 is seen below 64.
//
// Fades (no clicks): engage and disengage 5 ms dry <-> repeat (linear), every slice boundary a 2 ms crossfade (the
// old repeat reads on past its end while the new one starts), the gate's close 2 ms.
//
// The ring: 2^19 frames of mono float = 2 MiB (10.9 s at 48 kHz: 2 bars at 88 BPM, 1 bar at 44). Built where
// RYTM_SENDFX_LARGE (the Pi, the Mac): not the Teensy, which has no SendFx and not the RAM; there it is MISSING.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fx/knob_labels.h"

namespace core {
namespace fx {

// The Rytm's labels for P2..P5 (knob_labels.h, 0x24): P2 OFF / ON at 64; P3's seven lengths (zone = v * 7 / 128, so
// each starts at ceil(128 z / 7)); P4 NONE at 0, DCY 1..63 (shown 1..63), PTCH 64..127 (shown 1..64); P5 a number.
// P2 and P3 are pure selectors (step: the Rytm steps them, storing 0 / 64 and the lengths' lo); P4 has numbers inside.
inline constexpr Zone kStutterZones[] = {
    {0, 0, 0, 0, 1, "OFF"},    {0, 64, 0, 0, 1, "ON"},    {1, 0, 0, 0, 1, "1/64"},   {1, 19, 0, 0, 1, "1/32"},
    {1, 37, 0, 0, 1, "1/16"},  {1, 55, 0, 0, 1, "1/8"},   {1, 74, 0, 0, 1, "1/4"},   {1, 92, 0, 0, 1, "1/2"},
    {1, 110, 0, 0, 1, "1BAR"}, {2, 0, 0, 0, 0, "NONE"},   {2, 1, 1, 1, 0, "DCY"},    {2, 64, 1, 1, 0, "PTCH"}};
inline constexpr KnobLabels kStutterLabels = {{"ENG", "LEN", "D/P", "GATE"}, kStutterZones, 12, {"Engage", "Length", "Decay / pitch", "Gate"}};

// The MIDI clock word (Control::clock): bits 0..23 the song position of the latest tick + 1 (0: Start seen, no tick
// yet), 24..30 the tick count mod 128, 31 running. One aligned word: a torn read cannot happen.
struct ClockWord {
  static uint32_t pack(int32_t pos, uint32_t ticks, bool run) {
    return ((uint32_t)(pos + 1) & 0xFFFFFFu) | (ticks & 0x7Fu) << 24 | (run ? 1u << 31 : 0u);
  }
  static int32_t pos(uint32_t w) { return (int32_t)(w & 0xFFFFFFu) - 1; }
  static uint32_t ticks(uint32_t w) { return w >> 24 & 0x7Fu; }
  static bool running(uint32_t w) { return (w >> 31) != 0; }
};

class Stutter {
 public:
  static const int kRingBits = 19, kRing = 1 << kRingBits;   // frames (mono float): 2 MiB
  static const int kEnv = 240, kXf = 96, kSnap = 480;        // 5 ms engage / release, 2 ms boundary, 10 ms snap
  static const int kNoClock = 12000;                         // 250 ms without a tick: free run
  static constexpr float kFreeFpt = 1000.0f;                 // 120 BPM at 48 kHz: frames per tick
  // the LENGTH zones in half ticks: 1/64 .. 1 bar
  static int zone_half_ticks(uint8_t v) {
    static const int16_t kHt[7] = {3, 6, 12, 24, 48, 96, 192};
    int z = (v > 127 ? 127 : v) * 7 / 128;
    return kHt[z];
  }
  static float decay_db(uint8_t v) { return v >= 1 && v <= 63 ? 12.0f * (float)v / 63.0f : 0.0f; }
  static float semis(uint8_t v) { return v >= 64 ? 0.25f + 11.75f * (float)((v > 127 ? 127 : v) - 64) / 63.0f : 0.0f; }
  static float gate(uint8_t v) { return v ? 1.0f - 0.9f * (float)(v > 127 ? 127 : v) / 127.0f : 1.0f; }

  void Clear() {
    memset(buf_, 0, sizeof buf_);
    w_ = 0, filled_ = 0;
    ResetClock();
    Dry();
    latch_ = false;
  }
  void Dry() { mode_ = kDryMode, env_ = 0, xf_ = 0; }
  // selected again: the old capture forgotten (read as silence), the clock taken afresh, dry
  void Restart() {
    filled_ = 0;
    ResetClock();
    Dry();
  }
  // All Sound Off: to dry in kEnv, then dry until P2 < 64
  void Kill() {
    if (mode_ == kPending) mode_ = kDryMode;
    if (mode_ == kOn) mode_ = kRelease;
    latch_ = true;
  }
  bool Engaged() const { return mode_ == kOn || mode_ == kRelease || env_ > 0; }
  float FramesPerTick() const { return fpt_; }
  bool HasClock() const { return clock_ok_; }
  int SliceFrames() const { return len_; }
  uint32_t SliceStart() const { return start_; }   // the ring frame (Frame()'s count) the slice starts at
  uint32_t Frame() const { return w_; }            // frames captured so far (the next sample's index)

  // n frames of the dry sum in -> out (both sides the same: mono), clock = Control::clock, lag = frames the audio
  // trails the MIDI (added to each tick's stamp), k = P2..P5.
  void Process(const float* in, int n, float* out, uint32_t clock, int lag, const uint8_t* k) {
    TakeClock(clock, lag);
    bool want = k[0] >= 64;
    if (!want) latch_ = false;
    if (latch_) want = false;
    for (int i = 0; i < n; ++i) {
      float x = in[i];
      buf_[w_ & (kRing - 1)] = x;
      ++w_;
      if (filled_ < kRing) ++filled_;
      // the engage / release decisions, per frame
      if (mode_ == kDryMode && want) Arm(k);
      if (mode_ == kPending && !want) mode_ = kDryMode;
      if (mode_ == kPending && (int32_t)(w_ - 1 - fire_at_) >= 0) Fire(w_ - 1, w_ - 1, k);
      if ((mode_ == kOn || mode_ == kRelease) && t_ >= len_) Wrap(want, k);   // end of a repeat
      if (mode_ == kOn && !want && fired_) mode_ = kRelease;   // after the first repeat: released at once
      // envelope toward the repeat while on, toward dry otherwise (a release starts at once, the repeat reads on)
      bool to_wet = mode_ == kOn;
      if (to_wet && env_ < kEnv) ++env_;
      if (!to_wet && env_ > 0) --env_;
      if (mode_ == kRelease && env_ == 0) mode_ = kDryMode;
      if (env_ == 0 && mode_ != kOn) {   // dry: the input itself
        out[i] = x;
        continue;
      }
      float y = Voice(t_, rate_, gain_, gate_);
      if (xf_ > 0) {   // the boundary: the old repeat out, the new one in
        float a = (float)xf_ / (float)(kXf + 1);
        y = y * (1.0f - a) + a * Voice(old_t_, old_rate_, old_gain_, old_gate_);
        ++old_t_, --xf_;
      }
      ++t_;
      float e = (float)env_ / (float)kEnv;
      out[i] = env_ == kEnv ? y : x + (y - x) * e;
    }
  }

 private:
  enum Mode : uint8_t { kDryMode, kPending, kOn, kRelease };
  void ResetClock() {
    clock_word_ = 0, have_word_ = false, nstamp_ = 0, clock_ok_ = false, fpt_ = kFreeFpt;
    last_tick_ = 0, pos_ = -1, running_ = false;
  }
  void TakeClock(uint32_t c, int lag) {
    uint32_t now = w_ + (uint32_t)lag;
    if (!have_word_) clock_word_ = c, have_word_ = true;
    uint32_t d = (ClockWord::ticks(c) - ClockWord::ticks(clock_word_)) & 0x7Fu;
    running_ = ClockWord::running(c);
    pos_ = ClockWord::pos(c);
    clock_word_ = c;
    for (uint32_t j = 0; j < d; ++j) {   // d new ticks, all stamped now
      stamp_[nstamp_ % kStamps] = now;
      ++nstamp_;
    }
    if (d) {
      last_tick_ = now;
      int m = nstamp_ < kStamps ? (int)nstamp_ : kStamps;   // the oldest of up to 25 stamps
      if (m >= 3) {
        uint32_t old = stamp_[(nstamp_ - m) % kStamps], neu = stamp_[(nstamp_ - 1) % kStamps];
        float f = (float)(neu - old) / (float)(m - 1);
        if (f > 50.0f) fpt_ = f, clock_ok_ = true;   // above ~1200 BPM: a burst, not a clock
      }
    }
    if (clock_ok_ && (int32_t)(now - last_tick_) > kNoClock) clock_ok_ = false;   // the tempo stays, free run
  }
  int SliceLen(const uint8_t* k) const {
    float f = (float)zone_half_ticks(k[1]) * 0.5f * fpt_;
    int L = (int)(f + 0.5f);
    while (L > kRing / 2 - kEnv) L >>= 1;
    return L < 2 * kXf ? 2 * kXf : L;
  }
  // the grid point at or before frame f (ring frames) for a division of ht half ticks; false: no grid (free run)
  bool GridBefore(uint32_t f, int ht, uint32_t* g, float* left) const {
    if (!clock_ok_ || !running_ || pos_ < 0) return false;
    float frac = (float)(int32_t)(f - last_tick_) / fpt_;   // ticks since the latest
    frac = frac < 0.0f ? 0.0f : frac > 0.999f ? 0.999f : frac;
    double p2 = 2.0 * ((double)pos_ + frac);              // half ticks
    double ph = fmod(p2, (double)ht);                      // half ticks since the grid point
    float back = (float)(ph * 0.5 * fpt_);
    *g = f - (uint32_t)(back + 0.5f);
    *left = (float)((ht - ph) * 0.5 * fpt_);               // frames to the next one
    return true;
  }
  void Arm(const uint8_t* k) {
    uint32_t f = w_ - 1, g;
    float left;
    if (GridBefore(f, zone_half_ticks(k[1]), &g, &left)) {
      if (left < (float)kSnap) {   // the next grid point is close: the report was early, wait for it
        mode_ = kPending, fire_at_ = f + (uint32_t)(left + 0.5f);
        return;
      }
      Fire(f, g, k);
    } else {
      Fire(f, f, k);   // free run: the grid starts here
    }
  }
  // engage at frame f, the grid point g <= f: repeat [g - L, g) from phase f - g
  void Fire(uint32_t f, uint32_t g, const uint8_t* k) {
    len_ = SliceLen(k);
    start_ = g - (uint32_t)len_;
    t_ = (int)(f - g);
    if (t_ >= len_) t_ = len_ - 1;
    rep_ = 0, fired_ = false, xf_ = 0;
    SetRepeat(k);
    mode_ = kOn;
  }
  void SetRepeat(const uint8_t* k) {
    float st = semis(k[2]), db = decay_db(k[2]);
    rate_ = st > 0.0f ? exp2f(-(float)rep_ * st / 12.0f) : 1.0f;
    if (rate_ < 1.0f / 16.0f) rate_ = 1.0f / 16.0f;
    gain_ = db > 0.0f ? powf(10.0f, -(float)rep_ * db / 20.0f) : 1.0f;
    if (gain_ < 3e-5f) gain_ = 0.0f;
    gate_ = gate(k[3]) < 1.0f ? (int)(gate(k[3]) * (float)len_) : 0x7FFFFFFF;
  }
  void Wrap(bool want, const uint8_t* k) {
    fired_ = true;
    if (mode_ == kRelease || !want) {   // released: dry from here, the repeat reads on through the fade
      mode_ = kRelease;
      len_ = 0x7FFFFFFF;   // no more wraps
      return;
    }
    old_t_ = t_, old_rate_ = rate_, old_gain_ = gain_, old_gate_ = gate_, xf_ = kXf;
    len_ = SliceLen(k);   // the start stays (on the grid); LENGTH read again
    t_ = 0;
    ++rep_;
    SetRepeat(k);
  }
  // repeat sample at t frames into the repeat, speed r, gain g, gate at frame gate: the slice read with linear
  // interpolation (exact at speed 1), the gate closing over kXf
  float Voice(int t, float r, float g, int gate) const {
    float gt = 1.0f;
    if (t >= gate) gt = t - gate >= kXf ? 0.0f : 1.0f - (float)(t - gate) / (float)kXf;
    if (gt == 0.0f || g == 0.0f) return 0.0f;
    float p = (float)t * r;
    uint32_t i = (uint32_t)p;
    float fr = p - (float)i;
    uint32_t at = start_ + i;
    float a = Read(at), y = a;
    if (fr != 0.0f) y = a + (Read(at + 1) - a) * fr;
    return g == 1.0f && gt == 1.0f ? y : y * g * gt;
  }
  float Read(uint32_t at) const {   // a frame not captured (or overwritten): silence
    uint32_t age = w_ - at;
    if (age == 0 || age > filled_ || age > (uint32_t)kRing) return 0.0f;
    return buf_[at & (kRing - 1)];
  }

  static const int kStamps = 25;
  float buf_[kRing];
  uint32_t w_ = 0, filled_ = 0;
  // clock
  uint32_t clock_word_ = 0, stamp_[kStamps], nstamp_ = 0, last_tick_ = 0;
  bool have_word_ = false, clock_ok_ = false, running_ = false;
  int32_t pos_ = -1;
  float fpt_ = kFreeFpt;
  // the repeat
  Mode mode_ = kDryMode;
  bool latch_ = false, fired_ = false;
  uint32_t start_ = 0, fire_at_ = 0;
  int len_ = 0, t_ = 0, rep_ = 0, env_ = 0, xf_ = 0, old_t_ = 0, gate_ = 0x7FFFFFFF, old_gate_ = 0x7FFFFFFF;
  float rate_ = 1.0f, gain_ = 1.0f, old_rate_ = 1.0f, old_gain_ = 1.0f;
};

}  // namespace fx
}  // namespace core
