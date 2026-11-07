// core/mod/scene.cpp - see scene.h. Control side only, cold: a tick hands the layer one value per lock of A and B
// (at most 128), from a merged list rebuilt only when the locks or A / B change.
#include "mod/scene.h"

#include <string.h>

#include "fmt/fmt.h"
#include "mod/mod.h"
#include "platform.h"
#include "pool/pool.h"
#include "project_key.h"
#include "ui/remote.h"
#if CORE_MSAMP
#include "bundle/rename.h"
#include "store/store.h"
#endif

namespace core {
namespace scene {

namespace {

State g_st;
// the played fader: from g_from at g_set_ms to g_st.fader g_smooth_ms later, linearly
int g_played = 0, g_from = 0;
uint32_t g_set_ms = 0, g_now_ms = 0;
uint32_t g_smooth_ms = kSmoothMs;   // this move's smoothing (the QPERF knob's: its reports' interval)
int g_src = 0;                      // the fader's last source (Source), 0 none
// the QPERF knob (Task 47): its macros; the position last acted on (a baseline or a take), -1 none since the link came
// up (Phase 7 review M2); whether a take since then sets the smoothing's interval, and when
uint16_t g_qp_mask = 0;
int g_qp_pos = -1;
bool g_qp_seen = false;
uint32_t g_qp_ms = 0;
int g_rscn = -1;                    // the Rytm's own scene as reported (2D 01 0F), -1 none
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

// ---- the files' state (Task 9)
char g_proj[24] = "";
int g_kit = -1;        // the kit the scenes are of (0..127), -1 none named
int g_rytm_kit = -1;   // the Rytm's active kit as it last said (kit_changed)
bool g_dirty = false, g_locked = false;
uint32_t g_dirty_ms = 0;
int g_file_fader = -1;
bool g_said = false;     // a locked file's "not saved" logged (once a load, review L8)   // the F the kit's file has (loaded or saved), -1 none (review M2)
constexpr uint32_t kTextMax = 20480;   // 768 locks of the longest names: ~18 KB
char g_text[kTextMax];

CORE_COLD void changed() { g_dirty = true, g_dirty_ms = now_ms(); }
CORE_COLD void clear_state() {   // no locks, no A / B; the fader where it is
  for (Scene& s : g_st.s) s.n = 0;
  g_st.a = g_st.b = -1, g_st.sel = 0;
  g_last_a = g_last_b = -1;
  g_mn = 0, g_stale = true;
}
CORE_COLD void name_of(const char* proj, char* fn, int cap, int kit) {
  char n[kKeyMax];
  project_key(proj, n, sizeof n);
  if (kit < 0) sniprintf(fn, cap, CORE_STR("/.rytm/scene/%s.txt"), n);
  else sniprintf(fn, cap, CORE_STR("/.rytm/scene/%s/k%03d.txt"), n, kit + 1);
}
CORE_COLD const char* skip(const char* p) {
  while (*p == ' ' || *p == '\t') ++p;
  return p;
}
CORE_COLD bool num(const char** pp, long* out) {   // a decimal (no sign); false: none
  const char* p = skip(*pp);
  if (*p < '0' || *p > '9') return false;
  long v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  *out = v, *pp = p;
  return true;
}
CORE_COLD void word(const char** pp, char* w, int cap) {
  const char* p = skip(*pp);
  int i = 0;
  for (; *p && *p != ' ' && *p != '\t'; ++p)
    if (i < cap - 1) w[i++] = *p;
  w[i] = 0, *pp = p;
}
// one line of the file; false: one this build does not read (the file then locks)
CORE_COLD bool take_line(const char* b) {
  const char* p = b;
  char k[8];
  word(&p, k, sizeof k);
  long a, c;
  if (!strcmp(k, "AB")) {
    if (!num(&p, &a) || !num(&p, &c) || a > kScenes || c > kScenes) return false;
    g_st.a = (int8_t)(a - 1), g_st.b = (int8_t)(c - 1);
  } else if (!strcmp(k, "F")) {
    if (!num(&p, &a) || a > kFaderMax) return false;
    g_st.fader = (uint16_t)a, g_file_fader = (int)a;   // (load smooths the played fader to it)
  } else if (!strcmp(k, "S")) {
    if (!num(&p, &a) || a < 1 || a > kScenes) return false;
    g_st.sel = (int8_t)(a - 1);
  } else if (!strcmp(k, "L")) {
    char nm[24];
    mod::Tid t;
    if (!num(&p, &a) || a < 1 || a > kScenes) return false;
    word(&p, nm, sizeof nm);
    if (!mod::parse(nm, &t) || !num(&p, &c) || c > kFaderMax) return false;
    Scene& s = g_st.s[a - 1];
    int i = find_lock(s, t);
    if (i < 0) {
      if (s.n >= kLocks) return false;
      i = s.n++;
    }
    s.l[i] = {t, (uint16_t)c};
  } else {
    return false;
  }
  p = skip(p);
  return !*p || *p == '#';
}
// the file's text into g_text: its length; 0 when it does not fit (cannot happen at 768 locks)
CORE_COLD uint32_t to_text() {
  int n = sniprintf(g_text, sizeof g_text, CORE_STR("#sidecar scene 1\nAB %d %d\nF %u\nS %d\n"), g_st.a + 1, g_st.b + 1,
                    (unsigned)g_st.fader, g_st.sel + 1);
  for (int sc = 0; sc < kScenes; ++sc)
    for (int i = 0; i < g_st.s[sc].n; ++i) {
      char nm[16];
      mod::name(g_st.s[sc].l[i].t, nm, sizeof nm);
      int k = sniprintf(g_text + n, sizeof g_text - n, CORE_STR("L %d %s %u\n"), sc + 1, nm, (unsigned)g_st.s[sc].l[i].v);
      if (k <= 0 || n + k >= (int)sizeof g_text) return 0;
      n += k;
    }
  return (uint32_t)n;
}

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
  changed();
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
  changed();
  return true;
}

CORE_COLD void clear(int n) {
  if (!ok_scene(n)) return;
  g_st.s[n].n = 0;
  g_stale = true;
  changed();
}

CORE_COLD void copy(int from, int to) {
  if (!ok_scene(from) || !ok_scene(to) || from == to) return;
  g_st.s[to] = g_st.s[from];
  g_stale = true;
  changed();
}

CORE_COLD void set_ab(int a, int b) {
  int8_t na = (int8_t)(ok_scene(a) ? a : -1), nb = (int8_t)(ok_scene(b) ? b : -1);
  if (na == g_st.a && nb == g_st.b) return;
  g_st.a = na, g_st.b = nb;
  g_stale = true;
  changed();
}

CORE_COLD void set_sel(int n) {
  if (!ok_scene(n) || n == g_st.sel) return;
  g_st.sel = (int8_t)n;
  changed();
}

namespace {
CORE_COLD void move_fader(uint16_t v, int source, uint32_t smooth_ms) {   // whichever moved last wins
  if (v > kFaderMax) v = kFaderMax;
  if (v == g_st.fader) return;
  g_from = g_played, g_set_ms = now_ms(), g_smooth_ms = smooth_ms;
  g_st.fader = v;
  g_src = source;
  mod::fader_moved();   // a held manual edit lets go (spec-050 2.5)
}
}  // namespace

CORE_COLD void set_fader(uint16_t v, int source) { move_fader(v, source, kSmoothMs); }

CORE_COLD uint16_t fader_played() { return (uint16_t)g_played; }
CORE_COLD int fader_source() { return g_src; }

CORE_COLD void qperf(int pos, uint16_t mask, uint8_t flags) {
  g_qp_mask = (uint16_t)(mask & 0xFFF);
  if (!(flags & kQpMoved) || pos < 0 || pos > 255) return;   // the hello's report before the knob ever sent: no position
  if (g_qp_pos < 0) {   // the first report since the link came up: a baseline only, the knob takes on a change (M2)
    g_qp_pos = pos;
    return;
  }
  int d = pos > g_qp_pos ? pos - g_qp_pos : g_qp_pos - pos;
  if (d == 0) return;   // FUNC or the mask changed: the fader stays where it is
  // another source has the fader (or a held manual edit plays): a 1-step flicker of the pot does not take it (M1)
  if ((g_src != kQperf || mod::held_any()) && d < kQpTakeSteps) return;
  uint16_t v = (uint16_t)(pos * kFaderMax / 255);
  uint32_t ms = now_ms(), gap = ms - g_qp_ms;
  uint32_t sm = g_qp_seen && gap <= kQpSmoothMaxMs ? (gap > kSmoothMs ? gap : kSmoothMs) : kSmoothMs;
  g_qp_seen = true, g_qp_pos = pos, g_qp_ms = ms;
  move_fader(v, kQperf, sm);
}
CORE_COLD void qperf_rebase() { g_qp_pos = -1, g_qp_seen = false; }
CORE_COLD uint16_t qperf_mask() { return g_src == kQperf ? g_qp_mask : 0; }
CORE_COLD void rytm_scene_seen(int n) { g_rscn = n >= 0 && n <= kScenes ? n : 0; }
CORE_COLD int rytm_scene() { return g_rscn; }

CORE_COLD void tick(uint32_t ms) {
  g_now_ms = ms;
  uint32_t el = ms - g_set_ms;
  int to = g_st.fader;
  g_played = el >= g_smooth_ms ? to : g_from + (to - g_from) * (int)el / (int)g_smooth_ms;
  float f = (float)g_played / (float)kFaderMax;
  constexpr float kH = 1.0f / 64.0f;
  if (g_st.a != g_last_a || g_st.b != g_last_b) {
    // the old scenes' targets that the new A / B do not lock: back to their bases before the new ones' entries are
    // allocated (spec-050 1.4: so an A / B change of full scenes stays within kEntries)
    const int olds[2] = {g_last_a, g_last_b};   // (no <initializer_list> on the Pi's hosted side)
    for (int old : olds) {
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
    // the Rytm's own scene / macros (kRytm) have no base the Sidecar can read: a one-sided lock acts on its own half
    // only (its value, past halfway with the hysteresis) and leaves the Rytm alone on the other half
    // a PERF macro the QPERF knob moves while it is the fader: the Rytm moves it itself (spec 2.7), not followed
    if (k == mod::kRytm && mod::param_of(m.t) >= 1 && (qperf_mask() >> (mod::param_of(m.t) - 1) & 1)) continue;
    if (k == mod::kRytm && (m.av < 0 || m.bv < 0)) {
      if (m.bv < 0 ? !g_upper : g_upper) mod::morph(m.t, m.bv < 0 ? a : b);
      continue;
    }
    if (m.av < 0) a = mod::base(m.t);
    else if (m.bv < 0) b = mod::base(m.t);
    float val = step ? (g_upper ? b : a) : a + (b - a) * f;
    if (step && k == mod::kV) val = (float)(int)(val + 0.5f);   // a list cell plays whole entries (review L13)
    mod::morph(m.t, val);
  }
}

CORE_COLD void reset() {
  memset(&g_st, 0, sizeof g_st);
  g_st.a = g_st.b = -1;
  g_played = g_from = 0;
  g_set_ms = now_ms(), g_now_ms = g_set_ms;
  g_smooth_ms = kSmoothMs, g_src = 0;
  g_qp_mask = 0, g_qp_pos = -1, g_qp_seen = false, g_qp_ms = 0;
  g_rscn = -1;
  g_upper = false;
  g_last_a = g_last_b = -1;
  g_mn = 0, g_stale = true;
  g_dirty = g_locked = false, g_kit = g_rytm_kit = -1;
}

// ---------------------------------------------------------------------------------------------------- the card

CORE_COLD bool dirty() { return g_dirty; }
CORE_COLD bool locked() { return g_locked; }

// ---------------------------------------------------------------------------------------------------- console, JSON

namespace {
bool g_edit = false;
// the next ':'-separated field of *pp into f (at most cap - 1 chars); false: none left
CORE_COLD bool field(const char** pp, char* f, int cap) {
  const char* p = *pp;
  if (!p) return false;
  int i = 0;
  for (; *p && *p != ':'; ++p)
    if (i < cap - 1) f[i++] = *p;
  f[i] = 0;
  *pp = *p == ':' ? p + 1 : nullptr;
  return i > 0;
}
CORE_COLD int dec(const char* s, int lo, int hi) {   // a whole number lo..hi, else lo - 1
  if (!*s) return lo - 1;
  long v = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || v > 100000) return lo - 1;
    v = v * 10 + (*s - '0');
  }
  return v < lo || v > hi ? lo - 1 : (int)v;
}
CORE_COLD int value128(const char* s) {   // "50", "50.5", "127.99": x 128, rounded; -1 bad
  long w = 0, f = 0, fd = 1;
  const char* p = s;
  if (*p < '0' || *p > '9') return -1;
  while (*p >= '0' && *p <= '9' && w < 1000) w = w * 10 + (*p++ - '0');
  if (*p == '.')
    for (++p; *p >= '0' && *p <= '9'; ++p)
      if (fd < 10000) f = f * 10 + (*p - '0'), fd *= 10;
  if (*p) return -1;
  long v = (w * 128 * fd + f * 128 + fd / 2) / fd;
  return v > kFaderMax ? -1 : (int)v;
}
CORE_COLD int state_line(char* o, int cap) {
  int n = sniprintf(o, cap, CORE_STR("scene: A %d B %d fader %u (%d played) sel %d edit %s"), g_st.a + 1, g_st.b + 1,
                    (unsigned)g_st.fader, g_played, g_st.sel + 1, g_edit ? "on" : "off");
  bool first = true;
  for (int i = 0; i < kScenes && n > 0 && n < cap; ++i) {
    if (!g_st.s[i].n) continue;
    n += sniprintf(o + n, cap - n, CORE_STR("%sS%d %d lock%s"), first ? ": " : ", ", i + 1, g_st.s[i].n, g_st.s[i].n == 1 ? "" : "s");
    first = false;
  }
  if (g_locked && n > 0 && n < cap) n += sniprintf(o + n, cap - n, CORE_STR(" - SCENES LOCKED (NEWER FILE)"));
  return n;
}
}  // namespace

CORE_COLD void set_edit(bool on) { g_edit = on; }
CORE_COLD bool edit() { return g_edit; }

CORE_COLD bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "scene", 5) != 0 || (p[5] && p[5] != '=')) return false;
  if (cap <= 0) return true;
  o[0] = 0;
  if (!p[5]) return state_line(o, cap), true;
  const char* q = p + 6;
  char k[8], a[8], b[12], c[12];
  bool ok = field(&q, k, sizeof k);
  bool full = false;
  if (!ok) {
  } else if (!strcmp(k, "ab")) {
    int x = field(&q, a, sizeof a) ? dec(a, 0, kScenes) : -1, y = field(&q, b, sizeof b) ? dec(b, 0, kScenes) : -1;
    ok = x >= 0 && y >= 0 && !q;
    if (ok) set_ab(x - 1, y - 1);
  } else if (!strcmp(k, "fader")) {
    int x = field(&q, a, sizeof a) ? dec(a, 0, kFaderMax) : -1;
    ok = x >= 0 && !q;
    if (ok) set_fader((uint16_t)x, kConsole);
  } else if (!strcmp(k, "sel") || !strcmp(k, "clear")) {
    int x = field(&q, a, sizeof a) ? dec(a, 1, kScenes) : 0;
    ok = x > 0 && !q;
    if (ok && k[0] == 's') set_sel(x - 1);
    else if (ok) clear(x - 1);
  } else if (!strcmp(k, "copy")) {
    int x = field(&q, a, sizeof a) ? dec(a, 1, kScenes) : 0, y = field(&q, b, sizeof b) ? dec(b, 1, kScenes) : 0;
    ok = x > 0 && y > 0 && !q;
    if (ok) copy(x - 1, y - 1);
  } else if (!strcmp(k, "lock") || !strcmp(k, "unlock")) {   // N:NAME:V, NAME itself "V1:P3"
    char nm[24], pn[12];
    int x = field(&q, a, sizeof a) ? dec(a, 1, kScenes) : 0;
    ok = x > 0 && field(&q, b, sizeof b) && field(&q, pn, sizeof pn);
    mod::Tid t = 0;
    if (ok) sniprintf(nm, sizeof nm, "%s:%s", b, pn), ok = mod::parse(nm, &t);
    if (ok && k[0] == 'l') {
      int v = field(&q, c, sizeof c) ? value128(c) : -1;
      ok = v >= 0 && !q;
      if (ok && !lock(x - 1, t, (uint16_t)v)) ok = false, full = true;
    } else if (ok) {
      ok = !q && unlock(x - 1, t);
    }
  } else if (!strcmp(k, "edit")) {
    ok = field(&q, a, sizeof a) && !q && (!strcmp(a, "on") || !strcmp(a, "off"));
    if (ok) set_edit(a[1] == 'n');
  } else {
    ok = false;
  }
  if (full) sniprintf(o, cap, CORE_STR("scene: SCENE FULL (%d locks)"), kLocks);
  else if (!ok)
    sniprintf(o, cap, CORE_STR("scene: cannot \"%s\" (scene=ab:A:B | fader:V | sel:N | lock:N:NAME:V | unlock:N:NAME | "
                               "clear:N | copy:N:M | edit:on|off)"), p);
  else state_line(o, cap);
  return true;
}

CORE_COLD int json(char* o, int cap) {
  int n = sniprintf(o, cap, CORE_STR("\"scene\":{\"a\":%d,\"b\":%d,\"fader\":%u,\"played\":%d,\"sel\":%d,\"locked\":%d,"
                                     "\"edit\":%d,\"s\":["), g_st.a + 1, g_st.b + 1, (unsigned)g_st.fader, g_played,
                    g_st.sel + 1, g_locked ? 1 : 0, g_edit ? 1 : 0);
  bool first = true;
  for (int i = 0; i < kScenes; ++i) {
    const Scene& s = g_st.s[i];
    if (!s.n) continue;
    if (n < 0 || n >= cap) return cap;
    n += sniprintf(o + n, cap - n, CORE_STR("%s{\"n\":%d,\"l\":["), first ? "" : ",", i + 1);
    first = false;
    for (int j = 0; j < s.n; ++j) {
      char nm[16];
      mod::name(s.l[j].t, nm, sizeof nm);
      if (n >= cap) return cap;
      n += sniprintf(o + n, cap - n, CORE_STR("%s[\"%s\",%u]"), j ? "," : "", nm, (unsigned)s.l[j].v);
    }
    if (n >= cap) return cap;
    n += sniprintf(o + n, cap - n, "]}");
  }
  if (n >= cap) return cap;
  n += sniprintf(o + n, cap - n, CORE_STR("],\"src\":%d,\"rscn\":%d}"), g_src, g_rscn);
  return n >= cap ? cap : n;
}
CORE_COLD int kit() { return g_kit; }
// spec 2.8: the fader's position goes out with a kit change or a project leave, though a fader move alone is no edit (a
// kit with a file or locks: a kit with neither gets no file for its position alone)
static CORE_COLD bool fader_owed() {
  if (g_locked) return false;
  bool any = g_file_fader >= 0;
  for (const Scene& sc : g_st.s) any = any || sc.n;
  return any && g_st.fader != g_file_fader;
}
CORE_COLD void save_due() {
  if (!g_dirty && fader_owed()) g_dirty = true;
  if (g_dirty) g_dirty_ms = now_ms() - 1000;
}

#if CORE_MSAMP
CORE_COLD void rekey(const char* name) { sniprintf(g_proj, sizeof g_proj, "%s", name ? name : ""); }
CORE_COLD void want_files() {
  char fn[96];
  for (int k = -1; k < 128; ++k) name_of(project, fn, sizeof fn, k), store::want(fn);
}

CORE_COLD void load() {
  mod::restore_morphs();   // the old scenes' targets back to their bases now (an A / B of full scenes stays in kEntries)
  clear_state();
  const uint16_t was = g_st.fader;
  g_file_fader = -1, g_said = false;
  g_locked = g_dirty = false;
  strcpy(g_proj, project);   // the scenes are this project's and the Rytm's kit's from now on
  g_kit = g_rytm_kit;
  char fn[96], b[96];
  name_of(g_proj, fn, sizeof fn, g_kit);
  const uint8_t* d;
  uint32_t len, at = 0;
  bool own = true;   // the file read is the one save() writes
  store::State st = store::get(fn, &d, &len);
  if (st == store::kAbsent && g_kit >= 0) {   // a kit without its own file: the project's
    name_of(g_proj, fn, sizeof fn, -1), own = false;
    st = store::get(fn, &d, &len);
  }
  if (st == store::kAbsent) return;   // none: no scenes
  if (st != store::kPresent) {   // there but unreadable, or never filled: never taken for absent
    if (own) g_locked = true;
    log(CORE_STR("scene: %s %s: no scenes%s"), fn, st == store::kLocked ? "cannot be read" : "not in the store",
        own ? ", nothing written" : "");
    return;
  }
  int n = 0, bad = 0;
  fmt::Sniffed sn;   // a newer build's version: read what can be read, never written
  if (fmt::text_header(fmt::Bytes{d, len}, "scene", &sn) && sn.version > 1) ++bad;
  for (int k; (k = fmt::next_line(d, len, &at, b, sizeof b)) >= 0;) {
    if (k == 0 || b[0] == '#') continue;
    if (take_line(b)) ++n;
    else ++bad;
  }
  if (bad && own) g_locked = true;
  if (g_st.fader != was) g_from = g_played, g_set_ms = now_ms(), g_smooth_ms = kSmoothMs;   // a file's F: over 20 ms (M2)
  g_last_a = g_st.a, g_last_b = g_st.b;   // (restored above: nothing of the old A / B to release)
  g_upper = g_played >= kFaderMax / 2 + 1;
  log(CORE_STR("scene: %d lines from %s%s"), n, fn,
      bad ? (own ? " (lines it does not read: the file is kept, not written)" : " (lines it does not read)") : "");
}

// posted to the card core as one atomic write (fmt.h), its text put into the store first (a kit change back reads it
// from RAM before the card has it); queue (a kit change's save) or a text over kPostMax: fmt::post_queue, no wait
static CORE_COLD bool save_text(bool queue) {
  if (!g_dirty || now_ms() - g_dirty_ms < 1000) return false;
  char fn[96];
  name_of(g_proj, fn, sizeof fn, g_kit);
  if (g_locked) {   // 1042
    g_dirty = false;
    if (!g_said) log(CORE_STR("scene: %s not saved: it has lines this build does not read (kept as it is)"), fn);
    g_said = true;
    return true;
  }
  uint32_t n = to_text();
  if (!n) {
    g_dirty = false;
    log(CORE_STR("scene: %s not written: its text does not fit (kept as it is)"), fn);
    return true;
  }
  // the store is the project's now named; an old project's file saved while the next one's store fills is not put in it
  if (!strcmp(g_proj, project)) store::put(fn, (const uint8_t*)g_text, n);
  if (queue || n > fmt::kPostMax || fmt::post_stalled()) {
    if (!fmt::post_queue(fn, g_text, n)) log(CORE_STR("scene: %s not written to the card (refused or out of memory: in RAM only)"), fn);
  } else {
    char* p = fmt::post_begin();
    if (!p) return false;   // another settings write is in flight: at a later pass (still dirty)
    memcpy(p, g_text, n);
    if (!fmt::post_end(fn, n)) return false;
  }
  g_dirty = false, g_file_fader = g_st.fader;
  return true;
}
CORE_COLD bool save() { return save_text(false); }

CORE_COLD void project_changed() {
  if (!g_dirty && fader_owed()) g_dirty = true;   // the fader's position too (spec 2.8)
  if (g_dirty) g_dirty_ms = now_ms() - 1000, save_text(true);   // the old one's edits first, into its own file
  load();
}

CORE_COLD void kit_changed(int kit) {
  if (kit < 0 || kit > 127) return;
  g_rytm_kit = kit;
  // a project's store still filling: its load takes this kit; a rename moving the files: the switch after it does
  if (!store::ready() || rename::moving()) return;
  if (kit != g_kit) project_changed();
}
#else
CORE_COLD void load() {}
CORE_COLD bool save() { return false; }
CORE_COLD void project_changed() {}
CORE_COLD void kit_changed(int kit) { g_rytm_kit = kit; }
#endif

}  // namespace scene
}  // namespace core
