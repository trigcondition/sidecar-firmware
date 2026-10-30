// core/tests/test_poly_page.cpp - rui1048 (docs/SEQUENCER.md "E0 on the Rytm"): the POLY CONFIG page on the Rytm's screen
// (core/ui/page_poly.cpp), opened by the device's page request 30 page 0x20 + t (rytm-mods 0026: its track menu's POLY
// CONFIG item, FUNC + CHRO) and from the SIDECAR MENU: a Sidecar track not yet a poly track (YES makes it one), the six
// knobs as the console's seq=poly (pattern and default), the info line (inst::describe_short), the own-pool machines'
// VOICES only, GROUP stored only, an analog track (why not), NO back to the stock screen; and the SEQ page's make prompt
// on every Sidecar machine kind (E0) with the analog track the only one explained.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "core_port.h"
#include "engine.h"
#include "inst.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "rui_png.h"
#include "seq/seq.h"
#include "ui/extra.h"
#include "ui/page_keys.h"
#include "ui/remote.h"

using namespace core;
using namespace core::ui;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
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
static void machine(int t, int vb) { from_rytm({(uint8_t)t, 1, 44, (uint8_t)vb}, 0x33); }   // 33: track t's voice byte
static void key(int code) { from_rytm({0, (uint8_t)code, 1}, 0x31), from_rytm({0, (uint8_t)code, 0}, 0x31); }
static void turn(int e, int d, bool func = false) { from_rytm({(uint8_t)e, (uint8_t)(64 + d), (uint8_t)func}, 0x32); }
static void tick() { remote_tick(g_ms += 60); }
static std::vector<std::string> texts() {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  std::vector<std::string> v;
  for (int k = 0; k < n; ++k) v.emplace_back((const char*)t + 36 * k + 4);
  return v;
}
static bool shown(const char* s) {
  for (const std::string& x : texts())
    if (x == s) return true;
  return false;
}
static bool shown_prefix(const char* s) {
  for (const std::string& x : texts())
    if (!x.compare(0, strlen(s), s)) return true;
  return false;
}
static void dump(const char* what) {
  printf("  %s:", what);
  for (const std::string& x : texts()) printf(" [%s]", x.c_str());
  printf("\n");
}
static std::string cmd(const char* c) {
  char b[400];
  CHECK(seq::command(c, b, sizeof b));
  return b;
}
static int sent_3a() {
  int n = 0;
  for (auto& m : g_out)
    if (m.size() > 6 && m[4] == 0x3A) ++n;
  return n;
}

int main() {
  char tmpl[] = "/tmp/core_polypage_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'O', 'L', 'Y', 0xF7};
  engine_sysex(proj, sizeof proj);
  // the RAM store: the project loaded once its store is filled (an edit before that is refused: it is still loading)
  for (int i = 0; i < 200000 && core::project_loading(); ++i) core::idle_save();
  CHECK(!core::project_loading());
  remote_reset();
  const std::string pd = rui_png_dir("RUI_PAGES", "build/rui_pages");
  uint8_t fb[kBytes];
  auto png = [&](const char* n) { preview_sent(fb); CHECK(rui_png(pd, n, fb)); };

  // ---- the device's table: POLY CONFIG is the track menu's (FUNC + CHRO), its page "POLY"
  CHECK(!strcmp(sc_combo_keys("poly"), "FUNC+CHRO"));

  // ---- SD (track 2, voice 2) on PLAITS, not a poly track: the page from the track menu's item (30 page 0x21)
  int vi = remote_track_voice(1);
  CHECK(vi == 2);
  report(kPlaits | vi);
  machine(1, kPlaits | vi);
  from_rytm({0x21, 1, 0}, 0x30);
  tick();
  dump("SD, PLAITS, no poly track");
  CHECK(remote_page_title() && !strcmp(remote_page_title(), "POLY") && poly_page_track() == 1);
  CHECK(shown("POLY CONFIG") && shown("SD") && shown("PLAITS: NOT A POLY TRACK") && shown("YES: MAKE IT A POLY TRACK"));
  CHECK(shown("VOIC") && shown("ALLC") && shown("UNIS") && shown("DTUN") && shown("SPRD") && shown("GRP") && shown("AUTO"));
  png("poly_1_make.png");
  turn(0, +1);   // nothing to edit yet: the config stays AUTO, no pattern track made
  tick();
  CHECK(seq::poly_of(1).voices == 0 && !seq::owns_track(1) && !seq::edit_track(1, false));
  key(0x44);     // YES: a poly track now
  tick();
  dump("YES");
  CHECK(seq::owns_track(1) && shown("4 NOTES 4 INST 2.92%") && shown("A..F: SET  NO: BACK"));
  key(0x42);     // fix1049: UP (the Sidecar's modifier) is no page arrow: the page stays
  tick();
  CHECK(remote_page_title() && !strcmp(remote_page_title(), "POLY") && remote_open());
  for (int c : {0x40, 0x41, 0x43, 0x84}) {   // ux1027 (review 8): LEFT RIGHT DOWN PAGE - the page and its state stay
    key(c);                                   // (before: the ring's next page, the page's state dropped)
    tick();
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "POLY") && poly_page_track() == 1 && shown("SD"));
  }
  g_out.clear();
  key(0x45);   // and NO still goes back to the stock screen it came from (the track menu)
  CHECK(sent_3a() == 1);
  from_rytm({0, 0, 3}, 0x30);
  from_rytm({0x21, 1, 0}, 0x30);   // the track menu's POLY CONFIG again (the rest of the test goes on from here)
  tick();
  CHECK(remote_page_title() && !strcmp(remote_page_title(), "POLY"));
  png("poly_2_auto.png");

  // ---- the knobs: the console's seq=poly (the pattern's config and the track's default)
  for (int i = 0; i < 3; ++i) turn(0, +1);   // VOICES AUTO -> 3
  tick();
  CHECK(seq::poly_of(1).voices == 3 && shown("SD VOICES:3") && shown("3 NOTES 3 INST 2.19%"));
  turn(1, +1);                               // ALLOCATION REASSIGN -> ROTATE
  tick();
  CHECK(seq::poly_of(1).alloc == seq::kAllocRotate && shown("SD ALLOCATION:ROTATE") && shown("ROTATE"));
  turn(1, +5);                               // clamped at RESET (no wrap)
  turn(1, +1);
  tick();
  CHECK(seq::poly_of(1).alloc == seq::kAllocReset);
  turn(1, -1);
  turn(2, +1);                               // UNISON OFF -> 2: 3 notes x 2 = 6 instances
  tick();
  CHECK(seq::poly_of(1).unison == 2 && shown("3 NOTES 6 INST 4.38%"));
  for (int i = 0; i < 5; ++i) turn(3, +1);   // DETUNE 5
  turn(4, +1, true);                         // SPREAD by 8 with FUNC
  turn(5, +1);                               // GROUP 1: stored only
  tick();
  dump("set");
  CHECK(seq::poly_of(1).detune == 5 && seq::poly_of(1).spread == 8 && seq::poly_of(1).group == 1);
  CHECK(shown("GROUP 1: STORED ONLY"));
  png("poly_3_set.png");
  std::string r = cmd("seq=poly:2");
  printf("  %s\n", r.c_str());
  CHECK(r.find("VOICES 3 ROTATE UNISON 2 DETUNE 5 SPREAD 8 GROUP 1") != std::string::npos);
  CHECK(seq::edit_track(1, false) && seq::edit_track(1, false)->poly.voices == 3);   // the pattern's own
  for (int i = 0; i < 20; ++i) turn(0, -1);   // back to AUTO (clamped)
  tick();
  CHECK(seq::poly_of(1).voices == 0);

  // ---- fix1049 (hardware 2026-10-23: turning VOICES / GROUP while a note played retriggered it until the knob
  // stopped): a knob step is no card work on the control core (the Pi's core 3 renders between its passes: a card write
  // there is a dropout); tracks.txt is written once the knob settled, by the card core (cardq), and the notes sounding
  // are not touched
  {
    while (core::idle_save()) {}
    cardq::set_worker(true);   // the Pi: core 0's loop runs the card queue
    host_card_model(true);
    host_card_core(3);
    uint64_t c3 = host_card_us(3);
    inst::VoiceStat n0 = inst::stat(vi);
    for (int i = 0; i < 12; ++i) {   // 12 steps, 50 ms apart (a knob turned for 0.6 s): VOICES and GROUP
      turn(i & 1 ? 5 : 0, i < 6 ? +1 : -1);
      host_fake_time(true, g_us += 50000);
      tick();
      for (int k = 0; k < 8 && core::idle_save(); ++k) {}
    }
    uint64_t during = host_card_us(3) - c3;
    printf("  12 knob steps: %llu us of card time on the control core (the FIFO holds 2000 us)\n", (unsigned long long)during);
    CHECK(during == 0);
    CHECK(inst::stat(vi).notes == n0.notes);   // no note started by a config change
    host_fake_time(true, g_us += 1100000);     // settled: tracks.txt and the pattern written, posted
    uint32_t posted0 = cardq::stats().posted;
    for (int k = 0; k < 8; ++k) {   // the control core's passes, the card core's between them
      core::idle_save();
      host_card_core(0);
      for (int i = 0; i < 8 && cardq::service(); ++i) {}
      host_card_core(3);
    }
    uint32_t posted = cardq::stats().posted - posted0;
    printf("  settled: %u card requests posted, control core %llu us in all\n", (unsigned)posted,
           (unsigned long long)(host_card_us(3) - c3));
    CHECK(host_card_us(3) == c3);   // the saves too: the card core's alone
    CHECK(posted >= 2);
    FILE* f = popen(("cat '" + card + "'/.rytm/seq/*/tracks.txt").c_str(), "r");
    char tb[512] = {0};
    size_t got = f ? fread(tb, 1, sizeof tb - 1, f) : 0;
    if (f) pclose(f);
    printf("  tracks.txt: %.*s\n", (int)got, tb);
    CHECK(got > 0 && strstr(tb, "mask=") && strstr(tb, "poly=2:0,1,2,5,8,1"));
    host_card_model(false);
    cardq::set_worker(false);
    CHECK(seq::poly_of(1).voices == 0 && seq::poly_of(1).group == 1);
  }

  // ---- NO: back to the stock screen (the track menu opened it): the page asked closed
  g_out.clear();
  key(0x45);
  CHECK(sent_3a() == 1);
  from_rytm({0, 0, 1}, 0x30);   // the Rytm closed it
  CHECK(!remote_open());

  // ---- an own-pool machine (6-OP FM on BD, voice 0): VOICES only
  report(kFm | 0);
  machine(0, kFm | 0);
  CHECK(cmd("seq=track:1:on").find("sequenced") != std::string::npos);
  from_rytm({0x20, 1, 0}, 0x30);
  tick();
  seq::PolyCfg fm0 = seq::poly_of(0);
  turn(1, +1), turn(2, +1), turn(3, +1), turn(4, +1);
  tick();
  dump("BD, 6-OP FM");
  CHECK(seq::poly_of(0).alloc == fm0.alloc && seq::poly_of(0).unison == fm0.unison &&
        seq::poly_of(0).detune == fm0.detune && seq::poly_of(0).spread == fm0.spread);   // VOICES only: B..E unchanged
  CHECK(shown("FM: VOICES ONLY") && shown_prefix("6 NOTES 6 INST"));
  png("poly_4_fm.png");
  from_rytm({0, 0, 1}, 0x30);

  // ---- an analog track: why not
  machine(2, 0x7F);
  from_rytm({0x22, 1, 0}, 0x30);
  tick();
  dump("RS, analog");
  CHECK(shown("RS PLAYS ANALOG:") && shown("THE RYTM'S VOICE IS MONO") && shown("SIDECAR MACHINE ON SRC"));
  turn(0, +1), key(0x44);
  CHECK(!seq::owns_track(2));
  png("poly_5_analog.png");
  from_rytm({0, 0, 1}, 0x30);

  // ---- the SIDECAR MENU's entry: the selected track's page (SD: 49 2 selects it), NO back to SIDECAR
  from_rytm({2, 1, 44, 'S', 'D', ' ', 'P', 'L', 'A', 'I', 'T', 'S', ' ', ' ', ' '}, 0x49);
  from_rytm({0, 1, 0}, 0x30);
  tick();
  key(0x44);   // YES: MENU
  tick();
  CHECK(remote_page_title() && !strcmp(remote_page_title(), "MENU") && shown("POLY CONFIG") && shown("FUNC+CHRO"));
  for (int i = 0; i < menu_rows() && !(menu_row_page(i) && !strcmp(menu_row_page(i), "POLY")); ++i) key(0x43);   // DOWN to POLY CONFIG (rui1051: by name, not position)
  key(0x44);
  tick();
  dump("from the MENU");
  CHECK(remote_page_title() && !strcmp(remote_page_title(), "POLY") && poly_page_track() == 1 && shown("SD"));
  g_out.clear();
  key(0x45);   // NO: SIDECAR (the page stays open)
  tick();
  CHECK(remote_open() && remote_stats().page == 0 && sent_3a() == 0);
  from_rytm({0, 0, 1}, 0x30);

  // ---- E0's make prompt (the SEQ page of a track that is no poly track: 30 page 0x10 + t) on every Sidecar machine;
  // an analog track the only one explained
  struct M { int m; const char* name; } kinds[] = {{kPlaits, "PLAITS"}, {kJuno, "JUNO"}, {kDaisy, "DAISYSP"},
                                                   {kDspSample, "DSP SAMPLE"}, {kMd, "MD"}, {kFm, "6-OP FM"},
                                                   {kMultiMachine, "MULTISAMPLE"}, {kWaveMachine, "WAVETABLE"},
                                                   {kGranMachine, "GRANULAR"}};
  int t = 4, v4 = remote_track_voice(t);   // BT (voice 1)
  for (const M& k : kinds) {
    report(k.m | v4);
    machine(t, k.m | v4);
    from_rytm({(uint8_t)(0x10 + t), 1, 0}, 0x30);
    tick();
    char want[40];
    snprintf(want, sizeof want, "%s, ", k.name);
    dump(k.name);
    CHECK(shown("MAKE BT A POLY TRACK?") && shown_prefix(want) && shown("FUNC+CHRO: POLY CONFIG") &&
          shown("YES: MAKE IT   NO: BACK"));
    if (k.m == kPlaits) png("make_plaits.png");
    from_rytm({0, 0, 1}, 0x30);
  }
  machine(t, 0x7F);
  from_rytm({(uint8_t)(0x10 + t), 1, 0}, 0x30);
  tick();
  dump("analog");
  CHECK(shown("BT PLAYS ANALOG:") && shown("THE RYTM'S VOICE IS MONO") && shown("A SIDECAR MACHINE ON SRC") &&
        !shown_prefix("MAKE"));
  png("make_analog.png");
  from_rytm({0, 0, 1}, 0x30);
  // a kit that still names DIGIDRUM (0021's machine 42 before its MD page, voice byte 0x40): the built-in MD now - a
  // Sidecar machine, the make prompt as for MD, mono ("1 NOTE")
  report(kMd | v4);
  machine(t, kMd | v4);
  CHECK(remote_track_machine(t) == (kMd | v4) && v[v4].md);
  from_rytm({(uint8_t)(0x10 + t), 1, 0}, 0x30);
  tick();
  dump("digidrum");
  CHECK(shown("MAKE BT A POLY TRACK?") && shown_prefix("MD, 1 NOTE"));
  from_rytm({0, 0, 1}, 0x30);
  // MD's labels through the engine's SysEx path (host_link: a 19 on slot 0x40 answered by core/md, not the user slots)
  g_out.clear();
  from_rytm({0x40, 0, 0, 0}, 0x19);
  {
    int n1a = 0;
    for (auto& m : g_out) n1a += m.size() > 7 && m[4] == 0x1A && m[6] == 0x40;
    CHECK(n1a == 5);
  }
  // a project / kit load's snapshot (33, all 12 tracks) naming DIGIDRUM (now MD) on SD and CH between other machines:
  // every track keeps its machine (no number moved)
  {
    const int want[12] = {kPlaits, kMd, kJuno, 0x7F, kFm, kDaisy, kDspSample, 0x7F, kMd,
                          kMultiMachine, kWaveMachine, 0x50};
    std::vector<uint8_t> b = {0, 12};
    for (int k = 0; k < 12; ++k) b.push_back(44), b.push_back((uint8_t)(want[k] == 0x7F ? 0x7F : want[k] | remote_track_voice(k)));
    from_rytm(b, 0x33);
    for (int k = 0; k < 12; ++k) {
      int got = remote_track_machine(k);
      CHECK(want[k] == 0x7F ? got == 0x7F : (got & 0x78) == want[k]);
    }
    tick();
  }

  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_poly_page: OK\n");
  return 0;
}
