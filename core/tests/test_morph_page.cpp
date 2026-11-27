// core/tests/test_morph_page.cpp - the MORPH page (core/ui/page_morph.cpp, spec-050 2.6) through the remote host, as
// test_lfo.cpp's page part: the scene keys (tap selects, T15 / T16 held + Tn A / B, Tn held its lock list), the fader
// on encoder A, the list's edits (B..H, LEFT / RIGHT, NO removes, YES twice clears), no SCN / PERF cells (v2),
// the ADD row (v2 Task 5), the trig LEDs, and every text on the Rytm's screen fits (check.h's TEXT-FIT); on the other
// Sidecar pages the momentary EDIT (v2): PAGE + T15 / T16 held = scene A / B, a turn locks into it, + T1..T12 picks it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "machines.h"
#include "voices.h"
#include "rui_png.h"
#include "core.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "seq/rytm_dump.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;
using mod::Tid;

static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static int g_mask_encs = -1, g_masks = 0;
static uint32_t g_mask_trigs = 0;
static bool g_mask_func = false;
static uint8_t g_led[16];
static bool g_leds_on = false;
static std::vector<uint32_t> g_commit_ms;   // when each frame commit (0x39) went out
static int g_closes = 0;   // 3A 0 0: the host asks the page closed (nav044: NO on a shortcut's page)
static void out(const uint8_t* d, int len) {
  if (len < 7 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return;
  if (d[4] == 0x37 && len >= 10) {
    g_mask_encs = d[6] | d[7] << 7 | d[8] << 14, g_mask_trigs = 0, ++g_masks;
    const uint8_t* k = d + 9;
    int nk = len - 10;
    for (int c = 0; c < 16; ++c)
      if (c / 7 < nk && (k[c / 7] >> (c % 7) & 1)) g_mask_trigs |= 1u << c;
    g_mask_func = 0x80 / 7 < nk && (k[0x80 / 7] >> (0x80 % 7) & 1);
  } else if (d[4] == 0x39) {
    g_commit_ms.push_back(now_ms());
  } else if (d[4] == 0x3A && len >= 9 && d[7] == 0) {
    ++g_closes;
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
static Tid T(const char* n) {
  Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
static int lock_v(int n, Tid t) {
  const scene::Scene& s = scene::state().s[n];
  for (int i = 0; i < s.n; ++i)
    if (s.l[i].t == t) return s.l[i].v;
  return -1;
}
// the page's frame as the Rytm would get it: the text items
static ui::Canvas g_c;
static void draw() { ui::kMorphPage.draw(g_c, 0, 1, now_ms() | 1); }
static void png(const char* name) {   // the frame as the Rytm shows it (PNGs to $RUI_OUT, default build/rui_morph)
  static std::string dir = rui_png_dir("RUI_OUT", "build/rui_morph");
  uint8_t fb[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  rui_png(dir, name, fb);
}
static bool shows(const char* s) {
  draw();
  for (const ui::TextItem& t : g_c.txt)
    if (t.s[0] && strstr(t.s, s)) return true;
  return false;
}

static void open_page() {
  ui::remote_reset();
  ui::morph_page_reset();
  scene::reset();
  from_rytm(0x30, 0, 1, 0);
  tick();
  ui::remote_show_page("MORPH");
  tick();
  CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "MORPH"));
}

static void select_and_ab_by_holds() {
  open_page();
  CHECK(g_mask_trigs == 0xFFFF && g_mask_encs == 0xFF && g_mask_func);   // all 16 trig keys, A..H, FUNC (both sides')
  tap(0x02);
  CHECK(scene::state().sel == 2);
  scene::set_fader(5000, scene::kConsole);
  down(0x0E), tap(0x04), up(0x0E);   // T15 held + T5: A = 5
  CHECK(scene::state().a == 4 && scene::state().b == -1 && scene::state().sel == 2 && scene::state().fader == 5000);
  down(0x0F), tap(0x06), up(0x0F);   // T16 held + T7: B = 7
  CHECK(scene::state().a == 4 && scene::state().b == 6 && scene::state().fader == 5000);
  tap(0x0C), tap(0x0D);              // T13 / T14: nothing
  CHECK(scene::state().sel == 2 && ui::remote_page_title() && !strcmp(ui::remote_page_title(), "MORPH"));
  tap(0x41), tap(0x41);              // RIGHT: the next scene (the page stays)
  CHECK(scene::state().sel == 4 && !strcmp(ui::remote_page_title(), "MORPH"));
  tap(0x40);
  CHECK(scene::state().sel == 3);
  CHECK(scene::lock(2, T("V1:P3"), 6400) && scene::lock(6, T("V2:P1"), 640));
  scene::set_fader(8192, scene::kConsole);
  CHECK(shows("A5") && shows("B7") && shows("MORPH"));
  png("morph_main.png");
  printf("  select_and_ab_by_holds: T3 selects, T15 + T5 A, T16 + T7 B, the fader stays; LEFT / RIGHT the scene\n");
}

static void fader_encoder_steps() {
  open_page();
  turn(0, 1);
  CHECK(scene::state().fader == 128);
  turn(0, 1, true);
  CHECK(scene::state().fader == 144);
  turn(0, -5);
  CHECK(scene::state().fader == 0);
  for (int i = 0; i < 140; ++i) turn(0, 1);
  CHECK(scene::state().fader == scene::kFaderMax);
  CHECK(shows("FADER:16383") && shows("100%"));
  scene::set_ab(0, 1);   // a scene switch: the fader stays where it is (relative, no jump)
  turn(0, -1);
  CHECK(scene::state().fader == scene::kFaderMax - 128);
  printf("  fader_encoder_steps: A 128 a detent, FUNC 16, clamped 0..16383, relative\n");
}

static void hold_shows_lock_list_and_edits() {
  open_page();
  for (int k = 0; k < 9; ++k) {
    char n[8];
    snprintf(n, sizeof n, "V1:P%d", k + 1);
    CHECK(scene::lock(2, T(n), (uint16_t)((10 + k) * 128)));
  }
  down(0x02);
  CHECK(!shows("S3 LOCKS"));   // a tap's time: the main view
  tick(10);                    // held 300 ms: the list
  // 2026-11: a row names the param as voice 1's machine (PLAITS) does: P1 ENG, P7 TUN, P8 LEV, P9 FM (its page 2)
  CHECK(shows("S3 LOCKS") && shows("V1:ENG") && shows("V1:TUN") && !shows("V1:LEV") && shows("1-7/9") && !shows("V1:P1"));
  turn(1, 1);                  // B: row 1 + 1
  CHECK(lock_v(2, T("V1:P1")) == 11 * 128 && shows("V1:ENG:11"));
  turn(3, 2, true);            // D: row 3 + 16 (FUNC x 8)
  CHECK(lock_v(2, T("V1:P3")) == 28 * 128);
  tap(0x41), tap(0x41);        // RIGHT x 2: rows 3..9 (a third: the ADD row on top, v2)
  CHECK(shows("3-9/9") && shows("V1:FM") && !shows("V1:HRM"));
  turn(7, -1);                 // H: row 9
  CHECK(lock_v(2, T("V1:P9")) == 17 * 128);
  up(0x02);
  CHECK(!shows("S3 LOCKS") && shows("MORPH"));
  // a value never past its range: T1:DIR 0..5
  CHECK(scene::lock(5, T("T1:DIR"), 3 * 128));
  down(0x05), turn(1, 5);
  CHECK(lock_v(5, T("T1:DIR")) == 5 * 128);
  up(0x05);
  printf("  hold_shows_lock_list_and_edits: held 250 ms the list, B..H edit (FUNC x 8), LEFT / RIGHT scroll\n");
}

static void no_removes_row() {
  open_page();
  CHECK(scene::lock(0, T("V1:P1"), 128) && scene::lock(0, T("MFX:LEV"), 64 * 128) && scene::lock(0, T("V2:P2"), 256));
  down(0x00);
  tap(0x45);                   // NO: B's row (V1:P1) goes
  CHECK(scene::state().s[0].n == 2 && lock_v(0, T("V1:P1")) < 0 && lock_v(0, T("MFX:LEV")) == 64 * 128);
  for (int k = 0; k < 3; ++k) at(g_us + 2100000), tap(0x45);   // (past the undo's 2 s) NO on an empty list: nothing
  CHECK(scene::state().s[0].n == 0 && !strcmp(ui::remote_page_title(), "MORPH") && shows("S1 ADD"));
  up(0x00);
  int closes = g_closes;
  tap(0x45);                   // NO with no scene key held: back (nav044: a shortcut's page asks itself closed)
  CHECK(g_closes == closes + 1 || !ui::remote_page_title() || strcmp(ui::remote_page_title(), "MORPH"));
  printf("  no_removes_row: NO while held removes B's row; without a hold NO is back\n");
}

static void yes_clears_after_confirm() {
  open_page();
  CHECK(scene::lock(3, T("V1:P1"), 128) && scene::lock(3, T("V1:P2"), 128));
  down(0x03), tap(0x44);       // YES while held: asks
  CHECK(scene::state().s[3].n == 2 && shows("CLEAR S4? YES AGAIN"));
  tap(0x45);                   // NO: kept
  CHECK(scene::state().s[3].n == 2 && !shows("CLEAR S4?"));
  tap(0x44), up(0x03);         // asked again; the key let go: the question stays
  CHECK(shows("CLEAR S4? YES AGAIN"));
  tap(0x44);                   // YES again: cleared
  CHECK(scene::state().s[3].n == 0 && !shows("CLEAR S4?"));
  printf("  yes_clears_after_confirm: hold + YES asks, NO keeps, YES again clears\n");
}

// 0.5.0 v2 (S7): no SCN / PERF cells: G and H do nothing on the main view
static void no_scn_perf_cells() {
  open_page();
  scene::set_ab(0, 1);
  tap(0x01);
  turn(6, 1), turn(7, 1), turn(6, -3), turn(7, -2);
  CHECK(scene::state().s[1].n == 0 && scene::state().s[0].n == 0);
  CHECK(!shows("SCN") && !shows("PERF") && shows("FADER") && shows("LOCKS"));
  printf("  no_scn_perf_cells: G / H do nothing, no SCN / PERF cells\n");
}

// ---- v2 Task 5: the ADD row, the last of a held scene's list (on top: B track, C page, D param, E value, YES adds)
static void hold_add(int key) {   // the scene key held, scrolled until ADD is the top row
  down(key), tick(10);
  for (int i = 0; i < 70; ++i) tap(0x41);
  CHECK(shows(" ADD"));
}
static void add_row_rt() {
  open_page();
  hold_add(0x02);
  CHECK(shows("S3 ADD") && shows("RT1") && shows("SRC"));
  turn(2, 2);                  // C: SRC SMPL FLTR
  turn(3, 5);                  // D: FENV FATK FDEC FSUS FREL FRQ
  turn(4, 23);                 // E: 64 (the Rytm's own: no base here) + 23
  CHECK(shows("FLTR") && shows("FRQ") && shows("87"));
  tap(0x44);                   // YES adds
  CHECK(lock_v(2, T("RT1:FRQ")) == 87 * 128 && scene::state().s[2].n == 1);
  up(0x02);
  printf("  add_row_rt: hold T3, ADD: RT1 FLTR FRQ 87, YES -> scene 3 has RT1:FRQ 87\n");
}
static void add_row_rfx() {
  open_page();
  hold_add(0x04);
  turn(1, 12);                 // B: RT1..RT12, RFX
  turn(2, 1);                  // C: DELAY REVERB
  turn(3, 1);                  // D: PRE RDEC
  turn(4, 36);
  CHECK(shows("RFX") && shows("REVERB") && shows("RDEC"));
  tap(0x44);
  CHECK(lock_v(4, T("RFX:RDEC")) == 100 * 128);
  up(0x04);
  printf("  add_row_rfx: ADD RFX REVERB RDEC 100\n");
}
static void add_row_voice() {
  open_page();
  uint8_t m[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kPlaits | 0)};   // V1 PLAITS, its knobs at 40
  for (int k = 0; k < 16; ++k) m[6 + 2 * k] = 40, m[7 + 2 * k] = 0;
  m[38] = 0xF7;
  engine_sysex(m, 39);
  tick();
  hold_add(0x00);
  turn(1, 13);                 // B: .. RFX, V1
  CHECK(shows("V1") && shows("ENG"));
  turn(4, 5);                  // E: from the base (40) + 5
  tap(0x44);
  CHECK(lock_v(0, T("V1:P1")) == 45 * 128);
  up(0x00);
  printf("  add_row_voice: ADD V1 ENG (its machine's name) from the base 40 + 5\n");
}
static void add_row_full_refused() {
  open_page();
  for (int k = 0; k < 64; ++k) CHECK(scene::lock(3, mod::tid(mod::kV, k / 16, k % 16), 128));
  hold_add(0x03);
  tap(0x44);
  CHECK(scene::state().s[3].n == 64 && lock_v(3, T("RT1:P1")) < 0 && shows("SCENE FULL"));
  up(0x03);
  printf("  add_row_full_refused: a 65th lock by ADD: SCENE FULL\n");
}

static void leds() {
  open_page();
  CHECK(scene::lock(8, T("V1:P1"), 128));
  scene::set_ab(0, 1);
  tap(0x05);
  tick();
  CHECK(g_leds_on && g_led[0] == ui::kLedA && g_led[1] == ui::kLedB && g_led[5] == 111 && g_led[8] == 107 &&
        g_led[2] == ui::kLedOff && g_led[12] == ui::kLedOff && g_led[13] == ui::kLedOff && g_led[14] == ui::kLedA &&
        g_led[15] == ui::kLedB);
  printf("  leds: A blue, B orange, selected white, locks dim white, T13 / T14 off, T15 / T16 A / B\n");
}

static void text_fit() {
  open_page();   // the longest texts: S12, 64 locks with the longest names, every footer
  for (int k = 0; k < 64; ++k) {
    static const char* const kN[] = {"RT12:FTYP", "I8:A4", "MFX:MIX4", "X8:B3", "O8:WID", "V8:P16", "FX2:P5", "S12:F2"};
    Tid t = T(kN[k % 8]);
    if (k >= 8) t = mod::tid(mod::kV, k % 8, k / 8);
    scene::lock(11, t, (uint16_t)(k % 2 ? 16383 : 127 * 128 + 64));
  }
  CHECK(scene::state().s[11].n == 64);
  CHECK(!scene::lock(11, T("FX1:P2"), 0) && scene::state().s[11].n == 64);   // the 65th: SCENE FULL
  scene::set_ab(10, 11), scene::set_fader(16383, scene::kConsole);
  tap(0x0B), turn(0, 1), draw();
  hold_add(0x0B), tap(0x44);   // ADD on a full scene
  CHECK(shows("SCENE FULL"));
  up(0x0B);
  png("morph_full.png");
  down(0x0B), tick(10);
  png("morph_list.png");
  for (int i = 0; i < 64; ++i) tap(0x41), draw();
  turn(1, 1), draw(), tap(0x44), draw();
  png("morph_clear.png");
  up(0x0B), draw();
  tap(0x45), draw();
  printf("  text_fit: every state drawn with S12, 64 locks, the longest names (TEXT-FIT checks at exit)\n");
}

// ---- Task 39: EDIT on the other Sidecar pages
static bool sent_has(const char* s) {   // a text item the Rytm has now (the host's last commit)
  uint8_t t[36 * ui::kTextSlots];
  int n = ui::sent_texts(t, ui::kTextSlots);
  for (int i = 0; i < n; ++i)
    if (strstr((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
static void voice_page() {   // V1 a PLAITS voice (its report), the page open on it: SIDECAR, RIGHT x 2 (SYSTEM, V1)
  ui::remote_reset();
  ui::morph_page_reset();
  scene::reset();
  uint8_t m[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kPlaits | 0)};
  for (int k = 0; k < 16; ++k) m[6 + 2 * k] = 40, m[7 + 2 * k] = 0;
  m[38] = 0xF7;
  engine_sysex(m, 39);
  from_rytm(0x30, 0, 1, 0);
  tick();
  tap(0x41), tap(0x41);
  tick();
  CHECK(ui::remote_stats().voice == 0);
}
static int enc_param(int e) {   // the voice page's encoder e: its P (0..15)
  const ui::Page* p = ui::page_for_voice(kPlaits, 0);
  CHECK(p && p->enc[e] != ui::kNone);
  return p->enc[e] & 15;
}

static void page(bool down_) { from_rytm(0x31, 0x84 >> 7, 0x84 & 0x7F, down_ ? 1 : 0), tick(1); }
static void side(int key) { page(true), tick(), down(key), tick(); }   // PAGE + T15 / T16 held (PAGE still down)

static void page_t15_turn_locks_a() {
  voice_page();
  scene::set_ab(2, 6);
  tick();
  CHECK(!(g_mask_trigs & 0xFFFF));          // nothing held: the trig keys are the Rytm's
  page(true), tick();
  CHECK((g_mask_trigs & 0xFFFF) == 0xC000); // PAGE held: T15 / T16 the host's
  down(0x0E), tick();
  CHECK((g_mask_trigs & 0xFFFF) == 0xCFFF && scene::edit_side() == 0);   // EDIT A: T1..T12 too
  int k = enc_param(1);
  uint32_t edits = ui::remote_stats().edits;
  float before = v[0].p[k];
  turn(1, 3);                               // scene A (3) locks V1's P, from the base (40) + 3
  CHECK(lock_v(2, mod::tid(mod::kV, 0, k)) == 43 * 128);
  page(false), page(true), page(false);     // PAGE let go first (and again): EDIT stays while T15 is held
  turn(1, 1);
  CHECK(lock_v(2, mod::tid(mod::kV, 0, k)) == 44 * 128);
  CHECK(ui::remote_stats().edits == edits && v[0].p[k] == before && ui::remote_stats().voice == 0);   // no page step
  up(0x0E), tick();
  CHECK(scene::edit_side() < 0 && !(g_mask_trigs & 0xFFFF));
  turn(1, 1);                               // nothing held: the page's own edit, as always
  CHECK(ui::remote_stats().edits == edits + 1 && lock_v(2, mod::tid(mod::kV, 0, k)) == 44 * 128);
  // MASTER: its LEV (H) under PAGE + T15: scene A (3) locks MFX:LEV, the master's knob stays
  core::ins::set_type(core::master::chain(0), core::master::slot(0), core::ins::kComp);   // a master slot: MASTER shown
  ui::mfx_page_set(0, true);   // its EQ view: H the master's LEV
  ui::remote_show_page("MASTER");
  tick();
  CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "MASTER"));
  int lev = core::master::control.lev;
  side(0x0E), turn(7, 2);
  CHECK(lock_v(2, mod::tid(mod::kMfx, 0, 0)) == (lev + 2) * 128 && core::master::control.lev == lev);
  up(0x0E), page(false);
  CHECK(!strcmp(ui::remote_page_title(), "MASTER"));
  core::ins::set_type(core::master::chain(0), core::master::slot(0), 0);
  printf("  page_t15_turn_locks_a: PAGE + T15 held + a voice knob locks scene A (no set_param, no page step); MASTER's LEV\n");
}

static void page_t16_turn_locks_b() {
  voice_page();
  scene::set_ab(2, 6);
  side(0x0F);
  int k = enc_param(1);
  turn(1, 2);
  CHECK(lock_v(6, mod::tid(mod::kV, 0, k)) == 42 * 128 && scene::state().s[2].n == 0 && scene::edit_side() == 1);
  up(0x0F), page(false);
  printf("  page_t16_turn_locks_b: PAGE + T16 locks scene B\n");
}

static void t15_without_a_refused() {
  voice_page();
  side(0x0E);
  uint32_t edits = ui::remote_stats().edits;
  turn(1, 2);
  tick();
  CHECK(sent_has("SET A: T1-12") && ui::remote_stats().edits == edits);
  for (int n = 0; n < scene::kScenes; ++n) CHECK(scene::state().s[n].n == 0);
  up(0x0E), page(false);
  printf("  t15_without_a_refused: no A: the turn is refused, SET A: T1-12\n");
}

static void page_t15_plus_t3_sets_a_on_voice_page() {
  voice_page();
  scene::set_ab(0, 6), scene::set_fader(5000, scene::kConsole);
  side(0x0E);
  tap(0x02);                                // T3: scene 3 becomes A; the fader stays
  CHECK(scene::state().a == 2 && scene::state().b == 6 && scene::state().fader == 5000);
  turn(1, 1);
  CHECK(scene::state().s[2].n == 1);
  up(0x0E), page(false);
  side(0x0F), tap(0x04), up(0x0F), page(false);   // PAGE + T16 + T5: B = 5
  CHECK(scene::state().a == 2 && scene::state().b == 4 && ui::remote_stats().voice == 0);
  printf("  page_t15_plus_t3_sets_a_on_voice_page: PAGE + T15 + T3: A = 3; PAGE + T16 + T5: B = 5\n");
}

static void t1_alone_not_a_scene_key() {
  voice_page();
  scene::set_ab(0, 1);
  int k = enc_param(1);
  uint32_t edits = ui::remote_stats().edits;
  down(0x00), turn(1, 1), up(0x00);         // (a T1 the Rytm keeps: no EDIT)
  down(0x0E), turn(1, 1), up(0x0E);         // T15 without PAGE: the Rytm's too
  CHECK(scene::state().s[0].n == 0 && scene::state().a == 0 && ui::remote_stats().edits == edits + 2);
  (void)k;
  printf("  t1_alone_not_a_scene_key: T1 / T15 alone are the Rytm's, a turn edits the voice\n");
}

static void page_alone_still_steps() {
  voice_page();
  page(true), tick();
  CHECK(ui::remote_stats().voice == 0);     // held back while it may start the gesture
  page(false), tick();
  CHECK(ui::remote_stats().voice != 0);     // let go alone: PAGE's own step, as always
  printf("  page_alone_still_steps: PAGE alone steps the page at its release\n");
}

static void edit_released_on_kit_change() {
  voice_page();
  scene::set_ab(2, 6);
  side(0x0E);
  CHECK(scene::edit_side() == 0);
  const uint8_t kit[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, 4, 0xF7};
  engine_sysex(kit, 8);
  tick();
  CHECK(scene::edit_side() < 0);
  uint32_t edits = ui::remote_stats().edits;
  turn(1, 1);                               // the gesture let go: an edit, no lock
  CHECK(ui::remote_stats().edits == edits + 1);
  up(0x0E), page(false);
  printf("  edit_released_on_kit_change: a kit change ends the held EDIT\n");
}

static void edit_off_trigs_stock() {
  voice_page();
  CHECK(!(g_mask_trigs & 0xFFFF));          // nothing held: the trig keys are the Rytm's on a voice page
  int k = enc_param(1);
  uint32_t edits = ui::remote_stats().edits;
  down(0x02), turn(1, 1), up(0x02);         // (a T3 the Rytm would keep: nothing locks)
  CHECK(scene::state().s[2].n == 0 && ui::remote_stats().edits == edits + 1);
  (void)k;
  printf("  edit_off_trigs_stock: no PAGE + T15 / T16 - no trig keys caught, a turn edits the voice\n");
}

static void locked_value_shown_while_held() {
  voice_page();
  scene::set_ab(2, 6);
  tick();
  CHECK(!sent_has("EDIT"));
  side(0x0E);
  tick();
  CHECK(sent_has("BD EDIT A"));             // the header keeps the voice (its track: UX 7's "V1 EDIT A")
  turn(1, 5);
  tick();
  char want[32], nm[16];
  mod::label(mod::tid(mod::kV, 0, enc_param(1)), nm, sizeof nm);   // 2026-11: the param by its machine's name ("V1:HRM")
  CHECK(strncmp(nm, "V1:P", 4) != 0);
  snprintf(want, sizeof want, "S3 %s:45", nm);
  CHECK(sent_has(want));                    // the bottom bar: the scene's locked value
  up(0x0E), page(false);
  tick();
  CHECK(!sent_has(want) && !sent_has("EDIT"));
  printf("  locked_value_shown_while_held: BD EDIT A (the voice kept) in the header, the locked value in the bottom bar\n");
}

// ---- review: seq mode (a Sidecar seq track selected: 2D 01 08 1 t) sends every key and turn to the sequencer first
static void seq_mode(bool on) { from_rytm(0x2D, 8, on ? 1 : 0, 0), tick(); }

static void seq_mode_morph_knobs() {
  open_page();
  seq_mode(true);
  ui::remote_show_page("MORPH");
  tick();
  CHECK(!strcmp(ui::remote_page_title(), "MORPH"));
  turn(0, 2);                               // A: the fader, not the sequencer's
  CHECK(scene::state().fader == 256);
  tap(0x01);
  CHECK(scene::lock(1, T("V1:P1"), 10 * 128));
  down(0x01), turn(1, 1);                   // the list: B edits row 1 (V1:P1)
  CHECK(lock_v(1, T("V1:P1")) == 11 * 128);
  up(0x01);
  CHECK(ui::morph_page_held() == -1);
  seq_mode(false);
  printf("  seq_mode_morph_knobs: in seq mode A and the list's knobs are MORPH's\n");
}

static void seq_mode_edit_lock() {
  voice_page();
  seq_mode(true);                           // (it shows SEQ: back to V1 from SIDECAR)
  ui::remote_show_page("SIDECAR");
  tick(), tap(0x41), tap(0x41), tick();
  CHECK(ui::remote_stats().voice == 0);
  scene::set_ab(2, 6);
  int k = enc_param(1);
  side(0x0E), turn(1, 2);
  CHECK(lock_v(2, mod::tid(mod::kV, 0, k)) == 42 * 128);
  up(0x0E), page(false);
  seq_mode(false);
  printf("  seq_mode_edit_lock: in seq mode PAGE + T15 + a voice knob still locks\n");
}

static void release_while_seq_shown() {
  open_page();
  down(0x03);
  tick(10);
  CHECK(ui::morph_page_held() == 3);
  ui::remote_show_page("SEQ");
  tick();
  up(0x03);                                 // released with SEQ shown: still MORPH's release
  CHECK(ui::morph_page_held() == -1);
  voice_page();
  scene::set_ab(2, 6);
  side(0x0E);
  ui::remote_show_page("SEQ");
  tick();
  up(0x0E), page(false);                    // EDIT's key let go on SEQ
  tick();
  CHECK(scene::edit_side() < 0 && !sent_has("EDIT A"));
  ui::remote_show_page("SIDECAR");
  tick(), tap(0x41), tap(0x41), tick();
  uint32_t edits = ui::remote_stats().edits;
  turn(1, 1);                               // no scene key held any more: an edit, no lock
  CHECK(ui::remote_stats().edits == edits + 1 && scene::state().s[2].n == 0);
  printf("  release_while_seq_shown: a held key's release reaches its page even with SEQ shown\n");
}

static void not_lockable_note() {
  voice_page();
  scene::set_ab(0, 1);
  core::ins::set_type(core::master::chain(0), core::master::slot(0), core::ins::kComp);
  ui::mfx_page_set(0, false);
  ui::remote_show_page("MASTER");
  tick();
  side(0x0E), turn(4, 1);                   // E: the slot (a list, no target)
  tick();
  CHECK(sent_has("NOT LOCKABLE") && scene::state().s[0].n == 0);
  up(0x0E), page(false);
  core::ins::set_type(core::master::chain(0), core::master::slot(0), 0);
  printf("  not_lockable_note: a knob with no target under a held scene key says NOT LOCKABLE\n");
}

// ---- v2 Task 7: the screen and safety items
static bool text_is(const char* s) {   // a text item exactly s (the header's right slot, a cell)
  draw();
  for (const ui::TextItem& t : g_c.txt)
    if (!strcmp(t.s, s)) return true;
  return false;
}
static bool lit(int x, int y) {
  uint8_t fb[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  return fb[(7 - y / 8) + 8 * x] >> (y % 8) & 1;
}
static int bar_x(int f) { return 18 + 2 + f * 87 / 16383; }   // (kBarX + 2, inner - 1 = 87)
static void hello_caps2(int c2) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, 1, (uint8_t)c2, 0xF7};
  engine_sysex(m, (int)sizeof m);
}
static void header_shows_source_tag() {
  open_page();
  scene::set_ab(0, 1);
  CHECK(!text_is("PAGE") && !text_is("KNOB"));   // nobody moved it yet
  turn(0, 1);
  CHECK(text_is("PAGE"));
  scene::set_fader(100, scene::kTouch);
  CHECK(text_is("TCH"));
  scene::set_fader(200, scene::kWeb);
  CHECK(text_is("WEB"));
  scene::set_fader(300, scene::kLearn);
  CHECK(text_is("MIDI"));
  scene::set_fader(400, scene::kQperf);
  CHECK(text_is("KNOB"));
  png("morph_source_tag.png");
  printf("  header_shows_source_tag: the header names the fader's source (PAGE TCH WEB MIDI KNOB)\n");
}
static void qperf_tick_when_knob_differs() {
  open_page();
  scene::set_qp_mode(scene::kQpAlways);
  scene::set_ab(0, 1);
  scene::qperf(200, 0, 2);   // a baseline: the knob at 200, the fader still at 0
  const int kx = bar_x(200 * 16383 / 255);
  CHECK(lit(kx, 33) && lit(kx, 34) && !lit(kx + 3, 33));
  png("morph_qperf_tick.png");
  scene::qperf(202, 0, 2);   // the knob takes it: no tick of its own
  tick(10);
  const int kx2 = bar_x(202 * 16383 / 255);
  CHECK(!lit(kx2, 33) && !lit(kx2, 34));
  scene::set_fader(0, scene::kPage), tick(10);
  CHECK(lit(kx2, 33));
  scene::set_qp_mode(scene::kQpOff);   // OFF: the knob is not the fader's
  CHECK(!lit(kx2, 33));
  scene::set_qp_mode(scene::kQpMutedOnly);   // MUTED ONLY from a mod without the mute report acts as OFF (review L5)
  hello_caps2(0);
  CHECK(!lit(kx2, 33));
  printf("  qperf_tick_when_knob_differs: a tick under the bar where the QPERF knob is, when the fader is elsewhere\n");
}
// a QPERF knob turn while the sequencer plays: the bar is committed every kQpCommitMs, not at the 200 ms play cap
static int qperf_run(bool playing, bool encoder, uint32_t* min_gap, bool* settled) {
  open_page();
  scene::set_qp_mode(scene::kQpAlways);
  scene::set_ab(0, 1);
  scene::qperf(0, 0, 2);   // a baseline
  tick(10);
  rytm_dump::set_playing(playing);
  g_commit_ms.clear();
  for (int ms = 0; ms < 400; ++ms) {
    at(g_us + 1000);
    if (ms % 8 == 0) {
      if (encoder) turn(0, 1);
      else scene::qperf(ms * 255 / 400, 0, 2);
    }
    scene::tick(now_ms());
    ui::remote_tick(now_ms());
  }
  *min_gap = 100000;
  for (size_t i = 1; i < g_commit_ms.size(); ++i)
    if (g_commit_ms[i] - g_commit_ms[i - 1] < *min_gap) *min_gap = g_commit_ms[i] - g_commit_ms[i - 1];
  const int n = (int)g_commit_ms.size();
  for (int i = 0; i < 20; ++i) at(g_us + 30000), scene::tick(now_ms()), ui::remote_tick(now_ms());
  uint8_t fb[ui::kBytes], sent[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  ui::preview_sent(sent);
  *settled = !memcmp(fb, sent, ui::kBytes);
  rytm_dump::set_playing(false);
  return n;
}
static void qperf_follows_while_playing() {
  uint32_t gap = 0;
  bool settled = false;
  const int n = qperf_run(true, false, &gap, &settled);
  printf("    playing, QPERF: %d commits in 400 ms, min gap %u ms\n", n, (unsigned)gap);
  CHECK(n >= 6 && gap >= 50 && settled);
  const int stopped = qperf_run(false, false, &gap, &settled);
  CHECK(stopped >= 6 && settled);
  qperf_run(true, true, &gap, &settled);   // encoder A while playing: a user event, as before (no 50 ms floor)
  CHECK(settled);
  printf("  qperf_follows_while_playing: a QPERF turn while playing commits every 50 ms (stopped: %d)\n", stopped);
}
// the glide's last step: a QPERF move then the knob stops; the bar's final frame goes out within ~50 ms of the glide's end
static void qperf_glide_end_not_held() {
  open_page();
  scene::set_qp_mode(scene::kQpAlways);
  scene::set_ab(0, 1);
  scene::qperf(0, 0, 2);   // a baseline
  tick(10);
  rytm_dump::set_playing(true);
  for (int ms = 0; ms < 113; ++ms) {   // a turn: reports every 8 ms up to ms 112
    at(g_us + 1000);
    if (ms % 8 == 0) scene::qperf(ms * 100 / 112, 0, 2);
    scene::tick(now_ms());
    ui::remote_tick(now_ms());
  }
  g_commit_ms.clear();
  at(g_us + 51000);
  scene::tick(now_ms()), ui::remote_tick(now_ms());
  scene::qperf(255, 0, 2);   // one more move, ~51 ms after the last report: an 80 ms glide; the last commit must carry the target
  const uint32_t t0 = now_ms();
  for (int ms = 0; ms < 600; ++ms) {
    at(g_us + 1000);
    scene::tick(now_ms());
    ui::remote_tick(now_ms());
  }
  CHECK(scene::fader_played() == scene::state().fader);
  uint8_t fb[ui::kBytes], sent[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  ui::preview_sent(sent);
  CHECK(!memcmp(fb, sent, ui::kBytes));
  CHECK(!g_commit_ms.empty());
  scene::reset();   // the tick's last-played memory goes with the state (a stale one would flag the next tick)
  scene::tick(now_ms());
  CHECK(scene::fader_played() == 0);
  const uint32_t last = g_commit_ms.empty() ? 0 : g_commit_ms.back() - t0;
  printf("    glide end: last commit %u ms after the move\n", (unsigned)last);
  CHECK(last <= 51 + 50);   // the glide + the cap (not the 200 ms play cap)
  rytm_dump::set_playing(false);
}
static void hint_turn_quick_perf() {
  open_page();
  scene::set_qp_mode(scene::kQpAlways);
  scene::set_ab(0, 1);
  CHECK(scene::lock(0, T("V1:P1"), 128));
  CHECK(shows("TURN QUICK PERF"));   // the knob has not sent since the Rytm booted
  scene::qperf(10, 0, 2);
  CHECK(!shows("TURN QUICK PERF") && shows("HOLD T1-12: LOCKS"));
  // MUTED ONLY from a mod that reports the mute: a turn with the macros playing says how
  hello_caps2(ui::kCaps2QpMute);
  scene::set_qp_mode(scene::kQpMutedOnly);
  scene::qperf(40, 0, 2);
  CHECK(shows("FUNC+QPER: MUTE FOR FADER"));
  png("morph_hint_mute.png");
  at(g_us + 2100000);
  CHECK(!shows("FUNC+QPER"));
  scene::qperf(41, 0, 6), scene::qperf(60, 0, 6);   // muted: a baseline, then the knob
  CHECK(!shows("FUNC+QPER") && scene::state().fader == 60 * 16383 / 255);
  hello_caps2(0);
  printf("  hint_turn_quick_perf: TURN QUICK PERF until the knob sent; FUNC+QPER: MUTE FOR FADER in MUTED ONLY\n");
}
static void edit_tag_on_morph_page() {
  open_page();
  scene::set_ab(0, 1);
  scene::set_edit_side(1);
  CHECK(text_is("EDIT B"));
  scene::set_edit_side(0);
  CHECK(text_is("EDIT A"));
  scene::set_edit_side(-1);
  CHECK(!text_is("EDIT A"));
  printf("  edit_tag_on_morph_page: EDIT A / B in the MORPH header while held\n");
}
static void ab_boxed_in_row() {
  open_page();
  scene::set_ab(2, 5);   // boxes at x 4 + 10 n: a frame one column out
  CHECK(lit(23, 13) && lit(33, 13) && lit(53, 13) && lit(63, 13) && !lit(43, 13));
  scene::set_ab(7, 7);   // A = B: one box framed
  CHECK(lit(73, 13) && lit(83, 13) && !lit(23, 13));
  png("morph_ab_boxes.png");
  // the selected scene that is A too: its LED blinks between A's colour and white
  scene::set_ab(2, 5);
  tap(0x02);
  bool seen_a = false, seen_w = false;
  for (int k = 0; k < 40; ++k) {
    tick(1);
    seen_a |= g_led[2] == ui::kLedA, seen_w |= g_led[2] == 111;
  }
  CHECK(seen_a && seen_w && g_led[5] == ui::kLedB);
  printf("  ab_boxed_in_row: A and B framed in the scene row (A = B one); the selected A blinks\n");
}
static void hollow_bar_without_ab() {
  open_page();
  scene::set_fader(16383, scene::kConsole);
  for (int k = 0; k < 10; ++k) at(g_us + 5000), mod::tick(now_us());   // (the played fader: the layer's tick)
  CHECK(scene::fader_played() == 16383);
  scene::set_ab(0, -1);
  int on = 0;
  for (int x = 21; x < 100; ++x)
    for (int y = 25; y <= 29; ++y) on += lit(x, y);
  CHECK(on == 0 && shows("T15/16 + T1-12: SET A / B"));
  png("morph_hollow.png");
  scene::set_ab(0, 1);
  on = 0;
  for (int x = 21; x < 100; x += 3) on += lit(x, 26) + lit(x + 1, 26) + lit(x, 27) + lit(x + 1, 27);
  CHECK(on > 0 && shows("PAGE+T15/16+TURN: LOCK"));   // both set, both empty: how to lock
  printf("  hollow_bar_without_ab: A or B unset, the bar hollow and SET A / B; both empty: PAGE + T15 / 16\n");
}
static void kit_change_header_note() {
  open_page();
  const uint8_t kit[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, 11, 0xF7};
  engine_sysex(kit, 8);
  tick();
  CHECK(text_is("KIT 12: NO SCENES"));
  CHECK(scene::lock(0, T("V1:P1"), 128));
  CHECK(text_is("KIT 12 LOADED"));
  png("morph_kit_note.png");
  at(g_us + 2100000);
  CHECK(!text_is("KIT 12 LOADED") && !text_is("KIT 12: NO SCENES"));
  printf("  kit_change_header_note: KIT n LOADED / KIT n: NO SCENES in the header for 2 s\n");
}
static void no_removes_then_undo() {
  open_page();
  CHECK(scene::lock(0, T("RT12:FTYP"), 3 * 128) && scene::lock(0, T("MFX:LEV"), 64 * 128));
  down(0x00), tick(10);
  tap(0x45);   // NO: RT12:FTYP goes, the undo offered
  CHECK(scene::state().s[0].n == 1 && lock_v(0, T("RT12:FTYP")) < 0 && shows("RT12:FTYP GONE NO:UNDO"));
  png("morph_undo.png");
  tap(0x45);   // NO again: back
  CHECK(scene::state().s[0].n == 2 && lock_v(0, T("RT12:FTYP")) == 3 * 128 && !shows("GONE NO:UNDO"));
  // review L3: a kit change inside the 2 s drops the undo (the scene is another kit's now)
  tap(0x45);
  CHECK(scene::state().s[0].n == 1 && shows("GONE NO:UNDO"));
  const uint8_t kit[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, 20, 0xF7};
  engine_sysex(kit, 8);
  CHECK(!shows("GONE NO:UNDO"));
  int n = scene::state().s[0].n;
  tap(0x45);   // NO now removes (or does nothing on an empty list): it never writes the old kit's lock
  CHECK(lock_v(0, T("RT12:FTYP")) < 0 && scene::state().s[0].n <= n);
  up(0x00);
  printf("  no_removes_then_undo: NO removes with <name> GONE NO:UNDO; NO again restores; a kit change drops it\n");
}
static void undo_expires_2s() {
  open_page();
  CHECK(scene::lock(0, T("MFX:LEV"), 64 * 128) && scene::lock(0, T("V2:P2"), 256));
  down(0x00), tick(10);
  tap(0x45);
  CHECK(scene::state().s[0].n == 1);
  at(g_us + 2100000);
  CHECK(!shows("GONE NO:UNDO"));
  tap(0x45);   // past 2 s: NO removes the next row, it does not restore
  CHECK(scene::state().s[0].n == 0 && lock_v(0, T("MFX:LEV")) < 0);
  up(0x00);
  // a released key drops the undo
  CHECK(scene::lock(0, T("MFX:LEV"), 64 * 128));
  down(0x00), tick(10), tap(0x45), up(0x00), down(0x00), tick(10), tap(0x45), up(0x00);
  CHECK(scene::state().s[0].n == 0);
  printf("  undo_expires_2s: after 2 s (or the key let go) NO removes, it does not restore\n");
}
// review M1: PAGE held while the page closes: no stuck PAGE, so T15 alone later is a trig, not EDIT A
static void page_held_across_close() {
  voice_page();
  scene::set_ab(2, 6);
  page(true), tick();
  CHECK(g_mask_trigs & 0x4000);
  from_rytm(0x30, 0, 0, 0), tick();   // the page closes (its PAGE release goes to the Rytm, not here)
  from_rytm(0x30, 0, 1, 0), tick();
  tap(0x41), tap(0x41), tick();
  CHECK(ui::remote_stats().voice == 0 && !(g_mask_trigs & 0xC000));
  down(0x0E), tick();
  CHECK(scene::edit_side() < 0);
  up(0x0E);
  printf("  page_held_across_close: a PAGE held across a close leaves no stuck PAGE (T15 alone is no EDIT)\n");
}
static void stale_target_marked() {
  voice_page();   // V1 PLAITS (all 16 Ps used); V2 a machine that leaves a P unused
  int p = -1;
  char b[16];
  static const int kMach[] = {kJuno, kDaisy, kDspSample, kFm};
  for (int mi = 0; mi < 4 && p < 0; ++mi) {
    uint8_t m[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kMach[mi] | 1)};
    for (int k = 0; k < 16; ++k) m[6 + 2 * k] = 40, m[7 + 2 * k] = 0;
    m[38] = 0xF7;
    engine_sysex(m, 39);
    for (int k = 15; k >= 0 && p < 0; --k)
      if (!strcmp(mod::param_label(mod::tid(mod::kV, 1, k), b, sizeof b), "-")) p = k;
  }
  CHECK(p >= 0);
  const Tid t = mod::tid(mod::kV, 1, p);
  CHECK(scene::lock(0, T("V1:P1"), 128) && scene::lock(0, t, 128));
  CHECK(ui::morph_stale(t) && !ui::morph_stale(T("V1:P1")) && !ui::morph_stale(T("MFX:LEV")));
  ui::remote_show_page("MORPH");
  tick();
  down(0x00), tick(10);
  char want[16];
  snprintf(want, sizeof want, "V2:P%d?", p + 1);
  CHECK(shows(want) && shows("V1:ENG") && !shows("V1:ENG?"));
  png("morph_stale.png");
  up(0x00);
  static char js[60000];
  CHECK(scene::json(js, sizeof js) > 0);
  char g[32];
  snprintf(g, sizeof g, "\"gone\":[\"V2:P%d\"]", p + 1);
  CHECK(strstr(js, g));
  printf("  stale_target_marked: a lock on a P the machine leaves unused shows V2:P%d? (and the JSON's gone)\n", p + 1);
}

int main() {
  hooks.to_rytm = out;
  at(g_us);
  engine_begin();
  select_and_ab_by_holds();
  fader_encoder_steps();
  hold_shows_lock_list_and_edits();
  no_removes_row();
  yes_clears_after_confirm();
  no_scn_perf_cells();
  add_row_rt();
  add_row_rfx();
  add_row_voice();
  add_row_full_refused();
  leds();
  text_fit();
  page_t15_turn_locks_a();
  page_t16_turn_locks_b();
  t15_without_a_refused();
  page_t15_plus_t3_sets_a_on_voice_page();
  t1_alone_not_a_scene_key();
  page_alone_still_steps();
  edit_released_on_kit_change();
  edit_off_trigs_stock();
  locked_value_shown_while_held();
  seq_mode_morph_knobs();
  seq_mode_edit_lock();
  release_while_seq_shown();
  not_lockable_note();
  header_shows_source_tag();
  qperf_tick_when_knob_differs();
  qperf_follows_while_playing();
  qperf_glide_end_not_held();
  hint_turn_quick_perf();
  edit_tag_on_morph_page();
  ab_boxed_in_row();
  hollow_bar_without_ab();
  kit_change_header_note();
  no_removes_then_undo();
  undo_expires_2s();
  stale_target_marked();
  page_held_across_close();
  scene::reset();
  ui::remote_reset();
  printf("test_morph_page: ok\n");
  return 0;
}
