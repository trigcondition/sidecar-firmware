// plugins/api3_test/fxprobe.c - Plugin API 3's test effect (core/tests/test_api3.cpp; Makefile V=fx): an API 3 only
// RMP3_KIND_EFFECT for the send position. P1 MODE in zones of 16 (b->p): 0 writes, on the left, frame 0 = 0.5 (it ran),
// frame 1 = the block's edge flags (START 1, STOP 2, JUMP 4) / 16, frame 2 = n / 64, the rest of L and all of R 0;
// 1 render faults; 2 render runs long (1.2 M loop steps: over its budget); 3 never returns (a hang); 4 runs ~30 ms and
// returns (a render fx_stop gives up on); 5 runs a little (50 k loop steps: over a small budget, under the default).
// V=fx16 (PROBE_FX16, fx16.json: P1..P16 but P14): also frames 3..14 of L = P5..P16 / 128 (the send slot's held knobs),
// frame 15 = 1 when the host has RMP3_CAP_FX_P16.
// MIT, Copyright 2026 trigcondition and contributors.
#include "rmp/rmp_api3.h"

typedef struct { int32_t calls; } fxp;
#if PROBE_FX16
static uint32_t g_caps;   /* the host's RMP3_CAP_* (frame 15: RMP3_CAP_FX_P16) */
static int32_t f_init(const rmp_host *h) {
  const rmp3_host *x = rmp3_host_of(h);
  g_caps = x ? x->caps : 0u;
  return 0;
}
#else
static int32_t f_init(const rmp_host *h) { (void)h; return 0; }
#endif
static void f_render(void *st, const rmp3_block *b) {
  fxp *s = st;
  int mode = b->p ? (int)b->p[0] / 16 : 0;
  if (mode == 1) *(volatile int *)0 = 1;
  if (mode == 2)
    for (volatile uint32_t i = 0; i < 1200000u; ++i) {}
  if (mode == 3)
    for (;;) {}
  if (mode == 4)
    for (volatile uint32_t i = 0; i < 40000000u; ++i) {}
  if (mode == 5)
    for (volatile uint32_t i = 0; i < 50000u; ++i) {}
  ++s->calls;
  for (int32_t i = 0; i < b->n; ++i) b->out[0][i] = 0.0f, b->out[1][i] = 0.0f;
  uint32_t e = b->transport->flags;
  b->out[0][0] = 0.5f;
  b->out[0][1] = (float)((e & RMP3_T_START ? 1 : 0) | (e & RMP3_T_STOP ? 2 : 0) | (e & RMP3_T_JUMP ? 4 : 0)) / 16.0f;
  b->out[0][2] = (float)b->n / 64.0f;
#if PROBE_FX16
  // fx16 (sendfx-pages, issue #15): P5..P16 as the host hands them, frames 3..14 = p[4..15] / 128 (a block of 15 or more)
  for (int32_t j = 4; j < 16 && j - 1 < b->n; ++j) b->out[0][j - 1] = b->p ? b->p[j] / 128.0f : -1.0f;
  if (b->n > 15) b->out[0][15] = g_caps & RMP3_CAP_FX_P16 ? 1.0f : 0.0f;
#endif
}
static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_EFFECT, .places = RMP3_AT_SEND, .needs = RMP3_CAP_EFFECT,
  .in_channels = 1, .out_channels = 2, .render = f_render,
};
RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC, .api = 3u, .state_bytes = sizeof(fxp), .cpu_permille = 5,
  .init = f_init, .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
