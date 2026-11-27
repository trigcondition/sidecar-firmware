// core/ui/page_loop.cpp - see extra.h: the LOOP page (spec-050 3.12, core/loop/loop.h). Cold (program flash on the Teensy).
// Off the SIDECAR MENU's row (no UP shortcut: a page request past T14 needs the Rytm's mod, spec 5 / owner: no new flash).
//
// Trig keys (all 16 taken): T1..T6 grab the last 1/2, 1, 2, 4, 8, 16 bars (while a loop plays: a resample, swapped in at
// the next bar line); T8 STOP (a fade at the next bar line; FUNC + T8: a cut at once); T9..T16 the 8 grabs, the newest at
// T9 (a tap shows it and plays it from the next bar line). YES KEEPs the grab shown (a take on the card), NO is back.
// Encoders: A BAL, B LEV, C FADE, D AUTO-MUTE, E LIVE VIA, F the grab shown, G OUT (POST / PRE) (H nothing).
// Screen: the state (RING 34S / PLAYING / WAIT / FADING / RESAMPLE) with the bar position, the 8 grabs (length, a dot when
// kept, a bar under the one playing, the shown one inverted), then BAL LEV FADE MUTE LIVE GRAB OUT and a footer.
// LEDs: a grab green while it plays, blue when kept, dim white when held; T1..T6 dim white, T8 red while a loop is on.
#include <string.h>

#include "loop/loop.h"
#include "mod/mod.h"
#include "mod/target.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

// t050-d (the owner: "this also looks messy"): the grabs in 16 px boxes, the six knobs on the stock grid's four columns
// (A..D over E..F, as the encoders sit), the ring's length once (the header)
constexpr int kBoxW = 16, kBox0 = 0, kRowY = 11, kGrabY = 19;
constexpr int kCols = 7, kLabY = 30, kRowDy = 13;
constexpr uint32_t kNoteMs = 2500;   // a refusal / result shows this long
constexpr uint8_t kLedWhiteDim = 107, kLedGreen = 31, kLedRed = 15;

struct LoopPage {
  int sel = 0;              // the grab shown (0 newest; follows sel_id: a grab landing meanwhile moves the list, not the choice)
  uint32_t sel_id = 0;      // its id (0: none chosen yet)
  bool func = false;
  int touched = -1;         // the encoder turned last (0..5)
  uint32_t msg_gen = 0;     // the loop's last message seen
  uint32_t msg_ms = 0;      // when it appeared (the page's clock)
  const char* note = nullptr;   // a one-shot footer
  uint32_t now = 0;
};
CORE_RAM2 LoopPage lp;

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
CORE_COLD const char* len_text(int hb, char* b, int cap) {
  if (hb == 1) sniprintf(b, cap, ".5");   // (3 characters do not fit a 16 px box)
  else sniprintf(b, cap, "%d", hb / 2);
  return b;
}
CORE_COLD bool is_on() { return loop::on(); }
// a result the looper has to tell ("PLAY FIRST", "KEPT take_001"), shown for kNoteMs from the frame it is first seen
CORE_COLD const char* message() {
  uint32_t g = loop::message_gen();
  if (g != lp.msg_gen) lp.msg_gen = g, lp.msg_ms = lp.now;
  return g && loop::last_message()[0] && lp.now - lp.msg_ms < kNoteMs ? loop::last_message() : nullptr;
}

CORE_COLD void grab_box(Canvas& c, int i) {
  int x = kBox0 + i * kBoxW;
  const loop::Grab* g = loop::grab_at(i);
  bool sel = i == lp.sel && g;
  if (sel) c.fill(Rect{(int16_t)(x + 1), (int16_t)(kGrabY - 1), (int16_t)(kBoxW - 2), 7}, kOn);
  char t[8] = "";
  if (g) len_text(g->half_bars, t, sizeof t);
  c.put_text(kSlotFree + i, kRSmall, x + kBoxW / 2, kGrabY, t, kAlignCentre | (sel ? kTextInvert : 0));
  if (!g) return;
  if (g->kept) c.fill(Rect{(int16_t)(x + kBoxW / 2 - 1), (int16_t)(kGrabY + 7), 2, 1}, kOn);   // a dot: on the card
  if (i == loop::playing()) c.fill(Rect{(int16_t)(x + 2), (int16_t)(kGrabY + 9), (int16_t)(kBoxW - 4), 1}, kOn);   // a bar: it plays
}

CORE_COLD void sync_sel() {   // the shown grab stays the same grab as newer ones land
  int i = lp.sel_id ? loop::grab_index(lp.sel_id) : -1;
  if (lp.sel_id && i < 0) lp.sel_id = 0;   // that grab is gone: the newest again
  lp.sel = lp.sel_id ? i : 0;   // (no grab chosen yet: the newest, whatever lands)
  if (lp.sel < 0) lp.sel = 0;
}
CORE_COLD void remember_sel() {
  const loop::Grab* g = loop::grab_at(lp.sel);
  lp.sel_id = g ? g->id : 0;
}

CORE_COLD void loop_draw(Canvas& c, int page, int npages, uint32_t ms) {
  lp.now = ms;
  sync_sel();
  c.clear();
  char r[12], st[32], pos[16] = "";
  if (!is_on()) {
    header(c, "LOOP", page, npages, "OFF");
    c.put_text(kSlotFree, kRSmall, 2, kRowY + 6, "LOOPER OFF");
    c.put_text(kSlotFree + 1, kRSmall, 2, kRowY + 14, "BOOT: LOOPRAM=MB");
    return;
  }
  loop::State s = loop::state();
  sniprintf(r, sizeof r, "%dS", loop::ring_seconds());
  header(c, "LOOP", page, npages, r);
  if (loop::pending()) strcpy(st, loop::playing() >= 0 ? "RESAMPLE" : "GRAB...");
  else if (s == loop::kWait) strcpy(st, "STARTS AT BAR");
  else if (s == loop::kPlaying) strcpy(st, "PLAYING");
  else if (s == loop::kFading) strcpy(st, "FADING");
  else strcpy(st, loop::grabs() ? "T9-16 PLAY" : "READY");   // (the ring's length: the header)
  float bar, of;
  if (s != loop::kRing && loop::bar_position(&bar, &of)) {
    int b10 = (int)(bar * 10.0f);
    sniprintf(pos, sizeof pos, "%d.%d/%d", b10 / 10, b10 % 10, of >= 1 ? (int)of : 1);
    if (of < 1) strcpy(pos + strlen(pos) - 1, "1/2");
  }
  c.put_text(kSlotFree + 8, kRSmall, 2, kRowY, st);
  c.put_text(kSlotFree + 9, kRSmall, kW - 2, kRowY, pos, kAlignRight);
  for (int i = 0; i < loop::kMaxGrabs; ++i) grab_box(c, i);
  // BAL LEV FADE MUTE LIVE GRAB OUT (the 4-column grid, A..D over E..G: OUT is the second row's third cell)
  static UI_ROM const char kLab[kCols][6] = {"BAL", "LEV", "FADE", "MUTE", "LIVE", "GRAB", "OUT"};
  static UI_ROM const char kFades[loop::kFades][4] = {"1/4", "1/2", "1", "2", "4"};
  char val[kCols][8];
  sniprintf(val[0], 8, "%d", loop::bal());
  sniprintf(val[1], 8, "%d", loop::lev());
  sniprintf(val[2], 8, "%s", kFades[loop::fade()]);
  strcpy(val[3], loop::auto_mute() ? "ON" : "OFF");
  if (loop::live_via()) sniprintf(val[4], 8, "P%d", loop::live_via());
  else strcpy(val[4], "OFF");
  if (loop::grab_at(lp.sel)) sniprintf(val[5], 8, "%d/%d", lp.sel + 1, loop::grabs());
  else strcpy(val[5], "-");
  strcpy(val[6], loop::out() == loop::kOutPre ? "PRE" : "POST");
  for (int k = 0; k < kCols; ++k) {
    int x = (k % 4) * kCellW + kCellW / 2, y = kLabY + (k / 4) * kRowDy;
    c.put_text(kSlotLabels + k, kRSmall, x, y, kLab[k], kAlignCentre);
    c.put_text(kSlotCells + k, kRSmall, x, y + 6, val[k], kAlignCentre);
  }
  char f[kTextLen + 1];
  const char* m = lp.note ? lp.note : message();
  if (m) {
    footer(c, m);
  } else if (lp.touched == 0) {
    sniprintf(f, sizeof f, "BAL:%d  LIVE %d%%", loop::bal(), loop::live_perf(loop::bal()) * 100 / 127);
    footer(c, f, loop::bal());
  } else if (lp.touched == 1) {
    sniprintf(f, sizeof f, "LOOP LEVEL:%d", loop::lev());
    footer(c, f, loop::lev());
  } else if (lp.touched == 6) {
    footer(c, loop::out() == loop::kOutPre ? "OUT PRE: FULL, THRU FX" : "OUT POST: CLEAN, CAPPED");
  } else if (lp.touched == 4 && loop::live_via()) {
    sniprintf(f, sizeof f, "PERF %d: BAL 64..127 DOWN", loop::live_via());
    footer(c, f);
  } else {
    footer(c, "T1-6 GRAB  T8 STOP");
  }
}

CORE_COLD void loop_enc(int e, int d, bool func) {
  if (!d || !is_on()) return;
  lp.note = nullptr;
  int l;
  switch (e) {
    case 0:   // BAL (also the mod target LOOP:BAL: a manual edit wins over a morph)
    case 1: {   // LEV
      int v = clampi((e ? loop::lev() : loop::bal()) + d * (func ? 8 : 1), 0, 127);
      mod::manual(mod::tid(mod::kLoop, 0, e), (float)v);
      if (e) loop::set_lev(v);
      else loop::set_bal(v);
      break;
    }
    case 2:
      if ((l = list_turn(e, d))) loop::set_fade(clampi(loop::fade() + l, 0, loop::kFades - 1));
      break;
    case 3:
      if ((l = list_turn(e, d))) loop::set_auto_mute(l > 0);
      break;
    case 4:
      if ((l = list_turn(e, d))) loop::set_live_via(clampi(loop::live_via() + l, 0, 12));
      break;
    case 5:
      if ((l = list_turn(e, d))) sync_sel(), lp.sel = clampi(lp.sel + l, 0, loop::grabs() ? loop::grabs() - 1 : 0), remember_sel();
      break;
    case 6:
      if ((l = list_turn(e, d))) loop::set_out(l > 0 ? loop::kOutPre : loop::kOutPost);
      break;
    default: return;
  }
  lp.touched = e;
}

CORE_COLD bool loop_key(int code) {
  if (code == 0x80) {   // FUNC (both sides')
    lp.func = true;
    return true;
  }
  lp.note = nullptr;
  if (!is_on()) return code != 0x45 ? code >= 0 && code < 16 : false;
  if (code >= 0 && code <= 5) {   // T1..T6: a grab of 1/2 .. 16 bars
    static const int kLen[6] = {loop::kHalf, loop::k1, loop::k2, loop::k4, loop::k8, loop::k16};
    int e = loop::grab(kLen[code]);
    if (e) {
      static char t[32];
      lp.note = loop::err_text(e, kLen[code], t, sizeof t);
    }
    return true;
  }
  if (code == 7) {   // T8: STOP; FUNC + T8 the other one, at once
    if (lp.func) loop::stop(true, true);
    else loop::stop(false, false);
    return true;
  }
  if (code >= 8 && code <= 15) {   // T9..T16: the grabs, newest first: shown and played from the next bar line
    int g = code - 8;
    if (g < loop::grabs()) {
      lp.sel = g, remember_sel();
      if (!loop::play(g)) lp.note = "PLAY FIRST";
    }
    return true;
  }
  if (code >= 0 && code < 16) return true;   // T7: nothing
  if (code == 0x44) {   // YES: KEEP the grab shown
    static char t[32];
    sync_sel();
    if (!loop::keep(lp.sel, t, sizeof t)) lp.note = t;
    return true;
  }
  if (code == 0x45) return false;   // NO: back
  return false;
}

CORE_COLD bool loop_key_up(int code) {
  if (code == 0x80) lp.func = false;
  return true;
}

CORE_COLD uint16_t loop_trigs() { return 0xFFFF; }

CORE_COLD bool loop_leds(uint8_t c[16]) {
  memset(c, kLedOff, 16);
  if (!is_on()) return true;
  for (int k = 0; k < 6; ++k) c[k] = kLedWhiteDim;
  if (loop::state() != loop::kRing || loop::pending()) c[7] = kLedRed;
  int pl = loop::playing();
  for (int i = 0; i < loop::kMaxGrabs; ++i) {
    const loop::Grab* g = loop::grab_at(i);
    c[8 + i] = !g ? kLedOff : i == pl ? kLedGreen : g->kept ? kLedBlue : kLedWhiteDim;
  }
  return true;
}

CORE_COLD uint16_t loop_enc_tid(int e) { return e == 0 ? mod::tid(mod::kLoop, 0, 0) : e == 1 ? mod::tid(mod::kLoop, 0, 1) : 0; }

}  // namespace

UI_ROM const ExtraPage kLoopPage = {"LOOP", is_on, loop_draw, loop_enc, loop_key, 0x7F, false, nullptr, loop_trigs, loop_leds, true,
                                    loop_key_up, loop_enc_tid};

CORE_COLD void loop_page_reset() { lp = LoopPage(); }
CORE_COLD int loop_page_sel() { return lp.sel; }

}  // namespace ui
}  // namespace core
