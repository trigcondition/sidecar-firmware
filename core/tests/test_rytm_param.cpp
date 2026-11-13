// core/tests/test_rytm_param.cpp - the Rytm's own sound parameters modulated from the Sidecar (core/mod/rytm_param.h;
// notes/design rytm-param-mod.md 6.2, the host tests H1..H11) against a fake Rytm (tests/pm_fake.h) that does the
// contract's arithmetic on the bytes the host sends: the RT / RFX names, the 04..07 bytes, the sender's policy (changed
// records only, the 5 ms grid, the 100 ms refresh, the 16 KB/s cap, caps bit 64, a hello, a kit change, the link), an
// LFO / the XY pad / MIDI learn as offsets, the exclusions, a refusal never the CC fallback, the velocity words, the
// files, the morph's parts and the poly tracks' p-locks.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "device/device.h"
#include "bundle/rename.h"
#include "msamp/cardq.h"
#include "lfo.h"
#include "mod/learn.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/rytm_param.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "mod/xypad.h"
#include "ui/remote.h"
#include "pm_fake.h"

using namespace core;
using Msg = std::vector<uint8_t>;

static pmfake::Rytm* g_r = nullptr;
static std::vector<Msg> g_pm;      // every param message (04..07), with the time it went
static std::vector<uint64_t> g_pm_us;
static std::vector<Msg> g_other;   // everything else the host sent the Rytm
static uint64_t g_us = 1000000;
static void out(const uint8_t* m, int n) {
  if (n >= 8 && m[4] == mod::kHostCmd && m[6] >= 4 && m[6] <= 7) {
    g_pm.emplace_back(m, m + n), g_pm_us.push_back(g_us);
    if (g_r) {
      g_r->take(m, n);
      for (auto& a : g_r->answers) {   // the fake's answers back to the host (2D 01 10 / 11)
        std::vector<uint8_t> f = {0xF0, 0x7D, 0x52, 0x54};
        f.insert(f.end(), a.begin(), a.end());
        f.push_back(0xF7);
        engine_sysex(f.data(), (int)f.size());
      }
      g_r->answers.clear();
    }
    return;
  }
  g_other.emplace_back(m, m + n);
}
static void at(uint64_t us) {
  g_us = us, host_fake_time(true, g_us);
  if (g_r) g_r->advance(us);
}
static void run_ms(int ms) {
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), mod::tick(now_us());
}
static mod::Tid T(const char* n) {
  mod::Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
static void rx(std::vector<uint8_t> body) {   // a Rytm -> host message F0 7D 52 54 body F7
  std::vector<uint8_t> f = {0xF0, 0x7D, 0x52, 0x54};
  f.insert(f.end(), body.begin(), body.end());
  f.push_back(0xF7);
  engine_sysex(f.data(), (int)f.size());
}
static void hello(int caps) { rx({0x35, 1, 0, 40, (uint8_t)caps}); }
static void kit(int k) { rx({0x3B, 1, (uint8_t)k}); }
static void machines(int machine, int vb) {   // 33: every track the same machine (vb 0x7F: not a Sidecar machine)
  std::vector<uint8_t> b = {0x33, 1, 0, 12};
  for (int t = 0; t < 12; ++t) b.push_back((uint8_t)machine), b.push_back((uint8_t)vb);
  rx(b);
}
static void machine1(int t, int machine, int vb) { rx({0x33, 1, (uint8_t)t, 1, (uint8_t)machine, (uint8_t)vb}); }

// the sources a test drives (mod::set_source_fn): up to 16 targets with an offset each; g_src_hz nonzero: a sine of
// that rate, g_src_v its depth (0.5.0 owner test: no LFO on the Rytm, so these stand in for a moving layer source)
static mod::Tid g_src_t[16];
static float g_src_v[16], g_src_hz[16];
static void src(uint32_t us, uint32_t) {
  for (int i = 0; i < 16; ++i)
    if (g_src_t[i])
      mod::offset(i, g_src_t[i], 1.0f, g_src_hz[i] ? g_src_v[i] * (float)sin(2 * M_PI * g_src_hz[i] * (double)us / 1e6) : g_src_v[i]);
}
static pmfake::Rytm g_fake;
static void fresh(int caps = 127, int k = 4) {
  mod::reset();
  scene::reset();
  xypad::reset();
  learn::reset();
  lfo::reset();
  memset(g_src_t, 0, sizeof g_src_t);
  memset(g_src_v, 0, sizeof g_src_v);
  memset(g_src_hz, 0, sizeof g_src_hz);
  g_fake = pmfake::Rytm();
  g_fake.kit = k, g_fake.us = g_us, g_fake.last_op_us = g_us;
  g_r = &g_fake;
  machines(0, 0x7F);   // BD HARD everywhere, none a Sidecar machine
  hello(caps);
  kit(k);
  at(g_us + 1000), mod::tick(now_us());
  g_pm.clear(), g_pm_us.clear(), g_other.clear();
}
static int count_op(int op, size_t from = 0) {
  int n = 0;
  for (size_t i = from; i < g_pm.size(); ++i) n += g_pm[i][6] == op;
  return n;
}
static const Msg* last_op(int op) {
  for (size_t i = g_pm.size(); i-- > 0;)
    if (g_pm[i][6] == op) return &g_pm[i];
  return nullptr;
}

// ---- H1 names
static void h1_names() {
  int n = 0;
  for (int k = mod::kRs; k <= mod::kRfx; ++k)
    for (int i = 0; i < 64; ++i)
      for (int p = 0; p < 32; ++p) {
        mod::Tid t = mod::tid((mod::Kind)k, i, p);
        if (!mod::valid(t)) continue;
        ++n;
        char nm[16];
        mod::name(t, nm, sizeof nm);
        mod::Tid b = 0;
        CHECK(mod::parse(nm, &b) && b == t);
        int tr, w;
        CHECK(mod::rytm_word(t, &tr, &w) && mod::rytm_tid(tr, w) == t && mod::is_offset(mod::kind_of(t)));
      }
  CHECK(n == 12 * 23 + 12 * 26 + 29);   // kRs: 24 words but DST; kRa: 17 words + VM1..4 AT1..4 VEL; RFX: 29
  // the kind-table golden: RT1's names in word order, RFX's
  std::string all;
  for (int w = 0; w < 64; ++w) {
    mod::Tid t = mod::rytm_tid(0, w);
    char nm[16];
    if (t) all += mod::name(t, nm, sizeof nm), all += ' ';
  }
  CHECK(all == "RT1:MUL RT1:WAV RT1:MOD RT1:SPD RT1:FAD RT1:SPH RT1:DEP RT1:P1 RT1:P2 RT1:P3 RT1:P4 RT1:P5 RT1:P6 RT1:P7 "
               "RT1:P8 RT1:TUN RT1:FIN RT1:SMP RT1:BR RT1:STA RT1:END RT1:LOP RT1:LEV RT1:FENV RT1:FATK RT1:FDEC RT1:FSUS "
               "RT1:FREL RT1:FRQ RT1:RES RT1:FTYP RT1:ATK RT1:HLD RT1:DEC RT1:OVR RT1:VOL RT1:PAN RT1:ACC RT1:DEL RT1:REV "
               "RT1:VM1 RT1:VM2 RT1:VM3 RT1:VM4 RT1:AT1 RT1:AT2 RT1:AT3 RT1:AT4 RT1:VEL ");
  all.clear();
  for (int w = 0; w < 64; ++w) {
    mod::Tid t = mod::rytm_tid(12, w);
    char nm[16];
    if (t) all += mod::name(t, nm, sizeof nm), all += ' ';
  }
  CHECK(all == "RFX:SPD RFX:FAD RFX:DEP RFX:TIM RFX:X RFX:WID RFX:FDB RFX:DHPF RFX:DLPF RFX:DREV RFX:DVOL RFX:DOV RFX:PRE "
               "RFX:RDEC RFX:RFRQ RFX:GAI RFX:RHPF RFX:RLPF RFX:RVOL RFX:AMT RFX:SYM RFX:THR RFX:CATK RFX:CREL RFX:RAT "
               "RFX:SEQ RFX:MUP RFX:MIX RFX:CVOL ");
  int tr, w;
  CHECK(mod::rytm_word(T("RT3:FRQ"), &tr, &w) && tr == 2 && w == 30);
  CHECK(mod::rytm_word(T("rt1:p2"), &tr, &w) && tr == 0 && w == 10);
  CHECK(mod::rytm_word(T("RT2:VEL"), &tr, &w) && tr == 1 && w == 56);
  CHECK(mod::rytm_word(T("RT5:VM2"), &tr, &w) && tr == 4 && w == 49);
  CHECK(mod::rytm_word(T("RT12:AT4"), &tr, &w) && tr == 11 && w == 55);
  CHECK(mod::rytm_word(T("RFX:TIM"), &tr, &w) && tr == 12 && w == 9);
  CHECK(mod::rytm_word(T("RFX:DEP"), &tr, &w) && tr == 12 && w == 8);
  mod::Tid x;
  for (const char* bad : {"RT1:DST", "RT13:FRQ", "RT0:FRQ", "RFX1:TIM", "RT1:TIM", "RFX:FRQ", "RT:FRQ", "RT1:", "RFX:P1"})
    CHECK(!mod::parse(bad, &x));
  CHECK(mod::parse("R1:GATE", &x) && mod::kind_of(x) == mod::kArp && mod::parse("T1:SPD", &x) && mod::kind_of(x) == mod::kSeq &&
        mod::parse("V1:P1", &x) && mod::kind_of(x) == mod::kV && mod::parse("RYTM:PERF3", &x));   // the old names as they were
  // list words: override only, whole entries; the rest offsets -127..127
  for (const char* l : {"RT1:MUL", "RT1:WAV", "RT1:MOD", "RT1:SMP", "RT1:LOP", "RT1:FTYP"})
    CHECK(mod::override_only(T(l)) && mod::range(T(l)).mode == mod::kStep);
  CHECK(!mod::override_only(T("RT1:FRQ")) && mod::range(T("RT1:FRQ")).lo == -127 && mod::range(T("RT1:FRQ")).mode == mod::kCont);
  // P1..P8 labels per machine (rytm-param-mod M0d, ROM 1.73: all 53 machines): BD HARD's TUN, BT CLASSIC's empty
  // slots, a "2" variant's own order (TUN before LEV), the machines with no synth page, 4-letter labels, the test ones
  CHECK(!strcmp(mod::rp_machine_label(0, 10), "TUN") && !strcmp(mod::rp_machine_label(7, 12), "") &&
        !strcmp(mod::rp_machine_label(25, 16), "C3"));
  CHECK(!strcmp(mod::rp_machine_label(29, 9), "LEV") && !strcmp(mod::rp_machine_label(29, 16), "SPD"));   // SY CHIP
  CHECK(!strcmp(mod::rp_machine_label(35, 9), "TUN") && !strcmp(mod::rp_machine_label(35, 10), "LEV"));   // BD HARD 2
  CHECK(!strcmp(mod::rp_machine_label(40, 9), "BAL") && !strcmp(mod::rp_machine_label(42, 16), "LEV"));   // RS CLASSIC 2, RS FM 2
  CHECK(!strcmp(mod::rp_machine_label(32, 10), "TUNE") && !strcmp(mod::rp_machine_label(33, 16), "OSC6"));   // SY RAW, HH LAB
  for (int m : {27, 34, 43})   // DISABLE, BT IMPROVED, FUNC. GEN.: no synth page
    for (int w = 9; w <= 16; ++w) CHECK(!strcmp(mod::rp_machine_label(m, w), ""));
  CHECK(!strcmp(mod::rp_machine_label(51, 11), "50") && !strcmp(mod::rp_machine_label(52, 16), "PLN"));
  CHECK(mod::rp_machine_label(53, 9) == nullptr && mod::rp_machine_label(0, 8) == nullptr);   // beyond the table
  fresh();
  char b[16];
  CHECK(!strcmp(mod::rp_label(T("RT1:P2"), b, sizeof b), "TUN") && !strcmp(mod::rp_label(T("RT3:FRQ"), b, sizeof b), "FRQ"));
  machine1(1, 29, 0x7F);   // SY CHIP: OF2 on P4
  CHECK(!strcmp(mod::rp_label(T("RT2:P4"), b, sizeof b), "OF2"));
  machine1(1, 35, 0x7F);   // BD HARD 2: TUN first
  CHECK(!strcmp(mod::rp_label(T("RT2:P1"), b, sizeof b), "TUN") && !strcmp(mod::rp_label(T("RT2:P2"), b, sizeof b), "LEV"));
  machine1(1, 34, 0x7F);   // BT IMPROVED: no synth page, its P-words never sent
  CHECK(!mod::rp_active(T("RT2:P1")) && !mod::rp_active(T("RT2:P8")) && mod::rp_active(T("RT2:FRQ")));
  // the LFO's RT targets: the list's names, the file's word
  int rt3 = lfo::target_from_name("RT3");
  CHECK(rt3 == lfo::kTRt + 2 && lfo::target_from_name("RFX") == lfo::kTRfx && lfo::params_of(rt3) == 43);
  CHECK(lfo::tid_of(rt3, 0) == T("RT3:SPD") && lfo::tid_of(lfo::kTRfx, 3) == T("RFX:TIM"));
  // M0b: the compressor reads the effective words: THR MUP MIX CVOL take an LFO (appended: older files' indices
  // unchanged); its ATK REL RAT SEQ are lists (override only, as the RT list words)
  CHECK(lfo::params_of(lfo::kTRfx) == 25 && lfo::tid_of(lfo::kTRfx, 21) == T("RFX:THR") &&
        lfo::tid_of(lfo::kTRfx, 24) == T("RFX:CVOL"));
  for (const char* l : {"RFX:CATK", "RFX:CREL", "RFX:RAT", "RFX:SEQ"}) CHECK(mod::override_only(T(l)));
  for (const char* l : {"RFX:THR", "RFX:MUP", "RFX:MIX", "RFX:CVOL"}) CHECK(!mod::override_only(T(l)));
  bool frq = false;
  for (int p = 0; p < lfo::params_of(rt3); ++p) {
    CHECK(mod::valid(lfo::tid_of(rt3, p)) && !mod::override_only(lfo::tid_of(rt3, p)));
    frq |= !strcmp(lfo::param_name(rt3, p, b, sizeof b), "FRQ");
  }
  CHECK(frq);
  // the screens' names (mod::label / param_label: the morph, XY and scene pickers): by name, P1..P8 by the machine
  CHECK(!strcmp(mod::label(T("RT3:FRQ"), b, sizeof b), "RT3:FRQ") && !strcmp(mod::label(T("RFX:TIM"), b, sizeof b), "RFX:TIM"));
  CHECK(!strcmp(mod::label(T("RT1:P2"), b, sizeof b), "RT1:TUN") && !strcmp(mod::param_label(T("RT1:P2"), b, sizeof b), "TUN"));
  // (track 2 is on BT IMPROVED here: no synth page, the plain name) the 53-machine table is the one source: SY RAW's
  // 4-letter TUNE reaches the pickers' labels too; the compressor's appended words by name
  CHECK(!strcmp(mod::label(T("RT2:P2"), b, sizeof b), "RT2:P2") && !strcmp(mod::label(T("RT12:VEL"), b, sizeof b), "RT12:VEL"));
  machine1(1, 32, 0x7F);   // SY RAW
  CHECK(!strcmp(mod::label(T("RT2:P2"), b, sizeof b), "RT2:TUNE") && !strcmp(mod::param_label(T("RT2:P8"), b, sizeof b), "BAL"));
  CHECK(!strcmp(mod::label(T("RFX:THR"), b, sizeof b), "RFX:THR") && !strcmp(mod::label(T("RFX:CVOL"), b, sizeof b), "RFX:CVOL"));
  printf("rytm params H1: every RT / RFX name round-trips, the list words override only, P1..P8 by machine\n");
}

// ---- H2 the bytes
static void h2_encoding() {
  fresh(127, 4);
  g_src_t[0] = T("RT3:FRQ"), g_src_v[0] = 10.5f;   // +10.5 steps = 672 / 64
  g_src_t[1] = T("RT1:PAN"), g_src_v[1] = -1.0f;   // -64: 14-bit two's complement 0x3FC0
  mod::set_source_fn(src);
  run_ms(2);
  CHECK(count_op(4) == 1);
  const Msg& m = *last_op(4);
  // F0 7D 52 54 5A 01 04 <kit> <ramp> <n> {t w hi lo} x n F7; ramp 8 blocks (5 ms after a pause: the grid's)
  CHECK(m.size() == 11 + 8 && m[7] == 4 && m[8] == 8 && m[9] == 2 && m.back() == 0xF7);
  bool a = false, b = false;
  for (int i = 0; i < 2; ++i) {
    const uint8_t* r = &m[10 + 4 * i];
    a |= r[0] == 2 && r[1] == 30 && r[2] == 5 && r[3] == 0x20;
    b |= r[0] == 0 && r[1] == 38 && r[2] == 0x7F && r[3] == 0x40;
  }
  CHECK(a && b);
  run_ms(10);
  CHECK(fabs(g_fake.eff(2, 30) - 74.5) < 0.01 && fabs(g_fake.eff(0, 38) - 63.0) < 0.01);   // the fake took them
  // n limits: 30 records changing at once -> 28 now, the other 2 at the next 5 ms
  for (int i = 0; i < 4; ++i) g_src_t[i] = 0;
  mod::set_source_fn(nullptr);
  run_ms(20);
  g_pm.clear();
  for (int i = 0; i < 30; ++i) mod::rp_offset(mod::rytm_tid(i % 12, 25 + i / 12), 3.0f);
  run_ms(1);
  CHECK(count_op(4) == 1 && (*last_op(4))[9] == 28 && last_op(4)->size() == 11 + 4 * 28 && last_op(4)->size() <= 123);
  run_ms(5);
  CHECK(count_op(4) == 2 && (*last_op(4))[9] == 2);
  // 05: a p-lock's override (jump, mix 127, ramp 0) at its due time; 22 a message
  g_pm.clear();
  for (int i = 0; i < 24; ++i) mod::rp_lock(i % 12, 30 + i / 12, 100 * 128 + 5, now_us());
  run_ms(1);
  const Msg* o = nullptr;
  for (const Msg& m5 : g_pm)
    if (!o && m5[6] == 5) o = &m5;
  CHECK(o && (*o)[8] == 0 && (*o)[9] == 22 && o->size() == 11 + 5 * 22 && o->size() <= 121);
  CHECK((*o)[10] < 12 && ((*o)[11] & 0x40) && (*o)[12] == 100 && (*o)[13] == 5 && (*o)[14] == 127);
  CHECK(count_op(5) == 2);   // (the other 2: due too, a second message at once)
  run_ms(2);
  CHECK(fabs(g_fake.eff(0, 30) - (100 + 5 / 128.0)) < 0.01);
  // 06 / 07
  g_pm.clear();
  CHECK(mod::command("rp=clear", (char[200]){}, 200));
  run_ms(1);
  CHECK(count_op(6) == 1 && *last_op(6) == Msg({0xF0, 0x7D, 0x52, 0x54, 0x5A, 1, 6, 4, 0, 0x7F, 0x3F, 0xF7}));
  char o2[300];
  CHECK(mod::command("rp=status", o2, sizeof o2) && *last_op(7) == Msg({0xF0, 0x7D, 0x52, 0x54, 0x5A, 1, 7, 0xF7}));
  CHECK(mod::command("rp", o2, sizeof o2) && strstr(o2, "Rytm: ") && strstr(o2, "on, kit 5"));   // the fake's 2D 01 11
  printf("rytm params H2: 04 / 05 / 06 / 07 bytes, two's complement, n limits, the kit byte\n");
}

// ---- H3 the policy
static void h3_policy() {
  fresh();
  mod::Tid t = T("RT1:FRQ");
  g_src_t[0] = t, g_src_v[0] = 5.0f;
  mod::set_source_fn(src);
  run_ms(1);
  CHECK(count_op(4) == 1);
  run_ms(50);
  CHECK(count_op(4) == 1);   // unchanged: nothing until the refresh
  g_src_v[0] = 5.03f;        // < 1/16 step: not sent
  run_ms(20);
  CHECK(count_op(4) == 1);
  run_ms(40);                // the refresh (100 ms after it went)
  CHECK(count_op(4) == 2);
  g_pm.clear(), g_pm_us.clear();
  for (int i = 0; i < 40; ++i) g_src_v[0] = 5.0f + 0.2f * (float)i, run_ms(1);   // a change every ms
  for (size_t i = 1; i < g_pm_us.size(); ++i) CHECK(g_pm_us[i] - g_pm_us[i - 1] >= mod::kGridUs);   // the grid
  CHECK(count_op(4) >= 7 && count_op(4) <= 9);
  for (const Msg& m : g_pm) CHECK(m[8] == 8);   // ramp = the interval (5 ms = 7.5 blocks)
  // the cap: 128 entries moving every tick: at most 16 KB/s, the grid stretched, the ramp with it
  g_src_t[0] = 0;
  mod::set_source_fn(nullptr);
  run_ms(10);
  g_pm.clear(), g_pm_us.clear();
  uint64_t t0 = g_us;
  for (int ms = 0; ms < 2000; ++ms) {
    static const int kW[11] = {25, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36};
    for (int i = 0; i < 128; ++i) mod::rp_offset(mod::rytm_tid(i % 12, kW[i / 12]), (float)((ms + i) % 20));
    run_ms(1);
  }
  size_t bytes = 0;
  for (const Msg& m : g_pm) bytes += m.size();
  double secs = (double)(g_us - t0) / 1e6;
  // (the bucket's 100 ms of credit and one message may come on top)
  CHECK(bytes <= mod::kBytesPerS * secs + mod::kBytesPerS / 10 + 128 && bytes / secs > mod::kBytesPerS * 0.8);
  CHECK(mod::rp_live() == 128);
  uint64_t gap = 0;
  for (size_t i = 1; i < g_pm_us.size(); ++i) gap += g_pm_us[i] - g_pm_us[i - 1];
  CHECK(gap / (g_pm_us.size() - 1) > mod::kGridUs);   // stretched
  CHECK((*last_op(4))[8] > 8);                         // the ramp grew
  CHECK(mod::rp_live() <= mod::kRpEntries);
  // no caps 64: nothing; a hello with it: everything again (the Rytm cleared)
  fresh(63);
  g_src_t[0] = t, g_src_v[0] = 5.0f;
  mod::set_source_fn(src);
  run_ms(200);
  CHECK(g_pm.empty() && !mod::rp_on() && !mod::rp_active(t));
  char o[300];
  CHECK(mod::command("rp", o, sizeof o) && strstr(o, "needs Rytm mod 0.5"));
  hello(127);
  run_ms(1);
  CHECK(count_op(4) == 1);
  run_ms(30);
  hello(127);   // a new link: the same values again at once, not at the refresh
  run_ms(1);
  CHECK(count_op(4) == 2);
  // a kit change: 06 for every track under the new kit, then the live offsets under it
  g_pm.clear();
  g_fake.kit = 9;   // (the Rytm switched)
  kit(9);
  run_ms(6);
  CHECK(g_pm.size() >= 2 && g_pm[0][6] == 6 && g_pm[0][7] == 9 && g_pm[0][9] == 0x7F && g_pm[0][10] == 0x3F);
  CHECK(g_pm[1][6] == 4 && g_pm[1][7] == 9);
  run_ms(10);
  CHECK(fabs(g_fake.eff(0, 30) - 69.0) < 0.01);
  // the old kit's message: the Rytm drops it (the fake counts it)
  int kd = g_fake.kit_drops;
  g_fake.kit = 3;
  run_ms(110);
  CHECK(g_fake.kit_drops > kd);
  g_fake.kit = 9;
  // the link: its USB gone, nothing until the next hello
  mod::rp_link_lost();
  g_pm.clear();
  run_ms(300);
  CHECK(g_pm.empty());
  hello(127);
  run_ms(1);
  CHECK(count_op(4) == 1);
  // no kit named yet: nothing (every message needs the kit byte)
  mod::reset();
  mod::set_source_fn(src);
  g_pm.clear();
  hello(127);
  run_ms(50);
  CHECK(g_pm.empty());
  printf("rytm params H3: changed only, the 5 ms grid, the 100 ms refresh, the 16 KB/s cap, caps 64, hello, kit, link\n");
}

// ---- H4 a moving source (0.5.0 owner test: an LFO no more - every LFO refuses a Rytm target)
static void h4_lfo() {
  fresh();
  lfo::Settings s;
  lfo::defaults(&s);
  s.target = (uint8_t)lfo::target_from_name("RT1");
  s.depth = 40;
  CHECK(!lfo::set(0, s) && !lfo::set(1, s) && lfo::get(0).target == 0);
  char o[200];
  CHECK(!lfo::command("lfo=1:tgt:RT1", o, sizeof o) || strstr(o, "cannot"));
  CHECK(!lfo::command("lfo=3:tgt:RT2", o, sizeof o) || strstr(o, "cannot"));
  CHECK(!lfo::command("lfo=FXA:tgt:RFX", o, sizeof o) || strstr(o, "cannot"));
  for (int i = 0; i < lfo::kLfos; ++i) CHECK(lfo::get(i).target == 0);
  // the link itself: a slow, deep sine on RT1:FRQ (as an LFO's was: 0.02 Hz, depth 40) from a layer source
  g_src_t[0] = T("RT1:FRQ"), g_src_v[0] = 40, g_src_hz[0] = 0.02f;
  mod::set_source_fn(src);
  run_ms(1);
  double worst = 0;
  for (int i = 0; i < 3000; ++i) {
    run_ms(1);
    const mod::Entry* e = mod::find(T("RT1:FRQ"));
    CHECK(e);
    double d = fabs(g_fake.eff(0, 30) - (64.0 + e->off));
    if (i > 10 && d > worst) worst = d;
  }
  CHECK(worst < 0.125);   // within 1/16 step plus one interval's move
  for (size_t i = 1; i < g_pm_us.size(); ++i) CHECK(g_pm_us[i] - g_pm_us[i - 1] >= mod::kGridUs);   // <= 200 Hz
  // the ramp: the record's own update interval (review finding 7: a slow sine moves 1/16 step every ~12 ms), >= the grid
  for (size_t i = 1; i < g_pm.size(); ++i)
    if (g_pm[i][6] == 4 && g_pm[i - 1][6] == 4 && i > 3)
      CHECK(g_pm[i][8] >= 8 && g_pm[i][8] <= (g_pm_us[i] - g_pm_us[i - 1]) * 3 / 2000 + 2);
  CHECK(count_op(4) > 50);
  // the source off: offset 0 once, then nothing; the Rytm's entry freed
  g_src_t[0] = 0;
  g_pm.clear();
  run_ms(300);
  CHECK(count_op(4) == 1 && (*last_op(4))[12] == 0 && (*last_op(4))[13] == 0 && mod::rp_live() == 0);
  CHECK(!g_fake.has(0, 30) && fabs(g_fake.eff(0, 30) - 64.0) < 0.01);
  mod::set_source_fn(nullptr);
  printf("rytm params H4: no LFO takes RT / RFX; a source on RT1:FRQ at <= 200 Hz, ramp = its own interval; off: 0 once\n");
}

// ---- H5 the XY pad (+ MIDI learn)
static void h5_xy() {
  fresh();
  mod::Tid t = T("RT2:RES");
  CHECK(xypad::set(0, 0, t, 10));
  xypad::pos(16383, 0);
  run_ms(10);
  CHECK(fabs(g_fake.eff(1, 31) - (64 + 12.7)) < 0.05);   // + d x p x 127
  xypad::pos(8192, 0);
  run_ms(10);
  CHECK(fabs(g_fake.eff(1, 31) - (64 + 6.35)) < 0.05);
  xypad::up();
  run_ms(10);
  CHECK(fabs(g_fake.eff(1, 31) - 64) < 0.01);
  xypad::set_hold(true);
  xypad::pos(16383, 0);
  run_ms(5);
  xypad::up();
  run_ms(300);   // held: the refresh keeps it on the Rytm past its 250 ms timeout
  CHECK(fabs(g_fake.eff(1, 31) - (64 + 12.7)) < 0.05 && g_fake.timeouts == 0);
  xypad::set_hold(false);
  run_ms(10);
  CHECK(fabs(g_fake.eff(1, 31) - 64) < 0.01);
  // with another layer source on the same word (an LFO's, before the owner's test): the sum
  g_src_t[0] = t, g_src_v[0] = 8;
  mod::set_source_fn(src);
  xypad::pos(16383, 0);
  run_ms(20);
  const mod::Entry* e = mod::find(t);
  CHECK(e && fabs(e->off) > 8 + 12);
  CHECK(fabs(g_fake.eff(1, 31) - (64 + e->off)) < 0.05);
  xypad::up();
  g_src_t[0] = 0;
  mod::set_source_fn(nullptr);
  // MIDI learn: a CC route on RT4:VOL
  CHECK(learn::arm("RT4:VOL:20"));
  CHECK(learn::midi(0, 0xB0, 20, 127));   // the CC learnt (and its value)
  CHECK(learn::midi(0, 0xB0, 20, 127));
  run_ms(10);
  CHECK(fabs(g_fake.eff(3, 37) - 84) < 0.1);
  printf("rytm params H5: the XY pad's offset, HOLD past the timeout, the sum with another source; MIDI learn\n");
}

// ---- H7 exclusions
static void h7_exclusions() {
  fresh();
  machine1(0, 0, 0x00);   // track 1 plays a Sidecar machine (PLAITS): its words 9..24 are the machine's P1..P16
  g_src_t[0] = T("RT1:P3"), g_src_v[0] = 5;
  g_src_t[1] = T("RT1:FRQ"), g_src_v[1] = 5;   // its filter: fine
  g_src_t[2] = T("RT2:P3"), g_src_v[2] = 5;    // track 2: analog
  mod::set_source_fn(src);
  run_ms(10);
  CHECK(!mod::rp_active(T("RT1:P3")) && mod::rp_active(T("RT1:FRQ")) && mod::rp_active(T("RT2:P3")));
  CHECK(!g_fake.has(0, 11) && g_fake.has(0, 30) && g_fake.has(1, 11));
  // track 2 turns a Sidecar machine: its active P-word gets 0 at once
  g_pm.clear();
  machine1(1, 0, 0x08);
  run_ms(10);
  CHECK(count_op(4) == 1 && !g_fake.has(1, 11));
  // an empty slot of the machine (BT CLASSIC's P4): never sent
  machine1(2, 7, 0x7F);
  g_src_t[3] = T("RT3:P4"), g_src_v[3] = 5;
  run_ms(10);
  CHECK(!mod::rp_active(T("RT3:P4")) && !g_fake.has(2, 12));
  // the compressor words of the FX track (M0b: read from the effective array): active, sent
  CHECK(mod::rp_active(T("RFX:THR")) && mod::rp_active(T("RFX:CVOL")) && mod::rp_active(T("RFX:TIM")));
  mod::rp_offset(T("RFX:THR"), 6);
  run_ms(10);
  CHECK(g_fake.has(12, 30) && fabs(g_fake.eff(12, 30) - 70) < 0.01);
  // a list word takes no offset
  mod::rp_offset(T("RT5:FTYP"), 3);
  CHECK(!mod::rp_view(4, 32, nullptr));
  printf("rytm params H7: Sidecar machine tracks' synth / sample words, empty slots, the compressor, list words\n");
}

// ---- H8 refusals
static void h8_refusals() {
  fresh();
  mod::rytm_host_cmd(true);
  rx({0x2D, 1, 0x10, 4, 1});   // 2D 01 10 04 1: an OFFSET refused
  rx({0x2D, 1, 0x10, 5, 2});   // the Rytm's slots full
  CHECK(mod::rp_stats().refused == 1 && mod::rp_stats().full == 1);
  CHECK(mod::rytm_out_stats().refused == 0);   // the scenes / PERF / mutes keep the mod command
  mod::rytm_perf(3, 50);
  CHECK(!g_other.empty() && g_other.back()[4] == mod::kHostCmd && g_other.back()[6] == mod::kOpPerf);
  // the fake refuses a bad word (a host bug would show here): DST
  const uint8_t bad[] = {0xF0, 0x7D, 0x52, 0x54, 0x5A, 1, 4, 4, 0, 1, 0, 4, 0, 1, 0xF7};
  CHECK(g_fake.take(bad, (int)sizeof bad) && !g_fake.answers.empty() && g_fake.answers.back()[4] == 1);
  g_fake.answers.clear();
  mod::rytm_host_cmd(false);
  printf("rytm params H8: a refusal of 04..07 is counted, never the CC fallback\n");
}

// ---- H11 the velocity words
static void h11_velocity() {
  fresh();
  g_src_t[0] = T("RT2:VEL"), g_src_v[0] = 40;
  g_src_t[1] = T("RT5:VM2"), g_src_v[1] = 64;
  g_src_t[2] = T("RT1:AT4"), g_src_v[2] = -10;
  mod::set_source_fn(src);
  run_ms(2);
  const Msg& m = *last_op(4);
  bool vel = false, vm = false, at = false;
  for (int i = 0; i < m[9]; ++i) {
    const uint8_t* r = &m[10 + 4 * i];
    vel |= r[0] == 1 && r[1] == 56 && (r[2] << 7 | r[3]) == 40 * 64;
    vm |= r[0] == 4 && r[1] == 49 && (r[2] << 7 | r[3]) == 64 * 64;
    at |= r[0] == 0 && r[1] == 55 && (r[2] << 7 | r[3]) == (0x4000 - 640);
  }
  CHECK(vel && vm && at);
  // M0c [measured]: a route's amount is the displayed amount << 8 and the Rytm adds v x 4 (1/64 of a displayed step):
  // +64 on VM2 is the stock +64 amount (0x4000 in 8.8), -10 on AT4 is -10
  run_ms(20);   // (the ramp done)
  CHECK(fabs(g_fake.off(4, 49) - 64) < 1e-9 && fabs(g_fake.off(0, 55) + 10) < 1e-9 && fabs(g_fake.off(1, 56) - 40) < 1e-9);
  // VM / AT take no override (a morph on one sends nothing)
  mod::rp_morph(T("RT5:VM2"), true, 10, true, 20, 0.5f, false, false);
  CHECK(mod::rp_view(4, 49, nullptr));
  mod::RpView v;
  CHECK(mod::rp_view(4, 49, &v) && !v.morph);
  printf("rytm params H11: VEL / VM / AT as words 56 / 48..55, offsets only\n");
}

// ---- H10 the morph: a scene's lock on a Rytm word as an override's value and mix (rytm-param-mod 5.3)
static void h10_morph() {
  fresh();
  mod::Tid frq = T("RT1:FRQ"), res = T("RT1:RES"), env = T("RT1:FENV"), ftyp = T("RT1:FTYP");
  CHECK(scene::lock(0, frq, 100 << 7) && scene::lock(1, frq, 20 << 7));   // both: a + (b - a) f, mix 1
  CHECK(scene::lock(0, res, 100 << 7));                                     // A only: A's value, mix 1 - f
  CHECK(scene::lock(1, env, 100 << 7));                                     // B only: B's value, mix f
  CHECK(scene::lock(0, ftyp, 1 << 7) && scene::lock(1, ftyp, 5 << 7));     // a list word: whole values, at halfway
  scene::set_ab(0, 1);
  scene::set_fader(4096, scene::kConsole);   // f 0.25
  run_ms(60);
  printf("rytm params H10: f 0.25: FRQ %.2f RES %.2f FENV %.2f FTYP %.2f\n", g_fake.eff(0, 30), g_fake.eff(0, 31),
         g_fake.eff(0, 25), g_fake.eff(0, 32));
  CHECK(std::fabs(g_fake.eff(0, 30) - 80) < 0.6);              // 100 + (20 - 100) x 0.25
  CHECK(std::fabs(g_fake.eff(0, 31) - (64 + 0.75 * 36)) < 0.8);   // the Rytm's own 64 under the other quarter
  CHECK(std::fabs(g_fake.eff(0, 25) - (64 + 0.25 * 36)) < 0.8);
  CHECK(std::fabs(g_fake.eff(0, 32) - 1) < 0.01);
  scene::set_fader(12288, scene::kConsole);   // f 0.75: past halfway, FTYP B's
  run_ms(60);
  CHECK(std::fabs(g_fake.eff(0, 30) - 40) < 0.6 && std::fabs(g_fake.eff(0, 32) - 5) < 0.01);
  CHECK(std::fabs(g_fake.eff(0, 31) - (64 + 0.25 * 36)) < 0.8 && std::fabs(g_fake.eff(0, 25) - (64 + 0.75 * 36)) < 0.8);
  // a seq p-lock under a one-sided morph: the lock is the other side's value (A only: RES from 100 toward the lock 30)
  mod::rp_lock(0, 31, 30 << 7, 0);
  run_ms(60);
  CHECK(std::fabs(g_fake.eff(0, 31) - (100 + (30 - 100) * 0.75)) < 0.8);
  mod::rp_unlock(0, 31, 0);
  run_ms(60);
  CHECK(std::fabs(g_fake.eff(0, 31) - (64 + 0.25 * 36)) < 0.8);
  // the morph lets go (no A / B): every override released, the Rytm's own values back
  scene::set_ab(-1, -1);
  run_ms(60);
  CHECK(std::fabs(g_fake.eff(0, 30) - 64) < 0.01 && std::fabs(g_fake.eff(0, 32) - 64) < 0.01 && std::fabs(g_fake.eff(0, 25) - 64) < 0.01);
  CHECK(mod::rp_live() == 0);
  printf("rytm params H10: the morph's parts as value + mix, one-sided locks over the Rytm's own value, a p-lock as base\n");
}

// ---- the review of 2d6220d (rytm-pmod-host-review.md findings 1..8, 13)
static int64_t release_at(int t, int w, size_t from) {   // the first 05 record (t, w) with mix 0 from message `from`
  for (size_t i = from; i < g_pm.size(); ++i) {
    const Msg& m = g_pm[i];
    if (m[6] != 5) continue;
    for (int j = 0; j < m[9]; ++j) {
      const uint8_t* r = &m[10 + 5 * j];
      if (r[0] == t && (r[1] & 0x3F) == w && r[4] == 0) return (int64_t)g_pm_us[i];
    }
  }
  return -1;
}
static void moving_offsets(int n, int ms) {   // n entries on tracks 1..11 (not RT1:FRQ), each a step further every ms
  static const int kW[11] = {25, 26, 27, 28, 29, 31, 33, 34, 35, 36, 37};
  for (int i = 0; i < n; ++i) mod::rp_offset(mod::rytm_tid(1 + i % 11, kW[(i / 11) % 11]), (float)((ms + i) % 20));
}
// 1: the overrides take turns with the offsets under the cap; a release goes first
static void r1_override_under_cap() {
  fresh();
  mod::Tid frq = T("RT1:FRQ");
  mod::rp_morph(frq, true, 100, true, 100, 0.5f, false, false);
  uint64_t off_at = 0;
  size_t from = 0;
  for (int ms = 0; ms < 400; ++ms) {
    moving_offsets(40, ms);
    if (ms == 100) mod::rp_morph_off(frq), off_at = g_us, from = g_pm.size();
    run_ms(1);
  }
  int64_t rel = release_at(0, 30, from);
  printf("rytm params R1: 40 offsets moving every ms, a morph released: its 05 (mix 0) after %.1f ms\n",
         rel < 0 ? -1.0 : (double)(rel - (int64_t)off_at) / 1000.0);
  CHECK(rel >= 0 && rel - (int64_t)off_at <= 20000);   // the bound: 20 ms (one 04 and one 05 slot at the cap)
  CHECK(fabs(g_fake.eff(0, 30) - 64) < 0.01);
  int n5 = count_op(5, from), n4 = count_op(4, from);
  CHECK(n4 > 20 && n5 >= 1);
  // the override's refresh goes on while the offsets saturate (the Rytm never times it out)
  mod::rp_morph(frq, true, 90, true, 90, 0.5f, false, false);
  for (int ms = 0; ms < 600; ++ms) moving_offsets(40, ms), run_ms(1);
  CHECK(fabs(g_fake.eff(0, 30) - 90) < 0.01 && g_fake.timeouts == 0);
  mod::rp_morph_off(frq);
}
// 2: the sender's cost on core 3 (the Mac's; the Pi 4 about 10 to 30 x)
static void r2_cost() {
  fresh();
  double worst = 0;
  for (int i = 0; i < 100; ++i) mod::rp_offset(mod::rytm_tid(i % 12, 25 + i / 12), 1);
  for (int i = 0; i < 28; ++i) mod::rp_lock(i % 12, 30 + i / 12, 50 << 7, 0);
  for (int ms = 0; ms < 300; ++ms) {
    for (int i = 0; i < 100; ++i) mod::rp_offset(mod::rytm_tid(i % 12, 25 + i / 12), (float)((ms + i) % 30));
    for (int k = 0; k < 4; ++k) {
      at(g_us + 250);
      auto t0 = std::chrono::steady_clock::now();
      mod::rp_tick((uint32_t)now_us());
      double d = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      if (d > worst) worst = d;
    }
  }
  printf("rytm params R2: rp_tick worst %.1f us on this Mac (128 live entries, 100 offsets changing every ms)\n", worst);
  CHECK(worst < 500);
}
static void card_core_turn() {
  host_card_core(0);
  while (cardq::service()) {
  }
  host_card_core(3);
}
static void to_project(const char* name) {   // 0x10: the Rytm's project; its files into the store, then loaded
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x10};
  for (const char* c = name; *c; ++c) m.push_back((uint8_t)*c);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
  for (int i = 0; i < 400 && strcmp(core::project, name); ++i) {
    if (rename::state() == rename::kAsk) rename::answer(false);
    at(g_us + 20000), idle_save(), card_core_turn();
  }
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save(), card_core_turn();
  CHECK(!project_loading() && !strcmp(core::project, name));
}
// 3: a kit change with the XY pad live (a project's store: it switches with the kit): no old offset under the new kit's
// byte, ever (0.5.0 owner test: no LFO on the Rytm any more; R3b holds a source that lets go)
static void r3_kit_change() {
  CHECK(!strcmp(core::project, "RPK"));   // (main: before any hello, as a 0.4 Rytm names its project)
  fresh(127, 7);   // (kits 8 and 9: the other tests' kit 5 keeps no LFO or slot of this one's)
  CHECK(xypad::set(0, 0, T("RT2:RES"), 50));
  xypad::pos(16383, 0);
  run_ms(20);
  CHECK(g_fake.eff(1, 31) > 100);
  // the race (re-review N2): the grid due when the kit changes, the first tick's rp_tick before the layer's end_tick
  at(g_us + 6000);
  g_fake = pmfake::Rytm();   // the Rytm switched to kit 9: its entries freed
  g_fake.kit = 8, g_fake.us = g_us, g_fake.last_op_us = g_us;
  size_t from = g_pm.size();
  kit(8);
  // released at the kit change (the new kit has no XY slots): the word is not on the Rytm again
  double worst = 0;
  for (int i = 0; i < 80; ++i) {
    at(g_us + 250), mod::tick(now_us());
    worst = std::max(worst, std::max(fabs(g_fake.eff(1, 31) - 64), fabs(g_fake.eff(0, 30) - 64)));
  }
  CHECK(!xypad::active() && g_pm.size() > from && g_pm[from][6] == 6 && g_pm[from][7] == 8);
  for (size_t i = from; i < g_pm.size(); ++i)
    if (g_pm[i][6] == 4)
      for (int j = 0; j < g_pm[i][9]; ++j) CHECK(g_pm[i][12 + 4 * j] == 0 && g_pm[i][13 + 4 * j] == 0);
  CHECK(worst < 0.01 && !g_fake.has(1, 31) && !g_fake.has(0, 30));
  xypad::up();
  lfo::reset();
  printf("rytm params R3: a kit change with the XY pad live: 06, no old offset under the new kit\n");
}
// 3b: the hold itself. With a store, the 3B's lfo::kit_changed restores the whole layer at once (mod::restore_all ->
// rp_clear_all), so R3 has nothing left to blip even without the hold. A source that lets go at the layer's next tick
// (a test source here; the XY pad, the scenes without that restore) is what the hold is for: the grid due at the kit
// change, the first rp_tick before the layer's end_tick
static void r3b_kit_hold() {
  fresh();
  g_src_t[0] = T("RT2:RES"), g_src_v[0] = 20;
  mod::set_source_fn(src);
  run_ms(20);
  CHECK(fabs(g_fake.eff(1, 31) - 84) < 0.01);
  at(g_us + 6000);
  g_fake = pmfake::Rytm();
  g_fake.kit = 8, g_fake.us = g_us, g_fake.last_op_us = g_us;
  size_t from = g_pm.size();
  mod::rp_kit(8);
  g_src_t[0] = 0;   // the source let go for the new kit: the layer applies it at its next end_tick
  double worst = 0;
  for (int i = 0; i < 40; ++i) at(g_us + 250), mod::tick(now_us()), worst = std::max(worst, fabs(g_fake.eff(1, 31) - 64));
  CHECK(g_pm.size() > from && g_pm[from][6] == 6 && g_pm[from][7] == 8);
  for (size_t i = from; i < g_pm.size(); ++i)
    if (g_pm[i][6] == 4)
      for (int j = 0; j < g_pm[i][9]; ++j) CHECK(g_pm[i][12 + 4 * j] == 0 && g_pm[i][13 + 4 * j] == 0);
  CHECK(worst < 0.01);
  mod::set_source_fn(nullptr);
  printf("rytm params R3b: a kit change while a source lets go: nothing but the 06 until the layer applied it\n");
}
// 4: p-locks in bursts against the cap: the average holds, the debt is floored (the offsets never stall long)
static void r4_lock_bursts() {
  fresh();
  size_t from = g_pm.size();
  uint64_t t0 = g_us;
  for (int ms = 0; ms < 2000; ++ms) {
    if (ms % 50 == 0)
      for (int tr = 0; tr < 12; ++tr)
        for (int w = 0; w < 8; ++w) mod::rp_lock(tr, 25 + w, ((ms / 50 + w) % 2 ? 30 : 90) << 7, (uint32_t)now_us());
    for (int i = 0; i < 10; ++i) mod::rp_offset(mod::rytm_tid(i, 37), (float)((ms + i) % 20));
    run_ms(1);
  }
  size_t bytes = 0;
  uint64_t last4 = 0, gap4 = 0;
  for (size_t i = from; i < g_pm.size(); ++i) {
    bytes += g_pm[i].size();
    if (g_pm[i][6] == 4) {
      if (last4) gap4 = std::max(gap4, g_pm_us[i] - last4);
      last4 = g_pm_us[i];
    }
  }
  double secs = (double)(g_us - t0) / 1e6;
  printf("rytm params R4: 96 p-locks every 50 ms + 10 offsets: %.0f B/s, the longest 04 gap %.1f ms\n", bytes / secs,
         (double)gap4 / 1000.0);
  CHECK(bytes <= mod::kBytesPerS * secs + mod::kBytesPerS / 10 + 256);
  CHECK(gap4 <= 120000 && g_fake.timeouts == 0);
}
// 4b: one large burst of p-locks (many KB in 20 ms): the debt stops at the floor, the offsets go again within its
// 100 ms (unfloored, the grid would wait the whole burst's bytes: most of a second)
static void r4b_lock_flood() {
  fresh();
  for (int ms = 0; ms < 30; ++ms) {   // the offsets running first
    for (int i = 0; i < 5; ++i) mod::rp_offset(mod::rytm_tid(i, 37), (float)((ms + i) % 20));
    run_ms(1);
  }
  size_t from = g_pm.size();
  uint64_t burst_end = 0;
  for (int ms = 0; ms < 400; ++ms) {
    if (ms < 20)
      for (int tr = 0; tr < 12; ++tr)
        for (int w = 0; w < 10; ++w) mod::rp_lock(tr, 25 + w, (ms % 2 ? 30 : 90) << 7, (uint32_t)now_us());
    for (int i = 0; i < 5; ++i) mod::rp_offset(mod::rytm_tid(i, 37), (float)((ms + i) % 20));
    run_ms(1);
    if (ms == 19) burst_end = g_us;
  }
  size_t burst = 0;
  uint64_t first4 = 0;
  for (size_t i = from; i < g_pm.size(); ++i) {
    if (g_pm_us[i] <= burst_end) burst += g_pm[i].size();
    if (g_pm[i][6] == 4 && g_pm_us[i] > burst_end && !first4) first4 = g_pm_us[i];
  }
  printf("rytm params R4b: %zu B of p-locks in 20 ms: the next 04 %.1f ms after the burst\n", burst,
         first4 ? (double)(first4 - burst_end) / 1000.0 : -1.0);
  CHECK(burst > 4000);
  CHECK(first4 && first4 - burst_end <= 100000 + 2 * mod::kGridUs);
}
// N1: after an idle, a full table's change: one 04 per grid slot, never two in one call
static void r9_one_04_per_slot() {
  fresh();
  run_ms(500);
  size_t from = g_pm.size();
  for (int ms = 0; ms < 100; ++ms) moving_offsets(60, ms), run_ms(1);
  uint64_t last = 0;
  int n = 0;
  for (size_t i = from; i < g_pm.size(); ++i)
    if (g_pm[i][6] == 4) {
      if (last) CHECK(g_pm_us[i] - last >= mod::kGridUs);
      last = g_pm_us[i], ++n;
    }
  CHECK(n >= 10);
  printf("rytm params R9: 60 offsets after an idle: %d 04s, one per grid slot at most\n", n);
}
// ---- LFO FINAL (the owner, 2026-11-12), then the owner's test (2026-11-13): no LFO on the Rytm. The link's load, its
// saturation and the priorities stay (the XY pad, the scenes, MIDI learn): 13 layer sources stand in for 0.5.0-dev's 13
// Rytm LFOs - every track's own FRQ (the FX track: TIM), a 20 Hz sine at full depth
static void lfos_on_rytm(bool on) {
  for (int t = 0; t < 13; ++t) {
    char n[16];
    snprintf(n, sizeof n, t == 12 ? "RFX:TIM" : "RT%d:FRQ", t + 1);
    g_src_t[t] = on ? T(n) : 0, g_src_v[t] = 63, g_src_hz[t] = 20;
  }
  mod::set_source_fn(src);
}
// the longest gap between two 04s carrying (t, w), from message `from`
static uint64_t gap04(int t, int w, size_t from) {
  uint64_t last = 0, gap = 0;
  for (size_t i = from; i < g_pm.size(); ++i) {
    const Msg& m = g_pm[i];
    if (m[6] != 4) continue;
    for (int j = 0; j < m[9]; ++j)
      if (m[10 + 4 * j] == t && m[11 + 4 * j] == w) {
        if (last) gap = std::max(gap, g_pm_us[i] - last);
        last = g_pm_us[i];
      }
  }
  return last ? gap : UINT64_MAX;
}
static std::string page_texts(size_t from) {   // the remote page's 3D text items sent since g_other[from]
  std::string t = "|";
  for (size_t i = from; i < g_other.size(); ++i) {
    const Msg& m = g_other[i];
    if (m.size() > 13 && m[4] == 0x3D) t += std::string((const char*)m.data() + 12, m.size() - 13) + "|";
  }
  return t;
}
// 10: 13 moving sources on the Rytm fit the link; with 40 more moving offsets it is full (rp "sat"), the sources slower
// but never starved; gone a second after the backlog. The LFOs' warning never shows: no LFO is on the link
static void r10_link_full() {
  fresh();
  lfos_on_rytm(true);
  size_t from = g_pm.size();
  uint64_t t0 = g_us;
  run_ms(1000);
  size_t bytes = 0;
  for (size_t i = from; i < g_pm.size(); ++i) bytes += g_pm[i].size();
  printf("rytm params R10: 13 sources (T1..FX) on the Rytm at 20 Hz: %.0f B/s, the longest gap %.1f ms, saturated %d\n",
         bytes / ((double)(g_us - t0) / 1e6), (double)gap04(5, 30, from) / 1000.0, mod::rp_saturated());
  CHECK(!mod::rp_saturated() && !lfo::link_full() && gap04(5, 30, from) <= 2 * mod::kGridUs);
  // the other sources' 40 moving offsets on top: the link full
  from = g_pm.size();
  size_t ofrom = g_other.size();
  uint32_t ms = now_ms();
  rx({0x30, 1, 1, 1, 0});   // UP + LFO: the page
  int ok = 0;
  for (int k = 0; k < 600; ++k) {
    moving_offsets(40, k), run_ms(1);
    ui::remote_tick(ms += 1);
    ok += mod::rp_saturated();
  }
  CHECK(mod::rp_saturated() && !lfo::link_full() && ok > 400);
  char j[lfo::kJsonCap];
  CHECK(lfo::json(j, sizeof j) > 0 && strstr(j, "\"full\":0"));
  CHECK(mod::rp_json(j, sizeof j) > 0 && strstr(j, "\"sat\":1"));
  std::string pt = page_texts(ofrom);
  CHECK(pt.find("RYTM LINK FULL") == std::string::npos);
  uint64_t worst = 0;
  for (int t = 0; t < 12; ++t) worst = std::max(worst, gap04(t, 30, from));
  printf("rytm params R10: + 40 moving offsets: saturated (%d of 600 ms), every source's record still every %.1f ms at most\n",
         ok, (double)worst / 1000.0);
  CHECK(worst <= 60000 && g_fake.timeouts == 0);
  // the offsets hold still: the backlog gone, the saturation a second later
  run_ms(200);
  CHECK(mod::rp_saturated());   // (held: no flicker)
  run_ms(1000);
  CHECK(!mod::rp_saturated() && !lfo::link_full());
  lfos_on_rytm(false);
  for (int k = 0; k < 400; ++k) moving_offsets(40, k), run_ms(1);
  CHECK(mod::rp_saturated() && !lfo::link_full());
  for (int k = 0; k < 10; ++k) ui::remote_tick(ms += 10);
  rx({0x30, 1, 1, 1, 0});   // (the page left again)
  for (int k = 0; k < 10; ++k) ui::remote_tick(ms += 10);
  mod::set_source_fn(nullptr);
  printf("rytm params R10: the link full: rp \"sat\":1; no LFO on it, so no \"RYTM LINK FULL\" and \"full\":0\n");
}
// 11: the link full of source updates: a morph's release, a p-lock and a source's own release keep priority
static void r11_priority() {
  fresh();
  lfos_on_rytm(true);
  for (int k = 0; k < 300; ++k) moving_offsets(40, k), run_ms(1);
  CHECK(mod::rp_saturated());
  int k = 300;
  // a morph's release: its 05 (mix 0) at the next 05 slot, never behind the offsets' turn
  mod::Tid res = T("RT1:RES");
  mod::rp_morph(res, true, 100, true, 100, 0.5f, false, false);
  for (int i = 0; i < 50; ++i) moving_offsets(40, k++), run_ms(1);
  CHECK(std::fabs(g_fake.eff(0, 31) - 100) < 0.01);
  mod::rp_morph_off(res);
  size_t from = g_pm.size();
  uint64_t t0 = g_us;
  for (int i = 0; i < 30; ++i) moving_offsets(40, k++), run_ms(1);
  int64_t rel = release_at(0, 31, from);
  printf("rytm params R11: saturated: a morph's release after %.2f ms\n", rel < 0 ? -1.0 : (double)(rel - (int64_t)t0) / 1000.0);
  CHECK(rel >= 0 && rel - (int64_t)t0 <= (int64_t)mod::kGridUs + 1000 && std::fabs(g_fake.eff(0, 31) - 64) < 0.01);
  // a p-lock: its jump at its due time
  from = g_pm.size();
  uint64_t due = g_us + 3000;
  mod::rp_lock(2, 31, 90 << 7, (uint32_t)due);
  for (int i = 0; i < 10; ++i) moving_offsets(40, k++), run_ms(1);
  uint64_t lock_at = 0;
  for (size_t i = from; i < g_pm.size() && !lock_at; ++i)
    if (g_pm[i][6] == 5)
      for (int j = 0; j < g_pm[i][9]; ++j)
        if (g_pm[i][10 + 5 * j] == 2 && g_pm[i][11 + 5 * j] == (31 | 0x40)) lock_at = g_pm_us[i];
  printf("rytm params R11: saturated: a p-lock's jump %.2f ms after its due time\n", lock_at ? (double)(lock_at - due) / 1000.0 : -1.0);
  CHECK(lock_at >= due && lock_at - due <= 500);
  // a source let go (T5's, as an LFO turned off was): its 0 at the next 04 slot, first in its message
  g_src_t[4] = 0;
  from = g_pm.size();
  t0 = g_us;
  for (int i = 0; i < 30; ++i) moving_offsets(40, k++), run_ms(1);
  uint64_t zero_at = 0;
  bool first = false;
  for (size_t i = from; i < g_pm.size() && !zero_at; ++i)
    if (g_pm[i][6] == 4)
      for (int j = 0; j < g_pm[i][9]; ++j)
        if (g_pm[i][10 + 4 * j] == 4 && g_pm[i][11 + 4 * j] == 30 && !g_pm[i][12 + 4 * j] && !g_pm[i][13 + 4 * j])
          zero_at = g_pm_us[i], first = j == 0;
  printf("rytm params R11: saturated: a source's release (T5) after %.2f ms, first in its 04: %d\n",
         zero_at ? (double)(zero_at - t0) / 1000.0 : -1.0, first);
  CHECK(zero_at && zero_at - t0 <= mod::kGridUs + 1000 && first);
  CHECK(g_fake.timeouts == 0 && mod::rp_stats().rel_cut > 0);
  lfos_on_rytm(false);
  run_ms(1500);
  mod::set_source_fn(nullptr);
}
// 12: the release cut (a release past a spent credit) carries releases only, and the cap holds: saturated, an override's
// release every 5 ms for 2 s stays within 1.1 x kBytesPerS, and every message sent in a tick that cut holds releases
static void r12_release_cut_only_releases() {
  fresh();
  for (int k = 0; k < 300; ++k) moving_offsets(40, k), run_ms(1);
  CHECK(mod::rp_saturated());
  size_t from = g_pm.size();
  uint64_t t0 = g_us;
  uint32_t cuts0 = mod::rp_stats().rel_cut;
  int cut_msgs = 0, bad_cut_msgs = 0, slot = 0;
  for (int k = 0; k < 2000; ++k) {
    moving_offsets(40, 300 + k);
    if (k % 5 == 0) {   // a morph on (a change) 100 ms before its release: the release stream, one every 5 ms
      int n = slot++;
      mod::rp_morph(mod::rytm_tid(n % 10, 40 + n / 10 % 8), true, 50, true, 50, 0.5f, false, false);
      if (n >= 20) {
        int o = n - 20;
        mod::rp_morph_off(mod::rytm_tid(o % 10, 40 + o / 10 % 8));
      }
    }
    for (int q = 0; q < 4; ++q) {
      size_t m0 = g_pm.size();
      uint32_t c0 = mod::rp_stats().rel_cut;
      at(g_us + 250), mod::tick(now_us());
      if (mod::rp_stats().rel_cut == c0) continue;
      for (size_t i = m0; i < g_pm.size(); ++i) {
        const Msg& m = g_pm[i];
        bool rel = true;
        for (int j = 0; j < m[9]; ++j) {
          const uint8_t* r = m[6] == 4 ? &m[10 + 4 * j] : &m[10 + 5 * j];
          rel = rel && (m[6] == 4 ? !r[2] && !r[3] : r[4] == 0);
        }
        ++cut_msgs, bad_cut_msgs += !rel;
      }
    }
  }
  size_t bytes = 0;
  for (size_t i = from; i < g_pm.size(); ++i) bytes += g_pm[i].size();
  double bps = bytes / ((double)(g_us - t0) / 1e6);
  printf("rytm params R12: saturated + a release every 5 ms: %.0f B/s (cap %d), %d release-cut messages, %d with a change, "
         "%u cuts\n", bps, (int)mod::kBytesPerS, cut_msgs, bad_cut_msgs, (unsigned)(mod::rp_stats().rel_cut - cuts0));
  CHECK(mod::rp_stats().rel_cut > cuts0 && cut_msgs > 0);
  CHECK(bad_cut_msgs == 0);
  CHECK(bps <= 1.1 * mod::kBytesPerS);
  for (int n = 0; n < 40; ++n) mod::rp_morph_off(mod::rytm_tid(n % 10, 40 + n / 10 % 8));
  run_ms(1500);
}
// 13: the saturation timers run across the 32-bit microsecond wrap (us / 1000 jumps back every ~71.6 min): a backlog that
// began just before it is not "100 ms on end" at the wrap
static void r13_saturation_across_wrap() {
  g_us = (1ull << 32) - 40000;   // 40 ms before the wrap
  fresh();
  CHECK(now_us() > 0xFFFF0000u);
  uint64_t t0 = g_us;
  int64_t first = -1;
  for (int k = 0; k < 400; ++k) {
    moving_offsets(40, k), run_ms(1);
    if (first < 0 && mod::rp_saturated()) first = (int64_t)(g_us - t0);
  }
  CHECK(g_us > (1ull << 32) + 100000 && now_us() < 0x10000000u);
  printf("rytm params R13: the load starts 40 ms before the us wrap: saturated first after %.1f ms (>= 100)\n",
         first / 1000.0);
  CHECK(first >= (int64_t)mod::kSatMs * 1000);
  g_us = 1000000 + (g_us & 0xFFFFFFFFull);   // (the next tests: a plain clock)
  host_fake_time(true, g_us);
  mod::rp_reset();
}
// 5: a fader a hair outside 0..1 gives a full override, never a release; no byte above 0x7f is ever sent
static void r5_mix_clamp() {
  fresh();
  mod::rp_morph(T("RT1:FRQ"), true, 100, false, 0, -0.01f, false, false);   // A only at f < 0: mix 1
  mod::rp_morph(T("RT1:RES"), false, 0, true, 20, 1.01f, false, false);     // B only at f > 1: mix 1
  run_ms(10);
  mod::RpView v;
  CHECK(mod::rp_view(0, 30, &v) && v.mix == 127 && mod::rp_view(0, 31, &v) && v.mix == 127);
  CHECK(fabs(g_fake.eff(0, 30) - 100) < 0.01 && fabs(g_fake.eff(0, 31) - 20) < 0.01);
  for (const Msg& m : g_pm)
    for (size_t i = 1; i + 1 < m.size(); ++i) CHECK(m[i] <= 0x7F);
  CHECK(mod::rp_stats().bad == 0);
  char o[400];
  CHECK(mod::command("rp", o, sizeof o) && strstr(o, "bad 0"));
  CHECK(mod::rp_json(o, sizeof o) > 0 && strstr(o, "\"bad\":0"));
  printf("rytm params R5: mix clamped to 0..127 at the source\n");
}
// 6: a p-lock due across a long link loss (past the 32-bit microsecond wrap) is sent after the next hello
static void r6_link_loss_due() {
  fresh();
  mod::rp_lock(0, 31, 30 << 7, (uint32_t)now_us() + 1000);
  mod::rp_link_lost();
  g_us += 40ull * 60 * 1000000, host_fake_time(true, g_us);   // 40 minutes
  g_fake = pmfake::Rytm();
  g_fake.kit = 4, g_fake.us = g_us, g_fake.last_op_us = g_us;
  hello(127);
  run_ms(10);
  CHECK(g_fake.has(0, 31) && fabs(g_fake.eff(0, 31) - 30) < 0.01);
  mod::rp_unlock(0, 31, 0);
  printf("rytm params R6: a p-lock pending across a 40 min link loss goes out after the hello\n");
}
// 7: the ramp is each entry's own update interval: a slow LFO's line, many entries' rounds
static void r7_ramp() {
  fresh();
  g_src_t[0] = T("RT1:FRQ"), g_src_v[0] = 0;
  mod::set_source_fn(src);
  for (int i = 0; i < 12; ++i) g_src_v[0] = 0.1f * (float)i, run_ms(30);   // 1/10 step every 30 ms
  const Msg* m = last_op(4);
  printf("rytm params R7: a change every 30 ms: ramp %d blocks\n", m ? (*m)[8] : -1);
  CHECK(m && (*m)[8] >= 40 && (*m)[8] <= 50);   // 30 ms = 45 blocks
  g_src_t[0] = 0;
  mod::set_source_fn(nullptr);
  for (int ms = 0; ms < 50; ++ms) moving_offsets(60, ms), run_ms(1);
  m = last_op(4);
  printf("rytm params R7: 60 entries moving every ms: ramp %d blocks\n", m ? (*m)[8] : -1);
  CHECK(m && (*m)[8] >= 22);   // 3 rounds of 28 at >= 5 ms
}
// 8: rp=clear clears (the entries gone, the Rytm's too)
static void r8_rp_clear() {
  fresh();
  mod::rp_offset(T("RT1:FRQ"), 5);
  mod::rp_lock(1, 31, 20 << 7, 0);
  run_ms(10);
  CHECK(g_fake.has(0, 30) && g_fake.has(1, 31));
  char o[200];
  CHECK(mod::command("rp=clear", o, sizeof o) && strstr(o, "CLEAR"));
  run_ms(200);
  CHECK(mod::rp_live() == 0 && !g_fake.has(0, 30) && !g_fake.has(1, 31));
  printf("rytm params R8: rp=clear clears the Rytm and forgets the entries\n");
}

// ---- the M0 review's gaps (rytm-pmod-m0-review.md F1, F2)
// F1: every row of the machine table against rytm-param-mod's M0d table [measured, ROM 1.73] as it was given ("--"
// an empty slot), so a slip in any of the 53 rows shows
static void m0_machine_table() {
  struct Row { int m; const char* name; const char* l[8]; };
  static const Row kDoc[] = {
      {0, "BD HARD", {"LEV", "TUN", "DEC", "HLD", "SWT", "SNP", "WAV", "TIC"}},
      {1, "BD CLASSIC", {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "WAV", "TRA"}},
      {2, "SD HARD", {"LEV", "TUN", "DEC", "SWD", "TIC", "NOD", "NOL", "SWT"}},
      {3, "SD CLASSIC", {"LEV", "TUN", "DEC", "DET", "SNP", "NOD", "NOL", "BAL"}},
      {4, "RS HARD", {"LEV", "TUN", "DEC", "SWD", "TIC", "NOL", "SYM", "SWT"}},
      {5, "RS CLASSIC", {"LEV", "T1", "DEC", "BAL", "T2", "SYM", "NOL", "TIC"}},
      {6, "CP CLASSIC", {"LEV", "TON", "NOD", "NUM", "RAT", "NOL", "RND", "CPD"}},
      {7, "BT CLASSIC", {"LEV", "TUN", "DEC", "--", "NOL", "SNP", "SWD", "--"}},
      {8, "XT CLASSIC", {"LEV", "TUN", "DEC", "SWD", "SWT", "NOD", "NOL", "TON"}},
      {9, "CH CLASSIC", {"LEV", "TUN", "DEC", "COL", "--", "--", "--", "--"}},
      {10, "OH CLASSIC", {"LEV", "TUN", "DEC", "COL", "--", "--", "--", "--"}},
      {11, "CY CLASSIC", {"LEV", "TUN", "DEC", "COL", "TON", "--", "--", "--"}},
      {12, "CB CLASSIC", {"LEV", "TUN", "DEC", "DET", "PW1", "PW2", "--", "--"}},
      {13, "BD FM", {"LEV", "TUN", "DEC", "FMA", "SWT", "FMS", "FMD", "FMT"}},
      {14, "SD FM", {"LEV", "TUN", "DEC", "FMT", "FMD", "NOD", "NOL", "FMA"}},
      {15, "UT NOISE", {"LEV", "LPF", "DEC", "HPF", "LPQ", "ATK", "SWT", "SWD"}},
      {16, "UT IMPULSE", {"LEV", "ATK", "DEC", "HLD", "SHP", "REP", "NUM", "POL"}},
      {17, "CH METALLIC", {"LEV", "TUN", "DEC", "--", "--", "--", "--", "--"}},
      {18, "OH METALLIC", {"LEV", "TUN", "DEC", "--", "--", "--", "--", "--"}},
      {19, "CY METALLIC", {"LEV", "TUN", "DEC", "TON", "TRD", "--", "--", "--"}},
      {20, "CB METALLIC", {"LEV", "TUN", "DEC", "DET", "PW1", "PW2", "--", "--"}},
      {21, "BD PLASTIC", {"LEV", "TUN", "DEC", "TYP", "MOD", "SWT", "SWD", "TIC"}},
      {22, "BD SILKY", {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "DUS", "CLK"}},
      {23, "SD NATURAL", {"LEV", "TUN", "BDY", "DEC", "BAL", "LPF", "HPF", "RES"}},
      {24, "HH BASIC", {"LEV", "TUN", "DEC", "TON", "TRD", "RST", "NOL", "NOD"}},
      {25, "CY RIDE", {"LEV", "TUN", "DEC", "TYP", "HIT", "C1", "C2", "C3"}},
      {26, "BD SHARP", {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "WAV", "TIC"}},
      {27, "DISABLE", {"--", "--", "--", "--", "--", "--", "--", "--"}},
      {28, "SY DUAL VCO", {"LEV", "TUN", "DEC", "DET", "DEC", "BAL", "BND", "CFG"}},
      {29, "SY CHIP", {"LEV", "TUN", "DEC", "OF2", "OF3", "OF4", "WAV", "SPD"}},
      {30, "BD ACOUSTIC", {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "WAV", "IMP"}},
      {31, "SD ACOUSTIC", {"LEV", "TUN", "BDY", "NOD", "NOL", "HLD", "SWD", "IMP"}},
      {32, "SY RAW", {"LEV", "TUNE", "DEC2", "DET", "NLEV", "WAV1", "WAV2", "BAL"}},
      {33, "HH LAB", {"LEV", "OSC1", "DEC", "OSC2", "OSC3", "OSC4", "OSC5", "OSC6"}},
      {34, "BT IMPROVED", {"--", "--", "--", "--", "--", "--", "--", "--"}},
      {35, "BD HARD 2", {"TUN", "LEV", "HLD", "DEC", "SWT", "SNP", "WAV", "TIC"}},
      {36, "BD CLASSIC 2", {"TUN", "LEV", "HLD", "DEC", "SWT", "SWD", "WAV", "TRA"}},
      {37, "SD HARD 2", {"SWD", "DEC", "LEV", "TIC", "NOD", "NOL", "TUN", "SWT"}},
      {38, "SD CLASSIC 2", {"TUN", "BAL", "DET", "DEC", "NOD", "SNP", "NOL", "LEV"}},
      {39, "RS HARD 2", {"SWD", "DEC", "LEV", "TIC", "NOL", "SYM", "TUN", "SWT"}},
      {40, "RS CLASSIC 2", {"BAL", "DEC", "T1", "T2", "LEV", "TIC", "NOL", "SYM"}},
      {41, "BD FM 2", {"TUN", "LEV", "FMA", "DEC", "SWT", "FMS", "FMD", "FMT"}},
      {42, "RS FM 2", {"TUN", "FMA", "FMT", "DEC", "NOD", "FMD", "NOL", "LEV"}},
      {43, "FUNC. GEN.", {"--", "--", "--", "--", "--", "--", "--", "--"}},
      {44, "BD TEST", {"TU1", "TU2", "LE1", "LE2", "WF1", "WF2", "FM", "RM"}},
      {45, "SD TEST", {"TU1", "TU2", "LE1", "LE2", "TUN", "DEC", "WAV", "FRO"}},
      {46, "RS TEST", {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"}},
      {47, "BT TEST", {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"}},
      {48, "LT TEST", {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"}},
      {49, "MT TEST", {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"}},
      {50, "HH TEST", {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"}},
      {51, "CY TEST", {"TUN", "SEL", "50", "500", "80", "800", "8000", "VCA"}},
      {52, "CB TEST", {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"}},
  };
  CHECK(sizeof kDoc / sizeof kDoc[0] == 53);
  for (const Row& r : kDoc) {
    CHECK(r.m == (int)(&r - kDoc));
    for (int w = 9; w <= 16; ++w) {
      const char* want = strcmp(r.l[w - 9], "--") ? r.l[w - 9] : "";
      const char* got = mod::rp_machine_label(r.m, w);
      if (!got || strcmp(got, want)) printf("  machine %d %s word %d: \"%s\", the ROM \"%s\"\n", r.m, r.name, w, got ? got : "(null)", want);
      CHECK(got && !strcmp(got, want));
    }
  }
  printf("rytm params M0 F1: the 53 machines' P1..P8 labels as the ROM's table\n");
}
// F2: the RFX list's indices (an LFO file keeps the word, the web and the pages the index) and the touch picker's lists (pi/ui/sidecar_xy.c kRtP / kRfxP) the core's own, name for name
static std::vector<std::string> c_list(const std::string& src, const char* decl) {
  std::vector<std::string> v;
  size_t a = src.find(decl);
  CHECK(a != std::string::npos);
  size_t e = src.find("};", a);
  for (size_t q = src.find('"', a); q < e; q = src.find('"', q + 1)) {
    size_t z = src.find('"', q + 1);
    v.push_back(src.substr(q + 1, z - q - 1));
    q = z;
  }
  return v;
}
static void m0_rfx_lists() {
  struct Pin { int p; const char* n; };
  for (const Pin& x : {Pin{3, "RFX:TIM"}, Pin{20, "RFX:SYM"}, Pin{21, "RFX:THR"}, Pin{22, "RFX:MUP"}, Pin{23, "RFX:MIX"},
                       Pin{24, "RFX:CVOL"}})
    CHECK(lfo::tid_of(lfo::kTRfx, x.p) == T(x.n));
  // (an LFO on RFX:MIX saved and read back: test_lfo's kit-file test, the file keeping word 36)
  // the touch picker's lists: the core's names in the core's order
  FILE* f = fopen("../pi/ui/sidecar_xy.c", "rb");
  CHECK(f);
  std::string src;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) src.append(buf, n);
  fclose(f);
  struct L { const char* decl; int t; };
  for (const L& l : {L{"kRtP[] = {", lfo::kTRt}, L{"kRfxP[] = {", lfo::kTRfx}}) {
    std::vector<std::string> pi = c_list(src, l.decl);
    CHECK((int)pi.size() == lfo::params_of(l.t));
    for (int p = 0; p < (int)pi.size(); ++p) {
      char nm[24];
      mod::name(lfo::tid_of(l.t, p), nm, sizeof nm);
      CHECK(pi[p] == std::string(strchr(nm, ':') + 1));
    }
  }
  printf("rytm params M0 F2: RFX indices pinned, the touch picker's lists the core's\n");
}

// the LFO page's TGT / PAR display order (lfo.h): 0.5.0 owner test - no LFO on the Rytm, A's list is B's: OFF, the 8
// voices once each (labelled and ordered by the track each is shown under), ids 9..52; the stored ids never change
static void h12_target_order() {
  fresh();
  machines(0, 0x7F);   // every track stock analog
  machine1(4, 10, 0x08);   // BT on a Sidecar machine (voice 1)
  machine1(3, 10, 0x08);   // CP on a Sidecar machine (voice 7, shared with RS)
  CHECK(lfo::target_count() == lfo::kTRt);
  lfo::Settings s;
  lfo::defaults(&s);
  s.target = (uint8_t)(lfo::kTVoice + 7), s.param = 0;   // a stored V8 (RS / CP): shown under CP, the track on the machine
  CHECK(!strcmp(lfo::target_label(s.target), "CP"));
  // the voices in their tracks' order, each once (V8 under CP, the pair's Sidecar track), then FX1
  static const char* const kTr[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
  s.target = 0, s.param = 0;
  int last = -1;
  for (int i = 0; i < 8; ++i) {
    CHECK(lfo::target_step(&s, 1));
    CHECK(lfo::target_pos(s.target) == 1 + i && s.target >= lfo::kTVoice && s.target < lfo::kTVoice + 8);
    int tr = -1;
    for (int k = 0; k < 12; ++k)
      if (!strcmp(lfo::target_label(s.target), kTr[k])) tr = k;
    CHECK(tr > last);   // ascending track order, no track twice
    last = tr;
    if (s.target == lfo::kTVoice + 7) CHECK(tr == 3);
  }
  CHECK(lfo::target_step(&s, 1) && s.target == lfo::kTFx);
  s.target = lfo::kTFx;
  lfo::target_step(&s, 1000);
  CHECK(s.target == lfo::kTRt - 1);   // the last: X8, no RT / RFX
  for (int t = lfo::kTRt; t <= lfo::kTRfx; ++t) CHECK(lfo::target_pos(t) < 0);
  // stored ids round-trip: every id's position maps back, and a voice target keeps its id and param
  for (int t = 0; t < lfo::kTRt; ++t) CHECK(lfo::target_pos(t) >= 0 && lfo::target_pos(t) < lfo::target_count());
  // a stored voice param is never rewritten by reading it. Its PAR row: a voice's list is the params its machine names
  // plus the stored one (lfo.h), in param order - so its index is the named params below it. Since fix/poly-user's 33
  // hint (voices.cpp voice_machine_hint) V3 has a known machine here before any report (P1..P8 named, P10 not): the
  // index is 8 of 9 rows, not 9 of 16 (all 16 only while the machine names none)
  s.target = (uint8_t)(lfo::kTVoice + 2), s.param = 9;   // V3 (SD), P10
  int named = 0, below = 0;
  bool kept = false;   // P10 itself unnamed: a kept row
  for (int k = 0; k < 16; ++k) {
    char nb[8];
    const bool n = strcmp(lfo::param_name(s.target, k, nb, sizeof nb), "-") != 0;
    named += n, below += n && k < 9;
    if (k == 9) kept = !n;
  }
  printf("rytm param: V3 (machine %d) names %d params; a stored P10 is row %d of %d\n", ui::remote_voice_machine(2), named,
         lfo::param_index(s), lfo::param_count(s));
  CHECK(lfo::target_pos(s.target) == 1 + 1);
  if (!named) CHECK(lfo::param_index(s) == 9 && lfo::param_count(s) == 16);   // nothing named: all 16
  else CHECK(lfo::param_index(s) == below && lfo::param_count(s) == named + (kept ? 1 : 0));
  lfo::param_step(&s, 0);
  CHECK(s.target == lfo::kTVoice + 2 && s.param == 9);   // read, stepped by 0: still P10
  printf("rytm param: the LFO target order (no Rytm target)\n");
}

// merge fix (fix/050-owner-test + fix/poly-user): a voice whose machine is known from a 33 alone (voice_machine_hint, no
// report) and names P1..P8 only. A stored P12 is a kept row after the named ones - read, it stays P12 (the page shows
// "P12 -"); PAR down leaves it for P8 and the kept row is gone; it is never rewritten by a read or a step of 0
static void h12b_kept_row() {
  fresh();
  machines(0, 0x7F);
  machine1(4, 10, 0x08);   // BT on machine 0x08: its voice V2's machine known from the 33 (the hint)
  CHECK(ui::remote_track_voice(4) == 1 && ui::remote_voice_machine(1) == 0x08);
  lfo::Settings s;
  lfo::defaults(&s);
  s.target = (uint8_t)(lfo::kTVoice + 1), s.param = 11;   // V2 P12: not named by 0x08
  char nb[8];
  CHECK(!strcmp(lfo::param_name(s.target, 11, nb, sizeof nb), "-") && strcmp(lfo::param_name(s.target, 7, nb, sizeof nb), "-"));
  CHECK(lfo::param_count(s) == 9 && lfo::param_index(s) == 8);   // P1..P8, then the kept P12
  lfo::param_step(&s, 0);
  CHECK(s.param == 11);
  lfo::param_step(&s, 1);
  CHECK(s.param == 11);   // the end holds
  CHECK(lfo::set(0, s) && lfo::get(0).param == 11);   // stored as it is
  lfo::param_step(&s, -1);
  CHECK(s.param == 7 && lfo::param_count(s) == 8 && lfo::param_index(s) == 7);   // P8; the kept row is gone
  lfo::param_step(&s, 1);
  CHECK(s.param == 7);
  lfo::reset();
  printf("rytm param: a 33-known machine (no report): a stored unnamed P12 is a kept row, never rewritten\n");
}

// walk the WHOLE TGT list (one list for A and B since the owner's test) and every PAR list, forward
// and back: every step moves, no entry repeats, every entry is reachable, nothing stored as RT / RFX is landed on, every
// PAR list is the target's own
static void walk_all(const char* what) {
  lfo::Settings s;
  lfo::defaults(&s);
  s.target = 0, s.param = 0;
  const int n = lfo::target_count();
  CHECK(n == lfo::kTRt);
  std::vector<int> seen = {0};
  for (int pos = 1; pos < n; ++pos) {
    if (!lfo::target_step(&s, 1)) { printf("walk %s: TGT stuck at %d\n", what, pos); CHECK(false); }
    CHECK(lfo::target_pos(s.target) == pos);
    CHECK(s.target < lfo::kTRt && lfo::target_ok(0, s.target) && lfo::target_ok(1, s.target));   // never an RT / RFX
    for (int x : seen) CHECK(x != s.target);                      // no repeats
    seen.push_back(s.target);
    const int np = lfo::param_count(s);
    lfo::Settings q = s;
    std::vector<int> ps = {q.param};
    for (int k = 0; k < 100; ++k) {
      lfo::Settings r = q;
      lfo::param_step(&q, 1);
      if (q.param != r.param) {
        for (int x : ps) CHECK(x != q.param);   // every step moves to a new param
        ps.push_back(q.param);
      }
    }
    CHECK(q.target == s.target && lfo::param_index(q) == np - 1);   // the end of the target's own list
    for (int k = 0; k < 100; ++k) lfo::param_step(&q, -1);
    CHECK(q.target == s.target && lfo::param_index(q) == 0);
  }
  CHECK((int)seen.size() == lfo::kTRt);   // every id below RT1 reached exactly once
  CHECK(!lfo::target_step(&s, 1));   // the last entry: the end holds
  for (int pos = n - 2; pos >= 0; --pos) {
    CHECK(lfo::target_step(&s, -1));
    CHECK(lfo::target_pos(s.target) == pos);
  }
  CHECK(s.target == 0);
  lfo::Settings r;
  lfo::defaults(&r);
  r.target = (uint8_t)lfo::kTRt;
  CHECK(lfo::target_pos(r.target) < 0 && !lfo::target_step(&r, 1));   // a stored Rytm target is not in the list
}

static void h13_target_walks() {
  static const int kPair[4][2] = {{2, 3}, {6, 7}, {8, 9}, {10, 11}};
  fresh();
  // (b) no Rytm reported yet: every machine unknown
  walk_all("unknown");
  // (a) both tracks of every pair on a Sidecar machine
  machines(10, 0x08);
  walk_all("all sidecar");
  // (c) one Sidecar, one stock, each way round
  for (int flip = 0; flip < 2; ++flip) {
    machines(0, 0x7F);
    for (auto& pr : kPair) machine1(pr[flip], 10, 0x08);
    walk_all(flip ? "CP/HT/OH/CB sidecar" : "RS/MT/CH/CY sidecar");
  }
  machines(0, 0x7F);   // all stock
  walk_all("all stock");
  // (d) a foreign box: the old id order, voices land on P2 again
  static device::Screen sc = {{"A", "B", "C", "D", "E", "F", "G", "H"}, ""};
  static device::Profile pf;
  pf.screen = &sc;
  device::g_link.box = &pf;
  walk_all("foreign box");
  lfo::Settings f;
  lfo::defaults(&f);
  f.target = 0;
  lfo::target_step(&f, 1);
  CHECK(f.target == lfo::kTVoice && f.param == 1 && lfo::target_pos(lfo::kTRt) < 0);
  device::g_link.box = nullptr;
  printf("rytm param: the LFO target walks\n");
}

int main() {
  char tmpl[] = "/tmp/core_rytmparam_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  hooks.to_rytm = ::out;
  cardq::set_worker(true);
  to_project("RPK");
  h1_names();
  h2_encoding();
  h3_policy();
  h4_lfo();
  h5_xy();
  h7_exclusions();
  h8_refusals();
  h10_morph();
  h11_velocity();
  h12_target_order();
  h12b_kept_row();
  h13_target_walks();
  r1_override_under_cap();
  r2_cost();
  r3_kit_change();
  r3b_kit_hold();
  r4_lock_bursts();
  r4b_lock_flood();
  r9_one_04_per_slot();
  r10_link_full();
  r11_priority();
  r12_release_cut_only_releases();
  r13_saturation_across_wrap();
  r5_mix_clamp();
  r6_link_loss_due();
  r7_ramp();
  r8_rp_clear();
  m0_machine_table();
  m0_rfx_lists();
  CHECK(mod::rp_stats().bad == 0);
  printf("test_rytm_param: PASS\n");
  return 0;
}
