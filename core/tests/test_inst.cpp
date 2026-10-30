// core/tests/test_inst.cpp - E0's engines (core/inst.h): the sequencer's notes on every Sidecar machine with a track's
// poly config - the instance pool for PLAITS, JUNO, DAISYSP (allocation, steals, UNISON's stack and pan, the
// tails freed once quiet), the mono path for VOICES 1 and DSP SAMPLE (retrigger / choke), the note machines' own pools
// capped by VOICES (6-OP FM), a machine change and CC 120 silencing the instances, the cost per instance on STATUS
// (declared and measured, RAM), and no card time on the control core for any of it.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "inst.h"
#include "machines.h"
#include "poly.h"
#include "voices.h"

using namespace core;

static void report(int vb, std::vector<int> p) {   // a 0x03 PARAMS report: the machine and P1..P16
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)vb};
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(k < (int)p.size() ? p[k] : 0)), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static seq::PolyCfg cfg(int voices, int alloc = 0, int unison = 0, int detune = 0, int spread = 0) {
  seq::PolyCfg c = {};
  c.voices = (uint8_t)voices, c.alloc = (uint8_t)alloc, c.unison = (uint8_t)unison, c.detune = (uint8_t)detune,
  c.spread = (uint8_t)spread;
  return c;
}
static int16_t ring[kBlock][kVoices];
static uint64_t mach[kMachines];
// n blocks of every voice; voice vi's peak (int16)
static int render(int n, int vi, std::vector<int16_t>* out = nullptr) {
  int pk = 0;
  for (int b = 0; b < n; ++b) {
    render_voices(ring, kBlock, 0, 0, kVoices, mach);
    for (int s = 0; s < kBlock; ++s) {
      int a = std::abs((int)ring[s][vi]);
      pk = a > pk ? a : pk;
      if (out) out->push_back(ring[s][vi]);
    }
  }
  return pk;
}
static const std::vector<int> kPlaitsVa = {0, 64, 64, 64, 90, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100};   // VA, DEC 90

static void test_plaits() {
  report(kPlaits | 0, kPlaitsVa);
  inst::Resolved r;
  int m = -1;
  CHECK(inst::resolve_for(0, cfg(0), &r, &m) && m == kPlaits && r.instances == 4);   // AUTO: 4
  for (int n : {60, 64, 67, 71}) CHECK(inst::seq_on(0, n, 100, cfg(0)));
  inst::VoiceStat s = inst::stat(0);
  CHECK(s.path == 3 && s.instances == 4 && s.lanes == 4 && inst::voice_notes(0) == 4 && s.ram == 4 * inst::inst_bytes());
  CHECK(render(40, 0) > 1000);
  CHECK(inst::seq_on(0, 74, 100, cfg(0)) && inst::voice_notes(0) == 4 && inst::stat(0).steals == 1);   // all held: one cut
  s = inst::stat(0);
  CHECK(s.meas100 > 0 && s.cpu100 == 4 * inst::builtin(kPlaits)->cpu_mean);
  printf("  PLAITS x4: measured %d.%02d %% of a block (Mac), declared %d.%02d %% a core (Pi 4), %u KB\n", s.meas100 / 100,
         s.meas100 % 100, s.cpu100 / 100, s.cpu100 % 100, (unsigned)(s.ram >> 10));
  // released: the tails ring out, then the instances are free again
  inst::seq_off(0, -1);
  int blocks = 0;
  while (inst::voice_notes(0) && blocks < 48000) render(10, 0), blocks += 10;
  CHECK(!inst::voice_notes(0) && !inst::active(0));
  printf("  PLAITS tails free after %.2f s\n", blocks * kBlock / 48000.0);
  // VOICES 8: the 8 notes of a chord at once, and the headroom keeps the sum in range
  for (int n = 0; n < 8; ++n) CHECK(inst::seq_on(0, 48 + 3 * n, 127, cfg(8)));
  CHECK(inst::voice_notes(0) == 8 && inst::stat(0).instances == 8);
  int pk = render(200, 0);
  CHECK(pk > 1000 && pk < 32767);
  inst::seq_off(0, -1);
  inst::silence(0);
  CHECK(!inst::active(0));
}

static void test_mono() {
  // VOICES 1 on PLAITS: the voice's own machine (as a Rytm trig), every note retriggers it
  report(kPlaits | 0, kPlaitsVa);
  CHECK(inst::seq_on(0, 60, 100, cfg(1)));
  CHECK(inst::stat(0).path == 2 && inst::stat(0).instances == 1 && !inst::active(0));
  CHECK(v[0].trig && std::fabs(v[0].target - 60) < 0.01f);
  CHECK(render(20, 0) > 1000);
  uint32_t st0 = inst::stat(0).steals;
  CHECK(inst::seq_on(0, 67, 100, cfg(1)) && std::fabs(v[0].target - 67) < 0.01f && inst::stat(0).steals == st0 + 1);
  inst::seq_off(0, 67);
  // DSP SAMPLE: mono whatever VOICES says (one card stream a voice); the config says it was cut
  report(kDspSample | 4, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100});
  inst::Resolved r;
  CHECK(inst::resolve_for(4, cfg(8), &r, nullptr) && r.instances == 1 && r.clamped);
  CHECK(inst::seq_on(4, 60, 100, cfg(8)) && inst::stat(4).path == 2 && inst::stat(4).clamped);
  inst::seq_off(4, -1);
  // MD (0x40, core/md/md.h) on a seq track, AUTO and VOICES 4: mono (its max is 1), each note a hit
  report(kMd | 2, {0, 40, 64, 64, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 100});
  CHECK(inst::seq_on(2, 48, 100, cfg(0)) && inst::stat(2).path == 2 && !inst::active(2) && render(200, 2) > 500);
  CHECK(inst::seq_on(2, 60, 100, cfg(4)) && inst::stat(2).path == 2 && inst::stat(2).clamped);
  inst::seq_off(2, -1);
}

static void test_unison() {
  report(kPlaits | 0, kPlaitsVa);
  // one note, a stack of 4 (DETUNE / SPREAD full): 4 instances, a lane of 4
  CHECK(inst::seq_on(0, 60, 100, cfg(1, 0, 4, 127, 127)));
  inst::VoiceStat s = inst::stat(0);
  CHECK(s.path == 3 && s.instances == 4 && s.lanes == 1 && s.unison == 4 && inst::voice_notes(0) == 4);
  // its stereo pair: L and R differ (the outer instances panned apart); mono: the plain sum
  float l[kBlock], r[kBlock];
  double dl = 0, dr = 0, d = 0;
  for (int b = 0; b < 100; ++b) {
    for (int k = 0; k < kBlock; ++k) l[k] = r[k] = 0;
    inst::render_add(0, l, r, true);
    for (int k = 0; k < kBlock; ++k) dl += std::fabs(l[k]), dr += std::fabs(r[k]), d += std::fabs(l[k] - r[k]);
  }
  CHECK(dl > 0 && dr > 0 && d > 0.01 * dl);
  // a second note on a one-lane stack: the whole stack retriggers (mono unison)
  CHECK(inst::seq_on(0, 62, 100, cfg(1, 0, 4, 127, 127)) && inst::voice_notes(0) == 4);
  // the config changed (2 notes x 2): a fresh set
  CHECK(inst::seq_on(0, 64, 100, cfg(2, 0, 2, 20, 0)));
  s = inst::stat(0);
  CHECK(s.instances == 4 && s.lanes == 2 && s.unison == 2 && inst::voice_notes(0) == 2);
  CHECK(inst::seq_on(0, 67, 100, cfg(2, 0, 2, 20, 0)) && inst::voice_notes(0) == 4);
  inst::silence(0);
}

static void test_modes_engines() {
  // ROTATE on JUNO: notes cycle the lanes, a held note is never cut while a lane is free; note-offs release (JUNO's
  // envelope), the instances free once quiet
  report(kJuno | 1, {0, 64, 64, 64, 64, 64, 64, 64});
  CHECK(inst::seq_on(1, 60, 100, cfg(3, seq::kAllocRotate)) && inst::stat(1).path == 3);
  CHECK(inst::seq_on(1, 63, 100, cfg(3, seq::kAllocRotate)) && inst::seq_on(1, 67, 100, cfg(3, seq::kAllocRotate)));
  int jpk = render(400, 1);   // (100 ms: its attack)
  printf("  JUNO x3: %d notes, peak %d\n", inst::voice_notes(1), jpk);
  CHECK(inst::voice_notes(1) == 3 && jpk > 500);
  inst::seq_off(1, 63);
  CHECK(inst::seq_on(1, 70, 100, cfg(3, seq::kAllocRotate)) && inst::stat(1).steals == 0);   // 63's lane, no steal
  inst::seq_off(1, -1);
  int blocks = 0;
  while (inst::voice_notes(1) && blocks < 96000) render(10, 1), blocks += 10;
  CHECK(!inst::voice_notes(1));
  // DAISYSP: instances too
  report(kDaisy | 3, {0, 64, 64, 64, 64, 64, 64, 100});
  CHECK(inst::seq_on(3, 60, 100, cfg(0)) && inst::seq_on(3, 72, 100, cfg(0)) && inst::stat(3).path == 3);
  CHECK(inst::voice_notes(3) == 2 && render(40, 3) > 200);
  // PLAITS VOICES 4: rolls overlap (instances), RESET always from the first lane
  report(kPlaits | 2, kPlaitsVa);
  for (int k = 0; k < 4; ++k) {
    CHECK(inst::seq_on(2, 60, 100, cfg(4, seq::kAllocReset)));
    inst::seq_off(2, 60);
  }
  CHECK(inst::stat(2).path == 3 && inst::voice_notes(2) >= 1);
  inst::silence(1), inst::silence(2), inst::silence(3);
}

static void test_chord_steal() {
  // a 3-note chord on VOICES 2 (every mode): the third note cuts the oldest held one - one steal, two sounding
  for (int m = 0; m < seq::kAllocs; ++m) {
    report(kPlaits | 6, kPlaitsVa);
    inst::silence(6);
    uint32_t s0 = inst::stat(6).steals;
    for (int n : {48, 55, 60}) CHECK(inst::seq_on(6, n, 120, cfg(2, m)));
    CHECK(inst::voice_notes(6) == 2 && inst::stat(6).steals == s0 + 1);
    inst::seq_off(6, -1);
    inst::silence(6);
  }
}

static void test_own_pool() {
  // 6-OP FM: its own pool; VOICES caps the notes held (the oldest released for a new one); AUTO: as before E0
  report(kFm | 5, {0, 64, 64, 127, 64, 64, 100, 127});
  CHECK(v[5].fm);
  for (int n : {60, 64, 67}) CHECK(inst::seq_on(5, n, 100, cfg(2)));
  CHECK(poly::held_notes(5) == 2 && inst::stat(5).path == 1);
  inst::seq_off(5, -1);
  CHECK(poly::held_notes(5) == 0);
  for (int n : {60, 64, 67}) CHECK(inst::seq_on(5, n, 100, cfg(0)));
  CHECK(poly::held_notes(5) == 3);
  inst::seq_off(5, -1);
  poly::silence(5);
}

// The user, 2026-10-26: a PAT locked on a poly step (hold the step, turn PAT) left 6-OP FM on its first preset. The
// sequencer's lock sets the PAT (seq.cpp apply_locks: v.p, voice_params_changed); its note goes inst::seq_on ->
// poly::seq_on, which played the voice last selected - only a Rytm trig selected one. Now the note selects its PAT.
static void test_fm_locked_pat() {
  report(kFm | 5, {0, 64, 64, 127, 64, 64, 100, 127});
  CHECK(inst::seq_on(5, 60, 100, cfg(0)));
  CHECK((fm_key[5] & 0xff) == 0);
  inst::seq_off(5, -1);
  v[5].p[0] = 9;   // what a lock does (apply_locks), no report
  voice_params_changed(5);
  CHECK(inst::seq_on(5, 62, 100, cfg(0)));
  printf("FM: a note after a PAT lock 9 plays entry %d\n", fm_key[5] & 0xff);
  CHECK((fm_key[5] & 0xff) == 9 && fm_pat[5] == 9);
  inst::seq_off(5, -1);
  v[5].p[0] = 0, voice_params_changed(5);   // the lock gone: the base PAT at the next note
  CHECK(inst::seq_on(5, 64, 100, cfg(0)) && (fm_key[5] & 0xff) == 0);
  inst::seq_off(5, -1);
  poly::silence(5);
}

static void test_silence() {
  report(kPlaits | 0, kPlaitsVa);
  CHECK(inst::seq_on(0, 60, 100, cfg(4)) && inst::active(0));
  report(kJuno | 0, {0, 64, 64, 64, 64, 64, 64, 64});   // the voice's machine changed: PLAITS' instances gone
  CHECK(!inst::active(0));
  CHECK(inst::seq_on(0, 60, 100, cfg(4)) && inst::active(0) && inst::stat(0).machine == kJuno);
  voice_silence(0);   // CC 120
  CHECK(!inst::active(0));
  // PLAITS -> a note machine (MULTISAMPLE: the same engine signature before the review's fix): instances gone
  report(kPlaits | 0, kPlaitsVa);
  CHECK(inst::seq_on(0, 60, 100, cfg(4)) && inst::active(0));
  report(kMultiMachine | 0, {0, 64, 64, 64, 64, 64, 64, 100});
  CHECK(!inst::active(0));
  // a JUNO PAT change reaches an instance that is reused (not only a fresh one)
  report(kJuno | 0, {0, 64, 64, 64, 64, 64, 64, 64});
  CHECK(inst::seq_on(0, 60, 100, cfg(2)));
  inst::seq_off(0, 60);
  report(kJuno | 0, {30, 64, 64, 64, 64, 64, 64, 64});   // another PAT
  CHECK(inst::seq_on(0, 62, 100, cfg(2)));
  CHECK(inst::juno_patch_is(0, junos[0].name()));
  inst::silence(0);
}

static void test_pool_shared() {
  // 32 instances shared: 5 voices x 8 notes want 40 - the last ones take the quietest released (none here: refused)
  for (int vi = 0; vi < 5; ++vi) report(kPlaits | vi, kPlaitsVa);
  int on = 0;
  for (int vi = 0; vi < 5; ++vi)
    for (int n = 0; n < 8; ++n) on += inst::seq_on(vi, 50 + n, 100, cfg(8)) && inst::voice_notes(vi) > n;
  int total = 0;
  for (int vi = 0; vi < 5; ++vi) total += inst::voice_notes(vi);
  CHECK(total == inst::kTotal && on == inst::kTotal);   // (the notes that found an instance: every one until the pool was full)
  char b[1200];
  CHECK(inst::command("inst", b, sizeof b) && strstr(b, "no room"));
  printf("  %s\n", b);
  CHECK(inst::json(b, sizeof b) > 0 && strstr(b, "\"inst\":{") && strstr(b, "\"path\":\"inst\""));
  for (int vi = 0; vi < 5; ++vi) inst::silence(vi);
}

int main() {
  char tmpl[] = "/tmp/core_inst_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, 1000000);
  CHECK(engine_begin() && card_ok);
  host_card_model(true);
  host_card_core(3);
  uint64_t c3 = host_card_us(3), ch = host_changes();
  test_plaits();
  test_mono();
  test_unison();
  test_modes_engines();
  test_chord_steal();
  test_own_pool();
  test_fm_locked_pat();
  test_silence();
  test_pool_shared();
  CHECK(host_card_us(3) == c3 && host_changes() == ch);   // no card work on the control core (none at all)
  host_card_model(false);
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_inst: OK\n");
  return 0;
}
