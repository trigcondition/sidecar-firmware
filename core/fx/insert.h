// core/fx/insert.h - the Sidecar's INSERT effects (docs/INSERT-FX.md): per Sidecar voice a chain of two insert slots,
// applied to the voice before it goes back into the Rytm's analog chain (the Overbridge return per voice, mono), and per
// FX bus a chain of two more: bus 1 / 2 after send FX 1 / 2 (core/fx/send_fx.h, stereo), bus 3 a dry bus of the
// Sidecar voices' own sends (stereo, no send FX of its own). The Pi and the Mac (CORE_POLY builds); the Teensy has none.
//
// Two halves, as send_fx.h:
//   ins::Control  the values the DSP reads: each chain's slot TYPEs and the 4 params as they play (the knob, a step's
//                 p-lock in its place, an LFO's offset on top: insert.cpp works them out on the control side), each
//                 voice's send levels to the 3 buses, bus 3's route. Byte fields: a torn read cannot happen.
//   ins::Engine   the DSP: allocation-free; the DELAY's lines (kLine floats a channel) are memory the platform places
//                 (heap_buffers.h on the Pi, a static array elsewhere). A voice's chain runs on the render core that
//                 renders the voice (render_voices, after the voice's own sound: chains of other voices are other
//                 cores' business); the bus chains on the send FX's core (the Pi's render core 2).
//
// TYPE ids (stable: the card's files name them, sim/protocol.py INSERT_TYPES): 0 OFF, 1 FILTER, 2 CRUSH, 3 DRIVE,
// 4 EQ, 5 COMP, 6 DELAY, 7 TREMOLO, 8 RINGMOD, 9 AMP. Params P1..P4 0..127 (labels: kLabels):
//   FILTER  CUT 20 Hz x 2^(10 v) | RES Q 0.5..20 | MODE LP -> BP -> HP (a continuous morph) | DRV 0..+24 dB before it
//           (a TPT state-variable filter: stable for any cutoff / resonance, modulated per block without zipper)
//   CRUSH   RATE the hold 1..64 samples | BITS 16..2 | DRV 0..+18 dB before the quantizer | MIX
//   DRIVE   DRV 0..+30 dB | SHPE soft saturation -> sine fold | TONE a low-pass after, 18 kHz .. 1 kHz | MIX
//           (the level made up for a -12 dBFS input)
//   EQ      LOW shelf 250 Hz | MID bell (Q 0.9) | HIGH shelf 4 kHz, each +-15 dB (64 = 0 dB) | FREQ the bell 200 Hz..8 kHz
//   COMP    THR 0..-40 dB | RAT 1..20 | SPD attack 0.3..30 ms with release 40..640 ms | GAIN makeup 0..+24 dB
//           (peak detector, 6 dB soft knee)
//   DELAY   TIME 1..80 ms (a change glides: pitch, no click) | FDBK 0..0.92 | MOD a 0.7 Hz sweep up to 4 ms (chorus
//           / flanger) | MIX; the stereo bus chains run R a quarter cycle behind L
//   TREMOLO RATE 0.1..20 Hz | DPTH | SHPE sine -> square | PAN the R channel's phase offset (0: tremolo, 127: auto-pan;
//           on a voice's mono chain the pan sums to its mono tremolo)
//   RINGMOD FREQ 10 Hz..5 kHz | SHPE sine -> square | MOD a 0.3 Hz +-1 octave sweep | MIX
//   AMP     MODL a NAM A2-Lite capture of /NAM/ | GAIN -18..+18 dB into it | TONE a +-6 dB tilt | LEV -36..+6 dB
//           (core/fx/amp.h: the voice chains only, mono; a capture loads off the render core and crossfades in)
//
// A TYPE change never clicks: the slot's wet share crossfades to dry over kFadeFrames (5 ms), the new effect starts
// from a clean state, then fades in (OFF is the dry end of that fade). Params are smoothed per block (3 ms).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fx/amp.h"

namespace core {
namespace ins {

constexpr int kVoices = 8, kSlots = 2, kParams = 4, kBuses = 3;
constexpr int kChains = kVoices + kBuses;   // 0..7 the voices' chains (mono), 8..10 the buses' (stereo)
constexpr int kBus0 = kVoices;              // chain of bus b (0..2): kBus0 + b
enum Type : uint8_t { kOff = 0, kFilter, kCrush, kDrive, kEq, kComp, kDelay, kTrem, kRing, kAmp, kTypes };
constexpr int kLine = 4096;          // DELAY's line, a power of 2: 85 ms at 48 kHz (TIME's 80 ms + MOD's 4 ms)
// a line per voice slot channel (two: a voice on OUT PRE / POST runs its chain in stereo, core/fx/out.h), two per bus slot
constexpr int kLines = kVoices * kSlots * 2 + kBuses * kSlots * 2;
constexpr int kFadeFrames = 240;     // a TYPE change: 5 ms out, 5 ms in
constexpr int kChunk = 16;           // frames per parameter update (a voice block is 12)
constexpr float kPi = 3.14159265f;

// ins1112, the POST EQ: one more stage at the end of every chain, after both slots (AMP among them). The voices' is
// the full four; the buses' are the send FX returns (bus 1 = send FX 1, bus 2 = send FX 2), where HPF and LPF are what
// stock's DELAY and REVERB put on their returns. Knobs, all 0..127:
//   0 HPF  0 = off, else 20 Hz .. 20 kHz (20 x 2^(10 v))      - "how much low end is taken off", 0 does nothing
//   1 LPF  0 = off, else 20 kHz .. 20 Hz (20k x 2^(-10 v))    - the same the other way up, as the Rytm's delay reads it
//   2 LOW  a low shelf at 250 Hz, 64 flat, +-15 dB
//   3 HIGH a high shelf at 4 kHz, 64 flat, +-15 dB
constexpr int kEqParams = 4;
// What a knob is worth in Control::peq. 0 means "does nothing" for every one of them, so a zeroed Control (reset, a
// bench, a test) is a chain with no post EQ at all and the stage is skipped bit for bit: the shelves are therefore
// stored as the knob less 64 (an int8_t, byte-sized like every other Control field: a torn read cannot happen).
inline int eq_store(int k, int knob) { return k >= 2 ? knob - 64 : knob; }
inline int eq_knob(int k, int st) { return k >= 2 ? st + 64 : st; }
constexpr uint8_t kEqFlat(int k) { return k >= 2 ? (uint8_t)64 : (uint8_t)0; }

struct Control {
  volatile uint8_t type[kChains][kSlots];
  volatile uint8_t play[kChains][kSlots][kParams];   // 0..127 as it plays (insert.cpp: knob / lock + LFO)
  volatile uint8_t send[kVoices][kBuses];            // 0..127: voice v's send to bus b (bus 1 / 2: the send FX)
  volatile uint8_t route3;                           // bus 3: 0 PRE (host -> Rytm 2/3), 1 POST (0/1)
  volatile int8_t peq[kChains][kEqParams];           // ins1112 the POST EQ as it plays (eq_store: 0 = does nothing)
};
extern Control control;

// ---- small DSP helpers (no tables; the Pi's hosted side has libm)
inline float flush(float x) { return (x > -1e-20f && x < 1e-20f) ? 0.0f : x; }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
// a soft saturation toward +-1: ~linear below 1/4, 1 at |x| >= 3 (x (27 + x^2) / (27 + 9 x^2))
inline float sat(float x) {
  float u = clampf(x, -3.0f, 3.0f);
  return u * (27.0f + u * u) / (27.0f + 9.0f * u * u);
}
// sin(2 pi p) for any p (error ~1e-4)
inline float sin2pi(float p) {
  p -= floorf(p);
  float x = 1.0f - 4.0f * fabsf(p - 0.5f);   // triangle: the sine's phase - 1/4
  float x2 = x * x;
  return -x * (1.5707963f - x2 * (0.6459640f - x2 * 0.0794693f));
}
inline float fast_log2(float x) {   // x > 0, error ~1e-4
  uint32_t i;
  memcpy(&i, &x, 4);
  uint32_t mi = (i & 0x007FFFFFu) | 0x3F000000u;
  float m;
  memcpy(&m, &mi, 4);
  return (float)i * 1.1920928955078125e-7f - 124.22551499f - 1.498030302f * m - 1.72587999f / (0.3520887068f + m);
}
inline float fast_exp2(float p) {   // error ~1e-4 relative
  if (p < -126.0f) p = -126.0f;
  if (p > 60.0f) p = 60.0f;
  int w = (int)p;
  float z = p - (float)w + (p < 0.0f ? 1.0f : 0.0f);
  uint32_t i = (uint32_t)((float)(1 << 23) * (p + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z));
  float r;
  memcpy(&r, &i, 4);
  return r;
}

// One channel of one slot: every type's state (small) side by side, DELAY's line placed (null: DELAY plays dry).
struct Dsp {
  float s1 = 0, s2 = 0;          // FILTER's SVF; EQ's low shelf
  float m1 = 0, m2 = 0, h1 = 0, h2 = 0;   // EQ's bell and high shelf
  float hold = 0, ph = 0, lp = 0;   // CRUSH's sample-and-hold and phase; DRIVE's tone
  float env = 0;                 // COMP's detector
  float lfo = 0;                 // DELAY / TREMOLO / RINGMOD phase, 0..1
  float car = 0;                 // RINGMOD's carrier phase
  float dly = 0;                 // DELAY's smoothed time (samples), 0 = start at the target
  uint32_t w = 0;                // DELAY's write index
  float* line = nullptr;
  void Clear() {
    float* l = line;
    memset(this, 0, sizeof *this);
    line = l;
  }
  void ClearLine() {
    if (line) memset(line, 0, sizeof(float) * kLine);
  }
};

// The per-block coefficients of a slot's params (computed when a param moved: tanf / powf once a change)
struct Coef {
  float a[12];
};

// The SVF of A. Simper (Cytomic, "Solving the continuous SVF equations using trapezoidal integration"): g = tan(pi fc
// / sr), k = 1 / Q; one step returns v1 (band) and v2 (low); high = x - k v1 - v2.
struct SvfK {
  float a1, a2, a3, k;
  void set(float g, float k_) {
    k = k_;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
};
inline void svf(const SvfK& c, float x, float& s1, float& s2, float& bp, float& lp) {
  float v3 = x - s2;
  float v1 = c.a1 * s1 + c.a2 * v3;
  float v2 = s2 + c.a2 * s1 + c.a3 * v3;
  s1 = 2.0f * v1 - s1;
  s2 = 2.0f * v2 - s2;
  bp = v1, lp = v2;
}

// One slot: the TYPE playing, the wet share (fade), the smoothed params, a Dsp per channel.
struct Slot {
  uint8_t cur = kOff;
  float wet = 0.0f;          // 0 dry .. 1 the effect
  float sp[kParams] = {0, 0, 0, 0};
  bool fresh = true;         // the params start at their targets (a new TYPE, after a fade-out)
  Dsp d[2];
  float key = -1.0f;         // the params' sum the coefficients were computed for (+ cur)
  float feed0 = 0, feed1 = 0;   // this chunk's wet share, start and end: DELAY's line takes its input faded in with it
  // coefficients (per type, recomputed when the smoothed params move)
  SvfK f;                    // FILTER
  SvfK eq[3];                // EQ: low shelf, bell, high shelf
  float c[8];
};

inline float knob01(uint8_t v) { return (float)(v > 127 ? 127 : v) * (1.0f / 127.0f); }

// ins1112: a chain's POST EQ - HPF, LPF and the two shelves, after both slots. Each stage is skipped while its knob
// does nothing, and the whole stage is skipped (and its state cleared) while all four do: a chain with no post EQ
// set costs nothing and passes its samples through untouched.
struct PeqCh {   // one channel's filter state
  float hp1 = 0, hp2 = 0, lp1 = 0, lp2 = 0, ls1 = 0, ls2 = 0, hs1 = 0, hs2 = 0;
};
struct Peq {
  float wet = 0.0f;                        // 0 dry .. 1 filtered: the engage / release fade (kFadeFrames, as a TYPE's)
  float sp[kEqParams] = {0, 0, 0, 0};       // the smoothed knobs, in Control::peq's units
  float key[kEqParams] = {-1, -1, -1, -1};   // what the coefficients were computed for
  bool fresh = true, idle = true;
  SvfK hp, lp, ls, hs;
  float al = 1.0f, ah = 1.0f;              // the shelves' A = 10^(dB / 40)
  bool uhp = false, ulp = false, uls = false, uhs = false;   // the stages that run
  PeqCh d[2];
  void Clear() {
    d[0] = PeqCh(), d[1] = PeqCh();
    wet = 0.0f, fresh = true;
    for (int j = 0; j < kEqParams; ++j) sp[j] = 0.0f, key[j] = -1.0f;
    uhp = ulp = uls = uhs = false;
  }
};
// a chain's post EQ does something (the control side's view: the audio side also keeps running while it fades out)
inline bool peq_set(const Control& k, int c) {
  for (int j = 0; j < kEqParams; ++j)
    if (k.peq[c][j]) return true;
  return false;
}

class Engine {
 public:
  explicit Engine(float sample_rate = 48000.0f) : sr_(sample_rate) {}
  // The DELAY lines: kLines x kLine floats (zeroed), or null (DELAY plays dry). Before any Process.
  void Place(float (*lines)[kLine]) {
    int k = 0;
    for (int c = 0; c < kChains; ++c)
      for (int s = 0; s < kSlots; ++s)
        for (int ch = 0; ch < 2; ++ch) slot_[c][s].d[ch].line = lines ? lines[k++] : nullptr;
  }
  void Reset() {
    for (int c = 0; c < kChains; ++c)
      for (int s = 0; s < kSlots; ++s) {
        Slot& x = slot_[c][s];
        float* l0 = x.d[0].line;
        float* l1 = x.d[1].line;
        x = Slot();
        x.d[0].line = l0, x.d[1].line = l1;
        x.d[0].ClearLine(), x.d[1].ClearLine();
      }
    for (int c = 0; c < kChains; ++c) peq_[c].Clear(), peq_[c].idle = true;
    faults_ = 0;
  }
  // Chain c runs or fades (anything to do): a type set, a fade not yet at dry, or a post EQ set or still releasing
  bool Active(int c, const Control& k) const {
    for (int s = 0; s < kSlots; ++s)
      if (k.type[c][s] || slot_[c][s].cur || slot_[c][s].wet > 0.0f) return true;
    return peq_set(k, c) || !peq_[c].idle;   // ins1112
  }
  // A voice's chain, mono, in place (n any; processed in kChunk pieces)
  void ProcessMono(int c, float* x, int n, const Control& k) {
    for (int i = 0; i < n; i += kChunk) {
      int m = n - i < kChunk ? n - i : kChunk;
      for (int s = 0; s < kSlots; ++s) Run(c, s, x + i, nullptr, m, k);
      RunEq(c, x + i, nullptr, m, k);   // ins1112: the POST EQ, after both slots (AMP among them)
    }
  }
  // A voice's chain in stereo (OUT PRE / POST), in place: L in x, R in r. The first stereo block after mono: R's
  // state starts as a copy of L's (the chain goes on seamlessly on both sides)
  void ProcessVoiceStereo(int c, float* x, float* r, int n, const Control& k) {
    if (!st_[c]) {
      st_[c] = true;
      for (int s = 0; s < kSlots; ++s) {
        Slot& o = slot_[c][s];
        float* l1 = o.d[1].line;
        o.d[1] = o.d[0];
        o.d[1].line = l1;
        if (l1 && o.d[0].line) memcpy(l1, o.d[0].line, sizeof(float) * kLine);
        else if (l1) memset(l1, 0, sizeof(float) * kLine);
      }
      peq_[c].d[1] = peq_[c].d[0];   // ins1112: the post EQ's R side starts where L is, as the slots' do
    }
    for (int i = 0; i < n; i += kChunk) {
      int m = n - i < kChunk ? n - i : kChunk;
      for (int s = 0; s < kSlots; ++s) Run(c, s, x + i, r + i, m, k);
      RunEq(c, x + i, r + i, m, k);
    }
  }
  void MarkMono(int c) { st_[c] = false; }
  // A bus's chain, stereo, in place
  void ProcessStereo(int c, float (*y)[2], int n, const Control& k) {
    float l[kChunk], r[kChunk];
    for (int i = 0; i < n; i += kChunk) {
      int m = n - i < kChunk ? n - i : kChunk;
      for (int j = 0; j < m; ++j) l[j] = y[i + j][0], r[j] = y[i + j][1];
      for (int s = 0; s < kSlots; ++s) Run(c, s, l, r, m, k);
      RunEq(c, l, r, m, k);   // ins1112: bus 1 / 2 = send FX 1 / 2's return filter
      for (int j = 0; j < m; ++j) y[i + j][0] = l[j], y[i + j][1] = r[j];
    }
  }
  uint32_t Faults() const { return faults_; }
  const Slot& slot(int c, int s) const { return slot_[c][s]; }
  const Peq& peq(int c) const { return peq_[c]; }

 private:
  // one slot over m <= kChunk frames: L in x, R in r (null: mono)
  void Run(int c, int s, float* x, float* r, int m, const Control& k) {
    Slot& o = slot_[c][s];
    const int want = k.type[c][s] < kTypes ? k.type[c][s] : kOff;
    if (o.cur == kOff && want == kOff && o.wet <= 0.0f) return;   // off (the usual case): nothing
    const float step = (float)m / kFadeFrames;
    if (want != o.cur && o.wet <= 0.0f) {   // faded out (or off): the new effect from clean
      __atomic_store_n(&o.cur, (uint8_t)want, __ATOMIC_RELEASE);
      o.fresh = true;
      if (want == kAmp) amp::fresh(c, s);
      o.d[0].Clear(), o.d[1].Clear();
      if (want == kDelay) o.d[0].ClearLine(), o.d[1].ClearLine();
      o.key = -1.0f;
      if (want == kOff) return;
    }
    const float w0 = o.wet, w1 = clampf(want == o.cur && o.cur != kOff ? w0 + step : w0 - step, 0.0f, 1.0f);
    o.wet = w1;
    // the params toward their targets (3 ms one-pole a chunk; at once when fresh)
    const float b = (float)m / ((float)m + 0.003f * sr_);
    float sum = (float)o.cur * 1000.0f;
    for (int j = 0; j < kParams; ++j) {
      float t = knob01(k.play[c][s][j]);
      o.sp[j] = o.fresh ? t : o.sp[j] + (t - o.sp[j]) * b;
      if (fabsf(o.sp[j] - t) < 1e-4f) o.sp[j] = t;
      sum += o.sp[j] * (float)(1 << (3 * j));
    }
    o.fresh = false;
    if (sum != o.key) o.key = sum, Coefs(o);
    float y[2][kChunk];
    o.feed0 = w0, o.feed1 = w1;
    if (o.cur == kAmp) {
      amp::run(c, s, o.sp, x, r, y[0], y[1], m);   // mono: (L + R) / 2 onto both sides
    } else {
      Effect(o, 0, x, y[0], m, c >= kBus0 || r ? 0 : -1);
      if (r) Effect(o, 1, r, y[1], m, 1);
    }
    // NaN or a runaway: this slot cleared, silent until it fades in again
    float chk = 0.0f;
    for (int i = 0; i < m; ++i) chk += fabsf(y[0][i]) + (r ? fabsf(y[1][i]) : 0.0f);
    if (!(chk < 1e5f)) {
      ++faults_;
      o.d[0].Clear(), o.d[1].Clear(), o.d[0].ClearLine(), o.d[1].ClearLine(), o.wet = 0.0f, o.fresh = true;
      for (int i = 0; i < m; ++i) y[0][i] = x[i], y[1][i] = r ? r[i] : 0.0f;
    }
    // dry -> wet, the share ramped linearly over the chunk (fully wet: the effect's output as is)
    if (w0 >= 1.0f && w1 >= 1.0f) {
      for (int i = 0; i < m; ++i) x[i] = y[0][i];
      if (r)
        for (int i = 0; i < m; ++i) r[i] = y[1][i];
      return;
    }
    const float dw = (w1 - w0) / (float)m;
    float wv = w0;
    for (int i = 0; i < m; ++i) {
      wv += dw;
      x[i] += wv * (y[0][i] - x[i]);
      if (r) r[i] += wv * (y[1][i] - r[i]);
    }
  }

  // ins1112: the POST EQ over m <= kChunk frames, after both slots. L in x, R in r (null: mono).
  // A knob at 0 does nothing, so a chain whose four knobs are all 0 skips this entirely and its samples are passed
  // through untouched (bit for bit: a kit with no post EQ sounds exactly as it did before ins1112). Engaging and
  // releasing crossfade dry <-> filtered over kFadeFrames, the same 5 ms a TYPE change takes, so the step from "no
  // filter" to "a 21 Hz high pass" at the bottom of the HPF knob cannot click.
  void RunEq(int c, float* x, float* r, int m, const Control& k) {
    Peq& o = peq_[c];
    const bool want = peq_set(k, c);
    if (!want && o.wet <= 0.0f) {
      if (!o.idle) o.Clear(), o.idle = true;
      return;   // nothing at all: the chain is untouched
    }
    o.idle = false;
    const float step = (float)m / kFadeFrames;
    const float w0 = o.wet, w1 = clampf(want ? w0 + step : w0 - step, 0.0f, 1.0f);
    o.wet = w1;
    if (want) {   // the knobs toward their targets (3 ms a chunk, as a slot's; frozen while releasing: a filter must
      const float b = (float)m / ((float)m + 0.003f * sr_);   // not sweep on its way out)
      bool moved = false;
      for (int j = 0; j < kEqParams; ++j) {
        float t = (float)k.peq[c][j];
        o.sp[j] = o.fresh ? t : o.sp[j] + (t - o.sp[j]) * b;
        if (fabsf(o.sp[j] - t) < 1e-3f) o.sp[j] = t;
        moved = moved || o.sp[j] != o.key[j];
      }
      o.fresh = false;
      if (moved) EqCoefs(o);
    }
    float y[2][kChunk];
    EqRun(o, 0, x, y[0], m);
    if (r) EqRun(o, 1, r, y[1], m);
    float chk = 0.0f;   // NaN or a runaway: cleared, the chain passes dry until it fades in again
    for (int i = 0; i < m; ++i) chk += fabsf(y[0][i]) + (r ? fabsf(y[1][i]) : 0.0f);
    if (!(chk < 1e5f)) {
      ++faults_;
      o.Clear();
      for (int i = 0; i < m; ++i) y[0][i] = x[i], y[1][i] = r ? r[i] : 0.0f;
    }
    if (w0 >= 1.0f && w1 >= 1.0f) {   // fully in: the filtered signal as it is
      for (int i = 0; i < m; ++i) x[i] = y[0][i];
      if (r)
        for (int i = 0; i < m; ++i) r[i] = y[1][i];
      return;
    }
    const float dw = (w1 - w0) / (float)m;
    float wv = w0;
    for (int i = 0; i < m; ++i) {
      wv += dw;
      x[i] += wv * (y[0][i] - x[i]);
      if (r) r[i] += wv * (y[1][i] - r[i]);
    }
  }

  void EqCoefs(Peq& o) {
    const float ny = 0.45f * sr_;
    for (int j = 0; j < kEqParams; ++j) o.key[j] = o.sp[j];
    o.uhp = o.sp[0] > 0.0f;
    if (o.uhp) {   // 20 Hz x 2^(10 v): 20 Hz at the first step of the knob (open), ~20 kHz at the top
      float fc = fminf(20.0f * exp2f(10.0f * o.sp[0] * (1.0f / 127.0f)), ny);
      o.hp.set(tanf(kPi * fc / sr_), 1.41421356f);   // k = 1 / Q, Q 0.707: Butterworth, as a return filter wants
    }
    o.ulp = o.sp[1] > 0.0f;
    if (o.ulp) {   // 20 kHz x 2^(-10 v): open at the first step, ~20 Hz at the top
      float fc = fminf(20000.0f * exp2f(-10.0f * o.sp[1] * (1.0f / 127.0f)), ny);
      o.lp.set(tanf(kPi * fc / sr_), 1.41421356f);
    }
    // the shelves, A. Simper's forms as the INSERT EQ's (A = 10^(dB / 40), g scaled by sqrt(A)), fixed corners
    o.uls = o.sp[2] != 0.0f;
    o.uhs = o.sp[3] != 0.0f;
    float dbl = o.sp[2] * (o.sp[2] > 0.0f ? 15.0f / 63.0f : 15.0f / 64.0f);
    float dbh = o.sp[3] * (o.sp[3] > 0.0f ? 15.0f / 63.0f : 15.0f / 64.0f);
    o.al = powf(10.0f, dbl / 40.0f), o.ah = powf(10.0f, dbh / 40.0f);
    if (o.uls) o.ls.set(tanf(kPi * 250.0f / sr_) / sqrtf(o.al), 1.41421356f);
    if (o.uhs) o.hs.set(tanf(kPi * 4000.0f / sr_) * sqrtf(o.ah), 1.41421356f);
  }

  void EqRun(const Peq& o, int ch, const float* x, float* y, int m) {
    PeqCh& d = const_cast<Peq&>(o).d[ch];
    const bool uhp = o.uhp, ulp = o.ulp, uls = o.uls, uhs = o.uhs;
    const float hk = o.hp.k;
    const float sl1 = o.ls.k * (o.al - 1.0f), sl2 = o.al * o.al - 1.0f;
    const float sh0 = o.ah * o.ah, sh1 = o.hs.k * (1.0f - o.ah) * o.ah, sh2 = 1.0f - o.ah * o.ah;
    float a1 = d.hp1, a2 = d.hp2, b1 = d.lp1, b2 = d.lp2, c1 = d.ls1, c2 = d.ls2, e1 = d.hs1, e2 = d.hs2;
    for (int i = 0; i < m; ++i) {
      float v = x[i], bp, lp;
      if (uhp) svf(o.hp, v, a1, a2, bp, lp), v = v - hk * bp - lp;
      if (ulp) svf(o.lp, v, b1, b2, bp, lp), v = lp;
      if (uls) svf(o.ls, v, c1, c2, bp, lp), v = v + sl1 * bp + sl2 * lp;
      if (uhs) svf(o.hs, v, e1, e2, bp, lp), v = sh0 * v + sh1 * bp + sh2 * lp;
      y[i] = v;
    }
    d.hp1 = flush(a1), d.hp2 = flush(a2), d.lp1 = flush(b1), d.lp2 = flush(b2);
    d.ls1 = flush(c1), d.ls2 = flush(c2), d.hs1 = flush(e1), d.hs2 = flush(e2);
  }

  void Coefs(Slot& o) {
    const float* p = o.sp;
    const float ny = 0.45f * sr_;
    switch (o.cur) {
      case kFilter: {
        float fc = fminf(20.0f * exp2f(10.0f * p[0]), ny);
        o.f.set(tanf(kPi * fc / sr_), 2.0f - 1.95f * p[1]);
        float d = exp2f(4.0f * p[3]);   // 0..+24 dB
        o.c[0] = d, o.c[1] = 1.0f / sqrtf(d);
        break;
      }
      case kCrush: {
        o.c[0] = exp2f(-6.0f * p[0]);                    // 1 / the hold
        float bits = 16.0f - 14.0f * p[1];
        o.c[1] = exp2f(1.0f - bits), o.c[2] = 1.0f / o.c[1];
        float d = exp2f(3.0f * p[2]);                     // 0..+18 dB
        o.c[3] = d, o.c[4] = 0.25f / sat(0.25f * d);      // the level kept for a -12 dBFS input
        break;
      }
      case kDrive: {
        float d = exp2f(5.0f * p[0]);   // 0..+30 dB
        o.c[0] = d, o.c[1] = 0.25f / sat(0.25f * d);
        float fc = 18000.0f * exp2f(-4.17f * p[2]);   // 18 kHz .. 1 kHz
        o.c[2] = 1.0f - expf(-2.0f * kPi * fminf(fc, ny) / sr_);
        break;
      }
      case kEq: {
        // A = 10^(dB / 40), +-15 dB; the shelves' g scaled by sqrt(A) (Simper's shelf forms), Q 0.707 / 0.9
        for (int j = 0; j < 3; ++j) {
          float v = p[j] * 127.0f - 64.0f;   // 64 = 0 dB, 0 = -15, 127 = +15
          float db = v * (v > 0.0f ? 15.0f / 63.0f : 15.0f / 64.0f);
          o.c[j] = powf(10.0f, db / 40.0f);
        }
        const float fl = 250.0f, fh = 4000.0f, fm = fminf(200.0f * exp2f(5.32f * p[3]), ny);
        float al = o.c[0], am = o.c[1], ah = o.c[2];
        o.eq[0].set(tanf(kPi * fl / sr_) / sqrtf(al), 1.41421356f);
        o.eq[1].set(tanf(kPi * fm / sr_), 1.0f / (0.9f * am));
        o.eq[2].set(tanf(kPi * fh / sr_) * sqrtf(ah), 1.41421356f);
        break;
      }
      case kComp: {
        o.c[0] = -40.0f * p[0];                       // threshold dB
        o.c[1] = 1.0f - 1.0f / (1.0f + 19.0f * p[1] * p[1]);   // 1 - 1/ratio
        float att = 0.0003f + 0.03f * p[2] * p[2], rel = 0.04f + 0.6f * p[2];
        o.c[2] = 1.0f - expf(-1.0f / (att * sr_)), o.c[3] = 1.0f - expf(-1.0f / (rel * sr_));
        o.c[4] = 24.0f * p[3];                        // makeup dB
        break;
      }
      case kDelay: {
        o.c[0] = fminf((0.001f * powf(80.0f, p[0])) * sr_, (float)(kLine - 4) - 0.004f * sr_);   // samples
        o.c[1] = 0.92f * p[1];
        o.c[2] = 0.004f * sr_ * p[2] * p[2];   // MOD depth, samples
        o.c[3] = 1.0f - expf(-1.0f / (0.05f * sr_));   // the time's glide: 50 ms
        break;
      }
      case kTrem: {
        o.c[0] = 0.1f * powf(200.0f, p[0]) / sr_;   // cycles a sample
        o.c[1] = p[1];
        o.c[2] = 1.0f + 30.0f * p[2] * p[2];      // the square's drive
        o.c[3] = 1.0f / sat(o.c[2]);
        o.c[4] = 0.5f * p[3];                     // R's phase offset
        break;
      }
      case kRing: {
        o.c[0] = 10.0f * powf(500.0f, p[0]) / sr_;
        o.c[1] = 1.0f + 30.0f * p[1] * p[1];
        o.c[2] = 1.0f / sat(o.c[1]);
        o.c[3] = p[2];                            // the sweep's depth, octaves
        break;
      }
      default: break;
    }
  }

  // the effect's output for channel ch of slot o (wet only; MIX inside where the type has one). bus: -1 a voice
  // (mono), 0 / 1 a bus's L / R
  void Effect(Slot& o, int ch, const float* x, float* y, int m, int bus) {
    Dsp& d = o.d[ch];
    const float* p = o.sp;
    const float* c = o.c;
    switch (o.cur) {
      case kFilter: {
        // LP -> BP -> HP: the band normalised by k (unity at the peak)
        const float mo = p[2];
        const float wl = mo < 0.5f ? 1.0f - 2.0f * mo : 0.0f, wh = mo > 0.5f ? 2.0f * mo - 1.0f : 0.0f;
        const float wb = (1.0f - wl - wh) * o.f.k;
        float s1 = d.s1, s2 = d.s2;
        const bool drv = c[0] > 1.0001f;   // DRV 0: the filter alone (no saturation)
        for (int i = 0; i < m; ++i) {
          float in = drv ? sat(x[i] * c[0]) * c[1] : x[i], bp, lp;
          svf(o.f, in, s1, s2, bp, lp);
          float hp = in - o.f.k * bp - lp;
          y[i] = wl * lp + wb * bp + wh * hp;
        }
        d.s1 = flush(s1), d.s2 = flush(s2);
        break;
      }
      case kCrush: {
        float ph = d.ph, h = d.hold;
        const float inc = c[0], st = c[1], ist = c[2], dr = c[3], comp = c[4], mix = p[3], dry = 1.0f - mix;
        for (int i = 0; i < m; ++i) {
          ph += inc;
          if (ph >= 1.0f) ph -= 1.0f, h = st * floorf(sat(x[i] * dr) * ist + 0.5f) * comp;
          y[i] = dry * x[i] + mix * (h);
        }
        d.ph = ph, d.hold = h;
        break;
      }
      case kDrive: {
        const float dr = c[0], comp = c[1], a = c[2], mix = p[3], dry = 1.0f - mix, sh = p[1];
        float lp = d.lp;
        for (int i = 0; i < m; ++i) {
          float u = x[i] * dr;
          float soft = sat(u);
          float fold = sin2pi(0.125f * u);   // sin(pi u / 4): +-1 at |u| = 2, folding beyond
          float z = (soft + sh * (fold - soft)) * comp;
          lp += a * (z - lp);
          y[i] = dry * x[i] + mix * (lp);
        }
        d.lp = flush(lp);
        break;
      }
      case kEq: {
        // Simper: low shelf x + k (A - 1) bp + (A^2 - 1) lp; bell x + k (A^2 - 1) bp; high shelf A^2 x + k (1 - A) A bp
        // + (1 - A^2) lp
        const float al = c[0], am = c[1], ah = c[2];
        const float l1 = o.eq[0].k * (al - 1.0f), l2 = al * al - 1.0f, b1 = o.eq[1].k * (am * am - 1.0f);
        const float h0 = ah * ah, h1 = o.eq[2].k * (1.0f - ah) * ah, h2 = 1.0f - ah * ah;
        float s1 = d.s1, s2 = d.s2, m1 = d.m1, m2 = d.m2, q1 = d.h1, q2 = d.h2;
        for (int i = 0; i < m; ++i) {
          float bp, lp, v = x[i];
          svf(o.eq[0], v, s1, s2, bp, lp);
          v = v + l1 * bp + l2 * lp;
          svf(o.eq[1], v, m1, m2, bp, lp);
          v = v + b1 * bp;
          svf(o.eq[2], v, q1, q2, bp, lp);
          y[i] = h0 * v + h1 * bp + h2 * lp;
        }
        d.s1 = flush(s1), d.s2 = flush(s2), d.m1 = flush(m1), d.m2 = flush(m2), d.h1 = flush(q1), d.h2 = flush(q2);
        break;
      }
      case kComp: {
        const float thr = c[0], slope = c[1], att = c[2], rel = c[3], mk = c[4];
        float env = d.env;
        for (int i = 0; i < m; ++i) {
          float a = fabsf(x[i]);
          env += (a > env ? att : rel) * (a - env);
          float lv = 6.0206f * fast_log2(env + 1e-9f);   // dB
          float over = lv - thr, gr;
          if (over <= -3.0f) gr = 0.0f;
          else if (over < 3.0f) gr = slope * (over + 3.0f) * (over + 3.0f) * (1.0f / 12.0f);   // the 6 dB knee
          else gr = slope * over;
          y[i] = x[i] * fast_exp2((mk - gr) * (1.0f / 6.0206f));
        }
        d.env = flush(env);
        break;
      }
      case kDelay: {
        if (!d.line) {
          for (int i = 0; i < m; ++i) y[i] = x[i];
          break;
        }
        const float tgt = c[0], fb = c[1], dep = c[2], g = c[3], mix = p[3], dry = 1.0f - mix;
        const float rate = 0.7f / sr_;
        float t = d.dly > 0.0f ? d.dly : tgt, ph = d.lfo + (bus == 1 ? 0.25f : 0.0f);
        uint32_t w = d.w;
        float* L = d.line;
        // the input enters the line faded in with the wet share: its first sample would else come out after TIME as a
        // hard onset (the fade-in long over by then)
        float fd = o.feed0;
        const float dfd = (o.feed1 - o.feed0) / (float)m;
        for (int i = 0; i < m; ++i) {
          fd += dfd;
          t += g * (tgt - t);
          float dd = t + dep * (0.5f + 0.5f * sin2pi(ph)) + 1.0f;
          ph += rate;
          dd = clampf(dd, 1.0f, (float)(kLine - 2));
          float fi = floorf(dd), fr = dd - fi;
          uint32_t j = (uint32_t)fi;
          float a = L[(w - j) & (kLine - 1)], bq = L[(w - j - 1) & (kLine - 1)];
          float rd = a + (bq - a) * fr;
          L[w & (kLine - 1)] = flush(fd * x[i] + fb * sat(rd));
          ++w;
          y[i] = dry * x[i] + mix * (rd);
        }
        ph -= (bus == 1 ? 0.25f : 0.0f);
        d.lfo = ph - floorf(ph), d.dly = t, d.w = w;
        break;
      }
      case kTrem: {
        // gain 1 - DPTH (1 - w) / 2, w the wave -1..1; a bus's R at PAN's phase offset (auto-pan); a voice's mono chain
        // the two sides summed: (w(ph) + w(ph + off)) / 2
        const float inc = c[0], dep = c[1], dr = c[2], nrm = c[3], off = c[4];
        float ph = d.lfo;
        for (int i = 0; i < m; ++i) {
          float w = bus == 1 ? sat(sin2pi(ph + off) * dr) * nrm : sat(sin2pi(ph) * dr) * nrm;
          if (bus < 0 && off > 0.0f) w = 0.5f * (w + sat(sin2pi(ph + off) * dr) * nrm);
          ph += inc;
          y[i] = x[i] * (1.0f - dep * (0.5f - 0.5f * w));
        }
        d.lfo = ph - floorf(ph);
        break;
      }
      case kRing: {
        const float f0 = c[0], dr = c[1], nrm = c[2], dep = c[3], mix = p[3], dry = 1.0f - mix;
        float car = d.car + (bus == 1 ? 0.25f : 0.0f), ph = d.lfo;
        // the sweep once a chunk (0.3 Hz: far below the chunk rate)
        const float f = f0 * fast_exp2(dep * sin2pi(ph));
        ph += 0.3f * (float)m / sr_;
        for (int i = 0; i < m; ++i) {
          float s = sin2pi(car);
          car += f;
          if (car >= 1.0f) car -= 1.0f;
          float w = sat(s * dr) * nrm;
          y[i] = dry * x[i] + mix * (x[i] * w);
        }
        car -= (bus == 1 ? 0.25f : 0.0f);
        d.car = car - floorf(car), d.lfo = ph - floorf(ph);
        break;
      }
      default:
        for (int i = 0; i < m; ++i) y[i] = x[i];
        break;
    }
  }

  Slot slot_[kChains][kSlots];
  Peq peq_[kChains];        // ins1112: each chain's POST EQ
  bool st_[kChains] = {};   // a voice chain ran in stereo last
  float sr_;
  uint32_t faults_ = 0;
};

// ---- the control side (insert.cpp): the values, names, the console, the card. Everything but the DSP hooks runs on
// the control side (the Pi's core 3, where engine_sysex runs), between blocks.
struct Labels {
  const char* name;            // "FILTER" (<= 7 chars)
  const char* label[kParams];  // "CUT" "RES" "MODE" "DRV" (<= 4 chars)
  uint8_t def[kParams];        // the values a slot takes when this TYPE is chosen
};
const Labels& labels(int type);

// ---- ins1112, the POST EQ: the stage after both slots of a chain (docs/INSERT-FX.md "POST EQ"). On buses 1 and 2 -
// which are send FX 1 and 2 - it is the send FX's return filter, on the slot's wet return, before the TYPE fade, the
// PRE / POST crossfade and the limits, where stock's DELAY and REVERB put theirs.
// Knobs 0..3 = HPF LPF LOW HIGH, each 0..127 as the UI reads it; eq_labels() names them, eq_value_text() prints them.
// kEqFlat(k) is the knob that does nothing (0, 0, 64, 64): all four flat = the stage is skipped, bit for bit.
const Labels& eq_labels();
void set_eq(int c, int k, int v);
int eq_param(int c, int k);     // the knob as set
int eq_playing(int c, int k);   // the knob as it plays (the LFO's offset on top)
void eq_lfo_offset(int c, int k, int off);   // -127..127, on the knob (core/lfo.cpp targets I1..I8 / B1..B3, params 8..11)
bool eq_on(int c);              // chain c's post EQ is off its flat defaults
bool eq_any_on();
void eq_value_text(int k, int v, char* b, int cap);   // "OFF" "420" "6.2K" / "FLAT" "+3.0DB" (<= 7 chars)              // kOff's: "OFF", empty labels
const char* type_name(int type);             // "OFF" .. "RINGMOD"; "?" out of range
int type_from_name(const char* s);           // any case, or a number 0..kTypes-1; -1 neither
const char* chain_name(int c);               // "V1".."V8", "B1".."B3"
int chain_from_name(const char* s);          // -1 none
// The value as the Rytm / the display shows it ("1.6K", "LP>BP", "+3DB", "22MS", "1.4HZ", else the number), <= 7 chars
void value_text(int type, int k, int v, char* b, int cap);
// The same for chain c's slot s at v: AMP's MODL shows the slot's capture or why it does not play (amp::knob_text)
void slot_value_text(int c, int s, int k, int v, char* b, int cap);

// The settings (the knobs): what the remote page, the touchscreen, the console and the card set. set_type takes the
// type's defaults into the params unless keep. Each recomputes what plays (control.play) and marks the card file.
void set_type(int c, int s, int type, bool keep = false);
void set_param(int c, int s, int k, int v);
void set_send(int vi, int bus, int v);       // voice vi -> bus 0..2
void set_route3(int post);                   // bus 3: 0 PRE, 1 POST
int param(int c, int s, int k);              // the knob (0..127)
// seq-ux p-locks (core/seq/seq.cpp): a step's lock of voice chain vi's slot s param k in place of the knob (v 0..127),
// -1 the knob again
void lock(int vi, int s, int k, int v);
int locked(int c, int s, int k);             // the lock's value, -1 none
// The Sidecar's LFOs (core/lfo.cpp, targets I1..I8 B1..B3): an offset (-127..127) on top of the knob / lock
void lfo_offset(int c, int s, int k, int off);
bool any_on();                               // a chain has a TYPE
void touch();                                // the settings changed (the card file written a second later): OUT's

// ---- the DSP hooks
// render_voices (voices.cpp), after voice vi's own sound, before the CC 120 ramp: rows row[0..n-1] of ring column vi
// through the voice's chain (int16 in, float inside, the poly voices' soft limiter at the end); nothing when the
// chain is off. ring_r: the voice renders a stereo pair (OUT PRE / POST): its R in ring_r's column, the chain in
// stereo. The render core that renders vi.
void voice_out(int vi, int16_t (*ring)[kVoices], const int* row, int n, int16_t (*ring_r)[kVoices] = nullptr);
// The voices' sends into the buses (the Pi's OUT fill, core 0: the voices as rendered, after their chains): out[i][b]
// = sum over v of V[i][v] x send[v][b]^2 (ramped per block), full scale 1.0
void voice_sends(const int16_t (*v)[kVoices], int n, float (*out)[kBuses]);
bool sends_on();                             // a voice sends to a bus (else voice_sends writes zeros)
// Bus 3 (the send FX's core, after SendFx::Process): its send mix in[n] (mono) through its chain onto pre / post (the
// pair route3 names; soft-limited as the send FX's pairs). bus3_on: anything to do (a send and a TYPE, or its tail).
void bus3(const float* in, int n, float (*pre)[2], float (*post)[2]);
bool bus3_on();
Engine& engine();
// The DELAY lines: on CORE_HEAP_BUFFERS ports placed by heap_buffers_place (voices.cpp), else a static array.
unsigned long heap_bytes();
void heap_place(void* mem);

// ---- the card: per kit, as the LFOs (core/lfo.h): /.rytm/ins/<project>/kNNN.txt, a kit without its own file starts
// from /.rytm/ins/<project>.txt (also what is used while the Rytm has not named a kit); written a second after the
// last change (save(), engine.cpp idle_save)
void load();
bool save();
void project_changed();
void kit_changed(int kit);
int kit();                                   // the kit the settings are of (0..127), -1 unknown
#if CORE_MSAMP
// 2026-10-26, the RAM store (core/store/store.h), as lfo::want_files: the project's file and every kit's (128) of the
// project now named; a project load fills the store before load() runs, load() reads it only (a kit change while it
// fills is only noted), and save() puts its text there too while the store is its project's
void want_files();
void rekey(const char* name);   // as lfo::rekey: the settings in memory are name's from now on (a rename, the boot)
#endif
// insert.cpp's side for the card half: the edits since the last save (since: the last one's ms), saved, due now; every
// chain off, no sends, no locks; card_forget: the card half's kit forgotten (tests)
bool settings_dirty(uint32_t* since);
void settings_clean();
void settings_due();
void set_kit(int kit);
void clear();
void card_forget();
// The console: "ins" (the state), "ins=C:S:KEY:VALUE" (C V1..V8 B1..B3, S A / B, KEY type / p1..p4 / off),
// "ins=C:eq:hpf|lpf|low|high:V" and "ins=C:eq:off" (ins1112, the POST EQ), "ins=V:send:B:LEVEL" (B 1..3),
// "ins=b3:route:pre|post", "ins=reset"; the answer in out. False: not an ins command.
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);                // "\"ins\":{...}" (docs/INSERT-FX.md)
struct Stats { uint32_t sets, locks, lfo_sets, saves, loads, voice_blocks, bus_blocks; };
const Stats& stats();
void reset();                                // tests: every chain off, sends 0, the engine cleared

}  // namespace ins
}  // namespace core
