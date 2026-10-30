// core/tests/test_inst_alloc.cpp - E0's pure half (core/inst_alloc.h): every machine's declaration, the voice budget
// check, the allocation modes (REASSIGN / ROTATE / RESET, the Analog Four's), mono retrigger / choke at VOICES 1, the
// steal order (the quietest released tail, then the oldest held note), UNISON's stack.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "check.h"
#include "inst_alloc.h"
#include "machines.h"
#include "rmp_format.h"

using namespace core;
using inst::Alloc;

static seq::PolyCfg cfg(int voices, int alloc, int unison = 0, int detune = 0, int spread = 0) {
  seq::PolyCfg c = {};
  c.voices = (uint8_t)voices, c.alloc = (uint8_t)alloc, c.unison = (uint8_t)unison, c.detune = (uint8_t)detune,
  c.spread = (uint8_t)spread;
  return c;
}
// the lane each note goes to, the notes played in order (note < 0: -note released)
static void play(Alloc& a, const seq::PolyCfg& c, std::initializer_list<int> notes, int* lanes) {
  int i = 0;
  for (int n : notes) {
    int st = 0;
    lanes[i++] = n >= 0 ? inst::note_on(a, c, n, &st) : inst::note_off(a, -n);
  }
}
static void quiet(Alloc& a) {   // the renderer: every released tail gone quiet
  for (int k = 0; k < a.lanes; ++k)
    if (!a.lane[k].held) a.lane[k].sounding = false, a.lane[k].level = 0;
}

static void test_decls() {
  // every Sidecar machine is declared; drums mono by default, tonal machines poly; DSP SAMPLE mono only
  const int all[] = {kPlaits, kJuno, kDaisy, kDspSample, kMd, kFm, kGrainMachine, kMultiMachine, kWaveMachine,
                     kGranMachine, kLiveMachine, kChopMachine};
  for (int m : all) {
    const inst::Decl* d = inst::builtin(m);
    CHECK(d && d->machine == m && d->def_voices >= 1 && d->def_voices <= d->max_voices && d->max_voices <= inst::kMaxInst);
    CHECK(d->cpu_mean > 0 && d->cpu_peak >= d->cpu_mean);
  }
  CHECK(inst::builtin(kDspSample)->max_voices == 1 && inst::builtin(kGrainMachine)->max_voices == 1);
  CHECK(inst::builtin(kMd)->max_voices == 1 && inst::builtin(kMd)->pitch == inst::kPitchNone);   // MD: mono drums
  CHECK(inst::builtin(kPlaits)->def_voices == 4 && inst::builtin(kPlaits)->max_voices == 8 && !inst::builtin(kPlaits)->own_pool);
  CHECK(inst::builtin(kJuno)->def_voices == 6 && inst::builtin(kDaisy)->def_voices == 4);
  // the note machines keep their pools' counts before E0 at AUTO (FM 6, WAVETABLE 8, GRANULAR 6, MULTISAMPLE 8)
  CHECK(inst::builtin(kFm)->own_pool && inst::builtin(kFm)->def_voices == 6);
  CHECK(inst::builtin(kWaveMachine)->def_voices == 8 && inst::builtin(kGranMachine)->def_voices == 6);
  CHECK(inst::builtin(kMultiMachine)->own_pool && inst::builtin(kMultiMachine)->def_voices == 8);
  CHECK(inst::builtin(0x58) == nullptr);   // a user slot: from its package
  CHECK(inst::builtin(0x7F)->machine == kPlaits);   // an unknown value plays PLAITS (voices.cpp's rule; 0x78 is LIVE IN)
  CHECK(inst::builtin(kLiveMachine)->max_voices == 1 && inst::builtin(kLiveMachine)->pitch == inst::kPitchNone);
  // CHOP: its own pool of slice players, VOICES up to 8 (the note is the slice: kPitchNote, the pool's own rule)
  CHECK(inst::builtin(kChopMachine)->own_pool && inst::builtin(kChopMachine)->max_voices == 8 &&
        inst::builtin(kChopMachine)->def_voices == 8);
  // user machines: an old package (no flags) mono with the note's pitch; a package that opts in, its max and pitch
  inst::Decl u0 = inst::user_decl(0x50, 0, 500, 1024);
  CHECK(u0.max_voices == 1 && u0.def_voices == 1 && u0.pitch == inst::kPitchNote && !u0.own_pool);
  uint32_t fl = RMP_HF_INSTANCES | (5u << RMP_HF_VOICES_SHIFT) | (uint32_t)RMP_PITCH_NONE << RMP_HF_PITCH_SHIFT;
  inst::Decl u1 = inst::user_decl(0x58, fl, 500, 1024);
  CHECK(u1.max_voices == 6 && u1.def_voices == 1 && u1.pitch == inst::kPitchNone);   // a drum (pitch none): 1 at AUTO
  inst::Decl u2 = inst::user_decl(0x60, RMP_HF_INSTANCES | (7u << RMP_HF_VOICES_SHIFT), 500, 1024);
  CHECK(u2.max_voices == 8 && u2.def_voices == 4 && u2.pitch == inst::kPitchNote);   // tonal: 4 at AUTO
  CHECK(rmp_hf_max_voices(0) == 1 && rmp_hf_max_voices(7u << RMP_HF_VOICES_SHIFT) == 1);   // no opt-in: mono
}

static void test_resolve() {
  const inst::Decl& pl = *inst::builtin(kPlaits);
  inst::Resolved r = inst::resolve(cfg(0, 0), pl);   // AUTO
  CHECK(r.instances == 4 && r.lanes == 4 && r.unison == 1 && !r.clamped);
  r = inst::resolve(cfg(8, 0, 1), pl);
  CHECK(r.instances == 8 && r.lanes == 8);
  r = inst::resolve(cfg(3, 0, 2), pl);   // 3 notes x a stack of 2
  CHECK(r.instances == 6 && r.lanes == 3 && r.unison == 2);
  r = inst::resolve(cfg(4, 0, 4), pl);   // 4 x 4 = 16: the stack kept, the notes cut to 2
  CHECK(r.instances == 8 && r.lanes == 2 && r.unison == 4 && r.clamped);
  r = inst::resolve(cfg(1, 0, 8), pl);   // one note, a stack of 8
  CHECK(r.instances == 8 && r.lanes == 1 && r.unison == 8);
  r = inst::resolve(cfg(8, 0), *inst::builtin(kDspSample));   // DSP SAMPLE: mono only
  CHECK(r.instances == 1 && r.lanes == 1 && r.clamped);
  CHECK(inst::resolve(cfg(8, 0), pl).cpu100 == 8 * pl.cpu_mean);
  // the budget: a heavy user machine (its declared cost per instance) is held under 20 % of a core with the voice's
  // own machine; a light one gets its 8
  inst::Decl heavy = inst::user_decl(0x50, RMP_HF_INSTANCES | (7u << RMP_HF_VOICES_SHIFT), 1000, 1024);
  r = inst::resolve(cfg(8, 0), heavy);
  CHECK(r.clamped && r.instances >= 1 && r.instances < 8);
  CHECK((r.instances + 1) * heavy.cpu_peak <= inst::kBudgetPct100 && (r.instances + 2) * heavy.cpu_peak > inst::kBudgetPct100);
  printf("  budget: a 1000-permille user machine gets %d instances (%d.%02d %% a core each at its peak)\n", r.instances,
         heavy.cpu_peak / 100, heavy.cpu_peak % 100);
  inst::Decl light = inst::user_decl(0x50, RMP_HF_INSTANCES | (7u << RMP_HF_VOICES_SHIFT), 100, 1024);
  CHECK(inst::resolve(cfg(8, 0), light).instances == 8);
  for (int m : {kPlaits, kJuno, kDaisy, kFm}) {   // every built-in fits 8 in the budget
    const inst::Decl* d = inst::builtin(m);
    CHECK((8 + 1) * d->cpu_peak <= inst::kBudgetPct100);
  }
}

static void test_mono() {
  // VOICES 1: one lane, every note retriggers it (the new note chokes the old), held or not
  Alloc a;
  inst::reset(a, 1);
  int l[6];
  play(a, cfg(1, seq::kAllocRotate), {60, 62, -62, 64, 64, -64}, l);
  CHECK(l[0] == 0 && l[1] == 0 && l[3] == 0 && l[4] == 0 && l[2] == 0 && l[5] == 0);
  CHECK(a.lane[0].note == 64 && !a.lane[0].held);
  int st = 0;
  CHECK(inst::note_on(a, cfg(1, 0), 70, &st) == 0 && st == 2);   // its tail cut
  CHECK(inst::note_on(a, cfg(1, 0), 72, &st) == 0 && st == 1 && a.steals == 2);   // the held note cut
  CHECK(inst::note_off(a, 70) == -1 && inst::note_off(a, 72) == 0);   // 70 no longer holds the lane
}

static void test_modes() {
  int l[16];
  Alloc a;
  // ROTATE: round robin, a tail cut when its turn comes
  inst::reset(a, 4);
  play(a, cfg(4, seq::kAllocRotate), {60, -60, 62, -62, 64, -64, 65, -65, 67, -67, 60}, l);
  CHECK(l[0] == 0 && l[2] == 1 && l[4] == 2 && l[6] == 3 && l[8] == 0 && l[10] == 1);
  // ... skipping held lanes
  inst::reset(a, 4);
  play(a, cfg(4, seq::kAllocRotate), {60, 62, -60, 64, 65, 67}, l);   // 60 held on 0 released; 62 holds 1
  CHECK(l[0] == 0 && l[1] == 1 && l[3] == 2 && l[4] == 3 && l[5] == 0);
  int st = 0;
  CHECK(inst::note_on(a, cfg(4, seq::kAllocRotate), 69, &st) == 1 && st == 1);   // all held: the oldest (62 on 1)
  // RESET: always the lowest lane not held, tails cut
  inst::reset(a, 4);
  play(a, cfg(4, seq::kAllocReset), {60, -60, 62, -62, 64, 65, -64, 67}, l);
  CHECK(l[0] == 0 && l[2] == 0 && l[4] == 0 && l[5] == 1 && l[7] == 0);
  // REASSIGN: a pitch comes back to its lane (its tail kept going until then), other notes take idle lanes first
  inst::reset(a, 4);
  play(a, cfg(4, seq::kAllocReassign), {60, 64, 67, -60, -64, -67}, l);   // C E G on 0 1 2, released, tails ringing
  CHECK(l[0] == 0 && l[1] == 1 && l[2] == 2);
  play(a, cfg(4, seq::kAllocReassign), {65, 64, 69}, l);   // F: the idle lane 3; E: its own lane 1; A: no idle lane
  CHECK(l[0] == 3 && l[1] == 1);
  CHECK(l[2] == 0 || l[2] == 2);   // A: the quietest released tail (0 or 2: equally loud here)
  // the quietest tail steals first
  inst::reset(a, 3);
  play(a, cfg(3, seq::kAllocReassign), {60, 62, 64, -60, -62, -64}, l);
  a.lane[0].level = 0.5f, a.lane[1].level = 0.05f, a.lane[2].level = 0.3f;
  CHECK(inst::note_on(a, cfg(3, seq::kAllocReassign), 70, &st) == 1 && st == 2);
  // quiet tails are idle lanes again: no steal counted
  quiet(a);
  uint32_t s0 = a.steals;
  CHECK(inst::note_on(a, cfg(3, seq::kAllocReassign), 72, &st) >= 0 && st == 0 && a.steals == s0);
  // all held: the oldest held note is cut, in every mode
  for (int m = 0; m < seq::kAllocs; ++m) {
    inst::reset(a, 3);
    play(a, cfg(3, m), {60, 62, 64}, l);
    CHECK(inst::note_on(a, cfg(3, m), 70, &st) == 0 && st == 1);   // 60's lane
    CHECK(inst::note_on(a, cfg(3, m), 71, &st) == 1 && st == 1);   // then 62's
  }
  // the same pitch while held: its own lane again (no second copy), every mode
  for (int m = 0; m < seq::kAllocs; ++m) {
    inst::reset(a, 4);
    play(a, cfg(4, m), {60, 62, 60}, l);
    CHECK(l[2] == l[0]);
  }
}

static void test_unison() {
  float c, p;
  inst::unison_offset(cfg(1, 0, 1, 127, 127), 1, 0, &c, &p);
  CHECK(c == 0 && p == 0);   // one instance: no offset
  inst::unison_offset(cfg(1, 0, 4, 127, 127), 4, 0, &c, &p);
  CHECK(std::fabs(c + 50) < 1e-3f && std::fabs(p + 1) < 1e-6f);
  inst::unison_offset(cfg(1, 0, 4, 127, 127), 4, 3, &c, &p);
  CHECK(std::fabs(c - 50) < 1e-3f && std::fabs(p - 1) < 1e-6f);
  float sum = 0, psum = 0;
  for (int k = 0; k < 5; ++k) inst::unison_offset(cfg(1, 0, 5, 64, 32), 5, k, &c, &p), sum += c, psum += p;
  CHECK(std::fabs(sum) < 1e-3f && std::fabs(psum) < 1e-5f);   // symmetric: the stack's centre is the note
  inst::unison_offset(cfg(1, 0, 3, 0, 0), 3, 2, &c, &p);
  CHECK(c == 0 && p == 0);   // DETUNE 0, SPREAD 0: a plain stack
}

static void test_parse() {
  seq::PolyCfg c = {};
  char err[80];
  CHECK(inst::parse_cfg("voices=4,alloc=rotate,unison=2,detune=30,spread=64,group=1", &c, err, sizeof err));
  CHECK(c.voices == 4 && c.alloc == seq::kAllocRotate && c.unison == 2 && c.detune == 30 && c.spread == 64 && c.group == 1);
  CHECK(inst::parse_cfg("v=8,a=2,u=9,d=300", &c, err, sizeof err) && c.voices == 8 && c.alloc == seq::kAllocReset &&
        c.unison == 8 && c.detune == 127 && c.spread == 64);   // short keys, clamped, the rest kept
  CHECK(inst::parse_cfg("auto", &c, err, sizeof err) && seq::poly_default(c));
  CHECK(!inst::parse_cfg("alloc=sideways", &c, err, sizeof err) && strstr(err, "alloc="));
  CHECK(!inst::parse_cfg("frob=1", &c, err, sizeof err) && strstr(err, "frob"));
  CHECK(!inst::parse_cfg("voices", &c, err, sizeof err));
  CHECK(!inst::parse_cfg("voices=x", &c, err, sizeof err) && strstr(err, "number"));
  char b[200];
  c = {};
  c.voices = 3, c.alloc = seq::kAllocRotate;
  inst::Resolved r = inst::resolve(c, *inst::builtin(kPlaits));
  inst::describe(c, &r, kPlaits, b, sizeof b);
  CHECK(!strcmp(b, "VOICES 3 ROTATE UNISON 1 DETUNE 0 SPREAD 0 GROUP 0 -> PLAITS: 3 instances, 3 notes at once (2.19 % a core)"));
  inst::describe_short(r, kPlaits, b, sizeof b);
  CHECK(!strcmp(b, "PLAITS 3 INST 3 NOTES 2.19%"));
  // UNISON on a machine with its own pool: not stacked (the pool's notes only)
  c.unison = 4;
  r = inst::resolve(c, *inst::builtin(kFm));
  CHECK(r.unison == 1 && r.lanes == 3 && r.instances == 3);
}

int main() {
  test_parse();
  test_decls();
  test_resolve();
  test_mono();
  test_modes();
  test_unison();
  printf("test_inst_alloc: OK\n");
  return 0;
}
