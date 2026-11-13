// core/mod/xypad.cpp - see xypad.h. Control side only, cold: a tick hands the layer at most 8 offsets.
#include "mod/xypad.h"

#include <string.h>

#include "fmt/fmt.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/scene.h"
#include "platform.h"
#include "pool/pool.h"
#include "project_key.h"
#if CORE_MSAMP
#include "bundle/rename.h"
#include "store/store.h"
#endif

namespace core {
namespace xypad {

namespace {

Cfg g_cfg;
bool g_touch = false;    // a finger on the pad
bool g_held = false;     // HOLD on and lifted: the values stay
bool g_stale = false;    // a kit / project change while touching: the finger's positions ignored until its up
// a RYTM target let go (HOLD off's release, a slot cleared or retargeted): offered at its zero point until rytm_out has
// had its kSpacingMs to send it (a value offered sooner waits there, and a target the layer drops is forgotten unsent)
struct Settle {
  mod::Tid t;
  float v;
};
Settle g_settle[kAxes * kSlots];
int g_nsettle = 0;
uint32_t g_settle_ms = 0;
int g_settle_ticks = 0;   // ticks that offered it since: it goes after 2 and the spacing (a late tick still offers)
uint16_t g_pos[kAxes];
int g_fbase = -1;        // MORPH: the fader before the touch, -1 none

CORE_COLD bool ok_axis(int a) { return a >= 0 && a < kAxes; }
CORE_COLD bool ok_slot(int s) { return s >= 0 && s < kSlots; }
CORE_COLD bool ok_target(mod::Tid t) { return t == kMorph || mod::valid(t); }
// a target with no base the layer reads: the pad sets it from its range's end (xypad.h)
CORE_COLD bool value_only(mod::Kind k) { return k == mod::kSeq || k == mod::kArp || k == mod::kLoop || k == mod::kRytm; }
CORE_COLD bool any_morph() {
  for (const auto& ax : g_cfg.s)
    for (const Slot& s : ax)
      if (s.t == kMorph) return true;
  return false;
}
CORE_COLD void settle(const Slot& s) {   // a RYTM slot's target to its zero point (the value at position 0)
  if (!s.t || s.t == kMorph || !mod::valid(s.t) || mod::kind_of(s.t) != mod::kRytm) return;
  mod::Range r = mod::range(s.t);
  int i = 0;
  while (i < g_nsettle && g_settle[i].t != s.t) ++i;
  if (i == g_nsettle) {
    if (g_nsettle == kAxes * kSlots) return;
    ++g_nsettle;
  }
  g_settle[i] = {s.t, s.depth < 0 ? r.hi : r.lo};
  g_settle_ms = now_ms(), g_settle_ticks = 0;
}
CORE_COLD void apply_fader() {   // MORPH: its base + each MORPH slot's depth x its axis, clamped
  if (g_fbase < 0) return;
  long v = g_fbase;
  bool any = false;
  for (int a = 0; a < kAxes; ++a)
    for (const Slot& s : g_cfg.s[a])
      if (s.t == kMorph) v += (long)s.depth * g_pos[a] / 100, any = true;
  if (!any) return;
  scene::set_fader((uint16_t)(v < 0 ? 0 : v > scene::kFaderMax ? scene::kFaderMax : v), scene::kTouch);
}
// the held or touching state ends as HOLD off ends it: the offsets go (at the next tick), RYTM once to its zero point,
// MORPH back to its base unless another source has moved the fader since
CORE_COLD void release() {
  g_held = false;
  for (const auto& ax : g_cfg.s)
    for (const Slot& s : ax) settle(s);
  if (g_fbase >= 0 && scene::fader_source() == scene::kTouch) scene::set_fader((uint16_t)g_fbase, scene::kTouch);
  g_fbase = -1;
}
// MORPH slots changed while active: the fader from the ones left, or back to its base when none is left
CORE_COLD void morph_changed() {
  if (!g_touch && !g_held) return;
  if (any_morph()) {
    apply_fader();
  } else if (g_fbase >= 0) {
    if (scene::fader_source() == scene::kTouch) scene::set_fader((uint16_t)g_fbase, scene::kTouch);
    g_fbase = -1;
  }
}
// a kit or project change: released first (as an up); a held state is the old kit's: dropped, the fader left as it is.
// No RYTM settle: the Rytm has the new kit by now, whose macros the old kit's zero points are not
CORE_COLD void switch_release() {
  if (g_touch) up(), g_stale = true;
  if (g_held) g_held = false, g_fbase = -1;
  g_nsettle = 0;
}

// ---- the files' state (as the scenes')
char g_proj[24] = "";
int g_kit = -1;        // the kit the setup is of (0..127), -1 none named
int g_rytm_kit = -1;   // the Rytm's active kit as it last said (kit_changed)
bool g_dirty = false, g_locked = false, g_said = false;
uint32_t g_dirty_ms = 0;
char g_text[512];      // 8 slots of the longest names: ~200 bytes

CORE_COLD void changed() { g_dirty = true, g_dirty_ms = now_ms(); }
CORE_COLD void name_of(const char* proj, char* fn, int cap, int kit) {
  char n[kKeyMax];
  project_key(proj, n, sizeof n);
  if (kit < 0) sniprintf(fn, cap, CORE_STR("/.rytm/xy/%s.txt"), n);
  else sniprintf(fn, cap, CORE_STR("/.rytm/xy/%s/k%03d.txt"), n, kit + 1);
}
CORE_COLD const char* skip(const char* p) {
  while (*p == ' ' || *p == '\t') ++p;
  return p;
}
CORE_COLD bool word(const char** pp, char* w, int cap) {   // false: longer than w (cut: never read as another value)
  const char* p = skip(*pp);
  int i = 0;
  bool fit = true;
  for (; *p && *p != ' ' && *p != '\t'; ++p)
    if (i < cap - 1) w[i++] = *p;
    else fit = false;
  w[i] = 0, *pp = p;
  return fit;
}
// a whole number lo..hi, an optional sign; false: none, or out of range
CORE_COLD bool snum(const char* s, int lo, int hi, int* out) {
  bool neg = *s == '-';
  if (*s == '-' || *s == '+') ++s;
  if (*s < '0' || *s > '9') return false;
  long v = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || v > 100000) return false;
    v = v * 10 + (*s - '0');
  }
  if (neg) v = -v;
  if (v < lo || v > hi) return false;
  *out = (int)v;
  return true;
}
// one line of the file; false: one this build does not read (the file then locks)
CORE_COLD bool take_line(const char* b) {
  const char* p = b;
  char k[4], a[8], nm[24], d[8];
  if (!word(&p, k, sizeof k)) return false;
  int n, dep;
  if (!strcmp(k, "H")) {
    if (!word(&p, a, sizeof a) || !snum(a, 0, 1, &n)) return false;
    g_cfg.hold = n == 1;
  } else if (!strcmp(k, "X") || !strcmp(k, "Y")) {
    mod::Tid t;
    if (!word(&p, a, sizeof a) || !word(&p, nm, sizeof nm) || !word(&p, d, sizeof d)) return false;
    if (!snum(a, 1, kSlots, &n) || !parse_target(nm, &t) || !snum(d, -100, 100, &dep)) return false;
    g_cfg.s[k[0] == 'Y'][n - 1] = {t, (int8_t)dep};
  } else {
    return false;
  }
  p = skip(p);
  return !*p || *p == '#';
}
CORE_COLD uint32_t to_text() {   // the file's text into g_text: its length (0: it does not fit, cannot happen)
  int n = sniprintf(g_text, sizeof g_text, CORE_STR("#sidecar xy 1\nH %d\n"), g_cfg.hold ? 1 : 0);
  for (int a = 0; a < kAxes; ++a)
    for (int i = 0; i < kSlots; ++i) {
      const Slot& s = g_cfg.s[a][i];
      if (!s.t) continue;
      char nm[16];
      target_name(s.t, nm, sizeof nm);
      int k = sniprintf(g_text + n, sizeof g_text - n, CORE_STR("%c %d %s %d\n"), a ? 'Y' : 'X', i + 1, nm, s.depth);
      if (k <= 0 || n + k >= (int)sizeof g_text) return 0;
      n += k;
    }
  return (uint32_t)n;
}
CORE_COLD void clear_cfg() { memset(&g_cfg, 0, sizeof g_cfg); }

}  // namespace

CORE_COLD const Cfg& cfg() { return g_cfg; }

CORE_COLD const char* target_name(mod::Tid t, char* b, int cap) {
  if (cap <= 0) return b;
  if (t == kMorph) sniprintf(b, cap, "MORPH");
  else mod::name(t, b, cap);
  return b;
}
CORE_COLD bool parse_target(const char* s, mod::Tid* out) {
  if (!s) return false;
  static const char kM[] = "MORPH";
  int i = 0;
  while (s[i] && kM[i] && (s[i] == kM[i] || s[i] == kM[i] + 32)) ++i;
  if (!s[i] && !kM[i]) return *out = kMorph, true;
  return mod::parse(s, out);
}

CORE_COLD bool set(int axis, int slot, mod::Tid t, int depth) {
  if (!ok_axis(axis) || !ok_slot(slot) || !ok_target(t) || depth < -100 || depth > 100) return false;
  Slot& s = g_cfg.s[axis][slot];
  if (t == kMorph && s.t != kMorph && (g_touch || g_held) && g_fbase < 0) g_fbase = scene::state().fader;
  if (s.t != t && (g_touch || g_held)) settle(s);   // a RYTM target retargeted away: its macro back
  s = {t, (int8_t)depth};
  morph_changed();
  changed();
  return true;
}
CORE_COLD bool clear(int axis, int slot) {
  if (!ok_axis(axis) || !ok_slot(slot)) return false;
  if (g_touch || g_held) settle(g_cfg.s[axis][slot]);
  g_cfg.s[axis][slot] = Slot();
  morph_changed();
  changed();
  return true;
}
CORE_COLD void set_hold(bool on) {
  if (on == g_cfg.hold) return;
  g_cfg.hold = on;
  if (!on && g_held) release();
  changed();
}

CORE_COLD void pos(uint16_t x, uint16_t y) {
  if (g_stale) return;
  if (!g_touch) {   // a new touch: it replaces a held state; MORPH's base is the fader now unless the pad still has it
    g_touch = true, g_held = false;
    if (any_morph() && (g_fbase < 0 || scene::fader_source() != scene::kTouch)) g_fbase = scene::state().fader;
  }
  g_pos[0] = x > kMax ? kMax : x, g_pos[1] = y > kMax ? kMax : y;
  apply_fader();
}
CORE_COLD void up() {
  if (g_stale) {   // the finger of a touch a kit change released
    g_stale = false;
    return;
  }
  if (!g_touch) return;
  g_touch = false;
  if (g_cfg.hold) g_held = true;
  else release();
}
CORE_COLD bool touching() { return g_touch; }
CORE_COLD bool active() { return g_touch || g_held; }
CORE_COLD uint16_t x() { return g_pos[0]; }
CORE_COLD uint16_t y() { return g_pos[1]; }

CORE_COLD void tick() {
  bool on = g_touch || g_held;
  if (!on && !g_nsettle) return;
  mod::Tid seen[kAxes * kSlots];   // the value-only targets the slots drive: each from its range's end once
  int ns = 0;
  for (int a = 0; on && a < kAxes; ++a) {
    float w = (float)g_pos[a] / (float)kMax;
    for (const Slot& s : g_cfg.s[a]) {
      if (!s.t || s.t == kMorph || !mod::valid(s.t)) continue;
      mod::Kind k = mod::kind_of(s.t);
      mod::Range r = mod::range(s.t);
      float d = (float)s.depth / 100.0f, span = mod::is_offset(k) ? 127.0f : r.hi - r.lo;
      if (value_only(k)) {
        int i = 0;
        while (i < ns && seen[i] != s.t) ++i;
        if (i == ns) seen[ns++] = s.t, mod::offset(kSource, s.t, 1.0f, d < 0 ? r.hi : r.lo);   // from its range's end
      }
      mod::offset(kSource, s.t, d * span, w);
    }
  }
  for (int j = 0; j < g_nsettle; ++j) {   // a let-go RYTM target no slot drives now: its zero point
    int i = 0;
    while (i < ns && seen[i] != g_settle[j].t) ++i;
    if (i == ns) mod::offset(kSource, g_settle[j].t, 1.0f, g_settle[j].v);
  }
  // offered twice and the spacing passed: rytm_out has sent them by now (checked after the offer: a late tick offers)
  if (g_nsettle && ++g_settle_ticks >= 2 && now_ms() - g_settle_ms > mod::kSpacingMs + 1) g_nsettle = 0;
}

CORE_COLD void reset() {
  clear_cfg();
  g_touch = g_held = g_stale = false;
  g_nsettle = 0;
  g_pos[0] = g_pos[1] = 0;
  g_fbase = -1;
  g_dirty = g_locked = false, g_kit = g_rytm_kit = -1;
}

// ---------------------------------------------------------------------------------------------------- console

namespace {
CORE_COLD bool field(const char** pp, char* f, int cap) {   // the next ':'-separated field; false: none left
  const char* p = *pp;
  if (!p) return false;
  int i = 0;
  for (; *p && *p != ':'; ++p)
    if (i < cap - 1) f[i++] = *p;
  f[i] = 0;
  *pp = *p == ':' ? p + 1 : nullptr;
  return i > 0;
}
CORE_COLD int axis_of(const char* s) { return !strcmp(s, "x") || !strcmp(s, "X") ? 0 : !strcmp(s, "y") || !strcmp(s, "Y") ? 1 : -1; }
CORE_COLD int state_line(char* o, int cap) {
  int n = sniprintf(o, cap, CORE_STR("xy: hold %s, %s %u %u"), g_cfg.hold ? "on" : "off",
                    g_touch ? "touch" : g_held ? "held" : "idle", (unsigned)g_pos[0], (unsigned)g_pos[1]);
  for (int a = 0; a < kAxes; ++a)
    for (int i = 0; i < kSlots && n > 0 && n < cap; ++i) {
      const Slot& s = g_cfg.s[a][i];
      if (!s.t) continue;
      char nm[16];
      n += sniprintf(o + n, cap - n, CORE_STR(", %c%d %s %s%d"), a ? 'Y' : 'X', i + 1, target_name(s.t, nm, sizeof nm),
                    s.depth < 0 ? "-" : "+", s.depth < 0 ? -s.depth : s.depth);
    }
  if (g_locked && n > 0 && n < cap) n += sniprintf(o + n, cap - n, CORE_STR(" - XY LOCKED (NEWER FILE)"));
  return n;
}
}  // namespace

CORE_COLD bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "xy", 2) != 0 || (p[2] && p[2] != '=')) return false;
  if (cap <= 0) return true;
  o[0] = 0;
  if (!p[2]) return state_line(o, cap), true;
  const char* q = p + 3;
  char k[8], a[8], b[8];
  bool ok = field(&q, k, sizeof k);
  int v1, v2;
  if (!ok) {
  } else if (!strcmp(k, "pos")) {   // quiet: every 30 ms while a finger drags
    ok = field(&q, a, sizeof a) && field(&q, b, sizeof b) && !q && snum(a, 0, 1000000, &v1) && snum(b, 0, 1000000, &v2);
    if (ok) {
      pos((uint16_t)(v1 > kMax ? kMax : v1), (uint16_t)(v2 > kMax ? kMax : v2));
      return true;
    }
  } else if (!strcmp(k, "up")) {
    ok = !q;
    if (ok) {
      up();
      return true;
    }
  } else if (!strcmp(k, "hold")) {
    ok = field(&q, a, sizeof a) && !q && snum(a, 0, 1, &v1);
    if (ok) set_hold(v1 == 1);
  } else if (!strcmp(k, "clear")) {
    int ax = field(&q, a, sizeof a) ? axis_of(a) : -1;
    ok = ax >= 0 && field(&q, b, sizeof b) && !q && snum(b, 1, kSlots, &v1) && clear(ax, v1 - 1);
  } else if (!strcmp(k, "set")) {   // x|y:SLOT:TARGET:DEPTH, TARGET itself "RYTM:PERF1" (or "MORPH")
    int ax = field(&q, a, sizeof a) ? axis_of(a) : -1;
    ok = ax >= 0 && field(&q, b, sizeof b) && snum(b, 1, kSlots, &v1) && q;
    const char* last = ok ? strrchr(q, ':') : nullptr;
    ok = ok && last && last > q && last - q < 24;
    char nm[24];
    mod::Tid t = 0;
    if (ok) {
      memcpy(nm, q, (size_t)(last - q)), nm[last - q] = 0;
      ok = parse_target(nm, &t) && snum(last + 1, -100, 100, &v2) && set(ax, v1 - 1, t, v2);
    }
  } else {
    ok = false;
  }
  if (!ok)
    sniprintf(o, cap, CORE_STR("xy: cannot \"%s\" (xy=pos:X:Y | up | set:x|y:SLOT:TARGET:DEPTH | clear:x|y:SLOT | hold:0|1)"), p);
  else state_line(o, cap);
  log(CORE_STR("xy: %s%s"), p + 3, ok ? "" : " (refused)");   // set / clear / hold (a diagnosis's trail; pos / up stay quiet)
  return true;
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
  clear_cfg();
  g_locked = g_dirty = g_said = false;
  strcpy(g_proj, project);   // the setup is this project's and the Rytm's kit's from now on
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
  if (st == store::kAbsent) return;
  if (st != store::kPresent) {   // there but unreadable, or never filled: never taken for absent
    if (own) g_locked = true;
    log(CORE_STR("xy: %s %s: no setup%s"), fn, st == store::kLocked ? "cannot be read" : "not in the store",
        own ? ", nothing written" : "");
    return;
  }
  int n = 0, bad = 0;
  fmt::Sniffed sn;   // a newer build's version: read what can be read, never written
  if (fmt::text_header(fmt::Bytes{d, len}, "xy", &sn) && sn.version > 1) ++bad;
  for (int k; (k = fmt::next_line(d, len, &at, b, sizeof b)) >= 0;) {
    const char* q = skip(b);
    if (!*q || *q == '#') continue;   // blank, or a comment (indented too)
    if (take_line(q)) ++n;
    else ++bad;
  }
  if (bad && own) g_locked = true;
  log(CORE_STR("xy: %d lines from %s%s"), n, fn,
      bad ? (own ? " (lines it does not read: the file is kept, not written)" : " (lines it does not read)") : "");
}

// as the scenes' save_text (scene.cpp): the store first, then one posted write (queue: a kit change's save)
static CORE_COLD bool save_text(bool queue) {
  if (!g_dirty || now_ms() - g_dirty_ms < 1000) return false;
  char fn[96];
  name_of(g_proj, fn, sizeof fn, g_kit);
  if (g_locked) {   // 1042
    g_dirty = false;
    if (!g_said) log(CORE_STR("xy: %s not saved: it has lines this build does not read (kept as it is)"), fn);
    g_said = true;
    return true;
  }
  uint32_t n = to_text();
  if (!n) {
    g_dirty = false;
    return true;
  }
  if (!strcmp(g_proj, project)) store::put(fn, (const uint8_t*)g_text, n);
  if (queue || fmt::post_stalled()) {
    if (!fmt::post_queue(fn, g_text, n)) log(CORE_STR("xy: %s not written to the card (refused or out of memory: in RAM only)"), fn);
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
  // SAVE / RELOAD KIT of the live kit (kits' modules_again) is no change: the finger and a held state stay
  bool same = !strcmp(g_proj, project) && g_kit == g_rytm_kit;
  if (!same) switch_release();
  if (g_dirty) g_dirty_ms = now_ms() - 1000, save_text(true);   // the old one's edits first, into its own file
  load();
  if (same) morph_changed();   // (the setup read again: the fader from its MORPH slots, if any are left)
}

CORE_COLD void kit_changed(int kit) {
  if (kit < 0 || kit > 127) return;
  g_rytm_kit = kit;
  if (!store::ready() || rename::moving()) return;
  if (kit != g_kit) project_changed();
}
#else
CORE_COLD void load() {}
CORE_COLD bool save() { return false; }
CORE_COLD void project_changed() { switch_release(); }
CORE_COLD void kit_changed(int kit) {
  g_rytm_kit = kit;
  switch_release();
}
#endif

}  // namespace xypad
}  // namespace core
