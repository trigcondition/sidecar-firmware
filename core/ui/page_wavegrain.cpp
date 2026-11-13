// core/ui/page_wavegrain.cpp - see extra.h: the remote UI's pages of WAVETABLE (0x28, core/wavetable/wavetable.h) and
// GRANULAR (0x38, core/granular/granular.h): the SRC view (P1..P8); the source lists (src_list_page) of GRANULAR's SRC,
// WAVETABLE's TBL and MULTISAMPLE's INST, shown while P1 turns. Top row a picture over P1..P4's labels - WAVETABLE:
// the table's frame at POS, morphed, and the place in the table; GRANULAR: the source's outline (dithered), the head
// (POS + ANIM + the scan, a solid line) and every sounding grain as a bar as tall as its level (live: newest right,
// LIVE / FRZ in the corner), redrawn ~12 times a second while it changes; bottom row P5..P8's dials. Cold.
#include "hooks.h"
#include "machines.h"
#include "ui/extra.h"
#include "ui/remote.h"
#if CORE_MSAMP
#include "granular/granular.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "msamp/msamp.h"
#include "presets/preset_index.h"
#include "samp/preview.h"
#include "samp/sedit.h"
#include "wavetable/tables.h"
#include "wavetable/wavetable.h"
#endif

namespace core {
namespace ui {

namespace {
UI_ROM const Cell kWtCells[] = {
    {kDraw, 0, 0, 4, 1, {1, 0, 2, 3}, kDrawWt, nullptr},   // the frame at POS (TBL, WARP, AMT redraw it too)
    {kKnob, 0, 1, 1, 1, {4, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 1, 1, 1, 1, {5, kNone, kNone, kNone}, 0, nullptr},
    {kKnobBi, 2, 1, 1, 1, {6, kNone, kNone, kNone}, 0, nullptr},   // CUT: LP below 64, HP above
    {kKnob, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kWtPage = {
    "WAVETABLE", kWaveMachine, sizeof kWtCells / sizeof kWtCells[0], kWtCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"TBL", "POS", "WARP", "AMT", "UNI", "SUB", "CUT", "RES"},
};
UI_ROM const char kWinNames[] = "HANN\0TRI\0TUKEY\0PERC\0";
UI_ROM const Cell kGrCells[] = {
    {kDraw, 0, 0, 4, 1, {1, 0, 2, 3}, kDrawGran, nullptr},   // the cloud over its source
    {kKnob, 0, 1, 1, 1, {4, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 1, 1, 1, 1, {5, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 2, 1, 1, 1, {6, kNone, kNone, kNone}, 0, nullptr},
    {kList, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, kWinNames},
};
UI_ROM const Page kGrPage = {
    "GRANULAR", kGranMachine, sizeof kGrCells / sizeof kGrCells[0], kGrCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"SRC", "POS", "SPRY", "SIZE", "DENS", "PRND", "REV", "WIN"},
};
#if CORE_MSAMP
// The lists: one cell over both rows (P1 its input), the encoders and labels as the machine page's (the bottom bar).
UI_ROM const Cell kGrListCells[] = {{kDraw, 0, 0, 4, 2, {0, kNone, kNone, kNone}, kDrawGrList, nullptr}};
UI_ROM const Page kGrListPage = {
    "SOURCE", kGranMachine, 1, kGrListCells, {0, 1, 2, 3, 4, 5, 6, 7},
    {"SRC", "POS", "SPRY", "SIZE", "DENS", "PRND", "REV", "WIN"},
};
UI_ROM const Cell kWtListCells[] = {{kDraw, 0, 0, 4, 2, {0, kNone, kNone, kNone}, kDrawWtList, nullptr}};
UI_ROM const Page kWtListPage = {
    "TABLE", kWaveMachine, 1, kWtListCells, {0, 1, 2, 3, 4, 5, 6, 7},
    {"TBL", "POS", "WARP", "AMT", "UNI", "SUB", "CUT", "RES"},
};
UI_ROM const Cell kMsListCells[] = {{kDraw, 0, 0, 4, 2, {0, kNone, kNone, kNone}, kDrawMsList, nullptr}};
UI_ROM const Page kMsListPage = {
    "INSTRUMENT", kMultiMachine, 1, kMsListCells, {0, 1, 2, 3, 4, 5, 6, 7},
    {"INST", "VEL", "LEV", "RTRG", "ATK", "DEC", "SUS", "REL"},
};
// rui1038: the bank list (4A), one cell over both rows
UI_ROM const Cell kBankCells[] = {{kDraw, 0, 0, 4, 2, {kNone, kNone, kNone, kNone}, kDrawBankList, nullptr}};
UI_ROM const Page kGrBankPage = {"SOURCE", kGranMachine, 1, kBankCells, {0, 1, 2, 3, 4, 5, 6, 7},
                                 {"SRC", "POS", "SPRY", "SIZE", "DENS", "PRND", "REV", "WIN"}};
UI_ROM const Page kWtBankPage = {"TABLE", kWaveMachine, 1, kBankCells, {0, 1, 2, 3, 4, 5, 6, 7},
                                 {"TBL", "POS", "WARP", "AMT", "UNI", "SUB", "CUT", "RES"}};
UI_ROM const Page kMsBankPage = {"INSTRUMENT", kMultiMachine, 1, kBankCells, {0, 1, 2, 3, 4, 5, 6, 7},
                                 {"INST", "VEL", "LEV", "RTRG", "ATK", "DEC", "SUS", "REL"}};
// DSP SAMPLE's SMP (P11, rytm-mods 7h): knob A of its SRC page, as the others (ENG is SMPL knob D)
UI_ROM const Page kDsBankPage = {"SAMPLE", kDspSample, 1, kBankCells, {10, 1, 2, 3, 4, 5, 6, 7},
                                 {"SMP", "STR", "BRI", "DMP", "MIX", "SEED", "TUN", "LEV"}};
// 0.5.0: SAMPLER's (machines.h kSmplMachine; page 1 SMP STRT END PLAY / TUNE FINE LOOP LEV, page_wave.cpp kDspsPageDt)
UI_ROM const Page kDsBankPageDt = {"SAMPLE", kSmplMachine, 1, kBankCells, {10, 12, 13, 11, 8, 9, 14, 15},
                                   {"SMP", "STRT", "END", "PLAY", "TUNE", "FINE", "LOOP", "LEV"}};
// JUNO's and 6-OP FM's PAT (0.4.0): the project's preset list as rows, the same keys (no banks); FUNC + YES the card's
// presets, from the RAM index (core/presets/preset_index.h)
UI_ROM const Page kJnBankPage = {"PRESETS", kJuno, 1, kBankCells, {0, 1, 2, 3, 4, 5, 6, 7},
                                 {"PAT", "CUT", "RES", "ENV", "ATK", "DEC", "REL", "LEN"}};
UI_ROM const Page kFmBankPage = {"PRESETS", kFm, 1, kBankCells, {0, 1, 2, 3, 4, 5, 6, 7},
                                 {"PAT", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV", "LEN"}};
struct BankList {
  bool on = false;
  int preset = -1;              // 0.4.0: presets::kIxJuno, kIxFm: this list is that machine's; -1 a pool's
  int folder = 0;               // its browser's folder of the index
  bool reading = false;         // a voice added, on its way from its bank
  uint32_t pgen = 0;            // the index the browser's folder belongs to (a new walk: back to the root)
  int rcur = -1, rfolder = -1;  // the preset a full list's YES asked about: the second YES replaces the list's row
  int track = -1, vi = -1, machine = -1, bank = 0, row = 0, code0 = 0, commits = 0;
  uint32_t ms = 0;              // the cursor's last move (a GRANULAR / WAVETABLE preview after kSettleMs)
  bool previewed = true;
  // rui1040: FUNC + YES on a row - the card's files for that row (the machine's own browser, srclib); the pools
  // design: every bank (only row 000 and input / built-in rows refused)
  bool browse = false;
  int cur = 0;                  // the browser's cursor (folders first, then the files the machine reads)
  uint32_t bms = 0;             // its last move (a file's preview after kSettleMs)
  bool bpreviewed = true;
  char note[24] = "";           // a word in the tabs' place for kNoteMs (SET B005, why FUNC + YES did nothing)
  uint32_t note_ms = 0;
  bool note_on = false;
  int assigns = 0, refusals = 0;
  bool row_auto = false;        // fix1049: the browser aimed an OFF entry at a free row: no file set, OFF again
  char ask[srclib::kPath] = "";   // P3: the file a YES on an in-use row asked about (a second YES on it replaces)
  int ask_code = 0x44;          // 0.5.0: the key that asked (YES, or RIGHT's quick load): the same key confirms
  int menu = -1;                // 0.5.0: the file menu's cursor (FUNC + RIGHT on a file), -1 closed
  bool seek = false;            // 0.5.0: RIGHT's re-entry: the cursor to the file after the last loaded once listed
  int edit_code = -1;           // 0.5.0: SAMPLE EDIT asked for this row: the editor opens once its file is in
  bool edit_next = false;       // 0.5.0: SAMPLE EDIT's load asked first (a used row): its confirming YES opens the editor
  uint32_t edit_ms = 0;
  bool info_wait = false;       // 0.5.0: INFO asked: its note once the file's head is read
  bool hold = false;            // 0.5.0: FUNC + YES's preview: it plays while YES is held (its release stops it)
};
// 0.5.0: the last file a load set, per list kind (RIGHT in the list goes back to it); kept across the list's opens
char last_file[3][srclib::kPath] = {"", "", ""};
// 0.5.0 FILL (page_fill.cpp): the last quick-load batch (RIGHT's loads in one opening of a list) per track and list
// kind: its first and last row, the FILL page's FROM / TO
struct Batch { int8_t kind = -1; int16_t first = -1, last = -1; };
Batch batch[12];
bool batch_new = true;
bool func_seen = false;         // a 31 with down 2 since the hello: a 0.5.0 0026 (with or without its caps bit)
// the file menu (FUNC + RIGHT on a file)
enum : int { kMLoad, kMLoadNext, kMPreview, kMEdit, kMInfo, kMItems };
const char* const kMenuNames[kMItems] = {"LOAD", "LOAD TO NEXT EMPTY", "PREVIEW", "SAMPLE EDIT", "INFO"};
constexpr uint32_t kNoteMs = 2000;
BankList bl;
CORE_COLD bool bank_note_on() { return bl.note_on && now_ms() - bl.note_ms < kNoteMs; }
CORE_COLD void bank_note(const char* s) {
  sniprintf(bl.note, sizeof bl.note, "%s", s);
  bl.note_ms = now_ms(), bl.note_on = true;
}
uint32_t bank_closed_ms = 0;   // the bank list's close: P1's new value then is not a turn (no SOURCE list after it)
// ---- the list model: kind 0 GRANULAR, 1 WAVETABLE, 2 MULTISAMPLE
CORE_COLD int lst_kind(int arg) { return arg == kDrawGrList ? 0 : arg == kDrawWtList ? 1 : 2; }
CORE_COLD int lst_n(int k) { return k == 0 ? gr::entries() : k == 1 ? wt::entries() : msamp::entries(); }
CORE_COLD const char* lst_name(int k, int e) {
  return k == 0 ? gr::entry_name(e) : k == 1 ? wt::entry_name(e) : msamp::entry_name(e);
}
CORE_COLD const char* lst_group(int k, int e) {
  return k == 0 ? gr::entry_group(e) : k == 1 ? wt::entry_group(e) : msamp::entry_group(e);
}
CORE_COLD int lst_sel(int k, int vi, int v) {   // the entry P1 = v picks (a pick on the voice first)
  int n = lst_n(k);
  if (n <= 0) return -1;
  return k == 0 ? gr::src_entry(vi, v) : k == 1 ? wt::src_entry(vi, v) : msamp::src_entry(vi, v);
}
CORE_COLD int lst_lib(int k, int e) { return k == 0 ? gr::entry_lib(e) : k == 1 ? wt::entry_lib(e) : msamp::entry_lib(e); }
CORE_COLD uint32_t lst_gen(int k) { return k == 0 ? gr::list_gen() : k == 1 ? wt::list_gen() : msamp::list_gen(); }
CORE_COLD const char* lst_note(int k, int e, char* b, int cap) {
  return k == 0 ? gr::entry_note(e, b, cap) : k == 1 ? wt::entry_note(e, b, cap) : msamp::entry_note(e, b, cap);
}
CORE_COLD int lst_machine(int k) { return k == 0 ? kGranMachine : k == 1 ? kWaveMachine : kMultiMachine; }
bool push_seen = false;       // rui1036's pushes came (31 0x70 on the SRC view): no rest-to-open fallback
constexpr int kRows = 6, kRowH = 7, kSlotRowL = kSlotFree + 8, kSlotRowR = kSlotFree + 16, kSlotName = kSlotFree + 24;
CORE_COLD uint32_t list_key(const Cell& c, const Source& s) {
  int k = lst_kind(c.arg), vi = s.voice() < 0 ? 0 : s.voice() & 7, sel = lst_sel(k, vi, s.code(0));
  uint32_t h = (uint32_t)(sel + 2) * 2654435761u ^ (uint32_t)lst_n(k) << 4;
  char b[24];
  h ^= lst_gen(k) * 40503u ^ (uint32_t)push_seen << 30;
  for (const char* q = lst_note(k, sel, b, sizeof b); *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
  return h | 1;
}
CORE_COLD void draw_list(Canvas& c, const Rect& r, const Cell& cell, const Source& s) {
  int k = lst_kind(cell.arg), vi = s.voice() < 0 ? 0 : s.voice() & 7, n = lst_n(k), sel = lst_sel(k, vi, s.code(0));
  int b0 = sel >= 0 ? sel & ~127 : 0;   // the pool: the selected row's bank (128 rows, OFF at 000)
  if (n > 0) n = b0 + srclib::kBankRows;
  for (int i = 0; i < kRows; ++i) c.put_text(kSlotRowL + i, kRSmall, 0, 0, nullptr), c.put_text(kSlotRowR + i, kRSmall, 0, 0, nullptr);
  if (n <= 0 || sel < 0) {
    c.put_text(kSlotRowL, kRSmall, r.x + r.w / 2, r.y + r.h / 2 - 3, k == 0 ? "NO SOURCES" : k == 1 ? "NO TABLES" : "NO INSTRUMENTS",
               kAlignCentre);
    return;
  }
  int first = sel - 2;
  if (first > n - kRows) first = n - kRows;
  if (first < b0) first = b0;
  const int y0 = r.y + 1, wrow = r.w - 6;   // the rows, then a 2 px scroll bar at the right
  for (int i = 0; i < kRows; ++i) {
    int e = first + i;
    if (e >= n) break;
    int y = y0 + i * kRowH;
    bool on = e == sel;
    if (on) c.fill(Rect{(int16_t)r.x, (int16_t)(y - 1), (int16_t)wrow, kRowH}, kOn);
    char right[24] = "", left[kTextLen + 1];
    if (on) lst_note(k, e, right, sizeof right);   // LOADING, TOO LONG: FIRST 87 S, its length, FAILED, MISSING
    const char* g = lst_group(k, e);
    if (!right[0] && (i == 0 || strcmp(g, lst_group(k, e - 1))) && strncmp(g, "BANK", 4)) {   // LIVE / BUILT-IN rows
      const char* sl = strrchr(g, '/');
      sniprintf(right, sizeof right, "%.10s", sl ? sl + 1 : g);
    }
    int rl = (int)strlen(right), room = (wrow - 4) / kRSmall.advance - (rl ? rl + 1 : 0);
    sniprintf(left, sizeof left, "%c%03d %.*s", 'A' + (e >> 7), e & 127, room > 5 ? room - 5 : 1, lst_name(k, e));
    uint8_t fl = on ? kTextInvert : 0;
    c.put_text(kSlotRowL + i, kRSmall, r.x + 1, y, left, kAlignLeft | fl);
    if (rl) c.put_text(kSlotRowR + i, kRSmall, r.x + wrow - 2, y, right, kAlignRight | fl);
  }
  int bx = r.x + r.w - 3, by = r.y + 1, bh = r.h - 2;   // the scroll bar: where the rows are in the list
  c.vline(bx, by, by + bh - 1, kDither);
  if (n - b0 > kRows) {
    int m = n - b0, th = bh * kRows / m < 3 ? 3 : bh * kRows / m, ty = by + (bh - th) * (first - b0) / (m - kRows);
    c.fill(Rect{(int16_t)(bx - 1), (int16_t)ty, 3, (int16_t)th}, kOn);
  } else {
    c.fill(Rect{(int16_t)(bx - 1), (int16_t)by, 3, (int16_t)bh}, kOn);
  }
}

constexpr uint32_t kFrameMs = 83;   // GRANULAR's picture: a new one ~12 times a second at most
struct GrKey {
  uint32_t ms = 0, key = 0;
};
GrKey gr_keys[8];
LiveKey wt_keys[8];   // WAVETABLE's picture (0.4.3: its POS at most every kLiveMs)

CORE_COLD uint32_t mixk(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

CORE_COLD void draw_gran(Canvas& c, const Rect& r, const Source& s, int slot) {
  int vi = s.voice() < 0 ? 0 : s.voice() & 7;
  int x0 = r.x + 1, wd = r.w - 2, y0 = r.y + 1, h = kLabelDy - 2, mid = y0 + h / 2;
  static gr::Viz z;   // ~1.2 KB: not on the stack
  int se = gr::src_entry(vi, s.code(0));   // the source's name top left (the Rytm's SRC, or the voice's pick)
  c.put_text(kSlotName, kRSmall, r.x + 2, y0, se >= 0 ? gr::entry_name(se) : nullptr);
  if (!gr::viz(vi, &z)) {
    c.put_text(slot, kRSmall, r.x + r.w / 2, mid - 2, "NO SOURCE", kAlignCentre);
    return;
  }
  float mn[128], mx[128];
  int n = gr::viz_wave(vi, wd > 128 ? 128 : wd, mn, mx);
  float pk = 0.05f;
  for (int i = 0; i < n; ++i) pk = mx[i] > pk ? mx[i] : -mn[i] > pk ? -mn[i] : pk;
  int amp = h / 2;
  for (int i = 0; i < n; ++i) {   // the outline: its top and bottom edge, every other column (the grains stand out)
    if (i & 1) continue;
    int a = mid - (int)(mx[i] / pk * (amp - 1)), b = mid - (int)(mn[i] / pk * (amp - 1));
    c.pixel(x0 + i, a < y0 ? y0 : a, kOn);
    c.pixel(x0 + i, b > y0 + h - 1 ? y0 + h - 1 : b, kOn);
  }
  if (!n) c.hline(x0, x0 + wd - 1, mid, kDither);
  int xh = x0 + (int)(z.head * (float)(wd - 1) + 0.5f);
  c.vline(xh, y0, y0 + h - 1, kOn);
  if (z.anim > 0) c.hline(xh - 2, xh + 2, y0, kOn);   // ANIM on: a cap on the head
  for (int i = 0; i < z.n; ++i) {   // a grain: a 2 px bar as tall as its level (reversed: a notch on its left)
    int x = x0 + (int)(z.pos[i] * (float)(wd - 2) + 0.5f);
    int gh = 1 + (int)(z.amp[i] * (float)(amp - 1));
    c.fill(Rect{(int16_t)x, (int16_t)(mid - gh), 2, (int16_t)(2 * gh + 1)}, kOn);
    if (z.rev[i]) c.pixel(x - 1, mid, kOn);
  }
  c.put_text(slot, kRSmall, r.x + r.w - 2, y0, z.live ? (z.frozen ? "FRZ" : "LIVE") : z.frozen ? "FRZ" : nullptr,
             kAlignRight);
}

CORE_COLD void draw_wt(Canvas& c, const Rect& r, const Source& s, int slot) {
  int vi = s.voice() < 0 ? 0 : s.voice() & 7;
  int x0 = r.x + 2, wd = r.w - 4, y0 = r.y + 1, h = kLabelDy - 4, mid = y0 + h / 2;
  int nf = 0;
  float pos = wt::viz_pos(vi, &nf);
  float y[128];
  int n = nf ? wt::viz_frame(vi, pos, wd > 128 ? 128 : wd, y) : 0;
  int ne = wt::entries(), te = wt::src_entry(vi, s.code(0));   // the table's name top left (a folder row: what plays)
  c.put_text(kSlotName, kRSmall, r.x + 2, y0, ne > 0 ? wt::entry_name(te) : nullptr);
  if (!n) {
    c.put_text(slot, kRSmall, r.x + r.w / 2, mid - 2, "LOADING", kAlignCentre);
    return;
  }
  float pk = 0.05f;
  for (int i = 0; i < n; ++i) pk = y[i] > pk ? y[i] : -y[i] > pk ? -y[i] : pk;
  c.hline(x0, x0 + wd - 1, mid, kDither);
  int py = mid - (int)(y[0] / pk * (h / 2));
  for (int i = 1; i < n; ++i) {
    int q = mid - (int)(y[i] / pk * (h / 2));
    c.line(x0 + i - 1, py, x0 + i, q, kOn);
    py = q;
  }
  // the table: a track under it, a tick a frame (up to 32), the place filled
  int ty = y0 + h + 1;
  c.hline(x0, x0 + wd - 1, ty, kDither);
  int step = nf > 32 ? nf / 32 : 1;
  for (int f = 0; f < nf; f += step) c.pixel(x0 + (nf > 1 ? f * (wd - 1) / (nf - 1) : 0), ty - 1, kOn);
  int xp = x0 + (int)(pos * (float)(wd - 1));
  c.fill(Rect{(int16_t)(xp - 1), (int16_t)(ty - 1), 3, 2}, kOn);
  c.put_text(slot, kRSmall, r.x + r.w - 2, y0, nullptr, kAlignRight);
}

// ---- rui1038: the bank list (4A) as the SRC view's page (rui1045: DSP SAMPLE's SMP the SAMPLES list, as GRANULAR's SRC)
CORE_COLD int bank_kind() {
  return bl.machine == kGranMachine || sample_machine(bl.machine) || bl.machine == kChopMachine ? 0 : bl.machine == kWaveMachine ? 1 : 2;
}
CORE_COLD int bank_entry(int bank, int row) { return bank * srclib::kBankRows + row; }
CORE_COLD bool menu_has(int it) { return it != kMEdit || bank_kind() == 0; }   // SAMPLE EDIT: the SAMPLES pool's
// a row's name as the lists show it: the pool row's name, "!name" its file missing, "~name" its file changed
CORE_COLD const char* row_name(int k, int e, char* b, int cap) {
  const pools::Entry& p = pools::at(k, e);
  if (pools::has_file(p) && (p.st == lib::kMissing || p.st == lib::kChanged)) {
    sniprintf(b, cap, "%c%s", p.st == lib::kMissing ? '!' : '~', p.name);
    return b;
  }
  return lst_name(k, e);
}
CORE_COLD const char* bank_name(int k, int bank, int row) {
  static char b[48];
  return row_name(k, bank_entry(bank, row), b, sizeof b);
}
CORE_COLD uint32_t bank_key() {
  if (bl.preset >= 0) {
    uint32_t h = (uint32_t)(bl.row + 1) * 2654435761u ^ (uint32_t)bl.on << 31 ^ (uint32_t)presets::rows(bl.preset) << 8;
    char b[24];
    for (const char* q = presets::row_name(bl.preset, bl.row, b, sizeof b); *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
    if (bl.browse) h = (h ^ (uint32_t)(bl.cur + 1) * 69069u ^ (uint32_t)bl.folder * 4099u ^ presets::gen() ^ (uint32_t)presets::ready() << 30) * 16777619u;
    if (bank_note_on()) {
      h = (h ^ 0x20000000u) * 16777619u;
      for (const char* q = bl.note; *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
    }
    return h | 1;
  }
  int k = bank_kind();
  uint32_t h = (uint32_t)(bl.bank * 128 + bl.row + 1) * 2654435761u ^ lst_gen(k) * 40503u ^ (uint32_t)bl.on << 31;
  if (bl.browse) h = (h ^ (uint32_t)(bl.cur + 1) * 69069u ^ srclib::view_gen(k)) * 16777619u ^ 0x40000000u;   // rui1040
  if (bl.browse) h = (h ^ (uint32_t)(bl.menu + 2) * 40503u ^ (uint32_t)preview::sounding() << 29) * 16777619u;   // 0.5.0
  if (bank_note_on()) {   // (the note's text: a second note within its time redraws)
    h = (h ^ 0x20000000u) * 16777619u;
    for (const char* q = bl.note; *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
  }
  char b[24];
  for (const char* q = lst_note(k, bank_entry(bl.bank, bl.row), b, sizeof b); *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
  return h | 1;
}
constexpr int kBankRowsShown = 5;
constexpr int kTabPitch = 9;   // rui1040: a tab a letter, each its own text item at a fixed place
// the tabs: A .. H, each letter its own Rytm text item centred on a fixed 9 px pitch, the list's bank in a filled box
// with its letter inverted (as the row cursor), the arrows as pixels. rui1038 drew "< A B C D E F G H >" as one item and
// the bank's letter again, inverted, at 5 px a character (the widest glyph): the Rytm's own glyphs are narrower (its
// space 2 px), so the box landed on the next letter and over it (the hardware report on rui1039: "the bank indicator
// covers the bank letter") [measured, rytm-emu rui1039 + the real core: bank B's box over "C"]
CORE_COLD void draw_tabs(Canvas& c, const Rect& r, int ty) {
  int tw = srclib::kBanks * kTabPitch, tx = r.x + (r.w - 6 - tw) / 2;
  for (int b = 0; b < srclib::kBanks; ++b) {
    char lt[2] = {(char)('A' + b), 0};
    int x = tx + b * kTabPitch + kTabPitch / 2;
    bool on = b == bl.bank;
    if (on) c.fill(Rect{(int16_t)(x - 4), (int16_t)(ty - 1), 9, 7}, kOn);
    c.put_text(kSlotFree + b, kRSmall, x, ty, lt, kAlignCentre | (on ? kTextInvert : 0));
  }
  for (int i = 0; i < 3; ++i) {   // < and >: a bank before / after this one
    if (bl.bank > 0) c.vline(tx - 6 + i, ty + 2 - i, ty + 2 + i, kOn);
    if (bl.bank < srclib::kBanks - 1) c.vline(tx + tw + 4 - i, ty + 2 - i, ty + 2 + i, kOn);
  }
}
CORE_COLD void draw_browse(Canvas& c, const Rect& r) {
  int k = bank_kind(), n = srclib::count(k);
  char hd[kTextLen + 1];
  const char* cw = srclib::cwd(k);
  int room = (r.w - 6) / kRSmall.advance - 9;   // "-> B005 " and the path's tail
  int l = (int)strlen(cw);
  sniprintf(hd, sizeof hd, "%c%03d %s%s", 'A' + bl.bank, bl.row, l > room ? ".." : "", l > room ? cw + l - room + 2 : (l ? cw : "/"));
  c.put_text(kSlotFree, kRSmall, r.x + 1, r.y + 1, bank_note_on() ? bl.note : hd, kAlignLeft);   // (the ask, a refusal)
  for (int b = 1; b < 8; ++b) c.put_text(kSlotFree + b, kRSmall, 0, 0, nullptr);
  for (int i = 0; i < kRows; ++i) c.put_text(kSlotRowL + i, kRSmall, 0, 0, nullptr), c.put_text(kSlotRowR + i, kRSmall, 0, 0, nullptr);
  const int y0 = r.y + 9, wrow = r.w - 6;
  c.hline(r.x, r.x + wrow - 1, y0 - 2, kDither);
  if (n <= 0) {
    c.put_text(kSlotRowL, kRSmall, r.x + r.w / 2, y0 + 2 * kRowH, srclib::listing_done(k) ? "EMPTY FOLDER" : "READING", kAlignCentre);
    return;
  }
  if (bl.cur >= n) bl.cur = n - 1;
  if (bl.menu >= 0) {   // 0.5.0: the file menu over the rows, its file's name on the first
    const char* fn = "";
    srclib::item(k, bl.cur, &fn, nullptr, nullptr);
    char left[kTextLen + 1];
    sniprintf(left, sizeof left, "%.*s", (wrow - 4) / kRSmall.advance - 1, fn);
    c.put_text(kSlotRowL, kRSmall, r.x + 1, y0, left, kAlignLeft);
    int shown = 0, first = bl.menu > 3 ? bl.menu - 3 : 0;
    for (int it = first; it < kMItems && shown < kBankRowsShown - 1; ++it) {
      if (!menu_has(it)) continue;
      int y = y0 + (1 + shown) * kRowH;
      bool on = it == bl.menu;
      if (on) c.fill(Rect{(int16_t)r.x, (int16_t)(y - 1), (int16_t)wrow, kRowH}, kOn);
      c.put_text(kSlotRowL + 1 + shown, kRSmall, r.x + 5, y, kMenuNames[it], kAlignLeft | (on ? kTextInvert : 0));
      ++shown;
    }
    return;
  }
  int first = bl.cur - 2;
  if (first > n - kBankRowsShown) first = n - kBankRowsShown;
  if (first < 0) first = 0;
  for (int i = 0; i < kBankRowsShown && first + i < n; ++i) {
    int it = first + i, y = y0 + i * kRowH;
    const char* nm = "";
    bool dir = false;
    srclib::item(k, it, &nm, &dir, nullptr);
    bool on = it == bl.cur;
    if (on) c.fill(Rect{(int16_t)r.x, (int16_t)(y - 1), (int16_t)wrow, kRowH}, kOn);
    const char* right = on ? (dir ? "OPEN" : preview::sounding() ? "PLAY" : "SET") : "";
    int rl = (int)strlen(right), rm = (wrow - 4) / kRSmall.advance - (rl ? rl + 1 : 0);
    char left[kTextLen + 1];
    sniprintf(left, sizeof left, "%.*s%s", rm > 2 ? rm - 1 : 1, nm, dir ? "/" : "");
    uint8_t fl = on ? kTextInvert : 0;
    c.put_text(kSlotRowL + i, kRSmall, r.x + 1, y, left, kAlignLeft | fl);
    if (rl) c.put_text(kSlotRowR + i, kRSmall, r.x + wrow - 2, y, right, kAlignRight | fl);
  }
  int bx = r.x + r.w - 3, by = y0 - 1, bh = kBankRowsShown * kRowH;
  c.vline(bx, by, by + bh - 1, kDither);
  int th = n > kBankRowsShown ? (bh * kBankRowsShown / n < 3 ? 3 : bh * kBankRowsShown / n) : bh;
  c.fill(Rect{(int16_t)(bx - 1), (int16_t)(n > kBankRowsShown ? by + (bh - th) * first / (n - kBankRowsShown) : by), 3, (int16_t)th}, kOn);
}
// The scroll bar of the preset lists
CORE_COLD void draw_bar(Canvas& c, const Rect& r, int y0, int n, int first) {
  int bx = r.x + r.w - 3, by = y0 - 1, bh = kBankRowsShown * kRowH;
  c.vline(bx, by, by + bh - 1, kDither);
  int th = n > kBankRowsShown ? (bh * kBankRowsShown / n < 3 ? 3 : bh * kBankRowsShown / n) : bh;
  c.fill(Rect{(int16_t)(bx - 1), (int16_t)(n > kBankRowsShown ? by + (bh - th) * first / (n - kBankRowsShown) : by), 3, (int16_t)th}, kOn);
}
CORE_COLD void draw_preset_head(Canvas& c, const Rect& r, const char* s) {
  c.put_text(kSlotFree, kRSmall, r.x + 1, r.y + 1, bank_note_on() ? bl.note : s, kAlignLeft);
  for (int b = 1; b < 8; ++b) c.put_text(kSlotFree + b, kRSmall, 0, 0, nullptr);
  for (int i = 0; i < kRows; ++i) c.put_text(kSlotRowL + i, kRSmall, 0, 0, nullptr), c.put_text(kSlotRowR + i, kRSmall, 0, 0, nullptr);
  c.hline(r.x, r.x + r.w - 7, r.y + 7, kDither);
}
CORE_COLD void preset_sync() {   // folder numbers belong to one walk
  if (bl.pgen == presets::gen()) return;
  bl.pgen = presets::gen(), bl.folder = bl.preset, bl.cur = 0, bl.rcur = -1;
}
CORE_COLD void draw_presets(Canvas& c, const Rect& r) {
  preset_sync();
  const int y0 = r.y + 9, wrow = r.w - 6;
  const char* who = bl.preset == presets::kIxJuno ? "JUNO" : "FM";
  char hd[kTextLen + 1];
  if (bl.browse) {   // the index: no card read here
    int n = presets::ready() ? presets::count(bl.folder) : 0;
    if (presets::ready() && presets::full(bl.preset) && bl.folder == bl.preset) sniprintf(hd, sizeof hd, "ADD: %s FULL", who);   // (the rest left out)
    else sniprintf(hd, sizeof hd, "ADD: %s", presets::ready() ? presets::folder_name(bl.folder) : who);
    draw_preset_head(c, r, hd);
    if (n <= 0) {
      c.put_text(kSlotRowL, kRSmall, r.x + r.w / 2, y0 + 2 * kRowH, presets::ready() ? "EMPTY FOLDER" : "READING", kAlignCentre);
      return;
    }
    if (bl.cur >= n) bl.cur = n - 1;
    int first = bl.cur - 2;
    if (first > n - kBankRowsShown) first = n - kBankRowsShown;
    if (first < 0) first = 0;
    for (int i = 0; i < kBankRowsShown && first + i < n; ++i) {
      int it = first + i, y = y0 + i * kRowH;
      bool on = it == bl.cur, dir = presets::is_dir(bl.folder, it);
      if (on) c.fill(Rect{(int16_t)r.x, (int16_t)(y - 1), (int16_t)wrow, kRowH}, kOn);
      const char* right = on ? (dir ? "OPEN" : bl.reading ? "READING" : "ADD") : "";
      int rl = (int)strlen(right), rm = (wrow - 4) / kRSmall.advance - (rl ? rl + 1 : 0);
      char left[kTextLen + 1];
      sniprintf(left, sizeof left, "%.*s%s", rm > 2 ? rm - 1 : 1, presets::name(bl.folder, it), dir ? "/" : "");
      uint8_t fl = on ? kTextInvert : 0;
      c.put_text(kSlotRowL + i, kRSmall, r.x + 1, y, left, kAlignLeft | fl);
      if (rl) c.put_text(kSlotRowR + i, kRSmall, r.x + wrow - 2, y, right, kAlignRight | fl);
    }
    draw_bar(c, r, y0, n, first);
    return;
  }
  int n = presets::rows(bl.preset);
  if (bl.row >= n) bl.row = n - 1;
  if (presets::built_ins(bl.preset)) sniprintf(hd, sizeof hd, "%s BUILT-IN", who);
  else sniprintf(hd, sizeof hd, "%s LIST %d", who, n);
  draw_preset_head(c, r, hd);
  int first = bl.row - 2;
  if (first > n - kBankRowsShown) first = n - kBankRowsShown;
  if (first < 0) first = 0;
  for (int i = 0; i < kBankRowsShown && first + i < n; ++i) {
    int row = first + i, y = y0 + i * kRowH;
    bool on = row == bl.row;
    if (on) c.fill(Rect{(int16_t)r.x, (int16_t)(y - 1), (int16_t)wrow, kRowH}, kOn);
    char left[kTextLen + 1], nm[24];
    int room = (wrow - 4) / kRSmall.advance;
    sniprintf(left, sizeof left, "%03d %.*s", row, room > 4 ? room - 4 : 1, presets::row_name(bl.preset, row, nm, sizeof nm));
    c.put_text(kSlotRowL + i, kRSmall, r.x + 1, y, left, kAlignLeft | (on ? kTextInvert : 0));
  }
  draw_bar(c, r, y0, n, first);
}
CORE_COLD void draw_bank(Canvas& c, const Rect& r) {
  if (bl.preset >= 0) {
    draw_presets(c, r);
    return;
  }
  if (bl.browse) {
    draw_browse(c, r);
    return;
  }
  int k = bank_kind();
  if (bank_note_on()) {   // rui1040: SET B005 (the browser's file), or why FUNC + YES did nothing, in the tabs' place
    c.put_text(kSlotFree, kRSmall, r.x + (r.w - 6) / 2, r.y + 1, bl.note, kAlignCentre);
    for (int b = 1; b < 8; ++b) c.put_text(kSlotFree + b, kRSmall, 0, 0, nullptr);
  } else {
    draw_tabs(c, r, r.y + 1);
  }
  for (int i = 0; i < kRows; ++i) c.put_text(kSlotRowL + i, kRSmall, 0, 0, nullptr), c.put_text(kSlotRowR + i, kRSmall, 0, 0, nullptr);
  int first = bl.row - 2;
  if (first > srclib::kBankRows - kBankRowsShown) first = srclib::kBankRows - kBankRowsShown;
  if (first < 0) first = 0;
  const int y0 = r.y + 9, wrow = r.w - 6;
  for (int i = 0; i < kBankRowsShown; ++i) {
    int row = first + i, y = y0 + i * kRowH;
    bool on = row == bl.row;
    if (on) c.fill(Rect{(int16_t)r.x, (int16_t)(y - 1), (int16_t)wrow, kRowH}, kOn);
    char right[24] = "", left[kTextLen + 1];
    if (on) lst_note(k, bank_entry(bl.bank, row), right, sizeof right);   // LOADING, its length, FAILED
    int rl = (int)strlen(right), room = (wrow - 4) / kRSmall.advance - (rl ? rl + 1 : 0);
    sniprintf(left, sizeof left, "%03d %.*s", row, room > 4 ? room - 4 : 1, bank_name(k, bl.bank, row));
    uint8_t fl = on ? kTextInvert : 0;
    c.put_text(kSlotRowL + i, kRSmall, r.x + 1, y, left, kAlignLeft | fl);
    if (rl) c.put_text(kSlotRowR + i, kRSmall, r.x + wrow - 2, y, right, kAlignRight | fl);
  }
  int bx = r.x + r.w - 3, by = y0 - 1, bh = kBankRowsShown * kRowH;   // where the rows are in the bank's 128
  c.vline(bx, by, by + bh - 1, kDither);
  int th = bh * kBankRowsShown / srclib::kBankRows < 3 ? 3 : bh * kBankRowsShown / srclib::kBankRows;
  c.fill(Rect{(int16_t)(bx - 1), (int16_t)(by + (bh - th) * first / (srclib::kBankRows - kBankRowsShown)), 3, (int16_t)th}, kOn);
}
#endif
}  // namespace

CORE_COLD uint32_t wavegrain_key(const Cell& c, const Source& s) {
#if CORE_MSAMP
  int vi = s.voice() < 0 ? 0 : s.voice() & 7;
  if (c.arg == kDrawGrList || c.arg == kDrawWtList || c.arg == kDrawMsList) return list_key(c, s);
  if (c.arg == kDrawBankList) return bank_key();
  if (c.arg == kDrawWt) {
    int nf = 0;
    float pos = wt::viz_pos(vi, &nf);
    return wt_keys[vi].get((uint32_t)(wt::voice_entry(vi) + 2) * 2654435761u ^ (uint32_t)nf << 8 ^ (uint32_t)wt::entries() << 2,
                           (uint32_t)(pos * 255.0f), now_ms());
  }
  GrKey& k = gr_keys[vi];
  uint32_t now = now_ms();
  if (k.key && now - k.ms < kFrameMs) return k.key;   // a new picture at most every kFrameMs
  k.ms = now;
  static gr::Viz z;
  uint32_t h = 2166136261u;
  h = mixk(h, (uint32_t)(gr::src_entry(vi, s.code(0)) + 3) ^ gr::list_gen() << 8);   // the name shown
  if (gr::viz(vi, &z)) {
    h = mixk(h, (uint32_t)(z.entry + 3));
    h = mixk(h, (uint32_t)(z.head * 126.0f) | (uint32_t)z.live << 8 | (uint32_t)z.frozen << 9 | (uint32_t)(z.anim > 0) << 10);
    for (int i = 0; i < z.n; ++i) h = mixk(h, (uint32_t)(z.pos[i] * 126.0f) | (uint32_t)(z.amp[i] * 7.0f) << 8);
    if (z.live && !z.frozen) h = mixk(h, now / kFrameMs);   // the ring scrolls
  }
  k.key = h | 1;
  return k.key;
#else
  (void)c, (void)s;
  return 0;
#endif
}

CORE_COLD void wavegrain_draw(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot) {
#if CORE_MSAMP
  if (c.arg == kDrawGran) draw_gran(cv, r, s, slot);
  else if (c.arg == kDrawWt) draw_wt(cv, r, s, slot);
  else if (c.arg == kDrawBankList) draw_bank(cv, r);
  else draw_list(cv, r, c, s);
#else
  (void)cv, (void)r, (void)c, (void)s, (void)slot;
#endif
}

CORE_COLD const Page* wavegrain_page_for_machine(int machine) {
  return machine == kWaveMachine ? &kWtPage : machine == kGranMachine ? &kGrPage : nullptr;
}

#if CORE_MSAMP
namespace {
struct ListWatch {
  int machine = -1, vi = -1, last = -1;
  uint32_t ms = 0;            // the last turn
  bool turned = false, previewed = true;
  bool rested = true;         // the fallback's rest on a library row acted on (once a value)
};
ListWatch lw;
bool lw_pin = false;          // rui1036: P1's push held: the list stays open
struct Push {
  bool down = false;
  int v = -1;                 // P1 when it went down
  uint32_t ms = 0;
};
Push push;
// rui1036: the names the Rytm has (47), per list: a signature of what was sent (count, list generation), 0 none;
// rui1038: and the bank they are of (the Rytm keeps one bank a list)
uint32_t names_sig[3] = {0, 0, 0};
int names_bank[3] = {0, 0, 0};
}  // namespace
#endif

CORE_COLD const Page* src_list_page(const Page* p, const Source& s) {
#if CORE_MSAMP
  if (!p) return p;
  const Page* lp = p->machine == kGranMachine || p->machine == kChopMachine ? &kGrListPage : p->machine == kWaveMachine ? &kWtListPage
                 : p->machine == kMultiMachine ? &kMsListPage : nullptr;
  if (!lp) return p;
  int vi = s.voice(), v = s.code(0);
  uint32_t now = now_ms();
  if (lw.machine != p->machine || lw.vi != vi) {   // another voice / machine: its value taken in, no list
    lw = ListWatch();
    lw.machine = p->machine, lw.vi = vi, lw.last = v;
    return p;
  }
  if (bank_closed_ms && now - bank_closed_ms < 1500) lw.last = v;   // rui1038: the bank list's commit, not a turn
  if (v != lw.last) lw.last = v, lw.ms = now, lw.turned = true, lw.previewed = false, lw.rested = false;
  if (lw_pin) lw.turned = true, lw.ms = now;   // the push held: open
  if (!lw.turned || now - lw.ms >= kListMs) return p;
  if (!lw.previewed && now - lw.ms >= kSettleMs && p->machine == kGranMachine && vi >= 0) {
    lw.previewed = true;
    gr::preview(vi, gr::src_entry(vi, v));
  }
  return lp;
#else
  (void)s;
  return p;
#endif
}

CORE_COLD void src_list_push(bool down) {
#if CORE_MSAMP
  push_seen = true;
  lw_pin = down;
  uint32_t now = now_ms();
  if (down) {
    push.down = true, push.v = lw.last, push.ms = now;
    lw.turned = true, lw.ms = now;
    return;
  }
  push.down = false;   // (a click on a row: the pools have no folder rows to act on; the list only shows)
#else
  (void)down;
#endif
}
// rui1036 48: the Rytm's own P1 set as a knob turn (the kit keeps it); a refusal (an older build) leaves the voice's pick
// holding the value. GRANULAR's gr::pick writes its own.
CORE_COLD void src_list_write(int vi, int machine, int value) {
  (void)vi, (void)machine, (void)value;   // (the machines' picks write it: gr::pick, wt::pick, msamp::pick)
}
CORE_COLD bool src_list_pushes() {
#if CORE_MSAMP
  return push_seen;
#else
  return false;
#endif
}

CORE_COLD void src_list_quiet() {
#if CORE_MSAMP
  lw.machine = -1;
#endif
}

CORE_COLD void src_list_pin(bool on) {
#if CORE_MSAMP
  lw_pin = on;
#else
  (void)on;
#endif
}

// rui1036: 47 <list> <replace> <total> {<slot> <13 name bytes>} x 8: the lists' names for the Rytm's own value texts
// (GRANULAR SRC 0, WAVETABLE TBL 1, MULTISAMPLE INST 2: the knob's value v names entry min(v, total - 1)); sent when a
// list changed (its count, GRANULAR's generation) and after each hello (src_names_resend).
CORE_COLD void src_names_resend() {   // (each hello)
  names_sig[0] = names_sig[1] = names_sig[2] = 0;
#if CORE_MSAMP
  func_seen = false;   // 0.5.0: the new keys again only by this Rytm's caps or its first 2
#endif
}
CORE_COLD int src_names_tick() {
  int sent = 0;
#if CORE_MSAMP
  int st = remote_src_track(), sm = st >= 0 ? remote_track_machine(st) : -1, s14 = st >= 0 ? remote_track_src14(st) : -1;
  for (int k = 0; k < 3; ++k) {
    int n = lst_n(k);
    if (n < 0) n = 0;
    // rui1038: the bank the Rytm should name: the open list's, else the SRC view's P1's (its knob text), else as sent
    int bank = names_bank[k];
    if (bl.on && bank_kind() == k) bank = bl.bank;
    else if (sm >= 0 && ((sm & 0x78) == lst_machine(k) || (k == 0 && (sm & 0x78) == kDspSample)) && s14 >= 0)
      bank = (s14 & 0x7F) >> 4;   // (rui1045: DSP SAMPLE's SMP names list 0 too)
    uint32_t sig = 0x80000000u | (uint32_t)n << 16 | (lst_gen(k) & 0xFFFF) | (uint32_t)bank << 28;   // (a browse: the gen)
    if (sig == names_sig[k]) continue;
    names_sig[k] = sig;
    names_bank[k] = bank;
    // the pools design 3.5: every bank in the 47 form with the bank byte (bank A too: its knob text never falls back
    // to the old IN / MAIN texts); bank A's rows all named (000 OFF, an empty row "A005"), B..H the rows that hold one
    // (the Rytm names OFF / empty there itself)
    int row = bank ? 1 : 0;
    bool first = true;
    while (first || row < srclib::kBankRows) {
      uint8_t m[11 + 8 * 14];
      int c = 0;
      m[c++] = 0xF0, m[c++] = 0x7D, m[c++] = 0x52, m[c++] = 0x54, m[c++] = 0x47, m[c++] = 1;
      m[c++] = (uint8_t)k, m[c++] = first ? 1 : 0, m[c++] = 127, m[c++] = (uint8_t)bank;
      for (int i = 0; i < 8 && row < srclib::kBankRows; ++row) {
        int e = bank * srclib::kBankRows + row;
        int lb = lst_lib(k, e);
        if (bank && (lb == srclib::kLibOff || lb == srclib::kLibHole)) continue;
        char nb[48];
        const char* nm;
        if (lb == srclib::kLibOff) nm = "OFF";
        else if (lb == srclib::kLibHole) sniprintf(nb, sizeof nb, "%c%03d", 'A' + bank, row), nm = nb;
        else nm = row_name(k, e, nb, sizeof nb);
        m[c++] = (uint8_t)row;
        int j = 0;
        for (; nm && nm[j] && j < 13; ++j) m[c++] = (uint8_t)(nm[j] >= 0x20 && nm[j] < 0x7F ? (nm[j] >= 'a' && nm[j] <= 'z' ? nm[j] - 32 : nm[j]) : '_');
        for (; j < 13; ++j) m[c++] = 0;
        ++i;
      }
      m[c++] = 0xF7;
      if (first || c > 11) to_rytm(m, c), ++sent;
      first = false;
    }
  }
#endif
  return sent;
}

// ---- rui1038: the bank list (4A)
#if CORE_MSAMP
namespace {
CORE_COLD void browse_send(int t, int on) {   // rui1040: 4A 01 <track> <browse 0/1> to the Rytm (YES / NO the browser's)
  const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, (uint8_t)t, (uint8_t)on, 0xF7};
  to_rytm(m, 9);
}
CORE_COLD void preview_off() {   // 0.5.0: the browser's preview (an INFO read is left to finish)
  bl.hold = false;
  if (preview::sounding()) preview::stop();
}
CORE_COLD void browse_end(const char* why) {
  preview_off();
  bl.browse = false, bl.menu = -1, bl.seek = false, bl.info_wait = false, bl.edit_next = false;
  if (why) bank_note(why);
  browse_send(bl.track, 0);
}
// rui1040's FUNC + YES on a pool list's row (why 6: from the SRC view, fix1049), and 0.5.0's RIGHT in the list: the
// card's files for the list's row (the refusals: row 000, an input / built-in row, the pool locked). True: open.
CORE_COLD bool browse_open(int t, int why) {
  int k = bank_kind();
  // 0.5.0 (the owner): not on SAMPLER - its OFF is a choice (SMP OFF unloads the track), refused as on the list
  if (!bl.row && why == 6 && bl.machine != kSmplMachine) {   // fix1049: FUNC + YES on the SRC view (rytm-mods 0026 1049) on OFF: the bank's first free
    for (int r = 1; r < srclib::kBankRows && !bl.row; ++r)   // row (an empty pool's first file: A001 on DSP SAMPLE)
      if (pools::at(k, bank_entry(bl.bank, r)).type == pools::kEmpty) bl.row = r, bl.row_auto = true;
    if (!bl.row) {
      ++bl.refusals;
      browse_end("BANK FULL: PICK A ROW");
      return false;
    }
  }
  int e = bank_entry(bl.bank, bl.row);
  const pools::Entry& pe = pools::at(k, e);
  if (!bl.row || pe.type == pools::kInput || pe.type == pools::kBuiltin || pools::locked(k)) {   // the only refusals
    ++bl.refusals;
    browse_end(!bl.row ? "000 IS OFF" : pools::locked(k) ? "POOL LOCKED" : "BUILT-IN: UNLOAD FIRST");
    return false;
  }
  srclib::open(bank_kind());   // its folder listed (a browser not opened since boot had none: READING forever)
  bl.browse = true, bl.cur = 0, bl.bms = now_ms(), bl.bpreviewed = false, bl.note_on = false, bl.ask[0] = 0, bl.menu = -1;
  browse_send(t, 1);
  log(CORE_STR("rui: bank list on track %d: files for %c%03d"), t, 'A' + bl.bank, bl.row);
  return true;
}
}  // namespace
#endif
CORE_COLD void src_bank_list(int t, int vi, int machine, int code, int on, int why) {
#if CORE_MSAMP
  if (on == 3) {   // rui1040: FUNC + YES on the list's row - the card's files for that row (B..H, rows 1..127)
    if (!bl.on || bl.track != t) {
      browse_send(t, 0);
      return;
    }
    if (bl.preset >= 0) {   // the card's presets (every row: an add goes to the list's first empty slot)
      bl.browse = true, bl.cur = 0, bl.folder = bl.preset, bl.note_on = false, bl.pgen = presets::gen(), bl.rcur = -1;
      browse_send(t, 1);
      log(CORE_STR("rui: preset list on track %d: the card's presets"), t);
      return;
    }
    browse_open(t, why);
    return;
  }
  if (on) {
    if (machine != kGranMachine && machine != kWaveMachine && machine != kMultiMachine && !sample_machine(machine) &&
        machine != kChopMachine && machine != kJuno && machine != kFm)
      return;
    int commits = bl.commits;
    batch_new = true;   // FILL: the next quick load starts a batch
    bl = BankList();
    bl.on = true, bl.track = t, bl.vi = vi & 7, bl.machine = machine, bl.commits = commits;
    if (machine == kJuno || machine == kFm) {
      bl.preset = machine == kJuno ? presets::kIxJuno : presets::kIxFm;
      int n = presets::rows(bl.preset), row = srclib::index_of(code);
      bl.row = row >= n ? n - 1 : row;   // (as PAT plays: the last row)
      bl.code0 = code, bl.ms = now_ms();
      log(CORE_STR("rui: preset list on track %d: %03d"), t, bl.row);
      return;
    }
    bl.bank = srclib::bank_of(code), bl.row = srclib::index_of(code), bl.code0 = code, bl.ms = now_ms();
    pools::shown(bank_kind(), bl.bank);
    log(CORE_STR("rui: bank list on track %d: %c%03d"), t, 'A' + bl.bank, bl.row);
    return;
  }
  if (!bl.on || bl.track != t) return;
  preview_off();
  bl.on = false, bl.browse = false, bl.menu = -1, bl.edit_code = -1;
  bank_closed_ms = now_ms() | 1;
  if (why == 1) {   // YES: the cursor is the new P1 (the Rytm locks it on the steps held at the YES, else the kit)
    remote_track_write(t, sample_machine(bl.machine) ? 10 : 0, bl.row, srclib::lo_of(bl.bank * srclib::kBankRows), 1);   // (P11: SMP)
    if (bl.preset < 0) pools::seen(bank_kind(), bank_entry(bl.bank, bl.row));   // (the used set: a committed row)
    ++bl.commits;
    log(CORE_STR("rui: bank list on track %d: %c%03d chosen"), t, 'A' + bl.bank, bl.row);
  }
#else
  (void)t, (void)vi, (void)machine, (void)code, (void)on, (void)why;
#endif
}
#if CORE_MSAMP
namespace {
CORE_COLD void preset_added(int s, bool there) {   // the add's word; the list's cursor on its row, as a file set
  char w[24];
  if (s == presets::kAddFull) sniprintf(w, sizeof w, "LIST FULL");
  else if (s == presets::kAddBusy) sniprintf(w, sizeof w, "PROJECT LOADING");
  else if (s < 0) sniprintf(w, sizeof w, "NOT READ");
  else sniprintf(w, sizeof w, "%s %03d", there ? "IN LIST" : "ADDED", s);
  if (s < 0) {
    bank_note(w);
    return;
  }
  ++bl.assigns;
  bl.row = s;
  browse_end(w);
}
CORE_COLD bool preset_key(int code) {
  if (bl.browse) {   // UP / DOWN a row, RIGHT / YES into a folder, YES on a preset adds it, LEFT / NO up (at the top: the list)
    preset_sync();
    int n = presets::ready() ? presets::count(bl.folder) : 0;
    bool asked = code == 0x44 && bl.rcur == bl.cur && bl.rfolder == bl.folder;
    bl.rcur = -1;
    if (code == 0x42 && bl.cur > 0) --bl.cur;
    else if (code == 0x43 && bl.cur < n - 1) ++bl.cur;
    else if ((code == 0x41 || code == 0x44) && presets::ready() && presets::is_dir(bl.folder, bl.cur)) {
      bl.folder = presets::child(bl.folder, bl.cur), bl.cur = 0;
    } else if (code == 0x44 && n && !bl.reading) {
      bool there = false;
      int s = presets::add(bl.preset, bl.folder, bl.cur, &there, asked ? bl.row : -1);
      if (s == presets::kAddRead) bl.reading = true;
      else if (s == presets::kAddFull && !asked) {   // no free row: the list's cursor row, once said
        char w[24];
        sniprintf(w, sizeof w, "REPLACE %03d? YES", bl.row);
        bank_note(w);
        bl.rcur = bl.cur, bl.rfolder = bl.folder;
      } else preset_added(s, there);
    } else if (code == 0x40 || code == 0x45) {
      int up = presets::ready() ? presets::parent(bl.folder) : -1;
      if (up >= 0) bl.cur = presets::row_in_parent(bl.folder), bl.folder = up;
      else browse_end(nullptr);
    }
    return true;
  }
  if (code > 0x43) return true;
  int n = presets::rows(bl.preset);
  if (code == 0x42 && bl.row > 0) --bl.row;       // UP / DOWN; LEFT / RIGHT 8 rows, no banks here
  else if (code == 0x43 && bl.row < n - 1) ++bl.row;
  else if (code == 0x40) bl.row = bl.row < 8 ? 0 : bl.row - 8;
  else if (code == 0x41) bl.row = bl.row + 8 >= n ? n - 1 : bl.row + 8;
  return true;
}
}  // namespace
#endif
#if CORE_MSAMP
namespace {
// 0.5.0: the first empty row of list k from entry `from` on (row 000 of each bank skipped), -1 none
CORE_COLD int next_empty(int k, int from) {
  for (int e = from; e < srclib::kEntries; ++e)
    if (srclib::index_of(e) && pools::at(k, e).type == pools::kEmpty) return e;
  return -1;
}
// A file (or an input / built-in) of the browser's cursor into the pool row e: the pool's rules (one entry per hash,
// P3's ask on a row in use, refused while the Rytm plays). `how` 0: YES / LOAD (the list stays on the row), 1: RIGHT's
// quick load (the list on the next empty row after it), 2: LOAD TO NEXT EMPTY (e: that row; the list on it). The key
// that asked confirms (ask_code).
CORE_COLD void load_cur(int k, int e, int how, int key) {
  char path[srclib::kPath];
  int out = -1, res = pools::kErrPath, type = 0, id = 0;
  preview_off();
  if (srclib::item_path(k, bl.cur, path, sizeof path)) {
    int conf = !strcmp(bl.ask, path) && bl.ask_code == key ? pools::kConfirmed : pools::kAsk;   // P3: the second press on it confirms
    res = srclib::special(k, path, &type, &id) ? pools::set_special(k, e, type, id, conf) : pools::set_file(k, e, path, &out, conf);
  }
  char w[24];
  int b = e >> 7, r = e & 127;
  if (res == pools::kOk || res == pools::kPending) {
    sniprintf(w, sizeof w, "%s %c%03d", res == pools::kPending ? "READING" : "SET", 'A' + b, r);
    ++bl.assigns;
    sniprintf(last_file[k], sizeof last_file[k], "%s", path);
    log(CORE_STR("rui: bank list on track %d: %c%03d = %s"), bl.track, 'A' + b, r, path);
    bool edit = bl.edit_next;
    browse_end(w);
    bl.bank = b, bl.row = r;
    if (edit) bl.edit_code = e, bl.edit_ms = now_ms();   // SAMPLE EDIT's load (at once, or its ask confirmed)
    if (how == 1 && bl.track >= 0 && bl.track < 12) {   // FILL: the batch grows (a new opening: a new batch)
      Batch& q = batch[bl.track];
      if (batch_new || q.kind != k) q.kind = (int8_t)k, q.first = (int16_t)e;
      q.last = (int16_t)e, batch_new = false;
    }
    if (how == 1) {   // the list on the next empty row (none: stays, said)
      int n = next_empty(k, e + 1);
      if (n >= 0) bl.bank = n >> 7, bl.row = n & 127;
      else bank_note("POOL FULL");
    }
    pools::shown(k, bl.bank);   // (takes go to the bank shown)
    bl.previewed = false, bl.ms = now_ms();   // the row's new file: previewed as a list row (loads it)
  } else if (res == pools::kExisting && out >= 0) {   // one entry per hash
    sniprintf(w, sizeof w, "IN POOL %c%03d", 'A' + (out >> 7), out & 127);
    if (how == 1) sniprintf(last_file[k], sizeof last_file[k], "%s", path);   // (the quick load goes on after it)
    bool edit = bl.edit_next;
    browse_end(w);
    if (how != 1) bl.bank = out >> 7, bl.row = out & 127, bl.previewed = false, bl.ms = now_ms();   // the list goes to its row
    if (edit) bl.edit_code = out, bl.edit_ms = now_ms();   // (SAMPLE EDIT: the row that has it)
    pools::shown(k, bl.bank);
  } else if (res == pools::kErrUsed) {   // P3: ask - the row plays somewhere (a sound on it now, or seen playing)
    int u = pools::users(k, e);
    const char* kn = key == 0x41 ? "RIGHT" : "YES";
    sniprintf(bl.ask, sizeof bl.ask, "%s", path);
    bl.ask_code = key;
    if (u) sniprintf(w, sizeof w, "%c%03d USED %d: %s=SET", 'A' + b, r, u, kn);
    else sniprintf(w, sizeof w, "%c%03d IN USE: %s=SET", 'A' + b, r, kn);
    bank_note(w);
  } else {
    ++bl.refusals;
    bank_note(res == pools::kErrPlaying ? "PLAYING: STOP FIRST" : pools::result_text(res));
  }
}
// 0.5.0: RIGHT's re-entry - the cursor on the file after the last one loaded, once the folder is listed
CORE_COLD void seek_last() {
  int k = bank_kind();
  if (!bl.seek || !srclib::listing_done(k)) return;
  bl.seek = false;
  const char* lf = last_file[k];
  const char* base = strrchr(lf, '/');
  int n = srclib::count(k), dl = base ? (int)(base - lf) : -1;
  const char* cw = srclib::cwd(k);
  if (!base || dl != (int)strlen(cw) || strncmp(lf, cw, dl)) return;   // (another folder now: its top)
  for (int i = 0; i < n; ++i) {
    const char* nm = "";
    bool dir = false;
    if (srclib::item(k, i, &nm, &dir, nullptr) && !dir && !strcmp(nm, base + 1)) {
      bl.cur = i + 1 < n ? i + 1 : i;
      return;
    }
  }
}
CORE_COLD void preview_toggle(int k) {
  char path[srclib::kPath];
  const char* nm = "";
  bool dir = true;
  if (preview::sounding()) {
    preview::stop();
    return;
  }
  if (!srclib::item(k, bl.cur, &nm, &dir, nullptr) || dir || !srclib::item_path(k, bl.cur, path, sizeof path)) return;
  if (k == 2 || preview::start(path) < 0) bank_note("NO PREVIEW");   // (an instrument, an input / built-in)
}
// FUNC + YES (0.5.0, the owner's hardware test): the file under the cursor plays while YES is held, as the stock
// browser's; YES's release (31 44 0, with FUNC still held or not: the 0026 forwards it either way) stops it
CORE_COLD void preview_hold(int k) {
  char path[srclib::kPath];
  const char* nm = "";
  bool dir = true;
  preview_off();
  if (!srclib::item(k, bl.cur, &nm, &dir, nullptr) || dir || !srclib::item_path(k, bl.cur, path, sizeof path)) return;
  if (k == 2 || preview::start(path) < 0) bank_note("NO PREVIEW");   // (an instrument, an input / built-in)
  else bl.hold = true;
}
// PREVIEW LEVEL (0.5.0): knob H in the files (the 0026 forwards it while the host's browser is open), a Sidecar setting
constexpr int kPrevKnob = 7;
CORE_COLD void preview_level(int delta) {
  preview::set_level_db(preview::level_db() + (delta > 0 ? 1 : -1));
  char b[24];
  sniprintf(b, sizeof b, "PREVIEW LEVEL %d DB", preview::level_db());
  bank_note(b);
}
CORE_COLD int menu_items() { return kMItems; }
// The file menu's keys: UP / DOWN an item, YES / RIGHT do it, NO / LEFT close the menu
CORE_COLD void menu_key(int code) {
  int k = bank_kind(), e = bank_entry(bl.bank, bl.row);
  if (code == 0x42 || code == 0x43) {
    int m = bl.menu;
    do m += code == 0x42 ? -1 : 1;
    while (m >= 0 && m < menu_items() && !menu_has(m));
    if (m >= 0 && m < menu_items()) bl.menu = m;
    return;
  }
  if (code == 0x40 || code == 0x45) {
    bl.menu = -1;
    return;
  }
  if (code != 0x41 && code != 0x44) return;
  int it = bl.menu;
  bl.menu = -1;
  char path[srclib::kPath];
  switch (it) {
    case kMLoad:
      load_cur(k, e, 0, 0x44);
      break;
    case kMLoadNext: {
      int n = next_empty(k, e);
      if (n < 0) bank_note("POOL FULL");
      else load_cur(k, n, 2, 0x44);
      break;
    }
    case kMPreview:
      preview_toggle(k);
      break;
    case kMEdit:   // the load, then the editor on that row (its ask confirmed by YES: the same)
      bl.edit_next = true;
      load_cur(k, e, 0, 0x44);
      if (!bl.browse || !bl.ask[0]) bl.edit_next = false;
      break;
    case kMInfo:
      if (srclib::item_path(k, bl.cur, path, sizeof path) && path[0] == '/' && k != 2) {
        preview::info(path);
        bl.info_wait = true;
        bank_note("READING");
      } else {
        bank_note("NO INFO");
      }
      break;
  }
}
}  // namespace
#endif
CORE_COLD bool src_bank_new_keys() {
#if CORE_MSAMP
  return (remote_caps() & kCapsBrowseFunc) || func_seen;
#else
  return false;
#endif
}
CORE_COLD bool src_bank_key(int code, int down) {
#if CORE_MSAMP
  if (!bl.on || code < 0x40 || code > 0x45) return false;
  if (!down) {
    if (code == 0x44 && bl.hold) preview_off();   // 0.5.0: FUNC + YES's YES released: its preview stops
    return true;
  }
  bool func = down == 2;
  if (func) func_seen = true;   // a 0.5.0 0026 (its FUNC inside the open list)
  bool nk = src_bank_new_keys() && bl.preset < 0;   // (the JUNO / FM preset lists: as before)
  // FUNC + NO in a JUNO / FM preset list: the 0.5.0 0026 closes that list itself (4A on 0, why 2, rytm-mods 9fe0650)
  // and never forwards it; kept for a build that did (a NO here: the host's list closed, the Rytm's on its next key)
  if (bl.preset >= 0 && !bl.browse && func && code == 0x45) {
    src_bank_list(bl.track, bl.vi, bl.machine, 0, 0, 2);
    return true;
  }
  if (bl.preset >= 0) return preset_key(code);
  if (bl.browse) {   // rui1040: the files - UP / DOWN a row, RIGHT / YES into a folder, YES on a file sets the row,
    int k = bank_kind(), n = srclib::count(k);   // LEFT / NO up a folder (at the top: back to the list)
    if (nk && bl.menu >= 0) {   // 0.5.0: the file menu
      menu_key(code);
      if (code != 0x44 || bl.browse) bl.bms = now_ms(), bl.bpreviewed = false;
      return true;
    }
    const char* nm = "";
    bool dir = false;
    bool have = srclib::item(k, bl.cur, &nm, &dir, nullptr);
    char path[srclib::kPath];
    if (nk && func && code == 0x44) {   // 0.5.0: FUNC + YES - the preview while held
      // the Rytm repeats a held key: while this preview's YES is held, a repeat is ignored (no restart); YES's
      // release clears bl.hold, then FUNC + YES plays again
      if (have && !dir && !bl.hold) preview_hold(k);
      return true;
    }
    preview_off();   // any other key: the preview stops
    bl.seek = false;
    int step = nk && func ? 8 : 1;   // 0.5.0: FUNC + UP / DOWN a page
    if (code == 0x42 && bl.cur > 0) bl.cur = bl.cur > step ? bl.cur - step : 0;
    else if (code == 0x43 && bl.cur < n - 1) bl.cur = bl.cur + step < n ? bl.cur + step : n - 1;
    else if ((code == 0x41 || code == 0x44) && have && dir) {
      if (srclib::item_path(k, bl.cur, path, sizeof path) && srclib::cd(k, path)) bl.cur = 0;
    } else if (code == 0x44 && have && !dir) {   // a file (or an input / built-in) into the row: the pool's rules
      load_cur(k, bank_entry(bl.bank, bl.row), 0, 0x44);
    } else if (nk && code == 0x41 && have && !dir) {   // 0.5.0: RIGHT loads, FUNC + RIGHT the file menu
      if (func) bl.menu = kMLoad;
      else load_cur(k, bank_entry(bl.bank, bl.row), 1, 0x41);
    } else if (code == 0x40 || code == 0x45) {
      if (srclib::cwd(k)[0]) srclib::up(k), bl.cur = 0;
      else {
        browse_end(nullptr);
        if (bl.row_auto) bl.row = 0;   // (review: nothing set there: the list back on OFF, not an empty row)
      }
    }
    if (bl.browse == false) bl.row_auto = false;
    if (code != bl.ask_code) bl.ask[0] = 0, bl.edit_next = false;   // (moved away: the next press asks again)
    bl.bms = now_ms(), bl.bpreviewed = false;
    return true;
  }
  if (nk) {   // 0.5.0: the list - FUNC + LEFT / RIGHT banks, RIGHT the files (the last folder, after the last file),
              // LEFT nothing, FUNC + UP / FUNC + NO the bank's OFF row (YES then sets it: the unload)
    if (code == 0x40 && func && bl.bank > 0) --bl.bank;
    else if (code == 0x41 && func && bl.bank < srclib::kBanks - 1) ++bl.bank;
    else if (code == 0x41 && !func) {
      int k = bank_kind();
      const char* lf = last_file[k];
      const char* sl = lf[0] ? strrchr(lf, '/') : nullptr;
      if (sl) {   // the last file's folder (gone: the browser's own)
        char dir[srclib::kPath];
        sniprintf(dir, sizeof dir, "%.*s", (int)(sl - lf), lf);
        if (strcmp(srclib::cwd(k), dir)) srclib::cd(k, dir);
      }
      if (browse_open(bl.track, 0) && sl) bl.seek = true, seek_last();
      return true;
    } else if (code == 0x42 && func) bl.row = 0;
    else if (code == 0x45 && func) bl.row = 0, bank_note("OFF: YES SETS");   // (the owner: YES confirms the unload)
    else if (code == 0x42 && bl.row > 0) --bl.row;
    else if (code == 0x43 && bl.row < srclib::kBankRows - 1) ++bl.row;
    pools::shown(bank_kind(), bl.bank);
    bl.ms = now_ms(), bl.previewed = false;
    return true;
  }
  if (code > 0x43) return true;   // (YES / NO come only while the files are open: a late one after the Rytm let go)
  if (code == 0x40 && bl.bank > 0) --bl.bank;                                 // LEFT: the bank before
  else if (code == 0x41 && bl.bank < srclib::kBanks - 1) ++bl.bank;           // RIGHT: the next
  else if (code == 0x42 && bl.row > 0) --bl.row;                              // UP / DOWN: a row
  else if (code == 0x43 && bl.row < srclib::kBankRows - 1) ++bl.row;
  pools::shown(bank_kind(), bl.bank);   // takes go to the bank last shown
  bl.ms = now_ms(), bl.previewed = false;
  return true;
#else
  (void)code, (void)down;
  return false;
#endif
}
CORE_COLD bool src_bank_enc(int e, int delta, bool func) {
#if CORE_MSAMP
  if (bl.on && bl.browse && bl.preset < 0 && e == kPrevKnob) {   // 0.5.0: PREVIEW LEVEL, a dB a step (paced as a list's)
    if (int d = list_turn(e, delta)) preview_level(d);
    return true;
  }
  if (!bl.on || e != 0) return false;
  delta = list_turn_long(e, delta);
  if (!delta) return true;
  if (bl.preset >= 0) {   // (the index, or the list's rows)
    preset_sync();
    int& p = bl.browse ? bl.cur : bl.row;
    int n = bl.browse ? (presets::ready() ? presets::count(bl.folder) : 0) : presets::rows(bl.preset);
    bl.rcur = -1;
    p += delta * (func ? 8 : 1);
    if (p >= n) p = n - 1;
    if (p < 0) p = 0;
    return true;
  }
  if (bl.browse && bl.menu >= 0) {   // 0.5.0: the file menu's item
    int m = bl.menu + (delta > 0 ? 1 : -1);
    if (m >= 0 && m < menu_items() && !menu_has(m)) m += delta > 0 ? 1 : -1;
    if (m >= 0 && m < menu_items()) bl.menu = m;
    return true;
  }
  if (bl.browse) {   // rui1040: knob A moves the files' cursor
    preview_off(), bl.seek = false;   // 0.5.0: a move stops the preview
    int n = srclib::count(bank_kind()), c = bl.cur + delta * (func ? 8 : 1);
    bl.cur = c >= n ? n - 1 : c;
    if (bl.cur < 0) bl.cur = 0;
    bl.bms = now_ms(), bl.bpreviewed = false;
    return true;
  }
  int r = bl.row + delta * (func ? 8 : 1);
  bl.row = r < 0 ? 0 : r >= srclib::kBankRows ? srclib::kBankRows - 1 : r;
  bl.ms = now_ms(), bl.previewed = false;
  return true;
#else
  (void)e, (void)delta, (void)func;
  return false;
#endif
}
CORE_COLD const Page* src_bank_page(int t, int machine) {
#if CORE_MSAMP
  if (!bl.on || bl.track != t || machine != bl.machine) return nullptr;
  if (bl.preset >= 0) {
    int s = 0;
    bool there = false;
    if (presets::add_poll(&s, &there) && bl.reading) {
      bl.reading = false;
      if (bl.browse) preset_added(s, there);
    }
    return bl.preset == presets::kIxJuno ? &kJnBankPage : &kFmBankPage;
  }
  seek_last();   // 0.5.0: RIGHT's re-entry, once the folder is listed
  if (bl.info_wait && preview::status().st != preview::kHeader && preview::status().st != preview::kOpening) {   // INFO's answer
    bl.info_wait = false;
    const preview::Status& ps = preview::status();
    char w[24];
    if (ps.st == preview::kDone && !ps.play && ps.rate) {
      uint32_t ds = (uint32_t)((uint64_t)ps.frames * 10 / ps.rate);
      sniprintf(w, sizeof w, "%uK %uB %s %u.%uS", (unsigned)((ps.rate + 500) / 1000), (unsigned)ps.bits, ps.ch > 1 ? "ST" : "MO",
                (unsigned)(ds / 10), (unsigned)(ds % 10));
    } else {
      sniprintf(w, sizeof w, "NOT AUDIO");
    }
    bank_note(w);
  }
  if (bl.edit_code >= 0) {   // 0.5.0: the file menu's SAMPLE EDIT - the editor once the row's file is in (or a refusal)
    const pools::Entry& pe = pools::at(pools::kSamples, bl.edit_code);
    if (pe.type == pools::kSample || now_ms() - bl.edit_ms > 5000 || !pools::pending()) {
      int c = bl.edit_code;
      bl.edit_code = -1;
      bl.on = false, bl.browse = false;   // (the list's page gives way; the Rytm's list closes on its next key)
      bank_closed_ms = now_ms() | 1;
      sedit_page_open(-1, c);
      return nullptr;
    }
  }
  if (bl.browse && !bl.bpreviewed && preview::sounding()) bl.bpreviewed = true;   // (not over the dry preview)
  if (bl.browse && !bl.bpreviewed && now_ms() - bl.bms >= kSettleMs) {   // rui1040: a file rested on - previewed when
    bl.bpreviewed = true;                                                   // the machine's list has it (its LIBRARY
    int k = bank_kind();                                                    // window: the same browser's folder)
    const char* nm = "";
    bool dir = true;
    char path[srclib::kPath];
    if (k < 2 && srclib::item(k, bl.cur, &nm, &dir, nullptr) && !dir && srclib::item_path(k, bl.cur, path, sizeof path)) {
      lib::Ref r;   // a file the pool has already (by its hash): previewed from its row
      lib::ref_for_path(path, &r);
      int e = pools::find_hash(k, r);
      if (e >= 0 && bl.machine == kGranMachine) gr::preview(bl.vi, e);   // (rui1045: not on a DSP SAMPLE voice)
      else if (e >= 0 && bl.machine == kWaveMachine) wt::preview(bl.vi, e);
    }
  }
  if (!bl.browse && !bl.previewed && now_ms() - bl.ms >= kSettleMs) {   // the cursor rested: GRANULAR / WAVETABLE preview it (loads it)
    bl.previewed = true;
    int e = bank_entry(bl.bank, bl.row), k = bank_kind();
    if (lst_lib(k, e) == srclib::kLibNone) {
      if (bl.machine == kGranMachine) gr::preview(bl.vi, e);   // (rui1045: no GRANULAR preview on a DSP SAMPLE voice)
      else if (bl.machine == kWaveMachine) wt::preview(bl.vi, e);
    }
  }
  return machine == kGranMachine || machine == kChopMachine ? &kGrBankPage : machine == kWaveMachine ? &kWtBankPage
       : machine == kDspSample ? &kDsBankPage : machine == kSmplMachine ? &kDsBankPageDt : &kMsBankPage;
#else
  (void)t, (void)machine;
  return nullptr;
#endif
}
CORE_COLD bool fill_batch(int t, int kind, int* first, int* last) {
#if CORE_MSAMP
  if (t < 0 || t >= 12 || batch[t].kind != kind || batch[t].first < 0) return false;
  *first = batch[t].first, *last = batch[t].last;
  return true;
#else
  (void)t, (void)kind, (void)first, (void)last;
  return false;
#endif
}
CORE_COLD BankListState src_bank_state() {
#if CORE_MSAMP
  return BankListState{bl.on, bl.track, bl.machine, bl.bank, bl.row, bl.commits, bl.browse, bl.cur, bl.assigns, bl.refusals, bl.menu};
#else
  return BankListState{false, -1, -1, 0, 0, 0, false, 0, 0, 0, -1};
#endif
}

CORE_COLD const char* src_value_name(const Page& p, int param, const Source& s) {
#if CORE_MSAMP
  if (param != 0) return nullptr;
  int k = p.machine == kGranMachine || p.machine == kChopMachine ? 0 : p.machine == kWaveMachine ? 1
        : p.machine == kMultiMachine ? 2 : -1;
  if (k < 0) return nullptr;
  int vi = s.voice() < 0 ? 0 : s.voice() & 7, e = lst_sel(k, vi, s.code(0));
  return e >= 0 ? lst_name(k, e) : "-";
#else
  (void)p, (void)param, (void)s;
  return nullptr;
#endif
}

}  // namespace ui
}  // namespace core
