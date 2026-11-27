// core/tests/test_loop_closed.cpp - the looper against the real sequencer clock in a closed loop (plan Task 26's check, on the
// Mac without the Rytm emulator): a simulated Rytm plays 4-on-the-floor at 127 BPM (its step ticks stamped in 32-frame
// blocks, arriving late and jittery as test_seq's), its MAIN comes back to the host as the Overbridge IN stream, and the host's
// loop (the real core/loop with the real seq::beat_map) is rendered into the OUT stream, which the Rytm adds to MAIN after a
// round trip. Checked: a loop's kick lands within 1 ms of the live kick (the first grab, and a resample of loop + live), the
// resample holds loop + live (the ring never stops). The grab's slice and the loop's start come from the same bar-line estimate,
// so the sequencer clock's own error (the jitter's bias, up to ~1 ms) cancels in what the Rytm plays.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "core.h"
#include "hooks.h"
#include "loop/loop.h"
#include "seq/seq.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;
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
struct Msg {
  int64_t at;
  std::vector<uint8_t> m;
};
static std::vector<Msg> g_wire;
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

// ---- the simulated Rytm
static const double kPeriod = 48000.0 * 60.0 / 127.0 / 4.0;   // a 16th at 127 BPM (5669.29 frames)
static const int64_t kT0 = 48000;                              // the first tick, Rytm frame
static const int kTickDelay = 700;                             // a tick's USB arrival (the sequencer follows the fastest)
static int g_post = 0;                                          // the POST path's extra delay beyond the voice path (frames)
static int g_tick_jit = 96;                                    // + 0..jit frames of jitter (a scenario's)
static const int kX = 300, kDIn = 700, kAudioJit = 12;         // host render -> Rytm hears: kX; Rytm -> host IN: kDIn (+ jitter)
static const int kRtt = kX + kDIn;                              // the round trip the sequencer's latency stands for
static const double kBar = 16 * kPeriod;
static std::vector<int32_t> g_ring, g_ret;                      // the host's ring; the loop's return into the Rytm (per host frame)
static std::vector<uint8_t> g_store;
static std::vector<float> g_live, g_loopret;                    // for the analysis: per Rytm frame, the live kick and the loop's return
static int64_t g_next_k = 0;

static double kick_at(int64_t r) {   // the live kick: 4-on-the-floor, a 24-frame click on every 4th step from kT0
  double k = (double)(r - kT0) / (4 * kPeriod);
  if (r < kT0 || k < 0) return 0;
  double d = (double)(r - kT0) - std::floor(k) * 4 * kPeriod;
  return d >= 0 && d < 24 ? 5.0e8 * std::sin(3.14159265 * (d + 1) / 25) : 0;
}

// one render block of 12 frames: the Rytm's ticks that arrived, the engine's block, the OUT hook, the IN audio that arrived
static void block() {
  int64_t h = (int64_t)seq::frames();
  for (;;) {   // ticks fired up to now on the Rytm (the follower sees them kTickDelay + jitter later)
    int64_t tf = kT0 + (int64_t)llround((double)g_next_k * kPeriod);
    if (tf > h + 2) break;
    int step = (int)(g_next_k % 16);
    auto b = blk4(tf);
    b.insert(b.end(), {1, 0, (uint8_t)step, 16, 2});
    g_wire.push_back({tf + kTickDelay + jitter(g_tick_jit), msg(0x2C, b)});
    ++g_next_k;
  }
  deliver(h);
  seq::pre_block();
  g_us += 250;
  host_fake_time(true, g_us);
  if (((h / 12) & 15) == 0) seq::tick(now_ms());
  loop::tick(0);
  // OUT: what the loop adds to POST on this block (frames h..h+11), kept to come back through the Rytm
  int32_t F[12][12] = {};
  loop::out_mix(F, 12, (uint64_t)h);
  for (int f = 0; f < 12; ++f) g_ret[(size_t)h + f] = F[f][0];
  // IN: Rytm time r = host arrival frame - kDIn: the live kick plus the loop's return from kX ago
  int32_t lr[24];
  int jit = jitter(kAudioJit);
  for (int f = 0; f < 12; ++f) {
    int64_t hin = h + f, r = hin - kDIn;
    double live = kick_at(r);
    int64_t ho = r - kX - g_post;   // the host frame whose loop output the Rytm hears at r
    double lp = ho >= 0 && ho < (int64_t)g_ret.size() && ho < h ? (double)g_ret[(size_t)ho] : 0;
    if (r >= 0) g_live[(size_t)r] = (float)live, g_loopret[(size_t)r] = (float)lp;
    int32_t v = (int32_t)(live + lp);
    lr[2 * f] = v, lr[2 * f + 1] = -v;
  }
  loop::in_block(lr, 12, (uint64_t)h + 12 + (uint64_t)jit, 0);
}
static void run_frames(int64_t n) {
  int64_t end = (int64_t)seq::frames() + n;
  while ((int64_t)seq::frames() < end) block();
}
// onsets (first sample past the threshold after a quiet stretch) of a Rytm-time signal, in frames
static std::vector<int64_t> onsets(const std::vector<float>& x, int64_t from, int64_t to, float thr) {
  std::vector<int64_t> o;
  int64_t quiet = 1000;
  for (int64_t i = from; i < to && i < (int64_t)x.size(); ++i) {
    if (std::fabs(x[(size_t)i]) > thr && quiet >= 1000) o.push_back(i), quiet = 0;
    else ++quiet;
  }
  return o;
}
// the worst distance of the loop's onsets (Rytm time) in [from, to) from the nearest live kick
static double worst_offset(int64_t from, int64_t to, int* count) {
  std::vector<int64_t> lo = onsets(g_loopret, from, to, 1.0e8f);
  double worst = 0;
  for (int64_t t : lo) {
    double k = std::round((double)(t - kT0) / (4 * kPeriod));
    double d = (double)(t - kT0) - k * 4 * kPeriod;
    if (std::fabs(d) > worst) worst = std::fabs(d);
  }
  *count = (int)lo.size();
  return worst;
}

// one run with a tick jitter of `jit` frames (0 .. jit, arriving after the fastest): the limit the loop's kicks may be from the
// live ones, in frames (48 = 1 ms)
static void scenario(int jit, double limit, int post = 0, int applied = 0) {
  g_tick_jit = jit;
  g_post = post;
  seq::reset();
  g_wire.clear(), g_next_k = 0, g_rng = 1;
  printf("  -- tick jitter 0..%d frames (%.1f ms), limit %.0f frames\n", jit, jit / 48.0, limit);
  g_live.assign(1 << 23, 0), g_loopret.assign(1 << 23, 0), g_ret.assign(1 << 23, 0);
  g_ring.assign(2 * (size_t)(1 << 21), 0);   // 43 s
  g_store.assign(48u << 20, 0);
  loop::init(g_ring.data(), 1u << 21, g_store.data(), g_store.size());   // (the real clock: seq::beat_map, seq::frames)
  loop::reset();
  loop::set_post_lat(applied);
  seq::set_latency(kRtt);
  {   // 2D 07: the tempo the Rytm reports (BPM x 120, three 7-bit bytes), as test_seq's
    int v = 127 * 120;
    auto m = msg(0x2D, {7, (uint8_t)(v >> 14), (uint8_t)(v >> 7 & 0x7F), (uint8_t)(v & 0x7F)});
    engine_sysex(m.data(), (int)m.size());
  }
  // the Rytm plays: PLAY and the first tick at Rytm frame kT0 (a 2D state 01 01 with its block stamp)
  while ((int64_t)seq::frames() < 40000) block();
  {
    auto b = std::vector<uint8_t>{1, 1};
    auto x = blk4(kT0);
    b.insert(b.end(), x.begin(), x.end());
    g_wire.push_back({kT0 + kTickDelay, msg(0x2D, b)});
  }
  const int bars = 6;
  run_frames((int64_t)(kT0 + bars * kBar) - (int64_t)seq::frames());   // six bars of live kicks
  CHECK(seq::view().transport == seq::kPlay);
  double o, p16;
  int lat;
  CHECK(seq::beat_map(&o, &p16, &lat) && std::fabs(p16 - kPeriod) < 2.0 && lat == kRtt);

  // ---- a 1-bar grab pressed a third into a bar
  int64_t press = (int64_t)(kT0 + (bars + 0.3) * kBar);
  run_frames(press - (int64_t)seq::frames());
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_frames(400);
  CHECK(loop::grabs() == 1 && loop::stats().copies <= 1);
  const loop::Grab* g = loop::grab_at(0);
  run_frames((int64_t)(3.5 * kBar));   // the loop starts at the next bar line, plays; the ring keeps hearing loop + live
  CHECK(g->copied);
  // the grab starts on the bar line, so its kick is at its first sample (or, taken a hair late, at its very end): the kick
  // nearest the bar line, in frames from it (+ late, - early), within 1 ms
  int bl = 1 << 30;
  for (uint32_t i = 0; i < 4800; ++i)
    if (std::abs((int64_t)g->pcm[2 * i]) > 1.0e8) { bl = (int)i - 12; break; }   // (the click peaks 12 frames in)
  for (uint32_t i = g->frames - 4800; i < g->frames && bl == (1 << 30); ++i)
    if (std::abs((int64_t)g->pcm[2 * i]) > 1.0e8) { bl = (int)i - 12 - (int)g->frames; break; }
  printf("  the grab: %u frames, its bar line's kick %d frames from its start (%.2f ms)\n", g->frames, bl, bl / 48.0);
  // (informational: a bar-line estimate off by tens of frames under jitter shifts the grab's slice and the loop's start alike -
  // the kicks' return, checked below, stays on the beat)
  int n1;
  int64_t from = (int64_t)(kT0 + (bars + 2) * kBar), to = (int64_t)(kT0 + (bars + 4) * kBar);
  double w1 = worst_offset(from, to, &n1);
  printf("  the loop's kicks vs the live ones: %d onsets, worst %.1f frames (%.2f ms)\n", n1, w1, w1 / 48.0);
  CHECK(n1 >= 6 && w1 < limit);   // within 1 ms of the live kick (spec 3.4's goal)

  // ---- resample: a grab while the loop plays takes loop + live at once and swaps in at the next bar line in phase
  float pk_first = 0;
  for (uint32_t i = 0; i < g->frames; ++i) pk_first = std::fmax(pk_first, (float)std::abs(g->pcm[2 * i]));
  int64_t press2 = (int64_t)seq::frames();
  CHECK(loop::state() == loop::kPlaying);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_frames(400);
  CHECK(loop::grabs() == 2 && loop::stats().resamples == 1);
  const loop::Grab* g2 = loop::grab_at(0);
  run_frames((int64_t)(3.5 * kBar));
  float pk2 = 0;
  for (uint32_t i = 0; i < g2->frames; ++i) pk2 = std::fmax(pk2, (float)std::abs(g2->pcm[2 * i]));
  printf("  the resample: peak %.2e against the first grab's %.2e (loop + live: about twice)\n", pk2, pk_first);
  CHECK(pk2 > 1.6f * pk_first && pk2 < 2.4f * pk_first);
  int n2;
  int64_t from2 = press2 + (int64_t)(2.5 * kBar), to2 = press2 + (int64_t)(4 * kBar);
  double w2 = worst_offset(from2, to2, &n2);
  printf("  after the swap: %d onsets, worst %.1f frames (%.2f ms)\n", n2, w2, w2 / 48.0);
  CHECK(n2 >= 4 && w2 < limit);
}

int main() {
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  char tmpl[] = "/tmp/core_loopc_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  CHECK(engine_begin());
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'L', 'O', 'O', 'P', 'T', 0xF7};
  engine_sysex(proj, sizeof proj);
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  scenario(96, 48);    // the USB path's jitter as measured on the Pi (a few ms at worst): within 1 ms
  scenario(96, 48, 300, 300);   // a POST path 300 frames slower than the voice path, its post_lat (loop=cal) applied: on the beat
  scenario(400, 48);   // test_seq's heavy jitter (8 ms uniform): the bar line itself is ~1 ms off there, the loop's return still not
  hooks.to_rytm = nullptr;
  host_fake_time(false, 0);
  printf("test_loop_closed: OK\n");
  return 0;
}
