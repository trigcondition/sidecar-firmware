// core/tests/test_browser.cpp - the 0.5.0 sample browser (core/ui/page_wavegrain.cpp, the bank list's files; the design
// docs/SRC-BANKS.md "0.5.0") on a DSP SAMPLE track, through the Rytm's messages as the 0026 sends them (4A, 31 with
// the down byte, the hello's caps):
//   - an OLD 0026 (no caps bit, never a 2): every key as before - LEFT / RIGHT banks in the list, RIGHT on a file
//     nothing, FUNC + YES arrives as YES and sets the row, no preview;
//   - a 0.5.0 0026: LEFT alone nothing, FUNC + LEFT / RIGHT banks, FUNC + UP the bank's OFF row; RIGHT in the list the
//     files (the last folder, the cursor on the file after the last loaded); RIGHT on a file loads it and goes back to
//     the list on the next empty row (a used row asks once); FUNC + YES previews while YES is held (its release, a
//     cursor move, a load or the browser's close stops it; no pool row used); knob H the PREVIEW LEVEL (a Sidecar
//     setting, shown in the header); FUNC + RIGHT the file menu (LOAD, LOAD TO NEXT EMPTY, PREVIEW (a toggle),
//     SAMPLE EDIT, INFO), on a folder opens it;
//   - a SAMPLER track (machine 43): the same keys, its SMP (P11) committed, FUNC + YES on OFF refused (000 IS OFF).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "machines.h"
#include "nmach.h"
#include "pool_util.h"
#include "samp/preview.h"
#include "samp/sedit.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "wg_util.h"

using namespace core;
using namespace core::ui;

static std::vector<std::vector<uint8_t>> g_sent;
static void sysex(std::vector<uint8_t> m) { engine_sysex(m.data(), (int)m.size()); }
static void key(int code, int down = 1) {   // a press (1, or 2 with FUNC held) and its release
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x31, 1, 0, (uint8_t)code, (uint8_t)down, 0xF7});
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x31, 1, 0, (uint8_t)code, 0, 0xF7});
}
static void fkey(int code) { key(code, 2); }
static void press(int code, int down) { sysex({0xF0, 0x7D, 0x52, 0x54, 0x31, 1, 0, (uint8_t)code, (uint8_t)down, 0xF7}); }
static void release(int code) { sysex({0xF0, 0x7D, 0x52, 0x54, 0x31, 1, 0, (uint8_t)code, 0, 0xF7}); }
static uint64_t g_turn_us = 1000000;
static void turn(int e, int d) {   // 32 1 <encoder> <delta + 64> 0, a rest before it (list pacing: one step)
  host_fake_time(true, g_turn_us += 1000000);
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x32, 1, (uint8_t)e, (uint8_t)(64 + d), 0, 0xF7});
}
static void hello(int caps) { sysex({0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 1, (uint8_t)caps, 0xF7}); }
// the list open on track 0 (DSP SAMPLE: P11) at bank b row r; close: why 2 NO
static void list_open(int b, int r) { sysex({0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 1, (uint8_t)r, (uint8_t)(b << 4), 0, 0xF7}); }
static void list_files() { sysex({0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 3, 0, 0, 0, 0xF7}); }
static void list_close() { sysex({0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 0, 0, 0, 2, 0xF7}); }
static bool sent4a(int v) {
  for (auto& m : g_sent)
    if (m.size() == 9 && m[4] == 0x4A && m[6] == 0 && m[7] == v) return true;
  return false;
}
static int32_t g_au[64][12];
static void audio(int blocks = 8) {   // the output stage's fill: the preview's fade ends there
  for (int i = 0; i < blocks; ++i) {
    preview::step();
    memset(g_au, 0, sizeof g_au);
    preview::mix(g_au, 64, 0, 1);
  }
}
static int g_machine = kDspSample;   // the host's machine of track 0 (SAMPLER: kSmplMachine, its pages and lists)
static void settle() {
  pu::steps();
  for (int i = 0; i < 200; ++i) lib::step(), srclib::step(), preview::step(), pools::step();
  audio();
  src_bank_page(0, g_machine);   // (the draw's poll: re-entry, INFO, SAMPLE EDIT)
}
static int used_rows() {
  int n = 0;
  for (int c = 0; c < pools::kCodes; ++c) n += pools::row_of(c) && pools::at(pools::kSamples, c).type != pools::kEmpty;
  return n;
}
static const char* cur_name() {
  const char* n = "";
  srclib::item(srclib::kGr, src_bank_state().cur, &n, nullptr, nullptr);
  return n;
}
static void cursor_to(const char* name) {
  int at = -1;
  for (int i = 0; i < srclib::count(srclib::kGr); ++i) {
    const char* n = "";
    if (srclib::item(srclib::kGr, i, &n, nullptr, nullptr) && !strcmp(n, name)) at = i;
  }
  for (int i = 0; i < 40 && at >= 0 && src_bank_state().cur != at; ++i) key(src_bank_state().cur < at ? 0x43 : 0x42);
  CHECK(!strcmp(cur_name(), name));
}
static bool row_is(int code, const char* name) {
  const pools::Entry& e = pools::at(pools::kSamples, code);
  return e.type == pools::kSample && !strcmp(e.name, name);
}
static int code(int bank, int row) { return bank * 128 + row; }
// the SRC view's frame as the Rytm gets it: the text items (3D) that went out for this state, joined by '|'
static uint32_t g_t = 100000;
static std::string drawn() {
  g_sent.clear();
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, 0, 1, 0xF7});   // (the SRC view on: its frames drawn by the host)
  for (int i = 0; i < 4; ++i) remote_tick(g_t += 40);
  std::string t = "|";
  for (auto& m : g_sent)
    if (m.size() > 13 && m[4] == 0x3D) t += std::string(m.begin() + 12, m.end() - 1) + "|";
  return t;
}

int main() {
  char tmpl[] = "/tmp/core_browser_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples/Kit/Sub'").c_str()) == 0);
  const char* names[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
  for (int i = 0; i < 12; ++i)
    wg::write_file(card + "/Samples/Kit/" + names[i] + ".wav",
                   wg::wav_bytes(12000 + i, 1, 16, 48000, 1, "", [i](int k, int) { return 0.4 * std::sin(2 * M_PI * (200 + 50 * i) * k / 48000); }));
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  host_fake_time(true, 1000000);
  hooks.to_rytm = [](const uint8_t* d, int n) { g_sent.emplace_back(d, d + n); };
  CHECK(engine_begin());
  pu::steps();
  pu::clear(pools::kSamples);
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 45, 0x20, 0xF7});   // track 0 (BD) on DSP SAMPLE
  // the browser in the kit folder from the start (srclib keeps a browser's folder)
  CHECK(srclib::cd(srclib::kGr, "/Samples/Kit"));
  settle();

  // ---- an OLD 0026: caps without the bit, never a down byte 2
  hello(31);
  CHECK(!src_bank_new_keys());
  list_open(2, 1);   // C001
  settle();
  CHECK(src_bank_state().on && src_bank_state().bank == 2 && src_bank_state().row == 1);
  key(0x41);
  CHECK(src_bank_state().bank == 3);   // RIGHT: bank D
  key(0x40);
  CHECK(src_bank_state().bank == 2);   // LEFT: bank C
  g_sent.clear();
  list_files();
  settle();
  CHECK(src_bank_state().browse && sent4a(1));
  cursor_to("a.wav");
  key(0x41);   // RIGHT on a file: nothing (as before)
  settle();
  CHECK(src_bank_state().browse && !row_is(code(2, 1), "a"));
  key(0x44);   // FUNC + YES on an old 0026 arrives as YES: the row set, no preview
  settle();
  CHECK(!src_bank_state().browse && row_is(code(2, 1), "a") && !preview::sounding() && preview::stats().starts == 0);
  // a used row asks (YES=SET); the second YES (an old 0026's FUNC + YES too) replaces
  pools::seen(pools::kSamples, code(2, 1));
  list_files();
  settle();
  cursor_to("i.wav");
  key(0x44);
  settle();
  CHECK(src_bank_state().browse && row_is(code(2, 1), "a") && drawn().find("YES=SET") != std::string::npos);
  key(0x44);
  settle();
  CHECK(!src_bank_state().browse && row_is(code(2, 1), "i"));
  list_close();
  printf("browser: old 0026 - LEFT / RIGHT banks, RIGHT on a file nothing, YES sets, no preview\n");

  // ---- the 0.5.0 0026: a 2 alone turns the new keys on (until the next hello); then the caps bit
  hello(31);
  list_open(2, 2);   // C002
  settle();
  fkey(0x41);   // FUNC + RIGHT: bank D (and the new keys on)
  CHECK(src_bank_new_keys() && src_bank_state().bank == 3);
  hello(31);
  CHECK(!src_bank_new_keys());
  hello(31 | kCapsBrowseFunc);
  CHECK(src_bank_new_keys());
  list_open(2, 2);
  settle();
  key(0x40);
  CHECK(src_bank_state().bank == 2);   // LEFT alone: nothing
  fkey(0x41);
  CHECK(src_bank_state().bank == 3);   // FUNC + RIGHT: bank +
  fkey(0x40);
  CHECK(src_bank_state().bank == 2);   // FUNC + LEFT: bank -
  fkey(0x42);
  CHECK(src_bank_state().row == 0);    // FUNC + UP: OFF
  key(0x43), key(0x43);
  CHECK(src_bank_state().row == 2);
  g_sent.clear();
  auto sent48 = [](int row) {
    for (auto& m : g_sent)
      if (m.size() == 12 && m[4] == 0x48 && m[6] == 0 && m[7] == 10 && m[8] == row && m[9] == 0x20) return true;
    return false;
  };
  fkey(0x45);   // FUNC + NO (the owner): the cursor on OFF, nothing written; YES (the Rytm's close, why 1) sets it
  CHECK(!sent48(0) && src_bank_state().on && src_bank_state().row == 0 && drawn().find("|OFF: YES SETS|") != std::string::npos);
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 0, 0, 0x20, 1, 0xF7});
  CHECK(sent48(0) && !src_bank_state().on);
  list_open(2, 2);
  settle();
  printf("browser: 0.5.0 list - LEFT nothing, FUNC + LEFT / RIGHT banks, FUNC + UP / FUNC + NO to OFF, YES sets it\n");

  // RIGHT in the list: the files (nothing loaded yet: the folder, first item)
  g_sent.clear();
  key(0x41);
  settle();
  CHECK(src_bank_state().browse && sent4a(1) && !strcmp(srclib::cwd(srclib::kGr), "/Samples/Kit"));
  // FUNC + YES: the preview while YES is held (the owner's hardware test, as the stock browser); YES's release stops it
  // (FUNC still held: the 0026 forwards it as 31 44 0); a cursor move stops it; no pool row used
  int rows0 = used_rows();
  cursor_to("b.wav");
  press(0x44, 2);
  settle();
  CHECK(preview::sounding() && strstr(preview::status().path, "/b.wav") && src_bank_state().browse && drawn().find("|PLAY|") != std::string::npos);
  settle();
  CHECK(preview::sounding());   // (held: still playing)
  release(0x44);
  audio();
  CHECK(!preview::sounding() && src_bank_state().browse);
  // the Rytm repeats FUNC + YES while it is held: a repeat never restarts the preview (the owner's hardware test:
  // it restarted many times a second); only after YES's release does FUNC + YES play again
  press(0x44, 2);
  settle();
  audio();
  preview::Stats s0 = preview::stats();
  for (int i = 0; i < 5; ++i) press(0x44, 2), settle(), audio();
  preview::Stats s1 = preview::stats();
  CHECK(preview::sounding() && s1.starts == s0.starts && s1.ends == s0.ends);
  release(0x44);
  audio();
  CHECK(!preview::sounding());
  press(0x44, 2);
  settle();
  CHECK(preview::sounding());
  key(0x43);   // DOWN while YES is held: c.wav, the preview stops
  audio();
  CHECK(!preview::sounding() && used_rows() == rows0 && !strcmp(cur_name(), "c.wav"));
  release(0x44);   // (the late release: nothing)
  CHECK(!preview::sounding() && !strcmp(cur_name(), "c.wav") && src_bank_state().browse);
  key(0x42);   // b.wav again
  printf("browser: FUNC + YES previews while held, its release or a move stops it, no pool row used\n");
  // PREVIEW LEVEL: knob H in the files, a dB a step, -40 .. -6, said in the header; knob H in the list: not ours
  CHECK(preview::level_db() == preview::kLevelDefDb);
  turn(7, 8);
  CHECK(preview::level_db() == preview::kLevelDefDb + 1 && drawn().find("|PREVIEW LEVEL -8 DB|") != std::string::npos);
  for (int i = 0; i < 6; ++i) turn(7, 8);
  CHECK(preview::level_db() == preview::kLevelMaxDb && drawn().find("|PREVIEW LEVEL -6 DB|") != std::string::npos);
  turn(7, -8);
  CHECK(preview::level_db() == -7 && src_bank_state().browse && !strcmp(cur_name(), "b.wav"));
  for (int i = 0; i < 40; ++i) preview::save(), settle();   // (the idle chain's save)
  {
    FILE* f = fopen((card + "/.rytm/preview.txt").c_str(), "r");
    char b[64] = "";
    CHECK(f && fread(b, 1, sizeof b - 1, f) > 0);
    if (f) fclose(f);
    CHECK(strstr(b, "level=-7\n"));
  }
  printf("browser: knob H sets PREVIEW LEVEL (shown, clamped, saved)\n");

  // RIGHT on b.wav (its preview held): C002 = b, back to the list on the next empty row (C003)
  press(0x44, 2);
  settle();
  CHECK(preview::sounding());
  g_sent.clear();
  key(0x41);
  settle();
  CHECK(row_is(code(2, 2), "b") && !src_bank_state().browse && sent4a(0) && src_bank_state().row == 3 && !preview::sounding());
  release(0x44);
  CHECK(!src_bank_state().browse && src_bank_state().row == 3);
  turn(7, 8);   // knob H in the list (a 0026 never forwards it there): not the browser's
  CHECK(preview::level_db() == -7);
  // RIGHT again: the files, the cursor on the file after b (c.wav); RIGHT: C003 = c, the list on C004
  key(0x41);
  settle();
  CHECK(src_bank_state().browse && !strcmp(cur_name(), "c.wav"));
  key(0x41);
  settle();
  CHECK(row_is(code(2, 3), "c") && src_bank_state().row == 4);
  // the next empty row skips a used one: C005 holds e; after loading d into C004 the list goes to C006
  CHECK(pu::put(pools::kSamples, "C005", "/Samples/Kit/e.wav") == pools::kOk);
  key(0x41);
  settle();
  CHECK(!strcmp(cur_name(), "d.wav"));
  key(0x41);
  settle();
  CHECK(row_is(code(2, 4), "d") && src_bank_state().row == 6);
  // the browser left in another folder (Sub, the list closed by the Rytm there): RIGHT goes back to the last file's
  // folder, after it (M3); RIGHT on a file already in the pool (e: C005) says so, the list stays on the empty row, and
  // the next RIGHT goes on after it
  list_files();
  settle();
  cursor_to("Sub");
  key(0x44);
  settle();
  CHECK(!strcmp(srclib::cwd(srclib::kGr), "/Samples/Kit/Sub"));
  list_close();
  list_open(2, 6);
  settle();
  key(0x41);
  settle();
  CHECK(src_bank_state().browse && !strcmp(srclib::cwd(srclib::kGr), "/Samples/Kit") && !strcmp(cur_name(), "e.wav"));
  int used0 = used_rows();
  key(0x41);
  settle();
  CHECK(!src_bank_state().browse && src_bank_state().row == 6 && used_rows() == used0 &&
        drawn().find("|IN POOL C005|") != std::string::npos);
  key(0x41);
  settle();
  CHECK(src_bank_state().browse && !strcmp(cur_name(), "f.wav"));
  list_close();
  list_open(2, 6);
  settle();
  printf("browser: RIGHT loads and goes to the next empty row; RIGHT in the list re-enters after the last file\n");

  // a used row asks once: C003 (c) seen playing; RIGHT on f.wav asks, the second RIGHT replaces
  pools::seen(pools::kSamples, code(2, 3));
  key(0x42), key(0x42), key(0x42);   // C003
  CHECK(src_bank_state().row == 3);
  list_files();
  settle();
  cursor_to("f.wav");
  key(0x41);
  settle();
  CHECK(src_bank_state().browse && row_is(code(2, 3), "c"));
  key(0x41);
  settle();
  CHECK(!src_bank_state().browse && row_is(code(2, 3), "f"));
  printf("browser: a used row asks once (RIGHT again replaces)\n");

  // FUNC + RIGHT on a folder opens it; on a file the menu
  list_files();
  settle();
  cursor_to("Sub");
  fkey(0x41);
  settle();
  CHECK(!strcmp(srclib::cwd(srclib::kGr), "/Samples/Kit/Sub"));
  key(0x40);   // LEFT: up
  settle();
  cursor_to("g.wav");
  fkey(0x41);
  settle();
  CHECK(src_bank_state().menu == 0 && src_bank_state().browse);
  std::string tx = drawn();
  CHECK(tx.find("|G.WAV|") != std::string::npos && tx.find("|LOAD TO NEXT EMPTY|") != std::string::npos &&
        tx.find("|SAMPLE EDIT|") != std::string::npos);
  // PREVIEW (item 2)
  key(0x43), key(0x43);
  key(0x44);
  settle();
  CHECK(src_bank_state().menu < 0 && preview::sounding() && strstr(preview::status().path, "/g.wav"));
  // INFO (item 4): the head read, the note; the preview stopped
  fkey(0x41);
  for (int i = 0; i < 4; ++i) key(0x43);
  CHECK(src_bank_state().menu == 4);
  key(0x44);
  settle();
  settle();
  CHECK(!preview::sounding() && preview::status().st == preview::kDone && !preview::status().play && preview::status().rate == 48000);
  CHECK(drawn().find("|48K 16B MO 0.2S|") != std::string::npos);   // the note: rate, bits, mono, length
  // NO closes the menu, not the browser
  fkey(0x41);
  key(0x45);
  CHECK(src_bank_state().menu < 0 && src_bank_state().browse);
  // LOAD TO NEXT EMPTY (item 1) from C003 (used): the first empty row from there, C006; the list on it
  fkey(0x41);
  key(0x43);
  key(0x44);
  settle();
  CHECK(row_is(code(2, 6), "g") && !src_bank_state().browse && src_bank_state().row == 6);
  // SAMPLE EDIT (item 3) on C007: loads h there, the editor opens on that row, the list closed
  key(0x43);
  CHECK(src_bank_state().row == 7);
  list_files();
  settle();
  cursor_to("h.wav");
  fkey(0x41);
  key(0x43), key(0x43), key(0x43);
  key(0x44);
  for (int i = 0; i < 20; ++i) settle();
  CHECK(!src_bank_state().on);
  sedit::View sv;
  sedit::view(&sv);
  CHECK(sv.st != sedit::kClosed && !strncmp(sv.name, "h", 1) && row_is(code(2, 7), "h"));
  // SAMPLE EDIT on a used row (C007, seen): the load asks, its YES loads and opens the editor on that row (M5)
  sedit::close();
  for (int i = 0; i < 20; ++i) settle();
  pools::seen(pools::kSamples, code(2, 7));
  list_open(2, 7);
  settle();
  list_files();
  settle();
  cursor_to("l.wav");
  fkey(0x41);
  key(0x43), key(0x43), key(0x43);
  key(0x44);
  settle();
  CHECK(src_bank_state().browse && row_is(code(2, 7), "h") && drawn().find("YES=SET") != std::string::npos);
  key(0x44);
  for (int i = 0; i < 20; ++i) settle();
  sedit::view(&sv);
  CHECK(!src_bank_state().on && row_is(code(2, 7), "l") && sv.st != sedit::kClosed && !strncmp(sv.name, "l", 1));
  printf("browser: FUNC + RIGHT - folder opens, file menu (PREVIEW, INFO, LOAD TO NEXT EMPTY, SAMPLE EDIT)\n");

  // the preview stops when the browser closes (NO at the folder's top: up, up, the list)
  sedit::close();
  for (int i = 0; i < 20; ++i) settle();
  list_open(2, 8);
  settle();
  list_files();
  settle();
  cursor_to("a.wav");
  press(0x44, 2);
  settle();
  CHECK(preview::sounding());
  for (int i = 0; i < 4 && src_bank_state().browse; ++i) key(0x45);
  audio();
  CHECK(!src_bank_state().browse && !preview::sounding());
  list_close();
  // ... and when the Rytm closes the list (4A on 0) with the files open (the NOs above went up to the root)
  CHECK(!srclib::cwd(srclib::kGr)[0] && srclib::cd(srclib::kGr, "/Samples/Kit"));
  list_open(2, 8);
  settle();
  list_files();
  settle();
  cursor_to("a.wav");
  press(0x44, 2);
  settle();
  CHECK(preview::sounding());
  list_close();
  audio();
  CHECK(!src_bank_state().on && !preview::sounding());
  printf("browser: the preview stops when the browser closes, and on the Rytm's close\n");

  // POOL FULL: a quick load into H127 (no empty row after it): set, the list stays, said; LOAD TO NEXT EMPTY with none
  // free from H127: nothing loaded, said
  list_open(7, 127);
  settle();
  list_files();
  settle();
  cursor_to("j.wav");
  key(0x41);
  settle();
  CHECK(row_is(code(7, 127), "j") && !src_bank_state().browse && src_bank_state().bank == 7 && src_bank_state().row == 127 &&
        drawn().find("|POOL FULL|") != std::string::npos);
  list_files();
  settle();
  cursor_to("k.wav");
  fkey(0x41);
  key(0x43);
  key(0x44);
  settle();
  bool k_in = false;
  for (int c = 0; c < pools::kCodes; ++c) k_in = k_in || row_is(c, "k");
  CHECK(!k_in && src_bank_state().browse && src_bank_state().menu < 0 && drawn().find("|POOL FULL|") != std::string::npos);
  list_close();
  printf("browser: POOL FULL on a quick load and on LOAD TO NEXT EMPTY\n");

  // ---- a SAMPLER track (the Rytm's machine 43 on DSP SAMPLE's voice byte): the same browser, its SMP (P11) committed
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, kRytmSampler, 0x20, 0xF7});   // track 0 (BD) on SAMPLER
  g_machine = kSmplMachine;
  CHECK(remote_track_smpl(0));
  auto sent48p11 = [](int row) {
    for (auto& m : g_sent)
      if (m.size() == 12 && m[4] == 0x48 && m[6] == 0 && m[7] == 10 && m[8] == row) return true;
    return false;
  };
  list_open(3, 1);   // D001
  settle();
  CHECK(src_bank_state().on && src_bank_state().machine == kSmplMachine && src_bank_state().bank == 3);
  fkey(0x41);
  CHECK(src_bank_state().bank == 4);   // FUNC + RIGHT: bank +
  fkey(0x40);
  CHECK(src_bank_state().bank == 3);
  list_files();
  settle();
  CHECK(src_bank_state().browse && kSmplMachine == src_bank_state().machine);
  cursor_to("a.wav");
  press(0x44, 2);   // the preview, YES held
  settle();
  CHECK(preview::sounding() && strstr(preview::status().path, "/a.wav"));
  key(0x41);   // RIGHT: quick load into D001, the list on D002, the preview stopped
  settle();
  audio();
  CHECK(row_is(code(3, 1), "a") && !src_bank_state().browse && src_bank_state().row == 2 && !preview::sounding());
  // 0.5.0 FILL (fix round L3): a second quick load in the same opening grows the batch; FILL's SAMPLE then defaults
  // to it (FROM D001, TO D002)
  list_files();
  settle();
  cursor_to("c.wav");   // (c: in no row yet; b is in the pool already, C002)
  key(0x41);
  settle();
  CHECK(row_is(code(3, 2), "c"));
  {
    int a = -1, b = -1;
    CHECK(fill_batch(0, pools::kSamples, &a, &b) && a == code(3, 1) && b == code(3, 2));
    char o[400];
    fill_command("fill=open:0", o, sizeof o);
    FillView v = fill_page_view();
    CHECK(v.on && v.track == 0 && v.target == 0 && v.from == code(3, 1) && v.to == code(3, 2));
    fill_page_reset();
  }
  // YES (the Rytm's close, why 1) commits the row to SMP (P11), not P1
  g_sent.clear();
  key(0x42), key(0x42);   // D001 (the list on D003 after the two loads)
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 0, 0, 0x30, 1, 0xF7});
  CHECK(sent48p11(1) && !src_bank_state().on);
  // FUNC + NO: the cursor on OFF, nothing written; YES then writes SMP 000
  list_open(3, 1);
  settle();
  g_sent.clear();
  fkey(0x45);
  CHECK(!sent48p11(0) && src_bank_state().row == 0 && drawn().find("|OFF: YES SETS|") != std::string::npos);
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 0, 0, 0x30, 1, 0xF7});
  CHECK(sent48p11(0) && !src_bank_state().on);
  // FUNC + YES on SAMPLER's SRC view with SMP OFF (fix1049, why 6): 000 IS OFF, no free row picked, no files
  list_open(3, 0);
  settle();
  g_sent.clear();
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0, 3, 0, 0, 6, 0xF7});
  settle();
  CHECK(!src_bank_state().browse && src_bank_state().row == 0 && !sent4a(1) && drawn().find("|000 IS OFF|") != std::string::npos);
  list_close();
  // the file menu has SAMPLE EDIT: it loads k (in no row yet) into D003 (the first empty: c is in D002) and opens the
  // editor on that row
  list_open(3, 3);
  settle();
  list_files();
  settle();
  cursor_to("k.wav");
  fkey(0x41);
  CHECK(src_bank_state().menu == 0 && drawn().find("|SAMPLE EDIT|") != std::string::npos);
  key(0x43), key(0x43), key(0x43);
  key(0x44);
  for (int i = 0; i < 20; ++i) settle();
  sedit::view(&sv);
  CHECK(!src_bank_state().on && row_is(code(3, 3), "k") && sv.st != sedit::kClosed && !strncmp(sv.name, "k", 1));
  sedit::close();
  for (int i = 0; i < 20; ++i) settle();
  g_machine = kDspSample;
  printf("browser: SAMPLER - preview, quick load, SMP (P11) commit, FUNC + NO to OFF, 000 IS OFF, SAMPLE EDIT\n");

  // a JUNO preset list: a forwarded FUNC + NO closes it as NO did (L8; the 0.5.0 0026 closes it itself, 4A why 2)
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 41, 0x08, 0xF7});   // track 0 on JUNO
  list_open(0, 0);
  settle();
  CHECK(src_bank_state().on);
  g_sent.clear();
  fkey(0x45);
  CHECK(!src_bank_state().on);

  system(("rm -rf '" + card + "'").c_str());
  printf("test_browser: PASS\n");
  return 0;
}
