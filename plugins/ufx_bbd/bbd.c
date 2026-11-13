// plugins/ufx_bbd/bbd.c - BBD: a bucket-brigade style analog delay for the send FX, a Plugin API 3 user effect
// (sdk/rmp/rmp_api3.h, docs/plugins.md "User effects"). API 3 only, RMP3_KIND_EFFECT on a send slot (UFX1..UFX4 as a
// send FX TYPE): mono in, stereo wet out. Written for this repo (no code taken from elsewhere): a model of how such a
// pedal behaves, not of any one circuit.
//   P1 TIME  0..63 FREE: 20 ms .. 1 s (exponential); 64..127 SYNC to the Rytm's tempo:
//            1/16 1/8T 1/8 1/4T 1/8D 1/4 1/4D 1/2 (up to 2.4 s with the arena the host grants)
//   P2 FDBK  the repeats, 0 .. 1.08: from about 120 (W/F low) the loop runs away, held at about -10 dBFS by the soft clips
//   P3 W/F   wow and flutter: the clock wobbles (0.55 + 0.83 Hz wow, 6.5 Hz flutter), the two outputs a quarter
//            cycle apart, so a wobble also widens the echoes
//   P4 TONE  a low-pass on the repeats inside the loop, 400 Hz .. open
// What makes it a BBD and not a clean digital delay:
//   - the clock: a longer time is a slower clock, so the line's anti-alias and reconstruction filters close (about
//     10 kHz at 40 ms, 4 kHz at 200 ms, 1.5 kHz at 1 s) and its hiss rises (in the line about -82 dBFS at 40 ms, 3 dB
//     more a doubling of the time); a time change glides the clock: the echoes bend in pitch
//   - the compander: 2:1 into the line, 1:2 out of it (as the NE570 in such pedals): the hiss is gated under quiet
//     echoes and breathes with loud ones. The expander is the compressor's exact inverse (its gain travels down the
//     line beside each sample): an expander that follows its own input sees the compressor's attack overshoot and
//     holds it for its release, so each repeat comes back louder than it went in and a feedback loop grows
//   - the line's own soft clip, which also holds a runaway feedback in bounds
// The delay line lives in the slot arena (1.1 s at least, 2.5 s wanted; two floats a frame: the sample, the gain). No libm, no allocation, no host call in
// render; denormals flushed per block.
// MIT, Copyright 2026 trigcondition and contributors (LICENSE next to this file).
#include "rmp/rmp_api3.h"

#define REF 0.25f        /* the compander's unity level */
#define ENV_FLOOR 1e-3f  /* the compressor's most gain: sqrt(REF / 1e-3), 24 dB */
#define MOD_MARGIN 300u  /* frames kept free past the longest delay: the wobble (264 at most) and the interpolation */

typedef struct { float s1, s2; } svf;   /* a 2-pole low-pass (TPT state variable), Q 0.707 */

typedef struct {
  float *buf;          /* the arena: the line, frames x 2 floats (the compressed sample, the expander's gain) */
  uint32_t frames, w;
  float d;             /* the delay now, frames (glides to the target: the clock's slew) */
  float fc;            /* the clock filters' cutoff now, Hz */
  svf aa;              /* anti-alias, into the line */
  svf rc[2];           /* reconstruction, each output tap */
  float tone[2];       /* P4's one-pole per tap */
  float hp;            /* the loop's DC / rumble blocker (one-pole high-pass state) */
  float envx, smx;     /* the compressor's follower and its smoothing (the rectifier's capacitor) */
  float fbk;           /* smoothed P2 */
  float wf;            /* smoothed P3 (0..1, squared) */
  float wow1, wow2, flut;   /* the wobble's phases, 0..1 */
  float mod[2];        /* each tap's wobble at the end of the last block, frames */
  float tc;            /* smoothed P4 coefficient */
  uint32_t rng;
  int primed;
  float p[4];          /* P1..P4 as params() gave them (a host without b->p) */
} bbd;

static const float k_ticks[8] = {6.0f, 8.0f, 12.0f, 16.0f, 18.0f, 24.0f, 36.0f, 48.0f};   /* 24 ppqn */

static int32_t bb_init(const rmp_host *h) { return h->sample_rate == 48000.0f ? 0 : 1; }
static void bb_reset(bbd *s) {
  s->w = 0, s->d = 0.0f, s->fc = 10000.0f;
  s->aa.s1 = s->aa.s2 = 0.0f;
  for (int c = 0; c < 2; ++c) {
    s->rc[c].s1 = s->rc[c].s2 = 0.0f;
    s->tone[c] = 0.0f, s->mod[c] = 0.0f;
  }
  s->hp = 0.0f, s->envx = 0.0f, s->smx = 0.0f;
  s->fbk = s->wf = 0.0f, s->tc = 1.0f;
  s->wow1 = 0.0f, s->wow2 = 0.37f, s->flut = 0.11f;
  s->primed = 0;
}
static void bb_voice_init(void *st) {
  bbd *s = st;
  s->buf = 0, s->frames = 0, s->rng = 0x9E3779B9u;
  bb_reset(s);
  s->p[0] = 40.0f, s->p[1] = 56.0f, s->p[2] = 24.0f, s->p[3] = 80.0f;
}
static void bb_params(void *st, const float *p) {
  bbd *s = st;
  for (int k = 0; k < 4; ++k) s->p[k] = p[k];
}
static void bb_attach(void *st, const rmp3_voice *v) {
  bbd *s = st;
  s->buf = (float *)v->arena;
  s->frames = v->arena ? v->arena_bytes / (2u * sizeof(float)) : 0u;
  s->rng = 0x9E3779B9u ^ (uint32_t)(v->instance + 2) * 0x85EBCA6Bu;
  bb_reset(s);   /* (the line: the host zeroes an effect's arena when the slot loads; a zero gain reads as silence) */
}

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float flush(float x) { return x > -1e-18f && x < 1e-18f ? 0.0f : x; }
static inline float exp2_(float x) {   /* 2^x, the integer part as the exponent bits, the fraction by a cubic */
  x = clampf(x, -30.0f, 30.0f);
  float fl = (float)(int)x;
  if (fl > x) fl -= 1.0f;
  float f = x - fl;
  float m = 1.0f + f * (0.6951786f + f * (0.2261616f + f * 0.0786598f));
  union { float f; int32_t i; } u = {1.0f};
  u.i += (int32_t)((uint32_t)(int32_t)fl << 23);
  return u.f * m;
}
static inline float log2_(float x) {   /* log2 for x > 0: the exponent bits and a quartic on the mantissa (1e-4) */
  union { float f; int32_t i; } u = {x};
  float e = (float)((u.i >> 23) & 255) - 127.0f;
  u.i = (u.i & 0x007FFFFF) | 0x3F800000;   /* 1..2 */
  float m = u.f;
  return e - 1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m;
}
static inline float psin(float p) {    /* a sine of phase p (0..1), parabolic */
  float x = 1.0f - 4.0f * (p < 0.5f ? 0.5f - p : p - 0.5f);
  return x * (1.5f - 0.5f * x * x);
}
static inline float wrap01(float x) { return x - (float)(int)x; }   /* x >= 0 */
static inline float tan_(float w) {    /* tan by a Pade form, 0.1 % off up to 0.95 rad */
  float w2 = w * w;
  return w * (15.0f - w2) / (15.0f - 6.0f * w2);
}
static inline float soft(float x) {    /* tanh-like soft clip: slope 1 at 0, +-1 at +-3 */
  x = clampf(x, -3.0f, 3.0f);
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
/* the return's ceiling: unchanged up to 0.7, then a soft knee to 1 (reached at 1.6): the host clips at 1 */
static inline float knee(float x) {
  float a = x < 0.0f ? -x : x;
  if (a <= 0.7f) return x;
  float y = 0.7f + 0.3f * soft((a - 0.7f) * (1.0f / 0.3f));
  return x < 0.0f ? -y : y;
}
typedef struct { float a1, a2, a3; } svf_k;
static inline svf_k svf_coef(float fc) {
  float g = tan_(clampf(fc, 50.0f, 15000.0f) * (3.14159265f / 48000.0f));
  svf_k k;
  k.a1 = 1.0f / (1.0f + g * (g + 1.41421356f));
  k.a2 = g * k.a1, k.a3 = g * k.a2;
  return k;
}
static inline float svf_lp(svf *f, const svf_k *k, float v0) {
  float v3 = v0 - f->s2;
  float v1 = k->a1 * f->s1 + k->a2 * v3;
  float v2 = f->s2 + k->a2 * f->s1 + k->a3 * v3;
  f->s1 = 2.0f * v1 - f->s1;
  f->s2 = 2.0f * v2 - f->s2;
  return v2;
}
static void bb_render(void *st, const rmp3_block *b) {
  bbd *s = st;
  const float *p = b->p ? b->p : s->p;
  const int n = b->n;
  float *ol = b->out[0], *or_ = b->out[1];
  if (!s->buf || s->frames < 48000u) {
    for (int i = 0; i < n; ++i) ol[i] = 0.0f;
    if (or_) for (int i = 0; i < n; ++i) or_[i] = 0.0f;
    return;
  }
  const float maxd = (float)(s->frames - MOD_MARGIN);

  /* P1 TIME: free 20 ms .. 1 s, or a note value at the Rytm's tempo */
  int tv = (int)clampf(p[0], 0.0f, 127.0f);
  float target;
  if (tv < 64) {
    target = 960.0f * exp2_((float)tv * (5.6438562f / 63.0f));   /* 20 ms x 50^(v/63) = 2^5.64 */
  } else {
    const rmp3_transport *t = b->transport;
    float tick_us = t && t->tick_us > 1000.0f ? t->tick_us : 20833.333f;
    target = k_ticks[(tv - 64) >> 3] * tick_us * 0.048f;
  }
  target = clampf(target, 96.0f, maxd);
  if (!s->primed) s->d = target;

  /* P2..P4, smoothed per block (about 12 ms) */
  const float g = 0.02f * (float)n / 12.0f;
  const float t_fb = 1.08f * clampf(p[1], 0.0f, 127.0f) * (1.0f / 127.0f);
  const float wv = clampf(p[2], 0.0f, 127.0f) * (1.0f / 127.0f);
  const float t_wf = wv * wv;
  const float t_fct = 400.0f * exp2_(clampf(p[3], 0.0f, 127.0f) * (5.6f / 127.0f));   /* 400 Hz .. 19 kHz */
  const float t_tc = t_fct >= 18000.0f ? 1.0f : 1.0f - exp2_(-t_fct * (6.2831853f / 48000.0f) * 1.442695f);
  if (!s->primed) s->fbk = t_fb, s->wf = t_wf, s->tc = t_tc;
  s->fbk += (t_fb - s->fbk) * g;
  s->wf += (t_wf - s->wf) * g;
  s->tc += (t_tc - s->tc) * g;

  /* the clock filters follow the delay (the block's start): 10 kHz x (40 ms / d)^0.6, 700 Hz .. 14 kHz */
  float fc_t = clampf(10000.0f * exp2_(-0.6f * log2_(s->d * (1.0f / 1920.0f))), 700.0f, 14000.0f);
  s->fc = s->primed ? s->fc + (fc_t - s->fc) * 0.5f : fc_t;
  const svf_k kf = svf_coef(s->fc);
  /* the hiss into the compressed line: uniform, -82 dBFS RMS at 40 ms, 3 dB more a doubling of the time */
  const float hiss = 1.3e-4f * exp2_(0.5f * log2_(s->d * (1.0f / 1920.0f))) * (1.0f / 2147483648.0f);

  /* the wobble: each tap's offset at the block's end (the start is the last block's end), ramped across */
  const float wow_amp = s->wf * 120.0f, flut_amp = s->wf * 12.0f;   /* 2.5 ms and 0.25 ms at the most */
  s->wow1 = wrap01(s->wow1 + 0.55f * (float)n / 48000.0f);
  s->wow2 = wrap01(s->wow2 + 0.83f * (float)n / 48000.0f);
  s->flut = wrap01(s->flut + 6.5f * (float)n / 48000.0f);
  float m1[2], dm[2];
  for (int c = 0; c < 2; ++c) {
    float q = c ? 0.25f : 0.0f;
    float w = 0.7f * psin(wrap01(s->wow1 + q)) + 0.3f * psin(wrap01(s->wow2 + q));
    m1[c] = wow_amp * (1.0f + w) + flut_amp * (1.0f + psin(wrap01(s->flut + q)));   /* >= 0: never past the write */
    if (!s->primed) s->mod[c] = m1[c];
    dm[c] = (m1[c] - s->mod[c]) / (float)n;
  }

  /* the loop, on locals (the line could alias the state for the compiler) */
  const float fbk = s->fbk, tc = s->tc;
  const float *in0 = b->in[0], *in1 = b->in[1];
  float *const buf = s->buf;
  const uint32_t frames = s->frames;
  uint32_t w = s->w;
  float yl[16], yr[16];
  float mod0 = s->mod[0], mod1 = s->mod[1], d = s->d;
  svf aa = s->aa, rc0 = s->rc[0], rc1 = s->rc[1];
  float tone0 = s->tone[0], tone1 = s->tone[1], hp = s->hp;
  float envx = s->envx, smx = s->smx;
  uint32_t rng = s->rng;
  /* the compressor's follower: attack ~0.5 ms (drums keep their front), release ~30 ms, then smoothed (~0.7 ms); its
     gain 2:1, sqrt(REF / env); the line keeps the inverse, sqrt(env / REF), for the expander at the read */
  const float att = 0.04f, rel = 0.0007f, sm = 0.03f, hpk = 0.0078f;   /* hpk: the loop's high-pass, about 60 Hz */
  for (int i = 0; i < n; ++i) {
    /* the clock's slew: a time change glides (at most one frame a frame: up to an octave's bend) */
    d += clampf((target - d) * 0.0003f, -1.0f, 1.0f);
    mod0 += dm[0], mod1 += dm[1];
    /* read both taps (linear between frames): reconstruction filter, expander, tone */
    float r[2], ig[2];
    for (int c = 0; c < 2; ++c) {
      float pos = (float)w - d - (c ? mod1 : mod0);
      if (pos < 0.0f) pos += (float)frames;
      uint32_t k = (uint32_t)pos;
      float f = pos - (float)k;
      if (k >= frames) k -= frames;
      uint32_t j = k + 1u < frames ? k + 1u : 0u;
      r[c] = buf[2u * k] + f * (buf[2u * j] - buf[2u * k]);
      ig[c] = buf[2u * k + 1u] + f * (buf[2u * j + 1u] - buf[2u * k + 1u]);
    }
    float e0 = svf_lp(&rc0, &kf, r[0]) * ig[0], e1 = svf_lp(&rc1, &kf, r[1]) * ig[1];
    tone0 += tc * (e0 - tone0);
    tone1 += tc * (e1 - tone1);
    yl[i] = tone0, yr[i] = tone1;
    /* the loop: the repeats (both taps), DC blocked, limited; plus the input */
    float x = in0 ? in0[i] : 0.0f;
    if (!(x == x)) x = 0.0f;   /* a NaN in: silence, not a stuck line */
    if (in1) x = 0.5f * (x + in1[i]);
    float fbs = 0.5f * (tone0 + tone1);
    hp += hpk * (fbs - hp);
    float u = x + fbk * soft(fbs - hp);
    /* into the line: compressor, the line's soft clip, hiss, the anti-alias filter */
    float au = u < 0.0f ? -u : u;
    envx += (au - envx) * (au > envx ? att : rel);
    smx += (envx - smx) * sm;
    float ige = __builtin_sqrtf((smx > ENV_FLOOR ? smx : ENV_FLOOR) * (1.0f / REF));
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    float c = soft(u / ige * 0.8f) * 1.25f + (float)(int32_t)rng * hiss;
    buf[2u * w] = svf_lp(&aa, &kf, c);
    buf[2u * w + 1u] = ige;
    if (++w == frames) w = 0;
  }
  s->w = w, s->d = d, s->mod[0] = m1[0], s->mod[1] = m1[1], s->rng = rng;
  s->envx = flush(envx), s->smx = flush(smx);
  s->aa.s1 = flush(aa.s1), s->aa.s2 = flush(aa.s2);
  s->rc[0].s1 = flush(rc0.s1), s->rc[0].s2 = flush(rc0.s2), s->rc[1].s1 = flush(rc1.s1), s->rc[1].s2 = flush(rc1.s2);
  s->tone[0] = flush(tone0), s->tone[1] = flush(tone1), s->hp = flush(hp);
  s->primed = 1;
  /* the return under the host's clip at 1 (a transient past the compander's attack, a runaway loop) */
  for (int i = 0; i < n; ++i) yl[i] = knee(yl[i]), yr[i] = knee(yr[i]);
  if (or_) {
    for (int i = 0; i < n; ++i) ol[i] = yl[i], or_[i] = yr[i];
  } else {
    for (int i = 0; i < n; ++i) ol[i] = 0.5f * (yl[i] + yr[i]);
  }
}

static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_EFFECT, .places = RMP3_AT_SEND,
  .needs = RMP3_CAP_ARENA | RMP3_CAP_EFFECT,
  .wants = RMP3_CAP_TRANSPORT | RMP3_CAP_STEREO | RMP3_CAP_BLOCK_P,
  .voice_min = 2u * 52800u * sizeof(float),    /* 1.1 s */
  .voice_want = 2u * 120000u * sizeof(float),   /* 2.5 s: a 1/2 note at 60 BPM */
  .in_channels = 1, .out_channels = 2,
  .voice_attach = bb_attach, .render = bb_render,
};

RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC,
  .api = 3u,                     /* API 3 only */
  .state_bytes = sizeof(bbd), .cpu_permille = 20,
  .init = bb_init, .voice_init = bb_voice_init, .params = bb_params,
  .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
