// core/tests/test_ufx_slots.cpp - 3(c): each box its own user effects UFX1..UFX4 (decided 2026-11-14). With an A4 beside
// the Rytm, slot 0's and slot 1's UFX1 hold different effects at once: each box's own load slot, its own states, its
// own project slot map in its own folder (/.rytm/fxslots, /.a4/fxslots), its own TYPE names, and each box's send FX
// plays its own effect for TYPE 64 (never the other box's). A box's project change reloads its own effects only.
// Real RMPA packages run in place: arm64 hosts only (as test_api3; elsewhere it passes at once).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "pool/pool.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"

using namespace core;

#if defined(__aarch64__)
static std::vector<std::string> g_log;
static std::string g_root;   // the card's folder: a file as it is on the card (host_path follows the link in hand)
static void sink(const char* s, int n) { g_log.emplace_back(s, (size_t)n); }
static void out0(const uint8_t*, int) {}
static void out1(const uint8_t*, int) {}
static std::string slurp(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  CHECK(f);
  std::string b;
  char t[65536];
  for (size_t n; (n = fread(t, 1, sizeof t, f)) > 0;) b.append(t, n);
  fclose(f);
  return b;
}
static std::string get(const char* path) {
  FILE* f = fopen((g_root + path).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen((g_root + p).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
static int logged(const std::string& s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}
static uint64_t g_t = 1000000;
// every idle pass until nothing waits: the boxes' project reads, the user machines' and effects' loads, the saves
static void settle() {
  idle_flush(true);
  int quiet = 0;
  for (int i = 0; i < 200000; ++i) {
    g_t += 100;
    host_fake_time(true, g_t);
    device::step();
    idle_prepare();
    idle_save();
    idle_stream();
    bool busy = um::idle();
    bool loading = project_loading();
    for (int k = 1; k < kVoiceSlots; ++k) {
      device::Scope in(device::g_links[k]);
      loading = loading || project_loading();
    }
    if (!busy && !loading && idle_settled()) {
      if (++quiet > 60) break;
    } else {
      quiet = 0;
    }
  }
  idle_flush(false);
}
static void hello_a4() {
  const uint8_t h[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                       (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
  engine_sysex_link(1, h, (int)sizeof h);
}
static void name_project(int transport, const char* n) {   // a box's 0x10
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x10};
  for (const char* c = n; *c; ++c) m.push_back((uint8_t)*c);
  m.push_back(0xf7);
  engine_sysex_link(transport, m.data(), (int)m.size());
}
static void a4_project(const char* n) {   // a project name on the A4's link (its mod's 0x10 body, as set_project takes it)
  device::Scope in(device::g_links[1]);
  std::vector<uint8_t> m(n, n + strlen(n));
  m.push_back(0xf7);
  set_project(m.data(), (int)m.size());
}
// a console line in slot s's Scope (the slot in hand: its box's UFX1..UFX4), then everything settled
static std::string cmd(int s, const char* c) {
  char o[200] = "";
  {
    device::Scope in(device::g_links[s]);
    CHECK(um::command(c, o, sizeof o));
  }
  settle();
  return o;
}
static um::SlotView ufx(int s, int u) {   // slot s's UFX u + 1, as its box's pages see it
  device::Scope in(device::g_links[s]);
  return um::slot_view(5 + u);
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.0005f; }
// box's send FX over `blocks` 12-frame blocks of silence (the control core's idle between, for a fresh state): the
// largest |sample| of its return, both pairs
static float play(fx::SendFx& f, const fx::Control& c, int blocks) {
  float st[12][fx::kStems] = {}, o[12][2], m[12][2], pk = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    f.Process(st, 12, o, c, m);
    for (int i = 0; i < 12; ++i)
      for (int j = 0; j < 2; ++j) pk = std::fmax(pk, std::fmax(std::fabs(o[i][j]), std::fabs(m[i][j])));
    um::idle();
  }
  return pk;
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_ufx_slots: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-ufxslots-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  g_root = tmpl;
  host_set_log_sink(sink);
  host_fake_time(true, g_t);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  CHECK(fs.mkdir("/.a4/fxslots"));
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  for (const char* n : {"ufx_pingpong", "api3_fx"})
    put(std::string("/Plugins/") + n + ".rmp", slurp(std::string("tests/api3_pkgs/") + n + ".a64.rmp"));
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  device::set_out(1, out1);
  settle();
  // the Rytm's USB confirms slot 0, presumed its since the boot (attach order: a presumed Rytm that never spoke gives slot 0
  // to the first box that says hello, test_attach_order)
  CHECK(device::usb_attach(&device::rytm()) == 0);
  device::step(true);
  settle();
  hello_a4();   // the A4 beside the Rytm: slot 1, its folder /.a4, its project A4P
  settle();
  CHECK(!strcmp(prj(0).key, "RYP") && !strcmp(prj(1).key, "A4P"));

  // ---- each box's UFX1 its own effect, at once
  cmd(0, "um=fx:1:fx.pingpong");
  cmd(1, "um=fx:1:fx.api3_fx");
  CHECK(ufx(0, 0).state == RMP_SLOT_READY && !strcmp(ufx(0, 0).id, "fx.pingpong"));
  CHECK(ufx(1, 0).state == RMP_SLOT_READY && !strcmp(ufx(1, 0).id, "fx.api3_fx"));
  CHECK(ufx(1, 1).state == RMP_SLOT_EMPTY && ufx(0, 1).state == RMP_SLOT_EMPTY);
  CHECK(logged("um: UFX1 fx.pingpong: ready") == 1 && logged("um: S1:UFX1 fx.api3_fx: ready") == 1);
  // the hooks: box 0's UFX1 is effect 0, box 1's effect 4 (send_fx.h user_index)
  CHECK(fx::user_index(1, fx::kFirstUser) == fx::kUserSlots);
  CHECK(fx::user_ready(0) && fx::user_ready(fx::kUserSlots) && !fx::user_ready(1) && !fx::user_ready(fx::kUserSlots + 1));
  CHECK(std::string(fx::user[0].name) == "PINGPONG" && std::string(fx::user1[0].name) == "API3 FX");
  CHECK(fx::user1[0].tag == fx::fx_tag("API3 FX") && fx::user[0].tag == fx::fx_tag("PINGPONG"));
  CHECK(!strcmp(fx::type_name_in(0, fx::kFirstUser), "PINGPONG") && !strcmp(fx::type_name_in(1, fx::kFirstUser), "API3 FX"));
  CHECK(!strcmp(fx::type_name(fx::kFirstUser), "PINGPONG"));   // (the Rytm's list and states: box 0's)
  // each box's slot map in its own folder, nothing of one in the other's
  std::string m0 = get("/.rytm/fxslots/RYP.txt"), m1 = get("/.a4/fxslots/A4P.txt");
  CHECK(m0.find("fx.pingpong") != std::string::npos && m0.find("fx.api3_fx") == std::string::npos);
  CHECK(m1.find("fx.api3_fx") != std::string::npos && m1.find("fx.pingpong") == std::string::npos);
  CHECK(m1.find(" API3 FX\t") != std::string::npos);
  // the JSON names box 1's
  {
    static char js[65536];
    um::publish_now();
    CHECK(um::json_copy(js, sizeof js) > 0);
    std::string j(js);
    CHECK(j.find("\"ufx\":1,\"id\":\"fx.pingpong\"") != std::string::npos);
    CHECK(j.find("\"ufx\":1,\"slot\":1,\"id\":\"fx.api3_fx\"") != std::string::npos);
  }

  // ---- the hooks render each box's own: box 1's UFX1 the api3_fx probe (0.5 at frame 0), box 0's the ping-pong
  {
    float x[12] = {0}, y[12][2];
    uint8_t k[6] = {fx::kFirstUser, 0, 0, 0, 0, 0};
    for (auto& r : y) r[0] = r[1] = 0.0f;
    fx::user_run(fx::kUserSlots, 0, x, 12, y, k);
    CHECK(near(y[0][0], 0.5f));
    for (auto& r : y) r[0] = r[1] = 0.0f;
    fx::user_run(0, 0, x, 12, y, k);   // (silence in: the ping-pong's return silent)
    CHECK(y[0][0] == 0.0f);
  }

  // ---- each box's effects follow its own transport (um::pre_block: each box's from its own MIDI clock): the A4's Start
  // reaches box 1's effect once, never box 0's; the Rytm's Stop box 0's only. (The api3_fx probe: frame 1 = its block's
  // START / STOP edges, 1 / 16 and 2 / 16.)
  {
    cmd(0, "um=fx:2:fx.api3_fx");   // (box 0's UFX2 the same probe: effect 1)
    CHECK(fx::user_ready(1));
    float x[12] = {0}, y[12][2];
    uint8_t k[6] = {fx::kFirstUser, 0, 0, 0, 0, 0};
    auto edges = [&](int e) {
      for (auto& r : y) r[0] = r[1] = 0.0f;
      fx::user_run(e, 0, x, 12, y, k);
      return (int)std::lround(y[1][0] * 16);
    };
    um::pre_block();
    edges(1), edges(fx::kUserSlots);   // (whatever came before: seen)
    engine_realtime_link(1, 0xFA, 0);   // the A4's Start, on its own link
    um::pre_block();
    CHECK(edges(fx::kUserSlots) == 1 && edges(1) == 0);
    CHECK(edges(fx::kUserSlots) == 0);   // (once)
    engine_realtime_link(0, 0xFC, 0);   // the Rytm's Stop
    um::pre_block();
    CHECK(edges(1) == 2 && edges(fx::kUserSlots) == 0);
    cmd(0, "um=fx:2:-");
  }

  // ---- each box's send FX on TYPE 64 plays its own UFX1
  std::unique_ptr<fx::SendFx> f1(new fx::SendFx(48000.0f));
  fx::control1.route = fx::kRouteFx;
  fx::control1.knob[0][0] = fx::kFirstUser;
  fx::control1.knob[1][0] = fx::kOff;
  CHECK(fx::state(fx::control1, 0) == fx::kStReady && fx::any_on(fx::control1));
  CHECK(play(*f1, fx::control1, 200) > 0.25f);   // the probe's constant return: box 1's own effect
  // box 1's UFX2 holds nothing: MISSING and silent (never box 0's UFX2, never its UFX1)
  fx::control1.knob[0][0] = fx::kFirstUser + 1;
  CHECK(fx::state(fx::control1, 0) == fx::kStMissing && !fx::any_on(fx::control1));
  play(*f1, fx::control1, 100);   // (faded out)
  CHECK(play(*f1, fx::control1, 50) == 0.0f);
  fx::control1.knob[0][0] = fx::kFirstUser;

  // ---- box 0's UFX1 emptied: box 1's plays on
  cmd(0, "um=fx:1:-");
  CHECK(ufx(0, 0).state == RMP_SLOT_EMPTY && !fx::user_ready(0));
  CHECK(ufx(1, 0).state == RMP_SLOT_READY && fx::user_ready(fx::kUserSlots));
  CHECK(fx::state(fx::control, 0) != fx::kStReady || fx::control.knob[0][0] != fx::kFirstUser);
  CHECK(play(*f1, fx::control1, 200) > 0.25f);
  cmd(0, "um=fx:1:fx.pingpong");

  // ---- the A4's project change: its own map from its folder (B4: UFX1 empty), the Rytm's untouched; back to A4P: loaded again
  put("/.a4/fxslots/B4.txt", "\n\n\n\n");
  a4_project("B4");
  settle();
  CHECK(!strcmp(prj(1).key, "B4"));
  CHECK(ufx(1, 0).state == RMP_SLOT_EMPTY && !fx::user_ready(fx::kUserSlots));
  CHECK(ufx(0, 0).state == RMP_SLOT_READY && fx::user_ready(0) && std::string(fx::user[0].name) == "PINGPONG");
  fx::control1.knob[0][0] = fx::kFirstUser;   // (B4's own send FX settings: its TYPE set again here)
  CHECK(fx::state(fx::control1, 0) == fx::kStMissing);
  a4_project("A4P");
  settle();
  CHECK(ufx(1, 0).state == RMP_SLOT_READY && !strcmp(ufx(1, 0).id, "fx.api3_fx") && fx::user_ready(fx::kUserSlots));
  CHECK(std::string(fx::user1[0].name) == "API3 FX");
  fx::control1.knob[0][0] = fx::kFirstUser;
  CHECK(fx::state(fx::control1, 0) == fx::kStReady);

  // ---- the Rytm's project change: box 1's effect plays on
  put("/.rytm/fxslots/RYQ.txt", "\n\n\n\n");
  name_project(0, "RYQ");
  settle();
  CHECK(!strcmp(prj(0).key, "RYQ"));
  CHECK(ufx(0, 0).state == RMP_SLOT_EMPTY && !fx::user_ready(0));
  CHECK(ufx(1, 0).state == RMP_SLOT_READY && fx::user_ready(fx::kUserSlots));
  CHECK(get("/.a4/fxslots/A4P.txt").find("fx.api3_fx") != std::string::npos);
  CHECK(get("/.rytm/fxslots/RYQ.txt").find("fx.") == std::string::npos);

  printf("test_ufx_slots: PASS\n");
  return 0;
}
#else
int main() {
  printf("test_ufx_slots: RMPA images run on an AArch64 host only: skipped\n");
  return 0;
}
#endif
