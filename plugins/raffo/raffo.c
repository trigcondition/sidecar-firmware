// plugins/raffo/raffo.c - RAFFO: a 4-oscillator mono synth with a 4-pole ladder lowpass (basses, leads), one voice a
// state: a mono track plays it legato (a note while the gate is on glides there, no retrigger), a poly track plays
// chords on instances (manifest max_voices 8).
// SPDX-License-Identifier: MIT
// The DSP is RaffoSynth's (Nicolas Roulet, Julian Palladino, MIT: LICENSE.raffo) as the Schwung module "RaffoSynth"
// (charlesvestal/schwung-moog, src/dsp/moog_engine.c; NOTICE has the source and commit) has it: 4 oscillators
// (triangle, saw, square, 20 % pulse; an octave range and a +-50 cent detune each), the musicdsp "Moog VCF
// variation 1" 4-pole ladder (f = 1.16 fc, feedback x (1 - 0.15 f^2), input x 0.35013 f^4) with a contour (filter
// ADSR) on an exponential 20 Hz .. 20 kHz cutoff, an amp ADSR, glide only while legato, velocity at 0.5.
// Adapted for the Sidecar (Copyright 2026 trigcondition and contributors, MIT): 48 kHz phase accumulators instead of
// fmod on a sample counter, PolyBLEP on the saw / square / pulse edges (the original is naive), the pulse's DC
// removed, a fixed half key follow on the cutoff; the ladder with each stage's own input memory (as musicdsp's; the
// port reused the stage's output), RES mapped to a feedback up to ~3.9 (self-oscillation; the port's went to 1), a
// soft clip in the feedback and a passband make-up; the envelopes linear-attack / exponential decay and release
// (the port's quadratic curves) from where they are (no click); cutoff, coefficients, detune and glide per block
// (no powf in the sample loop), the knobs smoothed (~5 ms); no LFO, noise, mod wheel or bend (no host input for
// them); no allocation, NaN guarded.
// The 4 oscillators as 16 knobs: OSC1 (WAV1, the reference), OSC2 and OSC4 a detuned pair around it (WAV2, +-DTN2,
// level MIX), OSC3 the sub an octave down (WAV1, level SUB).
//   P1 WAV1  P2 WAV2  P3 DTN2  P4 MIX  P5 SUB  P6 CUT  P7 RES  P8 ENV
//   P9 ATK  P10 DEC  P11 SUS  P12 REL  P13 FDEC  P14 FSUS  P15 GLID  P16 LEV
#include <math.h>
#include "rmp/rmp_api.h"

enum { W_TRI, W_SAW, W_SQR, W_PLS };
enum { ENV_OFF, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };
#define CHUNK 32

typedef struct {
  /* parameters (scaled) */
  int32_t wav1, wav2;
  float det, mix, sub, cut, res, env, a_rate, d_k, sus, r_k, fd_k, fsus, glide_tau, level;
  /* smoothed (per block) */
  float sm_det, sm_mix, sm_sub, sm_cut, sm_res, sm_lev;
  /* the note */
  float note_cur, note_tgt, vel_gain;
  int32_t gate;
  /* oscillators: OSC1..OSC4 */
  float ph[4];
  /* the ladder: stage outputs y and inputs x */
  float y1, y2, y3, y4, x1, x2, x3, x4;
  /* envelopes */
  float aenv, fenv;
  int32_t astage, fstage;
  uint32_t rng;
} raffo_state;

static float g_fs = 48000.0f;

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline int32_t wave_of(float p) { return ((int32_t)clampf(p, 0.0f, 127.0f) * 4) >> 7; }   /* 4 bands of 32 */
static inline uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float rnd01(uint32_t *s) { return (float)(xs32(s) & 0x7FFFFF) * (1.0f / 8388608.0f); }
static inline float poly_blep(float t, float dt, float idt) {
  if (t < dt) { t *= idt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) * idt; return t * t + t + t + 1.0f; }
  return 0.0f;
}
static inline float soft(float x) {   /* a cubic soft clip, +-1 at |x| >= 1.5 */
  x = clampf(x, -1.5f, 1.5f);
  return x - x * x * x * (4.0f / 27.0f);
}
/* RaffoSynth's time law: 0..1 -> 1 ms + p^2 x 5 s */
static inline float p_secs(float p) { return 0.001f + p * p * 5.0f; }
static inline float env_k(float secs) { return expf(-4.0f / (secs * g_fs)); }   /* to e^-4 (~-35 dB) in secs */

static const float k_defaults[RMP_PARAMS] = {48, 80, 72, 85, 80, 44, 46, 64, 0, 60, 110, 30, 30, 24, 30, 100};

static int32_t raffo_init(const rmp_host *h) {
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  return 0;
}

static void raffo_params(void *st, const float *p) {
  raffo_state *s = st;
  s->wav1 = wave_of(p[0]);
  s->wav2 = wave_of(p[1]);
  s->det = (n01(p[2]) - 0.5039f) * 100.0f;                  /* cents, +-50 (RaffoSynth's detune range) */
  s->mix = n01(p[3]);
  s->sub = n01(p[4]);
  s->cut = n01(p[5]);
  float r = n01(p[6]);
  s->res = r * (2.0f - r);                                  /* more of the travel near the top */
  s->env = n01(p[7]);                                       /* the contour: up to the whole 20 Hz .. 20 kHz range */
  s->a_rate = 1.0f / (p_secs(n01(p[8])) * g_fs);
  s->d_k = env_k(p_secs(n01(p[9])));
  s->sus = n01(p[10]);
  s->r_k = env_k(p_secs(n01(p[11])));
  s->fd_k = env_k(p_secs(n01(p[12])));
  s->fsus = n01(p[13]);
  float g = n01(p[14]);
  s->glide_tau = g * g;                                     /* seconds, the time constant (0 .. 1 s) */
  float lv = n01(p[15]);
  s->level = lv * lv * 0.7f;
}

static void raffo_voice_init(void *st) {
  raffo_state *s = st;
  s->rng = 0x9E3779B9u ^ (uint32_t)(uintptr_t)st;
  if (!s->rng) s->rng = 1u;
  s->note_cur = s->note_tgt = 48.0f;
  raffo_params(s, k_defaults);
  s->sm_det = s->det; s->sm_mix = s->mix; s->sm_sub = s->sub; s->sm_cut = s->cut; s->sm_res = s->res;
  s->sm_lev = s->level;
}

static void raffo_note_on(void *st, float note, float vel) {
  raffo_state *s = st;
  note = clampf(note, 0.0f, 127.0f);
  s->vel_gain = 0.5f + 0.5f * clampf(vel, 0.0f, 1.0f);       /* RaffoSynth's velocity sensitivity 0.5 */
  if (s->gate && s->astage != ENV_OFF) {                    /* legato: glide there, no retrigger */
    s->note_tgt = note;
    if (s->glide_tau < 1e-4f) s->note_cur = note;
    return;
  }
  s->note_cur = s->note_tgt = note;
  if (s->astage == ENV_OFF || s->aenv < 1e-3f) {            /* a fresh note: phases, a clean ladder */
    s->ph[0] = s->ph[2] = 0.0f;
    s->ph[1] = rnd01(&s->rng); s->ph[3] = rnd01(&s->rng);
    s->y1 = s->y2 = s->y3 = s->y4 = s->x1 = s->x2 = s->x3 = s->x4 = 0.0f;
  }
  s->astage = ENV_ATK;                                      /* the envelopes go on from where they are */
  s->fstage = ENV_ATK;
  s->gate = 1;
}

static void raffo_note_off(void *st) {
  raffo_state *s = st;
  s->gate = 0;
  if (s->astage != ENV_OFF) s->astage = ENV_REL;
  if (s->fstage != ENV_OFF) s->fstage = ENV_REL;
}

static inline float env_step(float lv, int32_t *stage, float a_rate, float d_k, float sus, float r_k) {
  switch (*stage) {
    case ENV_ATK: lv += a_rate; if (lv >= 1.0f) { lv = 1.0f; *stage = ENV_DEC; } break;
    case ENV_DEC: lv = sus + (lv - sus) * d_k; if (lv <= sus + 1e-4f) { lv = sus; *stage = ENV_SUS; } break;
    case ENV_SUS: lv = sus; break;
    case ENV_REL: lv *= r_k; if (lv < 1e-4f) { lv = 0.0f; *stage = ENV_OFF; } break;
    default: lv = 0.0f; break;
  }
  return lv;
}

/* one oscillator added to buf: phase *ph, increment inc (cycles a sample), gain g */
static void osc_add(float *buf, int32_t n, float *ph, float inc, int32_t wave, float g) {
  float t = *ph, idt = 1.0f / inc;
  switch (wave) {
    case W_TRI:
      for (int32_t i = 0; i < n; ++i) {
        t += inc; t -= (t >= 1.0f) ? 1.0f : 0.0f;
        float u = t + 0.25f; u -= (u >= 1.0f) ? 1.0f : 0.0f;
        float a = u - 0.5f;
        buf[i] += g * (4.0f * (a < 0.0f ? -a : a) - 1.0f);
      }
      break;
    case W_SAW:
      for (int32_t i = 0; i < n; ++i) {
        t += inc; t -= (t >= 1.0f) ? 1.0f : 0.0f;
        buf[i] += g * (2.0f * t - 1.0f - poly_blep(t, inc, idt));
      }
      break;
    case W_SQR:
      for (int32_t i = 0; i < n; ++i) {
        t += inc; t -= (t >= 1.0f) ? 1.0f : 0.0f;
        float u = t + 0.5f; u -= (u >= 1.0f) ? 1.0f : 0.0f;
        buf[i] += g * ((t < 0.5f ? 1.0f : -1.0f) + poly_blep(t, inc, idt) - poly_blep(u, inc, idt));
      }
      break;
    default:   /* W_PLS: 20 % high, the DC (-0.6) removed */
      for (int32_t i = 0; i < n; ++i) {
        t += inc; t -= (t >= 1.0f) ? 1.0f : 0.0f;
        float u = t + 0.8f; u -= (u >= 1.0f) ? 1.0f : 0.0f;
        buf[i] += g * ((t < 0.2f ? 1.6f : -0.4f) + poly_blep(t, inc, idt) - poly_blep(u, inc, idt));
      }
      break;
  }
  *ph = t;
}

static void render_chunk(raffo_state *s, float *out, int32_t n) {
  /* per block: smoothing (~5 ms), glide, the oscillators' increments, the ladder's coefficients */
  float sk = 1.0f - expf(-(float)n / (0.005f * g_fs));
  s->sm_det += (s->det - s->sm_det) * sk;
  s->sm_mix += (s->mix - s->sm_mix) * sk;
  s->sm_sub += (s->sub - s->sm_sub) * sk;
  s->sm_cut += (s->cut - s->sm_cut) * sk;
  s->sm_res += (s->res - s->sm_res) * sk;
  s->sm_lev += (s->level - s->sm_lev) * sk;
  if (s->note_cur != s->note_tgt) {
    float gk = s->glide_tau < 1e-4f ? 1.0f : 1.0f - expf(-(float)n / (s->glide_tau * g_fs));
    s->note_cur += (s->note_tgt - s->note_cur) * gk;
    if (fabsf(s->note_tgt - s->note_cur) < 1e-3f) s->note_cur = s->note_tgt;
  }
  float f0 = 440.0f * expf((s->note_cur - 69.0f) * 0.05776227f);
  float inc = clampf(f0 / g_fs, 1e-6f, 0.45f);
  float dr = expf(s->sm_det * 0.00057762265f);              /* 2^(cents / 1200) */
  float inc2 = clampf(inc * dr, 1e-6f, 0.45f), inc4 = clampf(inc / dr, 1e-6f, 0.45f);
  float mix = s->sm_mix, sub = s->sm_sub;
  float norm = 1.0f / sqrtf(1.0f + 2.0f * mix * mix + sub * sub);
  float buf[CHUNK];
  for (int32_t i = 0; i < n; ++i) buf[i] = 0.0f;
  osc_add(buf, n, &s->ph[0], inc, s->wav1, norm);
  if (mix > 1e-3f) {
    osc_add(buf, n, &s->ph[1], inc2, s->wav2, mix * norm);
    osc_add(buf, n, &s->ph[3], inc4, s->wav2, mix * norm);
  }
  if (sub > 1e-3f) osc_add(buf, n, &s->ph[2], 0.5f * inc, s->wav1, sub * norm);

  /* half key follow (RaffoSynth's KEY FOLLOW at 0.5): C3 = 48 the reference, 0.5 oct an octave of the 9.97 */
  float cn = clampf(s->sm_cut + s->fenv * s->env + (s->note_cur - 48.0f) * 0.0041806f, 0.0f, 1.0f);
  float hz = 20.0f * expf(6.9077553f * cn);                 /* 20 * 1000^cn */
  float fc = clampf(hz / (0.5f * g_fs), 0.0005f, 0.8f);
  float f = fc * 1.16f, f2 = f * f;
  float fb = 3.9f * s->sm_res * (1.0f - 0.15f * f2);
  float ing = 0.35013f * f2 * f2, omf = 1.0f - f;
  float gain = s->sm_lev * s->vel_gain * (1.0f + 0.5f * s->sm_res);   /* the passband the resonance takes */
  float y1 = s->y1, y2 = s->y2, y3 = s->y3, y4 = s->y4, x1 = s->x1, x2 = s->x2, x3 = s->x3, x4 = s->x4;
  float aenv = s->aenv, fenv = s->fenv;
  int32_t ast = s->astage, fst = s->fstage;
  for (int32_t i = 0; i < n; ++i) {
    aenv = env_step(aenv, &ast, s->a_rate, s->d_k, s->sus, s->r_k);
    fenv = env_step(fenv, &fst, s->a_rate, s->fd_k, s->fsus, s->r_k);
    float in = (buf[i] - fb * soft(y4)) * ing;
    y1 = in + 0.3f * x1 + omf * y1; x1 = in;
    y2 = y1 + 0.3f * x2 + omf * y2; x2 = y1;
    y3 = y2 + 0.3f * x3 + omf * y3; x3 = y2;
    y4 = y3 + 0.3f * x4 + omf * y4; x4 = y3;
    if (!(y4 > -8.0f && y4 < 8.0f)) { y1 = y2 = y3 = y4 = x1 = x2 = x3 = x4 = 0.0f; }   /* never NaN, never runaway */
    out[i] = clampf(y4 * aenv * gain, -1.0f, 1.0f);
  }
  s->y1 = y1; s->y2 = y2; s->y3 = y3; s->y4 = y4; s->x1 = x1; s->x2 = x2; s->x3 = x3; s->x4 = x4;
  s->aenv = aenv; s->fenv = fenv; s->astage = ast; s->fstage = fst;
  if (ast == ENV_OFF) {
    s->fstage = ENV_OFF; s->fenv = 0.0f;
    s->y1 = s->y2 = s->y3 = s->y4 = s->x1 = s->x2 = s->x3 = s->x4 = 0.0f;
  }
}

static void raffo_render(void *st, float *out, int32_t n) {
  raffo_state *s = st;
  for (int32_t at = 0; at < n; at += CHUNK) {
    int32_t m = n - at < CHUNK ? n - at : CHUNK;
    if (s->astage == ENV_OFF) { for (int32_t i = 0; i < m; ++i) out[at + i] = 0.0f; continue; }
    render_chunk(s, out + at, m);
  }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(raffo_state), RAFFO_PERMILLE,   /* RAFFO_PERMILLE: the Makefile's, from tests/raffo_check */
  raffo_init, raffo_voice_init, raffo_note_on, raffo_note_off, raffo_params, raffo_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
