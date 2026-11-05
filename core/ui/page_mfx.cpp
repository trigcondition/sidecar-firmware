// core/ui/page_mfx.cpp - see extra.h: the MASTER page (core/fx/master.h, docs/MASTER-FX.md): the master chain on the Rytm's
// screen. Drawn by the host: no Rytm code at all. On the LEFT / RIGHT ring and in the SIDECAR MENU; no UP key (that takes a
// Rytm mod: sidecar-mods 0026's T1..T13 are taken). Cold. CORE_POLY builds only.
//
// CHAIN view: the four slots as a strip, then A..D the selected slot's P1..P4, E SLOT, F TYPE (what the slot can take), G MIX
// (a send TYPE) or MODE (AMP: ONE on the sum, L/R a pair), H SRC (RET / MIX). YES: bypass. The footer says what the chain
// holds: with SRC RET the Rytm's own mix is not in it.
// EQ view (RIGHT; LEFT or NO back, as on the INSERT page): the chain's POST EQ, A HPF, B LPF, C LOW, D HIGH, H LEV.
#include <string.h>

#include "fx/insert.h"
#include "fx/master.h"
#include "mod/mod.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

#if CORE_POLY
namespace {

constexpr int kRow0 = 28, kRow1 = 43, kStripY = 12, kStripH = 13;

struct MfxPage {
  bool eq = false;
  int slot = 0;
  int touched = -1;
};
CORE_RAM2 MfxPage mp;

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

CORE_COLD void strip(Canvas& c) {
  for (int i = 0; i < master::kSlots; ++i) {
    Rect r{(int16_t)(i * 21), (int16_t)kStripY, 19, (int16_t)kStripH};
    bool set = ins::control.type[master::chain(i)][master::slot(i)] != 0, sel = i == mp.slot && !mp.eq;
    if (sel) c.fill(r, kOn);
    else c.rect(r, kOn);
    char b[4];
    sniprintf(b, sizeof b, CORE_STR("%d%s"), i + 1, set ? "*" : "");
    c.put_text(kSlotFree + i, kRSmall, r.x + 9, kStripY + 4, b, kAlignCentre | (sel ? kTextInvert : 0));
  }
  char b[20];
  sniprintf(b, sizeof b, CORE_STR("%s %s"), master::control.src == master::kMix ? "MIX" : "RET", !master::on() ? "OFF" : master::control.byp ? "BYP" : "ON");
  c.put_text(kSlotFree + 4, kRSmall, 127, kStripY + 4, b, kAlignRight);
}

CORE_COLD void cells(Canvas& c, char (*lab)[6], char (*val)[8]) {
  for (int e = 0; e < 8; ++e) {
    int x = (e & 3) * kCellW + kCellW / 2, y = e < 4 ? kRow0 : kRow1;
    c.put_text(kSlotLabels + e, kRSmall, x, y, lab[e], kAlignCentre);
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, val[e], kAlignCentre);
  }
  c.hline(0, kW - 1, kRow1 - 2, kDither);
}

CORE_COLD void mfx_draw(Canvas& c, int page, int npages, uint32_t ms) {
  (void)ms;
  c.clear();
  char lab[8][6], val[8][8], b[kTextLen + 1];
  int e = mp.touched;
  if (mp.eq) {
    header(c, "MASTER EQ", page, npages, "");
    strip(c);
    const ins::Labels& L = ins::eq_labels();
    for (int k = 0; k < 8; ++k) sniprintf(lab[k], 6, "-"), sniprintf(val[k], 8, "-");
    for (int k = 0; k < ins::kEqParams; ++k) {
      sniprintf(lab[k], 6, "%s", L.label[k]);
      ins::eq_value_text(k, ins::eq_param(master::kEqChain, k), val[k], 8);
    }
    sniprintf(lab[7], 6, "LEV");
    sniprintf(val[7], 8, CORE_STR("%d"), master::control.lev);
    cells(c, lab, val);
    if (e >= 0 && e < ins::kEqParams) {
      sniprintf(b, sizeof b, CORE_STR("EQ %s:%s"), lab[e], val[e]);
      footer(c, b, ins::eq_param(master::kEqChain, e));
    } else {
      footer(c, "LEFT: BACK  H: LEV");
    }
    return;
  }
  const int ch = master::chain(mp.slot), s = master::slot(mp.slot), t = ins::control.type[ch][s];
  const ins::Labels& L = ins::labels(t);
  header(c, "MASTER", page, npages, master::control.byp ? "BYPASS" : "");
  strip(c);
  for (int k = 0; k < 4; ++k) {
    sniprintf(lab[k], 6, "%s", t ? L.label[k] : "-");
    if (t) ins::slot_value_text(ch, s, k, ins::param(ch, s, k), val[k], 8);
    else sniprintf(val[k], 8, "-");
  }
  sniprintf(lab[4], 6, "SLOT");
  sniprintf(val[4], 8, CORE_STR("%d"), mp.slot + 1);
  sniprintf(lab[5], 6, "TYPE");
  sniprintf(val[5], 8, "%.7s", ins::type_name(t));
  if (master::send_type(t)) {
    sniprintf(lab[6], 6, "MIX");
    sniprintf(val[6], 8, CORE_STR("%d"), master::control.mix[mp.slot]);
  } else if (t == ins::kAmp) {
    sniprintf(lab[6], 6, "MODE");
    sniprintf(val[6], 8, "%s", master::pair(mp.slot) ? "L/R" : "ONE");
  } else {
    sniprintf(lab[6], 6, "-"), sniprintf(val[6], 8, "-");
  }
  sniprintf(lab[7], 6, "SRC");
  sniprintf(val[7], 8, "%s", master::control.src == master::kMix ? "MIX" : "RET");
  cells(c, lab, val);
  if (e >= 0 && e < 4 && t) {
    sniprintf(b, sizeof b, CORE_STR("%d %s:%s"), mp.slot + 1, L.label[e], val[e]);
    footer(c, b, ins::param(ch, s, e));
  } else if (master::control.src == master::kMix) {
    const int on = master::stems_on_main();
    if (on) sniprintf(b, sizeof b, CORE_STR("%d VOICES STILL ON MAIN"), on);
    footer(c, on ? b : master::control.known ? "THROUGH: PANS FROM THE RYTM" : "MONO: TRACKS OFF MAIN");
  } else {
    footer(c, "RYTM MIX NOT IN THE CHAIN");
  }
}

CORE_COLD void mfx_enc(int e, int d, bool func) {
  if (!d) return;
  mp.touched = e;
  const int st = func ? 8 : 1, l = list_turn(e, d);   // the slot, type and switches: lists; the rest values
  if (mp.eq) {
    if (e < ins::kEqParams) ins::set_eq(master::kEqChain, e, ins::eq_param(master::kEqChain, e) + d * st), mod::manual_knob(mod::tid(mod::kIns, master::kEqChain, 8 + e));
    else if (e == 7) master::set_lev(master::control.lev + d * st), mod::manual_knob(mod::tid(mod::kMfx, 0, 0));
    return;
  }
  const int ch = master::chain(mp.slot), s = master::slot(mp.slot), t = ins::control.type[ch][s];
  if (e < 4) {
    if (t) ins::set_param(ch, s, e, ins::param(ch, s, e) + d * st), mod::manual_knob(mod::tid(mod::kIns, ch, s * 4 + e));
  } else if (e == 4) {
    mp.slot = clampi(mp.slot + l, 0, master::kSlots - 1);
  } else if (e == 5) {
    if (l) ins::set_type(ch, s, master::next_type(mp.slot, t, l));
  } else if (e == 6) {
    if (master::send_type(t)) master::set_mix(mp.slot, master::control.mix[mp.slot] + d * st), mod::manual_knob(mod::tid(mod::kMfx, 0, 1 + mp.slot));
    else if (t == ins::kAmp && l) master::set_pair(mp.slot, l > 0);
  } else if (e == 7) {
    if (l) master::set_src(l > 0 ? master::kMix : master::kRet);
  }
}

CORE_COLD bool mfx_key(int code) {
  if (code == 0x41 && !mp.eq) return mp.eq = true, mp.touched = -1, true;    // RIGHT: the EQ view
  if ((code == 0x40 || code == 0x45) && mp.eq) return mp.eq = false, mp.touched = -1, true;   // LEFT, NO: back
  if (code == 0x44) return master::set_byp(!master::control.byp), true;      // YES: bypass
  return false;
}

// as the INSERT page: a track on a Sidecar machine, or a chain set
CORE_COLD bool mfx_shown() {
  if (master::on()) return true;
  for (int t = 0; t < 12; ++t) {
    int vb = remote_track_machine(t);
    if (vb >= 0 && vb != 0x7F) return true;
  }
  return false;
}

}  // namespace

UI_ROM const ExtraPage kMasterPage = {"MASTER", mfx_shown, mfx_draw, mfx_enc, mfx_key, 0xFF, false, nullptr, nullptr, nullptr};

#if CORE_UI_TOOLS
void mfx_page_set(int slot, bool eq) { mp.slot = slot, mp.eq = eq, mp.touched = -1; }   // tests
#endif
#endif

}  // namespace ui
}  // namespace core
