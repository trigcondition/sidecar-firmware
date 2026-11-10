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
#include "device/a4.h"
#include "device/digitakt.h"
#include "keys.h"
#include "fmt/fmt.h"
#include "engine.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "lfo.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "sysex.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voice_name.h"
#include "host_link.h"
#include "inst.h"
#include "poly.h"
#include "voices.h"

using namespace core;
using ui::extra_for_box;

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
  CHECK(device::box_screen() == nullptr && !strcmp(voice_name(0), "BD"));   // no Screen: the Rytm's words
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
    CHECK(device::foreign() && device::box_screen() == device::kDigitakt.screen && !strcmp(voice_name(0), "T1"));   // its words (L7: one read of the box)
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

  // An Analog Four MKII with its mod: its USB id claims nothing until its hello has chosen it; four voices; a Rytm's command dropped
  {
    using device::kIdA4;
    auto steps = [&] { for (int k = 0; k < 40; ++k) { device::step(); fmt::post_step(); } };
    auto hello = [&] {
      send(sidecar({device::kCmdHello, 1, (uint8_t)(kIdA4 >> 7), (uint8_t)(kIdA4 & 0x7f), 1, 'A', '4'}));
      return (int)g_out.back()[8];
    };
    g_restarts = 0;
    device::reset();
    CHECK(device::by_usb(device::kA4Vid, device::kA4Pid) == nullptr);   // a stock A4 beside a Rytm: nothing
    CHECK(hello() == device::kRestart);
    CHECK(device::by_usb(device::kA4Vid, device::kA4Pid) == nullptr);
    steps();
    CHECK(g_restarts == 1);
    device::reset();
    g_out.clear();
    device::boot();
    CHECK(device::active().id == kIdA4 && !strcmp(device::g_link.state, "/.a4"));
    CHECK(!strcmp(ui::tap_source_name(1), "T1") && !strcmp(ui::tap_source_name(4), "T4") && !strcmp(ui::tap_source_name(9), "IN L"));   // the box's own words on SCOPE, its voices on their channels
    CHECK(ui::tap_channel_of_voice(1) == 3 && ui::tap_channel_of_voice(3) == 5);
    CHECK(!strcmp(device::kA4.screen->keys, "UP + YES: PAGES"));
    CHECK(!strcmp(device::short_name(device::active()), "A4") && !strcmp(device::short_name(device::rytm()), "Rytm"));   // the box's word on the touchscreen
    CHECK(device::box_screen() == device::kA4.screen && !strcmp(voice_name(0), "T1") && !strcmp(voice_name(4), "-"));
    CHECK(device::by_usb(device::kA4Vid, device::kA4Pid) == &device::kA4);
    CHECK(hello() == device::kOk);
    const out::Device& a = *device::kA4.audio;
    CHECK(a.channels == device::kA4OutChannels && a.in_channels == device::kA4InChannels && a.ret == out::kPerVoice);
    for (int v = 0; v < 4; ++v) CHECK(a.voice[v] == 2 + v);
    for (int v = 4; v < 8; ++v) CHECK(a.voice[v] == out::kNoChannel);
    // the mod's reports are the engine's own, the machine in the voice byte
    uint8_t buf[device::kMaxMessage];
    auto passes = [&](std::vector<uint8_t> m) {
      const uint8_t* d = m.data();
      int len = (int)m.size();
      return device::translate(device::g_link, d, len, buf);
    };
    std::vector<uint8_t> tr = {0x04, 0x03 | 0x08, 60, 100};   // JUNO on voice 3
    for (int k = 0; k < 2 * 16 + 2; ++k) tr.push_back(k % 2 ? 0 : 64);
    CHECK(passes(sidecar(tr)));
    send(sidecar(tr));
    tr[1] = 0x04 | 0x08;                                       // no fifth voice
    CHECK(!passes(sidecar(tr)));
    CHECK(!passes(sidecar({0x33, 1, 2})));                     // a Rytm-only command
    // the sequencer's notes as MIDI: an all-off (note -1, seq.cpp's stop) is an off for each note held, never a data byte over 127
    {
      static std::vector<std::vector<uint8_t>> sent;
      sent.clear();
      keys::set_forward([](uint8_t st, uint8_t a, uint8_t b) { sent.push_back({st, a, b}); });
      device::box_note(1, 60, 100);
      device::box_note(1, 64, 90);
      device::box_note(1, 67, 80);
      device::box_note(1, 60, 0);                              // one let go by itself
      device::box_note(2, 48, 100);
      sent.clear();
      device::box_note(1, -1, 0);
      CHECK(sent.size() == 2);
      for (auto& m : sent) CHECK(m[0] == 0x81 && m[1] <= 127 && (m[1] == 64 || m[1] == 67) && m[2] == 0);
      sent.clear();
      device::box_note(1, -1, 0);                              // nothing held: nothing sent
      CHECK(sent.empty());
      device::box_note(2, -1, 0);                              // another voice's stays its own
      CHECK(sent.size() == 1 && sent[0][0] == 0x82 && sent[0][1] == 48);
      keys::set_forward(nullptr);
    }
    g_out.clear();
    send(sidecar({device::kCmdHello, 1, (uint8_t)(kIdA4 >> 7), (uint8_t)(kIdA4 & 0x7f), 1, 'A', '4'}));
    CHECK(g_out.size() == 1);                                  // only the hello's answer goes back
    fmt::post_step();
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
    hooks.device_restart = [] { ++g_restarts, s_saved = !lfo::dirty() && !project_dirty && !seq::unsaved(); };
    for (int k = 0; k < 200000 && project_loading(); ++k) idle_save();
    lfo::Settings ls = lfo::get(0);
    ls.depth = ls.depth == 20 ? 10 : 20;
    lfo::set(0, ls);
    CHECK(lfo::dirty());
    project_dirty = true;
    char sb[80];
    CHECK(seq::command("seq=lat:200", sb, sizeof sb) && seq::unsaved());   // held for kSaveMs otherwise: edits of the last second lost at the restart
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

  // The voices' own-channel trim is the profile's: the A4's insert point sits low, the Rytm and the Digitakt stay at 0, bit for bit
  {
    device::reset();
    CHECK(out::control.trk10 == 0);
    CHECK(device::rytm().trk10 == 0 && device::kDigitakt.trk10 == 0 && device::kA4.trk10 == 180);
    CHECK(device::kA4.audio->loop_frames == 197 && device::kDigitakt.audio->loop_frames == 0);   // LIVE IN's latency, the chop's
    device::g_link.state = "/.a4";                     // the folder chosen at boot: this hello is accepted
    send(hello(device::kA4.id));
    CHECK(device::foreign() && device::active().id == device::kIdA4);
    CHECK(out::control.trk10 == 180);
    out::set_trim(2, 60);                              // a live stereo=trim:trk
    send(hello(device::kA4.id));                       // a repeated hello is no change of box
    CHECK(out::control.trk10 == 60);
    static int s_user = -1;                            // U1..U5: the A4 reports them as the Rytm does, the voice byte's flag
    hooks.user_report = [](int, int s, int, int, int, int) { s_user = s; };
    std::vector<uint8_t> t = sidecar({0x04, 0x50 + 1, 60, 100});
    t.pop_back();
    for (int k = 0; k < 16; ++k) t.push_back(64), t.push_back(0);
    t.push_back(0), t.push_back(0), t.push_back(0xf7);
    usleep((device::kEchoMs + 5) * 1000);              // (60 went to the box on this voice above: past its echo)
    send(t);
    CHECK(s_user == 0);
    hooks.user_report = nullptr;
    device::reset();                                   // back to the Rytm
    CHECK(!device::foreign() && out::control.trk10 == 0);
    {   // the usual way in: the boot after its hello's restart picks the box, and its hello then changes nothing
      Fs f;
      File w;
      CHECK(w.open("/.rytm/device", kWrite));
      CHECK(w.write((const uint8_t*)"3\n", 2) == 2);
      w.close();
      device::boot();
      CHECK(device::active().id == device::kIdA4 && out::control.trk10 == 180);
      send(hello(device::kA4.id));
      CHECK(out::control.trk10 == 180);
      device::reset();
      CHECK(f.remove("/.rytm/device"));
    }
  }

  // A poly track's chord on the A4's voices: one 04 a note, on the voice's machine; a lone trig is what it always was
  {
    auto trig = [](int vb, int note, int vel = 100) {
      std::vector<uint8_t> t = {0x04, (uint8_t)vb, (uint8_t)note, (uint8_t)vel};
      for (int k = 0; k < 2 * 16 + 2; ++k) t.push_back(k % 2 ? 0 : 64);
      return sidecar(t);
    };
    auto off = [](int vi) { return sidecar({0x11, (uint8_t)vi}); };
    auto notes = [](int vi) { return inst::voice_notes(vi) + poly::voice_notes(vi); };
    auto quiet = [&](int vi) { send(off(vi)); voice_silence(vi); usleep(25000); };
    device::g_link.state = "/.a4";
    send(hello(device::kA4.id));
    CHECK(device::kA4.chords && !device::rytm().chords && !device::kDigitakt.chords);

    for (int m : {kPlaits, kJuno, kDaisy}) {   // three notes: the voice's own machine, two beside it, in order
      inst::reset();
      send(trig(m | 1, 60));
      CHECK(notes(1) == 0);                    // a lone trig: no instance, as ever
      send(trig(m | 1, 64));
      send(trig(m | 1, 67));
      CHECK(inst::voice_notes(1) == 2);
      send(trig(m | 1, 64));                   // twice in a chord is once
      CHECK(inst::voice_notes(1) == 2);
      send(off(1));                            // 0x11 lets all of them go
      CHECK(inst::stat(1).sounding <= 2);
      quiet(1);
      CHECK(notes(1) == 0);
    }
    {   // 6-OP FM: its pool, released with the voice
      send(trig(kFm | 2, 60));
      CHECK(poly::voice_notes(2) == 0);
      send(trig(kFm | 2, 64));
      send(trig(kFm | 2, 67));
      send(trig(kFm | 2, 71));
      CHECK(poly::voice_notes(2) == 3 && poly::held_notes(2) == 3);
      send(off(2));
      CHECK(poly::held_notes(2) == 0);
      quiet(2);
      poly::silence(2);
    }
    {   // MD plays no second note: a chord is its first hit
      static int hits;
      auto seen = hooks.trig_seen;
      hooks.trig_seen = [] { ++hits; };
      hits = 0;
      send(trig(kMd | 3, 60));
      send(trig(kMd | 3, 64));
      send(trig(kMd | 3, 67));
      CHECK(hits == 1);                        // one hit: a chord's other notes are no flam
      quiet(3);
      hits = 0;
      send(trig(kMd | 3, 60));
      send(off(3));                            // the mod's mono line: an 11 between, two hits
      send(trig(kMd | 3, 64));
      CHECK(hits == 2);
      quiet(3);
      hits = 0;
      send(trig(kMd | 3, 60));
      usleep((device::kChordMs + 10) * 1000);
      send(trig(kMd | 3, 64));                 // too far apart: two hits
      CHECK(hits == 2);
      hooks.trig_seen = seen;
      quiet(3);
      send(trig(kMd | 3, 60));
      send(trig(kMd | 3, 64));
      CHECK(notes(3) == 0);
      quiet(3);
    }
    {   // too far apart, or an 11 between (the mod's mono track): notes of a line, not a chord
      send(trig(kPlaits | 0, 60));
      usleep((device::kChordMs + 5) * 1000);
      send(trig(kPlaits | 0, 64));
      CHECK(notes(0) == 0);
      usleep((device::kChordMs + 5) * 1000);
      send(trig(kPlaits | 0, 60));
      send(off(0));
      send(trig(kPlaits | 0, 64));
      CHECK(notes(0) == 0);
      send(trig(kPlaits | 0, 60));             // the same note at once: a retrig
      CHECK(notes(0) == 1);                    // (60 and 64: this one a chord's second note)
      quiet(0);
    }
    {   // a new chord with no off, in the old one's tail: the old notes go
      send(trig(kPlaits | 0, 60));
      send(trig(kPlaits | 0, 64));
      usleep((device::kChordMs + 5) * 1000);
      send(trig(kPlaits | 0, 62));
      CHECK(inst::stat(0).sounding <= 1);
      quiet(0);
    }
    {   // a machine change inside the window is no chord
      send(trig(kPlaits | 0, 60));
      send(trig(kJuno | 0, 64));
      CHECK(notes(0) == 0);
      quiet(0);
    }
    {   // the sequencer's chord to the box: each note's echo is dropped, the voice plays it once
      uint32_t before = inst::voice_notes(0);
      static std::vector<std::vector<uint8_t>> sent;
      keys::set_forward([](uint8_t st, uint8_t a, uint8_t b) { sent.push_back({st, a, b}); });
      device::box_note(0, 60, 100);
      device::box_note(0, 64, 100);
      device::box_note(0, 67, 100);
      const uint8_t* d;
      uint8_t buf[device::kMaxMessage];
      for (int n : {60, 64, 67}) {
        auto m = trig(kPlaits | 0, n);
        d = m.data();
        int len = (int)m.size();
        CHECK(!device::translate(device::g_link, d, len, buf));
      }
      auto m = trig(kPlaits | 0, 67);          // once only
      d = m.data();
      int len = (int)m.size();
      CHECK(device::translate(device::g_link, d, len, buf));
      device::box_note(0, -1, 0);
      keys::set_forward(nullptr);
      CHECK(inst::voice_notes(0) == before);
      quiet(0);
    }
    {   // a MIDI channel bound and in use (the A4's note send on 6-OP FM): its notes are the voice's, no second copy from the trigs
      send(trig(kFm | 2, 60));
      poly::midi(0x92, 60, 100);
      send(off(2));
      poly::midi(0x82, 60, 0);
      usleep(40000);
      for (int n = 0; n < 2; ++n) {
        poly::midi(0x92, 60, 100);
        send(trig(kFm | 2, 60));
        poly::midi(0x82, 60, 0);
        usleep(40000);
      }
      CHECK(poly::live_now(2));
      poly::midi(0x92, 60, 100);
      poly::midi(0x92, 64, 100);
      send(trig(kFm | 2, 60));
      send(trig(kFm | 2, 64));
      CHECK(poly::held_notes(2) == 2);
      poly::midi(0x82, 60, 0);
      poly::midi(0x82, 64, 0);
      quiet(2);
      poly::reset();
    }
    {   // what is heard, tonal settings through the real report: each note of the chord on the voice's one channel
      auto tuned = [](int vb, int note) {
        static const uint8_t pl[16] = {93, 64, 0, 0, 127, 0, 64, 127, 64, 64, 0, 64, 64, 0, 0, 100};   // a tonal PLAITS engine, a long DEC, TUN and FM centred, LEV up, GLD 0, VOL 100
        std::vector<uint8_t> t = {0x04, (uint8_t)vb, (uint8_t)note, 100};
        for (int k = 0; k < 16; ++k) t.push_back(pl[k]), t.push_back(0);
        t.push_back(0), t.push_back(0);
        return sidecar(t);
      };
      auto tone = [](int vi, int note) {          // Goertzel, one ring: 0.1 s
        double f = 440 * std::pow(2.0, (note - 69) / 12.0), k = 2 * std::cos(2 * M_PI * f / 48000), a = 0, b = 0;
        for (int i = 0; i < kBlock * 400; ++i) {
          double c = ring[i][vi] / 32768.0 + k * a - b;
          b = a, a = c;
        }
        return a * a + b * b - k * a * b;
      };
      auto heard = [&](double* o) {               // power over 0.8 s: a chord beats, one window says little
        double p[3] = {0, 0, 0};
        for (int w = 0; w < 8; ++w) {
          render();
          p[0] += tone(1, 60), p[1] += tone(1, 64), p[2] += tone(1, 66);
        }
        for (int k = 0; k < 3; ++k) o[k] = 10 * std::log10(p[k] / 8 + 1e-12);
      };
      for (int m : {kPlaits, kJuno}) {
        send(tuned(m | 1, 60));                   // the engine in place
        render();
        double alone[3], other[3], ch[3];
        const int ns[3] = {60, 64, 66};
        for (int k = 0; k < 3; ++k) {             // each note by itself on the voice's machine
          quiet(1);
          render();
          send(tuned(m | 1, ns[k]));
          double p[3];
          heard(p);
          alone[k] = p[k];
          if (k == 0) other[1] = p[1], other[2] = p[2];
        }
        quiet(1);
        render();
        for (int n : ns) send(tuned(m | 1, n));
        CHECK(inst::voice_notes(1) == 2);
        heard(ch);
        printf("  chord, machine %02x: dB on 60 64 66, each alone %.1f %.1f %.1f, 60's own on the others %.1f %.1f; in the chord %.1f %.1f %.1f\n", m, alone[0], alone[1], alone[2], other[1], other[2], ch[0], ch[1], ch[2]);
        for (int k = 1; k < 3; ++k) CHECK(ch[k] > other[k] + 12 && ch[k] > alone[k] - 6);   // there, about 3 dB under alone: the headroom gain
        quiet(1);
        render();
      }
    }
    {   // the Rytm: the same bytes are what they always were, no chord
      device::reset();
      device::g_link.state = nullptr;
      send(trig(kPlaits | 1, 60));
      send(trig(kPlaits | 1, 64));
      send(trig(kPlaits | 1, 67));
      CHECK(notes(1) == 0);
      send(off(1));
      voice_silence(1);
    }
  }

  {  // the A4's screen: the pages' messages alone, their text in the pixels, its hello never the Rytm's 35, the Rytm's pages hidden
    device::reset();
    device::g_link.state = "/.a4";
    send(hello(device::kA4.id));
    CHECK(device::foreign() && device::active().id == device::kIdA4);
    ui::remote_reset();
    g_out.clear();
    send(sidecar({0x36, 1, 0, 100, 1}));
    CHECK(ui::remote_caps() == 1 && device::active().id == device::kIdA4);
    CHECK(!strcmp(voice_name(0), "T1") && !strcmp(voice_name(3), "T4"));
    g_out.clear();
    send(sidecar({0x30, 1, 0, 1, 0}));
    CHECK(ui::remote_open());
    uint32_t ms = 1000;
    ui::remote_tick(ms);
    CHECK(count(0x37) == 1 && count(0x38) >= 8 && count(0x39) == 1);
    for (auto& m : g_out) CHECK(m[4] == 0x37 || m[4] == 0x38 || m[4] == 0x39 || m[4] == 0x3C);
    uint32_t dropped0 = device::dropped();
    int on = 0;
    const uint8_t* fb = ui::remote_frame();
    for (int x = 0; x < 128; ++x) on += __builtin_popcount(fb[7 + 8 * x]);   // the title bar's page
    CHECK(on > 100);
    for (int k = 0; k < 24; ++k) {
      send(sidecar({0x31, 1, 0, 0x41, 1}));
      send(sidecar({0x31, 1, 0, 0x41, 0}));
      ui::remote_tick(ms += 50);
      const char* t = ui::remote_page_title();
      for (const char* no : {"SEQ", "SONG", "POOL", "SAMPLE", "AUTO", "INSERT", "MASTER", "PROJECT", "KEYS", "ARP", "POLY"}) CHECK(!t || strcmp(t, no));
      CHECK(!t || extra_for_box(t));
    }
    CHECK(device::dropped() == dropped0);          // nothing it asked for was filtered
    send(sidecar({0x30, 1, 0, 0, 3}));
    CHECK(!ui::remote_open());
    ui::remote_reset();
    device::reset();
    CHECK(device::box_screen() == nullptr && !strcmp(voice_name(0), "BD"));
    CHECK(extra_for_box("SEQ"));
  }

  {  // the Digitakt's screen and slots (mod 0.3): the pages as the A4's, eight tracks T1..T8; a slot's 19 answered with its 1A, a 1D let
     // through; the Rytm's other messages still filtered
    device::reset();
    device::g_link.state = "/.digitakt";
    send(hello(device::kIdDigitakt));
    CHECK(device::foreign() && device::active().id == device::kIdDigitakt);
    CHECK((device::kDigitakt.features & (device::kRemoteUi | device::kUserSlots)) == (device::kRemoteUi | device::kUserSlots));
    CHECK(!(device::kDigitakt.features & (device::kProject | device::kKits | device::kSendFx | device::kPools | device::kRytmKeys)));
    CHECK(!strcmp(device::kDigitakt.screen->keys, "UP + YES: PAGES"));
    CHECK(device::box_screen() == device::kDigitakt.screen && !strcmp(voice_name(0), "T1") && !strcmp(voice_name(7), "T8"));
    g_out.clear();
    send(sidecar({0x19, 1, 2, 0, 0, 1}));            // ASK META, slot 2, generation 0
    CHECK(count(0x1A) >= 1);
    for (auto& m : g_out) CHECK(m[4] == 0x1A);
    g_out.clear();
    std::vector<uint8_t> slots = sidecar({0x1D, 1, 1, 1, 1, 1, 1});
    to_rytm(slots.data(), (int)slots.size());
    CHECK(count(0x1D) == 1);
    uint32_t d0 = device::dropped();
    g_out.clear();
    for (const std::vector<uint8_t>& m : {sidecar({0x3D, 1, 0}), sidecar({0x14, 0}), sidecar({0x1B, 1, 0, 0x21, 1, 1, 0})}) to_rytm(m.data(), (int)m.size());
    CHECK(g_out.empty() && device::dropped() == d0 + 3);   // the Rytm's text (drawn into the pixels for a box), a pool list, the installed list
    ui::remote_reset();
    send(sidecar({0x36, 1, 0, 100, 1}));
    CHECK(ui::remote_caps() == 1 && device::active().id == device::kIdDigitakt);
    g_out.clear();
    send(sidecar({0x30, 1, 0, 1, 0}));
    CHECK(ui::remote_open());
    uint32_t ms = 1000;
    ui::remote_tick(ms);
    CHECK(count(0x37) == 1 && count(0x38) >= 8 && count(0x39) == 1);
    for (auto& m : g_out) CHECK(m[4] == 0x37 || m[4] == 0x38 || m[4] == 0x39 || m[4] == 0x3C);
    uint32_t dropped0 = device::dropped();
    for (int k = 0; k < 24; ++k) {
      send(sidecar({0x31, 1, 0, 0x41, 1}));
      send(sidecar({0x31, 1, 0, 0x41, 0}));
      ui::remote_tick(ms += 50);
      const char* t = ui::remote_page_title();
      for (const char* no : {"SEQ", "SONG", "POOL", "SAMPLE", "AUTO", "INSERT", "MASTER", "PROJECT", "KEYS", "ARP", "POLY"}) CHECK(!t || strcmp(t, no));
      CHECK(!t || extra_for_box(t));
    }
    CHECK(device::dropped() == dropped0);
    send(sidecar({0x30, 1, 0, 0, 3}));
    CHECK(!ui::remote_open());
    ui::remote_reset();
    device::reset();
    CHECK(device::box_screen() == nullptr && !strcmp(voice_name(0), "BD"));
  }

  printf("test_device: ok\n");
  return 0;
}
