// core/ui/page_menu.cpp - see extra.h: the MENU and KEYS pages on the Rytm's screen (UX review R3, N3). Cold.
//
// MENU   YES on the SIDECAR page (UP + YES): every Sidecar page as a list in the stock GLOBAL / SETTINGS style, each
//        with its shortcut on the right (UP + T11, UP + TRIG ..): UP / DOWN or knob A move, YES opens, NO back to
//        SIDECAR (NO there leaves: one rule, NO goes up a level). The direct combos stay; the list teaches them.
// KEYS   the cheat sheet: every combo, one a row, then the older forms that still work. From MENU, or while UP is held
//        alone on the stock screen (rui1044's 49 1 4 1: the page opens on it as UP's map - every UP + key on one
//        screen, T1..T13 lit in their pages' colours - and closes at UP's release).
// WEB UI from MENU (where the platform has one: remote_webui, the Pi): the web page and network installs ON / OFF, the
//        pairing code (a headless Sidecar's only screen is the Rytm's), NEW CODE (the devices stay paired), FORGET
//        DEVICES (YES twice). The gate itself is not offered here: cmdline.txt paircode=0 stays the dev card's bypass.
// Every shortcut comes from the device's table (ui/page_keys.h). Both stay off the LEFT / RIGHT ring (ExtraPage::off_ring).
#include <string.h>

#include "ui/extra.h"
#include "ui/page_keys.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

enum : int { kYes = 0x44, kNo = 0x45, kUp = 0x42, kDown = 0x43 };
constexpr int kRows = 6, kRowY = 11, kRowH = 7;

struct Entry {
  const char* name;   // the row (and the page's title: remote_show_page)
  const char* page;   // the ExtraPage's title, kStatusTitle, "SYSTEM" (null: KEYS)
};
// rui1051: the pages only their keys open (page_keys.h sc_shortcut_only: SEQ, INSERT, POOL, CHOP) are not rows; the
// tests hold the list to that rule (test_ui)
UI_ROM const Entry kEntries[] = {
    {"POLY CONFIG", "POLY"}, {"SAMPLE", "SAMPLE"},   {"AUTO SAMPLE", "AUTO"}, {"SONG", "SONG"},     {"REC", "REC"},
    {"SCOPE", "SCOPE"},      {"LFO", "LFO"},         {"PROJECT", "PROJECT"},  {"STATUS", "SIDECAR"}, {"SYSTEM", "SYSTEM"},
    {"WEB UI", "WEB UI"},    {"KEYS (ALL COMBOS)", nullptr},
};
constexpr int kNum = sizeof kEntries / sizeof kEntries[0];

// a page's shortcut from the device's table: its page request (UP+T11), else the combo that opens it (UP+TRIG); KEYS:
// the modifier held alone
CORE_COLD const char* shortcut(const char* page, char* b, int cap) {
  const sc_keymap_t* m = sc_keymap();
  if (!page) { sniprintf(b, cap, "HOLD %s", m->modifier); return b; }
  for (int i = 0; i < m->npages; ++i)
    if (sc_streq(m->pages[i].rytm, page)) { sniprintf(b, cap, "%s%d", m->page_keys, m->pages[i].code); return b; }
  for (int i = 0; i < m->ncombos; ++i)
    if (sc_streq(m->combos[i].rytm, page)) return m->combos[i].keys;
  return nullptr;
}

// KEYS' rows: the page requests, the combos, then the legacy forms ("TRK+TRIG" "= UP+TRIG")
CORE_COLD int keys_rows() {
  const sc_keymap_t* m = sc_keymap();
  int n = m->npages + m->ncombos;
  for (int i = 0; i < m->ncombos; ++i) n += m->combos[i].legacy ? 1 : 0;
  return n;
}
CORE_COLD void keys_row(int r, char* l, char* t, int cap) {
  const sc_keymap_t* m = sc_keymap();
  if (r < m->npages) {
    const sc_page_key_t& p = m->pages[r];
    sniprintf(l, cap, "%s%d", m->page_keys, p.code);
    if (p.rytm) sniprintf(t, cap, "%s%s", p.name, p.more);
    else sniprintf(t, cap, "TOUCH: %s%s", p.name, p.more);
    return;
  }
  r -= m->npages;
  if (r < m->ncombos) { sniprintf(l, cap, "%s", m->combos[r].keys), sniprintf(t, cap, "%s", m->combos[r].what); return; }
  r -= m->ncombos;
  for (int i = 0; i < m->ncombos; ++i)
    if (m->combos[i].legacy && !r--) { sniprintf(l, cap, "%s", m->combos[i].legacy), sniprintf(t, cap, "= %s", m->combos[i].keys); return; }
  l[0] = t[0] = 0;
}

struct State {
  int cur, top;          // MENU's cursor
  int ktop;              // KEYS' first row
  bool menu_on, keys_on; // shown now (off the ring: in the list only while shown)
  bool map;              // KEYS opened by UP held (rui1044): closes at the release
  bool keys_was;         // rui1050: KEYS shown before UP's map over the page
  bool web_on;           // WEB UI shown
  int wcur;              // its cursor
  bool wconfirm;         // FORGET DEVICES asked once: YES again forgets
  bool wnote_new;        // its note set by a key: timed from the next frame (the frame's clock, not now_ms)
  uint32_t wnote_ms;
  char note[26];
  uint32_t note_ms;
};
State M;

CORE_COLD void clampv(int* cur, int* top, int n) {
  if (*cur >= n) *cur = n - 1;
  if (*cur < 0) *cur = 0;
  if (*cur < *top) *top = *cur;
  if (*cur >= *top + kRows) *top = *cur - kRows + 1;
  if (*top < 0) *top = 0;
}

CORE_COLD bool available(const Entry& e) {   // its page is in the list now (or a status page / KEYS)
  if (!e.page || !strcmp(e.page, "SIDECAR") || !strcmp(e.page, "SYSTEM")) return true;
  if (!strcmp(e.page, "WEB UI")) return remote_webui() != nullptr;
  for (int k = 0; k < extra_pages(); ++k) {
    const ExtraPage& x = extra_page(k);
    if (!strcmp(x.title, e.page)) return !x.shown || x.shown();
  }
  return false;
}

CORE_COLD void menu_draw(Canvas& c, int page, int npages, uint32_t ms) {
  c.clear();
  header(c, "SIDECAR MENU", page, npages, nullptr);
  clampv(&M.cur, &M.top, kNum);
  for (int i = 0; i < kRows && M.top + i < kNum; ++i) {
    const Entry& e = kEntries[M.top + i];
    bool on = M.top + i == M.cur, ok = available(e);
    int y = kRowY + i * kRowH;
    if (on) c.fill(Rect{0, (int16_t)(y - 1), 128, kRowH}, kOn);
    uint8_t inv = on ? kTextInvert : 0;
    char b[24], k[16];
    sniprintf(b, sizeof b, "%s%s", e.name, ok ? "" : " -");
    c.put_text(kSlotFree + 2 * i, kRSmall, 2, y, b, (uint8_t)(kAlignLeft | inv));
    c.put_text(kSlotFree + 2 * i + 1, kRSmall, 126, y, shortcut(e.page, k, sizeof k), (uint8_t)(kAlignRight | inv));
  }
  if (M.note_ms && ms - M.note_ms < 2000) footer(c, M.note);
  else footer(c, "YES: OPEN  NO: BACK");
}

// UP's map (rui1044, held alone): the page requests and the combos with the modifier on one screen, two columns of
// kMapRows - 18 rows, all there is - and no footer (letting go of UP closes it).
// rui1047 / rui1052: the pages only the touchscreen has (their `rytm` 0, not their key number) are one row of their
// keys, "T1-5,8 TOUCH" - 13 page requests and the 6 combos with a map text are 19, one over the 18 - and the
// touchscreen's own map lists them one by one. With no touchscreen up (remote_display_present: the engine's one signal
// for it) their keys show nothing on the Rytm at all, so they are left off the map altogether and the rows they free
// carry the UP combos that are otherwise off it (sc_combo_t.alone: the step-edit trio).
constexpr int kMapRows = 9, kMapY = 10, kMapH = 6;
// the keys of the pages only the touchscreen has, as one row: runs joined ("T1-5,8 TOUCH")
CORE_COLD void map_touch_row(char* b, int cap) {
  const sc_keymap_t* m = sc_keymap();
  int n = sniprintf(b, cap, "%s", m->map_key);
  bool first = true;
  for (int i = 0; i < m->npages; ++i) {
    if (m->pages[i].rytm) continue;
    int j = i;
    while (j + 1 < m->npages && !m->pages[j + 1].rytm && m->pages[j + 1].code == m->pages[j].code + 1) ++j;
    if (!first) n += sniprintf(b + n, cap - n, ",");
    n += sniprintf(b + n, cap - n, "%d", m->pages[i].code);
    if (j > i) n += sniprintf(b + n, cap - n, "-%d", m->pages[j].code);
    first = false, i = j;
  }
  sniprintf(b + n, cap - n, " TOUCH");
}
CORE_COLD void map_draw(Canvas& c, int page, int npages) {
  const sc_keymap_t* m = sc_keymap();
  const bool touch = remote_display_present();
  char t[24];
  sniprintf(t, sizeof t, "%s + KEY", m->modifier);
  header(c, t, page, npages, nullptr);
  int k = 0;
  auto row = [&](const char* s) {
    if (k >= 2 * kMapRows) return;
    c.put_text(kSlotFree + k, kRSmall, k < kMapRows ? 2 : 65, kMapY + (k % kMapRows) * kMapH, s);
    ++k;
  };
  char b[20];
  if (touch) {
    map_touch_row(b, sizeof b);
    row(b);
  }
  for (int i = 0; i < m->npages; ++i) {
    if (!m->pages[i].rytm) continue;
    sniprintf(b, sizeof b, "%s%d %s", m->map_key, m->pages[i].code, m->pages[i].name);
    row(b);
  }
  for (int i = 0; i < m->ncombos; ++i)
    if (m->combos[i].map) row(m->combos[i].map);
  if (!touch)
    for (int i = 0; i < m->ncombos; ++i)
      if (m->combos[i].alone) row(m->combos[i].alone);
}

CORE_COLD void keys_draw(Canvas& c, int page, int npages, uint32_t ms) {
  c.clear();
  if (M.map) { map_draw(c, page, npages); return; }
  header(c, "KEYS", page, npages, nullptr);
  int n = keys_rows();
  if (M.ktop > n - kRows) M.ktop = n - kRows;
  if (M.ktop < 0) M.ktop = 0;
  for (int i = 0; i < kRows && M.ktop + i < n; ++i) {
    int y = kRowY + i * kRowH;
    char l[24], t[28];
    keys_row(M.ktop + i, l, t, sizeof l);
    c.put_text(kSlotFree + 2 * i, kRSmall, 2, y, l[0] ? l : nullptr);
    c.put_text(kSlotFree + 2 * i + 1, kRSmall, 126, y, t, kAlignRight);
  }
  footer(c, "UP / DOWN: MORE  NO: BACK");
}

CORE_COLD void menu_enc(int e, int delta, bool) {
  if (e == 0 && delta) M.cur += delta > 0 ? 1 : -1, clampv(&M.cur, &M.top, kNum);
}
CORE_COLD void keys_enc(int e, int delta, bool) {
  if (e == 0 && delta) M.ktop += delta > 0 ? 1 : -1;
}

CORE_COLD bool menu_key(int code) {
  if (code == kUp || code == kDown) {
    M.cur += code == kUp ? -1 : 1;
    clampv(&M.cur, &M.top, kNum);
    return true;
  }
  if (code == kYes) {
    const Entry& e = kEntries[M.cur];
    if (!available(e)) {
      sniprintf(M.note, sizeof M.note, "%s: NOT HERE NOW", e.name);
      M.note_ms = now_ms() | 1;
      return true;
    }
    M.menu_on = false;
#if CORE_POLY
    if (e.page && !strcmp(e.page, "POLY")) poly_page_open(-1, false);   // rui1048: the selected track's
#endif
    if (!e.page) M.keys_on = true, M.map = false, M.ktop = 0, remote_show_page("KEYS");
    else if (!strcmp(e.page, "WEB UI")) M.web_on = true, M.wcur = 0, M.wconfirm = false, remote_show_page("WEB UI");
    else remote_show_page(!strcmp(e.page, "SIDECAR") ? kStatusTitle : e.page);
    return true;
  }
  if (code == kNo) M.menu_on = false;   // (remote.cpp: NO goes to SIDECAR)
  return false;
}
CORE_COLD bool keys_key(int code) {
  if (code == kUp || code == kDown) { M.ktop += code == kUp ? -1 : 1; return true; }
  if (code == kNo) M.keys_on = false, M.map = false;
  return false;
}
CORE_COLD bool menu_shown() { return M.menu_on; }
CORE_COLD bool keys_shown() { return M.keys_on; }
// KEYS opened by UP held: the page requests' trigs (T<code>) in their pages' colours from the device's table (the
// touchscreen's - a touchscreen-only page, or one the touchscreen has taken - blue, the Rytm's own screen amber, the
// Sidecar's status cyan)
CORE_COLD bool keys_leds(uint8_t c[16]) {
  if (!M.map) return false;
  memset(c, kLedOff, 16);
  const sc_keymap_t* m = sc_keymap();
  const bool touch = remote_display_present();
  for (int i = 0; i < m->npages; ++i) {
    const sc_page_key_t& p = m->pages[i];
    if (p.code < 1 || p.code > 16) continue;
    if (!p.rytm && !touch) continue;   // rui1052: nothing to show on either screen, and off the map: unlit
    c[p.code - 1] = !p.rytm || remote_display_owned(p.code) ? kLedBlue : sc_streq(p.rytm, "SIDECAR") ? 79 : 63;
  }
  return true;
}

// ---- WEB UI
enum : int { kWebRow, kCodeRow, kDevRow, kNewRow, kForgetRow, kWebRows };
CORE_COLD void web_note(const char* t) {
  sniprintf(M.note, sizeof M.note, "%s", t);
  M.wnote_new = true;
}
CORE_COLD void web_draw(Canvas& c, int page, int npages, uint32_t ms) {
  c.clear();
  header(c, "WEB UI", page, npages, nullptr);
  const RemoteWebUi* h = remote_webui();
  if (!h) return;
  if (M.wcur < 0) M.wcur = 0;
  if (M.wcur >= kWebRows) M.wcur = kWebRows - 1;
  const int gate = h->gate ? h->gate() : 0;
  for (int i = 0; i < kWebRows; ++i) {
    char v[24];
    const char* name = "";
    v[0] = 0;
    switch (i) {
      case kWebRow: name = "WEB UI"; sniprintf(v, sizeof v, "%s", h->on && h->on() ? "ON" : "OFF"); break;
      case kCodeRow:
        name = "CODE";
        sniprintf(v, sizeof v, "%s", gate == 0 ? "OFF (CMDLINE)" : gate == 3 ? "RNG FAILED"
                                     : remote_pair_code_now()[0] ? remote_pair_code_now() : "-");
        break;
      case kDevRow: name = "PAIRED DEVICES"; sniprintf(v, sizeof v, "%d", h->devices ? h->devices() : 0); break;
      case kNewRow: name = "NEW CODE"; sniprintf(v, sizeof v, "%s", gate == 2 ? "FIXED" : ""); break;
      case kForgetRow: name = "FORGET DEVICES"; break;
    }
    bool on = i == M.wcur;
    int y = kRowY + i * kRowH;
    if (on) c.fill(Rect{0, (int16_t)(y - 1), 128, kRowH}, kOn);
    uint8_t inv = on ? kTextInvert : 0;
    c.put_text(kSlotFree + 2 * i, kRSmall, 2, y, name, (uint8_t)(kAlignLeft | inv));
    c.put_text(kSlotFree + 2 * i + 1, kRSmall, 126, y, v[0] ? v : nullptr, (uint8_t)(kAlignRight | inv));
  }
  if (M.wconfirm) footer(c, "YES: FORGET ALL  NO: KEEP");
  else if (M.wnote_new || (M.wnote_ms && ms - M.wnote_ms < 2000)) footer(c, M.note);
  else footer(c, "YES: CHANGE  NO: BACK");
  if (M.wnote_new) M.wnote_new = false, M.wnote_ms = ms | 1;
}
CORE_COLD void web_enc(int e, int delta, bool) {
  if (e == 0 && delta) M.wcur += delta > 0 ? 1 : -1, M.wconfirm = false;
  if (M.wcur < 0) M.wcur = 0;
  if (M.wcur >= kWebRows) M.wcur = kWebRows - 1;
}
CORE_COLD bool web_key(int code) {
  const RemoteWebUi* h = remote_webui();
  if (code == kUp || code == kDown) {
    M.wcur += code == kUp ? -1 : 1, M.wconfirm = false;
    web_enc(0, 0, false);
    return true;
  }
  if (code == kNo) {
    if (M.wconfirm) { M.wconfirm = false; web_note("KEPT"); return true; }
    M.web_on = false;
    return false;   // (remote.cpp: NO goes to SIDECAR)
  }
  if (code != kYes || !h) return false;
  if (M.wcur == kWebRow && h->set_on && h->on) {
    bool on = !h->on();
    h->set_on(on);
    web_note(on ? "WEB UI ON" : "OFF: PAGE + 7310 REFUSE");
  } else if (M.wcur == kNewRow) {
    web_note(h->new_code && h->new_code() ? "A NEW CODE: SEE ABOVE" : "NOT ON A FIXED CODE");
  } else if (M.wcur == kForgetRow) {
    if (!M.wconfirm) M.wconfirm = true;
    else {
      M.wconfirm = false;
      if (h->forget) h->forget();
      web_note("ALL DEVICES FORGOTTEN");
    }
  }
  return true;
}
CORE_COLD bool web_shown() { return M.web_on && remote_webui() != nullptr; }

}  // namespace

UI_ROM const ExtraPage kWebUiPage = {"WEB UI", web_shown, web_draw, web_enc, web_key, 0x01, false, nullptr, nullptr,
                                     nullptr, true};
UI_ROM const ExtraPage kMenuPage = {"MENU", menu_shown, menu_draw, menu_enc, menu_key, 0x01, false, nullptr, nullptr,
                                    nullptr, true};
UI_ROM const ExtraPage kKeysPage = {"KEYS", keys_shown, keys_draw, keys_enc, keys_key, 0x01, false, nullptr, nullptr,
                                    keys_leds, true};

CORE_COLD int menu_rows() { return kNum; }
CORE_COLD const char* menu_row_page(int i) { return i >= 0 && i < kNum ? kEntries[i].page : nullptr; }
CORE_COLD void menu_open() {
  M.menu_on = true;
  remote_show_page("MENU");
}
CORE_COLD void keys_map(bool on) {
  if (on) {
    M.keys_on = true, M.map = true, M.ktop = 0;
    remote_show_page("KEYS");
  } else if (M.map) {
    M.map = false, M.keys_on = false;
    // UP let go: back to the stock screen it came from - only while the map still shows (another UP + T meanwhile opened
    // another page: that one stays)
    if (remote_page_title() && !strcmp(remote_page_title(), "KEYS")) remote_request(false);
  }
}
// rui1050 (rytm-mods 0026's 49 1 5): UP held alone with a page open - KEYS as the map over it; at the release remote.cpp
// shows the page under it again (true: the map was still the page shown)
CORE_COLD bool keys_map_over(bool on) {
  if (on) {
    M.keys_was = M.keys_on;   // (KEYS itself under the map, from the MENU: still in the page list after it)
    M.keys_on = true, M.map = true, M.ktop = 0;
    remote_show_page("KEYS");
    return true;
  }
  bool was = M.map && remote_page_title() && !strcmp(remote_page_title(), "KEYS");
  if (M.map) M.map = false, M.keys_on = M.keys_was;
  return was;
}
CORE_COLD bool keys_map_on() { return M.map; }
CORE_COLD void keys_map_drop() { M.map = false, M.keys_on = false; }   // a hello, the page closed: no map left up
CORE_COLD void menu_reset() { memset(&M, 0, sizeof M); }

}  // namespace ui
}  // namespace core
