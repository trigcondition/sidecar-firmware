// core/ui/page_lfo.cpp - see extra.h: the LFO page (core/lfo.h, docs/lfo.md). Cold (program flash on the Teensy).
//
// Main view: the selected LFO's wave over one cycle (rows 10..29) with a dot at its phase, then the eight encoders as
// two rows of four (label over value): A LFO (which one), B TGT (OFF, V1..V8, FX1, FX2), C PAR (the target's
// parameter by name), D DEP (bipolar), E WAVE, F RATE (Hz, or a
// division when synced), G SYNC (FREE / CLK), H TRIG (FREE / TRIG / PLAY). YES opens the design view.
// Design view (DESIGN's 16 steps): the curve over the width with the playhead, the selected step marked; T1..T16
// (caught only here) or A select a step, B its value, C its shape (HOLD / RAMP / SMTH), D a preset (RAMP TRI STEPS
// RND, applied on the turn). YES or NO back to the main view.
// The dot and the playhead move at most every kDotMs: a few tiles a frame.
#include <string.h>

#include "lfo.h"
#include "machines.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"
#include "voice_name.h"
#include "voices.h"

namespace core {
namespace ui {

// The label of voice vi's P(k+1) on its machine's page (lfo.cpp's parameter names), null when the page has none.
CORE_COLD const char* voice_label(int vi, int k) {
  const V& x = v[vi & 7];
  int m = x.user >= 0 ? 0x50 + 8 * x.user : x.fm ? kFm : x.juno ? kJuno : x.grain ? kGrainMachine : x.dsps ? kDspSample : x.daisy ? kDaisy
                                                                          : x.md ? kMd : kPlaits;
  const Page* p = page_for_voice(m, vi & 7);
  if (!p) return nullptr;
  for (int e = 0; e < 8; ++e)
    if (p->enc[e] == k) return p->label[e];
  return nullptr;
}

namespace {

constexpr uint32_t kDotMs = 50;
constexpr int kPlotY = 11, kPlotH = 18;              // main view: the wave's rows
constexpr int kRow0 = 31, kRow1 = 43;                // the two encoder rows' label rows (values 6 below)
constexpr int kDesY = 11, kDesH = 32;                // design view: the curve's rows

struct LfoPage {
  int sel = 0;          // the LFO shown
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

CORE_COLD void main_draw(Canvas& c, int page, int npages, const lfo::Settings& s) {
  char t[12], right[14], pn[8];
  sniprintf(t, sizeof t, CORE_STR("LFO %d"), lp.sel + 1);
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
  sniprintf(val[0], 8, CORE_STR("%d/%d"), lp.sel + 1, lfo::kLfos);
  sniprintf(val[1], 8, "%s", lfo::target_label(s.target));
  sniprintf(val[2], 8, "%s", s.target ? pn : "-");
  sniprintf(val[3], 8, CORE_STR("%+d"), s.depth);
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
    sniprintf(b, sizeof b, CORE_STR("L%d DEPTH:%+d"), lp.sel + 1, s.depth);
    footer(c, b, s.depth + 64, true);
  } else {
    footer(c, "YES: DESIGN  NO: BACK");
  }
}

CORE_COLD void design_draw(Canvas& c, int page, int npages, const lfo::Settings& s) {
  char t[16], right[10];
  sniprintf(t, sizeof t, CORE_STR("L%d DESIGN"), lp.sel + 1);
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
  sniprintf(val[1], 8, CORE_STR("%+d"), s.step[lp.step]);
  sniprintf(val[2], 8, "%s", lfo::shape_name(s.shape[lp.step]));
  sniprintf(val[3], 8, "%s", lfo::preset_name(lp.preset));
  for (int e = 0; e < 4; ++e) c.put_text(kSlotCells + e, kRSmall, e * kCellW + kCellW / 2, 48, val[e], kAlignCentre);
  char b[kTextLen + 1];
  if (lp.touched == 1) {
    sniprintf(b, sizeof b, CORE_STR("STEP %d VALUE:%+d"), lp.step + 1, s.step[lp.step]);
    footer(c, b, s.step[lp.step] + 64, true);
  } else if (s.wave != lfo::kDesign) {
    footer(c, "WAVE DESIGN TO HEAR IT");
  } else {
    footer(c, "T1-16: STEP  NO: BACK");
  }
}

CORE_COLD void lfo_draw(Canvas& c, int page, int npages, uint32_t ms) {
  const lfo::Settings& s = lfo::get(lp.sel);
  if (ms - lp.dot_ms >= kDotMs || !lp.dot_ms) lp.dot_ms = ms | 1, lp.dot_ph = lfo::phase(lp.sel), lp.dot_w = lfo::out(lp.sel);
  c.clear();
  if (lp.design) design_draw(c, page, npages, s);
  else main_draw(c, page, npages, s);
}

CORE_COLD void lfo_enc(int e, int d, bool func) {
  if (!d) return;
  lfo::Settings s = lfo::get(lp.sel);
  if (lp.design) {
    lp.touched = e;
    if (e == 0) lp.step = clampi(lp.step + d, 0, lfo::kSteps - 1);
    else if (e == 1) s.step[lp.step] = (int8_t)clampi(s.step[lp.step] + d * (func ? 8 : 1), -64, 63);
    else if (e == 2) s.shape[lp.step] = (uint8_t)clampi(s.shape[lp.step] + d, 0, lfo::kShapes - 1);
    else if (e == 3) lp.preset = clampi(lp.preset + d, 0, lfo::kPresets - 1), lfo::preset(&s, lp.preset);
    lfo::set(lp.sel, s);
    return;
  }
  lp.touched = e;
  switch (e) {
    case 0: lp.sel = clampi(lp.sel + d, 0, lfo::kLfos - 1); lp.dot_ms = 0; return;
    case 1: {
      int t = clampi(s.target + d, 0, lfo::kTargets - 1);
      if (t != s.target) s.target = (uint8_t)t, s.param = t >= lfo::kTVoice && t < lfo::kTFx ? 1 : 0;
      break;
    }
    case 2: s.param = (uint8_t)clampi(s.param + d, 0, lfo::params_of(s.target) ? lfo::params_of(s.target) - 1 : 0); break;
    case 3: s.depth = (int8_t)clampi(s.depth + d * (func ? 8 : 1), -64, 63); break;
    case 4: s.wave = (uint8_t)clampi(s.wave + d, 0, lfo::kWaves - 1); break;
    case 5: s.rate = (uint8_t)clampi(s.rate + d * (func && !s.sync ? 8 : 1), 0, s.sync ? lfo::kDivs - 1 : lfo::kFreeRates - 1); break;
    case 6: {
      int y = clampi(s.sync + d, 0, 1);
      if (y != s.sync) s.sync = (uint8_t)y, s.rate = y ? 8 : 64;   // 1/4, ~0.6 Hz
      break;
    }
    case 7: s.retrig = (uint8_t)clampi(s.retrig + d, 0, lfo::kRetrigs - 1); break;
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
    lfo_enc(0, code == 0x41 ? 1 : -1, false);
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

CORE_COLD void lfo_page_reset() { lp.design = false, lp.touched = -1; }

#if CORE_UI_TOOLS
void lfo_page_set(int sel, bool design, int step, uint32_t ms) {   // tests and the renderer
  lp.sel = sel, lp.design = design, lp.step = step, lp.touched = -1, lp.dot_ms = 0;
  (void)ms;
}
#endif

}  // namespace ui
}  // namespace core
