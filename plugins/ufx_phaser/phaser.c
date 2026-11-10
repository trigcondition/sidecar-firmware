// plugins/ufx_phaser/phaser.c - PHASER: a stereo phaser for the send FX, a Plugin API 3 user effect (sdk/rmp/rmp_api3.h,
// docs/plugins.md "User effects"). API 3 only, RMP3_KIND_EFFECT on a send slot (UFX1..UFX4 as a send FX TYPE): mono in,
// stereo out. Written for this repo (no code taken from elsewhere).
//   P1 RATE  0..63 FREE: 0.02 .. 8 Hz (exponential); 64..127 SYNC to the Rytm's tempo, one sweep each
//            8BAR 4BAR 2BAR 1BAR 1/2 1/4 1/8 1/16 (in sync it follows the song position while the Rytm plays)
//   P2 DPTH  the sweep's width: 0 = still at 600 Hz, 127 = +-2.5 octaves (about 106 Hz .. 3.4 kHz)
//   P3 FDBK  feedback around the stages, 0 .. 0.9 (sharper, ringing notches), soft-limited
//   P4 SPRD  the stage count and the stereo spread together: 4ST / 6ST / 8ST zones, inside each the right channel's
//            sweep runs 0 .. 180 degrees behind the left's
// The send returns wet only and comes back into the Rytm some milliseconds after the dry track, so the effect cannot
// leave the notches to the Rytm's own dry signal: the return is the whole phaser (the input and the swept copy mixed
// inside, in the proportion that keeps the notches exact at every feedback), so it sounds like a phaser on its own.
// The send level is the mix.
// The DSP: first-order allpass stages y = a x + s, s = x - a y, all eight run on each channel (a stage count change
// crossfades between taps 4, 6 and 8, no click); a = (t - 1) / (t + 1), t = tan(pi fc / fs), worked out at the block's
// start and end and ramped between; the LFO a parabolic sine, the sweep exponential (equal notch movement per octave).
// No libm, no allocation, no host call in render; denormals flushed per block.
// MIT, Copyright 2026 trigcondition and contributors (LICENSE next to this file).
#include "rmp/rmp_api3.h"

#define NST 8

typedef struct {
  float s[2][NST];     /* the allpass states, per channel */
  float fb[2];         /* the feedback taps (one sample behind) */
  float a[2];          /* each channel's coefficient at the end of the last block */
  float w[3];          /* the tap weights 4, 6, 8 stages (glide to the chosen one) */
  float depth, fbk;    /* smoothed P2, P3 */
  float phase;         /* the LFO, 0..1 */
  int primed;          /* the first block sets the smoothed values at once */
  float p[4];          /* P1..P4 as params() gave them (a host without b->p) */
} ph;

static const float k_div_ticks[8] = {768.0f, 384.0f, 192.0f, 96.0f, 48.0f, 24.0f, 12.0f, 6.0f};   /* 24 ppqn */

static int32_t ph_init(const rmp_host *h) { return h->sample_rate == 48000.0f ? 0 : 1; }
static void ph_voice_init(void *st) {
  ph *s = st;
  for (int c = 0; c < 2; ++c) {
    for (int k = 0; k < NST; ++k) s->s[c][k] = 0.0f;
    s->fb[c] = 0.0f, s->a[c] = 0.0f;
  }
  s->w[0] = 0.0f, s->w[1] = 1.0f, s->w[2] = 0.0f;
  s->depth = s->fbk = s->phase = 0.0f;
  s->primed = 0;
  s->p[0] = 28.0f, s->p[1] = 96.0f, s->p[2] = 40.0f, s->p[3] = 64.0f;
}
static void ph_params(void *st, const float *p) {
  ph *s = st;
  for (int k = 0; k < 4; ++k) s->p[k] = p[k];
}
static void ph_attach(void *st, const rmp3_voice *v) {
  (void)v;
  ph_voice_init(st);
}

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float flush(float x) { return x > -1e-18f && x < 1e-18f ? 0.0f : x; }
/* 2^x, x clamped to -30..30 (the integer part as the exponent bits, the fraction by a cubic: 0.01 % off) */
static inline float exp2_(float x) {
  x = clampf(x, -30.0f, 30.0f);
  float fl = (float)(int)x;
  if (fl > x) fl -= 1.0f;
  float f = x - fl;
  float m = 1.0f + f * (0.6951786f + f * (0.2261616f + f * 0.0786598f));
  union { float f; int32_t i; } u = {1.0f};
  u.i += (int32_t)((uint32_t)(int32_t)fl << 23);
  return u.f * m;
}
/* a sine of phase p (0..1) from a triangle: -1..1, the shape a parabolic sine's, 0.1 % off the true one */
static inline float psin(float p) {
  float x = 1.0f - 4.0f * (p < 0.5f ? 0.5f - p : p - 0.5f);   /* -1 at 0, 1 at 1/2 */
  return x * (1.5f - 0.5f * x * x);
}
/* the allpass coefficient for a centre fc (Hz) at 48 kHz: tan by its series (fc under 5 kHz: w under 0.33) */
static inline float ap_coef(float fc) {
  float w = clampf(fc, 20.0f, 5000.0f) * (3.14159265f / 48000.0f), w2 = w * w;
  float t = w * (1.0f + w2 * (0.33333333f + w2 * 0.13333333f));
  return (t - 1.0f) / (t + 1.0f);
}
/* tanh-like soft limit, slope 1 at 0, +-1 at +-3 */
static inline float soft(float x) {
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
static inline float wrap01(float x) { return x - (float)(int)x + (x < 0.0f ? 1.0f : 0.0f); }

static void ph_render(void *st, const rmp3_block *b) {
  ph *s = st;
  const float *p = b->p ? b->p : s->p;
  const rmp3_transport *t = b->transport;
  const int n = b->n;
  float *ol = b->out[0], *or_ = b->out[1];

  /* P1 RATE: free (0..63) or a note value (64..127) */
  int rv = (int)clampf(p[0], 0.0f, 127.0f);
  float hz;
  if (rv < 64) {
    hz = 0.02f * exp2_((float)rv * (8.6438562f / 63.0f));   /* 0.02 .. 8 Hz: 400 = 2^8.64 */
  } else {
    float ticks = k_div_ticks[(rv - 64) >> 3];
    float tick_us = t && t->tick_us > 1000.0f ? t->tick_us : 20833.333f;
    hz = 1e6f / (ticks * tick_us);
    if (t && (t->flags & RMP3_T_LOCKED) && (t->flags & RMP3_T_RUNNING) && t->song_tick >= 0) {
      /* follow the song: the sweep's phase from the song position, eased in (snapped on a start or a jump) */
      float target = wrap01(((float)(t->song_tick % (int32_t)ticks) + t->song_frac) / ticks);
      float e = target - s->phase;
      e -= (float)(int)(e + (e >= 0.0f ? 0.5f : -0.5f));   /* -0.5 .. 0.5 */
      if ((t->flags & (RMP3_T_START | RMP3_T_JUMP)) || e > 0.25f || e < -0.25f) s->phase = target;
      else s->phase = wrap01(s->phase + 0.05f * e);
    }
  }
  const float inc = hz * (1.0f / 48000.0f);

  /* P2, P3, P4: targets, smoothed per block (about 12 ms) */
  const float t_depth = clampf(p[1], 0.0f, 127.0f) * (2.5f / 127.0f);   /* octaves either way */
  const float fv = clampf(p[2], 0.0f, 127.0f) * (1.0f / 127.0f);
  const float t_fb = 0.9f * fv * (2.0f - fv);   /* more of the range near the top */
  int sv = (int)clampf(p[3], 0.0f, 127.0f);
  int zone = (sv * 3) >> 7;                     /* 0..42 4ST, 43..85 6ST, 86..127 8ST */
  int lo = (zone * 128 + 2) / 3;
  float spread = (float)(sv - lo) / (zone == 2 ? 41.0f : 42.0f) * 0.5f;   /* 0 .. half a cycle */
  if (!s->primed) {
    s->depth = t_depth, s->fbk = t_fb;
    for (int k = 0; k < 3; ++k) s->w[k] = k == zone ? 1.0f : 0.0f;
  }
  const float g = 0.02f * (float)n / 12.0f;
  s->depth += (t_depth - s->depth) * g;
  s->fbk += (t_fb - s->fbk) * g;

  /* each channel's coefficient at the block's end (the start is the last block's end), ramped across */
  float ph1 = wrap01(s->phase + inc * (float)n);
  float a0[2], da[2];
  const float fc0 = 600.0f;
  for (int c = 0; c < 2; ++c) {
    float off = c ? spread : 0.0f;
    float a1 = ap_coef(fc0 * exp2_(s->depth * psin(wrap01(ph1 + off))));
    if (!s->primed) s->a[c] = ap_coef(fc0 * exp2_(s->depth * psin(wrap01(s->phase + off))));
    a0[c] = s->a[c];
    da[c] = (a1 - a0[c]) / (float)n;
    s->a[c] = a1;
  }
  s->phase = ph1;
  s->primed = 1;

  /* the tap weights glide (about 3 ms: a stage change crossfades), per sample linear within the block */
  float w0[3], dw[3];
  for (int k = 0; k < 3; ++k) {
    float tw = k == zone ? 1.0f : 0.0f;
    float w1 = s->w[k] + (tw - s->w[k]) * (0.08f * (float)n / 12.0f);
    if (w1 < 1e-4f && tw == 0.0f) w1 = 0.0f;
    w0[k] = s->w[k], dw[k] = (w1 - w0[k]) / (float)n, s->w[k] = w1;
  }
  const float fbk = s->fbk;
  /* the mix that keeps the notches exact at any feedback: with y = A u, u = x + g y, out = (1 - g) / 2 (x + (1 + g) y)
     is 0 where A = -1 and 1 where A = 1; between the peaks it falls as g rises (the average power by 1 - g), so it is
     made up by (1 - g)^-1/4: the peaks reach 1.8 at the top (into the knee), the average loses 5 dB, not 10 */
  const float om = 1.0f - fbk;
  const float mk = 0.5f * om / __builtin_sqrtf(__builtin_sqrtf(om));
  const float dry_k = mk, wet_k = mk * (1.0f + fbk);

  const float *in0 = b->in[0], *in1 = b->in[1];
  const int nch = or_ ? 2 : 1;
  float wet[2][16];
  for (int c = 0; c < 2; ++c) {
    float *st8 = s->s[c];
    float a = a0[c], fbs = s->fb[c];
    float wa = w0[0], wb = w0[1], wc = w0[2];
    for (int i = 0; i < n; ++i) {
      float x = in0 ? in0[i] : 0.0f;
      if (!(x == x)) x = 0.0f;   /* a NaN in: silence, not a stuck filter */
      if (in1) x = 0.5f * (x + in1[i]);
      a += da[c], wa += dw[0], wb += dw[1], wc += dw[2];
      float u = x + fbk * fbs, y4, y6, y;
      y = a * u + st8[0], st8[0] = u - a * y, u = y;
      y = a * u + st8[1], st8[1] = u - a * y, u = y;
      y = a * u + st8[2], st8[2] = u - a * y, u = y;
      y = a * u + st8[3], st8[3] = u - a * y, u = y;
      y4 = u;
      y = a * u + st8[4], st8[4] = u - a * y, u = y;
      y = a * u + st8[5], st8[5] = u - a * y, u = y;
      y6 = u;
      y = a * u + st8[6], st8[6] = u - a * y, u = y;
      y = a * u + st8[7], st8[7] = u - a * y, u = y;
      float ap = wa * y4 + wb * y6 + wc * u;
      fbs = soft(ap);
      wet[c][i] = knee(dry_k * x + wet_k * ap);
    }
    for (int k = 0; k < NST; ++k) st8[k] = flush(st8[k]);
    s->fb[c] = flush(fbs);
    if (nch == 1) break;   /* a mono route: the left channel only */
  }
  if (or_) {
    for (int i = 0; i < n; ++i) ol[i] = wet[0][i], or_[i] = wet[1][i];
  } else {
    for (int i = 0; i < n; ++i) ol[i] = wet[0][i];
  }
}

static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_EFFECT, .places = RMP3_AT_SEND,
  .needs = RMP3_CAP_EFFECT,
  .wants = RMP3_CAP_TRANSPORT | RMP3_CAP_STEREO | RMP3_CAP_BLOCK_P,
  .in_channels = 1, .out_channels = 2,
  .voice_attach = ph_attach, .render = ph_render,
};

RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC,
  .api = 3u,                     /* API 3 only */
  .state_bytes = sizeof(ph), .cpu_permille = 10,
  .init = ph_init, .voice_init = ph_voice_init, .params = ph_params,
  .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
