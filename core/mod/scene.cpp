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

// ---- the files' state (Task 9)
char g_proj[24] = "";
int g_kit = -1;        // the kit the scenes are of (0..127), -1 none named
int g_rytm_kit = -1;   // the Rytm's active kit as it last said (kit_changed)
bool g_dirty = false, g_locked = false;
uint32_t g_dirty_ms = 0;
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
    g_st.fader = (uint16_t)a, g_played = g_from = (int)a;
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
  g_dirty = g_locked = false, g_kit = g_rytm_kit = -1;
}

// ---------------------------------------------------------------------------------------------------- the card

CORE_COLD bool dirty() { return g_dirty; }
CORE_COLD bool locked() { return g_locked; }
CORE_COLD int kit() { return g_kit; }
CORE_COLD void save_due() {
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
    log(CORE_STR("scene: %s not saved: it has lines this build does not read (kept as it is)"), fn);
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
  g_dirty = false;
  return true;
}
CORE_COLD bool save() { return save_text(false); }

CORE_COLD void project_changed() {
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
