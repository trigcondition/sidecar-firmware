// core/tests/test_api3.cpp - Plugin API 3 on the host (core/user/um.cpp, sdk/rmp/rmp_api3.h, docs/plugin-api-3-plan.md)
// with real RMPA packages run in place on this arm64 Mac: plugins/api3_test's probe in its variants, built with
// `make -C plugins/api3_test all-variants` and kept in tests/api3_pkgs (CI has no AArch64 compiler). Checked:
//   - a dual package loads as API 3: its API 3 render plays (not its API 2 one), the host table it finds, the block
//     (n, a mono route's null out[1]), its slot arena (want granted, zeroed before setup, a part per voice, kept);
//   - an API 3 only package loads, and is in the Rytm's list as a machine;
//   - the transport snapshot per block: 120 BPM until measured; Start, MIDI clock at 150 BPM, the song position, the
//     flags (RUNNING, CLOCKED, LOCKED, START for one block, STOP, JUMP on a song position message);
//   - guarded calls: a fault in params() or note_on() mutes the slot (silent, "muted: crashed in params at +0x...")
//     and the engine plays on (another slot's voice still sounds); three late params() in a row mute it; um=reload
//     brings it back; a fault in init() refuses the package (BAD), the engine plays on;
//   - refusals: a needed capability the host lacks (BAD "needs signal bus"), an arena past the per-slot limit (NO ROOM);
//   - two slots of one package: one image, an arena and a setup() each.
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include <memory>
#include <thread>
#include "hooks.h"
#include "host_link.h"
#include "lfo.h"
#include "rmp_api3.h"
#include "rmp_format.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"
#include "voices.h"

using namespace core;

#if defined(__aarch64__)
static std::vector<std::string> g_log;
static void sink(const char* s, int n) { g_log.emplace_back(s, (size_t)n); }
static std::vector<std::vector<uint8_t>> g_msgs;   // what the engine sent the Rytm
static void out(const uint8_t* m, int n) { g_msgs.emplace_back(m, m + n); }
// a 0x21 TYPE list sent since g_msgs was cleared holds TYPE id with this name
static bool listed(int id, const char* name) {
  std::string want = std::string(1, (char)id) + name;
  for (auto& m : g_msgs)
    if (m.size() > 8 && m[4] == 0x21 && std::string(m.begin(), m.end()).find(want) != std::string::npos) return true;
  return false;
}
static std::string slurp(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  CHECK(f);
  std::string b;
  char t[65536];
  size_t n;
  while ((n = fread(t, 1, sizeof t, f)) > 0) b.append(t, n);
  fclose(f);
  return b;
}
static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
static void sysex(std::vector<uint8_t> m) { engine_sysex(m.data(), (int)m.size()); }
static void idle_all() {
  int quiet = 0;
  for (int i = 0; i < 200000; ++i) {
    if (um::idle()) quiet = 0;
    else if (++quiet > 8) return;
  }
  for (size_t i = g_log.size() > 12 ? g_log.size() - 12 : 0; i < g_log.size(); ++i) fprintf(stderr, "  log: %s\n", g_log[i].c_str());
  CHECK(!"um::idle never settled");
}
static std::string cmd(const std::string& c) {
  char o[200];
  CHECK(um::command(c.c_str(), o, sizeof o));
  idle_all();
  return o;
}
// 0x04 (note >= 0) or 0x03 for voice vi on slot s (0..4): P1 = mode, P2 = 100, the rest 64
static void report(int s, int vi, int note, int mode) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)(note >= 0 ? 0x04 : 0x03), (uint8_t)(0x50 + 8 * s + vi)};
  if (note >= 0) m.push_back((uint8_t)note), m.push_back(100);
  for (int k = 0; k < 16; ++k) {
    int v = k == 0 ? mode : k == 1 ? 100 : 64;
    m.push_back((uint8_t)v), m.push_back(0);
  }
  m.push_back(0xf7);
  sysex(m);
  idle_all();
}
static uint64_t g_t = 1000000;
static int16_t g_ring[1024][kVoices];
static uint64_t g_mach[16];
static int g_w = 0;
// one block (the control core's pre_block first, as the Pi's RenderBlock and the Mac lib do); frames of voice vi
static std::vector<float> block(int vi) {
  g_t += 250;
  host_fake_time(true, g_t);
  um::pre_block();
  render_voices(g_ring, 1024, g_w, 0, kVoices, g_mach);
  std::vector<float> r(kBlock);
  for (int k = 0; k < kBlock; ++k) r[k] = g_ring[(g_w + k) % 1024][vi] / (0.7f * 32767.0f);
  g_w = (g_w + kBlock) % 1024;
  return r;
}
static int16_t g_ring_r[1024][kVoices];
static uint8_t g_stm[kVoices];
// one block with the voices' R rows (the Pi's render): L and R of voice vi
static void block2(int vi, std::vector<float>* l, std::vector<float>* r) {
  g_t += 250;
  host_fake_time(true, g_t);
  um::pre_block();
  render_voices(g_ring, 1024, g_w, 0, kVoices, g_mach, g_ring_r, g_stm);
  l->assign(kBlock, 0.0f), r->assign(kBlock, 0.0f);
  for (int k = 0; k < kBlock; ++k) {
    (*l)[k] = g_ring[(g_w + k) % 1024][vi] / (0.7f * 32767.0f);
    (*r)[k] = g_ring_r[(g_w + k) % 1024][vi] / (0.7f * 32767.0f);
  }
  g_w = (g_w + kBlock) % 1024;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.0005f; }
static bool silent(const std::vector<float>& f) {
  for (float x : f) if (x != 0.0f) return false;
  return true;
}
static int logged(const std::string& s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}
static void clock_ticks(int n, uint32_t tick_us) {   // MIDI clock at the LFOs' clock input, the control loop's tick
  for (int i = 0; i < n; ++i) {
    g_t += tick_us;
    host_fake_time(true, g_t);
    lfo::clock(0xF8, 0);
    lfo::tick((uint32_t)g_t);
  }
}

int main() {
  char tmpl[] = "/tmp/core-api3-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_set_log_sink(sink);
  host_fake_time(true, g_t);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/projects/A3"));
  put("/.rytm/project", "A3");
  for (const char* n : {"api3_test", "api3_only", "api3_initcrash", "api3_needs", "api3_big", "api3_stereo", "ufx_pingpong",
                        "api3_poly", "api3_polyar", "api3_setupcr", "api3_setupsl", "api3_setupmo", "api3_setupre",
                        "api3_attachcr", "api3_fx"})
    put(std::string("/Plugins/") + n + ".rmp", slurp(std::string("tests/api3_pkgs/") + n + ".a64.rmp"));
  hooks.to_rytm = ::out;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'A', '3', 0xf7});
  idle_all();
  cmd("um=empty");   // (a fresh project would take the installed machines)
  CHECK(logged("API 3: arena pool 65536 KB, guarded calls") == 1);

  // ---- a dual package: the API 3 render, the block, the arena
  cmd("um=assign:1:rytm.api3_test");
  um::SlotView v = um::slot_view(0);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3 && !v.muted);
  // shared 64 KB + 8 states x 256 KB (the wants): 2112 KB
  CHECK(v.arena == (64u << 10) + 8u * (256u << 10));
  CHECK(logged("um: U1 rytm.api3_test: ready (API 3, arena 2112 KB: 8 states)") == 1);
  report(0, 0, 60, 0);
  std::vector<float> f = block(0);
  CHECK(near(f[0], 0.5f));                     // the API 3 render (the API 2 one writes 0.25)
  CHECK(near(f[1], 0.12f));                    // 120 BPM until measured
  CHECK(near(f[2], 0.0f));                     // no flags: stopped, no clock
  CHECK(near(f[3], 0.5f) && near(f[4], 0.5f));   // its arena part zeroed and its own; setup saw the shared part zeroed
  CHECK(near(f[5], 1.0f / 16) && near(f[6], 0.015625f));   // voice 0, 256 KB
  CHECK(near(f[7], -0.5f) && near(f[8], 12.0f / 16) && near(f[9], 0.5f));   // not locked, n 12, mono (out[1] null)
  CHECK(near(f[10], 1.0f / 16));               // one setup() for the slot
  report(0, 3, 60, 0);   // voice 3: its own part
  f = block(3);
  CHECK(near(f[3], 0.5f) && near(f[5], 4.0f / 16));
  f = block(0);
  CHECK(near(f[3], 0.5f));   // (voice 0's pattern still its own)

  // ---- the transport: Start, clock at 150 BPM (16667 us a tick), the song position, Stop
  lfo::clock(0xFA, 0);
  clock_ticks(48, 16667);
  f = block(0);
  int fl = (int)lroundf(f[2] * 64);
  CHECK((fl & (RMP3_T_RUNNING | RMP3_T_CLOCKED | RMP3_T_LOCKED | RMP3_T_START)) ==
        (RMP3_T_RUNNING | RMP3_T_CLOCKED | RMP3_T_LOCKED | RMP3_T_START));
  CHECK(std::fabs(f[1] - 0.15f) < 0.002f);     // 150 BPM, smoothed
  CHECK(f[7] >= 0.0f && near(f[7], 0.047f));   // song_tick 47 (48 ticks after Start, the first is 0)
  f = block(0);
  fl = (int)lroundf(f[2] * 64);
  CHECK(!(fl & RMP3_T_START) && (fl & RMP3_T_LOCKED));   // START for one block only
  lfo::clock(0xF2, 32);   // song position 32 sixteenths: tick 191 until the next tick
  clock_ticks(1, 16667);
  f = block(0);
  fl = (int)lroundf(f[2] * 64);
  CHECK((fl & RMP3_T_JUMP) && near(f[7], 0.192f));
  lfo::clock(0xFC, 0);
  f = block(0);
  fl = (int)lroundf(f[2] * 64);
  CHECK((fl & RMP3_T_STOP) && !(fl & RMP3_T_RUNNING) && !(fl & RMP3_T_LOCKED) && near(f[7], -0.5f));
  // a Start while it plays (the position back to 0): START and JUMP
  lfo::clock(0xFA, 0);
  clock_ticks(30, 16667);
  f = block(0);
  lfo::clock(0xFA, 0);
  clock_ticks(2, 16667);
  f = block(0);
  fl = (int)lroundf(f[2] * 64);
  CHECK((fl & RMP3_T_START) && (fl & RMP3_T_JUMP) && near(f[7], 0.001f));
  lfo::clock(0xFC, 0);
  block(0);

  // ---- an API 3 only package on U2 (also a machine in the Rytm's list)
  cmd("um=assign:2:rytm.api3_only");
  v = um::slot_view(1);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3);
  report(1, 1, 60, 0);
  f = block(1);
  CHECK(near(f[0], 0.5f) && near(f[5], 2.0f / 16));
  CHECK(um::installed() == 13);   // every probe machine lists (the BAD ones too until loaded); the effects do not

  // ---- guarded calls: a fault in params mutes U1, U2 plays on
  report(0, 0, -1, 16);   // MODE zone 1: params() writes through a null pointer
  v = um::slot_view(0);
  CHECK(v.muted && v.state == RMP_SLOT_READY);
  CHECK(std::string(v.why).find("muted: crashed in params at +0x") == 0);
  CHECK(silent(block(0)) && silent(block(3)));
  CHECK(near(block(1)[0], 0.5f));   // the engine plays on: U2's voice
  report(0, 0, 60, 0);   // a muted slot takes no calls: still silent, no second fault
  CHECK(silent(block(0)));
  CHECK(logged("crashed in params") == 1);
  CHECK(cmd("um=reload:1").find("it was muted") != std::string::npos);
  v = um::slot_view(0);
  CHECK(v.state == RMP_SLOT_READY && !v.muted && v.why[0] == 0);
  report(0, 0, 60, 0);
  f = block(0);
  CHECK(near(f[0], 0.5f) && near(f[3], 0.5f) && near(f[4], 0.5f));   // fresh: the arena zeroed again, setup again
  // a fault in note_on
  report(0, 0, 60, 48);
  v = um::slot_view(0);
  CHECK(v.muted && std::string(v.why).find("muted: crashed in note_on at +0x") == 0);
  cmd("um=reload:1");
  // three late params() in a row (real time: the probe's ~2 ms loop; the one-call limit out of the way)
  um::call_stall_limit(1000000);
  host_fake_time(false, 0);
  report(0, 0, 60, 0);
  report(0, 0, -1, 32);
  report(0, 0, -1, 32);
  CHECK(!um::slot_view(0).muted);
  report(0, 0, -1, 32);
  v = um::slot_view(0);
  CHECK(v.muted && std::string(v.why).find("muted: params ran ") == 0 && std::string(v.why).find("three late calls") != std::string::npos);
  host_fake_time(true, g_t);
  cmd("um=reload:1");
  CHECK(!um::slot_view(0).muted);
  // one params() longer than the audio buffer: muted at once (here any late call is over the limit: 501 us)
  um::call_stall_limit(501);
  host_fake_time(false, 0);
  report(0, 0, 60, 0);
  CHECK(!um::slot_view(0).muted);
  report(0, 0, -1, 32);
  v = um::slot_view(0);
  CHECK(v.muted && std::string(v.why).find("muted: params ran ") == 0 &&
        std::string(v.why).find("us, over the FIFO's 501") != std::string::npos);
  host_fake_time(true, g_t);
  um::call_stall_limit(0);
  cmd("um=reload:1");
  CHECK(!um::slot_view(0).muted);

  // ---- init faults: BAD, the engine plays on
  cmd("um=assign:3:rytm.api3_initcrash");
  v = um::slot_view(2);
  CHECK(v.state == RMP_SLOT_BAD && std::string(v.why).find("bad: crashed in init at +0x") == 0);
  CHECK(near(block(1)[0], 0.5f));
  // ---- refusals: a capability the host lacks, an arena past the limit
  cmd("um=assign:4:rytm.api3_needs");
  v = um::slot_view(3);
  CHECK(v.state == RMP_SLOT_BAD && std::string(v.why) == "bad: needs signal bus");
  cmd("um=assign:5:rytm.api3_big");
  v = um::slot_view(4);
  CHECK(v.state == RMP_SLOT_NO_ROOM && std::string(v.why).find("no room: the arena: needs ") == 0);

  // ---- two slots of one package: one image, an arena and a setup() each
  cmd("um=assign:3:rytm.api3_test");
  v = um::slot_view(2);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3 && v.arena == um::slot_view(0).arena);
  CHECK(logged("um: U3 rytm.api3_test: shares U1's image, its own arena") == 1);
  report(2, 5, 60, 0);
  f = block(5);
  CHECK(near(f[0], 0.5f) && near(f[3], 0.5f) && near(f[4], 0.5f) && near(f[10], 1.0f / 16) && near(f[5], 6.0f / 16));

  // ---- stereo: a plugin of out_channels 2 writes R on PRE / POST (here R = -L), a mono mix on TRK (out[1] null);
  // a mono plugin on PRE plays R = L as before
  cmd("um=assign:4:rytm.api3_stereo");
  CHECK(um::slot_view(3).state == RMP_SLOT_READY);
  report(3, 6, 60, 0);
  std::vector<float> l, r;
  block2(6, &l, &r);
  CHECK(near(l[0], 0.5f) && near(l[9], 0.5f));   // TRK: mono
  for (int k = 0; k < kBlock; ++k) CHECK(r[k] == l[k]);
  out::set_route(6, out::kPre);
  block2(6, &l, &r);
  CHECK(near(l[0], 0.5f) && near(l[9], 0.25f));   // PRE: out[1] given
  for (int k = 0; k < kBlock; ++k) CHECK(near(r[k], -l[k]));
  CHECK(g_stm[6]);   // (the pair's real-stereo bit)
  out::set_route(0, out::kPre);
  report(0, 0, 60, 0);
  block2(0, &l, &r);
  CHECK(near(l[0], 0.5f) && near(l[9], 0.5f));   // the mono probe on PRE: out[1] null, R = L
  for (int k = 0; k < kBlock; ++k) CHECK(r[k] == l[k]);
  out::set_route(0, out::kTrk), out::set_route(6, out::kTrk);

  // ---- a user effect: plugins/ufx_pingpong in UFX1, a send FX TYPE (64), on the send FX's DSP as core 2 runs it
  CHECK(!fx::user_ready(0));
  CHECK(fx::state(0) == fx::kStEmpty);
  cmd("um=fx:1:fx.pingpong");
  v = um::slot_view(5);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3 && !v.muted);
  CHECK(fx::user_ready(0) && std::string(fx::user[0].name) == "PINGPONG" && fx::user[0].tag == fx::fx_tag("PINGPONG"));
  CHECK(logged("um: UFX1 fx.pingpong: ready (API 3, arena 3004 KB: 2 states)") == 1);
  {   // its labels for the Rytm's SEND FX page (0x24) from the manifest: P2..P5 short names, TIME's 8 names as stepped zones
    const fx::KnobLabels* l = fx::labels(fx::kFirstUser);
    CHECK(l && std::string(l->label[0]) == "TIM" && std::string(l->label[3]) == "WID");
    CHECK(l && l->nzones == 8 && l->zones[0].knob == 0 && l->zones[1].lo == 16 && l->zones[7].lo == 112);
    CHECK(l && std::string(l->zones[2].name) == "1/8" && l->zones[2].step && !l->zones[2].show);
    uint8_t m[160];
    int n = fx::sysex_labels(m, fx::kFirstUser);
    CHECK(n == 24 + 8 * 8 && m[4] == 0x24 && m[6] == fx::kFirstUser && memcmp(m + 7, "TIM", 3) == 0);
    CHECK(!fx::labels(fx::kFirstUser + 1));   // UFX2 empty: none
  }
  // a machine is not an effect, an effect is not a machine
  cmd("um=fx:2:rytm.api3_test");
  CHECK(um::slot_view(6).state == RMP_SLOT_BAD && std::string(um::slot_view(6).why) == "bad: a machine (U1..U5), not an effect");
  cmd("um=fx:2:-");
  cmd("um=assign:5:fx.pingpong");
  CHECK(um::slot_view(4).state == RMP_SLOT_BAD && std::string(um::slot_view(4).why) == "bad: an effect (UFX1..UFX4), not a machine");
  {
    auto sfx = std::make_unique<fx::SendFx>(48000.0f);
    memset((void*)&fx::control, 0, sizeof fx::control);
    fx::control.send[0][0] = 127;   // BD's send to FX 1
    fx::control.knob[0][0] = fx::kFirstUser;   // FX 1's TYPE: UFX1
    fx::control.knob[0][2] = 36, fx::control.knob[0][3] = 64, fx::control.knob[0][4] = 127, fx::control.knob[0][5] = 127;   // TIME 1/8, FDBK, TONE, WIDTH
    CHECK(fx::state(0) == fx::kStReady);
    const int kN = 26400, kAt = 1200;   // an impulse on BD's stem at frame 1200 (after the TYPE's fade-in)
    std::vector<float> L(kN), R(kN);
    float st[12][fx::kStems], o[12][2];
    for (int i = 0; i < kN; i += 12) {
      for (int j = 0; j < 12; ++j) {
        for (int k = 0; k < fx::kStems; ++k) st[j][k] = 0.0f;
        if (i + j == kAt) st[j][0] = 1.0f;
      }
      sfx->Process(st, 12, o, fx::control);
      for (int j = 0; j < 12; ++j) L[i + j] = o[j][0], R[i + j] = o[j][1];
    }
    auto peak = [&](const std::vector<float>& y, int a, int b, int* at) {
      float m = 0.0f;
      for (int i = a; i < b; ++i) if (std::fabs(y[i]) > m) m = std::fabs(y[i]), *at = i;
      return m;
    };
    float ticks, tick_us;
    lfo::clock_now(&ticks, &tick_us);   // the tempo the transport hands the effect (150 BPM from above, kept when stopped)
    const int d = (int)lroundf(12.0f * tick_us * 0.048f);   // 1/8: 12 ticks
    CHECK(d > 9400 && d < 10000);   // (about 150 BPM: 9600 frames)
    int at = -1, at2 = -1;
    float before = peak(L, 0, kAt + d - 500, &at) + peak(R, 0, kAt + d - 500, &at);   // wet only: nothing until the echo
    float l1 = peak(L, kAt + d - 500, kAt + d + 500, &at), r1 = peak(R, kAt + d - 500, kAt + d + 500, &at2);
    CHECK(before < 1e-4f);
    CHECK(l1 > 0.2f && std::abs(at - (kAt + d)) <= 2);   // the first echo on the left
    CHECK(r1 < 0.05f * l1);
    float r2 = peak(R, kAt + 2 * d - 500, kAt + 2 * d + 500, &at2);
    CHECK(r2 > 0.2f * l1 && std::abs(at2 - (kAt + 2 * d)) <= 2);   // the feedback's echo on the right
    printf("test_api3: PINGPONG echoes L %.3f at +%d, R %.3f at +%d\n", l1, at - kAt, r2, at2 - kAt);
  }
  // a reassign stops it on core 2 first; empty: MISSING again
  cmd("um=fx:1:-");
  CHECK(!fx::user_ready(0) && fx::state(0) == fx::kStMissing);
  CHECK(!fx::labels(fx::kFirstUser));   // its labels go with it

  // ---- a kit made for another effect: MISSING, and silence in that slot (not UFX1's PINGPONG)
  cmd("um=fx:1:fx.pingpong");
  {
    auto sfx = std::make_unique<fx::SendFx>(48000.0f);
    fx::kit_tag[0] = (uint16_t)(fx::user[0].tag ^ 0x155);
    CHECK(fx::state(0) == fx::kStMissing);
    float st[12][fx::kStems], o[12][2], peak = 0.0f;
    for (int i = 0; i < 24000; i += 12) {
      for (int j = 0; j < 12; ++j) {
        for (int k = 0; k < fx::kStems; ++k) st[j][k] = 0.0f;
        if (i + j == 1200) st[j][0] = 1.0f;
      }
      sfx->Process(st, 12, o, fx::control);
      for (int j = 0; j < 12; ++j) peak = std::fmax(peak, std::fmax(std::fabs(o[j][0]), std::fabs(o[j][1])));
    }
    CHECK(peak == 0.0f);
    fx::kit_tag[0] = fx::user[0].tag;   // the kit's own: READY
    CHECK(fx::state(0) == fx::kStReady);
    fx::kit_tag[0] = 0;
  }
  // ---- a rescan with a UFX assigned: its package index follows the new list (a package listed before it), it plays on
  {
    put("/Plugins/0first.rmp", slurp("tests/api3_pkgs/api3_only.a64.rmp"));   // (a duplicate id: listed, not loadable)
    um::rescan();
    idle_all();
    v = um::slot_view(5);
    CHECK(v.state == RMP_SLOT_READY && fx::user_ready(0));
    CHECK(cmd("um=reload:f1").find("UFX1") == 0);
    CHECK(um::slot_view(5).state == RMP_SLOT_READY && fx::user_ready(0));
  }

  // ---- setup(): a fault, a refusal, more than one call, and one too long; voice_attach faulting
  cmd("um=assign:5:rytm.api3_setupcr");
  CHECK(std::string(um::slot_view(4).why).find("bad: crashed in setup at +0x") == 0);
  CHECK(near(block(1)[0], 0.5f));   // (the engine plays on)
  cmd("um=assign:5:rytm.api3_setupre");
  CHECK(std::string(um::slot_view(4).why) == "bad: its setup() refused");
  cmd("um=assign:5:rytm.api3_setupmo");
  CHECK(um::slot_view(4).state == RMP_SLOT_READY);
  report(4, 7, 60, 0);
  f = block(7);
  CHECK(near(f[0], 0.5f) && near(f[4], 0.5f) && near(f[10], 3.0f / 16));   // three setup() calls, one an idle step
  host_fake_time(false, 0);   // (real time: the probe's ~3 ms loop)
  cmd("um=assign:5:rytm.api3_setupsl");
  host_fake_time(true, g_t);
  CHECK(std::string(um::slot_view(4).why).find("bad: setup ran ") == 0);
  cmd("um=assign:5:rytm.api3_attachcr");
  CHECK(um::slot_view(4).state == RMP_SLOT_READY);
  report(4, 7, 60, 0);
  CHECK(um::slot_view(4).muted && std::string(um::slot_view(4).why).find("muted: crashed in voice_attach at +0x") == 0);
  CHECK(silent(block(7)));

  // ---- poly instances (H2): a plugin without an arena plays every instance; one with an arena gets 40 states by
  // default when it is a poly machine; a machine of 8 states says so when an instance is past them (never silently)
  cmd("um=assign:5:rytm.api3_poly");
  report(4, 7, 60, 0);
  {
    float o[kBlock];
    CHECK(um::inst_note_on(7, 31, 64.0f, 1.0f));
    um::inst_render(7, 31, o);
    CHECK(near(o[0], 0.5f) && near(o[5], 8.0f / 16) && near(o[3], 0.0f));   // voice 7, no arena
    cmd("um=assign:5:rytm.api3_polyar");
    CHECK(um::slot_view(4).state == RMP_SLOT_READY && um::slot_view(4).arena == (64u << 10) + 40u * (256u << 10));
    report(4, 7, 60, 0);
    CHECK(um::inst_note_on(7, 31, 64.0f, 1.0f));
    um::inst_render(7, 31, o);
    CHECK(near(o[0], 0.5f) && near(o[3], 0.5f) && near(o[6], 0.015625f));   // instance 31: its own part (8 + 31)
    cmd("um=assign:5:rytm.api3_test");   // 8 states (no instances declared): an instance is refused, in words
    report(4, 7, 60, 0);
    CHECK(!um::inst_note_on(7, 3, 64.0f, 1.0f));
    CHECK(std::string(um::slot_view(4).why) == "no room: poly instance 3 past the arena's 8 states");
    CHECK(logged("past the arena's 8 states (set states in its rmp3_plugin)") == 1);
    CHECK(!um::inst_note_on(7, 4, 64.0f, 1.0f));
    CHECK(logged("past the arena's 8 states (set states in its rmp3_plugin)") == 1);   // (once)
  }

  // ---- the arena comes back (M3): reloads and replacements reuse it, the pool does not grow
  auto pool_used = [] {
    um::publish_now();
    static char js[16384];
    CHECK(um::json_copy(js, sizeof js) > 0);
    std::string j = js;
    size_t a = j.find("\"plugpool\":[");
    CHECK(a != std::string::npos);
    return std::stoul(j.substr(j.find(',', a) + 1));
  };
  {
    unsigned long u0 = pool_used();
    for (int k = 0; k < 6; ++k) cmd("um=reload:1");
    CHECK(pool_used() == u0);
    for (int k = 0; k < 4; ++k) cmd("um=assign:5:rytm.api3_polyar"), cmd("um=assign:5:rytm.api3_test");
    CHECK(pool_used() == u0);
    CHECK(logged("is in pieces") == 0);   // (no full reload)
    // N1: U5's block in the free list (U4's above it), a plugin without an arena in U5, then U5's old plugin again:
    // its block reused, the pool's use unchanged
    cmd("um=assign:5:rytm.api3_test");
    cmd("um=assign:4:rytm.api3_polyar");
    unsigned long u1 = pool_used();
    cmd("um=assign:5:rytm.api3_poly");
    CHECK(pool_used() == u1);
    cmd("um=assign:5:rytm.api3_test");
    CHECK(pool_used() == u1);
    cmd("um=assign:4:rytm.api3_stereo");
  }

  // ---- N3: a fault mutes the slots sharing the image, from every path; a reload never shares a muted image; late
  // calls mute only their own slot
  cmd("um=assign:1:rytm.api3_test");
  cmd("um=assign:3:rytm.api3_test");
  CHECK(logged("um: U3 rytm.api3_test: shares U1's image, its own arena") >= 1);
  report(0, 0, 60, 16);   // U1's params() faults
  CHECK(um::slot_view(0).muted && um::slot_view(2).muted);
  CHECK(std::string(um::slot_view(2).why) == "muted: shares the image of U1, which faulted");
  {
    int before = logged("um: U1 rytm.api3_test: shares U3's image");
    cmd("um=reload:1");   // U3 is muted: U1 loads a fresh image, not U3's
    CHECK(logged("um: U1 rytm.api3_test: shares U3's image") == before);
    CHECK(um::slot_view(0).state == RMP_SLOT_READY && !um::slot_view(0).muted && um::slot_view(2).muted);
  }
  cmd("um=reload:3");   // (now U3 shares U1's fresh image)
  CHECK(!um::slot_view(2).muted);
  um::call_stall_limit(1000000);
  host_fake_time(false, 0);   // three late params() on U1: U1 muted, U3 (same image) not
  report(0, 0, 60, 0);
  report(0, 0, -1, 32);
  report(0, 0, -1, 32);
  report(0, 0, -1, 32);
  host_fake_time(true, g_t);
  um::call_stall_limit(0);
  CHECK(um::slot_view(0).muted && !um::slot_view(2).muted);
  cmd("um=reload:1");
  report(0, 0, 60, 0);
  um::render_fault(0);   // a fault in voice 0's render (the board's render guard): U1 and its sharer U3
  idle_all();
  CHECK(std::string(um::slot_view(0).why) == "muted: crashed in render (voice's guard)" && um::slot_view(2).muted);
  cmd("um=reload:1");
  cmd("um=reload:3");

  // ---- a user effect on core 2 (the hooks, as the send FX calls them): the transport's edges once per state, a
  // fault, a render over its budget, a render that never returns (fx_stop gives up, bounded)
  cmd("um=fx:2:fx.api3_fx");
  CHECK(um::slot_view(6).state == RMP_SLOT_READY && fx::user_ready(1));
  {
    float x[40] = {0}, y[40][2];
    uint8_t k[6] = {65, 0, 0, 0, 0, 0};
    auto run = [&](int sl, int n) {
      for (int i = 0; i < n; ++i) y[i][0] = y[i][1] = 0.0f;
      fx::user_run(1, sl, x, n, y, k);
    };
    run(0, 12);
    CHECK(near(y[0][0], 0.5f) && near(y[1][0], 0.0f) && near(y[2][0], 12.0f / 64));
    lfo::clock(0xFA, 0);   // Start: one block's START
    um::pre_block();
    run(0, 40);
    CHECK(near(y[1][0], 1.0f / 16) && near(y[17][0], 0.0f) && near(y[18][0], 16.0f / 64));   // the first sub-block only
    run(0, 12);
    CHECK(near(y[1][0], 0.0f));   // the same block again: not twice
    run(1, 12);
    CHECK(near(y[1][0], 1.0f / 16));   // send FX 2's state sees it once too
    um::pre_block();
    um::pre_block();   // two blocks the effect did not see: no edge in them, none delivered
    run(0, 12);
    CHECK(near(y[1][0], 0.0f));
    lfo::clock(0xFC, 0);
    um::pre_block();
    um::pre_block();   // a STOP in a block the effect skipped still reaches it, once
    run(0, 12);
    CHECK(near(y[1][0], 2.0f / 16));
    run(0, 12);
    CHECK(near(y[1][0], 0.0f));
    // a fault in render: muted, the why from the control core, the slot back with um=reload
    k[2] = 16;
    run(0, 12);
    CHECK(!fx::user_ready(1) && y[0][0] == 0.0f);
    idle_all();
    CHECK(std::string(um::slot_view(6).why).find("muted: crashed in render at +0x") == 0);
    cmd("um=reload:f2");
    CHECK(fx::user_ready(1));
    // over its budget (real time): muted after a run of slow calls, or at once past the FIFO
    k[2] = 32;
    host_fake_time(false, 0);
    for (int i = 0; i < 40 && fx::user_ready(1); ++i) run(0, 12);
    host_fake_time(true, g_t);
    CHECK(!fx::user_ready(1));
    idle_all();
    CHECK(std::string(um::slot_view(6).why).find("muted: render ran ") == 0);
    cmd("um=reload:f2");
    CHECK(fx::user_ready(1));
  }

  // ---- the JSON says it
  um::publish_now();
  static char js[16384];
  CHECK(um::json_copy(js, sizeof js) > 0);
  std::string j = js;
  CHECK(j.find("\"api\":3") != std::string::npos && j.find("\"plugpool\":[67108864,") != std::string::npos);
  // the package list says each package's own API (a dual one's header says 2) and marks the effects
  auto entry = [&](const char* id) {
    size_t a = j.find(std::string("{\"t\":\"plugin\",\"id\":\"") + id + "\"");
    CHECK(a != std::string::npos);
    return j.substr(a, j.find('}', a) - a);
  };
  CHECK(entry("rytm.api3_test").find("\"api\":3") != std::string::npos && entry("rytm.api3_test").find("effect") == std::string::npos);
  CHECK(entry("rytm.api3_initcrash").find("\"api\":3") != std::string::npos);
  CHECK(entry("rytm.api3_only").find("\"api\":3") != std::string::npos);
  CHECK(entry("fx.pingpong").find("\"api\":3,\"effect\":true") != std::string::npos);
  CHECK(entry("fx.api3_fx").find("\"effect\":true") != std::string::npos);
  // ---- N5: a send slot's state not called for blocks: edges from before are not news, its own block's are
  {
    float x[12] = {0}, y[12][2];
    uint8_t k[6] = {65, 0, 0, 0, 0, 0};
    auto run = [&](int sl) {
      for (int i = 0; i < 12; ++i) y[i][0] = y[i][1] = 0.0f;
      fx::user_run(1, sl, x, 12, y, k);
    };
    run(1);
    lfo::clock(0xFA, 0);
    um::pre_block();
    um::pre_block();
    um::pre_block();
    um::pre_block();
    run(1);
    CHECK(near(y[0][0], 0.5f) && near(y[1][0], 0.0f));   // a Start three blocks ago: not news
    lfo::clock(0xFC, 0);
    um::pre_block();
    run(1);
    CHECK(near(y[1][0], 2.0f / 16));   // the Stop of its own block: delivered
  }
  // ---- N3 on core 2: UFX2 and UFX3 share an image; a fault in UFX2's render mutes UFX3 too
  cmd("um=fx:3:fx.api3_fx");
  CHECK(fx::user_ready(2));
  {
    float x[12] = {0}, y[12][2] = {};
    uint8_t k[6] = {65, 0, 16, 0, 0, 0};
    fx::user_run(1, 0, x, 12, y, k);
    idle_all();
    CHECK(!fx::user_ready(1) && !fx::user_ready(2));
    CHECK(std::string(um::slot_view(7).why) == "muted: shares the image of UFX2, which faulted");
  }
  cmd("um=fx:3:-");
  cmd("um=reload:f2");
  // ---- N4: the budget per send slot (a slow slot 0 is not reset by a fast slot 1), and core 2's total
  host_fake_time(false, 0);
  {
    float x[12] = {0}, y[12][2];
    uint8_t slow[6] = {65, 0, 80, 0, 0, 0}, fast[6] = {65, 0, 0, 0, 0, 0};   // mode 5 (a little), mode 0
    CHECK(cmd("um=fxbudget:1:1000000").find("1 us a call") != std::string::npos);
    for (int i = 0; i < 40 && fx::user_ready(1); ++i) {
      fx::user_run(1, 0, x, 12, y, slow);
      fx::user_run(1, 1, x, 12, y, fast);
    }
    CHECK(!fx::user_ready(1));
    idle_all();
    CHECK(std::string(um::slot_view(6).why).find("muted: render ran ") == 0 &&
          std::string(um::slot_view(6).why).find("over its budget (1 us a block)") != std::string::npos);
    cmd("um=reload:f2");
    cmd("um=fx:3:fx.pingpong");   // a second effect, so the dearest is chosen
    CHECK(cmd("um=fxbudget:1000000:1").find("1 us all of core 2's") != std::string::npos);
    for (int b = 0; b < 40 && fx::user_ready(1); ++b) {   // each block: UFX2 in both send slots (slow), UFX3 (fast)
      um::pre_block();
      fx::user_run(1, 0, x, 12, y, slow);
      fx::user_run(1, 1, x, 12, y, slow);
      fx::user_run(2, 0, x, 12, y, fast);
    }
    um::pre_block();
    fx::user_run(2, 0, x, 12, y, fast);
    CHECK(!fx::user_ready(1) && fx::user_ready(2));   // the dearest muted, the other plays on
    idle_all();
    CHECK(std::string(um::slot_view(6).why).find("muted: ran ") == 0 && std::string(um::slot_view(6).why).find("dearest over core 2's share (1 us)") !=
          std::string::npos);
    cmd("um=fxbudget:120:150");
    cmd("um=fx:3:-");
    cmd("um=reload:f2");
  }
  // ---- N2: a render fx_stop gives up on (~30 ms): the new load is not muted by its late return, the old one's memory
  // is kept out of reuse until it is back, and the flag clears once it is
  {
    static float x[12], y[12][2];
    static uint8_t k[6] = {65, 0, 64, 0, 0, 0};   // mode 4
    CHECK(fx::user_ready(1));
    std::thread t([] { fx::user_run(1, 0, x, 12, y, k); });
    usleep(2000);
    cmd("um=fx:2:-");
    CHECK(logged("um: UFX2 fx.api3_fx: core 2 never left its render (5 ms): left behind") == 1);
    cmd("um=fx:2:fx.api3_fx");   // (no second 5 ms wait: it is known to be left behind)
    CHECK(um::slot_view(6).state == RMP_SLOT_READY);
    um::publish_now();
    static char js[16384];
    CHECK(um::json_copy(js, sizeof js) > 0 && std::string(js).find("\"ufx\":2,\"id\":\"fx.api3_fx\",\"state\":\"READY\",\"muted\":false,\"left_behind\":true") != std::string::npos);
    t.join();   // its late return: over 2 ms, but of the load before
    CHECK(fx::user_ready(1) && !um::slot_view(6).muted);
    cmd("um=reload:f2");   // back: no longer left behind
    um::publish_now();
    CHECK(um::json_copy(js, sizeof js) > 0 && std::string(js).find("\"ufx\":2,\"id\":\"fx.api3_fx\",\"state\":\"READY\",\"muted\":false,\"left_behind\":false") != std::string::npos);
  }
  host_fake_time(true, g_t);

  // ---- the Rytm's TYPE list follows the UFX slots: an assign sends a fresh 0x21 with the effect at TYPE 64 (rate-
  // limited, from the control path), an empty one without it; a project change keeps a loaded effect's name
  {
    cmd("um=fx:1:-");
    g_t += 200000, host_fake_time(true, g_t);
    sendfx_step();
    g_msgs.clear();
    cmd("um=fx:1:fx.pingpong");
    g_t += 200000, host_fake_time(true, g_t);
    CHECK(sendfx_step());
    CHECK(listed(64, "PINGPONG"));
    CHECK(!sendfx_step());   // (sent: nothing owed)
    g_msgs.clear();
    cmd("um=fx:1:-");
    g_t += 200000, host_fake_time(true, g_t);
    CHECK(sendfx_step() && !listed(64, "PINGPONG"));
    for (auto& m : g_msgs) CHECK(m[4] != 0x21 || std::string(m.begin(), m.end()).find("PINGPONG") == std::string::npos);
    cmd("um=fx:1:fx.pingpong");
    g_t += 200000, host_fake_time(true, g_t);
    sendfx_step();
    g_msgs.clear();
    Fs fs2;
    CHECK(fs2.mkdir("/.rytm/projects/B3"));
    sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'B', '3', 0xf7});   // another project: its fxslots file names nothing
    idle_all();
    for (int i = 0; i < 20; ++i) g_t += 1000, host_fake_time(true, g_t), sendfx_step();
    CHECK(!strcmp(core::project, "B3"));
    CHECK(listed(64, "PINGPONG") && fx::user[0].tag == fx::fx_tag("PINGPONG"));
    sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'A', '3', 0xf7});
    idle_all();
    for (int i = 0; i < 20; ++i) g_t += 1000, host_fake_time(true, g_t), sendfx_step();
  }

  // ---- last (it leaves a thread spinning in the effect's code): a render that never returns. fx_stop gives up after
  // its bounded wait, the UFX is left behind (its memory never reused), and the control core goes on
  {
    static float x[12], y[12][2];
    static uint8_t k[6] = {65, 0, 48, 0, 0, 0};
    std::thread t([] { fx::user_run(1, 0, x, 12, y, k); });
    t.detach();
    usleep(20000);
    host_fake_time(false, 0);
    cmd("um=fx:2:-");
    CHECK(logged("um: UFX2 fx.api3_fx: core 2 never left its render (5 ms): left behind") == 2);
    CHECK(!fx::user_ready(1));
  }
  printf("test_api3: PASS\n");
  return 0;
}
#else
int main() {
  printf("test_api3: RMPA images run on an AArch64 host only: skipped\n");
  return 0;
}
#endif
