// pi/engine/seditview_pi.cpp - see seditview_pi.h.
#include "seditview_pi.h"

#include <stddef.h>
#include <string.h>

#include "platform.h"

#include "samp/sedit.h"

namespace seditview_pi {

namespace {
core::sedit::View g_v;   // (4 KB: the outline; not on the stack)
uint32_t mix(uint32_t h, uint32_t x) { return (h ^ x) * 16777619u; }
static_assert(SSE_COLS == core::sedit::kCols, "the outline's columns");
static_assert(sizeof(((sidecar_sedit_view_t*)0)->name) == sizeof g_v.name, "the name");
static_assert(sizeof(((sidecar_sedit_view_t*)0)->msg) == sizeof g_v.msg, "the message");
}  // namespace

bool Open() {
  core::sedit::view(&g_v);
  return g_v.st != core::sedit::kClosed;
}

void Fill(sidecar_sedit_view_t* o) {
  core::sedit::view(&g_v);
  memset(o, 0, offsetof(sidecar_sedit_view_t, gen));
  o->valid = g_v.st != core::sedit::kClosed;
  o->st = g_v.st;
  memcpy(o->name, g_v.name, sizeof o->name);
  memcpy(o->msg, g_v.msg, sizeof o->msg);
  o->name[sizeof o->name - 1] = o->msg[sizeof o->msg - 1] = 0;   // (a racy copy is still a string)
  o->frames = g_v.frames, o->rate = g_v.rate, o->a = g_v.a, o->b = g_v.b, o->wa = g_v.wa, o->wb = g_v.wb;
  o->ch = g_v.ch, o->bits = g_v.bits, o->fn = g_v.fn, o->zoom = g_v.zoom;
  o->undo = g_v.undo, o->dirty = g_v.dirty, o->has_track = g_v.has_track, o->track = g_v.track;
  o->progress = g_v.progress;
  o->ncols = o->valid ? SSE_COLS : 0;
  memcpy(o->lo, g_v.lo, sizeof o->lo);
  memcpy(o->hi, g_v.hi, sizeof o->hi);
  o->playhead = g_v.playhead;
  o->unity = g_v.unity;
  // the generation: everything drawn
  uint32_t h = 2166136261u;
  const uint8_t* b = (const uint8_t*)o;
  for (size_t i = 0; i + 4 <= offsetof(sidecar_sedit_view_t, gen); i += 4) {
    uint32_t w;
    memcpy(&w, b + i, 4);
    h = mix(h, w);
  }
  o->gen = h | 2u;
}

}  // namespace seditview_pi
