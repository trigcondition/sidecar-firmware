// core/ui/page_pool.cpp - the remote UI's POOL page (the Pi and the Mac: CORE_MSAMP; the pools design 5, the UX
// review's P1 and P3): the project's three POOL lists as the Tonverk's SAMPLE BANKS / WAVETABLES pages, a third tab
// for the instruments - host-drawn, no mod code. Drawn and keyed on the control core, which owns the lists
// (core/lib/pools.h): the page calls them directly. Cold.
//
// Knobs: A the list (SAMPLES / TABLES / INSTR), B the bank (A..H), C the row (FUNC: 8 at a time), D the action
// (SELECT, UNUSED, ALL, NONE, UNLOAD, REPLACE, UNDO). YES does the action: SELECT toggles the row; UNUSED selects the
// rows not seen playing in this project (the footer says a Rytm lock may still use them); ALL / NONE; UNLOAD the
// selection (or the row): P3 - rows in use ask first ("2 IN USE: YES AGAIN"), and while the Rytm plays they are refused;
// REPLACE opens the list's browser aimed at the row (C / UP / DOWN move, YES into a folder or sets the file - asks the
// same as UNLOAD -, NO up, at the top back to the rows); UNDO takes the last replace / unload back.
// Drawn: the header with the list's name and the RAM figure (MB and % of the pool RAM), five rows ("A001 name", the
// selection a '*', the cursor inverted with the row's state: RAM, LOADING, MISSING, CHANGED, its size), the footer the
// action (or the question).
#include "platform.h"

#if CORE_MSAMP && CORE_POLY
#include <string.h>

#include "lib/pools.h"
#include "lib/srclib.h"
#include "ui/extra.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

enum Act : uint8_t { kSelect = 0, kUnused, kAll, kNone, kUnload, kReplace, kUndo, kActs };
const char* const kActNames[kActs] = {"SELECT", "UNUSED", "ALL", "NONE", "UNLOAD", "REPLACE", "UNDO"};
const char* const kTabs[pools::kLists] = {"SAMPLES", "TABLES", "INSTR"};
const char* const kShort[pools::kLists] = {"SMP", "TBL", "INST"};
constexpr int kTabPitch = 9;   // a bank letter a text item at a fixed pitch (the Rytm's glyphs are narrower: rui1040)
constexpr int kRowsShown = 5, kRowH = 8;
constexpr uint32_t kNoteMs = 2500;

struct State {
  int list = 0, bank = 0, row = 1, act = kSelect;
  uint8_t sel[pools::kLists][pools::kCodes / 8];
  bool ask = false;             // P3: the next YES confirms the UNLOAD / the replace
  bool browse = false;
  int cur = 0;                  // the browser's cursor
  char ask_path[srclib::kPath];
  char note[kTextLen + 1];
  uint32_t note_ms = 0;
  bool unused_shown = false;    // the warning in the footer after UNUSED
} P;

CORE_COLD bool is_sel(int l, int c) { return P.sel[l][c >> 3] >> (c & 7) & 1; }
CORE_COLD void set_sel(int l, int c, bool on) {
  if (on) P.sel[l][c >> 3] |= (uint8_t)(1 << (c & 7));
  else P.sel[l][c >> 3] &= (uint8_t)~(1 << (c & 7));
}
CORE_COLD int nsel(int l) {
  int n = 0;
  for (int c = 0; c < pools::kCodes; ++c) n += is_sel(l, c);
  return n;
}
CORE_COLD void note(const char* s) {
  sniprintf(P.note, sizeof P.note, "%s", s);
  P.note_ms = now_ms() | 1;
}
CORE_COLD int code_now() { return pools::code(P.bank, P.row); }
CORE_COLD const char* state_of(const pools::Entry& e, int l, int c, char* b, int cap) {
  b[0] = 0;
  if (e.type == pools::kEmpty || e.type == pools::kOff) return b;
  if (e.type == pools::kInput) return "LIVE";
  if (e.type == pools::kBuiltin) return "BUILT-IN";
  if (e.st == lib::kMissing) return "MISSING";
  if (e.st == lib::kChanged) return "CHANGED";
  if (pools::resident(l, c)) return "RAM";
  uint64_t kb = e.ref.size >> 10;
  if (kb >= 1024) sniprintf(b, cap, CORE_STR("%lu.%lu MB"), (unsigned long)(kb >> 10), (unsigned long)((kb & 1023) * 10 >> 10));
  else if (kb) sniprintf(b, cap, CORE_STR("%lu KB"), (unsigned long)kb);
  return b;
}
// UNLOAD (rows) with P3: in use asks once, the Rytm playing refuses those rows
CORE_COLD void unload_rows() {
  int l = P.list, n = 0, used = 0;
  int16_t rows[pools::kCodes];
  for (int c = 0; c < pools::kCodes; ++c)
    if (is_sel(l, c)) rows[n++] = (int16_t)c;
  if (!n) rows[n++] = (int16_t)code_now();
  for (int i = 0; i < n; ++i) used += pools::in_use(l, rows[i]) ? 1 : 0;
  if (used && pools::playing()) {
    char b[kTextLen + 1];
    sniprintf(b, sizeof b, CORE_STR("%d IN USE: STOP FIRST"), used);
    note(b);
    return;
  }
  if (used && !P.ask) {
    char b[kTextLen + 1];
    sniprintf(b, sizeof b, CORE_STR("%d IN USE: YES AGAIN"), used);
    P.ask = true;
    note(b);
    return;
  }
  int done = 0;
  for (int i = 0; i < n; ++i) done += pools::unload(l, rows[i], P.ask ? pools::kConfirmed : pools::kAsk) == pools::kOk;
  memset(P.sel[l], 0, sizeof P.sel[l]);
  P.ask = false;
  char b[kTextLen + 1];
  sniprintf(b, sizeof b, CORE_STR("%d UNLOADED"), done);
  note(b);
}
CORE_COLD void browse_yes() {
  int k = P.list;   // srclib's kinds are the lists' numbers
  const char* nm;
  bool dir;
  char path[srclib::kPath];
  if (!srclib::item(k, P.cur, &nm, &dir, nullptr) || !srclib::item_path(k, P.cur, path, sizeof path)) return;
  if (dir) {
    if (srclib::cd(k, path)) P.cur = 0;
    return;
  }
  int c = code_now(), type = 0, id = 0, out = -1;
  bool again = P.ask && !strcmp(P.ask_path, path);
  if (!again && pools::in_use(k, c) && pools::at(k, c).type != pools::kEmpty) {
    if (pools::playing()) return note("IN USE: STOP FIRST");
    sniprintf(P.ask_path, sizeof P.ask_path, "%s", path);
    P.ask = true;
    return note("IN USE: YES AGAIN");
  }
  int r = srclib::special(k, path, &type, &id) ? pools::set_special(k, c, type, id, pools::kConfirmed)
                                               : pools::set_file(k, c, path, &out, pools::kConfirmed);
  P.ask = false;
  char b[kTextLen + 1];
  if (r == pools::kExisting && out >= 0) sniprintf(b, sizeof b, CORE_STR("IN POOL %c%03d"), 'A' + (out >> 7), out & 127);
  else sniprintf(b, sizeof b, CORE_STR("%c%03d %s"), 'A' + P.bank, P.row, pools::result_text(r));
  note(b);
  if (r == pools::kOk || r == pools::kPending || r == pools::kExisting) P.browse = false;
}

char g_key[24] = "";   // the project the selection is of (another one: cleared)
CORE_COLD void project_check() {
  if (!strcmp(g_key, pools::project())) return;
  sniprintf(g_key, sizeof g_key, "%s", pools::project());
  memset(P.sel, 0, sizeof P.sel);
  P.ask = P.browse = false;
}
CORE_COLD void pool_draw(Canvas& c, int page, int npages, uint32_t ms) {
  project_check();
  c.clear();
  int l = P.list;
  pools::Ram ram = pools::ram();
  char right[20];
  if (ram.budget) sniprintf(right, sizeof right, CORE_STR("%s %luMB %u%%"), kShort[l], (unsigned long)((ram.used + ram.tables) >> 20), (unsigned)ram.pct);
  else sniprintf(right, sizeof right, CORE_STR("%s RAM -"), kShort[l]);
  header(c, "POOL", page, npages, right);
  for (int i = 0; i < 8; ++i)
    c.put_text(kSlotFree + i, kRSmall, 0, 0, nullptr), c.put_text(kSlotFree + 16 + i, kRSmall, 0, 0, nullptr),
        c.put_text(kSlotFree + 24 + i, kRSmall, 0, 0, nullptr);
  const int y0 = 11, w = 126;
  if (P.browse) {   // REPLACE: the list's browser aimed at the row
    int k = l, n = srclib::count(k);
    char hd[kTextLen + 1];
    const char* cw = srclib::cwd(k);
    int len = (int)strlen(cw), room = 16;
    sniprintf(hd, sizeof hd, CORE_STR("%c%03d %s%s"), 'A' + P.bank, P.row, len > room ? ".." : "", len > room ? cw + len - room : (len ? cw : "/"));
    c.put_text(kSlotFree, kRSmall, 2, y0, hd, kAlignLeft);
    if (P.cur >= n) P.cur = n > 0 ? n - 1 : 0;
    int first = P.cur - 2;
    if (first > n - (kRowsShown - 1)) first = n - (kRowsShown - 1);
    if (first < 0) first = 0;
    for (int i = 0; i < kRowsShown - 1 && first + i < n; ++i) {
      const char* nm = "";
      bool dir = false;
      srclib::item(k, first + i, &nm, &dir, nullptr);
      int y = y0 + (i + 1) * kRowH;
      bool on = first + i == P.cur;
      if (on) c.fill(Rect{0, (int16_t)(y - 1), (int16_t)w, kRowH});
      char b[kTextLen + 1];
      sniprintf(b, sizeof b, CORE_STR("%.20s%s"), nm, dir ? "/" : "");
      c.put_text(kSlotFree + 1 + i, kRSmall, 2, y, b, kAlignLeft | (on ? kTextInvert : 0));
      if (on) c.put_text(kSlotFree + 16 + i, kRSmall, w - 2, y, dir ? "OPEN" : "SET", kAlignRight | kTextInvert);
    }
    if (n <= 0) c.put_text(kSlotFree + 1, kRSmall, 64, y0 + 2 * kRowH, srclib::listing_done(k) ? "EMPTY FOLDER" : "READING", kAlignCentre);
  } else {
    // the bank tabs: each letter its own item on a fixed pitch, the bank's in a filled box, inverted; then the rows
    for (int b = 0; b < pools::kBanks; ++b) {
      char lt[2] = {(char)('A' + b), 0};
      int x = 6 + b * kTabPitch;
      bool on = b == P.bank;
      if (on) c.fill(Rect{(int16_t)(x - 4), (int16_t)(y0 - 1), 9, 7});
      c.put_text(kSlotFree + 24 + b, kRSmall, x, y0, lt, kAlignCentre | (on ? kTextInvert : 0));
    }
    char cnt[12];
    sniprintf(cnt, sizeof cnt, CORE_STR("%d SEL"), nsel(l));
    if (nsel(l)) c.put_text(kSlotFree + 16 + 6, kRSmall, w - 2, y0, cnt, kAlignRight);
    int first = P.row - 2;
    if (first > pools::kRows - (kRowsShown - 1)) first = pools::kRows - (kRowsShown - 1);
    if (first < 0) first = 0;
    for (int i = 0; i < kRowsShown - 1; ++i) {
      int row = first + i, code = pools::code(P.bank, row), y = y0 + (i + 1) * kRowH;
      const pools::Entry& e = pools::at(l, code);
      bool on = row == P.row;
      if (on) c.fill(Rect{0, (int16_t)(y - 1), (int16_t)w, kRowH});
      char nm[48], b[kTextLen + 1], st[16];
      if (e.type == pools::kOff) sniprintf(nm, sizeof nm, "OFF");
      else if (e.type == pools::kEmpty) sniprintf(nm, sizeof nm, "-");
      else sniprintf(nm, sizeof nm, "%s%s", e.st == lib::kMissing && pools::has_file(e) ? "!" : e.st == lib::kChanged && pools::has_file(e) ? "~" : "", e.name);
      // docs/STEREO.md: a file's ST / M tag once its channels are known (the name 3 letters shorter for it)
      const char* tag = pools::has_file(e) && e.ch ? (e.ch >= 2 ? "ST" : "M") : "";
      sniprintf(b, sizeof b, tag[0] ? CORE_STR("%c%c%03d %.11s") : CORE_STR("%c%c%03d %.14s"), is_sel(l, code) ? '*' : ' ',
                'A' + P.bank, row, nm);
      uint8_t fl = on ? kTextInvert : 0;
      c.put_text(kSlotFree + 1 + i, kRSmall, 1, y, b, kAlignLeft | fl);
      const char* s = on ? state_of(e, l, code, st, sizeof st) : pools::in_use(l, code) && e.type != pools::kEmpty ? "USED" : "";
      char rt[24];
      sniprintf(rt, sizeof rt, "%s%s%s", tag, tag[0] && s[0] ? " " : "", s);
      if (rt[0]) c.put_text(kSlotFree + 16 + i, kRSmall, w - 2, y, rt, kAlignRight | fl);
    }
  }
  // the footer: a note for a while, the question, the warning after UNUSED, else the action
  char f[kTextLen + 1];
  if (P.note_ms && ms - P.note_ms < kNoteMs) sniprintf(f, sizeof f, "%s", P.note);
  else if (P.unused_shown && P.act == kUnused) sniprintf(f, sizeof f, "UNSEEN: A LOCK MAY USE IT");
  else if (P.browse) sniprintf(f, sizeof f, "YES: SET  NO: UP");
  else sniprintf(f, sizeof f, CORE_STR("D: %s  YES: DO"), kActNames[P.act]);
  footer(c, f);
}

CORE_COLD void pool_enc(int e, int delta, bool func) {
  if (!delta) return;
  P.ask = false;
  if (e == 0) {
    int l = P.list + delta;
    P.list = l < 0 ? 0 : l >= pools::kLists ? pools::kLists - 1 : l;
    P.browse = false;
  } else if (e == 1) {
    int b = P.bank + delta;
    P.bank = b < 0 ? 0 : b >= pools::kBanks ? pools::kBanks - 1 : b;
    pools::shown(P.list, P.bank);   // takes go to the bank last shown
  } else if (e == 2) {
    if (P.browse) {
      int n = srclib::count(P.list), cu = P.cur + delta * (func ? 8 : 1);
      P.cur = cu < 0 ? 0 : cu >= n ? (n > 0 ? n - 1 : 0) : cu;
    } else {
      int r = P.row + delta * (func ? 8 : 1);
      P.row = r < 0 ? 0 : r >= pools::kRows ? pools::kRows - 1 : r;
    }
  } else if (e == 3) {
    int a = P.act + delta;
    P.act = a < 0 ? 0 : a >= kActs ? kActs - 1 : a;
    P.unused_shown = false;
  }
}

CORE_COLD bool pool_key(int code) {
  project_check();
  int l = P.list;
  if (P.browse) {
    int n = srclib::count(l);
    if (code == 0x42 && P.cur > 0) --P.cur, P.ask = false;          // UP
    else if (code == 0x43 && P.cur < n - 1) ++P.cur, P.ask = false; // DOWN
    else if (code == 0x44) browse_yes();                             // YES
    else if (code == 0x45) {                                         // NO: up a folder, at the top the rows again
      if (srclib::cwd(l)[0]) srclib::up(l), P.cur = 0;
      else P.browse = false;
    } else {
      return false;
    }
    return true;
  }
  if (code == 0x42 && P.row > 0) return --P.row, P.ask = false, true;
  if (code == 0x43 && P.row < pools::kRows - 1) return ++P.row, P.ask = false, true;
  if (code != 0x44) return false;
  int c = code_now();
  switch (P.act) {
    case kSelect:
      if (pools::at(l, c).type != pools::kEmpty && pools::at(l, c).type != pools::kOff) set_sel(l, c, !is_sel(l, c));
      break;
    case kUnused: {
      static int16_t un[pools::kCodes];
      int n = pools::select_unused(l, un, pools::kCodes);
      memset(P.sel[l], 0, sizeof P.sel[l]);
      for (int i = 0; i < n; ++i) set_sel(l, un[i], true);
      P.unused_shown = true;
      char b[kTextLen + 1];
      sniprintf(b, sizeof b, CORE_STR("%d NOT SEEN PLAYING"), n);
      note(b);
      break;
    }
    case kAll:
      for (int r = 1; r < pools::kRows; ++r) {
        int k = pools::code(P.bank, r);
        if (pools::at(l, k).type != pools::kEmpty) set_sel(l, k, true);
      }
      break;
    case kNone: memset(P.sel[l], 0, sizeof P.sel[l]); break;
    case kUnload: unload_rows(); break;
    case kReplace:
      if (!P.row) return note("000 IS OFF"), true;
      if (pools::at(l, c).type == pools::kInput || pools::at(l, c).type == pools::kBuiltin) return note("BUILT-IN: UNLOAD FIRST"), true;
      srclib::open(l);   // its folder listed (a browser not opened since boot had none: READING forever)
      P.browse = true, P.cur = 0, P.ask = false;
      break;
    case kUndo: note(pools::undo(l) ? "UNDONE" : "NOTHING TO UNDO"); break;
  }
  return true;
}

}  // namespace

// tests: the page's state
CORE_COLD void pool_page_set(int list, int bank, int row, int act) {
  P.list = list, P.bank = bank, P.row = row, P.act = act, P.ask = P.browse = false;
}
CORE_COLD bool pool_page_browsing() { return P.browse; }
CORE_COLD int pool_page_selected() { return nsel(P.list); }
CORE_COLD void pool_page_reset() {
  P = State();
  memset(P.sel, 0, sizeof P.sel);
}

// Shown where SAMPLE is (the Pi: the machines that read the lists, a recorder)
CORE_COLD bool pool_shown() { return kRecPage.shown && kRecPage.shown(); }
UI_ROM const ExtraPage kPoolPage = {"POOL", pool_shown, pool_draw, pool_enc, pool_key, 0x0F, false, nullptr, nullptr, nullptr};

}  // namespace ui
}  // namespace core
#endif
