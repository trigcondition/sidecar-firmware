// core/tests/test_polymake.cpp - rui1051 (hardware 2026-10-25): making a track a poly track at runtime must give it a
// pattern track to edit, whatever else the pattern holds.
//
// The report: CH made a poly track (POLY CONFIG's YES, the machine WAVETABLE) on a pattern that had been played for a
// while drew "EMPTY PAGE" on the SEQ page and took no trigs from the step keys, while BD - a poly track since the
// pattern loaded - edited normally; a new pattern was fine. The cause: a Pattern holds kTracks (4) track slots, and the
// Rytm's own stock edits (2D 0A: a page CLEAR / PASTE / UNDO, a track UNDO) created a slot for ANY Rytm track they
// named, seq track or not. Four of those (BD SD RS CP in the test) fill the pattern, they are packed into pNNN.sqp and
// come back on every load, and set_seq() then set the mask bit while track_of(create) found no free slot: a track the UI
// calls poly with nowhere to write. toggle() (a step key's tap) and the SEQ page's roll both read that missing track.
//
// What this checks: the four slots eaten by stock edits alone, then CH (WAVETABLE) and OH (PLAITS) converted and a step
// key tapped on each - the trig has to land. Then (per pattern poly, 2026-10-26) another pattern is plain until a track
// is made poly there; and on a pattern that holds four real seq tracks the fifth conversion is refused (the mask bit
// is not set) instead of lying about it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "core_port.h"
#include "engine.h"
#include "machines.h"
#include "pool/pool.h"
#include "project_key.h"
#include "seq/seq.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;
using namespace core::ui;

static uint64_t g_us = 1000000;
static uint32_t g_ms = 10000;

static void from_rytm(std::vector<uint8_t> body, uint8_t cmd) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void report(int vb) {   // the voice's 03 (its machine), P1..P16 at 64
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)vb};
  for (int k = 0; k < 16; ++k) m.push_back(64), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void machine(int t, int m) {   // 33: track t's voice byte
  from_rytm({(uint8_t)t, 1, 44, (uint8_t)(m | remote_track_voice(t))}, 0x33);
  report(m | remote_track_voice(t));
}
static void key(int code) { from_rytm({0, (uint8_t)code, 1}, 0x31), from_rytm({0, (uint8_t)code, 0}, 0x31); }
static void tick() { remote_tick(g_ms += 60); }
// 2026-10-26, the RAM store (phase 2): a project's sequences are loaded once its store is filled (from the idle pass)
static void loaded() {
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
// 2D 0A EDIT: op on pattern p, Rytm track r, ok, src (0 the Rytm's own stock function)
static void edit(int op, int p, int r, int src) {
  from_rytm({seq::kEdit, (uint8_t)op, (uint8_t)p, (uint8_t)r, 1, (uint8_t)src, 0, 0, 0}, 0x2D);
}
static int slots() {
  const seq::Pattern& p = seq::edit_pattern();
  int n = 0;
  for (const seq::Track& t : p.t) n += t.rytm >= 0;
  return n;
}
static void dump_slots(const char* what) {
  const seq::Pattern& p = seq::edit_pattern();
  printf("  %s: slots", what);
  for (const seq::Track& t : p.t) printf(" %d", t.rytm);
  printf(" (mask");
  for (int r = 0; r < 12; ++r)
    if (seq::owns_track(r)) printf(" %d", r + 1);
  printf(")\n");
}
// POLY CONFIG's YES on track t (the device's page request 30 page 0x20 + t, rytm-mods 0026 rui1048), then the SEQ page
// of that track (30 page 0x10 + t) and a tap on step key k: the step that got a trig (-1 none)
static int make_and_tap(int t, int k) {
  from_rytm({(uint8_t)(0x20 + t), 1, 0}, 0x30);
  tick();
  CHECK(remote_page_title() && !strcmp(remote_page_title(), "POLY") && poly_page_track() == t);
  key(0x44);   // YES: make it a poly track
  tick();
  from_rytm({0, 0, 1}, 0x30);   // the Rytm closed the page
  if (!seq::owns_track(t)) return -2;
  from_rytm({(uint8_t)(0x10 + t), 1, 0}, 0x30);   // TRK + TRIG: its SEQ page
  tick();
  CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
  key(k);
  tick();
  const seq::Track* tr = seq::edit_track(t, false);
  from_rytm({0, 0, 1}, 0x30);
  if (!tr) return -1;
  return tr->step[k].n ? k : -1;
}

static const char* say_got(int got, int want) {
  return got == want ? "a trig" : got == -2 ? "NOTHING (not even called a seq track)" : "NOTHING";
}

int main() {
  char tmpl[] = "/tmp/core_polymake_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'M', 'A', 'K', 'E', 0xF7};
  engine_sysex(proj, sizeof proj);
  loaded();
  remote_reset();
  for (int t = 0; t < 12; ++t) machine(t, t == 8 ? kWaveMachine : kPlaits);
  from_rytm({seq::kPattern, 0, 0x7F}, 0x2D);   // pattern A01

  // ---- a used pattern: four tracks were poly here once and are not any more (their slots and steps stay, by design,
  // and are written into pNNN.sqp) - the four slots are taken by tracks nothing sequences
  CHECK(slots() == 0);
  {
    char b[200];
    for (int r = 0; r < 4; ++r) {
      char c[32];
      snprintf(c, sizeof c, "seq=track:%d:on", r + 1), seq::command(c, b, sizeof b);
      snprintf(c, sizeof c, "seq=track:%d:off", r + 1), seq::command(c, b, sizeof b);
    }
  }
  dump_slots("four tracks made poly here once and turned off again");
  CHECK(slots() == seq::kTracks);

  // ---- CH (WAVETABLE) and OH (PLAITS) made poly tracks, then a step key on each: the trig has to land, and the
  // machine is not what decides it (both are reported before either is checked: the hardware report named WAVETABLE)
  int ch = make_and_tap(8, 3), oh = make_and_tap(9, 5);
  printf("  CH (WAVETABLE) converted: step key 4 -> %s\n", say_got(ch, 3));
  printf("  OH (PLAITS)    converted: step key 6 -> %s\n", say_got(oh, 5));
  dump_slots("both converted");
  CHECK(ch == 3 && oh == 5);

  // ---- per pattern poly (2026-10-26, replacing fix1051b's "the mask is the project's"): CH and OH are poly tracks in
  // A01, not in A08 - another pattern is plain Rytm until a track is made poly there; back in A01 both are again, with
  // their trigs
  from_rytm({seq::kPattern, 7, 0x7F}, 0x2D);   // A08
  while (seq::idle()) {}
  tick();
  dump_slots("after the switch to A08");
  CHECK(!seq::owns_track(8) && !seq::owns_track(9) && !seq::any_tracks());
  CHECK(make_and_tap(8, 7) == 7);   // made poly here too: a slot of its own in A08, the trig lands
  dump_slots("CH made poly in A08");
  CHECK(seq::owns_track(8) && !seq::owns_track(9));
  from_rytm({seq::kPattern, 0, 0x7F}, 0x2D);   // A01 again
  while (seq::idle()) {}
  tick();
  dump_slots("back in A01");
  CHECK(seq::owns_track(8) && seq::owns_track(9));
  CHECK(seq::edit_track(8, false) && seq::edit_track(8, false)->step[3].n == 1);
  from_rytm({seq::kPattern, 7, 0x7F}, 0x2D);   // A08 for the rest
  while (seq::idle()) {}
  tick();

  // ---- four real seq tracks: the fifth conversion has nowhere to go and must refuse (no mask bit), not pretend
  {
    char b[200];
    for (int r = 0; r < 12 && slots() < seq::kTracks; ++r) {
      char c[32];
      snprintf(c, sizeof c, "seq=track:%d:on", r + 1);
      seq::command(c, b, sizeof b);
    }
    dump_slots("four seq tracks");
    CHECK(slots() == seq::kTracks);
    // fix1053: a slot with no steps can be taken by a track that needs one, so "every slot taken" means four tracks
    // that hold steps: one each
    for (const seq::Track& x : seq::edit_pattern().t) {
      char c[40];
      snprintf(c, sizeof c, "seq=step:%d:1:60", x.rytm + 1);
      seq::command(c, b, sizeof b);
      CHECK(seq::used_steps(x) > 0);
    }
    int t = -1;
    for (int r = 0; r < 12 && t < 0; ++r)
      if (!seq::owns_track(r) && !seq::edit_track(r, false)) t = r;
    CHECK(t >= 0);
    from_rytm({(uint8_t)(0x20 + t), 1, 0}, 0x30);
    tick();
    key(0x44);
    tick();
    printf("  a fifth track with every slot taken: %s\n", seq::owns_track(t) ? "CALLED POLY (wrong)" : "refused");
    CHECK(!seq::owns_track(t));
    from_rytm({0, 0, 1}, 0x30);
  }

  // ---- the device's case (audit 2026-10-26): a project whose tracks.txt has more seq tracks than a pattern has slots
  // (mask 0x737: BD SD RS BT LT CH OH CY, eight), and a pattern nobody used yet. Per pattern poly: mask= is no longer
  // membership - the new pattern is plain; CH made poly there takes a slot and its step key lands.
  {
    char pk[64];
    project_key("WIDE", pk, sizeof pk);
    std::string dir = card + "/.rytm/seq/" + pk;
    CHECK(system(("mkdir -p '" + dir + "'").c_str()) == 0);
    FILE* f = fopen((dir + "/tracks.txt").c_str(), "w");
    CHECK(f);
    fputs("mask=0x737\n", f);
    fclose(f);
    const uint8_t wide[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'W', 'I', 'D', 'E', 0xF7};
    engine_sysex(wide, sizeof wide);
    loaded();
    from_rytm({seq::kPattern, 2, 0x7F}, 0x2D);   // A03: never used in this project
    while (seq::idle()) {}
    tick();
    dump_slots("WIDE A03, tracks.txt mask=0x737, an empty pattern");
    CHECK(!seq::any_tracks() && slots() == 0);
    int got = make_and_tap(8, 2);
    printf("  WIDE A03: CH made poly, step key 3 -> %s\n", say_got(got, 2));
    dump_slots("after the tap");
    CHECK(got == 2);
  }

  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_polymake: OK\n");
  return 0;
}
