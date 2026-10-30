// plugins/nusaw/nusaw.c - NUSAW: a supersaw for pads and stabs, one voice per state (a poly track plays its
// notes on instances: manifest max_voices 8).
// SPDX-License-Identifier: MIT
// The DSP is NuSaw's (Charles Vestal, MIT: LICENSE.nusaw; NOTICE has the source and commit): 7 PolyBLEP saws
// (a centre and 3 detuned pairs, triangular 1:3:6 spacing, the outermost pair at up to 10 % of f0 through NuSaw's
// piecewise detune curve), the centre-anchored mix law (sides at spread^1.5 x 0.667, RMS normalised), random
// phases at a fresh note, a slow per-saw pitch drift, a sine sub an octave down, a 20 Hz DC blocker, a 2-pole
// TPT state-variable lowpass with its own ADSR and an ADSR amp envelope.
// Adapted for the Sidecar (Copyright 2026 trigcondition and contributors, MIT): mono (NuSaw's stereo pan of the
// pairs summed), one note a state, 48 kHz, the smoothing, detune, drift and filter coefficients per 12-sample
// block instead of per sample (no powf / tanf / sinf in the sample loop), resonance capped at Q 12, a retrigger
// of a sounding voice keeps its phases and filter (no click), no allocation, everything render() writes in the state.
//   P1 DETN detune  P2 MIX centre..sides  P3 CUT  P4 RES  P5 ENV filter env amount  P6 ATK  P7 REL  P8 SUB
//   P9 TUNE (64 = 0, +-12 st)  P10 DEC  P11 SUS  P12 FDEC filter decay  P13 FSUS filter sustain  P14 VEL  P15 LEV
#include <math.h>
#include "rmp/rmp_api.h"

#define NOSC 7

typedef struct {
  /* parameters (scaled) */
  float tune, detune, spread, cut, q_k, env_oct, a_rate, d_k, sus, r_k, fd_k, fsus, sub, vel_amt, level;
  /* the note */
  float note, f0, vel_gain;
  /* oscillators */
  float ph[NOSC], drift[NOSC], sub_ph;
  /* smoothed (per block) */
  float sm_det, sm_spr, sm_cut;
  /* DC blocker, SVF */
  float hp_x, hp_y, ic1, ic2;
  /* envelopes: level and stage (0 off, 1 attack, 2 decay, 3 sustain, 4 release) */
  float aenv, fenv;
  int32_t astage, fstage;
  uint32_t rng;
  int32_t gate;
} nsaw_state;

enum { ENV_OFF, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };

static const rmp_host *g_host;
static float g_fs = 48000.0f;
static float g_fatk = 1.0f / 48.0f;   /* the filter envelope's ~1 ms linear attack, per sample */

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float tan_pw(float x) {   /* tan(x) for 0..1.4 (filter prewarp), Pade, rel err < 3e-5 */
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}
static inline float poly_blep(float t, float dt, float idt) {   /* idt = 1/dt (no division per sample) */
  if (t < dt) { t *= idt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) * idt; return t * t + t + t + 1.0f; }
  return 0.0f;
}
static inline float fast_sin_cyc(float x) {   /* sin(2 pi x) for x in 0..1, parabolic + one correction, |err| ~ 1e-3 */
  x -= 0.5f;                                   /* -0.5..0.5, shifted by half a cycle */
  float ax = x < 0.0f ? -x : x;
  float y = 8.0f * x - 16.0f * x * ax;
  float ay = y < 0.0f ? -y : y;
  y += 0.225f * y * (ay - 1.0f);
  return -y;
}
static inline uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float rnd01(uint32_t *s) { return (float)(xs32(s) & 0x7FFFFF) * (1.0f / 8388608.0f); }

/* NuSaw's: 0..1 -> 1 ms .. 10 s, exponential */
static inline float p_secs(float p) { return 0.001f * expf(9.2103404f * p); }
/* NuSaw's decay / release multiplier: exp(-4 / (t fs)) (t: to e^-4, ~-35 dB) */
static inline float env_k(float secs) { return expf(-4.0f / (secs * g_fs)); }
/* NuSaw's piecewise-linear detune curve: gentle to 0.1, pads / stabs to 0.5, steep above */
static inline float detune_curve(float x) {
  if (x < 0.1f) return x * 0.2f;
  if (x < 0.5f) return 0.02f + (x - 0.1f) * 0.575f;
  return 0.25f + (x - 0.5f) * 1.5f;
}
/* the saws' offsets from f0 as a share of the outermost pair: centre, then +-k(k+1)/12 (1:3:6) */
static const float k_coeff[NOSC] = {0.0f, 1.0f / 6.0f, -1.0f / 6.0f, 0.5f, -0.5f, 1.0f, -1.0f};

static const float k_defaults[RMP_PARAMS] = {38, 89, 72, 13, 57, 23, 83, 16, 64, 64, 102, 70, 38, 64, 100, 64};

static int32_t nsaw_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  g_fatk = 1.0f / (0.001f * g_fs);
  return 0;
}

static void retune(nsaw_state *s) {
  s->f0 = 440.0f * expf((s->note + s->tune - 69.0f) * 0.05776227f);
}

static void nsaw_params(void *st, const float *p) {
  nsaw_state *s = st;
  s->detune = n01(p[0]);
  s->spread = n01(p[1]);
  s->cut = n01(p[2]);
  s->q_k = 1.0f / (0.5f + 11.5f * n01(p[3]));              /* Q 0.5..12 (NuSaw's goes to 20) */
  s->env_oct = 8.0f * n01(p[4]);                            /* octaves at the filter envelope's peak */
  s->a_rate = 1.0f / (p_secs(n01(p[5])) * g_fs);            /* linear attack */
  s->r_k = env_k(p_secs(n01(p[6])));
  s->sub = 0.8f * n01(p[7]);
  s->tune = (n01(p[8]) - 0.5039f) * 24.0f;
  s->d_k = env_k(p_secs(n01(p[9])));
  s->sus = n01(p[10]);
  s->fd_k = env_k(p_secs(n01(p[11])));
  s->fsus = n01(p[12]);
  s->vel_amt = n01(p[13]);
  float lv = n01(p[14]);
  s->level = lv * lv * 0.6f;
  retune(s);
}

static void nsaw_voice_init(void *st) {
  nsaw_state *s = st;
  s->rng = 0x9E3779B9u ^ (uint32_t)(uintptr_t)st;          /* each instance its own phases */
  if (!s->rng) s->rng = 1u;
  s->note = 60.0f;
  nsaw_params(s, k_defaults);
  s->sm_det = s->detune; s->sm_spr = s->spread; s->sm_cut = s->cut;
}

static void nsaw_note_on(void *st, float note, float vel) {
  nsaw_state *s = st;
  s->note = clampf(note, 0.0f, 127.0f);
  retune(s);
  vel = clampf(vel, 0.0f, 1.0f);
  s->vel_gain = 1.0f - s->vel_amt + s->vel_amt * vel;
  if (s->astage == ENV_OFF || s->aenv < 1e-3f) {           /* a fresh note: NuSaw's random phases, a clean filter */
    for (int j = 0; j < NOSC; ++j) { s->ph[j] = rnd01(&s->rng); s->drift[j] = 0.0f; }
    s->sub_ph = 0.0f;
    s->hp_x = s->hp_y = s->ic1 = s->ic2 = 0.0f;
  }
  s->astage = ENV_ATK;                                      /* the envelopes go on from where they are */
  s->fstage = ENV_ATK;
  s->gate = 1;
}

static void nsaw_note_off(void *st) {
  nsaw_state *s = st;
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

static void nsaw_render(void *st, float *out, int32_t n) {
  nsaw_state *s = st;
  if (s->astage == ENV_OFF) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  /* per block: smoothing (~5 ms), detune, the mix law, drift, the filter's cutoff */
  float sk = 1.0f - expf(-(float)n / (0.005f * g_fs));
  s->sm_det += (s->detune - s->sm_det) * sk;
  s->sm_spr += (s->spread - s->sm_spr) * sk;
  s->sm_cut += (s->cut - s->sm_cut) * sk;
  float f0 = s->f0;
  float inc0 = f0 / g_fs;
  float dinc = inc0 * 0.10f * detune_curve(s->sm_det);
  float gs = s->sm_spr * sqrtf(s->sm_spr) * 0.667f;
  if (gs < 0.015f) gs = 0.015f;
  float norm = 1.0f / sqrtf(1.0f + (float)(NOSC - 1) * gs * gs);
  float gc = norm, gside = gs * norm;
  float dk = 0.00114f * (float)n;                           /* NuSaw's ~8 Hz drift lowpass, per block */
  float inc[NOSC], idt[NOSC];
  for (int j = 0; j < NOSC; ++j) {
    float nz = rnd01(&s->rng) * 2.0f - 1.0f;
    s->drift[j] += (nz - s->drift[j]) * dk;
    float d = (inc0 + k_coeff[j] * dinc) * (1.0f + s->drift[j] * 0.0002f);
    d = clampf(d, 1e-6f, 0.45f);
    inc[j] = d; idt[j] = 1.0f / d;
  }
  float sub_inc = 0.5f * inc0, sub = s->sub;
  float base = 20.0f * expf(6.9077553f * s->sm_cut);       /* 20 Hz .. 20 kHz */
  float fc = base * expf(0.6931472f * s->fenv * s->env_oct);
  fc = clampf(fc, 20.0f, 0.4166f * g_fs);
  float g = tan_pw(3.14159265f * fc / g_fs);
  float k = s->q_k;
  float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
  const float hr = 1.0f - 6.2831853f * 20.0f / g_fs;       /* the 20 Hz DC blocker */
  float gain = s->level * s->vel_gain;
  float aenv = s->aenv, fenv = s->fenv;
  int32_t ast = s->astage, fst = s->fstage;
  float hx = s->hp_x, hy = s->hp_y, ic1 = s->ic1, ic2 = s->ic2, sph = s->sub_ph;
  float ph0 = s->ph[0], ph1 = s->ph[1], ph2 = s->ph[2], ph3 = s->ph[3], ph4 = s->ph[4], ph5 = s->ph[5], ph6 = s->ph[6];
  for (int32_t i = 0; i < n; ++i) {
#define SAW(P, J) (P += inc[J], P -= (P >= 1.0f) ? 1.0f : 0.0f, 2.0f * P - 1.0f - poly_blep(P, inc[J], idt[J]))
    float c = SAW(ph0, 0);
    float sd = SAW(ph1, 1) + SAW(ph2, 2) + SAW(ph3, 3) + SAW(ph4, 4) + SAW(ph5, 5) + SAW(ph6, 6);
#undef SAW
    float x = c * gc + sd * gside;
    sph += sub_inc; if (sph >= 1.0f) sph -= 1.0f;
    x += sub * fast_sin_cyc(sph);
    float h = x - hx + hr * hy;
    hx = x; hy = h;
    aenv = env_step(aenv, &ast, s->a_rate, s->d_k, s->sus, s->r_k);
    fenv = env_step(fenv, &fst, g_fatk, s->fd_k, s->fsus, s->r_k);   /* ~1 ms attack */
    float t3 = h - ic2;
    float t1 = a1 * ic1 + a2 * t3;
    float t2 = ic2 + a2 * ic1 + a3 * t3;
    ic1 = 2.0f * t1 - ic1;
    ic2 = 2.0f * t2 - ic2;
    float y = t2 * aenv * gain;
    if (!(y > -4.0f && y < 4.0f)) { y = 0.0f; ic1 = ic2 = hx = hy = 0.0f; }   /* never NaN */
    out[i] = clampf(y, -1.0f, 1.0f);
  }
  s->ph[0] = ph0; s->ph[1] = ph1; s->ph[2] = ph2; s->ph[3] = ph3; s->ph[4] = ph4; s->ph[5] = ph5; s->ph[6] = ph6;
  s->hp_x = hx; s->hp_y = hy; s->ic1 = ic1; s->ic2 = ic2; s->sub_ph = sph;
  s->aenv = aenv; s->fenv = fenv; s->astage = ast; s->fstage = fst;
  if (ast == ENV_OFF) { s->fstage = ENV_OFF; s->fenv = 0.0f; s->ic1 = s->ic2 = 0.0f; }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(nsaw_state), NSAW_PERMILLE,   /* NSAW_PERMILLE: the Makefile's, from tests/nsaw_check's Mac timing */
  nsaw_init, nsaw_voice_init, nsaw_note_on, nsaw_note_off, nsaw_params, nsaw_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
