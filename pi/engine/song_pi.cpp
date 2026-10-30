// pi/engine/song_pi.cpp - see song_pi.h. Hosted (the core's flags).
#include "song_pi.h"

#include <stddef.h>
#include <string.h>

#include "platform.h"
#include "song/song.h"

namespace song_pi {

namespace {
sidecar_song_view_t g_buf[2];
volatile uint32_t g_n = 0;
volatile bool g_want = false;
core::song::Lanes g_lanes;
uint32_t g_lanes_seen = 0, g_ms = 0, g_lanes_gen = 0, g_show_seen = 0;
}  // namespace

void Want(bool on) { g_want = on; }

void Tick() {
  uint32_t ms = core::now_ms();
  uint32_t req = core::song::show_requests();
  bool want = g_want || req != g_show_seen;
  core::song::lanes_want(g_want);
  if (!want || ms - g_ms < 100) return;
  g_ms = ms;
  g_show_seen = req;
  if (core::song::lanes_read(&g_lanes, &g_lanes_seen)) ++g_lanes_gen;
  core::song::Status s = core::song::status();
  uint32_t n = g_n;
  sidecar_song_view_t& o = g_buf[(n / 2 + 1) & 1];
  memset(&o, 0, offsetof(sidecar_song_view_t, nblocks));
  o.valid = 1;
  o.state = s.state;
  memcpy(o.name, s.name, sizeof o.name);
  memcpy(o.project, s.project, sizeof o.project);
  memcpy(o.msg, s.msg, sizeof o.msg);
  core::song::config_text(s.problems, o.config, sizeof o.config);
  o.cfg_wrong = s.cfg_wrong;
  for (int k = 0; k < SSG_CFG_ROWS && k < core::song::kCfgRows; ++k) sniprintf(o.cfg_row[k], sizeof o.cfg_row[k], "%s", core::song::config_row(k));
  o.length = s.length > g_lanes.length ? s.length : g_lanes.length;
  o.playhead = s.t_now, o.events = s.events;
  o.bpm10 = (int)((s.tempo * 10 + 60) / 120);
  o.pattern = s.pattern, o.takes = s.takes, o.take_i = s.take_i, o.recovered = s.recovered, o.settings = s.settings;
  o.undo = s.undo;
  const core::song::Lanes& L = g_lanes;
  o.nblocks = L.nblocks < SSG_BLOCKS ? L.nblocks : SSG_BLOCKS;
  for (int i = 0; i < o.nblocks; ++i) o.blocks[i] = {L.blocks[i].t0, L.blocks[i].t1, L.blocks[i].v};
  o.nmutes = L.nmutes < SSG_MUTES ? L.nmutes : SSG_MUTES;
  for (int i = 0; i < o.nmutes; ++i) o.mutes[i] = {L.mutes[i].t0, L.mutes[i].t1, L.mutes[i].v};
  o.nscenes = L.nscenes < SSG_SCENES ? L.nscenes : SSG_SCENES;
  for (int i = 0; i < o.nscenes; ++i) o.scenes[i] = {L.scenes[i].t0, L.scenes[i].t1, L.scenes[i].v};
  o.nperf = L.nperf < SSG_POINTS ? L.nperf : SSG_POINTS;
  for (int i = 0; i < o.nperf; ++i) o.perf[i] = {L.perf[i].t, L.perf[i].v};
  o.ntempo = L.ntempo < SSG_TEMPO ? L.ntempo : SSG_TEMPO;
  for (int i = 0; i < o.ntempo; ++i) o.tempo[i] = {L.tempo[i].t, L.tempo[i].v};
  o.nmarks = L.nmarks < SSG_MARKS ? L.nmarks : SSG_MARKS;
  for (int i = 0; i < o.nmarks; ++i) o.marks[i] = L.marks[i];
  o.nlanes = L.nlanes < SSG_LANES ? L.nlanes : SSG_LANES;
  for (int k = 0; k < o.nlanes; ++k) {
    const core::song::Lane& a = L.lane[k];
    ssg_lane_t& b = o.lane[k];
    memcpy(b.label, a.label, sizeof b.label);
    b.label[sizeof b.label - 1] = 0;
    b.sidecar = a.sidecar;
    b.n = a.n < SSG_POINTS ? a.n : SSG_POINTS;
    for (int i = 0; i < b.n; ++i) b.p[i] = {a.p[i].t, a.p[i].v};
  }
  o.show_req = req;
  o.gen = g_lanes_gen * 16u + (uint32_t)s.state;
  __atomic_store_n(&g_n, n + 2, __ATOMIC_RELEASE);
}

bool View(sidecar_song_view_t* out, unsigned* seen) {
  for (int tries = 0; tries < 4; ++tries) {
    uint32_t a = __atomic_load_n(&g_n, __ATOMIC_ACQUIRE);
    if (a == 0 || a == *seen) return false;
    memcpy(out, &g_buf[(a / 2) & 1], sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_n, __ATOMIC_ACQUIRE) - a < 2) {
      *seen = a;
      return true;
    }
  }
  return false;
}

}  // namespace song_pi
