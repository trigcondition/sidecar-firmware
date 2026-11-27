// core/tests/test_loop_page.cpp - the LOOP page (core/ui/page_loop.cpp, spec-050 3.12) through the remote host, as
// test_morph_page.cpp: T1..T6 grab, T8 STOP (FUNC + T8 a cut at once), T9..T16 the grabs, YES KEEP, NO back, the encoders
// A..F, the trig LEDs, the refusal texts, and every text on the Rytm's screen fits (check.h's TEXT-FIT).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core.h"
#include "loop/loop.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/target.h"
#include "rui_png.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;

static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static int g_mask_encs = -1;
static uint32_t g_mask_trigs = 0;
static bool g_mask_func = false;
static uint8_t g_led[16];
static bool g_leds_on = false;
static void out(const uint8_t* d, int len) {
  if (len < 7 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return;
  if (d[4] == 0x37 && len >= 10) {
    g_mask_encs = d[6] | d[7] << 7 | d[8] << 14, g_mask_trigs = 0;
    const uint8_t* k = d + 9;
    int nk = len - 10;
    for (int c = 0; c < 16; ++c)
      if (c / 7 < nk && (k[c / 7] >> (c % 7) & 1)) g_mask_trigs |= 1u << c;
    g_mask_func = 0x80 / 7 < nk && (k[0x80 / 7] >> (0x80 % 7) & 1);
  } else if (d[4] == 0x34) {
    g_leds_on = len >= 24 && d[6] == 1;
    if (g_leds_on) memcpy(g_led, d + 7, 16);
  }
}
static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  engine_sysex(m, 10);
}
static void tick(int n = 4) {
  for (int i = 0; i < n; ++i) at(g_us + 30000), ui::remote_tick(now_ms());
}
static void down(int code) { from_rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 1), tick(1); }
static void up(int code) { from_rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 0), tick(1); }
static void tap(int code) { down(code), up(code); }
static void turn(int e, int delta, bool func = false) { from_rytm(0x32, (uint8_t)e, (uint8_t)(64 + delta), func ? 1 : 0), tick(1); }

// ---- the looper's fixture: a fake sequencer clock (120 BPM), a ring of 2M frames fed as the Rytm's IN would
static const uint32_t kRing = 1u << 21;
static uint64_t g_now = 0, g_fed = 0;
static double g_origin = 10000, g_per16 = 6000;
static bool g_beats = true;
static std::vector<int32_t> g_ring;
static std::vector<uint8_t> g_store;
static bool env_beat(double* o, double* p, int* l) {
  *l = 384;
  if (!g_beats) return false;
  *o = g_origin, *p = g_per16;
  return true;
}
static uint64_t env_frames() { return g_now; }
static uint16_t env_muted() { return 0; }
static void feed() {
  while (g_fed + 6 + 1000 <= g_now) {
    int32_t b[14];
    for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(g_fed + i + 1), b[2 * i + 1] = -b[2 * i];
    loop::in_block(b, 7, g_fed + 6 + 1000, 0);
    g_fed += 7;
  }
}
static void run_frames(uint64_t n) {   // the clock on, the loop ticking each ms
  uint64_t end = g_now + n;
  while (g_now < end) {
    g_now += 48;
    feed();
    loop::tick(0);
  }
}
static void bars(double n) { run_frames((uint64_t)(n * 16 * g_per16)); }

static ui::Canvas g_c;
static void draw() { ui::kLoopPage.draw(g_c, 0, 1, now_ms() | 1); }
static bool shows(const char* s) {
  draw();
  for (const ui::TextItem& t : g_c.txt)
    if (t.s[0] && strstr(t.s, s)) return true;
  return false;
}
static void png(const char* name) {
  static std::string dir = rui_png_dir("RUI_OUT", "build/rui_loop");
  uint8_t fb[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  rui_png(dir, name, fb);
}

static void fresh_loop() {
  static loop::Env env;
  env.beat_map = env_beat, env.frames = env_frames, env.muted = env_muted;
  loop::set_env(&env);
  g_ring.assign(2 * (size_t)kRing, 0);
  g_store.assign(48u << 20, 0);
  loop::init(g_ring.data(), kRing, g_store.data(), g_store.size());
  loop::reset();
  mod::reset();
  g_now = 0, g_fed = 0, g_beats = true;
  bars(30.3);   // 30 bars in: a ring with history, a third into a bar
}
static void open_page() {
  fresh_loop();
  ui::remote_reset();
  ui::loop_page_reset();
  from_rytm(0x30, 0, 1, 0);
  tick();
  ui::remote_show_page("LOOP");
  tick();
  CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "LOOP"));
}

static void grab_keys() {
  open_page();
  CHECK(g_mask_trigs == 0xFFFF && g_mask_encs == 0x7F && g_mask_func);   // all 16 trig keys, A..G, FUNC
  CHECK(shows("LOOP") && shows("READY") && !shows("RING") && shows("T1-6 GRAB"));
  tap(0x01);   // T2: 1 bar
  bars(0.1);
  CHECK(loop::grabs() == 1 && loop::grab_at(0)->half_bars == loop::k1);
  tap(0x00);   // T1: 1/2 bar
  bars(0.1);
  CHECK(loop::grabs() == 2 && loop::grab_at(0)->half_bars == loop::kHalf && loop::grab_at(1)->half_bars == loop::k1);
  g_per16 = 9000;   // 80 BPM: 16 bars is 48 s, past the 43 s ring
  bars(0.3);
  tap(0x05);        // T6: refused ("16 BARS NEEDS n BPM")
  CHECK(shows("16 BARS NEEDS"));
  g_per16 = 6000;
  bars(0.5);
  tap(0x03);   // T4: 4 bars: fits
  bars(0.1);
  CHECK(loop::grabs() == 3 && loop::grab_at(0)->half_bars == loop::k4);
  CHECK(shows(".5") && shows("4") && shows("PLAYING") == (loop::state() == loop::kPlaying));
  png("loop_main.png");
  g_beats = false;   // the transport stopped
  tap(0x01);
  CHECK(shows("PLAY FIRST"));
  g_beats = true;
  printf("  grab_keys: T1..T6 grab 1/2 .. 16 bars, a refusal says why, PLAY FIRST\n");
}

static void grabs_play_and_stop() {
  open_page();
  tap(0x01), bars(0.1), tap(0x02), bars(0.1);   // two grabs
  CHECK(loop::grabs() == 2);
  tap(0x08);   // T9: the newest (2 bars), played from the next bar line
  CHECK(loop::playing() == 0 && loop::state() != loop::kRing && ui::loop_page_sel() == 0);
  bars(1.2);
  CHECK(loop::state() == loop::kPlaying && shows("PLAYING"));
  tap(0x09);   // T10: the older one: a swap at the next bar line, the page shows it
  CHECK(ui::loop_page_sel() == 1);
  bars(1.2);
  CHECK(loop::playing() == 1 && loop::state() == loop::kPlaying);
  tap(0x0F);   // T16: no such grab: nothing
  CHECK(ui::loop_page_sel() == 1);
  // stop_fade_and_cut: T8 a fade at the next bar line; FUNC + T8 a cut at once
  tap(0x07);
  CHECK(loop::state() == loop::kFading);
  bars(1.5);
  CHECK(loop::state() == loop::kRing);
  tap(0x08), bars(1.2);
  CHECK(loop::state() == loop::kPlaying);
  down(0x80), tap(0x07), up(0x80);   // FUNC + T8
  CHECK(loop::state() == loop::kFading);
  bars(0.05);   // a cut: 10 ms
  CHECK(loop::state() == loop::kRing);
  printf("  grabs_play_and_stop: T9.. play a grab from the next bar line, T8 fades, FUNC + T8 cuts at once\n");
}

static void grab_leds() {
  open_page();
  CHECK(g_leds_on);
  CHECK(g_led[8] == 0 && g_led[0] != 0 && g_led[7] == 0);   // no grabs, T8 off, T1 dim white
  tap(0x01), bars(0.1), tap(0x02), bars(0.1);
  tap(0x09);   // T10 plays the older: green; T9 held: dim white
  bars(1.3);
  tick(2);
  CHECK(g_led[9] == 31 && g_led[8] == 107 && g_led[10] == 0 && g_led[7] == 15);
  char t[48];
  CHECK(loop::keep(0, t, sizeof t));
  loop::KeepJob job;
  CHECK(loop::keep_take(&job));
  loop::keep_finished(job.id, true, "take");
  loop::tick(0);
  tick(2);
  CHECK(g_led[8] == 47 && g_led[9] == 31);   // kept: blue (not while it plays)
  printf("  grab_leds: playing green, kept blue, held dim, empty off, T8 red while a loop is on\n");
}

static void keep_on_yes() {
  open_page();
  tap(0x01), bars(0.1);
  bars(0.5);
  CHECK(loop::grabs() == 1 && loop::grab_at(0)->copied);
  tap(0x44);   // YES: KEEP the grab shown
  loop::KeepJob job;
  CHECK(loop::keep_take(&job) && job.frames == loop::grab_at(0)->frames);
  CHECK(shows("KEEPING") || shows("T1-6"));   // (the loop's message while it writes)
  loop::keep_finished(job.id, true, "demo_take");
  loop::tick(0);
  tick(1);
  CHECK(loop::grab_at(0)->kept && shows("KEPT DEMO_TAKE"));
  tap(0x44);
  CHECK(shows("ALREADY KEPT"));
  CHECK(loop::keep_take(&job) == false);
  // NO: back (the page asks itself closed; nothing else happens)
  tap(0x45);
  printf("  keep_on_yes: YES asks KEEP for the grab shown; the take's name comes back on the screen\n");
}

static void encoders() {
  open_page();
  turn(0, 5);   // A: BAL 64 + 5
  CHECK(loop::bal() == 69 && shows("BAL:69"));
  turn(0, 1, true);   // FUNC x 8
  CHECK(loop::bal() == 77);
  for (int i = 0; i < 9; ++i) turn(0, -9);
  CHECK(loop::bal() == 0);
  turn(1, -3);   // B: LEV 100 - 3
  CHECK(loop::lev() == 97 && shows("LOOP LEVEL:97"));
  turn(2, 1);    // C: FADE 1 -> 2
  CHECK(loop::fade() == loop::kFade2);
  for (int i = 0; i < 3; ++i) turn(2, -3);
  CHECK(loop::fade() == loop::kFadeQuarter);
  turn(3, 1);    // D: AUTO-MUTE on, off
  CHECK(loop::auto_mute());
  turn(3, -1);
  CHECK(!loop::auto_mute());
  turn(4, 1);    // E: LIVE VIA OFF -> PERF 1
  CHECK(loop::live_via() == 1);
  for (int i = 0; i < 14; ++i) turn(4, 1);
  CHECK(loop::live_via() == 12);
  tap(0x01), bars(0.1), tap(0x02), bars(0.1), tap(0x03), bars(0.1);
  turn(5, 1);    // F: the grab shown
  CHECK(ui::loop_page_sel() == 1 && shows("2/3"));
  turn(5, 3);
  CHECK(ui::loop_page_sel() == 2);
  CHECK(shows("BAL") && shows("LEV") && shows("FADE") && shows("MUTE") && shows("LIVE") && shows("GRAB"));
  // G: OUT, in the second row's third cell of the 4-column grid (x 80 .. 112), POST by default, PRE and back
  CHECK(shows("OUT") && shows("POST") && loop::out() == loop::kOutPost);
  for (const ui::TextItem& t : g_c.txt)
    if (t.s[0] && !strcmp(t.s, "OUT")) CHECK(t.x >= 64 && t.x < 96 + 16 && t.y > 30);
  turn(6, 1);
  CHECK(loop::out() == loop::kOutPre && shows("PRE") && !shows("POST") && shows("OUT PRE"));
  turn(6, -1);
  CHECK(loop::out() == loop::kOutPost && shows("OUT POST"));
  // a manual BAL while a scene holds LOOP:BAL: the edit wins (mod::manual), and the encoders are scene-lockable
  CHECK(ui::kLoopPage.enc_tid(0) == mod::tid(mod::kLoop, 0, 0) && ui::kLoopPage.enc_tid(1) == mod::tid(mod::kLoop, 0, 1) && !ui::kLoopPage.enc_tid(2));
  png("loop_set.png");
  printf("  encoders: A BAL, B LEV, C FADE, D AUTO-MUTE, E LIVE VIA, F the grab shown, G OUT\n");
}

static void off_when_no_looper() {
  ui::remote_reset();
  ui::loop_page_reset();
  loop::init(nullptr, 0, nullptr, 0);
  CHECK(!loop::on());
  draw();
  CHECK(shows("LOOPER OFF"));
  bool listed = false;   // not in the page list while it is off
  for (int k = 0; k < ui::extra_pages(); ++k)
    if (!strcmp(ui::extra_page(k).title, "LOOP") && (!ui::extra_page(k).shown || ui::extra_page(k).shown())) listed = true;
  CHECK(!listed);
  printf("  off_when_no_looper: the page says so and is out of the list\n");
}

int main() {
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  engine_begin();
  grab_keys();
  grabs_play_and_stop();
  grab_leds();
  keep_on_yes();
  encoders();
  off_when_no_looper();
  bool menu = false;
  for (int i = 0; i < ui::menu_rows(); ++i) menu |= ui::menu_row_page(i) && !strcmp(ui::menu_row_page(i), "LOOP");
  CHECK(menu);   // a SIDECAR MENU row
  printf("test_loop_page: OK\n");
  return 0;
}
