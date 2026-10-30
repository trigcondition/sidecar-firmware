// plugins/noiseplethora/np_single.cpp - one Noise Plethora plugin (class NP_PLUGIN
// from header NP_HEADER, copied from vendor/np by the Makefile) as a machine:
// P2 = X, P3 = Y (NP's two knobs, 0..1), P8 LEV. GPL-3.0-or-later (built with
// Befaco's GPL plugin code); the Teensy Audio objects it links are MIT.
//
// The plugin object and its Teensy Audio graph live in the voice state (placement
// new into an AudioGraph made current first); the graph runs one pass every
// AUDIO_BLOCK_SAMPLES (NP_BLOCK: 16, or 128 for Atari) samples and render() hands out
// 12 at a time, so a render call runs at most one pass (at 128 about every 10.7th call:
// size cpu_permille from that peak call, not from the average). As on NP, the
// output cord is made before init(), and process(X, Y) is called with the knobs
// (here on every params()). NP plugins drone (no notes): a trig sets the level
// from the velocity and the Rytm's AMP envelope shapes the sound, as DIGIDRUM's did.
// NP was written for 44117.647 Hz; at 48 kHz its frequencies are ~8.8 % higher.
#include <new>
#include <stdint.h>
#include "rmp/rmp_api.h"
#include "AudioStream.h"
#include "AudioTap.h"
#include NP_HEADER

#ifndef NP_GAIN
#define NP_GAIN 1.0f   /* NP's per-plugin output gain (Banks_Def.hpp) */
#endif

struct npv {
  AudioGraph graph;
  alignas(8) uint8_t obj[sizeof(NP_PLUGIN)];
  alignas(8) uint8_t tap_obj[sizeof(AudioTap)];
  alignas(8) uint8_t cord_obj[sizeof(AudioConnection)];
  NP_PLUGIN *plugin;
  AudioTap *tap;
  int pos;
  float level, vel;
};

static uint32_t np_voices;   // voice_init calls since the plugin was loaded (random() seeds)

#ifdef RMP_NATIVE
// native build only, for the harness's tests: allocation failures summed over all
// voices' graphs, and the most blocks any graph held at once
extern "C" __attribute__((visibility("default"))) uint32_t np_debug_alloc_failed, np_debug_blocks_max;
uint32_t np_debug_alloc_failed, np_debug_blocks_max;
static void np_debug(AudioGraph &g) {
  np_debug_alloc_failed += g.alloc_failed; g.alloc_failed = 0;
  if (g.memory_used_max > np_debug_blocks_max) np_debug_blocks_max = g.memory_used_max;
}
#else
static void np_debug(AudioGraph &) {}
#endif

static int32_t np_init(const rmp_host *h) { return h->api == RMP_API ? 0 : 1; }

static void np_voice_init(void *st) {
  npv *v = (npv *)st;
  // a seed per voice for random(), from the order of voice_init calls: voices do not
  // share a sequence, and a run is reproducible
  v->graph.begin(0x5EED1234u + 2654435761u * ++np_voices);                                 // objects register in construction order
  v->plugin = new (v->obj) NP_PLUGIN();
  v->tap = new (v->tap_obj) AudioTap();
  new (v->cord_obj) AudioConnection(v->plugin->getStream(), v->plugin->getPort(), *v->tap, 0);
  v->plugin->init();
  v->pos = AUDIO_BLOCK_SAMPLES;
  v->level = 1.0f;
  v->vel = 1.0f;
}

static void np_note_on(void *st, float note, float vel) { (void)note; ((npv *)st)->vel = vel; }
static void np_note_off(void *st) { (void)st; }

static void np_params(void *st, const float *p) {
  npv *v = (npv *)st;
  audio_current_graph = &v->graph;
  v->plugin->process(p[1] / 127.0f, p[2] / 127.0f);
  v->level = p[7] / 127.0f;
}

static void np_render(void *st, float *out, int32_t n) {
  npv *v = (npv *)st;
  float g = v->level * v->vel * (NP_GAIN / 32768.0f);
  for (int32_t i = 0; i < n; ++i) {
    if (v->pos == AUDIO_BLOCK_SAMPLES) { v->graph.update_all(); np_debug(v->graph); v->pos = 0; }
    out[i] = v->tap->buf[v->pos++] * g;
  }
}

RMP_ENTRY const rmp_plugin rmp_entry = {RMP_PLUGIN_MAGIC, RMP_API, sizeof(npv), NP_PERMILLE,
  np_init, np_voice_init, np_note_on, np_note_off, np_params, np_render, {0}};
