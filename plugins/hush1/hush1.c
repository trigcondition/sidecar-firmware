// plugins/hush1/hush1.c - HUSH1: an SH-101-style monophonic subtractive synth, one voice per state (a mono track
// is the SH-101: one state, glide between its notes; a poly track plays chords on instances: manifest max_voices 8).
// SPDX-License-Identifier: MIT
// The DSP is HUSH ONE's (Charles Vestal, MIT: LICENSE.hush1; NOTICE has the source and commit): the oscillator
// mixer (saw, pulse with its 1 / -0.95 asymmetry, a sub square an octave down with 0.94 / -1, coloured noise, the
// mix x 0.42 into a tanh(1.4 x) "pushed mixer"), the 4-pole lowpass (tanh(1.5 x) input drive 1.3, the input gain
// 1 - 0.22 res, feedback k res (y4 - 0.15 y3), a cubic soft clip x - 0.06 x^3 after stages 1..3), the cutoff law
// (30 Hz + c^2 x 15 kHz, key follow 2^(octaves from C4 x TRK), the envelope added to the normalised cutoff), the
// ADSR's RC segments, exponential glide, the slow random pitch drift (+-0.1 st), the VCA's ENV / GATE switch and
// the DC-blocking coupling capacitor after the filter.
// Adapted for the Sidecar (Copyright 2026 trigcondition and contributors, MIT): one note a state, 48 kHz; PolyBLEP
// on the saw, pulse and sub edges (upstream's are naive); the filter's one-poles use the prewarped G = g / (1 + g)
// (upstream's Euler g, capped at 0.35, needed a dry bypass above cutoff 0.85, a synthetic self-oscillator and a
// resonance make-up: all three dropped, this filter opens to 18 kHz and self-oscillates on its own); one ADSR for
// the VCF and the VCA, as on the SH-101 (upstream has two); the attack aims at 1.3 and turns at 1.0 (analogue-style,
// upstream's RC never quite reaches its target); no LFO, velocity, note stack, presets (the Rytm's own LFO, trigs
// and sound pool do those); the cutoff, glide, drift and smoothing per 12-sample block (no powf / tanf / expf in the
// sample loop); a ~1 ms ramp on the GATE VCA (upstream switches it hard); a retrigger of a sounding voice keeps its
// phases and filter and the envelope rises from where it is (no click); no allocation, everything render() writes in
// the state; a NaN guard and the output clamped to +-1.
//   P1 SAW  P2 PULS  P3 PW (50 % .. 5 %)  P4 SUB  P5 NOIS  P6 CUT  P7 RES  P8 ENV (filter env amount)
//   P9 ATK  P10 DEC  P11 SUS  P12 REL  P13 GLID  P14 TRK (keyboard tracking)  P15 LEV  P16 VCA (ENV / GATE)
#include <math.h>
#include "rmp/rmp_api.h"

typedef struct {
  /* parameters (scaled) */
  float saw, pulse, pw, sub, noise, cut, res, env_amt, a_k, d_k, sus, r_k, glide_s, trk, level;
  int32_t vca_gate;
  /* smoothed (per block, ~5 ms) */
  float sm_saw, sm_pulse, sm_pw, sm_sub, sm_noise, sm_cut, sm_res, sm_level;
  /* the note: target and current pitch (semitones), drift */
  float target, pitch, drift, drift_target;
  int32_t has_note;
  /* oscillators, noise colour, filter, DC blocker */
  float ph, sub_ph, noise_lp, y1, y2, y3, y4, dc;
  /* the envelope (0 off, 1 attack, 2 decay, 3 sustain, 4 release), the GATE VCA's ramp */
  float env, gate_amp;
  int32_t stage, gate;
  uint32_t rng;
} hush_state;

enum { ENV_OFF, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };

static float g_fs = 48000.0f;
static float g_sk12 = 0.0f;     /* the knob smoothing per 12-sample block */
static float g_gate_k = 0.0f;   /* the GATE VCA's ramp per sample (~0.7 ms) */

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float tanh_r(float x) {   /* a tanh-like cubic: slope 1 at 0, 1 with slope 0 at +-1.5 (no division) */
  x = clampf(x, -1.5f, 1.5f);
  return x - x * x * x * (1.0f / 6.75f);
}
static inline float tan_pw(float x) {   /* tan(x) for 0..1.4 (filter prewarp), Pade, rel err < 3e-5 */
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}
static inline float poly_blep(float t, float dt, float idt) {   /* a unit step's residual: -1..0 after, 0..1 before */
  if (t < dt) { t *= idt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) * idt; return t * t + t + t + 1.0f; }
  return 0.0f;
}
static inline uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float rnd_pm(uint32_t *s) { return (float)(int32_t)xs32(s) * (1.0f / 2147483648.0f); }   /* -1..1 */

/* 0..1 -> 1 ms .. 10 s, exponential (the SH-101's envelope ranges, roughly) */
static inline float p_secs(float p) { return 0.001f * expf(9.2103404f * p); }
/* an RC segment's multiplier: to e^-4.6 (-40 dB) in t */
static inline float rc_k(float secs) { return expf(-4.6f / (secs * g_fs)); }

static const float k_defaults[RMP_PARAMS] = {100, 0, 40, 72, 0, 34, 44, 84, 0, 46, 24, 22, 0, 64, 100, 0};

static int32_t hush_init(const rmp_host *h) {
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  g_sk12 = 1.0f - expf(-12.0f / (0.005f * g_fs));
  g_gate_k = 1.0f - expf(-1.0f / (0.0007f * g_fs));
  return 0;
}

static void hush_params(void *st, const float *p) {
  hush_state *s = st;
  s->saw = n01(p[0]);
  s->pulse = n01(p[1]);
  s->pw = 0.5f - 0.45f * n01(p[2]);                         /* 50 % (square) .. 5 % */
  s->sub = n01(p[3]);
  s->noise = n01(p[4]);
  s->cut = n01(p[5]);
  s->res = 1.06f * n01(p[6]);                               /* self-oscillates from ~110 */
  s->env_amt = n01(p[7]);
  /* the attack aims at 1.3 and turns at 1.0: 0 -> 1 in t takes t / ln(1.3 / 0.3) a time constant */
  s->a_k = 1.0f - expf(-1.4663371f / (p_secs(n01(p[8])) * g_fs));
  s->d_k = rc_k(p_secs(n01(p[9])));
  s->sus = n01(p[10]);
  s->r_k = rc_k(p_secs(n01(p[11])));
  float gl = n01(p[12]);
  s->glide_s = gl * gl * 1.2f;                              /* 0 (off) .. 1.2 s a time constant */
  s->trk = n01(p[13]);
  float lv = n01(p[14]);
  s->level = lv * lv * 1.6f;                                /* upstream's 0.8, x 2 (this filter's lower output) */
  s->vca_gate = ((int32_t)clampf(p[15], 0.0f, 127.0f) * 2) >> 7;
}

static void hush_voice_init(void *st) {
  hush_state *s = st;
  s->rng = 0x31415926u ^ (uint32_t)(uintptr_t)st;          /* each instance its own noise and drift */
  if (!s->rng) s->rng = 1u;
  hush_params(s, k_defaults);
  s->sm_saw = s->saw; s->sm_pulse = s->pulse; s->sm_pw = s->pw; s->sm_sub = s->sub; s->sm_noise = s->noise;
  s->sm_cut = s->cut; s->sm_res = s->res; s->sm_level = s->level;
  s->target = s->pitch = 60.0f;
}

static void hush_note_on(void *st, float note, float vel) {
  hush_state *s = st;
  (void)vel;                                                /* the SH-101 has no velocity */
  note = clampf(note, 0.0f, 127.0f);
  int32_t sounding = s->stage != ENV_OFF;
  if (!sounding) {                                          /* a fresh note: a clean filter, phases from 0 */
    s->ph = s->sub_ph = 0.0f;
    s->y1 = s->y2 = s->y3 = s->y4 = s->dc = s->noise_lp = 0.0f;
    s->env = 0.0f; s->gate_amp = 0.0f;
  }
  s->target = note;
  if (!s->has_note || s->glide_s <= 0.0f) s->pitch = note;  /* portamento ON: glide from the last note */
  s->has_note = 1;
  s->stage = ENV_ATK;                                       /* the envelope rises from where it is */
  s->gate = 1;
}

static void hush_note_off(void *st) {
  hush_state *s = st;
  s->gate = 0;
  if (s->stage != ENV_OFF) s->stage = ENV_REL;
}

static void hush_render(void *st, float *out, int32_t n) {
  hush_state *s = st;
  if (s->stage == ENV_OFF) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  /* per block: smoothing, glide, drift, pitch, cutoff */
  float sk = n == 12 ? g_sk12 : 1.0f - expf(-(float)n / (0.005f * g_fs));
  s->sm_saw += (s->saw - s->sm_saw) * sk;
  s->sm_pulse += (s->pulse - s->sm_pulse) * sk;
  s->sm_pw += (s->pw - s->sm_pw) * sk;
  s->sm_sub += (s->sub - s->sm_sub) * sk;
  s->sm_noise += (s->noise - s->sm_noise) * sk;
  s->sm_cut += (s->cut - s->sm_cut) * sk;
  s->sm_res += (s->res - s->sm_res) * sk;
  s->sm_level += (s->level - s->sm_level) * sk;
  if (s->glide_s > 0.0f) s->pitch += (s->target - s->pitch) * (1.0f - expf(-(float)n / (s->glide_s * g_fs)));
  else s->pitch = s->target;
  s->drift_target = clampf(s->drift_target + rnd_pm(&s->rng) * 0.0005f * (float)n, -0.10f, 0.10f);
  s->drift += (s->drift_target - s->drift) * 0.0012f * (float)n;
  float f0 = 440.0f * expf((s->pitch + s->drift - 69.0f) * 0.05776227f);
  float inc = clampf(f0 / g_fs, 1e-6f, 0.45f), idt = 1.0f / inc;
  float sinc = 0.5f * inc, sidt = 2.0f * idt;
  /* the cutoff: upstream's law, the envelope at the block's start */
  float c = clampf(s->sm_cut + s->env_amt * s->env, 0.0f, 1.0f);
  float fc = (30.0f + c * c * 15000.0f) * expf((s->pitch - 60.0f) * 0.05776227f * s->trk);
  fc = clampf(fc, 20.0f, 18000.0f);
  float g = tan_pw(3.14159265f * fc / g_fs);
  float G = g / (1.0f + g);
  float res = s->sm_res;
  float kfb = 4.0f * res, in_gain = 1.0f - 0.22f * res;
  float makeup = 1.0f + 0.7f * kfb;                         /* the ladder's passband loss, 1 / (1 + k), mostly back */
  float ga = s->sm_saw * 0.42f, gp = s->sm_pulse * 0.42f, gsub = s->sm_sub * 0.42f, gn = s->sm_noise * 0.42f;
  float pw = s->sm_pw, gain = s->sm_level * makeup;
  float ph = s->ph, sph = s->sub_ph, nlp = s->noise_lp, y1 = s->y1, y2 = s->y2, y3 = s->y3, y4 = s->y4, dc = s->dc;
  float env = s->env, gamp = s->gate_amp;
  int32_t stage = s->stage, gate = s->gate, vgate = s->vca_gate;
  float a_k = s->a_k, d_k = s->d_k, sus = s->sus, r_k = s->r_k;
  uint32_t rng = s->rng;
  for (int32_t i = 0; i < n; ++i) {
    /* oscillators */
    ph += inc; if (ph >= 1.0f) ph -= 1.0f;
    sph += sinc; if (sph >= 1.0f) sph -= 1.0f;
    float saw = 2.0f * ph - 1.0f - poly_blep(ph, inc, idt);
    float pt = ph - pw; if (pt < 0.0f) pt += 1.0f;
    float pulse = (ph < pw ? 1.0f : -0.95f) + 0.975f * (poly_blep(ph, inc, idt) - poly_blep(pt, inc, idt));
    float st2 = sph + 0.5f; if (st2 >= 1.0f) st2 -= 1.0f;
    float sub = (sph < 0.5f ? 0.94f : -1.0f) + 0.97f * (poly_blep(sph, sinc, sidt) - poly_blep(st2, sinc, sidt));
    float white = rnd_pm(&rng);
    nlp += 0.085f * (white - nlp);
    float noise = 0.72f * nlp + 0.28f * white;
    float x = tanh_r(1.4f * (ga * saw + gp * pulse + gsub * sub + gn * noise));
    /* the 4-pole lowpass */
    float u = tanh_r(1.3f * 1.5f * (x * in_gain - kfb * (y4 - 0.15f * y3)));
    y1 += G * (u - y1);  y1 -= 0.06f * y1 * y1 * y1;
    y2 += G * (y1 - y2); y2 -= 0.06f * y2 * y2 * y2;
    y3 += G * (y2 - y3); y3 -= 0.06f * y3 * y3 * y3;
    y4 += G * (y3 - y4);
    dc += (y4 - dc) * 0.00005f;                            /* the coupling capacitor, ~0.4 Hz */
    float y = y4 - dc;
    /* the envelope */
    switch (stage) {
      case ENV_ATK: env += (1.3f - env) * a_k; if (env >= 1.0f) { env = 1.0f; stage = ENV_DEC; } break;
      case ENV_DEC: env = sus + (env - sus) * d_k; if (env <= sus + 1e-4f) { env = sus; stage = ENV_SUS; } break;
      case ENV_SUS: env = sus; break;
      case ENV_REL: env *= r_k; if (env < 1e-4f) { env = 0.0f; if (!vgate) stage = ENV_OFF; } break;   /* GATE: its ramp ends it */
      default: env = 0.0f; break;
    }
    float vca;
    if (vgate) {
      gamp += ((float)gate - gamp) * g_gate_k;
      if (!gate && gamp < 1e-4f) { gamp = 0.0f; stage = ENV_OFF; }
      vca = gamp;
    } else {
      vca = env;
    }
    float o = y * vca * gain;
    if (!(o > -4.0f && o < 4.0f)) { o = 0.0f; y1 = y2 = y3 = y4 = dc = nlp = 0.0f; }   /* never NaN */
    out[i] = clampf(o, -1.0f, 1.0f);
    if (stage == ENV_OFF) { for (++i; i < n; ++i) out[i] = 0.0f; break; }
  }
  s->ph = ph; s->sub_ph = sph; s->noise_lp = nlp; s->y1 = y1; s->y2 = y2; s->y3 = y3; s->y4 = y4; s->dc = dc;
  s->env = env; s->gate_amp = gamp; s->stage = stage; s->rng = rng;
  if (stage == ENV_OFF) { s->env = 0.0f; s->gate_amp = 0.0f; }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(hush_state), HUSH_PERMILLE,   /* HUSH_PERMILLE: the Makefile's, from tests/hush_check's Mac timing */
  hush_init, hush_voice_init, hush_note_on, hush_note_off, hush_params, hush_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
