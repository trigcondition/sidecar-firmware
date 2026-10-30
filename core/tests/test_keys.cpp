// core/tests/test_keys.cpp - the Pi's own keyboards (core/keys.h): routing by the selected track (the SRC page's 29,
// learned from the Rytm's trig for a forwarded note) and by channel; a 6-note chord sounding polyphonically on
// MULTISAMPLE (and on 6-OP FM's pool); the sustain pedal; an analog track's notes forwarded to the Rytm (never played
// here as well), a mono retrigger of a forwarded key swallowed; the computer's notes by channel except on the auto
// channel; CCs / pitch bend forwarded; while the Rytm records live, the gesture's first note to the Rytm (its 0x04
// swallowed) and the chord written back (28 01); the remote SIDECAR page's line; the settings' file.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "pool_util.h"
#include "engine.h"
#include "keys.h"
#include "msamp/msamp.h"
#include "nmach.h"
#include "poly.h"
#include "seq/seq.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
struct Fwd {
  uint8_t s, d1, d2;
};
static std::vector<Fwd> g_fwd;
static void fwd(uint8_t s, uint8_t d1, uint8_t d2) { g_fwd.push_back({s, d1, d2}); }
static uint64_t g_us = 1000000;
static void advance_ms(int ms) {
  g_us += (uint64_t)ms * 1000;
  host_fake_time(true, g_us);
  poly::tick(now_ms());
}
static int16_t ring[kBlock * 400][kVoices];
static void render(int blocks) {
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < blocks; ++b) render_voices(ring, kBlock * 400, (b % 400) * kBlock, 0, kVoices, mach);
}
static double rms(int voice, int frames) {
  double s = 0;
  for (int i = 0; i < frames; ++i) s += (double)ring[i][voice] * ring[i][voice];
  return std::sqrt(s / frames);
}
static void report(int cmd, int vb, int note, int vel, const std::vector<int>& p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) m.push_back((uint8_t)note), m.push_back((uint8_t)vel);
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(k < (int)p.size() ? p[k] : 0)), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void src_view(int tr, int on) {
  const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)tr, (uint8_t)on, 0xF7};
  engine_sysex(m, 9);
}
static void seq_state(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void key(int src, int st, int d1, int d2) { keys::midi(src, (uint8_t)st, (uint8_t)d1, (uint8_t)d2); }
static int fwds(uint8_t s) {
  int n = 0;
  for (auto& f : g_fwd) n += f.s == s;
  return n;
}
static std::string cmd(const char* c) {
  char b[400];
  CHECK(keys::command(c, b, sizeof b));
  return b;
}

int main() {
  char tmpl[] = "/tmp/core_keys_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples' '" + card + "/MULTISAMPLE' '" + card + "/.rytm' && cp -R ../content/MULTISAMPLE/SidecarTest '" +
                card + "/MULTISAMPLE/'").c_str()) == 0);
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  for (int i = 0; i < 4000 && msamp::step(); ++i) {}
  CHECK(pu::put(pools::kInstr, "A001", "/MULTISAMPLE/SidecarTest/SidecarTest.sfz") == pools::kOk);   // INST A001
  keys::reset();
  keys::set_forward(fwd);
  keys::source(0, true, "MPK mini");

  // the Rytm's 33: BD 6-OP FM (voice 0), SD MULTISAMPLE (voice 2), LT analog (voice 3)
  const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 6, 49, (uint8_t)(kFm | 0), 50, (uint8_t)(kMultiMachine | 2), 0, 0x7F,
                         0, 0x7F, 0, 0x7F, 0, 0x7F, 0xF7};
  engine_sysex(m33, sizeof m33);
  CHECK(remote_track_machine(0) == (kFm | 0) && remote_track_machine(1) == (kMultiMachine | 2) && remote_track_machine(5) == 0x7F);
  std::vector<int> fm = {0, 64, 64, 127, 64, 64, 100, 127, 0, 0, 0, 0, 0, 0, 0, 0};   // CHD OFF
  report(0x03, kFm | 0, -1, 0, fm);
  std::vector<int> ms = {1, 127, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0};   // SidecarTest, CHD OFF
  report(0x03, kMultiMachine | 2, -1, 0, ms);
  for (int i = 0; i < 4000 && msamp::step(); ++i) {}
  CHECK(msamp::voice_slot(2) >= 0 && v[2].ms && v[0].fm);
  char lb[40];
  CHECK(keys::line(lb, sizeof lb) && !strcmp(lb, "KEYS > SELECTED"));

  // ---- selected = SD (its SRC page: 29 1 1): a 6-note chord plays 6 MULTISAMPLE notes, nothing to the Rytm
  src_view(1, 1);
  CHECK(keys::selected() == 1);
  const int chord[6] = {60, 64, 67, 71, 74, 77};
  for (int n : chord) key(0, 0x90, n, 100);
  CHECK(msamp::voice_notes(2) == 6 && g_fwd.empty() && keys::stats().poly == 6);
  render(200);
  double r6 = rms(2, kBlock * 200);
  printf("test_keys: 6-note MULTISAMPLE chord, rms %.0f\n", r6);
  CHECK(r6 > 100);
  keys::View kv = keys::view();
  CHECK(kv.route == keys::kPoly && kv.track == 1 && kv.voice == 2 && kv.held == 6);
  CHECK(keys::line(lb, sizeof lb) && !strcmp(lb, "KEYS MPKMINI > SD POLY"));
  advance_ms(poly::kGestureMs + 5);
  printf("test_keys: chord view live %d root %d chd %d\n", poly::view(2).live, poly::view(2).root, poly::view(2).c.chd);
  CHECK(poly::view(2).live);   // the chord view shows the keys' chord
  // the Rytm is not recording: no 28 write-back
  for (auto& w : g_out) CHECK(!(w.size() == 13 && w[4] == 0x28));
  for (int n : chord) key(0, 0x80, n, 0);
  render(4000);
  CHECK(msamp::voice_notes(2) == 0);

  // ---- the sustain pedal: a note-off held while down, released with the pedal
  key(0, 0xB0, 64, 127);
  key(0, 0x90, 62, 100);
  key(0, 0x80, 62, 0);
  render(400);
  CHECK(msamp::voice_notes(2) == 1 && keys::stats().sustained == 1);
  key(0, 0xB0, 64, 0);
  render(4000);
  CHECK(msamp::voice_notes(2) == 0);

  // ---- selected = BD (6-OP FM): three keys, three pool notes
  src_view(1, 0);
  src_view(0, 1);
  for (int n : {48, 52, 55}) key(0, 0x90, n, 100);
  CHECK(poly::voice_notes(0) == 3 && poly::held_notes(0) == 3 && g_fwd.empty());
  for (int n : {48, 52, 55}) key(0, 0x80, n, 0);
  CHECK(poly::held_notes(0) == 0);
  render(4000);
  src_view(0, 0);

  // ---- nothing says which track is selected: the notes go to the Rytm's auto channel (14). An analog track: no 0x04,
  // the Rytm plays them; the note-off follows the note there
  CHECK(keys::selected() == -1);
  key(0, 0x90, 60, 90);
  CHECK(g_fwd.size() == 1 && g_fwd[0].s == 0x9D && g_fwd[0].d1 == 60 && g_fwd[0].d2 == 90);
  CHECK(keys::view().route == keys::kProbe && msamp::voice_notes(2) == 0 && poly::voice_notes(0) == 0);
  advance_ms(50);
  key(0, 0x80, 60, 0);
  CHECK(fwds(0x8D) == 1 && keys::selected() == -1);
  // the pitch bend and a knob go to the Rytm on the same channel
  key(0, 0xE0, 0, 80);
  key(0, 0xB0, 74, 33);
  CHECK(fwds(0xED) == 1 && fwds(0xBD) == 1);

  // ---- learned: SD selected on the Rytm (its TRIG page): the forwarded note's 0x04 on voice 2 within kEchoMs names it;
  // the 0x04 swallowed, the key plays MULTISAMPLE here; the next keys go straight to the voice
  g_fwd.clear();
  advance_ms(10);
  key(0, 0x90, 62, 100);
  CHECK(g_fwd.size() == 1 && g_fwd[0].s == 0x9D);
  advance_ms(4);
  report(0x04, kMultiMachine | 2, 62, 100, ms);   // the Rytm's trig for it: swallowed
  CHECK(keys::selected() == 1 && msamp::voice_notes(2) == 1 && keys::stats().swallowed >= 1);
  key(0, 0x90, 65, 100);
  key(0, 0x90, 69, 100);
  CHECK(g_fwd.size() == 1 && msamp::voice_notes(2) == 3 && keys::view().route == keys::kPoly);
  // the Rytm's mono retrigger while the forwarded key is held: swallowed (no 4th note)
  advance_ms(20);
  report(0x04, kMultiMachine | 2, 65, 100, ms);
  CHECK(msamp::voice_notes(2) == 3);
  key(0, 0x80, 62, 0);   // the forwarded key's note-off goes to the Rytm as well
  key(0, 0x80, 65, 0), key(0, 0x80, 69, 0);
  CHECK(fwds(0x8D) == 1);
  render(4000);
  CHECK(msamp::voice_notes(2) == 0);
  // forgotten kLearnMs after the last key: learned again from the next one
  advance_ms((int)keys::kLearnMs + 100);
  CHECK(keys::selected() == -1);

  // ---- by channel: channel 2 plays SD, channel 6 LT (analog: forwarded as it is), 16 forwarded
  CHECK(cmd("keys=channel").find("by channel") != std::string::npos);
  g_fwd.clear();
  key(0, 0x91, 60, 100);
  key(0, 0x95, 45, 100);
  key(0, 0x9F, 40, 100);
  CHECK(msamp::voice_notes(2) == 1 && g_fwd.size() == 2 && g_fwd[0].s == 0x95 && g_fwd[1].s == 0x9F);
  key(0, 0x81, 60, 0), key(0, 0x85, 45, 0), key(0, 0x8F, 40, 0);
  CHECK(fwds(0x85) == 1 && fwds(0x8F) == 1);
  render(4000);
  CHECK(cmd("keys=selected").find("the selected track") != std::string::npos);

  // ---- the computer: its track channels by channel (channel 1 = BD's pool), its auto channel the selected track
  src_view(1, 1);
  g_fwd.clear();
  key(keys::kComputer, 0x90, 60, 100);
  CHECK(poly::held_notes(0) == 1 && g_fwd.empty());
  key(keys::kComputer, 0x9D, 72, 100);
  CHECK(msamp::voice_notes(2) == 1 && g_fwd.empty());
  key(keys::kComputer, 0x80, 60, 0), key(keys::kComputer, 0x8D, 72, 0);
  CHECK(poly::held_notes(0) == 0);
  // a DAW's all notes off: the channel's keys released at once, the CC still goes to the Rytm
  key(keys::kComputer, 0x9D, 74, 100);
  key(keys::kComputer, 0xBD, 123, 0);
  render(4000);
  CHECK(msamp::voice_notes(2) == 0 && fwds(0xBD) == 1);
  cmd("keys=mac:off");
  key(keys::kComputer, 0x9D, 74, 100);
  CHECK(msamp::voice_notes(2) == 0 && !keys::computer_on());
  cmd("keys=mac:on");
  CHECK(keys::computer_on());

  // ---- the Rytm records live (2D 06 REC, 2D 01 PLAY) on SD: the gesture's first note goes to the Rytm (a trig recorded),
  // its 0x04 swallowed; the chord of 3 written back to SD (28 01 track 1, root C, MAJ, 3 notes)
  seq_state({6, 1, 0});
  seq_state({1, 1, 0, 0, 0, 0});
  CHECK(seq::rytm_live_rec());
  advance_ms(200);
  g_fwd.clear(), g_out.clear();
  key(0, 0x90, 60, 100);
  advance_ms(2);
  key(0, 0x90, 64, 100);
  key(0, 0x90, 67, 100);
  CHECK(g_fwd.size() == 1 && g_fwd[0].s == 0x9D && g_fwd[0].d1 == 60 && msamp::voice_notes(2) == 3);
  advance_ms(3);
  report(0x04, kMultiMachine | 2, 60, 100, ms);   // the recorded trig as the Rytm plays it: swallowed
  CHECK(msamp::voice_notes(2) == 3);
  advance_ms(poly::kGestureMs + 2);
  int writes = 0;
  for (auto& w : g_out)
    if (w.size() == 13 && w[4] == 0x28 && w[5] == 0x01) {
      ++writes;
      CHECK(w[6] == 1 && w[7] == 60 && w[8] == 1 && w[11] == 3);
    }
  CHECK(writes == 1 && keys::stats().writes == 1);
  key(0, 0x80, 60, 0), key(0, 0x80, 64, 0), key(0, 0x80, 67, 0);
  CHECK(fwds(0x8D) == 1);
  seq_state({6, 0, 0});
  render(4000);

  // ---- unplugged: its held keys released
  key(0, 0x90, 60, 100);
  CHECK(msamp::voice_notes(2) == 1);
  keys::source(0, false, nullptr);
  render(4000);
  CHECK(msamp::voice_notes(2) == 0);

  // ---- integ-0.3: WAVETABLE and GRANULAR tracks play poly from the keys too (BD GRANULAR on voice 0, SD WAVETABLE on
  // voice 2: the tracks' voices), the Rytm gets nothing
  {
    const uint8_t w33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 6, 49, (uint8_t)(kGranMachine | 0), 50, (uint8_t)(kWaveMachine | 2), 0,
                           0x7F, 0, 0x7F, 0, 0x7F, 0, 0x7F, 0xF7};
    engine_sysex(w33, sizeof w33);
    report(0x03, kGranMachine | 0, -1, 0, {11, 40, 20, 64, 64, 0, 0, 0, 10, 40, 0, 0, 0, 0, 0, 0});   // BELLS
    report(0x03, kWaveMachine | 2, -1, 0, {0, 30, 0, 0, 0, 0, 64, 0, 10, 60, 100, 64, 0, 0, 0, 0});   // BASIC
    for (int i = 0; i < 4000 && nm::step(); ++i) {}
    CHECK(v[0].gr && v[2].wt);
    for (int t = 0; t < 2; ++t) {
      g_fwd.clear();
      src_view(t, 1);
      CHECK(keys::selected() == t);
      for (int n : {60, 64, 67}) key(0, 0x90, n, 100);
      int vi = t ? 2 : 0, held = t ? wt::voice_notes(2) : gr::voice_notes(0);
      printf("test_keys: %s chord: %d notes on voice %d, %d to the Rytm\n", t ? "WAVETABLE" : "GRANULAR", held, vi, (int)g_fwd.size());
      CHECK(held == 3 && g_fwd.empty() && keys::view().route == keys::kPoly);
      for (int n : {60, 64, 67}) key(0, 0x80, n, 0);
      render(8000);
      CHECK((t ? wt::voice_notes(2) : gr::voice_notes(0)) == 0);
      src_view(t, 0);
    }
  }

  // ---- the settings: kept on the card
  cmd("keys=channel"), cmd("keys=auto:10");
  CHECK(keys::save() && !keys::save());
  keys::reset();
  keys::load();
  CHECK(keys::view().mode == keys::kChannel && cmd("keys").find("auto channel 10, computer on") != std::string::npos);
  printf("test_keys: %s\n", cmd("keys").c_str());

  system(("rm -rf '" + card + "'").c_str());
  printf("test_keys: PASS\n");
  return 0;
}
