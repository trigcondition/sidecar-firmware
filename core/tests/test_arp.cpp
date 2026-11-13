// core/tests/test_arp.cpp - the arpeggiator (core/arp/arp.h, backlog 7ae): the code, each mode over octaves, speed, gate and swing on the Rytm's step
// clock and free, the notes coming from a keyboard, the sequencer, a Rytm trig (a chord too), on the MIDI machine (seen on its DIN bytes) and on
// polyphonic and mono Sidecar machines; off changes nothing; every way a note could hang (stop, pause, kit, machine by 33 and by report, hello,
// mode off); 45 both ways, a bad 45; the 2E's pin and clock; the ARP page; a block's cost.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "arp/arp.h"
#include "check.h"
#include "core.h"
#include "inst.h"
#include "keys.h"
#include "midim/midim.h"
#include "poly.h"
#include "seq/seq.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;

struct Ev {
  int64_t h;
  int status, note;
};
static std::vector<Ev> g;
static std::vector<std::vector<uint8_t>> g_rytm;
static void out(const uint8_t* m, int n) {
  g_rytm.emplace_back(m, m + n);
  if (n >= 9 && m[4] == midim::kDinCmd) g.push_back({(int64_t)seq::frames(), m[6] | 0x80, m[7]});
}
static uint64_t g_us = 1000000;
static uint32_t g_ms = 0;
struct Clock {
  bool on = false;
  double period = 6000;
  int64_t t0 = 0, k = 0;
  int delay = 300, speed = 2;
};
static Clock c;
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
static void send(const std::vector<uint8_t>& m) { engine_sysex(m.data(), (int)m.size()); }
static std::vector<std::vector<uint8_t>> g_late;
static std::vector<int64_t> g_late_at;
static void run(int64_t frames) {
  int64_t end = (int64_t)seq::frames() + frames;
  while ((int64_t)seq::frames() < end) {
    int64_t h = (int64_t)seq::frames();
    while (c.on) {
      int64_t tf = c.t0 + (int64_t)llround(c.k * c.period);
      if (tf > h + 2) break;
      auto b = blk4(tf);
      b.insert(b.end(), {1, 0, (uint8_t)(c.k % 16), 16, (uint8_t)c.speed});
      g_late.push_back(msg(0x2C, b)), g_late_at.push_back(tf + c.delay);
      ++c.k;
    }
    for (size_t i = 0; i < g_late.size();)
      if (g_late_at[i] <= h) {
        send(g_late[i]);
        g_late.erase(g_late.begin() + (long)i), g_late_at.erase(g_late_at.begin() + (long)i);
      } else {
        ++i;
      }
    seq::pre_block();
    g_us += 250;
    host_fake_time(true, g_us);
    if (now_ms() != g_ms) g_ms = now_ms(), seq::tick(g_ms), poly::tick(g_ms);
  }
}
static void state(std::vector<uint8_t> b) { send(msg(0x2D, b)); }
static void tempo(double bpm) {
  int t = (int)(bpm * 120);
  state({seq::kTempo, (uint8_t)(t >> 14 & 0x7F), (uint8_t)(t >> 7 & 0x7F), (uint8_t)(t & 0x7F)});
}
static void transport(int s) {
  auto b = blk4((int64_t)seq::frames());
  std::vector<uint8_t> x = {seq::kTransport, (uint8_t)s};
  x.insert(x.end(), b.begin(), b.end());
  state(x);
}
static void play() {
  c.on = true, c.k = 0, c.t0 = (int64_t)seq::frames() + 480;
  c.t0 -= c.t0 % 32;
  transport(seq::kPlay);
  run(c.delay + 960);
}
static void halt() {
  c.on = false;
  g_late.clear(), g_late_at.clear();
  transport(seq::kStop);
}
static std::vector<int> words(int dst = 32) {
  std::vector<int> p(16, 0);
  p[0] = dst;
  return p;
}
static void rep(int cmd, int vi, int note, int vel, const std::vector<int>& p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vi};
  if (cmd == 0x44 || cmd == 0x04) m.push_back((uint8_t)note), m.push_back((uint8_t)vel);
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)p[k]), m.push_back(0);
  m.push_back(0), m.push_back(0);
  m.push_back(0xf7);
  send(m);
}
static void off(int vi) {
  const uint8_t m[7] = {0xF0, 0x7D, 0x52, 0x54, 0x11, (uint8_t)vi, 0xF7};
  engine_sysex(m, 7);
}
static void machine33(int t, int machine, int vb) {
  const uint8_t m[11] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, (uint8_t)machine, (uint8_t)vb, 0xF7};
  engine_sysex(m, 11);
}
static arp::Cfg cfg(int mode, int speed = 3, int range = 0, int gate = 1, int swing = 0) {
  return {(uint8_t)mode, (uint8_t)speed, (uint8_t)range, (uint8_t)gate, (uint8_t)swing};
}
static void chord(int vi, std::vector<int> ns, int kc = poly::kKeysCh0) {
  for (int n : ns) CHECK(poly::key_on(vi, n, 100, kc));
}
static void unchord(int vi, std::vector<int> ns, int kc = poly::kKeysCh0) {
  for (int n : ns) poly::key_off(vi, n, kc);
}
static std::vector<int> ons() {
  std::vector<int> r;
  for (auto& e : g)
    if ((e.status & 0xF0) == 0x90) r.push_back(e.note);
  return r;
}
static void pairs_closed() {   // every on has its off
  int bal[128] = {0};
  for (auto& e : g) bal[e.note] += (e.status & 0xF0) == 0x90 ? 1 : -1;
  for (int b : bal) CHECK(b == 0);
}
static void same(const std::vector<int>& got, std::vector<int> want) {
  if (got != want) {
    for (int x : got) fprintf(stderr, "%d ", x);
    fprintf(stderr, "\n");
  }
  CHECK(got == want);
}

int main() {
  char tmpl[] = "/tmp/core_arp_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples' '" + card + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  std::vector<uint8_t> a33 = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 12};
  for (int t = 0; t < 12; ++t) a33.push_back(50), a33.push_back(0);
  a33.push_back(0xF7);
  send(a33);
  tempo(120);

  // the code: every value reads back, 0 is OFF at the defaults, past the end reads 0, the Rytm's bytes
  for (int i = 0; i < arp::kCodes; ++i) CHECK(arp::encode(arp::decode(i)) == i);
  CHECK(arp::kCodes <= 1024);
  {
    arp::Cfg d = arp::decode(0);
    CHECK(d.mode == arp::kOff && !strcmp(arp::speed_name(d.speed), "1/16") && d.range == 0 && arp::gate_pct(d.gate) == 50 && arp::swing_pct(d.swing) == 50);
    CHECK(arp::decode(1000).mode == arp::kOff && arp::decode(1023).mode == arp::kOff);
  }
  send({0xF0, 0x7D, 0x52, 0x54, arp::kCmd, 1, 12, 1, 0, 0xF7});   // bad track, short, top bits
  send({0xF0, 0x7D, 0x52, 0x54, arp::kCmd, 1, 0, 1, 8, 0xF7});
  send({0xF0, 0x7D, 0x52, 0x54, arp::kCmd, 1, 0, 0xF7});
  CHECK(!arp::on(0));

  // off: a keyboard chord and a trig play as ever, nothing held here (the MIDI machine's three notes at once)
  rep(0x43, 0, -1, 0, words());
  CHECK(v[0].mi && !arp::on(0));
  chord(0, {60, 64, 67});
  same(ons(), {60, 64, 67});
  unchord(0, {60, 64, 67});
  pairs_closed();
  g.clear();
  rep(0x44, 0, 60, 100, words());
  off(0);
  same(ons(), {60});
  pairs_closed();
  g.clear();

  // the Rytm's report turns it on (voice 0), a keyboard chord: free running at 120 BPM, one note each 1/16, gate 50 %, UP
  send(msg(arp::kCmd, {0, (uint8_t)(arp::encode(cfg(arp::kUp)) & 0x7F), (uint8_t)(arp::encode(cfg(arp::kUp)) >> 7)}));
  CHECK(arp::on(0) && arp::get(0).mode == arp::kUp);
  chord(0, {67, 60, 64});
  run(6000 * 6 + 100);
  same(ons(), {60, 64, 67, 60, 64, 67, 60});
  CHECK(g[2].h - g[0].h >= 5988 && g[2].h - g[0].h <= 6012);   // 1/16 at 120 BPM: 6000 frames (a block's rounding)
  CHECK(g[1].h - g[0].h >= 2988 && g[1].h - g[0].h <= 3012);   // its off at 50 %
  unchord(0, {60, 64, 67});
  CHECK(arp::busy(0));
  run(6000);
  CHECK(!arp::busy(0) && midim::voice_notes(0) == 0);
  pairs_closed();
  g.clear();

  // an off for a note not held here passes on
  arp::set(0, cfg(arp::kOff));
  chord(0, {50});
  arp::set(0, cfg(arp::kUp));
  poly::key_off(0, 50, poly::kKeysCh0);
  CHECK(midim::voice_notes(0) == 0);
  pairs_closed();
  g.clear();

  // the modes, over octaves
  struct {
    int mode, range;
    std::vector<int> want;
  } modes[] = {
      {arp::kDown, 0, {67, 64, 60, 67, 64, 60}},
      {arp::kUpDown, 0, {60, 64, 67, 64, 60, 64}},
      {arp::kUp, 1, {60, 64, 67, 72, 76, 79, 60}},
      {arp::kDown, 1, {79, 76, 72, 67, 64, 60, 79}},
      {arp::kUpDown, 1, {60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60}},
      {arp::kPlayed, 0, {67, 60, 64, 67, 60}},
      {arp::kPlayed, 1, {67, 60, 64, 79, 72, 76, 67}},
  };
  for (auto& m : modes) {
    arp::set(0, cfg(m.mode, 3, m.range));
    chord(0, {67, 60, 64});
    run(6000 * (int64_t)m.want.size() - 3000);
    unchord(0, {67, 60, 64});
    run(6000);
    same(ons(), m.want);
    pairs_closed();
    g.clear();
  }
  {   // RND stays in the chord's octaves and reaches them all; one note in: the note again and again
    arp::set(0, cfg(arp::kRandom, 5, 1));
    chord(0, {60, 64});
    run(1500 * 40);
    unchord(0, {60, 64});
    run(3000);
    int seen[128] = {0};
    for (int n : ons()) ++seen[n];
    CHECK(seen[60] && seen[64] && seen[72] && seen[76]);
    CHECK(seen[60] + seen[64] + seen[72] + seen[76] == (int)ons().size());
    pairs_closed();
    g.clear();
    arp::set(0, cfg(arp::kUp));
    chord(0, {60});
    run(6000 * 2 + 100);
    unchord(0, {60});
    run(6000);
    same(ons(), {60, 60, 60});
    pairs_closed();
    g.clear();
  }
  {   // above 127: an octave down, never out of range
    arp::set(0, cfg(arp::kUp, 3, 2));
    chord(0, {120});
    run(6000 * 3 - 3000);
    unchord(0, {120});
    run(6000);
    same(ons(), {120, 120, 120});
    g.clear();
  }

  // on the Rytm's clock: each interval on its grid point, `latency` early. 1/16 at 6000 frames: ticks at t0 + 6000 k
  arp::set(0, cfg(arp::kUp));
  int lat = 192;
  play();
  int64_t t0 = c.t0;
  run(6000 * 4 - 3000);   // a chord mid-step: its first note waits for the next point
  chord(0, {60, 64});
  int64_t sent = (int64_t)seq::frames();
  run(6000 * 5);
  unchord(0, {60, 64});
  run(6000);
  CHECK(g.size() >= 8);
  CHECK(g[0].h > sent);
  for (size_t i = 0; i < g.size(); ++i) {
    if ((g[i].status & 0xF0) != 0x90) continue;
    int64_t e = (g[i].h + lat - t0) % 6000;
    if (e > 3000) e -= 6000;
    CHECK(e >= -40 && e <= 40);
  }
  pairs_closed();
  g.clear();
  // a trig at a step (it comes after its step): its note at once, in that step, not a step late. Mono on the track, its 0x11 ends it
  arp::set(0, cfg(arp::kUp, 3, 1, 2));
  {
    int64_t next = t0 + (((int64_t)seq::frames() - t0) / 6000 + 2) * 6000;
    run(next + c.delay - (int64_t)seq::frames());
    rep(0x44, 0, 48, 90, words());
    run(24);
    CHECK(ons().size() == 1 && ons()[0] == 48);
    run(6000 * 2);
    same(ons(), {48, 60, 48});
    CHECK(g[0].status == 0x90);
    for (size_t i = 1; i + 1 < g.size(); i += 2) CHECK(g[i + 1].h - g[i].h <= 12);   // gate 100 %: legato, the off with the next on
    rep(0x44, 0, 50, 90, words());   // a new trig over a held one: its own pattern, from its first note
    run(120);
    CHECK(ons().back() == 50);
    off(0);
    run(6000 * 2);
    CHECK(!arp::busy(0));
    pairs_closed();
    g.clear();
  }
  // swing: every second point late by swing / 50 - 1 of the interval
  arp::set(0, cfg(arp::kUp, 3, 0, 0, 2));
  chord(0, {60});
  run(6000 * 8);
  unchord(0, {60});
  run(6000);
  {
    int late = 0, on_time = 0;
    for (auto& e : g) {
      if ((e.status & 0xF0) != 0x90) continue;
      int64_t x = (e.h + lat - t0) % 6000;
      if (x <= 40 || x >= 5960) ++on_time;
      else if (x >= 1920 - 40 && x <= 1920 + 40) ++late;
      else CHECK(false);
    }
    CHECK(late >= 3 && on_time >= 3);
    pairs_closed();
    g.clear();
  }
  // speeds: 1/8T on the clock
  arp::set(0, cfg(arp::kUp, 2));
  chord(0, {60});
  run(8000 * 9);
  unchord(0, {60});
  run(8000);
  {
    int64_t prev = -1;
    int n = 0;
    for (auto& e : g) {
      if ((e.status & 0xF0) != 0x90) continue;
      if (n++ >= 2) CHECK(e.h - prev >= 7980 && e.h - prev <= 8020);   // (the first may be a caught one)
      prev = e.h;
    }
    CHECK(n >= 8);
    pairs_closed();
    g.clear();
  }

  // the sequencer's notes: a chord held by its own offs; a chord change in one block is no gap; ends with its off
  arp::set(0, cfg(arp::kUp));
  seq::PolyCfg pc = {};
  CHECK(inst::seq_on(0, 60, 100, pc) && inst::seq_on(0, 67, 100, pc));
  run(6000 * 2);
  CHECK(ons().size() >= 2);
  inst::seq_off(0, 60), inst::seq_off(0, 67);
  CHECK(inst::seq_on(0, 62, 100, pc));
  run(6000);
  CHECK(ons().back() == 62);
  inst::seq_off(0, -1);
  run(6000);
  CHECK(!arp::busy(0) && midim::voice_notes(0) == 0);
  pairs_closed();
  g.clear();

  // a chord on a poly track, through the planner: its step's notes for its LEN, one at a time
  {
    machine33(0, 50, 0);
    char b[160];
    CHECK(seq::command("seq=track:1:on", b, sizeof b));
    seq::Track* tr = seq::edit_track(0, true);
    CHECK(tr);
    seq::set_chord(tr->step[0], 60, 1, 0, 100, 3 * seq::kSub + 48, 0);
    tr->step[0].flags |= 1;
    seq::touched();
    arp::set(0, cfg(arp::kUp));
    run(4800);
    halt();
    run(4800);
    g.clear();
    play();
    run(6000 * 8);
    same(ons(), {60, 64, 67, 60});
    pairs_closed();
    CHECK(!arp::busy(0));
    g.clear();
    halt();
    CHECK(seq::command("seq=track:1:off", b, sizeof b));
    run(960);
    if (seq::owns_track(0)) {   // (TURN OFF asks nothing on the console; else clear)
      seq::edit_track(0, true)->step[0].n = 0;
    }
    g.clear();
    play();
  }

  // STOP: the trig and the sequencer's notes end, on the machine too; keys run on, free, at the same speed
  rep(0x44, 0, 48, 90, words());
  run(6000);
  transport(seq::kStop), c.on = false, g_late.clear(), g_late_at.clear();
  CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));
  pairs_closed();
  g.clear();
  chord(0, {60});
  run(6000 * 3);
  CHECK(ons().size() >= 3);
  play();
  run(6000);
  transport(seq::kStop), c.on = false, g_late.clear(), g_late_at.clear();
  CHECK(arp::busy(0));
  run(12000);
  size_t before = ons().size();
  run(6000);
  CHECK(ons().size() > before);   // the key is still down
  unchord(0, {60});
  run(6000);
  CHECK(!arp::busy(0) && midim::voice_notes(0) == 0);
  pairs_closed();
  g.clear();
  // pause as stop
  play();
  rep(0x44, 0, 48, 90, words());
  run(6000);
  transport(seq::kPause), c.on = false, g_late.clear(), g_late_at.clear();
  CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));
  transport(seq::kStop);
  pairs_closed();
  g.clear();

  // a kit change, a machine change (by 33, by a report), a hello, the mode OFF: ended now, with keys down, nothing left
  uint8_t kit[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, 5, 0xF7};
  engine_sysex(kit, 8);
  // a 3B naming the same kit (every page open sends one): nothing ends; a real change with a trig just made: the trig stays, keys go
  {
    run(4800);
    chord(0, {60, 64});
    run(100);
    engine_sysex(kit, 8);
    CHECK(arp::busy(0) && midim::voice_notes(0) == 1);
    run(4800 * 3);
    size_t n = ons().size();
    CHECK(n >= 2);
    rep(0x44, 0, 48, 90, words());
    run(100);
    kit[6] = 6;
    engine_sysex(kit, 8);
    CHECK(arp::busy(0));
    run(6000);
    CHECK(ons().back() == 48);
    off(0);
    unchord(0, {60, 64});
    run(6000);
    CHECK(!arp::busy(0) && midim::voice_notes(0) == 0);
    pairs_closed();
    g.clear();
    // a stale trig does go
    rep(0x44, 0, 48, 90, words());
    run(4800 * 6);
    kit[6] = 7;
    engine_sysex(kit, 8);
    CHECK(!arp::busy(0) && midim::voice_notes(0) == 0);
    pairs_closed();
    g.clear();
  }
  const uint8_t hello[11] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 0, 0, 0, 0xF7};
  for (int how = 0; how < 5; ++how) {
    chord(0, {60, 64});
    run(100);
    CHECK(midim::voice_notes(0) == 1);
    if (how == 0) kit[6] = 8, engine_sysex(kit, 8);
    if (how == 1) machine33(0, 40, 0x48), machine33(1, 50, 0x01);
    if (how == 2) rep(0x03, 0, -1, 0, words(0));   // PLAITS now
    if (how == 3) engine_sysex(hello, 11);
    if (how == 4) arp::set(0, cfg(arp::kOff));
    CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));
    run(12000);
    CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));
    size_t n = ons().size();
    unchord(0, {60, 64});
    run(100);
    CHECK(ons().size() == n);
    pairs_closed();
    g.clear();
    send(a33);
    rep(0x43, 0, -1, 0, words());
    arp::set(0, cfg(arp::kUp));
  }

  // a new kit's first step: 45 just before its trig (ext_render), its 3B after: an OFF sound's trig plays as a trig, an arp's starts
  {
    send(msg(arp::kCmd, {0, 0, 0}));
    rep(0x44, 0, 52, 90, words());
    run(24);
    same(ons(), {52});
    kit[6] = 9;
    engine_sysex(kit, 8);
    off(0);
    pairs_closed();
    g.clear();
    arp::set(0, cfg(arp::kUp));
  }

  // the Rytm's USB gone: no 0x11 will come, a fresh trig ends too
  rep(0x44, 0, 48, 90, words());
  chord(0, {60});
  run(100);
  arp::link_lost();
  CHECK(!arp::busy(0) && midim::voice_notes(0) == 0);
  run(6000);
  CHECK(!arp::busy(0));
  unchord(0, {60});
  pairs_closed();
  g.clear();

  // a code past the table reads OFF everywhere: not "on" with a silent voice
  send(msg(arp::kCmd, {0, 0x7F, 7}));
  CHECK(!arp::on(0));
  chord(0, {60});
  run(100);
  same(ons(), {60});
  unchord(0, {60});
  pairs_closed();
  g.clear();
  arp::set(0, cfg(arp::kUp));

  // off the grid, a slow SPD, a short LEN: released before its first point: that note, its gate, not silence. Free too: a hold that long plays at once
  {
    arp::set(0, cfg(arp::kUp, 0));
    play();
    int64_t x = c.t0 + 24000 * ((((int64_t)seq::frames() - c.t0) / 24000) + 1) + 9000;
    run(x - (int64_t)seq::frames());
    rep(0x44, 0, 55, 90, words());
    run(1200);
    same(ons(), {});
    off(0);
    run(24);
    same(ons(), {55});
    run(24000);
    same(ons(), {55});
    CHECK(!arp::busy(0));
    pairs_closed();
    g.clear();
    arp::set(0, cfg(arp::kUp));
  }

  // a pattern switch: the clock starts over at step 0; the point played `latency` early is not played again
  {
    chord(0, {60, 64, 67});
    run(6000 * 4);
    int64_t tf = c.t0 + (((int64_t)seq::frames() - c.t0) / 6000 + 1) * 6000;
    run(tf + c.delay - 96 - (int64_t)seq::frames());   // its early note went, its tick not yet there
    auto b = blk4(tf);
    std::vector<uint8_t> sw = {seq::kSwitch, 0, 1};
    sw.insert(sw.end(), b.begin(), b.end());
    c.t0 = tf, c.k = 0;
    g_late.clear(), g_late_at.clear();
    state(sw);
    run(6000 * 4);
    unchord(0, {60, 64, 67});
    int64_t prev = -1;
    for (auto& e : g) {
      if ((e.status & 0xF0) != 0x90) continue;
      if (prev >= 0) CHECK(e.h - prev >= 5000);
      prev = e.h;
    }
    run(6000);
    pairs_closed();
    g.clear();
    halt();
  }

  // free running, a tempo change: the next note an interval of the new tempo after the last, no gap
  {
    tempo(120);
    chord(0, {60});
    run(6000 * 2 + 200);
    tempo(60);
    run(12000 * 2 + 200);
    tempo(240);
    int64_t mark = (int64_t)seq::frames();
    size_t n0 = ons().size();
    run(12000 + 200);
    CHECK(ons().size() >= n0 + 3);
    (void)mark;
    unchord(0, {60});
    run(6000);
    pairs_closed();
    g.clear();
    tempo(120);
  }

  // the track that matters on a shared voice; an analog track never
  CHECK(ui::remote_track_voice(2) == ui::remote_track_voice(3));
  arp::set(3, cfg(arp::kDown));
  CHECK(arp::on(ui::remote_track_voice(3)));
  machine33(3, 2, 0x7F);
  CHECK(!arp::on(ui::remote_track_voice(3)));
  arp::set(3, cfg(arp::kOff));
  send(a33);

  // poly and mono Sidecar machines: 6-OP FM's chord from one trig, its CHD; one note sounding at a time; PLAITS through its own voice
  const int sd = ui::remote_track_voice(1);
  arp::set(1, cfg(arp::kUp));
  {
    std::vector<int> p(16, 0);
    p[12] = 1;   // CHD MAJ
    send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 1, 1, 40, (uint8_t)(0x48 | sd), 0xF7});
    rep(0x04, 0x48 | sd, 60, 100, p);
    CHECK(v[sd].fm && arp::busy(sd));
    run(100);
    CHECK(poly::held_notes(sd) == 1);
    run(6000 * 3);
    CHECK(arp::stats().notes > 0);
    CHECK(poly::held_notes(sd) <= 1);
    off(sd);
    run(6000 * 2);
    CHECK(!arp::busy(sd) && poly::held_notes(sd) == 0);
    p[12] = 0;
    send({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 1, 1, 44, (uint8_t)sd, 0xF7});
    rep(0x04, sd, 60, 100, p);   // PLAITS
    CHECK(!v[sd].fm && arp::busy(sd));
    run(100);
    CHECK(inst::stat(sd).path == 2);   // mono: its own machine
    off(sd);
    run(6000 * 2);
    CHECK(!arp::busy(sd) && !inst::seq_mono(sd));
  }
  // keys on that mono machine reach the arp only while it is on (the Rytm plays them otherwise: poly::plays_poly)
  CHECK(poly::plays_poly(sd));
  arp::set(1, cfg(arp::kOff));
  CHECK(!poly::plays_poly(sd));

  // 2E: the track pinned (not the MIDI machine's) and BD's ticks asked for
  {
    run(48000 / 3);
    arp::set(1, cfg(arp::kUp));
    g_rytm.clear();
    run(48000 / 3);
    bool seen = false;
    for (auto& m : g_rytm)
      if (m.size() >= 14 && m[4] == seq::kMaskCmd) seen = (m[9] & 1) && (m[11] & 2) && !(m[11] & 1);
    CHECK(seen);
    arp::set(1, cfg(arp::kOff));
    g_rytm.clear();
    run(48000 / 3);
    for (auto& m : g_rytm)
      if (m[4] == seq::kMaskCmd) CHECK(m.size() <= 12 || (m[9] & 1));
  }

  // the edit goes to the Rytm as 45 <t> <lo> <hi>
  g_rytm.clear();
  arp::set(4, cfg(arp::kPlayed, 0, 2, 2, 2));
  {
    int code = arp::encode(cfg(arp::kPlayed, 0, 2, 2, 2));
    CHECK(g_rytm.size() == 1 && g_rytm[0] == (std::vector<uint8_t>{0xF0, 0x7D, 0x52, 0x54, arp::kCmd, 1, 4, (uint8_t)(code & 0x7F), (uint8_t)(code >> 7), 0xF7}));
    send(msg(arp::kCmd, {4, 0, 0}));
    CHECK(!arp::on(ui::remote_track_voice(4)));
  }

  // the page's knobs
  {
    const ui::ExtraPage* pg = nullptr;
    for (int k = 0; k < ui::extra_pages(); ++k)
      if (!strcmp(ui::extra_page(k).title, "ARP")) pg = &ui::extra_page(k);
    CHECK(pg && pg->shown() && pg->off_ring);
    {
      static ui::Canvas cv;
      pg->draw(cv, 0, 1, 0);   // (its text: the Rytm's font, slots)
    }
    arp::set(0, cfg(arp::kOff));
    pg->enc(0, 1, false), pg->enc(1, 1, false), pg->enc(2, 1, false), pg->enc(3, -1, false), pg->enc(4, 1, false);
    arp::Cfg x = arp::get(0);
    CHECK(x.mode == arp::kUp && x.speed == 4 && x.range == 1 && x.gate == 0 && x.swing == 1);
    arp::set(0, cfg(arp::kOff));
  }

  // the console
  {
    char b[200];
    CHECK(arp::command("arp=3:mode=1,spd=5,rng=2,gate=2,swng=1", b, sizeof b) && arp::get(2).mode == arp::kUp && arp::get(2).speed == 5);
    CHECK(arp::command("arp=3:mode=0", b, sizeof b) && arp::command("arp", b, sizeof b) && !arp::command("arpx", b, sizeof b));
  }

  // cost: a block with all eight voices running, each a 16-note hold, a note every block's worth at worst
  {
    for (int t : {0, 1, 2, 4, 5, 6, 8, 10}) arp::set(t, cfg(arp::kRandom, 5, 2));
    for (int vi = 0; vi < 8; ++vi)
      for (int n = 0; n < arp::kHeld; ++n) arp::live_on(vi, 40 + n, 100, 1, 40 + n);
    host_fake_time(false, 0);   // the real clock: stats().worst_us, as "arp" on the Pi's console
    double worst = 0;
    int64_t h = (int64_t)seq::frames();
    for (int i = 0; i < 40000; ++i, h += kBlock) {
      auto a = std::chrono::steady_clock::now();
      arp::block(h);
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
      if (us > worst) worst = us;
    }
    printf("arp: 8 voices, 16 notes each: a block's worst %.1f us here, stats %lu us\n", worst, (unsigned long)arp::stats().worst_us);
    CHECK(getenv("CI") || worst < 2000);   // (wall clock: a shared CI runner gets descheduled)
    for (int vi = 0; vi < 8; ++vi) arp::end(vi);
  }
  // a sound tagged for another plugin than the slot's, arp on: the trig goes on to um's guard (the arp's own notes have no tag), and an untagged one is the arp's
  {
    static int rn, rt, calls;
    auto was = hooks.user_report;
    hooks.user_report = [](int, int, int, int n, int, int t) { rn = n, rt = t, ++calls; };
    send(msg(0x33, {0, 1, 35, 0x50}));
    arp::Cfg up = arp::decode(0);
    up.mode = arp::kUp;
    arp::set(0, up);
    CHECK(arp::on(0));
    auto trig = [&](int tag) {
      std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x04, 0x50, 60, 100};
      for (int k = 0; k < 16; ++k) m.push_back(0), m.push_back(0);
      m.push_back((uint8_t)(tag >> 7)), m.push_back((uint8_t)(tag & 0x7F)), m.push_back(0xF7);
      CHECK(m.size() == 43);
      send(m);
    };
    trig(0x123);
    CHECK(calls == 1 && rn == 60 && rt == 0x123);                  // the guard's: no slot ready here says it is another's, so silent there
    CHECK(!arp::trig_off(0));                                      // nothing held by the arp
    trig(0);
    CHECK(calls >= 2 && rn == -1);                                 // no tag: the arp's trig, the note -1 to the machine
    CHECK(arp::trig_off(0));
    arp::Cfg off = arp::decode(0);
    arp::set(0, off);
    arp::end(0);
    send(msg(0x33, {0, 1, 50, 0}));
    hooks.user_report = was;
  }
  // arp locks (seq.h kLockArp): the sequencer's steps. A chord held over the whole loop, a lock on an empty step (a lock trig) or on the note's own step
  {
    char b[200];
    machine33(0, 50, 0);
    rep(0x43, 0, -1, 0, words());
    CHECK(seq::command("seq=track:1:on", b, sizeof b));
    struct Hit {
      int64_t at;
      int on, note;
    };
    int take_micro = 0;
    std::string take_cmd;
    auto take = [&](arp::Cfg base, std::vector<std::vector<int>> locks, std::vector<int> notes = {60, 64}, int len = 11 * seq::kSub, int step0 = 0) {
      halt();
      run(4800);
      seq::Track* tr = seq::edit_track(0, true);
      CHECK(tr);
      for (int s = 0; s < 16; ++s) tr->step[s] = {};
      for (int n : notes) seq::add_note(tr->step[step0], {(uint8_t)n, 100, (uint16_t)len, (int8_t)take_micro, 0});
      if (!notes.empty()) tr->step[step0].flags |= 1, tr->step[step0].prob = 100;
      for (auto& l : locks) CHECK(seq::set_lock(tr->step[l[0]], seq::kLockArp + l[1], l[2] << 7));
      if (!take_cmd.empty()) CHECK(seq::command(take_cmd.c_str(), b, sizeof b));
      seq::touched();
      arp::set(0, base);
      run(4800);
      g.clear();
      play();
      int64_t o = c.t0 - lat;
      run(6000 * 12 + 3000 - c.delay - 960);
      if (len < 12 * seq::kSub) CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));
      halt();
      run(12000);
      std::vector<Hit> h;
      for (auto& e : g) h.push_back({e.h - o, (e.status & 0xF0) == 0x90, e.note});
      pairs_closed();                                                 // nothing hangs, on the machine either
      CHECK(midim::voice_notes(0) == 0 && !arp::busy(0) && !arp::locked(0));
      g.clear();
      return h;
    };
    auto note_at = [](const std::vector<Hit>& h, int64_t from, int64_t to) {
      std::vector<int> r;
      for (auto& x : h)
        if (x.on && x.at >= from && x.at < to) r.push_back(x.note);
      return r;
    };
    auto same_but = [](const std::vector<Hit>& a, const std::vector<Hit>& k, int64_t from, int64_t to) {   // equal outside [from, to), to a Rytm block
      std::vector<Hit> x, y;
      for (auto& e : a)
        if (e.at < from || e.at >= to) x.push_back(e);
      for (auto& e : k)
        if (e.at < from || e.at >= to) y.push_back(e);
      CHECK(x.size() == y.size());
      for (size_t i = 0; i < x.size() && i < y.size(); ++i) CHECK(x[i].on == y[i].on && x[i].note == y[i].note && llabs(x[i].at - y[i].at) <= 32);
    };
    const int64_t S = 6000;
    auto base = take(cfg(arp::kUp), {});   // UP over {60, 64}: a note a step, 50 %
    same(note_at(base, 0, 6 * S - 100), {60, 64, 60, 64, 60, 64});
    // a lock on one step changes that step only: no lock anywhere is what the arp did before, each of the five
    auto dn = take(cfg(arp::kUp), {{2, 0, arp::kDown}});                    // MODE on step 3: DOWN's note at the same place of the pattern
    same(note_at(dn, 2 * S - 100, 3 * S - 100), {64});
    same_but(base, dn, 2 * S - 100, 2 * S + 3100);
    auto rg = take(cfg(arp::kUp), {{2, 2, 1}});                             // RNG 2 octaves: position 2 is the octave up
    same(note_at(rg, 2 * S - 100, 3 * S - 100), {72});
    same_but(base, rg, 2 * S - 100, 2 * S + 3100);
    auto sp = take(cfg(arp::kUp), {{2, 1, 5}});                             // SPD 1/32: two notes in that step, on its grid, and the next on the Rytm's step
    same(note_at(sp, 2 * S - 100, 3 * S - 100), {60, 64});
    {
      std::vector<int64_t> at;
      for (auto& x : sp)
        if (x.on && x.at >= 2 * S - 100 && x.at < 4 * S - 100) at.push_back(x.at);
      CHECK(at.size() == 3 && llabs(at[0] - 2 * S) <= 24 && llabs(at[1] - (2 * S + S / 2)) <= 24 && llabs(at[2] - 3 * S) <= 24);
    }
    same(note_at(sp, 3 * S - 100, 5 * S - 100), {60, 64});                  // and the pattern goes on from where it is
    auto gt = take(cfg(arp::kUp), {{2, 3, 2}});                             // GATE 100: that note long, as many, the others 50
    auto off_of = [](const std::vector<Hit>& h, int64_t from) {
      for (auto& x : h)
        if (!x.on && x.at > from) return x.at - from;
      return (int64_t)-1;
    };
    CHECK(llabs(off_of(base, 2 * S) - S / 2) <= 96 && off_of(gt, 2 * S) >= S - 96);
    CHECK(llabs(off_of(gt, 3 * S + 100) - (S / 2 - 100)) <= 96);
    same(note_at(gt, 0, 6 * S - 100), {60, 64, 60, 64, 60, 64});
    auto sw = take(cfg(arp::kUp), {{3, 4, 2}});                             // SWNG 66 on the odd point of step 4: late by 0.32 of it, then straight again
    {
      std::vector<int64_t> at;
      for (auto& x : sw)
        if (x.on && x.at >= 2 * S - 100 && x.at < 5 * S - 100) at.push_back(x.at);
      CHECK(at.size() == 3 && llabs(at[0] - 2 * S) <= 24 && llabs(at[1] - (3 * S + S * 32 / 100)) <= 24 && llabs(at[2] - 4 * S) <= 24);
    }
    same_but(base, sw, 3 * S - 100, 4 * S - 100);
    // on a swung base a lock of SPD lands on its grid: the sound's SWNG 66 stays in force over the lock
    auto bs = take(cfg(arp::kUp, 3, 0, 1, 2), {{5, 1, 3}});
    auto bl = take(cfg(arp::kUp, 3, 0, 1, 2), {{5, 2, 1}});
    same_but(bs, bl, 5 * S - 100, 6 * S + 2000);
    // MODE OFF: the step rests, the hold goes on after it; the arp's other notes where they were
    auto rs = take(cfg(arp::kUp), {{2, 0, arp::kOff}});
    same(note_at(rs, 2 * S - 100, 3 * S - 100), {});
    same(note_at(rs, 0, 2 * S - 100), {60, 64});
    same(note_at(rs, 3 * S - 100, 5 * S - 100), {60, 64});
    // MODE OFF on a step with its own note, the same pitch held from before: that note plays plain, its off reaches the machine though the arp holds the pitch
    {
      halt();
      run(4800);
      seq::Track* tr = seq::edit_track(0, true);
      for (int s = 0; s < 16; ++s) tr->step[s] = {};
      seq::add_note(tr->step[0], {60, 100, seq::kSub + 60, 0, 0});
      seq::add_note(tr->step[1], {60, 100, 80, 0, 0});
      tr->step[0].flags |= 1, tr->step[1].flags |= 1, tr->step[0].prob = tr->step[1].prob = 100;
      seq::set_lock(tr->step[1], seq::kLockArp, arp::kOff);
      seq::touched();
      arp::set(0, cfg(arp::kUp));
      run(4800);
      g.clear();
      play();
      run(6000 * 6);
      CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));   // (before STOP: the MIDI machine's own stop would close it)
      halt();
      run(12000);
      pairs_closed();
      g.clear();
    }
    // the sound's OFF: MODE UP on one step arps that step and no other; a note past the step ends with it, no stuck note. Its SPD the sound's
    auto of = take(cfg(arp::kOff), {});
    same(note_at(of, 0, 2 * S), {60, 64});   // (plain: the chord)
    auto on1 = take(cfg(arp::kOff, 5), {{0, 0, arp::kUp}});
    same(note_at(on1, 0, S - 200), {60, 64});
    same(note_at(on1, S - 200, 12 * S), {});
    // a swung track: the arp's grid is straight, a step's lock holds from the step's place on it, odd steps as even
    {
      auto swing = [&](int v) { seq::edit_track(0, true)->swing = (uint8_t)v, seq::touched(); };
      swing(58);
      auto b58 = take(cfg(arp::kUp), {});
      auto r3 = take(cfg(arp::kUp), {{3, 2, 1}});
      same(note_at(r3, 3 * S - 100, 4 * S - 100), {76});
      same_but(b58, r3, 3 * S - 100, 3 * S + 3100);
      auto r2 = take(cfg(arp::kUp), {{2, 2, 1}});
      same(note_at(r2, 2 * S - 100, 3 * S - 100), {72});
      same_but(b58, r2, 2 * S - 100, 2 * S + 3100);
      auto o3 = take(cfg(arp::kUp), {{3, 0, arp::kOff}});
      same(note_at(o3, 3 * S - 100, 4 * S - 100), {});
      same(note_at(o3, 4 * S - 100, 5 * S - 100), {64});
      // the sound's OFF, MODE locked on: no grid point left in the step: its first note at the note's own place, not lost, ended with the step
      auto first = [&](arp::Cfg sound, int64_t at) {
        auto h = take(sound, {{1, 0, arp::kUp}}, {60, 64}, 11 * seq::kSub, 1);
        same(note_at(h, S - 100, 2 * S - 100), {60});
        same(note_at(h, 2 * S - 100, 12 * S), {});
        for (auto& x : h)
          if (x.on) CHECK(llabs(x.at - at) <= 96);
      };
      first(cfg(arp::kOff), S + S * 16 / 100);   // swing 58, an odd step
      swing(50);
      first(cfg(arp::kOff, 1), S);               // SPD 1/8 and 1/4: their point is not on this step
      first(cfg(arp::kOff, 0), S);
      take_micro = 24;
      first(cfg(arp::kOff), S + S / 4);          // a note 1/4 late
      take_micro = 0;
      // DLY: the step's place on the grid is its window: the next step's point stays the sound's
      take_cmd = "seq=fx:1:3:1:DLY:48";
      auto d2 = take(cfg(arp::kUp), {{2, 2, 1}});
      same(note_at(d2, 2 * S - 100, 3 * S - 100), {72});
      same(note_at(d2, 3 * S - 100, 4 * S - 100), {64});
      take_cmd = "seq=fx:1:3:1:off";
      take(cfg(arp::kUp), {});
      take_cmd = "";
    }
    // a short plain note under MODE OFF, the pitch of a long one the arp holds: it ends at its own length
    {
      halt();
      run(4800);
      seq::Track* tr = seq::edit_track(0, true);
      for (int s = 0; s < 16; ++s) tr->step[s] = {};
      seq::add_note(tr->step[0], {60, 100, 8 * seq::kSub, 0, 0});
      seq::add_note(tr->step[2], {60, 100, 24, 0, 0});
      tr->step[0].flags |= 1, tr->step[2].flags |= 1, tr->step[0].prob = tr->step[2].prob = 100;
      seq::set_lock(tr->step[2], seq::kLockArp, arp::kOff);
      seq::touched();
      arp::set(0, cfg(arp::kUp));
      run(4800);
      g.clear();
      play();
      int64_t o = c.t0 - lat;
      run(6000 * 10);
      CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));
      bool ended = false;
      for (auto& e : g) ended = ended || ((e.status & 0xF0) != 0x90 && llabs(e.h - o - (2 * S + S / 4)) <= 96);
      CHECK(ended);
      halt();
      run(12000);
      pairs_closed();
      g.clear();
    }
    // a lock with no MODE on an arp that is off: nothing
    auto nm = take(cfg(arp::kOff), {{0, 1, 5}, {0, 2, 2}});
    same(note_at(nm, 0, 2 * S), {60, 64});
    // the same pitches, plain, then taken by a lock on the next step, the arp too slow to play them all: the plain note gives way, none left open
    {
      halt();
      run(4800);
      seq::Track* tr = seq::edit_track(0, true);
      for (int s = 0; s < 16; ++s) tr->step[s] = {};
      for (int n : {60, 64}) {
        seq::add_note(tr->step[0], {(uint8_t)n, 100, seq::kSub + 20, 0, 0});
        seq::add_note(tr->step[1], {(uint8_t)n, 100, seq::kSub - 10, 0, 0});
      }
      tr->step[0].flags |= 1, tr->step[1].flags |= 1, tr->step[0].prob = tr->step[1].prob = 100;
      seq::set_lock(tr->step[1], seq::kLockArp, arp::kUp << 7);
      seq::set_lock(tr->step[1], seq::kLockArp + 1, 0);   // 1/4
      seq::touched();
      arp::set(0, cfg(arp::kOff));
      run(4800);
      g.clear();
      play();
      run(6000 * 8);
      CHECK(midim::voice_notes(0) == 0 && !arp::busy(0));
      halt();
      run(12000);
      pairs_closed();
      g.clear();
    }
    // RNG and MODE moving with a note sounding at gate 100, every step locked some other way: every off finds its note
    {
      std::vector<std::vector<int>> l;
      for (int s = 0; s < 12; ++s) l.push_back({s, s & 1 ? 2 : 0, s & 1 ? (s >> 1) % 3 : 1 + (s >> 1) % 5});
      take(cfg(arp::kUp, 3, 2, 2), l);
    }
    // a condition that fails leaves the lock out: FILL, none held
    {
      halt();
      run(4800);
      seq::Track* tr = seq::edit_track(0, true);
      for (int s = 0; s < 16; ++s) tr->step[s] = {};
      seq::add_note(tr->step[0], {60, 100, 11 * seq::kSub, 0, 0});
      seq::add_note(tr->step[0], {64, 100, 11 * seq::kSub, 0, 0});
      tr->step[0].flags |= 1, tr->step[0].prob = 100;
      seq::set_lock(tr->step[2], seq::kLockArp + 2, 1 << 7);
      tr->step[2].lock[0].cond = seq::kCondFill;
      seq::touched();
      arp::set(0, cfg(arp::kUp));
      run(4800);
      g.clear();
      play();
      run(6000 * 5);
      same(ons(), {60, 64, 60, 64, 60, 64});
      halt();
      run(12000);
      g.clear();
    }
    // stopped, a key held: a lock of MODE OFF rests it for its time, no catching up after, and STOP or the end of its frame leaves nothing behind
    {
      halt();
      run(4800);
      for (int s = 0; s < 16; ++s) seq::edit_track(0, true)->step[s] = {};
      seq::touched();
      arp::set(0, cfg(arp::kUp));
      chord(0, {60});
      run(4000);
      g.clear();
      uint8_t val[seq::kArpLocks] = {arp::kOff, 0, 0, 0, 0};
      arp::lock(0, 0, 1, val, (int64_t)seq::frames() + 6000 * 2);
      CHECK(arp::locked(0) == 1);
      run(6000 * 2 - 100);
      CHECK(ons().empty());
      run(6000 * 2 + 300);
      CHECK(ons().size() == 3 && !arp::locked(0));   // at its end and a step each after
      arp::lock(0, 0, 1, val, (int64_t)seq::frames() + 6000);
      arp::lock(0, 0, 0, val, 0);
      CHECK(!arp::locked(0));
      unchord(0, {60});
      run(6000);
      pairs_closed();
      g.clear();
    }
    // a step's preview, stopped (hold a trig + YES): its arp locks for its step's time, then the sound's again
    {
      halt();
      run(4800);
      seq::Track* tr = seq::edit_track(0, true);
      for (int s = 0; s < 16; ++s) tr->step[s] = {};
      seq::add_note(tr->step[0], {60, 100, 48, 0, 0});
      tr->step[0].flags |= 1, tr->step[0].prob = 100;
      seq::set_lock(tr->step[0], seq::kLockArp, arp::kUp << 7);
      seq::touched();
      arp::set(0, cfg(arp::kOff));
      seq::view_set(0, 0, -1, false);
      CHECK(seq::ui_key(0, true, true));
      run(960);
      CHECK(seq::ui_key(0x44, true, true));
      seq::ui_key(0x44, false, true);
      run(960);
      CHECK(arp::locked(0) == 1);
      seq::ui_key(0, false, true);
      run(12000);
      CHECK(!arp::locked(0) && !arp::busy(0) && midim::voice_notes(0) == 0);
      pairs_closed();
      g.clear();
    }
    // the cost: 8 voices, 16 notes each at 1/32, every voice locked again at every pass
    {
      arp::Cfg cf = cfg(arp::kUp, 5, 2, 0);
      for (int t : {0, 1, 2, 4, 5, 6, 8, 10}) arp::set(t, cf);
      for (int vi = 0; vi < 8; ++vi)
        for (int k = 0; k < arp::kHeld; ++k) arp::live_on(vi, 30 + k, 100, 1, k);
      uint8_t val[seq::kArpLocks] = {arp::kRandom, 5, 2, 0, 2};
      double worst = 0;
      for (int i = 0; i < 2000; ++i) {
        auto a = std::chrono::steady_clock::now();
        for (int vi = 0; vi < 8; ++vi) arp::lock(vi, vi, 31, val, (int64_t)seq::frames() + 6000);
        seq::pre_block();
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
        if (us > worst) worst = us;
      }
      printf("arp locks: 8 voices locked at every block, 16 notes each: a block's worst %.1f us here\n", worst);
      CHECK(getenv("CI") || worst < 2000);   // (wall clock: a shared CI runner gets descheduled)
      for (int vi = 0; vi < 8; ++vi) arp::lock(vi, vi, 0, val, 0), arp::end(vi);
      for (int t : {0, 1, 2, 4, 5, 6, 8, 10}) arp::set(t, cfg(arp::kOff));
    }
    arp::set(0, cfg(arp::kOff));
    CHECK(seq::command("seq=track:1:off", b, sizeof b));
    run(960);
  }
  printf("test_arp OK\n");
  return 0;
}
