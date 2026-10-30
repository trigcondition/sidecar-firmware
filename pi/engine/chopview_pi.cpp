// pi/engine/chopview_pi.cpp - see chopview_pi.h.
#include "chopview_pi.h"

#include <stddef.h>
#include <string.h>

#include "platform.h"

#include "chop/chop.h"
#include "machines.h"
#include "ui/remote.h"
#include "voices.h"

namespace chopview_pi {

namespace {
bool shows(int vi) { return vi >= 0 && vi < 8 && (core::v[vi].cp || core::chop::forced(vi)); }
uint32_t mix(uint32_t h, uint32_t x) { return (h ^ x) * 16777619u; }
float g_mn[SCH_COLS], g_mx[SCH_COLS];
}  // namespace

void Fill(sidecar_chop_view_t* o, int want) {
  int vi = -1, track = -1;
  int t = core::ui::remote_selected_track(nullptr);
  if (t >= 0 && t < 12 && shows(core::ui::remote_track_voice(t))) vi = core::ui::remote_track_voice(t), track = t;
  if (vi < 0 && shows(want)) vi = want;
  for (int k = 0; k < 8 && vi < 0; ++k)
    if (shows(k)) vi = k;
  memset(o, 0, offsetof(sidecar_chop_view_t, gen));
  uint32_t h = 2166136261u;
  if (vi < 0) {
    o->gen = 1;   // (no voice: one generation)
    return;
  }
  o->valid = 1, o->voice = vi, o->track = track;
  if (track < 0)
    for (int k = 0; k < 12; ++k)
      if (core::ui::remote_track_voice(k) == vi && (core::ui::remote_track_machine(k) & 0x78) == core::kChopMachine) o->track = k;
  const core::chop::Map* m = core::chop::map_of(vi);
  o->ready = core::chop::sample_ready(vi) && m;
  o->frames = core::chop::sample_frames(vi), o->rate = core::chop::sample_rate(vi);
  if (m) {
    sniprintf(o->name, sizeof o->name, "%s", m->name);
    o->n = m->n > SCH_SLICES ? SCH_SLICES : m->n;
    for (int k = 0; k < o->n; ++k) {
      o->start[k] = m->s[k].start, o->end[k] = m->s[k].end;
      for (int f = 0; f < SCH_FIELDS && f < core::chop::kFields; ++f) {
        o->field[k][f] = core::chop::field_get(m->s[k], f);
        core::chop::field_text(m->s[k], f, o->rate, o->ftext[k][f], SCH_TEXT);
      }
    }
  }
  for (int f = 0; f < SCH_FIELDS && f < core::chop::kFields; ++f) {
    sniprintf(o->fname[f], sizeof o->fname[f], "%s", core::chop::field_name(f));
    core::chop::field_range(f, &o->flo[f], &o->fhi[f]);
  }
  if (o->ready) {
    sidecar_chop_range(o->frames, &o->a, &o->b);
    o->ncols = core::chop::outline(vi, o->a, o->b, SCH_COLS, g_mn, g_mx);
    for (int i = 0; i < o->ncols; ++i) o->lo[i] = (short)(g_mn[i] * 32767.0f), o->hi[i] = (short)(g_mx[i] * 32767.0f);
    int32_t ph[SCH_HEADS];
    o->nheads = core::chop::playheads(vi, ph, SCH_HEADS);
    for (int i = 0; i < o->nheads; ++i) o->head[i] = (unsigned)ph[i];
  }
  const core::chop::Lazy* l = core::chop::lazy_state(vi);
  if (l && l->on) {
    o->lazy = 1, o->lazy_n = l->n > SCH_SLICES ? SCH_SLICES : l->n;
    for (int i = 0; i < o->lazy_n; ++i) o->lazy_at[i] = l->at[i];
    o->lazy_pos = core::chop::lazy_playhead(vi);
  }
  o->slc = core::eng_zone(core::v[vi].p[1], 64);
  o->played = core::chop::played_slice(vi);
  o->dirty = core::chop::map_dirty(vi);
  o->rs_state = core::chop::resample_state(&o->rs_slice, o->rs_row);
  o->lat = core::chop::latency();
  // the generation: everything drawn
  const uint8_t* b = (const uint8_t*)o;
  for (size_t i = 0; i < offsetof(sidecar_chop_view_t, gen); i += 4) {
    uint32_t w;
    memcpy(&w, b + i, 4);
    h = mix(h, w);
  }
  o->gen = h | 2u;   // (never 1: that is "no voice")
}

}  // namespace chopview_pi
