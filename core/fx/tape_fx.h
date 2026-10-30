// core/fx/tape_fx.h - SPACE (send FX TYPE 5): an RE-201-style multi-head tape echo with a small spring reverb. Built
// where RYTM_SENDFX_LARGE is 1 (the Pi and the Mac; 0.5 MB of tape a slot), reserved and MISSING on the Teensy.
// Included by send_fx.h after its helpers (Smooth, knob01, flush, soft_limit, fast_sin, FX_DSP) and stutter_fx.h
// (ClockWord): not on its own.
//
// Written for the Sidecar (no code taken from anywhere; Airwindows TapeDelay2 and Dub Station (MIT) and ChowDSP's tape
// model (GPL-3) were read for ideas only). The model:
//   - one mono tape loop (2^17 floats: 2.73 s at 48 kHz), written once a sample, read by three playback heads at
//     1 : 2 : 3 of the shortest head's delay (the RE-201's head spacing, roughly). TIME is the tape speed: all heads
//     scale together, and a change glides (~100 ms, one-pole), so the echoes already on tape bend in pitch like a real
//     motor change, never jump. Reads are 4-point cubic (Catmull-Rom), the delay ramped per sample: no zipper;
//   - the record path: input + REPEAT x (the selected heads' sum / their count) -> a 45 Hz high-pass (no DC, no
//     sub-bass runaway) -> the AGE low-pass (10 kHz new .. 2 kHz worn: the loss per pass) -> the tape's saturation
//     (soft_limit's rational curve toward 0.7, unity for small signals). REPEAT is percent: 100 is the edge in every
//     MODE (the feedback is normalised by the head count), above it the loop self-oscillates and the saturation holds
//     the runaway at the tape's ceiling (the dub throw). Above 100 % a faint hiss (-80 dB x (REPEAT - 100 %)) goes
//     onto the tape, as on a real machine: a runaway that a TIME sweep pitched out of the loop's band builds again
//     from it (without, nothing is left to oscillate); at or below 100 % there is none (silence in, silence out).
//     A runaway in a multi-head MODE fades while TIME is swept (each head is pitched by its own delay's change, so
//     the heads stop adding up: as on the real machine) and builds again once TIME rests; H1 howls through a sweep;
//   - wow (0.55 Hz sine + a slow random drift, pitch up to +-0.6 %) and flutter (6.4 Hz, up to +-0.2 %) move every
//     head's read position, the later heads a little more (0.7 / 0.85 / 1 of the wow); depth in samples is set from
//     the pitch deviation, so it is the same at every TIME. Computed once a block, ramped per sample;
//   - the spring (MODE H23+S, H123S): per side a 36 / 41 ms loop with 12 first-order allpasses (the dispersion: each
//     round trip chirps), a 4.5 kHz damping low-pass, feedback 0.8 (~1.2 s); fed from the input and the echoes,
//     band-passed 120 Hz .. 5 kHz. It runs only while a spring mode plays (and is cleared when it leaves);
//   - the return is wet only and wide: H1 centre, H2 left of it, H3 right, the spring L / R from its two loops.
// Tempo sync: TIME 64..127 locks the shortest head to a note value of the MIDI clock (Control::clock, 24 ppqn: the
// tempo from the last up to 24 ticks; no clock yet: 120 BPM; the clock stopped: the last tempo). A delay longer than
// the tape allows is clamped (H3 at most 2.72 s: 1/8D below ~33 BPM).
//
// Knobs P2..P5 (0..127, all p-lockable; P1 is the send's ROUTE, not used here):
//   P2 TIME   0..63: free, the shortest head 40 .. 400 ms (exponential; H3 up to 1.2 s); 64..127: SYNC, the shortest
//             head = 1/16, 1/8T, 1/8, 1/8D (16 values each; the longer heads give 2x and 3x of it)
//   P3 RPT    REPEAT (feedback) in percent: 0 one echo a head .. 100 the edge .. 127 runaway (bounded)
//   P4 MODE   8 modes, stepped: H1, H2, H3, H1+2, H2+3, H123, H23+S, H123S (+S: with the spring)
//   P5 AGE    the tape's wear: 0 new (bright, steady) .. 127 worn (dark: the loop low-pass 10 kHz -> 2 kHz; wow and
//             flutter up to their full depth, wow ~ AGE^2)
#pragma once

namespace core {
namespace fx {

// TIME: the number up to 63, the note values from 64 (a continuous knob); MODE: a pure selector (stepped)
inline constexpr Zone kTapeZones[] = {
    {0, 64, 0, 0, 0, "1/16"}, {0, 80, 0, 0, 0, "1/8T"}, {0, 96, 0, 0, 0, "1/8"},  {0, 112, 0, 0, 0, "1/8D"},
    {2, 0, 0, 0, 1, "H1"},    {2, 16, 0, 0, 1, "H2"},   {2, 32, 0, 0, 1, "H3"},   {2, 48, 0, 0, 1, "H1+2"},
    {2, 64, 0, 0, 1, "H2+3"}, {2, 80, 0, 0, 1, "H123"}, {2, 96, 0, 0, 1, "H23+S"}, {2, 112, 0, 0, 1, "H123S"}};
inline constexpr KnobLabels kTapeLabels = {{"TIME", "RPT", "MODE", "AGE"}, kTapeZones, 12};

class TapeFx {
 public:
  static constexpr int kTape = 1 << 17;   // 2.73 s at 48 kHz, 512 KB
  static constexpr int kSpringLine = 4096;   // 85 ms at 48 kHz (the loops: 36 / 41 ms, at 96 kHz too)
  static constexpr int kAp = 12;          // the spring's dispersion allpasses a side
  static constexpr float kCeiling = 0.7f;   // the tape's saturation
  static constexpr float kWowDev = 0.006f, kWowHz = 0.55f, kFlutDev = 0.002f, kFlutHz = 6.4f;
  static constexpr uint8_t kModeHeads[8] = {1, 2, 4, 3, 6, 7, 6, 7};   // bit h: head h + 1
  static constexpr uint8_t kSyncTicks[4] = {6, 8, 12, 18};             // 1/16, 1/8T, 1/8, 1/8D at 24 ppqn
  static int Mode(uint8_t v) { return (v > 127 ? 127 : v) >> 4; }
  static bool ModeSpring(int m) { return m >= 6; }
  static int SyncTicks(uint8_t v) {   // 0: free
    v = v > 127 ? 127 : v;
    return v < 64 ? 0 : kSyncTicks[(v - 64) >> 4];
  }
  static float FreeDelay(uint8_t v, float sr) {   // the shortest head, samples: 40 ms * 10^(v / 63)
    v = v > 63 ? 63 : v;
    return 0.040f * sr * powf(10.0f, (float)v / 63.0f);
  }
  static float WowSamples(float sr) { return kWowDev / (6.2831853f * kWowHz) * sr; }   // the wow's full depth
  static float FlutterSamples(float sr) { return kFlutDev / (6.2831853f * kFlutHz) * sr; }

  TapeFx() { Clear(); }
  void Clear() {
    memset(tape_, 0, sizeof tape_);
    ClearSmall();
  }
  // The clear spread over calls: true when done. Each call zeroes at most `bytes` of the tape (then the rest at once).
  bool ClearStep(int bytes) {
    if (clear_at_ < 0) clear_at_ = 0;
    int take = bytes / (int)sizeof(float);
    if (take < 1) take = 1;
    if (take > kTape - clear_at_) take = kTape - clear_at_;
    memset(tape_ + clear_at_, 0, (size_t)take * sizeof(float));
    clear_at_ += take;
    if (clear_at_ < kTape) return false;
    ClearSmall();
    return true;
  }
  // the head h (0..2) read position now (samples behind the input, the wow included); the tempo (frames a tick)
  float HeadDelay(int h) const { return d1_.y * (float)(h + 1) + off_[h]; }
  float FramesPerTick() const { return fpt_; }

  // n frames of the mono send in -> out (added: stereo), k = P2..P5, a = the 10 ms smoothing, clock = Control::clock
  FX_DSP void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float a, uint32_t clock) {
    if (sr != sr_) Setup(sr);
    TakeClock(clock);
    frame_ += (uint32_t)n;
    const int kk = (k[0] & 127) | (k[1] & 127) << 7 | (k[2] & 127) << 14 | (k[3] & 127) << 21;
    if (kk != key_) key_ = kk, Targets(k, sr);
    const int st = SyncTicks(k[0]);
    float t_d1 = st ? (float)st * fpt_ : c_free_;
    t_d1 = fminf(d1_max_, fmaxf(8.0f, t_d1));
    if (fresh_) {   // from a blank tape: every value at its target (not a glide from 0)
      d1_.y = t_d1, g_.y = c_g_, lpc_.y = c_lpc_, wd_.y = c_wd_, fd_.y = c_fd_, sg_.y = c_sg_;
      for (int h = 0; h < 3; ++h) og_[h].y = c_og_[h], wf_[h].y = c_wf_[h];
      fresh_ = false;
    }
    const float inv = 1.0f / (float)n;
    float d1 = d1_.y;
    const float sd1 = d1_.Ramp(t_d1, a * 0.1f, n);   // the tape's speed: ~100 ms
    float g = g_.y, lc = lpc_.y, sp = sg_.y;
    const float sgf = g_.Ramp(c_g_, a, n), slc = lpc_.Ramp(c_lpc_, a, n), ssp = sg_.Ramp(c_sg_, a, n);
    wd_.Ramp(c_wd_, a, n), fd_.Ramp(c_fd_, a, n);
    // wow and flutter: the block-end offsets, each head's offset ramped to it
    ph_w_ += kWowHz * (float)n / sr, ph_f_ += kFlutHz * (float)n / sr;
    ph_w_ -= floorf(ph_w_), ph_f_ -= floorf(ph_f_);
    drift_ += (drift_t_ - drift_) * fminf(1.0f, (float)n * drift_c_);
    if ((int32_t)(frame_ - drift_at_) >= 0) {   // a new drift target every ~0.7 s
      rng_ = rng_ * 1664525u + 1013904223u;
      drift_t_ = (float)(rng_ >> 8) * (2.0f / 16777216.0f) - 1.0f;
      drift_at_ = frame_ + (uint32_t)(0.7f * sr);
    }
    const float mw = 0.75f * fast_sin(ph_w_) + 0.25f * drift_, mf = fast_sin(ph_f_);
    static constexpr float kHeadWow[3] = {0.7f, 0.85f, 1.0f};
    float off[3], soff[3], og[3], sog[3], wf[3], swf[3];
    bool on[3];
    for (int h = 0; h < 3; ++h) {
      off[h] = off_[h];
      const float e = wd_.y * kHeadWow[h] * mw + fd_.y * mf;
      soff[h] = (e - off[h]) * inv, off_[h] = e;
      og[h] = og_[h].y, wf[h] = wf_[h].y;
      sog[h] = og_[h].Ramp(c_og_[h], a, n), swf[h] = wf_[h].Ramp(c_wf_[h], a, n);
      on[h] = og[h] != 0.0f || wf[h] != 0.0f || og_[h].y != 0.0f || wf_[h].y != 0.0f;
    }
    const bool spring = sp != 0.0f || sg_.y != 0.0f;
    if (!spring && spring_on_) ClearSpring();   // left: its tail gone with it (a spring mode starts clean)
    spring_on_ = spring;
    float hx = hp_x_, hy = hp_y_, lp = lp_;
    const float hc = hpc_;
    uint32_t w = w_, hr = rng_;
    const float hiss = c_g_ > 1.0f ? 1e-4f * (c_g_ - 1.0f) * (2.0f / 16777216.0f) : 0.0f;
    for (int i = 0; i < n; ++i) {
      d1 += sd1, g += sgf, lc += slc;
      float r[3] = {0.0f, 0.0f, 0.0f};
      for (int h = 0; h < 3; ++h)
        if (on[h]) {
          off[h] += soff[h], og[h] += sog[h], wf[h] += swf[h];
          r[h] = Read(w, d1 * (float)(h + 1) + off[h]);
        }
      const float fb = wf[0] * r[0] + wf[1] * r[1] + wf[2] * r[2];
      float x = in[i] + g * fb;
      if (hiss != 0.0f) hr = hr * 1664525u + 1013904223u, x += hiss * ((float)(hr >> 8) - 8388608.0f);
      hy = hc * (hy + x - hx), hx = x;   // 45 Hz high-pass
      lp += (hy - lp) * lc;             // AGE's loss per pass
      tape_[w & (kTape - 1)] = flush(soft_limit(lp, kCeiling));
      ++w;
      const float e0 = og[0] * r[0], e1 = og[1] * r[1], e2 = og[2] * r[2];
      float yl = 0.7f * e0 + 0.85f * e1 + 0.5f * e2, yr = 0.7f * e0 + 0.5f * e1 + 0.85f * e2;
      if (spring) {
        sp += ssp;
        float sl, sr2;
        Spring(sp * 0.35f * (in[i] + e0 + e1 + e2), &sl, &sr2);
        yl += sp * 0.6f * sl, yr += sp * 0.6f * sr2;
      }
      out[i][0] += yl;
      out[i][1] += yr;
    }
    w_ = w, rng_ = hr;
    hp_x_ = flush(hx), hp_y_ = flush(hy), lp_ = flush(lp);
    if (spring) FlushSpring();
  }

 private:
  struct Side {
    float line[kSpringLine];
    float ap[kAp];
    float damp, hx, hy, lo;
    int len;
  };
  void ClearSmall() {
    ClearSpring();
    w_ = 0, hp_x_ = hp_y_ = lp_ = 0.0f;
    for (int h = 0; h < 3; ++h) off_[h] = 0.0f;
    ph_w_ = ph_f_ = drift_ = drift_t_ = 0.0f, drift_at_ = 0, rng_ = 12345u;
    have_word_ = false, word_ = 0, nstamp_ = 0, frame_ = 0, last_tick_ = 0;
    fpt_ = 1000.0f * (sr_ > 0.0f ? sr_ : 48000.0f) / 48000.0f;
    key_ = -1, fresh_ = true, clear_at_ = -1, spring_on_ = false;
  }
  void ClearSpring() {
    for (Side& s : side_) {
      memset(s.line, 0, sizeof s.line);
      for (float& v : s.ap) v = 0.0f;
      s.damp = s.hx = s.hy = s.lo = 0.0f;
    }
    sw_ = 0;
  }
  void FlushSpring() {
    for (Side& s : side_) {
      for (float& v : s.ap) v = flush(v);
      s.damp = flush(s.damp), s.hx = flush(s.hx), s.hy = flush(s.hy), s.lo = flush(s.lo);
    }
  }
  void Setup(float sr) {
    sr_ = sr;
    hpc_ = expf(-6.2831853f * 45.0f / sr);
    sp_hpc_ = expf(-6.2831853f * 120.0f / sr);
    sp_in_lp_ = 1.0f - expf(-6.2831853f * 5000.0f / sr);
    sp_damp_ = 1.0f - expf(-6.2831853f * 4500.0f / sr);
    side_[0].len = (int)(0.0361f * sr), side_[1].len = (int)(0.0407f * sr);
    for (Side& s : side_)
      if (s.len > kSpringLine - 2) s.len = kSpringLine - 2;
    d1_max_ = (float)(kTape - 16) / 3.0f - 1.2f * (WowSamples(sr) + FlutterSamples(sr));
    drift_c_ = 1.0f - expf(-1.0f / (0.35f * sr));
    fpt_ = 1000.0f * sr / 48000.0f;
    key_ = -1;
  }
  void Targets(const uint8_t* k, float sr) {
    c_free_ = FreeDelay(k[0], sr);
    c_g_ = (float)(k[1] > 127 ? 127 : k[1]) * 0.01f;
    const int m = Mode(k[2]), mask = kModeHeads[m];
    const int nh = (mask & 1) + (mask >> 1 & 1) + (mask >> 2 & 1);
    for (int h = 0; h < 3; ++h) {
      const bool act = mask >> h & 1;
      c_og_[h] = act ? 1.0f / sqrtf((float)nh) : 0.0f;
      c_wf_[h] = act ? 1.0f / (float)nh : 0.0f;
    }
    c_sg_ = ModeSpring(m) ? 1.0f : 0.0f;
    const float age = knob01(k[3]);
    c_lpc_ = 1.0f - expf(-6.2831853f * 10000.0f * powf(0.2f, age) / sr);
    c_wd_ = age * age * WowSamples(sr), c_fd_ = age * FlutterSamples(sr);
  }
  // the MIDI clock: each new tick stamped at this block's start, the tempo from the last up to 24 ticks
  void TakeClock(uint32_t c) {
    if (!have_word_) word_ = c, have_word_ = true;
    const uint32_t d = (ClockWord::ticks(c) - ClockWord::ticks(word_)) & 0x7Fu;
    word_ = c;
    if (!d) return;
    if (nstamp_ && (int32_t)(frame_ - last_tick_) > (int32_t)(0.3f * sr_)) nstamp_ = 0;   // a gap: a new series
    for (uint32_t j = 0; j < d; ++j) stamp_[nstamp_++ % kStamps] = frame_;
    last_tick_ = frame_;
    const int m = nstamp_ < (uint32_t)kStamps ? (int)nstamp_ : kStamps;
    if (m >= 3) {
      const uint32_t old = stamp_[(nstamp_ - m) % kStamps], neu = stamp_[(nstamp_ - 1) % kStamps];
      const float f = (float)(neu - old) / (float)(m - 1);
      if (f > 50.0f) fpt_ = f;   // above ~1200 BPM: a burst, not a clock
    }
  }
  float Read(uint32_t w, float d) const {   // d samples behind the next write; 4-point cubic
    d = fminf((float)(kTape - 4), fmaxf(2.0f, d));
    const float fi = floorf(d), f = d - fi;
    const uint32_t p = w - (uint32_t)fi;
    const float xm1 = tape_[(p + 1) & (kTape - 1)], x0 = tape_[p & (kTape - 1)], x1 = tape_[(p - 1) & (kTape - 1)],
                x2 = tape_[(p - 2) & (kTape - 1)];
    const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2,
                c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
  }
  void Spring(float x, float* l, float* r) {
    float* o[2] = {l, r};
    for (int s = 0; s < 2; ++s) {
      Side& q = side_[s];
      q.hy = sp_hpc_ * (q.hy + x - q.hx), q.hx = x;   // 120 Hz high-pass
      q.lo += (q.hy - q.lo) * sp_in_lp_;              // 5 kHz low-pass
      const float y = q.line[(sw_ - (uint32_t)q.len) & (kSpringLine - 1)];
      q.damp += (y - q.damp) * sp_damp_;
      float v = q.lo + 0.8f * q.damp;
      const float c = s ? 0.62f : 0.58f;   // the dispersion: first-order allpasses (y = c x + z, z = x - c y)
      for (int j = 0; j < kAp; ++j) {
        const float u = c * v + q.ap[j];
        q.ap[j] = v - c * u;
        v = u;
      }
      q.line[sw_ & (kSpringLine - 1)] = v;
      *o[s] = y;
    }
    ++sw_;
  }

  float tape_[kTape];
  Side side_[2];
  uint32_t w_ = 0, sw_ = 0;
  float hp_x_ = 0, hp_y_ = 0, lp_ = 0, off_[3] = {0, 0, 0};
  Smooth d1_, g_, lpc_, wd_, fd_, sg_, og_[3], wf_[3];
  float c_free_ = 0, c_g_ = 0, c_lpc_ = 1, c_wd_ = 0, c_fd_ = 0, c_sg_ = 0, c_og_[3] = {1, 0, 0}, c_wf_[3] = {1, 0, 0};
  float sr_ = 0, hpc_ = 0, sp_hpc_ = 0, sp_in_lp_ = 0, sp_damp_ = 0, d1_max_ = 0, drift_c_ = 0;
  float ph_w_ = 0, ph_f_ = 0, drift_ = 0, drift_t_ = 0;
  uint32_t drift_at_ = 0, rng_ = 12345u;
  static constexpr int kStamps = 25;
  uint32_t stamp_[kStamps] = {}, nstamp_ = 0, frame_ = 0, last_tick_ = 0, word_ = 0;
  float fpt_ = 1000.0f;
  bool have_word_ = false, fresh_ = true, spring_on_ = false;
  int key_ = -1, clear_at_ = -1;
};

}  // namespace fx
}  // namespace core
