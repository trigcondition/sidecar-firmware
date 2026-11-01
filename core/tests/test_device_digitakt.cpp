// core/tests/test_device_digitakt.cpp - the Digitakt end to end, on the Mac: what the sidecar-digitakt mod sent in an emulated
// OS 1.53 (the OS's USB sender a stand-in; tests/golden/device/digitakt.sysex holds only the mod's own messages) goes through
// host_sysex under the Digitakt profile and plays: eight tracks, each its machine. Not a hardware result. DTSC_CAPTURE: another
// capture.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/digitakt.h"
#include "device/strip.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "fmt/fmt.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "md/md.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static constexpr int kBlocks = 2000;   // 0.5 s
static int16_t ring[kBlocks * kBlock][kVoices];
// voice vi's sound from a clean ring
static double play(int vi) {
  uint64_t mach[kMachines] = {0};
  memset(ring, 0, sizeof ring);
  for (int b = 0; b < kBlocks; ++b) render_voices(ring, kBlocks * kBlock, b * kBlock, 0, kVoices, mach);
  double a = 0;
  for (int i = 0; i < kBlocks * kBlock; ++i) a += (double)ring[i][vi] * ring[i][vi];
  return a;
}

int main() {
  const char* path = getenv("DTSC_CAPTURE");
  FILE* f = fopen(path ? path : "tests/golden/device/digitakt.sysex", "rb");
  CHECK(f);
  std::vector<uint8_t> cap(1 << 20);
  cap.resize(fread(cap.data(), 1, cap.size(), f));
  fclose(f);
  CHECK(!cap.empty());

  char tmpl[] = "/tmp/core-dtsc-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples"));
  hooks.to_rytm = ::out;
  card_ok = cards.begin(log, shared, sizeof shared);
  CHECK(card_ok);
  junolib.begin(&cards.sd(), log);
  dxlib.begin(&cards.sd(), log);
  voices_init();
  CHECK(device::active().id == device::kIdRytm);

  // its first hello: a state folder of its own, so one restart; the capture plays on the boot after it
  static int restarts = 0;
  hooks.device_restart = [] { ++restarts; };
  const uint8_t hello[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, 0, device::kIdDigitakt, 1, 0xf7};
  host_sysex(hello, (int)sizeof hello);
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kRestart && !device::foreign());
  for (int k = 0; k < 100 && !restarts; ++k) {
    device::step();
    fmt::post_step();
  }
  CHECK(restarts == 1);
  device::reset();
  device::boot();
  CHECK(!strcmp(device::g_link.state, "/.digitakt") && device::active().id == device::kIdDigitakt);
  g_out.clear();

  int hellos = 0, trigs = 0, reports = 0, controls = 0, offs = 0;
  bool played[kVoices] = {};
  for (size_t i = 0; i < cap.size();) {
    size_t j = i;
    while (j < cap.size() && cap[j] != 0xf7) ++j;
    CHECK(j < cap.size() && cap[i] == 0xf0);
    int n = (int)(j - i + 1);
    const uint8_t* m = cap.data() + i;
    i = j + 1;
    host_sysex(m, n);
    if (m[4] == device::kCmdHello) {
      ++hellos;
      CHECK(device::active().id == device::kIdDigitakt);
    } else if (m[4] == 0x04) {
      int vi = m[5] & 7;
      CHECK(n == 43 && v[vi].live);
      if (!played[vi] && m[5] != (0x40 | 5)) {   // (MD's CT1..CT8 voice: FM BD at the mod's defaults, played below)
        CHECK(play(vi) > 0);
        played[vi] = true;
      }
      if (vi == 0) CHECK(!strcmp(md::set_name(md::voice_set(0)), "TRX B2"));
      ++trigs;
    } else if (m[4] == 0x11) {
      ++offs;
    } else if (m[4] == device::kCmdControls) {   // its FILTER and AMP values (core/device/strip.h), no answer
      CHECK(n == 7 + 2 * device::kDigitakt.controls->n);
      ++controls;
    } else {
      CHECK(m[4] == 0x03 && n == 41);
      ++reports;
    }
  }
  CHECK(hellos >= 2 && trigs >= 8 && reports > 100);
  // the machines the panel chose, each on its own voice
  CHECK(v[0].md && !v[1].daisy && !v[1].juno && !v[1].fm && !v[1].md && v[2].juno && v[3].fm && v[4].daisy && v[5].md &&
        !strcmp(md::set_name(md::voice_set(5)), "FM BD") && v[7].juno);
  // one answer a hello, nothing else: no traffic back but that
  CHECK((int)g_out.size() == hellos);
  for (auto& o : g_out) CHECK(o[4] == device::kCmdHello && o[8] == device::kOk);
  // a message past a track the profile has, or the Rytm's commands, go nowhere
  uint32_t d0 = device::dropped();
  const uint8_t pool[] = {0xf0, 0x7d, 0x52, 0x54, 0x13, 0xf7};
  host_sysex(pool, (int)sizeof pool);
  CHECK(device::dropped() == d0 + 1 && (int)g_out.size() == hellos);
  // The FILTER and AMP pages (core/device/strip.h, the table in core/device/digitakt.cpp): FREQ RESO TYPE ENV, A D S R, ATK HOLD DEC VOL PAN
  {
    auto ctl = [&](int t, std::vector<int> x) {
      std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, device::kCmdControls, (uint8_t)t};
      for (int k : x) m.push_back((uint8_t)k), m.push_back(0);
      m.push_back(0xf7);
      host_sysex(m.data(), (int)m.size());
    };
    using namespace core;
    CHECK(device::active().bracket == device::kBracketMix && device::active().controls);
    int vi = 5;
    uint32_t us = 1000000;
    auto run = [&](int ms) { for (int k = 0; k < ms; ++k) device::tick(us += 1000); };
    run(3000);   // (the capture's envelopes have run out)
    ctl(vi, {127, 0, 1, 64, 0, 64, 0, 64, 0, 127, 64, 100, 64});   // the default sound: an open low-pass, nothing to run
    device::strip::trig(vi);
    run(1);
    CHECK(ins::control.type[vi][0] == ins::kOff && out::control.lev[vi] == 100 && out::control.pan[vi] == 64);
    ctl(vi, {48, 20, 1, 64, 0, 64, 0, 64, 0, 127, 64, 90, 30});     // LP at 48, VOL 90, PAN 30
    CHECK(ins::control.type[vi][0] == ins::kFilter && ins::locked(vi, 0, 0) == 48 && ins::locked(vi, 0, 1) == 20 && ins::locked(vi, 0, 2) == 0);
    CHECK(out::control.lev[vi] == 90 && out::control.pan[vi] == 30);
    ctl(vi, {48, 20, 2, 64, 0, 64, 0, 64, 0, 127, 64, 90, 30});     // HP: FILTER's morph at its far end
    CHECK(ins::control.type[vi][0] == ins::kFilter && ins::locked(vi, 0, 2) == 127);
    ctl(vi, {70, 90, 5, 64, 0, 64, 0, 64, 0, 127, 64, 90, 30});     // EQ_3: a bell, GAIN from RESO's knob
    CHECK(ins::control.type[vi][0] == ins::kEq && ins::locked(vi, 0, 3) == 70 && ins::locked(vi, 0, 1) == 90);
    ctl(vi, {70, 90, 0, 64, 0, 64, 0, 64, 0, 127, 64, 90, 30});     // OFF
    CHECK(ins::control.type[vi][0] == ins::kOff);
    // ATK 0: at once, HOLD 127 (NOTE: no gate is reported): it stays
    ctl(vi, {127, 0, 1, 64, 0, 64, 0, 64, 0, 127, 64, 100, 64});
    device::strip::trig(vi);
    CHECK(device::strip::amp_env(vi) == 1.0f);
    run(2000);
    CHECK(device::strip::amp_env(vi) == 1.0f && out::control.lev[vi] == 100);
    // an attack rises, HOLD 0, a short DEC ends the voice
    ctl(vi, {127, 0, 1, 64, 60, 64, 0, 64, 60, 0, 30, 100, 64});
    device::strip::trig(vi);
    CHECK(device::strip::amp_env(vi) == 0.0f && out::control.lev[vi] <= 100);
    run(4);
    float rising = device::strip::amp_env(vi);
    CHECK(rising > 0.0f && rising < 1.0f);
    run(3000);
    CHECK(device::strip::amp_env(vi) == 0.0f && out::control.lev[vi] == 0);
    // the filter's envelope on CUT: up with a positive ENV, down to SUS
    ctl(vi, {40, 0, 1, 127, 0, 20, 0, 64, 0, 127, 64, 100, 64});
    device::strip::trig(vi);
    run(1);
    CHECK(device::strip::flt_env(vi) > 0.5f);
    CHECK(ins::control.play[vi][0][0] > 40);
    run(3000);
    CHECK(device::strip::flt_env(vi) < 0.01f && ins::control.play[vi][0][0] == 40);
    // a wrong length, a track past the box: dropped, not played
    uint32_t d1 = device::dropped();
    const uint8_t shortm[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdControls, 1, 10, 0, 0xf7};
    host_sysex(shortm, (int)sizeof shortm);
    CHECK(device::dropped() == d1 + 1);
    ctl(8, {127, 0, 1, 64, 0, 64, 0, 64, 0, 127, 64, 100, 64});
    CHECK(device::dropped() == d1 + 2);
    ctl(vi, {127, 0, 1, 64, 0, 64, 0, 64, 0, 127, 64, 100, 64});
  }
  printf("test_device_digitakt: ok: %d hellos, %d trigs, %d reports, %d controls, %d note-offs\n", hellos, trigs, reports, controls, offs);
  return 0;
}
