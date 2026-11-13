// pi/engine/xypad_pi.cpp - see xypad_pi.h. Hosted (the core's flags).
#include "xypad_pi.h"

#include <stddef.h>
#include <string.h>

#include "mod/target.h"
#include "mod/xypad.h"
#include "ui/remote.h"
#include "platform.h"

namespace xypad_pi {

namespace {
// a sequence lock, as morph_pi's: core 3 makes g_n odd, writes g_buf, makes it even again; core 0 copies while it is
// even and unchanged across the copy (0: nothing published yet)
sidecar_xy_view_t g_buf;
volatile uint32_t g_n = 0;
volatile bool g_want = false;
volatile bool g_names = false;   // the picker is open: its name tables wanted
bool g_named = false;            // built since it opened
uint32_t g_ms = 0, g_names_ms = 0;
// the picker's name tables, as last built (core 3's): about 290 labels, built only while the picker is open
char g_vl[8][16][5], g_fl[2][4][5], g_il[13][12][6], g_rl[12][8][6];
unsigned g_built = 0;
}  // namespace

void FillView(sidecar_xy_view_t* o, bool names) {
  namespace xy = core::xypad;
  namespace mod = core::mod;
  const xy::Cfg& c = xy::cfg();
  sidecar_xy_view_t v;
  memset(&v, 0, sizeof v);
  v.valid = 1;
  v.hold = c.hold, v.touching = xy::touching(), v.active = xy::active(), v.locked = xy::locked();
  v.x = xy::x(), v.y = xy::y();
  for (int t = 0; t < 12; ++t) {
    const int m = core::ui::remote_track_machine(t);
    v.tm[t] = (unsigned char)(m < 0 ? 0 : m == 0x7F ? 2 : 1);
  }
  for (int a = 0; a < xy::kAxes && a < 2; ++a)
    for (int i = 0; i < xy::kSlots && i < SXY_SLOTS; ++i) {
      if (c.s[a][i].t) xy::target_name(c.s[a][i].t, v.s[a][i].name, sizeof v.s[a][i].name);
      if (c.s[a][i].t == xy::kMorph) memcpy(v.s[a][i].label, v.s[a][i].name, sizeof v.s[a][i].label);
      else if (c.s[a][i].t) mod::label(c.s[a][i].t, v.s[a][i].label, sizeof v.s[a][i].label);
      v.s[a][i].depth = c.s[a][i].depth;
    }
  // the picker's param names (2026-11, the owner: names, not P1): as the screens name them now, built when asked (the
  // picker open), else the last ones
  static_assert(sizeof g_vl == sizeof v.vl && sizeof g_fl == sizeof v.fl && sizeof g_il == sizeof v.il &&
                sizeof g_rl == sizeof v.rl, "the tables");
  if (names) {
    for (int vi = 0; vi < 8; ++vi)
      for (int p = 0; p < 16; ++p) mod::param_label(mod::tid(mod::kV, vi, p), g_vl[vi][p], sizeof g_vl[vi][p]);
    for (int f = 0; f < 2; ++f)
      for (int p = 0; p < 4; ++p) mod::param_label(mod::tid(mod::kFx, f, p), g_fl[f][p], sizeof g_fl[f][p]);
    for (int ch = 0; ch < 13; ++ch)
      for (int p = 0; p < 12; ++p) mod::param_label(mod::tid(mod::kIns, ch, p), g_il[ch][p], sizeof g_il[ch][p]);
    for (int t = 0; t < 12; ++t)
      for (int p = 0; p < 8; ++p) {   // (by name: "RT3:P1")
        char n[12];
        mod::Tid id = 0;
        sniprintf(n, sizeof n, "RT%d:P%d", t + 1, p + 1);
        g_rl[t][p][0] = 0;
        if (mod::parse(n, &id)) mod::param_label(id, g_rl[t][p], sizeof g_rl[t][p]);
      }
    ++g_built;
  }
  memcpy(v.vl, g_vl, sizeof v.vl), memcpy(v.fl, g_fl, sizeof v.fl), memcpy(v.il, g_il, sizeof v.il), memcpy(v.rl, g_rl, sizeof v.rl);
  uint32_t h = 2166136261u;
  const unsigned char* b = (const unsigned char*)&v;
  for (size_t i = 0; i < offsetof(sidecar_xy_view_t, gen); ++i) h = (h ^ b[i]) * 16777619u;
  v.gen = h;
  *o = v;
}

void Tick() {
  uint32_t ms = core::now_ms();
  if (!g_want || ms - g_ms < 50) return;
  g_ms = ms;
  if (!g_names) g_named = false;
  bool names = g_names && (!g_named || ms - g_names_ms >= 250);   // (at once when it opens, then 4 a second)
  if (names) g_named = true, g_names_ms = ms;
  sidecar_xy_view_t v;
  FillView(&v, names);   // (built aside: the odd window is a copy long)
  uint32_t n = g_n;
  __atomic_store_n(&g_n, n + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);   // odd before the view's bytes
  memcpy(&g_buf, &v, sizeof v);
  __atomic_store_n(&g_n, n + 2, __ATOMIC_RELEASE);
}

bool View(sidecar_xy_view_t* out, unsigned* seen) {
  for (int tries = 0; tries < 4; ++tries) {
    uint32_t a = __atomic_load_n(&g_n, __ATOMIC_ACQUIRE);
    if (a == 0 || a == *seen) return false;
    if (a & 1) continue;   // being written
    memcpy(out, &g_buf, sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);   // the copy before the second look
    if (__atomic_load_n(&g_n, __ATOMIC_RELAXED) == a) {
      *seen = a;
      return true;
    }
  }
  return false;
}

void Want(bool on, bool names) { g_want = on, g_names = on && names; }
unsigned NamesBuilt() { return g_built; }

bool Quiet(const char* c) {
  return !strncmp(c, "xy=pos:", 7) || !strcmp(c, "xy=up") || !strncmp(c, "scene=fader:", 12);
}

}  // namespace xypad_pi
