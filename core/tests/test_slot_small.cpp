// core/tests/test_slot_small.cpp - 3(c): the last small pieces each box has of its own (docs/device-profiles.md "Several
// devices"). With the Rytm on transport 0 and the A4 on transport 1 (slot 1):
//   arp        each slot its own arpeggiator: its tracks' codes (the slot in hand's), a box's track t on its own voice
//              (kBoxVoices + t), its notes on its own slot's tempo and frames in its own pass, its transport's STOP
//              ending only its own voices' sequencer notes
//   transport  every slot's sequencer transport reaches hooks.transport_box with its slot; hooks.transport the Rytm's only
//   all sound  a link's reset clears its own box's send FX (fx::control_of(slot).kill), not the other's
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "arp/arp.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "store/store.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;
static std::vector<std::pair<int, int>> g_tr;   // (slot, status) from hooks.transport_box
static std::vector<int> g_tr0;                  // hooks.transport's

static void put(const char* path, const char* text) {
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  fputs(text, f);
  fclose(f);
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
static void transport(int t, int state) {   // a box's 2D TRANSPORT (0 stop, 1 play, 2 pause) at its frame 0
  const uint8_t m[] = {0xf0, 0x7d, 0x52, 0x54, 0x2D, 1, 1, (uint8_t)state, 0, 0, 0, 0, 0xf7};
  engine_sysex_link(t, m, (int)sizeof m);
}
static void blocks(int n) {   // n render blocks of every running slot (one pass: no side stage here)
  for (int i = 0; i < n; ++i) {
    g_us += 1000000ull * kBlock / 48000;
    host_fake_time(true, g_us);
    seq::pre_block_all();
  }
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_slot_small: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-slotsm-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  hooks.to_rytm = out0;
  hooks.transport = [](uint8_t s) { g_tr0.push_back(s); };
  hooks.transport_box = [](int slot, uint8_t s) { g_tr.emplace_back(slot, s); };
  CHECK(engine_begin());
  device::set_out(1, out1);
  device::rytm_on_usb(true);
  settle();
  hello_a4(1);
  settle();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[0]);
  const int a4v = kBoxVoices;   // the A4's track 1 plays on the engine's voice 8

  // ---- arp codes: the slot in hand's; a box's track on its own voice
  arp::Cfg up = arp::decode(0);
  up.mode = arp::kUp;
  {
    device::Scope in(device::g_links[1]);
    arp::set(0, up);
    CHECK(arp::get(0).mode == arp::kUp);
  }
  CHECK(arp::get(0).mode == arp::kOff);   // (outside a Scope: the Rytm's)
  CHECK(arp::on(a4v) && !arp::on(0) && !arp::on(a4v + 1));
  printf("  arp: the A4's track 1 UP on voice %d; the Rytm's track 1 %s\n", a4v, arp::mode_name(arp::get(0).mode));

  // ---- its notes on slot 1's own clock (stopped: free, 120 BPM a 1/16 = 6000 frames) in slot 1's own pass
  const uint64_t f0 = seq::frames(0), f1 = seq::frames(1);
  const uint32_t n0 = arp::stats(0).notes, n1 = arp::stats(1).notes;
  CHECK(arp::live_on(a4v, 60, 100, 0, 60) && arp::live_on(a4v, 64, 100, 0, 64));
  blocks(48000 / kBlock);   // one second
  const uint32_t d1 = arp::stats(1).notes - n1, d0 = arp::stats(0).notes - n0;
  printf("  arp: a second: the A4's %u notes, the Rytm's %u; frames slot 0 +%llu, slot 1 +%llu\n", (unsigned)d1, (unsigned)d0,
         (unsigned long long)(seq::frames(0) - f0), (unsigned long long)(seq::frames(1) - f1));
  CHECK(d1 >= 7 && d1 <= 9 && d0 == 0);
  CHECK(seq::frames(1) - f1 == seq::frames(0) - f0);
  CHECK(seq::tempo(1) == 0 && seq::tempo(0) == 0);
  CHECK(arp::live_off(a4v, 0, 60) && arp::live_off(a4v, 0, 64));
  blocks(8);
  CHECK(!arp::busy(a4v));

  // ---- transports: each slot's reaches transport_box with its slot; transport (the Rytm's) slot 0's only. A STOP
  // of one slot ends only its own voices' sequencer notes held by the arp
  CHECK(arp::seq_on(a4v, 62, 100, seq::PolyCfg{}));
  CHECK(arp::busy(a4v));
  g_tr.clear(), g_tr0.clear();
  transport(0, 1);
  transport(0, 0);   // the Rytm: PLAY, STOP
  CHECK(arp::busy(a4v));   // (the A4's held note runs on)
  transport(1, 1);
  transport(1, 0);   // the A4: PLAY, STOP
  blocks(2);
  printf("  transport: %zu box reports (", g_tr.size());
  for (auto& x : g_tr) printf(" %d:%02X", x.first, x.second);
  printf(" ), %zu of the Rytm's; the A4's held seq note %s\n", g_tr0.size(), arp::busy(a4v) ? "on" : "ended");
  const std::vector<std::pair<int, int>> want = {{0, 0xFA}, {0, 0xFC}, {1, 0xFA}, {1, 0xFC}};
  CHECK(g_tr == want);
  CHECK((g_tr0 == std::vector<int>{0xFA, 0xFC}));
  CHECK(!arp::busy(a4v));

  // ---- all sound off: a link's reset clears its own box's send FX only
  {
    const uint8_t k0 = fx::control.kill, k1 = fx::control1.kill;
    g_us += 200000;
    host_fake_time(true, g_us);
    link_reset("test", 1);
    CHECK(fx::control1.kill == (uint8_t)(k1 + 1) && fx::control.kill == k0);
    g_us += 200000;
    host_fake_time(true, g_us);
    link_reset("test", 0);
    CHECK(fx::control.kill == (uint8_t)(k0 + 1) && fx::control1.kill == (uint8_t)(k1 + 1));
    printf("  all sound off: slot 1's reset cleared control1 only, slot 0's control only\n");
  }

  // ---- the Rytm's tracks read its own remote UI's tables (10b: one per slot) whatever slot is in hand
  arp::set(0, up);   // (outside a Scope: the Rytm's track 1, on voice 0)
  CHECK(arp::on(0));
  {
    device::Scope in(device::g_links[1]);
    CHECK(arp::on(0) && arp::on(a4v) && arp::get(0).mode == arp::kUp);
  }
  {
    arp::Cfg off = arp::decode(0);
    device::Scope in(device::g_links[1]);
    arp::set(0, off);   // the A4's track 1 off: the Rytm's stays on
    CHECK(!arp::on(a4v) && arp::on(0));
  }
  printf("  arp: the Rytm's track 1 read from its own tables in the A4's Scope\n");

  hooks.transport = nullptr;
  hooks.transport_box = nullptr;
  printf("test_slot_small: PASS\n");
  return 0;
}
