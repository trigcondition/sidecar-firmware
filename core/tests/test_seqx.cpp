// core/tests/test_seqx.cpp - E1 and E2 of docs/SEQ-IDEAS.md (seqx): the seeded, keyed RNG (the same answer when a
// step is planned again: a replan, a second PLAY with a fixed seed, a save and load), the step map step_for (identical
// to the old "step = count mod length", so existing patterns play bit for bit as before), and .sqp v4's blocks (written
// only when used, older versions unchanged, unknown blocks kept, per-step ones dropped when their steps changed, a
// required unknown block locks the file as a newer Sidecar's).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "inst.h"
#include "ui/remote.h"
#include "machines.h"
#include "pool/pool.h"
#include "seq/rng.h"
#include "seq/seq.h"
#include "voices.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;

// ---- the Rytm side, as test_seq.cpp's (one clock; each tick arrives `delay` host frames after it fired)
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
// the notes that started: (pitch, the step count since PLAY they belong to)
static std::vector<std::pair<int, int64_t>> g_started;
static int64_t g_t0 = 0;
static double g_period = 6000;
static void (*g_each)() = nullptr;
static void block() {
  int64_t f[512];
  uint8_t on[512], vo[512], no[512];
  int n = seq::pending(f, on, vo, no, 512);
  uint32_t before = seq::stats().notes_on;
  int64_t h = (int64_t)seq::frames();
  seq::pre_block();
  if (g_each) g_each();
  g_us += 250;
  host_fake_time(true, g_us);
  if (seq::stats().notes_on == before) return;
  int64_t f2[512];
  uint8_t on2[512], vo2[512], no2[512];
  int n2 = seq::pending(f2, on2, vo2, no2, 512);
  for (int i = 0; i < n; ++i) {
    if (!on[i] || f[i] > h + kBlock) continue;
    bool still = false;
    for (int j = 0; j < n2 && !still; ++j) still = on2[j] && f2[j] == f[i] && no2[j] == no[i];
    if (!still) {
      int64_t k = (int64_t)llround((double)(f[i] + 192 - 700 - g_t0) / g_period);   // (latency 192, offset ~700)
      g_started.push_back({no[i], k});
    }
  }
}
struct Run {
  int track = 0, len = 16;
  int64_t next_k = 0;
};
static void run(Run& r, int64_t until) {
  while ((int64_t)seq::frames() < until) {
    int64_t h = (int64_t)seq::frames();
    for (;;) {
      int64_t tf = g_t0 + (int64_t)llround(r.next_k * g_period);
      if (tf > h + 2) break;
      auto b = blk4(tf);
      b.insert(b.end(), {1, (uint8_t)r.track, (uint8_t)(r.next_k % r.len), (uint8_t)r.len, 2});
      g_wire.push_back({tf + 700, msg(0x2C, b)});
      ++r.next_k;
    }
    deliver(h);
    block();
  }
}
static std::string cmd(const char* c) {
  char b[900];
  CHECK(seq::command(c, b, sizeof b));
  return b;
}
static void report(int vb, std::vector<int> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)vb};
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(k < (int)p.size() ? p[k] : 0)), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
// One PLAY of `loops` loops of the 16-step pattern from now: the notes that played, as (pitch, step count) pairs.
static int g_track = 0;   // the Rytm track whose ticks play() sends
static std::vector<std::pair<int, int64_t>> play(int loops, void (*each)() = nullptr) {
  g_wire.clear();
  g_started.clear();
  int64_t now = (int64_t)seq::frames();
  g_t0 = (now / 32 + 200) * 32;
  auto b = std::vector<uint8_t>{1, 1};
  auto x = blk4(g_t0);
  b.insert(b.end(), x.begin(), x.end());
  g_wire.push_back({g_t0 + 700, msg(0x2D, b)});
  Run r;
  r.track = g_track;
  g_each = each;
  run(r, g_t0 + (int64_t)(loops * 16 * g_period) - 3000);
  g_each = nullptr;
  auto s = blk4((int64_t)seq::frames());
  std::vector<uint8_t> stop = {1, 0};
  stop.insert(stop.end(), s.begin(), s.end());
  direct_state(stop);
  for (int i = 0; i < 200; ++i) block();
  auto res = g_started;
  // the decisions: which (note, step count) played. (A replan's cancelled note-ons leave the queue too, so the same
  // note can be counted more than once here: only whether it played is compared.)
  std::sort(res.begin(), res.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
  res.erase(std::unique(res.begin(), res.end()), res.end());
  return res;
}
static int g_replans = 0;
static void replan_often() {   // an edit's replan every 7 blocks (the swing set to what it is: the planned steps go and come back)
  static int k = 0;
  if (++k % 7 == 0) cmd("seq=swing:1:50"), ++g_replans;
}

// 2026-10-26, the RAM store (phase 2): the sequencer reads its files from the store a project load fills (pool.h): a
// project is loaded once the idle pass has filled it; refill(): loaded again (every write landed first)
static void loaded() {
  for (int i = 0; i < 200000 && core::project_loading(); ++i) core::idle_save();
  CHECK(!core::project_loading());
}
static void refill() {
  for (int i = 0; i < 1000 && core::fmt::post_busy(); ++i) core::fmt::post_step();
  core::project_fill_begin();
  loaded();
}
static void test_rng() {
  // the same inputs, the same bits; any input changed, other bits
  uint32_t a = seq::rng_key(41, 0, 5, 3, seq::kRngProb);
  CHECK(a == seq::rng_key(41, 0, 5, 3, seq::kRngProb));
  CHECK(a != seq::rng_key(42, 0, 5, 3, seq::kRngProb) && a != seq::rng_key(41, 1, 5, 3, seq::kRngProb));
  CHECK(a != seq::rng_key(41, 0, 6, 3, seq::kRngProb) && a != seq::rng_key(41, 0, 5, 4, seq::kRngProb));
  CHECK(a != seq::rng_key(41, 0, 5, 3, seq::kRngNote) && a != seq::rng_key(41, 0, 5, 3, seq::kRngProb, 1));
  CHECK(seq::rng_key(41, 0, 5, -1, seq::kRngProb) == seq::rng_key(41, 0, 5, -1, seq::kRngProb));   // "same every loop"
  printf("  rng_key(41, 0, 5, 3, prob) = 0x%08x\n", a);
  CHECK(a == 0x2b9067ffu);   // (pinned: a change of the hash changes every saved seed's music)
  // uniform enough: 100 000 draws, every percent bucket within 15 % of its share; rng_below in range
  int hist[100] = {0};
  for (int i = 0; i < 100000; ++i) {
    int v = seq::rng_below(seq::rng_key(7, i / 128 & 3, i % 128, i / 512, seq::kRngProb), 100);   // (each input once)
    CHECK(v >= 0 && v < 100);
    ++hist[v];
  }
  for (int v : hist) CHECK(v > 850 && v < 1150);
  CHECK(seq::rng_below(0xFFFFFFFFu, 7) == 6 && seq::rng_below(0, 7) == 0 && seq::rng_below(123, 1) == 0);
  int hits = 0;
  for (int i = 0; i < 10000; ++i) hits += seq::rng_chance(seq::rng_key(9, 0, i, 0, seq::kRngProb), 30);
  CHECK(hits > 2700 && hits < 3300);
}

static void test_step_for() {
  // forward: exactly the old "abs % len" and "abs / len" for every length and count (existing patterns bit for bit)
  for (int len = 1; len <= seq::kMaxSteps; ++len)
    for (int64_t abs = 0; abs < 4 * seq::kMaxSteps + 3; ++abs) {
      seq::StepAt at = seq::step_for(len, abs);
      CHECK(at.index == (int)(abs % len) && at.loop == abs / len);
    }
  CHECK(seq::step_for(16, 1000000007).index == (int)(1000000007 % 16));
  CHECK(seq::step_for(0, 5).index == 0 && seq::step_for(16, -1).index == 0);   // (guards)
}

static void test_file() {
  static seq::Pattern p, q;
  static uint8_t buf[1 << 17], buf2[1 << 17];
  {   // a buffer too small for the blocks: 0, never a write past it
    seq::clear(p, 1);
    seq::add_note(seq::track_of(p, 0, true)->step[0], {60, 100, 24, 0, 0});
    seq::track_of(p, 0, true)->on = 1;   // (per pattern poly: a poly track, as the old-file rule reads it)
    p.seed_fixed = 1, p.seed = 7;
    int full = seq::pack(p, buf, sizeof buf);
    CHECK(full > 0);
    for (int cap = 12; cap < full; ++cap) {
      memset(buf2, 0xEE, sizeof buf2);
      CHECK(seq::pack(p, buf2, cap) == 0);
      for (int k = cap; k < cap + 16; ++k) CHECK(buf2[k] == 0xEE);
    }
  }
  seq::clear(p, 3);
  seq::Track* t = seq::track_of(p, 2, true);
  t->on = 1;   // (per pattern poly: a poly track)
  seq::add_note(t->step[0], {60, 100, 24, 0, 0});
  seq::add_note(t->step[5], {64, 90, 24, 3, 0});
  int n0 = seq::pack(p, buf, sizeof buf);
  CHECK(n0 > 0 && buf[4] == 2);   // nothing new used: v2, as before (an older Sidecar reads it)
  // a default seed / poly config writes no block (and no trailer)
  t->poly = {};
  t->poly.unison = 1;   // (1 = off, the same as 0)
  CHECK(seq::pack(p, buf2, sizeof buf2) == n0 && !memcmp(buf, buf2, n0));
  // a fixed pattern seed: v4, read back
  p.seed_fixed = 1, p.seed = 9999;
  int n = seq::pack(p, buf, sizeof buf);
  CHECK(n == n0 + 4 + 4 + 3 && buf[4] == 4);
  CHECK(seq::unpack(q, 3, buf, n) && q.seed_fixed == 1 && q.seed == 9999 && seq::track_of(q, 2)->step[5].note[0].micro == 3);
  // a track's seed and poly config: their blocks
  t->seed_own = 1, t->seed = 12, t->seed_same = 1;
  t->poly = {6, seq::kAllocReset, 3, 100, 127, 2};
  n = seq::pack(p, buf, sizeof buf);
  CHECK(seq::unpack(q, 3, buf, n));
  const seq::Track* u = seq::track_of(q, 2);
  CHECK(u && u->seed_own && u->seed == 12 && u->seed_same && u->poly.voices == 6 && u->poly.alloc == seq::kAllocReset &&
        u->poly.unison == 3 && u->poly.detune == 100 && u->poly.spread == 127 && u->poly.group == 2);
  CHECK(seq::pack(q, buf2, sizeof buf2) == n && !memcmp(buf, buf2, n));   // the round trip: the same bytes
  // a damaged block (cut short inside the trailer, the CRC fixed up): refused as damaged, the pattern empty
  {
    std::vector<uint8_t> d(buf, buf + n);
    int at = n - 4 - 2;   // inside the last block's payload: one byte gone
    d.erase(d.begin() + at);
    uint32_t c = seq::crc32(d.data() + 12, (int)d.size() - 12);
    for (int k = 0; k < 4; ++k) d[8 + k] = (uint8_t)(c >> (8 * k));
    CHECK(!seq::unpack(q, 3, d.data(), (int)d.size()) && seq::unpack_error() == seq::kUnpackDamaged && !seq::track_of(q, 2));
  }
  // a version 5 file: newer, locked
  {
    std::vector<uint8_t> d(buf, buf + n);
    d[4] = 5;
    CHECK(!seq::unpack(q, 3, d.data(), (int)d.size()) && seq::unpack_error() == seq::kUnpackNewer);
  }
}

// a v4 file with extra blocks spliced into its trailer (as a newer Sidecar would write them)
static std::vector<uint8_t> with_blocks(const uint8_t* f, int n, const std::vector<std::vector<uint8_t>>& blocks) {
  std::vector<uint8_t> d(f, f + n - 4);   // without END
  int tr = -1;
  for (int i = 12; i + 1 < (int)d.size(); ++i)   // the trailer: the file's last "XB" whose count matches (test files: one)
    if (d[i] == 'X' && d[i + 1] == 'B') tr = i;
  CHECK(tr > 0);
  int cnt = d[tr + 2] | d[tr + 3] << 8;
  for (auto& b : blocks) d.insert(d.end(), b.begin(), b.end()), ++cnt;
  d[tr + 2] = (uint8_t)cnt, d[tr + 3] = (uint8_t)(cnt >> 8);
  d.insert(d.end(), {'E', 'N', 'D', 0});
  uint32_t c = seq::crc32(d.data() + 12, (int)d.size() - 12);
  for (int k = 0; k < 4; ++k) d[8 + k] = (uint8_t)(c >> (8 * k));
  return d;
}
static void test_unknown_blocks() {
  static seq::Pattern p, q;
  static uint8_t buf[1 << 17], buf2[1 << 17];
  seq::clear(p, 7);
  seq::Track* a = seq::track_of(p, 1, true);
  seq::Track* b = seq::track_of(p, 4, true);
  a->on = b->on = 1;   // (per pattern poly: poly tracks)
  seq::add_note(a->step[0], {60, 100, 24, 0, 0});
  seq::add_note(b->step[3], {48, 100, 24, 0, 0});
  p.seed_fixed = 1, p.seed = 5;
  int n = seq::pack(p, buf, sizeof buf);
  // three unknown blocks: a pattern one, one of track 1 (kept whatever happens to its steps), a per-step one of track 4
  std::vector<uint8_t> pat_blk = {0x50, 0, 3, 0, 9, 8, 7};
  std::vector<uint8_t> trk_blk = {0x51, seq::kBlkTrack, 2, 0, 1, 42};
  std::vector<uint8_t> stp_blk = {0x52, seq::kBlkTrack | seq::kBlkPerStep, 3, 0, 4, 3, 1};
  auto d = with_blocks(buf, n, {pat_blk, trk_blk, stp_blk});
  CHECK(seq::unpack(q, 7, d.data(), (int)d.size()) && q.nkeep == 7 + 6 + 7);
  // saved again untouched: the same bytes (every unknown block verbatim, in order)
  int m = seq::pack(q, buf2, sizeof buf2);
  CHECK(m == (int)d.size() && !memcmp(buf2, d.data(), m) && seq::pack_dropped() == 0);
  // track 1's steps edited: its plain block stays; track 4's steps edited: its per-step block goes (counted)
  seq::add_note(seq::track_of(q, 1, false)->step[9], {72, 100, 24, 0, 0});
  m = seq::pack(q, buf2, sizeof buf2);
  CHECK(m == (int)d.size() + 8 + 5 + 1 && seq::pack_dropped() == 0);   // (+ one step: its head, one note, no locks)
  seq::add_note(seq::track_of(q, 4, false)->step[3], {50, 100, 24, 0, 0});
  m = seq::pack(q, buf2, sizeof buf2);
  CHECK(seq::pack_dropped() == 1);
  static seq::Pattern r;
  CHECK(seq::unpack(r, 7, buf2, m) && r.nkeep == 7 + 6);   // the pattern's and track 1's kept
  // track 4 removed altogether: a block of its track goes too
  seq::Track* t4 = seq::track_of(q, 4, false);
  t4->rytm = -1;
  m = seq::pack(q, buf2, sizeof buf2);
  CHECK(seq::pack_dropped() == 1 && seq::unpack(r, 7, buf2, m) && r.nkeep == 7 + 6);
  // a required block this build does not know: locked as a newer Sidecar's (the pattern empty)
  std::vector<uint8_t> req = {0x53, seq::kBlkRequired, 1, 0, 0};
  auto d2 = with_blocks(buf, n, {req});
  CHECK(!seq::unpack(r, 7, d2.data(), (int)d2.size()) && seq::unpack_error() == seq::kUnpackNewer && !seq::track_of(r, 1));
  // more unknown bytes than the keep holds: the rest dropped, the file still reads
  std::vector<uint8_t> big = {0x54, 0, 0, 4};   // a 1024-byte payload: past kKeepBytes with its head
  big.resize(4 + 1024, 0xAB);
  auto d3 = with_blocks(buf, n, {big});
  CHECK(seq::unpack(r, 7, d3.data(), (int)d3.size()) && r.nkeep == 0 && r.seed == 5);
}

// ---- the planner: the keyed RNG makes probability repeatable
static void test_planner(const std::string& card) {
  CHECK(cmd("seq=track:1:on").find("mask 0x001") != std::string::npos);
  seq::Track* t = seq::edit_track(0, true);
  CHECK(t);
  t->length = 16;
  for (int s = 0; s < 16; ++s) {
    seq::add_note(t->step[s], {(uint8_t)(48 + s), 100, 24, 0, 0});
    t->step[s].prob = 50;   // a coin per step
  }
  seq::touched();
  seq::set_latency(192);
  direct_state({7, (uint8_t)(120 * 120 >> 14), (uint8_t)(120 * 120 >> 7 & 0x7F), (uint8_t)(120 * 120 & 0x7F)});
  for (int i = 0; i < 400; ++i) block();
  CHECK(cmd("seq=seed:41").find("41 (fixed)") != std::string::npos);
  auto a = play(4);
  auto b = play(4);
  printf("  fixed seed 41: %zu of 64 coins played, the same on a second PLAY: %s\n", a.size(), a == b ? "yes" : "no");
  CHECK(a.size() > 16 && a.size() < 48 && a == b);
  // the loops differ (the loop index is in the hash), each the same on every PLAY
  std::set<int> l0, l1;
  for (auto& e : a)
    if (e.second / 16 < 2) (e.second / 16 == 0 ? l0 : l1).insert((int)(e.second % 16));
  CHECK(l0 != l1);
  // an edit's replan every few blocks: the same decisions
  g_replans = 0;
  auto c = play(4, replan_often);
  CHECK(g_replans > 50 && c == a);
  printf("  %d replans while playing: the same notes\n", g_replans);
  // another seed, other notes; back to 41, the first again
  cmd("seq=seed:42");
  auto d = play(4);
  CHECK(d != a);
  cmd("seq=seed:41");
  CHECK(play(4) == a);
  // "same every loop": every loop plays the first loop's coins
  cmd("seq=tseed:1:same:1");
  auto e = play(4);
  std::map<int64_t, std::set<int>> per;
  for (auto& x : e) per[x.second / 16].insert((int)(x.second % 16));
  CHECK(per.size() == 4 && per[0] == per[1] && per[1] == per[2] && per[2] == per[3]);
  cmd("seq=tseed:1:same:0");
  // a track's own seed overrides the pattern's
  cmd("seq=tseed:1:42");
  CHECK(play(4) == d);
  cmd("seq=tseed:1:follow");
  // saved and loaded (another project and back): the seed is in the file, the same notes
  for (int i = 0; i < 20; ++i) seq::tick((uint32_t)(5000 + i * 200)), seq::idle();
  CHECK(seq::stats().saves > 0);
  {
    char fn[300];
    snprintf(fn, sizeof fn, "%s/.rytm/seq/SEQX/p000.sqp", card.c_str());
    FILE* f = fopen(fn, "rb");
    CHECK(f);
    uint8_t hd[8] = {0};
    if (f) {
      CHECK(fread(hd, 1, 8, f) == 8);
      fclose(f);
    }
    CHECK(hd[4] == 4);   // a fixed seed: v4
  }
  seq::reset();
  refill();   // (the RAM store: the project loaded again, its sequences from it)
  for (int i = 0; i < 40; ++i) seq::idle();
  CHECK(seq::edit_pattern().seed_fixed == 1 && seq::edit_pattern().seed == 41);
  cmd("seq=track:1:on");
  direct_state({7, (uint8_t)(120 * 120 >> 14), (uint8_t)(120 * 120 >> 7 & 0x7F), (uint8_t)(120 * 120 & 0x7F)});
  seq::set_latency(192);
  CHECK(play(4) == a);
  printf("  saved and loaded: the same notes\n");
  // FREE: a new seed at each PLAY (64 coins: two PLAYs alike by chance 1 in 2^64)
  cmd("seq=seed:free");
  auto f1 = play(4), f2 = play(4);
  CHECK(f1 != f2);
  // FREE held (seq=seed:hold:N): repeatable for A/B listening, and a replan agrees within a PLAY
  cmd("seq=seed:hold:1234");
  auto h1 = play(4);
  g_replans = 0;
  CHECK(play(4, replan_often) == h1);
  // no probability: every step plays, whatever the seed (no draw changes a step without a chance)
  for (int s = 0; s < 16; ++s) t = seq::edit_track(0, false), t->step[s].prob = 100;
  seq::touched();
  auto all = play(2);
  CHECK(all.size() == 32);
  for (int k = 0; k < 32; ++k) CHECK(all[(size_t)k].second == k && all[(size_t)k].first == 48 + k % 16);
}

// ---- E0 through the sequencer: a PLAITS seq track with a poly config (the console's seq=poly), its chord on the
// instances, the config in the pattern's file (v4) and as the track's default (tracks.txt), another pattern without its
// own config playing the default
static void test_poly_track(const std::string& card) {
  int vi = ui::remote_track_voice(1);
  CHECK(vi >= 0);
  report(kPlaits | vi, {0, 64, 64, 64, 90, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  CHECK(cmd("seq=track:2:on").find("sequenced") != std::string::npos);
  std::string r = cmd("seq=poly:2:voices=3,alloc=rotate");
  printf("  %s\n", r.c_str());
  CHECK(r.find("VOICES 3 ROTATE") != std::string::npos && r.find("PLAITS: 3 instances, 3 notes at once") != std::string::npos);
  CHECK(cmd("seq=poly:2").find("VOICES 3 ROTATE") != std::string::npos);
  CHECK(cmd("seq=poly:2:alloc=sideways").find("alloc=") != std::string::npos);   // refused, unchanged
  CHECK(cmd("seq=poly:2:voices=3,alloc=rotate,unison=1").find("VOICES 3") != std::string::npos);
  seq::Track* t = seq::edit_track(1, true);
  CHECK(t && t->poly.voices == 3 && t->poly.alloc == seq::kAllocRotate);
  t->length = 16;
  for (int s2 = 0; s2 < 16; ++s2) t->step[s2].n = 0, t->step[s2].nl = 0;
  seq::set_chord(t->step[0], 60, 6, 0, 100, 96 * 4, 0);   // MAJ7: 4 notes, 3 voices
  seq::touched();
  int most = 0;
  g_track = 1;
  {
    static int* pm;
    pm = &most;
    static int svi;
    svi = vi;
    auto res = play(1, [] {
      int n = inst::voice_notes(svi);
      if (n > *pm) *pm = n;
    });
    CHECK(res.size() >= 4);
  }
  g_track = 0;
  CHECK(most == 3 && inst::stat(vi).path == 3 && inst::stat(vi).instances == 3 && inst::stat(vi).steals >= 1);
  printf("  a MAJ7 on VOICES 3: %d instances at once, %u steals\n", most, (unsigned)inst::stat(vi).steals);
  // saved: the pattern's file is v4 (its poly block); tracks.txt has the track's default
  for (int i = 0; i < 20; ++i) seq::tick((uint32_t)(90000 + i * 200)), seq::idle();
  char fn[300];
  snprintf(fn, sizeof fn, "%s/.rytm/seq/SEQX/tracks.txt", card.c_str());
  FILE* f = fopen(fn, "r");
  CHECK(f);
  char tb[512] = {0};
  if (f) {
    CHECK(fread(tb, 1, sizeof tb - 1, f) > 0);
    fclose(f);
  }
  CHECK(strstr(tb, "poly=2:3,1,1,0,0,0") != nullptr);
  // another pattern (no config of its own): plays the track's default
  seq::reset();
  refill();   // (the RAM store: the project loaded again, its sequences from it)
  for (int i = 0; i < 40; ++i) seq::idle();
  CHECK(seq::edit_track(1, false) && seq::edit_track(1, false)->poly.voices == 3);   // p000 loaded with its block
  seq::Pattern& p = seq::edit_pattern();
  p.index = 5;   // (as if another pattern: the same track, its poly config cleared)
  seq::edit_track(1, false)->poly = seq::PolyCfg{};
  CHECK(cmd("seq=poly:2").find("VOICES 3 ROTATE") != std::string::npos);   // the default
  CHECK(cmd("seq=poly:2:auto").find("VOICES AUTO") != std::string::npos);
  CHECK(cmd("seq=poly:2").find("PLAITS: 4 instances") != std::string::npos);   // AUTO on PLAITS: 4
  // tracks.txt's poly lines: malformed ones (too few fields, a track out of range, junk) skipped, the good ones read
  {
    FILE* w = fopen(fn, "w");
    CHECK(w);
    if (w) {
      fputs("mask=0x002\nlat=192\npoly=1:4\npoly=13:4,1,0,0,0,0\npoly=x\npoly=4:6,2,2,9,8,1\npoly=5:9,7,99,300,300,9\n", w);
      fclose(w);
    }
    seq::reset();
    refill();   // (the RAM store: the project loaded again, its sequences from it)
    CHECK(seq::poly_of(0).voices == 0 && seq::poly_of(3).voices == 6 && seq::poly_of(3).alloc == seq::kAllocReset &&
          seq::poly_of(3).unison == 2 && seq::poly_of(3).detune == 9 && seq::poly_of(3).spread == 8 && seq::poly_of(3).group == 1);
    seq::PolyCfg c3 = seq::poly_of(4);   // clamped: VOICES 8, ALLOCATION back to REASSIGN, UNISON 8, 127, 127, GROUP 4
    CHECK(c3.voices == 8 && c3.alloc == 0 && c3.unison == 8 && c3.detune == 127 && c3.spread == 127 && c3.group == 4);
  }
}

int main() {
  test_rng();
  test_step_for();
  test_file();
  test_unknown_blocks();
  char tmpl[] = "/tmp/core_seqx_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'E', 'Q', 'X', 0xF7};
  engine_sysex(proj, sizeof proj);
  loaded();
  report(kFm | 0, {0, 64, 64, 127, 64, 64, 100, 127});
  CHECK(v[0].fm);
  test_planner(card);
  test_poly_track(card);
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_seqx: OK\n");
  return 0;
}
