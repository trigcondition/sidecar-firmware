// core/tests/test_seqtop.cpp - the sequencer's "top 8" of docs/SEQ-IDEAS.md (seqtop), host side: P2 per-track speed
// ratios, P1 play directions, M1 scale and key, R1 / R1b seeded random notes and the modulatable SEED, V1 conditions per
// lock, P4 / M4 ratchets and the per-step arp, X1 slides between locks, H1 the FX column with its jumps. Each through
// the real planner (the Rytm's ticks simulated as in test_seqx), each repeatable over replans (the keyed RNG: a step
// planned twice gives the same answer), each in the .sqp v4 file only when used.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "timing.h"
#include "engine.h"
#include "machines.h"
#include "pool/pool.h"
#include "seq/rng.h"
#include "seq/seq.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;

// ---- the Rytm side, as test_seqx's: one clock, each tick arriving 700 host frames after it fired
struct Msg {
  int64_t at;
  std::vector<uint8_t> m;
};
static std::vector<Msg> g_wire;
static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> blk4(int64_t frame) {
  uint32_t b = (uint32_t)(frame / 32) & ((1u << 28) - 1);
  return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)};
}
static void deliver(int64_t h) {
  for (size_t i = 0; i < g_wire.size();) {
    if (g_wire[i].at <= h) {
      engine_sysex(g_wire[i].m.data(), (int)g_wire[i].m.size());
      g_wire.erase(g_wire.begin() + (long)i);
    } else {
      ++i;
    }
  }
}
static void direct_state(std::vector<uint8_t> body) {
  auto m = msg(0x2D, body);
  engine_sysex(m.data(), (int)m.size());
}
static void tempo(int bpm) {
  int t = bpm * 120;
  direct_state({7, (uint8_t)(t >> 14), (uint8_t)(t >> 7 & 0x7F), (uint8_t)(t & 0x7F)});
}
// a note that started: its pitch, velocity, the Rytm step position (since PLAY) it started at, its planned length
struct Hit {
  int pitch, vel;
  double pos;
  double len;   // Rytm steps (its note-off's frame - its note-on's)
};
static std::vector<Hit> g_hits;
static int64_t g_t0 = 0;
static double g_period = 6000;
static void (*g_each)() = nullptr;
static int g_late_before = 0;
static double g_pb_ns = 0;   // the time in seq::pre_block (the planner and its events), the blocks counted
static long g_pb_n = 0;
static double g_pb_max = 0;   // the longest pre_block
static void block() {
  int64_t f[1024];
  uint8_t on[1024], vo[1024], no[1024];
  int n = seq::pending(f, on, vo, no, 1024);
  uint32_t before = seq::stats().notes_on;
  int64_t h = (int64_t)seq::frames();
  uint64_t t0 = timing_cpu_ns();   // (the thread's CPU: a block preempted on a loaded Mac is not slow - tests/timing.h)
  seq::pre_block();
  double ns = (double)(timing_cpu_ns() - t0);
  g_pb_ns += ns;
  if (ns > g_pb_max) g_pb_max = ns;
  ++g_pb_n;
  if (g_each) g_each();
  g_us += 250;
  host_fake_time(true, g_us);
  if (seq::stats().notes_on == before) return;
  int64_t f2[1024];
  uint8_t on2[1024], vo2[1024], no2[1024];
  int n2 = seq::pending(f2, on2, vo2, no2, 1024);
  for (int i = 0; i < n; ++i) {
    if (on[i] != 1 || f[i] > h + kBlock) continue;
    bool still = false;
    for (int j = 0; j < n2 && !still; ++j) still = on2[j] == 1 && f2[j] == f[i] && no2[j] == no[i];
    if (still) continue;
    double off = -1;   // its note-off: the next pending off of that note after it
    for (int j = 0; j < n2; ++j)
      if (on2[j] == 0 && no2[j] == no[i] && f2[j] > f[i] && (off < 0 || f2[j] < off)) off = (double)f2[j];
    g_hits.push_back({no[i], 0, (double)(f[i] + 192 - 700 - g_t0) / g_period, off < 0 ? -1 : (off - (double)f[i]) / g_period});
  }
}
static int g_track = 0;
static int g_len = 16;   // the Rytm track's length in its ticks
static void run_until(int64_t& next_k, int64_t until) {
  while ((int64_t)seq::frames() < until) {
    int64_t h = (int64_t)seq::frames();
    for (;;) {
      int64_t tf = g_t0 + (int64_t)llround((double)next_k * g_period);
      if (tf > h + 2) break;
      auto b = blk4(tf);
      b.insert(b.end(), {1, (uint8_t)g_track, (uint8_t)(next_k % g_len), (uint8_t)g_len, 2});
      g_wire.push_back({tf + 700, msg(0x2C, b)});
      ++next_k;
    }
    deliver(h);
    block();
  }
}
static std::string cmd(const char* c) {
  char b[1200];
  CHECK(seq::command(c, b, sizeof b));
  return b;
}
static std::string cmdf(const char* fmt, int a, int b = 0, int c = 0, int d = 0) {
  char l[200];
  snprintf(l, sizeof l, fmt, a, b, c, d);
  return cmd(l);
}
// One PLAY of `steps` Rytm steps from now: the notes that started, sorted by position then pitch
static std::vector<Hit> play_steps(double steps, void (*each)() = nullptr) {
  g_wire.clear();
  g_hits.clear();
  int64_t now = (int64_t)seq::frames();
  g_t0 = (now / 32 + 200) * 32;
  auto b = std::vector<uint8_t>{1, 1};
  auto x = blk4(g_t0);
  b.insert(b.end(), x.begin(), x.end());
  g_wire.push_back({g_t0 + 700, msg(0x2D, b)});
  int64_t k = 0;
  g_each = each;
  g_late_before = (int)seq::stats().late;
  run_until(k, g_t0 + (int64_t)(steps * g_period) - (int64_t)(g_period / 2));
  g_each = nullptr;
  auto s = blk4((int64_t)seq::frames() - 700);   // (stamped as the Rytm would: its frame, 700 frames before it arrives)
  std::vector<uint8_t> stop = {1, 0};
  stop.insert(stop.end(), s.begin(), s.end());
  direct_state(stop);
  for (int i = 0; i < 200; ++i) block();
  auto res = g_hits;
  std::sort(res.begin(), res.end(), [](const Hit& a, const Hit& b) {
    return std::fabs(a.pos - b.pos) > 0.01 ? a.pos < b.pos : a.pitch < b.pitch;
  });
  // (a replan's cancelled note-ons can show the same note twice: only which (pitch, position) played counts)
  res.erase(std::unique(res.begin(), res.end(), [](const Hit& a, const Hit& b) {
              return a.pitch == b.pitch && std::fabs(a.pos - b.pos) < 0.01;
            }),
            res.end());
  return res;
}
// a note at Rytm position want (the first step after PLAY starts on its tick's arrival: up to 700 frames late)
static bool at_pos(double pos, double want) { return std::fabs(pos - want) < (want < 0.5 ? 0.15 : 0.02); }
static bool same(const std::vector<Hit>& a, const std::vector<Hit>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].pitch != b[i].pitch || std::fabs(a[i].pos - b[i].pos) > (a[i].pos < 0.5 ? 0.15 : 0.01)) return false;
  return true;
}
static int g_replans = 0;
static void replan_often() {   // an edit's replan every 7 blocks (the swing set to what it is)
  static int k = 0;
  if (++k % 7 == 0) cmdf("seq=swing:%d:50", g_track + 1), ++g_replans;
}
// the track: 16 steps, step s a note 48 + s (velocity 100, a quarter step long)
static seq::Track* fresh_track(int r = 0) {
  seq::Track* t = seq::edit_track(r, true);
  CHECK(t);
  seq::track_init(*t, r);   // (every seqtop setting at its default)
  t->on = 1;                // (per pattern poly: still the pattern's poly track - track_init clears the flag too)
  t->length = 16;
  for (int s = 0; s < 16; ++s) seq::add_note(t->step[s], {(uint8_t)(48 + s), 100, 24, 0, 0});
  seq::touched();
  return t;
}

// ---- seq1112: LENGTH grows -> the pages copied as the Rytm does; shortening hides, it does not delete
static void mark_step(seq::Step& st, uint8_t pitch, int cond, int lockp, int lockv, int lockcond) {
  seq::add_note(st, {pitch, 100, 24, 0, 0});
  seq::cond_set(st, cond);
  st.prob = 73;
  CHECK(seq::set_lock(st, lockp, lockv));
  for (int i = 0; i < st.nl; ++i)
    if (st.lock[i].param == lockp) st.lock[i].cond = (uint8_t)lockcond;
  st.slide |= 1u << (lockp & 31);
  st.rt.cnt = 3, st.rt.rate = 2, st.rt.mode = seq::kRtUp, st.rt.oct = 1, st.rt.gate = 40;
  st.xf |= seq::kXfRnd;
  st.fx[0].cmd = seq::kFxJmp, st.fx[0].val = 5;
  st.strum = 4, st.chd = 2, st.inv = 1;
}
static bool same_step(const seq::Step& a, const seq::Step& b) { return memcmp(&a, &b, sizeof a) == 0; }

static void test_grow_pages() {
  seq::Track* t = fresh_track();
  // page 1 carries everything a step can carry, so a copy that drops any of it is caught
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  mark_step(t->step[0], 60, seq::kCondFill, 3, 1234, seq::kCond1st);
  mark_step(t->step[7], 67, seq::kCondAB, 24 + 2, 99, seq::kCondNotFill);   // kLockIns: an INSERT lock
  mark_step(t->step[15], 75, seq::kCondNotPre, 16 + 1, 512, seq::kCondAB);   // kLockFx: a send FX lock
  t->length = 16;
  seq::touched();
  seq::Step p1[16];
  memcpy(p1, t->step, sizeof p1);

  // 1 -> 2 pages: page 1 duplicated, every field with it
  cmdf("seq=len:%d:32", g_track + 1);
  CHECK(t->length == 32);
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[i], p1[i]));             // page 1 untouched
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[16 + i], p1[i]));        // page 2 == page 1
  CHECK(seq::cond_of(t->step[16]) == seq::kCondFill && t->step[16].prob == 73);
  CHECK(seq::lock_of(t->step[16], 3) == 1234 && t->step[16].lock[0].cond == seq::kCond1st);
  CHECK(seq::lock_of(t->step[16 + 7], 24 + 2) == 99 && seq::lock_of(t->step[16 + 15], 16 + 1) == 512);
  CHECK((t->step[16].slide >> 3 & 1) && seq::rt_on(t->step[16].rt) && (t->step[16].xf & seq::kXfRnd));
  CHECK(t->step[16].fx[0].cmd == seq::kFxJmp && t->step[16].fx[0].val == 5);

  // page 2 made its own, so the two pages differ
  t->step[23].note[0].pitch = 42;                                   // page 1's step 7, edited on page 2
  seq::cond_set(t->step[23], seq::kCondNot1st);
  CHECK(seq::set_lock(t->step[23], 5, 777));
  mark_step(t->step[20], 55, seq::kCondFill, 9, 321, seq::kCond1st);   // a step page 1 has not got
  seq::touched();
  seq::Step p2[16];
  memcpy(p2, t->step + 16, sizeof p2);

  // 2 -> 4 pages: pages 1-2 into 3-4
  cmdf("seq=len:%d:64", g_track + 1);
  CHECK(t->length == 64);
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[32 + i], p1[i]));
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[48 + i], p2[i]));

  // shortening HIDES: the steps past the length stay in the pattern untouched
  cmdf("seq=len:%d:16", g_track + 1);
  CHECK(t->length == 16);
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[16 + i], p2[i]));
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[48 + i], p2[i]));
  CHECK(seq::used_steps(*t) == 3 + 4 + 3 + 4);   // pages 1 and 3 three steps, 2 and 4 four: all still there

  // ... and growing back GIVES THEM BACK, locks and conditions intact - it does not re-copy page 1 over them
  cmdf("seq=len:%d:64", g_track + 1);
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[16 + i], p2[i]));
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[32 + i], p1[i]));
  for (int i = 0; i < 16; ++i) CHECK(same_step(t->step[48 + i], p2[i]));
  CHECK(seq::lock_of(t->step[16 + 7], 24 + 2) == 99 && t->step[16].lock[0].cond == seq::kCond1st);
  CHECK(seq::lock_of(t->step[23], 5) == 777 && seq::cond_of(t->step[23]) == seq::kCondNot1st);
  CHECK(t->step[23].note[0].pitch == 42 && t->step[20].note[0].pitch == 55);
  CHECK(seq::lock_of(t->step[20], 9) == 321 && seq::rt_on(t->step[20].rt));

  // the hidden steps survive the file too (pack writes every step index, not just the ones in range)
  cmdf("seq=len:%d:16", g_track + 1);
  static uint8_t buf[1 << 17];
  int n = seq::pack(seq::edit_pattern(), buf, sizeof buf);
  CHECK(n > 0);
  static seq::Pattern q;
  CHECK(seq::unpack(q, seq::edit_pattern().index, buf, n));
  const seq::Track* u = seq::track_of(q, g_track);
  CHECK(u && u->length == 16);
  for (int i = 0; i < 16; ++i) CHECK(same_step(u->step[48 + i], p2[i]));

  // a part-page grow fills only the steps it reveals, from step i % the old length
  seq::clear_steps(*t, 0, seq::kMaxSteps);
  for (int i = 0; i < 20; ++i) seq::add_note(t->step[i], {(uint8_t)(40 + i), 100, 24, 0, 0});
  t->length = 20;
  seq::touched();
  cmdf("seq=len:%d:32", g_track + 1);
  for (int i = 20; i < 32; ++i) CHECK(t->step[i].note[0].pitch == 40 + (i % 20));
  CHECK(t->step[19].note[0].pitch == 59 && t->step[32].n == 0);

  // shortening copies nothing, and an unchanged length copies nothing
  cmdf("seq=len:%d:16", g_track + 1);
  CHECK(t->step[16].note[0].pitch == 56);
  cmdf("seq=len:%d:16", g_track + 1);
  CHECK(t->step[16].note[0].pitch == 56);

  // grow_pages itself: 0 or a shrink is a no-op, and kMaxSteps is the ceiling
  seq::Track z = *t;
  seq::grow_pages(z, 0, 32);
  seq::grow_pages(z, 32, 16);
  CHECK(memcmp(&z, t, sizeof z) == 0);
  seq::grow_pages(z, 16, 1000);
  CHECK(seq::step_used(z.step[seq::kMaxSteps - 1]));

  fresh_track();
}

// ---- P2: per-track speed ratios
static void test_speed() {
  CHECK(seq::speed_ratio(seq::kSpd3_4).num == 3 && seq::speed_ratio(seq::kSpd3_4).den == 4);
  CHECK(seq::speed_ratio(seq::kSpdFollow).num == 1 && seq::speed_ratio(99).num == 1);
  CHECK(seq::speed_parse("2/3") == seq::kSpd2_3 && seq::speed_parse("follow") == 0 && seq::speed_parse("9/7") < 0);
  // the knob: Elektron's set on a plain turn, every ratio with FUNC (slower range: Q8)
  CHECK(seq::speed_step(seq::kSpdFollow, -1, false) == seq::kSpd3_4 && seq::speed_step(seq::kSpdFollow, -1, true) == seq::kSpd7_8);
  CHECK(seq::speed_step(seq::kSpdFollow, 1, false) == seq::kSpd3_2 && seq::speed_step(seq::kSpdFollow, 1, true) == seq::kSpd5_4);
  CHECK(seq::speed_step(seq::kSpd2, 5, false) == seq::kSpd2 && seq::speed_step(seq::kSpd1_8, -3, true) == seq::kSpd1_8);
  fresh_track();
  auto base = play_steps(32);
  CHECK(base.size() == 32);
  for (size_t i = 0; i < base.size(); ++i) CHECK(base[i].pitch == 48 + (int)i % 16 && at_pos(base[i].pos, (double)i));
  struct Case {
    const char* name;
    int num, den;
  } cases[] = {{"1/2", 1, 2}, {"2", 2, 1}, {"3/4", 3, 4}, {"3/2", 3, 2}, {"2/3", 2, 3}, {"5/4", 5, 4}, {"7/8", 7, 8}, {"1/8", 1, 8}};
  for (const Case& c : cases) {
    char l[40];
    snprintf(l, sizeof l, "seq=spd:1:%s", c.name);
    CHECK(cmd(l).find(c.name) != std::string::npos);
    auto h = play_steps(32);
    int want = (int)std::ceil(32.0 * c.num / c.den - 0.5 * c.num / c.den - 1e-9);   // steps starting before 31.5
    printf("  speed %s: %zu notes in 32 Rytm steps (want %d)\n", c.name, h.size(), want);
    CHECK((int)h.size() == want);
    if (getenv("SQT_DEBUG")) for (auto& x : h) printf("    %d @ %.3f len %.3f\n", x.pitch, x.pos, x.len);
    for (size_t v = 0; v < h.size(); ++v) {   // Sidecar step v at the Rytm's position v x den / num, its own pitch
      CHECK(h[v].pitch == 48 + (int)v % 16);
      CHECK(at_pos(h[v].pos, (double)v * c.den / c.num));
      CHECK(std::fabs(h[v].len - 0.25 * c.den / c.num) < 0.02);   // a quarter of the track's own step
    }
    // replans (an edit every few blocks): the same notes at the same places
    g_replans = 0;
    auto hr = play_steps(32, replan_often);
    if (!same(hr, h)) for (auto& x : hr) printf("    replanned %d @ %.3f\n", x.pitch, x.pos);
    CHECK(same(hr, h) && g_replans > 20);
  }
  // 2x at 300 BPM: two Sidecar steps between two Rytm ticks, nothing late but the first step after PLAY
  cmd("seq=spd:1:2");
  tempo(300);
  double keep = g_period;
  g_period = 2400;
  for (int i = 0; i < 400; ++i) block();
  auto fast = play_steps(64);
  int late = (int)seq::stats().late - g_late_before;
  printf("  speed 2 at 300 BPM: %zu notes in 64 Rytm steps, %d late\n", fast.size(), late);
  CHECK(fast.size() == 127 && late <= 1);
  g_period = keep;
  tempo(120);
  for (int i = 0; i < 400; ++i) block();
  cmd("seq=spd:1:follow");
  CHECK(same(play_steps(32), base));
}

// ---- the switch prediction across speeds: A01 at 1/2, A02 at 2 queued; the switch at the master length (16)
static void test_speed_switch() {
  direct_state({2, 0, 0x7F});
  while (seq::idle()) {}
  fresh_track();
  cmd("seq=spd:1:1/2");
  direct_state({2, 1, 0x7F});
  while (seq::idle()) {}
  seq::Track* b = fresh_track();
  for (int s = 0; s < 16; ++s) b->step[s].note[0].pitch = (uint8_t)(72 + s);
  cmd("seq=spd:1:2");
  for (int i = 0; i < 8; ++i) seq::tick((uint32_t)(20000 + i * 300)), seq::idle();
  direct_state({2, 0, 0x7F});
  while (seq::idle()) {}
  direct_state({4, 0x7F, 16, 0, 2, 0, 16, 0});   // the pattern's SCALE: length 16, speed 1x, normal mode
  uint32_t ok0 = seq::stats().predict_ok;
  static int queued_at;
  queued_at = 0;
  auto h = play_steps(24, [] {
    double pos = (double)((int64_t)seq::frames() - g_t0) / g_period;
    if (!queued_at && pos > 8.2) {
      queued_at = 1;
      direct_state({2, 0, 1});   // A02 queued at step 8
      while (seq::idle()) {}
    }
    if (queued_at == 1 && (int64_t)seq::frames() >= g_t0 + (int64_t)llround(16 * g_period) + 700) {
      queued_at = 2;   // the Rytm's 2D 03 at the switch block, arriving 700 frames later
      std::vector<uint8_t> x = {3, 0, 1};
      auto bb = blk4(g_t0 + (int64_t)llround(16 * g_period));
      x.insert(x.end(), bb.begin(), bb.end());
      direct_state(x);
    }
  });
  int a01 = 0, a02 = 0;
  if (getenv("SQT_DEBUG")) for (auto& x : h) printf("    sw %d @ %.3f\n", x.pitch, x.pos);
  for (const Hit& x : h) {
    if (x.pos < 15.9) {
      CHECK(x.pitch < 72 && at_pos(x.pos, 2.0 * (x.pitch - 48)));   // A01 at 1/2: every second tick
      ++a01;
    } else {
      CHECK(x.pitch >= 72);   // A02 at 2 from the switch: its step k at 16 + k / 2
      CHECK(at_pos(x.pos, 16 + (x.pitch - 72) / 2.0));
      ++a02;
    }
  }
  printf("  switch 1/2 -> 2: A01 %d notes, A02 %d notes, predicted ok %u\n", a01, a02, seq::stats().predict_ok - ok0);
  CHECK(a01 == 8 && a02 == 15 && seq::stats().predict_ok == ok0 + 1);
  direct_state({2, 0, 0x7F});
  while (seq::idle()) {}
  cmd("seq=spd:1:follow");
}

// ---- P1: play directions. The step map is pure: the same count, the same step (and FWD exactly as before)
static void test_dir_map() {
  for (int len = 1; len <= 20; ++len)
    for (int64_t a = 0; a < 200; ++a) {
      seq::StepAt f = seq::step_for(len, a, seq::kDirFwd, 41, 0, false), o = seq::step_for(len, a);
      CHECK(f.index == o.index && f.loop == o.loop);
      seq::StepAt rv = seq::step_for(len, a, seq::kDirRev, 41, 0, false);
      CHECK(rv.index == len - 1 - (int)(a % len) && rv.loop == a / len);
      for (int d = 0; d < seq::kDirs; ++d) {
        seq::StepAt x = seq::step_for(len, a, d, 7, 2, false);
        CHECK(x.index >= 0 && x.index < len);
        seq::StepAt y = seq::step_for(len, a, d, 7, 2, false);
        CHECK(x.index == y.index && x.loop == y.loop);   // repeatable
      }
    }
  auto seqof = [](int len, int dir, int n, uint32_t seed = 41, bool same_ = false) {
    std::vector<int> v;
    for (int a = 0; a < n; ++a) v.push_back(seq::step_for(len, a, dir, seed, 0, same_).index);
    return v;
  };
  CHECK((seqof(4, seq::kDirPend, 13) == std::vector<int>{0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1, 0}));
  CHECK((seqof(4, seq::kDirPing, 10) == std::vector<int>{0, 1, 2, 3, 3, 2, 1, 0, 0, 1}));
  CHECK((seqof(1, seq::kDirPend, 3) == std::vector<int>{0, 0, 0}) && (seqof(2, seq::kDirPend, 4) == std::vector<int>{0, 1, 0, 1}));
  CHECK(seq::step_for(4, 6, seq::kDirPend, 0, 0, false).loop == 1 && seq::step_for(4, 7, seq::kDirPing, 0, 0, false).loop == 0);
  // RND: every step reached, the loops differ, the same every loop with "same"; another seed, another order
  auto rnd = seqof(16, seq::kDirRnd, 64);
  CHECK(std::set<int>(rnd.begin(), rnd.end()).size() > 10);
  CHECK(!std::equal(rnd.begin(), rnd.begin() + 16, rnd.begin() + 16));
  auto rs = seqof(16, seq::kDirRnd, 32, 41, true);
  CHECK(std::equal(rs.begin(), rs.begin() + 16, rs.begin() + 16));
  CHECK(seqof(16, seq::kDirRnd, 16, 42) != seqof(16, seq::kDirRnd, 16, 41));
  // BROWN: step 0 at each loop's start, mostly +1 (a walk: steps of -1..+2 mod len)
  auto br = seqof(16, seq::kDirBrown, 160);
  int up = 0;
  for (int a = 0; a < 160; ++a) {
    if (a % 16 == 0) {
      CHECK(br[(size_t)a] == 0);
      continue;
    }
    int d = ((br[(size_t)a] - br[(size_t)a - 1]) % 16 + 16) % 16;
    CHECK(d == 0 || d == 1 || d == 2 || d == 15);
    up += d == 1;
  }
  printf("  BROWN: %d of 150 moves +1\n", up);
  CHECK(up > 55 && up < 95);
}
static void test_dir_planner() {
  fresh_track();
  cmd("seq=seed:41");
  auto pitches = [](const std::vector<Hit>& h) {
    std::vector<int> v;
    for (const Hit& x : h) v.push_back(x.pitch - 48);
    return v;
  };
  CHECK(cmd("seq=dir:1:rev").find("REV") != std::string::npos);
  auto rv = play_steps(16);
  CHECK(rv.size() == 16);
  for (size_t i = 0; i < rv.size(); ++i) CHECK(rv[i].pitch == 63 - (int)i && at_pos(rv[i].pos, (double)i));   // the time stays
  cmd("seq=dir:1:pend");
  auto pe = pitches(play_steps(32));
  std::vector<int> want;
  for (int a = 0; a < 32; ++a) want.push_back(seq::step_for(16, a, seq::kDirPend, 0, 0, false).index);
  CHECK(pe == want);
  // RND / BROWN: the keyed RNG, so replans and a second PLAY agree (fixed seed 41)
  for (const char* d : {"rnd", "brown"}) {
    char l[32];
    snprintf(l, sizeof l, "seq=dir:1:%s", d);
    cmd(l);
    auto a = play_steps(32);
    g_replans = 0;
    CHECK(same(play_steps(32, replan_often), a) && g_replans > 20);
    CHECK(same(play_steps(32), a));
    std::vector<int> w;
    int dd = strcmp(d, "rnd") ? seq::kDirBrown : seq::kDirRnd;
    for (int k = 0; k < 32; ++k) w.push_back(seq::step_for(16, k, dd, 41, 0, false).index);
    CHECK(pitches(a) == w);
    printf("  %s with seed 41: the same over replans and a second PLAY\n", d);
  }
  // with speed: the direction walks the track's own steps (2x: 32 steps in 16 ticks, the same order)
  cmd("seq=spd:1:2");
  auto r2 = pitches(play_steps(16));
  std::vector<int> w2;
  for (int k = 0; k < (int)r2.size(); ++k) w2.push_back(seq::step_for(16, k, seq::kDirBrown, 41, 0, false).index);
  CHECK(r2 == w2 && r2.size() == 31);
  cmd("seq=spd:1:follow");
  // Q3: swing under REV follows the tick (every second tick late), not the step's own parity
  cmd("seq=dir:1:rev");
  cmd("seq=swing:1:66");
  auto sw = play_steps(16);
  for (size_t i = 1; i < sw.size(); ++i) CHECK(at_pos(sw[i].pos, (double)i + (i & 1 ? 0.32 : 0)));
  cmd("seq=swing:1:50");
  // a pattern seed of FREE (the default): RND differs between PLAYs
  cmd("seq=dir:1:rnd");
  cmd("seq=seed:free");
  CHECK(!same(play_steps(32), play_steps(32)));
  cmd("seq=dir:1:fwd");
  cmd("seq=seed:41");
}

// ---- M1: scale and key per pattern
static void test_scale_fns() {
  CHECK(seq::scale_parse("min") == seq::kScaleMin && seq::scale_parse("BLUES") == seq::kScaleBlues && seq::scale_parse("x") < 0);
  CHECK(seq::scale_parse("off") == 0 && seq::scale_parse("4") == seq::kScalePent && seq::scale_parse("99") < 0);
  CHECK(seq::key_parse("F#") == 6 && seq::key_parse("c") == 0 && seq::key_parse("11") == 11 && seq::key_parse("H") < 0);
  for (int sc = 0; sc < seq::kScales; ++sc) {   // every scale holds its key; the masks' sizes
    CHECK(seq::in_scale(60, 0, sc) && seq::in_scale(62 + 12, 2, sc));
    int n = 0;
    for (int k = 0; k < 12; ++k) n += seq::scale_mask(sc) >> k & 1;
    CHECK(n == (sc == 0 ? 12 : sc == seq::kScalePent || sc == seq::kScalePmaj ? 5 : sc == seq::kScaleBlues || sc == seq::kScaleWhole ? 6 : 7));
  }
  // C minor: E -> Eb (a tie: down), F# -> F; A major: C -> C#? (C is between B and C#: down to B)
  CHECK(seq::quantise(64, 0, seq::kScaleMin) == 63 && seq::quantise(66, 0, seq::kScaleMin) == 65);
  CHECK(seq::quantise(60, 9, seq::kScaleMaj) == 59 && seq::quantise(61, 9, seq::kScaleMaj) == 61);
  CHECK(seq::degree_move(60, 0, seq::kScaleMaj, 1) == 62 && seq::degree_move(60, 0, seq::kScaleMaj, 7) == 72);
  CHECK(seq::degree_move(60, 0, seq::kScaleMin, -2) == 56 && seq::degree_move(60, 0, seq::kScaleChr, 3) == 63);
  // diatonic chords: CHD MAJ / MIN on degree 2 of C minor is D dim (D F Ab); 7 on degree 5 of C major is G7
  uint8_t x[8];
  CHECK(seq::diatonic_chord(62, 1, 0, 0, seq::kScaleMin, x) == 3 && x[0] == 62 && x[1] == 65 && x[2] == 68);
  CHECK(seq::diatonic_chord(67, 5, 0, 0, seq::kScaleMaj, x) == 4 && x[0] == 67 && x[1] == 71 && x[2] == 74 && x[3] == 77);
  CHECK(seq::diatonic_chord(62, 1, 1, 0, seq::kScaleMin, x) == 3 && x[0] == 65 && x[1] == 68 && x[2] == 74);   // INV 1
  CHECK(seq::diatonic_chord(60, 2, 0, 0, seq::kScaleChr, x) == 3 && x[1] == 63);   // CHR: poly.h's MIN
}
static void test_scale_planner() {
  seq::Track* t = fresh_track();
  for (int s = 0; s < 16; ++s) t->step[s].note[0].pitch = (uint8_t)(60 + s);   // C4 .. D#5, chromatic
  seq::touched();
  // QNT OFF / EDIT: what plays is what is written (an edit aid only)
  CHECK(cmd("seq=key:D:dor:edit").find("D DOR, QNT EDIT") != std::string::npos);
  auto e = play_steps(16);
  for (size_t i = 0; i < e.size(); ++i) CHECK(e[i].pitch == 60 + (int)i);
  // QNT PLAY: every note into D dorian when it plays (the steps keep their notes)
  cmd("seq=qnt:play");
  auto q = play_steps(16);
  CHECK(q.size() == 16);
  for (size_t i = 0; i < q.size(); ++i) CHECK(q[i].pitch == seq::quantise(60 + (int)i, 2, seq::kScaleDor) && seq::in_scale(q[i].pitch, 2, seq::kScaleDor));
  CHECK(seq::edit_track(0, false)->step[1].note[0].pitch == 61);
  // the NOTE knob under a held trig moves by degree (FUNC: by semitone); CHD builds a diatonic chord
  seq::view_set(0, 0, 1, false);
  CHECK(seq::ui_enc(0, 1, false, true));   // step 2: C#4 (out of D dorian) -> quantised C4 + 1 degree = D4
  CHECK(seq::edit_track(0, false)->step[1].note[0].pitch == 62);
  seq::ui_enc(0, 2, false, true);          // + 2 degrees: F4
  CHECK(seq::edit_track(0, false)->step[1].note[0].pitch == 65);
  seq::ui_enc(0, 1, true, true);           // FUNC: + 1 semitone (F#4)
  CHECK(seq::edit_track(0, false)->step[1].note[0].pitch == 66);
  seq::view_set(0, 0, 4, false);           // step 5 (E4): CHD MAJ in D dorian on E: E G B (E minor, diatonic)
  seq::ui_enc(4, 1, false, true);
  const seq::Step& s5 = seq::edit_track(0, false)->step[4];
  CHECK(s5.n == 3 && s5.note[0].pitch == 64 && s5.note[1].pitch == 67 && s5.note[2].pitch == 71);
  seq::view_set(0, 0, -1, false);
  // seq=quantise: the written notes into the scale (two on one pitch: one kept)
  seq::Step& s7 = seq::edit_track(0, false)->step[6];
  seq::add_note(s7, {67, 90, 24, 0, 0});   // F#4 and G4 -> F4? (F# between F and G: down to F), G4
  std::string r = cmd("seq=quantise:1");
  printf("  %s\n", r.c_str());
  const seq::Track* u = seq::edit_track(0, false);
  for (int s = 0; s < 16; ++s)
    for (int k = 0; k < u->step[s].n; ++k) CHECK(seq::in_scale(u->step[s].note[k].pitch, 2, seq::kScaleDor));
  CHECK(u->step[6].n == 2 && u->step[6].note[0].pitch == 65 && u->step[6].note[1].pitch == 67);
  // CHR / key off: nothing changes
  cmd("seq=key:off");
  CHECK(seq::edit_pattern().scale == 0 && seq::edit_pattern().qnt == 0);
}

// ---- R1 + R1b: random notes in the scale from the seed; SEED MOD
static void report_fm(int p9) {   // BD's voice report (6-OP FM; P9 is free on it: the SEED MOD slot of these tests)
  int prm[16] = {0, 64, 64, 127, 64, 64, 100, 127, p9, 0, 0, 0, 0, 0, 0, 0};
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(kFm | 0)};
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)prm[k]), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void test_rnd_fn() {
  uint8_t wr[8] = {60, 64, 67, 70, 72, 74, 76, 79}, o[8], o2[8];
  for (int sd = 0; sd < 200; ++sd) {   // distinct, in the range and the scale, repeatable
    seq::rnd_notes(4, (uint32_t)sd, 0, 3, 1, 0, seq::kScaleMin, 48, 2, seq::kWFlat, 0, wr, o);
    seq::rnd_notes(4, (uint32_t)sd, 0, 3, 1, 0, seq::kScaleMin, 48, 2, seq::kWFlat, 0, wr, o2);
    CHECK(!memcmp(o, o2, 4));
    for (int k = 0; k < 4; ++k) {
      CHECK(o[k] >= 48 && o[k] <= 72 && seq::in_scale(o[k], 0, seq::kScaleMin));
      for (int j = 0; j < k; ++j) CHECK(o[j] != o[k]);
    }
  }
  // weights: ROOT makes the key's notes 3x as likely; LOW / HIGH lean to an end; STEP stays near the written note
  auto share = [&](int w, auto pred) {
    int n = 0;
    for (int sd = 0; sd < 4000; ++sd) {
      seq::rnd_notes(1, (uint32_t)sd, 0, 0, 0, 0, seq::kScaleMaj, 48, 2, w, 0, wr, o);
      n += pred(o[0]);
    }
    return n / 40.0;
  };
  double rf = share(seq::kWFlat, [](int p) { return p % 12 == 0; }), rr = share(seq::kWRoot, [](int p) { return p % 12 == 0; });
  double lo = share(seq::kWLow, [](int p) { return p < 60; }), hi = share(seq::kWHigh, [](int p) { return p < 60; });
  double st = share(seq::kWStep, [](int p) { return p >= 56 && p <= 64; });
  printf("  R1 weights: root %.0f%% (flat %.0f%%), low half LOW %.0f%% HIGH %.0f%%, near C4 STEP %.0f%%\n", rr, rf, lo, hi, st);
  CHECK(rf > 15 && rf < 25 && rr > 35 && rr < 55 && lo > 55 && hi < 45 && st > 60);
  // a range smaller than the chord: the written notes fill in
  seq::rnd_notes(4, 9, 0, 0, 0, 0, seq::kScalePent, 60, 1, seq::kWFlat, 0, wr, o);
  for (int k = 0; k < 4; ++k) CHECK(seq::in_scale(o[k], 0, seq::kScalePent));
  seq::rnd_notes(8, 9, 0, 0, 0, 0, seq::kScaleMaj, 60, 1, seq::kWFlat, 0, wr, o);   // C major from C4: 8 notes in 60..72
  std::set<int> u(o, o + 8);
  CHECK(u.size() == 8);
}
static void test_rnd_planner() {
  seq::Track* t = fresh_track();
  cmd("seq=key:C:min:off");
  cmd("seq=seed:41");
  seq::set_chord(t->step[4], 60, 1, 0, 100, 24, 0);   // a 3-note step: 3 distinct random notes
  seq::touched();
  CHECK(cmd("seq=rnd:1:mode=track,low=48,oct=2,wght=flat").find("RND TRACK low 48 oct 2 FLAT") != std::string::npos);
  auto a = play_steps(32);
  CHECK(a.size() == 36);
  std::map<double, std::set<int>> at;
  for (const Hit& x : a) {
    CHECK(x.pitch >= 48 && x.pitch <= 72 && seq::in_scale(x.pitch, 0, seq::kScaleMin));
    at[std::round(x.pos)].insert(x.pitch);
  }
  CHECK(at[4].size() == 3 && at[20].size() == 3);
  std::vector<int> l0, l1;
  for (const Hit& x : a) (x.pos < 15.5 ? l0 : l1).push_back(x.pitch);
  CHECK(l0 != l1);   // the loop index is in the hash
  g_replans = 0;
  CHECK(same(play_steps(32, replan_often), a) && g_replans > 20);   // a replan never re-rolls a note
  CHECK(same(play_steps(32), a));                                   // a fixed seed: the same every PLAY
  cmd("seq=seed:42");
  auto b = play_steps(32);
  CHECK(!same(b, a));
  cmd("seq=seed:41");
  cmd("seq=tseed:1:same:1");   // "same every loop": loop 2 = loop 1
  auto c = play_steps(32);
  std::vector<int> c0, c1;
  for (const Hit& x : c) (x.pos < 15.5 ? c0 : c1).push_back(x.pitch);
  CHECK(c0 == c1);
  cmd("seq=tseed:1:same:0");
  // STEPS: only the flagged steps; the others play as written
  cmd("seq=rnd:1:mode=steps");
  cmd("seq=rndstep:1:3:1");
  auto d = play_steps(16);
  for (const Hit& x : d)
    if (std::lround(x.pos) != 2 && std::lround(x.pos) != 4) CHECK(x.pitch == 48 + (int)std::lround(x.pos));
  int p3 = -1;
  for (const Hit& x : d) p3 = std::lround(x.pos) == 2 ? x.pitch : p3;
  CHECK(p3 == a[2].pitch);   // step 3's draw is the same as in TRACK mode (the same key)
  // print: loop 2's notes written into the steps, the mode off; every loop then plays them
  cmd("seq=rnd:1:mode=track");
  std::string pr = cmd("seq=print:1:2");
  printf("  %s\n", pr.c_str());
  CHECK(pr.find("printed on 16 steps") != std::string::npos && seq::edit_track(0, false)->rnd_mode == 0);
  auto e = play_steps(32);
  CHECK(e.size() == a.size());
  for (size_t i = 0; i < e.size(); ++i) CHECK(e[i].pitch == a[18 + i % 18].pitch);   // loop 2 every loop
  fresh_track();
  cmd("seq=key:off");
}
static void test_smod() {
  seq::Track* t = fresh_track();
  cmd("seq=key:C:maj:off");
  cmd("seq=rnd:1:mode=track,low=48,oct=3,wght=flat");
  cmd("seq=seed:44");
  auto s44 = play_steps(32);
  cmd("seq=seed:41");
  auto s41 = play_steps(32);
  // SEED MOD from P9 (range 16): P9 24 -> 24 / 128 x 16 = 3: seed 41 + 3 plays seed 44's notes
  CHECK(cmd("seq=smod:1:src=9,range=16,quant=loop,aff=notes").find("SMOD P9 range 16 LOOP NOTES") != std::string::npos);
  report_fm(24);
  CHECK(same(play_steps(32), s44));
  report_fm(0);
  CHECK(same(play_steps(32), s41));
  // LOOP: moved mid-loop, the new seed from the next loop on (loop 1 seed 41's, loop 2 seed 44's)
  static int moved;
  moved = 0;
  auto m = play_steps(32, [] {
    if (!moved && (double)((int64_t)seq::frames() - g_t0) / g_period > 6.2) moved = 1, report_fm(24);
  });
  CHECK(m.size() == 32);
  for (size_t i = 0; i < m.size(); ++i) CHECK(m[i].pitch == (i < 16 ? s41 : s44)[i].pitch);
  // replans keep the sampled offsets; the value sitting on a boundary does not flicker (hysteresis)
  report_fm(24);
  g_replans = 0;
  CHECK(same(play_steps(32, replan_often), s44));
  static int wob;
  wob = 0;
  auto h = play_steps(32, [] {   // from the first step on: 2.875 / 3.125, the sampled 3 kept (hysteresis)
    if ((int64_t)seq::frames() > g_t0 + 2000) report_fm(++wob & 1 ? 23 : 25);
  });
  for (size_t i = 0; i < h.size(); ++i) CHECK(h[i].pitch == s44[i].pitch);
  // STEP: a new offset per step (P9 moved at step 6: steps 0..5 seed 41's, 7.. seed 44's)
  cmd("seq=smod:1:quant=step");
  report_fm(0);
  moved = 0;
  auto st = play_steps(16, [] {
    if (!moved && (double)((int64_t)seq::frames() - g_t0) / g_period > 5.2) moved = 1, report_fm(24);
  });
  for (size_t i = 0; i < st.size(); ++i)
    if (i <= 5) CHECK(st[i].pitch == s41[i].pitch);
    else if (i >= 7) CHECK(st[i].pitch == s44[i].pitch);
  // AFFECTS NOTES leaves the coins alone; +CHANCE moves them too
  for (int k = 0; k < 16; ++k) t = seq::edit_track(0, false), t->step[k].prob = 50;
  seq::touched();
  cmd("seq=smod:1:quant=loop,aff=notes");
  cmd("seq=rnd:1:mode=off");
  report_fm(0);
  auto c0 = play_steps(32);
  report_fm(24);
  CHECK(same(play_steps(32), c0));
  cmd("seq=smod:1:aff=chance");
  CHECK(!same(play_steps(32), c0));
  cmd("seq=seed:44");
  cmd("seq=smod:1:src=0");
  auto c44 = play_steps(32);
  cmd("seq=seed:41");
  cmd("seq=smod:1:src=9");
  CHECK(same(play_steps(32), c44));   // 41 + 3 = 44's coins
  cmd("seq=smod:1:src=0,aff=notes");
  report_fm(0);
  fresh_track();
  cmd("seq=key:off");
}

// ---- V1: conditions per lock. P1 of BD's voice sampled in the middle of every step while it plays
static std::vector<float> g_p1;   // by Rytm step position x 4 (quarter steps)
static void probe_p1() {
  double pos = (double)((int64_t)seq::frames() - g_t0) / g_period;
  int q = (int)(pos * 4);
  if (q >= 0) {
    if ((int)g_p1.size() <= q) g_p1.resize((size_t)q + 1, -1);
    g_p1[(size_t)q] = v[0].p[0];
  }
}
static std::vector<float> p1_per_step(double steps, double rate = 1) {   // P1 halfway through each (track) step
  g_p1.clear();
  play_steps(steps, probe_p1);
  std::vector<float> r;
  for (int k = 0; (k + 0.5) / rate * 4 < g_p1.size(); ++k) r.push_back(g_p1[(size_t)((k + 0.5) / rate * 4)]);
  return r;
}
static void test_lock_cond() {
  CHECK(seq::lock_cond_parse("1:4") >= seq::kCondAB && seq::lock_cond_parse("30%") == seq::kConds + 29 && seq::lock_cond_parse("!fill") == seq::kCondNotFill);
  CHECK(seq::lock_cond_parse("pre") < 0 && seq::lock_cond_parse("9:8") < 0 && seq::lock_cond_parse("100%") == 0 && seq::lock_cond_parse("3") < 0);
  char b[12];
  CHECK(!strcmp(seq::lock_cond_text(seq::lock_cond_parse("3:4"), b, sizeof b), "3:4") && !strcmp(seq::lock_cond_text(seq::kConds + 49, b, sizeof b), "50%"));
  fresh_track();
  cmd("seq=seed:41");
  report_fm(0);
  cmd("seq=lock:1:3:1:100");
  cmd("seq=lock:1:7:1:90");
  cmd("seq=lock:1:11:1:80");
  CHECK(cmd("seq=lcond:1:3:1:1:2").find("1:2") != std::string::npos);
  CHECK(cmd("seq=lcond:1:7:1:50%").find("50%") != std::string::npos);
  CHECK(cmd("seq=lcond:1:11:1:fill").find("FILL") != std::string::npos);
  CHECK(cmd("seq=lcond:1:4:1:fill").find("a lock that is there") != std::string::npos);   // (no lock on step 4)
  cmd("seq=lock:1:4:3:20"), cmd("seq=lock:1:4:5:30");   // "all": every lock of the step
  CHECK(cmd("seq=lcond:1:4:all:30%").find("2 locks 30%") != std::string::npos);
  CHECK(cmd("seq=slide:1:4:all:1").find("2 locks slide") != std::string::npos && seq::edit_track(0, false)->step[3].slide == (1u << 2 | 1u << 4));
  cmd("seq=slide:1:4:all:0"), cmd("seq=lock:1:4:3:-1"), cmd("seq=lock:1:4:5:-1");
  auto a = p1_per_step(64);
  CHECK(a.size() >= 64);
  int s7 = 0;
  for (int k = 0; k < 64; ++k) {
    int st = k % 16, loop = k / 16;
    if (st == 2) CHECK(a[(size_t)k] == (loop % 2 == 0 ? 100.f : 0.f));   // 1:2: loops 1 and 3
    else if (st == 6) {
      CHECK(a[(size_t)k] == 90.f || a[(size_t)k] == 0.f);
      s7 += a[(size_t)k] == 90.f;
    }
    else CHECK(a[(size_t)k] == 0.f);   // FILL not held: step 11's lock left out; the others: the base
  }
  printf("  V1: the 50%% lock applied on %d of 4 loops; 1:2 on loops 1 and 3; FILL's not without FILL\n", s7);
  {   // other seeds: the 50 % lock lands on about half the loops
    int hit = 0;
    for (int sd = 1; sd <= 6; ++sd) {
      cmdf("seq=seed:%d", sd);
      auto x = p1_per_step(64);
      for (int k = 6; k < 64; k += 16) hit += x[(size_t)k] == 90.f;
    }
    printf("  V1: the 50%% lock over 6 seeds x 4 loops: %d of 24\n", hit);
    CHECK(hit >= 6 && hit <= 18);
    cmd("seq=seed:41");
  }
  g_replans = 0;
  CHECK(p1_per_step(64) == a);   // the lock's coin is the keyed RNG's: the same every PLAY ...
  {
    g_p1.clear();
    play_steps(64, [] { probe_p1(); replan_often(); });   // ... and over replans
    std::vector<float> x;
    for (int k = 0; (k + 0.5) * 4 < g_p1.size(); ++k) x.push_back(g_p1[(size_t)((k + 0.5) * 4)]);
    CHECK(x == a);
  }
  direct_state({12, 1});   // FILL held: step 11's lock applies
  auto f = p1_per_step(16);
  CHECK(f[10] == 80.f);
  direct_state({12, 0});
  // the step's own condition still first: the step off 1:2 -> no lock at all on its other loops
  cmd("seq=cond:1:7:9");   // 1:2 on the step
  auto g = p1_per_step(32);
  CHECK(g[22] == 0.f);
  cmd("seq=cond:1:7:0");
  report_fm(0);
  fresh_track();
}

// ---- P4 / M4: ratchets, rolls, the per-step arp
static void test_rt_fn() {
  seq::Step s = {};
  seq::set_chord(s, 60, 1, 0, 100, 96, 0);   // C E G, a step long
  uint8_t pit[8] = {60, 64, 67};
  seq::RtNote o[64];
  s.rt = {4, 0, seq::kVcUp, 0, seq::kRtChord, 0, 0};   // 4 x the chord, AUTO: a quarter step apart
  int n = seq::rt_expand(s, pit, 0, 0, false, 1, 0, 0, 0, o, 64);
  CHECK(n == 12);
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 3; ++k) {
      const seq::RtNote& x = o[i * 3 + k];
      CHECK(x.off == 24 * i && x.pitch == pit[k] && x.len == 24);
      if (i) CHECK(x.vel > o[(i - 1) * 3 + k].vel);   // VCRV UP
    }
  CHECK(o[0].vel == 40 && o[9].vel == 100);
  s.rt = {3, seq::kRtRates - 3, seq::kVcFlat, 12, seq::kRtChord, 0, 50};   // 1/4 step apart, +12 per repeat, gate 50 %
  n = seq::rt_expand(s, pit, 0, 0, false, 1, 0, 0, 0, o, 64);
  CHECK(n == 9 && o[3].off == 24 && o[3].pitch == 72 && o[6].pitch == 84 && o[0].len == 12 && o[0].vel == 100);
  s.rt = {6, 0, 0, 0, seq::kRtWalk, 0, 0};   // WALK: the repeats walk the chord, low to high
  n = seq::rt_expand(s, pit, 0, 0, false, 1, 0, 0, 0, o, 64);
  CHECK(n == 6 && o[0].pitch == 60 && o[1].pitch == 64 && o[2].pitch == 67 && o[3].pitch == 60 && o[5].off == 80);
  auto arp = [&](int mode, int oct, int cnt = 0) {
    s.rt = {(uint8_t)cnt, seq::kRtRates - 3, 0, 0, (uint8_t)mode, (uint8_t)oct, 0};
    int m = seq::rt_expand(s, pit, 0, 0, false, 41, 0, 3, 0, o, 64);
    std::vector<int> v;
    for (int i = 0; i < m; ++i) v.push_back(o[i].pitch);
    return v;
  };
  CHECK((arp(seq::kRtUp, 2) == std::vector<int>{60, 64, 67, 72, 76, 79}));
  CHECK((arp(seq::kRtDown, 1) == std::vector<int>{67, 64, 60}));
  CHECK((arp(seq::kRtUpDn, 2) == std::vector<int>{60, 64, 67, 72, 76, 79, 76, 72, 67, 64}));
  CHECK((arp(seq::kRtConv, 2) == std::vector<int>{60, 79, 64, 76, 67, 72}));
  CHECK((arp(seq::kRtUp, 1, 8) == std::vector<int>{60, 64, 67, 60, 64, 67, 60, 64}));   // CNT 8: the pattern again
  uint8_t pp[8] = {67, 60, 64};   // PLAYED: as entered (G C E), not sorted
  seq::Step s2 = s;
  s2.note[0].pitch = 67, s2.note[1].pitch = 60, s2.note[2].pitch = 64;
  s2.rt = {0, seq::kRtRates - 3, 0, 0, seq::kRtPlayed, 1, 0};
  n = seq::rt_expand(s2, pp, 0, 0, false, 41, 0, 3, 0, o, 64);
  CHECK(n == 3 && o[0].pitch == 67 && o[1].pitch == 60 && o[2].pitch == 64);
  auto r1 = arp(seq::kRtRnd, 2, 12), r2 = arp(seq::kRtRnd, 2, 12);
  CHECK(r1 == r2 && r1.size() == 12);
  s.rt = {12, seq::kRtRates - 3, 0, 0, seq::kRtRnd, 2, 0};
  n = seq::rt_expand(s, pit, 0, 0, false, 42, 0, 3, 0, o, 64);
  std::vector<int> r3;
  for (int i = 0; i < n; ++i) r3.push_back(o[i].pitch);
  CHECK(r3 != r1);   // another seed, another order
  // PCRV 0 with the scale on: an out-of-scale note stays as written (the e2e found it quantised)
  s.rt = {3, 0, 0, 0, seq::kRtWalk, 0, 0};
  s.n = 1;
  uint8_t p61[8] = {61};
  n = seq::rt_expand(s, p61, 0, seq::kScaleMaj, true, 1, 0, 0, 0, o, 64);
  CHECK(n == 3 && o[0].pitch == 61 && o[2].pitch == 61);
  // PCRV in scale degrees (C major): + 1 degree per repeat from C: C D E
  s.rt = {3, 0, 0, 1, seq::kRtWalk, 0, 0};
  s.n = 1;
  n = seq::rt_expand(s, pit, 0, seq::kScaleMaj, true, 1, 0, 0, 0, o, 64);
  CHECK(n == 3 && o[0].pitch == 60 && o[1].pitch == 62 && o[2].pitch == 64);
  // an 8-note chord x 16: capped at the buffer
  seq::Step b = {};
  uint8_t p8[8];
  for (int k = 0; k < 8; ++k) seq::add_note(b, {(uint8_t)(48 + k * 3), 100, 96, 0, 0}), p8[k] = (uint8_t)(48 + k * 3);
  b.rt = {16, 0, 0, 0, seq::kRtChord, 0, 0};
  CHECK(seq::rt_expand(b, p8, 0, 0, false, 1, 0, 0, 0, o, 64) == 64);
}
static void test_rt_planner() {
  seq::Track* t = fresh_track();
  cmd("seq=seed:41");
  CHECK(cmd("seq=rtrg:1:5:cnt=4,vcrv=down").find("CHORD cnt 4 rate AUTO vel DOWN") != std::string::npos);
  // the notes are a quarter step long: AUTO spreads 4 repeats over it (1/16 step apart)
  t = seq::edit_track(0, false);
  t->step[4].note[0].len = 96;   // a whole step: 1/4 apart
  seq::touched();
  auto h = play_steps(16);
  CHECK(h.size() == 19);
  for (int i = 0; i < 4; ++i) CHECK(h[4 + (size_t)i].pitch == 52 && at_pos(h[4 + (size_t)i].pos, 4 + i * 0.25) && std::fabs(h[4 + (size_t)i].len - 0.25) < 0.02);
  g_replans = 0;
  CHECK(same(play_steps(16, replan_often), h));
  // the arp on a chord step at speed 2 (its own step: half a Rytm step)
  seq::set_chord(seq::edit_track(0, false)->step[8], 60, 1, 0, 100, 96, 0);
  cmd("seq=rtrg:1:9:mode=up,oct=2,rate=1/4");
  cmd("seq=spd:1:2");
  auto a = play_steps(8);
  std::vector<int> want = {60, 64, 67, 72, 76, 79};
  int got = 0;
  for (const Hit& x : a)
    if (x.pos >= 3.99 && x.pos < 4.74 && x.pitch >= 60) {   // (step 10's own note, 57, at 4.5)
      CHECK(x.pitch == want[(size_t)got] && at_pos(x.pos, 4 + got * 0.125));
      ++got;
    }
  CHECK(got == 6);
  cmd("seq=spd:1:follow");
  // the heaviest step (8 notes x 16 repeats = 64 note-ons) at 300 BPM: the queue holds
  seq::Step& big = seq::edit_track(0, false)->step[12];
  big.n = 0;
  for (int k = 0; k < 8; ++k) seq::add_note(big, {(uint8_t)(48 + k * 3), 100, 96, 0, 0});
  cmd("seq=rtrg:1:13:cnt=16");
  tempo(300);
  double keep = g_period;
  g_period = 2400;
  for (int i = 0; i < 400; ++i) block();
  uint32_t dq = seq::stats().dropped_queue;
  auto bh = play_steps(32);
  printf("  P4: 8 notes x 16 repeats at 300 BPM: %zu notes in 2 loops, %u dropped\n", bh.size(), seq::stats().dropped_queue - dq);
  CHECK(seq::stats().dropped_queue == dq);
  g_period = keep;
  tempo(120);
  for (int i = 0; i < 400; ++i) block();
  CHECK(cmd("seq=rtrg:1:13:off").find("OFF") != std::string::npos && !seq::rt_on(seq::edit_track(0, false)->step[12].rt));
  CHECK(cmd("seq=rtrg:1:13:rate=3").find("seq=rtrg") != std::string::npos);   // (not a rate)
  fresh_track();
}

// ---- X1: slides between locks
static void test_slide() {
  fresh_track();
  report_fm(0);
  cmd("seq=lock:1:1:1:0");
  cmd("seq=lock:1:5:1:100");
  CHECK(cmd("seq=slide:1:1:1:1").find("slide to the next") != std::string::npos);
  CHECK(cmd("seq=slide:1:2:1:1").find("a lock that is there") != std::string::npos);
  g_p1.clear();
  play_steps(16, probe_p1);
  // (the probe's positions are the host's: 700 frames of arrival + the 192 frames the planner starts early ahead of the
  // Rytm's, about 0.15 step) P1 glides from 0 at step 1 to 100 at step 5 (steps 2..4 have trigs without a P1 lock: they do not cut it), then
  // holds 100 on step 5, back to the base (0) from step 6 (no lock there)
  auto at = [](double pos) { return g_p1[(size_t)(pos * 4)]; };
  printf("  X1: P1 at steps 1.5 / 2.5 / 3.5 / 4.5 / 5.5 / 6.5: %.0f %.0f %.0f %.0f %.0f %.0f\n", at(0.5), at(1.5), at(2.5),
         at(3.5), at(4.5), at(5.5));
  for (int k = 1; k < 16; ++k) CHECK(at(k / 4.0) >= at((k - 1) / 4.0) - 0.01);   // rising to step 4
  CHECK(std::fabs(at(1.5) - 37.5) < 4 && std::fabs(at(2.5) - 62.5) < 4 && at(4.5) == 100 && at(5.5) == 0);
  // the slide's target in play order: REV - from step 5 (its lock 100) the next P1 lock is step 1's (0), 4 steps on
  cmd("seq=slide:1:1:1:0");
  cmd("seq=slide:1:5:1:1");
  cmd("seq=dir:1:rev");
  g_p1.clear();
  play_steps(16, probe_p1);
  // REV: step 5 plays at position 11 (16 - 5), step 1 at 15
  CHECK(at(11.5) < 100 && at(11.5) > 75 && at(13.5) < 50 && at(13.5) > 25);
  cmd("seq=dir:1:fwd");
  // no other lock of the param: it holds
  cmd("seq=lock:1:1:1:-1");
  g_p1.clear();
  play_steps(16, probe_p1);
  CHECK(at(4.5) == 100 && at(5.5) == 0);
  // the file: the slide's block; removing the lock takes its slide
  CHECK(seq::edit_track(0, false)->step[4].slide == 1u);
  cmd("seq=lock:1:5:1:-1");
  CHECK(seq::edit_track(0, false)->step[4].slide == 0);
  report_fm(0);
  fresh_track();
}

// ---- H1 + V3: the FX column and its jumps
static std::vector<int> order_of(const std::vector<Hit>& h) {   // the steps that played, by pitch (48 + step)
  std::vector<int> v;
  for (const Hit& x : h) v.push_back(x.pitch - 48);
  return v;
}
static void test_fx_fn() {
  seq::Fx f;
  char b[16];
  CHECK(seq::fx_parse("jmp", "9", &f) && f.cmd == seq::kFxJmp && f.val == 9 && !strcmp(seq::fx_text(f, b, sizeof b), "JMP 9"));
  CHECK(seq::fx_parse("LOP", "4x2", &f) && !strcmp(seq::fx_text(f, b, sizeof b), "LOP 4x2"));
  CHECK(seq::fx_parse("OCT", "-2", &f) && (int8_t)f.val == -2 && !strcmp(seq::fx_text(f, b, sizeof b), "OCT -2"));
  CHECK(seq::fx_parse("dir", "pend", &f) && f.val == seq::kDirPend && !strcmp(seq::fx_text(f, b, sizeof b), "DIR PEND"));
  CHECK(!seq::fx_parse("JMP", "0", &f) && !seq::fx_parse("JMP", "129", &f) && !seq::fx_parse("OCT", "5", &f) && !seq::fx_parse("XYZ", "1", &f));
  CHECK(!seq::fx_parse("LOP", "17x1", &f) && !seq::fx_parse("DLY", "x", &f) && !seq::fx_parse("SPD", "2", &f));
}
static void test_fx_planner() {
  fresh_track();
  cmd("seq=seed:41");
  // a track whose only order command can never play (step 20, the length 16): the walk = step_for, every direction
  CHECK(cmd("seq=fx:1:20:1:JMP:3").find("FX1 JMP 3") != std::string::npos);
  for (int d = 0; d < seq::kDirs; ++d) {
    char l[40];
    snprintf(l, sizeof l, "seq=dir:1:%s", seq::dir_name(d));
    cmd(l);
    std::vector<int> want;
    for (int k = 0; k < 48; ++k) want.push_back(seq::step_for(16, k, d, 41, 0, false).index);
    CHECK(order_of(play_steps(48)) == want);
  }
  cmd("seq=dir:1:fwd");
  cmd("seq=fx:1:20:1:off");
  // JMP: after step 4, step 9 (the loop stays 16 ticks: it begins again at step 1)
  cmd("seq=fx:1:4:1:JMP:9");
  CHECK((order_of(play_steps(20)) == std::vector<int>{0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 0, 1, 2, 3}));
  cmd("seq=fx:1:4:1:off");
  // SKP 2 on step 2; LOP 2x1 on step 6 (steps 5..6 once more); DIR REV on step 12
  cmd("seq=fx:1:2:1:SKP:2");
  cmd("seq=fx:1:6:2:LOP:2x1");
  cmd("seq=fx:1:12:1:DIR:rev");
  CHECK((order_of(play_steps(16)) == std::vector<int>{0, 1, 4, 5, 4, 5, 6, 7, 8, 9, 10, 11, 10, 9, 8, 7}));
  cmd("seq=fx:1:2:1:off"), cmd("seq=fx:1:6:2:off"), cmd("seq=fx:1:12:1:off");
  // JRN with a 50 % CHN in the other column: random, repeatable (replans, a second PLAY), another seed another walk
  cmd("seq=fx:1:8:1:JRN:0");
  cmd("seq=fx:1:8:2:CHN:50");
  auto j = play_steps(64);
  g_replans = 0;
  CHECK(same(play_steps(64, replan_often), j) && g_replans > 30);
  CHECK(same(play_steps(64), j));
  cmd("seq=seed:42");
  auto j2 = play_steps(64);
  cmd("seq=seed:41");
  CHECK(!same(j2, j));
  int jumps = 0;
  auto oj = order_of(j);
  for (size_t i = 0; i + 1 < oj.size(); ++i) jumps += oj[i] == 7 && oj[i + 1] != 8;
  printf("  H1: JRN with CHN 50%%: %d jumps in 4 loops\n", jumps);
  CHECK(jumps >= 1);
  cmd("seq=fx:1:8:1:off"), cmd("seq=fx:1:8:2:off");
  // plan-time: DLY, CUT, OCT, TRS (degrees in C major, QNT on), RTG, RNV, CHN 0 (never) / 100 (always)
  cmd("seq=key:C:maj:edit");
  cmd("seq=fx:1:3:1:DLY:48");
  cmd("seq=fx:1:4:1:CUT:12");
  cmd("seq=fx:1:5:1:OCT:1");
  cmd("seq=fx:1:6:1:TRS:2");     // F (53) + 2 degrees = A (57)
  cmd("seq=fx:1:7:1:RTG:3");
  cmd("seq=fx:1:9:1:OCT:-1"), cmd("seq=fx:1:9:2:CHN:0");
  cmd("seq=fx:1:10:1:OCT:1"), cmd("seq=fx:1:10:2:CHN:100");
  auto h = play_steps(16);
  std::map<int, std::vector<Hit>> by;
  for (const Hit& x : h) by[(int)std::floor(x.pos + 0.01)].push_back(x);
  CHECK(by[2].size() == 1 && at_pos(by[2][0].pos, 2.5) && by[2][0].pitch == 50);
  CHECK(by[3].size() == 1 && std::fabs(by[3][0].len - 0.125) < 0.02);
  CHECK(by[4][0].pitch == 52 + 12 && by[5][0].pitch == 57);
  CHECK(by[6].size() == 3 && at_pos(by[6][1].pos, 6 + 8 / 96.0) && at_pos(by[6][2].pos, 6 + 16 / 96.0));   // over its 24/96
  CHECK(by[8][0].pitch == 56 && by[9][0].pitch == 57 + 12);
  // RND +-3 semitones (QNT off: chromatic), repeatable; RNV
  cmd("seq=key:off");
  cmd("seq=fx:1:12:1:RND:3");
  cmd("seq=fx:1:12:2:RNV:40");
  auto r1 = play_steps(64), r2 = play_steps(64);
  CHECK(same(r1, r2));
  std::set<int> ps;
  for (const Hit& x : r1)
    if (std::lround(x.pos) % 16 == 11) {
      ps.insert(x.pitch);
      CHECK(x.pitch >= 59 - 3 && x.pitch <= 59 + 3);
    }
  CHECK(ps.size() >= 2);
  // FIL 2 on step 1: with FILL held, page 2's step 1 plays instead
  seq::Track* t = seq::edit_track(0, false);
  t->length = 32;
  seq::add_note(t->step[16], {90, 100, 24, 0, 0});
  seq::touched();
  cmd("seq=fx:1:1:1:FIL:2");
  CHECK(play_steps(1)[0].pitch == 48);
  direct_state({12, 1});
  CHECK(play_steps(1)[0].pitch == 90);
  direct_state({12, 0});
  // swing under jumps follows the tick
  fresh_track();
  cmd("seq=fx:1:2:1:JMP:4");
  cmd("seq=swing:1:66");
  auto sw = play_steps(8);
  for (size_t i = 1; i < sw.size(); ++i) CHECK(at_pos(sw[i].pos, (double)i + (i & 1 ? 0.32 : 0)));
  cmd("seq=swing:1:50");
  fresh_track();
  for (int k = 1; k <= 16; ++k) cmdf("seq=fx:1:%d:1:off", k), cmdf("seq=fx:1:%d:2:off", k);
  cmd("seq=fx:1:20:1:off");
}

// ---- the planner's cost (the Mac's; the Pi 4's core is about 3-4x slower [inferred]): the time in pre_block per block
// and per planned step, a plain track against one with every seqtop feature on
static void test_cost() {
  auto measure = [](const char* what) {
    for (int i = 0; i < 400; ++i) block();
    g_pb_ns = 0, g_pb_n = 0, g_pb_max = 0;
    play_steps(256);
    double per_step = g_pb_ns / 256.0 / 1000.0;
    printf("  cost %-38s %.1f us a step (pre_block's sum), the longest block %.1f us\n", what, per_step, g_pb_max / 1000.0);
    return per_step;
  };
  fresh_track();
  double plain = measure("plain (FWD, one note a step)");
  seq::Track* t = fresh_track();
  for (int k = 0; k < 16; ++k) seq::set_chord(t->step[k], 48 + k, 6, 0, 100, 96, 0);   // MAJ7 chords
  seq::touched();
  cmd("seq=key:C:min:play");
  cmd("seq=dir:1:brown");
  cmd("seq=rnd:1:mode=track,wght=chord");
  cmd("seq=smod:1:src=9,quant=step,aff=all");
  for (int k = 1; k <= 16; ++k) {
    cmdf("seq=lock:1:%d:1:%d", k, k * 7);
    cmdf("seq=lcond:1:%d:1:50%%", k);
    cmdf("seq=slide:1:%d:1:1", k);
    if (k % 2) cmdf("seq=rtrg:1:%d:mode=updn,oct=2,rate=1/8", k);
    else cmdf("seq=rtrg:1:%d:cnt=4,pcrv=3", k);
  }
  cmd("seq=fx:1:5:1:JRN:4"), cmd("seq=fx:1:5:2:CHN:50"), cmd("seq=fx:1:9:1:LOP:2x2"), cmd("seq=fx:1:13:1:RND:2");
  double all = measure("everything (BROWN + jumps, R1, slides)");
  printf("  cost: everything on %.1fx the plain track's\n", all / plain);
  CHECK(all < 400);   // (us a step on the Mac: far below a step's 125 ms at 120 BPM; a guard against a runaway walk)
  cmd("seq=key:off"), cmd("seq=dir:1:fwd"), cmd("seq=rnd:1:mode=off"), cmd("seq=smod:1:src=0,quant=loop,aff=notes");
  for (int k = 1; k <= 16; ++k) cmdf("seq=fx:1:%d:1:off", k), cmdf("seq=fx:1:%d:2:off", k);
  fresh_track();
}

// ---- the review's findings (a separate reviewer pass), each a case
static void test_review() {
  // 1: a slide's target search walked the track per sliding lock (BROWN, 128 steps, 12 sliding locks with no target:
  // 643 us in one block on the Mac before) - one walk ahead per planned step now
  seq::Track* t = fresh_track();
  t->length = 128;
  seq::touched();
  for (int k = 1; k <= 12; ++k) cmdf("seq=lock:1:1:%d:%d", k, 10 * k), cmdf("seq=slide:1:1:%d:1", k);
  cmd("seq=dir:1:brown");
  for (int i = 0; i < 400; ++i) block();
  g_pb_max = 0;
  play_steps(32);
  printf("  review 1: BROWN 128 steps, 12 sliding locks: the longest block %.1f us\n", g_pb_max / 1000.0);
  CHECK(g_pb_max < 150000);
  cmd("seq=dir:1:fwd");
  // 2: SEED MOD QUANT STEP with AFFECTS ORDER - the order's offset is the loop's, kept while steps come and go
  t = fresh_track();
  cmd("seq=seed:41"), cmd("seq=dir:1:rnd"), cmd("seq=smod:1:src=9,quant=step,aff=order");
  report_fm(0);
  auto still = order_of(play_steps(16));
  static int moved;
  moved = 0;
  auto mv = order_of(play_steps(16, [] {
    if (!moved && (double)((int64_t)seq::frames() - g_t0) / g_period > 2.2) moved = 1, report_fm(127);
  }));
  CHECK(still == mv);
  cmd("seq=smod:1:src=0,quant=loop,aff=notes"), cmd("seq=dir:1:fwd");
  report_fm(0);
  // 3: JRN on a step visited again in the loop draws again (it was keyed by the step: the same jump every visit)
  fresh_track();
  cmd("seq=fx:1:4:1:JRN:2");
  auto j = order_of(play_steps(32));
  std::set<int> l1(j.begin() + 16, j.end());
  printf("  review 3: JRN 2 on step 4, loop 2 visits %zu steps\n", l1.size());
  CHECK(l1.size() > 4);
  cmd("seq=fx:1:4:1:off");
  // 4: two overlapping LOPs finish (one counter made them loop forever)
  cmd("seq=fx:1:2:1:LOP:2x1"), cmd("seq=fx:1:4:1:LOP:4x1");
  CHECK((order_of(play_steps(16)) == std::vector<int>{0, 1, 0, 1, 2, 3, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7}));
  cmd("seq=fx:1:2:1:off"), cmd("seq=fx:1:4:1:off");
  // 5: a step delayed by a whole step (DLY 96) survives replans (a replan cut it by its delayed frame)
  cmd("seq=fx:1:5:1:DLY:96");
  auto d0 = play_steps(16);
  g_replans = 0;
  auto d1 = play_steps(16, replan_often);
  CHECK(d0.size() == 16 && same(d0, d1));
  cmd("seq=fx:1:5:1:off");
  fresh_track();
}

// ---- the web page's /status "seqx"
static void test_json() {
  fresh_track();
  cmd("seq=key:D:dor:edit");
  cmd("seq=seed:41");
  cmd("seq=spd:1:3/4"), cmd("seq=dir:1:pend"), cmd("seq=rnd:1:mode=steps,oct=3"), cmd("seq=smod:1:src=9");
  cmd("seq=rtrg:1:3:cnt=4"), cmd("seq=fx:1:5:1:OCT:-1"), cmd("seq=rndstep:1:6:1");
  cmd("seq=lock:1:7:2:50"), cmd("seq=lcond:1:7:2:1:4"), cmd("seq=slide:1:7:2:1");
  static char b[8192];
  int n = seq::seqx_json(b, sizeof b);
  printf("  %s\n", b);
  CHECK(n > 0 && strstr(b, "\"seqx\":{\"key\":2,\"scale\":3,\"qnt\":1,\"seed\":41,\"fixed\":1"));
  CHECK(strstr(b, "{\"t\":1,\"spd\":4,\"dir\":2,\"rnd\":[1,48,3,0],\"smod\":[9,16,1,0]"));
  CHECK(strstr(b, "{\"s\":3,\"rt\":[4,0,0,0,0,0,0]") && strstr(b, "{\"s\":5,\"rt\":[0,0,0,0,0,0,0],\"fx\":[[7,-1],[0,0]]"));
  CHECK(strstr(b, "\"s\":6,") && strstr(b, "\"r\":1") && strstr(b, "\"lc\":[[2,") && strstr(b, "\"sl\":[2]"));
  CHECK(seq::seqx_json(b, 40) == 0);
  // seq=played: the order the notes started in (the e2e's view), then forgotten
  fresh_track();
  cmd("seq=played");
  cmd("seq=dir:1:rev"), cmd("seq=spd:1:2");
  play_steps(2);
  std::string pl = cmd("seq=played:1");
  printf("  %s\n", pl.c_str());
  CHECK(pl.find("0/0:16:63 1/0:15:62 2/0:14:61") != std::string::npos);   // (the tick of 2 comes a round trip later)
  CHECK(cmd("seq=played:1") == "seq: played");
  cmd("seq=dir:1:fwd"), cmd("seq=spd:1:follow");
  cmd("seq=key:off"), cmd("seq=spd:1:follow"), cmd("seq=dir:1:fwd"), cmd("seq=rnd:1:mode=off,oct=2"), cmd("seq=smod:1:src=0");
  cmd("seq=fx:1:5:1:off");
  fresh_track();
}

// ---- the file: the blocks only when used, read back, unknown values locked
static void test_file() {
  static seq::Pattern p, q;
  static uint8_t buf[1 << 18], buf2[1 << 18];
  seq::clear(p, 3);
  seq::Track* t = seq::track_of(p, 2, true);
  t->on = 1;   // (per pattern poly: a poly track)
  seq::add_note(t->step[0], {60, 100, 24, 0, 0});
  int n0 = seq::pack(p, buf, sizeof buf);
  CHECK(n0 > 0 && buf[4] == 2);
  t->speed = seq::kSpd5_4;
  int n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == n0 + 4 + 4 + 2 && buf2[4] == 4);
  CHECK(seq::unpack(q, 3, buf2, n) && seq::track_of(q, 2)->speed == seq::kSpd5_4);
  t->speed = 0;
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0 && !memcmp(buf, buf2, n0));   // FOLLOW: the old bytes
  // a speed id this build does not know: a newer Sidecar's (locked)
  t->speed = seq::kSpd2;
  n = seq::pack(p, buf2, sizeof buf2);
  for (int i = 12; i + 5 < n; ++i)
    if (buf2[i] == seq::kBlkSpeed && buf2[i + 1] == (seq::kBlkTrack | seq::kBlkRequired) && buf2[i + 4] == 2) buf2[i + 5] = 77;
  uint32_t c = seq::crc32(buf2 + 12, n - 12);
  for (int k = 0; k < 4; ++k) buf2[8 + k] = (uint8_t)(c >> (8 * k));
  CHECK(!seq::unpack(q, 3, buf2, n) && seq::unpack_error() == seq::kUnpackNewer);
  // P1: the direction's block; FWD none
  t->speed = 0, t->dir = seq::kDirBrown;
  n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == n0 + 4 + 4 + 2 && seq::unpack(q, 3, buf2, n) && seq::track_of(q, 2)->dir == seq::kDirBrown);
  t->dir = 0;
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0);
  // M1: the pattern's key / scale (required only with QNT PLAY)
  p.key = 2, p.scale = seq::kScaleDor, p.qnt = seq::kQntEdit;
  n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == n0 + 4 + 4 + 3 && seq::unpack(q, 3, buf2, n) && q.key == 2 && q.scale == seq::kScaleDor && q.qnt == seq::kQntEdit);
  bool req = false;
  for (int i = 12; i + 1 < n; ++i) req |= buf2[i] == seq::kBlkScale && buf2[i + 2] == 3 && (buf2[i + 1] & seq::kBlkRequired);
  CHECK(!req);
  p.qnt = seq::kQntPlay;
  n = seq::pack(p, buf2, sizeof buf2);
  for (int i = 12; i + 1 < n; ++i) req |= buf2[i] == seq::kBlkScale && buf2[i + 2] == 3 && (buf2[i + 1] & seq::kBlkRequired);
  CHECK(req);
  p.key = p.scale = p.qnt = 0;
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0);
  // R1 / R1b: the defaults write nothing; a mode, a flagged step, a SEED MOD: their blocks
  CHECK(t->rnd_low == seq::kRndLowDef && t->smod_quant == seq::kSqLoop && t->smod_range == seq::kSmodRangeDef);
  t->step[0].xf = seq::kXfRnd;
  t->rnd_mode = seq::kRndSteps, t->rnd_wght = seq::kWChord, t->rnd_oct = 3;
  t->smod_src = 9, t->smod_range = 8, t->smod_quant = seq::kSqBar, t->smod_aff = seq::kAffOrder;
  n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == n0 + 4 + (4 + 21) + (4 + 5) && seq::unpack(q, 3, buf2, n));
  const seq::Track* u = seq::track_of(q, 2);
  CHECK(u->rnd_mode == seq::kRndSteps && u->rnd_wght == seq::kWChord && u->rnd_oct == 3 && u->rnd_low == 48 &&
        (u->step[0].xf & seq::kXfRnd) && !(u->step[1].xf & seq::kXfRnd));
  CHECK(u->smod_src == 9 && u->smod_range == 8 && u->smod_quant == seq::kSqBar && u->smod_aff == seq::kAffOrder);
  CHECK(seq::pack(q, buf, sizeof buf) == n && !memcmp(buf, buf2, n));
  t->step[0].xf = 0, t->rnd_mode = 0, t->rnd_wght = 0, t->rnd_oct = seq::kRndOctDef;
  t->smod_src = 0, t->smod_range = seq::kSmodRangeDef, t->smod_quant = seq::kSqLoop, t->smod_aff = 0;
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0);
  // V1: a lock's condition; none - no block (a lock alone stays v2)
  seq::set_lock(t->step[0], 3, 50 << 7);
  int nl0 = seq::pack(p, buf, sizeof buf);
  CHECK(buf[4] == 2);
  t->step[0].lock[0].cond = (uint8_t)seq::lock_cond_parse("1:4");
  n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == nl0 + 4 + 4 + 1 + 3 && seq::unpack(q, 3, buf2, n) && seq::track_of(q, 2)->step[0].lock[0].cond == t->step[0].lock[0].cond);
  // a code a lock may not hold (PRE): a newer Sidecar's
  for (int i = 12; i + 7 < n; ++i)
    if (buf2[i] == seq::kBlkLockCond && buf2[i + 4] == 2) buf2[i + 7] = seq::kCondPre;
  uint32_t c2 = seq::crc32(buf2 + 12, n - 12);
  for (int k = 0; k < 4; ++k) buf2[8 + k] = (uint8_t)(c2 >> (8 * k));
  CHECK(!seq::unpack(q, 3, buf2, n) && seq::unpack_error() == seq::kUnpackNewer);
  t->step[0].nl = 0;
  // P4 / M4: a step's ratchet / arp
  t->step[0].rt = {4, 6, seq::kVcUpDn, -3, seq::kRtConv, 2, 60};
  n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == n0 + 4 + 4 + 1 + 8 && seq::unpack(q, 3, buf2, n));
  const seq::Rtrg& x = seq::track_of(q, 2)->step[0].rt;
  CHECK(x.cnt == 4 && x.rate == 6 && x.vcrv == seq::kVcUpDn && x.pcrv == -3 && x.mode == seq::kRtConv && x.oct == 2 && x.gate == 60);
  t->step[0].rt = {};
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0);
  // X1: a sliding lock; a slide bit of a param the step does not lock writes nothing
  t->step[0].slide = 1u << 7;
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0);
  t->step[0].slide = 0;
  seq::set_lock(t->step[0], 7, 30 << 7);
  int nl1 = seq::pack(p, buf, sizeof buf);
  t->step[0].slide = 1u << 7;
  n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == nl1 + 4 + 4 + 1 + 2 && seq::unpack(q, 3, buf2, n) && seq::track_of(q, 2)->step[0].slide == 1u << 7);
  seq::clear_lock(t->step[0], 7);
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0);
  // H1: the FX column (an empty step's jump too); an unknown command: a newer Sidecar's
  seq::fx_parse("JMP", "3", &t->step[9].fx[0]);
  seq::fx_parse("OCT", "-1", &t->step[0].fx[1]);
  n = seq::pack(p, buf2, sizeof buf2);
  CHECK(n == n0 + 4 + 4 + 1 + 2 * 5 && seq::unpack(q, 3, buf2, n) && seq::track_of(q, 2)->step[9].fx[0].cmd == seq::kFxJmp &&
        (int8_t)seq::track_of(q, 2)->step[0].fx[1].val == -1);
  for (int i = 12; i + 6 < n; ++i)
    if (buf2[i] == seq::kBlkFx && buf2[i + 4] == 2) buf2[i + 6] = 99;
  uint32_t c3 = seq::crc32(buf2 + 12, n - 12);
  for (int k = 0; k < 4; ++k) buf2[8 + k] = (uint8_t)(c3 >> (8 * k));
  CHECK(!seq::unpack(q, 3, buf2, n) && seq::unpack_error() == seq::kUnpackNewer);
  t->step[9].fx[0] = {}, t->step[0].fx[1] = {};
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0);
}

int main() {
  test_file();
  test_dir_map();
  test_scale_fns();
  test_rnd_fn();
  test_rt_fn();
  test_fx_fn();
  char tmpl[] = "/tmp/core_seqtop_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'Q', 'T', 'O', 'P', 0xF7};
  engine_sysex(proj, sizeof proj);
  // 2026-10-26, the RAM store (phase 2): the project's sequences are loaded once its store is filled (the idle pass)
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
  // BD's voice a 6-OP FM (a Sidecar machine: its notes reach the planner's events)
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(kFm | 0)};
  int prm[8] = {0, 64, 64, 127, 64, 64, 100, 127};
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(k < 8 ? prm[k] : 0)), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
  CHECK(v[0].fm);
  CHECK(cmd("seq=track:1:on").find("mask 0x001") != std::string::npos);
  seq::set_latency(192);
  tempo(120);
  for (int i = 0; i < 400; ++i) block();
  test_speed();
  test_speed_switch();
  test_dir_planner();
  test_scale_planner();
  test_rnd_planner();
  test_smod();
  test_lock_cond();
  test_rt_planner();
  test_slide();
  test_fx_planner();
  test_cost();
  test_review();
  test_json();
  test_grow_pages();
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_seqtop: OK\n");
  return 0;
}
