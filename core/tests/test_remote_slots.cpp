// core/tests/test_remote_slots.cpp - 10b: the remote UI one per voice slot (core/ui/remote.cpp; option A, nothing shared
// between the boxes). A Rytm in slot 0 and an A4 in slot 1 (its hello on the second transport) each open, page and close
// their own screen: one box's open, page, held keys, frames, keepalives, track tables and selection never reach the
// other's link or state. The slot in hand's (a Scope, or the base box's outside one); remote_tick runs each slot in its
// own Scope. The touchscreen follows the box whose page or display report came last (remote_display_slot), and off the
// control core the FX settings' slot is that one (core/pool/pool.cpp fx_slot). A Rytm alone is as before.
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fx/insert.h"
#include "hooks.h"
#include "host_link.h"
#include "pool/pool.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }
static void sx(int transport, std::vector<uint8_t> b) {   // F0 7D 52 54 <b> F7 from transport t's box
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), b.begin(), b.end());
  m.push_back(0xF7);
  engine_sysex_link(transport, m.data(), (int)m.size());
}
static uint32_t g_t = 10000;
static uint64_t g_us = 1000000000;
static void tick(int n = 1) {
  for (int i = 0; i < n; ++i) host_fake_time(true, g_us += 30000), ui::remote_tick(g_t += 30);
}
static int count(const std::vector<std::vector<uint8_t>>& o, uint8_t cmd) {
  int n = 0;
  for (const auto& m : o) n += m.size() > 4 && m[4] == cmd;
  return n;
}
template <class F> static auto in(int slot, F f) -> decltype(f()) {
  device::Scope s(device::g_links[slot]);
  return f();
}
static bool on_page(int slot, const char* t) {
  return in(slot, [&] { return ui::remote_page_title() && !strcmp(ui::remote_page_title(), t); });
}
static bool g_control = true;
static bool control_core() { return g_control; }

int main() {
  if (kVoiceSlots < 2) {
    printf("test_remote_slots: PASS (an 8-voice engine: one slot, one remote UI)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-remoteslots-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples"));
  hooks.to_rytm = out0;
  card_ok = cards.begin(log, shared, sizeof shared);
  CHECK(card_ok);
  voices_init();
  device::set_out(1, out1);
  device::rytm_on_usb(true);
  ui::remote_reset();

  // a Rytm alone: its page opens and frames go to it, as before (outside a Scope the slot in hand is the base box's: 0)
  sx(0, {0x30, 1, 0, 1, 0});   // UP + YES on the Rytm
  CHECK(ui::remote_open() && in(0, [] { return ui::remote_open(); }));
  tick(4);
  CHECK(count(g_out, 0x39) > 0 && g_out1.empty());
  CHECK(ui::remote_display_slot() == 0);

  // the A4 says hello on the second transport: slot 1's own remote UI, closed; the Rytm's stays open
  const uint8_t hello[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                           (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
  engine_sysex_link(1, hello, sizeof hello);
  for (int k = 0; k < 20; ++k) device::step();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[0]);
  CHECK(in(0, [] { return ui::remote_open(); }) && !in(1, [] { return ui::remote_open(); }));
  g_out.clear(), g_out1.clear();
  tick(12);   // (0.36 s: a keepalive on the open one only)
  CHECK(count(g_out, 0x3C) >= 1 && count(g_out1, 0x3C) == 0 && count(g_out1, 0x39) == 0);

  // the A4 opens its own: both open, each its own frames on its own link
  sx(1, {0x30, 1, 0, 1, 0});
  CHECK(in(1, [] { return ui::remote_open(); }) && in(0, [] { return ui::remote_open(); }));
  CHECK(ui::remote_display_slot() == 1);   // the touchscreen follows the box that opened last
  g_out.clear(), g_out1.clear();
  tick(4);
  CHECK(count(g_out1, 0x39) > 0 && count(g_out1, 0x38) > 0);   // the A4's whole first frame
  CHECK(count(g_out, 0x38) == 0);                              // the Rytm's frame did not change: nothing redrawn there

  // a page shown in one slot is that slot's: SCOPE on the Rytm, the A4 stays on SIDECAR
  in(0, [] { ui::remote_show_page("SCOPE"); });
  tick(2);
  CHECK(on_page(0, "SCOPE") && !in(1, [] { return ui::remote_page_title(); }));
  // and the A4's key on its screen moves only its page (RIGHT: SIDECAR -> SYSTEM): the Rytm's SCOPE stays
  sx(1, {0x31, 1, 0, 0x41, 1}), sx(1, {0x31, 1, 0, 0x41, 0});
  tick(2);
  CHECK(on_page(0, "SCOPE") && in(1, [] { return ui::remote_stats().page; }) == 2);
  CHECK(in(0, [] { return ui::remote_stats().page; }) == 3 && in(1, [] { return ui::remote_stats().keys; }) == 2 &&
        in(0, [] { return ui::remote_stats().keys; }) == 0);

  // the A4 closes: the Rytm's page goes on (its keepalives on its link, none on the A4's)
  sx(1, {0x30, 1, 0, 0, 0});
  CHECK(!in(1, [] { return ui::remote_open(); }) && in(0, [] { return ui::remote_open(); }) && on_page(0, "SCOPE"));
  g_out.clear(), g_out1.clear();
  tick(12);
  CHECK(count(g_out, 0x3C) + count(g_out, 0x39) >= 1 && g_out1.empty());

  // the track tables (33) are each box's own: the Rytm's BD on PLAITS, slot 1 has none named
  sx(0, {0x33, 1, 0, 1, 1, 0x00});
  CHECK(in(0, [] { return ui::remote_track_machine(0); }) == 0 && in(1, [] { return ui::remote_track_machine(0); }) == -1);
  // each slot's tracks play its own voices: the Rytm's BD voice 0, slot 1's track 1 voice 8
  CHECK(in(0, [] { return ui::remote_track_voice(0); }) == 0 && in(1, [] { return ui::remote_track_voice(0); }) == kBoxVoices);
  // the selection (49 1 2 <track> ..): the Rytm's, the touchscreen back on slot 0
  sx(0, {0x49, 1, 2, 4, 0, 'L', 'T', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  CHECK(in(0, [] { return ui::remote_selected_track(nullptr); }) == 4);
  CHECK(in(1, [] { return ui::remote_selected_track(nullptr); }) == -1);
  CHECK(ui::remote_display_slot() == 0);

  // off the control core (the Pi's touchscreen): every remote_* reader and the FX settings' slot follow the display
  // slot, whatever the control core has in hand
  hooks.control_core = control_core;
  sx(1, {0x30, 1, 0, 1, 0});   // the A4 opens again: the display follows it
  CHECK(ui::remote_display_slot() == 1);
  g_control = false;
  {
    device::Scope s(device::g_links[0]);   // (the control core's hand on slot 0 meanwhile)
    CHECK(ins::slot_in_hand() == 1 && ui::remote_open() && ui::remote_selected_track(nullptr) == -1);
    CHECK(ui::remote_track_voice(0) == kBoxVoices);
  }
  g_control = true;
  CHECK(in(0, [] { return ins::slot_in_hand(); }) == 0 && in(1, [] { return ins::slot_in_hand(); }) == 1);
  hooks.control_core = nullptr;

  // a reset closes every slot's
  ui::remote_reset();
  CHECK(!in(0, [] { return ui::remote_open(); }) && !in(1, [] { return ui::remote_open(); }));
  CHECK(ui::remote_display_slot() == 0);

  printf("test_remote_slots: PASS (each box its own remote UI: open, page, keys, frames, keepalives, track tables and "
         "selection per slot; the touchscreen and the off-core FX readers follow the last box used)\n");
  return 0;
}
