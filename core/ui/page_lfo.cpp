// core/ui/page_lfo.cpp - see extra.h: the LFO page (core/lfo.h, docs/lfo.md). Cold (program flash on the Teensy).
//
// 0.5.0 (LFO FINAL, the owner): the Pi's 26 LFOs are two a track (T1..T12, FX: core/lfo.h track_of / slot_of), and the
// page shows the selected track's two only - the Rytm's selected track (49 1 2, remote_selected_track): its A, or its
// B; A, LEFT / RIGHT step between the two. FUNC + A steps the track the page shows (T1..T12, FX), until the Rytm selects
// another: the FX track's pair on a 0026 that reports sound tracks only (it never sends 49 1 2 with track 12), and any
// track's while the Rytm has named none. TGT ends before RT1 (0.5.0 owner test: no LFO uses the Rytm link). The footer
// says "RYTM LINK FULL: SLOWER" while lfo::link_full (kept; no LFO can be on the link now, so it does not show).
// The Mini (4 LFOs, no tracks): A steps through all four, as before.
// Main view: the selected LFO's wave over one cycle (rows 10..29) with a dot at its phase, then the eight encoders as
// two rows of four (label over value): A LFO (which one: "T3 A"), B TGT (OFF, V1..V8, FX1, FX2, ...), C PAR (the target's
// parameter by name), D DEP (bipolar), E WAVE, F RATE (Hz, or a
// division when synced), G SYNC (FREE / CLK), H TRIG (FREE / TRIG / PLAY). YES opens the design view.
// Design view (DESIGN's 16 steps): the curve over the width with the playhead, the selected step marked; T1..T16
// (caught only here) or A select a step, B its value, C its shape (HOLD / RAMP / SMTH), D a preset (RAMP TRI STEPS
// RND, applied on the turn). YES or NO back to the main view.
// The dot and the playhead move at most every kDotMs: a few tiles a frame.
#include <string.h>

#include "hooks.h"
#include "lfo.h"
#include "machines.h"
#include "md/md.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"
#include "voice_name.h"
#include "voices.h"

namespace core {
namespace ui {

// The labels of a machine's P1..P16 that no code page names (the Rytm's SMPL page, rytm-mods 0021 / 0026's tables;
// docs/*.md): k 0..15, null none (unused, or on a code page: voice_label looks there first)
CORE_COLD const char* machine_label(int m, int k) {
  static UI_ROM const char kPlaits2[8][5] = {"FM", "FIN", "AUX", "TMD", "MMD", "VEL", "GLD", "VOL"};   // PLAITS PAGE 2
  static UI_ROM const char kMs2[8][5] = {"TUNE", "FINE", "STRT", "LOOP", "CHD", "INV", "SPR", "VOI"};
  static UI_ROM const char kWt2[8][5] = {"ATK", "DEC", "SUS", "ENV", "CHD", "INV", "SPR", "VOI"};
  static UI_ROM const char kGr2[8][5] = {"ATK", "REL", "HOLD", "ANIM", "CHD", "INV", "SPR", "VOI"};
  static UI_ROM const char kDaisyL[16][5] = {"ENG", "STR", "BRI", "DMP", "INP", "EXC", "TUN", "LEV",
                                            "XTN", "XDC", "SEED", "OUT", "XP4", "XLV", "XP5", "WID"};   // rui1050: P11 SEED
  static UI_ROM const char kChop2[8][5] = {"STUT", "SLEN", "TAPE", "TTIM", "ATK", "DEC", "DIR", "LOOP"};
  static UI_ROM const char kFm2[8][5] = {"", "", "", "", "CHD", "INV", "SPR", "VOI"};   // P9..P12 unused (core/poly.h)
  static UI_ROM const char kLive2[8][5] = {"REL", "", "", "", "", "", "", ""};
  static UI_ROM const char kMidi[16][5] = {"DST", "CHAN", "BANK", "SBNK", "PROG", "PB", "AT", "MW",
                                           "VAL1", "VAL2", "VAL3", "VAL4", "SEL1", "SEL2", "SEL3", "SEL4"};
  if (k < 0 || k > 15) return nullptr;
  const char* l = nullptr;
  if (m == kDaisy) l = kDaisyL[k];
  else if (m == kMidiMachine) l = kMidi[k];
  else if (k < 8) return nullptr;
  else if (m == kPlaits) l = kPlaits2[k - 8];
  else if (m == kFm) l = kFm2[k - 8];
  else if (m == kMultiMachine) l = kMs2[k - 8];
  else if (m == kWaveMachine) l = kWt2[k - 8];
  else if (m == kGranMachine) l = kGr2[k - 8];
  else if (m == kChopMachine) l = kChop2[k - 8];
  else if (m == kLiveMachine) l = kLive2[k - 8];
  return l && l[0] ? l : nullptr;
}

// Voice vi's P(k+1), k 0..15, as the Rytm labels it for the machine the voice plays now (remote_voice_machine: follows
// a machine change, SAMPLER, GRAIN's mode): a user machine's manifest (its label set shown), MD's TYPE, the machine's
// code pages (its own and the SRC view's), machine_label. Into b; "" when the machine leaves that P unused.
CORE_COLD const char* voice_label(int vi, int k, char* b, int cap) {
  if (cap <= 0) return b;
  b[0] = 0;
  vi &= 7, k &= 15;
  const int m = remote_voice_machine(vi);
  if (m >= 0x50 && m <= 0x70 && !(m & 7)) {   // U1..U5
    if (!hooks.user_label || !hooks.user_label(vi, k, b, cap)) b[0] = 0;
    return b;
  }
  const char* l = nullptr;
#if CORE_MD
  if (m == kMd) l = md::label(vi, k);
#endif
  const Page* p = m == kMd ? nullptr : page_for_voice(m, vi);
  for (const Page* q : {p, p ? src_view_page(p) : nullptr})
    for (int e = 0; q && !l && e < 8; ++e)
      if (q->enc[e] == k && q->label[e][0]) l = q->label[e];
  if (!l && m != kMd) l = machine_label(m, k);
  if (l) sniprintf(b, cap, "%.4s", l);
  return b;
}

namespace {

constexpr uint32_t kDotMs = 50;
constexpr int kPlotY = 11, kPlotH = 18;              // main view: the wave's rows
constexpr int kRow0 = 31, kRow1 = 43;                // the two encoder rows' label rows (values 6 below)
constexpr int kDesY = 11, kDesH = 32;                // design view: the curve's rows

struct LfoPage {
  int sel = 0;          // the LFO shown
  uint32_t trk_gen = 0; // the Rytm's selection count it followed last (remote_selected_track)
  bool refollow = true; // the next look takes the Rytm's selected track whatever the count (the page left: lfo_page_reset)
  bool design = false;  // the design view
  int step = 0;         // its selected step
  int touched = -1;     // the encoder turned last (main: 0..7; design: 0..3)
  int preset = 0;
  uint32_t dot_ms = 0;
  float dot_ph = 0, dot_w = 0;
};
CORE_RAM2 LfoPage lp;

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

CORE_COLD int ywave(float w, int y0, int h) { return y0 + (int)((1.0f - w) * 0.5f * (float)(h - 1) + 0.5f); }

// the page follows the Rytm's selected track (kPerTrack): when it selects another, that track's LFO in the same slot
CORE_COLD void follow_track() {
  if (!lfo::kPerTrack) return;
  uint32_t gen = 0;
  int t = remote_lfo_track(&gen);
  if (gen == lp.trk_gen && !lp.refollow) return;
  lp.trk_gen = gen, lp.refollow = false;
  if (t < 0 || t >= lfo::kTracks || t == lfo::track_of(lp.sel)) return;
  lp.sel = lfo::lfo_of(t, lfo::slot_of(lp.sel)), lp.dot_ms = 0, lp.design = false, lp.touched = -1;
}
// the LFO's name for the page: "T3 A", "FX B" (the Mini: "1/4")
CORE_COLD void sel_name(char* b, int cap, bool of_n) {
  if (!lfo::kPerTrack) sniprintf(b, cap, of_n ? CORE_STR("%d/%d") : CORE_STR("%d"), lp.sel + 1, lfo::kLfos);
  else if (lfo::track_of(lp.sel) == 12) sniprintf(b, cap, CORE_STR("FX %c"), 'A' + lfo::slot_of(lp.sel));
  else sniprintf(b, cap, CORE_STR("T%d %c"), lfo::track_of(lp.sel) + 1, 'A' + lfo::slot_of(lp.sel));
}

CORE_COLD void main_draw(Canvas& c, int page, int npages, const lfo::Settings& s) {
  char t[12], right[14], pn[8], nm[8];
  sel_name(nm, sizeof nm, false);
  sniprintf(t, sizeof t, CORE_STR("LFO %s"), nm);
  lfo::param_name(s.target, s.param, pn, sizeof pn);
  if (s.target) sniprintf(right, sizeof right, CORE_STR("%s %s"), lfo::target_label(s.target), pn);
  else strcpy(right, "OFF");
  header(c, t, page, npages, right);
  // the wave over one cycle, the centre dotted, the dot at the phase
  for (int x = 0; x < kW; x += 2) c.pixel(x, kPlotY + kPlotH / 2);
  int py = -1;
  for (int x = 0; x < kW; ++x) {
    int y = ywave(lfo::wave_at(s, (float)x / kW, 0x1234u + (uint32_t)lp.sel), kPlotY, kPlotH);
    if (py >= 0) c.vline(x, py < y ? py + (y > py + 1 ? 1 : 0) : y, py < y ? y : py - (py > y + 1 ? 1 : 0));
    c.pixel(x, y);
    py = y;
  }
  int dx = clampi((int)(lp.dot_ph * kW), 0, kW - 1), dy = ywave(lp.dot_w, kPlotY, kPlotH);
  c.fill(Rect{(int16_t)(dx - 1), (int16_t)(dy - 1), 3, 3}, kOn);
  c.fill(Rect{(int16_t)dx, (int16_t)dy, 1, 1}, kOff);
  // the encoders: label row, value row
  static UI_ROM const char kLab[8][5] = {"LFO", "TGT", "PAR", "DEP", "WAVE", "RATE", "SYNC", "TRIG"};
  char val[8][8];
  sel_name(val[0], 8, true);
  sniprintf(val[1], 8, "%s", lfo::target_label(s.target));
  if (s.target >= lfo::kTVoice && s.target < lfo::kTVoice + 8 && !strcmp(pn, "-")) sniprintf(val[2], 8, CORE_STR("P%d -"), (s.param & 15) + 1);   // unused here
  else sniprintf(val[2], 8, "%s", s.target ? pn : "-");
  sniprintf(val[3], 8, CORE_STR("%s%d"), s.depth < 0 ? "-" : "+", s.depth < 0 ? -s.depth : s.depth);   // (the sign by hand: the Pi's printf has no plus flag)
  sniprintf(val[4], 8, "%s", lfo::wave_name(s.wave));
  lfo::rate_name(s, val[5], 8);
  sniprintf(val[6], 8, "%s", s.sync ? "CLK" : "FREE");
  sniprintf(val[7], 8, "%s", lfo::retrig_name(s.retrig));
  for (int e = 0; e < 8; ++e) {
    int x = (e & 3) * kCellW + kCellW / 2, y = e < 4 ? kRow0 : kRow1;
    c.put_text(kSlotLabels + e, kRSmall, x, y, kLab[e], kAlignCentre);
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, val[e], kAlignCentre);
  }
  c.hline(0, kW - 1, kRow1 - 2, kDither);
  char b[kTextLen + 1];
  if (lp.touched == 3) {
    sniprintf(b, sizeof b, CORE_STR("%s DEPTH:%s%d"), nm, s.depth < 0 ? "-" : "+", s.depth < 0 ? -s.depth : s.depth);
    footer(c, b, s.depth + 64, true);
  } else if (lfo::link_full()) {
    footer(c, "RYTM LINK FULL: SLOWER");
  } else {
    footer(c, "YES: DESIGN  NO: BACK");
  }
}

CORE_COLD void design_draw(Canvas& c, int page, int npages, const lfo::Settings& s) {
  char t[16], right[10], nm[8];
  sel_name(nm, sizeof nm, false);
  sniprintf(t, sizeof t, CORE_STR("%s DESIGN"), nm);
  sniprintf(right, sizeof right, CORE_STR("STEP %d"), lp.step + 1);
  header(c, t, page, npages, right);
  constexpr int sw = kW / lfo::kSteps;   // 8 px a step
  for (int x = 0; x < kW; x += 2) c.pixel(x, kDesY + kDesH / 2);
  for (int k = 1; k < lfo::kSteps; ++k)
    for (int y = kDesY; y < kDesY + kDesH; y += 4) c.pixel(k * sw, y);
  int py = -1;
  for (int x = 0; x < kW; ++x) {
    int y = ywave(lfo::design_at(s, ((float)x + 0.5f) / kW), kDesY, kDesH);
    if (py >= 0 && y != py) c.vline(x, py < y ? py + 1 : y, py < y ? y : py - 1);
    c.pixel(x, y);
    py = y;
  }
  // the selected step: a bar under the curve; the playhead: a dotted column
  c.fill(Rect{(int16_t)(lp.step * sw + 1), (int16_t)(kDesY + kDesH + 1), (int16_t)(sw - 1), 2}, kOn);
  int ph = clampi((int)(lp.dot_ph * kW), 0, kW - 1);
  if (s.wave == lfo::kDesign)
    for (int y = kDesY; y < kDesY + kDesH; y += 2) c.pixel(ph, y, kXor);
  char val[4][8];
  sniprintf(val[0], 8, CORE_STR("STP%d"), lp.step + 1);
  sniprintf(val[1], 8, CORE_STR("%s%d"), s.step[lp.step] < 0 ? "-" : "+", s.step[lp.step] < 0 ? -s.step[lp.step] : s.step[lp.step]);
  sniprintf(val[2], 8, "%s", lfo::shape_name(s.shape[lp.step]));
  sniprintf(val[3], 8, "%s", lfo::preset_name(lp.preset));
  for (int e = 0; e < 4; ++e) c.put_text(kSlotCells + e, kRSmall, e * kCellW + kCellW / 2, 48, val[e], kAlignCentre);
  char b[kTextLen + 1];
  if (lp.touched == 1) {
    sniprintf(b, sizeof b, CORE_STR("STEP %d VALUE:%s%d"), lp.step + 1, s.step[lp.step] < 0 ? "-" : "+",
              s.step[lp.step] < 0 ? -s.step[lp.step] : s.step[lp.step]);
    footer(c, b, s.step[lp.step] + 64, true);
  } else if (s.wave != lfo::kDesign) {
    footer(c, "WAVE DESIGN TO HEAR IT");
  } else {
    footer(c, "T1-16: STEP  NO: BACK");
  }
}

CORE_COLD void lfo_draw(Canvas& c, int page, int npages, uint32_t ms) {
  follow_track();
  const lfo::Settings& s = lfo::get(lp.sel);
  if (ms - lp.dot_ms >= kDotMs || !lp.dot_ms) lp.dot_ms = ms | 1, lp.dot_ph = lfo::phase(lp.sel), lp.dot_w = lfo::out(lp.sel);
  c.clear();
  if (lp.design) design_draw(c, page, npages, s);
  else main_draw(c, page, npages, s);
}

// The LFO or, in the design view, the step before / after (knob A's list, the keys' too): on the Pi the shown track's A
// or B (track: FUNC + A, the track before / after, same slot)
CORE_COLD void lfo_walk(int d, bool track = false) {
  if (lp.design) lp.step = clampi(lp.step + d, 0, lfo::kSteps - 1);
  else if (!lfo::kPerTrack) lp.sel = clampi(lp.sel + d, 0, lfo::kLfos - 1), lp.dot_ms = 0;
  else if (track) lp.sel = lfo::lfo_of(clampi(lfo::track_of(lp.sel) + d, 0, lfo::kTracks - 1), lfo::slot_of(lp.sel)), lp.dot_ms = 0;
  else lp.sel = lfo::lfo_of(lfo::track_of(lp.sel), clampi(lfo::slot_of(lp.sel) + d, 0, 1)), lp.dot_ms = 0;
  lp.touched = 0;
}

CORE_COLD void lfo_enc(int e, int d, bool func) {
  if (!d) return;
  follow_track();
  lfo::Settings s = lfo::get(lp.sel);
  const int l = !lp.design && (e == 1 || e == 2) ? list_turn_long(e, d) : list_turn(e, d);   // TARGET, PARAM: long lists
  if (e == 0) return lfo_walk(l, func && !lp.design);
  if (lp.design) {
    lp.touched = e;
    if (e == 1) s.step[lp.step] = (int8_t)clampi(s.step[lp.step] + d * (func ? 8 : 1), -64, 63);
    else if (e == 2) s.shape[lp.step] = (uint8_t)clampi(s.shape[lp.step] + l, 0, lfo::kShapes - 1);
    else if (e == 3 && l) lp.preset = clampi(lp.preset + l, 0, lfo::kPresets - 1), lfo::preset(&s, lp.preset);
    lfo::set(lp.sel, s);
    return;
  }
  lp.touched = e;
  switch (e) {
    case 1: {
      lfo::target_step(&s, l);   // the display order (the voices by their tracks first); no Rytm target
      break;
    }
    case 2: lfo::param_step(&s, l); break;   // the target's own params
    case 3: s.depth = (int8_t)clampi(s.depth + d * (func ? 8 : 1), -64, 63); break;
    case 4: s.wave = (uint8_t)clampi(s.wave + l, 0, lfo::kWaves - 1); break;
    case 5: s.rate = (uint8_t)clampi(s.rate + (s.sync ? l : d * (func ? 8 : 1)), 0, s.sync ? lfo::kDivs - 1 : lfo::kFreeRates - 1); break;
    case 6: {
      int y = clampi(s.sync + l, 0, 1);
      if (y != s.sync) s.sync = (uint8_t)y, s.rate = y ? 8 : 64;   // 1/4, ~0.6 Hz
      break;
    }
    case 7: s.retrig = (uint8_t)clampi(s.retrig + l, 0, lfo::kRetrigs - 1); break;
    default: return;
  }
  lfo::set(lp.sel, s);
}

CORE_COLD bool lfo_key(int code) {
  if (code == 0x44) {   // YES: the design view and back
    lp.design = !lp.design, lp.touched = -1;
    return true;
  }
  if (code == 0x40 || code == 0x41) {   // ux1027: LEFT / RIGHT the LFO before / after (the design view: the step), as A
    follow_track();
    lfo_walk(code == 0x41 ? 1 : -1);
    return true;
  }
  if (!lp.design) return false;
  if (code == 0x45) {   // NO: back to the main view (not SIDECAR)
    lp.design = false, lp.touched = -1;
    return true;
  }
  if (code >= 0x00 && code <= 0x0F) {   // T1..T16: the step
    lp.step = code, lp.touched = 0;
    return true;
  }
  return false;
}

CORE_COLD uint8_t lfo_encs() { return lp.design ? 0x0F : 0xFF; }
CORE_COLD uint16_t lfo_trigs() { return lp.design ? 0xFFFF : 0; }

}  // namespace

// ux1027: off the LEFT / RIGHT ring - UP + LFO's page (and the SIDECAR MENU's row) replaces the Rytm's screen
UI_ROM const ExtraPage kLfoPage = {"LFO", nullptr, lfo_draw, lfo_enc, lfo_key, 0xFF, false, lfo_encs, lfo_trigs, nullptr, true};

CORE_COLD void lfo_page_reset() { lp.design = false, lp.touched = -1, lp.refollow = true; }

#if CORE_UI_TOOLS
void lfo_page_set(int sel, bool design, int step, uint32_t ms) {   // tests and the renderer
  lp.sel = sel, lp.design = design, lp.step = step, lp.touched = -1, lp.dot_ms = 0;
  remote_lfo_track(&lp.trk_gen), lp.refollow = false;   // (shown as set: not the Rytm's selection)
  (void)ms;
}
#endif

}  // namespace ui
}  // namespace core
