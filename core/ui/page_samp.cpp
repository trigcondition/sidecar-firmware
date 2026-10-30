// core/ui/page_samp.cpp - the remote UI's SAMPLE page (the Pi and the Mac: CORE_MSAMP), next to REC: the sampler
// (core/samp/sampler.h) and the auto-sampler's progress (core/samp/autosamp.h). Drawn and keyed on the control core,
// which owns the sampler's settings: the page calls it directly. Cold.
//
// Shown where REC is. Keys: A turned the source (IN L/R, MAIN, BD .. CY/CB, VOICE 1..8), B the start (NOW, THR, PLAY, BAR), C the length in
// bars (FREE, 1..64), D the slices (TRANS, GRID 8, GRID 16, OFF); YES arms (NOW: records at once) or stops a take;
// B with FUNC: the threshold (THR start, -60..-1 dBFS). A pushed cancels (the sampler's take, else a running auto-sampler). NO, LEFT / RIGHT ... navigate as everywhere.
// Drawn: the four settings with their labels, the source's live meter (stereo L / R for IN and MAIN, mono for a track or
// voice; peak hold 1 s, a clip dot at the end; the threshold's tick for a THR start), the take's time, the last take's
// name and slices; the bottom bar what YES does now, or the auto-sampler's progress ("AUTO C4 v100 3/24").
#include "platform.h"

#if CORE_MSAMP && CORE_POLY
#include <string.h>

#include "samp/autosamp.h"
#include "samp/dsp.h"
#include "samp/sampler.h"
#include "ui/extra.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

// the sources in A's order: src, sub
constexpr int kSources = 18;
CORE_COLD void source_of(int i, samp::Config* c) {
  if (i == 0) c->src = samp::kSrcIn;
  else if (i == 1) c->src = samp::kSrcMain;
  else if (i < 10) c->src = samp::kSrcTrack, c->sub = (uint8_t)(i - 2);
  else c->src = samp::kSrcVoice, c->sub = (uint8_t)(i - 10);
}
CORE_COLD int index_of(const samp::Config& c) {
  return c.src == samp::kSrcIn ? 0 : c.src == samp::kSrcMain ? 1 : c.src == samp::kSrcTrack ? 2 + c.sub : 10 + c.sub;
}
CORE_COLD const char* source_text(int i, char* b, int n) {
  static const char* const t[10] = {"IN L/R", "MAIN", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB"};
  if (i < 10) return t[i];
  sniprintf(b, n, CORE_STR("VOICE %d"), i - 9);
  return b;
}
CORE_COLD int slices_index(const samp::Config& c) {
  return c.slices == samp::kTransients ? 0 : c.slices == samp::kGrid ? (c.grid_n <= 8 ? 1 : 2) : 3;
}
constexpr int kMeterX = 14, kMeterW = 96;   // the bars' left column and width (-48 dBFS .. 0)
Meters g_meters;
uint32_t g_meter_ms = 0;
// the tap channels of a source: n (1 mono, 2 L R) from ch[0]
CORE_COLD int source_channels(const samp::Config& c, int* ch) {
  if (c.src == samp::kSrcIn) return ch[0] = 10, 2;
  if (c.src == samp::kSrcMain) return ch[0] = 0, 2;
  ch[0] = c.src == samp::kSrcTrack ? 2 + (c.sub & 7) : tap_channel_of_voice(c.sub & 7);
  return 1;
}

}  // namespace

// The source meter (SAMPLE, and AUTO's Rytm-track source): the tap's peaks every 66 ms into one set of meters
CORE_COLD void source_meter_update(uint32_t ms) {
  if (ms - g_meter_ms < 66 && g_meter_ms) return;
  uint16_t pk[kTapCh];
  tap_take_peaks(pk);
  g_meters.update(pk, ms);
  g_meter_ms = ms;
}
CORE_COLD int source_meter_level(int ch) { return (kMeterFloor - g_meters.level[ch]) * 127 / kMeterFloor; }   // 0..127 for a footer's box
// n bars (1: rows y..y+7; 2: L over R, 5 rows each) of tap channels ch0, ch0+1: the level, a 1 px peak hold, a dot at the
// end lit while the hold is within 0.5 dB of full scale; thr_db < 0: the threshold's marker. Text slots slot, slot+1: L R.
CORE_COLD void source_meter_draw(Canvas& c, int y, int ch0, int n, int thr_db, int slot) {
  for (int k = 0; k < n; ++k) {
    int ch = ch0 + k, by = n == 1 ? y + 1 : y + k * 7, bh = n == 1 ? 7 : 5;
    int lv = (kMeterFloor - g_meters.level[ch]) * kMeterW / kMeterFloor, hd = (kMeterFloor - g_meters.hold[ch]) * kMeterW / kMeterFloor;
    c.rect(Rect{(int16_t)kMeterX, (int16_t)by, (int16_t)(kMeterW + 2), (int16_t)bh});
    if (lv > 0) c.fill(Rect{(int16_t)(kMeterX + 1), (int16_t)(by + 1), (int16_t)(lv > kMeterW ? kMeterW : lv), (int16_t)(bh - 2)});
    if (hd > lv + 1) c.vline(kMeterX + 1 + (hd > kMeterW ? kMeterW : hd) - 1, by + 1, by + bh - 2);
    Rect dot{(int16_t)(kMeterX + kMeterW + 6), (int16_t)(by + 1), 5, (int16_t)(bh - 2)};
    if (g_meters.hold[ch] < 1) c.fill(dot);
    else c.rect(dot);
    if (n == 2) c.put_text(slot + k, kRSmall, 8, by - 1, k ? "R" : "L", kAlignCentre);
  }
  if (thr_db < 0 && thr_db > -kMeterFloor / 2) {   // the threshold: a tick over and under the bars
    int x = kMeterX + 1 + (kMeterFloor + 2 * thr_db) * kMeterW / kMeterFloor;
    c.vline(x, y - 3, y - 1);
    c.vline(x, y + (n == 1 ? 9 : 12), y + (n == 1 ? 11 : 14));
    if (n == 2) c.vline(x, y + 5, y + 6, kDither);
  }
}

namespace {

CORE_COLD void samp_draw(Canvas& c, int page, int npages, uint32_t ms) {
  samp::Status s;
  samp::status(&s);
  static const char* const kState[] = {"IDLE", "ARMED", "RECORDING", "SAVING", "SAVING", "DONE", "FAILED"};
  c.clear();
  header(c, "SAMPLE", page, npages, kState[s.state % 7]);
  const samp::Config& g = s.state == samp::kIdle || s.state == samp::kDone || s.state == samp::kFailed ? samp::config() : s.cfg;
  char b[kTextLen + 1], v[4][12];
  int si = index_of(g);
  strncpy(v[0], source_text(si, b, sizeof b), sizeof v[0] - 1), v[0][sizeof v[0] - 1] = 0;
  static const char* const kStart[] = {"NOW", "THR", "PLAY", "BAR"};
  if (g.start == samp::kThreshold) sniprintf(v[1], sizeof v[1], CORE_STR("THR%d"), (int)g.thr_db);   // the level too (FUNC + B)
  else strcpy(v[1], kStart[g.start & 3]);
  if (g.bars) sniprintf(v[2], sizeof v[2], CORE_STR("%d BAR"), g.bars);
  else strcpy(v[2], "FREE");
  static const char* const kSl[] = {"TRANS", "GRID 8", "GRID16", "OFF"};
  strcpy(v[3], kSl[slices_index(g)]);
  for (int i = 0; i < 4; ++i) c.put_text(kSlotCells + i, kRSmall, i * kCellW + kCellW / 2, 14, v[i], kAlignCentre);
  static const char* const kLab[4] = {"SRC", "START", "LEN", "SLICE"};
  labels(c, cell(0, 0, 4, 1), kLab, 4, 0);
  // the source's meter (rows 32..46, live at all times), the take's time or the last take (row 48), the tempo
  source_meter_update(ms);
  int chs[2], nch = source_channels(g, chs);
  source_meter_draw(c, nch == 2 ? 36 : 37, chs[0], nch, g.start == samp::kThreshold ? (int)g.thr_db : 0, kSlotFree + 8);
  uint32_t secs = s.frames / samp::kRate;
  char t[8];
  sniprintf(t, sizeof t, CORE_STR("%lu:%02lu"), (unsigned long)(secs / 60), (unsigned long)(secs % 60));
  if (s.state == samp::kRecording || s.state == samp::kArmed) {
    c.put_text(kSlotFree, kRSmall, 4, 49, t, kAlignLeft);
    b[0] = 0;
  } else if (s.wav[0] && s.row[0]) {   // the pools design 3.7: the take is in the SAMPLES pool
    sniprintf(b, sizeof b, CORE_STR("TAKE -> %s %dSL"), s.row, s.slices);
  } else if (s.wav[0]) {
    const char* nm = strrchr(s.wav, '/');
    sniprintf(b, sizeof b, CORE_STR("%.12s %dSL"), nm ? nm + 1 : s.wav, s.slices);
  } else if (s.error[0]) {
    sniprintf(b, sizeof b, CORE_STR("ERR %.18s"), s.error);
  } else {
    b[0] = 0;
  }
  c.put_text(kSlotFree + 1, kRSmall, 4, 49, b, kAlignLeft);
  if (s.bpm > 0) {
    sniprintf(t, sizeof t, CORE_STR("%d BPM"), (int)(s.bpm + 0.5f));
    c.put_text(kSlotFree + 2, kRSmall, 126, 49, t, kAlignRight);
  }
  // the bottom bar
  samp::AStatus a;
  samp::asamp_status(&a);
  if (a.state == samp::kATaking || a.state == samp::kAWriting) sniprintf(b, sizeof b, CORE_STR("AUTO %.18s"), a.label);
  else if (s.state == samp::kRecording) strcpy(b, "YES:STOP A:CANCEL");   // (17 characters: clear of the level box)
  else if (s.state == samp::kArmed) sniprintf(b, sizeof b, CORE_STR("WAITS (%s)  A: OFF"), kStart[g.start & 3]);
  else if (s.state == samp::kCaptured || s.state == samp::kWriting) strcpy(b, "SAVING...");
  else strcpy(b, g.start == samp::kManual ? "YES: RECORD  NO: BACK" : "YES: ARM  NO: BACK");
  footer(c, b, s.state == samp::kRecording ? (int)(s.peak * 127) : -1);
}

// A list knob's step: one entry a detent, clamped at the list's ends (no wrap: a fast turn's large delta, or turning on
// past an end, never jumps to the other end), as every Sidecar list knob (pages.cpp step_value, SCOPE)
CORE_COLD int clamp_step(int v, int d, int n) { v += d; return v < 0 ? 0 : v >= n ? n - 1 : v; }

CORE_COLD void samp_enc(int e, int delta, bool func) {
  if (!delta) return;
  samp::Config c = samp::config();
  if (e == 0) {
    int i = clamp_step(index_of(c), delta, kSources);
    source_of(i, &c);
  } else if (e == 1) {
    if (func && c.start == samp::kThreshold) c.thr_db = c.thr_db + delta < -60 ? -60 : c.thr_db + delta > -1 ? -1 : c.thr_db + delta;   // FUNC: the threshold, dBFS
    else c.start = (samp::Start)clamp_step(c.start, delta, 4);
  } else if (e == 2) {
    int b = c.bars + delta;
    c.bars = (uint8_t)(b < 0 ? 0 : b > 64 ? 64 : b);
  } else if (e == 3) {
    int i = clamp_step(slices_index(c), delta, 4);
    c.slices = i == 0 ? samp::kTransients : i == 3 ? samp::kNoSlices : samp::kGrid;
    c.grid_n = i == 1 ? 8 : 16;
  } else {
    return;
  }
  samp::configure(c);   // (a take already armed keeps its own)
}

CORE_COLD bool samp_key(int code) {
  samp::Status s;
  samp::status(&s);
  if (code == 0x44) {   // YES
    if (s.state == samp::kRecording) samp::stop();
    else if (s.state != samp::kArmed && s.state != samp::kCaptured && s.state != samp::kWriting) {
      char err[64];
      if (!samp::arm(err, sizeof err)) log(CORE_STR("samp: remote page: %s"), err);
    }
    return true;
  }
  if (code == 0x70) {   // A pushed: cancel
    samp::AStatus a;
    samp::asamp_status(&a);
    if (s.state == samp::kArmed || s.state == samp::kRecording) samp::cancel();
    else if (a.state == samp::kATaking || a.state == samp::kAWriting) samp::asamp_cancel();
    return true;
  }
  return false;
}

// Shown where REC is (a platform with a recorder and the Overbridge IN: the Pi)
CORE_COLD bool samp_shown() { return kRecPage.shown && kRecPage.shown(); }

}  // namespace

UI_ROM const ExtraPage kSampPage = {"SAMPLE", samp_shown, samp_draw, samp_enc, samp_key, 0x0F, true, nullptr, nullptr, nullptr};

}  // namespace ui
}  // namespace core
#endif
