// core/tests/test_poly.cpp - poly FM (core/poly.h): chord expansion (inversion, spread, voices), chord detection (every
// chord the table plays comes back as the same notes; one note OFF; unknown sets the nearest chord or OFF), the
// value texts and the 28 write-back's bytes; then the pool through the engine's own entry (host_sysex): a CHD trig
// sounds its notes on its voice, 0x11 releases them, CHD OFF stays mono, the caps (6 a voice, 24 in all) steal; live
// chords: pad MIDI bound to the FM voice by its 0x04s, notes played at once, the Rytm's own trigs swallowed or held,
// the window's chord written back (28 01), the Rytm's 28 02 taken (not the remote UI's).
#include <cmath>
#include <cstring>
#include <vector>

#include "check.h"
#include "host_link.h"
#include "poly.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back(note); m.push_back(vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}
static void send(const std::vector<uint8_t>& m) { host_sysex(m.data(), (int)m.size()); }
// An FM sound: PAT 0 (ROM voice 1), TIM MOR 64, DEC 127 (no LPG), LPG 64, TUN 64, LEV 100, LEN 127; P13..P16 the chord
static std::vector<float> fm(int chd, int inv = 0, int spr = 0, int voi = 0) {
  return {0, 64, 64, 127, 64, 64, 100, 127, 0, 0, 0, 0, (float)chd, (float)inv, (float)spr, (float)voi};
}

static int16_t ring[core::kBlock * 400][core::kVoices];
static double rms(int voice, int frames) {
  double s = 0;
  for (int i = 0; i < frames; ++i) s += (double)ring[i][voice] * ring[i][voice];
  return std::sqrt(s / frames);
}
static void render(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) core::render_voices(ring, core::kBlock * 400, b * core::kBlock, 0, core::kVoices, mach);
}
static uint64_t g_us = 1000000;
static void advance_ms(int ms) {
  g_us += (uint64_t)ms * 1000;
  host_fake_time(true, g_us);
  poly::tick(now_ms());
}

static std::vector<int> ex(int root, int chd, int inv = 0, int spr = 0, int voi = 0) {
  uint8_t o[poly::kMaxNotes];
  int n = poly::expand(root, {chd, inv, spr, voi}, o);
  return std::vector<int>(o, o + n);
}

static void test_expand() {
  CHECK((ex(60, 0) == std::vector<int>{60}));
  CHECK((ex(60, 1) == std::vector<int>{60, 64, 67}));            // MAJ
  CHECK((ex(60, 1, 1) == std::vector<int>{64, 67, 72}));         // first inversion
  CHECK((ex(60, 1, 2) == std::vector<int>{67, 72, 76}));         // second
  CHECK((ex(60, 1, 3) == std::vector<int>{60, 64, 67}));         // INV 3 of a triad: round again (3 % 3)
  CHECK((ex(60, 1, 0, 1) == std::vector<int>{60, 64, 79}));      // SPR 1: the top an octave up
  CHECK((ex(60, 1, 0, 2) == std::vector<int>{60, 76, 91}));      // SPR 2: over three octaves
  CHECK((ex(60, 1, 0, 0, 4) == std::vector<int>{60, 64, 67, 72}));   // VOI 4: the root doubled above
  CHECK((ex(60, 1, 0, 0, 6) == std::vector<int>{60, 64, 67, 72, 76, 79}));
  CHECK((ex(60, 1, 0, 0, 2) == std::vector<int>{60, 64}));       // VOI 2: the top dropped
  CHECK((ex(60, 7) == std::vector<int>{60, 63, 67, 70}));        // MIN7
  CHECK((ex(60, 14) == std::vector<int>{60, 63, 67, 70, 74}));   // MIN9
  CHECK((ex(60, 12, 1) == std::vector<int>{64, 67, 70, 72, 74}));   // 9, first inversion
  CHECK((ex(60, 15) == std::vector<int>{60, 67}));               // 5
  CHECK((ex(122, 1) == std::vector<int>{122, 126}));             // beyond 127 dropped
  for (int chd = 0; chd < poly::kChords; ++chd)
    for (int inv = 0; inv < poly::kInvs; ++inv)
      for (int spr = 0; spr < poly::kSprs; ++spr)
        for (int voi = 0; voi <= poly::kVoiMax; ++voi) {
          std::vector<int> e = ex(48, chd, inv, spr, voi);
          CHECK(!e.empty() && (int)e.size() <= poly::kMaxNotes);
          for (size_t i = 1; i < e.size(); ++i) CHECK(e[i] > e[i - 1]);   // ascending, distinct
        }
  // the params from P13..P16, out of range = the first entry (an old sound's STA END LOP LEV: 0 120 0 100)
  float p[16] = {0};
  p[12] = 0, p[13] = 120, p[14] = 0, p[15] = 100;
  poly::Chord c = poly::chord_of(p);
  CHECK(c.chd == 0 && c.inv == 0 && c.spr == 0 && c.voi == 0);
  p[12] = 6.5f, p[13] = 2, p[14] = 1, p[15] = 5;
  c = poly::chord_of(p);
  CHECK(c.chd == 6 && c.inv == 2 && c.spr == 1 && c.voi == 5);
  p[12] = 16;
  CHECK(poly::chord_of(p).chd == 0);
}

static void test_detect() {
  // every chord the table plays: detected, and the detection plays the same notes
  int cases = 0;
  for (int chd = 1; chd < poly::kChords; ++chd)
    for (int inv = 0; inv < poly::kInvs; ++inv)
      for (int spr = 0; spr < poly::kSprs; ++spr)
        for (int voi = 0; voi <= poly::kVoiMax; ++voi)
          for (int root = 36; root <= 72; root += 5) {
            uint8_t o[poly::kMaxNotes];
            int n = poly::expand(root, {chd, inv, spr, voi}, o);
            if (n < 2) continue;
            poly::Detected d = poly::detect(o, n);
            CHECK(d.exact);
            uint8_t back[poly::kMaxNotes];
            int m = poly::expand(d.root, d.c, back);
            CHECK(m == n && memcmp(o, back, n) == 0);
            ++cases;
          }
  CHECK(cases > 3000);
  // the simplest reading wins
  const uint8_t cmaj[] = {67, 60, 64};   // any order
  poly::Detected d = poly::detect(cmaj, 3);
  CHECK(d.exact && d.root == 60 && d.c.chd == 1 && d.c.inv == 0 && d.c.spr == 0 && d.c.voi == 0);
  const uint8_t inv1[] = {64, 67, 72};
  d = poly::detect(inv1, 3);
  CHECK(d.exact && d.root == 60 && d.c.chd == 1 && d.c.inv == 1);
  const uint8_t am7[] = {57, 60, 64, 67};
  d = poly::detect(am7, 4);
  CHECK(d.exact && d.root == 57 && d.c.chd == 7 && d.c.inv == 0);
  const uint8_t c6[] = {60, 64, 67, 69};   // C6 in root position before Am7's first inversion
  d = poly::detect(c6, 4);
  CHECK(d.exact && d.root == 60 && d.c.chd == 10 && d.c.inv == 0);
  const uint8_t dbl[] = {60, 64, 67, 72, 60};   // a repeat ignored; the root doubled: MAJ VOI 4
  d = poly::detect(dbl, 5);
  CHECK(d.exact && d.root == 60 && d.c.chd == 1 && d.c.voi == 4);
  // one note (or none): OFF on it
  const uint8_t one[] = {62};
  d = poly::detect(one, 1);
  CHECK(d.c.chd == 0 && d.root == 62 && d.c.voi == 0);
  d = poly::detect(one, 0);
  CHECK(d.c.chd == 0);
  // an unknown set: the nearest chord (C major with an added F#: C MAJ, from its bass), or OFF when nothing is near
  const uint8_t add[] = {60, 64, 66, 67};
  d = poly::detect(add, 4);
  CHECK(!d.exact && d.c.chd == 1 && d.root == 60 && d.c.inv == 0 && d.c.voi == 4);
  const uint8_t e_bass[] = {52, 60, 66, 67};   // E bass under C, F#, G: still near C MAJ, inversion 1
  d = poly::detect(e_bass, 4);
  CHECK(!d.exact && d.c.chd == 1 && d.c.inv == 1 && d.root == 48 && d.c.spr == 1);
  const uint8_t semis[] = {60, 61};   // two notes a semitone apart: still exact (C# MAJ7, INV 3, the top two dropped)
  d = poly::detect(semis, 2);
  uint8_t back[poly::kMaxNotes];
  CHECK(d.exact && poly::expand(d.root, d.c, back) == 2 && back[0] == 60 && back[1] == 61);
  const uint8_t tri[] = {61, 60, 66};   // C# and F# of an F# 5 over a C bass: the 5 (2 of 3 shared), INV 0
  d = poly::detect(tri, 3);
  CHECK(!d.exact && d.c.chd == 15 && d.root == 54 && d.c.inv == 0 && d.c.voi == 3);
  const uint8_t cluster[] = {60, 61, 62};   // no chord shares more than half: OFF on the lowest
  d = poly::detect(cluster, 3);
  CHECK(!d.exact && d.c.chd == 0 && d.root == 60);
  const uint8_t cluster5[] = {64, 60, 61, 62, 63};
  d = poly::detect(cluster5, 5);
  CHECK(!d.exact && d.c.chd == 0 && d.root == 60);
}

static void test_texts() {
  char b[8];
  CHECK(strcmp(poly::value_text(0, 0, b, sizeof b), "OFF") == 0);
  CHECK(strcmp(poly::value_text(0, 6, b, sizeof b), "MAJ7") == 0);
  CHECK(strcmp(poly::value_text(0, 15, b, sizeof b), "5") == 0);
  CHECK(strcmp(poly::value_text(0, 99, b, sizeof b), "OFF") == 0);
  CHECK(strcmp(poly::value_text(1, 2, b, sizeof b), "INV2") == 0);
  CHECK(strcmp(poly::value_text(2, 1, b, sizeof b), "SPR1") == 0);
  CHECK(strcmp(poly::value_text(3, 0, b, sizeof b), "AUTO") == 0);
  CHECK(strcmp(poly::value_text(3, 4, b, sizeof b), "4") == 0);
  CHECK(strcmp(poly::value_text(3, 100, b, sizeof b), "AUTO") == 0);
  uint8_t m[16];
  int n = poly::sysex_chord_write(m, 8, 60, {7, 1, 0, 4});
  const uint8_t want[] = {0xF0, 0x7D, 0x52, 0x54, 0x28, 0x01, 8, 60, 7, 1, 0, 4, 0xF7};
  CHECK(n == 13 && memcmp(m, want, 13) == 0);
}

static void test_pool() {
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  voices_init();
  ui::remote_reset();
  // CHD OFF: the mono note, as before
  send(report(0x04, kFm | 4, 60, 100, fm(0)));
  CHECK(v[4].fm && v[4].live && poly::stats().active == 0);
  render(400);
  double mono = rms(4, 4800);
  CHECK(mono > 100);
  // CHD MAJ: three notes in the pool, on voice 4 only
  send(report(0x04, kFm | 4, 60, 100, fm(1)));
  CHECK(poly::stats().active == 3 && poly::stats().chords == 1 && poly::active(4) && !poly::active(0));
  render(400);
  CHECK(rms(4, 4800) > 100 && rms(0, 4800) == 0);
  // 0x11: released, then free once quiet (or 10 s on)
  uint8_t off[] = {0xF0, 0x7D, 0x52, 0x54, 0x11, 0x04, 0xF7};
  host_sysex(off, sizeof off);
  for (int k = 0; k < 30 && poly::stats().active; ++k) {
    render(400);
    advance_ms(500);
  }
  CHECK(poly::stats().active == 0);
  // the caps: MIN9 (5 notes) twice on one voice -> at most 6 on it, steals; on five voices -> 24 at most
  send(report(0x04, kFm | 0, 48, 100, fm(14)));
  send(report(0x04, kFm | 0, 50, 100, fm(14)));
  int on0 = 0;
  for (int k = 0; k < 2; ++k) render(1);
  on0 = poly::stats().active;
  CHECK(on0 <= poly::kPerVoice && poly::stats().steals > 0);
  for (int vi = 1; vi < 6; ++vi) send(report(0x04, kFm | vi, 48 + vi, 100, fm(14)));
  CHECK(poly::stats().active == poly::kTotal && poly::stats().peak == poly::kTotal);
  render(10);
  // the link reset: every note gone
  link_reset("test");
  CHECK(poly::stats().active == 0);
  // leaving FM silences the voice's pool
  send(report(0x04, kFm | 2, 60, 100, fm(2)));
  CHECK(poly::active(2));
  send(report(0x04, kPlaits | 2, 60, 100, {0}));
  CHECK(!poly::active(2));
  printf("test_poly: pool PASS\n");
}

static int count_cmd(uint8_t cmd, uint8_t sub) {
  int k = 0;
  for (auto& m : g_out) k += m.size() > 6 && m[4] == cmd && m[5] == sub;
  return k;
}

static void test_live() {
  voices_init();
  link_reset("test");
  g_out.clear();
  // voice 4 (CH / OH) on FM, CHD off. Pads in CHRO on the auto channel 14: MIDI note + 24 = the 0x04's note.
  send(report(0x04, kFm | 4, 60, 127, fm(0)));   // the first pad's trig (before its MIDI)
  advance_ms(1);
  poly::midi(0x9D, 36, 127);                     // one vote
  CHECK(poly::stats().bound[13] == -1 && poly::stats().active == 0);
  advance_ms(40);
  poly::midi(0x9D, 38, 127);
  advance_ms(1);
  send(report(0x04, kFm | 4, 62, 127, fm(0)));   // two votes: channel 14 -> voice 4; the held pads play from the pool
  CHECK(poly::stats().bound[13] == 4);
  CHECK(poly::stats().active == 2 && poly::stats().live_notes == 2);
  advance_ms(poly::kGestureMs + 2);              // the window (kGestureMs after its last note-on) closes
  CHECK(count_cmd(0x28, 0x01) == 1);
  auto& w = g_out.back();
  // C + D: SUS2 with VOI 2 (the top dropped); track 8 (CH, the first on voice 4: no 33 from the Rytm)
  CHECK(w.size() == 13 && w[6] == 8 && w[7] == 60 && w[8] == 3 && w[9] == 0 && w[10] == 0 && w[11] == 2);
  poly::midi(0x8D, 36, 0);
  poly::midi(0x8D, 38, 0);
  uint8_t off[] = {0xF0, 0x7D, 0x52, 0x54, 0x11, 0x04, 0xF7};
  host_sysex(off, sizeof off);
  render(40);
  // a chord, bound and active: MIDI first, the Rytm's trigs swallowed; its trig first: held, then swallowed
  advance_ms(500);
  uint32_t sw = poly::stats().swallowed, ln = poly::stats().live_notes;
  poly::midi(0x9D, 36, 127);
  advance_ms(2);
  send(report(0x04, kFm | 4, 60, 127, fm(0)));
  poly::midi(0x9D, 40, 127);
  send(report(0x04, kFm | 4, 64, 127, fm(0)));
  send(report(0x04, kFm | 4, 67, 127, fm(0)));   // before its note-on: held
  CHECK(poly::stats().held >= 1);
  advance_ms(2);
  poly::midi(0x9D, 43, 127);
  CHECK(poly::stats().swallowed == sw + 3 && poly::stats().live_notes == ln + 3);
  render(400);
  CHECK(rms(4, 4800) > 100);
  advance_ms(poly::kGestureMs + 2);
  CHECK(count_cmd(0x28, 0x01) == 2);
  auto& w2 = g_out.back();
  CHECK(w2[6] == 8 && w2[7] == 60 && w2[8] == 1 && w2[9] == 0 && w2[10] == 0 && w2[11] == 3);   // C MAJ, 3 notes
  // the Rytm's sequencer while the channel is active: a trig with no note-on is held kHoldMs, then played (mono)
  uint32_t held = poly::stats().held;
  send(report(0x04, kFm | 4, 72, 100, fm(1)));   // CHD MAJ on that step: after the hold, a chord
  CHECK(poly::stats().held == held + 1);
  uint32_t chords = poly::stats().chords;
  advance_ms(poly::kHoldMs + 2);
  CHECK(poly::stats().chords == chords + 1);
  // a single pad: no write-back (one note)
  poly::midi(0x9D, 36, 0);
  poly::midi(0x9D, 40, 0);
  poly::midi(0x9D, 43, 0);
  advance_ms(500);
  poly::midi(0x9D, 45, 127);
  advance_ms(100);
  CHECK(count_cmd(0x28, 0x01) == 2);
  // the Rytm's answer: poly's, never the remote UI's
  uint32_t bad = ui::remote_stats().bad;
  uint8_t ok[] = {0xF0, 0x7D, 0x52, 0x54, 0x28, 0x02, 8, 4, 0, 0xF7};
  host_sysex(ok, sizeof ok);
  uint8_t no[] = {0xF0, 0x7D, 0x52, 0x54, 0x28, 0x02, 8, 0x7F, 3, 0xF7};
  host_sysex(no, sizeof no);
  CHECK(poly::stats().replies_ok == 1 && poly::stats().replies_err == 1 && ui::remote_stats().bad == bad);
  char line[240];
  CHECK(poly::command("poly", line, sizeof line) && strstr(line, "ch14->v4"));
  printf("%s\n", line);
  // the voice left FM: the channel unbound at its next note
  send(report(0x04, kJuno | 4, 60, 100, {0, 80, 20, 64, 10, 64, 64, 127}));
  poly::midi(0x9D, 36, 127);
  CHECK(poly::stats().bound[13] == -1 && !poly::active(4));
  printf("test_poly: live PASS\n");
}

// ---- gain staging and the soft limiter: chords of 1 / 3 / 6 notes at full velocity never reach full scale (no hard
// clip), one note plays as loud as before (within 0.5 dB), the headroom gain moves smoothly (no zipper)
static int16_t peak_abs(int voice, int frames, int* at_full) {
  int16_t p = 0;
  *at_full = 0;
  for (int i = 0; i < frames; ++i) {
    int x = ring[i][voice] < 0 ? -ring[i][voice] : ring[i][voice];
    if (x > p) p = (int16_t)(x > 32767 ? 32767 : x);
    *at_full += x >= 32767;
  }
  return p;
}
// blocks rendered one at a time, the headroom's largest step from one block to the next
static float render_watch(int vi, int blocks) {
  float g = poly::headroom(vi), dmax = 0;
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) {
    core::render_voices(ring, core::kBlock * 400, b * core::kBlock, 0, core::kVoices, mach);
    float h = poly::headroom(vi), d = std::fabs(h - g);
    if (d > dmax) dmax = d;
    g = h;
  }
  return dmax;
}

static void test_headroom() {
  voices_init();
  link_reset("test");
  // one note (CHD MAJ VOI 1: the pool's one note): the headroom stays exactly 1 (the note as before, bit for bit), the
  // limiter untouched below its knee
  send(report(0x04, kFm | 3, 60, 127, fm(0)));   // (the voice's PAT loaded)
  render(40);
  link_reset("test");
  send(report(0x04, kFm | 3, 60, 127, fm(1, 0, 0, 1)));
  CHECK(poly::held_notes(3) == 1);
  uint32_t lim0 = voice_limited(3);
  float d = render_watch(3, 400);
  double mono = rms(3, 4800);
  CHECK(mono > 300 && d == 0 && poly::headroom(3) == 1.0f && voice_limited(3) == lim0);
  printf("  one note: rms %.0f, headroom %g\n", mono, poly::headroom(3));
  // live pads (a host keyboard, poly=bind): one note at 0.6 as before; 3 and 6 at full velocity: no hard clip
  char line[400];
  poly::command("poly=bind:2:3", line, sizeof line);   // (the voice still 6-OP FM: no link reset)
  poly::silence(3);
  render(40);
  const int keys[6] = {60, 64, 67, 72, 76, 79};   // (the first the one note above)
  double lv1 = 0;
  for (int pass = 0; pass < 6; ++pass) {   // twice (a slot's first note was quieter before test_first_note's fix)
    int n = pass % 3 == 0 ? 1 : pass % 3 == 1 ? 3 : 6;
    for (int k = 0; k < n; ++k) poly::midi(0x91, keys[k], 127);
    float dmax = render_watch(3, 400);
    int full;
    int16_t pk = peak_abs(3, core::kBlock * 400, &full);
    double r = rms(3, 4800);
    printf("  live %d notes: peak %d (%.2f dBFS), headroom %.3f (1/sqrt(n) %.3f), step %.4f a block, limited %lu\n", n,
           pk, 20 * std::log10(pk / 32767.0), poly::headroom(3), 1 / std::sqrt((double)n), dmax,
           (unsigned long)voice_limited(3));
    CHECK(full == 0 && pk <= (int)(core::kSoftCeil * 32767) + 1);
    CHECK(std::fabs(poly::headroom(3) - 1 / std::sqrt((float)n)) < 0.02f);
    CHECK(dmax < 0.06f);   // (3 ms down: about 0.08 of the change a 0.25 ms block)
    if (n == 1) {
      lv1 = r;
      CHECK(dmax == 0 && poly::headroom(3) == 1.0f);   // one live pad: its 0.6 exactly, as before
    } else {
      if (pass >= 3) CHECK(std::fabs(20 * std::log10(r / lv1)) < 3);   // a chord about as loud as one note (power)
    }
    for (int k = 0; k < n; ++k) poly::midi(0x81, keys[k], 0);
    for (int k = 0; k < 40 && poly::active(3); ++k) render(400), advance_ms(200);
    CHECK(!poly::active(3));
    render_watch(3, 400);
    CHECK(poly::headroom(3) == 1.0f);   // back to one note's
  }
  poly::command("poly=bind:2:off", line, sizeof line);
  // 6-OP FM trig chords of 3 and 6 notes (vel 127): their notes at 1 each, 1/sqrt(n) by the headroom, no hard clip
  for (int voi : {3, 6}) {
    link_reset("test");
    send(report(0x04, kFm | 3, 48, 127, fm(1, 0, 0, voi)));
    CHECK(poly::held_notes(3) == voi);
    float dmax = render_watch(3, 400);
    int full;
    int16_t pk = peak_abs(3, core::kBlock * 400, &full);
    CHECK(full == 0 && pk <= (int)(core::kSoftCeil * 32767) + 1 && dmax < 0.06f);
    CHECK(std::fabs(poly::headroom(3) - 1 / std::sqrt((float)voi)) < 0.02f);
  }
  CHECK(poly::command("poly", line, sizeof line));
  printf("%s\n", line);
  link_reset("test");
  printf("test_poly: headroom PASS\n");
}

// ---- the Rytm's mono pads (poly.h kStealMs): its note-off for the previous pad right after the next pad's note-on is
// a steal, not a release; the group sounds until the last pad's real note-off; strums keep their timing (the sink)
struct SinkEv {
  uint8_t st, d1;
  uint32_t ms;
};
static std::vector<SinkEv> g_sink;
static void sink(uint8_t st, uint8_t d1, uint8_t) { g_sink.push_back({st, d1, now_ms()}); }

static void bind_ch14_voice4() {
  voices_init();
  link_reset("test");
  for (int k = 0; k < 2; ++k) {   // two note-on / 0x04 pairs: channel 14 -> voice 4 (offset 24)
    poly::midi(0x9D, 30 + k, 100);
    advance_ms(1);
    send(report(0x04, kFm | 4, 54 + k, 100, fm(0)));
    advance_ms(1);
  }
  CHECK(poly::stats().bound[13] == 4);
  poly::midi(0x8D, 30, 0), poly::midi(0x8D, 31, 0);
  advance_ms(500);
}
// A pad as the Rytm sends it: its note-on, (1 ms) the previous pad's note-off (mono), its 0x04
static void pad(int note, int prev) {
  poly::midi(0x9D, note, 110);
  advance_ms(1);
  if (prev >= 0) poly::midi(0x8D, prev, 0);
  send(report(0x04, kFm | 4, note + 24, 110, fm(0)));
}

static void test_steal() {
  poly::set_note_sink(sink);
  const int A = 36, B = 40, C = 43;   // C E G at 60 64 67
  for (int gap : {20, 80}) {   // pads 20 ms apart, a strum 80 ms apart: one chord, all three sounding
    bind_ch14_voice4();
    g_out.clear(), g_sink.clear();
    uint32_t steals = poly::stats().mono_steals, t0 = now_ms();
    pad(A, -1);
    advance_ms(gap - 1);
    pad(B, A);
    CHECK(poly::held_notes(4) == 2);   // A kept
    advance_ms(gap - 1);
    pad(C, B);
    CHECK(poly::held_notes(4) == 3 && poly::stats().mono_steals == steals + 2);
    advance_ms(poly::kGestureMs + 5);   // the window closed: one write-back, C MAJ, 3 notes
    CHECK(count_cmd(0x28, 0x01) == 1);
    auto& w = g_out.back();
    CHECK(w[7] == 60 && w[8] == 1 && w[9] == 0 && w[10] == 0 && w[11] == 3);
    CHECK(poly::view(4).live && poly::view(4).root == 60 && poly::view(4).c.chd == 1);
    advance_ms(300);
    CHECK(poly::held_notes(4) == 3);   // still all three
    uint32_t t_off = now_ms();
    poly::midi(0x8D, C, 0);   // the last pad's real note-off: the group released
    CHECK(poly::held_notes(4) == 0);
    // the sink: the note-ons at their own times, every note-off at the release (none at a steal)
    int ons = 0, offs = 0;
    for (auto& e : g_sink) {
      if ((e.st & 0xF0) == 0x90) {
        CHECK(e.ms == t0 + (uint32_t)(ons * gap));
        ++ons;
      } else {
        CHECK(e.ms == t_off);
        ++offs;
      }
    }
    CHECK(ons == 3 && offs == 3);
    printf("  steal, pads %d ms apart: C MAJ, 3 notes held, released together\n", gap);
  }
  // a quick melody, single notes not overlapping (each note-off before the next note-on): no chord, no steal
  bind_ch14_voice4();
  g_out.clear();
  uint32_t steals = poly::stats().mono_steals;
  for (int n : {A, B, C}) {
    poly::midi(0x9D, n, 110);
    send(report(0x04, kFm | 4, n + 24, 110, fm(0)));
    advance_ms(70);
    CHECK(poly::held_notes(4) == 1);
    poly::midi(0x8D, n, 0);
    CHECK(poly::held_notes(4) == 0);
    advance_ms(10);
  }
  advance_ms(poly::kGestureMs + 5);
  CHECK(count_cmd(0x28, 0x01) == 0 && poly::stats().mono_steals == steals);
  // a pad again within the group (retriggered), then a new chord after a gap while the old one rings
  pad(A, -1);
  advance_ms(19);
  pad(B, A);
  advance_ms(19);
  pad(A, B);   // A again: B kept, A sounds again
  CHECK(poly::held_notes(4) == 2 && poly::stats().mono_steals == steals + 2);
  advance_ms(500);
  pad(C, A);   // 500 ms on: a new gesture; the Rytm's off for A is a real release of the old group
  CHECK(poly::held_notes(4) == 1);
  poly::midi(0x8D, C, 0);
  CHECK(poly::held_notes(4) == 0);
  // the keyboard path (poly=bind: real poly note-offs): an off right after another note-on releases it
  char line[64];
  poly::command("poly=bind:2:5", line, sizeof line);
  send(report(0x03, kFm | 5, -1, 0, fm(0)));
  steals = poly::stats().mono_steals;
  poly::midi(0x91, 60, 100);
  advance_ms(20);
  poly::midi(0x91, 64, 100);
  advance_ms(1);
  poly::midi(0x81, 60, 0);
  CHECK(poly::held_notes(5) == 1 && poly::stats().mono_steals == steals);
  poly::midi(0x81, 64, 0);
  CHECK(poly::held_notes(5) == 0);
  poly::command("poly=bind:2:off", line, sizeof line);
  poly::set_note_sink(nullptr);
  link_reset("test");
  printf("test_poly: steal PASS\n");
}

// A slot's first note and its later ones alike (poly.cpp start(): a free slot's Plaits voice re-initialised). Before,
// a freed slot's DX7 envelopes stayed mid-release (the render stops at -80 dB) and its next note started brighter and
// louder: ROM voice 10 with the LPG (DEC 20) 6..8 dB over its first 10 ms. Now each note's first 50 ms are the same
// samples, on the slot's first note and on its fourth.
static void test_first_note() {
  voices_init();
  link_reset("test");
  std::vector<float> p = {9, 64, 64, 20, 64, 64, 100, 127, 0, 0, 0, 0, 1, 0, 0, 1};   // PAT 10, DEC 20, CHD MAJ VOI 1
  std::vector<int16_t> first;
  for (int k = 0; k < 4; ++k) {
    send(report(0x04, kFm | 3, 60, 127, p));
    render(400);
    std::vector<int16_t> o;
    for (int i = 0; i < 2400; ++i) o.push_back(ring[i][3]);
    if (k == 0) first = o;
    CHECK(rms(3, 2400) > 300 && o == first);
    send({0xf0, 0x7d, 0x52, 0x54, 0x11, 3, 0xf7});
    for (int j = 0; j < 40 && poly::active(3); ++j) render(400), advance_ms(200);
    CHECK(!poly::active(3));
  }
  link_reset("test");
  printf("test_poly: first note PASS\n");
}

int main() {
  test_first_note();
  test_expand();
  test_detect();
  test_texts();
  test_pool();
  test_live();
  test_headroom();
  test_steal();
  printf("test_poly: PASS\n");
  return 0;
}
