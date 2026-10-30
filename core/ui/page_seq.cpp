// core/ui/page_seq.cpp - see extra.h: the Sidecar sequencer's SEQ page (core/seq/seq.h). Cold. CORE_POLY builds only.
//
// The title bar is the badge: SIDECAR SEQ and the track (BD .. CB). Under it a line with the pattern (A01), the page
// (PG 2/4), the length and the transport (PLAY / STOP / PAUSE, REC inverted while live recording). Then the page's 16
// steps as a strip (a filled box a step with notes, a notched one a chord, a dot an empty step, nothing past the
// length; the playing step framed, a held one underlined), and the page as a piano roll: each note a bar from its start
// (the step + its micro offset: strums and loose timing show as staggered starts) over its length, pitch upward, beats
// dotted, the playhead an inverted column.
// A held trig (step edit): a 3-octave keyboard with the step's notes (each octave's C named over it, its window kept
// while the notes fit), the timing lane (each note's start and length
// from one step early to one step late, the step's start dotted: a broken chord is a staircase), and the eight
// encoders' values: NOTE VEL LEN MICRO / CHD INV STRUM SEL (the one turned last inverted).
// The encoders without a held trig: A TRK, B SEQ (on / off), C LEN (AUTO, 16 .. 128; FUNC by one), D QNT, E FOLW,
// F LAT. The bottom bar names the one turned last, else a hint.
#if CORE_POLY
#include <string.h>

#include "fx/insert.h"
#include "fx/send_fx.h"
#include "inst.h"
#include "inst_alloc.h"
#include "machines.h"
#include "poly.h"
#include "seq/seq.h"
#include "seq/rng.h"
#include "ui/page_keys.h"
#include "ui/extra.h"
#include "ui/page.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

UI_ROM const char* const kTrackNames[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
UI_ROM const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr int kStripY = 18;   // (the roll: rows 27..53, roll()'s kRollY0 / kRollY1; NOTE EDIT's 8..38)
// chrorange: the keyboard's C names over it (kOctY), the keyboard shorter for them (its black keys 4 rows, white 6;
// a gap row, then the lane at 32 as before: an 8-note step's rows 32..39 stay clear of the values at 40)
constexpr int kHeldStripY = 10, kOctY = 19, kKeysY = 25, kKeysH = 6, kLaneY = 32, kValY0 = 40, kValY1 = 47;
constexpr int kSlotOct = kSlotFree + 12;   // the three C names (the held view's other items: kSlotFree, kSlotCells..)

CORE_COLD const char* note_name(int n, char* b, int cap) {
  sniprintf(b, cap, CORE_STR("%s%d"), kNoteNames[n % 12], n / 12 - 1);
  return b;
}
CORE_COLD void steps_text(int units, char* b, int cap) {   // 1/96 step units as steps, 2 decimals
  int h = (units * 100 + seq::kSub / 2) / seq::kSub;
  sniprintf(b, cap, CORE_STR("%d.%02d"), h / 100, h % 100);
}

CORE_COLD void strip(Canvas& c, const seq::View& w, int kStripY) {
  const seq::Track* t = w.t;
  for (int k = 0; k < 16; ++k) {
    int idx = w.page * seq::kStepsPerPage + k, x = 8 * k;
    if (idx >= w.length) continue;
    const seq::Step* s = t ? &t->step[idx] : nullptr;
    if (s && s->n) {
      c.fill(Rect{(int16_t)(x + 1), (int16_t)(kStripY + 1), 6, 6}, kOn);
      if (s->n > 1) c.fill(Rect{(int16_t)(x + 3), (int16_t)(kStripY + 1), 2, 2}, kOff);   // a chord: notched
    } else {
      c.pixel(x + 3, kStripY + 3), c.pixel(x + 4, kStripY + 4);
    }
    if (s && s->nl) {   // seq-ux: the lock lane over the strip: a tick a lock (1..6 px), a lock trig's box hollow
      int n = s->nl > 6 ? 6 : s->nl;
      c.hline(x + 1, x + n, kStripY - 1);
      if (!s->n) c.rect(Rect{(int16_t)(x + 1), (int16_t)(kStripY + 1), 6, 6}, kOn);
    }
    if (idx == w.play_step) c.rect(Rect{(int16_t)x, (int16_t)kStripY, 8, 8}, kXor);
    if (w.held_mask >> k & 1) c.hline(x, x + 7, kStripY + 8);
    if (k % 4 == 0) c.pixel(x, kStripY + 8);
  }
}

// fix1028: NOTE EDIT's lanes - 3 or 4 rows each (a 2-row bar and a gap: the highlight's frame stays readable), and over
// as many pitches as fit, the window: the lowest pitch it showed (kept across frames: the same notes give the same
// window), and what the last frame showed (ne_roll_shown)
constexpr int kNeMinRh = 3;
int ne_floor = -1;
NeRoll ne_shown = {0, 0, -1, -1, -1, false, false};

// ne: NOTE EDIT's marks - the highlighted note's bar framed, the selected notes' bars with a dark notch row (a hatch)
CORE_COLD void roll(Canvas& c, const seq::View& w, int kRollY0 = 27, int kRollY1 = 53, const seq::NoteEdit* ne = nullptr) {
  const seq::Track* t = w.t;
  int first = w.page * seq::kStepsPerPage;
  // the pitches of the notes that touch this page (steps from one page before: long notes, early starts), each its
  // own lane when they fit (4 rows at most: readable bars), else one row a semitone over the range
  uint32_t used[4] = {0, 0, 0, 0};
  int lo = 127, hi = 0;
  for (int i = first - 16; t && i < first + 17; ++i) {
    if (i < 0 || i >= w.length) continue;
    const seq::Step& s = t->step[i];
    for (int k = 0; k < s.n; ++k) {
      int x0 = (i - first) * 8 + s.note[k].micro * 8 / seq::kSub, x1 = x0 + s.note[k].len * 8 / seq::kSub;
      if (x1 < 0 || x0 > 127) continue;
      int p = s.note[k].pitch;
      used[p >> 5] |= 1u << (p & 31);
      if (p < lo) lo = p;
      if (p > hi) hi = p;
    }
  }
  const int rows = kRollY1 - kRollY0 + 1;   // 27 (NOTE EDIT: 31)
  if (ne) ne_shown = {0, 0, -1, -1, -1, false, false};
  for (int k = 4; k < 16; k += 4)            // beats
    for (int y = kRollY0; y <= kRollY1; y += 3) c.pixel(8 * k, y);
  if (lo > hi || !w.seq) {   // fix1028: an off track's kept steps are not drawn (no poly steps over a plain track)
    // fix1051b: a poly track with no track in this pattern is not an empty page - it cannot be edited here
    c.put_text(kSlotFree + 10, kRSmall, 64, (kRollY0 + kRollY1) / 2 - 3,
               !w.seq ? "NOT SEQUENCED" : ne && ne->idx < 0 ? "NO NOTES: TAP A STEP" : w.t ? "EMPTY PAGE" : "NO TRACK IN THIS PATTERN",
               kAlignCentre);
    return;
  }
  int lane_of[128];
  int nd = 0;
  for (int p = lo; p <= hi; ++p) lane_of[p] = used[p >> 5] >> (p & 31) & 1 ? nd++ : -1;
  int rh, nl;
  if (ne) {   // fix1028: a lane a pitch always; more than fit: the window over them that keeps the highlighted note in
    rh = rows / nd > 4 ? 4 : rows / nd < kNeMinRh ? kNeMinRh : rows / nd, nl = nd < rows / rh ? nd : rows / rh;
    int w0 = nd;   // the window's first lane: the lowest at or over the pitch it started at in the frame before
    for (int p = ne_floor < lo ? lo : ne_floor; p <= hi && w0 == nd; ++p)
      if (lane_of[p] >= 0) w0 = lane_of[p];
    int f = ne->idx >= 0 && ne->pitch >= lo && ne->pitch <= hi ? lane_of[ne->pitch] : -1;
    w0 = f >= 0 ? ne_window(w0, nl, nd, f) : ne_window(w0, nl, nd, nd - 1 < w0 ? nd - 1 : w0, 0);
    ne_shown.lanes = nd, ne_shown.visible = nl, ne_shown.more_below = w0 > 0, ne_shown.more_above = w0 + nl < nd;
    for (int p = lo; p <= hi; ++p) {
      if (lane_of[p] < 0) continue;
      lane_of[p] -= w0;
      if (lane_of[p] < 0 || lane_of[p] >= nl) lane_of[p] = -1;
      else if (lane_of[p] == 0) ne_floor = ne_shown.lo = p;
      if (lane_of[p] == nl - 1) ne_shown.hi = p;
    }
  } else if (nd * 2 <= rows) {   // a lane a pitch: 2..4 rows each, a gap row under each bar when 3+
    rh = rows / nd > 4 ? 4 : rows / nd, nl = nd;
  } else {
    rh = 1, nl = hi - lo + 1 > rows ? rows : hi - lo + 1;
    for (int p = lo; p <= hi; ++p) lane_of[p] = p - lo < nl ? p - lo : -1;
  }
  int base = kRollY1 - (rows - nl * rh) / 2;   // centred, the lowest lane at the bottom
  int bh = rh >= 3 ? rh - 1 : rh;
  for (int i = first - 16; i < first + 17; ++i) {
    if (i < 0 || i >= w.length) continue;
    const seq::Step& s = t->step[i];
    for (int k = 0; k < s.n; ++k) {
      const seq::Note& n = s.note[k];
      int ln = lane_of[n.pitch];
      if (ln < 0) continue;
      int x0 = (i - first) * 8 + (n.micro * 8 + (n.micro < 0 ? -seq::kSub / 2 : seq::kSub / 2)) / seq::kSub;
      int w8 = n.len * 8 / seq::kSub;
      if (w8 < 2) w8 = 2;
      int x1 = x0 + w8 - 1;
      if (x1 < 0 || x0 > 127) continue;
      int y = base - (ln + 1) * rh + 1;
      int a = x0 < 0 ? 0 : x0, b = x1 > 127 ? 127 : x1;
      c.fill(Rect{(int16_t)a, (int16_t)y, (int16_t)(b - a + 1), (int16_t)bh}, kOn);
      if (x0 >= 0 && bh >= 2) c.pixel(x0 + 1 <= b ? x0 + 1 : x0, y + bh - 1, kOff);   // a notch at its start: where it begins
      if (ne && seq::note_selected(i, n.pitch))   // U1: selected - every other pixel of its middle row dark
        for (int x = a + 1; x < b; x += 2) c.pixel(x, y + (bh - 1) / 2, kOff);
      if (ne && i == ne->idx && n.pitch == ne->pitch) {   // U1: the highlighted note - its bar framed, a mark at its start
        ne_shown.focus_y = y;
        c.rect(Rect{(int16_t)(a - 2), (int16_t)(y - 2), (int16_t)(b - a + 5), (int16_t)(bh + 4)}, kOn);
        c.rect(Rect{(int16_t)(a - 1), (int16_t)(y - 1), (int16_t)(b - a + 3), (int16_t)(bh + 2)}, kOff);
      }
    }
  }
  if (w.play_pos >= 0 && (int)w.play_pos / 16 == w.page) {
    int px = (int)((w.play_pos - first) * 8);
    if (px >= 0 && px < 128) c.vline(px, kRollY0 - 1, kRollY1, kXor);
  }
  if (ne && ne_shown.more_above)   // fix1028: pitches over the window: a notch pointing up at the right of its top row
    c.pixel(126, kRollY0, kXor), c.hline(125, 127, kRollY0 + 1, kXor);
  if (ne && ne_shown.more_below)   // and under it: pointing down at the bottom
    c.hline(125, 127, kRollY1 - 1, kXor), c.pixel(126, kRollY1, kXor);
}

// chrorange: each track's keyboard window (its C) and what it was for (the track's machine, the pattern and the pattern
// loads: a new pattern, project or machine starts it again at C3)
struct KbWin {
  int c0;
  uint32_t ctx;
};
KbWin kb_win[12] = {};

CORE_COLD void keyboard(Canvas& c, const seq::Step& s, const seq::View& w) {
  // 3 octaves from a C, the window kept while the step's notes fit (keys_window: a lone note shows in its own octave,
  // holding and playing do not move it): white keys lit (5 px and a gap), black keys dark over their tops; a note of
  // the step: a dark block at the foot of its white key, a lit one in its black key. Each octave's C named over it.
  int lo = 127, hi = -1;
  for (int k = 0; k < s.n; ++k) {
    if (s.note[k].pitch < lo) lo = s.note[k].pitch;
    if (s.note[k].pitch > hi) hi = s.note[k].pitch;
  }
  KbWin& kw = kb_win[w.track % 12];
  uint32_t ctx = 1u | (uint32_t)(remote_track_machine(w.track) & 0xFF) << 1 | (uint32_t)(w.pattern & 0x7F) << 9 |
                 seq::stats().loads << 16;
  if (kw.ctx != ctx) kw.ctx = ctx, kw.c0 = -1;
  int c0 = kw.c0 = keys_window(kw.c0, lo, hi);
  static const int8_t wk[7] = {0, 2, 4, 5, 7, 9, 11};
  static const int8_t bk[5] = {1, 3, 6, 8, 10}, bafter[5] = {0, 1, 3, 4, 5};   // the white key a black one follows
  auto held = [&](int note) {
    for (int k = 0; k < s.n; ++k)
      if (s.note[k].pitch == note) return true;
    return false;
  };
  char nb[6];
  for (int o = 0; o < 3; ++o) {
    c.put_text(kSlotOct + o, kRSmall, 1 + o * 42, kOctY, note_name(c0 + 12 * o, nb, sizeof nb));
    for (int i = 0; i < 7; ++i) {
      int x = 1 + (o * 7 + i) * 6;
      c.fill(Rect{(int16_t)x, kKeysY, 5, kKeysH}, kOn);
      if (held(c0 + 12 * o + wk[i])) c.fill(Rect{(int16_t)(x + 1), kKeysY + kKeysH - 3, 3, 2}, kOff);
    }
  }
  for (int o = 0; o < 3; ++o)
    for (int i = 0; i < 5; ++i) {
      int x = 1 + (o * 7 + bafter[i]) * 6 + 3;   // over the gap after its white key: 5 px from x
      c.fill(Rect{(int16_t)x, kKeysY, 5, 4}, kOff);
      if (held(c0 + 12 * o + bk[i])) c.fill(Rect{(int16_t)(x + 1), kKeysY + 1, 3, 2}, kOn);
    }
}

CORE_COLD void lane(Canvas& c, const seq::Step& s, int sel) {
  // one step early .. one late over the width: 64 px a step, the step's start at x 64
  for (int y = kLaneY; y < kLaneY + 7; y += 2) c.pixel(64, y);
  if (!s.n) return;
  int order[seq::kNotes], n = s.n;
  for (int i = 0; i < n; ++i) order[i] = i;
  for (int i = 1; i < n; ++i)   // by pitch, the highest on top
    for (int j = i; j > 0 && s.note[order[j]].pitch > s.note[order[j - 1]].pitch; --j) {
      int x = order[j];
      order[j] = order[j - 1], order[j - 1] = x;
    }
  int rh = n <= 3 ? 2 : 1;
  for (int r = 0; r < n; ++r) {
    const seq::Note& nt = s.note[order[r]];
    int x0 = 64 + nt.micro * 64 / seq::kSub, x1 = x0 + nt.len * 64 / seq::kSub;
    if (x1 > 127) x1 = 127;
    int y = kLaneY + r * rh;
    if (x1 > x0) c.hline(x0, x1, y);
    if (rh == 2) c.hline(x0, x0 + 2 > 127 ? 127 : x0 + 2, y + 1);
    if (sel == order[r] + 1) c.hline(0, 2, y);
  }
}

CORE_COLD void held_values(Canvas& c, const seq::View& w, const seq::Step& s) {
  char v[8][10];
  int lo = 0, hi = s.n;
  if (w.sel > 0 && w.sel <= s.n) lo = w.sel - 1, hi = w.sel;
  const seq::Note* n = s.n ? &s.note[lo] : nullptr;
  char nb[6];
  if (!n) {
    for (auto& x : v) strcpy(x, "-");
  } else {
    if (s.xf & seq::kXfRnd) sniprintf(v[0], 10, "N RND");   // rui1050: a random step (R1)
    else sniprintf(v[0], 10, CORE_STR("N %s"), note_name(n->pitch, nb, sizeof nb));
    sniprintf(v[1], 10, CORE_STR("V%d"), n->vel);
    char lb[8];
    steps_text(n->len, lb, sizeof lb);
    sniprintf(v[2], 10, CORE_STR("L%s"), lb);
    sniprintf(v[3], 10, CORE_STR("M%+d"), n->micro);
    sniprintf(v[4], 10, "%s", s.chd ? poly::kChordNames[s.chd % poly::kChords] : "NOCHD");
    sniprintf(v[5], 10, CORE_STR("INV%d"), s.inv);
    sniprintf(v[6], 10, CORE_STR("S%+d"), s.strum);
    if (w.sel > 0) sniprintf(v[7], 10, CORE_STR("#%d/%d"), w.sel, s.n);
    else sniprintf(v[7], 10, CORE_STR("ALL %d"), s.n);
    (void)hi;
  }
  for (int e = 0; e < 8; ++e) {
    int x = (e & 3) * 32 + 16, y = e < 4 ? kValY0 : kValY1;
    if (e == w.touched) c.fill(Rect{(int16_t)(x - 15), (int16_t)(y - 1), 30, 7}, kOn);
    c.put_text(kSlotCells + e, kRSmall, x, y, v[e], kAlignCentre | (e == w.touched ? kTextInvert : 0));
  }
}

// ---- seq-ux (1036): the prompts (YES / NO) and the lock pages (trigs held + SRC / SMPL / FX ...)
CORE_COLD const char* machine_long(int t) {
  int vb = remote_track_machine(t);
  if (vb < 0) return "?";
  if (vb == 0x7F) return "ANALOG";
  return user_slot_of(vb & 0x78) >= 0 ? "A USER MACHINE" : core::machine_long(vb & 0x78);   // (core/machines.h)
}
CORE_COLD void prompt_draw(Canvas& c, const seq::View& w) {
  char b[kTextLen + 1];
  const char* tn = kTrackNames[w.prompt_track % 12];
  int y = 14;   // lines 7 rows apart: five lines and a gap end at row 48, clear of the bottom bar (UX review R1)
  auto line = [&](int slot, const char* t) {
    c.put_text(kSlotFree + slot, kRSmall, 64, y, t, kAlignCentre);
    y += 7;
  };
  if (w.prompt == seq::kPromptMake) {
    sniprintf(b, sizeof b, CORE_STR("MAKE %s A POLY TRACK?"), tn);   // (UX review S1: one name, POLY TRACK)
    line(0, b);
    line(1, "SIDECAR SEQUENCES IT");
    y += 3;
    {   // E0 (rui1048): any Sidecar machine; the notes at once its poly config gives it (AUTO: the machine's default)
      inst::Resolved rs = {};
      int m = -1;
      if (inst::resolve_for(remote_track_voice(w.prompt_track), seq::poly_of(w.prompt_track), &rs, &m))
        sniprintf(b, sizeof b, CORE_STR("%s, %d NOTE%s"), machine_long(w.prompt_track), rs.lanes, rs.lanes == 1 ? "" : "S");
      else
        sniprintf(b, sizeof b, "%s", machine_long(w.prompt_track));
    }
    line(2, b);
    line(3, "PADS: CHRO FOR NOTES");
    sniprintf(b, sizeof b, CORE_STR("%s: POLY CONFIG"), sc_combo_keys("poly"));   // the device's table (ui/page_keys.h)
    line(4, b);
    footer(c, "YES: MAKE IT   NO: BACK");
  } else if (w.prompt == seq::kPromptMenu) {   // UX review S2: the poly track's menu (rui1050: six rows, the track in
    // the header's right; PLAY MODE shows the direction, PRINT RND prints the loop playing)
    char dir[24];
    sniprintf(dir, sizeof dir, CORE_STR("PLAY MODE: %s"), seq::dir_name(w.t ? w.t->dir : 0));
    const char* rows[seq::kMenuRows] = {"BACK TO THE SEQ", "TURN OFF (STEPS KEPT)", "LENGTH (KNOB C)", "QUANTISE (KNOB D)", dir,
                                        "PRINT RND (THIS LOOP)"};
    y = 11;
    for (int i = 0; i < seq::kMenuRows; ++i) {
      if (i == w.prompt_sel) c.fill(Rect{12, (int16_t)(y - 1), 104, 7}, kOn);
      c.put_text(kSlotFree + 1 + i, kRSmall, 64, y, rows[i], (uint8_t)(kAlignCentre | (i == w.prompt_sel ? kTextInvert : 0)));
      y += 7;
    }
    if (w.msg) footer(c, w.msg);
    else if (w.prompt_sel == seq::kMenuDir) footer(c, "YES / LEFT RIGHT: CHANGE");
    else footer(c, "YES: OK  NO: BACK");
  } else if (w.prompt == seq::kPromptOff) {
    sniprintf(b, sizeof b, CORE_STR("%s: POLY TRACK OFF?"), tn);
    line(0, b);
    y += 3;
    line(1, "ITS STEPS STAY SAVED");
    line(2, "B UP: BACK ON");
    footer(c, "YES: TURN OFF   NO: KEEP");
  } else if (w.prompt == seq::kPromptRestore) {   // fix1028 ("Ask on make"): the notes an earlier TURN OFF kept
    sniprintf(b, sizeof b, CORE_STR("%s IS A POLY TRACK"), tn);
    line(0, b);
    y += 3;
    if (w.prompt_n) sniprintf(b, sizeof b, CORE_STR("RESTORE %d NOTE%s?"), w.prompt_n, w.prompt_n == 1 ? "" : "S");
    else sniprintf(b, sizeof b, "RESTORE ITS LOCKS?");
    line(1, b);
    if (w.prompt_locks) {   // (review: steps with locks only come back too)
      sniprintf(b, sizeof b, CORE_STR("+ %d STEP%s OF LOCKS"), w.prompt_locks, w.prompt_locks == 1 ? "" : "S");
      line(2, b);
    } else {
      line(2, "KEPT SINCE ITS TURN OFF");
    }
    line(3, w.prompt_undo ? "NO: CLEARED (UNDO-ABLE)" : "NO: CLEARED FOR GOOD");
    footer(c, "YES: RESTORE  NO: EMPTY");
  } else {
    // kPromptExplain: E0 makes every Sidecar machine a poly track, so only an analog track gets here (can_seq 0; the
    // mono-machine reasons of before E0, 2 JUNO and 3 the other mono machines, are no longer answered). E0b FREEZE (not
    // built): this is where "YES: FREEZE TO POLY" goes - the auto-sampler on this track into a MULTISAMPLE poly track
    // (docs/SEQ-IDEAS.md E0b).
    sniprintf(b, sizeof b, CORE_STR("%s PLAYS %s:"), tn, machine_long(w.prompt_track));
    line(0, b);
    line(1, "THE RYTM'S VOICE IS MONO");
    y += 3;
    line(2, "FOR A POLY TRACK PICK");
    line(3, "A SIDECAR MACHINE ON SRC");
    footer(c, "NO: BACK");
  }
}
UI_ROM const char* const kLockNames[8] = {"TRIG", "SRC", "SMPL", "FLTR", "AMP", "LFO", "FX", "INS"};
// The SMPL page's labels by machine (rytm-mods 0026 cu tables; 0021's for 6-OP FM)
CORE_COLD const char* p2_label(int vb, int e) {
  static const char* const fm[8] = {"TUN", "FIN", "BR", "SMP", "CHD", "INV", "SPR", "VOI"};
  static const char* const ms[8] = {"TUNE", "FINE", "STRT", "LOOP", "CHD", "INV", "SPR", "VOI"};
  static const char* const wt[8] = {"ATK", "DEC", "SUS", "ENV", "CHD", "INV", "SPR", "VOI"};
  static const char* const gr[8] = {"ATK", "REL", "HOLD", "ANIM", "CHD", "INV", "SPR", "VOI"};
  static const char* const px[8] = {"P9", "P10", "P11", "P12", "P13", "P14", "P15", "P16"};
  static const char* const ds[8] = {"XTN", "XDC", "SEED", "OUT", "XP4", "XLV", "XP5", "WID"};   // rui1050: DAISYSP P11 SEED
  static const char* const cp[8] = {"STUT", "SLEN", "TAPE", "TTIM", "ATK", "DEC", "DIR", "LOOP"};   // CHOP (core/chop/chop.h)
  int m = vb >= 0 && vb != 0x7F ? vb & 0x78 : -1;
  return (m == kFm ? fm : m == kMultiMachine ? ms : m == kWaveMachine ? wt : m == kGranMachine ? gr : m == kDaisy ? ds
          : m == kChopMachine ? cp : px)[e & 7];
}
CORE_COLD void lock_label(int pg, int t, int e, char* b, int cap) {
  b[0] = 0;
  if (pg == seq::kLpSrc) {
    const Page* p = remote_track_page(t);
    if (p && p->label[e][0]) sniprintf(b, cap, "%.4s", p->label[e]);
    else sniprintf(b, cap, CORE_STR("P%d"), e + 1);
  } else if (pg == seq::kLpSmpl) {
    sniprintf(b, cap, "%s", p2_label(remote_track_machine(t), e));
  } else if (pg == seq::kLpFx && e / 4 < fx::kSlots) {
    const fx::KnobLabels* l = fx::labels(fx::control.knob[e / 4][0]);
    if (l && l->label[e % 4] && l->label[e % 4][0]) sniprintf(b, cap, "%.4s", l->label[e % 4]);
    else sniprintf(b, cap, CORE_STR("P%d"), 2 + e % 4);
  } else if (pg == seq::kLpIns) {   // fx-ins: the track's voice's INSERT A (A..D) and B (E..H)
    int vi = remote_track_voice(t);
    int ty = vi >= 0 && vi < ins::kVoices ? ins::control.type[vi][e / 4] : 0;
    const char* l = ins::labels(ty).label[e % 4];
    if (ty && l[0]) sniprintf(b, cap, "%.4s", l);
    else sniprintf(b, cap, CORE_STR("%c P%d"), 'A' + e / 4, 1 + e % 4);
  }
}
CORE_COLD void lock_draw(Canvas& c, const seq::View& w) {
  char b[kTextLen + 1], l[8];
  int pg = w.lock_page, held = 0, first = -1;
  for (int k = 0; k < 16; ++k)
    if (w.held_mask >> k & 1) ++held, first = first < 0 ? k : first;
  bool any = false;
  for (int e = 0; e < 8; ++e) {
    int prm = seq::lock_param(pg, e);
    Rect r = cell(e & 3, e >> 2);
    if (prm < 0) continue;
    any = true;
    int lv = seq::held_lock(prm);
    int v = (lv >= 0 ? lv : seq::lock_base(prm)) >> 7;
    knob(c, r, v);
    lock_label(pg, w.track, e, l, sizeof l);
    int lx = r.x + kCellW / 2, ly = r.y + kLabelDy;
    if (lv >= 0) {   // locked: the label inverted and the cell framed, as the Rytm marks a p-locked knob
      c.fill(Rect{(int16_t)(r.x + 2), (int16_t)(ly - 1), (int16_t)(kCellW - 4), 7}, kOn);
      c.rect(Rect{r.x, r.y, (int16_t)kCellW, (int16_t)kCellH}, kOn);
    }
    bool sl = false;
    int lc = lv >= 0 ? seq::held_lock_cond(prm, &sl) : -1;
    if (e == w.cond_e) {   // rui1050: this knob edits its lock's COND (a long push): the label gets a ?
      size_t n = strlen(l);
      if (n < sizeof l - 1) l[n] = '?', l[n + 1] = 0;
    }
    if (sl) c.line(r.x + kCellW - 6, r.y + 6, r.x + kCellW - 3, r.y + 3);   // rui1050: a slide - a rising tick top right
    if (lc > 0) c.fill(Rect{(int16_t)(r.x + 2), (int16_t)(r.y + 2), 3, 3}, kOn);   // a condition: a dot top left
    c.put_text(kSlotLabels + e, kRSmall, lx, ly, l, kAlignCentre | (lv >= 0 ? kTextInvert : 0));
  }
  if (!any) {
    char lb[32];
    sniprintf(lb, sizeof lb, "THE SIDECAR'S LFOS: %s", sc_combo_keys("lfo"));   // the device's table (ui/page_keys.h)
    c.put_text(kSlotFree + 2, kRSmall, 64, 22, pg == seq::kLpLfo ? lb : "THE RYTM'S ANALOG SIDE:", kAlignCentre);
    c.put_text(kSlotFree + 3, kRSmall, 64, 30, pg == seq::kLpLfo ? "(NOT LOCKED PER STEP)" : "NOT LOCKED BY THE SIDECAR",
               kAlignCentre);
    c.put_text(kSlotFree + 4, kRSmall, 64, 40, "SRC SMPL FX: STEP LOCKS", kAlignCentre);
  }
  int e = w.touched >= 0 && w.touched < 8 ? w.touched : -1;
  int prm = e >= 0 ? seq::lock_param(pg, e) : -1;
  if (prm >= 0) {
    int lv = seq::held_lock(prm);
    lock_label(pg, w.track, e, l, sizeof l);
    bool sl = false;
    int lc = lv >= 0 ? seq::held_lock_cond(prm, &sl) : -1;
    char cb[8] = "";
    if (lc > 0) seq::lock_cond_text(lc, cb, sizeof cb);
    if (w.msg) sniprintf(b, sizeof b, "%s", w.msg);   // rui1050: SLIDE ON / NO LOCK
    else if (e == w.cond_e) sniprintf(b, sizeof b, CORE_STR("S%d %s COND:%s"), w.page * 16 + first + 1, l, lc > 0 ? cb : "---");
    else sniprintf(b, sizeof b, CORE_STR("S%d %s:%d%s%s%s"), w.page * 16 + first + 1, l, (lv >= 0 ? lv : seq::lock_base(prm)) >> 7,
                   lv >= 0 && !lc && !sl ? " LOCK" : "", lc > 0 ? " " : "", cb);
    if (sl && !w.msg && e != w.cond_e) {
      size_t n = strlen(b);
      sniprintf(b + n, sizeof b - n, " SLD");
    }
    // (the level box only beside the plain form: a condition / slide / COND needs the bar's width)
    footer(c, b, w.msg || lc > 0 || sl || e == w.cond_e ? -1 : (lv >= 0 ? lv : seq::lock_base(prm)) >> 7);
  } else if (w.msg) {
    footer(c, w.msg);
  } else if (held > 1) {
    sniprintf(b, sizeof b, CORE_STR("%d STEPS: TURN=LOCK NO=CLEAR"), held);
    footer(c, b);
  } else {
    sniprintf(b, sizeof b, CORE_STR("S%d TURN=LOCK NO=CLEAR"), w.page * 16 + first + 1);
    footer(c, b);
  }
}

// rui1040: the TRIG view (TRIG beside the notes, as the stock TRIG page beside the grid): knobs with the stock look, the
// value of the one turned last in the bottom bar. rui1050: TRIG cycles its pages (seq.h kTv* / kSv*). Held steps: the
// first one's TRIG (NOTE VEL LEN MICR / COND STRM CHD INV; NOTE's RND below the lowest note: a random step), RTRG (RATE
// CNT VCRV PCRV / MODE OCT GATE) and FX (CMD1 VAL1 CMD2 VAL2); turning edits every held step. None: the track's DEFAULTS
// (NOTE VEL LEN SWNG / DIR SPD TLEN), the pattern's SCALE (KEY SCL QNT) and RANDOM (SEED RNG WGHT MODE / SRNG SQNT SAFF).
struct KPage {
  const char* title;
  const char* lab[8];
  char v[8][12];
  int kv[8];
  uint8_t n, bip;   // knobs shown, bipolar ones (bit e)
};
CORE_COLD int k127(int v, int lo, int hi) { return hi > lo ? (v - lo) * 127 / (hi - lo) : 0; }
CORE_COLD void kpage_fill(KPage& k, const seq::View& w, const seq::Step* hs) {
  memset(&k, 0, sizeof k);
  char nb[6], lb[8];
  if (hs && w.tpage == seq::kSvRtrg) {
    static const char* const L[8] = {"RATE", "CNT", "VCRV", "PCRV", "MODE", "OCT", "GATE", "-"};
    const seq::Rtrg& x = hs->rt;
    k.title = "SEQ RTRG STEP", memcpy(k.lab, L, sizeof L), k.n = 7, k.bip = 1 << 3;
    sniprintf(k.v[0], 12, "%s", seq::rt_rate_name(x.rate)), k.kv[0] = k127(x.rate, 0, seq::kRtRates - 1);
    if (x.cnt >= 2) sniprintf(k.v[1], 12, CORE_STR("%d"), x.cnt);
    else sniprintf(k.v[1], 12, "OFF");
    k.kv[1] = k127(x.cnt < 2 ? 0 : x.cnt, 0, seq::kRtMax);
    sniprintf(k.v[2], 12, "%s", seq::vcrv_name(x.vcrv)), k.kv[2] = k127(x.vcrv, 0, seq::kVcs - 1);
    sniprintf(k.v[3], 12, CORE_STR("%+d"), x.pcrv), k.kv[3] = 64 + x.pcrv * 63 / 24;
    sniprintf(k.v[4], 12, "%s", seq::rt_mode_name(x.mode)), k.kv[4] = k127(x.mode, 0, seq::kRtModes - 1);
    sniprintf(k.v[5], 12, CORE_STR("%d"), x.oct ? x.oct : 1), k.kv[5] = k127(x.oct ? x.oct : 1, 1, 4);
    sniprintf(k.v[6], 12, CORE_STR("%d%%"), x.gate ? x.gate : 100), k.kv[6] = k127(x.gate ? x.gate : 100, 1, 100);
    return;
  }
  if (hs && w.tpage == seq::kSvFx) {
    static const char* const L[8] = {"CMD1", "VAL1", "CMD2", "VAL2", "-", "-", "-", "-"};
    k.title = "SEQ FX STEP", memcpy(k.lab, L, sizeof L), k.n = 4;
    for (int c = 0; c < 2; ++c) {
      const seq::Fx& f = hs->fx[c];
      char ft[16];
      seq::fx_text(f, ft, sizeof ft);
      const char* sp = strchr(ft, ' ');
      sniprintf(k.v[2 * c], 12, "%s", seq::fx_name(f.cmd)), k.kv[2 * c] = k127(f.cmd, 0, seq::kFxCmds - 1);
      sniprintf(k.v[2 * c + 1], 12, "%s", sp ? sp + 1 : "-");
      int lo = 0, hi = 0;
      seq::fx_range(f.cmd, &lo, &hi);
      int vv = f.cmd == seq::kFxOct || f.cmd == seq::kFxTrs ? (int8_t)f.val : f.val;
      k.kv[2 * c + 1] = f.cmd ? k127(vv, lo, hi) : 0;
    }
    return;
  }
  if (hs) {
    static const char* const L[8] = {"NOTE", "VEL", "LEN", "MICR", "COND", "STRM", "CHD", "INV"};
    k.title = "SEQ TRIG STEP", memcpy(k.lab, L, sizeof L), k.n = 8, k.bip = 1 << 3 | 1 << 5;
    const seq::Note* nt = hs->n ? &hs->note[0] : nullptr;
    if (nt) {
      if (hs->xf & seq::kXfRnd) sniprintf(k.v[0], 12, "RND"), k.kv[0] = 0;
      else sniprintf(k.v[0], 12, "%s", note_name(nt->pitch, nb, sizeof nb)), k.kv[0] = nt->pitch;
      sniprintf(k.v[1], 12, CORE_STR("%d"), nt->vel), k.kv[1] = nt->vel;
      steps_text(nt->len, lb, sizeof lb), sniprintf(k.v[2], 12, "%s", lb), k.kv[2] = nt->len * 127 / (16 * seq::kSub);
      sniprintf(k.v[3], 12, CORE_STR("%+d"), nt->micro), k.kv[3] = 64 + nt->micro * 63 / seq::kSub;
    } else {
      for (int i = 0; i < 4; ++i) strcpy(k.v[i], "-");
    }
    seq::cond_text(*hs, k.v[4], 12), k.kv[4] = seq::cond_knob(*hs) * 127 / (seq::kCondKnob - 1);   // the condition / %
    sniprintf(k.v[5], 12, CORE_STR("%+d"), hs->strum), k.kv[5] = 64 + hs->strum * 63 / (seq::kSub / 2);
    sniprintf(k.v[6], 12, "%s", hs->chd ? poly::kChordNames[hs->chd % poly::kChords] : "OFF"), k.kv[6] = hs->chd * 127 / (poly::kChords - 1);
    sniprintf(k.v[7], 12, CORE_STR("%d"), hs->inv), k.kv[7] = hs->inv * 127 / (poly::kInvs - 1);
    return;
  }
  const seq::Track* t = w.t;
  if (w.tpage == seq::kTvScale) {
    static const char* const L[8] = {"KEY", "SCL", "QNT", "-", "-", "-", "-", "-"};
    k.title = "SEQ SCALE", memcpy(k.lab, L, sizeof L), k.n = 3;
    int key = 0, sc = 0, q = 0;
    seq::pattern_scale(&key, &sc, &q);
    sniprintf(k.v[0], 12, "%s", seq::key_name(key)), k.kv[0] = k127(key, 0, 11);
    sniprintf(k.v[1], 12, "%s", seq::scale_name(sc)), k.kv[1] = k127(sc, 0, seq::kScales - 1);
    sniprintf(k.v[2], 12, "%s", q == seq::kQntPlay ? "PLAY" : q == seq::kQntEdit ? "EDIT" : "OFF"), k.kv[2] = k127(q, 0, 2);
    return;
  }
  if (w.tpage == seq::kTvRandom) {
    static const char* const L[8] = {"SEED", "RNG", "WGHT", "MODE", "SRNG", "SQNT", "SAFF", "-"};
    static const char* const kQ[3] = {"STEP", "LOOP", "BAR"};
    static const char* const kA[4] = {"NOTES", "+CHNC", "+ORDR", "ALL"};
    k.title = "SEQ RANDOM", memcpy(k.lab, L, sizeof L), k.n = 7;
    int seed = 0;
    bool fixed = seq::pattern_seed(&seed);
    if (fixed) sniprintf(k.v[0], 12, CORE_STR("%d"), seed), k.kv[0] = 1 + seed * 126 / 9999;
    else sniprintf(k.v[0], 12, "FREE");
    if (!t) return;
    sniprintf(k.v[1], 12, CORE_STR("%d OCT"), t->rnd_oct ? t->rnd_oct : seq::kRndOctDef), k.kv[1] = k127(t->rnd_oct ? t->rnd_oct : seq::kRndOctDef, 1, 4);
    sniprintf(k.v[2], 12, "%s", seq::weight_name(t->rnd_wght)), k.kv[2] = k127(t->rnd_wght, 0, seq::kWeights - 1);
    sniprintf(k.v[3], 12, "%s", seq::rnd_mode_name(t->rnd_mode)), k.kv[3] = k127(t->rnd_mode, 0, seq::kRndModes - 1);
    if (t->smod_src) sniprintf(k.v[4], 12, CORE_STR("%d P%d"), t->smod_range, t->smod_src), k.kv[4] = k127(t->smod_range, 1, 64);
    else sniprintf(k.v[4], 12, "OFF");
    sniprintf(k.v[5], 12, "%s", kQ[t->smod_quant % 3]), k.kv[5] = k127(t->smod_quant, 0, 2);
    sniprintf(k.v[6], 12, "%s", kA[t->smod_aff % 4]), k.kv[6] = k127(t->smod_aff, 0, 3);
    return;
  }
  static const char* const L[8] = {"NOTE", "VEL", "LEN", "SWNG", "DIR", "SPD", "TLEN", "-"};
  k.title = "SEQ TRIG", memcpy(k.lab, L, sizeof L), k.n = 7, k.bip = 1 << 3;
  if (!t) return;
  sniprintf(k.v[0], 12, "%s", note_name(t->dnote, nb, sizeof nb)), k.kv[0] = t->dnote;
  sniprintf(k.v[1], 12, CORE_STR("%d"), t->dvel), k.kv[1] = t->dvel;
  steps_text(t->dlen, lb, sizeof lb), sniprintf(k.v[2], 12, "%s", lb), k.kv[2] = t->dlen * 127 / (16 * seq::kSub);
  sniprintf(k.v[3], 12, CORE_STR("%d%%"), w.swing), k.kv[3] = (w.swing - 50) * 127 / 30;
  sniprintf(k.v[4], 12, "%s", seq::dir_name(t->dir)), k.kv[4] = k127(t->dir, 0, seq::kDirs - 1);
  sniprintf(k.v[5], 12, "%s", seq::speed_name(t->speed)), k.kv[5] = k127(t->speed, 0, seq::kSpeeds - 1);
  if (t->length) sniprintf(k.v[6], 12, CORE_STR("%d"), w.length);
  else sniprintf(k.v[6], 12, CORE_STR("AUTO %d"), w.length);
  k.kv[6] = k127(t->length, 0, seq::kMaxSteps);
}
CORE_COLD void trig_draw(Canvas& c, const seq::View& w, const seq::Step* hs, int page, int npages, const char* right) {
  KPage k;
  kpage_fill(k, w, hs);
  header(c, k.title, page, npages, right);
  for (int e = 0; e < k.n; ++e) {
    Rect r = cell(e & 3, e >> 2);
    int v = k.kv[e];
    knob(c, r, v < 0 ? 0 : v > 127 ? 127 : v, (k.bip >> e & 1) != 0);
    int lx = r.x + kCellW / 2, ly = r.y + kLabelDy;
    bool on = e == w.touched;
    if (on) c.fill(Rect{(int16_t)(r.x + 2), (int16_t)(ly - 1), (int16_t)(kCellW - 4), 7}, kOn);
    c.put_text(kSlotLabels + e, kRSmall, lx, ly, k.lab[e], kAlignCentre | (on ? kTextInvert : 0));
  }
  char b[kTextLen + 1];
  int e = w.touched >= 0 && w.touched < k.n ? w.touched : -1;
  if (w.msg) sniprintf(b, sizeof b, "%s", w.msg);
  else if (e >= 0) sniprintf(b, sizeof b, CORE_STR("%s %s:%s"), hs ? "STEP" : kTrackNames[w.track % 12], k.lab[e], k.v[e]);
  else if (hs) sniprintf(b, sizeof b, CORE_STR("S%d %s  TRIG:NEXT"), w.page * 16 + w.held + 1, k.title + 4);
  else if (w.tpage == seq::kTvScale) sniprintf(b, sizeof b, CORE_STR("KEY %s %s QNT %s"), k.v[0], k.v[1], k.v[2]);
  else if (w.tpage == seq::kTvRandom) {
    int seed = 0, off = seq::seed_offset(w.track);
    if (!seq::pattern_seed(&seed)) sniprintf(b, sizeof b, off ? CORE_STR("SEED FREE+%d") : "SEED FREE", off);
    else if (off) sniprintf(b, sizeof b, CORE_STR("SEED %d+%d FIX"), seed, off);
    else sniprintf(b, sizeof b, CORE_STR("SEED %d FIX"), seed);
  } else if (w.t) sniprintf(b, sizeof b, CORE_STR("DIR %s  SPD %s"), k.v[4], k.v[5]);
  else sniprintf(b, sizeof b, "NEW STEPS  TRIG:NEXT");
  footer(c, b);
}

// U1 NOTE EDIT (docs/SEQ-IDEAS.md U1). fix1028 (hardware: "only room for 4 rows of notes"): no title bar - line 1
// says NOTE EDIT and the track, the note's step and place in its chord, its place in the pattern (or the selection);
// then the page's roll, rows 8..38 (was 19..31), a lane a pitch scrolled to the highlighted note (framed; the selected
// ones hatched; no strip: the trig LEDs show the steps); A..H's values in two rows, each with its letter (S5 N+0 L.75
// D4 / V100; a range lo..hi over the selection; the names in the footer when turned); the footer the knob turned
// last, else the keys.
constexpr int kNeTopY = 1, kNeRollY0 = 8, kNeRollY1 = 38, kNeRuleY = 40, kNeValY0 = 42, kNeValY1 = 49;
CORE_COLD void len_short(int units, char* b, int cap) {   // steps, the decimals only as needed: 0.75 -> .75, 1.00 -> 1
  int h = (units * 100 + seq::kSub / 2) / seq::kSub;
  if (h % 100 == 0) sniprintf(b, cap, CORE_STR("%d"), h / 100);
  else if (h % 10 == 0) sniprintf(b, cap, h >= 100 ? CORE_STR("%d.%d") : CORE_STR(".%d%.0d"), h >= 100 ? h / 100 : (h % 100) / 10, h >= 100 ? (h % 100) / 10 : 0);
  else if (h >= 100) sniprintf(b, cap, CORE_STR("%d.%02d"), h / 100, h % 100);
  else sniprintf(b, cap, CORE_STR(".%02d"), h % 100);
}
CORE_COLD void ne_value(int e, char* b, int cap) {
  int lo = 0, hi = 0;
  if (e > 4 || !seq::note_edit_range(e, &lo, &hi)) {
    sniprintf(b, cap, "-");
    return;
  }
  char x[8], y[8];
  switch (e) {
    case 0:
      if (lo == hi) sniprintf(b, cap, CORE_STR("S%d"), lo + 1);
      else sniprintf(b, cap, CORE_STR("S%d..%d"), lo + 1, hi + 1);
      break;
    case 1:
      if (lo == hi) sniprintf(b, cap, CORE_STR("N%+d"), lo);
      else sniprintf(b, cap, CORE_STR("%d..%d"), lo, hi);
      break;
    case 2:
      len_short(lo, x, sizeof x), len_short(hi, y, sizeof y);
      if (lo == hi) sniprintf(b, cap, CORE_STR("L%s"), x);
      else sniprintf(b, cap, CORE_STR("%s-%s"), x, y);   // (".75...81" read as three dots)
      break;
    case 3:
      note_name(lo, x, sizeof x), note_name(hi, y, sizeof y);
      if (lo == hi) sniprintf(b, cap, "%s", x);
      else if (strlen(x) + strlen(y) + 2 <= 7) sniprintf(b, cap, CORE_STR("%s..%s"), x, y);
      else sniprintf(b, cap, CORE_STR("%d..%d"), lo, hi);   // (C#4..D#5 is too wide: the MIDI numbers)
      break;
    default:
      if (lo == hi) sniprintf(b, cap, CORE_STR("V%d"), lo);
      else sniprintf(b, cap, CORE_STR("%d..%d"), lo, hi);
      break;
  }
}
CORE_COLD void ne_draw(Canvas& c, const seq::View& w, const seq::NoteEdit& n, const char* right) {
  char b[kTextLen + 1];
  sniprintf(b, sizeof b, CORE_STR("NOTE EDIT %s"), right);
  c.put_text(kSlotTitle, kRSmall, 1, kNeTopY, b);
  int at = 0;
  if (n.idx >= 0) at = sniprintf(b, sizeof b, CORE_STR("S%d %d/%d "), n.idx + 1, n.chord_i, n.chord_n);
  if (n.sel) sniprintf(b + at, sizeof b - at, CORE_STR("SEL %d"), n.sel);
  else if (n.idx >= 0) sniprintf(b + at, sizeof b - at, CORE_STR("%d/%d"), n.pos, n.count);
  else sniprintf(b, sizeof b, CORE_STR("PG%d/%d"), w.page + 1, w.npages);
  c.put_text(kSlotRight, kRSmall, 127, kNeTopY, b, kAlignRight);
  roll(c, w, kNeRollY0, kNeRollY1, &n);   // (no strip: the trig LEDs show the steps, the roll gets its rows)
  static const char* const kLong[9] = {"POSITION", "NUDGE", "LENGTH", "PITCH", "VELOCITY", "VEL RANGE", "PROBABILITY", "CONDITION", "NOTE"};
  c.hline(0, 127, kNeRuleY);
  for (int e = 0; e < 8; ++e) {
    char v[12];
    ne_value(e, v, sizeof v);
    int x = (e & 3) * 32 + 16, y = e < 4 ? kNeValY0 : kNeValY1;
    if (e == w.touched) c.fill(Rect{(int16_t)(x - 16), (int16_t)(y - 1), 32, 7}, kOn);
    c.put_text(kSlotCells + e, kRSmall, x, y, v, kAlignCentre | (e == w.touched ? kTextInvert : 0));
  }
  if (w.msg) {
    footer(c, w.msg);
  } else if (n.idx < 0) {
    footer(c, "NO NOTES  TAP A STEP");
  } else if (w.touched >= 5 && w.touched < 8) {
    sniprintf(b, sizeof b, CORE_STR("%s: NOT YET"), kLong[w.touched]);
    footer(c, b);
  } else if (w.touched >= 0 && w.touched < 5) {
    char v[12];
    ne_value(w.touched, v, sizeof v);
    sniprintf(b, sizeof b, CORE_STR("%s %s%s"), kLong[w.touched], v, n.sel > 1 ? " ALL" : "");
    footer(c, b);
  } else if (w.touched == 8) {
    char v[8];
    note_name(n.pitch, v, sizeof v);
    sniprintf(b, sizeof b, n.held >= 0 ? CORE_STR("CHORD %d/%d S%d %s") : CORE_STR("NOTE %d/%d S%d %s"), n.pos, n.count,
              n.idx + 1, v);
    footer(c, b);
  } else {
    footer(c, "PUSH:SEL F+NO:DEL NO:BACK");
  }
}

// dbg: the playhead on the Rytm's screen moves at most every kPlayheadMs (the trig LEDs show every step). Each changed
// frame is a commit (39), and rytm-mods 0026 redraws the Rytm's whole stock screen under the page on every commit: one
// a step (8 a second at 120 BPM) kept its UI task drawing at every 20 Hz tick (core/tests/test_traffic.cpp case 7).
constexpr uint32_t kPlayheadMs = 250;

CORE_COLD void seq_draw(Canvas& c, int page, int npages, uint32_t ms) {
  seq::View w = seq::view();
  static int shown_ps = -1;
  static double shown_pos = -1;
  static uint32_t shown_ms = 0;
  if (w.play_step != shown_ps && (shown_ps < 0 || w.play_step < 0 || ms - shown_ms >= kPlayheadMs))
    shown_ps = w.play_step, shown_pos = w.play_pos, shown_ms = ms;
  w.play_step = shown_ps;
  w.play_pos = shown_ps < 0 ? w.play_pos : shown_pos;   // the roll's playhead column with the strip's
  char right[8], b[kTextLen + 1];
  sniprintf(right, sizeof right, "%s", kTrackNames[w.track % 12]);
  c.clear();
  if (w.prompt) {   // seq-ux: the make / off prompt or why not
    sniprintf(right, sizeof right, "%s", kTrackNames[w.prompt_track % 12]);
    header(c, w.prompt == seq::kPromptMenu ? "POLY TRACK" : "SIDECAR SEQ", page, npages, right);
    prompt_draw(c, w);
    return;
  }
  if (w.lock_page >= 0 && w.held >= 0) {   // seq-ux: trigs held + a page key: that page's locks
    char t[16];
    sniprintf(t, sizeof t, CORE_STR("LOCK %s"), kLockNames[w.lock_page % 8]);
    header(c, t, page, npages, right);
    lock_draw(c, w);
    return;
  }
  {   // U1: NOTE EDIT (DATA turned on a poly track's page; a held step is its scope, not the step view)
    seq::NoteEdit ne = seq::note_edit();
    if (ne.on && !(w.held >= 0 && w.trigview)) {   // (a trig held + TRIG: its step pages, as before)
      ne_draw(c, w, ne, right);
      return;
    }
  }
  const seq::Step* hs = w.held >= 0 && w.t ? &w.t->step[w.page * seq::kStepsPerPage + w.held] : nullptr;
  static const seq::Step kEmpty = {};
  if (w.trigview) {   // rui1040: TRIG's view (rui1050: its pages)
    trig_draw(c, w, w.held >= 0 ? (hs ? hs : &kEmpty) : nullptr, page, npages, right);
    return;
  }
  header(c, "SIDECAR SEQ", page, npages, right);
  if (w.held >= 0) {
    if (!hs) hs = &kEmpty;
    strip(c, w, kHeldStripY);
    keyboard(c, *hs, w);
    lane(c, *hs, w.sel);
    held_values(c, w, *hs);
    sniprintf(b, sizeof b, CORE_STR("STEP %d"), w.page * 16 + w.held + 1);
    c.put_text(kSlotFree, kRSmall, 2, 10 + 0, nullptr);
    static const char* const kLong[8] = {"NOTE", "VELOCITY", "LENGTH", "MICRO", "CHORD", "INVERSION", "STRUM", "SELECT"};
    if (w.touched >= 0 && w.touched < 8) {
      sniprintf(b, sizeof b, CORE_STR("%s S%d %s"), kTrackNames[w.track % 12], w.page * 16 + w.held + 1, kLong[w.touched]);
      footer(c, b);
    } else {
      if (hs->nl) sniprintf(b, sizeof b, CORE_STR("S%d %d LOCKS SRC/SMPL/FX NO:CLR"), w.page * 16 + w.held + 1, hs->nl);
      else sniprintf(b, sizeof b, CORE_STR("S%d PADS:NOTES SRC:LOCKS"), w.page * 16 + w.held + 1);
      footer(c, b);
    }
    return;
  }
  // the line under the title: pattern, page, length; the transport at the right
  int pat = w.pattern < 0 ? 0 : w.pattern;
  sniprintf(b, sizeof b, CORE_STR("%c%02d PG%d/%d %d%s"), 'A' + pat / 16, pat % 16 + 1, w.page + 1, w.npages, w.length,
            w.t && !w.t->length ? "A" : "");
  c.put_text(kSlotFree, kRSmall, 1, 11, b);
  const char* tr = w.transport == seq::kPlay ? "PLAY" : w.transport == seq::kPause ? "PAUSE" : "STOP";
  if (w.rec_live && w.transport == seq::kPlay) {
    c.fill(Rect{103, 10, 25, 7}, kOn);
    c.put_text(kSlotFree + 1, kRSmall, 126, 11, "REC", kAlignRight | kTextInvert);
  } else {
    c.put_text(kSlotFree + 1, kRSmall, 127, 11, tr, kAlignRight);
  }
  if (w.seq) strip(c, w, kStripY);   // (fix1028: a track that is no poly track: none of its kept steps)
  roll(c, w);
  static const char* const kSet[6] = {"TRACK", "SEQ", "LENGTH", "QUANTISE", "FOLLOW", "LATENCY"};
  if (w.page_held) {   // UX review S4: PAGE held - the trig LEDs' colours named (the PAGE map's legend)
    footer(c, "BLU NOTE CYN CHD MAG LOCK");
  } else if (w.msg) {   // rui1050: PRINT RND's result
    footer(c, w.msg);
  } else if (w.off_hint) {   // R2: B turned down on a poly track: how to turn it off (never a prompt by itself)
    footer(c, "OFF: F+DOWN, TURN OFF");   // updown1029: FUNC + DOWN
  } else if (w.touched >= 0 && w.touched < 6) {
    char v[16];
    switch (w.touched) {
      case 0: sniprintf(v, sizeof v, "%s", kTrackNames[w.track % 12]); break;
      case 1: sniprintf(v, sizeof v, "%s", w.seq ? "ON" : "OFF"); break;
      case 2:
        if (w.t && w.t->length) sniprintf(v, sizeof v, CORE_STR("%d"), w.length);
        else sniprintf(v, sizeof v, CORE_STR("AUTO %d"), w.length);
        break;
      case 3: sniprintf(v, sizeof v, "%s", w.quant ? "1/16" : "OFF"); break;
      case 4: sniprintf(v, sizeof v, "%s", w.follow ? "ON" : "OFF"); break;
      default: sniprintf(v, sizeof v, CORE_STR("%d.%dMS"), w.latency / 48, w.latency % 48 * 10 / 48); break;
    }
    sniprintf(b, sizeof b, CORE_STR("%s %s:%s"), kTrackNames[w.track % 12], kSet[w.touched], v);
    footer(c, b);
  } else if (w.loading || w.locked == 3) {   // the RAM store: the project's files not here yet - edits refused
    footer(c, "PROJECT LOADING");
  } else if (w.locked) {   // 1042: the pattern's file did not load: its edits are not saved
    sniprintf(b, sizeof b, CORE_STR("%c%02d %s: LOCKED"), 'A' + pat / 16 % 8, pat % 16 + 1,
              w.locked == 1 ? "FROM NEWER SIDECAR" : "FILE DAMAGED");
    footer(c, b);
  } else if (!w.seq) {
    footer(c, "B UP: MAKE A POLY TRACK");
  } else if (w.seqmode) {
    footer(c, "TAP:STEP HOLD+PADS:NOTES");
  } else {
    footer(c, "GRID REC:EDIT F+DOWN:MENU");   // updown1029: FUNC + DOWN (UP / DOWN, YES: the Rytm's)
  }
}

// In the page list when there is something to sequence: a seq track, seq mode, a Rytm track on a Sidecar machine (the
// Rytm's 33; E0: every Sidecar machine can be a poly track, not only 6-OP FM / MULTISAMPLE / WAVETABLE / GRANULAR), or
// the host asked for it (seq=show, the Rytm's seq mode)
CORE_COLD bool seq_shown() {
  seq::View w = seq::view();
  if (w.seqmode || seq::shown_wanted()) return true;
  for (int t = 0; t < 12; ++t) {
    int vb = remote_track_machine(t);
    if (vb >= 0 && vb != 0x7F) return true;
  }
  return seq::any_tracks();
}
CORE_COLD void seq_enc(int e, int delta, bool func) { seq::ui_enc(e, delta, func, true); }
CORE_COLD bool seq_key(int code) { return code >= 0 && code < 16; }   // (remote.cpp gives them to seq first)
CORE_COLD uint16_t seq_trigs() { return seq::shown_poly() ? 0xFFFF : 0; }   // (fix1028 review: an off track's are stock's)
// fix1028: the trig LEDs are the poly track's only - a track turned off (or never poly) gets the Rytm's own back
CORE_COLD bool seq_leds(uint8_t c[16]) { return seq::shown_poly() && seq::leds(c); }

}  // namespace

CORE_COLD int ne_window(int w0, int visible, int n, int focus, int margin) {
  if (visible <= 0 || n <= visible) return 0;
  if (margin > (visible - 1) / 2) margin = (visible - 1) / 2;
  if (margin < 0) margin = 0;
  int top = n - visible;
  w0 = w0 < 0 ? 0 : w0 > top ? top : w0;
  if (focus < 0 || focus >= n) return w0;
  if (focus < w0 + margin) w0 = focus - margin;
  else if (focus > w0 + visible - 1 - margin) w0 = focus - (visible - 1 - margin);
  return w0 < 0 ? 0 : w0 > top ? top : w0;
}
CORE_COLD NeRoll ne_roll_shown() { return ne_shown; }

UI_ROM const ExtraPage kSeqPage = {"SEQ", seq_shown, seq_draw, seq_enc, seq_key, 0xFF, false, nullptr, seq_trigs, seq_leds};

}  // namespace ui
}  // namespace core
#endif
