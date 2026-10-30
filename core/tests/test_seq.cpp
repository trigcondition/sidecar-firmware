// core/tests/test_seq.cpp - the Sidecar sequencer (core/seq/seq.h): the pattern model and its file, the clock follower
// with jittery stamps and arrivals, the planner's ahead timing (notes fire `latency` frames before the Rytm's step
// would reach the host, at the frame the clock predicts, micro offsets and strums included), transport (play, pause,
// continue, stop), mutes, live record (OFF keeps micro timing, 1/16 snaps), step entry on the trig keys (tap, hold +
// pads, hold + encoders), the LEDs, the pattern switch with its prefetched file, the 2E mask after a hello, and the
// SEQ page's frames (PNGs to $SEQ_OUT, default build/seq: seq_roll.png, seq_held.png).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "hooks.h"
#include "bundle/rename.h"
#include "pool/pool.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "fx/insert.h"
#include "fx/send_fx.h"
#include "machines.h"
#include "keys.h"
#include "poly.h"
#include "seq/rytm_dump.h"
#include "seq/seq.h"
#include "seq/rng.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;

// ---- the Rytm side, simulated: its frames run with the host's render frames (one clock, as Overbridge locks them);
// each tick is stamped with its audio block and arrives `delay` host frames after it fired.
struct Msg {
  int64_t at;   // host frame it arrives
  std::vector<uint8_t> m;
};
static std::vector<Msg> g_wire;
static uint32_t g_rng = 1;
static int jitter(int max) {
  g_rng = g_rng * 1103515245u + 12345u;
  return (int)((g_rng >> 8) % (uint32_t)(max + 1));
}
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
static void wire(int64_t at, const std::vector<uint8_t>& m) { g_wire.push_back({at, m}); }
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
static void state(int64_t at, std::vector<uint8_t> body) { wire(at, msg(0x2D, body)); }
static void direct_state(std::vector<uint8_t> body) {
  auto m = msg(0x2D, body);
  engine_sysex(m.data(), (int)m.size());
}

// A fired note-on: when (host frame) and the frame the planner had for it.
struct Fired {
  int64_t h, planned;
  int note;
};
static std::vector<Fired> g_fired;
static void (*g_each)() = nullptr;   // seq-ux: called after each block's pre_block
static void block() {
  int64_t f[512];
  uint8_t on[512], vo[512], no[512];
  int n = seq::pending(f, on, vo, no, 512);
  uint32_t before = seq::stats().notes_on;
  int64_t h = (int64_t)seq::frames();
  seq::pre_block();
  if (g_each) g_each();
  g_us += 250;   // 12 frames at 48 kHz: the core's clock with the render
  host_fake_time(true, g_us);
  if (seq::stats().notes_on == before) return;
  int64_t f2[512];
  uint8_t on2[512], vo2[512], no2[512];
  int n2 = seq::pending(f2, on2, vo2, no2, 512);
  for (int i = 0; i < n; ++i) {   // the ons that went
    if (!on[i]) continue;
    bool still = false;
    for (int j = 0; j < n2 && !still; ++j) still = on2[j] && f2[j] == f[i] && no2[j] == no[i];
    if (!still) g_fired.push_back({h, f[i], no[i]});
  }
}
// Runs the Rytm and the host for `frames`: the ticks of track `track` (length len) from frame t0 at `period`, with
// `delay` + jitter(jit) arrival; each host block delivers what arrived, then pre_block.
struct Run {
  int track = 0, len = 16, speed = 2;
  double period = 6000;   // 120 BPM: 48000 * 60 / 120 / 4
  int64_t t0 = 0;
  int delay = 700, jit = 400;
  int64_t next_k = 0;
};
static void run(Run& r, int64_t until) {
  while ((int64_t)seq::frames() < until) {
    int64_t h = (int64_t)seq::frames();
    // ticks fired up to now on the Rytm (one block of messages per 32-frame block: n 1 here)
    for (;;) {
      int64_t tf = r.t0 + (int64_t)llround(r.next_k * r.period);
      if (tf > h + 2) break;
      int step = (int)(r.next_k % r.len);
      wire(tf + r.delay + jitter(r.jit),
           msg(0x2C, [&] { auto b = blk4(tf); b.insert(b.end(), {1, (uint8_t)r.track, (uint8_t)step, (uint8_t)r.len, (uint8_t)r.speed}); return b; }()));
      ++r.next_k;
    }
    deliver(h);
    block();
  }
}

// ---- PNGs of the sent frame (the Rytm's text in the host's font), as test_chord_view.cpp's
static uint32_t crc32_of(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
static std::vector<uint8_t> zstore(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t at = 0; at < raw.size();) {
    size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8)), z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + (long)at, raw.begin() + (long)(at + n));
    at += n;
  }
  for (uint8_t x : raw) a = (a + x) % 65521, b = (b + a) % 65521;
  uint32_t ad = b << 16 | a;
  for (int i = 3; i >= 0; --i) z.push_back((uint8_t)(ad >> (8 * i)));
  return z;
}
static void put_be32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
static void chunk(std::vector<uint8_t>& f, const char* t, const std::vector<uint8_t>& d) {
  put_be32(f, (uint32_t)d.size());
  std::vector<uint8_t> td(t, t + 4);
  td.insert(td.end(), d.begin(), d.end());
  f.insert(f.end(), td.begin(), td.end());
  put_be32(f, crc32_of(td.data(), td.size()));
}
static std::string g_dir;
static void png(const char* name) {
  uint8_t fb[kBytes];
  preview_sent(fb);
  const int s = 4, w = 128 * s, h = 64 * s;
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      int X = x / s, Y = y / s;
      bool on = fb[(7 - Y / 8) + 8 * X] >> (Y % 8) & 1;
      int c[3] = {on ? 232 : 12, on ? 238 : 14, on ? 255 : 22};
      bool grid = x % s == s - 1 || y % s == s - 1;
      for (int k = 0; k < 3; ++k) raw.push_back((uint8_t)(grid ? c[k] * 8 / 10 : c[k]));
    }
  }
  std::vector<uint8_t> z = zstore(raw);
  std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, ih;
  put_be32(ih, w), put_be32(ih, h);
  ih.insert(ih.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ih);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  std::string path = g_dir + "/" + name;
  FILE* fp = fopen(path.c_str(), "wb");
  CHECK(fp);
  fwrite(f.data(), 1, f.size(), fp);
  fclose(fp);
  printf("  %s\n", path.c_str());
}
static bool shown(const char* s) {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  for (int i = 0; i < n; ++i)
    if (strstr((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
static int px(int x, int y) { return remote_frame()[(7 - y / 8) + 8 * x] >> (y % 8) & 1; }
static void report(int vb, std::vector<int> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)vb};
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(k < (int)p.size() ? p[k] : 0)), m.push_back(0);
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void key(int code, bool down) {
  auto m = msg(0x31, {(uint8_t)(code >> 7), (uint8_t)(code & 0x7F), (uint8_t)down});
  engine_sysex(m.data(), (int)m.size());
}
static void enc(int e, int d, bool func = false) {
  auto m = msg(0x32, {(uint8_t)e, (uint8_t)(d + 64), (uint8_t)func});
  engine_sysex(m.data(), (int)m.size());
}
static void advance_ms(int ms) {
  g_us += (uint64_t)ms * 1000;
  host_fake_time(true, g_us);
}
// 2026-10-26, the RAM store (phase 2): the sequencer reads its files from the store a project load fills (pool.h), so a
// project change is loaded once the idle pass has filled it; refill(): the project loaded again (a file the test wrote
// on the card is seen, as the Sidecar sees it after a load)
static void loaded() {
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
static void refill() {
  for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();   // every write landed first
  project_fill_begin();
  loaded();
}
static std::string cmd(const char* c) {
  char b[900];
  CHECK(seq::command(c, b, sizeof b));
  return b;
}
// seq-p2: 2D 0A EDIT (op, pattern, track, ok, src, arg)
static void edit(int op, int p, int t, int ok, int src = 0, int arg = 0) {
  direct_state({10, (uint8_t)op, (uint8_t)p, (uint8_t)t, (uint8_t)ok, (uint8_t)src, (uint8_t)(arg & 0x7F),
                (uint8_t)(arg >> 7 & 0x7F), (uint8_t)(arg >> 14 & 0x7F)});
}
static int used(int rytm) {
  const seq::Track* t = seq::edit_track(rytm, false);
  return t ? seq::used_steps(*t) : 0;
}
static std::vector<std::vector<uint8_t>> sent(uint8_t cmd_) {
  std::vector<std::vector<uint8_t>> o;
  for (auto& m : g_out)
    if (m.size() > 5 && m[4] == cmd_) o.push_back(m);
  return o;
}
// a stock pattern dump of rec (sim/protocol.py rytm_dump)
static std::vector<uint8_t> dump_of(const std::vector<uint8_t>& rec, int pattern) {
  std::vector<uint8_t> m = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x54, 0x01, 0x01, (uint8_t)pattern, 0};
  m.insert(m.end(), rec.begin(), rec.begin() + 4);
  for (size_t i = 4; i < rec.size(); i += 7) {
    uint8_t hi = 0;
    for (size_t k = 0; k < 7 && i + k < rec.size(); ++k) hi |= (uint8_t)((rec[i + k] >> 7) << (6 - k));
    m.push_back(hi);
    for (size_t k = 0; k < 7 && i + k < rec.size(); ++k) m.push_back(rec[i + k] & 0x7F);
  }
  uint32_t cs = 0;
  for (size_t i = 10; i < m.size(); ++i) cs += m[i];   // after the pattern number
  cs &= 0x3FFF;
  m.push_back((uint8_t)(cs >> 7)), m.push_back((uint8_t)(cs & 0x7F));
  int n = (int)m.size() + 3 - 10;
  m.push_back((uint8_t)(n >> 7 & 0x7F)), m.push_back((uint8_t)(n & 0x7F)), m.push_back(0xF7);
  return m;
}

static void test_model() {
  seq::Step s = {};
  CHECK(seq::add_note(s, {60, 100, 48, 0, 0}) && s.n == 1);
  CHECK(seq::add_note(s, {60, 90, 24, 5, 0}) && s.n == 1 && s.note[0].vel == 90 && s.note[0].micro == 5);   // replaced
  for (int p = 61; p < 70; ++p) seq::add_note(s, {(uint8_t)p, 100, 48, 0, 0});
  CHECK(s.n == seq::kNotes && !seq::add_note(s, {80, 1, 1, 0, 0}));   // full
  CHECK(seq::remove_pitch(s, 63) && s.n == seq::kNotes - 1 && !seq::remove_pitch(s, 63));
  seq::set_chord(s, 60, 6, 0, 99, 72, 0);   // C MAJ7: C E G B
  CHECK(s.n == 4 && s.note[0].pitch == 60 && s.note[3].pitch == 71 && s.note[2].vel == 99 && s.note[1].len == 72);
  seq::apply_strum(s, 8);   // up: 0 8 16 24 by pitch
  for (int k = 0; k < 4; ++k) CHECK(s.note[k].micro == 8 * k);
  seq::apply_strum(s, -10);   // down: the highest first
  CHECK(s.note[3].micro == 0 && s.note[0].micro == 30 && s.strum == -10);
  seq::set_chord(s, 60, 1, 1, 100, 96, 0);   // C MAJ INV1: E G C
  CHECK(s.n == 3 && s.note[0].pitch == 64 && s.note[2].pitch == 72);

  // the file: round trip, sparse, the CRC and the version refused
  static seq::Pattern p, q;
  seq::clear(p, 5);
  seq::Track* t = seq::track_of(p, 4, true);
  CHECK(t && seq::track_of(p, 4, false) == t);
  t->on = 1;   // (per pattern poly: poly tracks here - the old-file rule says so, no kBlkOn)
  t->length = 96;
  seq::set_chord(t->step[0], 57, 2, 0, 100, 96 * 4, 6);
  seq::add_note(t->step[95], {72, 80, 30, -40, 0});
  seq::Track* t2 = seq::track_of(p, 0, true);
  t2->on = 1;
  seq::add_note(t2->step[3], {36, 127, 9, 0, 0});
  static uint8_t buf[1 << 16];
  int n = seq::pack(p, buf, sizeof buf);
  CHECK(n > 12 && n < 120);   // sparse: 3 steps
  CHECK(seq::unpack(q, 5, buf, n));
  const seq::Track* u = seq::track_of(q, 4);
  CHECK(u && u->length == 96 && u->step[0].n == 3 && u->step[0].strum == 6 && u->step[0].note[2].micro == 12 &&
        u->step[0].note[0].len == 384 && u->step[95].note[0].micro == -40 && u->step[95].note[0].pitch == 72);
  CHECK(seq::track_of(q, 0)->step[3].note[0].vel == 127 && seq::used_steps(*seq::track_of(q, 0)) == 1);
  buf[20] ^= 1;
  CHECK(!seq::unpack(q, 5, buf, n) && !seq::track_of(q, 4));   // corrupt: empty
  buf[20] ^= 1;
  buf[4] = 5;   // rui1040: v3 (trig conditions) reads; seqx: v4 (E2's blocks) too; a later version does not
  CHECK(!seq::unpack(q, 5, buf, n));
  buf[4] = 2;
  CHECK(seq::pack(p, buf, 20) == 0);   // too small
  printf("  model + file: %d bytes for 3 steps\n", n);
  // seq-ux: v1 (the MVP's file, no locks) still reads; v2 keeps the locks, a lock trig (locks, no notes) too
  {
    int n1 = seq::pack(p, buf, sizeof buf);
    buf[4] = 1;
    uint32_t c = seq::crc32(buf + 12, n1 - 12);
    for (int k = 0; k < 4; ++k) buf[8 + k] = (uint8_t)(c >> (8 * k));
    CHECK(seq::unpack(q, 5, buf, n1) && seq::track_of(q, 4) && seq::track_of(q, 4)->step[0].n == 3);
    seq::Step& ls = t->step[7];
    CHECK(seq::lock_of(ls, 3) == -1 && seq::set_lock(ls, 3, 100 << 7 | 64) && seq::lock_of(ls, 3) == (100 << 7 | 64));
    CHECK(seq::set_lock(ls, 3, 90 << 7) && ls.nl == 1 && seq::lock_of(ls, 3) == 90 << 7);   // replaced
    CHECK(seq::set_lock(ls, 17, 5 << 7) && seq::set_lock(t->step[0], 1, 20 << 7) && !seq::set_lock(ls, seq::kLockParams, 0));   // (fx-ins: 24..31 the INSERT params)
    for (int k = 0; k < seq::kLocks; ++k) seq::set_lock(t->step[95], k, k << 7);
    CHECK(t->step[95].nl == seq::kLocks && !seq::set_lock(t->step[95], 20, 0));   // full
    CHECK(seq::clear_lock(t->step[95], 0) && !seq::clear_lock(t->step[95], 0) && seq::lock_of(t->step[95], 1) == 1 << 7);
    CHECK(seq::has_locks(*t) && !seq::has_locks(*seq::track_of(p, 0)));
    int n2 = seq::pack(p, buf, sizeof buf);
    CHECK(n2 > 0 && buf[4] == 2 && seq::unpack(q, 5, buf, n2));
    const seq::Track* u2 = seq::track_of(q, 4);
    CHECK(u2 && u2->step[7].n == 0 && u2->step[7].nl == 2 && seq::lock_of(u2->step[7], 3) == 90 << 7 &&
          seq::lock_of(u2->step[7], 17) == 5 << 7 && seq::lock_of(u2->step[0], 1) == 20 << 7 && u2->step[0].n == 3 &&
          u2->step[95].nl == seq::kLocks - 1 && seq::used_steps(*u2) == 3);
    printf("  locks: v1 read, v2 %d bytes (2 + 1 + 11 locks)\n", n2);
  }
}

static void test_clock() {
  // the follower: arrivals 700 + 0..400 frames after their stamps (block-quantised): the offset settles on the fastest
  seq::Follower fo;
  fo.reset();
  int64_t base = ((1 << 28) - 300) * 32LL;   // across the 28-bit wrap
  for (int i = 0; i < 200; ++i) {
    int64_t f = base + i * 6000LL;
    uint32_t blk = (uint32_t)((f / 32) & ((1 << 28) - 1));
    int64_t rf = fo.unwrap(blk) * 32;
    fo.sample(f + 700 + jitter(400) + 5000000, rf);
  }
  int64_t want = 700 + 5000000 + (base - base / 32 * 32);
  CHECK(fo.valid && fo.offset >= want - 40 && fo.offset <= want + 40);
  CHECK(fo.last_blk > (1 << 28));   // unwrapped past the wrap
  // the step clock: 127 BPM (5669.29 frames a step), stamps quantised to 32 frames: within a block of the truth
  seq::StepClock c;
  double per = seq::step_frames(127.0, 2);
  CHECK(std::fabs(per - 5669.29) < 0.01);
  double worst = 0;
  for (int k = 0; k < 64; ++k) {
    double truth = 1000.0 + k * per;
    double stamp = std::floor(truth / 32) * 32;
    c.tick(k % 16, 16, 2, stamp, per, k == 0);
    if (k > 8) {
      double e = std::fabs(c.frame_of((double)c.abs + 1) - (truth + per) + 16);
      if (e > worst) worst = e;
    }
  }
  CHECK(c.abs == 63 && worst < 24 && c.jumps == 0);
  // a 2x track: two ticks a 1x step, its period half
  seq::StepClock d;
  for (int k = 0; k < 8; ++k) d.tick(k % 32, 32, 0, k * per / 2, per, k == 0);
  CHECK(std::fabs(d.period - per / 2) < 0.01);
  // no tempo known: measured; a dropped tick: two steps advanced, no jump
  seq::StepClock e;
  for (int k = 0; k < 10; ++k) e.tick(k, 16, 2, k * 6000.0, 0, k == 0);
  CHECK(std::fabs(e.period - 6000) < 1);
  CHECK(e.tick(12, 16, 2, 12 * 6000.0, 0, false) == 3 && e.jumps == 0 && e.abs == 12);
  // a jump (the Rytm's clock moved: a tempo set while playing): re-anchored
  e.tick(13, 16, 2, 13 * 6000.0 + 4000, 0, false);
  CHECK(e.anchor == 13 * 6000.0 + 4000 && e.anchor_abs == 13);
  printf("  clock: offset %lld (want %lld), step clock worst %.1f frames\n", (long long)fo.offset, (long long)want, worst);
}

int main() {
  const char* env = getenv("SEQ_OUT");
  g_dir = env && *env ? env : "build/seq";
  mkdir("build", 0755);
  mkdir(g_dir.c_str(), 0755);
  test_model();
  test_clock();

  char tmpl[] = "/tmp/core_seq_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'E', 'Q', 'T', 'E', 'S', 'T', 0xF7};
  engine_sysex(proj, sizeof proj);
  loaded();   // the project's store filled, then its sequences loaded from it
  report(kFm | 0, {0, 64, 64, 127, 64, 64, 100, 127});   // BD's voice 0 on 6-OP FM
  CHECK(v[0].fm);

  // ---- the mask: 2E at the first tick, again (with a resync) after a hello
  CHECK(cmd("seq=track:1:on").find("mask 0x001") != std::string::npos);
  CHECK(seq::owns_voice(0) && !seq::owns_voice(1));
  g_out.clear();
  seq::tick(1000);
  CHECK(g_out.size() >= 1 && g_out[0].size() == 10 && g_out[0][4] == 0x2E && g_out[0][6] == 1 && g_out[0][7] == 0 &&
        g_out[0][8] == 2);
  g_out.clear();
  seq::tick(1010);
  for (auto& m : g_out) CHECK(m[4] != 0x2E);
  const uint8_t hello[] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 5, 1, 0xF7};
  engine_sysex(hello, sizeof hello);
  g_out.clear();
  seq::tick(1020);
  CHECK(!g_out.empty() && g_out[0][4] == 0x2E && g_out[0][8] == 2);

  // ---- a pattern: C3 on step 0, E3 half a step late on 1, G3 a quarter early on 2, a strummed C MAJ7 on 4
  seq::Track* t = seq::edit_track(0, true);
  CHECK(t);
  t->length = 16;
  seq::add_note(t->step[0], {48, 100, 48, 0, 0});
  seq::add_note(t->step[1], {52, 100, 48, 48, 0});
  seq::add_note(t->step[2], {55, 100, 48, -24, 0});
  seq::set_chord(t->step[4], 60, 6, 0, 100, 96, 12);   // 0 12 24 36
  seq::touched();
  int lat = 192;
  seq::set_latency(lat);
  direct_state({7, (uint8_t)(120 * 120 >> 14), (uint8_t)(120 * 120 >> 7 & 0x7F), (uint8_t)(120 * 120 & 0x7F)});   // 120 BPM

  // ---- play: PLAY and the first tick at Rytm frame 48000 (arriving 700..1100 later); 4 bars
  Run r;
  r.t0 = 48000;
  r.next_k = 0;
  while ((int64_t)seq::frames() < 40000) block();
  state(r.t0 + r.delay, [&] { std::vector<uint8_t> b = {1, 1}; auto x = blk4(r.t0); b.insert(b.end(), x.begin(), x.end()); return b; }());
  g_fired.clear();
  run(r, r.t0 + (int64_t)(64 * r.period) + 2000);
  CHECK(seq::view().transport == seq::kPlay);
  // every note: fired at its planned frame (within half a block) and planned where the truth says (within 48 frames:
  // the fastest arrival + the stamps' block)
  int64_t off = seq::stats().offset;
  printf("  offset %lld\n", (long long)off);
  CHECK(off >= r.delay && off <= r.delay + 80);   // the fastest arrival seen: + the stamp block + the delivery block
  int checked = 0, ahead = 0;
  double worst = 0;
  for (const Fired& f : g_fired) {
    CHECK(std::llabs(f.h - f.planned) <= kBlock / 2 + 1 || f.planned < r.t0 + 2 * r.period);
    // which step / note: from the pitch
    int step = f.note == 48 ? 0 : f.note == 52 ? 1 : f.note == 55 ? 2 : 4;
    int micro = f.note == 52 ? 48 : f.note == 55 ? -24 : f.note == 60 ? 0 : f.note == 64 ? 12 : f.note == 67 ? 24 : f.note == 71 ? 36 : 0;
    double rel = (double)(f.planned - r.t0 + lat - off) - micro * r.period / 96;
    double k = std::round(rel / r.period);
    if ((int)k % 16 != step) continue;
    if (k < 16) continue;   // the first bar: the clock still settling (and step 0 after PLAY is late by design)
    // the Rytm plays a step at the start of the audio block its tick fired in: that block is the truth
    double tb = std::floor((r.t0 + k * r.period) / 32) * 32 - r.t0;
    double e = std::fabs(rel - tb);
    if (e > worst) worst = e;
    ++checked;
    // a note started when its step's tick arrives (+ its micro): the earliest that could be, without the planner
    int64_t arrives = r.t0 + (int64_t)(k * r.period + micro * r.period / 96) + r.delay;
    if (f.h < arrives) ++ahead;
  }
  printf("  playback: %d notes checked, worst %.1f frames from the truth, %d fired before their tick could arrive, late %u\n",
         checked, worst, ahead, seq::stats().late);
  CHECK(checked >= 3 * 7 && worst <= 48 && ahead == checked);
  CHECK(seq::stats().late >= 1 && seq::stats().late <= 2);   // step 0 after PLAY only

  // ---- the strum: the chord's notes 12/96 of a step apart
  {
    int64_t s60 = -1, s71 = -1;
    for (const Fired& f : g_fired) {
      if (f.note == 60 && f.planned > r.t0 + 32 * r.period) s60 = s60 < 0 ? f.planned : s60;
      if (f.note == 71 && f.planned > r.t0 + 32 * r.period) s71 = s71 < 0 ? f.planned : s71;
    }
    CHECK(s60 > 0 && s71 > 0 && std::llabs((s71 - s60) - (int64_t)(36 * r.period / 96)) <= 2);
  }

  // ---- LEDs on the shown track (BD): notes blue, the chord cyan, the playing step white
  {
    uint8_t c[16];
    CHECK(seq::leds(c));
    int ps = seq::view().play_step;
    CHECK(ps >= 0);
    for (int k = 0; k < 16; ++k) {
      int want = k == ps ? 111 : k == 4 ? 79 : k < 3 ? 47 : 0;
      CHECK(c[k] == want);
    }
    uint8_t p4[4];
    seq::page_leds(p4);
    CHECK(p4[0] == 47 && p4[1] == 0);
  }

  // ---- mutes: BD muted: no new notes, sounding ones end
  {
    uint32_t on0 = seq::stats().notes_on;
    state((int64_t)seq::frames(), {5, 1, 0});
    run(r, (int64_t)seq::frames() + (int64_t)(16 * r.period));
    CHECK(seq::stats().notes_on == on0);
    state((int64_t)seq::frames(), {5, 0, 0});
    run(r, (int64_t)seq::frames() + (int64_t)(16 * r.period));
    CHECK(seq::stats().notes_on > on0);
  }

  // ---- live record: channel 1 bound to voice 0 (a host keyboard), REC on; a note 0.3 step after step 6, held 1.5
  // steps: step 6, micro +29, len 144; quantise 1/16: micro 0
  {
    char b[200];
    CHECK(poly::command("poly=bind:1:0", b, sizeof b));
    state((int64_t)seq::frames(), {6, 1, 0});
    run(r, (int64_t)seq::frames() + 10);
    // wait for step 5 of a bar to have ticked, then aim at 6.3 (Rytm frame -> host frame = + offset)
    int64_t k0 = (r.next_k / 16 + 1) * 16;   // the next bar
    int64_t at = r.t0 + (int64_t)((k0 + 6.3) * r.period) + seq::stats().offset;
    run(r, at);
    poly::midi(0x90, 62, 110);   // through poly's note sink (as the Pi's bridge)
    run(r, at + (int64_t)(1.5 * r.period));
    poly::midi(0x80, 62, 0);
    const seq::Step& s6 = seq::edit_track(0, false)->step[6];
    CHECK(s6.n == 1 && s6.note[0].pitch == 62 && s6.note[0].vel == 110);
    printf("  record OFF: step 7, micro %d, len %d\n", s6.note[0].micro, s6.note[0].len);
    CHECK(std::abs(s6.note[0].micro - 29) <= 2 && std::abs((int)s6.note[0].len - 144) <= 3);
    CHECK(cmd("seq=quant:1").find("1/16") != std::string::npos);
    k0 = (r.next_k / 16 + 1) * 16;
    at = r.t0 + (int64_t)((k0 + 8.7) * r.period) + seq::stats().offset;   // nearest: step 9, early
    run(r, at);
    seq::midi(0x90, 65, 90);
    run(r, at + (int64_t)(0.5 * r.period));
    seq::midi(0x80, 65, 0);
    const seq::Step& s9 = seq::edit_track(0, false)->step[9];
    CHECK(s9.n == 1 && s9.note[0].pitch == 65 && s9.note[0].micro == 0);
    // the same from a keyboard on the Pi (core/keys.h, by channel: channel 1 = BD): recorded as the notes are heard,
    // the seq track's own voice playing them (nothing to the Rytm: it does not record a seq track)
    keys::reset();
    CHECK(keys::command("keys=channel", b, sizeof b));
    k0 = (r.next_k / 16 + 1) * 16;
    at = r.t0 + (int64_t)((k0 + 11.1) * r.period) + seq::stats().offset;
    run(r, at);
    keys::midi(0, 0x90, 70, 100);
    keys::midi(0, 0x90, 74, 100);
    CHECK(poly::held_notes(0) >= 2 && keys::stats().forwarded == 0);
    run(r, at + (int64_t)(0.5 * r.period));
    keys::midi(0, 0x80, 70, 0), keys::midi(0, 0x80, 74, 0);
    const seq::Step& s11 = seq::edit_track(0, false)->step[11];
    CHECK(s11.n == 2 && s11.note[0].pitch == 70 && s11.note[1].pitch == 74 && s11.note[0].micro == 0);
    keys::reset();
    cmd("seq=quant:0");
    state((int64_t)seq::frames(), {6, 0, 0});
    run(r, (int64_t)seq::frames() + 100);
    CHECK(seq::stats().recorded == 4);
  }

  // ---- pause / continue / stop
  {
    // the platform hears each change as MIDI realtime (hooks.transport: the Pi's recorder arms on it, whatever the
    // Rytm's TRANSPORT SEND says): pause -> FC, continue -> FB, once each
    static std::vector<uint8_t> tr;
    core::hooks.transport = [](uint8_t st) { tr.push_back(st); };
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> b = {1, 2}; auto x = blk4(now - r.delay); b.insert(b.end(), x.begin(), x.end()); return b; }());
    deliver(now);
    block();
    int64_t f[64];
    uint8_t on[64], vo[64], no[64];
    CHECK(seq::pending(f, on, vo, no, 64) == 0 && seq::view().transport == seq::kPause);
    // continue: the Rytm resumes 3 bars later from where it stopped
    int64_t gap = (int64_t)(48 * r.period);
    while ((int64_t)seq::frames() < now + gap) block();
    r.t0 += gap;
    state((int64_t)seq::frames(), [&] { std::vector<uint8_t> b = {1, 1}; auto x = blk4((int64_t)seq::frames() - r.delay); b.insert(b.end(), x.begin(), x.end()); return b; }());
    uint32_t on0 = seq::stats().notes_on;
    run(r, (int64_t)seq::frames() + (int64_t)(32 * r.period));
    CHECK(seq::stats().notes_on > on0 && seq::view().transport == seq::kPlay);
    CHECK(tr.size() == 2 && tr[0] == 0xFC && tr[1] == 0xFB);
    // STOP -> FC; then the Rytm's ticks before (or without) its TRANSPORT report: seq plays from them -> FA, once
    state((int64_t)seq::frames(), [&] { std::vector<uint8_t> b = {1, 0}; auto x = blk4((int64_t)seq::frames() - r.delay); b.insert(b.end(), x.begin(), x.end()); return b; }());
    deliver((int64_t)seq::frames());
    block();
    CHECK(seq::view().transport == seq::kStop && tr.size() == 3 && tr[2] == 0xFC);
    for (int k = 0; k < 4; ++k) block();
    r.t0 = (int64_t)seq::frames() + 100, r.next_k = 0;
    run(r, (int64_t)seq::frames() + (int64_t)(8 * r.period));
    printf("transport hook: %zu calls, last %02X\n", tr.size(), tr.empty() ? 0 : tr.back());
    CHECK(seq::view().transport == seq::kPlay && tr.size() == 4 && tr[3] == 0xFA);
    core::hooks.transport = nullptr;
  }

  // ---- step entry (seq mode on BD): T3 tapped = a step with the last notes played; T6 held + pads = its chord; held
  // + E (CHD) + G (STRUM); T1 tapped on a step with notes = cleared
  {
    direct_state({8, 1, 0});
    CHECK(seq::view().seqmode && seq::view().track == 0);
    seq::view_set(0, 0, -1, false);
    seq::midi(0x90, 67, 100), seq::midi(0x80, 67, 0);   // the last chord: G3
    advance_ms(200);
    seq::Track* bt = seq::edit_track(0, false);
    bt->step[2].n = 0;
    key(2, true), advance_ms(80), key(2, false);
    printf("  tap: n %d pitch %d keys %lu\n", bt->step[2].n, bt->step[2].note[0].pitch, (unsigned long)remote_stats().keys);
    CHECK(bt->step[2].n == 1 && bt->step[2].note[0].pitch == 67);
    key(5, true);
    advance_ms(100);
    seq::midi(0x90, 57, 100), seq::midi(0x90, 60, 90), seq::midi(0x90, 64, 80);
    CHECK(bt->step[5].n == 3 && bt->step[5].note[0].pitch == 57 && bt->step[5].note[2].vel == 80);
    seq::midi(0x80, 57, 0), seq::midi(0x80, 60, 0), seq::midi(0x80, 64, 0);
    enc(4, 2);   // CHD: OFF -> MIN (2) from the root under the lowest note (A)
    CHECK(bt->step[5].chd == 2 && bt->step[5].n == 3 && bt->step[5].note[0].pitch == 57 && bt->step[5].note[1].pitch == 60);
    enc(6, 4);   // STRUM +8
    CHECK(bt->step[5].strum == 8 && bt->step[5].note[2].micro == 16);
    enc(3, 3);   // MICRO +6 on all
    CHECK(bt->step[5].note[0].micro == 6 && bt->step[5].note[2].micro == 22);
    enc(7, 2);   // SEL the second note, then NOTE +1 on it alone
    enc(0, 1);
    CHECK(bt->step[5].note[1].pitch == 61 && bt->step[5].note[0].pitch == 57);
    enc(7, -2);
    enc(2, 4);   // LEN + 4/16 step on all
    CHECK(bt->step[5].note[0].len == 96 + 24 || bt->step[5].note[0].len > 24);
    uint8_t c[16];
    seq::leds(c);
    CHECK(c[5] == 63);   // held: amber
    key(5, false);
    advance_ms(600);
    CHECK(bt->step[5].n == 3);   // held long and used: not toggled
    key(0, true), advance_ms(50), key(0, false);
    CHECK(bt->step[0].n == 0);
    // dbg (hardware report, Pi fe1e176: "a new chord made by holding a trig and turning comes out at C0"): an empty step
    // held, nothing played for a while, the knobs turned: the step starts at the track's default note (60), A moves it,
    // the chord (E) is built on it
    {
      advance_ms(500);
      bt->step[9].n = 0, bt->step[10].n = 0, bt->dnote = 60;
      seq::midi(0x99, 0, 100), seq::midi(0x89, 0, 0);   // the Rytm's own drum note (an analog track's channel: no voice)
      advance_ms(500);
      printf("  fresh: dnote %d chord_n %d\n", bt->dnote, seq::view().held);
      key(9, true), advance_ms(30);
      enc(4, 1);   // CHD OFF -> MAJ on an empty step
      printf("  fresh CHD: n %d pitches %d %d %d chd %d\n", bt->step[9].n, bt->step[9].note[0].pitch,
             bt->step[9].note[1].pitch, bt->step[9].note[2].pitch, bt->step[9].chd);
      CHECK(bt->step[9].n == 3 && bt->step[9].note[0].pitch == 60 && bt->step[9].note[1].pitch == 64 &&
            bt->step[9].note[2].pitch == 67);
      key(9, false), advance_ms(500);
      key(10, true), advance_ms(30);
      enc(0, 2), enc(1, 3), enc(4, 2);   // A +2 (D), B +3, CHD MIN
      printf("  fresh A B CHD: n %d pitches %d %d %d\n", bt->step[10].n, bt->step[10].note[0].pitch,
             bt->step[10].note[1].pitch, bt->step[10].note[2].pitch);
      CHECK(bt->step[10].n == 3 && bt->step[10].note[0].pitch == 62 && bt->step[10].note[1].pitch == 65);
      key(10, false), advance_ms(500);
      // a tap takes the last chord a Sidecar voice played (57 60 64 above), never the Rytm's own drum note
      seq::midi(0x99, 0, 100), seq::midi(0x89, 0, 0);
      bt->step[12].n = 0;
      key(12, true), advance_ms(80), key(12, false);
      CHECK(bt->step[12].n == 3 && bt->step[12].note[0].pitch == 57);
      bt->step[12].n = 0, bt->step[9].n = 0, bt->step[10].n = 0;
      // a pattern file whose track default note is 0 (unset): 60 when loaded
      {
        static seq::Pattern pa, pb;
        pa = seq::edit_pattern();
        seq::Track* ta = seq::track_of(pa, 0, false);
        CHECK(ta);
        ta->dnote = 0;
        static uint8_t f[200000];
        int n = seq::pack(pa, f, (int)sizeof f);
        CHECK(n > 0 && seq::unpack(pb, 0, f, n));
        CHECK(seq::track_of(pb, 0, false) && seq::track_of(pb, 0, false)->dnote == 60);
      }
    }
    // PAGE held: the page map; PAGE + T2 on a one-page track does nothing; a plain PAGE stays on page 1
    key(0x84, true);
    seq::leds(c);
    CHECK(c[0] == 47 || c[0] == 111);
    CHECK(c[1] == 0);
    CHECK(seq::view().page_held);   // UX review S4: the page's footer names the LED colours meanwhile (page_seq.cpp)
    key(0x84, false);
    CHECK(seq::view().page == 0);
  }

  // ---- the SEQ page: the remote page opened (30), SEQ shown; the roll with the playhead, then a held step
  {
    seq::Track* bt = seq::edit_track(0, false);
    bt->length = 64;
    cmd("seq=demo:1");
    seq::view_set(0, 0, -1, true);
    const uint8_t open[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 1, 0, 0xF7};
    engine_sysex(open, sizeof open);
    remote_show_page("SEQ");
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
    run(r, (int64_t)seq::frames() + (int64_t)(2.4 * r.period));
    for (int i = 0; i < 4; ++i) remote_tick((uint32_t)(5000 + 30 * i)), advance_ms(30);
    CHECK(shown("SIDECAR SEQ") && shown("PG") && shown("PLAY"));
    CHECK(px(1 + 1, 19 + 1));   // step 1 of the strip: notes
    png("seq_roll.png");
    // T1 held: the keyboard, the lane, the values
    key(0, true);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5200 + 30 * i)), advance_ms(30);
    CHECK(shown("N C4") && shown("MIN") && shown("S+16"));
    png("seq_held.png");
    enc(6, 3);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5400 + 30 * i)), advance_ms(30);
    CHECK(shown("S+22") && shown("STRUM"));
    png("seq_held_strum.png");
    {   // rui1046 (the docs' renders): the poly track's TRIG view, T1 held - the notes view (as above), TRIG: the trig
        // features, COND turned to a condition; TRIG again: the notes. The step restored after.
      seq::Step keep = bt->step[0];
      png("trig_1_notes_view.png");
      key(0x30, true), key(0x30, false);
      for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5460 + 30 * i)), advance_ms(30);
      CHECK(seq::view().trigview && shown("COND") && shown("S1 TRIG"));
      enc(4, 9);
      for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5560 + 30 * i)), advance_ms(30);
      CHECK(seq::cond_of(bt->step[0]) != 0 && shown("STEP COND:"));
      png("trig_2_features_cond.png");
      // rui1050: TRIG under the held trig cycles TRIG -> RTRG -> FX -> the notes
      key(0x30, true), key(0x30, false);
      for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5660 + 30 * i)), advance_ms(30);
      CHECK(seq::view().tpage == seq::kSvRtrg && shown("SEQ RTRG STEP") && shown("CNT"));
      key(0x30, true), key(0x30, false);
      for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5700 + 30 * i)), advance_ms(30);
      CHECK(seq::view().tpage == seq::kSvFx && shown("SEQ FX STEP") && shown("CMD1"));
      key(0x30, true), key(0x30, false);
      for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5740 + 30 * i)), advance_ms(30);
      CHECK(!seq::view().trigview);
      bt->step[0] = keep;
    }
    {   // chrorange: the held keyboard names each octave's C and keeps its window while the notes fit: a lone CHRO pad
        // note at the bottom (C2, 36) moves it down, the top (B5, 83) up by the fewest octaves, a note inside it stays
      seq::Step keep = bt->step[0];
      auto lone = [&](int pitch) {
        bt->step[0].n = 1, bt->step[0].note[0].pitch = (uint8_t)pitch;
        for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5800 + 30 * i)), advance_ms(30);
      };
      CHECK(shown("C3") && shown("C4") && shown("C5"));   // the chord C4 MIN: the first window, C3..B5
      lone(36);
      CHECK(shown("C2") && shown("C3") && shown("C4") && !shown("C5") && shown("N C2"));
      CHECK(!px(2, 25 + 6 - 2) && px(2 + 6, 25 + 6 - 2));   // C2's white key marked (the first key), D2's not
      png("seq_held_low.png");
      lone(83);
      CHECK(shown("C3") && shown("C5") && !shown("C2") && shown("N B5"));
      CHECK(!px(1 + 20 * 6 + 1, 25 + 6 - 2));   // B5: the last white key
      png("seq_held_high.png");
      lone(60);
      CHECK(shown("C3") && shown("C5") && shown("N C4"));   // inside the window: it stays
      CHECK(!px(1 + 7 * 6 + 1, 25 + 6 - 2));   // C4: the second octave's first key
      bt->step[0] = keep;
    }
    key(0, false);
    advance_ms(600);
    // the encoders without a held trig: C LEN
    enc(2, -1);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(5600 + 30 * i)), advance_ms(30);
    CHECK(shown("LENGTH:48"));
    enc(2, 1);
    {   // ======== rui1050: the Rytm side of seqtop - the TRIG view's pages, the lock pages' COND / SLIDE, the menu's rows
      static uint32_t tk = 20000;
      auto draw = [&] { for (int i = 0; i < 3; ++i) remote_tick(tk), tk += 30, advance_ms(30); };
      static seq::Pattern keep_p;
      keep_p = seq::edit_pattern();
      CHECK(!seq::view().trigview && remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
      // DEFAULTS: E DIR, F SPD (FUNC: the free ratios), G TLEN
      key(0x30, true), key(0x30, false);
      draw();
      CHECK(seq::view().tpage == seq::kTvDefaults && shown("SEQ TRIG") && shown("DIR") && shown("SPD") && shown("TLEN"));
      enc(4, 2);
      draw();
      CHECK(bt->dir == seq::kDirPend && shown("BD DIR:PEND"));
      enc(5, 1);
      CHECK(bt->speed == seq::kSpd3_2);   // FOLLOW -> 3/2 (Elektron's set)
      enc(5, -1), enc(5, -1, true);
      CHECK(bt->speed == seq::kSpd7_8);   // FUNC: the free ratios too (FOLLOW -> 7/8)
      enc(5, 1, true);
      draw();
      CHECK(bt->speed == seq::kSpdFollow);
      CHECK(shown("DIR PEND  SPD FOLLOW") || shown("BD SPD:FOLLOW"));
      png("rui1050_1_defaults.png");
      enc(6, -1);
      draw();
      CHECK(bt->length == 48 && shown("BD TLEN:48"));
      enc(6, 1);
      // SCALE: A KEY, B SCL, C QNT (the pattern's)
      key(0x30, true), key(0x30, false);
      enc(0, 2), enc(1, 2), enc(2, 1);
      draw();
      CHECK(seq::view().tpage == seq::kTvScale && seq::edit_pattern().key == 2 && seq::edit_pattern().scale == seq::kScaleMin &&
            seq::edit_pattern().qnt == seq::kQntEdit && shown("SEQ SCALE"));
      png("rui1050_2_scale.png");
      // RANDOM: A SEED (FREE fully down; FUNC +-100), B RNG, C WGHT, D MODE, E SRNG (up from OFF: the SEED slot), F SQNT, G SAFF
      key(0x30, true), key(0x30, false);
      draw();
      CHECK(seq::view().tpage == seq::kTvRandom && shown("SEED FREE") && shown("SRNG"));
      enc(0, 1), enc(0, 41);
      CHECK(seq::edit_pattern().seed_fixed && seq::edit_pattern().seed == 41);
      enc(0, 1, true);
      CHECK(seq::edit_pattern().seed == 141);
      enc(0, -1, true);
      enc(4, 1);   // (BD's machine not known yet: no SEED slot - the seq_e2e run has the real one)
      draw();
      CHECK(bt->smod_src == 0 && shown("NO SEED SLOT ON IT"));
      cmd("seq=smod:1:src=12");
      enc(1, 1), enc(2, 1), enc(3, 1), enc(4, 1), enc(5, 1), enc(6, 1);
      CHECK(bt->rnd_oct == 3 && bt->rnd_wght == seq::kWRoot && bt->rnd_mode == seq::kRndSteps && bt->smod_src == 12 &&
            bt->smod_range == seq::kSmodRangeDef + 1 && bt->smod_quant == seq::kSqBar && bt->smod_aff == seq::kAffChance);
      seq::view_set(0, 0, -1, false);
      draw();
      CHECK(shown("SAFF"));
      advance_ms(3000);   // (the message gone; the touched knob's footer stays)
      draw();
      png("rui1050_3_random.png");
      CHECK(shown("BD SAFF:+CHNC"));   // (the message gone: the touched knob)
      CHECK(cmd("seq=x:1").find("seed 41") != std::string::npos);
      enc(4, -40);
      CHECK(bt->smod_src == 0);   // SRNG fully down: OFF
      key(0x30, true), key(0x30, false);
      CHECK(seq::view().tpage == seq::kTvNotes);
      // held trig, TRIG: TRIG -> RTRG (CNT 4) -> FX (JMP 9); NOTE below the lowest: RND
      seq::Step keep1 = bt->step[0];
      key(0, true);
      key(0x30, true), key(0x30, false);
      CHECK(seq::view().tpage == seq::kSvTrig);
      key(0x30, true), key(0x30, false);
      enc(1, 4);
      draw();
      CHECK(seq::view().tpage == seq::kSvRtrg && bt->step[0].rt.cnt == 4 && shown("STEP CNT:4"));
      png("rui1050_4_rtrg.png");
      key(0x30, true), key(0x30, false);
      enc(0, 9), enc(1, 8);
      draw();
      CHECK(seq::view().tpage == seq::kSvFx && bt->step[0].fx[0].cmd == seq::kFxJmp && bt->step[0].fx[0].val == 9 &&
            shown("STEP VAL1:9"));
      png("rui1050_5_fx.png");
      enc(2, 7), enc(3, -2);   // CMD2 OCT (its own value +1), VAL2 -2: -1 (int8 through the knob)
      CHECK(bt->step[0].fx[1].cmd == seq::kFxOct && (int8_t)bt->step[0].fx[1].val == -1);
      enc(2, -7);
      CHECK(bt->step[0].fx[1].cmd == 0);
      key(0x30, true), key(0x30, false);
      CHECK(seq::view().tpage == seq::kSvNotes);
      key(0x30, true), key(0x30, false);   // TRIG again: the NOTE knob down past C-1 -> RND
      int turns = 0;
      while (!(bt->step[0].xf & seq::kXfRnd) && turns < 140) enc(0, -1), ++turns;
      printf("  rui1050: NOTE RND after %d turns down (the lowest note %d)\n", turns, bt->step[0].note[0].pitch);
      CHECK(turns > 5 && turns < 140);
      draw();
      CHECK((bt->step[0].xf & seq::kXfRnd) && bt->rnd_mode && shown("RND"));
      png("rui1050_6_trig_rnd.png");
      enc(0, 1);
      CHECK(!(bt->step[0].xf & seq::kXfRnd));
      // the lock page: SRC; turn A (a lock), a long push (COND, the label A?), turn (1:2), FUNC + push (SLIDE)
      key(0x31, true), key(0x31, false);
      CHECK(seq::view().lock_page == seq::kLpSrc);
      bt->step[0].nl = 0, bt->step[0].slide = 0;
      key(0x70, true);   // a long push with no lock yet: no COND (the message says to turn first)
      advance_ms(450);
      seq::tick(now_ms());
      key(0x70, false);
      CHECK(seq::view().cond_e == -1 && seq::view().msg && seq::lock_of(bt->step[0], 0) < 0);
      advance_ms(2600);   // (the message gone)
      enc(0, 5);
      CHECK(seq::lock_of(bt->step[0], 0) >= 0);
      key(0x70, true);
      advance_ms(450);
      seq::tick(now_ms());
      CHECK(seq::view().cond_e == 0);
      key(0x70, false);
      CHECK(seq::view().cond_e == 0 && seq::lock_of(bt->step[0], 0) >= 0);   // (a long push keeps the lock)
      enc(0, 5);   // --- FILL !FILL 1ST !1ST 1:2
      draw();
      CHECK(bt->step[0].lock[0].cond == seq::kCondAB && shown("COND:1:2") && shown("PAT?"));
      png("rui1050_7_lock_cond.png");
      key(0x80, true);   // FUNC (the SEQ page catches it)
      key(0x70, true), key(0x70, false);
      key(0x80, false);
      advance_ms(2600);
      draw();
      CHECK((bt->step[0].slide & 1) != 0);
      png("rui1050_8_lock_slide.png");
      key(0x80, true), key(0x70, true), key(0x70, false), key(0x80, false);   // FUNC + push again: the slide off
      CHECK(!(bt->step[0].slide & 1) && seq::lock_of(bt->step[0], 0) >= 0);
      key(0x80, true), key(0x70, true), key(0x70, false), key(0x80, false);
      CHECK((bt->step[0].slide & 1) != 0);
      CHECK(cmd("seq=x:1").size() > 0);
      key(0x70, true);
      advance_ms(450);
      seq::tick(now_ms());
      key(0x70, false);
      CHECK(seq::view().cond_e == -1);   // a long push again: the value
      key(0x70, true), key(0x70, false);   // short: the lock cleared
      CHECK(seq::lock_of(bt->step[0], 0) < 0);
      key(0, false);
      bt->step[0] = keep1;
      // the menu: PLAY MODE (YES / RIGHT the next), PRINT RND
      advance_ms(600);
      bt->dir = 0;
      key(0x80, true), key(0x43, true), key(0x43, false), key(0x80, false);   // updown1029: FUNC + DOWN opens the menu
      CHECK(seq::view().prompt == seq::kPromptMenu);
      for (int i = 0; i < seq::kMenuDir; ++i) key(0x43, true), key(0x43, false);
      key(0x44, true), key(0x44, false);
      CHECK(seq::view().prompt == seq::kPromptMenu && bt->dir == seq::kDirRev);
      key(0x41, true), key(0x41, false);
      draw();
      CHECK(bt->dir == seq::kDirPend && shown("PLAY MODE: PEND"));
      png("rui1050_9_menu.png");
      key(0x43, true), key(0x43, false);
      cmd("seq=rndstep:1:1:1");
      key(0x44, true), key(0x44, false);
      draw();
      CHECK(!(bt->step[0].xf & seq::kXfRnd) && !bt->rnd_mode && shown("PRINTED: 1 STEP"));
      png("rui1050_10_printed.png");
      {   // UP held alone over the open page (rytm-mods rui1050's 49 1 5): KEYS as the map over it, the SEQ page at the release
        auto m1 = msg(0x49, {5, 1});
        engine_sysex(m1.data(), (int)m1.size());
        draw();
        CHECK(remote_page_title() && !strcmp(remote_page_title(), "KEYS") && shown("UP + KEY"));
        png("rui1050_11_upmap_over_seq.png");
        auto m0 = msg(0x49, {5, 0});
        engine_sysex(m0.data(), (int)m0.size());
        draw();
        CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
        engine_sysex(m1.data(), (int)m1.size());   // UP + T3 while the map shows (another page), then the release: it stays
        remote_show_page("SCOPE");
        draw();
        engine_sysex(m0.data(), (int)m0.size());
        draw();
        CHECK(remote_page_title() && !strcmp(remote_page_title(), "SCOPE"));
        remote_show_page("SEQ");
        draw();
        auto m4 = msg(0x49, {4, 0});   // (a stray 49 1 4 0 leaves the page alone: no map of its own up)
        engine_sysex(m4.data(), (int)m4.size());
        draw();
        CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ"));
      }
      seq::edit_pattern() = keep_p;
      printf("  rui1050: DEFAULTS / SCALE / RANDOM / RTRG / FX / NOTE RND / lock COND + SLIDE / PLAY MODE / PRINT RND\n");
    }
    direct_state({8, 0, 0x7F});
  }

  // ---- stop: everything released, the pattern saved; pattern 1 made, then a switch 0 -> 1 with its file prefetched
  {
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> b = {1, 0}; auto x = blk4(now - r.delay); b.insert(b.end(), x.begin(), x.end()); return b; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    CHECK(seq::view().transport == seq::kStop);
    uint32_t saves = seq::stats().saves;
    for (int i = 0; i < 5 && !seq::idle(); ++i) {}
    while (seq::idle()) {}
    CHECK(seq::stats().saves > saves);
    struct stat sb;
    CHECK(stat((card + "/.rytm/seq/SEQTEST/p000.sqp").c_str(), &sb) == 0 && sb.st_size > 20);
    CHECK(stat((card + "/.rytm/seq/SEQTEST/tracks.txt").c_str(), &sb) == 0);
    // pattern 1 (stopped, chosen on the Rytm): empty; a single note; saved; back to 0 (loaded from its file)
    direct_state({2, 1, 0x7F});
    while (seq::idle()) {}
    // per pattern poly (2026-10-26): a new pattern has no poly tracks - BD is one in A01 only (fix1051b gave every
    // track of the project's mask a slot in every pattern); the edit below makes it one here (edit_track's create)
    CHECK(seq::view().pattern == 1 && !seq::edit_track(0, false) && !seq::owns_track(0));
    seq::Track* p1 = seq::edit_track(0, true);
    p1->length = 16;
    seq::add_note(p1->step[0], {84, 127, 96, 0, 0});
    seq::touched();
    direct_state({2, 0, 1});   // back to 0, next 1: the prefetch reads p001
    advance_ms(1500);
    while (seq::idle()) {}
    CHECK(seq::view().pattern == 0 && seq::edit_track(0, false) && seq::edit_track(0, false)->step[0].n > 0);
    CHECK(stat((card + "/.rytm/seq/SEQTEST/p001.sqp").c_str(), &sb) == 0);
    uint32_t pre = seq::stats().prefetched;
    // play 0 again, the switch at the end of its bar
    Run r2;
    r2.t0 = (int64_t)seq::frames() + 3000;
    r2.len = 16;
    run(r2, r2.t0 + (int64_t)(15.5 * r2.period));
    int64_t sw = r2.t0 + (int64_t)llround(16 * r2.period);
    state(sw + r2.delay, [&] { std::vector<uint8_t> b = {3, 0, 1}; auto x = blk4(sw); b.insert(b.end(), x.begin(), x.end()); return b; }());
    g_fired.clear();
    run(r2, sw + (int64_t)(4 * r2.period));
    CHECK(seq::view().pattern == 1 && seq::stats().prefetched == pre + 1 && seq::stats().switches == 1);
    bool got = false;
    for (const Fired& f : g_fired) got |= f.note == 84;
    CHECK(got);   // pattern 1's step 0 played after the switch
    for (const Fired& f : g_fired) CHECK(f.planned >= sw - 2 * r2.period || f.note != 84);
  }
  // the project's tracks.txt read back: another project (no seq tracks), then SEQTEST again (BD sequenced)
  {
    while (seq::idle()) {}
    cmd("seq=lat:240");   // fix round 1: SEQTEST's tracks.txt edit, still settling when the Rytm switches
    const uint8_t other[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 0xF7};
    engine_sysex(other, sizeof other);
    // UX review B1: OTH has no Sidecar files, SEQTEST some - the switch waits for the Rytm's answer; NO: a new project
    CHECK(rename::state() == rename::kAsk && !strcmp(project, "SEQTEST") && seq::any_tracks());
    rename::answer(false);
    // fix round 1: while OTH's store fills, an edit of OTH (the project the Rytm is on) is refused at once with that
    // reason - a track made a seq track, a pattern edit on a pattern switched to meanwhile - never taken and then lost
    CHECK(project_loading() && seq::view().loading);
    uint32_t r0 = seq::stats().loading_refused;
    CHECK(cmd("seq=track:2:on").find("still loading") != std::string::npos && !seq::owns_track(1));
    direct_state({2, 3, 0x7F});   // A04, during the fill
    while (seq::idle()) {}
    CHECK(seq::view().pattern == 3 && seq::view().locked == 3);
    seq::Track* lt = seq::edit_track(0, true);
    if (lt) seq::add_note(lt->step[0], {50, 100, 48, 0, 0});
    seq::touched();
    CHECK(used(0) == 0 && seq::stats().loading_refused == r0 + 2);   // undone at once (and logged)
    loaded();
    CHECK(!seq::view().loading && seq::view().locked == 0);
    CHECK(!seq::any_tracks() && !seq::owns_voice(0));
    for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();
    {
      FILE* tf = fopen((card + "/.rytm/seq/SEQTEST/tracks.txt").c_str(), "r");
      char tb[512] = {0};
      if (tf) tb[fread(tb, 1, sizeof tb - 1, tf)] = 0, fclose(tf);
      CHECK(strstr(tb, "lat=240"));   // the old project's settling edit, in its own tracks.txt
    }
    printf("  store: during a fill the loading project's edits are refused (with the reason); the old one's land\n");
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    engine_sysex(proj, sizeof proj);
    loaded();
    CHECK(seq::any_tracks() && seq::owns_voice(0) && seq::view().latency == 240);
    seq::set_latency(lat);
    cmd("seq=lat:192");
  }
  // final review (Important 1): during a fill, an edit of the OLD project's loaded pattern, then the Rytm moves to
  // another pattern and queues the edited one back: the prefetch reads the NEW project's file of that index (not in
  // its store yet: kLockLoading on the index) and copies the save buffer over it; the lock was the new project's, so
  // the old project's pending save of it was refused ("its project still loading") and the edit lost. Now the index
  // keeps the old project's lock and the edit lands in the old project's own file.
  {
    while (seq::idle()) {}
    for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    CHECK(!strcmp(project, "SEQTEST") && seq::view().pattern == 0 && seq::view().locked == 0);
    const uint8_t other[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 0xF7};
    engine_sysex(other, sizeof other);
    if (rename::state() == rename::kAsk) rename::answer(false);
    CHECK(project_loading() && !strcmp(project, "OTH"));
    CHECK(seq::view().locked == 0);   // the loaded pattern is still SEQTEST's A01: editable
    int u0 = used(0);
    seq::Track* lt = seq::edit_track(0, true);
    CHECK(lt);
    if (lt) seq::add_note(lt->step[7], {50, 100, 48, 0, 0});
    seq::touched();
    CHECK(used(0) == u0 + 1);
    uint32_t refused0 = seq::stats().save_refused, saves0 = seq::stats().saves;
    direct_state({2, 1, 0});   // the Rytm on A02 now, A01 queued back
    while (seq::idle()) {}
    CHECK(seq::stats().save_refused == refused0 && seq::stats().saves == saves0 + 1);   // A01 saved, not refused
    CHECK(seq::pattern_locked(0) == 0);   // (SEQTEST's A01 lock, not OTH's stand-in)
    loaded();
    for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();
    struct stat sb;
    CHECK(stat((card + "/.rytm/seq/OTH/p000.sqp").c_str(), &sb) != 0);   // nothing of SEQTEST's written into OTH
    direct_state({2, 1, 0x7F});   // (playing: A01 unqueued first, else the jump to it waits for its switch)
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    engine_sysex(proj, sizeof proj);
    loaded();
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    CHECK(seq::view().pattern == 0 && seq::view().locked == 0 && used(0) == u0 + 1);   // the edit, in SEQTEST's file
    printf("  store: an old project's edit queued back during a fill (the prefetch) lands in its own file\n");
    cmd("seq=lat:192");
  }
  // ======== seq-p2 ========
  // stopped, pattern 0 (BD: the demo's steps)
  {
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> b = {1, 0}; auto x = blk4(now - 700); b.insert(b.end(), x.begin(), x.end()); return b; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    CHECK(seq::view().transport == seq::kStop && seq::view().pattern == 0);
    // rui1051 (the hardware report: a 2-page poly track, GRID REC, stopped - PAGE moved the PAGE LEDs, the trig LEDs and
    // the touchscreen stayed on page 1): a track that follows the Rytm's length, the Rytm on PATTERN scale (2D 04 mode 0)
    // of 32 steps while its per-track length byte says 16; stopped: two pages, PAGE to page 2, T3 places step 19, its LED
    // lit, T1's dark
    {
      seq::Track* t0 = seq::edit_track(0, false);
      uint16_t was = t0->length;
      t0->length = 0;
      direct_state({0x04, 0, 16, 0, 2, 0});
      direct_state({0x04, 0x7F, 32, 0, 2, 0, 16, 0});
      CHECK(seq::view().transport != seq::kPlay && seq::view().length == 32 && seq::view().npages == 2);
      static seq::Step keep[32];
      for (int k = 0; k < 32; ++k) keep[k] = t0->step[k], t0->step[k].n = 0;
      key(0x84, true), key(0x84, false);
      CHECK(seq::view().page == 1);
      key(2, true), advance_ms(30), key(2, false);
      CHECK(t0->step[18].n >= 1 && t0->step[2].n == 0);
      uint8_t c[16];
      seq::leds(c);
      CHECK((c[2] == 47 || c[2] == 79) && c[0] == 0);   // (a note or a chord: a tap takes the last chord played)
      printf("  rui1051: PATTERN scale 32 steps, stopped: 2 pages, PAGE -> page 2, T3 -> step 19 (LED %d)\n", c[2]);
      for (int k = 0; k < 32; ++k) t0->step[k] = keep[k];
      direct_state({0x04, 0x7F, 1, 0, 2, 1, 16, 0});   // per-track scale again (INF): 16 steps, one page
      CHECK(seq::view().length == 16 && seq::view().npages == 1);
      key(0x84, true), key(0x84, false);
      t0->length = was;
      seq::view_set(0, 0, -1, false);
    }
  }
  int n0 = used(0);
  CHECK(n0 > 4);
  // ---- the Rytm's pattern copy / paste / undo / clear / undo (FUNC + REC, then on A03 FUNC + STOP twice, FUNC + PLAY)
  {
    edit(seq::kPCopy, 0, 0x7F, 1);
    direct_state({2, 2, 0x7F});   // A03 (empty), as the Rytm says before the paste
    while (seq::idle()) {}
    CHECK(seq::view().pattern == 2 && used(0) == 0);
    edit(seq::kPUndo, 2, 0x7F, 0, 0, 2);   // the paste's undo check: nothing to undo
    edit(seq::kPPaste, 2, 0x7F, 1);
    CHECK(used(0) == n0);
    edit(seq::kPUndo, 2, 0x7F, 1, 0, 2);   // FUNC + STOP again: UNDO PASTE
    CHECK(used(0) == 0);
    edit(seq::kPPaste, 2, 0x7F, 1);
    edit(seq::kPUndo, 2, 0x7F, 0, 0, 1);
    edit(seq::kPClear, 2, 0x7F, 1);
    CHECK(used(0) == 0);
    edit(seq::kPUndo, 2, 0x7F, 1, 0, 1);   // UNDO CLEAR
    CHECK(used(0) == n0);
    edit(seq::kPPaste, 2, 0x7F, 0);   // a paste the Rytm refused (ok 0): nothing
    CHECK(used(0) == n0 && seq::stats().undos == 2);
    printf("  edits: pattern copy / paste / undo / clear / undo on A03: %d steps\n", used(0));
  }
  // ---- track copy (BD) / paste (on RS) / undo / clear BD / undo
  {
    edit(seq::kTCopy, 2, 0, 1);
    edit(seq::kTUndo, 2, 2, 0, 0, 2);
    edit(seq::kTPaste, 2, 2, 1);
    CHECK(used(2) == n0);
    edit(seq::kTUndo, 2, 2, 1, 0, 2);
    CHECK(used(2) == 0);
    edit(seq::kTClear, 2, 0, 1);
    CHECK(used(0) == 0);
    edit(seq::kTUndo, 2, 0, 1, 0, 1);
    CHECK(used(0) == n0);
  }
  // ---- seq mode's own gestures on BD: page copy (page 1) / paste on page 2 / the same again = undo; trigs
  {
    direct_state({8, 1, 0});
    seq::view_set(0, 0, -1, false);
    seq::Track* t = seq::edit_track(0, false);
    CHECK(t && t->length == 64);
    int p0 = 0;
    for (int k = 0; k < 16; ++k) p0 += t->step[k].n > 0;
    for (int k = 16; k < 32; ++k) t->step[k].n = 0;
    edit(seq::kGCopy, 2, 0, 1, 1);
    seq::view_set(0, 1, -1, false);
    edit(seq::kGPaste, 2, 0, 1, 1);
    int p1 = 0;
    for (int k = 16; k < 32; ++k) p1 += t->step[k].n > 0;
    CHECK(p1 == p0 && t->step[16].n == t->step[0].n);
    edit(seq::kGPaste, 2, 0, 1, 1);   // again, nothing edited between: undone
    p1 = 0;
    for (int k = 16; k < 32; ++k) p1 += t->step[k].n > 0;
    CHECK(p1 == 0);
    seq::view_set(0, 0, -1, false);
    t->step[3].n = 0, t->step[4].n = 0;
    edit(seq::kRCopy, 2, 0, 1, 1, (1 << 0) | (1 << 6));
    edit(seq::kRPaste, 2, 0, 1, 1, (1 << 3) | (1 << 4));
    CHECK(t->step[3].n == t->step[0].n && t->step[4].n == t->step[6].n && t->step[3].note[0].pitch == t->step[0].note[0].pitch);
    edit(seq::kRClear, 2, 0, 1, 1, 1 << 3);
    CHECK(t->step[3].n == 0 && t->step[4].n > 0);
    edit(seq::kRClear, 2, 0, 1, 1, 1 << 3);   // the same clear again: undone
    CHECK(t->step[3].n == t->step[0].n);
    edit(seq::kRCopy, 2, 0, 1, 0, 0);   // a stock trig copy (no mask: the Rytm's own track): nothing
    direct_state({8, 0, 0x7F});
    printf("  edits: page and trig gestures, undos %u\n", seq::stats().undos);
  }
  // ---- SCALE: the first report is the Rytm's state; then BD 64 -> 24 on the Rytm: the host's BD follows; AUTO stays
  {
    advance_ms(400);   // (a SCALE right after a pattern change is that pattern's: not followed)
    uint32_t sc0 = seq::stats().scales;   // (rui1051's PATTERN scale case above sent three)
    direct_state({4, 0, 64, 0, 2, 1});
    CHECK(seq::edit_track(0, false)->length == 64);
    direct_state({4, 0, 24, 0, 2, 1});
    CHECK(seq::edit_track(0, false)->length == 24 && seq::view().rytm_len == 24);
    seq::edit_track(2, true)->length = 0;
    direct_state({4, 2, 16, 0, 2, 1});
    direct_state({4, 2, 20, 0, 1, 1});
    CHECK(seq::edit_track(2, false)->length == 0);
    direct_state({4, 0x7F, 16, 0, 2, 0, 16, 0});
    seq::edit_track(0, false)->length = 16;
    CHECK(seq::stats().scales - sc0 == 5);
    direct_state({2, 3, 0x7F});   // another pattern: its BD length (48 on the Rytm) is not a SCALE edit of ours
    while (seq::idle()) {}
    seq::edit_track(0, true)->length = 32;
    direct_state({4, 0, 48, 0, 2, 1});
    CHECK(seq::edit_track(0, false)->length == 32);
  }
  // ---- seq1112: LENGTH grows -> the pages copied (docs/SEQUENCER.md). Rendered, not asserted by return value: the
  // SEQ page's roll and its PG LEDs on page 1, then on page 2 after the grow, then back to one page after the shrink.
  {
    seq::Track* t = seq::edit_track(0, true);
    CHECK(t);
    seq::clear_steps(*t, 0, seq::kMaxSteps);
    for (int k = 0; k < 16; k += 3) seq::add_note(t->step[k], {(uint8_t)(55 + k), 100, 24, 0, 0});
    seq::cond_set(t->step[3], seq::kCondFill);
    CHECK(seq::set_lock(t->step[6], 3, 99 << 7));
    cmd("seq=len:1:16");
    seq::touched();
    remote_show_page("SEQ");
    seq::view_set(0, 0, -1, false);
    for (int i = 0; i < 4; ++i) remote_tick((uint32_t)(41000 + 30 * i)), advance_ms(30);
    CHECK(seq::view().length == 16 && seq::view().npages == 1);
    png("seq1112_1_one_page.png");
    cmd("seq=len:1:32");   // 1 -> 2 pages: page 1 duplicated, its condition and lock with it
    CHECK(t->step[16 + 3].n && seq::cond_of(t->step[16 + 3]) == seq::kCondFill);
    CHECK(seq::lock_of(t->step[16 + 6], 3) == 99 << 7);
    seq::view_set(0, 1, -1, false);
    for (int i = 0; i < 4; ++i) remote_tick((uint32_t)(41200 + 30 * i)), advance_ms(30);
    CHECK(seq::view().npages == 2);
    png("seq1112_2_grown_page2.png");
    cmd("seq=len:1:16");   // shortening hides: page 2's steps are still in the pattern
    CHECK(t->step[16 + 3].n && seq::cond_of(t->step[16 + 3]) == seq::kCondFill);
    seq::view_set(0, 0, -1, false);
    for (int i = 0; i < 4; ++i) remote_tick((uint32_t)(41400 + 30 * i)), advance_ms(30);
    CHECK(seq::view().npages == 1);
    png("seq1112_3_shrunk_hidden.png");
    t->step[16 + 1].note[0].vel = 40;   // page 2's own edit, made while it was visible above
    cmd("seq=len:1:32");               // growing back gives page 2 back, it does not re-copy page 1 over it
    CHECK(t->step[16 + 1].note[0].vel == 40 && seq::cond_of(t->step[16 + 3]) == seq::kCondFill);
    seq::view_set(0, 1, -1, false);
    for (int i = 0; i < 4; ++i) remote_tick((uint32_t)(41600 + 30 * i)), advance_ms(30);
    png("seq1112_4_grown_back.png");
    cmd("seq=len:1:64");
    printf("  seq1112: 1 -> 2 pages copied, shrink hid them, grow gave them back; %d steps used\n", seq::used_steps(*t));
    seq::clear_steps(*t, 0, seq::kMaxSteps);
    cmd("seq=len:1:64");
    seq::touched();
  }
  // ---- the pin: the 2E's 7-byte form only with more pinned tracks or the seq tracks' pin off; the silence (46)
  {
    g_out.clear();
    cmd("seq=pin:3:on");
    seq::tick(now_ms());
    auto e = sent(0x2E);
    CHECK(e.size() == 1 && e[0].size() == 14 && e[0][11] == 4 && e[0][12] == 0 && !(e[0][8] & 4));
    cmd("seq=pinseq:off");
    g_out.clear();
    seq::tick(now_ms());
    e = sent(0x2E);
    CHECK(e.size() == 1 && (e[0][8] & 4));
    cmd("seq=pinseq:on");
    cmd("seq=pin:3:off");
    poly::silence(0);   // the step entry's notes above, still in the pool: gone
    advance_ms(450);
    g_out.clear();
    seq::tick(now_ms());
    e = sent(0x2E);
    CHECK(e.size() == 1 && e[0].size() == 10);
    auto si = sent(0x46);   // after each 2E the silence again: BD (the seq track, nothing sounding for long) silent
    CHECK(si.size() == 1 && si[0][6] == 1 && si[0][7] == 0);
    advance_ms(600);
    g_out.clear();
    seq::tick(now_ms());
    CHECK(sent(0x46).empty());   // only changes
    // fix1052: a seq track's own pad plays its voice's own machine (pad_play: JUNO, PLAITS, DD, DAISY - not the pool),
    // which the silence did not count, so the VCA stayed closed under a sounding note [measured 2026-10-26, B07's SD
    // on JUNO: the Pi's output peak 250/1000 for a 2 s hold, nothing heard; the sequencer's notes on the same voice
    // were]. Held: open at once and for as long as it is held; released: closed again kSilentMs later (the tail's grace)
    const int pvi = remote_track_voice(0);
    poly::midi(0x9D, 0, 100);   // BD's pad (its trigger note on the auto channel), then its 0x04
    CHECK(poly::seq_pad(pvi, 60, 100));
    g_out.clear();
    seq::tick(now_ms());
    si = sent(0x46);
    CHECK(si.size() == 1 && si[0][6] == 0 && si[0][7] == 0);   // BD open
    advance_ms(1000);   // a long hold
    g_out.clear();
    seq::tick(now_ms());
    CHECK(sent(0x46).empty());   // still open
    poly::midi(0x8D, 0, 0);
    advance_ms(300);
    g_out.clear();
    seq::tick(now_ms());
    CHECK(sent(0x46).empty());   // within the grace
    advance_ms(150);
    g_out.clear();
    seq::tick(now_ms());
    si = sent(0x46);
    CHECK(si.size() == 1 && si[0][6] == 1 && si[0][7] == 0);   // closed again
    printf("  fix1052: a seq track's own pad opens its pinned VCA while held, closed %u ms after release\n", 400u);
  }
  // ---- swing, the switch prediction: pattern 0 BD notes on steps 0 and 1 (C5 / D5), pattern 1 one on step 0 (C6);
  // swing 66 on pattern 0; PLAY; A02 queued; the Rytm's pattern length 16 (normal mode); the switch at step 16
  {
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    seq::Track* a = seq::edit_track(0, true);
    for (seq::Step& st_ : a->step) st_.n = 0;
    a->length = 16;
    seq::add_note(a->step[0], {72, 100, 24, 0, 0});
    seq::add_note(a->step[1], {74, 100, 24, 0, 0});
    seq::touched();
    CHECK(cmd("seq=swing:1:66").find("66") != std::string::npos);
    direct_state({2, 1, 0x7F});
    while (seq::idle()) {}
    seq::Track* b = seq::edit_track(0, true);
    for (seq::Step& st_ : b->step) st_.n = 0;
    b->length = 16;
    seq::add_note(b->step[0], {84, 127, 24, 0, 0});
    seq::touched();
    advance_ms(1500);
    while (seq::idle()) {}
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    CHECK(seq::edit_track(0, false)->swing == 66);
    Run r3;
    r3.t0 = (int64_t)seq::frames() + 3000;
    r3.len = 16;
    r3.jit = 0;
    int64_t t0 = r3.t0;
    state(t0 + r3.delay, [&] { std::vector<uint8_t> x = {1, 1}; auto bb = blk4(t0); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    g_fired.clear();
    run(r3, t0 + (int64_t)(8.5 * r3.period));
    direct_state({2, 0, 1});   // A02 queued at step 8
    while (seq::idle()) {}
    int64_t sw = t0 + (int64_t)llround(16 * r3.period);
    int64_t arrive = sw + r3.delay;
    state(arrive, [&] { std::vector<uint8_t> x = {3, 0, 1}; auto bb = blk4(sw); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    run(r3, sw + (int64_t)(6 * r3.period));
    int64_t off = seq::stats().offset;
    int c84 = 0;
    for (const Fired& f : g_fired) {
      if (f.note == 74 && f.planned > t0 + 2 * r3.period && f.planned < sw - r3.period) {   // a swung step 1 (or 3, 5 ..)
        double rel = (double)(f.planned - t0 + lat - off) / r3.period;
        double frac = rel - std::floor(rel);
        CHECK(std::fabs(frac - 0.32) < 0.02);
      }
      if (f.note == 84 && f.planned < sw + 4 * r3.period) {
        ++c84;
        double e = (double)(f.planned + lat - off) - (double)sw;
        printf("  switch: A02's step 0 planned %.0f frames from the Rytm's switch block, fired at host %lld (2D 03 arrived %lld)\n",
               e, (long long)f.h, (long long)arrive);
        CHECK(std::fabs(e) <= 48 && f.h < arrive);   // on time, before the switch report could arrive
      }
    }
    CHECK(c84 == 1 && seq::stats().predict_ok == 1 && seq::view().pattern == 1);
    // stop
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> x = {1, 0}; auto bb = blk4(now - 700); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
  }
  // ---- latency calibration: the click rendered into BD's voice, back in its stream 700 + 13 frames later
  {
    const int D = 713;
    std::vector<int16_t> hist;   // BD's rendered output, by host frame
    int16_t ring[64][8];
    uint64_t mach[16] = {};
    CHECK(cmd("seq=cal:1").find("calibrating") != std::string::npos);
    int64_t start = (int64_t)seq::frames();
    std::vector<int32_t> fr(12 * kBlock);
    for (int b = 0; b < 4000 && seq::stats().cals == 0 && seq::stats().cal_fails == 0; ++b) {
      seq::pre_block();
      render_voices(ring, 64, 0, 0, 8, mach);
      for (int s = 0; s < kBlock; ++s) hist.push_back(ring[s][0]);
      g_us += 250;
      host_fake_time(true, g_us);
      int64_t h = (int64_t)seq::frames();   // the frames rendered so far: the arrival of what comes back now
      for (int s = 0; s < kBlock; ++s) {
        int64_t src = h - kBlock + s - D - start;
        int32_t x = src >= 0 && src < (int64_t)hist.size() ? (int32_t)hist[(size_t)src] << 16 : 0;
        for (int c = 0; c < 12; ++c) fr[(size_t)(12 * s + c)] = c == 2 ? x : 0;   // BD's stem: channel 2
      }
      seq::cal_audio(fr.data() + 2, kBlock, 12, h);
      seq::tick(now_ms());
    }
    printf("  calibration: round trip %d frames (latency %d)\n", seq::stats().cal_last, seq::view().latency);
    CHECK(seq::stats().cals == 1 && std::abs(seq::view().latency - D) <= kBlock);
    cmd("seq=lat:192");
  }
  // ---- the Rytm's own pattern by its SysEx dump: asked when the consumer turns it on, on a pattern change (debounced),
  // on 2D 0B (paced: core/seq/rytm_dump.h); the answer read from the MIDI stream, checked, its record kept
  {
    std::vector<uint8_t> rec(rytm_dump::kRecord);
    for (size_t i = 0; i < rec.size(); ++i) rec[i] = (uint8_t)(i * 37 + (i >> 5));
    rec[0] = 0, rec[1] = 0, rec[2] = 0, rec[3] = 5;
    auto m = dump_of(rec, 3);
    CHECK((int)m.size() == rytm_dump::kAnswer);
    std::vector<uint8_t> back(rytm_dump::kRecord);
    int pat = -1;
    CHECK(rytm_dump::parse(m.data(), (int)m.size(), back.data(), &pat) && pat == 3 && back == rec);
    {   // the real Rytm's checksum leaves the pattern number out [measured]: one with it in (as 0.3.0 summed) is refused
      auto w = m;
      int cs = ((w[w.size() - 5] << 7) | w[w.size() - 4]) + 3;
      w[w.size() - 5] = (uint8_t)(cs >> 7 & 0x7F), w[w.size() - 4] = (uint8_t)(cs & 0x7F);
      CHECK(!rytm_dump::parse(w.data(), (int)w.size(), back.data(), &pat));
    }
    g_out.clear();
    rytm_dump::set_active(true);
    seq::tick(now_ms());
    uint8_t req[15];
    rytm_dump::request(req, 0x80);
    CHECK(req[9] == 0x80);
    auto rq = [&] { int k = 0; for (auto& x : g_out) k += x.size() == 15 && !memcmp(x.data(), req, 9) && x[9] < 0x80 && !memcmp(x.data() + 10, req + 10, 5); return k; };
    CHECK(rq() == 1);
    for (uint8_t c : m) rytm_dump::byte(c);
    uint32_t gen = 0;
    const uint8_t* got = rytm_dump::record(&pat, &gen);
    CHECK(got && pat == 3 && gen == 1 && !memcmp(got, rec.data(), rec.size()));
    m[100] ^= 1;   // a damaged one: refused, the last good kept
    for (uint8_t c : m) rytm_dump::byte(c);
    CHECK(rytm_dump::stats().bad == 1 && rytm_dump::record(nullptr, &gen) && gen == 1);
    g_out.clear();
    rytm_dump::set_playing(false);
    auto tk = [] { rytm_dump::tick(now_ms()); };   // the dump's own tick: the transport as set here, not seq's
    auto ans = [&] { for (uint8_t c : dump_of(rec, 3)) rytm_dump::byte(c); };
    direct_state({11, 2});   // DIRTY, stopped: kQuietMs after the last one
    tk();
    CHECK(rq() == 0 && rytm_dump::stale() == 1);
    advance_ms(350), tk();
    CHECK(rq() == 0);
    advance_ms(700), tk();
    CHECK(rq() == 1 && seq::stats().dirties == 1 && rytm_dump::stale() == 0);
    ans();
    // stopped, an edit every 200 ms for 10 s: trailing (none while they come), at most kStaleMs after the first
    g_out.clear();
    for (int i = 0; i < 50; ++i) {
      direct_state({11, 2});
      for (int k = 0; k < 4; ++k) {
        advance_ms(50), tk();
        if (rytm_dump::pending() && rytm_dump::stats().requests > rytm_dump::stats().answers) ans();
      }
    }
    int edits = rq();
    advance_ms(1100), tk();
    CHECK(edits == 2 && rq() == 3);   // at 4 s and 8 s, then the settle after the last
    ans();
    // playing, an edit every 200 ms for 20 s: one fetch every kPlayGapMs
    g_out.clear();
    rytm_dump::set_playing(true);
    for (int i = 0; i < 100; ++i) {
      direct_state({11, 2});
      for (int k = 0; k < 4; ++k) {
        advance_ms(50), tk();
        if (rytm_dump::stats().requests > rytm_dump::stats().answers) ans();
      }
    }
    CHECK(rq() >= 4 && rq() <= 5);
    // the VOICE page while playing: no fetch on DIRTY (held: after STOP), a pattern change still fetched
    ans();
    rytm_dump::set_page(rytm_dump::kPageVoice);
    g_out.clear();
    advance_ms(5000), tk();
    CHECK(rq() == 0);   // (the last loop's final edit: held)
    direct_state({11, 2});
    advance_ms(5000), tk();
    CHECK(rq() == 0 && rytm_dump::stale() == 2);
    rytm_dump::set_playing(false);
    advance_ms(1100), tk();
    CHECK(rq() == 1 && rytm_dump::stale() == 0);
    ans();
    // a song take recording: DIRTY held until it ends; a pattern change too on the VOICE page, fetched on the PAT page
    g_out.clear();
    rytm_dump::set_page(rytm_dump::kPagePattern);
    rytm_dump::set_playing(true), rytm_dump::set_hold(1);
    for (int i = 0; i < 50; ++i) direct_state({11, 2}), advance_ms(200), tk();
    CHECK(rq() == 0 && rytm_dump::stale() == 2);
    direct_state({2, 5, 0x7F}), advance_ms(100), tk();
    direct_state({2, 6, 0x7F}), advance_ms(100), tk();   // a quick run of switches: one fetch, the last pattern's
    CHECK(rq() == 0);
    advance_ms(300), tk();
    CHECK(rq() == 1 && g_out.back()[9] == 6);
    ans();
    rytm_dump::set_page(rytm_dump::kPageVoice);
    direct_state({2, 7, 0x7F}), advance_ms(1000), tk();
    CHECK(rq() == 1 && rytm_dump::stale() == 2);
    rytm_dump::set_hold(2), rytm_dump::want(0);   // the song's own dumps running: nothing, not even an asked fetch
    advance_ms(2000), tk();
    CHECK(rq() == 1);
    rytm_dump::set_playing(false), rytm_dump::set_hold(0);   // STOP: the take ends, one fetch
    advance_ms(300), tk();
    CHECK(rq() == 2);
    ans();
    advance_ms(2000), tk();
    CHECK(rq() == 2);
    // a lost answer: asked again once after kTimeoutMs, then nothing until the next trigger (one request at a time)
    g_out.clear();
    rytm_dump::want(0), tk();
    for (int i = 0; i < 100; ++i) direct_state({11, 2}), advance_ms(100), tk();   // edits meanwhile, unanswered
    CHECK(rq() >= 2 && rq() <= 5);
    for (int i = 0; i < 100; ++i) advance_ms(100), tk();
    int lost = rq();
    for (int i = 0; i < 100; ++i) advance_ms(100), tk();
    CHECK(rq() == lost);
    ans();
    rytm_dump::set_page(rytm_dump::kPagePattern);
    g_out.clear();
    direct_state({2, 4, 0x7F});
    tk();
    CHECK(rq() == 0);
    advance_ms(300), tk();
    CHECK(rq() == 1);
    ans();
    rytm_dump::set_active(false);
    printf("  rytm dump: requests %u answers %u bad %u\n", rytm_dump::stats().requests, rytm_dump::stats().answers,
           rytm_dump::stats().bad);
  }
  // ======== seq-ux (1036) ========
  // ---- TRK + TRIG (rui1037; rui1036's FUNC + LFO; 30 page 0x10 + t): the SEQ page of t; not a seq track: the make prompt (a poly machine) or why not
  {
    std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 12};
    const uint8_t vb[12] = {kFm | 0, 0x7F, kJuno | 7, 0x7F, 0x7F, kWaveMachine | 3, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F};
    for (int t = 0; t < 12; ++t) m.push_back((uint8_t)(t == 0 ? 40 : t == 5 ? 46 : t == 2 ? 41 : 5)), m.push_back(vb[t]);
    m.push_back(0xF7);
    engine_sysex(m.data(), (int)m.size());
    CHECK(remote_track_machine(5) == (kWaveMachine | 3));
    // rui1050: the SEED MOD slots (a param the machine leaves free): 6-OP FM / JUNO P12, WAVETABLE none
    CHECK(seq::seed_slot_of(0) == 12 && seq::seed_slot_of(2) == 12 && seq::seed_slot_of(5) == -1 && seq::seed_slot_of(1) == -1);
    // rui1036: the source lists' names to the Rytm (47) once per change and after a hello; a pick's write (48)
    g_out.clear();
    remote_tick(8800);
    auto nm = sent(0x47);
    // (the pools design 3.5: every bank in the 47 form with its bank byte, bank A's rows all named: 128 rows)
    CHECK(!nm.empty() && nm[0].size() >= 11 && nm[0][5] == 1 && nm[0][7] == 1 && nm[0][9] == 0 && (nm[0].size() - 11) % 14 == 0);
    bool l1 = false;
    for (auto& x : nm) l1 |= x[6] == 1 && x[7] == 1;   // WAVETABLE's TBL list (its built-in tables)
    CHECK(l1);
    g_out.clear();
    remote_tick(8810);
    CHECK(sent(0x47).empty());
    g_out.clear();
    CHECK(remote_param_write(3, kWaveMachine, 0, 5) == 1);
    auto w = sent(0x48);
    CHECK(w.size() == 1 && w[0].size() == 10 && w[0][6] == 5 && w[0][7] == 0 && w[0][8] == 5);
    CHECK(remote_param_write(3, kGranMachine, 0, 5) == 0);   // LT is WAVETABLE: nothing for GRANULAR
    // rui1036b (49): UP + T4 -> the display's page 4; the selected track's machine, name, P1..P8 and labels
    {
      int pg = 0;
      uint32_t g0 = remote_display_request(&pg);
      auto m1 = msg(0x49, {1, 4});
      engine_sysex(m1.data(), (int)m1.size());
      CHECK(remote_display_request(&pg) == g0 + 1 && pg == 4);
      std::vector<uint8_t> b2 = {2, 5, 46};
      for (char c : std::string("WAVETABLE\0\0\0")) b2.push_back((uint8_t)c);
      b2.resize(15);
      auto m2 = msg(0x49, b2);
      uint32_t tg0 = 0, tg1 = 0;
      remote_selected_track(&tg0);
      engine_sysex(m2.data(), (int)m2.size());
      CHECK(remote_selected_track(&tg1) == 5 && tg1 == tg0 + 1);
      std::vector<uint8_t> b3 = {3, 5};
      for (int k = 0; k < 8; ++k) b3.insert(b3.end(), {(uint8_t)(10 * k), 'T', 'B', (uint8_t)('0' + k), 0});
      auto m3 = msg(0x49, b3);
      engine_sysex(m3.data(), (int)m3.size());
      int tt = -1, mm = -1;
      char nm[13];
      uint8_t vv[8];
      char lb[8][5];
      remote_track_info(&tt, &mm, nm, vv, lb);
      CHECK(tt == 5 && mm == 46 && !strcmp(nm, "WAVETABLE") && vv[3] == 30 && !strcmp(lb[7], "TB7"));
      engine_sysex(m2.data(), (int)m2.size());
      CHECK(remote_selected_track(&tg1) == 5 && tg1 == tg0 + 1);   // the same track again: no change counted
    }
    auto open_seq = [&](int t) {
      const uint8_t o[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, (uint8_t)(0x10 + t), 1, 0, 0xF7};
      engine_sysex(o, sizeof o);
      for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9000 + 30 * i)), advance_ms(30);
    };
    uint32_t g0 = seq::view_requests(nullptr);
    open_seq(1);   // SD: analog
    int vt = -1;
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ") && seq::view_requests(&vt) == g0 + 1 && vt == 1);
    CHECK(seq::view().prompt == seq::kPromptExplain && seq::view().prompt_why == 0 && shown("SD PLAYS ANALOG"));
    png("seq_explain.png");
    g_out.clear();
    key(0x45, true), key(0x45, false);   // NO: back to the Rytm's screen (3A 0 0)
    auto cl = sent(0x3A);
    CHECK(cl.size() == 1 && cl[0][7] == 0 && seq::view().prompt == 0);
    const uint8_t closed[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 0, 1, 0xF7};
    engine_sysex(closed, sizeof closed);
    open_seq(2);   // RS: JUNO - seqx (E0): every Sidecar machine can be a poly track now (JUNO on the instance pool)
    CHECK(seq::view().prompt == seq::kPromptMake && shown("MAKE RS A POLY TRACK?"));
    engine_sysex(closed, sizeof closed);
    CHECK(seq::view().prompt == 0);   // the page closed: the prompt with it
    open_seq(5);   // LT: WAVETABLE, no seq track: MAKE LT A SIDECAR SEQ TRACK?
    CHECK(!seq::owns_track(5) && seq::view().prompt == seq::kPromptMake && shown("MAKE LT A POLY TRACK?"));
    png("seq_make.png");
    enc(0, 3);   // knobs and trig keys do nothing under it
    key(3, true), key(3, false);
    CHECK(seq::view().prompt == seq::kPromptMake && !seq::edit_track(5, false));
    key(0x44, true), key(0x44, false);   // YES
    CHECK(seq::owns_track(5) && seq::view().prompt == 0 && seq::view().track == 5 && seq::edit_track(5, false));
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9200 + 30 * i)), advance_ms(30);
    CHECK(shown("SIDECAR SEQ") && !shown("MAKE"));
    seq::add_note(seq::edit_track(5, false)->step[0], {60, 100, 48, 0, 0});
    // UX review R2: B down never asks - the footer says how; S2: DOWN (ux1027; YES before) opens the poly track's menu, DOWN to TURN OFF, YES
    // asks, NO keeps it; again with YES: off, its steps stay
    enc(1, -1);
    CHECK(seq::view().prompt == 0 && seq::owns_track(5) && seq::view().off_hint);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9300 + 30 * i)), advance_ms(30);
    CHECK(shown("OFF: F+DOWN, TURN OFF"));
    key(0x43, true), key(0x43, false);   // updown1029: DOWN alone is the Rytm's (CHRO's octave): no menu
    CHECK(seq::view().prompt == 0);
    key(0x80, true), key(0x43, true), key(0x43, false), key(0x80, false);   // FUNC + DOWN: the menu
    CHECK(seq::view().prompt == seq::kPromptMenu && seq::view().prompt_sel == 0);
    key(0x43, true), key(0x43, false);   // DOWN: TURN OFF
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9400 + 30 * i)), advance_ms(30);
    CHECK(shown("POLY TRACK") && shown("TURN OFF (STEPS KEPT)") && shown("PLAY MODE: FWD") && shown("PRINT RND"));   // rui1050: 6 rows
    png("seq_menu.png");
    key(0x44, true), key(0x44, false);   // YES: asked
    CHECK(seq::view().prompt == seq::kPromptOff);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9500 + 30 * i)), advance_ms(30);
    CHECK(shown("LT: POLY TRACK OFF?"));
    png("seq_off.png");
    key(0x45, true), key(0x45, false);
    CHECK(seq::view().prompt == 0 && seq::owns_track(5) && remote_page_title());   // NO: kept (and the page stays)
    key(0x80, true), key(0x43, true), key(0x43, false), key(0x80, false), key(0x43, true), key(0x43, false), key(0x44, true),
        key(0x44, false);
    key(0x44, true), key(0x44, false);
    CHECK(!seq::owns_track(5) && seq::edit_track(5, false) && seq::edit_track(5, false)->step[0].n == 1);
    enc(1, 1);   // B up: the make prompt again (a poly machine), YES: the same steps back
    CHECK(seq::view().prompt == seq::kPromptMake);
    key(0x44, true), key(0x44, false);
    CHECK(seq::owns_track(5) && seq::edit_track(5, false)->step[0].n == 1);
    cmd("seq=track:6:off");
    engine_sysex(closed, sizeof closed);
  }
  // ---- the lock pages: BD's SEQ page; T3 held + SRC: A..H lock P1..P8 of step 3; SMPL P9..P16, FX FX1 / FX2 P2..P5;
  // a push clears one, NO every lock of the held steps; the LEDs magenta; T3 let go: no toggle, the lock page ends
  {
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    const uint8_t o[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0x10, 1, 0, 0xF7};
    engine_sysex(o, sizeof o);
    CHECK(seq::view().prompt == 0 && seq::view().track == 0 && seq::owns_track(0));
    seq::view_set(0, 0, -1, false);
    seq::Track* bt = seq::edit_track(0, true);
    for (seq::Step& x : bt->step) x.n = 0, x.nl = 0;
    bt->length = 16;
    seq::add_note(bt->step[0], {60, 100, 48, 0, 0});
    seq::add_note(bt->step[2], {64, 100, 48, 0, 0});
    seq::add_note(bt->step[4], {67, 100, 48, 0, 0});
    report(kFm | 0, {0, 64, 64, 127, 64, 64, 100, 127});
    // the SRC view's values of BD (2A): P1..P16 as the Rytm shows them
    std::vector<uint8_t> a = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 0, 16};
    for (int k = 0; k < 16; ++k) a.push_back((uint8_t)(k == 1 ? 64 : 10 + k)), a.push_back(0);
    a.push_back(0xF7);
    engine_sysex(a.data(), (int)a.size());
    key(2, true);
    key(0x31, true), key(0x31, false);   // SRC
    CHECK(seq::view().lock_page == seq::kLpSrc && seq::lock_param(seq::kLpSrc, 1) == 1 && seq::held_lock(1) == -1 &&
          seq::lock_base(1) == 64 << 7);
    enc(1, 5);
    CHECK(seq::lock_of(bt->step[2], 1) == 69 << 7 && seq::held_lock(1) == 69 << 7);
    enc(1, 2, true);   // FUNC: x 8
    CHECK(seq::lock_of(bt->step[2], 1) == 85 << 7);
    enc(6, -3);
    CHECK(seq::lock_of(bt->step[2], 6) == (16 - 3) << 7);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9500 + 30 * i)), advance_ms(30);
    CHECK(shown("LOCK SRC") && shown("LOCK"));
    png("seq_lock_src.png");
    key(0x32, true), key(0x32, false);   // SMPL: P9..P16
    enc(4, 2);
    CHECK(seq::view().lock_page == seq::kLpSmpl && seq::lock_of(bt->step[2], 12) == (22 + 2) << 7);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9600 + 30 * i)), advance_ms(30);
    CHECK(shown("LOCK SMPL") && shown("CHD"));
    key(0x74, true), key(0x74, false);   // PUSH_E: that lock cleared
    CHECK(seq::lock_of(bt->step[2], 12) == -1 && bt->step[2].nl == 2);
    key(0x10, true), key(0x10, false);   // FX: FX1 P2..P5 on A..D
    fx::control.knob[0][3] = 40;
    enc(1, 1);
    CHECK(seq::view().lock_page == seq::kLpFx && seq::lock_of(bt->step[2], seq::kLockFx + 1) == 41 << 7);
    key(0x33, true), key(0x33, false);   // FLTR: the Rytm's analog side, nothing to lock
    enc(0, 4);
    CHECK(seq::view().lock_page == seq::kLpFltr && bt->step[2].nl == 3);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9700 + 30 * i)), advance_ms(30);
    CHECK(shown("LOCK FLTR") && shown("ANALOG"));
    uint8_t c[16];
    seq::leds(c);
    CHECK(c[2] == 63);   // (held: amber)
    key(2, false);
    advance_ms(100);
    CHECK(seq::view().lock_page == -1 && bt->step[2].n == 1 && bt->step[2].nl == 3);   // no tap toggle
    seq::leds(c);
    CHECK(c[2] == 95 && c[0] == 47);
    // a lock trig: T6 (empty) held + SRC + A
    key(5, true), key(0x31, true), key(0x31, false), enc(0, 1), key(5, false);
    CHECK(bt->step[5].n == 0 && bt->step[5].nl == 1 && seq::lock_of(bt->step[5], 0) == 11 << 7);
    seq::leds(c);
    CHECK(c[5] == 85);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9800 + 30 * i)), advance_ms(30);
    png("seq_locks.png");   // the strip's lock lane
    key(5, true), key(0x45, true), key(0x45, false), key(5, false);   // NO: every lock of T6 gone
    CHECK(bt->step[5].nl == 0 && bt->step[5].n == 0);
    key(4, true), advance_ms(50), key(4, false);   // a plain tap still toggles (T5 off)
    CHECK(bt->step[4].n == 0);
    seq::add_note(bt->step[4], {67, 100, 48, 0, 0});
    seq::touched();
    const uint8_t closed[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 0, 1, 0xF7};
    engine_sysex(closed, sizeof closed);
  }
  // ---- the locks played: BD step 3 locks P2 85 and P7 13 and FX1 P3 41; steps 1 and 5 none: the bases between
  // (the Rytm's report moves a base under a lock: the lock stays, the new base comes back after it); STOP: the bases
  {
    seq::Track* bt = seq::edit_track(0, false);
    CHECK(bt && bt->step[2].nl == 3);
    report(kFm | 0, {0, 64, 64, 127, 64, 64, 100, 127});
    fx::control.knob[0][3] = 40;
    static std::vector<std::pair<int64_t, float>> tl;
    static std::vector<std::pair<int64_t, int>> tf;
    tl.clear(), tf.clear();
    g_each = [] {
      tl.push_back({(int64_t)seq::frames(), v[0].p[1]});
      tf.push_back({(int64_t)seq::frames(), (int)fx::control.knob[0][3]});
    };
    Run r4;
    r4.t0 = (int64_t)seq::frames() + 3000;
    r4.len = 16;
    r4.jit = 0;
    int64_t t0 = r4.t0;
    state(t0 + r4.delay, [&] { std::vector<uint8_t> x = {1, 1}; auto bb = blk4(t0); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    uint32_t ap = seq::stats().locks_applied;
    run(r4, t0 + (int64_t)(18 * r4.period));
    report(kFm | 0, {0, 30, 64, 127, 64, 64, 100, 127});   // the Rytm's P2 now 30 (bar 2, before step 3)
    float during = -1, after = -1;
    run(r4, t0 + (int64_t)(20 * r4.period));   // bar 2's step 3 (index 2) at 18 periods: locked
    during = v[0].p[1];
    run(r4, t0 + (int64_t)(22.5 * r4.period));   // past step 5 (index 4): the base back, the Rytm's new 30
    after = v[0].p[1];
    g_each = nullptr;
    int64_t off = seq::stats().offset;
    auto at = [&](double step) {   // P2 / FX1 P3 a third into `step` of bar 1 (host frames: the Rytm's + the offset - latency)
      int64_t h = t0 + (int64_t)(step * r4.period + r4.period / 3) + off - lat;
      float pv = -1;
      int fv = -1;
      for (auto& e : tl) if (e.first <= h) pv = e.second;
      for (auto& e : tf) if (e.first <= h) fv = e.second;
      return std::make_pair(pv, fv);
    };
    auto s1 = at(1), s2 = at(2), s3 = at(3), s4 = at(4), s5 = at(5);
    printf("  locks: P2 / FX1 P3 by step: %.0f/%d %.0f/%d %.0f/%d %.0f/%d %.0f/%d; bar 2 locked %.0f, after %.0f; applied %u\n",
           s1.first, s1.second, s2.first, s2.second, s3.first, s3.second, s4.first, s4.second, s5.first, s5.second, during,
           after, seq::stats().locks_applied - ap);
    CHECK(s1.first == 64 && s1.second == 40);    // step 2 (no lock, no note): the bases
    CHECK(s2.first == 85 && s2.second == 41);    // step 3: locked
    CHECK(s3.first == 85 && s3.second == 41);    // step 4: empty, the lock holds
    CHECK(s4.first == 64 && s4.second == 40);    // step 5: a note without locks: the bases back
    CHECK(std::fabs(v[0].p[6] - 100) < 0.01f);   // P7 back too
    CHECK(during == 85 && after == 30);          // the report under the lock: its new base after it
    CHECK(seq::stats().locks_applied - ap >= 6);
    // the lock's frame: the step's (a lock set before its note), within a block of the note-on
    int64_t lh = -1;
    for (size_t i = 1; i < tl.size(); ++i)
      if (tl[i].second == 85 && tl[i - 1].second != 85) { lh = tl[i].first; break; }
    int64_t want = t0 + (int64_t)(2 * r4.period) + off - lat;
    printf("  locks: P2 locked at host frame %lld, the step's note due %lld\n", (long long)lh, (long long)want);
    CHECK(lh >= 0 && std::llabs(lh - want) <= kBlock + 1);
    // STOP: every base back
    report(kFm | 0, {0, 30, 64, 127, 64, 64, 100, 127});
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> x = {1, 0}; auto bb = blk4(now - 700); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    CHECK(v[0].p[1] == 30 && fx::control.knob[0][3] == 40);
    // the console: seq=lock / unlock
    CHECK(cmd("seq=lock:1:9:3:77").find("1 locks") != std::string::npos && seq::lock_of(bt->step[8], 2) == 77 << 7);
    CHECK(cmd("seq=lock:1:9:3:-1").find("0 locks") != std::string::npos);
  }
  // ---- fx-ins: the INSERT locks (core/fx/insert.h). BD's voice: INSERT A FILTER (CUT 80), B DRIVE (SHPE 0). Step 3
  // locks A CUT 20 (the console, P 25); the INS lock page (T5 held + FX + FX: A..D INSERT A, E..H INSERT B) locks B SHPE
  // on step 5; played: each lock at its step, the knob back at the next step without it; STOP: the knobs
  {
    seq::Track* bt = seq::edit_track(0, false);
    CHECK(bt);
    ins::reset();
    const int vi = remote_track_voice(0);
    CHECK(vi >= 0 && vi < 8);
    ins::set_type(vi, 0, ins::kFilter);
    ins::set_type(vi, 1, ins::kDrive);
    CHECK(cmd("seq=lock:1:3:25:20").find("locks") != std::string::npos && seq::lock_of(bt->step[2], seq::kLockIns) == 20 << 7);
    const uint8_t o[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0x10, 1, 0, 0xF7};
    engine_sysex(o, sizeof o);
    seq::view_set(0, 0, -1, false);
    key(4, true);
    key(0x10, true), key(0x10, false);   // FX: the send FX's P2..P5
    CHECK(seq::view().lock_page == seq::kLpFx);
    key(0x10, true), key(0x10, false);   // FX again: INS
    CHECK(seq::view().lock_page == seq::kLpIns && seq::lock_param(seq::kLpIns, 5) == seq::kLockIns + 5 &&
          seq::lock_base(seq::kLockIns + 5) == 0 && seq::lock_base(seq::kLockIns) == 80 << 7);
    enc(5, 9);
    CHECK(seq::lock_of(bt->step[4], seq::kLockIns + 5) == 9 << 7);
    for (int i = 0; i < 3; ++i) remote_tick((uint32_t)(9900 + 30 * i)), advance_ms(30);
    CHECK(shown("LOCK INS") && shown("SHPE"));
    png("seq_lock_ins.png");
    key(0x10, true), key(0x10, false);   // and back to FX
    CHECK(seq::view().lock_page == seq::kLpFx);
    key(4, false);
    const uint8_t closed[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 1, 0, 0, 1, 0xF7};
    engine_sysex(closed, sizeof closed);
    seq::touched();
    static std::vector<std::pair<int64_t, int>> ta, tb;
    ta.clear(), tb.clear();
    g_each = [] {
      int w = remote_track_voice(0);
      ta.push_back({(int64_t)seq::frames(), (int)ins::control.play[w][0][0]});
      tb.push_back({(int64_t)seq::frames(), (int)ins::control.play[w][1][1]});
    };
    Run r5;
    r5.t0 = (int64_t)seq::frames() + 3000;
    r5.len = 16;
    r5.jit = 0;
    int64_t t0 = r5.t0;
    state(t0 + r5.delay, [&] { std::vector<uint8_t> x = {1, 1}; auto bb = blk4(t0); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    run(r5, t0 + (int64_t)(7 * r5.period));
    g_each = nullptr;
    int64_t off = seq::stats().offset;
    auto at = [&](double step) {
      int64_t h = t0 + (int64_t)(step * r5.period + r5.period / 3) + off - lat;
      int a = -1, b = -1;
      for (auto& e : ta) if (e.first <= h) a = e.second;
      for (auto& e : tb) if (e.first <= h) b = e.second;
      return std::make_pair(a, b);
    };
    auto s1 = at(1), s2 = at(2), s3 = at(3), s4 = at(4), s5 = at(5);
    printf("  ins locks: A CUT / B SHPE by step: %d/%d %d/%d %d/%d %d/%d %d/%d\n", s1.first, s1.second, s2.first, s2.second,
           s3.first, s3.second, s4.first, s4.second, s5.first, s5.second);
    CHECK(s1.first == 80 && s1.second == 0);   // step 2: the knobs
    CHECK(s2.first == 20 && s2.second == 0);   // step 3: A CUT locked
    CHECK(s3.first == 20);                     // step 4: empty, the lock holds
    CHECK(s4.first == 80 && s4.second == 9);   // step 5: A back, B SHPE locked
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> x = {1, 0}; auto bb = blk4(now - 700); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    CHECK(ins::control.play[vi][0][0] == 80 && ins::control.play[vi][1][1] == 0);   // STOP: the knobs
    CHECK(cmd("seq=lock:1:3:25:-1").find("locks") != std::string::npos && seq::lock_of(bt->step[2], seq::kLockIns) == -1);
    ins::reset();
  }
  // ---- rui1037, the step preview: seq mode on BD (the page closed), T9 held + YES plays step 9's chord (C E G, each its
  // velocity) with its lock (P2 100) at the next block, stopped; each note ends after its length (half a step), the lock's
  // base back after the last; YES's release and T9's are taken, the step not toggled, nothing recorded. Then playing:
  // the same while BD's ticks run. No note left held (poly::held_notes) after either.
  {
    seq::Track* bt = seq::edit_track(0, true);
    CHECK(bt);
    seq::clear_steps(*bt, 0, seq::kMaxSteps);   // BD: step 9 alone
    seq::Step& s9 = bt->step[8];
    s9 = seq::Step();
    s9.flags = 1, s9.prob = 100;
    const int pitches[3] = {60, 64, 67}, vels[3] = {90, 70, 50};
    for (int i = 0; i < 3; ++i) seq::add_note(s9, {(uint8_t)pitches[i], (uint8_t)vels[i], (uint16_t)(seq::kSub / 2), 0, 0});
    CHECK(seq::set_lock(s9, 1, 100 << 7));
    seq::touched();
    report(kFm | 0, {0, 30, 64, 127, 64, 64, 100, 127});
    const int vi = remote_track_voice(0);
    CHECK(vi >= 0 && vi < 8);
    direct_state({8, 1, 0});
    seq::view_set(0, 0, -1, false);
    for (int i = 0; i < 4; ++i) block();
    const seq::Step before = s9;
    uint32_t rec = seq::stats().recorded, on0 = seq::stats().notes_on, off0 = seq::stats().notes_off;
    uint32_t pv0 = seq::stats().previews;
    key(8, true);
    advance_ms(100);
    key(0x44, true);
    int64_t f[512];
    uint8_t on[512], vo[512], no[512];
    int n = seq::pending(f, on, vo, no, 512), ons = 0;
    for (int i = 0; i < n; ++i) ons += on[i] == 1 && vo[i] == vi && (no[i] == 60 || no[i] == 64 || no[i] == 67);
    CHECK(seq::stats().previews == pv0 + 1 && ons == 3);
    block();
    printf("  preview (stopped): notes on %u, P2 %.0f, held %d\n", seq::stats().notes_on - on0, v[vi].p[1],
           poly::held_notes(vi));
    CHECK(seq::stats().notes_on - on0 == 3 && v[vi].p[1] == 100 && poly::held_notes(vi) == 3);
    key(0x44, false);
    advance_ms(100);
    key(8, false);   // held 200 ms: a tap's time, but YES used it - no toggle
    for (int i = 0; i < 600; ++i) block();   // half a step at the reported tempo (<= 120 BPM: 3000 frames, 250 blocks)
    printf("  preview (stopped): after the gate notes off %u, P2 %.0f, held %d\n", seq::stats().notes_off - off0,
           v[vi].p[1], poly::held_notes(vi));
    CHECK(seq::stats().notes_off - off0 == 3 && v[vi].p[1] == 30 && poly::held_notes(vi) == 0);
    CHECK(s9.n == 3 && !memcmp(&s9, &before, sizeof s9) && seq::stats().recorded == rec);
    // YES without a held trig: not the preview's (the page closed: not taken)
    CHECK(!seq::ui_key(0x44, true, false) && !seq::ui_key(0x44, false, false) && seq::stats().previews == pv0 + 1);
    // playing: BD's ticks; T9 + YES mid-bar
    Run r6;
    r6.t0 = (int64_t)seq::frames() + 3000;
    r6.len = 16;
    r6.jit = 0;
    int64_t t0 = r6.t0;
    state(t0 + r6.delay, [&] { std::vector<uint8_t> x = {1, 1}; auto bb = blk4(t0); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    run(r6, t0 + (int64_t)(4.5 * r6.period));   // between steps 5 and 6: nothing of BD's sounds there
    on0 = seq::stats().notes_on;
    key(8, true);
    key(0x44, true);
    key(0x44, false);
    key(8, false);
    run(r6, (int64_t)seq::frames() + 2 * kBlock);
    printf("  preview (playing): notes on %u, held %d\n", seq::stats().notes_on - on0, poly::held_notes(vi));
    CHECK(seq::stats().notes_on - on0 == 3 && seq::stats().previews == pv0 + 2);
    run(r6, t0 + (int64_t)(5.9 * r6.period));   // past the preview's half step, before step 7
    CHECK(poly::held_notes(vi) == 0 && !memcmp(&s9, &before, sizeof s9));
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> x = {1, 0}; auto bb = blk4(now - 700); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    CHECK(poly::held_notes(vi) == 0);
    direct_state({8, 0, 0x7F});
  }
  // ---- rui1040: trig conditions (seq.h kCond*): each condition's rule, PRE / NEI from the results, probability, the
  // COND knob's order and texts, the file (v3 only with a condition), the planner over loops (1:2 on step 1 of a 4-step
  // track: loops 1 and 3 play it, loop 2 does not), the TRIG view (TRIG toggles it, a held step's COND turned)
  {
    seq::Step c = {};
    c.flags = 1, c.prob = 100;
    CHECK(seq::cond_pass(c, 0, 5));   // no condition
    auto with = [&](int k) { seq::Step x = c; seq::cond_set(x, k); return x; };
    int a = 0, b = 0;
    CHECK(seq::cond_ab(seq::kCondAB, &a, &b) && a == 1 && b == 2);
    CHECK(seq::cond_ab(seq::kConds - 1, &a, &b) && a == 8 && b == 8);
    CHECK(seq::cond_ab(seq::kCondAB + 3, &a, &b) && a == 2 && b == 3);   // 1:2 2:2 1:3 2:3
    seq::Step c12 = with(seq::kCondAB), c23 = with(seq::kCondAB + 3);
    CHECK(seq::cond_pass(c12, 0, 0) && !seq::cond_pass(c12, 0, 1) && seq::cond_pass(c12, 0, 2));
    CHECK(!seq::cond_pass(c23, 0, 0) && seq::cond_pass(c23, 0, 1) && !seq::cond_pass(c23, 0, 2) && seq::cond_pass(c23, 0, 4));
    seq::Step f1 = with(seq::kCond1st), nf1 = with(seq::kCondNot1st);
    CHECK(seq::cond_pass(f1, 0, 0) && !seq::cond_pass(f1, 0, 1) && !seq::cond_pass(nf1, 0, 0) && seq::cond_pass(nf1, 0, 3));
    seq::Step fl = with(seq::kCondFill), nfl = with(seq::kCondNotFill);
    direct_state({12, 1});   // 2D 0C FILL held
    CHECK(seq::cond_pass(fl, 0, 0) && !seq::cond_pass(nfl, 0, 0));
    direct_state({12, 0});
    CHECK(!seq::cond_pass(fl, 0, 0) && seq::cond_pass(nfl, 0, 0));
    seq::Step pre = with(seq::kCondPre), npre = with(seq::kCondNotPre), nei = with(seq::kCondNei), nnei = with(seq::kCondNotNei);
    CHECK(!seq::cond_pass(c12, 3, 1));   // track 3's last result: false
    CHECK(!seq::cond_pass(pre, 3, 0) && seq::cond_pass(npre, 3, 0) && !seq::cond_pass(pre, 3, 0));   // PRE sets nothing
    CHECK(!seq::cond_pass(nei, 4, 0) && seq::cond_pass(nnei, 4, 0));   // NEI: track 3's (the track before)
    CHECK(seq::cond_pass(c12, 3, 2));   // true now
    CHECK(seq::cond_pass(pre, 3, 0) && !seq::cond_pass(npre, 3, 0) && seq::cond_pass(nei, 4, 0) && !seq::cond_pass(nnei, 4, 0));
    CHECK(!seq::cond_pass(nei, 0, 0));   // track 0 has no track before it
    seq::Step p50 = c;
    p50.prob = 50;
    int hits = 0;
    for (int i = 0; i < 1000; ++i) hits += seq::cond_pass(p50, 0, i);
    CHECK(hits > 400 && hits < 600);
    char tb[16];
    seq::Step k = c;
    seq::cond_knob_set(k, 0);
    CHECK(!strcmp(seq::cond_text(k, tb, sizeof tb), "---") && k.prob == 100 && !seq::cond_of(k));
    seq::cond_knob_set(k, 1);
    CHECK(!strcmp(seq::cond_text(k, tb, sizeof tb), "FILL"));
    seq::cond_knob_set(k, 8);
    CHECK(!strcmp(seq::cond_text(k, tb, sizeof tb), "!1ST"));
    seq::cond_knob_set(k, 9);
    CHECK(!strcmp(seq::cond_text(k, tb, sizeof tb), "1:2") && seq::cond_knob(k) == 9);
    seq::cond_knob_set(k, seq::kConds - 1);
    CHECK(!strcmp(seq::cond_text(k, tb, sizeof tb), "8:8"));
    seq::cond_knob_set(k, seq::kConds + 36);
    CHECK(!strcmp(seq::cond_text(k, tb, sizeof tb), "37%") && k.prob == 37 && !seq::cond_of(k) && seq::cond_knob(k) == seq::kConds + 36);
    CHECK((k.flags & 3) == 1);   // on, not muted: the condition bits only
    // the file: version 2 without a condition, 3 with one; the condition read back; a copy carries it
    static seq::Pattern pc, qc;
    seq::clear(pc, 7);
    seq::Track* tc = seq::track_of(pc, 2, true);
    tc->on = 1;   // (per pattern poly: a poly track, as the old-file rule reads it)
    seq::add_note(tc->step[0], {60, 100, 24, 0, 0});
    static uint8_t fb[1 << 16];
    int nb = seq::pack(pc, fb, sizeof fb);
    CHECK(nb > 0 && fb[4] == 2);
    seq::cond_set(tc->step[0], seq::kCondAB + 5);   // 3:3
    nb = seq::pack(pc, fb, sizeof fb);
    CHECK(nb > 0 && fb[4] == 3 && seq::unpack(qc, 7, fb, nb));
    CHECK(seq::cond_of(seq::track_of(qc, 2)->step[0]) == seq::kCondAB + 5 && (seq::track_of(qc, 2)->step[0].flags & 3) == 1);
    printf("  conditions: A:B, 1ST, FILL, PRE, NEI, %d/1000 at 50%%, the knob, the file v3\n", hits);
    // the planner: BD 4 steps, step 1 C5 on 1:2, step 2 D5 always; three loops
    seq::Track* bt = seq::edit_track(0, true);
    CHECK(bt);
    seq::clear_steps(*bt, 0, seq::kMaxSteps);
    bt->length = 4, bt->swing = 0;
    seq::add_note(bt->step[0], {72, 100, 24, 0, 0});
    seq::add_note(bt->step[1], {74, 100, 24, 0, 0});
    seq::cond_set(bt->step[0], seq::kCondAB);
    seq::touched();
    Run rc;
    rc.t0 = (int64_t)seq::frames() + 3000;
    rc.len = 4;
    rc.jit = 0;
    int64_t t0 = rc.t0;
    state(t0 + rc.delay, [&] { std::vector<uint8_t> x = {1, 1}; auto bb = blk4(t0); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    g_fired.clear();
    run(rc, t0 + (int64_t)(11.5 * rc.period));   // steps 0..11: three loops
    int n72 = 0, n74 = 0;
    for (const Fired& f : g_fired) n72 += f.note == 72, n74 += f.note == 74;
    printf("  conditions, planned: 1:2 step %d of 3 loops, the plain step %d\n", n72, n74);
    CHECK(n72 == 2 && n74 == 3);
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> x = {1, 0}; auto bb = blk4(now - 700); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    // the TRIG view: seq mode on BD; TRIG toggles notes / trig values; T2 held + E (COND) +9 -> 1:2 on step 2; TRIG again:
    // the notes; T2's release toggles nothing
    direct_state({8, 1, 0});
    seq::view_set(0, 0, -1, false);
    CHECK(!seq::view().trigview);   // (the SEQ page shown: ui_key / ui_enc as remote.cpp calls them with it)
    CHECK(seq::ui_key(0x30, true, true) && seq::ui_key(0x30, false, true) && seq::view().trigview);
    CHECK(seq::ui_key(1, true, true) && seq::ui_enc(4, 9, false, true));
    advance_ms(50);
    seq::ui_key(1, false, true);
    CHECK(seq::cond_of(bt->step[1]) == seq::kCondAB && bt->step[1].n == 1);
    CHECK(seq::ui_enc(1, 5, false, true) && bt->dvel == 105);   // no held step: the track's defaults (VEL)
    bt->dvel = 100;
    // rui1050: TRIG cycles DEFAULTS -> SCALE -> RANDOM -> the notes
    CHECK(seq::ui_key(0x30, true, true) && seq::view().tpage == seq::kTvScale);
    CHECK(seq::ui_key(0x30, true, true) && seq::view().tpage == seq::kTvRandom);
    CHECK(seq::ui_key(0x30, true, true) && !seq::view().trigview);
    direct_state({8, 0, 0x7F});
    printf("  TRIG view: toggled, COND turned on a held step\n");
  }
  // ---- rui1040 bug 5: the Rytm's normal-mode pads send note t (0..11, the pad's track) on the auto channel (14) - a
  // trigger, not a pitch: no step entry under a held trig; a CHRO note (12..) is one. Selecting another track (49 02)
  // closes the SEQ page the seq mode opened when that track is no seq track
  {
    seq::Track* bt = seq::edit_track(0, true);
    seq::clear_steps(*bt, 0, seq::kMaxSteps);
    direct_state({8, 1, 0});
    CHECK(seq::ui_key(4, true, true));
    poly::midi(0x9D, 1, 100), poly::midi(0x8D, 1, 0);   // the SD pad (normal mode)
    CHECK(bt->step[4].n == 0);
    poly::midi(0x9D, 40, 100), poly::midi(0x8D, 40, 0);   // a CHRO pad
    CHECK(bt->step[4].n == 1);
    // 1042 (the CHRO leftover): TRK held on the Rytm (the page's caught TRK, 31) + a pad is a track selection; in CHRO
    // stock still sends the pad's chromatic note before the selection report: not a note (no step entry, no voice)
    uint32_t sel0 = poly::stats().trk_pads, live0 = poly::stats().live_notes;
    key(0x86, true);
    poly::midi(0x9D, 41, 100), poly::midi(0x8D, 41, 0);
    CHECK(bt->step[4].n == 1 && poly::stats().trk_pads == sel0 + 1 && poly::stats().live_notes == live0);
    key(0x86, false);
    poly::midi(0x9D, 43, 100), poly::midi(0x8D, 43, 0);   // TRK released: a CHRO note again
    CHECK(bt->step[4].n == 2 && poly::stats().trk_pads == sel0 + 1);
    key(0x86, true);
    {   // the page closed while TRK is held (TRK + YES leaves): the Rytm sends no TRK release then
      auto m = msg(0x30, {0, 0, 3});
      engine_sysex(m.data(), (int)m.size());
    }
    poly::midi(0x9D, 45, 100), poly::midi(0x8D, 45, 0);
    CHECK(bt->step[4].n == 3 && poly::stats().trk_pads == sel0 + 1);
    printf("  1042: TRK held + a CHRO pad: a selection, no note; a note again after TRK's release or the page's close\n");
    advance_ms(50);
    seq::ui_key(4, false, true);
    direct_state({8, 0, 0x7F});
    printf("  rui1040: normal pads' triggers no step entry, a CHRO note one\n");
    // fix1040b: BD (a seq track) its own pad in the normal mode: the pad's trigger note and the voice's 0x04 meet within
    // kBindMs, either order -> played once (seq pads +1); a 0x04 without a pad (the Rytm's own trig): no note
    CHECK(seq::owns_track(0) && remote_track_voice(0) >= 0);
    const int bvi = remote_track_voice(0);
    uint32_t sp0 = poly::stats().seq_pads;
    poly::midi(0x9D, 0, 100);                               // pad first
    CHECK(poly::seq_pad(bvi, 60, 100));                     // its 0x04: play it
    poly::midi(0x8D, 0, 0);
    advance_ms(200);
    CHECK(!poly::seq_pad(bvi, 60, 100));                    // 0x04 first (held for the pad) ...
    poly::midi(0x9D, 0, 90);                                // ... the pad plays it
    poly::midi(0x8D, 0, 0);
    CHECK(poly::stats().seq_pads == sp0 + 2);
    advance_ms(200);
    CHECK(!poly::seq_pad(bvi, 60, 100));                    // the Rytm's own trig: no pad, no note
    advance_ms(200);
    poly::midi(0x9D, 1, 100), poly::midi(0x8D, 1, 0);       // SD's pad: not a seq track
    CHECK(poly::stats().seq_pads == sp0 + 2);
    printf("  fix1040b: a seq track's own pad plays its voice (either order), the Rytm's own trig and SD's pad do not\n");
  }
  // ---- 1042 data safety: a pattern file that does not load (a newer Sidecar's version, a bad CRC, cut short) is locked:
  // edits stay in memory, the file is never written over (before: the next save wrote an empty pattern + the edit)
  {
    while (seq::idle()) {}
    std::string dir = card + "/.rytm/seq/SEQTEST/";
    FILE* fp = fopen((dir + "p000.sqp").c_str(), "rb");
    CHECK(fp);
    std::vector<uint8_t> good(65536);
    good.resize(fread(good.data(), 1, good.size(), fp));
    fclose(fp);
    CHECK(good.size() > 30 && !memcmp(good.data(), "SQP1", 4));
    std::vector<uint8_t> newer = good, crc = good, cut(good.begin(), good.begin() + good.size() / 2);
    newer[4] = 200;                    // a version this build does not know (a newer Sidecar's)
    crc[good.size() / 2] ^= 0x5A;      // a damaged byte: the CRC fails
    const std::vector<uint8_t>* files[3] = {&newer, &crc, &cut};
    const int why[3] = {1, 2, 2};
    for (int i = 0; i < 3; ++i) {
      int idx = 40 + i;
      char fn[32];
      snprintf(fn, sizeof fn, "p%03d.sqp", idx);
      fp = fopen((dir + fn).c_str(), "wb");
      fwrite(files[i]->data(), 1, files[i]->size(), fp);
      fclose(fp);
      refill();   // (the RAM store: the file the test wrote, as a project load reads it)
      direct_state({2, (uint8_t)idx, 0x7F});
      while (seq::idle()) {}
      CHECK(seq::view().pattern == idx && seq::view().locked == why[i]);
      seq::Track* t = seq::edit_track(0, true);   // an edit on it: kept in memory, never saved
      CHECK(t);
      if (t) seq::add_note(t->step[3], {72, 100, 96, 0, 0});
      seq::touched();
      uint32_t refused = seq::stats().save_refused;
      advance_ms(1500);
      seq::tick(now_ms());
      while (seq::idle()) {}
      direct_state({2, 0, 0x7F});   // away (the dirty pattern goes to the save buffer, then idle saves it) and back
      while (seq::idle()) {}
      direct_state({2, (uint8_t)idx, 0x7F});
      while (seq::idle()) {}
      fp = fopen((dir + fn).c_str(), "rb");
      std::vector<uint8_t> now(65536);
      now.resize(fp ? fread(now.data(), 1, now.size(), fp) : 0);
      if (fp) fclose(fp);
      CHECK(now == *files[i]);   // the original bytes, unchanged
      CHECK(seq::stats().save_refused > refused && seq::view().locked == why[i]);
      printf("  1042: %s file: locked (%d), the edit refused on the card, the file unchanged\n",
             i == 0 ? "a newer" : i == 1 ? "a CRC-bad" : "a truncated", seq::view().locked);
    }
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    CHECK(seq::view().locked == 0);
    // a project change with an unsaved edit: the edit saved under the old project's folder, never the new one's
    seq::Track* t = seq::edit_track(0, true);
    CHECK(t);
    if (t) seq::add_note(t->step[9], {77, 100, 96, 0, 0});
    seq::touched();
    struct stat sb;
    CHECK(stat((card + "/.rytm/seq/OTH/p000.sqp").c_str(), &sb) != 0);
    const uint8_t other[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 0xF7};
    engine_sysex(other, sizeof other);
    loaded();
    while (seq::idle()) {}
    CHECK(stat((card + "/.rytm/seq/OTH/p000.sqp").c_str(), &sb) != 0);   // nothing of SEQTEST's written into OTH
    engine_sysex(proj, sizeof proj);
    loaded();
    while (seq::idle()) {}
    t = seq::edit_track(0, false);
    bool has77 = false;
    for (int k = 0; t && k < t->step[9].n; ++k) has77 |= t->step[9].note[k].pitch == 77;
    CHECK(has77);   // the edit, in SEQTEST's own file
    printf("  1042: a project change saves the old project's edit in its own folder\n");
  }
  // final review (Important 1), the locked case: an old project's pattern locked by 1042 (here A42, CRC-bad above) and
  // edited during the next project's fill keeps ITS lock when it comes back - from the save buffer at a switch back
  // (install) or into the prefetch - never the stand-in's kLockLoading cleared to "editable": its file is not written over
  {
    while (seq::idle()) {}
    for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();
    std::string fn = card + "/.rytm/seq/SEQTEST/p041.sqp";
    auto bytes = [&] {
      std::vector<uint8_t> b(65536);
      FILE* f = fopen(fn.c_str(), "rb");
      b.resize(f ? fread(b.data(), 1, b.size(), f) : 0);
      if (f) fclose(f);
      return b;
    };
    std::vector<uint8_t> orig = bytes();
    CHECK(orig.size() > 30);
    const uint8_t other[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 0xF7};
    for (int way = 0; way < 2; ++way) {   // 0: back to it (install), 1: queued back (the prefetch)
      direct_state({2, 41, 0x7F});
      while (seq::idle()) {}
      CHECK(!strcmp(project, "SEQTEST") && seq::view().pattern == 41 && seq::view().locked == 2);
      engine_sysex(other, sizeof other);
      if (rename::state() == rename::kAsk) rename::answer(false);
      CHECK(project_loading() && seq::view().locked == 2);
      seq::Track* t = seq::edit_track(0, true);   // kept in memory (1042), never saved
      CHECK(t);
      if (t) seq::add_note(t->step[5], {70, 100, 96, 0, 0});
      seq::touched();
      if (way == 0) {
        direct_state({2, 0, 0x7F});
        seq::idle();   // (A01 installed: A42's edit to the save buffer)
        direct_state({2, 41, 0x7F});
        seq::idle();   // back to it, before the idle pass saved the buffer
      } else {
        direct_state({2, 0, 41});
        while (seq::idle()) {}
      }
      CHECK(seq::pattern_locked(41) == 2);
      loaded();
      while (seq::idle()) {}
      for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();
      CHECK(bytes() == orig);   // SEQTEST's damaged file, unchanged
      direct_state({2, 0, 0x7F});
      while (seq::idle()) {}
      engine_sysex(proj, sizeof proj);
      loaded();
      while (seq::idle()) {}
    }
    printf("  store: an old project's locked pattern edited during a fill keeps its lock (install and prefetch)\n");
  }
  // ---- 1042: the Rytm's SAVE / RELOAD PATTERN and SAVE / RELOAD TRACK (2D 0A ops 16..19, rytm-mods 0026 1042): the
  // Sidecar keeps the saved state per pattern (pNNN.sqs); a reload brings it back (the working copy first kept as
  // pNNN.prv); a reload with no saved state keeps the steps
  {
    while (seq::idle()) {}
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    std::string dir = card + "/.rytm/seq/SEQTEST/";
    struct stat sb;
    unlink((dir + "p000.sqs").c_str());
    auto pitches = [](int step) {
      std::vector<int> v;
      const seq::Track* t = seq::edit_track(0, false);
      for (int k = 0; t && k < t->step[step].n; ++k) v.push_back(t->step[step].note[k].pitch);
      return v;
    };
    seq::Track* t = seq::edit_track(0, true);
    seq::clear_steps(*t, 0, seq::kMaxSteps);
    seq::add_note(t->step[0], {60, 100, 96, 0, 0});
    seq::touched();
    edit(seq::kPReload, 0, 0x7F, 1);   // no saved state yet: the steps kept
    CHECK(pitches(0) == std::vector<int>{60});
    edit(seq::kPSave, 0, 0x7F, 1);     // SAVE PATTERN: C4 on step 1 saved
    CHECK(stat((dir + "p000.sqs").c_str(), &sb) == 0);
    t = seq::edit_track(0, true);
    seq::add_note(t->step[0], {64, 100, 96, 0, 0});
    seq::add_note(t->step[5], {67, 100, 96, 0, 0});
    seq::touched();
    edit(seq::kPReload, 0, 0x7F, 1);   // RELOAD PATTERN: back to the saved C4 alone
    CHECK(pitches(0) == std::vector<int>{60} && pitches(5).empty());
    CHECK(stat((dir + "p000.prv").c_str(), &sb) == 0);   // the working copy before it, kept
    t = seq::edit_track(0, true);
    seq::add_note(t->step[2], {62, 100, 96, 0, 0});
    seq::touched();
    edit(seq::kTSave, 0, 0, 1);        // SAVE TRACK BD: step 3's D4 saved
    t = seq::edit_track(0, true);
    seq::clear_steps(*t, 0, seq::kMaxSteps);
    seq::touched();
    edit(seq::kTReload, 0, 0, 1);      // RELOAD TRACK BD
    CHECK(pitches(0) == std::vector<int>{60} && pitches(2) == std::vector<int>{62});
    advance_ms(1500);
    seq::tick(now_ms());
    while (seq::idle()) {}
    printf("  1042: SAVE / RELOAD PATTERN and TRACK follow the Rytm; a reload without a saved state keeps the steps\n");
  }
  // ---- 1042: a DIRECT / TEMP JUMP (rytm-mods 0026 1042: 03 at the new pattern's first step, which is the old position,
  // not its step 0): A02's step 11 plays at A01's position + 2 steps after the jump at step 9
  {
    while (seq::idle()) {}
    direct_state({2, 1, 0x7F});
    while (seq::idle()) {}
    seq::Track* t1 = seq::edit_track(0, true);
    seq::clear_steps(*t1, 0, seq::kMaxSteps);
    t1->length = 64;
    seq::add_note(t1->step[10], {90, 100, 48, 0, 0});
    seq::touched();
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    seq::Track* t0 = seq::edit_track(0, true);
    seq::clear_steps(*t0, 0, seq::kMaxSteps);
    seq::add_note(t0->step[3], {50, 100, 48, 0, 0});
    seq::touched();
    Run rj;
    rj.t0 = (int64_t)seq::frames() + 3000;
    rj.jit = 0;
    state(rj.t0 + rj.delay - 1, [&] { std::vector<uint8_t> x = {1, 1}; auto bb = blk4(rj.t0); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    run(rj, rj.t0 + (int64_t)(8.5 * rj.period));
    direct_state({2, 0, 1});   // A02 queued (the jump: no wait for the bar)
    int64_t sw = rj.t0 + (int64_t)llround(9 * rj.period);
    state(sw + rj.delay - 1, [&] { std::vector<uint8_t> x = {3, 0, 1}; auto bb = blk4(sw); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    rj.len = 64;   // A02's ticks from step 9 on (rj.next_k is 9)
    g_fired.clear();
    run(rj, rj.t0 + (int64_t)(13 * rj.period));
    int64_t want = (int64_t)llround(10 * rj.period);
    bool got = false;
    for (const Fired& f : g_fired)
      if (f.note == 90) got = true, printf("  1042 jump: A02 step 11 planned %lld frames from the Rytm's step 11\n",
                                          (long long)(f.planned - (rj.t0 + want) - seq::stats().offset));
    CHECK(got && seq::view().pattern == 1);
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> x = {1, 0}; auto bb = blk4(now - 700); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    while (seq::idle()) {}
  }
  // ---- the RAM store, phase 2 (2026-10-26): the prefetch and the switch read the pattern from the store, never the
  // card (measured on the device: the prefetch's .sqp read cost core 3 ~14 ms at every switch)
  {
    while (seq::idle()) {}
    auto has = [](int step, int pitch) {
      const seq::Track* t = seq::edit_track(0, false);
      for (int k = 0; t && k < t->step[step].n; ++k)
        if (t->step[step].note[k].pitch == pitch) return true;
      return false;
    };
    // pattern_switch_reads_ram_only: patterns 0 and 1 saved (a note each), the project loaded again
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    seq::Track* t = seq::edit_track(0, true);
    seq::clear_steps(*t, 0, seq::kMaxSteps);
    t->length = 16;
    seq::add_note(t->step[1], {61, 100, 48, 0, 0});
    seq::touched();
    direct_state({2, 1, 0x7F});
    while (seq::idle()) {}
    t = seq::edit_track(0, true);
    seq::clear_steps(*t, 0, seq::kMaxSteps);
    t->length = 16;
    seq::add_note(t->step[0], {63, 127, 96, 0, 0});
    seq::touched();
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    refill();
    CHECK(seq::view().pattern == 0 && has(1, 61));
    host_card_model(true);
    uint64_t c3 = host_card_us(3);
    direct_state({2, 0, 1});   // 1 queued: the prefetch
    while (seq::idle()) {}
    uint32_t pre = seq::stats().prefetched, sw0 = seq::stats().switches;
    Run rs;
    rs.t0 = (int64_t)seq::frames() + 3000;
    rs.len = 16;
    run(rs, rs.t0 + (int64_t)(15.5 * rs.period));
    int64_t sw = rs.t0 + (int64_t)llround(16 * rs.period);
    state(sw + rs.delay, [&] { std::vector<uint8_t> b = {3, 0, 1}; auto x = blk4(sw); b.insert(b.end(), x.begin(), x.end()); return b; }());
    g_fired.clear();
    run(rs, sw + (int64_t)(4 * rs.period));
    CHECK(seq::view().pattern == 1 && seq::stats().prefetched == pre + 1 && seq::stats().switches == sw0 + 1);
    bool got = false;
    for (const Fired& f : g_fired) got |= f.note == 63;
    CHECK(got && has(0, 63));   // pattern 1's step 0, from the store
    int64_t now = (int64_t)seq::frames();
    state(now, [&] { std::vector<uint8_t> x = {1, 0}; auto bb = blk4(now - 700); x.insert(x.end(), bb.begin(), bb.end()); return x; }());
    deliver(now);
    for (int i = 0; i < 10; ++i) block();
    direct_state({2, 0, 0x7F});   // and back
    while (seq::idle()) {}
    CHECK(seq::view().pattern == 0 && has(1, 61));
    printf("  store: prefetch, switch, switch back: %llu us of card time on the control core\n",
           (unsigned long long)(host_card_us(3) - c3));
    CHECK(host_card_us(3) == c3);
    // edit_switch_back: an edit on 0, a switch to 1 (the edit's save posted, held by the card core) and back before it
    // lands: the edit is there (the store has it), and still no card time on the control core
    cardq::set_worker(true);
    t = seq::edit_track(0, true);
    seq::add_note(t->step[7], {70, 100, 48, 0, 0});
    seq::touched();
    direct_state({2, 1, 0x7F});
    while (seq::idle()) {}   // the edit's save: posted
    CHECK(fmt::pending("/.rytm/seq/SEQTEST/p000.sqp"));
    direct_state({2, 0, 0x7F});
    while (seq::idle()) {}
    CHECK(seq::view().pattern == 0 && has(7, 70) && has(1, 61));
    CHECK(host_card_us(3) == c3);
    host_card_model(false);
    for (int i = 0; i < 1000 && fmt::post_busy(); ++i) {
      fmt::post_step();
      host_card_core(0);
      while (cardq::service()) {}
      host_card_core(3);
    }
    CHECK(!fmt::post_busy());
    cardq::set_worker(false);
    refill();   // landed on the card: the project loaded again has it
    CHECK(seq::view().pattern == 0 && has(7, 70));
    printf("  store: an edit, a switch away and back before its save lands: the edit is there\n");
    // round 2: during a fill (the project loaded again), on a pattern switched to meanwhile (locked: loading) - a COPY
    // is refused (the clipboard kept), a PASTE is refused and leaves no undo: the Rytm's UNDO PASTE after the load
    // changes nothing (before: the empty stand-in was undone onto the real pattern and its file written over)
    {
      direct_state({2, 1, 0x7F});
      while (seq::idle()) {}
      CHECK(has(0, 63));
      edit(seq::kPCopy, 1, 0x7F, 1);   // the clipboard: pattern 1
      uint32_t r0 = seq::stats().loading_refused;
      project_fill_begin();
      direct_state({2, 0, 0x7F});
      while (seq::idle()) {}
      CHECK(seq::view().pattern == 0 && seq::view().locked == 3);
      edit(seq::kPCopy, 0, 0x7F, 1);    // refused
      edit(seq::kPPaste, 0, 0x7F, 1);   // refused
      CHECK(used(0) == 0 && seq::stats().loading_refused == r0 + 2);
      CHECK(cmd("seq=lat:300").find("still loading") != std::string::npos && seq::view().latency != 300);
      loaded();
      CHECK(seq::view().locked == 0 && has(1, 61) && has(7, 70));
      std::vector<uint8_t> before;
      {
        FILE* fp = fopen((card + "/.rytm/seq/SEQTEST/p000.sqp").c_str(), "rb");
        before.resize(65536);
        before.resize(fp ? fread(before.data(), 1, before.size(), fp) : 0);
        if (fp) fclose(fp);
      }
      uint32_t saves0 = seq::stats().saves;
      edit(seq::kPUndo, 0, 0x7F, 1, 0, 2);   // UNDO PASTE: nothing to undo
      advance_ms(1500);
      while (seq::idle()) {}
      CHECK(has(1, 61) && has(7, 70) && seq::stats().saves == saves0);
      {
        FILE* fp = fopen((card + "/.rytm/seq/SEQTEST/p000.sqp").c_str(), "rb");
        std::vector<uint8_t> now(65536);
        now.resize(fp ? fread(now.data(), 1, now.size(), fp) : 0);
        if (fp) fclose(fp);
        CHECK(now == before && !before.empty());
      }
      edit(seq::kPPaste, 0, 0x7F, 1);   // loaded now: the paste is pattern 1's (the clipboard was kept)
      CHECK(has(0, 63));
      edit(seq::kPUndo, 0, 0x7F, 1, 0, 2);
      CHECK(has(1, 61) && !has(0, 63));
      // an old pattern prefetched before a fill began is loaded anew when it is switched to during it: not editable
      direct_state({2, 0, 1});   // 1 queued: prefetched
      while (seq::idle()) {}
      project_fill_begin();
      direct_state({2, 1, 0x7F});
      while (seq::idle()) {}
      CHECK(seq::view().pattern == 1 && seq::view().locked == 3);
      loaded();
      CHECK(seq::view().locked == 0 && has(0, 63));
      direct_state({2, 0, 0x7F});
      while (seq::idle()) {}
      printf("  store: a copy / paste on a loading pattern refused, no undo left; a prefetched pattern not editable\n");
    }
  }
  printf("%s\n", cmd("seq").c_str());
  printf("test_seq: OK\n");
  return 0;
}
