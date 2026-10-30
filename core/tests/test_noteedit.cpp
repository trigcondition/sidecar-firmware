// core/tests/test_noteedit.cpp - NOTE EDIT on the SEQ page (docs/SEQ-IDEAS.md U1, docs/SEQUENCER.md "Built: NOTE EDIT"):
// the DATA encoder (LEVEL, rytm-mods 0026 encoder 8) and its push (key 0x78) caught on a poly track's SEQ page, the
// highlight walking every note in play order (step, then pitch) across pages, a held step's scope, taps, the selection
// (the DATA push, FUNC + push), A..E editing the highlighted note or the selection (POS NUDG LEN PTCH VEL), FUNC + NO
// deleting, NO / TRIG leaving, YES still the track's menu, the trig LEDs, a vanished note, another pattern. Everything
// goes through remote.cpp as the Rytm sends it (31 / 32). The renders: $SEQ_OUT (default build/seq) noteedit_*.png.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "hooks.h"
#include "engine.h"
#include "bundle/rename.h"
#include "machines.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/ui.h"
#include "voices.h"
#include "rui_png.h"
#include "timing.h"

using namespace core;
using namespace core::ui;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;
static uint32_t g_ms = 5000;
static std::string g_dir;

static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static void advance_ms(int ms) {
  g_us += (uint64_t)ms * 1000;
  host_fake_time(true, g_us);
}
static void frames(int n = 3) {
  for (int i = 0; i < n; ++i) remote_tick(g_ms), g_ms += 30, advance_ms(30);
}
static void key(int code, bool down) {
  auto m = msg(0x31, {(uint8_t)(code >> 7), (uint8_t)(code & 0x7F), (uint8_t)down});
  engine_sysex(m.data() + 0, (int)m.size());
}
static void press(int code) { key(code, true), advance_ms(40), key(code, false), advance_ms(40); }
static void enc(int e, int d, bool func = false) {
  auto m = msg(0x32, {(uint8_t)e, (uint8_t)(d + 64), (uint8_t)func});
  engine_sysex(m.data(), (int)m.size());
}
static constexpr int kData = 8, kPush = 0x78, kYes = 0x44, kNo = 0x45, kFunc = 0x80, kTrig = 0x30, kDown = 0x43;
static void data(int d, bool func = false) { enc(kData, d, func); }
static std::string cmd(const char* c) {
  char b[900];
  CHECK(seq::command(c, b, sizeof b));
  return b;
}
static bool shown(const char* s) {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  for (int i = 0; i < n; ++i)
    if (strstr((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
static void dump_texts() {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  printf("  texts:");
  for (int i = 0; i < n; ++i) printf(" [%s]", (const char*)t + 36 * i + 4);
  printf("\n");
}
static void png(const char* name) {
  uint8_t fb[kBytes];
  preview_sent(fb);
  rui_png(g_dir, name, fb);
}
// the last catch mask (37) the host sent: encoder i caught, key c caught
static std::vector<uint8_t> last_mask() {
  for (size_t i = g_out.size(); i-- > 0;)
    if (g_out[i].size() > 6 && g_out[i][4] == 0x37) return std::vector<uint8_t>(g_out[i].begin() + 6, g_out[i].end() - 1);
  return {};
}
static bool mask_enc(const std::vector<uint8_t>& b, int e) { return b.size() > 3 && (b[e / 7] >> (e % 7) & 1); }
static bool mask_key(const std::vector<uint8_t>& b, int k) { return b.size() > 3 + (size_t)(k / 7) && (b[3 + k / 7] >> (k % 7) & 1); }

static seq::Track* bd() { return seq::edit_track(0, false); }
static seq::Note nt(int p, int vel = 100, int len = 72, int micro = 0) { return {(uint8_t)p, (uint8_t)vel, (uint16_t)len, (int8_t)micro, 0}; }
// BD's pattern: S1 C4 E4 G4 (a chord), S5 D4, S9 F4, S17 A#4 (page 2); 32 steps
static void fill_pattern() {
  seq::Track* t = bd();
  CHECK(t);
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  t->length = 32;
  seq::add_note(t->step[0], nt(60)), seq::add_note(t->step[0], nt(64)), seq::add_note(t->step[0], nt(67));
  seq::add_note(t->step[4], nt(62));
  seq::add_note(t->step[8], nt(65));
  seq::add_note(t->step[16], nt(70));
  seq::touched();
}
static void at(int idx, int pitch, const char* what) {
  seq::NoteEdit n = seq::note_edit();
  printf("  %s: on %d highlight S%d pitch %d (%d of %d) sel %d page %d\n", what, n.on, n.idx + 1, n.pitch, n.pos, n.count,
         n.sel, seq::view().page + 1);
  CHECK(n.on && n.idx == idx && n.pitch == pitch);
}
static int find(const seq::Step& s, int pitch) {
  for (int i = 0; i < s.n; ++i)
    if (s.note[i].pitch == pitch) return i;
  return -1;
}

// ---- review round 1 (2026-10-27): the reviewer's probe scenarios (scratchpad nerev/t.cpp), each a failing check first
static std::vector<std::pair<int, seq::Track>> g_undo;   // the platform's undo history (the Pi: seq_touch's), as a hook
static void undo_save(int r, int pattern, const seq::Track& t) { (void)pattern; g_undo.push_back({r, t}); }
static bool undo_take(int r, int pattern, seq::Track& t) {
  (void)pattern;
  if (g_undo.empty() || g_undo.back().first != r) return false;
  t = g_undo.back().second;
  g_undo.pop_back();
  return true;
}
static void leave() {
  for (int i = 0; i < 3 && seq::note_edit().on; ++i) press(kNo), advance_ms(2100);
}
static void walk_to(int idx, int pitch) {
  if (!seq::note_edit().on) data(1);
  for (int i = 0; i < 20; ++i) data(-60);   // to the first note (a held trig: its chord's lowest)
  for (int i = 0; i < 2000; ++i) {
    auto n = seq::note_edit();
    if (n.on && n.idx == idx && n.pitch == pitch) return;
    data(1);
  }
  CHECK(!"walk_to: not reached");
}
static void func_push() { key(kFunc, true), press(kPush), key(kFunc, false); }
static void func_no() { key(kFunc, true), press(kNo), key(kFunc, false); }
static void func_yes() { key(kFunc, true), press(kYes), key(kFunc, false); }
static int notes_in(const seq::Track* t) {
  int n = 0;
  for (int i = 0; i < seq::kMaxSteps; ++i) n += t->step[i].n;
  return n;
}
static void review_round1() {
  seq::UndoHooks h = {undo_save, undo_take};
  seq::set_undo_hooks(&h);
  seq::Track* t = bd();
  CHECK(t);
  leave();
  // C1: a chord step with a condition, a lock and a ratchet, and a single-note step with its own: all selected, POS +1:
  // both trigs move whole
  seq::clear_steps(*t, 0, seq::kMaxSteps), t->length = 32;
  seq::add_note(t->step[0], nt(60)), seq::add_note(t->step[0], nt(64)), seq::add_note(t->step[0], nt(67));
  seq::cond_set(t->step[0], seq::kCondFill), t->step[0].nl = 1, t->step[0].lock[0] = {0, 0, 5000}, t->step[0].rt.cnt = 3;
  t->step[0].chd = 2, t->step[0].strum = 6;
  seq::add_note(t->step[8], nt(65)), seq::cond_set(t->step[8], seq::kCondFill), t->step[8].nl = 1, t->step[8].lock[0] = {1, 0, 7000};
  t->step[8].note[0].fresh = 1;
  seq::touched();
  data(1);
  func_push();
  CHECK(seq::note_edit().sel == 4);
  enc(0, 1);
  printf("  C1: S1 used %d | S2 n %d cond %d nl %d rt %d chd %d strum %d | S9 used %d | S10 n %d cond %d nl %d fresh %d\n",
         seq::step_used(t->step[0]), t->step[1].n, seq::cond_of(t->step[1]), t->step[1].nl, t->step[1].rt.cnt, t->step[1].chd,
         t->step[1].strum, seq::step_used(t->step[8]), t->step[9].n, seq::cond_of(t->step[9]), t->step[9].nl, t->step[9].note[0].fresh);
  CHECK(!seq::step_used(t->step[0]) && t->step[1].n == 3 && seq::cond_of(t->step[1]) == seq::kCondFill && t->step[1].nl == 1 &&
        t->step[1].lock[0].value == 5000 && t->step[1].rt.cnt == 3 && t->step[1].chd == 2 && t->step[1].strum == 6);
  CHECK(!seq::step_used(t->step[8]) && t->step[9].n == 1 && seq::cond_of(t->step[9]) == seq::kCondFill && t->step[9].nl == 1);
  CHECK(t->step[9].note[0].fresh == 1);   // (minor: the move never touches Note::fresh)
  CHECK(seq::note_edit().sel == 4 && seq::note_selected(1, 60) && seq::note_selected(9, 65));
  // the undo (the platform's history through the hooks): FUNC + YES puts the steps back
  func_yes();
  CHECK(t->step[0].n == 3 && t->step[8].n == 1 && !seq::step_used(t->step[1]) && seq::view().prompt == 0);
  leave();
  // I1: a single note with a lock and a condition moved onto an occupied step: the source stays a lock trig
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60)), t->step[0].nl = 1, t->step[0].lock[0] = {0, 0, 5000}, seq::cond_set(t->step[0], seq::kCondFill);
  seq::add_note(t->step[1], nt(64));
  seq::touched();
  data(1), walk_to(0, 60);
  enc(0, 1);
  printf("  I1: S1 n %d nl %d | S2 n %d nl %d\n", t->step[0].n, t->step[0].nl, t->step[1].n, t->step[1].nl);
  CHECK(t->step[0].n == 0 && t->step[0].nl == 1 && t->step[0].lock[0].value == 5000 && t->step[1].n == 2 && t->step[1].nl == 0);
  leave();
  // I2: C4 and C#4 by scale degree (C major, QNT EDIT): never merged into one note
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60)), seq::add_note(t->step[0], nt(61));
  seq::edit_pattern().key = 0, seq::edit_pattern().scale = seq::kScaleMaj, seq::edit_pattern().qnt = seq::kQntEdit;
  seq::touched();
  data(1), func_push();
  enc(3, 1);
  printf("  I2: S1 n %d: %d %d\n", t->step[0].n, t->step[0].note[0].pitch, t->step[0].note[1].pitch);
  CHECK(t->step[0].n == 2 && t->step[0].note[0].pitch != t->step[0].note[1].pitch);
  seq::edit_pattern().scale = 0, seq::edit_pattern().qnt = 0;
  leave();
  // I3: a held trig is what A..E edit - an empty one gets the track's note at the first turn; a selection elsewhere is
  // not touched while a trig is held
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60, 100)), seq::add_note(t->step[4], nt(62, 100));
  seq::touched();
  data(1), walk_to(0, 60);
  key(5, true), advance_ms(40);
  enc(4, 10);
  key(5, false), advance_ms(40);
  printf("  I3: S1 vel %d | S6 n %d vel %d | highlight S%d\n", t->step[0].note[0].vel, t->step[5].n, t->step[5].n ? t->step[5].note[0].vel : -1,
         seq::note_edit().idx + 1);
  CHECK(t->step[0].note[0].vel == 100 && t->step[5].n == 1 && t->step[5].note[0].vel == t->dvel + 10 && seq::note_edit().idx == 5);
  walk_to(0, 60), press(kPush);
  key(4, true), advance_ms(40);
  CHECK(seq::note_edit().idx == 4);
  enc(4, -30);
  printf("  I3: selection S1 vel %d, held S5 vel %d\n", t->step[0].note[0].vel, t->step[4].note[0].vel);
  CHECK(t->step[0].note[0].vel == 100 && t->step[4].note[0].vel == 70);
  // the step pages under a held trig with NOTE EDIT on: TRIG cycles TRIG RTRG FX as before, NOTE EDIT stays
  press(kTrig);
  CHECK(seq::note_edit().on && seq::view().tpage == seq::kSvTrig);
  enc(4, 9);   // COND
  CHECK(seq::cond_of(t->step[4]) != 0);
  frames();
  CHECK(shown("SEQ TRIG STEP"));
  key(4, false), advance_ms(40);
  frames();
  CHECK(seq::note_edit().on && shown("NOTE EDIT"));
  leave();
  // I4: FUNC + NO on more than one note asks first; the undo brings them back
  int all = notes_in(t);
  data(1), func_push();
  func_no();
  frames();
  dump_texts();
  CHECK(notes_in(t) == all && shown("AGAIN"));
  func_no();
  CHECK(notes_in(t) == 0);
  func_yes();
  CHECK(notes_in(t) == all);
  // one undo level a gesture: five VEL detents within a second are one; then PTCH another
  g_undo.clear();
  walk_to(0, 60);
  for (int i = 0; i < 5; ++i) enc(4, 1), advance_ms(100);
  CHECK(g_undo.size() == 1);
  enc(3, 1);
  CHECK(g_undo.size() == 2 && t->step[0].note[0].pitch == 61 && t->step[0].note[0].vel == 105);
  func_yes();
  CHECK(t->step[0].note[0].pitch == 60 && t->step[0].note[0].vel == 105);
  func_yes();
  CHECK(t->step[0].note[0].vel == 100);
  leave();
  // I5: the DATA push says what it did (the pause the Rytm needs after a push comes naturally)
  data(1), press(kPush);
  frames();
  CHECK(shown("SELECTED 1"));
  leave();
  // minor: FUNC + NO clears a step left with locks only (as a trig off)
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60)), t->step[0].nl = 1, t->step[0].lock[0] = {0, 0, 5000};
  seq::add_note(t->step[2], nt(62));
  seq::touched();
  data(1), walk_to(0, 60), func_no();
  CHECK(!seq::step_used(t->step[0]));
  leave();
  // minor: LEN on a chord's upper note sets the track's default length from that note
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60, 100, 48)), seq::add_note(t->step[0], nt(64, 100, 72));
  seq::touched();
  data(1), walk_to(0, 64), enc(2, 1);
  CHECK(t->dlen == 78);
  // minor: a selected note removed elsewhere (the touchscreen) leaves no stale bit
  press(kPush);
  CHECK(seq::note_edit().sel == 1);
  seq::remove_pitch(t->step[0], 64), seq::touched();
  CHECK(seq::note_edit().sel == 0);
  // minor: the touchscreen moved the highlighted note - the highlight goes with it
  walk_to(0, 60);
  seq::remove_pitch(t->step[0], 60), seq::add_note(t->step[6], nt(63)), seq::touched();
  seq::note_moved(0, 0, 60, 6, 63);
  CHECK(seq::note_edit().idx == 6 && seq::note_edit().pitch == 63);
  leave();
  // minor: a tap beyond the track's length adds nothing
  t->length = 12;
  data(1), press(13);
  CHECK(t->step[13].n == 0);
  frames();
  CHECK(shown("BEYOND"));
  leave();
  t->length = 32;
  // minor: NOTE EDIT entered with a trig held starts on that step
  key(6, true), advance_ms(40);
  data(1);
  CHECK(seq::note_edit().idx == 6);
  key(6, false), advance_ms(40);
  // minor: the knobs' labels drawn (fix1028: each value with its letter - S7 N+0 L.75 - the names in the footer)
  frames();
  CHECK(shown("S7") && shown("N+0") && shown("L.75"));
  enc(1, 1), frames();
  CHECK(shown("NUDGE"));
  enc(1, -1);
  png("noteedit_6_labels.png");
  leave();
  // I6: the dense worst case - 128 steps of 8 notes, 512 selected - POS and PTCH within a bound on the CPU clock
  t->length = 128;
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  for (int i = 0; i < 128; ++i)
    for (int k = 0; k < 8; ++k) seq::add_note(t->step[i], nt(60 + k));
  seq::touched();
  data(1);
  for (int i = 0; i < 64 * 8; ++i) press(kPush), data(1);
  CHECK(seq::note_edit().sel == 512);
  uint64_t c0 = timing_cpu_us();
  enc(0, 1);   // blocked all the way to the end (every step is full): the longest shift loop
  uint64_t c1 = timing_cpu_us();
  enc(3, 1);   // every pitch above is in the step: skipped up to 127
  uint64_t c2 = timing_cpu_us();
  printf("  I6: POS %llu us, PTCH %llu us (CPU)\n", (unsigned long long)(c1 - c0), (unsigned long long)(c2 - c1));
  CHECK(c1 - c0 < 3000 && c2 - c1 < 3000);
  t->length = 32;
  leave();
  seq::set_undo_hooks(nullptr);
}

// ---- review round 2 (2026-10-27): the re-review's probe (scratchpad nerev/t.cpp G2 G3 K) and findings N1..N4
static int g_clears = 0;
static void undo_clear() { g_undo.clear(), ++g_clears; }
static void select_steps(seq::Track* t, std::initializer_list<int> steps) {
  for (int i : steps)
    for (int k = 0; k < t->step[i].n; ++k) {
      walk_to(i, t->step[i].note[k].pitch);
      if (!seq::note_selected(i, t->step[i].note[k].pitch)) press(kPush), advance_ms(1100);
    }
}
static void review_round2() {
  seq::UndoHooks h = {undo_save, undo_take, undo_clear};
  seq::set_undo_hooks(&h);
  seq::Track* t = bd();
  CHECK(t);
  leave();
  // N1 (probe K): a move refused at the track's end leaves no landing marks behind
  t->length = 16;
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60)), seq::add_note(t->step[15], nt(60)), seq::add_note(t->step[4], nt(60));
  seq::touched();
  select_steps(t, {0, 15});
  CHECK(seq::note_edit().sel == 2);
  enc(0, 1);
  CHECK(t->step[0].n == 1 && t->step[15].n == 1);   // refused: S16 is the end
  leave();
  walk_to(4, 60);
  enc(0, -3);
  printf("  N1: S2 n %d S5 n %d\n", t->step[1].n, t->step[4].n);
  CHECK(t->step[1].n == 1 && t->step[4].n == 0);
  leave();
  // I6 again (probe G2): PTCH by degree, 112 notes at C-1 selected, the late steps' notes blocking every degree up:
  // refused within the cap (7 degrees), the work a deterministic count, the CPU time Pi-safe
  t->length = 128;
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  for (int i = 0; i < 128; ++i) seq::add_note(t->step[i], nt(0));
  {
    int deg = 1;
    for (int i = 112; i < 128; ++i)
      for (int k = 0; k < 7; ++k, ++deg) {
        int p = seq::degree_move(0, 0, seq::kScaleMaj, deg);
        if (p <= 127) seq::add_note(t->step[i], nt(p));
      }
  }
  seq::edit_pattern().key = 0, seq::edit_pattern().scale = seq::kScaleMaj, seq::edit_pattern().qnt = seq::kQntEdit;
  seq::touched();
  data(1);
  key(kFunc, true), press(kPush), key(kFunc, false);   // all, then the late steps' own notes out: 112 at C-1 stay in
  for (int i = 112; i < 128; ++i)
    for (int k = 0; k < t->step[i].n; ++k)
      if (t->step[i].note[k].pitch) walk_to(i, t->step[i].note[k].pitch), press(kPush), advance_ms(1100);
  seq::NoteEdit ne = seq::note_edit();
  printf("  G2: sel %d\n", ne.sel);
  CHECK(ne.sel == 128);
  bool ok = timing_retry("G2 PTCH by degree", [&] {
    uint64_t c0 = timing_cpu_us();
    enc(3, 1);
    uint64_t us = timing_cpu_us() - c0;
    printf("  G2: PTCH +1 degree: %llu us CPU, work %u, S1 pitch %d\n", (unsigned long long)us, seq::note_edit_work(),
           t->step[0].note[0].pitch);
    return us <= 250;
  });
  CHECK(ok);
  CHECK(t->step[0].note[0].pitch == 0 && seq::note_edit_work() <= 7u * 128u);
  seq::edit_pattern().scale = 0, seq::edit_pattern().qnt = 0;
  leave();
  // probe G3: POS, every step full: refused within the cap (16 shifts)
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  for (int i = 0; i < 128; ++i)
    for (int k = 0; k < 8; ++k) seq::add_note(t->step[i], nt(60 + k));
  seq::touched();
  walk_to(0, 60);
  for (int i = 0; i < 64 * 8; ++i) press(kPush), data(1);
  printf("  G3: sel %d\n", seq::note_edit().sel);
  CHECK(seq::note_edit().sel == 512);
  ok = timing_retry("G3 POS", [&] {
    uint64_t c0 = timing_cpu_us();
    enc(0, 1);
    uint64_t us = timing_cpu_us() - c0;
    printf("  G3: POS +1: %llu us CPU, work %u\n", (unsigned long long)us, seq::note_edit_work());
    return us <= 250;
  });
  CHECK(ok);
  CHECK(seq::note_edit_work() <= 16u * 512u);
  leave();
  // the degree table's steps equal degree_move's, every pitch of D dorian's range
  t->length = 16;
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::edit_pattern().key = 2, seq::edit_pattern().scale = seq::kScaleDor, seq::edit_pattern().qnt = seq::kQntEdit;
  for (int p = 0; p < 120; ++p) {
    seq::clear_steps(*t, 0, 1);
    seq::add_note(t->step[0], nt(p));
    seq::touched();
    walk_to(0, p);
    enc(3, 1);
    int want = seq::degree_move(p, 2, seq::kScaleDor, 1);
    if (t->step[0].note[0].pitch != want) printf("  degree: %d -> %d, degree_move %d\n", p, t->step[0].note[0].pitch, want);
    CHECK(t->step[0].note[0].pitch == want);
  }
  seq::edit_pattern().scale = 0, seq::edit_pattern().qnt = 0;
  leave();
  // N2: the undo's gesture is the knob on one note of one track and pattern: VEL on X, DATA, VEL on Y - two levels
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60)), seq::add_note(t->step[2], nt(62));
  seq::touched();
  walk_to(0, 60), enc(4, 1);
  g_undo.clear();   // (the level before it, and the state after the last gesture taken over the edits above: N3's cover)
  enc(4, 1);        // the same gesture: no level
  CHECK(g_undo.empty());
  data(1), enc(4, 1);
  printf("  N2: levels %d\n", (int)g_undo.size());
  CHECK(g_undo.size() == 1);
  // minor: a refused gesture makes no level (POS at the start)
  walk_to(0, 60), enc(0, -1);
  CHECK(g_undo.size() == 1);
  // minor: a held empty step filled and turned: one level
  key(5, true), advance_ms(40), enc(4, 3), key(5, false), advance_ms(40);
  CHECK(t->step[5].n == 1 && g_undo.size() == 2);
  // N3: a NOTE EDIT edit, then an edit outside it (the held trig's step page: its COND; a pad note into a held trig -
  // live recording's path, mark_dirty): FUNC + YES takes back the outside edit alone first, never both at once
  g_undo.clear();
  walk_to(0, 60);
  int v0 = t->step[0].note[0].vel;
  enc(4, 5);
  printf("  N3: vel %d levels %d\n", t->step[0].note[0].vel, (int)g_undo.size());
  CHECK(t->step[0].note[0].vel == v0 + 5 && g_undo.size() == 1);
  key(2, true), advance_ms(40), press(kTrig), enc(4, 9), key(2, false), advance_ms(40);
  CHECK(seq::cond_of(t->step[2]) != 0);
  func_yes();
  printf("  N3: after FUNC+YES: S1 vel %d, S3 cond %d\n", t->step[0].note[0].vel, seq::cond_of(t->step[2]));
  CHECK(t->step[0].note[0].vel == v0 + 5 && seq::cond_of(t->step[2]) == 0);
  func_yes();
  CHECK(t->step[0].note[0].vel == v0);
  walk_to(0, 60), enc(4, 5);
  key(3, true), advance_ms(40), seq::note(0, true, 72, 90, 0), seq::note(0, false, 72, 0, 0), key(3, false), advance_ms(40);
  CHECK(t->step[3].n == 1);
  func_yes();
  CHECK(t->step[3].n == 0 && t->step[0].note[0].vel == v0 + 5);
  // the same through the touchscreen's UNDO (seqed_pi: undo_prepare before seqed=undo)
  key(3, true), advance_ms(40), seq::note(0, true, 72, 90, 0), seq::note(0, false, 72, 0, 0), key(3, false), advance_ms(40);
  seq::undo_prepare(0);
  CHECK(undo_take(0, 0, *t));
  seq::touched();
  CHECK(t->step[3].n == 0 && t->step[0].note[0].vel == v0 + 5);
  // delete confirmation: the second FUNC + NO deletes only what the first asked about
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  seq::add_note(t->step[0], nt(60)), seq::add_note(t->step[0], nt(64)), seq::add_note(t->step[2], nt(62));
  seq::touched();
  leave();
  select_steps(t, {0});
  func_no();   // asks
  data(1);     // the highlight moved: asked again, nothing deleted
  func_no();
  CHECK(t->step[0].n == 2);
  // minor: FUNC + NO on a held trig's step pages is not NOTE EDIT's delete
  walk_to(2, 62);
  key(0, true), advance_ms(40), press(kTrig), func_no(), key(0, false), advance_ms(40);
  CHECK(t->step[0].n == 2 && t->step[2].n == 1);
  leave();
  // N4: a project change clears the history (the Pi: seq_touch's) - FUNC + YES never writes the old project's track
  // into the new one; while the project loads the undo is refused before it takes a level
  walk_to(0, 60), enc(4, 1);
  CHECK(!g_undo.empty());
  size_t lv = g_undo.size();
  seq::loading_begin();
  func_yes();
  CHECK(g_undo.size() == lv);
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 'E', 'R', 0xF7};
  int clears = g_clears;
  engine_sysex(proj, sizeof proj);
  if (rename::state() == rename::kAsk) rename::answer(false);   // (OTHER is a new project, not NOTEED renamed)
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  printf("  N4: clears %d -> %d, levels %d\n", clears, g_clears, (int)g_undo.size());
  CHECK(g_clears > clears && g_undo.empty());
  seq::set_undo_hooks(nullptr);
}

// ---- review round 3 (2026-10-27): the N3 cover's holes (scratchpad nerev/t2.cpp P1 P2 P4), with a history shaped as the
// Pi's (pi/ui/seq_touch: undo and redo per track and pattern; a new level drops the redo) and the touchscreen's UNDO /
// REDO as seqed_pi runs them (undo_prepare before seqed=undo / redo; level_taken after the other lines)
struct Lv { int r, p; seq::Track t; };
static std::vector<Lv> g_h_undo, g_h_redo;
static Lv* h_newest(std::vector<Lv>& v, int r, int p) {
  for (size_t i = v.size(); i-- > 0;)
    if (v[i].r == r && v[i].p == p) return &v[i];
  return nullptr;
}
static void h_drop(std::vector<Lv>& v, Lv* e) { v.erase(v.begin() + (e - v.data())); }
static int h_levels(int r, int p) {
  int n = 0;
  for (auto& x : g_h_undo) n += x.r == r && x.p == p;
  return n;
}
static void h_save(int r, int p, const seq::Track& t) {
  g_h_undo.push_back({r, p, t});
  for (size_t i = g_h_redo.size(); i-- > 0;)
    if (g_h_redo[i].r == r && g_h_redo[i].p == p) g_h_redo.erase(g_h_redo.begin() + (long)i);
}
static bool h_undo(int r, int p, seq::Track& t) {
  Lv* e = h_newest(g_h_undo, r, p);
  if (!e) return false;
  seq::Track old = e->t;
  h_drop(g_h_undo, e);
  g_h_redo.push_back({r, p, t});
  t = old;
  return true;
}
static void h_clear() { g_h_undo.clear(), g_h_redo.clear(); }
static int pat() { return seq::edit_pattern().index; }
static void touch_undo(int r, bool redo) {   // as seqed_pi::Command runs seqed=undo / seqed=redo
  seq::Track* t = seq::edit_track(r, false);
  CHECK(t);
  seq::undo_prepare(r);   // (seqed_pi: before seqed=undo and seqed=redo)
  std::vector<Lv>& from = redo ? g_h_redo : g_h_undo;
  Lv* e = h_newest(from, r, pat());
  if (!e) return;
  seq::Track old = e->t;
  h_drop(from, e);
  (redo ? g_h_undo : g_h_redo).push_back({r, pat(), *t});
  *t = old;
  seq::touched();
}
static void review_round3() {
  seq::UndoHooks h = {h_save, h_undo, h_clear};
  seq::set_undo_hooks(&h);
  h_clear();
  CHECK(cmd("seq=track:1:on").find("mask") != std::string::npos);
  CHECK(cmd("seq=track:2:on").find("mask") != std::string::npos);
  seq::Track* t = bd();
  CHECK(t);
  leave();
  seq::clear_steps(*t, 0, seq::kMaxSteps), t->length = 16;
  seq::add_note(t->step[0], nt(60, 100)), seq::add_note(t->step[4], nt(62, 100));
  seq::touched();
  walk_to(0, 60);
  enc(4, 1), enc(4, -1), advance_ms(1500);   // (a gesture: the edits above - outside NOTE EDIT - become their own level)
  h_clear();
  // NB-1 (P1): NOTE EDIT VEL, the touchscreen's UNDO, NOTE EDIT PTCH, FUNC + YES twice: never the undone VEL again
  enc(4, 10), advance_ms(1500);
  CHECK(t->step[0].note[0].vel == 110);
  touch_undo(0, false);
  CHECK(t->step[0].note[0].vel == 100);
  enc(3, 1), advance_ms(1500);
  CHECK(t->step[0].note[0].pitch == 61 && t->step[0].note[0].vel == 100);
  func_yes();
  CHECK(t->step[0].note[0].pitch == 60 && t->step[0].note[0].vel == 100);
  func_yes();
  printf("  P1: FUNC+YES twice: pitch %d vel %d, S9 n %d\n", t->step[0].note[0].pitch, t->step[0].note[0].vel, t->step[8].n);
  CHECK(t->step[0].note[0].pitch == 60 && t->step[0].note[0].vel == 100 && t->step[8].n == 0 && t->length == 16);
  // and REDO from the touchscreen: then FUNC + YES takes that back
  advance_ms(1500);
  // NB-2 (P2): one gesture of 10 VEL detents with SD (another track) edited between each: one level
  h_clear(), advance_ms(1500);
  for (int i = 0; i < 10; ++i) {
    enc(4, 1), advance_ms(25);
    cmd(i & 1 ? "seq=step:2:3:60" : "seq=step:2:3:62"), advance_ms(25);
  }
  printf("  P2: BD levels %d\n", h_levels(0, pat()));
  CHECK(h_levels(0, pat()) == 1);
  // P2b: the same without them: one level
  h_clear(), advance_ms(1500);
  for (int i = 0; i < 10; ++i) enc(4, 1), advance_ms(50);
  printf("  P2b: BD levels %d\n", h_levels(0, pat()));
  CHECK(h_levels(0, pat()) == 1);
  advance_ms(1500);
  // NB-3 (P4): a gesture on A, an outside edit on A, another pattern B and a gesture there, back to A: FUNC + YES takes
  // back the outside edit alone
  h_clear();
  int v0 = t->step[0].note[0].vel;
  walk_to(0, 60), enc(4, -30), advance_ms(1500);
  CHECK(t->step[0].note[0].vel == v0 - 30);
  cmd("seq=step:1:9:65");
  CHECK(t->step[8].n == 1);
  auto m2 = msg(0x2D, {2, 1, 0x7F});
  engine_sysex(m2.data(), (int)m2.size());
  while (seq::idle()) {}
  CHECK(pat() == 1);
  cmd("seq=track:1:on");
  {
    seq::Track* tb = seq::edit_track(0, true);
    CHECK(tb);
    seq::add_note(tb->step[3], nt(48));
    seq::touched();
  }
  data(1), enc(4, 3), advance_ms(1500);
  auto m1 = msg(0x2D, {2, 0, 0x7F});
  engine_sysex(m1.data(), (int)m1.size());
  while (seq::idle()) {}
  CHECK(pat() == 0);
  t = bd();
  CHECK(t && t->step[8].n == 1 && t->step[0].note[0].vel == v0 - 30);
  data(1), func_yes();
  printf("  P4: FUNC+YES on A: S9 n %d, S1 vel %d (v0 %d)\n", t->step[8].n, t->step[0].note[0].vel, v0);
  CHECK(t->step[8].n == 0 && t->step[0].note[0].vel == v0 - 30);
  func_yes();
  CHECK(t->step[0].note[0].vel == v0);
  leave();
  seq::set_undo_hooks(nullptr);
}

// ---- fix1028 (hardware report): "the current note you move to using the data encoder should always be in viewport;
// there is only room for 4 rows of notes". NOTE EDIT's screen has no title bar now (its line 1 says NOTE EDIT and the
// track), the roll rows 8..38, a lane a pitch; more pitches than lanes: a window that follows the highlight, the least
// move, a lane of margin where the ends allow, no move while the highlight stays inside.
static bool lit(const uint8_t* fb, int x, int y) { return fb[(7 - y / 8) + 8 * x] >> (y % 8) & 1; }
static int run_in_row(const uint8_t* fb, int y) {   // the longest run of lit pixels in row y
  int best = 0, n = 0;
  for (int x = 0; x < 128; ++x) n = lit(fb, x, y) ? n + 1 : 0, best = n > best ? n : best;
  return best;
}
static void fix1028_window() {
  // the pure window rule
  CHECK(ne_window(5, 10, 8, 7) == 0);    // all fit: no window
  CHECK(ne_window(5, 10, 20, 0) == 0);   // the lowest pitch: the window at the bottom (no margin below the end)
  CHECK(ne_window(0, 10, 20, 19) == 10); // the highest: at the top
  CHECK(ne_window(0, 10, 20, 5) == 0);   // inside: no move
  {
    int w = 0;   // up one lane at a time: still until the margin is reached, then a lane a step
    for (int f = 0; f <= 8; ++f) CHECK((w = ne_window(w, 10, 20, f)) == 0);
    CHECK((w = ne_window(w, 10, 20, 9)) == 1);
    CHECK((w = ne_window(w, 10, 20, 10)) == 2);
    for (int f = 9; f >= 3; --f) CHECK((w = ne_window(w, 10, 20, f)) == 2);   // back down inside: no jitter
    CHECK((w = ne_window(w, 10, 20, 2)) == 1);
    CHECK((w = ne_window(1, 10, 20, 17)) == 9);   // a jump up: the least move, a lane of margin above
    CHECK((w = ne_window(w, 10, 20, 3)) == 2);    // and down
    CHECK(ne_window(0, 1, 20, 7) == 7 && ne_window(0, 2, 20, 7) == 6);   // too few lanes for a margin
    CHECK(ne_window(3, 10, 1, 0) == 0 && ne_window(3, 10, 10, 9) == 0);   // one pitch; exactly as many as fit
    CHECK(ne_window(4, 10, 20, -1) == 4 && ne_window(15, 10, 20, -1) == 10);   // no focus: kept, clamped to the top
    CHECK(ne_window(30, 10, 20, 12) == 10 && ne_window(-5, 10, 20, 3) == 0);    // a start past either end: clamped
  }
  seq::Track* t = bd();
  CHECK(t);
  leave();
  // page 1: 14 steps jumping five octaves (36 + i, 96 + i), S15 a chord of eight over eight octaves: 21 pitches (44 twice)
  seq::clear_steps(*t, 0, seq::kMaxSteps), t->length = 16;
  for (int i = 0; i < 14; ++i) seq::add_note(t->step[i], nt(i % 2 ? 96 + i : 36 + i));
  const int chord[8] = {30, 44, 58, 72, 86, 100, 114, 126};
  for (int p : chord) seq::add_note(t->step[14], nt(p));
  seq::touched();
  data(1);
  for (int i = 0; i < 40; ++i) data(-60);
  frames();
  int plo = -1, phi = -1, scrolls = 0, top = 0, bottom = 0;
  for (int k = 0; k < 40; ++k) {
    seq::NoteEdit n = seq::note_edit();
    frames();
    NeRoll r = ne_roll_shown();
    uint8_t fb[kBytes];
    preview_sent(fb);
    bool in = r.lo <= n.pitch && n.pitch <= r.hi;
    printf("  fix1028 %2d: S%d %3d  lanes %d shown %d [%d..%d] focus_y %d %s%s\n", k, n.idx + 1, n.pitch, r.lanes, r.visible,
           r.lo, r.hi, r.focus_y, r.more_below ? "v" : "", r.more_above ? "^" : "");
    CHECK(r.lanes == 21 && r.visible >= 8 && r.visible < r.lanes);
    CHECK(in && r.focus_y >= 0);                                // the highlighted note always shows
    CHECK(r.focus_y - 2 >= 6 && r.focus_y + 2 + 1 <= 40);       // its frame inside the roll (rows 6..39)
    CHECK(run_in_row(fb, r.focus_y - 2) >= 5);                  // the frame drawn
    if (plo >= 0 && plo < n.pitch && n.pitch < phi) CHECK(r.lo == plo && r.hi == phi);   // inside with a margin: still
    if (plo >= 0 && (r.lo != plo || r.hi != phi)) ++scrolls;
    if (n.pitch == 30) { CHECK(!r.more_below && r.more_above); ++bottom; }    // the range's lowest: the window at the bottom
    if (n.pitch == 126) { CHECK(r.more_below && !r.more_above); ++top; }     // the highest: at the top
    if (k == 0) png("noteedit_7_window_low.png");
    if (n.idx == 1) png("noteedit_8_window_jump.png");   // S1 36 -> S2 97: five octaves up
    plo = r.lo, phi = r.hi;
    if (n.idx == 14 && n.pitch == 126) break;
    data(1);
  }
  printf("  fix1028: %d scrolls, bottom %d top %d\n", scrolls, bottom, top);
  CHECK(scrolls > 0 && bottom == 1 && top == 1);
  {   // the redesign: no inverted title bar (row 3 was lit across), line 1 NOTE EDIT and the track, the values
    uint8_t fb[kBytes];
    preview_sent(fb);
    int on = 0;
    for (int x = 0; x < 128; ++x) on += lit(fb, x, 3);
    dump_texts();
    CHECK(on < 64 && shown("NOTE EDIT BD") && shown("S15 8/8 22/22") && shown("V100"));
    png("noteedit_9_window_top.png");
  }
  // a held step's chord (DATA walks only it): each of its notes shows, top to bottom of the range
  leave();
  key(14, true), advance_ms(600);
  data(1);
  for (int i = 0; i < 10; ++i) data(-1);
  for (int k = 0; k < 8; ++k) {
    seq::NoteEdit n = seq::note_edit();
    frames();
    NeRoll r = ne_roll_shown();
    printf("  fix1028 held S15: %3d [%d..%d] focus_y %d\n", n.pitch, r.lo, r.hi, r.focus_y);
    CHECK(n.idx == 14 && n.pitch == chord[k] && r.lo <= n.pitch && n.pitch <= r.hi && r.focus_y >= 0);
    if (k == 4) png("noteedit_10_window_chord.png");
    data(1);
  }
  key(14, false), advance_ms(40);
  // a selection: the highlight is what the window follows (the selected notes elsewhere may be out of it)
  walk_to(0, 36), press(kPush);
  walk_to(14, 126);
  frames();
  {
    NeRoll r = ne_roll_shown();
    CHECK(seq::note_edit().sel == 1 && r.lo <= 126 && r.hi == 126 && r.focus_y >= 0 && r.lo > 36);
  }
  // few pitches: every one shows, no window
  leave();
  seq::clear_steps(*t, 0, seq::kMaxSteps), t->length = 32;
  seq::add_note(t->step[0], nt(60)), seq::add_note(t->step[4], nt(62)), seq::add_note(t->step[8], nt(65));
  seq::touched();
  data(1);
  frames();
  {
    NeRoll r = ne_roll_shown();
    CHECK(r.lanes == 3 && r.visible == 3 && r.lo == 60 && r.hi == 65 && !r.more_below && !r.more_above && r.focus_y >= 0);
  }
  leave();
}

// ---- ux1027 (the user, 2026-10-27: "you cant use yes + play or yes + scene to quick save a kit or pattern"): on a poly
// track's SEQ page YES is the Rytm's (its YES combos, the quick saves), caught only while a question shows (the menu,
// RESTORE n NOTES?, TURN OFF?), FUNC is held (FUNC + YES: UNDO) or a trig is held (trig + YES: the step preview). DOWN
// opens the track's menu; the page has no LEFT / RIGHT siblings (it replaces the Rytm's screen, as the SRC view does).
// updown1029 (the user, 2026-10-29: "in poly mode i want to use the chromatic mode / pads to play notes. i cant change
// octaves now with the up and down arrow"): UP / DOWN are the Rytm's too (CHRO's octave), caught only under a question
// or with FUNC held; the menu is FUNC + DOWN.
static constexpr int kUp = 0x42;
static constexpr int kPlay = 0x51, kScne = 0x13, kPage = 0x84;   // rytm-emu panel.rs: PLAY 0x51, SCNE 0x13
// review 1 / 8: YES + PLAY and YES + SCNE are the Rytm's quick saves: PLAY and SCNE never in the SEQ page's mask
static bool saves_free() {
  auto m = last_mask();
  return !m.empty() && !mask_key(m, kPlay) && !mask_key(m, kScne);
}
static void ux1027_keys() {
  frames();
  auto b = last_mask();
  printf("  ux1027: YES %d UP %d DOWN %d NO %d FUNC %d\n", mask_key(b, kYes), mask_key(b, kUp), mask_key(b, kDown),
         mask_key(b, kNo), mask_key(b, kFunc));
  CHECK(!b.empty() && !mask_key(b, kYes) && !mask_key(b, kUp) && !mask_key(b, kDown) && mask_key(b, kNo) &&
        mask_key(b, kFunc) && saves_free());   // updown1029: UP / DOWN the Rytm's (CHRO's octave)
  CHECK(mask_key(b, 0x40) && mask_key(b, 0x41) && mask_key(b, kPage));   // (LEFT / RIGHT / PAGE: still the page's)
  dump_texts();
  CHECK(shown("F+DOWN:MENU") && !shown("YES: MENU"));
  press(kYes);   // (were one to come: no menu)
  CHECK(seq::view().prompt == 0);
  press(kDown);  // updown1029: DOWN alone (were one to come: it is the Rytm's) - no menu
  press(kUp);
  CHECK(seq::view().prompt == 0 && remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
  key(kFunc, true), press(kDown), key(kFunc, false);   // FUNC + DOWN: the menu
  CHECK(seq::view().prompt == seq::kPromptMenu && seq::view().prompt_sel == 0);
  frames();
  b = last_mask();   // a question on screen: YES answers it, UP / DOWN its rows (FUNC let go)
  CHECK(mask_key(b, kYes) && mask_key(b, kUp) && mask_key(b, kDown) && saves_free());
  png("ux1027_1_menu_by_down.png");
  press(kDown);  // DOWN in the menu: its next row, as before
  CHECK(seq::view().prompt == seq::kPromptMenu && seq::view().prompt_sel == 1);
  press(kDown), press(kUp);   // UP: back a row
  CHECK(seq::view().prompt == seq::kPromptMenu && seq::view().prompt_sel == 1);
  press(kNo);
  CHECK(seq::view().prompt == 0);
  frames();
  b = last_mask();
  CHECK(!mask_key(b, kYes) && !mask_key(b, kUp) && !mask_key(b, kDown));
  key(kFunc, true);   // FUNC held: FUNC + YES is the undo - the mask names YES at the first tick after FUNC's press
  {                   // (kInputMs after the last frame: well before a YES pressed >= 100 ms after FUNC)
    size_t n0 = g_out.size();
    remote_tick(g_ms += 10), advance_ms(10);
    bool got = false;
    for (size_t i = n0; i < g_out.size(); ++i) got |= g_out[i].size() > 6 && g_out[i][4] == 0x37;
    printf("  ux1027: FUNC down, 10 ms later: a mask %d, YES %d UP %d DOWN %d\n", got, mask_key(last_mask(), kYes),
           mask_key(last_mask(), kUp), mask_key(last_mask(), kDown));
    CHECK(got && mask_key(last_mask(), kYes) && mask_key(last_mask(), kUp) && mask_key(last_mask(), kDown));
  }
  frames();
  CHECK(mask_key(last_mask(), kYes) && mask_key(last_mask(), kDown) && saves_free());
  press(kUp);     // updown1029: FUNC + UP - nothing (caught: not the Rytm's octave either)
  CHECK(seq::view().prompt == 0);
  press(kDown);   // updown1029: FUNC + DOWN is the menu now (review 6's guard: a stray menu in a FUNC chord)
  CHECK(seq::view().prompt == seq::kPromptMenu && seq::view().prompt_sel == 0);
  press(kDown);   // FUNC still held: the menu's rows as without it
  CHECK(seq::view().prompt == seq::kPromptMenu && seq::view().prompt_sel == 1);
  press(kNo);
  CHECK(seq::view().prompt == 0);
  key(kFunc, false);
  frames();
  b = last_mask();
  CHECK(!mask_key(b, kYes) && !mask_key(b, kUp) && !mask_key(b, kDown) && saves_free());
  key(kPage, true), key(kFunc, true), press(kDown), key(kFunc, false), key(kPage, false);   // PAGE held + FUNC + DOWN: no menu
  CHECK(seq::view().prompt == 0);
  seq::view_set(0, 0, -1, true);   // (PAGE's release stepped the page: back to page 1)
  key(15, true);   // T16 held: trig + YES is the step preview
  frames();
  b = last_mask();   // updown1029: UP / DOWN stay the Rytm's under a trig (the page has no use for them)
  CHECK(mask_key(b, kYes) && !mask_key(b, kUp) && !mask_key(b, kDown) && saves_free());
  key(kFunc, true), press(kDown), key(kFunc, false);   // review 8: (FUNC +) DOWN with a trig held - no menu, the page stays
  CHECK(seq::view().prompt == 0 && remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
  advance_ms(600);   // (a hold, not a tap: S16 stays empty)
  key(15, false);
  frames();
  CHECK(!mask_key(last_mask(), kYes) && !bd()->step[15].n);
  // LEFT / RIGHT / DOWN: never another page (they moved on the SIDECAR ring before)
  for (int c : {0x40, 0x41}) {
    press(c);
    frames();
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
  }
  press(kDown);   // updown1029: DOWN alone - nothing here (the Rytm's), the page stays
  CHECK(seq::view().prompt == 0 && remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
  key(kFunc, true), press(kDown), key(kFunc, false), press(kNo);   // FUNC + DOWN, NO: the menu and back
  CHECK(seq::view().prompt == 0 && remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
}

int main() {
  g_dir = rui_png_dir("SEQ_OUT", "build/seq");
  char tmpl[] = "/tmp/core_noteedit_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'N', 'O', 'T', 'E', 'E', 'D', 0xF7};
  engine_sysex(proj, sizeof proj);
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(kFm | 0)};
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(k < 8 ? 64 : 0)), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
  CHECK(cmd("seq=track:1:on").find("mask 0x001") != std::string::npos);
  fill_pattern();

  // ---- the page open on BD's SEQ: DATA (encoder 8) and its push (0x78) caught; on a plain track not
  const uint8_t open[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 1, 0, 0xF7};
  engine_sysex(open, sizeof open);
  remote_show_page("SEQ");
  frames();
  {
    int nm = 0;
    for (auto& x : g_out) nm += x.size() > 6 && x[4] == 0x37;
    printf("  page %s, masks sent %d of %d messages\n", remote_page_title() ? remote_page_title() : "-", nm, (int)g_out.size());
    auto b = last_mask();
    printf("  the SEQ page's mask on BD (poly): DATA %d push %d A %d\n", mask_enc(b, kData), mask_key(b, kPush), mask_enc(b, 0));
    CHECK(mask_enc(b, 0) && mask_enc(b, kData) && mask_key(b, kPush));
  }
  seq::view_set(1, 0, -1, true);   // SD: no poly track
  frames();
  {
    auto b = last_mask();
    printf("  on SD (plain): DATA %d push %d\n", mask_enc(b, kData), mask_key(b, kPush));
    CHECK(!b.empty() && !mask_enc(b, kData) && !mask_key(b, kPush));
    data(1);   // (were it caught: nothing on a plain track)
    CHECK(!seq::note_edit().on);
  }
  seq::view_set(0, 0, -1, true);
  frames();
  CHECK(mask_enc(last_mask(), kData));
  ux1027_keys();

  // ---- enter: the first detent lands on the page's first note, then step by step through the chord, the pages
  CHECK(!seq::note_edit().on);
  data(1);
  at(0, 60, "the first detent");
  frames();
  dump_texts();
  CHECK(shown("NOTE EDIT") && shown("S1 1/3") && shown("C4"));
  png("noteedit_1_entered.png");
  data(1), at(0, 64, "+1");
  data(1), at(0, 67, "+1");
  data(1), at(4, 62, "+1 (the next step)");
  data(2), at(16, 70, "+2 (page 2)");
  CHECK(seq::view().page == 1);   // the page follows the highlight
  frames();
  png("noteedit_2_page2.png");
  data(3), at(16, 70, "+3 at the end: stays");
  data(-1, true), at(8, 65, "FUNC -1: the step before");
  data(-1, true), at(4, 62, "FUNC -1");
  CHECK(seq::view().page == 0);
  data(-5), at(0, 60, "-5 at the start: the first");

  // ---- a held step: its chord only; let go: on through the pattern. A tap moves the highlight, never deletes
  data(2), at(0, 67, "S1's G4");
  key(8, true), advance_ms(40);
  at(8, 65, "T9 held: its lowest note");
  data(1), at(8, 65, "T9 held, +1: its only note (the scope)");
  key(8, false), advance_ms(40);
  CHECK(bd()->step[8].n == 1);   // the tap did not toggle it off
  key(0, true), advance_ms(600);
  at(0, 60, "T1 held");
  data(1), at(0, 64, "T1 held +1");
  data(1), at(0, 67, "T1 held +1");
  frames();
  png("noteedit_3_chord_held.png");
  data(1), at(0, 67, "T1 held +1: the chord's end");
  key(0, false), advance_ms(40);
  CHECK(bd()->step[0].n == 3);
  data(1), at(4, 62, "let go, +1: on through the pattern");
  press(2);   // a tap on an empty step: the track's note added there and highlighted
  printf("  T3 tapped: n %d\n", bd()->step[2].n);
  CHECK(bd()->step[2].n >= 1);
  at(2, bd()->step[2].note[0].pitch, "T3 tapped (empty): a note added");
  key(kFunc, true), press(kNo), key(kFunc, false);   // FUNC + NO deletes it again
  CHECK(bd()->step[2].n == 0 && !seq::step_used(bd()->step[2]));
  at(4, 62, "deleted: the next note");

  // ---- the knobs on the highlighted note: E VEL, D PTCH, C LEN, B NUDG, A POS
  {
    seq::Step& s = bd()->step[4];
    enc(4, 5); CHECK(s.note[0].vel == 105);
    enc(4, 2, true); CHECK(s.note[0].vel == 121);
    enc(3, 2); CHECK(s.note[0].pitch == 64);
    at(4, 64, "PTCH +2: the highlight follows its pitch");
    enc(2, 1); CHECK(s.note[0].len == 78);
    enc(1, 3); CHECK(s.note[0].micro == 3);
    enc(0, 1, true); CHECK(s.note[0].micro == 4);   // FUNC + POS: by 1/96
    seq::cond_set(s, seq::kCondFill);
    enc(0, 1);   // POS +1: the step's only note into an empty step - the trig moves (its condition along)
    seq::Step& d = bd()->step[5];
    printf("  POS +1: S5 n %d, S6 n %d pitch %d micro %d cond %d\n", bd()->step[4].n, d.n, d.note[0].pitch, d.note[0].micro,
           seq::cond_of(d));
    CHECK(bd()->step[4].n == 0 && !seq::step_used(bd()->step[4]) && d.n == 1 && d.note[0].pitch == 64 && d.note[0].micro == 4 &&
          seq::cond_of(d) == seq::kCondFill);
    at(5, 64, "POS +1");
    frames();
    CHECK(shown("S6"));
    png("noteedit_4_moved.png");
    // POS into steps that hold the pitch: skipped. S1 has E4 (64): -5 lands on S1? it holds 64 - skipped to... the start
    enc(0, -5);
    printf("  POS -5 into S1 (holds E4): S1 n %d, S6 n %d\n", bd()->step[0].n, bd()->step[5].n);
    CHECK(bd()->step[5].n == 1 && bd()->step[0].n == 3);   // blocked: S1 is the start and holds E4, nothing moved
    enc(0, -4);   // S2: free
    CHECK(bd()->step[5].n == 0 && bd()->step[1].n == 1 && bd()->step[1].note[0].pitch == 64);
    at(1, 64, "POS -4");
    // PTCH within a chord skips a pitch the step holds: S1's C4 +4 -> E4 is there -> F4
    data(-1), at(0, 67, "-1: S1's G4");
    data(-2), at(0, 60, "-2: S1's C4");
    enc(3, 4);
    printf("  S1 C4 +4 (E4 there): %d %d %d\n", bd()->step[0].note[0].pitch, bd()->step[0].note[1].pitch, bd()->step[0].note[2].pitch);
    CHECK(find(bd()->step[0], 65) >= 0 && find(bd()->step[0], 64) >= 0 && find(bd()->step[0], 60) < 0);
    at(0, 65, "PTCH +4 skipped E4");
    enc(3, -5);   // back to C4 (E4 skipped on the way: 65 - 5 = 60)
    CHECK(find(bd()->step[0], 60) >= 0);
  }

  // ---- the selection: the DATA push toggles, edits are relative on every selected note, ranges shown
  {
    data(-9), at(0, 60, "the first");
    press(kPush);
    data(1), at(0, 64, "+1");
    press(kPush);
    CHECK(seq::note_edit().sel == 2 && seq::note_selected(0, 60) && seq::note_selected(0, 64) && !seq::note_selected(0, 67));
    seq::Step& s = bd()->step[0];
    int g = find(s, 67), v67 = s.note[g].vel;
    enc(4, -20);
    CHECK(s.note[find(s, 60)].vel == 80 && s.note[find(s, 64)].vel == 80 && s.note[find(s, 67)].vel == v67);
    enc(3, 12);   // both up an octave: C5 E5
    CHECK(find(s, 72) >= 0 && find(s, 76) >= 0 && find(s, 67) >= 0 && find(s, 60) < 0);
    CHECK(seq::note_selected(0, 72) && seq::note_selected(0, 76));
    at(0, 76, "the highlight moved with its note");
    data(-1), at(0, 72, "-1 (C5: G4 67 < C5 72)");
    press(kPush);   // C5 out of the selection
    CHECK(seq::note_edit().sel == 1);
    press(kPush);   // in again
    enc(3, -12);
    // S9's F4 into the selection too: then a range of pitches and steps
    data(2, true);
    press(kPush);
    CHECK(seq::note_edit().sel == 3);
    frames();
    dump_texts();
    CHECK(shown("SEL 3") && shown(".."));
    png("noteedit_5_selection.png");
    {   // the LEDs: the highlighted step amber, steps with selected notes green
      uint8_t c[16];
      CHECK(seq::leds(c));
      printf("  LEDs: S1 %d S9 %d S17 %d\n", c[0], c[8], c[16 % 16]);
      CHECK(c[8] == 63 && c[0] == 27);
    }
    key(kFunc, true), press(kPush), key(kFunc, false);   // FUNC + push: all
    CHECK(seq::note_edit().sel == seq::note_edit().count);
    key(kFunc, true), press(kPush), key(kFunc, false);   // again: none
    CHECK(seq::note_edit().sel == 0);
    key(kFunc, true), press(kPush), key(kFunc, false);
    press(kNo);   // NO: the selection cleared first, NOTE EDIT stays
    CHECK(seq::note_edit().on && seq::note_edit().sel == 0);
  }

  // ---- ux1027: DOWN the poly track's menu (NOTE EDIT under it; YES alone is the Rytm's now); NO there back, NO again
  // leaves NOTE EDIT. updown1029: FUNC + DOWN; DOWN alone not in NOTE EDIT either
  {
    press(kYes);   // (YES is not caught here: were one to come, nothing)
    CHECK(seq::view().prompt == 0 && seq::note_edit().on);
    press(kDown);
    CHECK(seq::view().prompt == 0 && seq::note_edit().on);
    key(kFunc, true), press(kDown), key(kFunc, false);
    CHECK(seq::view().prompt == seq::kPromptMenu);
    press(kNo);
    CHECK(seq::view().prompt == 0 && seq::note_edit().on);
    press(kNo);
    CHECK(!seq::note_edit().on);
    frames();
    CHECK(shown("SIDECAR SEQ"));
    // back in: where it was (on the page shown)
    data(1), at(8, 65, "again: the last highlighted");
    press(kTrig);   // TRIG: leaves, on to DEFAULTS
    CHECK(!seq::note_edit().on && seq::view().tpage == seq::kTvDefaults);
    data(1), at(8, 65, "DATA on DEFAULTS: NOTE EDIT on the notes");
    CHECK(seq::view().tpage == seq::kTvNotes);
  }

  // ---- the highlighted note vanishes (the touchscreen, an undo): the next turn finds the next one at or after it
  {
    seq::remove_pitch(bd()->step[8], 65);
    seq::touched();
    data(1), at(16, 70, "S9 gone: the next note at or after it");
    // PAGE to page 1: the highlight is not on the page shown - the next turn lands on its first note
    press(0x84);
    printf("  PAGE: page %d\n", seq::view().page + 1);
    data(1), at(0, 60, "PAGE then DATA: the page's first note");
  }

  // ---- another pattern: the same (step, pitch) looked up there, the selection cleared
  {
    press(kPush);
    CHECK(seq::note_edit().sel == 1);
    seq::Pattern& p = seq::edit_pattern();
    int was = p.index;
    // the Rytm's pattern change to A02 (stopped, 2D 02), BD made poly there with a note of its own
    auto m2 = msg(0x2D, {2, 1, 0x7F});
    engine_sysex(m2.data(), (int)m2.size());
    while (seq::idle()) {}
    printf("  pattern %d -> %d\n", was, seq::edit_pattern().index);
    CHECK(seq::edit_pattern().index == 1);
    {
      cmd("seq=track:1:on");
      seq::Track* t = seq::edit_track(0, true);
      CHECK(t);
      seq::add_note(t->step[3], nt(48));
      seq::touched();
      data(1);
      seq::NoteEdit n = seq::note_edit();
      printf("  A02: highlight S%d %d sel %d\n", n.idx + 1, n.pitch, n.sel);
      CHECK(n.on && n.idx == 3 && n.pitch == 48 && n.sel == 0);
    }
  }
  seq::edit_pattern();
  {   // back to A01 (BD poly) for the review's scenarios
    auto m1 = msg(0x2D, {2, 0, 0x7F});
    engine_sysex(m1.data(), (int)m1.size());
    while (seq::idle()) {}
    CHECK(seq::edit_pattern().index == 0 && bd());
  }
  fix1028_window();
  review_round1();
  review_round3();
  review_round2();
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_noteedit: OK\n");
  return 0;
}
