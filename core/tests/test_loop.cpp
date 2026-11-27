// core/tests/test_loop.cpp - the master looper (core/loop, spec-050 section 3): the MAIN ring and its frame map (Task 19),
// the grabs (20), playback (21), resample / stop / auto-mute / LIVE VIA (22), calibration and console (25).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core.h"
#include "hooks.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/target.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "song/song.h"
#include "loop/loop.h"
#include "loop/loop_ring.h"

using namespace core;

// ---- Task 19: the ring
static const uint32_t kCap = 1u << 16;   // frames of a test ring (the Pi's is 4,194,304)

// a source of audio: frame k's L = k + 1 (never 0, so silence shows), R = -(k + 1)
static void blocks(loop::Ring& r, uint64_t from, uint64_t to, int jit, uint64_t arrive0, uint64_t lost_at = ~0ull, unsigned lost = 0) {
  uint64_t k = from;
  uint32_t rng = 7;
  while (k < to) {
    unsigned l = k == lost_at ? lost : 0;   // `lost` frames of silence come before this block
    k += l;
    int32_t b[14];
    for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(k + i + 1), b[2 * i + 1] = -(int32_t)(k + i + 1);
    rng = rng * 1103515245u + 12345u;
    uint64_t arr = arrive0 + k + 6 + (jit ? (rng >> 8) % (uint32_t)(jit + 1) : 0);   // the block's last frame arrives (+ jitter)
    r.write(b, 7, arr, l);
    k += 7;
  }
}

static void test_ring_gap_cost() {
  // H1: a stream that comes back after hours writes no silence: a pause longer than the ring touches no memory but the block
  loop::Ring r;
  std::vector<int32_t> mem(2 * (size_t)kCap, 0x55555555);
  r.init(mem.data(), kCap);
  int32_t b[14] = {};
  for (int i = 0; i < 7; ++i) b[2 * i] = 1000 + i, b[2 * i + 1] = 1000 + i;
  r.write(b, 7, 6 + 500, 0);
  uint64_t w0 = r.written();
  r.write(b, 7, 6 + 500 + 7 + 10 * 3600 * 48000ull, 0);   // ten hours later
  size_t touched = 0;
  for (int32_t x : mem)
    if (x != 0x55555555) ++touched;
  CHECK(touched <= 2 * 14 + 2);   // the two blocks, nothing else (the old code wrote the whole ring)
  CHECK(r.written() > w0 + 7 + (uint64_t)kCap);
  std::vector<int32_t> c(2 * 70);
  uint64_t end = r.written();
  CHECK(r.copy_idx(end - 70, 70, c.data()) == 70 && c[0] == 0 && c[2 * 62] == 0 && c[2 * 63] == 1000);
  printf("  ring: a pause of ten hours costs no stores\n");
}

static void test_ring() {
  static int32_t mem[2 * kCap];
  loop::Ring r;
  r.init(mem, kCap);
  // wrap_keeps_last_87s (scaled): write 1.5 rings' worth; a frame older than the ring is gone, a recent one is there
  blocks(r, 0, kCap + kCap / 2, 0, 1000);
  uint32_t idx;
  uint64_t w = r.written();
  CHECK(w >= kCap + kCap / 2);
  CHECK(!r.frame_at(1000 + 100, &idx));                 // host frame of ring frame ~100: overwritten
  CHECK(r.frame_at(1000 + w - kCap / 2, &idx));          // half a ring ago: there
  CHECK(mem[2 * idx] == (int32_t)(w - kCap / 2 + 1));
  // map_ignores_jitter: arrivals jittered +0..12 frames: frame_at exact to a frame
  loop::Ring j;
  static int32_t mem2[2 * kCap];
  j.init(mem2, kCap);
  blocks(j, 0, 20000, 12, 5000);
  for (uint64_t f : {6000ull, 9999ull, 15000ull}) {
    CHECK(j.frame_at(5000 + f, &idx));
    CHECK(mem2[2 * idx] == (int32_t)(f + 1));
  }
  // gap_written_as_silence: 70 lost frames are zeros at those indices, the map stays linear, gap_in says so
  loop::Ring g;
  static int32_t mem3[2 * kCap];
  g.init(mem3, kCap);
  blocks(g, 0, 1400, 0, 100, 693, 70);   // 70 frames lost before the block at 693 + 70 = 763
  CHECK(g.written() == 1400 + 70 - 70 + 0 || g.written() >= 1400);
  {   // zeros at 693..762 for every reader (not written: stale memory sits there), linear again after, the same map
    std::vector<int32_t> c(2 * 80);
    CHECK(g.copy_idx(690, 80, c.data()) == 80);
    CHECK(c[2 * 2] == 693 && c[2 * 3] == 0 && c[2 * 72] == 0 && c[2 * 73] == 764);
    CHECK(g.copy_idx(763, 10, c.data()) == 10 && c[0] == 764);
    int32_t l, r;
    g.frame(700, &l, &r);
    CHECK(l == 0 && r == 0);
    g.frame(1000, &l, &r);
    CHECK(l == 1001);
  }
  CHECK(g.gap_in(100 + 650, 200) && !g.gap_in(100 + 100, 100) && !g.gap_in(100 + 900, 200));
  // stream_restart_keeps_map_linear: the Rytm's stream stops for 3 s (the host's frames go on) and comes back
  loop::Ring q;
  static int32_t mem4[2 * (1 << 18)];
  q.init(mem4, 1 << 18);
  blocks(q, 0, 7000, 0, 500);                 // index k arrives at 500 + k + 6 (offset 500)
  uint64_t before = q.written();
  {
    uint64_t k = before;
    for (; k < before + 7000; k += 7) {       // ... then the next block arrives 144000 frames later
      int32_t b[14];
      for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(k + 144000 + i + 1), b[2 * i + 1] = 0;
      q.write(b, 7, 500 + k + 144000 + 6, 0);
    }
  }
  CHECK(q.frame_at(500 + 3000, &idx) && mem4[2 * idx] == 3001);                // before the pause: as it was
  CHECK(q.frame_at(500 + before + 144000 + 70, &idx) && mem4[2 * idx] == (int32_t)(before + 144000 + 71));   // after: the same line
  CHECK(q.gap_in(500 + before + 100, 100) && !q.gap_in(500 + 3000, 100));
  {   // H1: the 144000-frame pause was not written (bounded work in the completion): stale memory there, zeros for readers
    std::vector<int32_t> c(2 * 64);
    CHECK(q.copy_idx(before + 1000, 64, c.data()) == 64 && c[0] == 0 && c[2 * 63] == 0);
  }
  // records_during_playback: no pause exists: writes always advance
  uint64_t w0 = g.written();
  blocks(g, w0, w0 + 700, 0, 100);
  CHECK(g.written() == w0 + 700);
  // copy: a stretch by host frame, and refused once overwritten
  std::vector<int32_t> out(2 * 50);
  CHECK(j.copy(5000 + 9000, 50, out.data()) == 50 && out[0] == 9001 && out[2 * 49] == 9050);
  CHECK(j.copy(5000 + 30, 50, out.data()) == 50);   // (the ring is far from full: there)
  printf("  ring: ok\n");
}

// ---- the looper's fixture: a fake sequencer clock (origin, per16, lat), the ring fed as the Rytm's IN would, ticks every ms
static const uint32_t kRingFrames = 4194304;
static const int64_t kD = 1000;                 // host frame = ring index + kD (a block's last frame arrives that late)
static uint64_t g_now = 0;                      // seq::frames()
static double g_origin = 10000, g_per16 = 6000;   // 120 BPM
static int g_lat = 384;
static bool g_beats = true;
static uint16_t g_muted = 0;
static uint64_t g_fed = 0;
static int32_t (*g_sig)(uint64_t) = nullptr;   // the source's L at ring index k (null: the ramp k + 1)                      // ring indices written so far
static uint32_t g_us = 1000000;
static uint64_t g_fake = 10000000;   // core's now_us / now_ms (the Rytm-out sender's spacing)
static std::vector<uint8_t> g_store;
static int32_t* g_ring = nullptr;
static bool env_beat(double* o, double* p, int* l) {
  *l = g_lat;
  if (!g_beats) return false;
  *o = g_origin, *p = g_per16;
  return true;
}
static uint64_t env_frames() { return g_now; }
static uint16_t env_muted() { return g_muted; }
static double bar() { return 16 * g_per16; }
static double line(double k, double g = 0) { return g_origin + g_lat + k * (g ? g : bar()); }   // when bar line k's audio arrives
static void feed() {   // the ring up to host frame g_now: block b's last frame (index k + 6) arrives at k + 6 + kD
  while ((int64_t)(g_fed + 6 + kD) <= (int64_t)g_now) {
    int32_t b[14];
    for (int i = 0; i < 7; ++i) b[2 * i] = g_sig ? g_sig(g_fed + i) : (int32_t)(g_fed + i + 1), b[2 * i + 1] = -b[2 * i];
    loop::in_block(b, 7, g_fed + 6 + kD, 0);
    g_fed += 7;
  }
}
static void fresh(uint64_t store_bytes = 48u << 20) {
  static loop::Env env;
  env.beat_map = env_beat, env.frames = env_frames, env.muted = env_muted;
  loop::set_env(&env);
  if (!g_ring) g_ring = new int32_t[2 * (size_t)kRingFrames];
  g_store.assign(store_bytes, 0);
  loop::init(g_ring, kRingFrames, g_store.data(), store_bytes);
  loop::reset();
  host_fake_time(true, g_fake);
  g_sig = nullptr, g_now = 0, g_fed = 0, g_us = 1000000, g_origin = 10000, g_per16 = 6000, g_lat = 384, g_beats = true, g_muted = 0;
}
static void to(uint64_t h) {   // the clock to host frame h, a tick a ms on the way
  while (g_now + 48 <= h) {
    g_now += 48, g_us += 1000, g_fake += 1000;
    host_fake_time(true, g_fake);
    mod::rytm_out_tick(now_ms());
    feed();
    loop::tick(g_us);
  }
  g_now = h;
  feed();
  loop::tick(g_us);
}
static void jump(uint64_t h) {   // the clock on without a tick (the control loop busy elsewhere)
  g_now = h;
  feed();
}
static void run_ms(int ms) { to(g_now + 48u * ms); }
static int32_t srcval(double host_frame) { return (int32_t)((int64_t)host_frame - kD + 1); }   // the ring's L at that arrival frame
// what out_mix puts on F[.][0] for frame h (a block of 12 starting at h, frame 0)
static int32_t played_at(uint64_t h) {
  int32_t F[12][12] = {};
  loop::out_mix(F, 12, h);
  return F[0][0];
}
static int32_t played_over(uint64_t h, int32_t base) {   // what plays onto a POST frame that already holds `base`
  int32_t F[12][12] = {};
  F[0][0] = base;
  loop::out_mix(F, 12, h);
  return F[0][0];
}

// ---- Task 20: grabs
static void test_grabs() {
  // grab_ends_on_last_bar_line: pressed a third into a bar, forty bars in
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(5);
  CHECK(loop::grabs() == 1);
  const loop::Grab* g = loop::grab_at(0);
  CHECK(g->frames == (uint32_t)bar() && g->half_bars == loop::k1);
  CHECK(g->start == (uint64_t)line(39));   // bars [39, 40): ends on the last bar line (40)
  run_ms(50);
  CHECK(g->copied && g->pcm[0] == srcval(line(39)) && g->pcm[2 * (g->frames - 1)] == srcval(line(40) - 1));
  CHECK(g->pcm[-2 * loop::kPre] == srcval(line(39) - loop::kPre));
  CHECK(g->bpm > 119.9f && g->bpm < 120.1f);

  // starts_at_next_bar_line: sample 0 is rendered at origin + 41 x bar (lat early), nothing before it
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  CHECK(loop::state() == loop::kWait);
  for (uint64_t h = s0 - 24; h < s0; h += 12) CHECK(played_at(h) == 0);
  CHECK(played_at(s0 + 63) == g->pcm[2 * 63]);   // (a start fades in over 64 frames: unity from the 64th)
  // out_gain_scales_playback: the USB return's make-up (the Pi's 3.508, POST 10.9 dB down) multiplies what plays; 1 again after
  loop::set_out_gain(3.508f);
  { int64_t want = (int64_t)(3.508f * (float)g->pcm[2 * 63]), got = played_at(s0 + 63);
    CHECK(got - want <= 2 && want - got <= 2); }
  loop::set_out_gain(-1.0f);   // out of range: unity
  CHECK(loop::out_gain() == 1.0f && played_at(s0 + 63) == g->pcm[2 * 63]);
  loop::set_out_gain(1.0f);
  // soft_knee_not_clip: under 75 % of full scale the loop adds exactly; above it bends under full scale, never a hard clip
  { int32_t p = g->pcm[2 * 63];
    CHECK(played_over(s0 + 63, 1000000000) == 1000000000 + p);
    int32_t hot = played_over(s0 + 63, 2140000000), hot2 = played_over(s0 + 63, 2147000000);
    CHECK(hot > 1610612736 && hot < 2147483520 && hot2 >= hot && hot2 < 2147483520);
    int32_t neg = played_over(s0 + 63, -2147000000);
    CHECK(neg < -1610612736 && neg > -2147483520); }
  to(s0 + 200);
  CHECK(loop::state() == loop::kPlaying);

  // half_bar_grid: the grid is the half bar: the grab ends on the last half-bar line, 1 half bar long
  fresh();
  to((uint64_t)(line(40.5) + bar() / 8));
  CHECK(loop::grab(loop::kHalf) == loop::kOk);
  run_ms(60);
  g = loop::grab_at(0);
  CHECK(loop::grabs() == 1 && g->frames == (uint32_t)(bar() / 2) && g->start == (uint64_t)line(40));
  CHECK(played_at((uint64_t)(g_origin + 41 * bar()) + 63) == g->pcm[2 * 63]);   // the next half-bar line (E = 81 half bars, +1)

  // grab_waits_for_bar_audio: 2 ms after a bar line the line's audio has not come yet: wait, then take that bar
  fresh();
  to((uint64_t)(g_origin + 40 * bar()) + 96);   // 2 ms after the render-domain line; its audio is lat (8 ms) later
  CHECK(loop::grab(loop::k1) == loop::kOk);
  CHECK(loop::pending() && loop::grabs() == 0);
  run_ms(4);
  CHECK(loop::pending() && loop::grabs() == 0);
  run_ms(8);
  CHECK(!loop::pending() && loop::grabs() == 1);
  CHECK(loop::grab_at(0)->start == (uint64_t)line(39));   // bars 39: ends on line 40 itself

  // grab_refused_stopped
  fresh();
  to((uint64_t)line(40));
  g_beats = false;
  CHECK(loop::grab(loop::k1) == loop::kErrStopped);
  char t[48];
  CHECK(std::string(loop::err_text(loop::kErrStopped, loop::k1, t, sizeof t)) == "PLAY FIRST");
  CHECK(loop::stats().refused == 1);

  // grab_refused_when_ring_too_short: the press-time rule of spec 3.3, 16 bars at 45 BPM (bar 256000 frames, 5.33 s)
  g_per16 = 16000;
  fresh();
  g_per16 = 16000;
  to((uint64_t)(line(17) + bar() * 3 / 4));
  CHECK(loop::grab(loop::k16) == loop::kErrTooLong);
  CHECK(std::string(loop::err_text(loop::kErrTooLong, loop::k16, t, sizeof t)) == "16 BARS NEEDS 48 BPM");   // (47 while a piece counted 4.8 frames; now 48 frames a 64 KB piece: 24000 frames for the longest grab)
  CHECK(loop::grab(loop::k8) == loop::kOk);   // 8 bars at 45 BPM: fine
  fresh();
  g_per16 = 16000;
  to((uint64_t)(line(17) + bar() / 16));
  CHECK(loop::grab(loop::k16) == loop::kOk);   // pressed early in the bar: accepted
  fresh();
  g_per16 = 6000 * 120 / 48;   // 48 BPM
  to((uint64_t)(line(17) + bar() * 99 / 100));
  CHECK(loop::grab(loop::k16) == loop::kOk);   // any point: accepted
  fresh();
  g_per16 = 6000 * 120 / 30;   // 8 bars at 30 BPM
  to((uint64_t)(line(17) + bar() * 99 / 100));
  CHECK(loop::grab(loop::k8) == loop::kOk);
  printf("  grabs: bar line, start, half bar, waits, refusals ok\n");
}

static void test_store() {
  // store_drops_oldest: a store of 4 MB holds two 1-bar grabs (768 KB each), the 9th grab drops the oldest, at most 8 held
  fresh(8u << 20);
  to((uint64_t)line(10));
  uint32_t first_id = 0;
  for (int i = 0; i < 10; ++i) {
    CHECK(loop::grab(loop::k1) == loop::kOk);
    run_ms(40);
    if (i == 0) first_id = loop::grab_at(0)->id;
    to((uint64_t)(g_origin + (11 + i) * bar() + bar() / 2 + g_lat));
  }
  CHECK(loop::grabs() == 8);                              // 8 x 768 KB fits 8 MB: the 9th and 10th dropped the oldest
  CHECK(loop::grab_at(7)->id == first_id + 2);
  CHECK(loop::stats().dropped == 2);
  // a store that holds fewer: 2 MB, the third 1-bar grab (768 KB) drops the first
  fresh(2u << 20);
  to((uint64_t)line(10));
  for (int i = 0; i < 3; ++i) {
    CHECK(loop::grab(loop::k1) == loop::kOk);
    run_ms(40);
    to((uint64_t)(g_origin + (11 + i) * bar() + bar() / 2 + g_lat));
  }
  CHECK(loop::grabs() == 2 && loop::stats().dropped == 1);
  // grab_bigger_than_budget_refused: a 2-bar grab (1.5 MB) into 1 MB; nothing dropped for it
  fresh(1u << 20);
  to((uint64_t)line(10));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(40);
  CHECK(loop::grab(loop::k2) == loop::kErrNoRoom);
  char t[48];
  CHECK(std::string(loop::err_text(loop::kErrNoRoom, loop::k2, t, sizeof t)) == "NO ROOM");
  CHECK(loop::grabs() == 1 && loop::stats().dropped == 0);

  // copy_in_pieces: a 16-bar grab at 120 BPM (12.3 MB), never more than 64 KB in a tick (188 ticks)
  fresh(32u << 20);
  to((uint64_t)(line(40) + bar() / 4));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  run_ms(2);
  CHECK(loop::grabs() == 1 && !loop::grab_at(0)->copied);
  run_ms(250);
  CHECK(loop::grab_at(0)->copied && loop::stats().worst_copy_bytes <= loop::kPieceBytes && loop::stats().worst_copy_bytes > 0);
  CHECK(loop::stats().copies == 1);

  // plays_from_ring_until_copied: the frames are the ring's while the copy runs, the same after it
  fresh(32u << 20);
  to((uint64_t)(line(40) + bar() / 4));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  const loop::Grab* g = loop::grab_at(0);
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  CHECK(g && !g->copied);
  run_ms(3);   // (a piece is scanned: before it a loop from the ring is capped as if full scale)
  jump(s0 + 10);
  CHECK(!g->copied);
  int32_t early = played_at(s0 + 200);
  CHECK(early == srcval(line(40 - 16) + 200));   // the ring's own frames (the grab starts 16 bars back)
  run_ms(300);
  CHECK(g->copied && played_at(s0 + 200) == early);

  // copy_stays_ahead_of_writer: at 47 BPM a 16-bar grab barely fits; a tick a second (the writer 1.5 x the copy) overtakes it
  fresh(48u << 20);
  g_per16 = 6000 * 120 / 47;
  to((uint64_t)(line(20) + bar() / 16));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  for (int i = 0; i < 400 && loop::grabs(); ++i) {
    g_now += 48000, g_us += 1000;
    feed();
    loop::tick(g_us);
  }
  CHECK(loop::grabs() == 0 && loop::stats().abandoned == 1);

  // tempo_change_ignored: the loop keeps its length when the tempo moves
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  g = loop::grab_at(0);
  s0 = (uint64_t)(g_origin + 41 * bar());
  const uint32_t blen = g->frames;
  to(s0 + 100);
  g_per16 = 9000;
  g_origin += 777;
  to(s0 + blen + 200);
  CHECK(played_at(s0 + blen + 100) == g->pcm[2 * 100]);

  // transport_stop_plays_on: the loop plays on free; a new grab is refused (PLAY FIRST)
  g_beats = false;
  run_ms(10);
  CHECK(loop::state() == loop::kPlaying && played_at(s0 + 2 * blen + 100) == g->pcm[2 * 100]);
  CHECK(loop::grab(loop::k1) == loop::kErrStopped);

  // ring_records_during_playback: the ring keeps taking blocks while a loop plays
  uint64_t w0 = g_fed;
  run_ms(100);
  CHECK(g_fed > w0 && loop::state() == loop::kPlaying);
  printf("  store: drops, budget, pieces, ring-until-copied, overtaken ok\n");
}

// ---- Task 21: playback
static int32_t sine997(uint64_t k) { return (int32_t)(1e9 * sin(2 * M_PI * 997.0 * (double)k / 48000.0)); }
static void started(int hb = loop::k1, uint32_t* len = nullptr) {   // a grab pressed a third into bar 40, playing from bar 41's line
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(hb) == loop::kOk);
  run_ms(60);
  if (len) *len = loop::grab_at(0)->frames;
  to((uint64_t)(g_origin + 41 * bar()) + 500);
}
static void test_playback() {
  // seam_crossfaded: a 997 Hz sine does not fit the loop's length; across the end / start the step stays within the
  // signal's own slope (1.3e8 a frame) plus the crossfade's (2A / 64 = 3.1e7), where a plain wrap would jump up to 2e9
  fresh();
  g_sig = sine997;
  uint32_t len;
  started(loop::k1, &len);
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  double worst = 0;
  int32_t prev = played_at(s0 + len - 200);
  for (uint64_t h = s0 + len - 199; h < s0 + len + 200; ++h) {
    int32_t x = played_at(h);
    worst = fmax(worst, fabs((double)x - prev));
    prev = x;
  }
  CHECK(worst < 1.7e8);
  // (the loop's own frames away from the seam are the source's)
  CHECK(std::abs(played_at(s0 + 3000) - loop::grab_at(0)->pcm[2 * 3000]) <= 128);   // (float: 24 bits of mantissa)

  // lev_unity_at_100: LEV 100 is the source as it is; 50 half; 0 nothing; 127 1.27
  fresh();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  const int32_t* pcm = loop::grab_at(0)->pcm;
  loop::set_lev(100);
  CHECK(played_at(s0 + 5000) == pcm[2 * 5000]);
  loop::set_lev(50);
  run_ms(20);   // (a level change slews over 10 ms)
  CHECK(std::abs(played_at(s0 + 30000) - pcm[2 * 30000] / 2) <= 2);
  loop::set_lev(0);
  run_ms(20);
  CHECK(played_at(s0 + 40000) == 0);
  loop::set_lev(127);
  run_ms(20);
  CHECK(std::abs((double)played_at(s0 + 50000) - 1.27 * pcm[2 * 50000]) < 2000);

  // bal_curve (spec 3.8): 0: loop 0, own 1; 64: both 1; 127: loop 1, own 0; equal power at 32 / 96
  CHECK(fabs(loop::loop_gain(0)) < 1e-6 && fabs(loop::own_gain(0) - 1) < 1e-6);
  CHECK(fabs(loop::loop_gain(64) - 1) < 1e-6 && fabs(loop::own_gain(64) - 1) < 1e-6);
  CHECK(fabs(loop::loop_gain(127) - 1) < 1e-6 && fabs(loop::own_gain(127)) < 1e-6);
  CHECK(fabs(loop::loop_gain(32) * loop::loop_gain(32) + 0.5 - 1) < 0.01 && fabs(loop::own_gain(32) - 1) < 1e-6);   // (the loop: 1/2 power, own full)
  CHECK(fabs(loop::own_gain(96) * loop::own_gain(96) - 0.5) < 0.03 && fabs(loop::loop_gain(96) - 1) < 1e-6);
  // ... and in the mix: the Sidecar's own output (every channel of F) falls with BAL while the loop plays
  fresh();
  g_sig = nullptr;
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  pcm = loop::grab_at(0)->pcm;
  loop::set_bal(127);
  run_ms(30);
  int32_t F[12][12];
  for (auto& r : F)
    for (int& c : r) c = 100000000;
  loop::out_mix(F, 12, s0 + 60000);
  CHECK(F[11][5] == 0 && F[11][2] == 0);                              // the own channels silent at BAL 127
  CHECK(F[11][0] == pcm[2 * (60000 + 11)] && F[11][1] == pcm[2 * (60000 + 11) + 1]);   // the loop alone (its own was 0)
  loop::set_bal(0);
  run_ms(30);
  for (auto& r : F)
    for (int& c : r) c = 100000000;
  loop::out_mix(F, 12, s0 + 61000);
  CHECK(F[11][5] == 100000000 && F[11][0] == 100000000);              // BAL 0: no loop

  // cut_fades_10ms: a CUT at once fades the loop over 480 frames, then nothing; the state goes back to the ring
  fresh();
  g_sig = nullptr;
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  pcm = loop::grab_at(0)->pcm;
  uint64_t t = g_now;
  loop::stop(true, true);
  CHECK(loop::state() == loop::kFading);
  uint64_t at = t + 96;   // the margin the audio thread needs
  CHECK(played_at(at - 1) == pcm[2 * (at - 1 - s0)]);
  double prevv = 1;
  for (int i = 0; i < 480; i += 40) {
    int32_t x = played_at(at + i);
    double gain = (double)x / pcm[2 * (at + i - s0)];
    CHECK(gain <= prevv + 1e-6 && gain > -1e-6);
    prevv = gain;
  }
  CHECK(played_at(at + 480) == 0 && played_at(at + 2000) == 0);
  to(at + 600);
  CHECK(loop::state() == loop::kRing);
  printf("  playback: seam, LEV, BAL, cut ok\n");
}


// ---- Task 22: resample, stop / fade back, auto-mute, LIVE VIA
using Msg = std::vector<uint8_t>;
static std::vector<Msg> g_wire;
static void wire(const uint8_t* m, int n) { g_wire.emplace_back(m, m + n); }
static int wires(const Msg& m) {
  int n = 0;
  for (const Msg& o : g_wire) n += o == m;
  return n;
}
static void test_resample_stop_mute() {
  hooks.to_rytm = wire;
  // resample_instant_in_phase: a grab while a loop plays takes the last N bars at once, replaces the loop at the next
  // bar line (a 64-frame crossfade), and its start is a whole number of loops after the old one's
  fresh();
  uint32_t len;
  started(loop::k1, &len);
  uint64_t old_start, s0 = (uint64_t)(g_origin + 41 * bar());
  uint32_t old_len;
  CHECK(loop::playing_info(&old_start, &old_len) && old_start == s0 && old_len == len);
  played_at(s0 + 1000);   // (the audio thread runs: it has the first loop)
  const int32_t* first_pcm = loop::grab_at(0)->pcm;   // (the store's copy stays where it is as newer grabs shift the list)
  to((uint64_t)(line(43) + bar() / 4));
  CHECK(loop::state() == loop::kPlaying);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grabs() == 2 && loop::stats().resamples == 1);
  const loop::Grab* g2 = loop::grab_at(0);
  CHECK(g2->start == (uint64_t)line(42));   // the last bar, as the ring has it (loop + live), taken at once
  uint64_t ns;
  uint32_t nl;
  CHECK(loop::playing_info(&ns, &nl) && ns == (uint64_t)(g_origin + 44 * bar()) && (ns - old_start) % len == 0);
  CHECK(played_at(ns - 200) == first_pcm[2 * ((ns - 200 - old_start) % len)]);   // still the old loop until the bar line
  CHECK(played_at(ns + 100) == g2->pcm[2 * 100]);                              // the new one after the crossfade

  // stop_fade_and_cut (the FADE setting): quantised to the next bar line, a FADE of 1 bar lasts a bar; FUNC (now) at once
  fresh();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  loop::set_fade(loop::kFade1);
  to(s0 + 20000);
  loop::stop(false, false);
  uint64_t line42 = (uint64_t)(g_origin + 42 * bar());
  const int32_t* pcm = loop::grab_at(0)->pcm;
  CHECK(played_at(line42 - 200) == pcm[2 * ((line42 - 200 - s0) % len)]);   // full level until the line
  int32_t mid = played_at(line42 + len / 2);
  double gm = (double)mid / pcm[2 * ((line42 + len / 2 - s0) % len)];
  CHECK(gm > 0.45 && gm < 0.55);                                            // halfway down halfway through the bar
  CHECK(played_at(line42 + len + 100) == 0);
  to(line42 + len + 100);
  CHECK(loop::state() == loop::kRing);
  // a stop before the loop has started: it never starts
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  loop::stop(true, false);   // (the start is the next bar line, so is the stop's)
  CHECK(loop::state() == loop::kRing);

  // automute_default_off / live_via_default_off: a fresh state sends nothing
  fresh();
  g_wire.clear();
  g_muted = 0x005;
  started(loop::k1, &len);
  run_ms(12);
  CHECK(g_wire.empty());

  // automute_mutes_only_unmuted: tracks 1 and 3 muted before: one MUTE op sets the other ten; the fade back unmutes those
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_auto_mute(true);
  g_muted = 0x005;
  g_wire.clear();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(g_wire.empty());   // (not before the loop starts)
  s0 = (uint64_t)(g_origin + 41 * bar());
  to(s0 + 500);
  CHECK(g_wire.size() == 1 && g_wire[0] == Msg({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 1, 0x7A, 0x1F, 0, 0, 0xF7}));   // set 0xFFA
  g_muted = 0xFFF;   // the Rytm reports them muted
  to(s0 + 20000);
  g_wire.clear();
  loop::stop(true, false);
  to((uint64_t)(g_origin + 42 * bar()) - 100);
  CHECK(g_wire.empty());
  to((uint64_t)(g_origin + 42 * bar()) + 100);
  CHECK(g_wire.size() == 1 && g_wire[0] == Msg({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 1, 0, 0, 0x7A, 0x1F, 0xF7}));   // clr 0xFFA
  // with the caps off: CC 94 on each track's channel instead
  fresh();
  mod::rytm_out_reset();
  loop::set_auto_mute(true);
  g_muted = 0x005;
  g_wire.clear();
  started(loop::k1, &len);
  CHECK(g_wire.size() == 10 && wires({0xB1, 94, 1}) == 1 && wires({0xB2, 94, 1}) == 0 && wires({0xB0 | 11, 94, 1}) == 1);

  // user_mute_during_loop_kept: the user unmutes track 4 meanwhile: the looper no longer owns it
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_auto_mute(true);
  g_muted = 0x005;
  started(loop::k1, &len);
  g_muted = 0xFFF;
  run_ms(10);
  g_muted = 0xFF7;   // track 4 (bit 3) unmuted by hand
  run_ms(10);
  g_wire.clear();
  loop::stop(true, true);
  run_ms(10);
  CHECK(g_wire.size() == 1 && g_wire[0] == Msg({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 1, 0, 0, 0x72, 0x1F, 0xF7}));   // clr 0xFF2

  // live_via_perf: BAL 64: 127, 96: 63, 127: 0 on the macro asked for; nothing before it is asked for
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_live_via(3);
  CHECK(loop::live_perf(64) == 127 && loop::live_perf(96) == 63 && loop::live_perf(127) == 0 && loop::live_perf(0) == 127);
  g_wire.clear();
  started(loop::k1, &len);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 127, 0xF7}) == 1);
  loop::set_bal(96);
  run_ms(10);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 63, 0xF7}) == 1);
  loop::set_bal(127);
  run_ms(10);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 0, 0xF7}) == 1);
  loop::stop(true, true);   // the live comes back with the loop's end
  run_ms(30);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 127, 0xF7}) == 2);
  hooks.to_rytm = nullptr;
  host_fake_time(false, 0);
  printf("  resample, stop, auto-mute, live via ok\n");
}


// ---- Task 25: calibration, console, targets, settings
static uint64_t g_click_c0 = 0;
static int g_rtt = 0;
static int32_t g_click_amp = 6000000;   // the level the clicks come back at
static int32_t click_sig(uint64_t k) {   // the clicks as the Rytm hands them back: g_rtt frames after they were rendered
  for (int c = 0; c < 3; ++c) {
    int64_t kc = (int64_t)(g_click_c0 + (uint64_t)c * 9600 + (uint64_t)g_rtt) - kD;
    if ((int64_t)k >= kc && (int64_t)k < kc + 24) return g_click_amp;
  }
  return 0;
}
static std::string cmd(const char* c) {
  char o[600];
  CHECK(loop::command(c, o, sizeof o));
  return o;
}
static void test_cal_console() {
  hooks.to_rytm = wire;
  // cal_finds_click: a simulated round trip of 282 + 30 frames against the sequencer's latency (g_lat = 282): post_lat 30
  fresh();
  g_lat = 282;
  g_rtt = 282 + 30;
  g_sig = click_sig;
  to((uint64_t)line(10));
  loop::cal_start();
  CHECK(loop::cal_wanted());
  uint64_t first;
  int n;
  uint32_t gap;
  CHECK(loop::cal_info(&first, &n, &gap) && n == 3);
  g_click_c0 = first;
  // the OUT hook renders the clicks on POST 0 / 1 (and only there) at their frames
  int32_t F[12][12] = {};
  loop::out_mix(F, 12, first - 4);
  CHECK(F[3][0] == 0 && F[4][0] != 0 && F[4][1] == F[4][0] && F[4][2] == 0);
  // ... and the IN hook finds them: run the clock past the last click
  to(first + 3 * 9600 + 6000);
  CHECK(!loop::cal_wanted() && loop::post_lat() == 30 && loop::stats().cal_ok == 1);
  CHECK(std::string(loop::last_message()) == "POST LAT 30");
  // no click heard: refused, post_lat kept
  fresh();
  g_sig = nullptr;
  to((uint64_t)line(10));
  loop::set_post_lat(5);
  loop::cal_start();
  to(g_now + 14400 + 3 * 9600 + 6000);
  CHECK(loop::post_lat() == 5 && loop::stats().cal_fail == 1 && std::string(loop::last_message()) == "CAL: NO CLICK HEARD");

  // cal with the transport stopped: refused, post_lat kept (the sequencer's latency is not there to compare with)
  fresh();
  g_sig = click_sig;
  g_lat = 282, g_rtt = 312;
  to((uint64_t)line(10));
  loop::set_post_lat(7);
  loop::cal_start();
  CHECK(loop::cal_info(&first, &n, &gap));
  g_click_c0 = first;
  g_beats = false;
  to(first + 3 * 9600 + 6000);
  CHECK(loop::post_lat() == 7 && std::string(loop::last_message()) == "CAL: PLAY FIRST");

  // console_lines (spec 3.12)
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(cmd("loop").find("loop: ring,") == 0 && cmd("loop").find("0 grabs") != std::string::npos);
  CHECK(cmd("loop=grab:1").find("grab 1 bars") != std::string::npos);
  run_ms(60);
  CHECK(loop::grabs() == 1);
  CHECK(cmd("loop=grab:0.5").find("grab 1/2 bars") != std::string::npos);
  run_ms(60);
  CHECK(loop::grabs() == 2 && loop::grab_at(0)->half_bars == 1);
  CHECK(cmd("loop=grab:3").find("loop=grab:L") != std::string::npos);
  CHECK(cmd("loop=play:1").find("play grab 1") != std::string::npos);
  {   // by id: a grab landing meanwhile does not move what is meant (L6)
    uint32_t id1 = loop::grab_at(1)->id;
    CHECK(cmd(("loop=play:#" + std::to_string(id1)).c_str()).find("play grab 1") != std::string::npos);
    CHECK(cmd("loop=play:#9999").find("no grab") != std::string::npos);
    CHECK(cmd("loop=keep:#9999").find("NO GRAB") != std::string::npos);
  }
  CHECK(cmd("loop=bal:90").find("bal 90") != std::string::npos && loop::bal() == 90);
  CHECK(cmd("loop=lev:80").find("lev 80") != std::string::npos && loop::lev() == 80);
  CHECK(cmd("loop=fade:2").find("fade 2 bars") != std::string::npos && loop::fade() == loop::kFade2);
  CHECK(cmd("loop=fade:1/4").find("fade 1/4 bars") != std::string::npos && loop::fade() == loop::kFadeQuarter);
  CHECK(cmd("loop=mute:on").find("auto-mute on") != std::string::npos && loop::auto_mute());
  CHECK(cmd("loop=mute:off").find("off") != std::string::npos && !loop::auto_mute());
  CHECK(cmd("loop=live:perf5").find("PERF 5") != std::string::npos && loop::live_via() == 5);
  CHECK(cmd("loop=live:off").find("off") != std::string::npos && loop::live_via() == 0);
  CHECK(cmd("loop=keep:0").find("COPYING") == std::string::npos);   // (copied by now)
  loop::KeepJob job;
  CHECK(loop::keep_take(&job) && job.frames == loop::grab_at(0)->frames);
  CHECK(!loop::grab_at(0)->kept);
  loop::keep_finished(job.id, true, "take_001");
  loop::tick(g_us);   // core 3 applies it
  CHECK(loop::grab_at(0)->kept && std::string(loop::last_message()) == "KEPT take_001");
  CHECK(cmd("loop=keep:0").find("ALREADY KEPT") != std::string::npos);
  CHECK(cmd("loop=keep:7").find("NO GRAB") != std::string::npos);
  CHECK(cmd("loop=stop:cut:now").find("cut, at once") != std::string::npos);
  CHECK(cmd("loop=stop").find("fade, at the next bar line") != std::string::npos);
  CHECK(cmd("loop=cal").find("calibrating") != std::string::npos && loop::cal_wanted());
  g_beats = false;
  CHECK(cmd("loop=grab:1").find("PLAY FIRST") != std::string::npos);
  CHECK(std::string(loop::last_message()) == "PLAY FIRST");
  char js[1200];
  CHECK(loop::json(js, sizeof js) > 0 && strstr(js, "\"loop\":{\"on\":1") && strstr(js, "\"grabs\":[{") && strstr(js, "\"kept\":1"));
  CHECK(!loop::command("looper", js, sizeof js) && !loop::command("lo", js, sizeof js));

  {   // the status text without floats (the Pi's sniprintf has none): exact
    fresh();
    started(loop::k1);
    char jb[1500];
    CHECK(loop::json(jb, sizeof jb) > 0);
    CHECK(strstr(jb, "\"bpm\":120.0,") && strstr(jb, "\"bars\":1.0") && !strstr(jb, "e+"));
    const char* bp = strstr(jb, "\"bar\":");
    CHECK(bp && bp[6] >= '0' && bp[6] <= '1' && bp[7] == '.' && bp[8] >= '0' && bp[11] == ',');   // d.ddd
    std::string c = cmd("loop");
    CHECK(c.find(" pos 0.") != std::string::npos || c.find(" pos 1.") != std::string::npos);
    CHECK(c.find("/1,") != std::string::npos);
  }

  // bal_is_a_target: LOOP:BAL / LOOP:LEV are modulation targets; a value replaces the knob, a release gives it back
  fresh();
  mod::Tid tb = 0, tl = 0;
  CHECK(mod::parse("LOOP:BAL", &tb) && mod::parse("LOOP:LEV", &tl) && mod::valid(tb) && mod::valid(tl));
  mod::Range r = mod::range(tb);
  CHECK(r.lo == 0 && r.hi == 127);
  mod::reset();
  loop::set_bal(40);
  loop::set_lev(100);
  mod::offset(0, tb, 1.0f, 30.0f);   // an LFO's offset of +30 around the knob
  mod::end_tick();
  CHECK(loop::bal_eff() == 70 && loop::bal() == 40);
  mod::begin_tick();
  mod::end_tick();   // nothing refers to it any more: released
  CHECK(loop::bal_eff() == 40);
  CHECK(loop::own_gain(loop::bal_eff()) == 1.0f);

  // settings_persist: the card's loop.txt carries them; a line from elsewhere is kept
  fresh();
  loop::set_bal(96), loop::set_lev(77), loop::set_fade(loop::kFade4), loop::set_auto_mute(true), loop::set_live_via(7), loop::set_post_lat(-12);
  CHECK(loop::dirty());
  char tmpl[] = "/tmp/core_loop_XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  card_ok = true;
  system((std::string("mkdir -p ") + tmpl + "/.rytm; printf 'other=1\\nbal=1\\n' > " + tmpl + "/.rytm/loop.txt").c_str());
  loop::load();   // (an older file with other keys: bal=1 read, our dirty state replaced by the file's)
  CHECK(loop::bal() == 1 && !loop::dirty());
  loop::set_bal(96), loop::set_lev(77), loop::set_fade(loop::kFade4), loop::set_auto_mute(true), loop::set_live_via(7), loop::set_post_lat(-12);
  CHECK(loop::save());
  for (int i = 0; i < 2000 && fmt::post_busy(); ++i) {
    fmt::post_step();
    host_card_core(0);
    while (cardq::service()) {
    }
    host_card_core(3);
  }
  loop::reset();
  CHECK(loop::bal() == 64 && !loop::auto_mute());
  loop::load();
  CHECK(loop::bal() == 96 && loop::lev() == 77 && loop::fade() == loop::kFade4 && loop::auto_mute() && loop::live_via() == 7 && loop::post_lat() == -12);
  CHECK(!loop::save());   // nothing changed: nothing written
  hooks.to_rytm = nullptr;
  host_fake_time(false, 0);
  printf("  calibration, console, targets, settings ok\n");
}


// ---- the hooks' cost (spec 3.11: well under 1 us a 7-frame block on the Pi 4); here on the Mac, idle and with a loop playing
static void test_cost() {
  fresh();
  started(loop::k1);
  using clk = std::chrono::steady_clock;
  int32_t b[14] = {};
  int32_t F[7][12] = {};
  const int N = 1000000;
  auto t0 = clk::now();
  uint64_t arr = g_fed + kD;
  for (int i = 0; i < N; ++i) loop::in_block(b, 7, arr += 7, 0);
  double in_ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  t0 = clk::now();
  for (int i = 0; i < N; ++i) loop::out_mix(F, 7, s0 + 5000 + 7 * (uint64_t)(i % 10000));
  double play_ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  CHECK(F[3][0] != 0);   // (the loop was really playing)
  loop::stop(true, true);
  to(g_now + 20000);
  t0 = clk::now();
  for (int i = 0; i < N; ++i) loop::out_mix(F, 7, s0 + 5000 + 7 * (uint64_t)(i % 10000));
  double idle_ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  printf("  hook cost per 7-frame block (Mac): IN %.0f ns, OUT playing %.0f ns, OUT idle %.0f ns\n", in_ns, play_ns, idle_ns);
  CHECK(in_ns < 1000 && idle_ns < 200 && play_ns < 5000);   // generous: the Mac; the Pi 4 is a few times slower
}


// ---- fix round 1: the resample's wait (a new loop scheduled while the old one is heard)
static void test_resample_wait() {
  hooks.to_rytm = wire;
  // H2: a second grab, and a tap, during the wait do not cut the audible loop (a click, then silence)
  fresh();
  uint32_t len;
  started(loop::k1, &len);
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  played_at(s0 + 1000);   // (the audio thread runs: it has loop A)
  const int32_t* a_pcm = loop::grab_at(0)->pcm;
  to((uint64_t)(line(43) + bar() / 4));
  played_at(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  played_at(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);   // a third grab in the same wait
  run_ms(60);
  CHECK(loop::grabs() == 3);
  CHECK(loop::play(2));                        // and a tap on A's own pad
  uint64_t ns;
  uint32_t nl;
  CHECK(loop::playing_info(&ns, &nl) && ns == (uint64_t)(g_origin + 44 * bar()));
  uint64_t h = ns - 3000;
  CHECK(played_at(h) == a_pcm[2 * ((h - s0) % len)] && a_pcm[2 * ((h - s0) % len)] != 0);   // A is still heard until the bar line
  CHECK(loop::grab_at(2)->copied);   // (A is the oldest of three and still held: protected while heard)

  // H3: BAL 127: the Sidecar's own output stays down through the wait
  fresh();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  loop::set_bal(127);
  run_ms(30);
  int32_t F[12][12];
  auto own = [&](uint64_t hh) {
    for (auto& r : F)
      for (int& c : r) c = 100000000;
    loop::out_mix(F, 12, hh);
    return F[11][5];
  };
  CHECK(own(s0 + 20000) == 0);
  to((uint64_t)(line(43) + bar() / 4));
  own(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::playing_info(&ns, &nl));
  CHECK(own(g_now) == 0 && own(ns - 20000) == 0 && own(ns - 100) == 0);   // (it jumped back to full here)
  for (uint64_t hh = ns - 20; hh < ns + 200; ++hh) CHECK(own(hh) == 0);       // N1: and through the swap (a click 64 frames in)

  // H4: auto-mute and LIVE VIA hold through the wait; M1: a stop in the wait stops what is heard
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_auto_mute(true);
  loop::set_live_via(3);
  g_muted = 0x005;
  g_wire.clear();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  g_muted = 0xFFF;
  run_ms(20);
  size_t before = g_wire.size();
  played_at(g_now);
  to((uint64_t)(line(43) + bar() / 4));
  played_at(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(100);
  CHECK(g_wire.size() == before);   // no unmute, no PERF back to 127: the Rytm stays muted under the resample's wait
  CHECK(loop::playing_info(&ns, &nl));
  played_at(g_now);
  const int32_t* a2 = loop::grab_at(1)->pcm;
  uint64_t t = g_now;
  loop::stop(true, true);   // FUNC + T8 in the wait
  CHECK(loop::state() == loop::kFading);
  uint64_t at = t + 96;
  uint64_t hb = at - 2;
  CHECK(played_at(hb) == a2[2 * ((hb - s0) % len)]);   // A plays to the cut
  CHECK(played_at(at + 480) == 0 && played_at(at + 3000) == 0 && played_at(ns + 100) == 0);   // then nothing: not B at its start
  to(at + 600);
  CHECK(loop::state() == loop::kRing && g_wire.size() > before);   // and the mutes are given back
  hooks.to_rytm = nullptr;
  printf("  resample wait: a second grab / a tap, BAL, auto-mute, LIVE VIA and stop hold up\n");
}


// ---- M4: a grab that cannot fit even with every droppable grab gone drops none
static void test_room_without_drops() {
  fresh(4u << 20);   // 4 MB: four 1-bar grabs (768 KB each) fill it
  to((uint64_t)line(10));
  for (int i = 0; i < 4; ++i) {
    CHECK(loop::grab(loop::k1) == loop::kOk);
    run_ms(40);
    to((uint64_t)(g_origin + (11 + i) * bar() + bar() / 2 + g_lat));
  }
  CHECK(loop::grabs() == 4);
  CHECK(loop::play(2));   // the second grab made plays: it sits in the middle of the store, protected
  uint32_t played_id = loop::grab_at(2)->id;
  CHECK(loop::grab(loop::k4) == loop::kOk);   // 3 MB: fits the store's size, not any gap around the playing grab
  run_ms(60);
  CHECK(loop::grabs() == 4 && loop::stats().dropped == 0 && loop::grab_at(2)->id == played_id);
  printf("  room: a grab that cannot fit drops nothing\n");
}


// ---- N4 / N5
static void test_wait_edges() {
  // N5: a tap while the first loop waits for its start puts nothing under it
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grab(loop::kHalf) == loop::kOk);
  run_ms(60);
  CHECK(loop::play(1));   // (the first loop has not started)
  to((uint64_t)(g_origin + 45 * bar()));
  CHECK(loop::stats().grabs == 2);
  // N4: the copy of the loop under a waiting resample is abandoned: the new loop plays on, the old one is dropped
  fresh(48u << 20);
  g_per16 = 6000 * 120 / 47;   // 47 BPM: a 16-bar grab barely fits
  to((uint64_t)(line(20) + bar() / 16));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grab(loop::k16) == loop::kOk);   // copying from the ring: the loop plays from the ring
  CHECK(loop::grabs() == 2);
  uint32_t big = loop::grab_at(0)->id, small = loop::grab_at(1)->id;
  (void)big;
  CHECK(loop::play(loop::grab_index(small)));   // a tap on the 1-bar grab while the 16-bar one is still copying
  for (int i = 0; i < 400 && loop::stats().abandoned == 0; ++i) {
    g_now += 48000, g_us += 1000;
    feed();
    loop::tick(g_us);
  }
  CHECK(loop::stats().abandoned == 1 && loop::grabs() == 1);
  uint64_t st;
  uint32_t ln;
  CHECK(loop::playing_info(&st, &ln) && ln == loop::grab_at(0)->frames && loop::state() != loop::kRing);   // the tapped loop plays on
  printf("  wait edges: a first loop's tap, an abandoned copy under a resample\n");
}

// ---- OUT = POST / PRE, the level cap from each loop's peak, the POST return's gain (loop=cal), loop.txt's `out` and `postgain`
static int32_t g_sig_peak = 0;
static int32_t peaked_sig(uint64_t k) {   // a source with a known peak (every 97th frame), a ramp under it
  return k % 97 == 0 ? g_sig_peak : (int32_t)((k % 50) * (g_sig_peak / 50));
}
static int32_t fs(double x) { return (int32_t)(x * 2147483648.0); }   // x of full scale
static uint32_t pcm_peak(const loop::Grab* g) {
  uint32_t m = 0;
  for (uint32_t i = 0; i < 2 * g->frames; ++i) {
    int32_t x = g->pcm[i];
    uint32_t a = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
    m = a > m ? a : m;
  }
  return m;
}
// the loop's frames from h0 on, blocks of 12 as the OUT hook gets them: the largest |sample| on F[ch] / F[ch + 1]; `base` is on F[ch]
// and F[ch + 1] before (the Sidecar's own signal there)
static int32_t out_peak(uint64_t h0, int64_t frames, int ch, int32_t base = 0) {
  uint32_t m = 0;
  for (int64_t d = 0; d < frames; d += 12) {
    int32_t F[12][12] = {};
    for (int f = 0; f < 12; ++f) F[f][ch] = F[f][ch + 1] = base;
    loop::out_mix(F, 12, h0 + (uint64_t)d);
    for (int f = 0; f < 12; ++f)
      for (int c = ch; c < ch + 2; ++c) {
        int32_t x = F[f][c] - base;
        uint32_t a = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
        m = a > m ? a : m;
      }
  }
  return (int32_t)m;
}
static const loop::Grab* grab_loop(double peak_fs) {   // a bar grabbed from a source of this peak; plays from s0
  g_sig_peak = fs(peak_fs);
  g_sig = peaked_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  return loop::grab_at(0);
}
static const double kS0 = 41;   // the first loop's bar line (origin + kS0 x bar)
static uint64_t s0_of() { return (uint64_t)(g_origin + kS0 * bar()); }

static void test_out() {
  const int32_t kKneeFs = fs(loop::kLoopCeil);
  // peak_is_measured: the grab's peak is the store copy's (L or R)
  fresh();
  loop::set_out_gain(3.508f);
  const loop::Grab* g = grab_loop(0.9);
  // (the store keeps the grab as it was, 0.9 FS; the peak the cap uses is the loop as played through the limiter's envelope: the limiter
  // takes it down by 6 dB at most, 0.45 FS, a little more where the envelope's points are conservative)
  CHECK(g->copied && pcm_peak(g) >= (uint32_t)fs(0.9) - 1 && pcm_peak(g) <= (uint32_t)fs(0.9));
  CHECK(g->peak >= (uint32_t)fs(0.449) && g->peak <= (uint32_t)fs(0.47));
  // post_hot_loop_stays_clean: a loop that peaks at 0.9 FS plays at under the knee (x3.508 would be 3.2 FS): no knee, no clip
  int32_t pk = out_peak(s0_of(), (int64_t)bar(), 0);
  CHECK(pk <= kKneeFs + 64);
  CHECK(pk >= fs(0.975));   // ... and not quieter than it must be: the loop alone is not bent by the knee (it starts at the ceiling)
  // the same loop from the older-grab path (a tap) and with the Sidecar's own signal under it: the knee is the backstop, never a wrap
  int32_t own = out_peak(s0_of(), 4800, 0, fs(0.6));
  CHECK(own < 2147483520 && own > 0);

  // post_quiet_loop_gets_full_makeup: -20 dBFS keeps the whole x3.508
  fresh();
  loop::set_out_gain(3.508f);
  g = grab_loop(0.1);
  pk = out_peak(s0_of(), (int64_t)bar(), 0);
  double want = 3.508 * (double)g->peak;
  CHECK(pk > want * 0.99 && pk < want * 1.01);
  CHECK(g->peak < (uint32_t)fs(0.1) + 1 && g->peak > (uint32_t)fs(0.1) - 1000);

  // pre_adds_on_2_3: OUT = PRE: the loop lands on F[2..3] x 1.259 on top of what is there; F[0..1] are not touched
  fresh();
  loop::set_out_gain(3.508f);
  loop::set_pre_gain(3.508f / 2.786f);
  loop::set_out(loop::kOutPre);
  CHECK(loop::out() == loop::kOutPre && loop::dirty());
  g = grab_loop(0.1);
  {
    uint64_t h = s0_of() + 64;
    int32_t F[12][12] = {};
    for (int f = 0; f < 12; ++f) F[f][0] = 555, F[f][1] = -555, F[f][2] = 777, F[f][3] = -333;
    loop::out_mix(F, 12, h);
    for (int f = 0; f < 12; ++f) {
      int64_t wl = 777 + (int64_t)(loop::pre_gain() * (float)g->pcm[2 * (64 + f)]), wr = -333 + (int64_t)(loop::pre_gain() * (float)g->pcm[2 * (64 + f) + 1]);
      CHECK(F[f][0] == 555 && F[f][1] == -555);
      CHECK(llabs(F[f][2] - wl) <= 64 && llabs(F[f][3] - wr) <= 64);
      CHECK(F[f][4] == 0 && F[f][11] == 0);
    }
  }
  // ... and a hot loop is capped there too (same peak rule)
  fresh();
  loop::set_out_gain(3.508f);
  loop::set_pre_gain(3.508f / 2.786f);
  loop::set_out(loop::kOutPre);
  grab_loop(0.9);
  pk = out_peak(s0_of(), (int64_t)bar(), 2);
  CHECK(pk <= kKneeFs + 64 && pk >= fs(0.975));
  CHECK(out_peak(s0_of() + 64, 4800, 0) == 0);   // (nothing on 0/1)

  // postgain_replaces_the_makeup: the measured POST gain (1.5 .. 6.0) is used on POST instead of out_gain; insane: ignored
  fresh();
  loop::set_out_gain(3.508f);
  g = grab_loop(0.05);
  loop::set_postgain(2.0f);
  CHECK(loop::postgain() == 2.0f && loop::makeup() == 2.0f);
  { int64_t want2 = (int64_t)(2.0f * (float)g->pcm[2 * 70]), got = played_at(s0_of() + 70);
    CHECK(llabs(got - want2) <= 64); }
  loop::set_postgain(9.0f);
  CHECK(loop::postgain() == 0.0f && loop::makeup() == 3.508f);
  loop::set_postgain(1.0f);
  CHECK(loop::postgain() == 0.0f);
  loop::set_postgain(2.0f);
  loop::set_out(loop::kOutPre);
  loop::set_pre_gain(1.259f);
  CHECK(loop::makeup() == 1.259f);   // (PRE: the measured POST gain is not for it)
  loop::set_pre_gain(-1.0f);
  CHECK(loop::pre_gain() == 1.0f);

  // two_loops_in_a_row: a second grab while the first plays does not lower the first, and gets its own level; an older hot grab
  // played again after a quiet one is capped from its own peak
  fresh();
  loop::set_out_gain(3.508f);
  g = grab_loop(0.9);
  g_sig_peak = fs(0.1);   // what the Rytm carries from here on (the next bar line on) is quiet
  uint64_t s1 = s0_of();
  to(s1 + (uint64_t)(bar() / 2));
  CHECK(loop::state() == loop::kPlaying);
  int32_t x1 = played_at(s1 + 5000);
  CHECK(x1 != 0);
  to((uint64_t)(line(42) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grabs() == 2 && loop::grab_at(0)->copied);
  uint64_t s2, st;
  uint32_t ln;
  CHECK(loop::playing_info(&st, &ln));
  s2 = st;
  CHECK(s2 > s1 + (uint64_t)(bar() / 2) + 5000);
  CHECK(llabs((int64_t)played_at(s1 + 5000) - x1) <= 1);   // the first loop, heard until the second starts, is as loud as before
  to(s2 + 200);
  double want2 = 3.508 * (double)loop::grab_at(0)->peak;   // the second's own: the quiet one keeps the whole make-up
  int32_t pk2 = out_peak(s2 + 64, (int64_t)bar() - 128, 0);
  CHECK(pk2 > want2 * 0.99 && pk2 < want2 * 1.01);
  CHECK(loop::grab_at(1)->peak > (uint32_t)fs(0.44));
  CHECK(loop::play(1));   // the older, hot grab again
  to(g_now + (uint64_t)(2 * bar()));
  CHECK(loop::playing_info(&st, &ln) && st > s2);
  CHECK(out_peak(st + 64, (int64_t)bar() - 128, 0) <= kKneeFs + 64);
  // lev_bal_inside_the_cap: the cap is on the final gain: LEV 127 and any BAL never take a 0.9 FS loop over the ceiling (on L and R, both
  // outs); LEV under unity is quieter as before; a quiet loop at LEV 127 is louder than at 100
  for (int out : {loop::kOutPost, loop::kOutPre}) {
    fresh();
    loop::set_out_gain(3.508f);
    loop::set_pre_gain(3.508f / 2.786f);
    loop::set_out(out);
    grab_loop(0.9);
    const int c0 = out == loop::kOutPre ? 2 : 0;
    loop::set_lev(127);
    for (int bal : {127, 96, 64, 0}) {
      loop::set_bal(bal);
      pk = out_peak(s0_of(), (int64_t)bar(), c0);   // (both channels of the pair)
      CHECK(pk <= kKneeFs + 64);
      if (bal >= 64) CHECK(pk >= fs(0.97));
    }
    loop::set_bal(64);
    loop::set_lev(50);
    pk = out_peak(s0_of(), (int64_t)bar(), c0);
    CHECK(pk > fs(0.49) * 0.98 && pk < fs(0.49) * 1.02);   // LEV 50: half of the ceiling
  }
  fresh();
  loop::set_out_gain(3.508f);
  g = grab_loop(0.05);
  loop::set_lev(127);
  pk = out_peak(s0_of(), (int64_t)bar(), 0);
  want = 3.508 * 1.27 * (double)g->peak;
  CHECK(pk > want * 0.99 && pk < want * 1.01);

  // soft_knee_leaves_the_base: content already on the pair (a hot PRE / POST signal) is never bent by the loop's knee: a silent loop
  // gives the base back exactly, a tiny one adds plainly
  for (int out : {loop::kOutPost, loop::kOutPre}) {
    fresh();
    loop::set_out_gain(3.508f);
    loop::set_pre_gain(3.508f / 2.786f);
    loop::set_out(out);
    const int c0 = out == loop::kOutPre ? 2 : 0;
    g = grab_loop(1e-7);   // (a tiny loop: peak ~ 200)
    CHECK(g->peak > 0 && g->peak < 1000);
    int32_t base = fs(0.99);   // (past the knee on its own)
    int32_t F[12][12] = {};
    for (int f = 0; f < 12; ++f) F[f][c0] = base, F[f][c0 + 1] = -base;
    loop::out_mix(F, 12, s0_of() + 64);
    for (int f = 0; f < 12; ++f) {
      int64_t wl = base + (int64_t)(loop::makeup() * (float)g->pcm[2 * (64 + f)]);
      CHECK(llabs(F[f][c0] - wl) <= 2048 && llabs(F[f][c0] - base) <= 2048 && F[f][c0] > base - 4096);
    }
    // ... and a silent loop (nothing in the ring): exactly the base
    fresh();
    loop::set_out_gain(3.508f);
    loop::set_pre_gain(3.508f / 2.786f);
    loop::set_out(out);
    g_sig_peak = 0;
    g = grab_loop(0.0);
    CHECK(g->peak == 0);
    int32_t E[12][12] = {};
    for (int f = 0; f < 12; ++f) E[f][c0] = base, E[f][c0 + 1] = -base;
    loop::out_mix(E, 12, s0_of() + 64);
    for (int f = 0; f < 12; ++f) CHECK(E[f][c0] == base && E[f][c0 + 1] == -base);
  }
  // hot base + a loud loop: shaped into the headroom, never above 2147483520, above the base
  fresh();
  loop::set_out_gain(3.508f);
  loop::set_pre_gain(3.508f / 2.786f);
  loop::set_out(loop::kOutPre);
  g = grab_loop(0.14);
  for (int base : {fs(0.9), fs(0.99)}) {
    int32_t F[12][12] = {};
    for (int f = 0; f < 12; ++f) F[f][2] = base;
    loop::out_mix(F, 12, s0_of() + 64);
    for (int f = 0; f < 12; ++f) {
      if (g->pcm[2 * (64 + f)] > 100000000) CHECK(F[f][2] > base && F[f][2] < 2147483520);
    }
  }
  // a base hard-clipped on the rail, the loop pulling it back: added as it is (no jump between the rail and a little off it)
  fresh();
  loop::set_out_gain(3.508f);
  g = grab_loop(0.1);
  for (int rail : {INT32_MIN, INT32_MIN + 64, INT32_MIN + 4096}) {
    int32_t F[12][12] = {};
    for (int f = 0; f < 12; ++f) F[f][0] = rail, F[f][1] = -(rail + 1);   // L on the negative rail, R on the positive one
    loop::out_mix(F, 12, s0_of() + 64);
    for (int f = 0; f < 12; ++f) {
      int64_t wl = (int64_t)rail + (int64_t)(3.508f * (float)g->pcm[2 * (64 + f)]);   // (the loop's L is positive: pulls L back)
      int64_t wr = -((int64_t)rail + 1) - (int64_t)(3.508f * (float)g->pcm[2 * (64 + f)]);
      CHECK(llabs(F[f][0] - wl) <= 64 && llabs(F[f][1] - wr) <= 64);
    }
  }

  // makeup_change_inside_the_cap: the cap follows a make-up change at once (a loop=cal result while a hot loop plays): no overshoot
  fresh();
  loop::set_out_gain(3.508f);
  grab_loop(0.9);
  { uint64_t h = s0_of();
    pk = out_peak(h, 2400, 0);
    CHECK(pk <= kKneeFs + 64);
    loop::set_postgain(6.0f);
    pk = out_peak(h + 2400, 4800, 0);
    CHECK(pk <= kKneeFs + 64);
    loop::set_postgain(1.6f);   // and down: the loop eases up, never over
    pk = out_peak(h + 7200, 4800, 0);
    CHECK(pk <= kKneeFs + 64); }

  printf("  out: POST cap from the peak, quiet keeps the make-up, PRE onto 2/3, postgain, two loops in a row, LEV / BAL inside the cap ok\n");
}

// ---- the grab limiter
static int32_t drum_sig(uint64_t k) {   // a body at 0.1 FS and a short, loud transient (0.8 FS, decaying over a few ms) every 12000 frames
  uint64_t m = k % 12000;
  double body = 0.1 * sin((double)k * 0.01), tr = m < 400 ? 0.8 * exp(-(double)m / 60.0) * sin((double)k * 0.9) : 0.0;
  return (int32_t)((body + tr) * 2147483648.0);
}
static int32_t sine_sig(uint64_t k) { return (int32_t)((double)g_sig_peak * sin((double)k * 0.01)); }
static int32_t src_at(const loop::Grab* g, uint32_t i) { return g_sig((uint64_t)((int64_t)g->start - kD) + i); }   // the loop's L as it was in the ring
static void test_limiter() {
  loop::set_out_gain(3.508f);
  // below_threshold_is_bit_identical: a sine at 0.27 FS never reaches the threshold (0.98 / 3.508 = 0.279 FS): not a bit changes
  fresh();
  loop::set_out_gain(3.508f);
  g_sig_peak = fs(0.27);
  g_sig = sine_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  const loop::Grab* g = loop::grab_at(0);
  CHECK(g->copied);
  int same = 0;
  for (uint32_t i = 0; i < g->frames; ++i)
    if (g->pcm[2 * i] == src_at(g, i) && g->pcm[2 * i + 1] == -src_at(g, i)) ++same;
  CHECK(same == (int)g->frames);   // (also proves the alignment of src_at)
  // a sine just above it is changed, by at most 6 dB
  fresh();
  loop::set_out_gain(3.508f);
  g_sig_peak = fs(0.4);
  g_sig = sine_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  g = loop::grab_at(0);
  CHECK(g->copied && g->peak < (uint32_t)fs(0.4) - 1000 && g->peak >= (uint32_t)fs(0.2) - 1000);

  // drum-like loop: the limiter's gain moves smoothly, never takes more than 6 dB, never raises; the loop is louder than it was
  fresh();
  loop::set_out_gain(3.508f);
  g_sig = drum_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  g = loop::grab_at(0);
  CHECK(g->copied);
  // the store is the audio as grabbed, bit for bit (KEEP saves it): not a sample changed by the limiter
  int same2 = 0;
  double e_src = 0, srcpk = 0;
  for (uint32_t i = 0; i < g->frames; ++i) {
    double sv = (double)src_at(g, i);
    if (g->pcm[2 * i] == src_at(g, i) && g->pcm[2 * i + 1] == -src_at(g, i)) ++same2;
    e_src += sv * sv, srcpk = fmax(srcpk, fabs(sv));
  }
  CHECK(same2 == (int)g->frames);
  { loop::KeepJob job;
    char kmsg[48];
    CHECK(loop::keep(0, kmsg, sizeof kmsg) && loop::keep_take(&job));
    CHECK(job.pcm == g->pcm && job.frames == g->frames);
    int ksame = 0;
    for (uint32_t i = 0; i < job.frames; ++i)
      if (job.pcm[2 * i] == src_at(g, i)) ++ksame;
    CHECK(ksame == (int)job.frames);   // KEEP's source is the original, the limited hot loop included
    loop::keep_finished(job.id, true, "x");
    run_ms(5); }
  // the played loop is the original x the envelope x the cap's gain G: the envelope never steps more than 0.5 / 72 a frame, never takes
  // more than 6 dB, never adds
  const double G = fmin(3.508, 0.98 * 2147483648.0 / (double)g->peak);
  double minr = 2, maxr = 0, maxstep = 0, prev = -1;
  uint64_t limited = 0;
  double e_out = 0;
  int64_t n = 0;
  int32_t pk = 0;
  std::vector<double> played((size_t)bar());
  for (int64_t d = 0; d < (int64_t)bar(); d += 12) {
    int32_t F[12][12] = {};
    loop::out_mix(F, 12, s0_of() + (uint64_t)d);
    for (int f = 0; f < 12; ++f) {
      played[(size_t)(d + f)] = (double)F[f][0];
      e_out += (double)F[f][0] * (double)F[f][0], ++n;
      pk = std::max<int32_t>(pk, std::abs(F[f][0]));
    }
  }
  for (uint32_t i = 80; i + 80 < g->frames && i < played.size(); ++i) {   // (past the start's fade, short of the seam's crossfade)
    double sv = (double)src_at(g, i);
    if (fabs(sv) > 67108864.0) {   // (> 1/32 FS: the ratio is exact enough)
      double r = played[i] / (sv * G);
      minr = fmin(minr, r), maxr = fmax(maxr, r);
      if (prev >= 0) maxstep = fmax(maxstep, fabs(r - prev));
      prev = r;
      if (r < 0.999) ++limited;
    } else {
      prev = -1;
    }
  }
  CHECK(limited > 100);
  CHECK(minr >= 0.495 && maxr <= 1.0002);   // never more than 6 dB (0.5), never a boost
  CHECK(maxstep < 0.0075);                   // 0.5 / 72 frames at the fastest
  // louder than on the 0.75 cap without the limiter (225824e): its gain was min(3.508, 0.75 / peak) on the whole loop
  double rms_old = sqrt(e_src / g->frames) * fmin(3.508, 0.75 * 2147483648.0 / srcpk);
  double gain_db = 20.0 * log10(sqrt(e_out / (double)n) / rms_old);
  printf("  limiter: drum loop %.1f dB louder in RMS than the 0.75 cap alone (peak out %.3f FS)\n", gain_db, (double)pk / 2147483648.0);
  CHECK(gain_db >= 4.0 && gain_db >= 7.0);   // (7.5 with the limiter writing the audio in place: the envelope is within 0.5 dB of it)
  CHECK(pk <= fs(0.98) + 128 && pk >= fs(0.975));   // the loop alone: under the ceiling, not bent by the knee

  // a second grab does not touch the first's store copy
  uint64_t h1 = 1469598103934665603ull;
  for (uint32_t i = 0; i < 2 * g->frames; ++i) h1 = (h1 ^ (uint32_t)g->pcm[i]) * 1099511628211ull;
  uint32_t first_id = g->id;
  to((uint64_t)(line(42) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grabs() == 2 && loop::grab_at(0)->copied);
  const loop::Grab* old = loop::grab_at(1);
  CHECK(old->id == first_id);
  uint64_t h2 = 1469598103934665603ull;
  for (uint32_t i = 0; i < 2 * old->frames; ++i) h2 = (h2 ^ (uint32_t)old->pcm[i]) * 1099511628211ull;
  CHECK(h1 == h2);

  // timing on core 3: a full-length loop (16 bars at 120 BPM = 12 MB, 47 pieces of 256 KB), the time of each tick that copies a piece
  fresh();
  loop::set_out_gain(3.508f);
  g_sig = drum_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  std::vector<double> piece_us;
  for (int it = 0; it < 4000 && !(loop::grabs() && loop::grab_at(0)->copied); ++it) {
    g_now += 48, g_us += 1000, g_fake += 1000;
    host_fake_time(true, g_fake);
    mod::rytm_out_tick(now_ms());
    feed();
    bool copying = loop::grabs() && !loop::grab_at(0)->copied;
    auto t0 = std::chrono::steady_clock::now();
    loop::tick(g_us);
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (copying && loop::stats().worst_copy_bytes == loop::kPieceBytes) piece_us.push_back(us);
  }
  CHECK(loop::grabs() && loop::grab_at(0)->copied && piece_us.size() > 150);
  std::sort(piece_us.begin(), piece_us.end());
  double med = piece_us[piece_us.size() / 2], worst = piece_us.back(), total = 0;
  for (double u : piece_us) total += u;
  printf("  limiter timing (core 3, Mac): %zu pieces of 64 KB, median %.0f us, mean %.0f us, worst %.0f us per tick (copy + peak + envelope); Pi 4 at ~1/4 the speed: ~%.0f us median, ~%.0f us mean\n",
         piece_us.size(), med, total / (double)piece_us.size(), worst, 4 * med, 4 * total / (double)piece_us.size());
  // the budget: a piece on the Pi must stay near 1 ms (the control pass, 48 frames, and the send FX's 2 ms margin): 250 us on the Mac (3x
  // what is measured: ~85 us). The median, not the worst: a loaded machine delays single ticks
  CHECK(med < 250);

  // a loop still being copied plays from the ring, unlimited, with the cap from the peak so far: it cannot exceed the ceiling
  fresh();
  loop::set_out_gain(3.508f);
  g_sig = drum_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  for (int it = 0; it < 200 && !loop::grabs(); ++it) run_ms(1);
  CHECK(loop::grabs() == 1 && !loop::grab_at(0)->copied);
  run_ms(3);   // (the loop starts at the next bar line, 2 ms away at the soonest: by then a piece or more is scanned)
  CHECK(!loop::grab_at(0)->copied && loop::grab_at(0)->peak > 0);
  uint64_t st;
  uint32_t ln;
  CHECK(loop::playing_info(&st, &ln));
  int32_t mp = out_peak(st + 100, 20000, 0);   // (the copy has not moved: the ring, the peak so far)
  CHECK(!loop::grab_at(0)->copied && mp <= fs(0.98) + 128 && mp > fs(0.5));
}

// ---- the envelope: the floor, the release, the tail, the ring -> store switch
static int32_t burst_sig(uint64_t k) {   // a quiet sine and, once a bar, 200 frames at 0.9 FS
  double v = 0.05 * sin((double)k * 0.01);
  uint64_t m = k % 96000;
  if (m >= 30000 && m < 30200) v = 0.9 * sin((double)k * 0.5);
  return (int32_t)(v * 2147483648.0);
}
static void test_envelope() {
  fresh();
  loop::set_out_gain(3.508f);
  g_sig = burst_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  const loop::Grab* g = loop::grab_at(0);
  CHECK(g->copied && g->env);
  const uint32_t pts = (g->frames + loop::kPre + 15) / 16 + 2;
  int64_t first = -1, last = -1;
  for (uint32_t i = 0; i < g->frames; ++i)
    if (llabs((long long)src_at(g, i)) > fs(0.5)) {
      if (first < 0) first = i;
      last = i;
    }
  CHECK(first > 0 && last > first);
  uint32_t minv = 65535, maxstep = 0;
  for (uint32_t j = 0; j < pts; ++j) {
    minv = std::min<uint32_t>(minv, g->env[j]);
    if (j) maxstep = std::max<uint32_t>(maxstep, (uint32_t)abs((int)g->env[j] - (int)g->env[j - 1]));
  }
  CHECK(minv == 16384);       // the 6 dB floor: a burst that needs 10 dB gets exactly 6 (x 32768)
  CHECK(maxstep <= 3642);     // a point every 16 frames, 0.5 / 72 a frame at the fastest: 0.111 x 32768
  const uint32_t jb = (uint32_t)(first + loop::kPre) / 16, je = (uint32_t)(last + loop::kPre) / 16;
  for (uint32_t j = 0; j + 8 < jb; ++j) CHECK(g->env[j] == 32768);   // (before the look-ahead: unity)
  CHECK(g->env[je + 2880 / 16] < 32768 && g->env[je + 2880 / 16] > 16384);   // 60 ms after the burst: still coming back
  for (uint32_t j = je + 9600 / 16; j < pts; ++j) CHECK(g->env[j] == 32768);  // 200 ms after it: exactly unity, to the end
  // a loop whose tail is quiet plays it at the make-up alone (the envelope is 1 there)
  printf("  envelope: floor 6 dB exactly, step <= %u of 32768 per point, release past 60 ms, unity from 200 ms\n", maxstep);

  // ring_to_store_switch: a loop that plays from the ring while its copy runs (unlimited, the cap from its peak so far) moves to the
  // limited copy without a step: the envelope fades in over 10 ms as the cap's gain eases up
  fresh(32u << 20);
  loop::set_out_gain(3.508f);
  g_sig_peak = fs(0.8);
  g_sig = sine_sig;
  to((uint64_t)(line(40) + bar() / 4));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  for (int it = 0; it < 300 && !loop::grabs(); ++it) run_ms(1);
  run_ms(3);
  g = loop::grab_at(0);
  uint64_t st;
  uint32_t ln;
  CHECK(!g->copied && g->peak > 0 && loop::playing_info(&st, &ln));
  double prev = 0, worst2 = 0, late_peak = 0, all_peak = 0;
  int sw_it = -1;
  for (int it = 0; it < 500; ++it) {
    jump(st + 48u * (uint64_t)it);
    g_us += 1000;
    loop::tick(g_us);
    if (sw_it < 0 && g->copied) sw_it = it;
    for (int b = 0; b < 4; ++b) {
      int32_t F[12][12] = {};
      loop::out_mix(F, 12, st + 48u * (uint64_t)it + 12u * (uint64_t)b);
      for (int f = 0; f < 12; ++f) {
        double x = (double)F[f][0] / 2147483648.0;
        if (it >= 3) worst2 = fmax(worst2, fabs(x - prev));
        all_peak = fmax(all_peak, fabs(x));
        prev = x;
        if (sw_it >= 0 && it >= sw_it + 40) late_peak = fmax(late_peak, fabs(x));
      }
    }
  }
  CHECK(sw_it > 10 && sw_it < 450);   // it did switch, in the middle of the playing
  CHECK(worst2 < 0.03);               // a sine at 0.98 FS moves 0.01 a frame; an envelope of 0.5 appearing at once would step 0.49
  CHECK(all_peak <= 0.98 + 128.0 / 2147483648.0);   // every frame through the switch: under the ceiling (the cap follows the envelope's fade)
  CHECK(late_peak > 0.9 && late_peak < 0.985);   // and the loop is at the ceiling again after the switch (limited copy x the cap's gain)

  // first_pass_held_back: a grab whose first control pass has not scanned a piece yet (peak 0) plays capped as if full scale
  fresh(32u << 20);
  loop::set_out_gain(3.508f);
  g_sig_peak = fs(0.8);
  g_sig = sine_sig;
  to((uint64_t)(line(40) + bar() / 4));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  for (int it = 0; it < 300 && !loop::grabs(); ++it) run_ms(1);
  g = loop::grab_at(0);
  CHECK(g && !g->copied && g->peak == 0 && loop::playing_info(&st, &ln));
  int32_t held = out_peak(st + 100, 20000, 0);   // (no tick since: the peak is still 0)
  CHECK(held <= fs(0.98) + 128 && held > 0);
  printf("  envelope: ring -> store switch at tick %d: largest step %.4f FS\n", sw_it, worst2);
}

static int32_t smooth_sig(uint64_t k) { return (int32_t)(0.1 * 2147483648.0 * sin((double)k * 0.01)); }
// OUT switched while the loop plays: a 10 ms crossfade between the pairs, no step, the end state on the new pair only.
// back_at: flip back at this block (inside the fade); stop_at: cut the loop at this block (inside the fade)
static void switch_case(int to_pre, int back_at, int stop_at) {
  const float kPostMk = 3.508f, kPreMk = 3.508f / 2.786f;
  fresh();
  loop::set_out_gain(kPostMk);
  loop::set_pre_gain(kPreMk);
  loop::set_out(to_pre ? loop::kOutPost : loop::kOutPre);
  g_sig = smooth_sig;
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  uint64_t h = s0_of() + 64;
  double prev_a = 0, prev_b = 0, worst = 0;
  bool both = false;
  const int nblk = 200;   // 2400 frames: the switch at block 40, 3 ms in; the fade is 480 frames = 40 blocks
  for (int b = 0; b < nblk; ++b) {
    if (b == 40) loop::set_out(to_pre ? loop::kOutPre : loop::kOutPost);
    if (b == back_at) loop::set_out(to_pre ? loop::kOutPost : loop::kOutPre);
    if (b == stop_at) loop::stop(true, true);
    int32_t F[12][12] = {};
    loop::out_mix(F, 12, h);
    for (int f = 0; f < 12; ++f) {
      double a = (double)F[f][0] / 2147483648.0, c = (double)F[f][2] / 2147483648.0;
      if (b > 0 || f > 0) worst = fmax(worst, fmax(fabs(a - prev_a), fabs(c - prev_b)));
      prev_a = a, prev_b = c;
      if (F[f][0] != 0 && F[f][2] != 0) both = true;
    }
    h += 12;
    if (b == 0) to(g_now + 48);   // (the control loop goes on: the stop is taken)
    else if (b % 4 == 0) to(g_now + 48);
  }
  CHECK(worst < 0.004);   // a jump would be 0.1 x 3.5 = 0.35
  CHECK(both);            // it did cross through both
  int32_t F[12][12] = {};
  loop::out_mix(F, 12, h);
  const bool end_pre = back_at >= 0 ? !to_pre : to_pre;
  for (int f = 0; f < 12; ++f) {
    if (stop_at >= 0) CHECK(F[f][0] == 0 && F[f][1] == 0 && F[f][2] == 0 && F[f][3] == 0);
    else if (end_pre) CHECK(F[f][0] == 0 && F[f][1] == 0 && F[f][2] != 0);
    else CHECK(F[f][2] == 0 && F[f][3] == 0 && F[f][0] != 0);
  }
  if (stop_at >= 0) {   // the next loop starts on the pair OUT names now (no fade from the old one)
    CHECK(loop::play(0));
    to(g_now + (uint64_t)(2 * bar()));
    uint64_t st;
    uint32_t ln;
    CHECK(loop::playing_info(&st, &ln));
    int32_t G[12][12] = {};
    loop::out_mix(G, 12, st + 100);
    if (end_pre) CHECK(G[0][0] == 0 && G[0][2] != 0);
    else CHECK(G[0][2] == 0 && G[0][0] != 0);
  }
}
static void test_out_switch() {
  for (int to_pre = 0; to_pre < 2; ++to_pre) {
    switch_case(to_pre, -1, -1);   // one flip
    switch_case(to_pre, 50, -1);   // flipped back 10 blocks (120 frames) into the fade: ends on the first pair
    switch_case(to_pre, -1, 50);   // the loop is cut 10 blocks into the fade: silence, and the next loop starts on the new pair
  }
  printf("  out: switching OUT while a loop plays crossfades (also flipped twice, or the loop stopped, in the fade), no step, ends on the right pair only\n");
}

static void test_out_cal_settings() {
  hooks.to_rytm = wire;
  // cal_measures_the_return_gain: the clicks come back at 1 / 3.508 of what went out: the gain is found within 0.2 dB, saved and used
  const double click_pk = 8.0e8 * sin(3.14159265358979 * 12.0 / 25.0);
  for (double gain : {3.508, 2.2, 5.5}) {
    fresh();
    g_lat = 282, g_rtt = 282 + 30;
    g_click_amp = (int32_t)(click_pk / gain);
    g_sig = click_sig;
    to((uint64_t)line(10));
    loop::set_out_gain(1.0f);
    loop::cal_start();
    uint64_t first;
    int n;
    uint32_t gap;
    CHECK(loop::cal_info(&first, &n, &gap));
    g_click_c0 = first;
    to(first + 3 * 9600 + 6000);
    CHECK(!loop::cal_wanted() && loop::post_lat() == 30);
    double db = 20.0 * log10((double)loop::postgain() / gain);
    CHECK(loop::postgain() > 0.0f && std::fabs(db) < 0.2);
    CHECK(loop::makeup() == loop::postgain() && loop::dirty());
    std::string m = loop::last_message();
    CHECK(m.find("POST LAT 30 X") == 0 && m.find("DB") != std::string::npos);
    if (gain == 3.508) CHECK(m.find("X3.5") != std::string::npos && m.find("10.9DB") != std::string::npos);
  }
  // a return that is not a POST return (live signal on top, a gain of 30): ignored, the latency still taken
  fresh();
  g_lat = 282, g_rtt = 312;
  g_click_amp = (int32_t)(click_pk / 30.0);
  g_sig = click_sig;
  to((uint64_t)line(10));
  loop::cal_start();
  { uint64_t first; int n; uint32_t gap;
    CHECK(loop::cal_info(&first, &n, &gap));
    g_click_c0 = first;
    to(first + 3 * 9600 + 6000); }
  CHECK(loop::post_lat() == 30 && loop::postgain() == 0.0f && std::string(loop::last_message()) == "POST LAT 30");
  g_click_amp = 6000000;

  // out_and_postgain_persist: loop.txt carries `out` and `postgain`; an unknown value is ignored
  fresh();
  char tmpl[] = "/tmp/core_loop_XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  card_ok = true;
  std::string file = std::string(tmpl) + "/.rytm/loop.txt";
  auto put = [&](const char* txt) { system((std::string("mkdir -p ") + tmpl + "/.rytm; printf '" + txt + "' > " + file).c_str()); };
  auto slurp = [&]() {
    std::string r;
    if (FILE* f = fopen(file.c_str(), "r")) {
      char b[256];
      size_t k;
      while ((k = fread(b, 1, sizeof b, f)) > 0) r.append(b, k);
      fclose(f);
    }
    return r;
  };
  put("other=1\\nout=pre\\npostgain=3508\\n");
  loop::load();
  CHECK(loop::out() == loop::kOutPre && std::fabs(loop::postgain() - 3.508f) < 1e-4f && !loop::dirty());
  put("out=sideways\\npostgain=9000\\n");
  loop::reset();
  loop::load();
  CHECK(loop::out() == loop::kOutPost && loop::postgain() == 0.0f);
  put("postgain=1000\\n");
  loop::load();
  CHECK(loop::postgain() == 0.0f);
  put("postgain=-3508\\nout=prefix\\n");
  loop::load();
  CHECK(loop::postgain() == 0.0f && loop::out() == loop::kOutPost);
  put("out=post\\n");
  loop::load();
  CHECK(loop::out() == loop::kOutPost && loop::postgain() == 0.0f);
  loop::reset();
  loop::set_out(loop::kOutPre);
  loop::set_postgain(2.5f);
  CHECK(loop::dirty() && loop::save());
  for (int i = 0; i < 2000 && fmt::post_busy(); ++i) {
    fmt::post_step();
    host_card_core(0);
    while (cardq::service()) {
    }
    host_card_core(3);
  }
  std::string txt = slurp();
  CHECK(txt.find("out=pre\n") != std::string::npos && txt.find("postgain=2500\n") != std::string::npos);
  loop::reset();
  CHECK(loop::out() == loop::kOutPost && loop::postgain() == 0.0f);
  loop::load();
  CHECK(loop::out() == loop::kOutPre && std::fabs(loop::postgain() - 2.5f) < 1e-4f && !loop::save());
  // the console and the status
  CHECK(cmd("loop=out:post").find("out post") != std::string::npos && loop::out() == loop::kOutPost && loop::dirty());
  CHECK(cmd("loop=out:pre").find("out pre") != std::string::npos && loop::out() == loop::kOutPre);
  CHECK(cmd("loop=out:sideways").find("out:post|pre") != std::string::npos && loop::out() == loop::kOutPre);
  CHECK(cmd("loop").find("out pre") != std::string::npos);
  { char j[1200];
    CHECK(loop::json(j, sizeof j) > 0 && strstr(j, "\"out\":\"pre\"") && strstr(j, "\"postgain\":2500")); }
  hooks.to_rytm = nullptr;
  host_fake_time(false, 0);
  printf("  out: the gain cal, loop.txt out and postgain, the console ok\n");
}

int main() {
  test_ring();
  test_ring_gap_cost();
  test_grabs();
  test_store();
  test_playback();
  test_resample_stop_mute();
  test_cal_console();
  test_out();
  test_limiter();
  test_envelope();
  test_out_switch();
  test_out_cal_settings();
  test_resample_wait();
  test_room_without_drops();
  test_wait_edges();
  test_cost();
  printf("test_loop: OK\n");
  return 0;
}
