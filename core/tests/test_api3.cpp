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
#include "hooks.h"
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
static void out(const uint8_t*, int) {}
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
  for (const char* n : {"api3_test", "api3_only", "api3_initcrash", "api3_needs", "api3_big"})
    put(std::string("/Plugins/") + n + ".rmp", slurp(std::string("tests/api3_pkgs/") + n + ".a64.rmp"));
  hooks.to_rytm = out;
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

  // ---- an API 3 only package on U2 (also a machine in the Rytm's list)
  cmd("um=assign:2:rytm.api3_only");
  v = um::slot_view(1);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3);
  report(1, 1, 60, 0);
  f = block(1);
  CHECK(near(f[0], 0.5f) && near(f[5], 2.0f / 16));
  CHECK(um::installed() == 5);   // (every probe package lists: they are machines; the BAD ones too until loaded)

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
  // three late params() in a row (real time: the probe's ~2 ms loop)
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

  // ---- the JSON says it
  um::publish_now();
  static char js[16384];
  CHECK(um::json_copy(js, sizeof js) > 0);
  std::string j = js;
  CHECK(j.find("\"api\":3") != std::string::npos && j.find("\"plugpool\":[67108864,") != std::string::npos);
  printf("test_api3: PASS\n");
  return 0;
}
#else
int main() {
  printf("test_api3: RMPA images run on an AArch64 host only: skipped\n");
  return 0;
}
#endif
