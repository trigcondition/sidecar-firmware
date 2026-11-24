// core/fx/rings_fx.h - RINGS (send FX TYPE 6): a modal / sympathetic-string resonator on the send input, a port
// of Mutable Instruments Rings' resonator models in their external-exciter mode (the send is the exciter).
//
// Ported from pichenettes/eurorack (commit 08460a6, rings/dsp/resonator.cc, string.cc, part.cc, limiter.h and the
// stmlib pieces they use: Svf, DCBlocker, CosineOscillator, DelayLine, ParameterInterpolator), MIT:
//
//   Copyright 2015 Emilie Gillet.
//   Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without
//   limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the following
//   conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//   TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// (also in core/fx/THIRD_PARTY_LICENSES.txt). Kept as in Rings: the modal resonator (ComputeFilters, the pickup
// position's cosine weights, odd/even outputs), the string (comb + FIR/IIR damping, no dispersion), the
// sympathetic-string note sets and the polyphony-2 chord table, the excitation low-pass, the voice allocation on a
// strum, the limiter and the model gains. Rewritten for the Sidecar: no internal exciter (an external input only),
// the strum detector (an onset detector of our own on the send input), the lookup tables computed at construction
// (the same formulas as rings/resources/lookup_tables.py), exp2f for SemitonesToRatio, no allocation, the denormal
// flushes, the knob map. Rings' FM voice, dispersion string and string+reverb models are not ported.
//
// Knobs P2..P5 (0..127, all p-lockable, 0 = the default; P1 is the send's ROUTE, not used here): P2 PITCH: semitones,
// C2 + v/2 (C2 65.4 Hz .. D#7); P3 MODEL x STRUCTURE: the zone 0..31 MODAL (2 voices, alternate strums ring on),
// 32..63 MODAL MONO (1 voice, 60 modes, odd partials L / even R), 64..95 SYMPATHETIC STRINGS (2 voices x 4 strings),
// 96..127 STRINGS CHORD, within it (32 steps) the structure 0.25..1 (harmonic -> stretched/bell; strings: the interval
// set; CHORD: one of Rings' 11 chords); a zone change goes through SendFx's fade and clear; P4 BRIGHTNESS 0.35..1; P5
// DECAY: Rings' damping 0.4..0.9 (modal T60 ~0.9 s .. minutes, strings ~2.4 s .. ~40 s). A strum (an onset in the
// send) moves to the next voice and latches P2 there, so a p-locked PITCH per step plays notes that ring over each
// other.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fx/knob_labels.h"

namespace core {
namespace fx {

// The Rytm's labels for P2..P5 (knob_labels.h, 0x24): P3's four models, each with its structure step (0..31).
// Continuous (no step): a structure step 0..31 inside each model.
inline constexpr Zone kRingsZones[] = {{1, 0, 0, 1, 0, "MODAL"}, {1, 32, 0, 1, 0, "MONO"}, {1, 64, 0, 1, 0, "STRNG"},
                                       {1, 96, 0, 1, 0, "CHORD"}};
inline constexpr KnobLabels kRingsLabels = {{"PTCH", "MODL", "BRIT", "DCY"}, kRingsZones, 4, {"Pitch", "Model", "Brightness", "Decay"}};
namespace rings {

constexpr float kPi = 3.14159265358979f;
inline float flush(float x) { return (x > -1e-20f && x < 1e-20f) ? 0.0f : x; }
inline float semitones(float s) { return exp2f(s * (1.0f / 12.0f)); }   // stmlib SemitonesToRatio
inline float lut(const float* t, float index, float size) {              // stmlib Interpolate
  index *= size;
  int i = (int)index;
  if (i < 0) i = 0;
  if (i > 255) i = 255;
  float f = index - (float)i;
  return t[i] + (t[i + 1] - t[i]) * f;
}

// stmlib::Svf (FREQUENCY_FAST / FREQUENCY_ACCURATE tan approximations)
struct Svf {
  float g = 0, r = 1, h = 1, s1 = 0, s2 = 0;
  void Reset() { s1 = s2 = 0.0f; }
  static float tan_fast(float f) {
    const float a = 3.260e-01f * kPi * kPi * kPi, b = 1.823e-01f * kPi * kPi * kPi * kPi * kPi;
    float f2 = f * f;
    return f * (kPi + f2 * (a + b * f2));
  }
  static float tan_accurate(float f) {
    const float p3 = kPi * kPi * kPi, p5 = p3 * kPi * kPi, p7 = p5 * kPi * kPi, p9 = p7 * kPi * kPi,
                p11 = p9 * kPi * kPi;
    const float a = 3.333314036e-01f * p3, b = 1.333923995e-01f * p5, c = 5.33740603e-02f * p7, d = 2.900525e-03f * p9,
                e = 9.5168091e-03f * p11;
    float f2 = f * f;
    return f * (kPi + f2 * (a + f2 * (b + f2 * (c + f2 * (d + f2 * e)))));
  }
  void set_g_q(float gg, float q) {
    g = gg, r = 1.0f / q;
    h = 1.0f / (1.0f + r * g + g * g);
  }
  float Bp(float in) {
    float hp = (in - r * s1 - g * s1 - s2) * h, bp = g * hp + s1;
    s1 = g * hp + bp;
    float lp = g * bp + s2;
    s2 = g * bp + lp;
    return bp;
  }
  float Lp(float in) {
    float hp = (in - r * s1 - g * s1 - s2) * h, bp = g * hp + s1;
    s1 = g * hp + bp;
    float lp = g * bp + s2;
    s2 = g * bp + lp;
    return lp;
  }
  void Flush() { s1 = flush(s1), s2 = flush(s2); }
};

struct DcBlocker {   // stmlib::DCBlocker
  float pole = 0.999f, x = 0, y = 0;
  void Init(float p) { pole = p, x = y = 0.0f; }
  float Process(float in) {
    float old = x;
    x = in;
    return y = flush(y * pole + x - old);
  }
};

struct CosOsc {   // stmlib::CosineOscillator, COSINE_OSCILLATOR_APPROXIMATE
  float y0 = 0, y1 = 0, coef = 0, init = 0;
  void Init(float frequency) {
    float sign = 16.0f;
    frequency -= 0.25f;
    if (frequency < 0.0f) frequency = -frequency;
    else if (frequency > 0.5f) frequency -= 0.5f;
    else sign = -16.0f;
    coef = sign * frequency * (1.0f - 2.0f * frequency);
    init = coef * 0.25f;
    y1 = init, y0 = 0.5f;
  }
  float Next() {
    float t = y0;
    y0 = coef * y0 - y1;
    y1 = t;
    return t + 0.5f;
  }
};

struct Tables {   // rings/resources/lookup_tables.py: 4_decades, stiffness, svf_shift (257 entries each)
  float four_decades[257], stiffness[257], svf_shift[257];
  Tables() {
    for (int i = 0; i < 257; ++i) {
      float x = (float)i / 256.0f;
      four_decades[i] = powf(10.0f, 4.0f * x);
      float g = x, s;
      if (g < 0.25f) s = -(0.25f - g) * 0.25f;
      else if (g < 0.3f) s = 0.0f;
      else if (g < 0.9f) s = 0.01f * powf(10.0f, (g - 0.3f) / 0.6f * 2.005f) - 0.01f;
      else {
        g = (g - 0.9f) / 0.1f;
        g *= g;
        s = 1.5f - cosf(g * kPi) / 2.0f;
      }
      stiffness[i] = s;
      float ratio = exp2f((float)i / 12.0f);
      svf_shift[i] = 2.0f * atanf(1.0f / ratio) / (2.0f * kPi);
    }
    stiffness[256] = stiffness[255] = 2.0f;
  }
};

constexpr int kMaxModes = 64;

// rings::Resonator (modal)
struct Resonator {
  float frequency = 220.0f / 48000.0f, structure = 0.25f, brightness = 0.5f, damping = 0.3f, position = 0.999f,
        previous_position = 0.0f;
  int resolution = kMaxModes;
  Svf f[kMaxModes];
  void Init(int res) {
    for (auto& m : f) m.Reset(), m.set_g_q(Svf::tan_fast(0.01f), 100.0f);
    previous_position = 0.0f;
    res -= res & 1;
    resolution = res < kMaxModes ? res : kMaxModes;
  }
  int ComputeFilters(const Tables& t) {
    float stiffness = lut(t.stiffness, structure, 256.0f), harmonic = frequency, stretch = 1.0f;
    float q = 500.0f * lut(t.four_decades, damping, 256.0f);
    float att = 1.0f - structure;
    att *= att, att *= att, att *= att;
    float b = brightness * (1.0f - 0.2f * att);
    float q_loss = b * (2.0f - b) * 0.85f + 0.15f, q_loss_rate = structure * (2.0f - structure) * 0.1f;
    int n = 0;
    for (int i = 0; i < resolution; ++i) {
      float pf = harmonic * stretch;
      if (pf >= 0.49f) pf = 0.49f;
      else n = i + 1;
      f[i].set_g_q(Svf::tan_fast(pf), 1.0f + pf * q);
      stretch += stiffness;
      stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
      q_loss += q_loss_rate * (1.0f - q_loss);
      harmonic += frequency;
      q *= q_loss;
    }
    return n;
  }
  void Process(const Tables& t, const float* in, float* out, float* aux, int size) {
    int n = ComputeFilters(t);
    n -= n & 1;   // pairs (odd, even)
    float step = (position - previous_position) / (float)size;
    for (int s = 0; s < size; ++s) {
      previous_position += step;
      CosOsc amp;
      amp.Init(previous_position);
      float x = in[s] * 0.125f, odd = 0.0f, even = 0.0f;
      for (int i = 0; i < n;) {
        odd += amp.Next() * f[i++].Bp(x);
        even += amp.Next() * f[i++].Bp(x);
      }
      out[s] = odd, aux[s] = even;
    }
    previous_position = position;
    for (int i = 0; i < resolution; ++i) f[i].Flush();
  }
};

// rings::String without dispersion (the sympathetic-string model's strings)
struct String {
  static constexpr int kLine = 2048;   // rings kDelayLineSize: 23.5 Hz lowest at 48 kHz
  float line[kLine];
  int wp = 0;   // stmlib DelayLine: written at wp, then wp decrements
  float frequency = 220.0f / 48000.0f, brightness = 0.5f, damping = 0.3f, position = 0.8f;
  float delay = 0, clamped_position = 0, prev_comp = 0, src_phase = 0, out_s[2] = {0, 0}, aux_s[2] = {0, 0};
  float fir_x = 0, fir_xx = 0, fir_b = 0, fir_d = 0;   // DampingFilter
  Svf iir;
  void Init() {
    memset(line, 0, sizeof line);
    wp = 0;
    delay = 1.0f / frequency;
    clamped_position = prev_comp = src_phase = 0.0f;
    out_s[0] = out_s[1] = aux_s[0] = aux_s[1] = 0.0f;
    fir_x = fir_xx = fir_b = fir_d = 0.0f;
    iir.Reset();
  }
  float Read(float d) const {
    int di = (int)d;
    float fr = d - (float)di;
    float a = line[(wp + di) & (kLine - 1)], b = line[(wp + di + 1) & (kLine - 1)];
    return a + (b - a) * fr;
  }
  float ReadHermite(float d) const {
    int di = (int)d;
    float fr = d - (float)di;
    int t = wp + di + kLine;
    float xm1 = line[(t - 1) & (kLine - 1)], x0 = line[t & (kLine - 1)], x1 = line[(t + 1) & (kLine - 1)],
          x2 = line[(t + 2) & (kLine - 1)];
    float c = (x1 - xm1) * 0.5f, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5f, b_neg = w + a;
    return (((a * fr) - b_neg) * fr + c) * fr + x0;
  }
  void Write(float x) {
    line[wp] = flush(x);
    wp = (wp - 1 + kLine) & (kLine - 1);
  }
  void Process(const Tables& t, const float* in, float* out, float* aux, int size) {
    float dl = 1.0f / frequency;
    dl = dl < 4.0f ? 4.0f : dl > kLine - 4.0f ? kLine - 4.0f : dl;
    float src_ratio = dl * frequency;
    if (src_ratio >= 0.9999f) src_phase = 1.0f, src_ratio = 1.0f;
    float cp = 0.5f - 0.98f * fabsf(position - 0.5f);
    float lf = damping * (2.0f - damping);
    float rt60 = 0.07f * semitones(lf * 96.0f) * 48000.0f;
    float e = -120.0f * dl / src_ratio / rt60;
    float dcoef = semitones(e < -127.0f ? -127.0f : e);
    float br = brightness * brightness;
    float cutoff = 24.0f + damping * damping * 48.0f + brightness * brightness * 24.0f;
    if (cutoff > 84.0f) cutoff = 84.0f;
    float df = frequency * semitones(cutoff);
    if (df > 0.499f) df = 0.499f;
    if (damping >= 0.95f) {
      float ti = 20.0f * (damping - 0.95f);
      dcoef += ti * (1.0f - dcoef), br += ti * (1.0f - br), df += ti * (0.4999f - df), cutoff += ti * (128.0f - cutoff);
    }
    float inv = 1.0f / (float)size;
    float d_step = (dl - delay) * inv, p_step = (cp - clamped_position) * inv;
    float fd_step = (dcoef - fir_d) * inv, fb_step = (br - fir_b) * inv;
    iir.set_g_q(Svf::tan_accurate(df), 0.5f);
    float comp = 1.0f - lut(t.svf_shift, cutoff, 1.0f), c_step = (comp - prev_comp) * inv;
    for (int s = 0; s < size; ++s) {
      src_phase += src_ratio;
      if (src_phase > 1.0f) {
        src_phase -= 1.0f;
        delay += d_step, clamped_position += p_step, prev_comp += c_step;
        float d = delay, comb = d * clamped_position;
        d *= prev_comp;   // IIR delay
        d -= 1.0f;        // FIR delay
        float x = ReadHermite(d) + in[s];
        float h0 = (1.0f + fir_b) * 0.5f, h1 = (1.0f - fir_b) * 0.25f;   // DampingFilter
        float y = fir_d * (h0 * fir_x + h1 * (x + fir_xx));
        fir_xx = fir_x, fir_x = x;
        fir_b += fb_step, fir_d += fd_step;
        y = iir.Lp(y);
        Write(y);
        out_s[1] = out_s[0], aux_s[1] = aux_s[0];
        out_s[0] = y, aux_s[0] = Read(comb);
      }
      out[s] += out_s[1] + (out_s[0] - out_s[1]) * src_phase;
      aux[s] += aux_s[1] + (aux_s[0] - aux_s[1]) * src_phase;
    }
    delay = dl, clamped_position = cp, prev_comp = comp, fir_d = dcoef, fir_b = br;
    iir.Flush();
    fir_x = flush(fir_x), fir_xx = flush(fir_xx);
  }
};

// rings::Part (external exciter only), polyphony 2 (MODAL MONO: 1)
class RingsFx {
 public:
  static constexpr int kVoices = 2, kStrings = 4, kBlock = 24;   // Rings' kMaxBlockSize
  RingsFx() { Clear(); }
  void Clear() {
    for (int v = 0; v < kVoices; ++v)
      for (int s = 0; s < kStrings; ++s) str_[v][s].Init();
    ClearSmall();
  }
  // the clear spread over calls (a string's 8 KB line at a time, then the small state): true when done
  bool ClearStep(int bytes) {
    for (; bytes > 0 && clear_at_ < kVoices * kStrings; ++clear_at_, bytes -= (int)sizeof(String))
      str_[clear_at_ / kStrings][clear_at_ % kStrings].Init();
    if (clear_at_ < kVoices * kStrings) return false;
    ClearSmall();
    return true;
  }
  void ClearSmall() {
    clear_at_ = 0;
    for (int v = 0; v < kVoices; ++v) {
      res_[v].Init(64 / kVoices - 4);
      exc_[v].Reset();
      dc_[v].Init(1.0f - 10.0f / 48000.0f);
      note_[v] = 36.0f;
    }
    active_ = 0;
    model_ = -1;
    env_fast_ = env_slow_ = 0.0f;
    holdoff_ = 0;
    peak_ = 0.5f;
    strums_ = 0;
  }
  uint32_t Strums() const { return strums_; }
  // in[n] mono -> out[n] L/R (added); k = P2..P5
  void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float /*a*/) {
    for (int i = 0; i < n; i += kBlock) Block(in + i, n - i < kBlock ? n - i : kBlock, out + i, k, sr);
  }

 private:
  static float k01(uint8_t v) { return (float)(v > 127 ? 127 : v) * (1.0f / 127.0f); }
  void Block(const float* in, int size, float (*out)[2], const uint8_t* k, float sr) {
    int ms = k[1] > 127 ? 127 : k[1], model = ms >> 5;   // 0 MODAL, 1 MODAL MONO, 2 STRINGS, 3 STRINGS CHORD
    int poly = model == 1 ? 1 : kVoices;
    if (model != model_) {   // Rings' ConfigureResonators: the resonators of the new model from silence (SendFx
                             // changes the model zone only through a fade and a clear: the strings are clean when
                             // model_ < 0)
      for (int v = 0; v < kVoices; ++v) {
        res_[v].Init(64 / poly - 4);
        if (model_ >= 0)
          for (int s = 0; s < kStrings; ++s) str_[v][s].Init();
        exc_[v].Reset();
      }
      active_ = 0;
      model_ = model;
    }
    float structure = 0.25f + 0.75f * (float)(ms & 31) / 31.0f, brightness = 0.35f + 0.65f * k01(k[2]),
          damping = 0.4f + 0.5f * k01(k[3]), position = 0.3f;
    float note = 36.0f + (float)((k[0] > 127 ? 127 : k[0]) >> 1);
    // the strum: an onset in the send (a fast envelope over twice a slow one), at most every 50 ms
    bool strum = false;
    for (int i = 0; i < size; ++i) {
      float x = fabsf(in[i]);
      env_fast_ += (x > env_fast_ ? 0.05f : 0.002f) * (x - env_fast_);    // ~0.4 ms up, 10 ms down
      env_slow_ += (x > env_slow_ ? 0.002f : 0.0005f) * (x - env_slow_);  // 10 ms up, 40 ms down
      if (holdoff_ > 0) --holdoff_;
      else if (env_fast_ > 2.0f * env_slow_ + 0.002f) strum = true, holdoff_ = (int)(0.05f * sr);
    }
    env_fast_ = flush(env_fast_), env_slow_ = flush(env_slow_);
    if (strum) {
      ++strums_;
      if (poly > 1) active_ = (active_ + 1) % poly;
    }
    note_[active_] = note;
    float lo[kBlock], ro[kBlock];
    for (int i = 0; i < size; ++i) lo[i] = ro[i] = 0.0f;
    float cutoff = brightness * (2.0f - brightness);
    for (int v = 0; v < poly; ++v) {
      float frequency = semitones(note_[v] - 69.0f) * (440.0f / sr);
      float fc = v == active_ ? 0.4f * semitones((cutoff - 1.0f) * 108.0f) : 10.0f / sr;
      if (fc > 0.499f) fc = 0.499f;
      exc_[v].set_g_q(Svf::tan_fast(fc), 0.8f);   // Rings: FREQUENCY_DIRTY; the fast one is closer
      float x[kBlock], ob[kBlock], ab[kBlock];
      for (int i = 0; i < size; ++i) x[i] = v == active_ ? in[i] : 0.0f;
      if (model <= 1) {
        for (int i = 0; i < size; ++i) x[i] = exc_[v].Lp(x[i]);
        Resonator& r = res_[v];
        r.frequency = frequency, r.structure = structure, r.brightness = brightness * brightness,
        r.position = position, r.damping = damping;
        r.Process(t_, x, ob, ab, size);
      } else {
        float notes[kStrings];
        SympatheticNotes(note_[v], model == 3 ? 2.0f + (float)((ms & 31) * 11 / 32) : structure,
                         notes);
        if (v == active_)
          for (int i = 0; i < size; ++i) x[i] *= 0.35355339f;   // 1 / sqrt(num_strings * 2)
        for (int i = 0; i < size; ++i) x[i] = dc_[v].Process(exc_[v].Lp(x[i])), ob[i] = ab[i] = 0.0f;
        for (int s = 0; s < kStrings; ++s) {
          String& st = str_[v][s];
          st.frequency = semitones(notes[s] - 69.0f) * (440.0f / sr);
          st.brightness = brightness, st.position = position;
          st.damping = damping + (float)s / (float)kStrings * (0.95f - damping);
          st.Process(t_, x, ob, ab, size);
        }
      }
      exc_[v].Flush();
      if (poly == 1)
        for (int i = 0; i < size; ++i) lo[i] += ob[i], ro[i] += ab[i];
      else {
        float* d = v & 1 ? ro : lo;
        for (int i = 0; i < size; ++i) d[i] += ob[i] - ab[i];
      }
    }
    // rings::Limiter with the model's gain (Rings' 1.4 / 1.0) and a make-up for line-level drum sends (x6 modal, x3
    // strings: a drum off the resonator's pitch comes back ~-10 dB, on it the limiter holds it); a quarter of each side
    // to the other (Rings pans voices hard)
    float pre = model <= 1 ? 1.4f * 6.0f : 1.0f * 3.0f;
    for (int i = 0; i < size; ++i) {
      float l = lo[i] * pre, r = ro[i] * pre;
      float pk = fabsf(l), pr = fabsf(r), ps = fabsf(r - l);
      pk = pk > pr ? pk : pr, pk = pk > ps ? pk : ps;
      float e = pk - peak_;
      peak_ += (e > 0 ? 0.05f : 0.00002f) * e;
      float g = peak_ <= 1.0f ? 1.0f : 1.0f / peak_;
      l *= g * 0.8f, r *= g * 0.8f;
      l = l * (27.0f + l * l) / (27.0f + 9.0f * l * l);
      r = r * (27.0f + r * r) / (27.0f + 9.0f * r * r);
      out[i][0] += 0.5f * (l + 0.25f * r);
      out[i][1] += 0.5f * (r + 0.25f * l);
    }
    peak_ = flush(peak_);
  }
  // rings::Part::ComputeSympatheticStringsNotes for 4 strings (polyphony 2): parameter < 2 free (structure), >= 2
  // chord index + 2
  static void SympatheticNotes(float n, float parameter, float* dst) {
    static const float kChords[11][4] = {   // Rings' original table, polyphony 2
        {-12.0f, 0.0f, 0.01f, 12.0f}, {-12.0f, 3.0f, 7.0f, 10.0f}, {-12.0f, 3.0f, 7.0f, 12.0f},
        {-12.0f, 3.0f, 7.0f, 14.0f},  {-12.0f, 3.0f, 7.0f, 17.0f}, {-12.0f, 7.0f, 12.0f, 19.0f},
        {-12.0f, 4.0f, 7.0f, 17.0f},  {-12.0f, 4.0f, 7.0f, 14.0f}, {-12.0f, 4.0f, 7.0f, 12.0f},
        {-12.0f, 4.0f, 7.0f, 11.0f},  {-12.0f, 5.0f, 7.0f, 12.0f}};
    if (parameter >= 2.0f) {
      int c = (int)(parameter - 2.0f);
      c = c < 0 ? 0 : c > 10 ? 10 : c;
      for (int i = 0; i < kStrings; ++i) dst[i] = kChords[c][i] + n;
      return;
    }
    const float notes[9] = {n - 12.0f, n - 12.0f, n - 7.01955f, n, n + 7.01955f, n + 12.0f, n + 19.01955f,
                            n + 24.0f, n + 24.0f};   // [0] the tonic: Rings' tonic is the note's octave here
    const float detunings[4] = {0.013f, 0.011f, 0.007f, 0.017f};
    const int detuned = (kStrings - 1) >> 1, first = kStrings - detuned;
    for (int i = 0; i < first; ++i) {
      float x = 3.0f;
      if (i != 0) {
        x = parameter * 7.0f;
        parameter += (1.0f - parameter) * 0.2f;
      }
      int xi = (int)x;
      float xf = x - (float)xi;
      if (xf < 0.5f) {   // Part::Squash
        xf *= 2.0f, xf *= xf, xf *= xf, xf *= xf, xf *= xf, xf *= 0.5f;
      } else {
        xf = 1.0f - xf;
        xf *= 2.0f, xf *= xf, xf *= xf, xf *= xf, xf *= xf, xf *= 0.5f;
        xf = 1.0f - xf;
      }
      xi = xi > 7 ? 7 : xi;
      dst[i] = notes[xi] + (notes[xi + 1] - notes[xi]) * xf;
      if (i + first < kStrings) dst[i + first] = dst[i] + detunings[i & 3];
    }
  }

  Tables t_;
  Resonator res_[kVoices];
  String str_[kVoices][kStrings];
  Svf exc_[kVoices];
  DcBlocker dc_[kVoices];
  float note_[kVoices];
  int active_, model_, holdoff_, clear_at_ = 0;
  float env_fast_, env_slow_, peak_;
  uint32_t strums_;
};

}  // namespace rings
using rings::RingsFx;
}  // namespace fx
}  // namespace core
