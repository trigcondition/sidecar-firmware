// core/ui/page_chop.cpp - CHOP on the Rytm's screen (docs/CHOP.md "On the Rytm"; core/chop/chop.h). Cold. Pi and Mac
// (CORE_POLY && CORE_MSAMP builds).
//
// The SRC view (the Rytm's SRC page on a CHOP track): the sample's outline with its slices, the slice SLC plays
// inverted, the playheads; P1..P8 SRC SLC TUNE MRPH / SEED NBR VEL LEV. A push of encoder B..H there opens the editor.
//
// The editor (page "CHOP", off the ring; the SIDECAR MENU's CHOP EDITOR, or a push on the SRC view), the Octatrack's
// slice editor as the model (its behaviour from its manual, none of its code): the outline of the selected slice and its
// neighbours, the markers, the selection, the playheads. Three views, PAGE steps through them:
//   EDIT     A SLC (the slice)  B STRT  C END (FUNC: a frame a step)  D ZOOM (ALL NEAR SLICE START END)
//            E GRID (2..64)  F SENS (TRANSIENT's)  G LAT (LAZY's latency)  H RSMP (DRY / +FX)
//            pushes: A ADD (a marker at the view's middle)  B DEL  C MERGE (with the next)  D UNDO
//                    E GRID  F TRANSIENT  G LAZY (arm; again: STOP)  H RESAMPLE the slice
//   SLICE 1  A PTCH B FINE C GAIN D REV / E ATK F DEC G LOOP H CHOK      (the selected slice's own; a push auditions)
//   SLICE 2  A NUDG B STUT C SLEN D SDEC / E TAPE F TTIM G NBR H RSET (push: SHUFFLE back to the map's order)
// Keys: YES auditions the slice; LEFT / RIGHT the slice before / after; T1..T16 select and audition the slices of the
// trig bank (DOWN: the next bank of 16); NO leaves. LAZY armed: the pads, the trigs and T1..T16 drop markers, push D
// takes the last back, YES keeps them (STOP), NO drops them. A RESAMPLE's offer: YES replaces the slice, NO keeps it.
#if CORE_POLY && CORE_MSAMP
#include <string.h>

#include "chop/chop.h"
#include "machines.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"
#include "voices.h"

namespace core {
namespace ui {

namespace {

UI_ROM const char* const kTrackNames[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
UI_ROM const char* const kZoomNames[5] = {"ALL", "NEAR", "SLICE", "START", "END"};
UI_ROM const char* const kViewNames[3] = {"EDIT", "SLICE 1", "SLICE 2"};
UI_ROM const char* const kEditLab[8] = {"SLC", "STRT", "END", "ZOOM", "GRID", "SENS", "LAT", "RSMP"};
UI_ROM const int kFields1[8] = {chop::kFPitch, chop::kFFine, chop::kFGain, chop::kFRev,
                                chop::kFAtk, chop::kFDec, chop::kFLoop, chop::kFChoke};
UI_ROM const int kFields2[8] = {chop::kFNudge, chop::kFStut, chop::kFSlen, chop::kFSdec,
                                chop::kFTape, chop::kFTms, chop::kFNbr, -1};
constexpr int kWaveY = 10, kWaveH = 20, kRow0 = 31, kRow1 = 44;
constexpr uint32_t kMsgMs = 2500;

struct Ed {
  bool on;
  int track;            // the Rytm track the page was opened for (-1 the selected one)
  int sel, view, zoom, grid, sens, bank, touched;
  bool fx;
  char msg[28];
  uint32_t msg_ms;
  uint32_t edits;
};
CORE_RAM2 Ed ed = {false, -1, 0, 0, 1, 16, 64, 0, -1, false, "", 0, 0};

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
CORE_COLD void say(const char* m) {
  sniprintf(ed.msg, sizeof ed.msg, "%s", m);
  ed.msg_ms = now_ms() | 1;
}
// the voice the editor edits: the page's track's (or the Rytm's selected track's) when it plays CHOP, else the first
// voice that does; -1 none
CORE_COLD bool chop_track(int t) {
  int vb = t >= 0 ? remote_track_machine(t) : -1;
  return vb >= 0 && vb != 0x7F && (vb & 0x78) == kChopMachine;
}
CORE_COLD int ed_voice(int* track = nullptr) {
  int t = ed.track >= 0 ? ed.track : remote_selected_track(nullptr);
  if (t >= 0 && t < 12 && (chop_track(t) || chop::forced(remote_track_voice(t)))) {
    if (track) *track = t;
    return remote_track_voice(t);
  }
  for (t = 0; t < 12; ++t)
    if (chop_track(t)) {
      if (track) *track = t;
      return remote_track_voice(t);
    }
  for (int vi = 0; vi < kVoices; ++vi)
    if (v[vi].cp || chop::forced(vi)) {
      if (track) *track = -1;
      return vi;
    }
  return -1;
}
CORE_COLD void secs_text(uint32_t frames, uint32_t rate, char* b, int cap) {
  uint32_t ms = (uint32_t)((uint64_t)frames * 1000 / (rate ? rate : 48000));
  if (ms < 10000) sniprintf(b, cap, CORE_STR("%lu.%03lu"), (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  else sniprintf(b, cap, CORE_STR("%lu.%02lu"), (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
}
// the frames the outline shows for the selected slice at the zoom
CORE_COLD void view_span(const chop::Map& m, int k, int zoom, uint32_t* a, uint32_t* b) {
  uint32_t fr = m.frames ? m.frames : 1;
  const chop::Slice& s = m.s[k];
  uint32_t len = s.end > s.start ? s.end - s.start : 1;
  switch (zoom) {
    case 0: *a = 0, *b = fr; break;
    case 1:   // the neighbours
      *a = k > 0 ? m.s[k - 1].start : s.start;
      *b = k + 1 < m.n ? m.s[k + 1].end : s.end;
      break;
    case 2: *a = s.start, *b = s.end; break;
    case 3: {   // around the start: 10 ms either way (FUNC's frames visible)
      uint32_t w = (m.rate ? m.rate : 48000) / 100;
      *a = s.start > w ? s.start - w : 0, *b = s.start + w;
      break;
    }
    default: {
      uint32_t w = (m.rate ? m.rate : 48000) / 100;
      *a = s.end > w ? s.end - w : 0, *b = s.end + w;
      break;
    }
  }
  (void)len;
  if (*b > fr) *b = fr;
  if (*b <= *a) *b = *a + 1;
}
// the outline of [a, b) into r with the slices' markers, the selection inverted, the playheads; markers at their x
CORE_COLD void draw_wave(Canvas& c, const Rect& r, int vi, const chop::Map& m, int sel, uint32_t a, uint32_t b, bool sel_box) {
  float mn[128], mx[128];
  int cols = r.w > 128 ? 128 : r.w;
  int n = chop::outline(vi, a, b, cols, mn, mx);
  int mid = r.y + r.h / 2, half = r.h / 2 - 1;
  c.hline(r.x, r.x + r.w - 1, mid, kDither);
  for (int x = 0; x < n; ++x) {
    int y0 = mid - (int)(mx[x] * (float)half * 1.15f), y1 = mid - (int)(mn[x] * (float)half * 1.15f);
    y0 = clampi(y0, r.y, r.y + r.h - 1), y1 = clampi(y1, r.y, r.y + r.h - 1);
    c.vline(r.x + x, y0, y1, kOn);
  }
  double span = (double)(b - a);
  auto xof = [&](uint32_t f) { return r.x + (int)((double)((int64_t)f - (int64_t)a) * (double)(cols - 1) / span); };
  for (int k = 0; k < m.n; ++k) {   // the markers: a slice's start solid on top, dotted down
    const chop::Slice& s = m.s[k];
    if (s.alt >= 0) continue;
    if (s.start >= a && s.start <= b) {
      int x = xof(s.start);
      c.vline(x, r.y, r.y + 2, kOn);
      c.vline(x, r.y + 3, r.y + r.h - 1, kDither);
    }
  }
  if (sel_box && sel >= 0 && sel < m.n && m.s[sel].alt < 0) {   // the selection, inverted
    int x0 = clampi(xof(m.s[sel].start), r.x, r.x + cols - 1), x1 = clampi(xof(m.s[sel].end), r.x, r.x + cols - 1);
    if (x1 > x0) c.invert(Rect{(int16_t)x0, r.y, (int16_t)(x1 - x0 + 1), r.h});
  }
  int32_t ph[chop::kPerVoice];
  int np = chop::playheads(vi, ph, chop::kPerVoice);
  for (int i = 0; i < np; ++i)
    if ((uint32_t)ph[i] >= a && (uint32_t)ph[i] < b) c.vline(xof((uint32_t)ph[i]), r.y, r.y + r.h - 1, kXor);
}

// ---- the SRC view's picture (kDrawChop)
CORE_COLD uint32_t src_key(const Cell& cell, const Source& s) {
  int vi = s.voice() < 0 ? 0 : s.voice() & 7;
  int32_t ph[chop::kPerVoice];
  int np = chop::playheads(vi, ph, chop::kPerVoice);
  uint32_t h = chop::map_gen(vi) * 2654435761u ^ (uint32_t)s.param(1) << 3 ^ (uint32_t)chop::sample_ready(vi) << 1;
  for (int i = 0; i < np; ++i) h = (h ^ (uint32_t)(ph[i] >> 10)) * 16777619u;   // (moving playheads: ~5 px steps)
  (void)cell;
  return h | 1;
}
CORE_COLD void src_draw(Canvas& c, const Rect& r, const Cell& cell, const Source& s, int slot) {
  (void)cell;
  int vi = s.voice() < 0 ? 0 : s.voice() & 7;
  const chop::Map* m = chop::map_of(vi);
  Rect w = {r.x, (int16_t)(r.y + 7), r.w, 10};   // (the labels under it from r.y + 17: kLabelDy)
  if (!m || !chop::sample_ready(vi) || !m->n) {
    c.put_text(slot, kRSmall, r.x + r.w / 2, r.y + 6, m ? "LOADING" : "NO SAMPLE", kAlignCentre);
    return;
  }
  int sel = clampi(eng_zone((float)s.param(1), 64), 0, 64) % m->n;
  draw_wave(c, w, vi, *m, sel, 0, m->frames, true);
  char b[kTextLen + 1];
  sniprintf(b, sizeof b, CORE_STR("SLICE %d/%d %.14s"), sel + 1, m->n, m->name);
  c.put_text(slot, kRSmall, r.x + 2, r.y + 1, b, kAlignLeft);
}

// ---- the editor
CORE_COLD void field_lab(int f, char* b, int cap) { sniprintf(b, cap, "%s", f >= 0 ? chop::field_name(f) : "RSET"); }
CORE_COLD void ed_draw(Canvas& c, int page, int npages, uint32_t ms) {
  (void)ms;
  c.clear();
  int t = -1, vi = ed_voice(&t);
  char right[12];
  sniprintf(right, sizeof right, "%s", t >= 0 ? kTrackNames[t] : vi >= 0 ? "V" : "");
  if (vi >= 0 && t < 0) sniprintf(right, sizeof right, CORE_STR("V%d"), vi + 1);
  const chop::Map* m = vi >= 0 ? chop::map_of(vi) : nullptr;
  char title[16];
  sniprintf(title, sizeof title, CORE_STR("CHOP %s"), kViewNames[ed.view]);
  header(c, title, page, npages, right);
  if (vi < 0) {
    c.put_text(kSlotFree, kRSmall, 64, 22, "NO CHOP TRACK", kAlignCentre);
    c.put_text(kSlotFree + 1, kRSmall, 64, 30, "PICK CHOP IN A TRACK'S", kAlignCentre);
    c.put_text(kSlotFree + 2, kRSmall, 64, 37, "MACHINE LIST (SRC SRC)", kAlignCentre);
    footer(c, "NO: BACK");
    return;
  }
  if (!m || !m->n || !chop::sample_ready(vi)) {
    c.put_text(kSlotFree, kRSmall, 64, 26, m ? "LOADING THE SAMPLE" : "NO SAMPLE ON SRC", kAlignCentre);
    footer(c, "NO: BACK");
    return;
  }
  ed.sel = clampi(ed.sel, 0, m->n - 1);
  const chop::Slice& s = m->s[ed.sel];
  bool lazy = chop::lazy_on(vi);
  uint32_t a, b;
  if (lazy) a = 0, b = m->frames;
  else view_span(*m, ed.sel, ed.zoom, &a, &b);
  Rect wr = {0, kWaveY, kW, kWaveH};
  draw_wave(c, wr, vi, *m, ed.sel, a, b, !lazy);
  if (lazy) {   // the markers dropped so far, solid
    const chop::Lazy* l = chop::lazy_state(vi);
    for (int i = 0; l && i < l->n; ++i) {
      int x = (int)((double)l->at[i] * (kW - 1) / (double)(m->frames ? m->frames : 1));
      c.vline(x, kWaveY, kWaveY + kWaveH - 1, kOn);
    }
  }
  char v[8][10];
  char lab[8][6];
  for (int e = 0; e < 8; ++e) lab[e][0] = v[e][0] = 0;
  if (ed.view == 0) {
    for (int e = 0; e < 8; ++e) sniprintf(lab[e], sizeof lab[e], "%s", kEditLab[e]);
    sniprintf(v[0], sizeof v[0], CORE_STR("%d/%d"), ed.sel + 1, m->n);
    secs_text(s.start, m->rate, v[1], sizeof v[1]);
    secs_text(s.end, m->rate, v[2], sizeof v[2]);
    sniprintf(v[3], sizeof v[3], "%s", kZoomNames[ed.zoom]);
    sniprintf(v[4], sizeof v[4], CORE_STR("%d"), ed.grid);
    sniprintf(v[5], sizeof v[5], CORE_STR("%d"), ed.sens);
    int lat10 = chop::latency() * 10 / 48;
    sniprintf(v[6], sizeof v[6], CORE_STR("%d.%dMS"), lat10 / 10, lat10 % 10);
    sniprintf(v[7], sizeof v[7], "%s", ed.fx ? "+FX" : "DRY");
  } else {
    const int* fs = ed.view == 1 ? kFields1 : kFields2;
    for (int e = 0; e < 8; ++e) {
      field_lab(fs[e], lab[e], sizeof lab[e]);
      if (fs[e] >= 0) chop::field_text(s, fs[e], m->rate, v[e], sizeof v[e]);
      else sniprintf(v[e], sizeof v[e], "PUSH");
    }
  }
  for (int e = 0; e < 8; ++e) {
    int x = (e & 3) * kCellW + kCellW / 2, y = e < 4 ? kRow0 : kRow1;
    bool on = e == ed.touched;
    if (on) c.fill(Rect{(int16_t)(x - 15), (int16_t)(y - 1), 30, 7}, kOn);
    c.put_text(kSlotLabels + e, kRSmall, x, y, lab[e], kAlignCentre | (on ? kTextInvert : 0));
    c.put_text(kSlotCells + e, kRSmall, x, y + 6, v[e], kAlignCentre);
  }
  c.hline(0, kW - 1, kRow1 - 2, kDither);
  // the bottom bar: a message, the LAZY state, a RESAMPLE's offer, else the slice's length and the keys
  char f[kTextLen + 1];
  int rk = -1;
  char row[8] = "";
  int rst = chop::resample_state(&rk, row);
  if (rst == chop::kRsOffer) sniprintf(f, sizeof f, CORE_STR("S%d -> %s: YES REPLACE"), rk + 1, row);
  else if (rst == chop::kRsPlaying || rst == chop::kRsWriting) sniprintf(f, sizeof f, CORE_STR("RESAMPLING S%d"), rk + 1);
  else if (lazy) {
    const chop::Lazy* l = chop::lazy_state(vi);
    sniprintf(f, sizeof f, CORE_STR("LAZY %d: TAP  YES KEEP"), l ? l->n : 0);
  } else if (ed.msg_ms && now_ms() - ed.msg_ms < kMsgMs) sniprintf(f, sizeof f, "%s", ed.msg);
  else if (chop::transient_busy(vi)) sniprintf(f, sizeof f, "TRANSIENT...");
  else {
    char l[12];
    secs_text(s.end - s.start, m->rate, l, sizeof l);
    sniprintf(f, sizeof f, CORE_STR("S%d %sS%s B%d"), ed.sel + 1, l, chop::map_dirty(vi) ? " *" : "", ed.bank + 1);
  }
  footer(c, f);
}
CORE_COLD void ed_audition(int vi) {
  if (vi >= 0) chop::audition(vi, ed.sel);
}
CORE_COLD void ed_enc(int e, int d, bool func) {
  if (!d || e < 0 || e > 7) return;
  int vi = ed_voice();
  const chop::Map* m = vi >= 0 ? chop::map_of(vi) : nullptr;
  if (!m || !m->n) return;
  ed.touched = e;
  ed.sel = clampi(ed.sel, 0, m->n - 1);
  const chop::Slice& s = m->s[ed.sel];
  if (ed.view == 0) {
    uint32_t a, b;
    view_span(*m, ed.sel, ed.zoom, &a, &b);
    int32_t px = (int32_t)((b - a) / 128);   // a pixel of the view a step; FUNC: one frame
    if (px < 1) px = 1;
    int32_t st = func ? d : d * px;
    switch (e) {
      case 0: ed.sel = clampi(ed.sel + d, 0, m->n - 1); break;
      case 1: chop::edit_start(vi, ed.sel, (int32_t)s.start + st), ++ed.edits; break;
      case 2: chop::edit_end(vi, ed.sel, (int32_t)s.end + st), ++ed.edits; break;
      case 3: ed.zoom = clampi(ed.zoom + (d > 0 ? 1 : -1), 0, 4); break;
      case 4: ed.grid = clampi(ed.grid + d * (func ? 1 : (ed.grid >= 16 ? 4 : 1)), 2, chop::kMaxSlices); break;
      case 5: ed.sens = clampi(ed.sens + d * (func ? 1 : 4), 0, 127); break;
      case 6: chop::set_latency(clampi(chop::latency() + d * (func ? 1 : 24), 0, 9600)); break;
      default: ed.fx = d > 0; break;
    }
    return;
  }
  const int* fs = ed.view == 1 ? kFields1 : kFields2;
  int f = fs[e];
  if (f < 0) return;
  int lo, hi;
  chop::field_range(f, &lo, &hi);
  int step = 1;
  if (!func && (f == chop::kFAtk || f == chop::kFDec || f == chop::kFTms)) step = 10;
  if (!func && f == chop::kFNudge) step = 48;   // 1 ms (FUNC: a frame)
  if (!func && (f == chop::kFSlen || f == chop::kFSdec || f == chop::kFNbr)) step = 5;
  chop::edit_field(vi, ed.sel, f, clampi(chop::field_get(s, f) + d * step, lo, hi));
  ++ed.edits;
}
CORE_COLD bool ed_key(int code) {
  int vi = ed_voice();
  const chop::Map* m = vi >= 0 ? chop::map_of(vi) : nullptr;
  bool lazy = vi >= 0 && chop::lazy_on(vi);
  int rst = chop::resample_state();
  if (code == 0x44 || code == 0x45) {   // YES / NO
    if (rst == chop::kRsOffer) {
      bool ok = chop::resample_answer(code == 0x44);   // (review: a slice changed meanwhile refuses the take)
      say(code != 0x44 ? "KEPT THE SLICE" : ok ? "REPLACED" : "SLICE CHANGED: NOT USED");
      return true;
    }
    if (lazy) {
      chop::lazy_stop(vi, code == 0x44);
      say(code == 0x44 ? "LAZY: SLICES KEPT" : "LAZY: CANCELLED");
      return true;
    }
    if (code == 0x44) {
      ed_audition(vi);
      return true;
    }
    ed.on = false, ed.track = -1, ed.touched = -1;   // NO: back to SIDECAR (remote.cpp)
    return false;
  }
  if (code == 0x42) return true;   // UP: the Sidecar's modifier
  if (code == 0x84) {              // PAGE: the next view
    ed.view = (ed.view + 1) % 3, ed.touched = -1;
    return true;
  }
  if (!m || !m->n) return code == 0x40 || code == 0x41 || code == 0x43 || (code >= 0 && code < 16);
  if (code >= 0 && code < 16) {    // T1..T16: LAZY taps, else the bank's slice selected and auditioned
    if (lazy) {
      if (chop::lazy_tap(vi) < 0) say("TAP REFUSED");
      return true;
    }
    int k = ed.bank * 16 + code;
    if (k < m->n) ed.sel = k, ed_audition(vi);
    return true;
  }
  if (code == 0x40 || code == 0x41) {   // LEFT / RIGHT: the slice before / after
    ed.sel = clampi(ed.sel + (code == 0x41 ? 1 : -1), 0, m->n - 1);
    return true;
  }
  if (code == 0x43) {   // DOWN: the trig keys' next bank of 16
    ed.bank = (ed.bank + 1) % 4;
    return true;
  }
  if (code >= 0x70 && code <= 0x77) {   // the encoders' pushes
    int e = code - 0x70;
    if (ed.view != 0) {
      const int* fs = ed.view == 1 ? kFields1 : kFields2;
      if (fs[e] < 0) {   // RSET: SHUFFLE back to the map's order (the Rytm's SEED to 0, P5)
        remote_param_write(vi, kChopMachine, 4, 0, 0);
        say("SHUFFLE: THE MAP'S ORDER");
      } else {
        ed_audition(vi);
      }
      return true;
    }
    const chop::Slice& s = m->s[clampi(ed.sel, 0, m->n - 1)];
    bool ok = true;
    switch (e) {
      case 0: {   // ADD: a marker at the view's middle (the slice split there)
        uint32_t a, b;
        view_span(*m, ed.sel, ed.zoom, &a, &b);
        uint32_t at = ed.zoom >= 3 ? s.start + (s.end - s.start) / 2 : a + (b - a) / 2;
        ok = chop::edit_add(vi, at);
        say(ok ? "MARKER ADDED" : "NO ROOM FOR A MARKER");
        break;
      }
      case 1: ok = chop::edit_del(vi, ed.sel), say(ok ? "SLICE DELETED" : "THE LAST SLICE STAYS"); break;
      case 2: ok = chop::edit_merge(vi, ed.sel), say(ok ? "MERGED" : "NO NEXT SLICE"); break;
      case 3:
        if (lazy) ok = chop::lazy_undo(vi), say(ok ? "LAZY: MARKER UNDONE" : "NO MARKER");
        else ok = chop::edit_undo(vi), say(ok ? "UNDONE" : "NOTHING TO UNDO");
        break;
      case 4: ok = chop::edit_grid(vi, ed.grid); if (ok) { char b2[20]; sniprintf(b2, sizeof b2, CORE_STR("GRID %d"), ed.grid); say(b2); } break;
      case 5: ok = chop::edit_transient(vi, ed.sens), say(ok ? "TRANSIENT..." : "NOT NOW"); break;
      case 6:
        if (lazy) chop::lazy_stop(vi, true), say("LAZY: SLICES KEPT");
        else ok = chop::lazy_arm(vi), say(ok ? "LAZY: TAP PADS / TRIGS" : "NOT NOW");
        break;
      default: ok = chop::resample(vi, ed.sel, ed.fx), say(ok ? "RESAMPLING" : "RESAMPLE: NOT NOW"); break;
    }
    if (ok) ++ed.edits;
    ed.sel = clampi(ed.sel, 0, (chop::map_of(vi) ? chop::map_of(vi)->n : 1) - 1);
    return true;
  }
  return false;
}
CORE_COLD bool ed_shown() {
  if (ed.on) return true;
  return ed_voice() >= 0;
}
CORE_COLD uint16_t ed_trigs() { return 0xFFFF; }
CORE_COLD bool ed_leds(uint8_t c[16]) {
  int vi = ed_voice();
  const chop::Map* m = vi >= 0 ? chop::map_of(vi) : nullptr;
  if (!m) return false;
  bool lazy = chop::lazy_on(vi);
  const chop::Lazy* l = lazy ? chop::lazy_state(vi) : nullptr;
  for (int k = 0; k < 16; ++k) {
    int s = ed.bank * 16 + k;
    if (lazy) c[k] = l && k < l->n ? kLedOrange : kLedBlueDim;
    else c[k] = s >= m->n ? kLedOff : s == ed.sel ? kLedOrange : kLedBlueDim;
  }
  return true;
}

}  // namespace

UI_ROM const Cell kChopSrcCells[] = {
    {kDraw, 0, 0, 4, 1, {0, 1, 3, kNone}, kDrawChop, nullptr},
    {kKnob, 0, 1, 1, 1, {4, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 1, 1, 1, 1, {5, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 2, 1, 1, 1, {6, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kChopSrcPage = {
    "CHOP", kChopMachine, sizeof kChopSrcCells / sizeof kChopSrcCells[0], kChopSrcCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"SRC", "SLC", "TUNE", "MRPH", "SEED", "NBR", "VEL", "LEV"},
};
UI_ROM const ExtraPage kChopPage = {"CHOP", ed_shown, ed_draw, ed_enc, ed_key, 0xFF, false, nullptr, ed_trigs, ed_leds, true};

CORE_COLD uint32_t chop_key(const Cell& c, const Source& s) { return src_key(c, s); }
CORE_COLD void chop_draw(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot) { src_draw(cv, r, c, s, slot); }
CORE_COLD void chop_page_open(int track) {
  ed.on = true, ed.track = track >= 0 && track < 12 ? track : -1, ed.touched = -1, ed.view = 0;
  remote_show_page("CHOP");
}
CORE_COLD bool chop_src_push(int track, int code, bool down) {
  if (!chop_track(track) || code < 0x71 || code > 0x77) return false;
  if (down) chop_page_open(track);
  return true;
}
#if CORE_UI_TOOLS
CORE_COLD void chop_page_set(int view, int sel, int zoom) {
  ed.on = true, ed.view = clampi(view, 0, 2), ed.sel = sel, ed.zoom = clampi(zoom, 0, 4), ed.touched = -1, ed.msg_ms = 0;
}
CORE_COLD int chop_page_sel() { return ed.sel; }
CORE_COLD void chop_page_reset() { ed = Ed{false, -1, 0, 0, 1, 16, 64, 0, -1, false, "", 0, 0}; }
#endif

}  // namespace ui
}  // namespace core
#endif
