// core/ui/page_ins.cpp - see extra.h: the INSERT page (core/fx/insert.h, docs/INSERT-FX.md): the Sidecar track's two
// insert slots and its sends to the FX buses, and (YES) the buses' own chains. Cold. CORE_POLY builds only.
//
// TRACK view (the Rytm's selected track, rui1036b; else the last one, FUNC + G another): the chain as a strip (A, B and
// the three sends), then A..D the selected slot's P1..P4 (the TYPE's labels, the values as the effect reads them:
// Hz, dB, ms), E SLOT (A / B), F TYPE (a list: OFF FILTER CRUSH DRIVE EQ COMP DELAY TREMOLO RINGMOD; a turn takes the
// type's defaults), G BUS (which send H edits), H SEND (the voice's level into that bus).
// BUS view (YES; NO back): the same for bus G's chain: A..D, E SLOT, F TYPE, G BUS 1..3, H ROUTE (bus 3: PRE /
// POST; buses 1 and 2 return as their send FX's P1 says: shown, not set here). Step locks of A..H: hold a trig on the
// SEQ page and press FX twice (seq.h kLpIns).
// OUT view (YES from the BUS view, i.e. YES twice; YES or NO back to the TRACK view): the voice's OUT route (core/fx/out.h,
// docs/STEREO.md): A OUT (TRK / PRE / POST), B LEV, C PAN, D WID, E DEC (DSP SAMPLE only, else -); the line under the
// title says what PRE / POST leave out (the Rytm's FLTR, AMP, DEL / REV). The TRACK view's strip ends in the OUT box
// (the sends: H, and the footer).
// EQ view (ins1112: **RIGHT** from any of them, LEFT or NO back; RIGHT again steps on to the next page as it always
// did): the chain's POST EQ, the stage after both slots (AMP among them). A HPF, B LPF, C LOW shelf, D HIGH shelf,
// G the bus / FUNC + G the track as elsewhere; YES swaps between the voice's EQ and the bus's. Its strip is
// [A][B] > [POST EQ] > the OUT box / the bus's return, which is where the stage really is: after the inserts, before
// the voice goes back to the Rytm (or, on buses 1 and 2, on the send FX's return).
#include <string.h>

#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "machines.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"
#include "voice_name.h"

namespace core {
namespace ui {

#if CORE_POLY
namespace {

UI_ROM const char* const kTrackNames[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
constexpr int kRow0 = 28, kRow1 = 43;   // the two encoder rows' label rows (values 6 below)
constexpr int kStripY = 12, kStripH = 13;

struct InsPage {
  bool bus = false;      // the BUS view
  bool out = false;      // the OUT view
  bool eq = false;       // ins1112: the EQ view (RIGHT)
  int slot = 0;          // the slot A..D edit
  int busel = 0;         // G: the bus (the TRACK view: the send H edits; the BUS view: the chain shown)
  int track = -1;        // the track shown (-1: none known yet)
  uint32_t gen = 0;      // the Rytm's selection count seen
  bool manual = false;   // FUNC + G picked the track: kept until the Rytm selects another
  int touched = -1;
};
CORE_RAM2 InsPage ip;

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// the track shown: the Rytm's selection when it names one (a new selection wins over a FUNC + G pick)
CORE_COLD int track_now() {
  uint32_t g = 0;
  int t = remote_selected_track(&g);
  if (t >= 0 && t < 12 && (g != ip.gen || ip.track < 0) && !(ip.manual && g == ip.gen)) ip.track = t, ip.manual = false;
  ip.gen = g;
  if (ip.track < 0) ip.track = 0;
  return ip.track;
}
CORE_COLD int voice_now() {
  int vi = remote_track_voice(track_now());
  return vi >= 0 && vi < ins::kVoices ? vi : 0;
}
CORE_COLD int chain_now() { return ip.bus ? ins::kBus0 + ip.busel : voice_now(); }

CORE_COLD void strip(Canvas& c, int ch) {
  // [A TYPE] > [B TYPE] > the sends (TRACK) / the return (BUS)
  for (int s = 0; s < ins::kSlots; ++s) {
    Rect r{(int16_t)(s * 41), (int16_t)kStripY, 38, (int16_t)kStripH};
    int t = ins::control.type[ch][s];
    bool sel = s == ip.slot;
    if (sel) c.fill(r, kOn);
    else c.rect(r, kOn);
    // an effect: its name alone (the box's place says the slot; "A TREMOLO" did not fit the 38 px box, 7 letters of
    // the Rytm's small font do); an empty slot keeps its letter
    char b[12];
    if (t) sniprintf(b, sizeof b, "%s", ins::type_name(t));
    else sniprintf(b, sizeof b, CORE_STR("%c -"), 'A' + s);
    c.put_text(kSlotFree + s, kRSmall, r.x + 19, kStripY + 4, b, kAlignCentre | (sel ? kTextInvert : 0));
    c.hline(r.x + 38, r.x + 40, kStripY + kStripH / 2, kOn);   // the arrow's shaft
  }
  char b[20];
  if (!ip.bus) {   // the OUT box
    Rect r{83, (int16_t)kStripY, 45, (int16_t)kStripH};
    c.rect(r, kOn);
    sniprintf(b, sizeof b, CORE_STR("OUT %s"), out::route_name(out::control.route[ch & 7]));
    c.put_text(kSlotFree + 2, kRSmall, r.x + 22, kStripY + 4, b, kAlignCentre);
    return;
  } else if (ip.busel < 2) {
    int k = ip.busel;
    bool post = fx::control.route == fx::kRouteMain || (fx::control.route == fx::kRouteSlot && fx::control.knob[k][1] >= 64);
    sniprintf(b, sizeof b, CORE_STR("%s"), post ? "POST" : "PRE");
  } else {
    sniprintf(b, sizeof b, "%s", ins::control.route3 ? "POST" : "PRE");
  }
  c.put_text(kSlotFree + 2, kRSmall, 127, kStripY + 4, b, kAlignRight);
}

CORE_COLD bool dsp_sample_now() { return machine_of(remote_track_machine(track_now()) & 0x7F) == kDspSample; }
// the OUT view (see the top)
CORE_COLD void out_draw(Canvas& c, int page, int npages) {
  int vi = voice_now();
  header(c, "INSERT OUT", page, npages, voice_name(vi));
  int r = out::control.route[vi];
  c.put_text(kSlotFree, kRSmall, 64, kStripY + 4, r == out::kTrk ? "THROUGH THE RYTM'S TRACK" : "RYTM FLTR AMP DEL REV OFF",
             kAlignCentre);
  bool dec = dsp_sample_now();
  static UI_ROM const char* const kLab[8] = {"OUT", "LEV", "PAN", "WID", "DEC", "-", "-", "-"};
  char val[8][8];
  sniprintf(val[0], 8, "%s", out::route_name(r));
  for (int k = 0; k < 4; ++k) out::value_text(k, out::param(vi, k), val[1 + k], 8);
  if (!dec) sniprintf(val[4], 8, "-");
  for (int e = 5; e < 8; ++e) sniprintf(val[e], 8, "-");
  for (int e = 0; e < 8; ++e) {
    int x = (e & 3) * kCellW + kCellW / 2, y = e < 4 ? kRow0 : kRow1;
    c.put_text(kSlotLabels + e, kRSmall, x, y, e == 4 && !dec ? "-" : kLab[e], kAlignCentre);
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, val[e], kAlignCentre);
  }
  c.hline(0, kW - 1, kRow1 - 2, kDither);
  char b[kTextLen + 1];
  int e = ip.touched;
  if (e >= 1 && e <= 4 && (e < 4 || dec)) {
    char v[8];
    out::value_text(e - 1, out::param(vi, e - 1), v, sizeof v);
    sniprintf(b, sizeof b, CORE_STR("%s %s:%s"), voice_name(vi), kLab[e], v);
    footer(c, b, out::param(vi, e - 1));
  } else if (r != out::kTrk) {
    footer(c, r == out::kPre ? "PRE: 2/3, MASTER FX ON" : "POST: 0/1, NOTHING AFTER");
  } else {
    footer(c, "A: TRK PRE POST  YES: BACK");
  }
}

// ins1112, the EQ view's strip: [A][B] > [POST EQ] > the OUT box (a voice) / the return (a bus). Small slot boxes,
// because what this view is about is the stage after them.
CORE_COLD void eq_strip(Canvas& c, int ch) {
  for (int s = 0; s < ins::kSlots; ++s) {
    Rect r{(int16_t)(s * 17), (int16_t)kStripY, 14, (int16_t)kStripH};
    bool set = ins::control.type[ch][s] != 0;
    if (set) c.fill(r, kOn);
    else c.rect(r, kOn);
    char b[4];
    sniprintf(b, sizeof b, CORE_STR("%c"), 'A' + s);
    c.put_text(kSlotFree + s, kRSmall, r.x + 7, kStripY + 4, b, kAlignCentre | (set ? kTextInvert : 0));
  }
  c.hline(34, 36, kStripY + kStripH / 2, kOn);
  Rect e{37, (int16_t)kStripY, 42, (int16_t)kStripH};
  bool on = ins::eq_on(ch);
  if (on) c.fill(e, kOn);
  else c.rect(e, kOn);
  c.put_text(kSlotFree + 2, kRSmall, e.x + 21, kStripY + 4, "POST EQ", kAlignCentre | (on ? kTextInvert : 0));
  c.hline(79, 81, kStripY + kStripH / 2, kOn);
  char b[20];
  if (ch < ins::kVoices) {
    Rect r{82, (int16_t)kStripY, 46, (int16_t)kStripH};
    c.rect(r, kOn);
    sniprintf(b, sizeof b, CORE_STR("OUT %s"), out::route_name(out::control.route[ch & 7]));
    c.put_text(kSlotFree + 3, kRSmall, r.x + 23, kStripY + 4, b, kAlignCentre);
    return;
  }
  int k = ch - ins::kBus0;
  if (k < 2) {
    bool post = fx::control.route == fx::kRouteMain || (fx::control.route == fx::kRouteSlot && fx::control.knob[k][1] >= 64);
    sniprintf(b, sizeof b, CORE_STR("FX%d %s"), k + 1, post ? "POST" : "PRE");
  } else {
    sniprintf(b, sizeof b, CORE_STR("B3 %s"), ins::control.route3 ? "POST" : "PRE");
  }
  c.put_text(kSlotFree + 3, kRSmall, 127, kStripY + 4, b, kAlignRight);
}

// ins1112: the POST EQ of the chain the view is on (the voice, or bus G in the BUS context)
CORE_COLD void eq_draw(Canvas& c, int page, int npages) {
  int ch = chain_now();
  char right[16];
  if (ip.bus) sniprintf(right, sizeof right, CORE_STR("BUS %d"), ip.busel + 1);
  else sniprintf(right, sizeof right, CORE_STR("%s"), voice_name(voice_now()));
  header(c, "INSERT EQ", page, npages, right);   // (the right text says BUS n / the voice: "INSERT BUS EQ" ran into it)
  eq_strip(c, ch);
  const ins::Labels& L = ins::eq_labels();
  char lab[8][6], val[8][8];
  for (int k = 0; k < ins::kEqParams; ++k) {
    sniprintf(lab[k], 6, "%s", L.label[k]);
    ins::eq_value_text(k, ins::eq_param(ch, k), val[k], 8);
  }
  for (int e = ins::kEqParams; e < 8; ++e) sniprintf(lab[e], 6, "-"), sniprintf(val[e], 8, "-");
  sniprintf(lab[6], 6, "BUS");
  sniprintf(val[6], 8, CORE_STR("%d"), ip.busel + 1);
  for (int e = 0; e < 8; ++e) {
    int x = (e & 3) * kCellW + kCellW / 2, y = e < 4 ? kRow0 : kRow1;
    c.put_text(kSlotLabels + e, kRSmall, x, y, lab[e], kAlignCentre);
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, val[e], kAlignCentre);
  }
  c.hline(0, kW - 1, kRow1 - 2, kDither);
  int e = ip.touched;
  char b[kTextLen + 1];
  if (e >= 0 && e < ins::kEqParams) {
    sniprintf(b, sizeof b, CORE_STR("%s EQ %s:%s"), ch < ins::kVoices ? voice_name(ch) : ins::chain_name(ch), lab[e], val[e]);
    footer(c, b, ins::eq_param(ch, e));
  } else if (ip.bus && ip.busel < 2) {
    footer(c, "ON THE SEND FX RETURN");
  } else {
    footer(c, "LEFT: BACK  YES: BUS");
  }
}

CORE_COLD void ins_draw(Canvas& c, int page, int npages, uint32_t ms) {
  (void)ms;
  c.clear();
  if (ip.eq) return eq_draw(c, page, npages);   // ins1112 (RIGHT)
  if (ip.out) return out_draw(c, page, npages);
  int ch = chain_now(), t = ins::control.type[ch][ip.slot];
  const ins::Labels& L = ins::labels(t);
  char right[16];
  int tr = track_now();
  if (ip.bus) sniprintf(right, sizeof right, CORE_STR("BUS %d"), ip.busel + 1);
  else sniprintf(right, sizeof right, CORE_STR("%s"), voice_name(voice_now()));
  header(c, ip.bus ? "INSERT BUS" : "INSERT", page, npages, right);
  strip(c, ch);
  char lab[8][6], val[8][8];
  for (int k = 0; k < 4; ++k) {
    sniprintf(lab[k], 6, "%s", t ? L.label[k] : "-");
    if (t) ins::slot_value_text(ch, ip.slot, k, ins::param(ch, ip.slot, k), val[k], 8);
    else sniprintf(val[k], 8, "-");
  }
  sniprintf(lab[4], 6, "SLOT");
  sniprintf(val[4], 8, "%c", 'A' + ip.slot);
  sniprintf(lab[5], 6, "TYPE");
  sniprintf(val[5], 8, "%.7s", ins::type_name(t));
  sniprintf(lab[6], 6, "BUS");
  sniprintf(val[6], 8, CORE_STR("%d"), ip.busel + 1);
  if (!ip.bus) {
    sniprintf(lab[7], 6, "SEND");
    sniprintf(val[7], 8, CORE_STR("%d"), ins::control.send[ch][ip.busel]);
  } else {
    sniprintf(lab[7], 6, "ROUT");
    if (ip.busel == 2) sniprintf(val[7], 8, "%s", ins::control.route3 ? "POST" : "PRE");
    else sniprintf(val[7], 8, CORE_STR("FX%d P1"), ip.busel + 1);
  }
  for (int e = 0; e < 8; ++e) {
    int x = (e & 3) * kCellW + kCellW / 2, y = e < 4 ? kRow0 : kRow1;
    c.put_text(kSlotLabels + e, kRSmall, x, y, lab[e], kAlignCentre);
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, val[e], kAlignCentre);
  }
  c.hline(0, kW - 1, kRow1 - 2, kDither);
  char b[kTextLen + 1];
  int e = ip.touched;
  if (e >= 0 && e < 4 && t) {
    char v[8];
    ins::slot_value_text(ch, ip.slot, e, ins::param(ch, ip.slot, e), v, sizeof v);
    sniprintf(b, sizeof b, CORE_STR("%s %c %s:%s"), ch < ins::kVoices ? voice_name(ch) : ins::chain_name(ch), 'A' + ip.slot, L.label[e], v);
    footer(c, b, ins::param(ch, ip.slot, e));
  } else if (e == 7 && !ip.bus) {
    sniprintf(b, sizeof b, CORE_STR("%s SEND BUS %d:%d"), voice_name(ch), ip.busel + 1, ins::control.send[ch][ip.busel]);
    footer(c, b, ins::control.send[ch][ip.busel]);
  } else if (!ip.bus && remote_track_machine(tr) == 0x7F) {
    footer(c, "ANALOG TRACK: NO SIDECAR VOICE");
  } else {
    footer(c, ip.bus ? "NO: TRACK  RIGHT: EQ" : "YES: BUSES  RIGHT: EQ");
  }
}

CORE_COLD void ins_enc(int e, int d, bool func) {
  if (!d) return;
  ip.touched = e;
  if (ip.eq) {   // ins1112: A HPF, B LPF, C LOW, D HIGH; G the bus (FUNC + G another track, as elsewhere)
    int ch = chain_now();
    if (e < ins::kEqParams) ins::set_eq(ch, e, ins::eq_param(ch, e) + d * (func ? 8 : 1));
    else if (e == 6) {
      if (func && !ip.bus) ip.track = clampi(track_now() + d, 0, 11), ip.manual = true;
      else ip.busel = clampi(ip.busel + d, 0, ins::kBuses - 1);
    }
    return;
  }
  if (ip.out) {   // A OUT, B LEV, C PAN, D WID, E DEC (DSP SAMPLE)
    int vi = voice_now();
    if (e == 0) out::set_route(vi, clampi(out::control.route[vi] + d, 0, out::kRoutes - 1));
    else if (e <= 3 || (e == 4 && dsp_sample_now())) out::set_param(vi, e - 1, out::param(vi, e - 1) + d * (func ? 8 : 1));
    return;
  }
  int ch = chain_now(), t = ins::control.type[ch][ip.slot];
  if (e < 4) {
    if (t) ins::set_param(ch, ip.slot, e, ins::param(ch, ip.slot, e) + d * (func ? 8 : 1));
    return;
  }
  switch (e) {
    case 4: ip.slot = clampi(ip.slot + d, 0, ins::kSlots - 1); break;
    case 5: ins::set_type(ch, ip.slot, clampi(t + d, 0, ins::kTypes - 1)); break;
    case 6:
      if (func && !ip.bus) {   // FUNC + G: another track
        ip.track = clampi(track_now() + d, 0, 11), ip.manual = true;
      } else {
        ip.busel = clampi(ip.busel + d, 0, ins::kBuses - 1);
      }
      break;
    case 7:
      if (!ip.bus) ins::set_send(ch, ip.busel, ins::control.send[ch][ip.busel] + d * (func ? 8 : 1));
      else if (ip.busel == 2) ins::set_route3(d > 0);
      break;
    default: break;
  }
}

CORE_COLD bool ins_key(int code) {
  if (code == 0x41 && !ip.eq) {   // ins1112 RIGHT: the EQ view (RIGHT again is the page list's, as always)
    ip.eq = true, ip.out = false, ip.touched = -1;
    return true;
  }
  if (code == 0x40 && ip.eq) {   // LEFT: back out of the EQ view (LEFT from the others is the page list's)
    ip.eq = false, ip.touched = -1;
    return true;
  }
  if (code == 0x44 && ip.eq) {   // YES on the EQ view: the voice's EQ <-> the bus's
    ip.bus = !ip.bus, ip.touched = -1;
    return true;
  }
  if (code == 0x44) {   // YES: TRACK -> BUS -> OUT -> TRACK
    if (ip.out) ip.out = false;
    else if (ip.bus) ip.bus = false, ip.out = true;
    else ip.bus = true;
    ip.touched = -1;
    return true;
  }
  if (code == 0x45 && (ip.bus || ip.out || ip.eq)) {   // NO: back to the TRACK view (then SIDECAR, as every page)
    ip.bus = ip.out = ip.eq = false, ip.touched = -1;
    return true;
  }
  return false;
}

// in the page list once the Rytm has named a track on a Sidecar machine (its 33 / the voices' reports), or a chain is set
CORE_COLD bool ins_shown() {
  if (ins::any_on()) return true;
  for (int t = 0; t < 12; ++t) {
    int vb = remote_track_machine(t);
    if (vb >= 0 && vb != 0x7F) return true;
  }
  return false;
}

}  // namespace

// rui1047, UP + AMP (30 page 3): the OUT view of the Rytm's selected track, not a track picked by FUNC + G before
CORE_COLD void ins_open_out() {
  ip.bus = false, ip.out = true, ip.eq = false, ip.track = -1, ip.manual = false, ip.touched = -1;
}

UI_ROM const ExtraPage kInsPage = {"INSERT", ins_shown, ins_draw, ins_enc, ins_key, 0xFF, false, nullptr, nullptr, nullptr};

#if CORE_UI_TOOLS
void ins_page_set(bool bus, int slot, int busel, int track, bool out, bool eq) {   // tests and the renderer
  ip.bus = bus, ip.out = out, ip.eq = eq, ip.slot = slot, ip.busel = busel, ip.track = track, ip.manual = track >= 0,
  ip.touched = -1;
}
#endif
#endif

}  // namespace ui
}  // namespace core
