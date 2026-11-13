// core/ui/page_sedit.cpp - SAMPLE EDIT on the Rytm's screen (core/samp/sedit.h holds the edit; the touchscreen shows
// the same state). Cold. Pi and Mac (CORE_POLY && CORE_MSAMP builds).
//
// Opened by a push of encoder B on a DSP SAMPLE / SAMPLER track's SRC view (the track's sample, its STA..END the region), by the
// POOL page's EDIT on a SAMPLES row (the whole sample, no track), or by the SIDECAR MENU (the selected track's sample;
// the edit open already: back to it). Off the ring: in the page list only while it shows.
// Drawn: the header with the file's name; the window's outline in rows 10..47 (the region solid, the rest dithered, the
// start and end markers, the preview's playhead; 0 dBFS dotted when the edit goes over it); zoomed in, the window's
// place in the sample as a bar in row 48; the region's start and end in seconds and the zoom; the footer the
// function and YES, the job's progress ("NORMALISE 42%"), or a message for 2 s.
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
constexpr int kWaveY = 10, kWaveH = 38, kInfoY = 49;   // the outline in rows 10..47, the region's line under it
constexpr uint32_t kNoteMs = 2000;
enum Ask : uint8_t { kAskNone, kAskSave, kAskDiscard };
enum Opt : uint8_t { kOptUse, kOptSave, kOptCancel };
UI_ROM const char* const kOptNames[3] = {"SAVE+USE", "SAVE", "CANCEL"};

struct Pg {
  bool func;          // FUNC held
  uint8_t ask, pick;  // the question on screen, the save choice's pick (an index into opts())
  bool refused;       // the last open was refused: the page shows why until NO
  char why[32];       // that refusal
  char note[32];      // the footer's message (kNoteMs)
  uint32_t note_ms;
  char seen[32];      // the editor's msg last seen: a new one becomes the note
  bool was_open;      // an edit open at the last refresh (its close elsewhere drops the page's question)
};
Pg pg = {};
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
// The save choice's options: SAVE+USE only for an edit opened from a track (and first: the default)
CORE_COLD int opts(uint8_t* o) {
  int n = 0;
  if (g_v.has_track) o[n++] = kOptUse;
  o[n++] = kOptSave, o[n++] = kOptCancel;
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
  const int y = kWaveY + kWaveH;
  int x0 = (int)((uint64_t)v.wa * kW / v.frames), x1 = (int)((uint64_t)v.wb * kW / v.frames) - 1;
  if (x1 < x0 + 1) x1 = x0 + 1;
  if (x1 > kW - 1) x1 = kW - 1, x0 = x0 > kW - 2 ? kW - 2 : x0;
  c.hline(0, kW - 1, y, kDither);
  c.hline(x0, x1, y, kOn);
}

CORE_COLD void sedit_draw(Canvas& c, int page, int npages, uint32_t ms) {
  (void)ms;
  c.clear();
  const sedit::View& v = refresh();
  // the name in the header's room right of the title (and left of the page dots)
  int dots = npages > 1 ? (npages > 12 ? 12 : npages) : 0;
  int room = (kW - 2 - dots * 4 - 1 - (2 + 12 * kRSmall.advance)) / kRSmall.advance;   // (the title 11 + a gap)
  char name[24] = "";
  if (v.st != sedit::kClosed && room > 0) name_fit(name, sizeof name, v.name, room);
  header(c, "SAMPLE EDIT", page, npages, name);
  if (v.st == sedit::kClosed) {   // the open refused (or the edit closed elsewhere)
    c.put_text(kSlotFree, kRSmall, 64, 26, pg.refused && pg.why[0] ? pg.why : "NOTHING OPEN", kAlignCentre);
    footer(c, "NO: BACK");
    return;
  }
  draw_wave(c, v);
  char b[kTextLen + 1];
  if (pg.ask == kAskSave) {   // the save choice: the pick inverted
    uint8_t o[3];
    int n = opts(o), x = 2;
    pg.pick = (uint8_t)clampi(pg.pick, 0, n - 1);
    for (int i = 0; i < n; ++i) {
      const char* s = kOptNames[o[i]];
      int w = (int)strlen(s) * kRSmall.advance + 2;
      bool on = i == pg.pick;
      if (on) c.fill(Rect{(int16_t)(x - 1), (int16_t)(kInfoY - 1), (int16_t)w, 7}, kOn);
      c.put_text(kSlotFree + i, kRSmall, x, kInfoY, s, (uint8_t)(kAlignLeft | (on ? kTextInvert : 0)));
      x += w + 6;
    }
  } else {
    draw_place(c, v);
    char s0[12], s1[12];
    secs_text(v.a, v.rate, s0, sizeof s0);
    secs_text(v.b, v.rate, s1, sizeof s1);
    sniprintf(b, sizeof b, CORE_STR("%s-%sS"), s0, s1);
    c.put_text(kSlotFree, kRSmall, 2, kInfoY, b, kAlignLeft);
    sniprintf(b, sizeof b, CORE_STR("%sX%d"), v.dirty ? "* " : "", 1 << v.zoom);   // (the window's zoom: X1 X2 .. X256)
    c.put_text(kSlotFree + 1, kRSmall, 126, kInfoY, b, kAlignRight);
  }
  const char* fn = sedit::kFnNames[v.fn < sedit::kFns ? v.fn : 0];
  if (pg.ask == kAskDiscard) sniprintf(b, sizeof b, "DISCARD EDIT? YES/NO");
  else if (pg.ask == kAskSave) sniprintf(b, sizeof b, "A:PICK  YES:OK  NO:BACK");
  else if (pg.note_ms && now_ms() - pg.note_ms < kNoteMs) sniprintf(b, sizeof b, "%s", pg.note);
  else if (v.st == sedit::kLoading) sniprintf(b, sizeof b, CORE_STR("LOADING %d%%"), v.progress);
  else if (v.st == sedit::kSaving) sniprintf(b, sizeof b, CORE_STR("SAVING %d%%"), v.progress);
  else if (v.st == sedit::kBusy) sniprintf(b, sizeof b, CORE_STR("%s %d%%"), fn, v.progress);
  else if (v.st == sedit::kError) sniprintf(b, sizeof b, "%s", v.msg[0] ? v.msg : "ERROR");
  else sniprintf(b, sizeof b, CORE_STR("E:%s  YES:DO"), fn);
  footer(c, b);
}

CORE_COLD void sedit_enc(int e, int d, bool func) {
  if (!d) return;
  const sedit::View& v = refresh();
  if (v.st == sedit::kClosed) return;
  if (pg.ask == kAskSave) {   // A picks; the rest wait for the answer
    uint8_t o[3];
    if (e == 0) pg.pick = (uint8_t)clampi(pg.pick + d, 0, opts(o) - 1);
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
      uint8_t o[3];
      int n = opts(o);
      uint8_t pick = o[clampi(pg.pick, 0, n - 1)];
      pg.ask = kAskNone;
      if (pick != kOptCancel) act(sedit::save(pick == kOptUse));
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

CORE_COLD void sedit_page_open(int track, int code) {
  const sedit::View& v = refresh();
  bool open = v.st != sedit::kClosed;
  pg.ask = kAskNone, pg.pick = 0, pg.func = false, pg.note_ms = 0, pg.refused = false;
  sniprintf(pg.seen, sizeof pg.seen, "%s", v.msg);   // (an old message is not news)
  if (open && ((track < 0 && code < 0) || (track >= 0 && v.has_track && v.track == track))) {   // back to the edit
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
  if (track >= 0) r = sedit::open_track(track);
  else if (code >= 0) r = sedit::open_code(code);
  if (r < 0) {
    refresh();
    pg.refused = true;
    sniprintf(pg.why, sizeof pg.why, "%s", track < 0 && code < 0 ? "NO TRACK SELECTED" : g_v.msg[0] ? g_v.msg : "NOT NOW");
  }
  remote_show_page("SAMPLE EDIT");
}
// A push of encoder B on a DSP SAMPLE / SAMPLER track's SRC view (A is the list's push; SAMPLER: vb 0x20 too); true: taken
CORE_COLD bool sedit_src_push(int track, int code, bool down) {
  int vb = track >= 0 ? remote_track_machine(track) : -1;
  if (code != 0x71 || vb < 0 || vb == 0x7F || (vb & 0x78) != kDspSample) return false;
  if (down) sedit_page_open(track, -1);
  return true;
}
#if CORE_UI_TOOLS
CORE_COLD void sedit_page_reset() { pg = Pg{}; }
#endif

}  // namespace ui
}  // namespace core
#endif
