// plugins/api3_test/drawprobe.c - drawn pages' test machine (sdk/rmp/rmp_api3.h RMP3_CAP_DRAW; core/tests/test_draw.cpp,
// and by hand on a Sidecar: put it in a user slot, play it on a track, open that track's SRC page, turn knob A).
// A dual package (an API 2 table with an rmp3_plugin) whose draw does what P1 says, in zones of 16:
//   0 draws: P1..P16 as bars (row 2k + 1, P(k+1) pixels long), the page number as a bar on row 34, a pixel at (0, 36)
//     when it got a state, a fixed pattern on rows 48..63 (x + 2y) % 5 == 0; returns 1
//   1 declines (returns 0: the host's page)      5 writes the byte before fb, returns 1
//   2 faults (a write to address 0)               6 runs ~100 us (under the limit)  , then draws as 0
//   3 never returns (a loop: the hard limit)      7 calls the host's log, then draws as 0 (the line must not appear)
//   4 writes 64 bytes past fb's end, returns 1
// Variant (Makefile V=drawnw): the same draw but without RMP3_CAP_DRAW in wants: never called.
// The render is silence; P2 is LEVEL only so the stock SRC page has two knobs to show.
// MIT, Copyright 2026 trigcondition and contributors.
#include "rmp/rmp_api3.h"

typedef struct { int32_t voice; } dprobe;
static const rmp_host *g_host;

static int32_t d_init(const rmp_host *h) { g_host = h; return 0; }
static void d_voice_init(void *st) { ((dprobe *)st)->voice = -1; }
static void d_note_on(void *st, float note, float vel) { (void)st; (void)note; (void)vel; }
static void d_note_off(void *st) { (void)st; }
static void d_params(void *st, const float *p) { (void)st; (void)p; }
static void d_render2(void *st, float *out, int32_t n) {
  (void)st;
  for (int32_t i = 0; i < n; ++i) out[i] = 0.0f;
}
static void d_render3(void *st, const rmp3_block *b) {
  (void)st;
  for (int32_t i = 0; i < b->n; ++i) b->out[0][i] = 0.0f;
}
static void lit(uint8_t *fb, int x, int y) { fb[y * 16 + x / 8] |= (uint8_t)(0x80u >> (x & 7)); }
static void bar(uint8_t *fb, int y, int n) {
  for (int x = 0; x < n && x < RMP3_DRAW_W; ++x) lit(fb, x, y);
}
static int32_t d_draw(const void *state, int32_t page, const float *p, uint8_t *fb) {
  const int mode = (int)p[0] / 16;
  if (mode == 1) return 0;
  if (mode == 2) *(volatile int *)0 = 1;
  if (mode == 3)
    for (volatile uint32_t i = 0;; ++i) {}
  if (mode == 6)   /* ~100 us on a Pi 4 (150000 took 838 us there), less on a Mac: the limit is 250 us */
    for (volatile uint32_t i = 0; i < 18000u; ++i) {}
  if (mode == 7 && g_host && g_host->log) g_host->log("drawprobe: a log line from inside a draw");
  for (int k = 0; k < 16; ++k) bar(fb, 2 * k + 1, (int)p[k]);
  bar(fb, 34, page + 1);
  if (state) lit(fb, 0, 36);
  for (int y = 48; y < RMP3_DRAW_H; ++y)
    for (int x = 0; x < RMP3_DRAW_W; ++x)
      if ((x + 2 * y) % 5 == 0) lit(fb, x, y);
  if (mode == 4)
    for (int i = 0; i < 64; ++i) ((volatile uint8_t *)fb)[RMP3_DRAW_BYTES + i] = 0x5A;
  if (mode == 5) ((volatile uint8_t *)fb)[-1] = 0x5A;
  return 1;
}

static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_MACHINE, .places = RMP3_AT_VOICE,
#ifdef DPROBE_NOWANT
  .wants = RMP3_CAP_GUARD,
#else
  .wants = RMP3_CAP_GUARD | RMP3_CAP_DRAW,
#endif
  .out_channels = 1,
  .render = d_render3,
  .reserved = { [RMP3_P_DRAW] = RMP_FN(d_draw) },
};

RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC,
  .api = 2u,                     /* dual: an older host plays the API 2 render and never sees the draw */
  .render = d_render2,
  .state_bytes = sizeof(dprobe), .cpu_permille = 5,
  .init = d_init, .voice_init = d_voice_init, .note_on = d_note_on, .note_off = d_note_off, .params = d_params,
  .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
