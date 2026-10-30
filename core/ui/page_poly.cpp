// core/ui/page_poly.cpp - see extra.h: the POLY CONFIG page on the Rytm's screen (docs/SEQ-IDEAS.md E0, docs/SEQUENCER.md
// "E0 on the Rytm"). Cold. CORE_POLY builds only.
//
// A track's poly config (seq.h PolyCfg, the Analog Four MKII's POLY CONFIG words): A VOICES (AUTO, 1..8), B ALLOCATION
// (REASSIGN / ROTATE / RESET), C UNISON (OFF, 2..8), D DETUNE and E SPREAD (0..127; FUNC: by 8), F GROUP (OFF, 1..4:
// stored, not played yet). The line under the title is what it resolves to on the track's machine now, as the
// touchscreen's POLY tool says it (inst::describe_short's words, the machine left out for the Rytm's 25 characters:
// "4 NOTES 4 INST 2.92%", CUT when the budget or the machine's maximum cut it). Every edit is the console's
// `seq=poly:T:<key>=<value>` (seq::command): the pattern's config and the track's default, as there.
// Opened by the device's page request "POLY CONFIG of track t" (the Rytm: its track menu's POLY CONFIG, FUNC + CHRO,
// 30 page 0x20 + t, rytm-mods 0026 rui1048; ui/page_keys.h) or from the SIDECAR MENU (the selected track). NO: back to
// the stock screen when the track menu opened it (one level up is that menu's place), else to SIDECAR as every page.
// A Sidecar track that is no poly track yet: the config shown, YES makes it one (as the SEQ page's make prompt's YES);
// an analog track: why not (the analog voice is mono hardware). Off the LEFT / RIGHT ring (ExtraPage::off_ring).
#if CORE_POLY
#include <string.h>

#include "inst.h"
#include "inst_alloc.h"
#include "machines.h"
#include "seq/seq.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

UI_ROM const char* const kTrackNames[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
UI_ROM const char* const kLab[8] = {"VOIC", "ALLC", "UNIS", "DTUN", "SPRD", "GRP", "-", "-"};
UI_ROM const char* const kLong[6] = {"VOICES", "ALLOCATION", "UNISON", "DETUNE", "SPREAD", "GROUP"};
UI_ROM const char* const kAllocShort[seq::kAllocs] = {"REASGN", "ROTATE", "RESET"};
constexpr int kInfoY = 12, kRow0 = 28, kRow1 = 43;   // the info line; the encoder rows' label rows (values 6 below)

struct PolyPage {
  bool on;           // asked for (the page request or the SIDECAR MENU) and not left
  bool from_menu;    // the device's track menu opened it: NO goes back to the stock screen
  int req_track;     // the page request's track (-1: the Rytm's selection)
  uint32_t req_gen;  // the selection count at the request: a new selection after it wins
  int touched;
  uint32_t edits, makes;
};
CORE_RAM2 PolyPage pp = {false, false, -1, 0, -1, 0, 0};

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

CORE_COLD int track_now() {
  uint32_t g = 0;
  int t = remote_selected_track(&g);
  if (pp.req_track >= 0 && (g == pp.req_gen || t < 0 || t > 11)) return pp.req_track;
  pp.req_track = -1;
  return t >= 0 && t < 12 ? t : 0;
}
// the track's machine: 0 none known, 1 analog, 2 a Sidecar machine (*m its voice byte & 0x78)
CORE_COLD int kind_of(int t, int* m) {
  int vb = remote_track_machine(t);
  *m = vb >= 0 && vb != 0x7F ? vb & 0x78 : -1;
  return vb < 0 ? 0 : vb == 0x7F ? 1 : 2;
}
CORE_COLD bool own_pool(int m) {
  const inst::Decl* d = m >= 0 ? inst::builtin(m) : nullptr;
  return d && d->own_pool;
}
CORE_COLD void value_text(const seq::PolyCfg& c, int e, char* b, int cap) {
  switch (e) {
    case 0: c.voices ? sniprintf(b, cap, CORE_STR("%d"), c.voices) : sniprintf(b, cap, "AUTO"); break;
    case 1: sniprintf(b, cap, "%s", kAllocShort[c.alloc < seq::kAllocs ? c.alloc : 0]); break;
    case 2: c.unison > 1 ? sniprintf(b, cap, CORE_STR("%d"), c.unison) : sniprintf(b, cap, "OFF"); break;
    case 3: sniprintf(b, cap, CORE_STR("%d"), c.detune); break;
    case 4: sniprintf(b, cap, CORE_STR("%d"), c.spread); break;
    case 5: c.group ? sniprintf(b, cap, CORE_STR("%d"), c.group) : sniprintf(b, cap, "OFF"); break;
    default: sniprintf(b, cap, "-"); break;
  }
}

// An analog track: the Rytm's own voice is mono hardware. (E0b FREEZE, not built: this is where "YES: FREEZE TO POLY"
// goes - the auto-sampler on this track into a MULTISAMPLE poly track, docs/SEQ-IDEAS.md E0b.)
CORE_COLD void analog_draw(Canvas& c, int t) {
  char b[kTextLen + 1];
  sniprintf(b, sizeof b, CORE_STR("%s PLAYS ANALOG:"), kTrackNames[t]);
  c.put_text(kSlotFree, kRSmall, 64, 16, b, kAlignCentre);
  c.put_text(kSlotFree + 1, kRSmall, 64, 23, "THE RYTM'S VOICE IS MONO", kAlignCentre);
  c.put_text(kSlotFree + 2, kRSmall, 64, 33, "FOR A POLY TRACK PICK A", kAlignCentre);
  c.put_text(kSlotFree + 3, kRSmall, 64, 40, "SIDECAR MACHINE ON SRC", kAlignCentre);
  footer(c, "NO: BACK");
}

CORE_COLD void poly_draw(Canvas& c, int page, int npages, uint32_t ms) {
  (void)ms;
  c.clear();
  int t = track_now(), m = -1, k = kind_of(t, &m);
  header(c, "POLY CONFIG", page, npages, kTrackNames[t]);
  if (k == 1) return analog_draw(c, t);
  if (k == 0) {
    c.put_text(kSlotFree, kRSmall, 64, 28, "NO MACHINE REPORTED YET", kAlignCentre);
    footer(c, "NO: BACK");
    return;
  }
  bool poly = seq::owns_track(t);
  seq::PolyCfg cfg = seq::poly_of(t);
  char info[48];
  inst::Resolved rs = {};
  int rm = -1;
  if (!poly) sniprintf(info, sizeof info, CORE_STR("%s: NOT A POLY TRACK"), machine_short(m));
  else if (inst::resolve_for(remote_track_voice(t), cfg, &rs, &rm))   // (describe_short's words without the machine:
    sniprintf(info, sizeof info, CORE_STR("%d NOTE%s %d INST %d.%02d%%%s"), rs.lanes, rs.lanes == 1 ? "" : "S",   // the
              rs.instances, rs.cpu100 / 100, rs.cpu100 % 100, rs.clamped ? " CUT" : "");   // Rytm's line has 25 characters)
  else sniprintf(info, sizeof info, "%s", machine_long(m));
  c.put_text(kSlotFree, kRSmall, 64, kInfoY, info, kAlignCentre);
  bool pool = own_pool(m);
  for (int e = 0; e < 8; ++e) {
    char v[8];
    value_text(cfg, e, v, sizeof v);
    if (pool && e >= 1 && e <= 4) sniprintf(v, sizeof v, "-");   // the machine's own pool: VOICES only (POLYPHONY.md 7)
    int x = (e & 3) * kCellW + kCellW / 2, y = e < 4 ? kRow0 : kRow1;
    bool on = e == pp.touched;
    if (on) c.fill(Rect{(int16_t)(x - 15), (int16_t)(y - 1), 30, 7}, kOn);
    c.put_text(kSlotLabels + e, kRSmall, x, y, kLab[e], kAlignCentre | (on ? kTextInvert : 0));
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, v, kAlignCentre);
  }
  c.hline(0, kW - 1, kRow1 - 2, kDither);
  char b[kTextLen + 1];
  int e = pp.touched;
  if (!poly) {
    footer(c, "YES: MAKE IT A POLY TRACK");
  } else if (e >= 0 && e < 6) {
    char v[12];
    if (e == 1) sniprintf(v, sizeof v, "%s", inst::alloc_name(cfg.alloc));
    else value_text(cfg, e, v, sizeof v);
    if (pool && e >= 1 && e <= 4) sniprintf(b, sizeof b, CORE_STR("%s: VOICES ONLY"), machine_short(m));
    else if (e == 5) sniprintf(b, sizeof b, CORE_STR("GROUP %s: STORED ONLY"), v);
    else sniprintf(b, sizeof b, CORE_STR("%s %s:%s"), kTrackNames[t], kLong[e], v);
    footer(c, b);
  } else {
    footer(c, "A..F: SET  NO: BACK");
  }
}

CORE_COLD void apply(int t, const char* key, int v) {
  char cmd[40], out[160];
  sniprintf(cmd, sizeof cmd, CORE_STR("seq=poly:%d:%s=%d"), t + 1, key, v);
  seq::command(cmd, out, sizeof out);
  ++pp.edits;
}

CORE_COLD void poly_enc(int e, int d, bool func) {
  if (!d || e < 0 || e > 5) return;
  int t = track_now(), m = -1;
  if (kind_of(t, &m) != 2 || !seq::owns_track(t)) return;   // (no config to edit: the page says why)
  pp.touched = e;
  if (own_pool(m) && e >= 1 && e <= 4) return;   // the machine's own pool: VOICES only (the page shows "-")
  seq::PolyCfg c = seq::poly_of(t);
  int step = d > 0 ? 1 : -1;
  switch (e) {
    case 0: apply(t, "voices", clampi(c.voices + step, 0, seq::kMaxVoicesCfg)); break;
    case 1: apply(t, "alloc", clampi(c.alloc + step, 0, seq::kAllocs - 1)); break;
    case 2: apply(t, "unison", clampi((c.unison > 1 ? c.unison : 1) + step, 1, seq::kMaxUnison)); break;
    case 3: apply(t, "detune", clampi(c.detune + d * (func ? 8 : 1), 0, 127)); break;
    case 4: apply(t, "spread", clampi(c.spread + d * (func ? 8 : 1), 0, 127)); break;
    default: apply(t, "group", clampi(c.group + step, 0, seq::kPolyGroups)); break;
  }
}

CORE_COLD bool poly_key(int code) {
  if (code == 0x44) {   // YES: a Sidecar track that is no poly track yet becomes one (the make prompt's YES)
    int t = track_now(), m = -1;
    if (kind_of(t, &m) == 2 && !seq::owns_track(t)) {
      char cmd[24], out[96];
      sniprintf(cmd, sizeof cmd, CORE_STR("seq=track:%d:on"), t + 1);
      seq::command(cmd, out, sizeof out);
      ++pp.makes;
    }
    return true;
  }
  if (code == 0x42) return true;   // UP: the Sidecar's modifier, no page arrow (fix1049)
  // LEFT RIGHT DOWN PAGE: nothing (ux1027: the page replaces the track menu's screen, no ring siblings; they moved on
  // to the ring's next page before)
  if (code == 0x40 || code == 0x41 || code == 0x43 || code == 0x84) return true;
  if (code == 0x45) {   // NO: the track menu's page goes back to the stock screen; else SIDECAR (remote.cpp)
    pp.on = false, pp.touched = -1;
    if (pp.from_menu) {
      pp.from_menu = false;
      remote_request(false);
      return true;
    }
  }
  return false;
}

// In the SIDECAR MENU once a track plays a Sidecar machine; on the glass only while it is the page shown (off the ring)
CORE_COLD bool poly_shown() {
  if (pp.on) return true;
  for (int t = 0; t < 12; ++t) {
    int vb = remote_track_machine(t);
    if (vb >= 0 && vb != 0x7F) return true;
  }
  return false;
}

}  // namespace

UI_ROM const ExtraPage kPolyPage = {"POLY", poly_shown, poly_draw, poly_enc, poly_key, 0x3F, false, nullptr, nullptr, nullptr,
                                    true};

CORE_COLD void poly_page_open(int track, bool from_menu) {
  uint32_t g = 0;
  remote_selected_track(&g);
  pp.on = true, pp.from_menu = from_menu, pp.touched = -1;
  pp.req_track = track >= 0 && track < 12 ? track : -1, pp.req_gen = g;
}
CORE_COLD void poly_page_closed() { pp.on = false, pp.from_menu = false, pp.touched = -1; }
CORE_COLD int poly_page_track() { return track_now(); }

}  // namespace ui
}  // namespace core
#endif
