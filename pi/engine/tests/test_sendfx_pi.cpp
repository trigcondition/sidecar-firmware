// pi/engine/tests/test_sendfx_pi.cpp - sendfx_pi (the send FX between the IN completion, render core 2 and the OUT
// fill), one thread standing in for the three: off = no work and OUT untouched; on = the return primes (kPrime
// frames), lands on 2/3 (P1 PRE, or route fx) or 0/1 (P1 POST, or route main) added to what is there, the other channels untouched; a
// dry ring is counted and primes again; the console's sendfx line.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "sendfx_pi.h"

static std::string g_log;
namespace core {
uint32_t cycles() { static uint32_t c; return c += 1000; }
uint32_t cycles_per_second() { return 54000000; }
uint32_t g_ms;
uint32_t now_ms() { return g_ms; }
void log(const char* fmt, ...) {
  char b[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  g_log += b;
  g_log += "\n";
}
}  // namespace core
// the Pi's integer printf (core_port.h), the Mac's here (core/fx/insert.cpp's value names)
extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap) { return vsnprintf(out, n, fmt, ap); }
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r;
}

using namespace core::fx;

#include <algorithm>
#include <cmath>
#include <set>
static std::set<int64_t> g_sent;   // every stem 0 value as float -> int (the insert's exact passthrough)
static void in_block(int k) {   // 7 frames: stem 0 a 220 Hz sine at 0.1 FS
  int32_t s[7][8] = {};
  for (int f = 0; f < 7; ++f) s[f][0] = (int32_t)(0.1 * 2147483647.0 * std::sin(6.2831853 * 220 * (7 * k + f) / 48000));
  for (int f = 0; f < 7; ++f) g_sent.insert((int64_t)(float)s[f][0]);
  sendfx_pi::FromRytm(s, 7);
}
static void out_block(int32_t (*o)[12]) {
  for (int f = 0; f < 7; ++f)
    for (int c = 0; c < 12; ++c) o[f][c] = 1000 * (c + 1);
  sendfx_pi::AddReturn(o, 7);
}

int main() {
  int32_t o[7][12];
  CHECK(!sendfx_pi::On());
  for (int k = 0; k < 100; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
  for (int c = 0; c < 12; ++c) CHECK(o[6][c] == 1000 * (c + 1));   // off: OUT untouched
  control.knob[0][0] = kComb;
  control.send[0][0] = 127;
  CHECK(sendfx_pi::On());
  bool seen = false;
  int blocks_to_return = -1;
  for (int k = 0; k < 2000; ++k) {
    in_block(k);
    if (k % 2) sendfx_pi::Render(), sendfx_pi::Render();   // core 2 at its own pace (every 14 frames here)
    out_block(o);
    bool moved = o[3][2] != 3000 || o[3][3] != 4000;
    if (moved && blocks_to_return < 0) blocks_to_return = k;
    seen |= moved;
    for (int c = 0; c < 12; ++c)
      if (c != 2 && c != 3) CHECK(o[3][c] == 1000 * (c + 1));
  }
  CHECK(seen && blocks_to_return * 7 >= (int)sendfx_pi::kPrime - 7);
  {   // each FX's own return (the recorder's FX1/FX2): FX 1 alone plays, FX 2's slot stays silent
    float sl[7][4] = {};
    float m1 = 0, m2 = 0;
    for (int k = 0; k < 50; ++k) {
      in_block(k), sendfx_pi::Render();
      for (int f = 0; f < 7; ++f)
        for (int c = 0; c < 12; ++c) o[f][c] = 0;
      sendfx_pi::AddReturn(o, 7, sl);
      for (int f = 0; f < 7; ++f) m1 = std::max(m1, std::fabs(sl[f][0]) + std::fabs(sl[f][1])), m2 = std::max(m2, std::fabs(sl[f][2]) + std::fabs(sl[f][3]));
    }
    CHECK(m1 > 1e-4f && m2 == 0.0f);
  }
  control.route = kRouteMain;
  for (int k = 0; k < 200; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
  CHECK((o[3][0] != 1000 || o[3][1] != 2000) && o[3][2] == 3000 && o[3][3] == 4000);
  // a stalled core 2: the ring runs dry, the fill counts it and leaves OUT as it was
  for (int k = 0; k < 40; ++k) in_block(k), out_block(o);
  CHECK(o[3][0] == 1000 && o[3][1] == 2000);
  g_log.clear();
  CHECK(sendfx_pi::Command("sendfx") && g_log.find("fx1 COMB (2)") != std::string::npos);
  CHECK(g_log.find("underruns 1") != std::string::npos && g_log.find("faults 0") != std::string::npos);
  printf("%s", g_log.c_str());
  {   // the load is busy ticks over the elapsed milliseconds, not a 32-bit tick difference: 2000 renders (1000 ticks each at
      // 54 MHz) in the 5 minutes since the last line (4 wraps of a 32-bit tick count) are 0.012 %, not 4 wraps' worth
    control.route = kRouteSlot;
    core::g_ms += 300000;
    for (int k = 0; k < 2000; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
    g_log.clear();
    CHECK(sendfx_pi::Command("sendfx"));
    CHECK(g_log.find("core 2 load 0.012 percent") != std::string::npos && g_log.find("frames, core 2") != std::string::npos);
    printf("%s", g_log.c_str());
    core::g_ms += 1000;   // a second later with no render: 0 %, and the frames since the last line are 0
    g_log.clear();
    CHECK(sendfx_pi::Command("sendfx") && g_log.find(" 0 frames, core 2 load 0.000 percent") != std::string::npos);
  }
  CHECK(!sendfx_pi::Command("out=main"));
  // the display's reads and its kill button
  sendfx_pi::View v;
  control.send[3][1] = 77;
  sendfx_pi::Read(&v);
  CHECK(strcmp(v.type[0], "COMB") == 0 && v.send[0][0] == 127 && v.send[1][3] == 77);
  g_log.clear();
  CHECK(sendfx_pi::Command("killfx") && g_log.find("kill tails") != std::string::npos);
  // STUTTER (FX 2 here): the stems' sum onto MAIN 0/1 with the route at fx, 2/3 still FX 1's; disengaged = the sum
  control.route = kRouteSlot;   // P1 per slot: FX 1 (COMB) PRE on 2/3, the STUTTER on FX 2 POST on 0/1
  control.knob[0][1] = 0;
  control.knob[1][1] = 127;
  control.knob[1][0] = kStutter;
  sendfx_pi::Clock(0xFA, 0), sendfx_pi::Clock(0xF8, 0);
  for (int k = 0; k < 400; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
  for (int f = 0; f < 7; ++f) {   // every frame one of the stem's values exactly (its own latency), on both sides
    int64_t d0 = (int64_t)o[f][0] - 1000, d1 = (int64_t)o[f][1] - 2000;
    CHECK(d0 == d1 && g_sent.count(d0) && d0 != 0 && o[f][2] != 3000);
  }
  g_log.clear();
  CHECK(sendfx_pi::Command("sendfx") && g_log.find("fx2 STUTTER (15)") != std::string::npos);
  // the OUT route (core/fx/out.h): a voice on PRE / POST stands in for its silent stem (Taps::feed): with STUTTER's
  // insert disengaged (the stems' sum), the same values on stem 0 (the IN) and as voice 3's feed come out as twice the
  // value on every frame: the feed lands in its stem's place, frame-aligned with the stems
  {
    std::set<int64_t> two;
    core::out::control.route[2] = core::out::kPost;
    for (int k = 0; k < 400; ++k) {
      int32_t st[7][8] = {};
      float fd[7][8] = {};
      int16_t V[7][8] = {};
      for (int f = 0; f < 7; ++f) {
        st[f][0] = (int32_t)(0.1 * 2147483647.0 * std::sin(6.2831853 * 220 * (7 * k + f) / 48000));
        fd[f][2] = (float)st[f][0] * (1.0f / 2147483648.0f);   // voice 3 (SD: stem 1) on POST
        two.insert(2 * (int64_t)(float)st[f][0]);
      }
      sendfx_pi::FromVoices(V, 7, fd);
      sendfx_pi::FromRytm(st, 7);
      sendfx_pi::Render();
      out_block(o);
    }
    for (int f = 0; f < 7; ++f) {
      int64_t d0 = (int64_t)o[f][0] - 1000;
      CHECK(two.count(d0) && d0 != 0);
    }
    printf("the OUT route's stem stand-in: aligned with the stems (every frame twice the value)\n");
    core::out::control.route[2] = core::out::kTrk;
  }
  // the stand-in lands in its voice's stem (core::out::Device stem): COMB on FX1, only track BT (4) sends: voice 2 (BT)
  // on POST is heard, voice 3 (SD) is not
  {
    control.knob[1][0] = kOff, control.knob[0][0] = kComb, control.knob[0][1] = 0;
    for (int t = 0; t < 12; ++t) control.send[t][0] = 0;
    control.send[4][0] = 127;
    core::out::reset();
    for (int which : {1, 2}) {
      core::out::control.route[which] = core::out::kPost;
      for (int k = 0; k < 4000; ++k) {   // settle silent first (the comb's tail)
        int32_t st[7][8] = {};
        sendfx_pi::FromRytm(st, 7), sendfx_pi::Render(), out_block(o);
      }
      double e = 0;
      for (int k = 0; k < 600; ++k) {
        int32_t st[7][8] = {};
        float fd[7][8] = {};
        int16_t V[7][8] = {};
        for (int f = 0; f < 7; ++f) fd[f][which] = 0.2f * (float)std::sin(6.2831853 * 220 * (7 * k + f) / 48000);
        sendfx_pi::FromVoices(V, 7, fd);
        sendfx_pi::FromRytm(st, 7), sendfx_pi::Render(), out_block(o);
        if (k > 300) e += std::fabs((double)o[3][2] - 3000);
      }
      printf("stand-in of voice %d (send only on BT): FX1 return %.0f\n", which + 1, e);
      CHECK(which == 1 ? e > 1e6 : e < 1e3);
      core::out::control.route[which] = core::out::kTrk;
    }
  }
  // fx-ins: the Sidecar voices' sends (the OUT fill's FromVoices) into bus 3 (a dry bus with its own chain), every FX
  // off: On() for bus 3 alone, its return on 2/3 (PRE), then 0/1 (route3 POST); off again: no work
  {
    control.knob[0][0] = control.knob[1][0] = kOff;
    for (int k = 0; k < 200; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
    CHECK(!sendfx_pi::On());
    core::ins::set_type(core::ins::kBus0 + 2, 0, core::ins::kTrem);
    core::ins::set_send(2, 2, 127);
    CHECK(sendfx_pi::On());
    float pk_pre = 0, pk_post = 0;
    for (int k = 0; k < 600; ++k) {
      int16_t V[7][8] = {};
      for (int f = 0; f < 7; ++f) V[f][2] = (int16_t)(8000 * std::sin(6.2831853 * 330 * (7 * k + f) / 48000));
      if (k == 300) core::ins::set_route3(1);
      sendfx_pi::FromVoices(V, 7);
      in_block(k), sendfx_pi::Render(), out_block(o);
      float a = std::fabs((float)(o[3][2] - 3000)), b = std::fabs((float)(o[3][0] - 1000));
      if (k > 100 && k < 290) {
        pk_pre = std::max(pk_pre, a);
        CHECK(o[3][0] == 1000);
      }
      if (k > 400) {
        pk_post = std::max(pk_post, b);
        CHECK(o[3][2] == 3000);
      }
    }
    printf("bus 3 (TREMOLO) from voice 3's send: PRE peak %.0f, POST peak %.0f\n", pk_pre, pk_post);
    CHECK(pk_pre > 1e6f && pk_post > 1e6f);
    core::ins::reset();
    for (int k = 0; k < 50; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
    CHECK(!sendfx_pi::On());
  }
  printf("test_sendfx_pi: PASS\n");
  return 0;
}
