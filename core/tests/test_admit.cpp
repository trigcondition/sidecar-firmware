// core/tests/test_admit.cpp - CPU admission of a box's voices at attach (core/device/admit.h; design decided 2026-11-21):
// the cost model (voices per render core by each box's split, AMPs counted where they play (admitted when loaded:
// amp_fits, amp_core; decided 2026-11-22), slot 0's chains on core 2 and slot 1's on core 1 (decided 2026-11-22), the clock's
// scale), fit (the
// most voices that keep every render core its voices land on under 80 %), voices= per box type, and in the engine: the
// first box keeps its voices, the second gets what fits and its reports past them are dropped, a box left alone gets its
// count back, readmit after the bench. Synthetic costs: the Pi's come from its boot bench (pi/engine/kernel.cpp LogBench).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/admit.h"
#include "device/device.h"
#include "device/digitakt.h"
#include "engine.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "host_link.h"
#include "voices.h"

using namespace core;

static void model() {
  admit::reset();
  CHECK(!admit::on());
  admit::Load l = {{8, 0}, {true, false}};
  CHECK(admit::fit(1, 8, l) == 8);   // no bench: nothing refused
  CHECK(admit::load_pct().amp == admit::kAmpLoadPct && admit::load_pct().voice == admit::kVoiceLoadPct &&
        admit::load_pct().chain == admit::kChainLoadPct);
  admit::set_load_pct({100, 100, 100});   // the synthetic costs below as they are (the LT3 factors: lt3() below)
  // a voice 10 %, a box's chains 20 % (slot 0's on core 2, slot 1's on core 1), at 1500 MHz; split=4
  admit::set_costs({1500, 1000, 0, 0, 2000});
  CHECK(admit::on() && admit::clock() == 1500);
  CHECK(admit::core_bp(1, l) == 4000 && admit::core_bp(2, l) == 6000 && admit::core_bp(0, l) == 0);
  // a second box of 8: core 1 has its chains (40 + 20 %) and takes two of its 0..3 (80 % exactly: allowed); core 2 none
  CHECK(admit::fit(1, 8, l) == 2);
  // asking for 3 it splits 2 + 1 (split_for): core 1 80 %, core 2 70 %; never more than it asked for
  CHECK(admit::fit(1, 3, l) == 3);
  CHECK(admit::split_for(8) == 4 && admit::split_for(4) == 2 && admit::split_for(3) == 2 && admit::split_for(1) == 1 &&
        admit::split_for(0) == 4);
  {
    admit::Load l2 = {{8, 0}, {true, true}, {8, 8}};   // slot 1's chains count on core 1 only
    CHECK(admit::core_bp(1, l2) == 4000 + 2000 && admit::core_bp(2, l2) == 4000 + 2000);
  }
  // INSERT and AMP: a voice 10 % + 2 %, AMP 5 %; nothing reserved for AMP at attach, only those playing (Load::amps) count
  admit::set_costs({1500, 1000, 200, 500, 2000});
  CHECK(admit::core_bp(1, l) == 4 * 1200 && admit::core_bp(2, l) == 4 * 1200 + 2000);   // 48 % / 68 %
  {
    admit::Load p = l;
    p.amps[1] = 2, p.amps[2] = 1;
    CHECK(admit::core_bp(1, p) == 4 * 1200 + 2 * 500 && admit::core_bp(2, p) == 4 * 1200 + 2000 + 500);
    // core 1: 4 x 12 + 2 x 5 = 58 %, + slot 1's chains 20 % = 78 %: no voice of its fits (90 % with one)
    CHECK(admit::fit(1, 8, p) == 0);
    CHECK(admit::fit(1, 8, l) == 1);   // without those AMPs: core 1 48 + 20 + 12 = 80 %
  }
  // amp_fits: one more AMP on a core within 80 % (68 + 5 = 73 % fits; at 76 % + 5 it does not)
  CHECK(admit::amp_fits(1, l) && admit::amp_fits(2, l));
  {
    admit::Load p = l;
    p.amps[2] = 2;   // core 2: 68 + 10 = 78 %
    CHECK(!admit::amp_fits(2, p) && admit::amp_fits(1, p));
    CHECK(admit::amp_fits(3, p) && admit::amp_fits(0, p));   // core 3 (slot 0's master chain: lt6 below), another core
    admit::set_enforce(false);
    CHECK(admit::amp_fits(2, p));   // admit=report: nothing refused
    admit::set_enforce(true);
  }
  // INSERT types: each at its bench row (type_bp; no row: slot_bp, the worst pair's slot), on its voice's core
  {
    admit::Costs co = admit::costs();
    co.slot_bp = 150, co.type_bp[10] = 400;   // SAT
    admit::set_costs(co);
    CHECK(admit::type_bp(0) == 0 && admit::type_bp(9) == 0 && admit::type_bp(10) == 400 && admit::type_bp(1) == 150);
    admit::Load p = l;
    CHECK(admit::voice_core(0, 3, p) == 1 && admit::voice_core(0, 4, p) == 2 && admit::voice_core(1, 0, p) == 1);
    p.ins[2] = 1000;   // core 2: 68 + 10 = 78 %; a SAT (4 %) is over, an unmeasured type (1.5 %) fits
    CHECK(admit::core_bp(2, p) == 4 * 1200 + 2000 + 1000 && !admit::ins_fits(2, p, 10) && admit::ins_fits(2, p, 1));
    CHECK(admit::ins_fits(1, p, 10) && admit::ins_fits(2, p, 0) && admit::ins_fits(2, p, 9));
    co.slot_bp = 0, co.type_bp[10] = 0;
    admit::set_costs(co);
  }
  // amp_core: the base box's voice rows by its split (an A4's 4: 2 + 2), its master rows on master_core
  {
    admit::Load a4 = {{4, 0}, {true, false}, {4, 0}};
    CHECK(admit::amp_core(0, 10, a4) == 1 && admit::amp_core(1, 10, a4) == 1 && admit::amp_core(2, 10, a4) == 2 &&
          admit::amp_core(3, 10, a4) == 2 && admit::amp_core(8, 10, a4) == 0);
    CHECK(admit::amp_core(10, 10, a4) == admit::master_core(0) && admit::amp_core(12, 10, a4) == admit::master_core(0));
    CHECK(admit::master_core(0) == (kVoiceSlots > 1 ? 3 : 2) && admit::master_core(1) == 1);
    admit::Load b1 = {{0, 8}, {false, true}, {0, 8}};   // slot 1 alone is the base box: split 4, its master on core 1
    CHECK(admit::base_of(b1) == 1 && admit::amp_core(4, 10, b1) == 2 && admit::amp_core(10, 10, b1) == 1);
    // AMP on both boxes (amp_core_of): each box's voice chains by its own split, its master chains on its own master_core
    admit::Load two = {{4, 8}, {true, true}, {4, 8}};   // an A4 in slot 0 (2 + 2), a Rytm in slot 1 (4 + 4)
    CHECK(admit::amp_core_of(0, 1, 10, two) == 1 && admit::amp_core_of(0, 2, 10, two) == 2);
    CHECK(admit::amp_core_of(1, 3, 10, two) == 1 && admit::amp_core_of(1, 4, 10, two) == 2 && admit::amp_core_of(1, 8, 10, two) == 0);
    CHECK(admit::amp_core_of(0, 10, 10, two) == admit::master_core(0) && admit::amp_core_of(1, 11, 10, two) == admit::master_core(1));
  }
  admit::set_costs({1500, 1000, 200, 300, 400});
  {
    admit::Load b1 = {{0, 8}, {false, true}};   // slot 1 alone: its whole chains on core 1, no AMP reserved
    CHECK(admit::core_bp(1, b1) == 4 * 1200 + 400 && admit::core_bp(2, b1) == 4 * 1200);
  }
  // LT1's boot bench (2026-11-22, 1500 MHz): a voice 1.59 + 2.05 %, at the bench's own cost (factors 100). An A4 first (4:
  // 2 + 2), a Rytm second, no AMP playing: all 8 while a box's chains (dry) are at most 58.16 % (each core: 6 x 3.64 + chains
  // <= 80 %: core 2 the A4's chains, core 1 the Rytm's)
  {
    admit::Load a4 = {{4, 0}, {true, false}, {4, 0}};
    admit::set_costs({1500, 159, 205, 401, 5816});
    CHECK(admit::fit(1, 8, a4) == 8);
    a4.amps[2] = 1;   // one of the A4's AMPs playing on core 2 (4.01 %): the first ask wins, the Rytm gets 6 (core 2 76.7 %)
    CHECK(admit::fit(1, 8, a4) == 6);
    a4.amps[2] = 0;
    admit::set_costs({1500, 159, 205, 401, 5817});   // core 1 over at the Rytm's 4th voice there (its chains on core 1)
    CHECK(admit::fit(1, 8, a4) == 3);
  }
  // the clock: 1000 MHz costs 1.5 x the bench's 1500
  admit::set_costs({1500, 1000, 0, 0, 2000});
  admit::set_clock(1000);
  CHECK(admit::clock() == 1000 && admit::core_bp(1, l) == 6000 && admit::core_bp(2, l) == 9000);
  CHECK(admit::fit(1, 8, l) == 0);   // core 1 (60 + 30 %) and core 2 (90 %) are over by the chains alone: none of its voices
  admit::set_clock(1500);
  // split=2: a box's 0..1 on core 1, 2..7 on core 2
  admit::set_split(2);
  admit::Load a4 = {{4, 0}, {true, false}};   // an A4 (4 voices): 2 + 2
  CHECK(admit::core_bp(1, a4) == 2000 && admit::core_bp(2, a4) == 4000);
  CHECK(admit::fit(1, 8, a4) == 6);   // core 1: 2 + 20 + 2 = 40 %; core 2: 2 + 20 % + 4 = 80 %, a 7th is 90 %
  admit::set_split(0);
  CHECK(admit::split() == 1);
  admit::set_split(9);
  CHECK(admit::split() == 7);
  admit::set_split(4);
  // admit=report (the two-box load test): fit unchanged, nothing refused; enforce again
  admit::set_costs({1500, 1000, 0, 0, 2000});
  CHECK(admit::enforce() && admit::admit(1, 8, l) == 2);
  admit::set_enforce(false);
  CHECK(admit::admit(1, 8, l) == 8 && admit::admit(1, 3, l) == 3 && admit::admit(1, 12, l) == 8 && admit::fit(1, 8, l) == 2);
  admit::set_enforce(true);
  CHECK(admit::admit(1, 8, l) == 2);
  // voices=
  CHECK(admit::parse_override("a4:2,dt:6"));
  CHECK(admit::override_for("A4") == 2 && admit::override_for("Digitakt") == 6 && admit::override_for("Rytm") == -1);
  CHECK(admit::parse_override("RYTM:5,digitakt:7"));
  CHECK(admit::override_for("rytm") == 5 && admit::override_for("Digitakt") == 7 && admit::override_for("A4") == 2);
  CHECK(!admit::parse_override("md:3") && !admit::parse_override("a4:9") && !admit::parse_override("a4") &&
        !admit::parse_override("a4:x") && !admit::parse_override("a4:3;dt:2"));
  CHECK(admit::override_for("A4") == 2);   // (a refused text changes nothing)
  admit::reset();
  CHECK(admit::override_for("A4") == -1);
}

static std::vector<std::vector<uint8_t>> g_out, g_out1;
// LT3's boot bench (2026-11-22, chains-core1 64fdd7b, 1500 MHz: a voice 1.57 + 2.05 %, AMP 4.02 %, the box row 30.28 %) at
// the LT3 load factors (admit.h; a voice 4.89 %, AMP 12.06 %, a box's chains 33.31 %). Reserving an AMP on every base-box
// voice and its master pair on core 2 (2ee233c) left a Rytm or Digitakt base box over even alone (core 2 125 %). With AMPs
// admitted when loaded and slot 0's master chain on core 3 (decided 2026-11-22; design/rebalance-core2/MODEL.md): Rytm+A4
// 4/4, Rytm+Digitakt 8/8, A4+Rytm 8/8. AMPs then fit where there is room: beside two 8-voice boxes none (core 1 72.4 %,
// core 2 72.4 %: + 12.06 is over), beside a Rytm base + an A4 one on each core.
static void lt3() {
  admit::reset();
  admit::set_costs({1500, 157, 205, 402, 3028});
  admit::set_clock(1500);
  admit::set_load_pct({135, 300, 110});   // LT3's factors (AMP 500 since LT4: lt4() below)
  admit::Load r = {{8, 0}, {true, false}, {8, 0}}, a = {{4, 0}, {true, false}, {4, 0}};
  CHECK(admit::core_bp(2, r) == 5286 && admit::core_bp(1, r) == 1955);
  CHECK(admit::fit(1, 4, r) == 4 && admit::fit(1, 8, r) == 8);
  CHECK(admit::core_bp(1, a) == 978 && admit::core_bp(2, a) == 4309);
  CHECK(admit::fit(1, 8, a) == 8);
  admit::Load rd = {{8, 8}, {true, true}, {8, 8}};   // Rytm + Digitakt, all 8 each: 72.4 % a core, no AMP room
  CHECK(admit::core_bp(1, rd) == 7241 && admit::core_bp(2, rd) == 7241);
  CHECK(!admit::amp_fits(1, rd) && !admit::amp_fits(2, rd));
  admit::Load ra = {{8, 4}, {true, true}, {8, 4}};   // Rytm + A4: one AMP on each core (core 2 62.6 %, + 12.06 = 74.7 %)
  CHECK(admit::amp_fits(1, ra) && admit::amp_fits(2, ra));
  ra.amps[1] = ra.amps[2] = 1;
  CHECK(!admit::amp_fits(1, ra) && !admit::amp_fits(2, ra));
  admit::Load b = {{4, 8}, {true, true}, {4, 8}};   // order 1 config B as run (A4 base, 4 voice AMPs: 2 a core): core 1 87 %
  b.amps[1] = b.amps[2] = 2;
  CHECK(admit::core_bp(1, b) == 8675);
  admit::reset();
}

// LT4 (2026-11-22, rebalance-core2 81b96b4): one voice AMP beside a Rytm base box + an A4 under the full INSERT load cost
// ~19-20 points of core 1 (c1 with chains 78.8 % against core1Bp 70.07), so AMP counts at 500 % (admit.h): 4.02 % -> 20.1 %.
// Beside two boxes at the full load (every voice's two INSERT slots set) no voice AMP fits on either core any more.
static void lt4() {
  admit::reset();
  CHECK(admit::load_pct().amp == 500);
  admit::set_costs({1500, 157, 205, 402, 3028});
  admit::set_clock(1500);
  admit::Load ra = {{8, 4}, {true, true}, {8, 4}};   // Rytm + A4: core 2 62.6 % + 20.1 = 82.7 %: no AMP
  CHECK(!admit::amp_fits(1, ra) && !admit::amp_fits(2, ra));
  admit::Load r = {{8, 0}, {true, false}, {8, 0}};   // a Rytm alone: room on core 1 (19.6 % + 20.1)
  CHECK(admit::amp_fits(1, r));
  r.amps[1] = 2;
  CHECK(admit::core_bp(1, r) == 1955 + 2 * 2010 && admit::amp_fits(1, r));
  admit::reset();
}

// LT6 (2026-11-23, A4 base + Rytm, full load): slot 0's master AMP pair on core 3 at 1500 MHz played clean, at 1200 it
// made core 3's passes 10-19 ms and the engine underran. Core 3 is counted for AMP and the master's INSERT types: control
// 13.4 % + the master chain 8 % at 1500, each AMP 20.1 % (4.02 x 500 %), within 70 %.
static void lt6() {
  admit::reset();
  admit::set_costs({1500, 157, 205, 402, 3028});
  admit::set_clock(1500);
  admit::Load l = {{4, 8}, {true, true}, {4, 8}};
  CHECK(admit::core3_bp(l) == (kVoiceSlots > 1 ? 2140u : 0u));
  if (kVoiceSlots > 1) {
    CHECK(admit::amp_fits(3, l));
    l.amps[3] = 1;
    CHECK(admit::core3_bp(l) == 4150 && admit::amp_fits(3, l));   // the pair at 1500: 61.6 %
    l.amps[3] = 2;
    CHECK(admit::core3_bp(l) == 6160 && !admit::amp_fits(3, l));   // (a third would be 81.7 %)
    admit::set_clock(1200);                                        // 1200: the pair is 77 %, over 70 %
    CHECK(admit::core3_bp(l) == 7700 && admit::core3_bp(l) > admit::kC3LimitBp);
    l.amps[3] = 1;
    CHECK(admit::core3_bp(l) == 5188 && !admit::amp_fits(3, l));   // one plays, the second does not fit
    // the master's INSERT types on core 3 (stereo: twice the mono row): a SAT at 0.9 % a slot fits beside one AMP
    admit::Costs co = admit::costs();
    co.type_bp[ins::kSat] = 90;
    admit::set_costs(co);
    CHECK(admit::ins3_fits(l, ins::kSat));
    l.amps[3] = 2;
    CHECK(!admit::ins3_fits(l, ins::kSat) && admit::ins3_fits(l, ins::kOff));
    // the master's send TYPEs on core 3 at their own rows (bench sfxins "T x4" / 4, stereo already: once), a voice type
    // twice, a send TYPE not measured twice the worst pair's slot. Beside one AMP at 1200 (51.9 %): a CLOUD at 10 % a slot
    // (13.75 % at 1200 with the chain factor) fits, at 15 % (20.6 %) not
    l.amps[3] = 1;
    co.send_bp[ins::kMCloud - admit::kSendType0] = 1000;
    admit::set_costs(co);
    CHECK(admit::type_bp(ins::kMCloud) == 0 && admit::master_slot_bp(ins::kMCloud) == 1000);
    CHECK(admit::master_slot_bp(ins::kSat) == 180 && admit::master_slot_bp(ins::kMHalo) == 0);
    CHECK(admit::ins3_fits(l, ins::kMCloud));
    co.send_bp[ins::kMCloud - admit::kSendType0] = 1500;
    co.slot_bp = 100;
    admit::set_costs(co);
    CHECK(!admit::ins3_fits(l, ins::kMCloud) && admit::master_slot_bp(ins::kMHalo) == 200);
    l.held[0] = false;   // no box in slot 0: its master chain not counted
    l.amps[3] = 0;
    CHECK(admit::core3_bp(l) == 1675);
    admit::set_enforce(false);
    l.held[0] = true, l.amps[3] = 3;
    CHECK(admit::amp_fits(3, l) && admit::ins3_fits(l, ins::kSat));   // admit=report: nothing refused
  }
  admit::reset();
}

static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }
static void send(int t, const std::vector<uint8_t>& m) { engine_sysex_link(t, m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static int hello(int t, uint16_t id) {
  std::vector<std::vector<uint8_t>>& o = t ? g_out1 : g_out;
  o.clear();
  send(t, sidecar({device::kCmdHello, 1, (uint8_t)(id >> 7), (uint8_t)(id & 0x7f), 1}));
  return o.empty() ? -1 : o.back()[8];
}
static void rytm35(int t) { send(t, sidecar({0x35, 1, 0, 100, 3})); }
static std::vector<uint8_t> trig(int k, int note) {   // a PLAITS 0x04 on the box's voice k (the Rytm's and the A4's report)
  static const uint8_t pl[16] = {93, 64, 0, 0, 127, 0, 64, 127, 64, 64, 0, 64, 64, 0, 0, 100};
  std::vector<uint8_t> t = {0x04, (uint8_t)(kPlaits | k), (uint8_t)note, 100};
  for (int i = 0; i < 16; ++i) t.push_back(pl[i]), t.push_back(0);
  return sidecar(t);
}
static constexpr int kRows = kBlock * 100;
static int16_t ring[kRows][kVoices];
static double rms(int vi) {
  uint64_t mach[kMachines] = {0};
  memset(ring, 0, sizeof ring);
  for (int b = 0; b < kRows / kBlock; ++b) render_voices(ring, kRows, b * kBlock, 0, kVoices, mach);
  double s = 0;
  for (int i = kRows / 2; i < kRows; ++i) s += (double)ring[i][vi] * ring[i][vi];
  return std::sqrt(s / (kRows / 2));
}

static void engine() {
  if (kVoiceSlots < 2) {
    printf("test_admit: an 8-voice engine: one slot, the engine part skipped\n");
    return;
  }
  char tmpl[] = "/tmp/core-admit-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4") && fs.mkdir("/.digitakt"));
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  // boot: the presumed Rytm in slot 0 has her 8; no bench yet: a second box would get all of its own
  CHECK(device::admitted(0) == 8 && device::admitted(1) == 0 && device::g_links[0].want == 8);
  CHECK(device::voices_asked(nullptr) == 8 && device::voices_asked(&device::kA4) == 4 && device::voices_asked(&device::kDigitakt) == 8);

  // the bench: a voice 12 %, a box's chains 10 %; split=4. The A4 attaches first (USB-C): its 4 as 2 + 2 (24 % a core)
  admit::set_load_pct({100, 100, 100});   // (synthetic costs as they are)
  admit::set_costs({1500, 1200, 0, 0, 1000});
  device::readmit();
  CHECK(hello(0, device::kIdA4) == device::kOk);
  CHECK(device::slot_held_by(&device::kA4) == 0 && device::admitted(0) == 4 && !device::g_links[0].tell);
  // the Rytm second, on the relay: her chains on core 1 (24 + 10 %), room for three of her voices there (70 %, a fourth 82 %)
  rytm35(1);
  CHECK(device::rytm_slot() == 1 && device::admitted(1) == 3 && device::g_links[1].want == 8 && device::g_links[1].tell);
  CHECK(device::voice_admitted(8 + 2) && !device::voice_admitted(8 + 3) && !device::voice_admitted(8 + 7));
  CHECK(device::voice_admitted(3) && !device::voice_admitted(4));   // the A4's own 0..3 (its 4..7: not its)
  // her reports past her two are dropped; her voice 1 plays
  send(1, trig(1, 60));
  send(1, trig(5, 60));
  CHECK(rms(8 + 1) > 100 && rms(8 + 5) == 0);
  // a USB re-enumeration (a plug on the Pi's other hub port knocks a box off and back, seen on a unit): the A4 leaves and comes
  // back while its slot is kept for it (slothold=, g_hold_ms). The Rytm is not raised meanwhile, and the A4 keeps its place:
  // first, its 4; the Rytm still 3
  uint64_t t = 1000000000ull;
  host_fake_time(true, t);
  device::g_hold_ms = 5000;
  device::release(0);
  CHECK(device::admitted(0) == 0 && device::admitted(1) == 3);
  device::step(true);
  CHECK(device::admitted(1) == 3);   // (not yet: the A4 may come back)
  t += 2000000, host_fake_time(true, t);
  CHECK(hello(0, device::kIdA4) == device::kOk);
  CHECK(device::slot_held_by(&device::kA4) == 0 && device::admitted(0) == 4 && device::admitted(1) == 3);
  CHECK(device::g_links[0].order < device::g_links[1].order);
  // the same box is its type and, where both are known, its DeviceUID (two boxes of one type): the A4 comes back once more by
  // its USB UID, leaves, and another A4 (another UID) does not take the slot kept for it; the first one back keeps its place
  const uint32_t kUidA = 0x0a4a4a4a, kUidB = 0x0b4b4b4b;
  device::release(0);
  t += 1000000, host_fake_time(true, t);
  CHECK(device::usb_attach(&device::kA4, kUidA) == 0);
  device::step(true);
  CHECK(device::g_links[0].uid == kUidA && device::admitted(0) == 4 && device::g_links[0].order < device::g_links[1].order);
  device::release(0);
  device::step(true);
  t += 1000000, host_fake_time(true, t);
  CHECK(device::usb_attach(&device::kA4, kUidB) == -1);   // (slot 0 is kept for A, slot 1 is the Rytm's: no stream for B)
  device::step(true);
  CHECK(device::admitted(0) == 0 && device::admitted(1) == 3);
  CHECK(device::usb_attach(&device::kA4, kUidA) == 0);
  device::step(true);
  CHECK(device::g_links[0].uid == kUidA && device::admitted(0) == 4 && device::admitted(1) == 3);
  CHECK(device::g_links[0].order < device::g_links[1].order);
  printf("  the same A4 (its UID) back in its kept slot keeps its place; another A4 does not take that slot\n");
  // the A4 leaves for good: the Rytm is alone once its slot is no longer kept, and gets her 8 back (and is told)
  device::release(0);
  device::step(true);
  CHECK(device::admitted(1) == 3);
  t += 11000000, host_fake_time(true, t);
  device::step(true);
  CHECK(device::admitted(0) == 0 && device::admitted(1) == 8 && device::g_links[1].tell);
  CHECK(device::voice_admitted(0));   // (a free slot: nothing of a box's plays there)
  host_fake_time(false, 0);
  device::g_hold_ms = 0;
  send(1, trig(5, 62));
  CHECK(rms(8 + 5) > 100);
  for (int i = 0; i < 10; ++i) device::step(true);   // (the A4's saves settle: slot 0 free for any box)

  // a Digitakt now attaches second (slot 0: the base box now), beside the Rytm's 8 and her chains on core 1 (48 + 10 %):
  // one of its voices fits there
  CHECK(hello(0, device::kIdDigitakt) == device::kOk);
  CHECK(device::slot_held_by(&device::kDigitakt) == 0 && device::admitted(0) == 1 && device::admitted(1) == 8);
  // admit=report: the second box gets its 8, /status's core_bp shows the worst case of both; the Pi's own notes as either box's
  // (engine_sysex_self_slot: the load test's drone on stock boxes) play its voices; enforce again: back to 1
  admit::set_enforce(false);
  device::readmit();
  CHECK(device::admitted(0) == 8 && device::admitted(1) == 8);
  {
    admit::Load both = {{8, 8}, {true, true}};
    CHECK(admit::core_bp(1, both) == 8 * 1200 + 1000 && admit::core_bp(2, both) == 8 * 1200 + 1000);
  }
  {
    std::vector<uint8_t> a = trig(6, 60), b = trig(3, 64);
    engine_sysex_self_slot(0, a.data(), (int)a.size());   // the Digitakt's voice 6
    engine_sysex_self_slot(1, b.data(), (int)b.size());   // the Rytm's voice 3
    engine_sysex_self_slot(2, b.data(), (int)b.size());   // (no such slot: nothing)
    CHECK(rms(6) > 100 && rms(8 + 3) > 100);
  }
  admit::set_enforce(true);
  device::readmit();
  CHECK(device::admitted(1) == 8 && device::admitted(0) == 1);
  {
    std::vector<uint8_t> a = trig(5, 62);
    engine_sysex_self_slot(0, a.data(), (int)a.size());   // past the Digitakt's one: dropped
    CHECK(rms(5) == 0);
  }
  // loadtest=on (idle_nosave): the flag only (the Pi's console; idle_save writes nothing while on)
  idle_nosave(true);
  CHECK(idle_nosave());
  idle_nosave(false);
  // voices= lowers a count (never raises one past its profile's): readmit keeps the first box (the Rytm) first
  CHECK(admit::parse_override("rytm:2,dt:8,a4:6"));
  CHECK(device::voices_asked(nullptr) == 2 && device::voices_asked(&device::kA4) == 4);
  device::readmit();
  // her 2 as 1 + 1; the Digitakt all 8: core 1 12 + 10 + 48 = 70 %, core 2 12 + 10 + 48 = 70 %
  CHECK(device::admitted(1) == 2 && device::admitted(0) == 8);
  // INSERT types admitted when set (decided 2026-11-22): a SAT slot 6 % here. The slot in hand's box's chain on core 2 (the
  // Digitakt's voice 4, or the Rytm's voice 1: her 2 as 1 + 1): core 2 70 % (48 + 10 + 12), one SAT fits (76 %), a second
  // not (82 %): it plays dry, NO CPU. OFF frees its place; the next idle pass admits the refused one again
  {
    admit::Costs co = admit::costs();
    co.type_bp[ins::kSat] = 600;
    admit::set_costs(co);
    const int b = ins::slot_in_hand(), c = b == 0 ? 4 : 1;
    admit::Load ld;
    device::load_now(&ld);
    CHECK(admit::voice_core(b, c, ld) == 2 && admit::core_bp(2, ld) == 7000 && ld.ins[1] == 0 && ld.ins[2] == 0);
    ins::set_type(c, 0, ins::kSat);
    CHECK(!ins::refused(c, 0));
    device::load_now(&ld);
    CHECK(ld.ins[2] == 600 && admit::core_bp(2, ld) == 7600);
    ins::set_type(c, 1, ins::kSat);
    CHECK(ins::refused(c, 1) && ins::stats().refused >= 1);
    CHECK(ins::refused_count(b) == 1 && ins::refused_count(1 - b) == 0);
    device::load_now(&ld);
    CHECK(ld.ins[2] == 600);   // (a refused slot is not counted)
    ins::set_type(c, 0, ins::kSat);   // set again (the same type): still admitted, its own place not counted twice
    CHECK(!ins::refused(c, 0));
    ins::set_type(c, 1, ins::kOff);
    CHECK(!ins::refused(c, 1));
    ins::set_type(c, 1, ins::kSat);
    CHECK(ins::refused(c, 1));
    ins::set_type(c, 0, ins::kOff);
    device::step(true);   // regrant
    CHECK(!ins::refused(c, 1));
    device::load_now(&ld);
    CHECK(ld.ins[2] == 600);
    admit::set_enforce(false);   // admit=report: nothing refused
    ins::set_type(c, 0, ins::kSat);
    CHECK(!ins::refused(c, 0));
    admit::set_enforce(true);
    ins::set_type(c, 0, ins::kOff), ins::set_type(c, 1, ins::kOff);
    // the clock lowered (the step-down for heat, decided 2026-11-22: a Pi without cooling): core 2 76 % at 1500 is 95 % at
    // 1200, over even without the SAT (87.5 %): the next idle pass sheds the SAT (dry, NO CPU; voices never), and the
    // clock back at 1500 admits it again
    ins::set_type(c, 0, ins::kSat);
    CHECK(!ins::refused(c, 0));
    admit::set_clock(1200);
    device::step(true);
    CHECK(ins::refused(c, 0) && ins::refused_count(b) == 1);
    device::load_now(&ld);
    CHECK(ld.ins[2] == 0 && device::admitted(0) == 8 && device::admitted(1) == 2);
    device::step(true);   // (still over: nothing else to shed, nothing admitted again)
    CHECK(ins::refused(c, 0));
    admit::set_clock(1500);
    device::step(true);
    CHECK(!ins::refused(c, 0));
    // LT7 (2026-11-23): under loadtest=on idle_save returns before device::step, so nothing was shed on the unit at
    // 1200 (core 3 at 82 %, the master AMP pair still playing). The load test's own pass sheds and admits again too
    idle_nosave(true);
    admit::set_clock(1200);
    for (int i = 0; i < 10; ++i) idle_save();   // (the idle passes: a step that is due goes first)
    CHECK(ins::refused(c, 0));
    admit::set_clock(1500);
    for (int i = 0; i < 10; ++i) idle_save();
    CHECK(!ins::refused(c, 0));
    idle_nosave(false);
    // a voice on OUT PRE / POST runs its chain in stereo: its types count twice (12 % for the SAT). Core 2 70 % + 12 %
    // is over 80 %: set again on a stereo voice, the SAT is refused; the voice back on TRK admits it again
    ins::set_type(c, 0, ins::kOff);
    out::set_route(c, out::kPre);
    ins::set_type(c, 0, ins::kSat);
    CHECK(ins::refused(c, 0));
    device::load_now(&ld);
    CHECK(ld.ins[2] == 0);
    out::set_route(c, out::kTrk);
    device::step(true);   // regrant
    CHECK(!ins::refused(c, 0));
    device::load_now(&ld);
    CHECK(ld.ins[2] == 600);
    // a SAT admitted on TRK, then its voice put on POST: over 80 % at once, the next idle pass sheds it
    out::set_route(c, out::kPost);
    device::load_now(&ld);
    CHECK(ld.ins[2] == 1200);
    device::step(true);
    CHECK(ins::refused(c, 0));
    out::set_route(c, out::kTrk);
    device::step(true);
    CHECK(!ins::refused(c, 0));
    ins::set_type(c, 0, ins::kOff);
    admit::set_clock(0);
  }
  // no bench: everything its count
  admit::reset();
  device::readmit();
  CHECK(device::admitted(1) == 8 && device::admitted(0) == 8);
  device::reset();
  CHECK(device::admitted(0) == 8 && device::admitted(1) == 0);
}

int main() {
  model();
  lt3();
  lt4();
  lt6();
  engine();
  printf("test_admit: PASS\n");
  return 0;
}
