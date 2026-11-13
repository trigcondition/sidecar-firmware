// core/tests/test_rytmboot.cpp - poly tracks across a Pi or Rytm restart (the 0.4.4 user report, item 4,
// four patterns).
//   1. F-b1: a cold boot (the Pi and the Rytm on together): the 33 names CH's JUNO, no 0x03 / 0x04 comes (0021's one
//      boot report went before the Pi listened) - the voice plays JUNO all the same; a later report wins.
//   2. F-b3: the Rytm restarted within ~10 s of its own boot: its new block counter (from 0) is no new time base to
//      the clock follower (a step back of < 15000 blocks reads as "a little older") - a burst of late notes, then the
//      wrong steps. seq::hello() starts the follower afresh - review L1: only when the hello's uptime shows a restart; a
//      re-plug mid-play (the uptime goes on) keeps it, and the playhead with it.
//   3. F-a1: COPY PAT, the Pi restarts (a second engine process on the same card: fresh seq state), PASTE PAT on A06:
//      the poly slots and their on flags arrive (the clipboard kept in the project's clip.sqp).
//   4. F-a2: COPY PAT while the project loads (its store filling) is taken once it is loaded, not refused: a PASTE
//      PAT after the load brings the copied pattern's slots.
//   5. M1 (review): a queued copy whose pattern file does not load leaves the clipboard as it was (A01's), not empty.
//   6. L3 (review): a queued copy dropped because its load was abandoned (another fill began) says so on the console.
//   7. L2 (review): the clipboard is the project's, as its clip.sqp: A01 copied in BOOT, project OTHR (no copy of its
//      own) - a paste there brings nothing.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "core_port.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "hooks.h"
#include "inst.h"
#include "machines.h"
#include "pool/pool.h"
#include "project_key.h"
#include "seq/seq.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static uint64_t g_us = 1000000;
static uint32_t g_ms = 10000;
struct Out {
  int64_t frame;   // seq::frames() when it went
  std::vector<uint8_t> m;
};
static std::vector<Out> g_out;
static void out(const uint8_t* m, int n) { g_out.push_back({(int64_t)seq::frames(), std::vector<uint8_t>(m, m + n)}); }
static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static void from_rytm(std::vector<uint8_t> body, uint8_t cmd) {
  auto m = msg(cmd, body);
  engine_sysex(m.data(), (int)m.size());
}
static void report(int vb) {   // the voice's 03 (its machine), P1..P16 at 64
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)vb};
  for (int k = 0; k < 16; ++k) m.push_back(64), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void tick() {
  g_ms += 60;
  g_us += 60000;
  host_fake_time(true, g_us);
  remote_tick(g_ms);
  seq::tick(now_ms());
}
static void loaded() {
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
static void hello(int up20 = 2) {   // the Rytm's 35: its uptime (1/20 s), caps 1
  from_rytm({(uint8_t)(up20 >> 7 & 0x7F), (uint8_t)(up20 & 0x7F), 1}, 0x35);
}
constexpr int CH = 8;
static void snapshot(int ch_machine) {   // 33 for all 12 tracks: CH on ch_machine, the others Rytm machines
  std::vector<uint8_t> b = {0, 12};
  for (int t = 0; t < 12; ++t)
    b.push_back(t == CH ? 41 : 0), b.push_back(t == CH ? (uint8_t)(ch_machine | remote_track_voice(t)) : 0x7F);
  from_rytm(b, 0x33);
}

static std::string cmd(const char* c) {
  char b[900];
  CHECK(seq::command(c, b, sizeof b));
  return b;
}

// ---- the Rytm side simulated (test_polypat 7's): its ticks stamped with their block, arriving kDelay later; it ticks
// every track the last 2E names once that 2E has reached it. Its frame = the host's - g_roff.
struct Msg {
  int64_t at;
  std::vector<uint8_t> m;
};
static std::vector<Msg> g_wire;
static std::vector<uint8_t> blk4(int64_t frame) {
  uint32_t b = (uint32_t)(frame / 32) & ((1u << 28) - 1);
  return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)};
}
static int g_rytm_mask = 0;
static int64_t g_t0 = 0, g_next = 0, g_roff = 0;
static bool g_rytm_on = true;
static const double kPer = 6000;   // 120 BPM
static const int kDelay = 700;
struct Fired {
  int64_t h;
  int voice, note;
};
static std::vector<Fired> g_fired;
static void run_until(int64_t until) {
  while ((int64_t)seq::frames() < until) {
    int64_t h = (int64_t)seq::frames();
    for (auto& o : g_out)
      if (o.m.size() >= 10 && o.m[4] == 0x2E && o.frame + 48 <= h && o.frame + 48 > h - kBlock) {
        int m = o.m[6] | o.m[7] << 7;
        if (o.m.size() >= 12) m |= o.m[9] | o.m[10] << 7;
        g_rytm_mask = m;
      }
    for (;;) {
      int64_t tf = g_t0 + (int64_t)llround((double)g_next * kPer);
      if (!g_rytm_on || tf + g_roff > h + 2) break;
      std::vector<uint8_t> b = blk4(tf);
      std::vector<uint8_t> tr;
      for (int r = 0; r < 12; ++r)
        if (g_rytm_mask >> r & 1) tr.insert(tr.end(), {(uint8_t)r, (uint8_t)(g_next % 16), 16, 2});
      if (!tr.empty()) {
        b.push_back((uint8_t)(tr.size() / 4));
        b.insert(b.end(), tr.begin(), tr.end());
        g_wire.push_back({tf + g_roff + kDelay, msg(0x2C, b)});
      }
      ++g_next;
    }
    for (size_t i = 0; i < g_wire.size();) {
      if (g_wire[i].at <= h) {
        engine_sysex(g_wire[i].m.data(), (int)g_wire[i].m.size());
        g_wire.erase(g_wire.begin() + (long)i);
      } else {
        ++i;
      }
    }
    int64_t f[512], f2[512];
    uint8_t on[512], vo[512], no[512], on2[512], vo2[512], no2[512];
    int n = seq::pending(f, on, vo, no, 512);
    seq::pre_block();
    int n2 = seq::pending(f2, on2, vo2, no2, 512);
    for (int i = 0; i < n; ++i) {   // the ons that went
      if (!on[i]) continue;
      bool still = false;
      for (int j = 0; j < n2 && !still; ++j) still = on2[j] && f2[j] == f[i] && no2[j] == no[i];
      if (!still) g_fired.push_back({h, vo[i], no[i]});
    }
    g_us += 250;
    host_fake_time(true, g_us);
    seq::tick(now_ms());
  }
}
static void transport(int s) {   // the Rytm's 2D 01 now
  auto b = blk4((int64_t)seq::frames() - g_roff);
  b.insert(b.begin(), (uint8_t)s);
  b.insert(b.begin(), seq::kTransport);
  from_rytm(b, 0x2D);
}
// PLAY for 2 bars from the next 4800 frames: CH's notes (steps 1, 5, 9, 13: 60 64 68 72) each on its step, none late
// (the first step's tick comes with PLAY: that note may be missed, as before the restart - 7 or 8)
static int play_bars(const char* what) {
  const int vi = remote_track_voice(CH);
  g_t0 = (int64_t)seq::frames() - g_roff + 4800, g_next = 0;
  g_rytm_on = true;
  run_until(g_t0 + g_roff - 100);
  transport(seq::kPlay);
  size_t f0 = g_fired.size();
  const int64_t base = g_t0 + g_roff;   // step 0 on the host (the Rytm's frame + its offset)
  run_until(g_t0 + g_roff + (int64_t)(kPer * 32));
  int n = 0, wrong = 0;
  for (size_t i = f0; i < g_fired.size(); ++i) {
    if (g_fired[i].voice != vi) continue;
    double st = (double)(g_fired[i].h - base) / kPer;
    int at = (int)lround(st);
    bool ok = std::fabs(st - at) < 0.25 && at % 4 == 0 && g_fired[i].note == 60 + at % 16;
    printf("    %s: note %d at step %.2f%s\n", what, g_fired[i].note, st, ok ? "" : "  <- wrong");
    ++n, wrong += !ok;
  }
  transport(seq::kStop);
  run_until((int64_t)seq::frames() + 4800);
  printf("  %s: %d notes on CH's voice in 2 bars, %d off their step (want 7-8, 0)\n", what, n, wrong);
  return n >= 7 && n <= 8 && wrong == 0;
}

static void go(int p) {   // the Rytm on pattern p (stopped: a change without a switch), loaded
  from_rytm({seq::kPattern, (uint8_t)p, 0x7F}, 0x2D);
  while (seq::idle()) {}
  tick();
}
static void settle() {   // the debounced saves written (into the store and the card queue)
  for (int i = 0; i < 30; ++i) tick(), seq::idle();
  for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();
}
static void edit(int op, int p) {   // 2D 0A: the Rytm's pattern op on pattern p, done (ok 1), the Rytm's own gesture
  from_rytm({0x0A, (uint8_t)op, (uint8_t)p, 0x7F, 1, 0, 0, 0, 0}, 0x2D);
  tick();
}
static const seq::Track* slot(int r) {   // track r's slot in the pattern shown, if it has one
  for (const seq::Track& t : seq::edit_pattern().t)
    if (t.rytm == r) return &t;
  return nullptr;
}
static const uint8_t kProj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'B', 'O', 'O', 'T', 0xF7};
// 3's second half, in a second process on the first one's card: the Pi restarted, the Rytm's hello, A06, PASTE PAT
static int after_restart(const char* card) {
  host_set_card(card);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  hooks.to_rytm = out;
  engine_sysex(kProj, sizeof kProj);
  loaded();
  hello();
  snapshot(kJuno);
  report(kJuno | remote_track_voice(CH));
  go(5);
  CHECK(!slot(CH) || seq::used_steps(*slot(CH)) == 0);
  edit(seq::kPPaste, 5);
  const seq::Track* t = slot(CH);
  printf("  F-a1 after the restart, A06 pasted: CH slot %s, on %d, steps %d, owned %d (want on 1, 4 steps, owned 1)\n",
         t ? "there" : "none", t ? (int)t->on : -1, t ? seq::used_steps(*t) : -1, (int)seq::owns_track(CH));
  CHECK(t && t->on && seq::used_steps(*t) == 4 && seq::owns_track(CH));
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && !strcmp(argv[1], "restart")) return after_restart(argv[2]);
  char tmpl[] = "/tmp/core_rytmboot_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  hooks.to_rytm = out;
  for (int i = 0; i < 50; ++i) tick();   // the Pi up before the Rytm's USB

  // ---- 1. F-b1: the cold boot. The hello, the resync, the 33 (CH = JUNO), the project: no 0x03 for CH's voice
  {
    hello();
    from_rytm({seq::kTransport, 0, 0, 0, 0, 0}, 0x2D);
    from_rytm({seq::kPattern, 0, 0x7F}, 0x2D);
    const int vi = remote_track_voice(CH);
    CHECK(vi >= 0 && vi < kVoices && !v[vi].juno);
    g_out.clear();
    snapshot(kJuno);
    {   // review: DSP SAMPLE is not hinted (its P9..P16, the sample, are the report's): BD's voice waits for it
      const int bv = remote_track_voice(0);
      from_rytm({0, 1, 41, (uint8_t)(kDspSample | bv)}, 0x33);
      printf("  F-b1: a 33 naming DSP SAMPLE for BD's voice %d: dsps %d (want 0, until its report)\n", bv, (int)v[bv].dsps);
      CHECK(!v[bv].dsps);
      from_rytm({0, 1, 0, 0x7F}, 0x33);   // (BD a Rytm machine again)
    }
    int knobs = 0;   // the hint sends the Rytm nothing: no JUNO knobs (15) over the Rytm's own
    for (auto& o : g_out) knobs += o.m.size() > 5 && o.m[4] == 0x15;
    CHECK(knobs == 0);
    engine_sysex(kProj, sizeof kProj);
    loaded();
    tick();
    printf("  F-b1 cold boot: CH's voice %d juno %d fm %d (want juno 1, no 0x03)\n", vi, (int)v[vi].juno, (int)v[vi].fm);
    CHECK(v[vi].juno && !v[vi].fm);
    seq::PolyCfg cfg = {};
    inst::play_on(vi, 60, 100, cfg);   // a sequencer note: the JUNO path (before: PLAITS, the voice's default)
    inst::play_off(vi, 60);
    printf("  F-b1: a sequencer note's path machine 0x%02x (want 0x%02x JUNO)\n", inst::stat(vi).machine, kJuno);
    CHECK(inst::stat(vi).machine == kJuno);
    snapshot(kJuno);   // again (the next hello): nothing changes
    CHECK(v[vi].juno);
    // a later report wins: the Rytm says 6-OP FM; a 33 naming JUNO after it changes nothing (the report's machine)
    report(kFm | vi);
    CHECK(v[vi].fm && !v[vi].juno);
    snapshot(kJuno);
    CHECK(v[vi].fm && !v[vi].juno);
    printf("  F-b1: a 0x03 after it wins (fm %d), a 33 after the 0x03 changes nothing\n", (int)v[vi].fm);
  }

  // ---- 2. F-b3: CH a poly JUNO track (steps 1 5 9 13); the Rytm up 2 s at its hello, 2 bars; off 3 s, up again 2 s
  // at the next hello (its counter ~6 s back: < 15000 blocks), 2 bars: on time both
  {
    const int vi = remote_track_voice(CH);
    report(kJuno | vi);
    snapshot(kJuno);
    from_rytm({seq::kPattern, 0, 0x7F}, 0x2D);
    tick();
    cmd("seq=track:9:on");
    for (int st = 0; st < 16; st += 4) {
      char c[48];
      snprintf(c, sizeof c, "seq=step:9:%d:%d", st + 1, 60 + st);
      cmd(c);
    }
    tick();
    g_roff = (int64_t)seq::frames() - 48000 * 2;   // the Rytm has been up 2 s by its hello
    g_out.clear();
    hello(40);
    run_until((int64_t)seq::frames() + 2400);
    printf("  F-b3: mask 0x%03x, Rytm mask 0x%03x\n", seq::owns_track(CH) ? 1u << CH : 0u, g_rytm_mask);
    CHECK(seq::owns_track(CH) && (g_rytm_mask >> CH & 1));
    CHECK(play_bars("F-b3 first boot"));
    {   // L1: a re-plug mid-play, the Rytm not restarted (its uptime goes on): the follower and the playhead kept
      g_t0 = (int64_t)seq::frames() - g_roff + 4800, g_next = 0;
      run_until(g_t0 + g_roff - 100);
      transport(seq::kPlay);
      run_until(g_t0 + g_roff + (int64_t)(kPer * 8.5));
      double pp0 = seq::play_pos(CH);
      hello((int)(((int64_t)seq::frames() - g_roff) / 2400));
      double pp1 = seq::play_pos(CH);
      printf("  L1: a re-plug hello mid-play: playhead %.2f -> %.2f (want kept, not -1)\n", pp0, pp1);
      CHECK(pp0 >= 0 && pp1 >= 0);
      run_until((int64_t)seq::frames() + (int64_t)(kPer * 4));
      transport(seq::kStop);
      run_until((int64_t)seq::frames() + 4800);
    }
    g_rytm_on = false;   // the Rytm off 3 s (its counter from 0), up 2 s at its hello: the resync, the 33
    run_until((int64_t)seq::frames() + 48000 * 3);
    g_us += 5000000, host_fake_time(true, g_us);   // (the Pi's clock meanwhile: run_until's runs slower than the frames)
    g_roff = (int64_t)seq::frames() - 48000 * 2;
    g_out.clear();
    hello(40);
    run_until((int64_t)seq::frames() + 2400);
    transport(seq::kStop);
    from_rytm({seq::kPattern, 0, 0x7F}, 0x2D);
    snapshot(kJuno);
    run_until((int64_t)seq::frames() + 4800);
    CHECK(play_bars("F-b3 quick restart"));
  }

  // ---- 3. F-a1: A01 (CH on, 4 steps) copied, the saves settled; the Pi restarts before the paste
  {
    go(0);
    CHECK(slot(CH) && slot(CH)->on && seq::used_steps(*slot(CH)) == 4);
    edit(seq::kPCopy, 0);
    settle();
    std::string c = std::string("'") + argv[0] + "' restart '" + card + "'";
    int rc = system(c.c_str());
    printf("  F-a1: the paste after the restart: exit %d (want 0)\n", rc);
    CHECK(rc == 0);
  }

  // ---- 4. F-a2: the clipboard an empty A04's; the project loaded again (its store filling): A01 copied meanwhile, then
  // A07 pasted once it is loaded
  {
    go(3);
    edit(seq::kPCopy, 3);
    project_fill_begin();
    CHECK(project_loading());
    go(0);
    edit(seq::kPCopy, 0);
    CHECK(project_loading());
    loaded();
    tick();
    go(6);
    CHECK(!slot(CH) || seq::used_steps(*slot(CH)) == 0);
    edit(seq::kPPaste, 6);
    const seq::Track* t = slot(CH);
    printf("  F-a2 copied while loading, A07 pasted: CH slot %s, on %d, steps %d (want on 1, 4 steps)\n",
           t ? "there" : "none", t ? (int)t->on : -1, t ? seq::used_steps(*t) : -1);
    CHECK(t && t->on && seq::used_steps(*t) == 4 && seq::owns_track(CH));
  }

  // ---- 5. M1: A01 copied (CH on, 4 steps); A05's file damaged on the card; the project loaded again, A05 copied while
  // it loads (queued), its file does not load at the load's end: the clipboard still A01's, and a paste on A08 is A01's
  {
    go(0);
    edit(seq::kPCopy, 0);
    settle();
    CHECK(seq::view().clip_pattern);
    char pk[64];
    project_key("BOOT", pk, sizeof pk);
    FILE* f = fopen((card + "/.rytm/seq/" + pk + "/p004.sqp").c_str(), "wb");
    CHECK(f);
    fputs("SQP1 not a pattern, damaged", f);
    fclose(f);
    project_fill_begin();
    go(4);
    edit(seq::kPCopy, 4);
    loaded();
    tick();
    printf("  M1: the queued copy of A05 (damaged) taken: clipboard %d (want 1: A01's kept)\n", (int)seq::view().clip_pattern);
    CHECK(seq::view().clip_pattern);
    go(7);
    edit(seq::kPPaste, 7);
    const seq::Track* t = slot(CH);
    printf("  M1: A08 pasted: CH slot %s, steps %d (want 4, A01's)\n", t ? "there" : "none", t ? seq::used_steps(*t) : -1);
    CHECK(t && t->on && seq::used_steps(*t) == 4);
  }

  // ---- 6. L3: A01 copied while the project loads, then the fill begun again before it ends: the drop logged
  {
    static int dropped = 0;
    host_set_log_sink([](const char* s, int) { dropped += strstr(s, "copy") && strstr(s, "dropped") ? 1 : 0; });
    project_fill_begin();
    go(0);
    edit(seq::kPCopy, 0);
    project_fill_begin();   // (the load abandoned: another begins)
    loaded();
    tick();
    host_set_log_sink(nullptr);
    printf("  L3: a queued copy dropped with its load: logged %d (want 1)\n", dropped);
    CHECK(dropped == 1);
  }

  // ---- 7. L2: A01 copied in BOOT; project OTHR (its folder there, no clip.sqp): no clipboard, a paste brings nothing
  {
    go(0);
    edit(seq::kPCopy, 0);
    settle();
    CHECK(seq::view().clip_pattern);
    char pk[64];
    project_key("OTHR", pk, sizeof pk);
    CHECK(system(("mkdir -p '" + card + "/.rytm/seq/" + pk + "'").c_str()) == 0);
    FILE* f = fopen((card + "/.rytm/seq/" + pk + "/tracks.txt").c_str(), "w");
    CHECK(f);
    fputs("mask=0x000\n", f);
    fclose(f);
    const uint8_t other[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 'R', 0xF7};
    engine_sysex(other, sizeof other);
    for (int i = 0; i < 50 && !project_loading(); ++i) tick(), idle_save();
    loaded();
    tick();
    go(0);
    printf("  L2: project OTHR: clipboard %d (want 0)\n", (int)seq::view().clip_pattern);
    CHECK(!seq::view().clip_pattern && (!slot(CH) || seq::used_steps(*slot(CH)) == 0));
    edit(seq::kPPaste, 0);
    const seq::Track* t = slot(CH);
    printf("  L2: OTHR A01 pasted: CH steps %d (want 0)\n", t ? seq::used_steps(*t) : 0);
    CHECK(!t || seq::used_steps(*t) == 0);
  }
  printf("test_rytmboot: OK\n");
  return 0;
}
