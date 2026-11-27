// core/tests/test_two_slots.cpp - sixteen voices, two boxes (docs/device-profiles.md "Several devices"): the Rytm in slot 0
// plays engine voices 0..7, a box in slot 1 (here an A4 on the second transport) its voice k on engine voice 8 + k. The
// same report on both renders the same samples on voice k and 8 + k; a machine, a note-off or a link reset on one slot
// leaves the other's voice alone; the base box (the one output stage, the pages) is slot 1's while the box is alone.
// On an 8-voice engine (the Teensy's, -DCORE_VOICES=8) every link plays 0..7 and there is nothing to test.
#include <cmath>
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "host_link.h"
#include "pool/pool.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }
static void send(int transport, const std::vector<uint8_t>& m) { engine_sysex_link(transport, m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
// a 0x04 trig on the box's voice k with the machine m: a tonal PLAITS sound (or JUNO's), P1..P16
static std::vector<uint8_t> trig(int m, int k, int note) {
  static const uint8_t pl[16] = {93, 64, 0, 0, 127, 0, 64, 127, 64, 64, 0, 64, 64, 0, 0, 100};
  std::vector<uint8_t> t = {0x04, (uint8_t)(m | k), (uint8_t)note, 100};
  for (int i = 0; i < 16; ++i) t.push_back(pl[i]), t.push_back(0);
  return sidecar(t);
}

static constexpr int kRows = kBlock * 400;
static int16_t ring[kRows][kVoices];
static void render(int blocks = 400) {
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < blocks; ++b) render_voices(ring, kRows, b * kBlock, 0, kVoices, mach);
}
static double rms(int vi) {
  double s = 0;
  for (int i = 0; i < kRows; ++i) s += (double)ring[i][vi] * ring[i][vi];
  return std::sqrt(s / kRows);
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_two_slots: PASS (an 8-voice engine: every link plays 0..7, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-twoslots-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples"));
  hooks.to_rytm = out0;
  card_ok = cards.begin(log, shared, sizeof shared);
  CHECK(card_ok);
  junolib.begin(&cards.sd(), log);
  voices_init();
  device::set_out(1, out1);
  device::rytm_on_usb(true);   // a Rytm on USB: slot 0 stays the base while a box joins

  // the helpers: slot 0's voice k is k, slot 1's 8 + k; a voice byte from either link
  CHECK(device::voice_base(device::g_links[0]) == 0 && device::voice_base(device::g_links[1]) == kBoxVoices);
  CHECK(device::voice_in(0x08 | 3, device::g_links[1]) == 11 && device::voice_in(0x08 | 3, device::g_links[0]) == 3);
  CHECK(box_voice(11) == 3 && slot_of_voice(11) == 1 && slot_of_voice(7) == 0 && voice_wrap(19) == 3);

  // the Rytm plays voice 3 before the A4 says hello: the A4's arrival silences none of the Rytm's voices
  send(0, trig(kPlaits, 3, 60));
  CHECK(v[3].live && !v[11].live);
  render(40);
  const uint8_t hello[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                           (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
  send(1, std::vector<uint8_t>(hello, hello + sizeof hello));
  for (int k = 0; k < 20; ++k) device::step();
  CHECK(device::g_links[1].box == &device::kA4 && !device::g_links[0].box);
  CHECK(!g_out1.empty() && g_out1.back()[8] == device::kOk);   // its hello answered on its own transport
  CHECK(device::base_voice_base() == 0);                       // the Rytm on USB: the base box is the Rytm
  render();
  CHECK(rms(3) > 100);   // still sounding (the slot 1 link reset was slot 1's voices only)

  // the same report on both slots: voice 3 and voice 11 from fresh, sample for sample
  voices_init();
  send(0, trig(kPlaits, 3, 60));
  send(1, trig(kPlaits, 3, 60));
  CHECK(v[3].live && v[11].live);
  for (int vi = 0; vi < kVoices; ++vi) CHECK(vi == 3 || vi == 11 || !v[vi].live);
  memset(ring, 0, sizeof ring);
  render();
  CHECK(rms(3) > 100);
  for (int i = 0; i < kRows; ++i) CHECK(ring[i][3] == ring[i][11]);
  for (int vi = 0; vi < kVoices; ++vi) CHECK(vi == 3 || vi == 11 || rms(vi) == 0);

  // a machine on one slot is that slot's: JUNO on the A4's voice 3, the Rytm's voice 3 stays PLAITS
  send(1, trig(kJuno, 3, 64));
  CHECK(v[11].juno && !v[3].juno && v[3].live);
  send(0, trig(kPlaits, 3, 62));
  CHECK(v[11].juno && !v[3].juno);

  // a note-off (0x11) on one slot is that slot's: JUNO on both voice 3s, the A4's gate closes, its voice 11 releases while
  // the Rytm's voice 3 holds
  voices_init();
  send(0, trig(kJuno, 3, 60));
  send(1, trig(kJuno, 3, 60));
  CHECK(v[3].juno && v[11].juno);
  render(100);
  auto late = [](int vi) {   // the last quarter of the ring
    double e = 0;
    for (int i = 3 * kRows / 4; i < kRows; ++i) e += (double)ring[i][vi] * ring[i][vi];
    return std::sqrt(e / (kRows / 4));
  };
  send(1, sidecar({0x11, 3}));
  memset(ring, 0, sizeof ring);
  render();
  printf("  JUNO after the A4's 0x11: voice 3 %.1f, voice 11 %.1f (the last quarter)\n", late(3), late(11));
  CHECK(late(3) > 100 && late(11) < 0.25 * late(3));
  send(0, sidecar({0x11, 3}));

  // the A4's voice 4 does not exist (4 voices): dropped, nothing plays on 12
  send(1, trig(kPlaits, 4, 60));
  CHECK(!v[12].live);

  // a link reset on slot 1 silences 8..15 only: the Rytm's voice 3 rings on, the A4's goes quiet
  voices_init();
  send(0, trig(kPlaits, 3, 60));
  send(1, trig(kPlaits, 3, 60));
  render(40);
  link_reset("test", 1);
  memset(ring, 0, sizeof ring);
  render();
  CHECK(rms(3) > 100);
  double tail = 0;
  for (int i = kRows / 2; i < kRows; ++i) tail += std::fabs((double)ring[i][11]);
  CHECK(tail == 0);
  // and the Rytm's own (a 35 hello, slot 0's): slot 1's voice plays on
  send(1, trig(kPlaits, 3, 60));
  render(40);
  send(0, sidecar({0x35, 1, 0, 100, 3}));
  memset(ring, 0, sizeof ring);
  render();
  CHECK(rms(11) > 100);
  tail = 0;
  for (int i = kRows / 2; i < kRows; ++i) tail += std::fabs((double)ring[i][3]);
  CHECK(tail == 0);

  // each slot's INSERT and OUT settings are its own, alone or beside a Rytm (control_of(slot)). The same trig on voice 3
  // and 11 with a TREMOLO on slot 0's chain 3: voice 3 only; on slot 1's chain 3: voice 11 only. AMP the same
  CHECK(device::side_slot() == 1 && device::on_side(11) && !device::on_side(3) && out::g_base_slot == 0);
  CHECK(&out::control_of(1) == &out::control1 && out::control1.route[3] == out::kTrk && out::control1.lev[3] == 100);
  auto same_3_11 = [] {
    voices_init();
    send(0, trig(kPlaits, 3, 60));
    send(1, trig(kPlaits, 3, 60));
    memset(ring, 0, sizeof ring);
    render();
    CHECK(rms(3) > 100 && rms(11) > 100);
    for (int i = 0; i < kRows; ++i)
      if (ring[i][3] != ring[i][11]) return false;
    return true;
  };
  auto trem = [](ins::Control& k) {   // chain 3's slot 0: TREMOLO, full depth, as insert.cpp's settings would play it
    k.type[3][0] = ins::kTrem;
    for (int p = 0; p < ins::kParams; ++p) k.play[3][0][p] = 127;
  };
  ins::reset();
  CHECK(same_3_11());   // both slots' chains off
  ins::set_type(3, 0, ins::kTrem);
  ins::set_param(3, 0, 1, 127);
  ins::set_param(3, 0, 0, 127);
  CHECK(!same_3_11());
  const double plain = rms(11);
  CHECK(std::fabs(rms(3) - plain) > 100);   // slot 0's TREMOLO on voice 3
  ins::reset();
  trem(ins::control1);
  CHECK(!same_3_11());
  const double slot1_trem = rms(11);
  printf("  slot 1's TREMOLO: voice 11 %.1f, voice 3 %.1f (plain %.1f)\n", slot1_trem, rms(3), plain);
  CHECK(std::fabs(rms(3) - plain) < 1e-6 && ins::control.type[3][0] == ins::kOff);
  ins::reset();
  // AMP plays on either box's chains (decided 2026-11-26: slot 0 and slot 1 the same; before, slot 1's AMP was dry beside
  // the Rytm): slot 1's on voice 11 alone, slot 0's on voice 3 alone, the same samples either way
  ins::control1.type[3][0] = ins::kAmp;
  for (int p = 0; p < ins::kParams; ++p) ins::control1.play[3][0][p] = 20;   // (LEV / TONE away from unity: audible with or without a capture)
  CHECK(!same_3_11());
  const double amp11 = rms(11);
  printf("  slot 1's AMP: voice 11 %.1f, voice 3 %.1f (plain %.1f)\n", amp11, rms(3), plain);
  CHECK(std::fabs(rms(3) - plain) < 1e-6 && std::fabs(amp11 - plain) > 100);
  ins::reset();
  ins::control.type[3][0] = ins::kAmp;
  for (int p = 0; p < ins::kParams; ++p) ins::control.play[3][0][p] = 20;
  CHECK(!same_3_11());
  CHECK(std::fabs(rms(11) - plain) < 1e-6 && std::fabs(rms(3) - amp11) < 1e-3);
  ins::reset();

  // the Rytm leaves (its USB gone): attach order, the A4 stays in slot 1 and is the base box now, its tracks the pages'
  // voices 8..15; nothing more comes on its transport (a message there would be the Rytm back)
  device::rytm_on_usb(false);
  device::step();
  CHECK(device::g_base == &device::g_links[1] && device::base_voice_base() == kBoxVoices && !device::held(0));
  CHECK(device::route_of(0) < 0 && device::route_of(1) == 1);
  CHECK(device::base_voice(2) == 10 && device::base_box_voice(10) == 2 && device::base_box_voice(2) == -1);
  CHECK(ui::remote_track_voice(0) == kBoxVoices + 0);   // track 1 (BD's place): the base box's voice 0
  CHECK(device::side_slot() == -1 && out::g_base_slot == 1);   // alone: AMP on its chains
  auto solo11 = [] {   // the A4's voice 3 alone
    voices_init();
    send(1, trig(kPlaits, 3, 60));
    memset(ring, 0, sizeof ring);
    render();
    return rms(11);
  };
  // still slot 1's own chains: slot 1's TREMOLO on voice 11 as beside the Rytm
  trem(ins::control1);
  CHECK(std::fabs(solo11() - slot1_trem) < 1e-6);
  ins::reset();
  {
    device::Scope in(device::g_links[0]);   // (3(c): the settings are the slot in hand's: slot 0's here, free)
    ins::set_type(3, 0, ins::kTrem);
    ins::set_param(3, 0, 1, 127);
    ins::set_param(3, 0, 0, 127);
  }
  CHECK(std::fabs(solo11() - plain) < 1e-6);
  ins::reset();
  // outside a Scope the slot in hand is the lone A4's: its settings go into its own chains (control1)
  ins::set_type(3, 0, ins::kTrem);
  ins::set_param(3, 0, 1, 127);
  ins::set_param(3, 0, 0, 127);
  CHECK(ins::control1.type[3][0] == ins::kTrem && ins::control.type[3][0] == ins::kOff);
  CHECK(std::fabs(solo11() - plain) > 100);
  ins::reset();
  // the Rytm back (its USB again): the lowest free slot, 0 (its own, saved), the base again; the A4 stays in slot 1
  device::rytm_on_usb(true);
  device::step();
  CHECK(device::base_voice_base() == 0 && ui::remote_track_voice(0) == 0);
  CHECK(device::rytm_slot() == 0 && device::slot_held_by(&device::kA4) == 1 && device::route_of(1) == 1);

  printf("test_two_slots: PASS (slot 0 on 0..7, slot 1 on 8..15: the same report the same samples, machines, "
         "note-offs and link resets each slot's own, the base box's voices follow g_base, each slot's INSERT chains its own "
         "alone or paired)\n");
  return 0;
}
