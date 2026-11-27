// core/tests/test_sedit_page.cpp - SAMPLE EDIT on the Rytm's screen (core/ui/page_sedit.cpp over core/samp/sedit.h)
// through the remote host, as test_morph_page.cpp: a push of B on a DSP SAMPLE track's SRC view opens it; A the
// function, B / C the markers (FUNC: coarse), D the zoom; YES applies, FUNC + YES the save choice (SAVE+USE only from a
// track), NO undoes, then asks, then leaves; the POOL page's EDIT; FUNC in the catch mask; PNGs of the page and every
// text fits (check.h's TEXT-FIT). Track 0 (voice 0) is DSP SAMPLE, SMP A001 = kick.wav (1 s, mono, 16-bit).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "nmach.h"
#include "rui_png.h"
#include "screens_util.h"
#include "samp/dsp.h"
#include "samp/sedit.h"
#include "mod/scene.h"
#include "samp/sedit_dsp.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"
#include "wavetable/wavetable.h"

#include "../../pi/engine/voice_pi.cpp"       // the touchscreen's VOICE routing (as test_page_keys.cpp has it)
#include "../../pi/engine/seditview_pi.cpp"   // (voice_pi asks it whether an edit is open)

using namespace core;
using SV = sedit::View;

// (pi/ui/rytm_pattern.c, used by voice_pi::Fill only: not called here)
extern "C" int rytm_locks_on(const rytm_pattern_t*, int, int) { return 0; }

static std::string g_card;
static uint64_t g_us = 1000000;
static constexpr uint32_t kN = 48000;

static void write_wav16(const char* path, uint32_t frames) {   // 0.5 s of silence, then a decaying 110 Hz sine
  uint8_t h[samp::kWavHeaderMax];
  int hn = samp::wav_header(h, 1, 16, 48000, frames);
  std::vector<uint8_t> b(h, h + hn);
  uint8_t o[2];
  for (uint32_t f = 0; f < frames; ++f) {
    double t = f < 24000 ? 0 : (f - 24000) / 48000.0;
    samp::put16(o, f < 24000 ? 0.f : (float)(0.8 * exp(-t / 0.08) * sin(2 * M_PI * 110 * t)));
    b.insert(b.end(), o, o + 2);
  }
  FILE* fp = fopen(host_path(path).c_str(), "wb");
  CHECK(fp);
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
}
static bool exists(const char* path) { return access(host_path(path).c_str(), F_OK) == 0; }

// ---- what goes to the Rytm: the 37 catch mask, the 48 knob writes
static int g_mask_encs = -1;
static bool g_mask_func = false;
static std::vector<std::vector<uint8_t>> g_sent;
static int g_closes = 0;   // 3A 0 0: the host asks the page closed (nav044)
static void out(const uint8_t* d, int len) {
  if (len < 7 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return;
  if (d[4] == 0x37 && len >= 10) {
    g_mask_encs = d[6] | d[7] << 7 | d[8] << 14;
    const uint8_t* k = d + 9;
    int nk = len - 10;
    g_mask_func = 0x80 / 7 < nk && (k[0x80 / 7] >> (0x80 % 7) & 1);
  } else if (d[4] == 0x48) {
    g_sent.emplace_back(d, d + len);
  } else if (d[4] == 0x3A && len >= 9 && d[7] == 0) {
    ++g_closes;
  }
}
static bool sent48(int t, int k) {
  for (auto& m : g_sent)
    if (m.size() >= 10 && m[6] == t && m[7] == k) return true;
  return false;
}

// ---- the clock, the Rytm's messages, the control core's pass and the card core's turn
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void from_rytm(const std::vector<uint8_t>& body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void tick(int n = 4) {
  for (int i = 0; i < n; ++i) at(g_us + 30000), ui::remote_tick(now_ms());
}
static void down(int code) { from_rytm({0x31, 1, (uint8_t)(code >> 7), (uint8_t)(code & 0x7F), 1}), tick(1); }
static void up(int code) { from_rytm({0x31, 1, (uint8_t)(code >> 7), (uint8_t)(code & 0x7F), 0}), tick(1); }
static void tap(int code) { down(code), up(code); }
static void turn(int e, int delta, bool func = false) { from_rytm({0x32, 1, (uint8_t)e, (uint8_t)(64 + delta), (uint8_t)(func ? 1 : 0)}), tick(1); }

static void pass() {
  host_card_core(3);
  for (int k = 0; k < 4; ++k) idle_stream();
  host_card_core(0);
  for (int k = 0; k < 16 && cardq::service(); ++k) {}
  host_card_core(3);
  for (int k = 0; k < 4 && lib::step(); ++k) {}
  at(g_us + 1000);
}
static SV g_v;
static const SV& vw() {
  sedit::view(&g_v);
  return g_v;
}
static bool settled(sedit::State s) { return s == sedit::kReady || s == sedit::kError || s == sedit::kClosed; }
static void run() {
  for (int i = 0; i < 200000 && !settled(vw().st); ++i) pass();
  CHECK(settled(vw().st));
}
static int code(const char* row) {
  int c = -1;
  CHECK(pools::parse_row(row, &c));
  return c;
}
static void put_row(const char* row, const char* path) {
  char c[256], o[256];
  snprintf(c, sizeof c, "pool=smp:set:%s:%s", row, path);
  host_card_core(0);
  lib::touched(path, nullptr, 0);
  host_card_core(3);
  for (int i = 0; i < 200000 && (i < 20 || lib::busy()); ++i) pass();
  CHECK(pools::command(c, o, sizeof o));
  for (int i = 0; i < 4000 && (pools::pending() || strcmp(pools::at(pools::kSamples, code(row)).path, path)); ++i) pass();
  CHECK(!strcmp(pools::at(pools::kSamples, code(row)).path, path));
}
// voice 0's DSP SAMPLE report: P11 SMP 1 (A001), STA 0, END 120
static void params() {
  float p[16] = {0, 64, 64, 64, 0, 0, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100};
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(kDspSample | 0)};
  for (float x : p) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}

// ---- the page as the Rytm would show it
static ui::Canvas g_c;
static void draw() { ui::kSeditPage.draw(g_c, 0, 1, now_ms() | 1); }
static bool shows(const char* s) {
  draw();
  for (const ui::TextItem& t : g_c.txt)
    if (t.s[0] && strstr(t.s, s)) return true;
  return false;
}
static void png(const char* name) {
  draw();
  CHECK(scr_items(g_c) < 64);   // PNGs to $RUI_OUT, default build/rui_sedit
  static std::string dir = rui_png_dir("RUI_OUT", "build/rui_sedit");
  uint8_t fb[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  rui_png(dir, name, fb);
  FILE* fp = fopen((dir + "/" + name).c_str(), "rb");
  CHECK(fp);
  fseek(fp, 0, SEEK_END);
  CHECK(ftell(fp) > 0);
  fclose(fp);
  for (const ui::TextItem& t : g_c.txt) CHECK(strlen(t.s) <= 32);
}
static void cmd_close() {
  char out[256];
  CHECK(nm::command("sedit=close", out, sizeof out));
}
static bool title_is(const char* t) { return ui::remote_page_title() && !strcmp(ui::remote_page_title(), t); }

static void t_open_from_src() {
  ui::remote_reset();
  ui::sedit_page_reset();
  from_rytm({0x33, 1, 0, 1, 49, (uint8_t)(kDspSample | 0)});   // track 0 (voice 0): DSP SAMPLE (after the reset)
  CHECK((ui::remote_track_machine(0) & 0x78) == kDspSample && ui::remote_track_voice(0) == 0);
  from_rytm({0x29, 1, 0, 1});   // track 0's stock SRC page shows: the SRC view
  tick();
  CHECK(ui::remote_src_track() == 0 && !ui::remote_open());
  CHECK(!ui::sedit_src_push(0, 0x72, true));   // C: not SAMPLE EDIT's
  down(0x71);   // push B: the editor opens on the track's sample, the page asked open
  up(0x71);
  CHECK(vw().st == sedit::kLoading && vw().has_track && vw().track == 0);
  from_rytm({0x30, 1, 0, 1, 0});   // the Rytm opens the page
  tick();
  CHECK(title_is("SAMPLE EDIT"));
  CHECK(shows("LOADING"));
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == kN && vw().a == 0 && vw().b == kN);
  tick();
  CHECK(shows("DO") && shows("BACK") && shows("CROP") && shows("KICK.WAV")   /* (the Rytm's font: capitals) */);
  png("sedit-ready.png");
}

// 0.5.0 screens (the UI system, AROUND): names over the wave, the function's tag, the key boxes, the turned knob's name on
// the title bar and its value in the bottom bar, the modals without the page's own text; always under the item budget
static const ui::TextItem* item(const char* s) { return scr_find(g_c, s); }
static void t_screens() {
  tick(80);
  draw();
  CHECK(scr_items(g_c) < 64);
  const char* nm[4] = {"STRT", "END", "ZOOM", "VIEW"};
  for (int i = 0; i < 4; ++i) CHECK(item(nm[i]) && item(nm[i])->x == i * 32 + 16 && item(nm[i])->y == 10 && (item(nm[i])->flags & 3) == ui::kAlignCentre);
  CHECK(item("CROP") && (item("CROP")->flags & ui::kTextInvert) && item("CROP")->y == 49);   // the function as a filled tag
  CHECK(!strcmp(g_c.txt[ui::kSlotTitle].s, "SAMPLE EDIT") && !strcmp(g_c.txt[ui::kSlotRight].s, "KICK.WAV"));
  CHECK(!strcmp(g_c.txt[ui::kSlotFoot].s, "YES") && !strcmp(g_c.txt[ui::kSlotFoot + 1].s, "DO") && !strcmp(g_c.txt[ui::kSlotFoot + 2].s, "NO") &&
        !strcmp(g_c.txt[ui::kSlotFoot + 3].s, "BACK"));   // nothing to undo, nothing unsaved: NO leaves
  CHECK(!item("E:CROP  YES:DO"));
  png("sedit-idle.png");
  down(0x80);   // FUNC held: A and B jump to the transients, YES saves
  draw();
  CHECK(item("TRNS") && !item("STRT") && !item("END") && item("ZOOM") && item("VIEW"));
  CHECK(!strcmp(g_c.txt[ui::kSlotFoot + 1].s, "SAVE") && !strcmp(g_c.txt[ui::kSlotFoot + 3].s, "BACK"));
  png("sedit-func.png");
  up(0x80);
  turn(0, -1);   // A at its left limit: the name and value show, nothing moves
  draw();
  CHECK(!strcmp(g_c.txt[ui::kSlotTitle].s, "REGION START") && !strcmp(g_c.txt[ui::kSlotFoot].s, "START") && !strcmp(g_c.txt[ui::kSlotFoot + 1].s, "0.000S") &&
        g_c.txt[ui::kSlotFoot + 1].x == 70 && (g_c.txt[ui::kSlotFoot + 1].flags & ui::kTextInvert));
  CHECK(item("STRT") && (item("STRT")->flags & ui::kTextInvert) && !(item("END")->flags & ui::kTextInvert) && scr_items(g_c) < 64);
  png("sedit-turning-a.png");
  turn(4, +1);   // E: the function, its whole name
  draw();
  CHECK(!strcmp(g_c.txt[ui::kSlotTitle].s, "FUNCTION") && !strcmp(g_c.txt[ui::kSlotFoot].s, "FUNC") && !strcmp(g_c.txt[ui::kSlotFoot + 1].s, "NORMALISE") && item("NORM"));
  turn(4, -1);
  CHECK(vw().fn == sedit::kCrop);
  tap(0x43);   // any key ends the turn display
  draw();
  CHECK(!strcmp(g_c.txt[ui::kSlotTitle].s, "SAMPLE EDIT") && !strcmp(g_c.txt[ui::kSlotFoot].s, "YES"));
}

static void t_push_under_side() {   // 0.5.2: a knob press under PAGE + T15 / T16 is the scene edit's, not the SRC view's
  ui::remote_reset();
  ui::sedit_page_reset();
  from_rytm({0x33, 1, 0, 1, 49, (uint8_t)(kDspSample | 0)});
  from_rytm({0x29, 1, 0, 1});
  tick();
  CHECK(ui::remote_src_track() == 0 && !ui::remote_open());
  cmd_close();
  tick();
  CHECK(vw().st == sedit::kClosed);
  scene::set_edit_side(1);
  down(0x71);   // under the side: no SAMPLE EDIT
  CHECK(vw().st == sedit::kClosed);
  scene::set_edit_side(-1);   // the side let go before the release: the release is still the gesture's
  up(0x71);
  CHECK(vw().st == sedit::kClosed);
  down(0x71);   // no side: the view's push as before
  up(0x71);
  CHECK(vw().st != sedit::kClosed);
  cmd_close();
  tick();
  printf("sedit: a push under the held side does not reach the SRC view\n");
}

static void t_send_mask() {   // A..E and FUNC (FUNC + YES, FUNC + A / B / D) are the page's
  CHECK(g_mask_encs == 0x1F && g_mask_func);
}

// sedit-050 #4 / #6 / #7 / #8: A START, B END (zero crossings; FUNC: transients), C ZOOM, D the window's place (FUNC:
// coarse), E the function. kick.wav: 0.5 s of silence (every frame a zero: the place asked for), then the sine.
static void t_knobs() {
  for (int i = 0; i < 20; ++i) pass();   // (the transients found)
  SV v0 = vw();
  uint32_t px = std::max<uint32_t>(1, (v0.wb - v0.wa) / 128);
  turn(0, +3);   // A: the start
  CHECK(vw().a == v0.a + 3 * px);
  turn(0, +1, true);   // FUNC + A: the next transient, the sine's start (a zero itself)
  CHECK(vw().a == 24000);
  turn(0, +1, true);   // none after it
  CHECK(vw().a == 24000 && shows("NO MORE TRANSIENTS"));
  turn(0, -1, true);   // none before it either: kept
  CHECK(vw().a == 24000);
  turn(1, -2);   // B: the end, on a crossing of the sine
  CHECK(vw().b < v0.b && vw().b + sedit::kSnapReach >= v0.b - 2 * px && vw().b <= v0.b - 2 * px + sedit::kSnapReach);
  turn(1, -1, true);   // FUNC + B: the previous transient after the start: none
  CHECK(vw().b + sedit::kSnapReach >= v0.b - 2 * px);
  tick(80);   // (past the message)
  turn(4, +2);   // E: the function
  CHECK(vw().fn == sedit::kTrim);
  CHECK(shows("TRIM") && !shows("CROP"));
  turn(4, -5);   // clamped at CROP
  CHECK(vw().fn == sedit::kCrop);
  CHECK(vw().zoom == 0 && vw().wa == 0);
  turn(3, -1);   // D at X1: nowhere to go
  CHECK(vw().wa == 0 && vw().wb == kN);
  turn(2, +3);   // C: one zoom level a turn message, around the end (moved last)
  CHECK(vw().zoom == 1 && vw().wb - vw().wa == kN / 2 && vw().wb == kN);
  for (int i = 0; i < 50; ++i) pass();
  png("sedit-zoomed.png");   // (the window's place: the bar under the outline)
  turn(3, -1);   // D: a sixteenth of the window
  CHECK(vw().wa == kN / 2 - kN / 32 && vw().wb == kN - kN / 32);
  turn(3, -1, true);   // FUNC + D: 8 of them
  CHECK(vw().wa == kN / 2 - 9 * (kN / 32));
  turn(3, -60);   // clamped at the start
  CHECK(vw().wa == 0 && vw().wb == kN / 2);
  // #6: the end out of the window: moving it scrolls the window so the end shows, an eighth inside
  turn(1, -1);
  CHECK(vw().b > vw().wa && vw().b <= vw().wb && vw().wb - vw().wa == kN / 2);
  turn(2, -1);
  CHECK(vw().zoom == 0 && vw().wa == 0 && vw().wb == kN);
  sedit::set_start(0);
  sedit::set_end(kN);
}

static void t_yes_apply() {
  sedit::set_start(12000);
  sedit::set_end(36000);
  CHECK(vw().fn == sedit::kCrop);
  tap(0x44);
  CHECK(vw().st == sedit::kBusy || vw().st == sedit::kReady);
  if (vw().st == sedit::kBusy) png("sedit-busy.png");
  run();
  CHECK(vw().frames == 24000 && vw().undo && vw().dirty);
}

static void t_func_yes_save() {
  down(0x80);
  tap(0x44);
  up(0x80);
  CHECK(shows("SAVE+USE") && shows("CANCEL"));
  CHECK(shows("SAVE REGION  A: PICK") && shows("TRACK USES IT") && shows("NEW FILE") && shows("NOTHING SAVED"));
  CHECK(!shows("STRT") && !shows("ZOOM") && !shows("VIEW") && scr_items(g_c) < 64);   // under the box the page's own text is not put
  CHECK(!strcmp(g_c.txt[ui::kSlotFoot].s, "YES") && !strcmp(g_c.txt[ui::kSlotFoot + 1].s, "OK") && !strcmp(g_c.txt[ui::kSlotFoot + 3].s, "BACK"));
  png("sedit-save.png");
  turn(0, +5);   // A: the pick, clamped at CANCEL
  turn(0, -5);   // back to SAVE+USE, the default
  g_sent.clear();
  tap(0x44);
  CHECK(!shows("SAVE+USE"));
  run();
  CHECK(exists("/Samples/k/kick-ed1.wav") && !strcmp(vw().name, "kick-ed1.wav") && !vw().dirty);
  CHECK(sent48(0, 10));   // SAVE + USE: the track's SMP written
  // FUNC + YES, NO: the choice cancelled, nothing saved
  down(0x80);
  tap(0x44);
  up(0x80);
  CHECK(shows("SAVE+USE"));
  tap(0x45);
  CHECK(!shows("SAVE+USE") && title_is("SAMPLE EDIT"));
  tick(80);   // (past a message's 2 s)
  CHECK(shows("CROP") && shows("DO"));
  CHECK(!exists("/Samples/k/kick-ed2.wav"));
}

static void t_pool_save_only() {
  ui::RecInfo ri = {};
  ri.state = ui::kRecIdle;   // (POOL shows where a recorder is: the Pi)
  ui::remote_rec(ri);
  ui::remote_show_page("POOL");
  tick();
  CHECK(title_is("POOL"));
  ui::pool_page_set(0, 0, 1, 7);   // SAMPLES, A001, EDIT
  tap(0x44);
  CHECK(title_is("SAMPLE EDIT"));
  run();
  CHECK(vw().st == sedit::kReady && !vw().has_track && !strcmp(vw().name, "kick.wav") && vw().a == 0 && vw().b == kN);
  down(0x80);
  tap(0x44);
  up(0x80);
  CHECK(!shows("SAVE+USE") && shows("SAVE") && shows("CANCEL"));
  tap(0x45);
  CHECK(!shows("CANCEL"));
  // sedit-050 #2: push A plays the region from a pool open too (on a DSP SAMPLE track: track 0's voice 0)
  for (int i = 0; i < 20; ++i) pass();
  tap(0x70);
  CHECK(!shows("NO SAMPLE TRACK") && !shows("BUSY"));
  CHECK(from_ram[0] && engines[0].playing());
  static int16_t ring[kBlock * 400][kVoices];   // (played out: a note still reading a play buffer holds its refill)
  uint64_t mach[kMachines] = {0};
  for (int i = 0; i < 10000 && engines[0].playing(); ++i) render_voices(ring, kBlock * 400, 0, 0, kVoices, mach);
  CHECK(!engines[0].playing());
}

static void t_no_undo_then_discard() {
  turn(4, +6);   // E: GAIN +3
  CHECK(vw().fn == sedit::kGainUp);
  tap(0x44);
  run();
  CHECK(vw().undo);
  tap(0x45);   // NO: undo
  run();
  CHECK(!vw().undo && title_is("SAMPLE EDIT"));
  tap(0x44);
  run();
  tap(0x44);
  run();
  tap(0x45);   // the second undone
  run();
  CHECK(!vw().undo && vw().dirty);
  tick(80);
  draw();
  CHECK(!strcmp(g_c.txt[ui::kSlotFoot + 3].s, "DISCARD"));   // what NO will do: it asks
  tap(0x45);   // nothing to undo, an edit: asked
  CHECK(shows("DISCARD EDIT?") && vw().st == sedit::kReady);
  CHECK(shows("UNSAVED CHANGES") && !shows("STRT") && !strcmp(g_c.txt[ui::kSlotFoot + 1].s, "DISCARD") && !strcmp(g_c.txt[ui::kSlotFoot + 3].s, "KEEP"));
  png("sedit-discard.png");
  tap(0x45);   // NO: kept
  CHECK(!shows("DISCARD EDIT?") && vw().st == sedit::kReady);
  tap(0x45);
  CHECK(shows("DISCARD EDIT?"));
  tap(0x44);   // YES: dropped, back to the page before
  CHECK(vw().st == sedit::kClosed);
  CHECK(title_is("POOL"));
}

static void t_close_clean() {   // NO with nothing changed leaves at once
  ui::pool_page_set(0, 0, 1, 7);
  tap(0x44);
  run();
  CHECK(title_is("SAMPLE EDIT") && !vw().dirty);
  tap(0x45);
  CHECK(vw().st == sedit::kClosed && title_is("POOL"));
}

// sedit050 (the 0.4.4 merge): POOL put up by its shortcut, EDIT, NO: back on POOL (not closed), and POOL keeps
// nav044's rule: one more NO closes the page
static void t_back_keeps_shortcut() {
  ui::remote_show_page("POOL");
  tick();
  CHECK(title_is("POOL"));
  ui::pool_page_set(0, 0, 1, 7);
  tap(0x44);
  run();
  CHECK(title_is("SAMPLE EDIT") && !vw().dirty);
  int closes = g_closes;
  tap(0x45);
  CHECK(vw().st == sedit::kClosed && title_is("POOL") && g_closes == closes);
  tap(0x45);
  CHECK(g_closes == closes + 1);
}

static void t_guard_and_menu() {   // an edit with changes is not dropped by another open; the MENU's open goes back to it
  ui::sedit_page_open(0, -1);
  run();
  CHECK(title_is("SAMPLE EDIT") && vw().has_track && vw().track == 0);
  turn(4, +7);   // E: GAIN -3
  tap(0x44);
  run();
  CHECK(vw().dirty);
  ui::remote_show_page("POOL");
  tick();
  ui::pool_page_set(0, 0, 1, 7);
  tap(0x44);
  CHECK(title_is("SAMPLE EDIT") && vw().has_track && vw().dirty && shows("SAVE OR DROP THE EDIT"));
  ui::remote_show_page("POOL");
  tick();
  ui::sedit_page_open(-1, -1);   // the MENU's SAMPLE EDIT: the edit open
  tick();
  CHECK(title_is("SAMPLE EDIT") && vw().has_track && vw().dirty && vw().undo);
  tap(0x44);   // a second apply: an edit left after the undo
  run();
  tap(0x45);   // undo
  run();
  CHECK(vw().dirty && !vw().undo);
  tap(0x45);   // asked
  CHECK(shows("DISCARD EDIT?"));
  tap(0x44);   // dropped
  CHECK(vw().st == sedit::kClosed && !title_is("SAMPLE EDIT"));
}

static void t_refused() {   // a pool row with no sample: the POOL page says so; a track that is not DSP SAMPLE: the page says why
  ui::pool_page_set(0, 0, 9, 7);
  tap(0x44);
  CHECK(title_is("POOL") && vw().st == sedit::kClosed);
  ui::sedit_page_open(5, -1);   // track 5: no machine reported
  tick();
  CHECK(title_is("SAMPLE EDIT") && shows("NOT A SAMPLE TRACK"));
  tap(0x45);
  CHECK(title_is("POOL"));
  ui::sedit_page_open(-1, -1);   // the MENU's, no track selected on the Rytm yet
  tick();
  CHECK(ui::remote_selected_track(nullptr) < 0 && title_is("SAMPLE EDIT") && shows("NO TRACK SELECTED"));
  tap(0x45);
  CHECK(!title_is("SAMPLE EDIT") && vw().st == sedit::kClosed);
}

// The touchscreen with an edit open: VOICE's key (UP + T4) leads to SAMPLE EDIT; a track change while on VOICE keeps
// VOICE following the track (the page the machine needs), never pulled to SAMPLE EDIT
static void select_track(int t) {
  std::vector<uint8_t> m = {0x49, 1, 2, (uint8_t)t, (uint8_t)(kDspSample | 0)};
  for (int i = 0; i < 12; ++i) m.push_back('A');
  from_rytm(m);
}
static void t_voice_follow() {
  ui::sedit_page_open(0, -1);
  run();
  CHECK(vw().st == sedit::kReady && seditview_pi::Open());
  uint32_t seen = 0, tseen = 0;
  voice_pi::Seen(&seen, &tseen);
  select_track(1);
  CHECK(ui::remote_selected_track(nullptr) == 1);
  CHECK(voice_pi::Request(&seen, true, &tseen) == voice_pi::PageForMachine(ui::remote_track_machine(1)));
  CHECK(voice_pi::PageForMachine(ui::remote_track_machine(1)) != seditview_pi::kPage);
  select_track(0);
  CHECK(voice_pi::Request(&seen, true, &tseen) != seditview_pi::kPage);
  from_rytm({0x49, 1, 1, 4, 0});   // UP + T4: VOICE, an edit open: SAMPLE EDIT
  CHECK(voice_pi::Request(&seen, false, &tseen) == seditview_pi::kPage);
  cmd_close();
}

// The edit closed elsewhere (the touchscreen, the console) while the page asked DISCARD EDIT?: the question goes with
// it, so the next edit's YES is not taken as its answer
static void t_closed_elsewhere() {
  ui::sedit_page_open(0, -1);
  run();
  turn(4, +7);   // E: GAIN -3, twice
  tap(0x44);
  run();
  tap(0x44);
  run();
  tap(0x45);   // undo: the first apply's edit left
  run();
  tap(0x45);   // asked
  CHECK(shows("DISCARD EDIT?"));
  cmd_close();   // the console's close
  CHECK(vw().st == sedit::kClosed);
  tick();
  char out[256];
  CHECK(nm::command("sedit=open:track:0", out, sizeof out));   // an edit opened elsewhere
  run();
  ui::remote_show_page("SAMPLE EDIT");
  tick();
  CHECK(title_is("SAMPLE EDIT") && !shows("DISCARD EDIT?"));
  tap(0x44);   // YES: the function, not a discard
  run();
  CHECK(vw().st == sedit::kReady && vw().undo);
  tap(0x45);   // undo, then NO leaves
  run();
  tap(0x45);
  CHECK(vw().st == sedit::kClosed);
}

// The owner's case (2026-11-11, Pi 5): BT (track 4, voice 1) DSP SAMPLE plays SMP A008, its SRC view draws the waveform,
// push B said NO SAMPLE: open_track's slot -> row used the editor's own kPool (SavePass, 2), not the pool's 127. Slot 1
// (A001, every test above) maps the same either way; A008 and bank B's rows did not.
static void dsp_report(int vi, int smp, int lo) {   // voice vi's DSP SAMPLE report: SMP smp + lo / 128, STA 0, END 120
  float p[16] = {0, 64, 64, 64, 0, 0, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100};
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(kDspSample | vi)};
  for (int k = 0; k < 16; ++k) {
    int i = (int)p[k];
    m.push_back((uint8_t)(k == 10 ? smp : i)), m.push_back((uint8_t)(k == 10 ? lo : 0));
  }
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void t_open_other_slots() {
  if (vw().st != sedit::kClosed) cmd_close();
  ui::remote_reset();
  ui::sedit_page_reset();
  write_wav16("/Samples/k/beat.wav", kN / 2);
  write_wav16("/Samples/k/rim.wav", kN / 4);
  put_row("A008", "/Samples/k/beat.wav");
  put_row("B002", "/Samples/k/rim.wav");
  pool_sync();
  from_rytm({0x33, 1, 4, 1, 49, (uint8_t)(kDspSample | 1)});   // BT (track 4, voice 1): DSP SAMPLE
  CHECK(ui::remote_track_voice(4) == 1);
  dsp_report(1, 8, 0);   // SMP A008
  CHECK(pool_slot(v[1].p[10]) == 8);
  from_rytm({0x29, 1, 4, 1});   // BT's SRC view
  tick();
  CHECK(ui::remote_src_track() == 4);
  down(0x71);
  up(0x71);
  CHECK(!shows("NO SAMPLE"));
  CHECK(vw().st == sedit::kLoading && vw().has_track && vw().track == 4);
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == kN / 2);   // beat.wav, A008's
  tick();
  CHECK(shows("BEAT.WAV"));
  cmd_close();
  dsp_report(1, 2, 1 << 4);   // SMP B002: slot 129
  CHECK(pool_slot(v[1].p[10]) == kPool + 2);
  CHECK(sedit::open_track(4) == 0);
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == kN / 4);   // rim.wav, B002's (not A001's kick)
  cmd_close();
  params();   // voice 0 back on A001
}

// 0.5.0: a SAMPLER track (the Rytm's machine 43 on DSP SAMPLE's voice byte): the push of B on its SRC view opens the
// editor on its sample as on DSP SAMPLE, the page's SAVE+USE writes its SMP (P11)
static void t_sampler_src() {
  ui::remote_reset();
  ui::sedit_page_reset();
  cmd_close();
  for (int i = 0; i < 20; ++i) pass();
  from_rytm({0x33, 1, 0, 1, kRytmSampler, (uint8_t)(kDspSample | 0)});   // track 0 (voice 0): SAMPLER
  params();
  CHECK(ui::remote_track_smpl(0) && ui::remote_track_voice(0) == 0);
  from_rytm({0x29, 1, 0, 1});   // its SRC view
  tick();
  CHECK(ui::remote_src_track() == 0);
  down(0x71);
  up(0x71);
  CHECK(vw().has_track && vw().track == 0 && vw().st != sedit::kClosed && !shows("NOT A"));
  from_rytm({0x30, 1, 0, 1, 0});
  tick();
  run();
  CHECK(title_is("SAMPLE EDIT") && vw().st == sedit::kReady && vw().frames == kN);
  turn(0, +1);   // a change: the save has something to write
  tap(0x44);
  run();
  down(0x80);
  tap(0x44);
  up(0x80);
  CHECK(shows("SAVE+USE"));
  g_sent.clear();
  tap(0x44);
  run();
  CHECK(sent48(0, 10) && !vw().dirty);   // SAVE + USE: SAMPLER's SMP (P11)
  cmd_close();
  for (int i = 0; i < 20; ++i) pass();
  from_rytm({0x33, 1, 0, 1, 49, (uint8_t)(kDspSample | 0)});
  printf("sedit page: SAMPLER - B on its SRC view opens its sample, SAVE+USE writes its SMP\n");
}

// owner 2026-11-15 (hardware): a new SAMPLER on a track, a sample picked in the pool list (YES), SAMPLE EDIT said NO
// SAMPLE until the sample had played once. The voice is idle: the Rytm's 0x03 reports come only while it plays (its first
// one, at the machine's choice, has SMP OFF), and the pick reaches the host as the SRC view's 2A (P11). SAMPLE EDIT
// takes the kit's SMP from it with no trig (SAMPLER's waveform on the SRC view already drew the 2A's: test_ui_wave).
static void t_sampler_idle_pick() {
  if (vw().st != sedit::kClosed) cmd_close();
  ui::remote_reset();
  ui::sedit_page_reset();
  for (int i = 0; i < 20; ++i) pass();
  from_rytm({0x33, 1, 4, 1, kRytmSampler, (uint8_t)(kDspSample | 1)});   // BT (track 4, voice 1): a new SAMPLER
  dsp_report(1, 0, 0);   // its first report: SMP OFF
  CHECK(pool_slot(v[1].p[10]) == 0);
  from_rytm({0x29, 1, 4, 1});   // BT's SRC view (the SMPL page with the pool list)
  tick();
  from_rytm({0x2A, 1, 4, 10, 1, 8, 0});   // the pick: SMP A008 (beat.wav), no trig
  tick();
  CHECK(pool_slot(v[1].p[10]) == 0);   // (the voice's state untouched: SAMPLE EDIT reads the kit's SMP, the 2A's)
  ui::sedit_page_open(4, -1);
  CHECK(!shows("NO SAMPLE") && vw().has_track && vw().track == 4 && vw().st != sedit::kClosed);
  from_rytm({0x30, 1, 0, 1, 0});
  tick();
  run();
  CHECK(title_is("SAMPLE EDIT") && vw().st == sedit::kReady && vw().frames == kN / 2);
  tick();
  CHECK(shows("BEAT.WAV"));
  cmd_close();
  from_rytm({0x2A, 1, 4, 10, 1, 2, 1 << 4});   // another pick, bank B: B002 (rim.wav)
  tick();
  CHECK(sedit::open_track(4) == 0);
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == kN / 4);
  cmd_close();
  // a report after the 2A (the voice played): its SMP is the newer word
  dsp_report(1, 8, 0);
  CHECK(ui::remote_track_src14_fresh(4) == -1 && sedit::open_track(4) == 0);
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == kN / 2);   // A008 (the report's), not B002 (the older 2A)
  cmd_close();
  // a kit change: the old kit's SMP is forgotten until its 2A comes again
  from_rytm({0x2A, 1, 4, 10, 1, 2, 1 << 4});
  CHECK(ui::remote_track_src14_fresh(4) >= 0);
  from_rytm({0x3B, 1, 9});
  CHECK(ui::remote_track_src14(4) == -1);
  from_rytm({0x30, 1, 0, 0, 0});
  from_rytm({0x33, 1, 4, 1, 49, 0x7F});
  CHECK(ui::remote_track_src14(4) == -1);   // (another machine: its knob is no SMP)
  // a voice shared by two SAMPLER tracks (RS and CP, voice 7): CP's trig reports on the voice after RS's pick; RS's
  // edit still opens RS's pick (the report may be CP's)
  from_rytm({0x33, 1, 2, 2, kRytmSampler, (uint8_t)(kDspSample | 7), kRytmSampler, (uint8_t)(kDspSample | 7)});
  from_rytm({0x2A, 1, 2, 10, 1, 8, 0});   // RS: A008
  dsp_report(7, 2, 1 << 4);               // the voice: B002 (CP's trig)
  CHECK(ui::remote_track_src14_fresh(2) == (8 << 7) && sedit::open_track(2) == 0);
  run();
  CHECK(vw().st == sedit::kReady && vw().frames == kN / 2);   // A008
  cmd_close();
  from_rytm({0x33, 1, 2, 2, 0, 0x7F, 0, 0x7F});
  for (int i = 0; i < 20; ++i) pass();
  printf("sedit page: SAMPLER - a pool pick on an idle voice (2A) opens in SAMPLE EDIT with no trig\n");
}

// MAKE WT (docs/SAMPLE-EDIT.md): a push of B on a WAVETABLE track's SRC view opens table A's card file; FUNC + YES offers MAKE
// WT first (B the frames), YES makes the table, its row lands on the track's TBL (a 48 write), the editor closes itself and
// the Rytm's page goes back to the SRC view. A built-in table A opens the POOL page on the SAMPLES list instead, EDIT chosen:
// YES on a sample opens it for the voice; the new table goes onto the track the same way.
static void write_saw16(const char* path, uint32_t frames) {
  uint8_t h[samp::kWavHeaderMax];
  int hn = samp::wav_header(h, 1, 16, 48000, frames);
  std::vector<uint8_t> b(h, h + hn);
  uint8_t o[2];
  for (uint32_t f = 0; f < frames; ++f) {
    double a = 0;
    for (int k = 1; k <= 40; ++k) a += sin(2 * M_PI * k * 220.0 * f / 48000.0) / k;
    samp::put16(o, (float)(0.3 * a));
    b.insert(b.end(), o, o + 2);
  }
  FILE* fp = fopen(host_path(path).c_str(), "wb");
  CHECK(fp);
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
}
static void run_closed() {
  for (int i = 0; i < 400000 && vw().st != sedit::kClosed; ++i) pass();
  CHECK(vw().st == sedit::kClosed);
}
static void t_wavetable_make() {
  if (vw().st != sedit::kClosed) cmd_close();
  ui::remote_reset();
  ui::sedit_page_reset();
  for (int i = 0; i < 20; ++i) pass();
  CHECK(system(("mkdir -p '" + g_card + "/WAVETABLES'").c_str()) == 0);
  write_saw16("/WAVETABLES/own.wav", 24000);
  {
    char o[256];
    host_card_core(0);
    lib::touched("/WAVETABLES/own.wav", nullptr, 0);
    host_card_core(3);
    for (int i = 0; i < 200000 && (i < 20 || lib::busy()); ++i) pass();
    CHECK(pools::command("pool=wt:set:A007:/WAVETABLES/own.wav", o, sizeof o));
    for (int i = 0; i < 4000 && (pools::pending() || strcmp(pools::at(pools::kTables, code("A007")).path, "/WAVETABLES/own.wav")); ++i) pass();
  }
  for (int i = 0; i < 400; ++i) pass();   // (the table list composed from the pool)
  from_rytm({0x33, 1, 0, 1, 46, (uint8_t)(kWaveMachine | 0)});   // track 0 (voice 0): WAVETABLE
  CHECK((ui::remote_track_machine(0) & 0x78) == kWaveMachine && ui::remote_track_voice(0) == 0);
  from_rytm({0x29, 1, 0, 1});   // its SRC view
  tick();
  from_rytm({0x2A, 1, 0, 0, 1, 7, 0});   // TBL A007: the card file
  tick();
  CHECK(ui::remote_src_track() == 0 && !ui::remote_open());
  CHECK(!ui::sedit_src_push(0, 0x72, true) && !ui::sedit_src_push(0, 0x70, true));   // C and A: not SAMPLE EDIT's
  down(0x71);   // push B: SAMPLE EDIT on the table's file, for voice 0
  up(0x71);
  CHECK(vw().st == sedit::kLoading && vw().file && vw().wt_voice == 0 && vw().wt_track == 0 && !vw().has_track);
  from_rytm({0x30, 1, 0, 1, 0});
  tick();
  run();
  CHECK(title_is("SAMPLE EDIT") && vw().st == sedit::kReady && vw().frames == 24000 && vw().file);
  tick();
  CHECK(shows("OWN.WAV"));
  down(0x80);   // FUNC + YES: the choice, MAKE WT first
  tap(0x44);
  up(0x80);
  CHECK(shows("MAKE WT") && shows("SAVE") && shows("CANCEL") && !shows("SAVE+USE") && shows("B: 64 FRAMES"));
  png("sedit-make-wt.png");
  turn(1, -1);   // B: 32 frames
  CHECK(shows("B: 32 FRAMES") && vw().wt_frames == 32);
  turn(1, +1), turn(1, +1), turn(1, +1), turn(1, +1);   // (the end of the choice: 256)
  CHECK(shows("B: 256 FRAMES"));
  turn(1, -1), turn(1, -1), turn(1, -1);
  CHECK(shows("B: 32 FRAMES"));
  g_sent.clear();
  const int closes0 = g_closes;
  tap(0x44);   // YES: make
  CHECK(vw().st == sedit::kBusy && vw().making);
  tick();
  CHECK(shows("MAKE WT"));
  for (int i = 0; i < 400000 && vw().st != sedit::kReady && vw().st != sedit::kClosed; ++i) pass();
  tick();
  CHECK(vw().st == sedit::kReady && vw().wt_back && shows("WT A008 PITCHED 220HZ"));
  png("sedit-made-wt.png");
  CHECK(sent48(0, 0));   // the TBL written to the Rytm (P1 of track 0)
  CHECK(exists("/WAVETABLES/own-wt1.wav") && pools::at(pools::kTables, code("A008")).type == pools::kSample);
  run_closed();   // the editor closes itself 1.8 s later
  tick(6);
  CHECK(g_closes > closes0);   // and the Rytm's page goes back to the SRC view (it closes: 30 0, its SRC view again)
  from_rytm({0x30, 1, 0, 0, 0});
  from_rytm({0x29, 1, 0, 1});
  tick();
  // a built-in table A (A001 BASIC): B opens the POOL page on the SAMPLES list, EDIT chosen, a sample picked, the table made
  from_rytm({0x2A, 1, 0, 0, 1, 1, 0});
  tick();
  CHECK(ui::remote_src_track() == 0);
  down(0x71);
  up(0x71);
  CHECK(vw().st == sedit::kClosed);
  from_rytm({0x30, 1, 0, 1, 0});
  tick();
  CHECK(title_is("POOL"));
  tap(0x44);   // YES on the first sample (EDIT): its editor, for voice 0
  CHECK(vw().st == sedit::kLoading && !vw().file && vw().wt_voice == 0 && !vw().has_track);
  run();
  CHECK(title_is("SAMPLE EDIT") && vw().st == sedit::kReady);
  g_sent.clear();
  down(0x80);
  tap(0x44);
  up(0x80);
  CHECK(shows("MAKE WT"));
  tap(0x44);
  for (int i = 0; i < 400000 && vw().st != sedit::kReady && vw().st != sedit::kClosed; ++i) pass();
  tick();
  CHECK(vw().st == sedit::kReady && sent48(0, 0) && strstr(vw().msg, "WT A009"));
  run_closed();
  CHECK(exists("/WAVETABLES/kick-wt1.wav"));
  tick(6);
  CHECK(!title_is("SAMPLE EDIT"));
  // the editor opened another way keeps no voice: a SAVE choice has no MAKE WT first
  CHECK(sedit::open_code(code("A001")) == 0);
  run();
  CHECK(vw().wt_voice == -1);
  ui::sedit_page_open(-1, -1);
  tick();
  down(0x80);
  tap(0x44);
  up(0x80);
  CHECK(shows("SAVE") && shows("MAKE WT"));
  tap(0x45);
  cmd_close();
  printf("sedit page: WAVETABLE - B on its SRC view opens table A's file or the SAMPLES list, MAKE WT puts the new row on its TBL\n");
}

int main() {
  char tmpl[] = "/tmp/sedit-page-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/k' '" + g_card + "/.rytm'").c_str()) == 0);
  setenv("CORE_POOL_RAM_MB", "64", 1);
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  hooks.to_rytm = out;
  write_wav16("/Samples/k/kick.wav", kN);
  cardq::reset();
  cardq::set_worker(true);
  CHECK(engine_begin() && card_ok);
  snprintf(core::prj().key, sizeof core::prj().key, "LIVE");
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) pass();
  for (int i = 0; i < 4000 && !(pools::ready() && !strcmp(pools::project(), "LIVE")); ++i) pass();
  CHECK(pools::ready());
  put_row("A001", "/Samples/k/kick.wav");
  pool_sync();
  params();

  t_open_from_src();
  t_send_mask();
  t_screens();
  t_knobs();
  t_yes_apply();
  t_func_yes_save();
  t_pool_save_only();
  t_no_undo_then_discard();
  t_close_clean();
  t_guard_and_menu();
  t_refused();
  t_voice_follow();
  t_closed_elsewhere();
  t_back_keeps_shortcut();
  t_open_other_slots();
  t_sampler_idle_pick();
  t_sampler_src();
  t_wavetable_make();
  t_push_under_side();
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);
  printf("test_sedit_page: PASS\n");
  return 0;
}
