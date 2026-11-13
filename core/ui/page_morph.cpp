// core/ui/page_morph.cpp - see extra.h: the MORPH page (spec-050 2.6, core/mod/scene.h). Cold (program flash on the
// Teensy). Off the LEFT / RIGHT ring: the SIDECAR MENU's row and UP + T14 (page_keys.h) replace the Rytm's screen.
//
// Main view: the scene row 1..12 (the selected one inverted, a dot under a scene with locks), the fader as a bar with
// A's scene boxed on its left and B's on its right, then FADER / LOCKS / SCN / PERF. Encoder A the fader (128 a detent,
// FUNC 16; relative: no jump after a scene switch), G the selected scene's Rytm scene lock (RYTM:SCN, OFF / 1..12), H
// its PERF lock (RYTM:PERFm at 127, at 0 on the A scene: A and B with the same m crossfade the macro; OFF / 1..12).
// Trig keys (all 16 taken): T1..T12 select; T15 / T16 held + Tn: Tn becomes A / B (the fader stays); Tn held: its lock
// list (7 rows, B..H edit their values, LEFT / RIGHT scroll, NO removes B's row, YES asks to clear the scene, YES
// again clears). FUNC + YES: EDIT on / off (hold a scene key and turn a knob on any Sidecar page: remote.cpp).
// LEDs: A blue, B orange, the selected scene white, a scene with locks dim white, T15 / T16 their colours.
#include <string.h>

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
constexpr int kLabY = 37;           // FADER / LOCKS / SCN / PERF labels (values 6 below)
constexpr uint8_t kLedWhite = 111, kLedWhiteDim = 107;   // 34's white ramp (96..111), dim as kLedBlueDim is to kLedBlue

struct MorphPage {
  int held = -1;          // the scene key held (0..11), -1 none
  uint32_t held_ms = 0;   // since when
  bool held_used = false; // a turn or a key while held: the list shows at once
  int top = 0;            // the list's first row (B's)
  bool ab_a = false, ab_b = false;   // T15 / T16 held
  bool func = false;      // FUNC held (the page's mask names it: both sides')
  int confirm = -1;       // CLEAR S<n>? asked (YES again clears)
  int touched = -1;       // the encoder turned last (main view: 0, 6, 7; the list: 1..7)
  const char* note = nullptr;   // a one-shot footer ("SCENE FULL")
  uint32_t now = 0;
};
CORE_RAM2 MorphPage mp;

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
CORE_COLD const scene::Scene& sc(int n) { return scene::state().s[n]; }
CORE_COLD bool list_shown() { return mp.held >= 0 && (mp.held_used || mp.now - mp.held_ms >= kHoldMs); }

}  // namespace

// the value as its page shows it: a scene number (RYTM:SCN 0 = OFF), else whole units, one decimal when not whole
CORE_COLD void morph_value_text(uint16_t t, uint16_t v, char* b, int cap) {
  if (mod::kind_of(t) == mod::kRytm && mod::param_of(t) == 0) {
    int n = (v + 64) / 128;
    if (n) sniprintf(b, cap, "%d", n);
    else sniprintf(b, cap, "OFF");
  } else if (v % 128) {
    int d = v * 10 / 128;   // (cut, not rounded: 16383 shows 127.9, not 128.0)
    sniprintf(b, cap, "%d.%d", d / 10, d % 10);
  } else {
    sniprintf(b, cap, "%d", v / 128);
  }
}

namespace {
CORE_COLD void value_text(mod::Tid t, uint16_t v, char* b, int cap) { morph_value_text(t, v, b, cap); }

// the scene's RYTM:SCN lock (1..12, 0 none) and its first RYTM:PERFm lock (m 1..12, 0 none)
CORE_COLD int scn_of(int n) {
  const scene::Scene& s = sc(n);
  for (int i = 0; i < s.n; ++i)
    if (s.l[i].t == mod::tid(mod::kRytm, 0, 0)) return clampi((s.l[i].v + 64) / 128, 0, 12);
  return 0;
}
CORE_COLD int perf_of(int n) {
  const scene::Scene& s = sc(n);
  int m = 0;
  for (int i = 0; i < s.n; ++i)
    if (mod::kind_of(s.l[i].t) == mod::kRytm && mod::param_of(s.l[i].t) >= 1 && (!m || mod::param_of(s.l[i].t) < m))
      m = mod::param_of(s.l[i].t);
  return m;
}

CORE_COLD int perf_value(int n, int m) {
  const scene::Scene& s = sc(n);
  for (int i = 0; i < s.n; ++i)
    if (s.l[i].t == mod::tid(mod::kRytm, 0, m)) return (s.l[i].v + 64) / 128;
  return 0;
}

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
  c.put_text(kSlotFree + i, kRSmall, x + kBoxW / 2 - 1, kRowY, t, kAlignCentre | (sel ? kTextInvert : 0));
  if (sc(i).n) c.fill(Rect{(int16_t)(x + kBoxW / 2 - 2), (int16_t)(kRowY + 8), 2, 1}, kOn);   // locks: a dot under it
}

CORE_COLD void main_draw(Canvas& c, int page, int npages) {
  const scene::State& st = scene::state();
  header(c, "MORPH", page, npages, scene::locked() ? "LOCKED" : nullptr);
  for (int i = 0; i < scene::kScenes; ++i) scene_box(c, i);
  // the fader: A's scene left, B's right, the bar filled to where the morph plays, a notch where it is set
  char a[4] = "A-", b[4] = "B-";
  if (st.a >= 0) sniprintf(a, sizeof a, "A%d", st.a + 1);
  if (st.b >= 0) sniprintf(b, sizeof b, "B%d", st.b + 1);
  c.put_text(kSlotFree + 12, kRSmall, 1, kBarY + 2, a);
  c.put_text(kSlotFree + 13, kRSmall, kW - 1, kBarY + 2, b, kAlignRight);
  c.rect(Rect{kBarX, kBarY, kBarW, kBarH}, kOn);
  int inner = kBarW - 4, played = scene::fader_played() * inner / scene::kFaderMax;
  c.fill(Rect{(int16_t)(kBarX + 2), (int16_t)(kBarY + 2), (int16_t)played, (int16_t)(kBarH - 4)}, kDither);
  int at = kBarX + 2 + st.fader * (inner - 1) / scene::kFaderMax;
  c.vline(at, kBarY - 1, kBarY + kBarH, kOn);
  // FADER (A), LOCKS (the selected scene's), SCN (G), PERF (H)
  static UI_ROM const char kLab[4][6] = {"FADER", "LOCKS", "SCN", "PERF"};
  char val[4][8];
  sniprintf(val[0], 8, "%d%%", (st.fader * 100 + scene::kFaderMax / 2) / scene::kFaderMax);
  sniprintf(val[1], 8, "%d", sc(st.sel).n);
  int scn = scn_of(st.sel), perf = perf_of(st.sel);
  if (scn) sniprintf(val[2], 8, "%d", scn);
  else strcpy(val[2], "OFF");
  if (perf) sniprintf(val[3], 8, "%d", perf);
  else strcpy(val[3], "OFF");
  for (int k = 0; k < 4; ++k) {
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
  } else if (mp.touched == 6) {
    sniprintf(f, sizeof f, "S%d RYTM SCENE:%s", st.sel + 1, val[2]);
    footer(c, f);
  } else if (mp.touched == 7) {
    if (perf) sniprintf(f, sizeof f, "S%d PERF %d AT %d", st.sel + 1, perf, perf_value(st.sel, perf));
    else sniprintf(f, sizeof f, "S%d PERF:OFF", st.sel + 1);
    footer(c, f);
  } else {
    footer(c, scene::edit() ? "EDIT ON  FUNC+YES: OFF" : "HOLD T1-12: LOCKS");
  }
}

CORE_COLD void list_draw(Canvas& c, int page, int npages) {
  const scene::Scene& s = sc(mp.held);
  char t[12], r[12];
  sniprintf(t, sizeof t, "S%d LOCKS", mp.held + 1);
  if (s.n > kRows) sniprintf(r, sizeof r, "%d-%d/%d", mp.top + 1, mp.top + kRows < s.n ? mp.top + kRows : s.n, s.n);
  else sniprintf(r, sizeof r, "%d", s.n);
  header(c, t, page, npages, r);
  for (int i = 0; i < kRows; ++i) {
    int k = mp.top + i, y = kRowY + 6 * i;
    if (k >= s.n) {
      for (int j = 0; j < 3; ++j) c.put_text(kSlotFree + 3 * i + j, kRSmall, 0, 0, nullptr);
      continue;
    }
    char e[2] = {(char)('B' + i), 0}, nm[16], v[8];
    mod::label(s.l[k].t, nm, sizeof nm);   // "V1:HRM": the param as the machine names it
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
  } else if (!s.n) {
    footer(c, "NO LOCKS IN THIS SCENE");
  } else if (mp.touched >= 1 && k < s.n) {
    char nm[16], v[8];
    mod::label(s.l[k].t, nm, sizeof nm);   // "V1:HRM": the param as the machine names it
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
    return;
  }
  const int l = list_turn(e, d);
  if (!l) return;
  mp.touched = e;
  if (e == 6) {   // G: the Rytm scene lock, OFF / 1..12
    int n = clampi(scn_of(st.sel) + l, 0, 12);
    if (n) lock_or_note(st.sel, mod::tid(mod::kRytm, 0, 0), (uint16_t)(n * 128));
    else scene::unlock(st.sel, mod::tid(mod::kRytm, 0, 0));
  } else if (e == 7) {   // H: the PERF lock, OFF / 1..12
    int was = perf_of(st.sel), m = clampi(was + l, 0, 12);
    if (m == was) return;
    if (was) scene::unlock(st.sel, mod::tid(mod::kRytm, 0, was));
    if (m) lock_or_note(st.sel, mod::tid(mod::kRytm, 0, m), st.sel == st.a ? 0 : 127 * 128);
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
    return true;
  }
  if (code == 0x0E || code == 0x0F) {
    (code == 0x0E ? mp.ab_a : mp.ab_b) = true;
    return true;
  }
  if (code >= 0 && code < 16) return true;   // T13 / T14: nothing
  if (code == 0x44) {   // YES
    if (mp.confirm >= 0) {
      scene::clear(mp.confirm), mp.confirm = -1, mp.top = 0;
      return true;
    }
    if (mp.func) {
      scene::set_edit(!scene::edit());
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
      const scene::Scene& s = sc(mp.held);
      if (mp.top < s.n) scene::unlock(mp.held, s.l[mp.top].t);
      if (mp.top > 0 && mp.top >= sc(mp.held).n) --mp.top;
      return true;
    }
    return false;   // back
  }
  if (code == 0x40 || code == 0x41) {   // LEFT / RIGHT: the list's rows, else the scene before / after
    int d = code == 0x41 ? 1 : -1;
    if (mp.held >= 0) {
      mp.held_used = true;
      mp.top = clampi(mp.top + d, 0, sc(mp.held).n > kRows ? sc(mp.held).n - kRows : 0);
    } else {
      scene::set_sel(clampi(st.sel + d, 0, scene::kScenes - 1));
    }
    return true;
  }
  return false;
}

CORE_COLD bool morph_key_up(int code) {
  if (code == 0x80) mp.func = false;
  else if (code == mp.held) mp.held = -1, mp.touched = -1;
  else if (code == 0x0E) mp.ab_a = false;
  else if (code == 0x0F) mp.ab_b = false;
  return true;
}

CORE_COLD uint16_t morph_trigs() { return 0xFFFF; }

CORE_COLD bool morph_leds(uint8_t c[16]) {
  const scene::State& st = scene::state();
  memset(c, kLedOff, 16);
  for (int k = 0; k < scene::kScenes; ++k)
    c[k] = k == st.a ? kLedA : k == st.b ? kLedB : k == st.sel || k == mp.held ? kLedWhite : sc(k).n ? kLedWhiteDim : kLedOff;
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
