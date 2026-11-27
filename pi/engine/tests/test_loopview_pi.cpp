// pi/engine/tests/test_loopview_pi.cpp - the display's LOOP page view (loopview_pi.cpp) from the looper (core/loop/loop.h) on
// the core's Mac library: FillView's hash changes with a grab, a loop's start, a setting, the message, and comes back with
// the same state; the grabs' fields; the looper off; core 3's publish and core 0's take. make -C pi/engine/tests (make -C
// core engines lib first).
#include <cstdio>
#include <cstring>
#include <vector>

#include "../loopview_pi.h"
#include "check.h"
#include "loop/loop.h"
#include "platform.h"

using namespace core;

static const uint32_t kRing = 1u << 20;
static uint64_t g_now = 0, g_fed = 0, g_us = 50000000;
static bool env_beat(double* o, double* p, int* l) { *o = 10000, *p = 6000, *l = 384; return true; }
static uint64_t env_frames() { return g_now; }
static uint16_t env_muted() { return 0; }
static void run_to(uint64_t h) {
  while (g_now < h) {
    g_now += 48;
    while (g_fed + 6 + 1000 <= g_now) {
      int32_t b[14] = {};
      loop::in_block(b, 7, g_fed + 6 + 1000, 0);
      g_fed += 7;
    }
    loop::tick(0);
  }
}

int main() {
  host_fake_time(true, g_us);
  static loop::Env env;
  env.beat_map = env_beat, env.frames = env_frames, env.muted = env_muted;
  loop::set_env(&env);
  std::vector<int32_t> ring(2 * (size_t)kRing);
  std::vector<uint8_t> store(16u << 20);
  sidecar_loop_view_t off, a, b, e;
  // the looper off: a view that says so
  loop::init(nullptr, 0, nullptr, 0);
  loopview_pi::FillView(&off, 0);
  CHECK(off.valid && !off.on && off.ngrabs == 0 && off.playing == -1);
  loop::init(ring.data(), kRing, store.data(), store.size());
  loop::reset();
  run_to(10 * 96000);   // ten bars at 120 BPM in
  // fill_view_hash_changes_on_grab
  loopview_pi::FillView(&a, 0);
  CHECK(a.on && a.state == 0 && a.ngrabs == 0 && a.ring_s >= 15 && a.bal == 64 && a.lev == 100 && a.fade == 2 && a.gen != off.gen);
  CHECK(loop::grab(loop::k2) == loop::kOk);
  run_to(g_now + 4800);
  loopview_pi::FillView(&b, 0);
  CHECK(b.gen != a.gen && b.ngrabs == 1 && b.g[0].hb == 4 && b.g[0].bpm10 == 1200 && b.g[0].copied && !b.g[0].kept && b.state == 1);
  CHECK(b.playing == 0 && b.sel == 0 && b.g[0].id == loop::grab_at(0)->id && b.g[0].id != 0);   // the id the commands name
  run_to(g_now + 200000);   // the loop's start passed
  loopview_pi::FillView(&a, 0);
  CHECK(a.state == 2 && a.gen != b.gen && a.bars10 == 20);
  // the same state: the same hash (a redraw only when something drawn changed)
  loopview_pi::FillView(&e, 0);
  CHECK(e.gen == a.gen);
  loop::set_bal(100);
  loopview_pi::FillView(&b, 0);
  CHECK(b.bal == 100 && b.gen != a.gen);
  loop::set_auto_mute(true), loop::set_live_via(4), loop::set_fade(loop::kFade4);
  loopview_pi::FillView(&a, 0);
  CHECK(a.mute == 1 && a.live == 4 && a.fade == 4 && a.gen != b.gen);
  // KEEP's result shows as a message
  char t[40];
  CHECK(loop::keep(0, t, sizeof t));
  loop::KeepJob job;
  CHECK(loop::keep_take(&job));
  loop::keep_finished(job.id, true, "take_9");
  loop::tick(0);   // core 3 applies it
  loopview_pi::FillView(&b, 0);
  CHECK(b.g[0].kept && !strcmp(b.msg, "KEPT take_9") && b.gen != a.gen);
  // a selection past the grabs is held to the last
  loopview_pi::FillView(&e, 5);
  CHECK(e.sel == 0);
  // core 3 publishes while wanted (every 50 ms at most), core 0 takes the newest once
  unsigned seen = 0;
  loopview_pi::Tick();
  CHECK(!loopview_pi::View(&a, &seen));   // not wanted: nothing built
  loopview_pi::Want(true, 0);
  g_us += 60000, host_fake_time(true, g_us);
  loopview_pi::Tick();
  CHECK(loopview_pi::View(&a, &seen) && a.on && a.ngrabs == 1);
  CHECK(!loopview_pi::View(&a, &seen));
  loopview_pi::Want(false, 0);
  loop::init(nullptr, 0, nullptr, 0);
  printf("test_loopview_pi: ok (the hash follows a grab, a start, the settings, the message; off; publish / take)\n");
  return 0;
}
