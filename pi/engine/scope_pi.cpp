// pi/engine/scope_pi.cpp - see scope_pi.h.
#include "scope_pi.h"

#include <string.h>

#include "platform.h"
#include "ui/extra.h"
#include "ui/remote.h"

namespace scope_pi {

namespace {
namespace ui = core::ui;
constexpr int kCols = SSC_COLS, kHist = 2 * SSC_COLS;
const uint8_t kEntries[SSC_TBS] = {1, 2, 4, 8, 16, 32, 64};   // the tap's entries a column (sidecar_scope.c's timebases)
// Rytm track -> the scope's source: the stems are the Rytm's voices in track order (BD SD RS/CP BT LT MT/HT CH/OH CY/CB)
const int8_t kTrackSrc[12] = {1, 2, 3, 3, 4, 5, 6, 6, 7, 7, 8, 8};
struct State {
  int src = -1, tb = -1;
  uint32_t rd = 0, wp_seen = 0, wp_ms = 0;
  int16_t hist[kHist][2];   // the triggered timebases: the last kHist columns
  int hw = 0, hn = 0;
  int16_t sweep[kCols][2];  // the sweeping ones: the screen as written
  int cursor = 0, sn = 0;
  int32_t plo = 0, phi = 0;
  int pcnt = 0;
  uint32_t gen = 1;
};
State g;   // ~3 KB, not on the stack

void column(int16_t lo, int16_t hi) {
  if (g.tb >= SSC_TRIG_TBS) {
    g.sweep[g.cursor][0] = lo, g.sweep[g.cursor][1] = hi;
    g.cursor = (g.cursor + 1) % kCols;
    if (g.sn < kCols) ++g.sn;
  } else {
    g.hist[g.hw][0] = lo, g.hist[g.hw][1] = hi;
    g.hw = (g.hw + 1) % kHist;
    if (g.hn < kHist) ++g.hn;
  }
}

// the newest kCols columns of the history, started on a rising crossing of the middle searched over the window before
// (page_scope.cpp's scope_window); fewer known: from the left
int window(sidecar_scope_view_t* o) {
  int n = g.hn < kCols ? g.hn : kCols;
  int start = g.hw - n;
  if (g.hn >= kHist) {
    int peak = 0;
    for (int i = 0; i < kHist; ++i) {
      int a = g.hist[i][1] > -g.hist[i][0] ? g.hist[i][1] : -g.hist[i][0];
      if (a > peak) peak = a;
    }
    int thr = peak / 8;
    for (int s = start; s > start - (kCols - 1); --s) {
      const int16_t* p = g.hist[(s - 1 + 2 * kHist) % kHist];
      const int16_t* q = g.hist[(s + 2 * kHist) % kHist];
      if ((p[0] + p[1]) / 2 < -thr && (q[0] + q[1]) / 2 >= 0) { start = s; break; }
    }
  }
  for (int i = 0; i < n; ++i) {
    const int16_t* h = g.hist[(start + i + 2 * kHist) % kHist];
    o->lo[i] = h[0], o->hi[i] = h[1];
  }
  return n;
}
}  // namespace

int SourceOfTrack(int t) { return t >= 0 && t < 12 ? kTrackSrc[t] : 0; }

void Leave() {
  ui::tap_enable(false);
  g.src = -1;   // the next visit starts over
}

void Fill(sidecar_scope_view_t* o, int want, int tb, const uint32_t peaks[SSC_CH]) {
  ui::tap_enable(true);   // (a Rytm hello turns it off: on again each fill)
  int track = ui::remote_selected_track(nullptr);
  int src = want >= 0 && want < SSC_SOURCES ? want : track >= 0 ? SourceOfTrack(track) : 1;
  tb = tb < 0 ? 0 : tb >= SSC_TBS ? SSC_TBS - 1 : tb;
  uint32_t now = core::now_ms();
  if (src != g.src || tb != g.tb) {   // start over: what the tap wrote before is another source's
    ui::tap_source(src);
    g.src = src, g.tb = tb;
    g.rd = ui::tap_write_pos();
    g.hw = g.hn = g.sn = g.cursor = g.pcnt = 0;
    ++g.gen;
  }
  int k = kEntries[tb], got = 0;
  int16_t e[256][2];
  for (int n; (n = ui::tap_read(&g.rd, e, 256)) > 0;) {
    got += n;
    for (int i = 0; i < n; ++i) {
      if (!g.pcnt) g.plo = e[i][0], g.phi = e[i][1];
      if (e[i][0] < g.plo) g.plo = e[i][0];
      if (e[i][1] > g.phi) g.phi = e[i][1];
      if (++g.pcnt == k) column((int16_t)g.plo, (int16_t)g.phi), g.pcnt = 0;
    }
  }
  if (got) ++g.gen;
  uint32_t wp = ui::tap_write_pos();
  if (wp != g.wp_seen) g.wp_seen = wp, g.wp_ms = now ? now : 1;
  o->valid = g.wp_ms && now - g.wp_ms < 500;   // the IN path wrote within the last half second: Overbridge audio runs
  o->src = src, o->track = track, o->tb = tb;
  if (tb >= SSC_TRIG_TBS) {
    for (int i = 0; i < kCols; ++i) o->lo[i] = g.sweep[i][0], o->hi[i] = g.sweep[i][1];
    o->ncols = g.sn;          // the first lap: the columns written so far, from the left
    o->cursor = g.cursor;     // the next column written (the page leaves a gap there)
  } else {
    o->ncols = window(o);
    o->cursor = -1;
  }
  for (int s = 0; s < SSC_SOURCES; ++s) sniprintf(o->name[s], sizeof o->name[s], "%s", ui::tap_source_name(s));
  for (int c = 0; c < SSC_CH; ++c) o->peak[c] = peaks ? peaks[c] : 0;
  o->gen = g.gen * 2u + 1u;
}

}  // namespace scope_pi
