// core/tests/test_page_keys.cpp - rui1046 / rui1052: the device's shortcut table (ui/page_keys.h) and the Pi's routing
// of the Rytm's page requests (pi/engine/voice_pi.cpp, compiled in here with the core): UP + T1..T13 (49 1 <code>) to
// the touchscreen's pages and OwnPages taking the pages the touchscreen has.
// rui1052: T1..T8 are the touchscreen's sidebar top to bottom (pi/ui/sidecar_ui.c kNavPage, checked against the painted
// captions in pi/ui/host/shot.c), T9..T13 the pages with no sidebar row.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "engine.h"
#include "ui/page_keys.h"
#include "ui/remote.h"

#include "../../pi/engine/voice_pi.cpp"   // (its namespace; the core's functions it calls link from CORE_SRC)

using namespace core;

// (pi/ui/rytm_pattern.c, used by voice_pi::Fill only: not called here)
extern "C" int rytm_locks_on(const rytm_pattern_t*, int, int) { return 0; }

static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  core::engine_sysex(m, 10);
}

int main() {
  // ---- the table: one page request a code 1..13, a name each; the combos the host asks for by id
  const sc_keymap_t* m = sc_keymap();
  CHECK(sc_page_max() == 13 && m->npages == 13 && sc_streq(m->modifier, "UP"));
  for (int c = 1; c <= 13; ++c) CHECK(sc_page_of_code(c) && sc_page_of_code(c)->name && sc_page_of_code(c)->name[0]);
  CHECK(!sc_page_of_code(0) && !sc_page_of_code(14));
  // rui1052: the order, as the rows stand - T1..T8 the touchscreen's sidebar top to bottom, then the rest
  static const char* const kOrder[13] = {"HOME", "SEQ",  "PAT",     "VOICE",  "INSERT FX", "SONG",   "REC",
                                         "SET",  "SCOPE", "STATUS", "SAMPLE", "PROJECT",   "POOL"};
  for (int c = 1; c <= 13; ++c) {
    CHECK(sc_streq(sc_page_of_code(c)->name, kOrder[c - 1]));
    CHECK(sc_page_of_name(kOrder[c - 1])->code == c);            // and back, one key a page
    CHECK(sc_page_of_code(c) == &m->pages[c - 1]);               // the rows in that order, no gap
  }
  CHECK(!sc_page_of_name("NOPE"));
  // the touchscreen's own pages are the rows with no title on the Rytm's screen (their keys, not a leading run)
  for (int c = 1; c <= 13; ++c)
    CHECK(!sc_page_of_code(c)->rytm == (c <= 5 || c == 8));
  CHECK(!strcmp(sc_combo_keys("pages"), "UP+YES") && !strcmp(sc_combo_keys("seq"), "UP+TRIG") &&
        !strcmp(sc_combo_keys("lfo"), "UP+LFO") && !strcmp(sc_combo_keys("ins"), "UP+FX") && !strcmp(sc_combo_keys("out"), "UP+AMP") &&
        !strcmp(sc_combo_keys("srclist"), "UP+SRC") && !strcmp(sc_combo_keys("copy"), "T+UP+REC") && !*sc_combo_keys("x"));
  for (int i = 0; i < m->ncombos; ++i) {   // every combo on either map fits the map's column (12 characters)
    CHECK(!m->combos[i].map || strlen(m->combos[i].map) <= 12);
    CHECK(!m->combos[i].alone || strlen(m->combos[i].alone) <= 12);
    CHECK(!m->combos[i].alone || !m->combos[i].map);   // one map or the other, never both
  }
  for (int i = 0; i < m->npages; ++i) {   // and every page request's "T11 SAMPLE"
    char b[24];
    snprintf(b, sizeof b, "%s%d %s", m->map_key, m->pages[i].code, m->pages[i].name);
    CHECK(strlen(b) <= 12);
  }
  // rui1052: both maps fit the Rytm's 18 rows - with a touchscreen the pages only it has are one row of their keys,
  // without one they are off the map and the combos with an `alone` text take the rows they free
  {
    int only = 0, rytm = 0, map = 0, alone = 0;
    for (int i = 0; i < m->npages; ++i) m->pages[i].rytm ? ++rytm : ++only;
    for (int i = 0; i < m->ncombos; ++i) map += m->combos[i].map != 0, alone += m->combos[i].alone != 0;
    printf("  map rows: with a touchscreen %d (1 + %d pages + %d combos), without %d (%d pages + %d + %d combos)\n",
           1 + rytm + map, rytm, map, rytm + map + alone, rytm, map, alone);
    CHECK(only > 0 && 1 + rytm + map <= 18 && rytm + map + alone <= 18);
  }

  // ---- the Pi's routing: each code to its touchscreen page (sidecar_ui.h's numbers), SAMPLE none
  ui::remote_reset();
  static const int want[14] = {-1, 0, 4, 5, 8, 9, 10, 11, 1, 12, 13, -1, 14, 15};   // (4: VOICE, an analog track)
  uint32_t seen = 0, tseen = 0;
  voice_pi::Seen(&seen, &tseen);
  for (int c = 1; c <= 13; ++c) {
    from_rytm(0x49, 1, (uint8_t)c, 0);
    int pg = voice_pi::Request(&seen, false, &tseen);
    printf("  49 1 %d (%s) -> touchscreen page %d\n", c, sc_page_of_code(c)->name, pg);
    CHECK(pg == want[c]);
  }
  CHECK(voice_pi::Request(&seen, false, &tseen) == -1);   // nothing new
  from_rytm(0x49, 1, 14, 0);                               // past the table: not a request
  CHECK(voice_pi::Request(&seen, false, &tseen) == -1);

  // ---- OwnPages: the pages with a title on the Rytm's screen that the touchscreen has (SONG REC SCOPE STATUS PROJECT
  // POOL), not SAMPLE and not the touchscreen's own pages (no code to own). It is the engine's one "a touchscreen is
  // up" signal (remote_display_present), so it is false until OwnPages and false again when the pages are given back.
  CHECK(!ui::remote_display_present());
  voice_pi::OwnPages();
  for (int c = 1; c <= 13; ++c) CHECK(ui::remote_display_owned(c) == (c == 6 || c == 7 || c == 9 || c == 10 || c == 12 || c == 13));
  CHECK(ui::remote_display_present());
  ui::remote_display_owns(0);
  CHECK(!ui::remote_display_present());
  printf("test_page_keys: ok (the table in sidebar order; UP + T1..T13 routed, T11 SAMPLE the Rytm's; OwnPages 6 7 9 10 12 13)\n");
  return 0;
}
