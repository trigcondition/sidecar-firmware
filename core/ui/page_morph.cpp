// core/ui/page_morph.cpp - see extra.h: the MORPH page (spec-050 2.6, core/mod/scene.h). Cold (program flash on the
// Teensy). Off the LEFT / RIGHT ring: the SIDECAR MENU's row and UP + T14 (page_keys.h) replace the Rytm's screen.
//
// Main view: the scene row 1..12 (the selected one inverted, a dot under a scene with locks), the fader as a bar with
// A's scene boxed on its left and B's on its right, then FADER / LOCKS. Encoder A the fader (128 a detent, FUNC 16;
// relative: no jump after a scene switch). (0.5.0 v2, S7: the Rytm's own scene and PERF macros are no scene targets.)
// Trig keys (all 16 taken): T1..T12 select; T15 / T16 held + Tn: Tn becomes A / B (the fader stays); Tn held: its lock
// list (7 rows, B..H edit their values, LEFT / RIGHT scroll, NO removes B's row, YES asks to clear the scene, YES
// again clears), its last row ADD (v2 Task 5): scrolled to the top, B the track (RT1..RT12, RFX, V1..V8), C its page,
// D the param, E the value (from the target's base when the Sidecar knows it, else 64); YES adds it. EDIT on the other
// Sidecar pages is PAGE + T15 / T16 held (remote.cpp; 0.5.0 v2, momentary); on the Rytm's own pages a knob press under it
// removes that parameter's lock (0.5.2, 2D 01 14: scene::capture_clear).
// LEDs: A blue, B orange, the selected scene white (blinking slowly when it is A or B too), a scene with locks dim white,
// T15 / T16 their colours.
// v2 Task 7: the header's right names the fader's source (KNOB, MIDI, PAGE, TCH, WEB; EDIT A / B while held; for 2 s
// after a kit change KIT n LOADED / KIT n: NO SCENES); A and B framed in the scene row; the bar hollow while A or B is
// unset; a tick under the bar where the QPERF knob is when the fader is elsewhere; hints in the footer; NO in the list
// removes with an undo (NO again within 2 s); a lock whose target is gone (an unused P, an RT word a Sidecar machine's
// track does not have) shows its name with "?".
#include <string.h>

#include "platform.h"
#include "mod/mod.h"
#include "mod/rytm_param.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

constexpr uint32_t kHoldMs = 250;   // a scene key held this long shows its lock list (a tap only selects)
constexpr int kRows = 7;            // the lock list's rows: B..H
constexpr int kBoxW = 10, kBox0 = 4, kRowY = 11;   // the scene row
constexpr int kBarX = 18, kBarW = 92, kBarY = 23, kBarH = 9;   // the fader
constexpr int kLabY = 37;           // FADER / LOCKS labels (values 6 below)
constexpr uint8_t kLedWhite = 111, kLedWhiteDim = 107;   // 34's white ramp (96..111), dim as kLedBlueDim is to kLedBlue
constexpr uint32_t kUndoMs = 2000;  // NO again within this restores the lock NO removed

struct MorphPage {
  int held = -1;          // the scene key held (0..11), -1 none
  uint32_t held_ms = 0;   // since when
  bool held_used = false; // a turn or a key while held: the list shows at once
  int top = 0;            // the list's first row (B's)
  bool ab_a = false, ab_b = false;   // T15 / T16 held
  bool func = false;      // FUNC held (the page's mask names it: both sides')
  int confirm = -1;       // CLEAR S<n>? asked (YES again clears)
  int touched = -1;       // the encoder turned last (main view: 0; the list: 1..7)
  int add_tr = 0, add_pg = 0, add_pr = 0, add_v = 64;   // the ADD row: track, page, param (indices), value 0..127
  const char* note = nullptr;   // a one-shot footer ("SCENE FULL")
  int undo_n = -1;        // the lock NO removed last: its scene, target, value, when (kUndoMs)
  mod::Tid undo_t = 0;
  uint16_t undo_v = 0;
  uint32_t undo_ms = 0;
  int undo_kit = -1;
  uint32_t now = 0;
};
CORE_RAM2 MorphPage mp;

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
CORE_COLD const scene::Scene& sc(int n) { return scene::state().s[n]; }
CORE_COLD bool list_shown() { return mp.held >= 0 && (mp.held_used || mp.now - mp.held_ms >= kHoldMs); }

// ---- the ADD row (v2 Task 5): a target by track, page and param, as the Rytm's and the Sidecar's pages group them
struct AddPage {
  const char* name;
  uint8_t kind, lo, hi;   // the kind's params lo..hi (mod::tid)
};
constexpr int kAddRytm = 12, kAddFx = 12, kAddV0 = 13, kAddTracks = 21;   // RT1..RT12, RFX, V1..V8
UI_ROM const AddPage kAddRt[] = {{"SRC", mod::kRs, 9, 16}, {"SMPL", mod::kRs, 17, 24}, {"FLTR", mod::kRa, 0, 7},
                                 {"AMP", mod::kRa, 8, 16}, {"LFO", mod::kRs, 1, 8}};
UI_ROM const AddPage kAddRfx[] = {{"DELAY", mod::kRfx, 0, 8}, {"REVERB", mod::kRfx, 10, 16}, {"DIST", mod::kRfx, 18, 19},
                                  {"COMP", mod::kRfx, 21, 28}, {"LFO", mod::kRfx, 29, 31}};
UI_ROM const AddPage kAddVp[] = {{"P1-8", mod::kV, 0, 7}, {"P9-16", mod::kV, 8, 15}};
CORE_COLD const AddPage* add_pages(int tr, int* n) {
  if (tr < kAddRytm) return *n = 5, kAddRt;
  if (tr == kAddFx) return *n = 5, kAddRfx;
  return *n = 2, kAddVp;
}
CORE_COLD int add_index(int tr) { return tr < kAddRytm ? tr : tr == kAddFx ? 0 : tr - kAddV0; }
// the targets of a track's page this build has (a voice's: the Ps its machine uses), at most 16
CORE_COLD int add_list(int tr, int pg, mod::Tid* out) {
  int np = 0, n = 0;
  const AddPage* p = add_pages(tr, &np);
  if (pg < 0 || pg >= np) return 0;
  for (int k = p[pg].lo; k <= p[pg].hi && n < 16; ++k) {
    mod::Tid t = mod::tid((mod::Kind)p[pg].kind, add_index(tr), k);
    char b[16];
    if (!mod::valid(t) || !mod::name(t, b, sizeof b)[0]) continue;
    if (p[pg].kind == mod::kV && !strcmp(mod::param_label(t, b, sizeof b), "-")) continue;   // unused on its machine
    out[n++] = t;
  }
  return n;
}
CORE_COLD bool add_track_ok(int tr) {
  int np = 0;
  add_pages(tr, &np);
  mod::Tid l[16];
  for (int pg = 0; pg < np; ++pg)
    if (add_list(tr, pg, l)) return true;
  return false;
}
CORE_COLD mod::Tid add_tid() {
  mod::Tid l[16];
  int n = add_list(mp.add_tr, mp.add_pg, l);
  return n ? l[mp.add_pr < n ? mp.add_pr : n - 1] : 0;
}
CORE_COLD void add_range(mod::Tid t, int* lo, int* hi) {
  mod::Range r = mod::range(t);
  bool off = mod::is_offset(mod::kind_of(t));
  *lo = off || r.lo < 0 ? 0 : (int)r.lo, *hi = off || r.hi > 127 ? 127 : (int)r.hi;
}
CORE_COLD void add_value_from_base() {   // E starts at the param's base when the Sidecar knows it, else 64
  mod::Tid t = add_tid();
  if (!t) return;
  int lo, hi;
  add_range(t, &lo, &hi);
  int v = mod::is_remote(mod::kind_of(t)) ? 64 : (int)(mod::base(t) + 0.5f);
  mp.add_v = v < lo ? lo : v > hi ? hi : v;
}
CORE_COLD bool add_on_top() { return mp.held >= 0 && mp.top >= scene::state().s[mp.held].n; }

// a lock whose target is gone (v2 Task 7, UX 17): a voice's P its machine leaves unused, an RT word the track's machine
// does not have (a Sidecar machine's track: its V targets do that job); the Rytm's own only while its link says so
CORE_COLD bool stale_target(mod::Tid t) {
  if (!mod::valid(t)) return true;
  const mod::Kind k = mod::kind_of(t);
  char b[16];
  if (k == mod::kV) return !strcmp(mod::param_label(t, b, sizeof b), "-");
  int tr, w;
  return mod::is_remote(k) && mod::rp_on() && mod::rytm_word(t, &tr, &w) && !mod::rp_word_active(tr, w);
}
CORE_COLD const char* row_name(mod::Tid t, char* b, int cap) {   // "V1:HRM", a gone one "V1:P16?"
  if (!stale_target(t)) return mod::label(t, b, cap);
  char n[16];
  sniprintf(b, cap, "%s?", mod::name(t, n, sizeof n)[0] ? n : "?");
  return b;
}
CORE_COLD bool undo_shown(uint32_t now) {   // (review L3: a kit change since makes the scene another's: no undo)
  return mp.undo_ms && mp.held == mp.undo_n && mp.undo_kit == scene::rytm_kit() && now - mp.undo_ms < kUndoMs;
}
CORE_COLD bool undo_footer(Canvas& c) {
  if (!undo_shown(mp.now)) return false;
  char nm[16], f[kTextLen + 1];
  row_name(mp.undo_t, nm, sizeof nm);
  sniprintf(f, sizeof f, "%s GONE NO:UNDO", nm);   // (a stale name 10: 23 of 25)
  footer(c, f);
  return true;
}
CORE_COLD const char* src_tag(int src) {
  switch (src) {
    case scene::kQperf: return "KNOB";
    case scene::kLearn: return "MIDI";
    case scene::kPage: return "PAGE";
    case scene::kTouch: return "TCH";
    case scene::kWeb: return "WEB";
    default: return nullptr;
  }
}

}  // namespace

CORE_COLD bool morph_stale(uint16_t t) { return stale_target(t); }

// the value as its page shows it: whole units, one decimal when not whole
CORE_COLD void morph_value_text(uint16_t t, uint16_t v, char* b, int cap) {
  (void)t;
  if (v % 128) {
    int d = v * 10 / 128;   // (cut, not rounded: 16383 shows 127.9, not 128.0)
    sniprintf(b, cap, "%d.%d", d / 10, d % 10);
  } else {
    sniprintf(b, cap, "%d", v / 128);
  }
}

namespace {
CORE_COLD void value_text(mod::Tid t, uint16_t v, char* b, int cap) { morph_value_text(t, v, b, cap); }

CORE_COLD void lock_or_note(int n, mod::Tid t, uint16_t v) {
  if (!scene::lock(n, t, v)) mp.note = "SCENE FULL";
}

CORE_COLD void scene_box(Canvas& c, int i) {
  const scene::State& st = scene::state();
  int x = kBox0 + i * kBoxW;
  char t[4];
  sniprintf(t, sizeof t, "%d", i + 1);
  bool sel = i == st.sel;
  if (sel) c.fill(Rect{(int16_t)x, (int16_t)(kRowY - 1), (int16_t)(kBoxW - 1), 8}, kOn);
  if (i == st.a || i == st.b) c.rect(Rect{(int16_t)(x - 1), (int16_t)(kRowY - 2), (int16_t)(kBoxW + 1), 10}, kOn);   // A / B framed
  c.put_text(kSlotFree + i, kRSmall, x + kBoxW / 2 - 1, kRowY, t, kAlignCentre | (sel ? kTextInvert : 0));
  if (sc(i).n) c.fill(Rect{(int16_t)(x + kBoxW / 2 - 2), (int16_t)(kRowY + 8), 2, 1}, kOn);   // locks: a dot under it
}

CORE_COLD void main_draw(Canvas& c, int page, int npages) {
  const scene::State& st = scene::state();
  // the header's right: LOCKED, a kit change's note (2 s), EDIT A / B while held, else the fader's source
  char hr[20];
  const char* right = src_tag(scene::fader_source());
  const int kn = scene::kit_note(mp.now);
  if (scene::locked()) {
    right = "LOCKED";
  } else if (kn >= 0) {
    bool any = false;
    for (int i = 0; i < scene::kScenes; ++i) any |= sc(i).n > 0;
    sniprintf(hr, sizeof hr, any ? "KIT %d LOADED" : "KIT %d: NO SCENES", kn + 1);
    right = hr;
  } else if (scene::edit()) {
    right = scene::edit_side() ? "EDIT B" : "EDIT A";
  }
  header(c, "MORPH", page, npages, right);
  for (int i = 0; i < scene::kScenes; ++i) scene_box(c, i);
  // the fader: A's scene left, B's right, the bar filled to where the morph plays, a notch where it is set
  char a[4] = "A-", b[4] = "B-";
  if (st.a >= 0) sniprintf(a, sizeof a, "A%d", st.a + 1);
  if (st.b >= 0) sniprintf(b, sizeof b, "B%d", st.b + 1);
  c.put_text(kSlotFree + 12, kRSmall, 1, kBarY + 2, a);
  c.put_text(kSlotFree + 13, kRSmall, kW - 1, kBarY + 2, b, kAlignRight);
  c.rect(Rect{kBarX, kBarY, kBarW, kBarH}, kOn);
  int inner = kBarW - 4, played = scene::fader_played() * inner / scene::kFaderMax;
  if (st.a >= 0 && st.b >= 0)   // (A or B unset: hollow, the fader morphs nothing)
    c.fill(Rect{(int16_t)(kBarX + 2), (int16_t)(kBarY + 2), (int16_t)played, (int16_t)(kBarH - 4)}, kDither);
  int at = kBarX + 2 + st.fader * (inner - 1) / scene::kFaderMax;
  c.vline(at, kBarY - 1, kBarY + kBarH, kOn);
  const scene::QpView q = scene::qperf_view();   // the QPERF knob's own position, when the fader is elsewhere
  if (q.pos >= 0 && scene::qp_wants_knob()) {   // (review L5: not while MUTED ONLY acts as OFF)
    int kx = kBarX + 2 + q.pos * scene::kFaderMax / 255 * (inner - 1) / scene::kFaderMax;
    if (kx - at > 1 || at - kx > 1) c.vline(kx, kBarY + kBarH + 1, kBarY + kBarH + 2, kOn);
  }
  // FADER (A), LOCKS (the selected scene's)
  static UI_ROM const char kLab[2][6] = {"FADER", "LOCKS"};
  char val[2][8];
  sniprintf(val[0], 8, "%d%%", (st.fader * 100 + scene::kFaderMax / 2) / scene::kFaderMax);
  sniprintf(val[1], 8, "%d", sc(st.sel).n);
  for (int k = 0; k < 2; ++k) {
    int x = k * kCellW + kCellW / 2;
    c.put_text(kSlotLabels + k, kRSmall, x, kLabY, kLab[k], kAlignCentre);
    c.put_text(kSlotCells + k, kRSmall, x, kLabY + 6, val[k], kAlignCentre);
  }
  char f[kTextLen + 1];
  if (mp.confirm >= 0) {
    sniprintf(f, sizeof f, "CLEAR S%d? YES AGAIN", mp.confirm + 1);
    footer(c, f);
  } else if (mp.note) {
    footer(c, mp.note);
  } else if (scene::locked()) {
    footer(c, "LOCKED: A NEWER FILE");
  } else if (mp.touched == 0) {
    sniprintf(f, sizeof f, "FADER:%d", st.fader);
    footer(c, f, st.fader >> 7);
  } else if (scene::qp_mode() == scene::kQpMutedOnly && scene::qp_wants_knob() && !q.muted && q.ignored_ms &&
             mp.now - q.ignored_ms < 2000) {
    footer(c, "FUNC+QPER: MUTE FOR FADER");   // the knob turned with the macros playing: it is not the fader now
  } else if (st.a < 0 || st.b < 0) {
    footer(c, "T15/16 + T1-12: SET A / B");
  } else if (!sc(st.a).n && !sc(st.b).n) {
    footer(c, "PAGE+T15/16+TURN: LOCK");
  } else if (scene::qp_wants_knob() && !q.moved && !scene::fader_source()) {
    footer(c, "TURN QUICK PERF");   // (the knob reads 0 until it has sent since the Rytm booted)
  } else {
    footer(c, "HOLD T1-12: LOCKS");
  }
}

CORE_COLD void add_draw(Canvas& c, int page, int npages) {   // the ADD row on top: its fields on B..E
  char t[12];
  sniprintf(t, sizeof t, "S%d ADD", mp.held + 1);
  header(c, t, page, npages, nullptr);
  int np = 0;
  const AddPage* pg = add_pages(mp.add_tr, &np);
  char tr[8], pr[16] = "-", v[8];
  if (mp.add_tr < kAddRytm) sniprintf(tr, sizeof tr, "RT%d", mp.add_tr + 1);
  else if (mp.add_tr == kAddFx) sniprintf(tr, sizeof tr, "RFX");
  else sniprintf(tr, sizeof tr, "V%d", mp.add_tr - kAddV0 + 1);
  mod::Tid at = add_tid();
  if (at) mod::param_label(at, pr, sizeof pr);
  sniprintf(v, sizeof v, "%d", mp.add_v);
  static UI_ROM const char kF[4][7] = {"TRACK", "PAGE", "PARAM", "VALUE"};
  const char* val[4] = {tr, pg[mp.add_pg < np ? mp.add_pg : 0].name, pr, v};
  for (int i = 0; i < 4; ++i) {
    int y = kRowY + 6 * i;
    char e[2] = {(char)('B' + i), 0};
    c.put_text(kSlotFree + 3 * i, kRSmall, 2, y, e);
    c.put_text(kSlotFree + 3 * i + 1, kRSmall, 10, y, kF[i]);
    c.put_text(kSlotFree + 3 * i + 2, kRSmall, kW - 2, y, val[i], kAlignRight);
  }
  for (int i = 4; i < kRows; ++i)
    for (int j = 0; j < 3; ++j) c.put_text(kSlotFree + 3 * i + j, kRSmall, 0, 0, nullptr);
  if (mp.confirm >= 0) {
    char f[kTextLen + 1];
    sniprintf(f, sizeof f, "CLEAR S%d? YES AGAIN", mp.confirm + 1);
    footer(c, f);
  } else if (mp.note || !undo_footer(c)) {
    footer(c, mp.note ? mp.note : at ? "YES: ADD  LEFT: LOCKS" : "NOTHING TO ADD HERE");
  }
}

CORE_COLD void list_draw(Canvas& c, int page, int npages) {
  if (add_on_top()) return add_draw(c, page, npages);
  const scene::Scene& s = sc(mp.held);
  char t[12], r[12];
  sniprintf(t, sizeof t, "S%d LOCKS", mp.held + 1);
  if (s.n > kRows) sniprintf(r, sizeof r, "%d-%d/%d", mp.top + 1, mp.top + kRows < s.n ? mp.top + kRows : s.n, s.n);
  else sniprintf(r, sizeof r, "%d", s.n);
  header(c, t, page, npages, r);
  for (int i = 0; i < kRows; ++i) {
    int k = mp.top + i, y = kRowY + 6 * i;
    if (k == s.n) {   // the last row: ADD (RIGHT brings it to the top)
      c.put_text(kSlotFree + 3 * i, kRSmall, 0, 0, nullptr);
      c.put_text(kSlotFree + 3 * i + 1, kRSmall, 10, y, "ADD...");
      c.put_text(kSlotFree + 3 * i + 2, kRSmall, 0, 0, nullptr);
      continue;
    }
    if (k > s.n) {
      for (int j = 0; j < 3; ++j) c.put_text(kSlotFree + 3 * i + j, kRSmall, 0, 0, nullptr);
      continue;
    }
    char e[2] = {(char)('B' + i), 0}, nm[16], v[8];
    row_name(s.l[k].t, nm, sizeof nm);   // "V1:HRM": the param as the machine names it ("V1:P16?": gone)
    value_text(s.l[k].t, s.l[k].v, v, sizeof v);
    uint8_t fl = i == 0 ? kTextInvert : 0;   // B's row: the one NO removes
    if (i == 0) c.fill(Rect{0, (int16_t)(y - 1), kW, 7}, kOn);
    c.put_text(kSlotFree + 3 * i, kRSmall, 2, y, e, fl);
    c.put_text(kSlotFree + 3 * i + 1, kRSmall, 10, y, nm, fl);
    c.put_text(kSlotFree + 3 * i + 2, kRSmall, kW - 2, y, v, kAlignRight | fl);
  }
  char f[kTextLen + 1];
  int k = mp.top + mp.touched - 1;
  if (mp.confirm >= 0) {
    sniprintf(f, sizeof f, "CLEAR S%d? YES AGAIN", mp.confirm + 1);
    footer(c, f);
  } else if (mp.note) {
    footer(c, mp.note);
  } else if (undo_footer(c)) {
  } else if (mp.touched >= 1 && k < s.n) {
    char nm[16], v[8];
    row_name(s.l[k].t, nm, sizeof nm);   // "V1:HRM": the param as the machine names it
    value_text(s.l[k].t, s.l[k].v, v, sizeof v);
    sniprintf(f, sizeof f, "%s:%s", nm, v);
    footer(c, f, s.l[k].v >> 7);
  } else {
    footer(c, "NO: REMOVE  YES: CLEAR");
  }
}

CORE_COLD void morph_draw(Canvas& c, int page, int npages, uint32_t ms) {
  mp.now = ms;
  c.clear();
  if (list_shown()) list_draw(c, page, npages);
  else main_draw(c, page, npages);
}

CORE_COLD void morph_enc(int e, int d, bool func) {
  if (!d) return;
  mp.note = nullptr, mp.confirm = -1;
  const scene::State& st = scene::state();
  if (e == 0) {   // the fader: relative, 128 a detent, FUNC 16
    mp.touched = 0;
    scene::set_fader((uint16_t)clampi(st.fader + d * (func ? 16 : 128), 0, scene::kFaderMax), scene::kPage);
    return;
  }
  if (mp.held >= 0 && add_on_top()) {   // the ADD row: B track, C page, D param, E value
    mp.held_used = true, mp.touched = e;
    int np = 0;
    if (e == 1) {
      int tr = mp.add_tr;
      for (int i = 0; i < (d < 0 ? -d : d); ++i) {
        int nx = tr + (d < 0 ? -1 : 1);
        while (nx >= 0 && nx < kAddTracks && !add_track_ok(nx)) nx += d < 0 ? -1 : 1;
        if (nx < 0 || nx >= kAddTracks) break;
        tr = nx;
      }
      if (tr != mp.add_tr) mp.add_tr = tr, mp.add_pg = 0, mp.add_pr = 0, add_value_from_base();
    } else if (e == 2) {
      add_pages(mp.add_tr, &np);
      int pg = clampi(mp.add_pg + d, 0, np - 1);
      if (pg != mp.add_pg) mp.add_pg = pg, mp.add_pr = 0, add_value_from_base();
    } else if (e == 3) {
      mod::Tid l[16];
      int n = add_list(mp.add_tr, mp.add_pg, l);
      int pr = clampi(mp.add_pr + d, 0, n > 0 ? n - 1 : 0);
      if (pr != mp.add_pr) mp.add_pr = pr, add_value_from_base();
    } else if (e == 4) {
      int lo = 0, hi = 127;
      if (mod::Tid t = add_tid()) add_range(t, &lo, &hi);
      mp.add_v = clampi(mp.add_v + d * (func ? 8 : 1), lo, hi);
    }
    return;
  }
  if (mp.held >= 0) {   // B..H: the list's rows
    mp.held_used = true, mp.touched = e;
    const scene::Scene& s = sc(mp.held);
    int k = mp.top + e - 1;
    if (k >= s.n) return;
    mod::Tid t = s.l[k].t;
    mod::Range r = mod::range(t);
    int lo = mod::is_offset(mod::kind_of(t)) || r.lo < 0 ? 0 : (int)(r.lo * 128.0f + 0.5f);
    int hi = mod::is_offset(mod::kind_of(t)) || r.hi > 127 ? scene::kFaderMax : (int)(r.hi * 128.0f + 0.5f);
    lock_or_note(mp.held, t, (uint16_t)clampi(s.l[k].v + d * (func ? 8 : 1) * 128, lo, hi));
  }
}

CORE_COLD bool morph_key(int code) {
  const scene::State& st = scene::state();
  if (code == 0x80) {   // FUNC (both sides': the Rytm keeps it too)
    mp.func = true;
    return true;
  }
  if (code != 0x44 && code != 0x45) mp.confirm = -1;
  mp.note = nullptr;
  if (code >= 0 && code < scene::kScenes) {
    if (mp.ab_a || mp.ab_b) {   // T15 / T16 held: A / B, the fader stays
      scene::set_ab(mp.ab_a ? code : st.a, mp.ab_b ? code : st.b);
      return true;
    }
    scene::set_sel(code);
    mp.held = code, mp.held_ms = mp.now, mp.held_used = false, mp.top = 0, mp.touched = -1;
    if (!add_track_ok(mp.add_tr)) mp.add_tr = kAddV0;   // (the Sidecar Mini: its voices only)
    add_value_from_base();
    return true;
  }
  if (code == 0x0E || code == 0x0F) {
    (code == 0x0E ? mp.ab_a : mp.ab_b) = true;
    return true;
  }
  if (code >= 0 && code < 16) return true;   // T13 / T14: nothing
  if (code == 0x44) {   // YES
    if (mp.confirm >= 0) {
      scene::clear(mp.confirm), mp.confirm = -1, mp.top = 0, mp.undo_ms = 0;   // (review L3: nothing to undo into)
      return true;
    }
    if (mp.held >= 0 && add_on_top()) {   // the ADD row: the target at E's value
      mp.held_used = true;
      if (mod::Tid t = add_tid()) {
        int n = sc(mp.held).n;
        lock_or_note(mp.held, t, (uint16_t)(mp.add_v * 128));
        if (sc(mp.held).n > n) mp.top = sc(mp.held).n;   // (ADD stays on top for the next one)
      }
      return true;
    }
    if (mp.held >= 0) {
      mp.held_used = true, mp.confirm = mp.held;
      return true;
    }
    return true;
  }
  if (code == 0x45) {   // NO
    if (mp.confirm >= 0) {
      mp.confirm = -1;
      return true;
    }
    if (mp.held >= 0) {
      mp.held_used = true;
      if (undo_shown(now_ms())) {   // NO again within 2 s: the removed lock back (at the list's end)
        lock_or_note(mp.undo_n, mp.undo_t, mp.undo_v);
        mp.undo_ms = 0;
        return true;
      }
      const scene::Scene& s = sc(mp.held);
      if (mp.top >= s.n) return true;   // (the ADD row: nothing to remove)
      mp.undo_n = mp.held, mp.undo_t = s.l[mp.top].t, mp.undo_v = s.l[mp.top].v, mp.undo_ms = now_ms() | 1;
      mp.undo_kit = scene::rytm_kit();
      scene::unlock(mp.held, mp.undo_t);
      if (mp.top > 0 && mp.top >= sc(mp.held).n) --mp.top;
      return true;
    }
    return false;   // back
  }
  if (code == 0x40 || code == 0x41) {   // LEFT / RIGHT: the list's rows, else the scene before / after
    int d = code == 0x41 ? 1 : -1;
    if (mp.held >= 0) {
      mp.held_used = true;
      mp.top = clampi(mp.top + d, 0, sc(mp.held).n);   // (v2: on to the ADD row)
    } else {
      scene::set_sel(clampi(st.sel + d, 0, scene::kScenes - 1));
    }
    return true;
  }
  return false;
}

CORE_COLD bool morph_key_up(int code) {
  if (code == 0x80) mp.func = false;
  else if (code == mp.held) mp.held = -1, mp.touched = -1, mp.undo_ms = 0;
  else if (code == 0x0E) mp.ab_a = false;
  else if (code == 0x0F) mp.ab_b = false;
  return true;
}

CORE_COLD uint16_t morph_trigs() { return 0xFFFF; }

CORE_COLD bool morph_leds(uint8_t c[16]) {
  const scene::State& st = scene::state();
  memset(c, kLedOff, 16);
  const bool blink = now_ms() / 500 & 1;   // the selected scene, when it is A or B too: its colour and white, slowly
  for (int k = 0; k < scene::kScenes; ++k)
    c[k] = k == st.sel && blink && (k == st.a || k == st.b) ? kLedWhite
           : k == st.a ? kLedA : k == st.b ? kLedB : k == st.sel || k == mp.held ? kLedWhite : sc(k).n ? kLedWhiteDim : kLedOff;
  c[14] = kLedA, c[15] = kLedB;
  return true;
}

}  // namespace

UI_ROM const ExtraPage kMorphPage = {"MORPH", nullptr, morph_draw, morph_enc, morph_key, 0xFF, false, nullptr, morph_trigs,
                                     morph_leds, true, morph_key_up};

CORE_COLD void morph_page_reset() { mp = MorphPage(); }
CORE_COLD int morph_page_held() { return mp.held; }

}  // namespace ui
}  // namespace core
