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
#include "samp/dsp.h"
#include "samp/sedit.h"
#include "samp/sedit_dsp.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

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
static void png(const char* name) {   // PNGs to $RUI_OUT, default build/rui_sedit
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
  CHECK(shows("E:CROP  YES:DO") && shows("KICK.WAV")   /* (the Rytm's font: capitals) */);
  png("sedit-ready.png");
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
  CHECK(shows("E:TRIM"));
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
  CHECK(shows("E:CROP  YES:DO"));
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
  tap(0x45);   // nothing to undo, an edit: asked
  CHECK(shows("DISCARD EDIT?") && vw().st == sedit::kReady);
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
  snprintf(core::project, sizeof core::project, "LIVE");
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) pass();
  for (int i = 0; i < 4000 && !(pools::ready() && !strcmp(pools::project(), "LIVE")); ++i) pass();
  CHECK(pools::ready());
  put_row("A001", "/Samples/k/kick.wav");
  pool_sync();
  params();

  t_open_from_src();
  t_send_mask();
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
  t_sampler_src();
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);
  printf("test_sedit_page: PASS\n");
  return 0;
}
