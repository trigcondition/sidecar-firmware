// core/tests/test_slot_kits_clock.cpp - 3(c) steps 9 and 10 (docs/device-profiles.md "Several devices"): each box's kits and
// MIDI clock are its own. With the Rytm on transport 0 and the A4 on transport 1 (slot 1):
//   clock  each transport's realtime bytes (engine_realtime_link) reach only its own slot: its LFOs' clock (run, position,
//          tempo, PLAY's retrig), its send FX's STUTTER grid (fx::control_of(slot).clock); the Rytm's alone as before.
//          A lone A4 on transport 0 (its hello moved the transport to slot 1) clocks slot 1, not the Rytm's.
//   kits   a box's 3B moves only its own per-kit modules (the LFOs' kit); a kit report (4C) in a slot's Scope waits in
//          that slot's queue and writes its record into that box's folder; one box's project change makes only its
//          own reports stale.
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "bundle/kits.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "lfo.h"
#include "pool/pool.h"
#include "store/store.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;

static void put(const char* path, const char* text) {
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  fputs(text, f);
  fclose(f);
}
static bool exists(const char* path) {
  FILE* f = fopen(host_path(path).c_str(), "rb");
  if (f) fclose(f);
  return f != nullptr;
}
static void settle() {
  idle_flush(true);
  for (int i = 0; i < 4000; ++i) {
    device::step();
    idle_prepare();
    idle_save();
    idle_stream();
    bool loading = project_loading();
    for (int k = 1; k < kVoiceSlots; ++k) {
      device::Scope in(device::g_links[k]);
      loading = loading || project_loading();
    }
    if (i > 50 && idle_settled() && !loading) break;
  }
  idle_flush(false);
}
static void hello_a4(int transport) {
  const uint8_t h[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                       (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
  engine_sysex_link(transport, h, (int)sizeof h);
}
static void kit3b(int transport, int kit) {   // a box's active kit (its remote UI's 3B)
  const uint8_t m[] = {0xf0, 0x7d, 0x52, 0x54, 0x3B, 1, (uint8_t)kit, 0xf7};
  engine_sysex_link(transport, m, (int)sizeof m);
}
static void kit4c(int op, int kit) {   // a SAVE / RELOAD KIT report, in the Scope the caller opened
  const uint8_t m[] = {0xf0, 0x7d, 0x52, 0x54, 0x4C, 1, (uint8_t)op, (uint8_t)((kit + 1) >> 7), (uint8_t)((kit + 1) & 0x7f),
                       (uint8_t)((kit + 1) >> 7), (uint8_t)((kit + 1) & 0x7f), 0xf7};
  kits::sysex(m, (int)sizeof m);
}
static void ticks(int t, int n, uint32_t gap_us) {   // n clock ticks gap_us apart on transport t
  for (int i = 0; i < n; ++i) {
    g_us += gap_us;
    host_fake_time(true, g_us);
    CHECK(engine_realtime_link(t, 0xF8, 0) >= 0);
  }
}
static lfo::ClockState cs(int slot) {
  lfo::ClockState c;
  lfo::clock_state(slot, &c);
  return c;
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_slot_kits_clock: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-slotkc-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  device::rytm_on_usb(true);
  settle();
  hello_a4(1);
  settle();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[0]);
  CHECK(!strcmp(prj(0).key, "RYP") && !strcmp(prj(1).key, "A4P"));

  // ---- the MIDI clock: each transport's bytes its own slot's
  const uint32_t w0 = fx::control.clock, w1 = fx::control1.clock;
  CHECK(engine_realtime_link(1, 0xFA, 0) == 1);   // the A4 starts: slot 1's PLAY only
  ticks(1, 24, 25000);                             // 100 BPM on the A4
  lfo::ClockState a = cs(1), r = cs(0);
  printf("  A4 clock: run %d pos %d tick %.0f us starts %u; Rytm's: run %d ticking %d starts %u\n", a.run, (int)a.pos,
         a.tick_us, (unsigned)a.starts, r.run, r.ticking, (unsigned)r.starts);
  CHECK(a.run && a.ticking && a.pos == 23 && a.starts == 1 && a.tick_us > 24000 && a.tick_us < 26000);
  CHECK(!r.run && !r.ticking && r.pos == -1 && r.starts == 0);
  CHECK(fx::control1.clock != w1 && fx::control.clock == w0);   // the send FX grid: the A4's own
  // the Rytm starts at 125 BPM: its own clock, the A4's runs on unchanged
  CHECK(engine_realtime_link(0, 0xFA, 0) == 0);
  ticks(0, 48, 20000);
  a = cs(1), r = cs(0);
  printf("  Rytm clock: run %d pos %d tick %.0f us; A4's pos %d tick %.0f us\n", r.run, (int)r.pos, r.tick_us, (int)a.pos,
         a.tick_us);
  CHECK(r.run && r.pos == 47 && r.starts == 1 && r.tick_us > 19000 && r.tick_us < 21000);
  CHECK(a.pos == 23 && a.tick_us > 24000 && a.tick_us < 26000 && a.starts == 1);
  CHECK(fx::control.clock != w0);
  // the A4 stops and sets a song position: the Rytm plays on where it was
  CHECK(engine_realtime_link(1, 0xFC, 0) == 1 && engine_realtime_link(1, 0xF2, 8) == 1);
  a = cs(1), r = cs(0);
  CHECK(!a.run && a.stops == 1 && a.jumps == 1 && a.pos == 8 * 6 - 1);
  CHECK(r.run && r.stops == 0 && r.jumps == 0 && r.pos == 47);
  // the slot in hand's clock_state is its own (a user machine's transport in that box's pass)
  {
    device::Scope in(device::g_links[1]);
    lfo::ClockState h;
    lfo::clock_state(&h);
    CHECK(!h.run && h.pos == 8 * 6 - 1);
  }
  float tp, tus;
  lfo::clock_now(1, &tp, &tus);
  CHECK(tus > 24000 && tus < 26000);
  lfo::clock_now(0, &tp, &tus);
  CHECK(tus > 19000 && tus < 21000);

  // ---- kits: a box's 3B is its own
  kit3b(0, 4);   // the Rytm on kit 5
  kit3b(1, 9);   // the A4 on kit 10
  settle();
  {
    device::Scope in(device::g_links[0]);
    CHECK(lfo::kit() == 4);
  }
  {
    device::Scope in(device::g_links[1]);
    CHECK(lfo::kit() == 9);
  }
  // a kit SAVE in each box's Scope: each record into its own folder
  {
    device::Scope in(device::g_links[1]);
    kit4c(kits::kSave, 9);
  }
  settle();
  CHECK(kits::stats(1).saves == 1 && kits::stats(0).saves == 0);
  CHECK(exists("/.a4/kits/A4P/k010.sks") && !exists("/.rytm/kits/A4P/k010.sks") && !exists("/.rytm/kits/RYP/k010.sks"));
  {
    device::Scope in(device::g_links[0]);
    kit4c(kits::kSave, 4);
  }
  settle();
  CHECK(kits::stats(0).saves == 1 && kits::stats(1).saves == 1);
  CHECK(exists("/.rytm/kits/RYP/k005.sks") && !exists("/.a4/kits/A4P/k005.sks"));
  // one box's project change makes only its own waiting reports stale: the Rytm's queue holds a SAVE while the A4's
  // project changes; the A4's SAVE after the change and the Rytm's both land
  {
    device::Scope in(device::g_links[0]);
    kit4c(kits::kSave, 5);
  }
  {
    device::Scope in(device::g_links[1]);
    kit4c(kits::kSave, 11);
    kits::project_changing();   // (the A4's 10 with another name)
    kit4c(kits::kSave, 12);
  }
  settle();
  printf("  kits: Rytm saves %u stale %u; A4 saves %u stale %u\n", (unsigned)kits::stats(0).saves,
         (unsigned)kits::stats(0).stale, (unsigned)kits::stats(1).saves, (unsigned)kits::stats(1).stale);
  CHECK(kits::stats(0).saves == 2 && kits::stats(0).stale == 0 && exists("/.rytm/kits/RYP/k006.sks"));
  CHECK(kits::stats(1).saves == 2 && kits::stats(1).stale == 1);
  CHECK(!exists("/.a4/kits/A4P/k012.sks") && exists("/.a4/kits/A4P/k013.sks"));

  // ---- a lone A4 on transport 0 (no Rytm, the A4's hello on the host link): attach order, the first attached is slot 0,
  // its clock slot 0's; nothing reaches slot 1
  device::reset();
  lfo::reset();
  device::set_out(1, out1);
  hello_a4(0);
  settle();
  CHECK(device::slot_of(0, nullptr, 0) == 0 && device::slot_held_by(&device::kA4) == 0 && device::rytm_slot() < 0);
  CHECK(engine_realtime_link(0, 0xFA, 0) == 0);
  CHECK(engine_realtime_link(1, 0xFA, 0) == -1);   // (transport 1: no box, nowhere)
  ticks(0, 3, 20000);
  CHECK(cs(0).run && cs(0).pos == 2 && !cs(1).run && cs(1).pos == -1);

  printf("test_slot_kits_clock: PASS\n");
  return 0;
}
