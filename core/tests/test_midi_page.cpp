// core/tests/test_midi_page.cpp - the MIDI page (core/ui/page_midi.cpp, 0.5.0): SIDECAR MENU > MIDI on the Rytm's
// screen. Its rows: KEYBOARD ROUTE (SELECTED / CHANNEL), AUTO CHANNEL, COMPUTER NOTES (core/keys.h), MIDI LEARN (the
// routes, YES twice clears them; core/mod/learn.h), then what is plugged into the USB-A ports and where the last key
// went (read only). Knob A / UP / DOWN pick a row, YES or knob B change it; each change takes effect at once and is
// saved by idle_save (/.rytm/keys.txt, /.rytm/learn.txt) and read back at boot. Every text fits (check.h's TEXT-FIT).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "keys.h"
#include "mod/learn.h"
#include "rui_png.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void out(const uint8_t*, int) {}
static void fwd(uint8_t, uint8_t, uint8_t) {}
static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  engine_sysex(m, 10);
}
static void tick(int n = 2) {
  for (int i = 0; i < n; ++i) at(g_us + 30000), ui::remote_tick(now_ms());
}
static void tap(int code) { from_rytm(0x31, 0, (uint8_t)code, 1), from_rytm(0x31, 0, (uint8_t)code, 0), tick(); }
static void turn(int e, int delta) { from_rytm(0x32, (uint8_t)e, (uint8_t)(64 + delta), 0), tick(); }
enum { kUp = 0x42, kDown = 0x43, kYes = 0x44, kNo = 0x45 };
enum { kRoute, kAuto, kMac, kLearn, kUsbA, kLast };

static ui::Canvas g_c;
static void draw() { ui::kMidiPage.draw(g_c, 0, 1, now_ms() | 1); }
static bool shows(const char* s) {
  draw();
  for (const ui::TextItem& t : g_c.txt)
    if (t.s[0] && strstr(t.s, s)) return true;
  return false;
}
static void png(const char* name) {   // the frame as the Rytm shows it (PNGs to $RUI_OUT, default build/rui_midi)
  static std::string dir = rui_png_dir("RUI_OUT", "build/rui_midi");
  uint8_t fb[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  rui_png(dir, name, fb);
}
static bool is(const char* title) { return ui::remote_page_title() && !strcmp(ui::remote_page_title(), title); }
static std::string rd(const std::string& p) {
  std::string s;
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "<none>";
  char b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void flush() {   // the idle saves, as the engine runs them between blocks
  at(g_us + 1100000);
  for (int i = 0; i < 64; ++i) idle_save();
}
static int routes() {
  const learn::Route* r = nullptr;
  return learn::routes(&r);
}

// SIDECAR (UP + YES), YES: the MENU; DOWN to its MIDI row, YES: the MIDI page
static void open_page() {
  ui::remote_reset();
  ui::menu_reset();
  ui::midi_page_reset();
  from_rytm(0x30, 0, 1, 0);
  tick();
  tap(kYes);
  CHECK(is("MENU"));
  int row = -1;
  for (int i = 0; i < ui::menu_rows(); ++i)
    if (ui::menu_row_page(i) && !strcmp(ui::menu_row_page(i), "MIDI")) row = i;
  CHECK(row >= 0);
  for (int i = 0; i < row; ++i) tap(kDown);
  tap(kYes);
  CHECK(is("MIDI"));
}

static void menu_row_opens_the_page() {
  open_page();
  CHECK(shows("KEYBOARD ROUTE") && shows("SELECTED") && shows("AUTO CHANNEL") && shows("14") && shows("COMPUTER NOTES") &&
        shows("ON") && shows("MIDI LEARN") && shows("NONE") && shows("USB-A") && shows("MPKMINI +1") && shows("LAST") &&
        shows("YES / B: SET  NO: BACK"));
  png("midi_1_open.png");
  tap(kNo);   // NO: back to SIDECAR, the page out of the list
  CHECK(!is("MIDI") && ui::remote_stats().page == 0);
  printf("  menu_row_opens_the_page: SIDECAR MENU > MIDI, its rows; NO back to SIDECAR\n");
}

static void route_changes_saves_and_reloads() {
  open_page();
  tap(kYes);   // YES on KEYBOARD ROUTE: CHANNEL, at once
  CHECK(keys::view().mode == keys::kChannel && shows("CHANNEL") && shows("CH 1-12 PLAY TRACKS 1-12"));
  png("midi_2_channel.png");
  flush();
  CHECK(rd("/.rytm/keys.txt").find("mode=channel") != std::string::npos);
  keys::reset(), keys::load();   // a boot: read back
  CHECK(keys::view().mode == keys::kChannel && shows("CHANNEL"));
  turn(1, -1);   // knob B down: SELECTED again
  CHECK(keys::view().mode == keys::kSelected && shows("NOTES: THE SELECTED TRACK"));
  turn(1, -1);   // (clamped: no wrap)
  CHECK(keys::view().mode == keys::kSelected);
  turn(1, 1);
  CHECK(keys::view().mode == keys::kChannel);
  tap(kYes);
  CHECK(keys::view().mode == keys::kSelected);
  flush();
  CHECK(rd("/.rytm/keys.txt").find("mode=selected") != std::string::npos);
  printf("  route_changes_saves_and_reloads: YES / knob B, keys.txt by idle_save, read back at boot\n");
}

static void auto_channel_and_computer() {
  open_page();
  turn(0, 1);   // knob A: AUTO CHANNEL
  CHECK(shows("AUTO CHANNEL"));
  turn(1, 1), turn(1, 1);
  CHECK(keys::auto_channel() == 16 && shows("16") && shows("SET THE RYTM'S TO MATCH"));
  turn(1, 1);   // clamped at 16
  CHECK(keys::auto_channel() == 16);
  turn(1, -5);  // review L2: a report of several counts (unpaced) steps by its sign, clamped
  CHECK(keys::auto_channel() == 15);
  turn(1, 4);
  CHECK(keys::auto_channel() == 16);
  tap(kYes);    // YES: the next channel, 16 wraps to 1
  CHECK(keys::auto_channel() == 1);
  turn(1, -1);
  CHECK(keys::auto_channel() == 1);
  tap(kDown);   // COMPUTER NOTES: YES off, again on
  tap(kYes);
  CHECK(!keys::computer_on() && shows("OFF") && shows("COMPUTER: TO THE RYTM"));
  png("midi_3_computer_off.png");
  flush();
  std::string f = rd("/.rytm/keys.txt");
  CHECK(f.find("auto=1\n") != std::string::npos && f.find("mac=0") != std::string::npos);
  keys::reset(), keys::load();
  CHECK(keys::auto_channel() == 1 && !keys::computer_on());
  turn(1, 1);
  CHECK(keys::computer_on() && shows("COMPUTER: SIDECAR ROUTES"));
  tap(kUp), tap(kYes);   // AUTO CHANNEL back to 14 for the rest
  for (int i = 0; i < 12; ++i) turn(1, 1);
  CHECK(keys::auto_channel() == 14);
  printf("  auto_channel_and_computer: B steps 1..16 (clamped), YES wraps; COMPUTER NOTES on / off; saved, read back\n");
}

static void learn_routes_clear_after_confirm() {
  learn::reset();
  CHECK(learn::arm("fader") && learn::midi(1, 0xB0, 7, 64));
  CHECK(learn::arm("a") && learn::midi(2, 0xB0, 8, 127));
  CHECK(routes() == 2);
  open_page();
  for (int i = 0; i < kLearn; ++i) tap(kDown);
  CHECK(shows("2 ROUTES") && shows("YES TWICE: CLEAR ALL"));
  tap(kYes);   // asked
  CHECK(routes() == 2 && shows("YES: CLEAR ALL  NO: KEEP"));
  png("midi_4_learn_confirm.png");
  tap(kNo);    // kept, still on the page
  CHECK(routes() == 2 && is("MIDI") && shows("KEPT"));
  tap(kYes), tap(kDown);   // a move drops the question
  CHECK(!shows("YES: CLEAR ALL  NO: KEEP"));
  tap(kUp), tap(kYes), tap(kYes);
  CHECK(routes() == 0 && shows("LEARN ROUTES CLEARED") && shows("NONE"));
  // review L1: asked, then the routes go from the console (learn=clear:N) before YES: the question goes with them, a
  // route learned later is not cleared by the next YES
  CHECK(learn::arm("fader") && learn::midi(1, 0xB0, 7, 64) && routes() == 1);
  tap(kYes);
  CHECK(shows("YES: CLEAR ALL  NO: KEEP"));
  CHECK(learn::clear(0) && routes() == 0);
  tap(kYes);
  CHECK(!shows("YES: CLEAR ALL  NO: KEEP") && shows("ARM IT: CONSOLE LEARN="));
  CHECK(learn::arm("fader") && learn::midi(1, 0xB0, 7, 64) && routes() == 1);
  tap(kYes);   // asks again first
  CHECK(routes() == 1 && shows("YES: CLEAR ALL  NO: KEEP"));
  tap(kNo);
  CHECK(learn::clear(0) && routes() == 0);
  flush();
  learn::reset(), learn::load();
  CHECK(routes() == 0);
  CHECK(learn::arm("fader"));   // armed (the console's learn=fader): WAITING; YES there stops it
  CHECK(shows("WAITING"));
  tap(kYes);
  CHECK(learn::armed() < 0 && shows("NONE"));
  // a learn.txt a newer build wrote: locked (not written), the page says so and clears nothing
  learn::reset();
  CHECK(learn::arm("fader") && learn::midi(1, 0xB0, 7, 64));
  flush();
  FILE* f = fopen((g_card + "/.rytm/learn.txt").c_str(), "ab");
  CHECK(f);
  fputs("Z something newer\n", f);
  fclose(f);
  learn::reset(), learn::load();
  CHECK(learn::locked() && routes() == 1 && shows("1 LOCKED"));
  tap(kYes), tap(kYes);
  CHECK(routes() == 1 && shows("LEARN FILE IS NEWER"));
  learn::reset();
  printf("  learn_routes_clear_after_confirm: the routes' count, YES asks, NO keeps, YES again clears (saved); WAITING; LOCKED\n");
}

static void status_rows() {
  open_page();
  CHECK(shows("MPKMINI +1") && shows("LAST") && shows("-"));
  keys::source(0, false, nullptr);
  CHECK(shows("LAUNCHCO"));   // the one left, 8 characters
  keys::source(1, false, nullptr);
  CHECK(shows("NONE"));
  keys::source(0, true, "MPK mini");
  keys::midi(0, 0x90, 60, 100), keys::midi(0, 0x80, 60, 0);   // no track known: to the Rytm, learning
  CHECK(shows("MPKMINI > RYTM"));
  png("midi_5_last.png");
  for (int i = 0; i < 9; ++i) tap(kDown);   // clamped at the last row; the read-only rows: NO back only
  CHECK(shows("NO: BACK"));
  tap(kYes);
  CHECK(is("MIDI") && keys::view().mode == keys::kSelected);
  keys::source(1, true, "Launch Control XL");
  printf("  status_rows: the USB-A devices by name (+N more), the last key's way; read only\n");
}

static void reset_both() {   // the defaults, two devices in, the forward set
  keys::reset();
  keys::set_forward(fwd);
  keys::source(0, true, "MPK mini");
  keys::source(1, true, "Launch Control XL");
  learn::reset();
}

int main() {
  char tmpl[] = "/tmp/core_midipage_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples' '" + g_card + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  at(g_us);
  CHECK(engine_begin());
  reset_both();
  menu_row_opens_the_page();
  route_changes_saves_and_reloads();
  reset_both();
  auto_channel_and_computer();
  reset_both();
  learn_routes_clear_after_confirm();
  reset_both();
  status_rows();
  ui::remote_reset();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_midi_page: PASS\n");
  return 0;
}
