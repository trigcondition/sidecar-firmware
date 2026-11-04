// plugins/api3_test/probe.c - Plugin API 3's test machine (core/tests/test_api3.cpp). A dual package by default: an
// API 2 table whose reserved[RMP_P_V3] is an rmp3_plugin, so an API 2 host plays its API 2 render (a marker 0.25 in
// frame 0, silence after) and an API 3 host its API 3 render, which reports what the host gave it as levels, one
// frame each (the test reads them back from the voice's output):
//   0  0.5: the API 3 render ran            6  the voice's arena part in 16 MB units (bytes / 2^24)
//   1  the transport's bpm / 1000           7  song_tick % 1000 / 1000 while locked, else -0.5
//   2  the transport's flags / 64           8  the block's n / 16
//   3  0.5: the arena part was zero at voice_attach, is still the voice's own (a pattern it wrote), else 0
//   4  0.5: setup() saw the shared part zeroed and wrote it; the shared part still says so, else 0
//   5  (voice + 1) / 16                     9  0.5 when out[1] is null (a mono route), else 0.25
//   10 setup() calls / 16 (the shared part's counter)
// P1 MODE in zones of 16: 0 plays, 1 params() faults, 2 params() runs ~2 ms (late), 3 note_on() faults.
// Variants (Makefile V=): PROBE_ONLY an API 3 only table (api 3, no API 2 render); PROBE_INITCRASH init() faults;
// PROBE_NEEDS needs the signal bus (no host has it yet); PROBE_BIG wants an arena past any slot's limit;
// PROBE_STEREO out_channels 2: on a stereo route R is -L.
// MIT, Copyright 2026 trigcondition and contributors.
#include "rmp/rmp_api3.h"

typedef struct {
  int32_t mode, voice, attached, arena_ok;
  uint32_t arena_bytes;
  uint32_t *arena;
  uint32_t *shared;
  float level;
} probe;

#define SHARED_MAGIC 0xA3A3A3A3u
static const rmp_host *g_host;

static int32_t p_init(const rmp_host *h) {
#ifdef PROBE_INITCRASH
  *(volatile int *)0 = 1;
#endif
  g_host = h;
  return 0;
}
static void p_voice_init(void *st) { ((probe *)st)->level = 100.0f / 127.0f; }
static void p_note_on(void *st, float note, float vel) {
  (void)note; (void)vel;
  if (((probe *)st)->mode == 3) *(volatile int *)0 = 1;
}
static void p_note_off(void *st) { (void)st; }
static void p_params(void *st, const float *p) {
  probe *s = st;
  s->mode = (int32_t)p[0] / 16;
  s->level = p[1] / 127.0f;
  if (s->mode == 1) *(volatile int *)0 = 1;
  if (s->mode == 2)
    for (volatile uint32_t i = 0; i < 6000000u; ++i) {}
}
static void p_render2(void *st, float *out, int32_t n) {
  (void)st;
  for (int32_t i = 0; i < n; ++i) out[i] = 0.0f;
  out[0] = 0.25f;
}

static int32_t p_setup(const rmp3_setup *su) {
  uint32_t *sh = (uint32_t *)su->shared;
  if (!sh || su->shared_bytes < 16) return 0;
  for (uint32_t i = 0; i < su->shared_bytes / 4; ++i)
    if (sh[i]) return 0;   /* not zeroed: the probe's frame 4 stays 0 */
  sh[0] = SHARED_MAGIC;
  sh[1] += 1;
  return 0;
}
static void p_attach(void *st, const rmp3_voice *v) {
  probe *s = st;
  s->attached = 1;
  s->voice = v->voice;
  s->shared = (uint32_t *)v->shared;
  s->arena = (uint32_t *)v->arena;
  s->arena_bytes = v->arena_bytes;
  s->arena_ok = 0;
  if (!s->arena || s->arena_bytes < 4096) return;
  uint32_t words = s->arena_bytes / 4, bad = 0;
  for (uint32_t i = 0; i < 1024; ++i) bad |= s->arena[i] | s->arena[words - 1 - i];
  if (bad) return;
  s->arena[0] = 0x5EED0000u + (uint32_t)v->voice;   /* its own: another voice's part would say another number */
  s->arena[words - 1] = s->arena[0];
  s->arena_ok = 1;
}
static void p_render3(void *st, const rmp3_block *b) {
  probe *s = st;
  float *o = b->out[0];
  for (int32_t i = 0; i < b->n; ++i) o[i] = 0.0f;
  if (b->out[1])
    for (int32_t i = 0; i < b->n; ++i) b->out[1][i] = 0.0f;
  if (b->n < 11) return;
  const rmp3_transport *t = b->transport;
  uint32_t mine = 0x5EED0000u + (uint32_t)s->voice;
  o[0] = 0.5f;
  o[1] = t->bpm / 1000.0f;
  o[2] = (float)(t->flags & 63u) / 64.0f;
  o[3] = s->arena_ok && s->arena[0] == mine && s->arena[s->arena_bytes / 4 - 1] == mine ? 0.5f : 0.0f;
  o[4] = s->shared && s->shared[0] == SHARED_MAGIC ? 0.5f : 0.0f;
  o[5] = (float)(s->voice + 1) / 16.0f;
  o[6] = (float)s->arena_bytes / 16777216.0f;
  o[7] = t->song_tick >= 0 ? (float)(t->song_tick % 1000) / 1000.0f : -0.5f;
  o[8] = (float)b->n / 16.0f;
  o[9] = b->out[1] ? 0.25f : 0.5f;
  o[10] = s->shared ? (float)s->shared[1] / 16.0f : 0.0f;
  if (b->out[1])   /* a stereo route (PROBE_STEREO): R is L upside down */
    for (int32_t i = 0; i < b->n; ++i) b->out[1][i] = -o[i];
}

static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_MACHINE, .places = RMP3_AT_VOICE,
#ifdef PROBE_NEEDS
  .needs = RMP3_CAP_BUS,
#endif
  .wants = RMP3_CAP_TRANSPORT | RMP3_CAP_ARENA | RMP3_CAP_GUARD | RMP3_CAP_STEREO,
  .shared_min = 4096, .shared_want = 65536,
#ifdef PROBE_BIG
  .voice_min = 64u << 20, .voice_want = 64u << 20,
#else
  .voice_min = 65536, .voice_want = 262144,
#endif
#ifdef PROBE_STEREO
  .out_channels = 2,
#else
  .out_channels = 1,
#endif
  .setup = p_setup, .voice_attach = p_attach, .render = p_render3,
};

RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC,
#ifdef PROBE_ONLY
  .api = 3u,                     /* API 3 only: an older host refuses it */
#else
  .api = 2u,                     /* dual: a literal, an older host plays the API 2 render */
  .render = p_render2,
#endif
  .state_bytes = sizeof(probe), .cpu_permille = 10,
  .init = p_init, .voice_init = p_voice_init, .note_on = p_note_on, .note_off = p_note_off, .params = p_params,
  .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
