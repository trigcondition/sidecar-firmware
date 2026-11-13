// core/tests/test_master.cpp - the master FX's core half (core/fx/master.h, docs/MASTER-FX.md): which TYPEs a slot takes, the
// send TYPEs against the send FX's own return (one code path), every TYPE in stereo under noise, AMP outside the voices' cap
// and as an L/R pair, the card per kit and an old kit's file unchanged, the console and /status, the control core's time
// and no card on it, the cost of a full chain. The OUT fill's side: pi/engine/tests/test_masterfx_pi.cpp.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "timing.h"
#include "core.h"
#include "fmt/fmt.h"
#include "fx/amp.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/send_fx.h"
#include "msamp/cardq.h"
#include "store/store.h"
#include "ui/extra.h"
#include "ui/ui.h"

using namespace core;
using Clock = std::chrono::steady_clock;

static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }
static void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { memcpy(&b[at], &v, 2); }
static uint32_t crc32_of(const uint8_t* d, size_t n, size_t skip) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    if (i >= skip && i < skip + 4) continue;
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
static std::vector<uint8_t> make_namb(const std::vector<float>& w) {   // as test_amp.cpp's
  const size_t cfg = 4 + 64;
  size_t woff = (80 + 4 + cfg + 3) & ~(size_t)3, size = woff + 4 * w.size();
  std::vector<uint8_t> b(size, 0);
  put32(b, 0, 0x4E414D42u), put16(b, 4, 1), put32(b, 8, (uint32_t)size), put32(b, 12, (uint32_t)woff);
  put32(b, 16, (uint32_t)w.size()), put32(b, 20, (uint32_t)(4 + cfg));
  double rate = 48000.0;
  memcpy(&b[32 + 4], &rate, 8);
  b[80] = 3, put16(b, 82, (uint16_t)cfg);
  size_t wn = 84, la = wn + 4;
  b[wn] = 1, b[wn + 1] = 0, b[wn + 2] = 1, b[wn + 3] = 0;
  put16(b, la + 0, 1), put16(b, la + 2, 1), put16(b, la + 4, 1), put16(b, la + 6, 3), put16(b, la + 8, 3);
  put16(b, la + 10, 16), b[la + 12] = 0, b[la + 13] = 23;
  memcpy(&b[woff], w.data(), 4 * w.size());
  put32(b, 24, crc32_of(b.data(), size, 24));
  return b;
}
static std::string rd(const std::string& p) {
  std::string s;
  if (FILE* f = fopen(p.c_str(), "rb")) {
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
  }
  return s;
}
static void wr(const std::string& p, const void* d, size_t n) {
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(d, 1, n, f);
  fclose(f);
}
static void wrs(const std::string& p, const char* t) { wr(p, t, strlen(t)); }
static void card_core_turn() {
  host_card_core(0);
  while (cardq::service()) {}
  host_card_core(3);
}
static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save(), card_core_turn();
  CHECK(!project_loading());
}
static void settle() {
  for (int i = 0; i < 8000; ++i) amp::step(), card_core_turn();
}
// n frames of a stereo test signal through the render side, in 12-frame blocks
static std::vector<float> run(int n, float gl = 0.2f, float gr = 0.15f, uint32_t seed = 1) {
  std::vector<float> o;
  uint32_t r = seed;
  for (int i = 0; i < n; i += 12) {
    float y[12][2];
    for (int j = 0; j < 12; ++j) {
      r = r * 1664525u + 1013904223u;
      float nz = (float)(int32_t)r * (1.0f / 2147483648.0f);
      y[j][0] = gl * nz, y[j][1] = gr * std::sin(0.02f * (float)(i + j));
    }
    master::render(y, 12);
    for (int j = 0; j < 12; ++j) o.push_back(y[j][0]), o.push_back(y[j][1]);
  }
  return o;
}
static double rms(const std::vector<float>& x, size_t from, int ch) {
  double s = 0;
  size_t k = 0;
  for (size_t i = from * 2 + ch; i < x.size(); i += 2, ++k) s += x[i] * x[i];
  return std::sqrt(s / std::max<size_t>(k, 1));
}
static void all_off() {
  ins::reset();
}
static void set(int i, int t) { ins::set_type(master::chain(i), master::slot(i), t); }

static void types() {
  all_off();
  // a send TYPE is not a voice's; AMP is not a bus's
  ins::set_type(0, 0, ins::kMSpace);
  CHECK(ins::control.type[0][0] == ins::kOff);
  ins::set_type(ins::kBus0, 0, ins::kMRings);   // 0.5.0: a bus takes one too (test_sendfx_ins.cpp)
  CHECK(ins::control.type[ins::kBus0][0] == ins::kMRings);
  ins::set_type(ins::kBus0, 0, ins::kOff);
  ins::set_type(ins::kBus0, 0, ins::kAmp);
  CHECK(ins::control.type[ins::kBus0][0] == ins::kOff);
  char o[200];
  CHECK(ins::command("ins=V1:A:type:space", o, sizeof o) && strstr(o, "master"));
  for (int t = 1; t < ins::kAllTypes; ++t) {
    set(0, t);
    CHECK(ins::control.type[master::chain(0)][0] == t);
    CHECK(ins::type_from_name(ins::type_name(t)) == t);
    CHECK(ins::labels(t).label[0][0] != 0);
  }
  // 0.5.0: four send TYPEs at a time with the buses' (two taken there here): the master's third is refused and its slot
  // keeps what it has; a freed room is free again
  ins::set_type(ins::kBus0 + 2, 0, ins::kMRings), ins::set_type(ins::kBus0 + 2, 1, ins::kMChorus);
  set(0, ins::kMSpace), set(1, ins::kMCloud), set(2, ins::kComp);
  set(2, ins::kMHalo);
  CHECK(ins::control.type[master::chain(2)][master::slot(2)] == ins::kComp);
  CHECK(master::command("mfx=3:type:halo", o, sizeof o) && strstr(o, "at most"));
  CHECK(master::next_type(2, ins::kSat, 1) == ins::kOff);   // past SAT, past the send TYPEs: no room
  set(1, ins::kOff), set(2, ins::kMHalo);
  CHECK(ins::control.type[master::chain(2)][master::slot(2)] == ins::kMHalo);
  CHECK(master::control.fx[0] != master::control.fx[2] && master::control.fx[0] >= 0 && master::control.fx[2] >= 0);
  CHECK(master::next_type(1, ins::kSat, 1) == ins::kOff);
  set(0, ins::kOff);
  CHECK(master::next_type(1, ins::kSat, 1) == ins::kMChorus);
  all_off();
  CHECK(!master::on());
  printf("test_master: send TYPEs not on a voice, four at a time with the buses'; AMP not on a bus\n");
}

// one code path: a send TYPE as a master slot at MIX 127 = the send FX's own return for the same input and knobs
static void same_as_the_send_fx() {
  static const struct { int mt, id; } kT[] = {{ins::kMChorus, fx::kChorus}, {ins::kMComb, fx::kComb}, {ins::kMSpace, fx::kTape},
                                              {ins::kMRings, fx::kRings},   {ins::kMCloud, fx::kCloudSeed}, {ins::kMHalo, fx::kHalo}};
  static fx::SendFx* sf = new fx::SendFx(48000.0f);
  for (const auto& t : kT) {
    all_off();
    sf->Reset();
    static fx::Control c;
    memset((void*)&c, 0, sizeof c);
    c.route = fx::kRouteMain;
    c.knob[0][0] = (uint8_t)t.id;
    for (int k = 0; k < 4; ++k) c.knob[0][2 + k] = ins::labels(t.mt).def[k];
    c.send[0][0] = 127;
    set(0, t.mt);
    master::set_mix(0, 127);
    double err = 0, pk = 0;
    uint32_t r = 7;
    for (int b = 0; b < 24000; ++b) {   // (8 s: the start differs, the feedback paths forget it)
      float st[16][fx::kStems] = {}, o[16][2], m[16][2], sl[16][4], y[16][2];
      for (int j = 0; j < 16; ++j) {
        r = r * 1664525u + 1013904223u;
        st[j][0] = 0.05f * (float)(int32_t)r * (1.0f / 2147483648.0f);
        y[j][0] = y[j][1] = st[j][0];
      }
      sf->Process(st, 16, o, c, m, sl);
      ins::engine().ProcessStereo(ins::kM0, y, 16, ins::control);
      if (b > 20000)
        for (int j = 0; j < 16; ++j)
          err = std::max(err, (double)std::fabs(y[j][0] - sl[j][0])), err = std::max(err, (double)std::fabs(y[j][1] - sl[j][1])),
          pk = std::max(pk, (double)std::fabs(sl[j][0]));
    }
    printf("test_master: %-6s as a master slot vs the send FX: max difference %.2e of peak %.4f\n", ins::type_name(t.mt), err, pk);
    CHECK(pk > 1e-4);
    CHECK(err <= 1e-3 * pk + 2e-6);
  }
}

static void stereo_noise() {
  for (int t = 1; t < ins::kAllTypes; ++t) {
    if (t == ins::kAmp) continue;
    all_off();
    set(0, t), set(3, ins::kComp);
    master::set_mix(0, 90);
    for (int k = 0; k < 4; ++k) ins::set_param(master::chain(0), 0, k, 127);
    auto a = run(48000, 3.0f, 3.0f);
    for (int k = 0; k < 4; ++k) ins::set_param(master::chain(0), 0, k, 0);
    auto b = run(48000, 3.0f, 3.0f, 3);
    for (float x : a) CHECK(std::isfinite(x) && std::fabs(x) <= master::kOutLimit);
    for (float x : b) CHECK(std::isfinite(x) && std::fabs(x) <= master::kOutLimit);
  }
  CHECK(ins::engine().Faults() == 0);
  printf("test_master: every TYPE, knobs at both ends, +10 dB noise in: finite, under the ceiling, no fault\n");
}

// stereo survives: a side stays apart through the insert types; LEV; the ceiling's knee leaves low levels alone
static void stereo_and_lev() {
  all_off();
  set(0, ins::kEq);   // flat
  for (int k = 0; k < 4; ++k) ins::set_param(master::chain(0), 0, k, 64);
  auto a = run(48000, 0.0f, 0.1f);
  CHECK(rms(a, 12000, 0) < 1e-4 && rms(a, 12000, 1) > 0.05);
  master::set_lev(50);
  auto b = run(24000, 0.0f, 0.1f);
  CHECK(std::fabs(rms(b, 12000, 1) / rms(a, 12000, 1) - 0.5) < 0.02);
  master::set_lev(master::kLevUnity);
  printf("test_master: a side stays a side, LEV 50 = half\n");
}

static void amp_slots(const std::string& card) {
  all_off();
  std::vector<float> w(amp::kWeights);
  amp::synthetic_weights(w.data(), 1);
  auto a = make_namb(w);
  wr(card + "/NAM/Synth A.namb", a.data(), a.size());
  amp::rescan();
  settle();
  CHECK(amp::models() == 1);
  // the voices' cap full: the master still plays, it has its own
  for (int v = 0; v < 3; ++v) ins::set_type(v, 0, ins::kAmp), ins::set_type(v, 1, ins::kAmp);
  ins::set_type(3, 0, ins::kAmp);
  set(0, ins::kAmp);
  float y[12][2] = {};
  for (int i = 0; i < 3000; ++i) amp::step(), card_core_turn(), master::render(y, 12);
  CHECK(amp::running() == 6 && amp::status(3, 0) == amp::kCap);
  CHECK(amp::running_master() == 1 && amp::status(master::chain(0), 0) == amp::kPlaying);
  all_off();
  settle();
  // one instance: (L + R) / 2 on both sides. L/R pair: each side its own, what is not on a side stays off it
  set(0, ins::kAmp);
  for (int i = 0; i < 2000; ++i) amp::step(), card_core_turn(), master::render(y, 12);
  CHECK(amp::running_master() == 1);
  auto m = run(24000, 0.0f, 0.2f);
  CHECK(rms(m, 12000, 0) > 1e-4 && std::fabs(rms(m, 12000, 0) - rms(m, 12000, 1)) < 1e-5);
  master::set_pair(0, 1);
  for (int i = 0; i < 2000; ++i) amp::step(), card_core_turn(), master::render(y, 12);
  CHECK(amp::running_master() == 2);
  auto p = run(24000, 0.0f, 0.2f);
  CHECK(rms(p, 12000, 1) > 1e-4 && rms(p, 12000, 0) < rms(p, 12000, 1) * 0.02);
  // a third instance on the master: MAX 2
  set(1, ins::kAmp);
  for (int i = 0; i < 2000; ++i) amp::step(), card_core_turn(), master::render(y, 12);
  CHECK(amp::status(master::chain(1), 1) == amp::kCapM && !strcmp(amp::status_short(amp::kCapM), "MAX 2"));
  set(1, ins::kOff);
  master::set_pair(0, 0);
  for (int i = 0; i < 2000; ++i) amp::step(), card_core_turn(), master::render(y, 12);
  CHECK(amp::running_master() == 1);
  printf("test_master: AMP outside the voices' 6, one instance or an L/R pair, MAX 2 past that\n");
  all_off();
  settle();
}

static void per_kit(const std::string& c) {
  const std::string old = "# Sidecar inserts: CHAIN SLOT TYPE P1 P2 P3 P4 / PEQ CHAIN HPF LPF LOW HIGH / AMP CHAIN SLOT FILE / SEND Vn B1 B2 B3 / ROUTE B3 PRE|POST / OUT Vn TRK|PRE|POST LEV PAN WID DEC\nV2 A EQ 64 64 100 64\nSEND V2 0 0 55\n";
  wrs(c + "/.rytm/ins/MFX/k006.txt", old.c_str());
  wrs(c + "/.rytm/ins/MFX/k007.txt", "# x\nV1 A DRIVE 50 0 30 127\nM1 A COMP 50 60 40 20\nM1 B SPACE 40 50 0 20\nM2 A AMP 30 64 64 109\nAMP M2 A Synth A.namb\nM2 B EQ 64 64 64 64\nPEQ M2 40 0 64 70\nMFX 1 MIX 90 40 77 40 40 4\n");
  wrs(c + "/.rytm/ins/MFX/k013.txt", "# x\nMFX 0 MIX 100 40 40 40 40 0\n");   // SRC MIX and nothing else: no chain
  uint8_t msg[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'M', 'F', 'X', 0xF7};
  engine_sysex(msg, (int)sizeof msg);
  loaded();
  CHECK(store::ready());
  host_card_model(true);
  uint64_t ctl = host_card_us(3);
  ins::kit_changed(5);
  CHECK(!master::on() && !master::control.byp && master::control.src == master::kRet);
  ins::kit_changed(6);   // the chain with the kit
  CHECK(master::on() && ins::control.type[ins::kM0][0] == ins::kComp && ins::control.type[ins::kM0][1] == ins::kMSpace);
  CHECK(ins::control.type[ins::kM0 + 1][0] == ins::kAmp && ins::control.type[ins::kM0 + 1][1] == ins::kEq);
  CHECK(master::control.byp == 1 && master::control.src == master::kMix && master::control.lev == 90 && master::control.mix[1] == 77);
  CHECK(master::pair(2) && !master::pair(0) && ins::eq_on(master::kEqChain) && ins::eq_param(master::kEqChain, 3) == 70);
  ins::kit_changed(5);   // and gone with it
  CHECK(!master::on() && master::control.src == master::kRet && !master::pair(2));
  CHECK(host_card_us(3) == ctl);   // a kit change with a chain: RAM only
  host_card_model(false);
  // an old kit: edited and saved, its text as before and nothing of the master's in it
  ins::set_param(1, 0, 2, 101);
  ins::set_param(1, 0, 2, 100);
  ins::settings_due();
  for (int i = 0; i < 40; ++i) idle_save(), card_core_turn();
  CHECK(rd(c + "/.rytm/ins/MFX/k006.txt") == old);
  // the settings written and read back
  ins::kit_changed(10);
  set(0, ins::kMRings), master::set_mix(0, 100), master::set_src(master::kMix), ins::set_eq(master::kEqChain, 0, 33);
  ins::settings_due();
  for (int i = 0; i < 40; ++i) idle_save(), card_core_turn();
  std::string t = rd(c + "/.rytm/ins/MFX/k011.txt");
  CHECK(t.find("M1 A RINGS") != std::string::npos && t.find("PEQ M2 33") != std::string::npos && t.find("MFX 0 MIX 100 100 40 40 40 0") != std::string::npos);
  ins::kit_changed(5);
  CHECK(!master::on());
  ins::kit_changed(10);
  CHECK(ins::control.type[ins::kM0][0] == ins::kMRings && master::control.mix[0] == 100 && master::control.src == master::kMix);
  ins::kit_changed(5);
  // the stem trim: off its default in the line, at its default not; a line without it, the default; "MFX" stays off the file for a default kit
  CHECK(master::control.trim == master::kTrimDef);
  ins::kit_changed(10);
  master::set_trim(-12);
  ins::settings_due();
  for (int i = 0; i < 40; ++i) idle_save(), card_core_turn();
  CHECK(rd(c + "/.rytm/ins/MFX/k011.txt").find("MFX 0 MIX 100 100 40 40 40 0 -12\n") != std::string::npos);
  ins::kit_changed(5);
  CHECK(master::control.trim == master::kTrimDef);
  ins::kit_changed(10);
  CHECK(master::control.trim == 12);
  master::set_trim(0);
  char tl[80];
  CHECK(master::line(tl, sizeof tl) > 0 && std::string(tl) == "MFX 0 MIX 100 100 40 40 40 0 0\n");
  master::set_trim(-6);
  CHECK(master::line(tl, sizeof tl) > 0 && std::string(tl) == "MFX 0 MIX 100 100 40 40 40 0\n");
  master::set_trim(-40);
  CHECK(master::control.trim == master::kTrimMax);
  master::set_trim(-6);
  ins::settings_due();
  for (int i = 0; i < 40; ++i) idle_save(), card_core_turn();
  ins::kit_changed(5);
  ins::kit_changed(6);   // k007: an old line, 9 words
  CHECK(master::control.trim == master::kTrimDef && master::control.src == master::kMix);
  ins::kit_changed(5);
  ins::kit_changed(12);   // k013: SRC MIX with an empty chain loads as it is (the stems summed: test_masterfx_pi)
  CHECK(master::control.src == master::kMix && !master::on() && !master::control.byp && master::control.trim == master::kTrimDef);
  ins::kit_changed(5);
  CHECK(master::control.src == master::kRet);
  printf("test_master: per kit: read, written, a kit change on RAM only; an old kit's file byte for byte\n");
}

static void console_json() {
  all_off();
  char o[1000];
  CHECK(master::command("mfx=1:type:comp", o, sizeof o) && strstr(o, "COMP"));
  CHECK(master::command("mfx=3:type:space", o, sizeof o) && strstr(o, "SPACE mix 40"));
  CHECK(master::command("mfx=3:mix:70", o, sizeof o) && strstr(o, "mix 70"));
  CHECK(master::command("mfx=2:type:amp", o, sizeof o) && master::command("mfx=2:pair:1", o, sizeof o) && strstr(o, "AMP L/R"));
  CHECK(master::command("mfx=1:p1:99", o, sizeof o) && ins::param(ins::kM0, 0, 0) == 99);
  CHECK(master::command("mfx=eq:hpf:40", o, sizeof o) && strstr(o, " EQ"));
  CHECK(master::command("mfx=byp:1", o, sizeof o) && strstr(o, "BYPASS"));
  CHECK(master::command("mfx=src:mix", o, sizeof o) && strstr(o, "src MIX"));
  CHECK(master::command("mfx=lev:90", o, sizeof o) && strstr(o, "lev 90"));
  CHECK(strstr(o, "trim -6"));
  CHECK(master::command("mfx=trim:-12", o, sizeof o) && strstr(o, "trim -12") && master::control.trim == 12);
  CHECK(master::json(o, sizeof o) > 0 && strstr(o, "\"trim\":-12,\"gr\":0.0"));
  CHECK(master::command("mfx=trim:0", o, sizeof o) && master::control.trim == 0);
  CHECK(master::command("mfx=trim:-99", o, sizeof o) && master::control.trim == master::kTrimMax);
  CHECK(master::command("mfx=trim:6", o, sizeof o) && strstr(o, "mfx=trim") && master::control.trim == master::kTrimMax);   // refused: the help
  CHECK(master::command("mfx=trim:-6", o, sizeof o) && master::control.trim == master::kTrimDef);
  master::lim_gain = 0.5f;
  CHECK(master::gr10() == 60);
  master::lim_gain = 1.0f;
  CHECK(master::command("mfx=nonsense:1", o, sizeof o) && strstr(o, "mfx=byp"));
  CHECK(!master::command("mfx=stats", o, sizeof o));
  CHECK(!master::command("mfxx", o, sizeof o));
  CHECK(master::json(o, sizeof o) > 0 && strstr(o, "\"byp\":1") && strstr(o, "\"src\":\"mix\"") && strstr(o, "\"t\":\"SPACE\"") && strstr(o, "\"peq\":[40,"));
  static char big[8000];
  CHECK(ins::json(big, sizeof big) < (int)sizeof big && strstr(big, "\"mfx\":{") && !strstr(big, "\"M1\""));
  CHECK(master::command("mfx=reset", o, sizeof o) && !master::on() && master::control.src == master::kRet);
  {   // 50: the wire's bits to the stems', the status; a kit's clear leaves it; the link gone forgets it
    const uint8_t m[16] = {0xF0, 0x7D, 0x52, 0x54, 0x50, 1, 2, 3, 4, 5, 6, 7, 127, 1, 0x7F - 8, 0xF7};   // route 0xF7: voice 3's bit off
    CHECK(!master::report(m, 15));
    host_sysex(m, 16);   // through the host link's dispatch
    CHECK(master::control.known && master::control.pan[7] == 127);
    CHECK(master::control.off_main == (0xFF & ~1) && master::stems_on_main() == 1);   // bit 3 is BD's, stem 0
    master::clear();
    CHECK(master::control.known);
    char j[900];
    master::json(j, sizeof j);
    CHECK(strstr(j, "\"rep\":1,\"onmain\":1}"));
    master::link_lost();
    CHECK(!master::control.known && master::stems_on_main() == 0);
  }
  printf("test_master: the console, /status\n");
}

// the Rytm's page: every TYPE on every knob corner, both views: the texts fit (check.h fails at exit if not)
static void rytm_page() {
  all_off();
  const ui::ExtraPage* p = nullptr;
  for (int k = 0; k < ui::extra_pages(); ++k)
    if (!strcmp(ui::extra_page(k).title, "MASTER")) p = &ui::extra_page(k);
  CHECK(p);
  static ui::Canvas c;
  ui::mfx_page_set(0, false);
  for (int t = 0; t < ins::kAllTypes; ++t) {
    if (master::next_type(0, t ? t - 1 : 0, 1) != t && t) continue;
    p->enc(5, 0, false);
    set(0, t);
    for (int v : {0, 1, 40, 64, 90, 127}) {
      for (int k = 0; k < 4; ++k) ins::set_param(master::chain(0), 0, k, v);
      for (int e = 0; e < 8; ++e) {
        p->enc(e == 4 || e == 5 || e == 7 ? 6 : e, 0, false);
        p->draw(c, 3, 9, 0);
      }
      p->enc(0, 1, false), p->draw(c, 3, 9, 0);
    }
  }
  set(0, ins::kOff);
  CHECK(p->key(0x44) && master::control.byp == 1);       // YES: bypass
  CHECK(p->key(0x44) && master::control.byp == 0);
  p->enc(5, 1, false);                                    // F: the next TYPE
  CHECK(ins::control.type[ins::kM0][0] == ins::kFilter);
  p->enc(7, 1, false), p->draw(c, 3, 9, 0);               // H: SRC
  CHECK(master::control.src == master::kMix);
  CHECK(p->key(0x41));                                    // RIGHT: the EQ view
  p->enc(0, 5, false), p->enc(7, -1, false), p->draw(c, 3, 9, 0);
  CHECK(ins::eq_param(master::kEqChain, 0) == 5 && master::control.lev == 99);
  CHECK(p->key(0x45) && !p->key(0x45));                   // NO: back, then the page's
  all_off();
  printf("test_master: the Rytm's MASTER page: every TYPE, its keys and knobs, the texts fit\n");
}

// the control core: the longest single call of what a master page does, and no card
static void control_core() {
  all_off();
  host_card_model(true);
  uint64_t ctl = host_card_us(3);
  double worst = 0;
  char o[400];
  static const char* const kCmd[] = {"mfx=1:type:comp", "mfx=3:type:cloud", "mfx=4:type:space", "mfx=2:type:amp", "mfx=2:pair:1", "mfx=1:p1:40",
                                     "mfx=3:mix:90",    "mfx=byp:1",        "mfx=byp:0",        "mfx=src:mix",    "mfx=src:ret",  "mfx=eq:lpf:30",
                                     "mfx=3:off",       "mfx=4:off",        "mfx=2:off",        "mfx=1:off",      "mfx=eq:off"};
  double best[sizeof kCmd / sizeof kCmd[0]];   // each command's quickest of 200: its cost, not a busy Mac's scheduler
  std::fill(std::begin(best), std::end(best), 1e9);
  for (int r = 0; r < 200; ++r)
    for (size_t i = 0; i < sizeof kCmd / sizeof kCmd[0]; ++i) {
      auto t0 = Clock::now();
      CHECK(master::command(kCmd[i], o, sizeof o));
      amp::step();
      master::json(o, sizeof o);
      double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
      best[i] = std::min(best[i], us);
    }
  for (double b : best) worst = std::max(worst, b);
  CHECK(host_card_us(3) == ctl);
  host_card_model(false);
  printf("test_master: control core: longest command + amp step + status %.1f us on the Mac (~%.0f us on a Pi 4 at x 4.4), no card time\n", worst, worst * 4.4);
  CHECK(timing_ok(worst * 4.4 < 2000.0));
}

// what a full chain costs on a core; the numbers go to the report
static void cost() {
  all_off();
  uint32_t worst = 0;
  auto t0 = Clock::now();
  uint64_t ticks = master::bench(&worst);
  double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
  CHECK(ticks > 0);
  uint32_t aw = 0;
  uint64_t at = amp::bench(&aw);
  const double hz = cycles_per_second();
  double pct = 100.0 * (double)ticks / hz, wpct = 100.0 * worst / (hz * 12 / 48000.0), apct = 100.0 * (double)at / hz;
  printf("test_master: bench mfx COMP EQ SPACE CLOUD POSTEQ: %.2f %% of a Mac core, worst block %.1f %% (run took %.0f ms)\n", pct, wpct, us / 1000);
  printf("test_master: one AMP instance %.2f %% of a Mac core; the full chain with an L/R pair %.2f %%\n", apct, pct + 2 * apct);
  // no chain: render's cost with nothing set
  all_off();
  float y[12][2] = {};
  auto t1 = Clock::now();
  for (int i = 0; i < 40000; ++i) master::render(y, 12);
  printf("test_master: render with every slot off: %.1f ns a block\n", std::chrono::duration<double, std::nano>(Clock::now() - t1).count() / 40000);
  CHECK(!master::on());
}

int main() {
  char tmpl[] = "/tmp/test_master.XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string c = tmpl;
  host_set_card(tmpl);
  CHECK(mkdir((c + "/NAM").c_str(), 0755) == 0);
  CHECK(mkdir((c + "/Samples").c_str(), 0755) == 0);
  host_fake_time(true, 5000000);
  CHECK(engine_begin());
  ins::reset();
  settle();
  types();
  same_as_the_send_fx();
  stereo_noise();
  stereo_and_lev();
  amp_slots(c);
  per_kit(c);
  console_json();
  rytm_page();
  control_core();
  host_fake_time(false, 0);
  cost();
  printf("test_master: PASS\n");
  return 0;
}
