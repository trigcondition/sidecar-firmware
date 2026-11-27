// core/tests/test_projload_cost.cpp - what the control core spends at a project load (notes of the 2026-11-20 black box: every
// project load held the control pass 10.3 ms, 24 ms at 1 GHz, with ~400 output underflows; the FIFO holds 2 ms).
// The project switch has two parts on the control core: the Rytm's 10 (set_project: the midi section of a pass) and the
// RAM store fill that follows it (idle_save's "fill" step, pool.h project_fill_step), whose last step loads the LFOs, inserts,
// scenes, XY pad and the sequencer from the store. Card time is the host's card model (the card core does the reads); this
// measures the CPU of each control-core call, on the thread's own CPU clock, scaled to the Pi as the other stall tests do
// (the Mac's x 4.4, tests/timing.h). The fixture is a project of 128 patterns on 4 tracks with notes and 26 LFOs.
//
// Found with it (the first version of this test printed the steps): project_fill_begin, in the pass of the switch (the rename /
// slot step that makes it, or the Rytm's 10), wanted ~1000 paths and checked each against all the ones before - 7.3 ms on the
// Pi here, the 10.3 ms of the black box. store::want now finds a repeat by a hash (core/store/store.cpp). Checked: that call
// and every control-core call of a whole switch, both ways, under the FIFO (2 ms), and no card time on the control core.
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "check.h"
#include "timing.h"
#include "../core.h"
#include "../engine.h"
#include "../fmt/fmt.h"
#include "../fx/insert.h"
#include "../host/core_port.h"
#include "../lfo.h"
#include "../mod/scene.h"
#include "../mod/xypad.h"
#include "../msamp/cardq.h"
#include "../seq/seq.h"
#include "../store/store.h"

namespace cardq = core::cardq;

static std::string g_card;
constexpr uint64_t kFifoUs = 2000;   // 96 frames at 48 kHz: a longer control pass is silence
static uint64_t g_us = 1000000;
static void card_turn() {
  core::host_card_core(0);
  while (cardq::service()) {
  }
  core::host_card_core(3);
}
static void noop_out(const uint8_t*, int) {}
static void sx(const std::vector<uint8_t>& m) { core::engine_sysex(m.data(), (int)m.size()); }
static std::vector<uint8_t> project_msg(const char* name) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x10};
  for (const char* c = name; *c; ++c) m.push_back((uint8_t)*c);
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> state(std::vector<uint8_t> body) {   // 2D <1> body: the sequencer's state messages
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static void pass() {
  core::host_fake_time(true, g_us += 20000);
  core::idle_save();
  card_turn();
}
static void settle() {
  for (int i = 0; i < 20000 && core::project_loading(); ++i) pass();
  for (int i = 0; i < 4000 && (core::fmt::post_busy() || core::lfo::dirty() || core::ins::settings_dirty(nullptr) || core::seq::unsaved()); ++i) pass();
}

// The Pi's figure for a span of Mac CPU time (the boot bench's PLAITS ratio, as test_loadstall)
static uint64_t pi_us(uint64_t mac_us) { return mac_us * 44 / 10; }

struct Cost {
  uint64_t sysex_us = 0;       // the Rytm's 10, engine_sysex
  uint64_t worst_idle_us = 0;  // the worst idle_save call of the fill
  uint64_t worst_card3 = 0;    // the most card time one idle call spent on the control core (the card model)
  uint32_t worst_call = 0;     // which call that was
  const char* worst_name = "";   // its slowest named step
  uint32_t idle_calls = 0;
  uint64_t total_us = 0;
};

// One project switch to `name`, timed: the 10, then idle_save (the fill is its "fill" step) with the card core's turn between,
// until the store is filled and the LFOs, inserts, scenes and sequences are loaded. The clock is the real one while it runs,
// so core::IdleSlow (engine.h: the slowest named step of a pass) works.
static Cost switch_cost(const char* name) {
  Cost c;
  core::host_fake_time(false, 0);
  core::idle_slowest_take();
  uint64_t t0 = timing_cpu_us();
  sx(project_msg(name));
  c.sysex_us = pi_us(timing_cpu_us() - t0);
  c.total_us = c.sysex_us;
  for (int i = 0; i < 20000 && (strcmp(core::prj().key, name) || core::project_loading()); ++i) {
    uint64_t t = timing_cpu_us(), c3 = core::host_card_us(3);
    core::idle_save();
    uint64_t d = pi_us(timing_cpu_us() - t) + (core::host_card_us(3) - c3);
    if (core::host_card_us(3) - c3 > c.worst_card3) c.worst_card3 = core::host_card_us(3) - c3;
    core::IdleSlow s = core::idle_slowest_take();
    ++c.idle_calls;
    c.total_us += d;
    if (d > c.worst_idle_us) c.worst_idle_us = d, c.worst_call = c.idle_calls, c.worst_name = s.name[0] ? s.name : "(none)";
    card_turn();
  }
  core::host_fake_time(true, g_us += 20000);
  return c;
}

int main() {
  char tmpl[] = "/tmp/projload-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  core::host_set_card(g_card.c_str());
  core::host_fake_time(true, g_us);
  core::hooks.to_rytm = noop_out;
  CHECK(core::engine_begin());
  cardq::set_worker(true);
  core::host_card_model(true);

  // the fixture: a project PB of 128 patterns, notes on 4 tracks, 26 LFOs and an insert; and an empty PA to come from
  sx(project_msg("PB"));
  settle();
  char o[200];
  for (int i = 1; i <= 26; ++i) {
    char c[48];
    snprintf(c, sizeof c, "lfo=%d:dep:%d", i, 10 + i);
    CHECK(core::lfo::command(c, o, sizeof o));
  }
  for (int r = 1; r <= 4; ++r) {
    char c[40];
    snprintf(c, sizeof c, "seq=track:%d:on", r);
    CHECK(core::seq::command(c, o, sizeof o));
  }
  for (int p = 0; p < 128; ++p) {
    sx(state({2, (uint8_t)p, 0x7F}));   // pattern p now
    for (int pass_n = 0; pass_n < 6; ++pass_n) pass();
    for (int r = 0; r < 4; ++r) {
      core::seq::Track* t = core::seq::edit_track(r, true);
      CHECK(t);
      t->length = 16;
      for (int s = 0; s < 16; s += 2) core::seq::add_note(t->step[s], {(uint8_t)(36 + r + s), 100, 48, 0, 0});
    }
    core::seq::touched();
    g_us += 3000000;   // (the edit settles, then idle_save writes it)
    for (int pass_n = 0; pass_n < 12; ++pass_n) pass();
  }
  settle();
  // PA has files of its own (a project with none, named while PB has some, makes the Rytm's rename question - rename.h)
  CHECK(system(("mkdir -p '" + g_card + "/.rytm/seq/PA' && cp '" + g_card + "/.rytm/lfo/PB.txt' '" + g_card + "/.rytm/lfo/PA.txt' && cp '" + g_card + "/.rytm/seq/PB/p000.sqp' '" + g_card + "/.rytm/seq/PA/p000.sqp'").c_str()) == 0);
  sx(project_msg("PA"));
  for (int i = 0; i < 400 && strcmp(core::prj().key, "PA"); ++i) pass();
  settle();
  CHECK(!strcmp(core::prj().key, "PA"));

  // the first step of every load: the wanted paths (project_fill_begin), one call in one pass
  CHECK(timing_retry("project_fill_begin", [] {
    uint64_t t = timing_cpu_us();
    core::project_fill_begin();
    uint64_t us = pi_us(timing_cpu_us() - t);
    printf("projload: project_fill_begin: %llu us on the Pi\n", (unsigned long long)us);
    settle();
    return us < kFifoUs;
  }));
  Cost c = switch_cost("PB");
  printf("projload: the Rytm's 10: %llu us on the Pi; the fill: %u calls, worst %llu us (call %u, %s), total %llu us\n",
         (unsigned long long)c.sysex_us, c.idle_calls, (unsigned long long)c.worst_idle_us, c.worst_call, c.worst_name, (unsigned long long)c.total_us);
  CHECK(c.worst_card3 == 0 && c.worst_idle_us < kFifoUs && c.sysex_us < kFifoUs);
  c = switch_cost("PA");
  printf("projload: and back: the Rytm's 10: %llu us; the fill: %u calls, worst %llu us (call %u, %s), total %llu us\n",
         (unsigned long long)c.sysex_us, c.idle_calls, (unsigned long long)c.worst_idle_us, c.worst_call, c.worst_name, (unsigned long long)c.total_us);
  CHECK(c.worst_card3 == 0 && c.worst_idle_us < kFifoUs && c.sysex_us < kFifoUs);
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_projload_cost: ok\n");
  return 0;
}
