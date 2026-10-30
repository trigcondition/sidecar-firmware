// DSP SAMPLE's playback modes on a sample held whole in RAM (docs/psram.md,
// PLAN item 5a), shared by the simulator (sim/samplemodes.py through
// libsampler) and the Teensy (libraries/Sampler, teensy/setup_sampler_lib.sh),
// where the sample sits in PSRAM. Random access is what the streamed voice
// (core/engines/stream_voice.h) cannot give: slices, grains, a stretch,
// a wavetable scan.
//
// The sample: int16 at its file's rate, frames + 1 readable frames (the
// last frame repeated: interpolation reads k + 1 without a check). int16
// because PSRAM is read through a 32-byte cache line per miss: float would
// halve the frames a line holds (docs/psram.md, bandwidth). Mono, or (the Pi's
// pool RAM, docs/STEREO.md) stereo interleaved L R: Render reads a stereo
// sample as mono ((L + R) / 2 truncated: the summed load's value), Render with
// a right channel plays it in stereo (ONE LOOP SLICE GRAIN STRCH; PLUCK and
// WAVE play mono on both sides).
//
// The knobs are DSP SAMPLE's SD SAMPLE page, P9..P16 as the report sends
// them (0..127.99), by sample word: 17 TUN 18 FIN 19 SMP 20 MOD (was FLD,
// unused) 21 STA 22 END 23 LOP 24 LEV. MOD in seven zones picks the mode at a
// trig; the others are read every block (p-locks, the LFO):
//
//   ONE   one shot STA..END (END before STA: reversed); LOP >= 1 loops -
//         MOD 0 is today's streamed voice, knob for knob
//   LOOP  STA..END looped until the note-off; LOP < 64 forward, >= 64
//         ping-pong; reversed when END < STA
//   SLICE STA..END cut into LOP's count of equal slices (2 4 6 8 12 16 32
//         64); the trig's note picks one, C4 (60) the first, chromatically
//         up, wrapping; END < STA plays each slice reversed; TUN/FIN pitch
//   GRAIN a cloud: STA the position (the LFO scans it), END the grain size
//         (10 ms..0.5 s), LOP the density (2..256 grains/s); spray a quarter
//         of a grain; up to kGrains at once, Hann windowed; until the note-off
//   STRCH STA..END at LOP's speed (0 frozen, 64 as recorded, 127 twice),
//         the pitch apart (TUN/FIN/note): two Hann grains of 43 ms at 50 %
//         overlap; reversed when END < STA
//   PLUCK Karplus-Strong at the note's pitch, its delay line filled with the
//         sample from STA (a burst of it); END the decay (50 ms..10 s), LOP
//         the brightness
//   WAVE  the sample as a wavetable of single cycles of LOP's length (256
//         512 600 1024 2048 4096 frames); STA scans it (adjacent cycles
//         crossfaded), the note's pitch at C4 = 261.6 Hz; END not used;
//         until the note-off
//
// The pitch everywhere: TUN - 64 semitones, FIN - 64 64ths of one, and the
// trig's note - 60 (except SLICE, where the note picks the slice). LEV / 100
// the level, as the streamed voice's.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

// RYTM_RENDER_CORES > 1 (the Pi engine: several cores render voices at once): the grain budget is per core,
// rytm_render_core() the calling core (as sim/plaits/shim/plaits/user_data.h). Unset: one count, as before.
#ifndef RYTM_RENDER_CORES
#define RYTM_RENDER_CORES 1
#endif  // RYTM_RENDER_CORES
#if RYTM_RENDER_CORES > 1
extern "C" int rytm_render_core();
#endif  // RYTM_RENDER_CORES > 1

namespace rytm {

// The simulator's build counts reads and runs every sample read through a
// model of the Teensy's data cache (sampler_c.cc, RYTM_SAMPLE_OPS); the
// Teensy's build has neither.
#ifdef RYTM_SAMPLE_OPS
void SampleOpsTouch(const void* p);
#define RYTM_OPS(x) x
#else
#define RYTM_OPS(x)
#endif

// How a DSP SAMPLE trig plays (the Teensy's rule, docs/psram.md): without
// PSRAM always today's streamed voice, MOD ignored (exactly as before the
// sample RAM); with it, from RAM once the sample is resident; until then ONE
// and LOOP stream (LOOP looping forward) and the random-access modes are
// silent while the sample loads.
enum PlayPath { kPathStream, kPathRam, kPathSilent };
inline int ChoosePath(bool psram, bool resident, int mode) {
  if (!psram) return kPathStream;
  if (resident) return kPathRam;
  return mode == 0 || mode == 1 ? kPathStream : kPathSilent;   // ONE, LOOP
}

class SampleModes {
 public:
  enum Mode { kOneShot, kLoop, kSlice, kGrain, kStretch, kPluck, kWave, kModes };
  static const int kGrains = 8;        // per voice, GRAIN and STRCH
  static const int kGrainBudget = 32;  // all voices together (CPU: docs/psram.md); per core with RYTM_RENDER_CORES
  static const int kPluckLen = 2048;   // the pluck's delay line (floats): >= 23.5 Hz
  static const int kWinBits = 10;      // the Hann table: 1024 points (4 KB, from the heap)

  // Counters for the CPU and PSRAM estimate (docs/psram.md; the simulator's
  // build only): sample values read, grains started.
  struct Ops {
    uint32_t reads, grains;
  };

  // GRAIN's spray: 0..1 of a grain's length either way (0.25 by default; the GRAIN machine's SPRY sets it).
  void SetSpray(float s) { spray_ = 2.0f * (s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s); }

  // Zone of a knob value v (0..127.99) in n equal zones (the ENG knobs' rule).
  static int Zone(float v, int n) {
    int i = (int)v;
    i = i < 0 ? 0 : i > 127 ? 127 : i;
    int z = (i * n) >> 7;
    return z >= n ? n - 1 : z;
  }
  static const char* Name(int mode) {
    static const char* const n[kModes] = {"ONE", "LOOP", "SLICE", "GRAIN", "STRCH", "PLUCK", "WAVE"};
    return mode >= 0 && mode < kModes ? n[mode] : "?";
  }
  static int SliceCount(float lop) {
    static const uint8_t c[8] = {2, 4, 6, 8, 12, 16, 32, 64};
    return c[Zone(lop, 8)];
  }
  static int WaveLen(float lop) {
    static const uint16_t c[6] = {256, 512, 600, 1024, 2048, 4096};
    return c[Zone(lop, 6)];
  }

  void Init(uint32_t seed = 1) {
    win_ = Window();   // builds the table once
    rng_ = seed ? seed : 1;
    for (int g = 0; g < kGrains; ++g) grains_[g].on = false;
    playing_ = false;
    pcm_ = nullptr;
    frames_ = 0;
    memset(&ops, 0, sizeof ops);
  }
  // The pluck's delay line, kPluckLen floats (the Teensy: in PSRAM); none:
  // PLUCK is silent.
  void AttachPluck(float* line) { line_ = line; }

  // The sample (see above); null or < 2 frames: silent. ch 2: interleaved L R.
  void Set(const int16_t* pcm, uint32_t frames, float rate, int ch = 1) {
    ch = ch == 2 ? 2 : 1;
    if (pcm == pcm_ && frames == frames_ && rate == rate_ && ch == ch_) return;
    pcm_ = pcm;
    frames_ = frames;
    ch_ = ch;
    rate_ = rate > 0 ? rate : 48000.0f;
    playing_ = false;
    StopGrains();
  }
  ~SampleModes() { StopGrains(); }
  const int16_t* pcm() const { return pcm_; }
  int channels() const { return ch_; }

  // A trig: note (60 = as recorded), sp the SD SAMPLE page (8 values).
  void Trigger(float note, const float* sp) {
    if (!pcm_ || frames_ < 2) return;
    mode_ = Zone(sp[3], kModes);
    note_ = note;
    gain_ = 1.0f;
    release_ = false;
    StopGrains();
    last_ = -1;
    t_ = 0;
    float lo, hi;
    bool rev;
    Region(sp, &lo, &hi, &rev);
    switch (mode_) {
      case kOneShot:
      case kLoop:
        pos_ = rev ? hi : lo;
        dir_ = rev ? -1 : 1;
        break;
      case kSlice: {
        int n = SliceCount(sp[6]);
        int k = ((int)floorf(note) - 60) % n;
        if (k < 0) k += n;
        double w = (double)(hi - lo) / n;
        s_lo_ = lo + w * k;
        s_hi_ = s_lo_ + w;
        pos_ = rev ? s_hi_ : s_lo_;
        dir_ = rev ? -1 : 1;
        break;
      }
      case kGrain:
        next_ = 0;
        break;
      case kStretch:
        pos_ = rev ? hi : lo;   // the read position, moving at the speed
        dir_ = rev ? -1 : 1;
        next_ = 0;
        break;
      case kPluck:
        if (!line_) {
          playing_ = false;
          return;
        }
        PluckStart(sp);
        break;
      case kWave:
        phase_ = 0.0f;
        break;
    }
    playing_ = true;
  }

  // The gate closed: LOOP, GRAIN, STRCH and WAVE fade out (10 ms); ONE plays
  // out (as the streamed voice), PLUCK rings out.
  void NoteOff() {
    if (mode_ == kLoop || mode_ == kGrain || mode_ == kStretch || mode_ == kWave) release_ = true;
  }

  // n samples, -1..1 (before LEV's level: included). outr (optional): the right
  // channel - a stereo sample's own R in the modes that read it, else a copy
  // of out; null: mono (a stereo sample summed).
  void Render(const float* sp, float* out, int n, float* outr = nullptr) {
    if (!playing_) {
      for (int i = 0; i < n; ++i) out[i] = 0.0f;
      if (outr)
        for (int i = 0; i < n; ++i) outr[i] = 0.0f;
      return;
    }
    float level = sp[7] / 100.0f;
    r_ = outr && ch_ == 2 && mode_ != kPluck && mode_ != kWave ? outr : nullptr;
    switch (mode_) {
      case kOneShot: RenderPlay(sp, sp[6] >= 1.0f, false, out, n); break;
      case kLoop: RenderPlay(sp, true, sp[6] >= 64.0f, out, n); break;
      case kSlice: RenderSlice(sp, out, n); break;
      case kGrain: RenderGrain(sp, out, n); break;
      case kStretch: RenderStretch(sp, out, n); break;
      case kPluck: RenderPluck(sp, out, n); break;
      case kWave: RenderWave(sp, out, n); break;
      default:
        Zero(out, 0, n);
        playing_ = false;
    }
    for (int i = 0; i < n; ++i) {
      if (release_) {
        gain_ -= 1.0f / 480.0f;
        if (gain_ <= 0.0f) {
          gain_ = 0.0f;
          playing_ = false;
        }
      }
      out[i] *= gain_ * level;
      if (r_) r_[i] *= gain_ * level;
    }
    if (outr && !r_)
      for (int i = 0; i < n; ++i) outr[i] = out[i];
    r_ = nullptr;
    if (!playing_) StopGrains();
  }

  // Grains sounding now, all voices (at most kGrainBudget). With RYTM_RENDER_CORES > 1: the calling core's
  // voices, each core with the whole budget (a Pi core has the CPU for it; one shared count raced: a lost
  // decrement could hold it at the budget, GRAIN silent until a reboot).
  static int& ActiveGrains() { return Grains(CoreNow()); }
  // Core c's count. A grain gives its count back to the core that started it (Grain::core), not the caller's:
  // Set/Trigger/Stop run on the control core (between blocks, the render cores idle) and stop grains the
  // render core counted.
  static int& Grains(int c) {
    static int n[RYTM_RENDER_CORES] = {};
    return n[c];
  }

  // Silent now (the voice streams, or plays another machine): its grains
  // go back to the budget.
  void Stop() {
    playing_ = false;
    StopGrains();
  }

  bool playing() const { return playing_; }
  int mode() const { return mode_; }
  Ops ops;

 private:
  // A grain: its source frame ip + fr, moving step frames a sample (the
  // stream voice's frame + fraction), its window phase in 32-bit fixed point
  // (the table's index its top bits) and the samples it has left.
  struct Grain {
    uint32_t ip;
    float fr, step;
    uint32_t wph, wdelta, left;
    bool on;
#if RYTM_RENDER_CORES > 1
    uint8_t core;   // the core that counted it (StartGrain)
#endif  // RYTM_RENDER_CORES > 1
  };
  static int CoreNow() {
#if RYTM_RENDER_CORES > 1
    return rytm_render_core();
#else
    return 0;
#endif  // RYTM_RENDER_CORES > 1
  }
  static int CoreOf(const Grain& x) {
#if RYTM_RENDER_CORES > 1
    return x.core;
#else
    (void)x;
    return 0;
#endif  // RYTM_RENDER_CORES > 1
  }
  // A grain off: its count back to the core that counted it.
  static void EndGrain(Grain& x) {
    x.on = false;
    --Grains(CoreOf(x));
  }
  void StopGrains() {
    for (int g = 0; g < kGrains; ++g)
      if (grains_[g].on) EndGrain(grains_[g]);
  }

  static float* Window() {
    static float* w = nullptr;
    if (!w) {
      w = new float[1 << kWinBits];
      for (int i = 0; i < (1 << kWinBits); ++i) w[i] = 0.5f - 0.5f * cosf(6.2831853f * (i + 0.5f) / (1 << kWinBits));
    }
    return w;
  }
  static float Clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
  float Random01() {   // xorshift32
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ >> 8) * (1.0f / 16777216.0f);
  }

  // STA..END in frames (0..120 as the Rytm's), low to high, and whether END < STA.
  void Region(const float* sp, float* lo, float* hi, bool* rev) const {
    float a = Clamp01(sp[4] / 120.0f) * (frames_ - 1), b = Clamp01(sp[5] / 120.0f) * (frames_ - 1);
    *rev = b < a;
    *lo = *rev ? b : a;
    *hi = *rev ? a : b;
  }
  float Ratio(const float* sp, bool with_note) const {
    float semis = (sp[0] - 64.0f) + (sp[1] - 64.0f) / 64.0f + (with_note ? note_ - 60.0f : 0.0f);
    return powf(2.0f, semis / 12.0f);
  }

  // Frame k as stored, mono (a stereo frame: (L + R) / 2 truncated) (the
  // simulator's build: counted, through the cache model).
  inline float Fetch(uint32_t k) {
    RYTM_OPS(++ops.reads; SampleOpsTouch(pcm_ + k * ch_));
    if (ch_ == 2) return (float)(((int32_t)pcm_[2 * k] + (int32_t)pcm_[2 * k + 1]) / 2);
    return pcm_[k];
  }
  // Frame k's R (a stereo sample; Fetch's L counterpart: FetchL)
  inline float FetchL(uint32_t k) { return pcm_[2 * k]; }
  inline float FetchR(uint32_t k) { return pcm_[2 * k + 1]; }
  // The sample at source frame p (>= 0), linearly interpolated.
  inline float Read(double p) {
    uint32_t k = (uint32_t)p;
    if (k >= frames_) k = frames_ - 1;
    float f = (float)(p - k);
    f = f < 1.0f ? f : 1.0f;
    float s0 = Fetch(k), s1 = Fetch(k + 1);   // pcm_[frames_] is the guard
    return (s0 + (s1 - s0) * f) * (1.0f / 32768.0f);
  }
  // A stereo sample's L into *l and R into *r at p (r_ set: the stereo render)
  inline void Read2(double p, float* l, float* r) {
    uint32_t k = (uint32_t)p;
    if (k >= frames_) k = frames_ - 1;
    float f = (float)(p - k);
    f = f < 1.0f ? f : 1.0f;
    float a0 = FetchL(k), a1 = FetchL(k + 1), b0 = FetchR(k), b1 = FetchR(k + 1);
    *l = (a0 + (a1 - a0) * f) * (1.0f / 32768.0f);
    *r = (b0 + (b1 - b0) * f) * (1.0f / 32768.0f);
  }

  // ONE and LOOP: STA..END forward or reversed, looping (wrapping, or
  // ping-pong) or once.
  void RenderPlay(const float* sp, bool loop, bool pingpong, float* out, int n) {
    float lo, hi;
    bool rev;
    Region(sp, &lo, &hi, &rev);
    double step = rate_ / 48000.0 * Ratio(sp, true);
    int want = rev ? -1 : 1;
    if (!pingpong) dir_ = want;
    for (int i = 0; i < n; ++i) {
      if (pos_ < lo || pos_ > hi) {
        if (!loop || hi - lo < 1.0f) {
          playing_ = false;
          Zero(out, i, n);
          return;
        }
        if (pingpong) {
          if (pos_ > hi) pos_ = 2.0 * hi - pos_, dir_ = -1;
          else pos_ = 2.0 * lo - pos_, dir_ = 1;
          if (pos_ < lo || pos_ > hi) pos_ = dir_ > 0 ? lo : hi;
        } else {   // wrap (a step longer than the loop too)
          double len = (double)hi - lo;
          pos_ = lo + fmod(pos_ - lo, len);
          if (pos_ < lo) pos_ += len;
        }
      }
      if (r_) Read2(pos_, &out[i], &r_[i]);
      else out[i] = Read(pos_);
      pos_ += dir_ * step;
    }
  }
  // out (and the right channel when stereo) silent from i on
  void Zero(float* out, int i, int n) {
    for (int j = i; j < n; ++j) out[j] = 0.0f;
    if (r_)
      for (int j = i; j < n; ++j) r_[j] = 0.0f;
  }

  // SLICE: the trig's slice once, a 2 ms fade at either end against clicks.
  void RenderSlice(const float* sp, float* out, int n) {
    double step = rate_ / 48000.0 * Ratio(sp, false);
    const float kFade = 96.0f;   // output samples
    for (int i = 0; i < n; ++i) {
      if (pos_ < s_lo_ || pos_ > s_hi_) {
        playing_ = false;
        Zero(out, i, n);
        return;
      }
      double left = dir_ > 0 ? s_hi_ - pos_ : pos_ - s_lo_;   // source frames to the end
      float g = (float)(left / step) / kFade;
      float gi = (float)t_ / kFade;
      g = g < 1.0f ? g : 1.0f;
      g = gi < g ? gi : g;
      if (r_) {
        Read2(pos_, &out[i], &r_[i]);
        out[i] *= g, r_[i] *= g;
      } else {
        out[i] = Read(pos_) * g;
      }
      pos_ += dir_ * step;
      ++t_;
    }
  }

  // Start a grain at source frame `at` (clamped so the grain stays inside),
  // unless all of this voice's or the budget's grains are busy.
  void StartGrain(double at, double step, float size_out) {
    if (ActiveGrains() >= kGrainBudget || !win_) return;
    for (int g = 0; g < kGrains; ++g) {
      Grain& x = grains_[g];
      if (x.on) continue;
      double span = step * size_out;
      double maxp = (double)frames_ - 1 - span;
      if (maxp < 0) maxp = 0;
      double p = at < 0 ? 0 : at > maxp ? maxp : at;
      x.ip = (uint32_t)p;
      x.fr = (float)(p - x.ip);
      x.step = (float)step;
      x.wph = 0;
      x.left = size_out < 2.0f ? 2 : (uint32_t)size_out;
      x.wdelta = (uint32_t)(4294967296.0 / x.left);
      x.on = true;
#if RYTM_RENDER_CORES > 1
      x.core = (uint8_t)CoreNow();
#endif  // RYTM_RENDER_CORES > 1
      ++ActiveGrains();
      RYTM_OPS(++ops.grains);
      return;
    }
  }
  // The grains' sum for one output sample (r: the right channel's, a stereo
  // render; the return value is then L)
  inline float GrainSum(float* r = nullptr) {
    float s = 0.0f, sr = 0.0f;
    for (int g = 0; g < kGrains; ++g) {
      Grain& x = grains_[g];
      if (!x.on) continue;
      uint32_t k = x.ip < frames_ ? x.ip : frames_ - 1;
      float wv = win_[x.wph >> (32 - kWinBits)];
      if (r) {
        float a0 = FetchL(k), a1 = FetchL(k + 1), b0 = FetchR(k), b1 = FetchR(k + 1);
        s += (a0 + (a1 - a0) * x.fr) * wv;
        sr += (b0 + (b1 - b0) * x.fr) * wv;
      } else {
        float s0 = Fetch(k), s1 = Fetch(k + 1);
        s += (s0 + (s1 - s0) * x.fr) * wv;
      }
      x.fr += x.step;
      uint32_t w = (uint32_t)x.fr;
      x.ip += w;
      x.fr -= w;
      x.wph += x.wdelta;
      if (--x.left == 0) EndGrain(x);
    }
    if (r) *r = sr * (1.0f / 32768.0f);
    return s * (1.0f / 32768.0f);
  }

  void RenderGrain(const float* sp, float* out, int n) {
    double step = rate_ / 48000.0 * Ratio(sp, true);
    float size_s = 0.01f * powf(50.0f, Clamp01(sp[5] / 127.0f));   // 10 ms..0.5 s
    float dens = 2.0f * powf(2.0f, 7.0f * Clamp01(sp[6] / 127.0f));   // 2..256 a second
    float size_out = size_s * 48000.0f;
    float centre = Clamp01(sp[4] / 120.0f) * (frames_ - 1);
    double span = step * size_out;   // source frames a grain reads
    float overlap = dens * size_s;   // grains sounding at once, on average
    if (overlap > kGrains) overlap = kGrains;
    float norm = 1.0f / (0.5f * overlap > 1.0f ? 0.5f * overlap : 1.0f);   // Hann's mean is 0.5
    for (int i = 0; i < n; ++i) {
      if (--next_ <= 0) {
        float spray = (Random01() - 0.5f) * spray_ * (float)span;
        StartGrain(centre - span * 0.5 + spray, step, size_out);
        next_ = (int32_t)(48000.0f / dens * (0.75f + 0.5f * Random01()));
        if (next_ < 1) next_ = 1;
      }
      if (r_) {
        out[i] = GrainSum(&r_[i]) * norm;
        r_[i] *= norm;
      } else {
        out[i] = GrainSum() * norm;
      }
    }
  }

  void RenderStretch(const float* sp, float* out, int n) {
    float lo, hi;
    bool rev;
    Region(sp, &lo, &hi, &rev);
    dir_ = rev ? -1 : 1;
    double step = rate_ / 48000.0 * Ratio(sp, true);
    double speed = sp[6] / 64.0 * rate_ / 48000.0;   // source frames per output sample
    const float size_out = 2048.0f, hop = 1024.0f;   // 43 ms grains at 50 % overlap
    for (int i = 0; i < n; ++i) {
      bool done = dir_ > 0 ? pos_ >= hi : pos_ <= lo;
      if (!done && --next_ <= 0) {
        StartGrain(pos_ + Align(pos_, step, size_out), step, size_out);
        last_ = Newest();
        next_ = (int32_t)hop;
      }
      out[i] = GrainSum(r_ ? &r_[i] : nullptr);
      if (!done) {
        pos_ += dir_ * speed;
      } else {
        bool any = false;
        for (int g = 0; g < kGrains; ++g) any |= grains_[g].on;
        if (!any) {
          playing_ = false;
          Zero(out, i + 1, n);
          return;
        }
      }
    }
  }

  // STRCH's grains start where they match the sounding grain's continuation
  // best (WSOLA): the offset from p, within +-kSeek source frames, whose
  // frames (at the grain's step) correlate best with what the newest grain
  // reads next - plain OLA would jump the phase at every hop, which a steady
  // tone hears as a pitch shift. Coarse to fine: every 4th offset on 16
  // points, then +-4 around the best on 32: ~1.8k multiplies a hop (1024
  // samples), under 2 a sample.
  static const int kSeek = 192, kMatch = 32;
  float Match(const float* ref, double s, double step, int npts, int stride) {
    float c = 0.0f;
    for (int j = 0; j < npts; ++j) c += ref[j * stride] * Fetch((uint32_t)(s + j * stride * step));
    return c;
  }
  double Align(double p, double step, float size_out) {
    if (last_ < 0 || !grains_[last_].on) return 0.0;
    const Grain& g = grains_[last_];
    double span = step * size_out, maxp = (double)frames_ - 1 - span - kMatch * step;
    if (maxp < 0) return 0.0;
    float ref[kMatch];
    for (int j = 0; j < kMatch; ++j) {
      double q = g.ip + g.fr + j * step;
      ref[j] = Fetch((uint32_t)(q < frames_ - 1 ? q : frames_ - 1));
    }
    int best_d = 0;
    float best = -1e30f;
    for (int d = -kSeek; d <= kSeek; d += 4) {
      double s = p + d;
      if (s < 0 || s > maxp) continue;
      float c = Match(ref, s, step, kMatch / 2, 2);
      if (c > best) best = c, best_d = d;
    }
    int centre = best_d;
    best = -1e30f;
    for (int d = centre - 4; d <= centre + 4; ++d) {
      double s = p + d;
      if (s < 0 || s > maxp) continue;
      float c = Match(ref, s, step, kMatch, 1);
      if (c > best) best = c, best_d = d;
    }
    return best_d;
  }
  int Newest() const {   // the grain StartGrain just started
    int k = -1;
    uint32_t w = 0xffffffffu;
    for (int g = 0; g < kGrains; ++g)
      if (grains_[g].on && grains_[g].wph < w) w = grains_[g].wph, k = g;
    return k;
  }

  // PLUCK: the delay line D = 48000 / f - k long (k the loop filter's delay),
  // filled with the sample from STA at its own rate.
  void PluckStart(const float* sp) {
    float f = 261.6256f * Ratio(sp, true);
    float k = 0.5f * (1.0f - Clamp01(sp[6] / 127.0f));
    float d = 48000.0f / f - k;
    if (d < 2.0f) d = 2.0f;
    if (d > kPluckLen - 3) d = kPluckLen - 3;
    delay_ = d;
    double p = Clamp01(sp[4] / 120.0f) * (frames_ - 1);
    double step = rate_ / 48000.0;
    int len = (int)d + 2;
    for (int i = 0; i < kPluckLen; ++i) line_[i] = 0.0f;
    for (int i = 0; i < len; ++i) {
      line_[i] = p < frames_ - 1 ? Read(p) : 0.0f;
      p += step;
    }
    w_ = len;   // the burst is the line's past: read back D behind w
    prev_ = 0.0f;
  }
  void RenderPluck(const float* sp, float* out, int n) {
    float k = 0.5f * (1.0f - Clamp01(sp[6] / 127.0f));
    float t60 = 0.05f * powf(200.0f, Clamp01(sp[5] / 127.0f));   // 50 ms..10 s
    float g = powf(0.001f, delay_ / (48000.0f * t60));
    for (int i = 0; i < n; ++i) {
      float r = (float)w_ - delay_;
      if (r < 0) r += kPluckLen;
      int i0 = (int)r;
      if (i0 >= kPluckLen) i0 -= kPluckLen;   // r rounded up to kPluckLen
      float fr = r - (int)r;
      int i1 = i0 + 1 >= kPluckLen ? 0 : i0 + 1;
      float y = line_[i0] + (line_[i1] - line_[i0]) * fr;
      float x = (1.0f - k) * y + k * prev_;
      prev_ = y;
      line_[w_] = x * g;
      if (++w_ >= kPluckLen) w_ = 0;
      out[i] = y;
    }
    t_ += n;
    if (t_ > (uint32_t)(t60 * 48000.0f) + kPluckLen) playing_ = false;
  }

  void RenderWave(const float* sp, float* out, int n) {
    uint32_t w = WaveLen(sp[6]);
    if (w > frames_) w = frames_;
    uint32_t cycles = frames_ / w;
    float pos = Clamp01(sp[4] / 120.0f) * (cycles - 1);
    uint32_t c0 = (uint32_t)pos, c1 = c0 + 1 < cycles ? c0 + 1 : c0;
    float cf = pos - c0;
    float inc = 261.6256f * Ratio(sp, true) / 48000.0f;
    double b0 = (double)c0 * w, b1 = (double)c1 * w;
    for (int i = 0; i < n; ++i) {
      float x = phase_ * w;   // within the cycle; the last point wraps to its first
      uint32_t j = (uint32_t)x;
      float f = x - j;
      uint32_t j1 = j + 1 < w ? j + 1 : 0;
      float a0 = Fetch((uint32_t)b0 + j), a1 = Fetch((uint32_t)b0 + j1);
      float d0 = Fetch((uint32_t)b1 + j), d1 = Fetch((uint32_t)b1 + j1);
      float a = a0 + (a1 - a0) * f, d = d0 + (d1 - d0) * f;
      out[i] = (a + (d - a) * cf) * (1.0f / 32768.0f);
      phase_ += inc;
      if (phase_ >= 1.0f) phase_ -= (float)(int)phase_;
    }
  }

  const int16_t* pcm_ = nullptr;
  uint32_t frames_ = 0;
  int ch_ = 1;               // 2: pcm_ interleaved L R
  float* r_ = nullptr;       // Render's right channel while it renders a stereo sample in stereo
  float rate_ = 48000.0f;
  int mode_ = kOneShot;
  float note_ = 60.0f;
  bool playing_ = false, release_ = false;
  float gain_ = 1.0f;
  double pos_ = 0.0;   // ONE LOOP SLICE STRCH: the source frame
  int dir_ = 1;
  double s_lo_ = 0, s_hi_ = 0;   // SLICE: the trig's slice
  uint32_t t_ = 0;               // samples since the trig (SLICE, PLUCK)
  Grain grains_[kGrains];
  int32_t next_ = 0;   // GRAIN, STRCH: samples to the next grain
  float spray_ = 0.5f;   // GRAIN: the start's spread in grain lengths (a quarter either way; the GRAIN machine's SPRY)
  int last_ = -1;      // STRCH: the newest grain
  uint32_t rng_ = 1;
  const float* win_ = nullptr;   // the Hann table
  float* line_ = nullptr;   // PLUCK
  float delay_ = 100.0f, prev_ = 0.0f;
  int w_ = 0;
  float phase_ = 0.0f;   // WAVE
};

}  // namespace rytm
