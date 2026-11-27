// core/tests/test_fx_guard.cpp - the measured-time guard of every effect (core/fx/fx_guard.h, decided 2026-11-23): an
// INSERT slot or a send FX slot over its budget for 16 blocks on a hot core is muted, a cool core never mutes one, one
// call past 2 ms mutes at once, a core hot 16 blocks in a row mutes its dearest effect (a user effect too, through
// um.cpp's hook), the mute fades the slot out (dry; the send FX's return silent), shows NO CPU, holds while the core
// cools and through the admission's regrant, and is released when the slot is set again or its box attaches again.
// /status's "fxMuted" lists it. The render cores' timing is driven here with fxguard=slow: (a busy wait inside the
// slot's call) and core_done called by hand.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core.h"
#include "fx/fx_guard.h"
#include "fx/insert.h"
#include "fx/send_fx.h"

using namespace core;
namespace g = core::fx::guard;

static const float kSr = 48000.0f;
static std::vector<float> g_mem((size_t)ins::kLines * ins::kLine);
static float g_x[12];

static void fresh_ins(ins::Engine& e) {
  std::fill(g_mem.begin(), g_mem.end(), 0.0f);
  e.Place((float(*)[ins::kLine])g_mem.data());
  e.Reset();
  memset((void*)&ins::control, 0, sizeof ins::control);
}
static std::string cmd(const char* c) {
  char out[600] = {0};
  CHECK(g::command(c, out, sizeof out));
  return out;
}
// one 12-frame block of chain c (mono) on core 1, then the core's verdict
static float block(ins::Engine& e, int c, bool hot, float amp = 0.25f, bool done = true, bool late = false) {
  static int ph = 0;
  for (int i = 0; i < 12; ++i, ++ph) g_x[i] = amp * std::sin(2 * M_PI * 440.0 * ph / kSr);
  float x[12];
  memcpy(x, g_x, sizeof x);
  e.ProcessMono(c, x, 12, ins::control);
  if (done) g::core_done(1, hot, late);
  float d = 0;   // how far from dry
  for (int i = 0; i < 12; ++i) d = std::max(d, std::fabs(x[i] - g_x[i]));
  return d;
}
static void set(int c, int s, int t, std::vector<int> p = {100, 100, 64, 64}) {
  ins::control.type[c][s] = (uint8_t)t;
  for (int k = 0; k < 4; ++k) ins::control.play[c][s][k] = (uint8_t)p[k];
}

int main() {
  g::set_core(1);
  g::set_clock([] { return 0u; });   // only fxguard=slow: time counts (a CI runner's preemption read as a 2 ms huge block)
  // ---- a slot over its budget: muted after 16 hot blocks, never on a cool core
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive);
    cmd("fxguard=slow:0:V1:A:300");
    for (int b = 0; b < 200; ++b) block(e, 0, false);
    CHECK(!g::ins_muted(0, 0, 0));   // cool: never
    for (int b = 0; b < 15; ++b) block(e, 0, true);
    CHECK(!g::ins_muted(0, 0, 0));
    block(e, 0, false);              // a cool block in between: the count starts again
    for (int b = 0; b < 15; ++b) block(e, 0, true);
    CHECK(!g::ins_muted(0, 0, 0));
    block(e, 0, true);
    CHECK(g::ins_muted(0, 0, 0));
    g::poll();
    printf("test_fx_guard: slow V1 A: %s\n", g::why(g::ins_unit(0, 0, 0)));
    CHECK(strstr(g::why(g::ins_unit(0, 0, 0)), "over its budget"));
    CHECK(ins::refused(0, 0));       // the box shows NO CPU
    CHECK(g::muted_count(0) == 1 && g::muted_count(1) == 0);
    // it fades out (5 ms) and plays dry; then the chain is idle
    cmd("fxguard=slow:0:V1:A:0");
    float d = 0;
    for (int b = 0; b < 40; ++b) d = block(e, 0, false);
    CHECK(d < 1e-6f);
    CHECK(!e.Active(0, ins::control));
    // held: a cool core, the admission's regrant
    for (int b = 0; b < 400; ++b) block(e, 0, false);
    ins::regrant();
    g::poll();
    CHECK(g::ins_muted(0, 0, 0));
    // json
    char j[2048];
    g::json(j, sizeof j);
    printf("test_fx_guard: %s\n", j);
    CHECK(strstr(j, "\"at\":\"V1 A\"") && strstr(j, "\"type\":\"DRIVE\"") && strstr(j, "\"user\":false"));
    // released when its TYPE changes (the kit, the page): it plays the new one
    set(0, 0, ins::kFilter, {40, 0, 0, 0});
    g::poll();
    CHECK(!g::ins_muted(0, 0, 0) && !ins::refused(0, 0));
    for (int b = 0; b < 40; ++b) d = block(e, 0, false);
    CHECK(d > 1e-3f);
    printf("test_fx_guard: slow slot muted on a hot core only, faded to dry, held, released on a new TYPE\n");
  }
  // ---- one slot over its budget mutes that slot only: the dearest rule waits while it fades (kSettle, LT12 prep)
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive), set(1, 0, ins::kDrive);
    cmd("fxguard=slow:0:V1:A:180");
    cmd("fxguard=slow:0:V2:A:40");
    for (int b = 0; b < 16; ++b) block(e, 0, true, 0.25f, false), block(e, 1, true);
    CHECK(g::ins_muted(0, 0, 0) && !g::ins_muted(0, 1, 0));
    for (int b = 0; b < 30; ++b) block(e, 0, true, 0.25f, false), block(e, 1, true);   // (still hot, within the settle)
    CHECK(!g::ins_muted(0, 1, 0) && g::stats().dearest == 0);
    printf("test_fx_guard: an over-budget slot mutes alone; the dearest rule waits while it fades\n");
  }
  // ---- a late block (the core past a whole block): the slot over its budget at once, a 1 ms fade (LT12b, 2026-11-24)
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive), set(1, 0, ins::kDrive);
    for (int b = 0; b < 40; ++b) block(e, 0, false, 0.25f, false), block(e, 1, false);   // (both faded in)
    cmd("fxguard=slow:0:V1:A:180");
    cmd("fxguard=slow:0:V2:A:40");
    block(e, 0, true, 0.25f, false), block(e, 1, true, 0.25f, true, true);   // one late block
    CHECK(g::ins_muted(0, 0, 0) && !g::ins_muted(0, 1, 0) && g::stats().late == 1);
    CHECK(g::short_fade(g::ins_unit(0, 0, 0)));
    g::poll();
    printf("test_fx_guard: late V1 A: %s\n", g::why(g::ins_unit(0, 0, 0)));
    CHECK(strstr(g::why(g::ins_unit(0, 0, 0)), "made core 1 late"));
    // out in 1 ms (48 frames: 4 blocks), the slot still costing its time; the core still late meanwhile takes nothing else
    float d = 1;
    for (int b = 0; b < 5; ++b) d = block(e, 0, true, 0.25f, false), block(e, 1, true, 0.25f, true, true);
    CHECK(d < 1e-6f);
    CHECK(!g::ins_muted(0, 1, 0) && g::stats().late == 1);
    // released (set again): it fades back in over the TYPE's 5 ms, not 1 ms
    cmd("fxguard=slow:0:V1:A:0");
    set(0, 0, ins::kFilter, {40, 0, 0, 0});
    g::poll();
    CHECK(!g::ins_muted(0, 0, 0) && !g::short_fade(g::ins_unit(0, 0, 0)));
    printf("test_fx_guard: a late block mutes the slot over its budget at once, out in 1 ms, alone\n");
  }
  // ---- a late core with no effect over its budget: its dearest after 4 late blocks in a row, never after one
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive), set(1, 0, ins::kDrive);
    cmd("fxguard=slow:0:V1:A:30");
    cmd("fxguard=slow:0:V2:A:90");
    for (int b = 0; b < 3; ++b) block(e, 0, true, 0.25f, false), block(e, 1, true, 0.25f, true, true);
    block(e, 0, true, 0.25f, false), block(e, 1, true);   // one on time: the run starts again
    for (int b = 0; b < 3; ++b) block(e, 0, true, 0.25f, false), block(e, 1, true, 0.25f, true, true);
    CHECK(!g::ins_muted(0, 0, 0) && !g::ins_muted(0, 1, 0) && g::stats().late == 0);
    block(e, 0, true, 0.25f, false), block(e, 1, true, 0.25f, true, true);
    CHECK(!g::ins_muted(0, 0, 0) && g::ins_muted(0, 1, 0) && g::stats().late == 1);
    printf("test_fx_guard: a late core's dearest after 4 late blocks in a row, not after one\n");
    CHECK(g::fifo_late());   // the FIFO's lateness (the Pi's cores.cpp): on by default, fxguard=fifo:off|on
    CHECK(cmd("fxguard=fifo:off").find("off") != std::string::npos && !g::fifo_late());
    CHECK(cmd("fxguard=fifo:on").find(": on") != std::string::npos && g::fifo_late());
  }
  // ---- two boxes take turns on a core, one box's effects a block (LT12j, 2026-11-25): a late core's dearest over the
  // last blocks, not the block's own, so a cheap effect in the 4th late block is not taken for a costly one in the 3rd
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive), set(1, 0, ins::kDrive);
    cmd("fxguard=slow:0:V1:A:2");
    cmd("fxguard=slow:0:V2:A:80");
    for (int b = 0; b < 2; ++b) block(e, 1, true, 0.25f, true, true), block(e, 0, true, 0.25f, true, true);
    CHECK(!g::ins_muted(0, 0, 0) && g::ins_muted(0, 1, 0) && g::stats().late == 1);
    printf("test_fx_guard: a late core's dearest over the last blocks, not the 4th block's own\n");
  }
  // ---- a late core whose effects are all near free (LT12k, 2026-11-25): nothing muted, under the 8 us floor
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive), set(1, 0, ins::kDrive);
    cmd("fxguard=slow:0:V1:A:2");
    cmd("fxguard=slow:0:V2:A:4");
    // (not hot: the hot core's dearest rule is its own)
    for (int b = 0; b < 64; ++b) block(e, 0, false, 0.25f, false), block(e, 1, false, 0.25f, true, true);
    CHECK(!g::ins_muted(0, 0, 0) && !g::ins_muted(0, 1, 0) && g::stats().late == 0);
    cmd("fxguard=slow:0:V2:A:12");   // one at 12 us a block: muted
    for (int b = 0; b < 32; ++b) block(e, 0, false, 0.25f, false), block(e, 1, false, 0.25f, true, true);
    CHECK(!g::ins_muted(0, 0, 0) && g::ins_muted(0, 1, 0) && g::stats().late == 1);
    printf("test_fx_guard: a late core's near-free effects stay (8 us floor), a 12 us one goes\n");
  }
  // ---- one call past 2 ms: at once, hot or not
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(9, 1, ins::kComp);   // bus 2, slot B
    cmd("fxguard=slow:0:B2:B:2100");
    float x[16][2] = {};
    e.ProcessStereo(9, x, 12, ins::control);
    g::core_done(1, false);
    CHECK(g::ins_muted(0, 9, 1));
    g::poll();
    printf("test_fx_guard: huge B2 B: %s\n", g::why(g::ins_unit(0, 9, 1)));
    CHECK(strstr(g::why(g::ins_unit(0, 9, 1)), "in one block"));
    CHECK(g::stats().huge == 1);
  }
  // ---- a hot core: its dearest effect after 16 blocks, each under its own budget
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive), set(1, 0, ins::kDrive);
    cmd("fxguard=slow:0:V1:A:30");
    cmd("fxguard=slow:0:V2:A:70");
    for (int b = 0; b < 15; ++b) block(e, 0, true, 0.25f, false), block(e, 1, true);   // (one core_done a block)
    CHECK(!g::ins_muted(0, 0, 0) && !g::ins_muted(0, 1, 0));
    block(e, 0, true, 0.25f, false), block(e, 1, true);
    CHECK(!g::ins_muted(0, 0, 0) && g::ins_muted(0, 1, 0));   // V2 A: the dearest
    g::poll();
    printf("test_fx_guard: dearest V2 A: %s\n", g::why(g::ins_unit(0, 1, 0)));
    CHECK(strstr(g::why(g::ins_unit(0, 1, 0)), "dearest on core 1"));
    for (int b = 0; b < 30; ++b) block(e, 0, true, 0.25f, false), block(e, 1, true);   // V2 A fading, the core still hot
    CHECK(!g::ins_muted(0, 0, 0) && g::stats().dearest == 1);   // V1 A not taken with it
    // set again with the same TYPE (set_type, keep: false): released
    ins::set_type(1, 0, ins::kDrive, false);
    CHECK(!g::ins_muted(0, 1, 0));
    // the settle's last 2 blocks, then the next 16 hot blocks take the next dearest
    for (int b = 0; b < 2 + 16; ++b) block(e, 0, true);
    CHECK(g::ins_muted(0, 0, 0));
    // a box attaching again releases its slot's mutes
    g::clear_box(0);
    CHECK(!g::ins_muted(0, 0, 0) && g::muted_count(0) == 0);
    printf("test_fx_guard: a hot core mutes its dearest effect; set again / attach releases it\n");
  }
  // ---- a user effect as the dearest: um.cpp's hook
  {
    g::reset();
    static int s_u = -1, s_core = -1;
    static uint32_t s_us = 0;
    g::user_mute = [](int u, uint32_t us, int c) { s_u = u, s_us = us, s_core = c; };
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive);
    cmd("fxguard=slow:0:V1:A:20");
    for (int b = 0; b < 16; ++b) {
      g::note_user(5, 90);   // box 1's UFX2, 90 us a block
      block(e, 0, true);
    }
    CHECK(s_u == 5 && s_core == 1 && s_us == 90);
    CHECK(!g::ins_muted(0, 0, 0));
    g::user_mute = nullptr;
    printf("test_fx_guard: a user effect picked as the dearest goes to um's mute\n");
  }
  // ---- report mode: nothing muted; off: nothing either
  {
    g::reset();
    ins::Engine e(kSr);
    fresh_ins(e);
    set(0, 0, ins::kDrive);
    cmd("fxguard=report");
    cmd("fxguard=slow:0:V1:A:2100");
    block(e, 0, true);
    CHECK(!g::ins_muted(0, 0, 0));
    g::poll();
    cmd("fxguard=off");
    block(e, 0, true);
    CHECK(!g::ins_muted(0, 0, 0));
    printf("test_fx_guard: report and off mute nothing\n");
  }
  // ---- the send FX: a built-in TYPE over its budget, its return silent; released on a new TYPE
  {
    g::reset();
    static fx::SendFx sfx(kSr);
    sfx.Reset();
    memset((void*)&fx::control, 0, sizeof fx::control);
    fx::control.knob[0][0] = fx::kChorus;
    fx::control.knob[0][2] = 100, fx::control.knob[0][3] = 100, fx::control.knob[0][4] = 100, fx::control.knob[0][5] = 100;
    for (int t = 0; t < fx::kTracks; ++t) fx::control.send[t][0] = 127;
    float st[12][fx::kStems], o[12][2];
    int ph = 0;
    auto run = [&](bool hot) {
      for (int i = 0; i < 12; ++i, ++ph) {
        for (int v = 0; v < fx::kStems; ++v) st[i][v] = 0.0f;
        st[i][0] = 0.25f * std::sin(2 * M_PI * 220.0 * ph / kSr);
      }
      sfx.Process(st, 12, o, fx::control);
      g::core_done(1, hot);
      float p = 0;
      for (int i = 0; i < 12; ++i) p = std::max(p, std::max(std::fabs(o[i][0]), std::fabs(o[i][1])));
      return p;
    };
    float p = 0;
    for (int b = 0; b < 400; ++b) p = std::max(p, run(false));
    CHECK(p > 1e-3f);   // CHORUS returns
    cmd("fxguard=slow:0:FX1:300");
    for (int b = 0; b < 16; ++b) run(true);
    CHECK(g::send_muted(0, 0));
    g::poll();
    printf("test_fx_guard: send FX1: %s\n", g::why(g::send_unit(0, 0)));
    cmd("fxguard=slow:0:FX1:0");
    for (int b = 0; b < 40; ++b) run(false);
    p = 0;
    for (int b = 0; b < 40; ++b) p = std::max(p, run(false));
    CHECK(p < 1e-6f);   // silent
    char j[1024];
    g::json(j, sizeof j);
    CHECK(strstr(j, "\"at\":\"FX1\"") && strstr(j, "CHORUS"));
    fx::control.knob[0][0] = fx::kComb;   // another TYPE: released, it plays
    g::poll();
    CHECK(!g::send_muted(0, 0));
    p = 0;
    for (int b = 0; b < 800; ++b) p = std::max(p, run(false));
    CHECK(p > 1e-3f);
    printf("test_fx_guard: send FX slot muted (silent return), released on a new TYPE\n");
  }
  printf("test_fx_guard: PASS\n");
  return 0;
}
