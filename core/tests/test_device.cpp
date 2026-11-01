// core/tests/test_device.cpp - the device profiles (core/device/device.h, docs/device-profiles.md): the Rytm by default
// and untouched; a second, minimal box picked at its hello, with its own SysEx identity, message ids, track and
// parameter counts; what it may and may not reach; the way back to the Rytm.
#include <cmath>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/device.h"
#include "device/digitakt.h"
#include "fmt/fmt.h"
#include "engine.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "lfo.h"
#include "pool/pool.h"
#include "sysex.h"
#include "ui/remote.h"
#include "host_link.h"

using namespace core;

static int g_restarts = 0;
static bool exists_on_card(const char* root, const char* p) {   // past the engine's own paths: where it really is
  struct stat st;
  return stat((std::string(root) + p).c_str(), &st) == 0;
}
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void send(const std::vector<uint8_t>& m) { host_sysex(m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static int count(uint8_t cmd) {
  int n = 0;
  for (auto& m : g_out) n += m[4] == cmd;
  return n;
}

static constexpr uint16_t kBoxId = 0x3F00;   // the range for private tests (docs/device-profiles.md)
// Two tracks, three parameters: P1 ENG = 12 (a Plaits model), HARM from its parameter 2, TIMB from 0, MORPH fixed.
static const device::Profile kBox = {
    kBoxId, "Test box", 0x1234, 0x0001,
    4, 2, 3,
    {0x7D, 'T', 'B'},
    0x61, 0x62, 0x63,
    0,
    kPlaits, 0,
    {-1, 2, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {12, 0, 0, 33, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100},
    {1u << 0x15},   // of the engine's messages it takes only 15
    nullptr};

static std::vector<uint8_t> box(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 'T', 'B'};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static std::vector<uint8_t> box_trig(int track, int note, int vel, int a, int b, int c) {
  return box({0x61, (uint8_t)track, (uint8_t)note, (uint8_t)vel, (uint8_t)a, 0, (uint8_t)b, 64, (uint8_t)c, 0});
}
static std::vector<uint8_t> hello(int id, int proto = 1) {
  return sidecar({device::kCmdHello, 1, (uint8_t)(id >> 7), (uint8_t)(id & 0x7f), (uint8_t)proto, 'T', 'B'});
}

static int16_t ring[kBlock * 400][kVoices];
static double rms(int voice) {
  double s = 0;
  for (int i = 0; i < kBlock * 400; ++i) s += (double)ring[i][voice] * ring[i][voice];
  return std::sqrt(s / (kBlock * 400));
}
static void render() {
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < 400; ++b) render_voices(ring, kBlock * 400, b * kBlock, 0, kVoices, mach);
}

int main() {
  char tmpl[] = "/tmp/core-device-XXXXXX";
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

  // the default: the Rytm, nothing in between
  CHECK(!device::foreign() && device::active().id == device::kIdRytm && hooks.to_rytm == ::out);
  CHECK(device::rytm().voices == kVoices && device::rytm().params == 16 && device::rytm().tracks == 12);
  CHECK(device::rytm().usb_vid == 0x1935 && device::rytm().usb_pid == 0x0b48);
  send(sidecar({0x10, 'A', 'B'}));
  CHECK(!strcmp(project, "AB") && count(0x14) == 1);
  std::vector<uint8_t> trig = sidecar({0x04, 0, 60, 100});
  for (int k = 0; k < 16; ++k) trig.insert(trig.end() - 1, {64, 0});
  send(trig);
  CHECK(v[0].live && !v[0].daisy);
  voice_silence(0);

  // a hello nobody knows, or of another protocol: told so, the Rytm stays
  g_out.clear();
  send(hello(0x1234));
  CHECK(g_out.size() == 1 && g_out[0][4] == device::kCmdHello && g_out[0].size() == 10 && g_out[0][8] == device::kUnknown);
  CHECK(!device::foreign());
  CHECK(device::add(&kBox));
  CHECK(!device::add(&kBox));   // the id is taken
  g_out.clear();
  send(hello(kBoxId, 9));
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kBadProto && !device::foreign());

  // its hello: answered, and it is the box from here on; again: answered, nothing else (no link reset)
  g_out.clear();
  send(hello(kBoxId));
  CHECK(g_out.size() == 1 && g_out[0][6] == kBoxId >> 7 && g_out[0][7] == (kBoxId & 0x7f) && g_out[0][8] == device::kOk);
  CHECK(device::foreign() && device::active().id == kBoxId && hooks.to_rytm != ::out);
  send(box_trig(0, 48, 127, 10, 20, 30));
  CHECK(v[0].live);
  g_out.clear();
  send(hello(kBoxId));   // (its periodic hello: the voice keeps playing, no link reset)
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kOk);
  render();
  CHECK(rms(0) > 50);

  // its trig on its own head and ids: played through the map (P1 fixed, P2 its c, P3 its a + fraction 0, P4 fixed)
  send(box_trig(1, 48, 127, 10, 20, 30));
  CHECK(v[1].live && (int)v[1].p[0] == 12 && (int)v[1].p[1] == 30 && (int)v[1].p[2] == 10 && (int)v[1].p[3] == 33);
  CHECK((int)v[1].target == 48 + (int)roundf(v[1].p[6] - 64));
  render();
  CHECK(rms(1) > 50);
  // a params message moves them without a note; the fraction arrives
  send(box({0x62, 1, 40, 0, 0, 0, 50, 64}));
  CHECK((int)v[1].p[1] == 50 && fabsf(v[1].p[1] - 50.5f) < 0.01f && (int)v[1].p[2] == 40);
  // its note-off closes the voice as a 0x11
  send(box({0x63, 1}));

  // what it may not do: a track past its voices, a wrong length, another head, the Rytm's commands
  uint32_t d0 = device::dropped();
  send(box_trig(2, 48, 100, 1, 2, 3));
  CHECK(!v[2].live);
  send(box({0x62, 1, 40, 0}));
  send(sidecar({0x10, 'X', 'Y'}));
  CHECK(!strcmp(project, "AB"));   // the box does not move the Rytm's project
  send(trig);                      // a Rytm report's bytes: not this box's
  send(sidecar({0x23}));           // SENDFX ASK
  send(sidecar({0x30, 1, 0, 1, 0}));
  CHECK(!ui::remote_open());
  CHECK(device::dropped() >= d0 + 6);

  // the engine's messages to it: only what its profile takes
  g_out.clear();
  uint32_t d1 = device::dropped();
  uint8_t juno[13];
  to_rytm(juno, sysex_juno_knobs(juno, 0, (const uint8_t[6]){1, 2, 3, 4, 5, 6}));
  CHECK(g_out.size() == 1 && g_out[0][4] == 0x15);
  uint8_t pool_msg[12] = {0xf0, 0x7d, 0x52, 0x54, 0x14, 1, 0, 0xf7};
  to_rytm(pool_msg, 8);
  send_pool();
  CHECK(g_out.size() == 1 && device::dropped() > d1);

  // a Rytm plugged in: its hello 35 gives it back, handled as ever; the output is the platform's again
  const std::vector<uint8_t> h35 = sidecar({0x35, 1, 0, 100, 3});
  send(h35);
  CHECK(!device::foreign() && hooks.to_rytm == ::out && device::active().id == device::kIdRytm);
  CHECK(ui::remote_caps() == 3);
  send(sidecar({0x10, 'C', 'D'}));
  CHECK(!strcmp(project, "CD"));
  send(trig);
  CHECK(v[0].live);

  // reset() from a box
  send(hello(kBoxId));
  CHECK(device::foreign());
  device::reset();
  CHECK(!device::foreign() && hooks.to_rytm == ::out);

  // a box with a state folder of its own: answered "restart", the choice kept in /.rytm, one restart after the saves
  static const device::Profile kBox2 = [] { device::Profile p = kBox; p.id = kBoxId + 1; p.state_dir = "/.boxtest"; return p; }();
  CHECK(device::add(&kBox2));
  mkdir((std::string(tmpl) + "/.rytm").c_str(), 0755);
  g_out.clear();
  send(hello(kBoxId + 1));   // no restart hook on this board: told it has no such profile, nothing changes
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kUnknown && !device::foreign());
  hooks.device_restart = [] { ++g_restarts; };
  g_out.clear();
  send(hello(kBoxId + 1));
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kRestart && !device::foreign() && !device::g_link.state);
  send(hello(kBoxId + 1));   // its repeat before the restart: the same answer, one request
  for (int k = 0; k < 100 && !g_restarts; ++k) {
    device::step();
    fmt::post_step();
  }
  CHECK(g_restarts == 1 && exists_on_card(tmpl, "/.rytm/device"));
  // the next boot: the box's folder from the first file on; the Rytm's folder not touched, the Pi's files stay
  device::reset();
  device::boot();
  CHECK(device::foreign() && device::active().id == kBoxId + 1 && !strcmp(device::g_link.state, "/.boxtest"));
  {
    File f;
    CHECK(fs.mkdir("/.rytm/pools") && f.open("/.rytm/pools/P.txt", kWrite) && f.write("x", 1) == 1);
    f.close();
    CHECK(f.open("/.rytm/wifi.txt", kWrite));
    f.close();
    CHECK(exists_on_card(tmpl, "/.boxtest/pools/P.txt") && !exists_on_card(tmpl, "/.rytm/pools/P.txt"));
    CHECK(exists_on_card(tmpl, "/.rytm/wifi.txt") && !exists_on_card(tmpl, "/.boxtest/wifi.txt"));
    CHECK(f.open("/.rytm/pools/P.txt", kRead));   // the engine's own path reads it back
    f.close();
    CHECK(exists_on_card(tmpl, "/Samples") && fs.exists("/Samples"));   // shared content: one place
  }
  g_out.clear();
  send(hello(kBoxId + 1));   // after the restart: accepted
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kOk);
  // a Rytm plugged in now: its 35 asks for /.rytm, the next boot has no box
  send(h35);
  for (int k = 0; k < 100 && g_restarts < 2; ++k) {
    device::step();
    fmt::post_step();
  }
  CHECK(g_restarts == 2);
  device::reset();
  device::boot();
  CHECK(!device::foreign() && !device::g_link.state && hooks.to_rytm == ::out);
  {
    char b[64];
    const char* p = "/.rytm/pools/P.txt";
    CHECK(device::state_path(p, b, sizeof b) == p);   // the Rytm's paths: the very pointer
  }
  CHECK(!fs.exists("/.rytm/pools/P.txt"));

  // A stock Rytm on USB (never a 35) and a Digitakt: the driver's rytm_on_usb, restarts as boots, no loop
  {
    using device::kIdDigitakt;
    int boots = 0;
    auto steps = [&] { for (int k = 0; k < 40; ++k) { device::step(); fmt::post_step(); } };
    auto reboot = [&] {   // what a restart is: the choice on the card read by a fresh boot
      ++boots;
      device::reset();
      g_out.clear();
      device::boot();
    };
    auto hello = [&](uint16_t id) {
      send(sidecar({device::kCmdHello, 1, (uint8_t)(id >> 7), (uint8_t)(id & 0x7f), 1, 'D', 'T'}));
      return (int)g_out.back()[8];
    };
    auto folder = [] { return device::g_link.state ? device::g_link.state : "/.rytm"; };
    uint64_t t = 1000000000;
    host_fake_time(true, t);
    g_restarts = 0;
    device::reset();
    // a Digitakt's session
    CHECK(hello(kIdDigitakt) == device::kRestart);
    steps();
    CHECK(g_restarts == 1);
    reboot();
    CHECK(!strcmp(folder(), "/.digitakt") && hello(kIdDigitakt) == device::kOk);
    reboot();   // the next day: no Digitakt, a stock Rytm: its id alone goes home, once
    CHECK(!strcmp(folder(), "/.digitakt"));
    device::rytm_on_usb(true);
    steps();
    CHECK(g_restarts == 2);
    reboot();
    CHECK(!strcmp(folder(), "/.rytm"));
    device::rytm_on_usb(true);
    steps();
    CHECK(g_restarts == 2);
    // both on USB, the Rytm's id first: the Digitakt waits, for as long as the Rytm is there
    CHECK(hello(kIdDigitakt) == device::kWait);
    t += 3600000000ull, host_fake_time(true, t);
    CHECK(hello(kIdDigitakt) == device::kWait);
    steps();
    CHECK(g_restarts == 2 && !strcmp(folder(), "/.rytm"));
    // the same boot again, the Digitakt's hello before the Rytm has enumerated: the settle window holds it
    reboot();
    CHECK(hello(kIdDigitakt) == device::kWait);
    device::rytm_on_usb(true);
    CHECK(hello(kIdDigitakt) == device::kWait);
    steps();
    CHECK(g_restarts == 2);
    // the Rytm gone: the Digitakt alone gets through, past the window
    reboot();
    t += (uint64_t)device::kSettleMs * 1000 + 1000, host_fake_time(true, t);
    CHECK(hello(kIdDigitakt) == device::kRestart);
    steps();
    CHECK(g_restarts == 3);
    reboot();
    CHECK(!strcmp(folder(), "/.digitakt"));
    // its hello first, then a Rytm: the Digitakt keeps it
    CHECK(hello(kIdDigitakt) == device::kOk);
    device::rytm_on_usb(true);
    steps();
    CHECK(g_restarts == 3 && !strcmp(folder(), "/.digitakt"));
    // a Digitakt's USB id: rytm_on_usb is never called, so nothing moves without its hello
    reboot();
    steps();
    CHECK(g_restarts == 3 && !strcmp(folder(), "/.digitakt"));
    host_fake_time(false, 0);
    device::reset();
  }

  // A failed write of the choice: no restart, so no restart loop; the next hello tries again and goes through
  {
    device::reset();
    fmt::post_wait();
    remove((std::string(tmpl) + "/.rytm/device").c_str());
    g_restarts = 0;
    host_fail_open("/.rytm/device.tmp", true);
    for (int round = 0; round < 3; ++round) {
      send(hello(kBoxId + 1));
      for (int k = 0; k < 200 && device::leaving(); ++k) {
        device::step();
        fmt::post_step();
      }
      CHECK(!device::leaving() && g_restarts == 0);
    }
    host_fail_open("/.rytm/device.tmp", false);
    send(hello(kBoxId + 1));
    for (int k = 0; k < 200 && !g_restarts; ++k) {
      device::step();
      fmt::post_step();
    }
    CHECK(g_restarts == 1 && exists_on_card(tmpl, "/.rytm/device"));
  }

  // A restart comes after the saves: the LFOs' 1 s debounce not waited for, the project first, the choice last
  {
    device::reset();
    fmt::post_wait();
    remove((std::string(tmpl) + "/.rytm/device").c_str());
    static bool s_saved;
    s_saved = false, g_restarts = 0;
    hooks.device_restart = [] { ++g_restarts, s_saved = !lfo::dirty() && !project_dirty; };
    lfo::Settings ls = lfo::get(0);
    ls.depth = ls.depth == 20 ? 10 : 20;
    lfo::set(0, ls);
    CHECK(lfo::dirty());
    project_dirty = true;
    send(hello(kBoxId + 1));
    for (int k = 0; k < 200 && !g_restarts; ++k) idle_save();
    CHECK(g_restarts == 1 && s_saved);
    hooks.device_restart = [] { ++g_restarts; };
  }

  // A stock Digitakt's stereo mix plays POST and leaves the project's routes alone
  {
    char b[64];
    out::set_route(0, out::kPre);
    out::set_route(1, out::kTrk);
    out::set_device(device::kDigitakt.audio);
    CHECK(out::control.route[0] == out::kPost && out::control.route[1] == out::kPost);
    CHECK(out::line(0, b, sizeof b) > 0 && strstr(b, "OUT V1 PRE") && !out::line(1, b, sizeof b));
    out::set_route(2, out::kPre);   // a project read meanwhile: kept for the Rytm, POST plays
    CHECK(out::control.route[2] == out::kPost);
    out::set_device(nullptr);
    CHECK(out::control.route[0] == out::kPre && out::control.route[1] == out::kTrk && out::control.route[2] == out::kPre);
    out::set_route(0, out::kTrk), out::set_route(2, out::kTrk);
  }

  printf("test_device: ok\n");
  return 0;
}
