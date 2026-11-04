// plugins/ufx_pingpong/pingpong.c - PINGPONG: a tempo-synced stereo ping-pong delay, the example Plugin API 3 user
// effect (sdk/rmp/rmp_api3.h; docs/plugin-api-3-plan.md). API 3 only (an effect: an older host must not load it as a
// machine), RMP3_KIND_EFFECT on a send slot (UFX1..UFX4 as a send FX TYPE): mono in, stereo wet out.
//   P1 TIME   the echo's time in the Rytm's tempo (the block's transport): 1/16, 1/8T, 1/8, 1/4T, 1/8D, 1/4, 1/2, 1 BAR
//   P2 FDBK   feedback 0..0.9
//   P3 TONE   a low-pass in the feedback path, dark .. open
//   P4 WIDTH  the echoes' stereo width, mono .. full ping-pong
// What it shows of API 3: the slot arena (the delay lines: up to 4 s a side, sized from what the host granted, so the
// same package uses more on a bigger board), the transport (TIME follows the tempo; a time change glides), the knobs
// from the block (b->p), stereo out with a mono route's mix, no allocation, no host call in render.
// MIT, Copyright 2026 trigcondition and contributors (LICENSE next to this file).
#include "rmp/rmp_api3.h"

typedef struct {
  float *buf;          /* the arena part: frames x 2 (L, R interleaved) */
  uint32_t frames;     /* the delay line's length a side */
  uint32_t w;          /* the write position */
  float d;             /* the delay now, in frames (glides to the target) */
  float lp[2];         /* the feedback path's low-pass state */
  float p[4];          /* P1..P4 as params() gave them (a host without b->p) */
} pp;

static const float k_ticks[8] = {6.0f, 8.0f, 12.0f, 16.0f, 18.0f, 24.0f, 48.0f, 96.0f};   /* 24 ppqn */

static int32_t pp_init(const rmp_host *h) { return h->sample_rate == 48000.0f ? 0 : 1; }
static void pp_voice_init(void *st) {
  pp *s = st;
  s->p[0] = 36.0f, s->p[1] = 64.0f, s->p[2] = 96.0f, s->p[3] = 127.0f;
}
static void pp_params(void *st, const float *p) {
  pp *s = st;
  for (int k = 0; k < 4; ++k) s->p[k] = p[k];
}
static void pp_attach(void *st, const rmp3_voice *v) {
  pp *s = st;
  s->buf = (float *)v->arena;
  s->frames = v->arena ? v->arena_bytes / (2u * sizeof(float)) : 0u;
  s->w = 0;
  s->d = 0.0f;
}
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
/* channel c of the line, d frames behind the write position (linear between frames) */
static inline float tap(const pp *s, int c, float d) {
  float pos = (float)s->w - d;
  while (pos < 0.0f) pos += (float)s->frames;
  uint32_t i = (uint32_t)pos;
  float f = pos - (float)i;
  uint32_t j = i + 1u < s->frames ? i + 1u : 0u;
  return s->buf[2u * i + (uint32_t)c] * (1.0f - f) + s->buf[2u * j + (uint32_t)c] * f;
}
static void pp_render(void *st, const rmp3_block *b) {
  pp *s = st;
  float *l = b->out[0], *r = b->out[1];
  const float *p = b->p ? b->p : s->p;
  if (!s->buf || s->frames < 4u) {
    for (int32_t i = 0; i < b->n; ++i) l[i] = 0.0f;
    if (r) for (int32_t i = 0; i < b->n; ++i) r[i] = 0.0f;
    return;
  }
  int sel = (int)clampf(p[0], 0.0f, 127.0f) * 8 / 128;
  float target = clampf(k_ticks[sel] * b->transport->tick_us * 0.048f, 1.0f, (float)(s->frames - 2u));
  if (s->d < 1.0f) s->d = target;   /* the first block: at once */
  float fb = clampf(p[1], 0.0f, 127.0f) / 127.0f * 0.9f;
  float a = 0.05f + 0.95f * clampf(p[2], 0.0f, 127.0f) / 127.0f;
  float wid = clampf(p[3], 0.0f, 127.0f) / 127.0f;
  for (int32_t i = 0; i < b->n; ++i) {
    s->d += (target - s->d) * 0.0005f;   /* a time change glides (pitch, no click) */
    float x = b->in[0] ? b->in[0][i] : 0.0f;
    if (b->in[1]) x = 0.5f * (x + b->in[1][i]);
    float yl = tap(s, 0, s->d), yr = tap(s, 1, s->d);
    s->lp[0] += a * (yr - s->lp[0]);   /* R's echo feeds L, L's feeds R: the ping-pong */
    s->lp[1] += a * (yl - s->lp[1]);
    s->buf[2u * s->w] = x + fb * s->lp[0];
    s->buf[2u * s->w + 1u] = fb * s->lp[1];
    if (++s->w == s->frames) s->w = 0;
    float m = 0.5f * (yl + yr), sd = 0.5f * (yl - yr) * wid;
    if (r) l[i] = m + sd, r[i] = m - sd;
    else l[i] = m;   /* a mono route: the mix */
  }
}

static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_EFFECT, .places = RMP3_AT_SEND,
  .needs = RMP3_CAP_ARENA | RMP3_CAP_EFFECT,
  .wants = RMP3_CAP_TRANSPORT | RMP3_CAP_STEREO | RMP3_CAP_BLOCK_P,
  .voice_min = 48000u * 2u * sizeof(float),        /* 1 s a side */
  .voice_want = 2u * 192000u * sizeof(float),      /* 4 s a side: 1 BAR at 60 BPM */
  .in_channels = 1, .out_channels = 2,
  .voice_attach = pp_attach, .render = pp_render,
};

RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC,
  .api = 3u,                     /* API 3 only */
  .state_bytes = sizeof(pp), .cpu_permille = 20,
  .init = pp_init, .voice_init = pp_voice_init, .params = pp_params,
  .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
