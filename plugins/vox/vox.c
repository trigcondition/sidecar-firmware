// plugins/vox/vox.c - VOX: a sung / spoken vocal tract, one voice per state (a poly track plays its notes on
// instances: manifest max_voices).
// SPDX-License-Identifier: MIT
// The DSP is Pink Trombone's (Neil Thapen, version 1.1, March 2017, MIT: LICENSE.pinktrombone; NOTICE has the
// source): the glottis (the Liljencrants-Fant waveform, its Rd from the tenseness, aspiration noise through a
// 500 Hz bandpass, modulated by the glottal cycle, vibrato and the simplex pitch / tenseness wobble) into the tract
// (a 44-segment Kelly-Lochbaum waveguide run at twice the sample rate, glottal reflection 0.75, lip -0.85, the
// 28-segment nasal tract at the velum's 3-way junction, the tongue's rest shape, constrictions with their cosine
// skirts, fricative noise through a 1 kHz bandpass injected at a constriction, plosive transients when a closure
// opens, the diameters moving towards their targets at 15 / s (slower back, faster closing), the reflections
// interpolated across each block).
// Adapted for the Sidecar (Copyright 2026 trigcondition and contributors, MIT): the touches become knobs (the tongue
// index within the range Pink Trombone's tongue control allows for the tongue diameter, one constriction's
// position and diameter, the velum), the glottis's intensity an envelope (ATK / REL, velocity) and its pitch the note
// (a glide), one voice a state, 32-bit floats, the LF waveform's exp / sin per sample as recurrences restarted each
// period, the simplex noise a periodic 1D gradient noise, the Web Audio bandpasses RBJ biquads, transients in a fixed
// array with a 0.2 ms onset, a closed segment's area a small floor (upstream's 0.999 / -1 reflections
// jump as a closure opens: a click), the nose junction's interpolation the same way round as the others' (upstream swaps old and new there),
// a release to exact silence, a NaN guard; no allocation, everything render() writes in the state.
//   P1 VOWL tongue position (back .. front)  P2 TONG tongue diameter (high .. low)  P3 CONS constriction position
//   (throat .. lips)  P4 CDIA constriction diameter (closed .. open: stops, fricatives, approximants, none)
//   P5 TENS tenseness (breathy .. pressed)  P6 BRTH aspiration  P7 VIBR vibrato depth  P8 NASL velum
//   P9 ATK  P10 REL  P11 GLID portamento  P12 WOBL natural wobble  P13 VRAT vibrato rate  P14 VEL  P15 LEV
#include <math.h>
#include "rmp/rmp_api.h"

#define TN 44                 /* tract segments */
#define NN 28                 /* nose segments (floor(28 n / 44)) */
#define BLADE 10
#define TIP 32
#define LIP 39
#define NOSE0 (TN - NN + 1)   /* 17: the velum junction */
#define NTRANS 4

typedef struct { float amp, on, pos; } vox_trans;

typedef struct {
  /* parameters (scaled) */
  float tongue_i, tongue_d, cons_i, cons_d, tens, breath, vib_amt, velum, a_rate, r_k, glide_k, wobble, vib_hz,
      vel_amt, level;
  float target[TN];                /* the tract's target diameters (rest shape + constriction), from params() */
  /* the tract */
  float R[TN], L[TN], diam[TN], refl[TN], nrefl[TN];   /* refl: the block's start, nrefl: its end (index 1..TN-1) */
  float noseR[NN], noseL[NN], nose_d0;
  float rl, rr, rn, nrl, nrr, nrn;  /* the velum junction's reflections, start and end of the block */
  vox_trans tr[NTRANS];
  int32_t last_obst;
  /* the glottis */
  float note, semi, freq, t_wave, dt_wave;            /* semi: the gliding pitch (semitones); t in the period 0..1 */
  float lf_alpha, lf_e0, lf_eps, lf_shift, lf_idelta, lf_te, lf_omega;
  float ea, ka, sn, cs, rs, rc, eb, kb;              /* the waveform's recurrences */
  int32_t lf_tail;
  float tens_now, loud, vib_ph, nz[5];               /* nz: the noise channels' positions (0..256) */
  float frc_i, frc_d;                                 /* the smoothed constriction for the fricative noise */
  /* noise bandpasses (aspiration, fricative) */
  float a1x, a2x, a1y, a2y, f1x, f2x, f1y, f2y;
  /* envelope: 0 off, 1 attack, 2 sustain, 3 release, 4 the tract's tail */
  float env, vel_gain, hp_x, hp_y;
  int32_t stage, tail_blocks;
  uint32_t rng, seed;
} vox_state;

enum { ST_OFF, ST_ATK, ST_SUS, ST_REL, ST_TAIL };

static const rmp_host *g_host;
static float g_fs = 48000.0f;
static float g_nrefl[NN];          /* the nose's inner reflections: fixed (upstream computes them once) */
static float g_nose_rest[NN];
static float g_bp_b0, g_bp_a1, g_bp_a2, g_bf_b0, g_bf_a1, g_bf_a2;   /* RBJ bandpass, b1 = 0, b2 = -b0 */
static float g_trans_k;            /* the transient's 2^(-200 t) per tract step */
static float g_trans_on;           /* its onset, ~0.2 ms (upstream's starts at full strength: a click) */

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float move_towards(float c, float t, float up, float down) {
  return c < t ? (c + up < t ? c + up : t) : (c - down > t ? c - down : t);
}
static inline uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float rnd_pm(uint32_t *s) { return (float)(xs32(s) & 0xFFFFFF) * (1.0f / 8388608.0f) - 1.0f; }
static inline float fast_sin_cyc(float x) {   /* sin(2 pi x) for x in 0..1, |err| ~ 1e-3 */
  x -= 0.5f;
  float ax = x < 0.0f ? -x : x;
  float y = 8.0f * x - 16.0f * x * ax;
  float ay = y < 0.0f ? -y : y;
  y += 0.225f * y * (ay - 1.0f);
  return -y;
}
/* a periodic (256) 1D gradient noise, about -1..1: Pink Trombone's simplex1 stand-in */
static inline float grad(uint32_t i, uint32_t seed) {
  uint32_t h = (i & 255u) * 0x9E3779B1u ^ seed;
  h ^= h >> 15; h *= 0x85EBCA77u; h ^= h >> 13;
  return (float)(h & 0xFFFF) * (2.0f / 65535.0f) - 1.0f;
}
static float gnoise(float x, uint32_t seed) {
  float fl = floorf(x);
  float f = x - fl;
  uint32_t i = (uint32_t)(int32_t)fl;
  float u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
  return 2.0f * ((1.0f - u) * grad(i, seed) * f + u * grad(i + 1u, seed) * (f - 1.0f));
}
static const float k_nz_rate[5] = {4.07f, 2.15f, 0.46f, 0.36f, 1.99f};   /* vibrato x2, tenseness x2, aspiration */

static void bandpass(float f, float q, float *b0, float *a1, float *a2) {   /* RBJ, 0 dB peak (Web Audio's) */
  float w = 6.2831853f * f / g_fs, al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
  *b0 = al / a0; *a1 = -2.0f * cosf(w) / a0; *a2 = (1.0f - al) / a0;
}

static const float k_defaults[RMP_PARAMS] = {4, 33, 96, 127, 94, 64, 21, 0, 68, 74, 16, 32, 64, 64, 100, 64};

static int32_t vox_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  for (int i = 0; i < NN; ++i) {
    float d = 2.0f * ((float)i / NN), dia = d < 1.0f ? 0.4f + 1.6f * d : 0.5f + 1.5f * (2.0f - d);
    g_nose_rest[i] = dia < 1.9f ? dia : 1.9f;
  }
  g_nrefl[0] = 0.0f;
  for (int i = 1; i < NN; ++i) {
    float a0 = g_nose_rest[i - 1] * g_nose_rest[i - 1], a1 = g_nose_rest[i] * g_nose_rest[i];
    g_nrefl[i] = (a0 - a1) / (a0 + a1);
  }
  bandpass(500.0f, 0.5f, &g_bp_b0, &g_bp_a1, &g_bp_a2);
  bandpass(1000.0f, 0.5f, &g_bf_b0, &g_bf_a1, &g_bf_a2);
  g_trans_k = exp2f(-200.0f / (2.0f * g_fs));
  g_trans_on = 1.0f - expf(-1.0f / (0.0002f * 2.0f * g_fs));
  return 0;
}

/* the targets: the tongue's rest shape (setRestDiameter), then the constriction (handleTouches) */
static void shape_targets(vox_state *s) {
  for (int i = 0; i < TN; ++i) s->target[i] = i < 7 ? 0.6f : i < 12 ? 1.1f : 1.5f;
  float ftd = 2.0f + (s->tongue_d - 2.0f) / 1.5f, amp = 1.5f - ftd + 1.7f;
  for (int i = BLADE; i < LIP; ++i) {
    float t = 1.1f * 3.14159265f * (s->tongue_i - (float)i) / (float)(TIP - BLADE);
    float curve = amp * cosf(t);
    if (i == LIP - 1) curve *= 0.8f;
    if (i == BLADE || i == LIP - 2) curve *= 0.94f;
    s->target[i] = 1.5f - curve;
  }
  float idx = s->cons_i, dia = s->cons_d - 0.3f;
  if (dia < 0.0f) dia = 0.0f;
  if (dia < 3.0f) {
    float width = idx < 25.0f ? 10.0f : idx >= (float)TIP ? 5.0f : 10.0f - 5.0f * (idx - 25.0f) / (float)(TIP - 25);
    int ii = (int)(idx + 0.5f), w = (int)ceilf(width);
    for (int k = -w - 1; (float)k < width + 1.0f; ++k) {
      int j = ii + k;
      if (j < 0 || j >= TN) continue;
      float rel = fabsf((float)j - idx) - 0.5f, shrink;
      if (rel <= 0.0f) shrink = 0.0f;
      else if (rel > width) shrink = 1.0f;
      else shrink = 0.5f * (1.0f - cosf(3.14159265f * rel / width));
      if (dia < s->target[j]) s->target[j] = dia + (s->target[j] - dia) * shrink;
    }
  }
}

static void vox_params(void *st, const float *p) {
  vox_state *s = st;
  /* P2: the tongue control's radius 2.05 (high) .. 3.5 (low); P1 the index within what that radius allows */
  float td = 2.05f + 1.45f * n01(p[1]);
  float fp = (3.5f - td) / 1.45f;
  fp = powf(fp, 0.58f) - 0.2f * (fp * fp - fp);
  float lo = BLADE + 2, hi = TIP - 3, centre = 0.5f * (lo + hi), out = fp * 0.5f * (hi - lo);
  s->tongue_d = td;
  s->tongue_i = centre - out + 2.0f * out * n01(p[0]);
  s->cons_i = 6.0f + 36.0f * n01(p[2]);                    /* 6 (pharynx) .. 42 (the lips) */
  float cd = n01(p[3]);
  s->cons_d = 3.4f * cd * cd - 0.1f;                       /* closed to ~20, fricative ~44..62, none at 127 */
  float tx = n01(p[4]);
  s->tens = 1.0f - cosf(tx * 1.5707963f);                  /* upstream's tenseness from its control */
  s->breath = 2.0f * n01(p[5]);                            /* 64: upstream's aspiration */
  s->vib_amt = 0.03f * n01(p[6]);                          /* 21: upstream's 0.005 */
  s->velum = 0.01f + 0.59f * n01(p[7]);                    /* upstream: 0.01 closed, 0.4 nasal */
  s->a_rate = 1.0f / (0.001f * powf(2000.0f, n01(p[8])) * g_fs);   /* 1 ms .. 2 s, linear */
  s->r_k = expf(-4.6f / (0.005f * powf(800.0f, n01(p[9])) * g_fs)); /* 5 ms .. 4 s to -40 dB */
  s->glide_k = 0.003f * powf(500.0f, n01(p[10]));          /* the glide's time constant, s */
  s->wobble = 2.0f * n01(p[11]);                           /* 64: upstream's wobble */
  s->vib_hz = 2.0f + 8.0f * n01(p[12]);                    /* 2 .. 10 Hz, 64: ~6 */
  s->vel_amt = n01(p[13]);
  float lv = n01(p[14]);
  s->level = lv * lv;
  shape_targets(s);
}

static void reflections(vox_state *s);

static void vox_voice_init(void *st) {
  vox_state *s = st;
  s->seed = 0x9E3779B9u ^ (uint32_t)(uintptr_t)st;
  s->rng = s->seed ? s->seed : 1u;
  for (int k = 0; k < 5; ++k) s->nz[k] = (float)(xs32(&s->rng) & 255u);
  s->note = 60.0f;
  vox_params(s, k_defaults);
  for (int i = 0; i < TN; ++i) s->diam[i] = s->target[i];
  s->nose_d0 = s->velum;
  s->last_obst = -1;
  s->semi = s->note;
  s->tens_now = s->tens;
  s->frc_i = s->cons_i; s->frc_d = s->cons_d;
  reflections(s);
  reflections(s);
}

/* the reflections at the block's end (calculateReflections) */
static void reflections(vox_state *s) {
  float A[TN];
  for (int i = 0; i < TN; ++i) { float a = s->diam[i] * s->diam[i]; A[i] = a > 1e-4f ? a : 1e-4f; }
  for (int i = 1; i < TN; ++i) {   /* upstream: 0.999 where A is 0, -1 next to it: a jump when a closure opens */
    s->refl[i] = s->nrefl[i];
    s->nrefl[i] = (A[i - 1] - A[i]) / (A[i - 1] + A[i]);
  }
  s->rl = s->nrl; s->rr = s->nrr; s->rn = s->nrn;
  float na = s->nose_d0 * s->nose_d0, sum = A[NOSE0] + A[NOSE0 + 1] + na, is = 1.0f / sum;
  s->nrl = (2.0f * A[NOSE0] - sum) * is;
  s->nrr = (2.0f * A[NOSE0 + 1] - sum) * is;
  s->nrn = (2.0f * na - sum) * is;
}

/* the diameters towards their targets (reshapeTract), a transient when a closure opens */
static void reshape(vox_state *s, float dt) {
  float amount = dt * 15.0f;
  int obst = -1;
  for (int i = 0; i < TN; ++i) {
    float d = s->diam[i];
    if (d <= 0.0f) obst = i;
    float slow = i < NOSE0 ? 0.6f : i >= TIP ? 1.0f : 0.6f + 0.4f * (float)(i - NOSE0) / (float)(TIP - NOSE0);
    s->diam[i] = move_towards(d, s->target[i], slow * amount, 2.0f * amount);
  }
  if (s->last_obst > -1 && obst == -1 && s->nose_d0 * s->nose_d0 < 0.05f) {
    int k = 0;
    for (int j = 1; j < NTRANS; ++j) if (s->tr[j].amp < s->tr[k].amp) k = j;
    s->tr[k].amp = 0.3f * s->env;   /* scaled by the envelope: silent after the release */ s->tr[k].pos = (float)s->last_obst; s->tr[k].on = 0.0f;
  }
  s->last_obst = obst;
  s->nose_d0 = move_towards(s->nose_d0, s->velum, amount * 0.25f, amount * 0.1f);
}

/* the LF waveform's shape for a new period (setupWaveform), its recurrences from t */
static void setup_wave(vox_state *s) {
  float Rd = clampf(3.0f * (1.0f - s->tens_now), 0.5f, 2.7f);
  float Ra = -0.01f + 0.048f * Rd, Rk = 0.224f + 0.118f * Rd;
  float Rg = (Rk / 4.0f) * (0.5f + 1.2f * Rk) / (0.11f * Rd - Ra * (0.5f + 1.2f * Rk));
  float Ta = Ra, Tp = 1.0f / (2.0f * Rg), Te = Tp + Tp * Rk;
  float eps = 1.0f / Ta, shift = expf(-eps * (1.0f - Te)), delta = 1.0f - shift;
  float rhs = ((1.0f / eps) * (shift - 1.0f) + (1.0f - Te) * shift) / delta;
  float lower = -(Te - Tp) / 2.0f + rhs, upper = -lower;
  float omega = 3.14159265f / Tp, sn = sinf(omega * Te);
  float y = -3.14159265f * sn * upper / (Tp * 2.0f);
  float alpha = logf(y) / (Tp / 2.0f - Te);
  float e0 = -1.0f / (sn * expf(alpha * Te));
  s->lf_alpha = alpha; s->lf_e0 = e0; s->lf_eps = eps; s->lf_shift = shift; s->lf_idelta = 1.0f / delta;
  s->lf_te = Te; s->lf_omega = omega;
  float t = s->t_wave, dt = s->dt_wave;
  s->ea = expf(alpha * t); s->ka = expf(alpha * dt);
  s->sn = sinf(omega * t); s->cs = cosf(omega * t);
  s->rs = sinf(omega * dt); s->rc = cosf(omega * dt);
  s->kb = expf(-eps * dt);
  s->lf_tail = 0;
  if (!(s->ea == s->ea) || !(e0 == e0)) { s->lf_e0 = 0.0f; s->ea = 0.0f; }   /* never NaN */
}

static void vox_note_on(void *st, float note, float vel) {
  vox_state *s = st;
  s->note = clampf(note, 0.0f, 127.0f);
  vel = clampf(vel, 0.0f, 1.0f);
  s->vel_gain = 1.0f - s->vel_amt + s->vel_amt * vel;
  if (s->stage == ST_OFF || s->env < 1e-3f) {               /* a fresh note: the pitch is there at once */
    s->semi = s->note;
    if (s->stage == ST_OFF) { s->t_wave = 1.0f; s->dt_wave = 0.0f; s->tens_now = s->tens; }   /* a new period next */
  }
  s->stage = ST_ATK;                                        /* the envelope goes on from where it is */
}

static void vox_note_off(void *st) {
  vox_state *s = st;
  if (s->stage == ST_ATK || s->stage == ST_SUS) s->stage = ST_REL;
}

static void silence(vox_state *s) {
  for (int i = 0; i < TN; ++i) s->R[i] = s->L[i] = 0.0f;
  for (int i = 0; i < NN; ++i) s->noseR[i] = s->noseL[i] = 0.0f;
  for (int k = 0; k < NTRANS; ++k) s->tr[k].amp = 0.0f;
  s->a1x = s->a2x = s->a1y = s->a2y = s->f1x = s->f2x = s->f1y = s->f2y = 0.0f;
  s->hp_x = s->hp_y = 0.0f;
}

static void vox_render(void *st, float *out, int32_t n) {
  vox_state *s = st;
  if (s->stage == ST_OFF || n <= 0) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  const float bt = (float)n / g_fs;
  /* per block (upstream's finishBlock, here every block): the tract's shape, the reflections, pitch, tenseness */
  reshape(s, bt);
  reflections(s);
  for (int k = 0; k < 5; ++k) { s->nz[k] += k_nz_rate[k] * bt; if (s->nz[k] >= 256.0f) s->nz[k] -= 256.0f; }
  s->vib_ph += s->vib_hz * bt; if (s->vib_ph >= 1.0f) s->vib_ph -= 1.0f;
  float vib = s->vib_amt * fast_sin_cyc(s->vib_ph)
            + s->wobble * 0.5f * (0.02f * gnoise(s->nz[0], s->seed) + 0.04f * gnoise(s->nz[1], s->seed ^ 0x51u));
  float gk = 1.0f - expf(-bt / s->glide_k);
  s->semi += (s->note - s->semi) * gk;
  float freq = 440.0f * exp2f((s->semi - 69.0f) * (1.0f / 12.0f)) * (1.0f + vib);
  freq = clampf(freq, 20.0f, 1500.0f);
  float tens = s->tens + s->wobble * 0.5f * (0.1f * gnoise(s->nz[2], s->seed ^ 0xA3u) + 0.05f * gnoise(s->nz[3], s->seed ^ 0x3Cu));
  tens = clampf(tens, 0.0f, 1.0f);
  s->loud = sqrtf(sqrtf(clampf(s->tens, 0.0f, 1.0f)));
  float asp_g = (1.0f - sqrtf(clampf(s->tens, 0.0f, 1.0f))) * s->breath * (0.2f + 0.02f * gnoise(s->nz[4], s->seed ^ 0x77u));
  /* the fricative: the constriction smoothed (~8 ms) */
  float fk = 1.0f - expf(-bt / 0.008f);
  s->frc_i += (s->cons_i - s->frc_i) * fk;
  s->frc_d += (s->cons_d - s->frc_d) * fk;
  float frc_g = 0.0f;
  int fi = (int)s->frc_i;
  float fdel = s->frc_i - (float)fi;
  if (fi >= 2 && fi + 2 < TN && s->frc_d > 0.0f) {
    float thin = clampf(8.0f * (0.7f - s->frc_d), 0.0f, 1.0f), open = clampf(30.0f * (s->frc_d - 0.3f), 0.0f, 1.0f);
    frc_g = 0.66f * thin * open;
  }
  /* the reflections' steps across the block's 2n tract steps */
  float drefl[TN];
  const float il = 1.0f / (float)(2 * n);
  for (int i = 1; i < TN; ++i) drefl[i] = (s->nrefl[i] - s->refl[i]) * il;
  float drl = (s->nrl - s->rl) * il, drr = (s->nrr - s->rr) * il, drn = (s->nrn - s->rn) * il;
  float refl[TN];
  for (int i = 1; i < TN; ++i) refl[i] = s->refl[i];
  float rl = s->rl, rr = s->rr, rn = s->rn;
  float *restrict R = s->R, *restrict L = s->L, *restrict nR = s->noseR, *restrict nL = s->noseL;
  float jR[TN + 1], jL[TN + 1], njR[NN + 1], njL[NN + 1];
  float env = s->env, gain = s->level * s->vel_gain * 3.0f, peak = 0.0f;   /* ~0.35 peak a voice at the defaults */
  int32_t stage = s->stage;
  const float hr = 1.0f - 6.2831853f * 20.0f / g_fs;
  for (int32_t j = 0; j < n; ++j) {
    /* the envelope (the glottis's intensity) */
    if (stage == ST_ATK) { env += s->a_rate; if (env >= 1.0f) { env = 1.0f; stage = ST_SUS; } }
    else if (stage == ST_REL) { env *= s->r_k; if (env < 1e-4f) { env = 0.0f; stage = ST_TAIL; s->tail_blocks = 0; } }
    else if (stage == ST_TAIL) env = 0.0f;
    float intensity = env * s->vel_gain;
    /* the glottis: a new period when this one ends */
    s->t_wave += s->dt_wave;
    if (s->t_wave >= 1.0f) {
      s->t_wave -= 1.0f;
      if (s->t_wave >= 1.0f || s->t_wave < 0.0f) s->t_wave = 0.0f;
      s->dt_wave = freq / g_fs;
      s->tens_now = tens;
      setup_wave(s);
    } else {
      if (!s->lf_tail) {
        s->ea *= s->ka;
        float ns = s->sn * s->rc + s->cs * s->rs, nc = s->cs * s->rc - s->sn * s->rs;
        s->sn = ns; s->cs = nc;
      } else s->eb *= s->kb;
    }
    float t = s->t_wave, lf;
    if (t > s->lf_te) {
      if (!s->lf_tail) { s->lf_tail = 1; s->eb = expf(-s->lf_eps * (t - s->lf_te)); }
      lf = (-s->eb + s->lf_shift) * s->lf_idelta;
    } else lf = s->lf_e0 * s->ea * s->sn;
    float glot = lf * intensity * s->loud;
    /* the noise: white through the two bandpasses */
    float wn = rnd_pm(&s->rng) * 0.5f;
    float an = g_bp_b0 * (wn - s->a2x) - g_bp_a1 * s->a1y - g_bp_a2 * s->a2y;
    s->a2x = s->a1x; s->a1x = wn; s->a2y = s->a1y; s->a1y = an;
    float fn = g_bf_b0 * (wn - s->f2x) - g_bf_a1 * s->f1y - g_bf_a2 * s->f2y;
    s->f2x = s->f1x; s->f1x = wn; s->f2y = s->f1y; s->f1y = fn;
    float sw = fast_sin_cyc(t);
    float voiced = 0.1f + 0.2f * (sw > 0.0f ? sw : 0.0f);
    float ti = s->tens * intensity, nmod = ti * voiced + (1.0f - ti) * 0.3f;
    glot += intensity * asp_g * nmod * an;
    float turb = fn * frc_g * nmod * env;
    float n0 = turb * (1.0f - fdel) * 0.5f, n1 = turb * fdel * 0.5f;
    float vo = 0.0f;
    for (int h = 0; h < 2; ++h) {                          /* the tract at twice the rate */
      for (int k = 0; k < NTRANS; ++k) if (s->tr[k].amp > 1e-6f) {
        int pp = (int)s->tr[k].pos;
        float a = s->tr[k].amp * s->tr[k].on * 0.5f;
        R[pp] += a; L[pp] += a;
        s->tr[k].amp *= g_trans_k;
        s->tr[k].on += (1.0f - s->tr[k].on) * g_trans_on;
      }
      if (frc_g > 0.0f) { R[fi + 1] += n0; L[fi + 1] += n0; R[fi + 2] += n1; L[fi + 2] += n1; }
      jR[0] = L[0] * 0.75f + glot;
      jL[TN] = R[TN - 1] * -0.85f;
      for (int i = 1; i < TN; ++i) {
        refl[i] += drefl[i];
        float w = refl[i] * (R[i - 1] + L[i]);
        jR[i] = R[i - 1] - w;
        jL[i] = L[i] + w;
      }
      rl += drl; rr += drr; rn += drn;
      {
        const int i = NOSE0;
        jL[i] = rl * R[i - 1] + (1.0f + rl) * (nL[0] + L[i]);
        jR[i] = rr * L[i] + (1.0f + rr) * (R[i - 1] + nL[0]);
        njR[0] = rn * nL[0] + (1.0f + rn) * (L[i] + R[i - 1]);
      }
      for (int i = 0; i < TN; ++i) { R[i] = jR[i] * 0.999f; L[i] = jL[i + 1] * 0.999f; }
      njL[NN] = nR[NN - 1] * -0.85f;
      for (int i = 1; i < NN; ++i) {
        float w = g_nrefl[i] * (nR[i - 1] + nL[i]);
        njR[i] = nR[i - 1] - w;
        njL[i] = nL[i] + w;
      }
      for (int i = 0; i < NN; ++i) { nR[i] = njR[i]; nL[i] = njL[i + 1]; }
      vo += R[TN - 1] + nR[NN - 1];
    }
    float x = vo * 0.125f;
    float hp = x - s->hp_x + hr * s->hp_y;                  /* a 20 Hz DC blocker */
    s->hp_x = x; s->hp_y = hp;
    float yv = hp * gain;
    if (!(yv > -8.0f && yv < 8.0f)) { yv = 0.0f; silence(s); }   /* never NaN, never a runaway */
    yv = clampf(yv, -1.0f, 1.0f);
    float ay = yv < 0.0f ? -yv : yv;
    if (ay > peak) peak = ay;
    out[j] = yv;
  }
  for (int i = 1; i < TN; ++i) s->refl[i] = s->nrefl[i];
  s->rl = s->nrl; s->rr = s->nrr; s->rn = s->nrn;
  s->env = env;
  s->stage = stage;
  if (stage == ST_TAIL && (peak < 1e-5f || ++s->tail_blocks > (int32_t)(0.5f / bt))) {   /* to exact silence */
    silence(s);
    s->stage = ST_OFF;
  }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(vox_state), VOX_PERMILLE,   /* VOX_PERMILLE: the Makefile's, from tests/vox_check's Mac timing */
  vox_init, vox_voice_init, vox_note_on, vox_note_off, vox_params, vox_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
