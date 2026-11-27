// core/ui/page_sedit.cpp - SAMPLE EDIT on the Rytm's screen (core/samp/sedit.h holds the edit; the touchscreen shows
// the same state). Cold. Pi and Mac (CORE_POLY && CORE_MSAMP builds).
//
// Opened by a push of encoder B on a DSP SAMPLE / SAMPLER track's SRC view (the track's sample, its STA..END the region; on a
// WAVETABLE track's: table A's card file, else the POOL page's SAMPLES list to pick a sample), by the
// POOL page's EDIT on a SAMPLES row (the whole sample, no track), or by the SIDECAR MENU (the selected track's sample;
// the edit open already: back to it). Off the ring: in the page list only while it shows.
// Drawn: the header with the file's name; the window's outline in rows 10..47 (the region solid, the rest dithered, the
// start and end markers, the preview's playhead; 0 dBFS dotted when the edit goes over it); zoomed in, the window's
// place in the sample as a bar in row 48; the region's start and end in seconds and the zoom; the footer the
// function and YES, the job's progress ("NORMALISE 42%"), or a message for 2 s.
// MAKE WT in the save choice (FUNC + YES, A picks, B the frames 16 .. 256): the region as a wavetable (docs/SAMPLE-EDIT.md).
// Knobs (sedit-050 #4): A the start, B the end (on zero crossings; FUNC: the next / previous transient), C the zoom, D
// the window's place (FUNC: 8 times the step; the bar under the outline shows it), E the function (CROP NORMALISE TRIM
// FADE IN FADE OUT REVERSE GAIN +3 GAIN -3). Keys: YES does the function; FUNC + YES saves (A picks SAVE+USE / SAVE / CANCEL, YES
// confirms, NO cancels; SAVE+USE only when opened from a track); NO undoes the last apply, asks before it drops an edit
// it cannot undo, else leaves (the track back on its original sample); push A previews the region on the track (from
// the pool: on the selected DSP SAMPLE / SAMPLER track, else the first one).
#if CORE_POLY && CORE_MSAMP
#include <string.h>

#include "machines.h"
#include "samp/sedit.h"
#include "samp/sedit_dsp.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

constexpr int kYes = 0x44, kNo = 0x45, kFunc = 0x80, kPushA = 0x70, kPushD = 0x73;
// 0.5.0 screens (the UI system, AROUND): the encoders' names in row 10, the outline in rows 16..43, the window's place in
// row 46, the function's tag and the region's line in rows 48..54, the bottom bar's key boxes
constexpr int kWaveY = 16, kWaveH = 28, kPlaceY = 46, kNamesY = 10, kInfoY = 49;
constexpr int kTurnMs = 1500;
constexpr uint32_t kNoteMs = 2000;
enum Ask : uint8_t { kAskNone, kAskSave, kAskDiscard };
enum Opt : uint8_t { kOptUse, kOptSave, kOptWt, kOptCancel };
UI_ROM const char* const kOptNames[4] = {"SAVE+USE", "SAVE", "MAKE WT", "CANCEL"};

struct Pg {
  bool func;          // FUNC held
  uint8_t ask, pick;  // the question on screen, the save choice's pick (an index into opts())
  bool refused;       // the last open was refused: the page shows why until NO
  char why[32];       // that refusal
  char note[32];      // the footer's message (kNoteMs)
  uint32_t note_ms;
  char seen[32];      // the editor's msg last seen: a new one becomes the note
  int turn_e;         // the encoder turned last (-1 none): its name on the title bar and its value in the bottom bar
  uint32_t turn_ms;
  bool was_open;      // an edit open at the last refresh (its close elsewhere drops the page's question)
  bool wt_back;       // a table was made for a voice: the editor closes itself, and the page goes back to where it came from
};
Pg pg = {false, 0, 0, false, "", "", 0, "", -1, 0, false, false};
sedit::View g_v;   // (4 KB: the outline; not on the stack)

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
CORE_COLD void note(const char* m) {
  sniprintf(pg.note, sizeof pg.note, "%s", m);
  pg.note_ms = now_ms() | 1;
}
// An edit closed elsewhere (the touchscreen, the console) takes the page's question, FUNC and refusal with it: none of
// them is the next edit's
CORE_COLD void seen_open() {
  bool open = g_v.st != sedit::kClosed;
  if (pg.was_open && !open) pg.ask = kAskNone, pg.pick = 0, pg.func = false, pg.refused = false, pg.why[0] = 0;
  pg.was_open = open;
}
// The editor's state read again; a message it has not shown yet becomes the footer's note
CORE_COLD const sedit::View& refresh() {
  sedit::view(&g_v);
  seen_open();
  if (strcmp(g_v.msg, pg.seen)) {
    sniprintf(pg.seen, sizeof pg.seen, "%s", g_v.msg);
    if (g_v.msg[0]) note(g_v.msg);
  }
  return g_v;
}
CORE_COLD void act(int r) {   // an apply / undo / save / preview refused: why, again (the same words are news here)
  if (r >= 0) return;
  refresh();
  note(g_v.msg[0] ? g_v.msg : "NOT NOW");
}
// The save choice's options: SAVE+USE only for an edit opened from a track (and first: the default); MAKE WT (the region
// as a wavetable, B picks its frames) the first when the edit came from a WAVETABLE track
CORE_COLD int opts(uint8_t* o) {
  int n = 0;
  const bool for_wt = g_v.wt_voice >= 0;
  if (for_wt) o[n++] = kOptWt;
  if (g_v.has_track) o[n++] = kOptUse;
  o[n++] = kOptSave;
  if (!for_wt) o[n++] = kOptWt;
  o[n++] = kOptCancel;
  return n;
}
CORE_COLD void secs_text(uint32_t frames, uint32_t rate, char* b, int cap) {
  uint32_t ms = (uint32_t)((uint64_t)frames * 1000 / (rate ? rate : 48000));
  sniprintf(b, cap, CORE_STR("%lu.%03lu"), (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
}

// The window [wa, wb) as 128 columns, each the min / max of 8 of the outline's 1024; the region solid, the rest
// dithered (page_wave.cpp's draw_sample), the markers, the playhead inverted
CORE_COLD void draw_wave(Canvas& c, const sedit::View& v) {
  constexpr int kStep = sedit::kCols / kW;
  const int mid = kWaveY + kWaveH / 2, half = kWaveH / 2 - 1;
  const uint64_t span = v.wb > v.wa ? v.wb - v.wa : 1;
  for (int x = 0; x < kW; ++x) {
    int lo = 32767, hi = -32768;
    for (int k = x * kStep; k < (x + 1) * kStep; ++k) lo = v.lo[k] < lo ? v.lo[k] : lo, hi = v.hi[k] > hi ? v.hi[k] : hi;
    int y0 = clampi(mid - hi * half / 32767, kWaveY, kWaveY + kWaveH - 1);
    int y1 = clampi(mid - lo * half / 32767, kWaveY, kWaveY + kWaveH - 1);
    uint32_t fa = v.wa + (uint32_t)(span * x / kW), fb = v.wa + (uint32_t)(span * (x + 1) / kW);
    if (fb <= fa) fb = fa + 1;
    c.vline(x, y0, y1, fb > v.a && fa < v.b ? kOn : kDither);
  }
  auto xof = [&](uint32_t f) { return (int)((uint64_t)(f - v.wa) * kW / span); };
  if (v.a >= v.wa && v.a < v.wb) {   // the start: a line with a tab to the right
    int x = xof(v.a);
    c.vline(x, kWaveY, kWaveY + kWaveH - 1, kOn);
    c.hline(x, clampi(x + 2, 0, kW - 1), kWaveY, kOn);
  }
  if (v.b > v.wa && v.b <= v.wb) {   // the end (the region's last frame): a tab to the left
    int x = clampi(xof(v.b) - 1, 0, kW - 1);
    c.vline(x, kWaveY, kWaveY + kWaveH - 1, kOn);
    c.hline(clampi(x - 2, 0, kW - 1), x, kWaveY, kOn);
  }
  if (v.playhead >= 0 && (uint32_t)v.playhead >= v.wa && (uint32_t)v.playhead < v.wb)
    c.vline(xof((uint32_t)v.playhead), kWaveY, kWaveY + kWaveH - 1, kXor);
  if (v.unity < 32767) {   // sedit-050 #3: over 0 dBFS (the outline scaled to fit): 0 dBFS dotted, both ways
    for (int k = 0; k < 2; ++k) {
      int y = clampi(mid + (k ? 1 : -1) * v.unity * half / 32767, kWaveY, kWaveY + kWaveH - 1);
      for (int x = 0; x < kW; x += 2) c.pixel(x, y, kXor);
    }
  }
}
// sedit-050 #4: zoomed in, where the window is in the sample: a solid bar over a dithered line in row kWaveY + kWaveH
CORE_COLD void draw_place(Canvas& c, const sedit::View& v) {
  if (!v.frames || v.wb - v.wa >= v.frames) return;
  const int y = kPlaceY;
  int x0 = (int)((uint64_t)v.wa * kW / v.frames), x1 = (int)((uint64_t)v.wb * kW / v.frames) - 1;
  if (x1 < x0 + 1) x1 = x0 + 1;
  if (x1 > kW - 1) x1 = kW - 1, x0 = x0 > kW - 2 ? kW - 2 : x0;
  c.hline(0, kW - 1, y, kDither);
  c.hline(x0, x1, y, kOn);
}

CORE_COLD void modal_box(Canvas& c, const Rect& r) {
  c.fill(r, kOff);
  c.rect(r);
}

CORE_COLD void sedit_draw(Canvas& c, int page, int npages, uint32_t ms) {
  c.clear();
  const sedit::View& v = refresh();
  // the file's whole name in the header's room right of the title (and left of the page dots)
  int dots = npages > 1 ? (npages > 12 ? 12 : npages) : 0;
  int room = (kW - 2 - dots * 4 - 1 - (2 + 12 * kRSmall.advance)) / kRSmall.advance;   // (the title 11 + a gap)
  char name[24] = "";
  if (v.st != sedit::kClosed && room > 0) name_fit(name, sizeof name, v.name, room);
  if (v.st == sedit::kClosed) {   // the open refused (or the edit closed elsewhere)
    header(c, "SAMPLE EDIT", page, npages, name);
    c.put_text(kSlotFree, kRSmall, 64, 26, pg.refused && pg.why[0] ? pg.why : "NOTHING OPEN", kAlignCentre);
    footer_keys(c, nullptr, nullptr, "NO", "BACK");
    return;
  }
  const bool modal = pg.ask != kAskNone;
  const bool turning = !modal && pg.turn_e >= 0 && turn_live(ms, pg.turn_ms, kTurnMs);
  static const char* const kFull[5] = {"REGION START", "REGION END", "ZOOM", "VIEW", "FUNCTION"};
  static const char* const kShort[5] = {"START", "END", "ZOOM", "VIEW", "FUNC"};
  header(c, turning ? kFull[pg.turn_e] : "SAMPLE EDIT", page, npages, name);
  draw_wave(c, v);
  draw_place(c, v);
  char b[kTextLen + 1], s0[12], s1[12];
  secs_text(v.a, v.rate, s0, sizeof s0);
  secs_text(v.b, v.rate, s1, sizeof s1);
  const char* fn = sedit::kFnNames[v.fn < sedit::kFns ? v.fn : 0];
  if (!modal) {   // under a modal the page's text items are not put (the Rytm draws them over the pixels)
    static const char* const kNames[4] = {"STRT", "END", "ZOOM", "VIEW"};
    static const char* const kTag[sedit::kFns] = {"CROP", "NORM", "TRIM", "FD IN", "FD OUT", "REV", "GAIN+3", "GAIN-3"};
    for (int i = 0; i < 4; ++i) {   // the encoders' names over their columns; FUNC held: A and B jump to the transients
      const char* t = pg.func && i < 2 ? "TRNS" : kNames[i];
      bool on = turning && pg.turn_e == i;
      int w = (int)strlen(t) * kRSmall.advance + 3;
      if (on) c.fill(Rect{(int16_t)(i * kCellW + kCellW / 2 - w / 2), kNamesY - 1, (int16_t)w, 7});
      c.put_text(kSlotLabels + i, kRSmall, i * kCellW + kCellW / 2, kNamesY, t, (uint8_t)(kAlignCentre | (on ? kTextInvert : 0)));
    }
    c.fill(Rect{0, kInfoY - 1, kCellW, 7});   // E: the function as a filled tag
    c.put_text(kSlotLabels + 4, kRSmall, kCellW / 2, kInfoY, kTag[v.fn < sedit::kFns ? v.fn : 0], (uint8_t)(kAlignCentre | kTextInvert));
    sniprintf(b, sizeof b, CORE_STR("%s-%sS"), s0, s1);
    c.put_text(kSlotFree, kRSmall, kCellW + 4, kInfoY, b, kAlignLeft);
    sniprintf(b, sizeof b, CORE_STR("%sX%d"), v.dirty ? "* " : "", 1 << v.zoom);   // (the window's zoom: X1 X2 .. X256)
    c.put_text(kSlotFree + 1, kRSmall, 126, kInfoY, b, kAlignRight);
  }
  const char* no = v.undo ? "UNDO" : v.dirty ? "DISCARD" : "BACK";   // what NO does now
  if (pg.ask == kAskSave) {   // the save choice: a box with a line a choice, the pick inverted
    uint8_t o[4];
    int n = opts(o);
    pg.pick = (uint8_t)clampi(pg.pick, 0, n - 1);
    static const char* const kDesc[4] = {"TRACK USES IT", "NEW FILE", "", "NOTHING SAVED"};
    char wd[16];
    sniprintf(wd, sizeof wd, CORE_STR("B: %u FRAMES"), (unsigned)v.wt_frames);
    modal_box(c, Rect{4, 11, 120, 44});
    c.put_text(kSlotFree, kRSmall, 64, 14, "SAVE REGION  A: PICK", kAlignCentre);
    dotted_h(c, 5, 122, 21);
    for (int i = 0; i < n; ++i) {
      int y = 23 + 8 * i;
      bool on = i == pg.pick;
      if (on) c.fill(Rect{6, (int16_t)(y - 1), 116, 8});
      c.put_text(kSlotFree + 1 + 2 * i, kRSmall, 9, y, kOptNames[o[i]], on ? kTextInvert : kAlignLeft);
      c.put_text(kSlotFree + 2 + 2 * i, kRSmall, 119, y, o[i] == kOptWt ? wd : kDesc[o[i]], (uint8_t)(kAlignRight | (on ? kTextInvert : 0)));
    }
    footer_keys(c, "YES", "OK", "NO", "BACK");
  } else if (pg.ask == kAskDiscard) {
    modal_box(c, Rect{16, 18, 96, 28});
    c.put_text(kSlotFree, kRSmall, 64, 22, "DISCARD EDIT?", kAlignCentre);
    dotted_h(c, 17, 110, 29);
    c.put_text(kSlotFree + 1, kRSmall, 64, 34, "UNSAVED CHANGES", kAlignCentre);
    footer_keys(c, "YES", "DISCARD", "NO", "KEEP");
  } else if (pg.note_ms && now_ms() - pg.note_ms < kNoteMs) {
    footer(c, pg.note);
  } else if (v.st == sedit::kLoading) {
    sniprintf(b, sizeof b, CORE_STR("LOADING %d%%"), v.progress), footer(c, b);
  } else if (v.st == sedit::kSaving) {
    sniprintf(b, sizeof b, CORE_STR("SAVING %d%%"), v.progress), footer(c, b);
  } else if (v.st == sedit::kBusy) {
    sniprintf(b, sizeof b, CORE_STR("%s %d%%"), v.making ? "MAKE WT" : fn, v.progress), footer(c, b);
  } else if (v.st == sedit::kError) {
    footer(c, v.msg[0] ? v.msg : "ERROR");
  } else if (turning) {   // the touched knob: its name, a bar, its value
    const uint32_t fr = v.frames ? v.frames : 1;
    int bar = 0;
    switch (pg.turn_e) {
      case 0: bar = (int)((uint64_t)v.a * 127 / fr), sniprintf(b, sizeof b, CORE_STR("%sS"), s0); break;
      case 1: bar = (int)((uint64_t)v.b * 127 / fr), sniprintf(b, sizeof b, CORE_STR("%sS"), s1); break;
      case 2: bar = v.zoom * 127 / 8, sniprintf(b, sizeof b, CORE_STR("X%d"), 1 << v.zoom); break;
      case 3: {
        char w0[12];
        secs_text(v.wa, v.rate, w0, sizeof w0);
        bar = (int)((uint64_t)v.wa * 127 / fr), sniprintf(b, sizeof b, CORE_STR("%sS"), w0);
        break;
      }
      default: bar = v.fn * 127 / (sedit::kFns - 1), sniprintf(b, sizeof b, "%s", fn); break;
    }
    footer_value(c, kShort[pg.turn_e], bar, false, b);
  } else {
    footer_keys(c, "YES", pg.func ? "SAVE" : "DO", "NO", no);
  }
}

CORE_COLD void sedit_enc(int e, int d, bool func) {
  if (!d) return;
  const sedit::View& v = refresh();
  if (v.st == sedit::kClosed) return;
  if (e <= 4) pg.turn_e = e, pg.turn_ms = now_ms();
  if (pg.ask == kAskSave) {   // A picks, B the table's frames (16 .. 256); the rest wait for the answer
    uint8_t o[4];
    if (e == 0) pg.pick = (uint8_t)clampi(pg.pick + d, 0, opts(o) - 1);
    else if (e == 1) sedit::set_wt_frames(clampi((int)v.wt_frames * (d > 0 ? 2 : 1) / (d > 0 ? 1 : 2), 16, 256));
    return;
  }
  if (pg.ask == kAskDiscard) return;
  switch (e) {   // sedit-050 #4: A START, B END, C ZOOM, D the window's place, E the function
    case 0:
    case 1:
      if (func) act(sedit::jump(e, d));   // #8: FUNC: transient to transient
      else sedit::nudge(e, d, false);
      break;
    case 2: sedit::zoom(clampi(v.zoom + (d > 0 ? 1 : -1), 0, 8)); break;
    case 3: sedit::pan(d, func); break;
    case 4: sedit::set_fn(clampi(v.fn + d, 0, sedit::kFns - 1)); break;   // a detent a function
    default: break;
  }
}

CORE_COLD void drop_state() { pg.ask = kAskNone, pg.pick = 0, pg.refused = false, pg.why[0] = 0, pg.note_ms = 0; }

CORE_COLD bool sedit_key(int code) {
  const sedit::View& v = refresh();
  pg.turn_e = -1;
  if (code == kFunc) {   // FUNC (both sides': the Rytm keeps it too)
    pg.func = true;
    return true;
  }
  bool push = code >= kPushA && code <= kPushD;
  if (v.st == sedit::kClosed) {   // nothing open (the open was refused): NO back
    if (code == kNo) {
      drop_state();
      return false;
    }
    return code == kYes || push;
  }
  if (pg.ask == kAskSave) {
    if (code == kYes) {
      uint8_t o[4];
      int n = opts(o);
      uint8_t pick = o[clampi(pg.pick, 0, n - 1)];
      pg.ask = kAskNone;
      if (pick == kOptWt) act(sedit::make_wt());
      else if (pick != kOptCancel) act(sedit::save(pick == kOptUse));
      return true;
    }
    if (code == kNo) pg.ask = kAskNone;
    return code == kNo || push;
  }
  if (pg.ask == kAskDiscard) {
    if (code == kYes) {   // dropped: the page has nothing to show and goes back (remote.cpp)
      sedit::close();
      drop_state();
      return true;
    }
    if (code == kNo) pg.ask = kAskNone;
    return code == kNo || push;
  }
  if (code == kYes) {
    if (pg.func) pg.ask = kAskSave, pg.pick = 0;
    else act(sedit::apply());
    return true;
  }
  if (code == kNo) {
    if (v.undo) {
      act(sedit::undo());
      return true;
    }
    if (v.dirty) {
      pg.ask = kAskDiscard;
      return true;
    }
    sedit::close();   // the track back on its original; NO: back where the page was opened from
    drop_state();
    return false;
  }
  if (code == kPushA) {
    act(sedit::preview());
    return true;
  }
  return push;   // B..D's pushes: nothing (not the Rytm's under the page)
}
CORE_COLD bool sedit_key_up(int code) {
  if (code == kFunc) pg.func = false;
  return true;
}
CORE_COLD bool sedit_shown() {
  sedit::view(&g_v);
  seen_open();
  return pg.refused || g_v.st != sedit::kClosed;
}

}  // namespace

UI_ROM const ExtraPage kSeditPage = {"SAMPLE EDIT", sedit_shown, sedit_draw, sedit_enc, sedit_key, 0x1F, false,
                                     nullptr, nullptr, nullptr, true, sedit_key_up};

// remote_tick: true once, when a table made for a voice has closed the editor: the Rytm's page leaves, back to the SRC view
// (or the page the editor was opened over)
CORE_COLD bool sedit_page_leaves() {
  sedit::view(&g_v);
  if (g_v.wt_back) pg.wt_back = true;
  if (!pg.wt_back || g_v.st != sedit::kClosed) return false;
  pg.wt_back = false;
  return true;
}
CORE_COLD void sedit_page_open(int track, int code) {
  const sedit::View& v = refresh();
  bool open = v.st != sedit::kClosed;
  pg.ask = kAskNone, pg.pick = 0, pg.func = false, pg.note_ms = 0, pg.refused = false, pg.turn_e = -1, pg.wt_back = false;
  sniprintf(pg.seen, sizeof pg.seen, "%s", v.msg);   // (an old message is not news)
  const bool wt_track = track >= 0 && (remote_track_machine(track) & 0x78) == kWaveMachine && remote_track_machine(track) != 0x7F;
  if (open && ((track < 0 && code < 0) || (track >= 0 && v.has_track && v.track == track) || (wt_track && v.wt_track == track))) {   // back to the edit
    remote_show_page("SAMPLE EDIT");
    return;
  }
  if (open && v.dirty) {   // another sample asked for: the edit open is not dropped unasked
    note("SAVE OR DROP THE EDIT");
    remote_show_page("SAMPLE EDIT");
    return;
  }
  if (track < 0 && code < 0) track = remote_selected_track(nullptr);
  int r = -1;
  if (wt_track) {   // a WAVETABLE track: table A's card file; a built-in or OFF: the SAMPLES list to pick a sample from
    r = sedit::open_wt_track(track);
    if (r > 0) {
      sedit::set_wt_voice(remote_track_voice(track));
      pool_page_pick_sample();
      return;
    }
  } else if (track >= 0) {
    r = sedit::open_track(track);
  } else if (code >= 0) {
    r = sedit::open_code(code);
  }
  if (r < 0) {
    refresh();
    pg.refused = true;
    sniprintf(pg.why, sizeof pg.why, "%s", track < 0 && code < 0 ? "NO TRACK SELECTED" : g_v.msg[0] ? g_v.msg : "NOT NOW");
  }
  remote_show_page("SAMPLE EDIT");
}
// A push of encoder B on a DSP SAMPLE / SAMPLER or WAVETABLE track's SRC view (A is the list's push; SAMPLER: vb 0x20 too;
// WAVETABLE: B is POS, which has no push of its own); true: taken
CORE_COLD bool sedit_src_push(int track, int code, bool down) {
  int vb = track >= 0 ? remote_track_machine(track) : -1;
  if (code != 0x71 || vb < 0 || vb == 0x7F || ((vb & 0x78) != kDspSample && (vb & 0x78) != kWaveMachine)) return false;
  if (down) sedit_page_open(track, -1);
  return true;
}
#if CORE_UI_TOOLS
CORE_COLD void sedit_page_reset() { pg = Pg{false, 0, 0, false, "", "", 0, "", -1, 0, false, false}; }
#endif

}  // namespace ui
}  // namespace core
#endif
