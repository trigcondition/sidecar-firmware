// core/mod/scene.cpp - see scene.h. Control side only, cold: a tick hands the layer one value per lock of A and B
// (at most 128), from a merged list rebuilt only when the locks or A / B change.
#include "mod/scene.h"

#include <string.h>

#include "mod/mod.h"
#include "platform.h"
#include "ui/remote.h"

namespace core {
namespace scene {

namespace {

State g_st;
// the played fader: from g_from at g_set_ms to g_st.fader kSmoothMs later, linearly
int g_played = 0, g_from = 0;
uint32_t g_set_ms = 0, g_now_ms = 0;
bool g_upper = false;            // the stepped targets' side: B's (true) or A's; switched with 1/64 hysteresis
int g_last_a = -1, g_last_b = -1;   // the A / B the layer's entries are of (an A / B change releases the old ones)
// A's and B's locks merged: each target once, with its A value and its B value (-1: that side does not lock it)
struct M {
  mod::Tid t;
  int32_t av, bv;
};
M g_m[2 * kLocks];
int g_mn = 0;
bool g_stale = true;   // the merged list needs rebuilding

CORE_COLD bool ok_scene(int n) { return n >= 0 && n < kScenes; }
CORE_COLD int find_lock(const Scene& s, mod::Tid t) {
  for (int i = 0; i < s.n; ++i)
    if (s.l[i].t == t) return i;
  return -1;
}
CORE_COLD void merge() {
  g_mn = 0;
  if (ok_scene(g_st.a)) {
    const Scene& a = g_st.s[g_st.a];
    for (int i = 0; i < a.n; ++i) g_m[g_mn++] = {a.l[i].t, a.l[i].v, -1};
  }
  if (ok_scene(g_st.b)) {
    const Scene& b = g_st.s[g_st.b];
    for (int i = 0; i < b.n; ++i) {
      int j = 0;
      while (j < g_mn && g_m[j].t != b.l[i].t) ++j;
      if (j < g_mn) g_m[j].bv = b.l[i].v;
      else g_m[g_mn++] = {b.l[i].t, -1, b.l[i].v};
    }
  }
  g_stale = false;
}
// a lock of scene n (0..11, -1 none) on t
CORE_COLD bool locks(int n, mod::Tid t) { return ok_scene(n) && find_lock(g_st.s[n], t) >= 0; }

struct Init {   // before the first reset(): no A, no B
  Init() { g_st.a = g_st.b = -1; }
} g_init;

}  // namespace

CORE_COLD const State& state() { return g_st; }

CORE_COLD bool lock(int n, mod::Tid t, uint16_t v) {
  if (!ok_scene(n) || !mod::valid(t) || v > kFaderMax) return false;
  Scene& s = g_st.s[n];
  int i = find_lock(s, t);
  if (i < 0) {
    if (s.n >= kLocks) return false;   // SCENE FULL
    i = s.n++;
  }
  s.l[i] = {t, v};
  g_stale = true;
  return true;
}

CORE_COLD bool unlock(int n, mod::Tid t) {
  if (!ok_scene(n)) return false;
  Scene& s = g_st.s[n];
  int i = find_lock(s, t);
  if (i < 0) return false;
  memmove(&s.l[i], &s.l[i + 1], (size_t)(s.n - i - 1) * sizeof(Lock));
  --s.n;
  g_stale = true;
  return true;
}

CORE_COLD void clear(int n) {
  if (!ok_scene(n)) return;
  g_st.s[n].n = 0;
  g_stale = true;
}

CORE_COLD void copy(int from, int to) {
  if (!ok_scene(from) || !ok_scene(to) || from == to) return;
  g_st.s[to] = g_st.s[from];
  g_stale = true;
}

CORE_COLD void set_ab(int a, int b) {
  g_st.a = (int8_t)(ok_scene(a) ? a : -1);
  g_st.b = (int8_t)(ok_scene(b) ? b : -1);
  g_stale = true;
}

CORE_COLD void set_sel(int n) {
  if (ok_scene(n)) g_st.sel = (int8_t)n;
}

CORE_COLD void set_fader(uint16_t v, int source) {
  (void)source;   // (whichever moved last wins: all sources set the same value)
  if (v > kFaderMax) v = kFaderMax;
  if (v == g_st.fader) return;
  g_from = g_played, g_set_ms = now_ms();
  g_st.fader = v;
  mod::fader_moved();   // a held manual edit lets go (spec-050 2.5)
}

CORE_COLD uint16_t fader_played() { return (uint16_t)g_played; }

CORE_COLD void tick(uint32_t ms) {
  g_now_ms = ms;
  uint32_t el = ms - g_set_ms;
  int to = g_st.fader;
  g_played = el >= kSmoothMs ? to : g_from + (to - g_from) * (int)el / (int)kSmoothMs;
  float f = (float)g_played / (float)kFaderMax;
  constexpr float kH = 1.0f / 64.0f;
  if (g_st.a != g_last_a || g_st.b != g_last_b) {
    // the old scenes' targets that the new A / B do not lock: back to their bases before the new ones' entries are
    // allocated (spec-050 1.4: so an A / B change of full scenes stays within kEntries)
    for (int old : {g_last_a, g_last_b}) {
      if (!ok_scene(old) || old == g_st.a || old == g_st.b) continue;
      const Scene& s = g_st.s[old];
      for (int i = 0; i < s.n; ++i)
        if (!locks(g_st.a, s.l[i].t) && !locks(g_st.b, s.l[i].t)) mod::release(s.l[i].t);
    }
    g_last_a = g_st.a, g_last_b = g_st.b;
    g_upper = f >= 0.5f;
  } else if (g_upper ? f < 0.5f - kH : f >= 0.5f + kH) {
    g_upper = !g_upper;
  }
  if (g_stale) merge();
  uint16_t lists[8];   // the voices' list knobs this tick (a model, a pattern: they step), looked up once a voice
  uint8_t looked = 0;
  for (int i = 0; i < g_mn; ++i) {
    const M& m = g_m[i];
    mod::Kind k = mod::kind_of(m.t);
    bool step = mod::range(m.t).mode == mod::kStep;
    if (k == mod::kV) {
      int vi = mod::index_of(m.t) & 7;
      if (!(looked >> vi & 1)) {
        lists[vi] = 0;
        for (int p = 0; p < 16; ++p)
          if (ui::remote_is_list(vi, p)) lists[vi] |= (uint16_t)(1u << p);
        looked |= (uint8_t)(1u << vi);
      }
      step = lists[vi] >> mod::param_of(m.t) & 1;
    }
    float a = m.av >= 0 ? (float)m.av / 128.0f : 0, b = m.bv >= 0 ? (float)m.bv / 128.0f : 0;
    if (m.av < 0) a = mod::base(m.t);
    else if (m.bv < 0) b = mod::base(m.t);
    float val = step ? (g_upper ? b : a) : a + (b - a) * f;
    mod::morph(m.t, val);
  }
}

CORE_COLD void reset() {
  memset(&g_st, 0, sizeof g_st);
  g_st.a = g_st.b = -1;
  g_played = g_from = 0;
  g_set_ms = now_ms(), g_now_ms = g_set_ms;
  g_upper = false;
  g_last_a = g_last_b = -1;
  g_mn = 0, g_stale = true;
}

}  // namespace scene
}  // namespace core
