// pi/engine/morph_pi.cpp - see morph_pi.h. Hosted (the core's flags).
#include "morph_pi.h"

#include <stddef.h>
#include <string.h>

#include "mod/scene.h"
#include "mod/target.h"
#include "platform.h"
#include "ui/extra.h"

namespace morph_pi {

namespace {
sidecar_morph_view_t g_buf[2];
volatile uint32_t g_n = 0;
volatile bool g_want = false;
volatile int g_top = 0;
uint32_t g_ms = 0;
}  // namespace

void FillView(sidecar_morph_view_t* o, int top) {
  namespace sc = core::scene;
  namespace mod = core::mod;
  const sc::State& st = sc::state();
  sidecar_morph_view_t v;
  memset(&v, 0, sizeof v);
  v.valid = 1;
  v.fader = st.fader, v.a = st.a, v.b = st.b, v.sel = st.sel;
  v.edit = sc::edit(), v.locked = sc::locked();
  for (int k = 0; k < sc::kScenes && k < SMO_SCENES; ++k) v.nlocks[k] = st.s[k].n;
  const sc::Scene& s = st.s[st.sel >= 0 && st.sel < sc::kScenes ? st.sel : 0];
  top = top < 0 ? 0 : top >= s.n ? (s.n > 0 ? s.n - 1 : 0) : top;
  v.top = (unsigned char)top;
  for (int i = top; i < s.n && v.nrows < SMO_ROWS; ++i) {
    sidecar_morph_row_t& r = v.row[v.nrows++];
    mod::name(s.l[i].t, r.name, sizeof r.name);
    core::ui::morph_value_text(s.l[i].t, s.l[i].v, r.value, sizeof r.value);
    mod::Range g = mod::range(s.l[i].t);
    bool off = mod::is_offset(mod::kind_of(s.l[i].t));
    r.lo = (short)(off || g.lo < 0 ? 0 : (int)g.lo), r.hi = (short)(off || g.hi > 127 ? 127 : (int)g.hi);
    if (r.hi <= r.lo) r.hi = (short)(r.lo + 1);
    int u = (s.l[i].v + 64) / 128;
    u = u < r.lo ? r.lo : u > r.hi ? r.hi : u;
    r.v = (unsigned char)((u - r.lo) * 127 / (r.hi - r.lo));
  }
  uint32_t h = 2166136261u;
  const unsigned char* b = (const unsigned char*)&v;
  for (size_t i = 0; i < offsetof(sidecar_morph_view_t, gen); ++i) h = (h ^ b[i]) * 16777619u;
  v.gen = h;
  *o = v;
}

void Tick() {
  uint32_t ms = core::now_ms();
  if (!g_want || ms - g_ms < 50) return;
  g_ms = ms;
  uint32_t n = g_n;
  FillView(&g_buf[(n / 2 + 1) & 1], g_top);
  __atomic_store_n(&g_n, n + 2, __ATOMIC_RELEASE);   // the reader takes g_buf[(n / 2) & 1]
}

bool View(sidecar_morph_view_t* out, unsigned* seen) {
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

void Want(bool on, int top) { g_want = on, g_top = top; }

}  // namespace morph_pi
