// pi/engine/loopview_pi.cpp - see loopview_pi.h. Hosted (the core's flags).
#include "loopview_pi.h"

#include <stddef.h>
#include <string.h>

#include "loop/loop.h"
#include "platform.h"

namespace loopview_pi {

namespace {
// a sequence lock - core 3 makes g_n odd, writes g_buf, makes it even again; core 0 copies while it is even and unchanged
// across the copy (0: nothing published yet)
sidecar_loop_view_t g_buf;
volatile uint32_t g_n = 0;
volatile bool g_want = false;
volatile int g_sel = 0;
uint32_t g_ms = 0;
}  // namespace

void FillView(sidecar_loop_view_t* o, int sel) {
  namespace lp = core::loop;
  sidecar_loop_view_t v;
  memset(&v, 0, sizeof v);
  v.valid = 1, v.on = lp::on() ? 1 : 0;
  v.playing = -1;
  if (v.on) {
    v.state = (unsigned char)lp::state(), v.pending = lp::pending() ? 1 : 0;
    int s = lp::ring_seconds();
    v.ring_s = (unsigned char)(s > 87 ? 87 : s);
    v.bal = (unsigned char)lp::bal(), v.lev = (unsigned char)lp::lev(), v.fade = (unsigned char)lp::fade();
    v.mute = lp::auto_mute() ? 1 : 0, v.live = (unsigned char)lp::live_via();
    float bar, of;
    if (lp::bar_position(&bar, &of)) v.pos10 = (short)(bar * 10.0f), v.bars10 = (short)(of * 10.0f + 0.5f);
    v.playing = (signed char)lp::playing();
    v.ngrabs = (unsigned char)lp::grabs();
    for (int i = 0; i < lp::grabs() && i < SLP_GRABS; ++i) {
      const lp::Grab* g = lp::grab_at(i);
      v.g[i].hb = g->half_bars, v.g[i].kept = g->kept, v.g[i].gap = g->gap, v.g[i].copied = g->copied;
      v.g[i].id = g->id;
      v.g[i].bpm10 = (unsigned short)(g->bpm * 10.0f + 0.5f);
    }
    // the message shows for a few seconds after it came
    static uint32_t s_gen = 0, s_ms = 0;
    uint32_t mg = lp::message_gen(), ms = core::now_ms();
    if (mg != s_gen) s_gen = mg, s_ms = ms;
    if (mg && ms - s_ms < 3000) strncpy(v.msg, lp::last_message(), sizeof v.msg - 1), v.msg[sizeof v.msg - 1] = 0;
  }
  v.sel = (signed char)(sel < 0 ? 0 : sel >= v.ngrabs ? (v.ngrabs ? v.ngrabs - 1 : 0) : sel);
  uint32_t h = 2166136261u;
  const unsigned char* b = (const unsigned char*)&v;
  for (size_t i = 0; i < offsetof(sidecar_loop_view_t, gen); ++i) h = (h ^ b[i]) * 16777619u;
  v.gen = h;
  *o = v;
}

void Tick() {
  uint32_t ms = core::now_ms();
  if (!g_want || ms - g_ms < 50) return;
  g_ms = ms;
  sidecar_loop_view_t v;
  FillView(&v, g_sel);   // (built aside: the odd window is a copy long)
  uint32_t n = g_n;
  __atomic_store_n(&g_n, n + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);   // odd before the view's bytes
  memcpy(&g_buf, &v, sizeof v);
  __atomic_store_n(&g_n, n + 2, __ATOMIC_RELEASE);
}

bool View(sidecar_loop_view_t* out, unsigned* seen) {
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

void Want(bool on, int sel) { g_want = on, g_sel = sel; }

}  // namespace loopview_pi
