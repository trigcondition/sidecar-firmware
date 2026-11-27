// core/tests/test_midim.cpp - the MIDI machine (core/midim/midim.h, backlog 7o): no audio and no render, the words as messages on
// each destination (DIN through the 4F SysEx), the send order at a trig, locks, the OFF zones, a CC number's change, DST and CHN
// changes (the note's off follows the note), 0x11, mono trigs, polyphony (the sequencer's path, the pads' binding, the keyboards), sharing
// a note between voices, a full table, every way a note could hang (stop, machine change, kit change, link hello, leaving by a report),
// a report from an older host's view (0x04 on the same voice), the budget, the SRC view's knobs, realtime to USB-A, the bytes on the wire.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "core.h"
#include "inst.h"
#include "keys.h"
#include "midim/midim.h"
#include "poly.h"
#include "seq/seq.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

// everything sent: {dest, bytes}; DIN unpacked from its 4F
struct Msg {
  int d;
  std::vector<uint8_t> b;
};
static std::vector<Msg> g;
static bool g_usb_up = true;
static std::vector<std::vector<uint8_t>> g_rytm;   // everything for the Rytm
static void out(const uint8_t* m, int n) {
  g_rytm.emplace_back(m, m + n);
  if (n >= 9 && m[4] == midim::kDinCmd) {
    CHECK(m[5] == 1 && m[n - 1] == 0xF7);
    std::vector<uint8_t> b(m + 6, m + n - 1);
    CHECK(b.size() == 2 || b.size() == 3);
    b[0] |= 0x80;
    g.push_back({midim::kDin, b});
  }
}
static std::vector<std::vector<uint8_t>> g_thru;   // while g_loop: what USB-C sent, coming back on the computer's port
static bool g_loop = false;
static bool usb(int d, const uint8_t* m, int n) {
  if (!g_usb_up) return false;
  g.push_back({d, std::vector<uint8_t>(m, m + n)});
  if (g_loop && d == midim::kUsbC && n == 3) g_thru.emplace_back(m, m + n);
  return true;
}
static uint64_t g_us = 1000000;
static void ms(int n) {
  for (int i = 0; i < n; ++i) {
    g_us += 1000;
    host_fake_time(true, g_us);
    poly::tick(now_ms());
  }
}
static std::string dump() {
  std::string s;
  char b[16];
  for (auto& m : g) {
    snprintf(b, sizeof b, "%d:", m.d);
    s += b;
    for (uint8_t x : m.b) snprintf(b, sizeof b, "%02X", x), s += b;
    s += " ";
  }
  return s;
}
static void same(const char* want) {
  std::string d = dump();
  if (d != want) fprintf(stderr, "want [%s]\n got [%s]\n", want, d.c_str());
  CHECK(d == want);
  g.clear();
}

// P1 DST P2 CHN P3 BNK P4 SBK P5 PRG P6 PB P7 AT P8 MW P9..12 VL P13..16 CC
enum { DST, CHN, BNK, SBK, PRG, PB, AT, MW, VL1, VL2, VL3, VL4, CC1, CC2, CC3, CC4 };
static std::vector<int> words(int dst = 32, int ch0 = 0) {
  std::vector<int> p(16, 0);
  p[DST] = dst, p[CHN] = ch0 * 8;
  return p;
}
constexpr int kDin = 32, kUsbC = 64, kUsbA = 96;
static void rep(int cmd, int vi, int note, int vel, const std::vector<int>& p, int frac5 = 0) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vi};
  if (cmd == 0x44 || cmd == 0x04) m.push_back((uint8_t)note), m.push_back((uint8_t)vel);
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)p[k]), m.push_back((uint8_t)(k == PB ? frac5 : 0));
  m.push_back(0), m.push_back(0);   // the sound tag
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void off(int vi) {
  const uint8_t m[7] = {0xF0, 0x7D, 0x52, 0x54, 0x11, (uint8_t)vi, 0xF7};
  engine_sysex(m, 7);
}
static void hello() {
  const uint8_t m[11] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 0, 0, 0, 0xF7};
  engine_sysex(m, 11);
}
static void kit(int k) {
  const uint8_t m[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, (uint8_t)k, 0xF7};
  engine_sysex(m, 8);
}
static void machine33(int t, int machine, int vb) {
  const uint8_t m[11] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, (uint8_t)machine, (uint8_t)vb, 0xF7};
  engine_sysex(m, 11);
}
static void all33_analog() {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 12};
  for (int t = 0; t < 12; ++t) m.push_back(0), m.push_back(0x7F);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void transport(int s) {   // 2D 01 <kTransport> <state> <frame: 4 x 7 bits>
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, seq::kTransport, (uint8_t)s, 0, 0, 0, 0, 0xF7};
  engine_sysex(m, sizeof m);
}

int main() {
  char tmpl[] = "/tmp/core_midim_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples' '" + card + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  midim::set_out(usb);
  all33_analog();
  g.clear();

  // names, the host's number off the wire
  CHECK(machine_of(0x80) == kMidiMachine && !strcmp(machine_short(kMidiMachine), "MIDI"));
  CHECK(machine_of(0x7F) == 0x78);
  CHECK(!strcmp(machine_long(kMidiMachine), "MIDI"));

  // a trig on DIN channel 1: the note and nothing else (all OFF), and it renders nothing
  auto p = words();
  rep(0x44, 0, 60, 100, p);
  CHECK(v[0].mi && !v[0].live);
  same("1:903C64 ");
  off(0);
  same("1:803C00 ");
  CHECK(midim::voice_notes(0) == 0);
  {   // no render: the voice's ring rows stay zero, no machine time
    static int16_t ring[kBlock * 40][kVoices];
    memset(ring, 0x55, sizeof ring);
    uint64_t mach[kMachines] = {0};
    rep(0x44, 0, 60, 100, p);
    render_voices(ring, kBlock * 40, 0, 0, 1, mach);
    for (int s = 0; s < kBlock; ++s) CHECK(ring[s][0] == 0);
    for (int m = 0; m < kMachines; ++m) CHECK(mach[m] == 0);
    CHECK(inst::builtin(kMidiMachine)->own_pool && inst::builtin(kMidiMachine)->cpu_mean == 0);
    off(0);
    g.clear();
  }

  // velocity 0 is never a note-off; note 127 passes
  rep(0x44, 0, 127, 0, p);
  same("1:907F01 ");
  off(0);
  same("1:807F00 ");

  // the trig's order: bank, sub bank, program, bend, touch, wheel, the CCs, then the note
  p = words(kUsbC, 2);
  p[BNK] = 6, p[SBK] = 8, p[PRG] = 11, p[PB] = 64, p[AT] = 1, p[MW] = 127, p[VL1] = 90, p[CC1] = 75, p[VL2] = 0, p[CC2] = 3;
  rep(0x44, 2, 48, 90, p);
  same("2:B20005 2:B22007 2:C20A 2:E20040 2:D200 2:B2017F 2:B24A5A 2:B20200 2:92305A ");
  // the same words again: only the note; the old note's off first, mono
  rep(0x44, 2, 50, 90, p);
  same("2:823000 2:92325A ");
  // a p-lock at the next trig: value before the note, the program with its bank
  auto q = p;
  q[VL1] = 20, q[PRG] = 12;
  rep(0x44, 2, 50, 90, q);
  same("2:823200 2:B20005 2:B22007 2:C20B 2:B24A14 2:92325A ");
  // no lock on the next: back
  rep(0x44, 2, 50, 90, p);
  same("2:823200 2:B20005 2:B22007 2:C20A 2:B24A5A 2:92325A ");
  // a knob between trigs (0x43): the change only, the note held; at most what the budget gives, the rest in the next ms
  ms(60);
  q = p, q[VL2] = 33;
  rep(0x43, 2, -1, 0, q);
  same("2:B20221 ");
  CHECK(midim::voice_notes(2) == 1);
  // bend: the extremes, 14 bits with the fraction
  q[PB] = 127;
  rep(0x43, 2, -1, 0, q);
  same("2:E27F7F ");
  q[PB] = 1;
  rep(0x43, 2, -1, 0, q);
  same("2:E20000 ");
  q[PB] = 64;
  rep(0x43, 2, -1, 0, q, 64);   // half a step above centre
  CHECK(g.size() == 1 && g[0].b[1] + (g[0].b[2] << 7) > 8192 && g[0].b[1] + (g[0].b[2] << 7) < 8192 + 130);
  rep(0x43, 2, -1, 0, q);
  g.clear();
  // OFF: wheel off sends nothing; on again sends the value
  q[MW] = 0;
  rep(0x43, 2, -1, 0, q);
  same("");
  q[MW] = 2;
  rep(0x43, 2, -1, 0, q);
  same("2:B20101 ");
  // another CC number: the value goes out on it; slot off: nothing
  q[CC1] = 11;
  rep(0x43, 2, -1, 0, q);
  same("2:B20A5A ");
  q[CC1] = 0;
  rep(0x43, 2, -1, 0, q);
  same("");
  // another channel: everything again on it; the held note's off stays on its old channel
  q[CC1] = 11, q[CHN] = 3 * 8;
  rep(0x43, 2, -1, 0, q);
  same("2:B30005 2:B32007 2:C30A 2:E30040 2:D300 2:B30101 2:B30A5A 2:B30221 ");
  off(2);
  same("2:823200 ");
  // another destination, a lock on one trig: the note there, its off there
  q[DST] = kUsbA;
  rep(0x44, 2, 40, 80, q);
  CHECK(dump().find("3:932850") != std::string::npos);
  g.clear();
  q[DST] = kDin;
  rep(0x43, 2, -1, 0, q);
  g.clear();
  off(2);
  same("3:832800 ");
  // DST OFF: nothing goes out, no note held
  q[DST] = 0;
  rep(0x44, 2, 40, 80, q);
  same("");
  CHECK(midim::voice_notes(2) == 0 && midim::dest_of(2) == midim::kOff);

  // the zones: the knob's stepped values land on the right destination and channel
  for (int w = 0; w < 128; ++w) {
    auto r = words(w, 0);
    r[CHN] = w;
    rep(0x43, 3, -1, 0, r);
    CHECK(midim::dest_of(3) == w * 4 / 128);
  }
  g.clear();

  // two voices share a note on one channel: the off only when the last lets go
  p = words(kDin, 5);
  rep(0x44, 4, 62, 100, p);
  rep(0x44, 6, 62, 100, p);
  same("1:953E64 1:953E64 ");
  off(4);
  same("");
  off(6);
  same("1:853E00 ");

  // polyphony: the sequencer's path (a poly track's notes), held separately, ended by note and all at once
  p = words(kUsbA, 0);
  rep(0x43, 5, -1, 0, p);
  g.clear();
  CHECK(v[5].mi && poly::plays_poly(5));
  seq::PolyCfg c = {};
  for (int n : {60, 64, 67, 71}) CHECK(inst::seq_on(5, n, 100, c));
  same("3:903C64 3:904064 3:904364 3:904764 ");
  CHECK(midim::voice_notes(5) == 4 && !v[5].live && !inst::active(5));
  inst::seq_off(5, 64);
  same("3:804000 ");
  inst::seq_off(5, -1);
  CHECK(midim::voice_notes(5) == 0);
  g.clear();
  // the Rytm's note-off leaves sequencer notes alone; STOP ends them
  CHECK(inst::seq_on(5, 72, 100, c));
  g.clear();
  off(5);
  CHECK(midim::voice_notes(5) == 1);
  midim::stop();
  CHECK(midim::voice_notes(5) == 0);
  same("3:804800 ");

  // the keyboards' notes (core/keys.h's way) and a machine on another voice: to the machine's destination, polyphonic
  CHECK(poly::key_on(5, 60, 90, poly::kKeysCh0));
  CHECK(poly::key_on(5, 64, 90, poly::kKeysCh0));
  same("3:903C5A 3:90405A ");
  midim::stop();   // keys are not the sequencers'
  same("");
  poly::key_off(5, 60, poly::kKeysCh0);
  poly::key_off(5, -1, poly::kKeysCh0);
  CHECK(midim::voice_notes(5) == 0);
  g.clear();

  // no hanging notes: STOP
  p = words(kDin, 0);
  rep(0x44, 1, 36, 100, p);
  g.clear();
  midim::stop();
  same("1:802400 ");
  // through the sequencer's transport report too (set_transport): PLAY by the report, then STOP
  transport(seq::kPlay);
  rep(0x44, 1, 36, 100, p);
  g.clear();
  transport(seq::kStop);
  same("1:802400 ");
  rep(0x44, 1, 36, 100, p);
  g.clear();

  // no hanging notes: another machine by 33 (no 0x11 will come: ext_render left the voice)
  machine33(4, 50, 1);     // BT (voice 1) on MIDI
  CHECK(remote_track_machine(4) == (kMidiMachine | 1) && (remote_track_machine(4) & 0x7F) != 0x7F);
  CHECK(remote_track_page(4) == nullptr);
  machine33(4, 41, 0x08 | 1);
  same("1:802400 ");
  CHECK(!v[1].mi && midim::voice_notes(1) == 0 && midim::dest_of(1) == midim::kOff);

  // ... to analog
  machine33(4, 50, 1);
  rep(0x44, 1, 36, 100, p);
  g.clear();
  machine33(4, 3, 0x7F);
  same("1:802400 ");

  // ... by a report of another machine, 0x04 on the same voice
  rep(0x44, 1, 36, 100, p);
  g.clear();
  auto fm = std::vector<int>(16, 0);
  rep(0x04, 0x48 | 1, 60, 100, fm);
  same("1:802400 ");
  CHECK(!v[1].mi && v[1].fm && v[1].live);
  voice_silence(1);
  g.clear();

  // a shared voice: its other track leaving does not end the machine
  machine33(2, 50, 7), machine33(3, 50, 7);
  rep(0x44, 7, 60, 100, p);
  g.clear();
  machine33(3, 3, 0x7F);
  same("");
  CHECK(v[7].mi);
  machine33(2, 3, 0x7F);
  same("1:803C00 ");

  // kit change: notes off, and the next report sends PRG again though the value is the same
  p = words(kDin, 1);
  p[PRG] = 5;
  rep(0x44, 3, 60, 100, p);
  same("1:C104 1:913C64 ");
  rep(0x43, 3, -1, 0, p);
  same("");
  kit(2);          // the new kit's own step: its trig came first, its 3B a UI tick later: it stays
  same("");
  kit(2);          // the same kit (a page open sends one): nothing
  ms(200);
  kit(2);
  same("");
  kit(3);
  same("1:813C00 ");
  rep(0x43, 3, -1, 0, p);
  same("1:C104 ");

  // the link's hello: every voice's notes off, every value again
  rep(0x44, 3, 62, 100, p);
  rep(0x44, 2, 64, 100, words(kDin, 2));
  g.clear();
  hello();
  CHECK(dump().find("1:813E00") != std::string::npos && dump().find("1:824000") != std::string::npos);
  CHECK(midim::stats().held == 0);
  g.clear();
  rep(0x43, 3, -1, 0, p);
  same("1:C104 ");

  // CC 120 path: voice_silence
  rep(0x44, 3, 62, 100, p);
  g.clear();
  voice_silence(3);
  same("1:813E00 ");

  // a report back on the machine after a silence: the note goes out again
  rep(0x44, 3, 62, 100, p);
  same("1:913E64 ");
  off(3);
  g.clear();

  // the table full: the oldest released, never a note that cannot end
  for (int n = 0; n < midim::kMaxHeld + 3; ++n) CHECK(poly::key_on(3, n, 100, poly::kKeysCh0));
  CHECK(midim::stats().held == midim::kMaxHeld && midim::stats().steals == 3);
  g.clear();
  voice_silence(3);
  CHECK((int)g.size() == midim::kMaxHeld && midim::stats().held == 0);
  g.clear();

  // the budget: a burst of 0x43s on DIN does not outrun the wire, the last value still arrives
  ms(200);
  p = words(kDin, 0);
  p[CC1] = 75;
  uint32_t v0 = midim::stats().values;
  for (int i = 0; i < 1000; ++i) {
    p[VL1] = i % 128;
    rep(0x43, 3, -1, 0, p);
    ms(1);
  }
  uint32_t sent = midim::stats().values - v0;   // 1 s at 2400 B/s: 800 three-byte messages
  CHECK(sent <= midim::kDinBytesPerSec / 3 + 60);
  g.clear();
  p[VL1] = 111;
  rep(0x43, 3, -1, 0, p);
  ms(100);
  CHECK(!g.empty() && g.back().b[2] == 111);
  g.clear();
  // notes never wait
  rep(0x44, 3, 70, 100, p);
  same("1:904664 ");
  off(3);
  g.clear();

  // a USB destination with no device: counted, nothing held back, no hang
  g_usb_up = false;
  uint32_t lost0 = midim::stats().lost;
  rep(0x44, 3, 70, 100, words(kUsbA, 0));
  off(3);
  CHECK(midim::stats().lost == lost0 + 2 && midim::stats().held == 0);
  g_usb_up = true;

  // a note-off the queue refused: tried again until it goes; a note played again meanwhile is not cut; a dead port gives up
  ms(10);
  g.clear();
  uint32_t retried0 = midim::stats().retried;
  rep(0x44, 3, 70, 100, words(kUsbA, 0));
  g.clear();
  g_usb_up = false;
  off(3);
  CHECK(midim::stats().held == 0);
  same("");
  ms(5);
  g_usb_up = true;
  ms(2);
  same("3:804600 ");
  CHECK(midim::stats().retried == retried0 + 1);
  rep(0x44, 3, 71, 100, words(kUsbA, 0));
  g_usb_up = false;
  off(3);
  g_usb_up = true;
  rep(0x44, 3, 71, 100, words(kUsbA, 0));
  g.clear();
  ms(5);
  same("");   // (the note sounds again: the stale off is dropped)
  off(3);
  same("3:804700 ");
  rep(0x44, 3, 72, 100, words(kUsbA, 0));
  g_usb_up = false;
  off(3);
  ms((int)midim::kOffRetryMs + 20);
  g_usb_up = true;
  g.clear();
  ms(5);
  same("");

  // the Rytm's USB gone: no hello will come, USB-A's and USB-C's notes end now (the voice stays as it was)
  rep(0x44, 3, 73, 100, words(kUsbA, 0));
  g.clear();
  midim::link_lost();
  same("3:804900 ");
  CHECK(midim::stats().held == 0);

  // a DAW's thru on USB-C: a key's note out, back in on the computer's port, out ... stopped at the first echo
  {
    midim::reset();
    ms(10);
    keys::reset();
    machine33(0, 50, 0);
    rep(0x43, 0, -1, 0, words(kUsbC, 0));
    CHECK(v[0].mi && poly::plays_poly(0));
    keys::source(keys::kComputer, true, "DAW");
    g.clear();
    g_loop = true;
    keys::midi(keys::kComputer, 0x90, 60, 100);
    CHECK(midim::stats().ons == 1);
    for (int i = 0; i < 40 && !g_thru.empty(); ++i) {
      ms(2);
      auto t = g_thru;
      g_thru.clear();
      for (auto& m : t) keys::midi(keys::kComputer, m[0], m[1], m[2]);
    }
    CHECK(midim::stats().ons == 1 && midim::stats().echoes == 1);
    // the same key played later is a new note; so is another port's or another velocity's
    keys::midi(keys::kComputer, 0x80, 60, 0);
    ms((int)midim::kEchoMs + 20);
    g_loop = false;
    g.clear();
    keys::midi(keys::kComputer, 0x90, 60, 100);
    same("2:903C64 ");
    keys::midi(keys::kComputer, 0x80, 60, 0);
    same("2:803C00 ");
    keys::midi(keys::kComputer, 0x90, 60, 101);   // (within kEchoMs of the note above: another velocity, a played note)
    same("2:903C65 ");
    keys::midi(keys::kComputer, 0x80, 60, 0);
    g.clear();
    CHECK(!midim::echo(midim::kUsbA, 0, 60, 101));
  }

  // realtime: to USB-A only while a voice sends there
  rep(0x43, 3, -1, 0, words(kUsbA, 0));
  g.clear();
  midim::realtime(0xF8);
  CHECK(g.size() == 1 && g[0].d == midim::kUsbA && g[0].b[0] == 0xF8);
  g.clear();
  rep(0x43, 3, -1, 0, words(kDin, 0));
  midim::leave(5);
  midim::realtime(0xF8);
  same("");
  // each box's own clock (decided 2026-11-26): USB-A hears a box's realtime bytes only while its voices alone send there
  if (kVoiceSlots > 1) {
    float pa[16] = {0}, pd[16] = {0};
    pa[0] = kUsbA, pd[0] = kDin;
    midim::report(kBoxVoices + 2, -1, 0, pa);   // slot 1's voice 3 on USB-A, slot 0 none
    CHECK(midim::usba_slots() == 2);
    g.clear();
    midim::realtime(0, 0xF8);
    same("");
    midim::realtime(1, 0xF8);
    CHECK(g.size() == 1 && g[0].d == midim::kUsbA && g[0].b[0] == 0xF8);
    rep(0x43, 3, -1, 0, words(kUsbA, 0));   // slot 0's too: both on USB-A, no clock from either
    CHECK(midim::usba_slots() == 3);
    g.clear();
    midim::realtime(0, 0xF8), midim::realtime(1, 0xFA), midim::realtime(1, 0xF8);
    same("");
    midim::report(kBoxVoices + 2, -1, 0, pd);   // slot 1 off USB-A: slot 0's clock again
    g.clear();
    midim::realtime(1, 0xF8);
    same("");
    midim::realtime(0, 0xF8);
    CHECK(g.size() == 1 && g[0].b[0] == 0xF8);
    midim::leave(kBoxVoices + 2);
    rep(0x43, 3, -1, 0, words(kDin, 0));
    midim::leave(3);
    g.clear();
  }

  // the SRC view's 2A sends with the voice idle
  midim::reset();
  ms(10);
  machine33(0, 50, 0);
  {
    std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 0, 16};
    auto w = words(kDin, 0);
    w[PRG] = 3;
    for (int k = 0; k < 16; ++k) m.push_back((uint8_t)w[k]), m.push_back(0);
    m.push_back(0xF7);
    engine_sysex(m.data(), (int)m.size());
    same("1:C002 ");
    const uint8_t one[] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 4, 1, 9, 0, 0xF7};
    engine_sysex(one, sizeof one);
    same("1:C008 ");
  }

  // the pads (CHRO): a channel bound to the voice plays polyphonic notes, a chord of them goes out as played and writes no chord into the
  // sound (P13..P16 are CC numbers: the host's 28 01 would overwrite them)
  {
    midim::reset();
    ms(10);
    rep(0x43, 0, -1, 0, words(kUsbC, 0));
    char b[160];
    CHECK(poly::command("poly=bind:14:0", b, sizeof b));
    g_rytm.clear();
    g.clear();
    poly::midi(0x9D, 60, 100);
    poly::midi(0x9D, 64, 100);
    poly::midi(0x9D, 67, 100);
    ms(150);
    CHECK(midim::voice_notes(0) == 3);
    for (auto& m : g_rytm) CHECK(m[4] != poly::kChordCmd);
    poly::midi(0x8D, 60, 0);
    poly::midi(0x8D, 64, 0);
    poly::midi(0x8D, 67, 0);
    ms(150);
    CHECK(midim::voice_notes(0) == 0);
    g.clear();
  }
  // DIN's note-offs lost on the way (midim.h): the record goes, emit cannot tell; a resync sends them again
  {
    midim::reset();
    ms(10);
    g.clear();
    rep(0x44, 1, 60, 100, words(kDin, 0));
    same("1:903C64 ");
    auto was = hooks.to_rytm;
    hooks.to_rytm = nullptr;   // nothing gets through (a failed send's off-time, a full queue)
    off(1);
    rep(0x44, 1, 62, 100, words(kDin, 0));
    off(1);
    hooks.to_rytm = was;
    same("");
    CHECK(midim::stats().held == 0);
    midim::link_state(false);
    ms(midim::kOffKeepMs * 2);    // a link off for long: nothing forgotten
    midim::link_state(true);
    midim::resync();
    ms(1);
    same("1:803C00 1:803E00 ");   // (the on of 62 was lost too: an off for nothing does nothing)
    CHECK(midim::stats().din_again == 2);
    midim::resync();              // this try may be lost as well: again at the next
    ms(1);
    same("1:803C00 1:803E00 ");
    // a note held now keeps its off for its own end
    rep(0x44, 1, 60, 100, words(kDin, 0));
    same("1:903C64 ");
    midim::resync();
    ms(1);
    same("1:803E00 ");
    // kOffKeepMs of a link that is up: forgotten
    ms(midim::kOffKeepMs + 10);
    midim::resync();
    ms(2);
    same("");
    // under the byte budget: a burst of 50 ms, then a few a ms
    midim::reset();
    ms(10);
    g.clear();
    for (int n = 0; n < 127; ++n) {
      rep(0x44, 1, n, 100, words(kDin, 0));
      off(1);
    }
    g.clear();
    ms(100);
    midim::resync();
    ms(1);
    int first = (int)g.size();
    CHECK(first > 10 && first <= 45);
    ms(200);
    CHECK((int)g.size() == 127);
    g.clear();
    // a USB destination has its own retry
    midim::reset();
    ms(10);
    rep(0x44, 1, 60, 100, words(kUsbA, 0));
    off(1);
    g.clear();
    midim::resync();
    ms(2);
    same("");
  }
  // the arp's and the sequencer's note (a live note): the bank and program before it, past the budget as a trig
  {
    midim::reset();
    g.clear();
    auto p2 = words(kDin, 0);
    p2[PRG] = 8;
    rep(0x43, 1, -1, 0, p2);
    same("");                     // no budget yet: held back
    midim::live_on(1, 60, 100, poly::kSeqCh, 60);
    same("1:C007 1:903C64 ");
    midim::live_off(1, poly::kSeqCh, 60);
    g.clear();
  }
  // the arp's note of a new kit's first step, on the instance path: as the trig, the 3B a UI tick later does not end it
  {
    midim::reset();
    g.clear();
    ms(10);
    rep(0x43, 1, -1, 0, words(kDin, 0));
    midim::live_on(1, 60, 100, poly::kSeqCh, 60);
    same("1:903C64 ");
    midim::kit_switched();
    same("");
    ms(midim::kFreshHeldMs + 1);
    midim::kit_switched();
    same("1:803C00 ");
  }
  // 3(c) step 9: another box's kit (its 3B, its link's reset) ends only its own voices' notes; the Rytm's voice keeps its
  {
    midim::reset();
    g.clear();
    ms(10);
    rep(0x43, 1, -1, 0, words(kDin, 0));
    midim::live_on(1, 60, 100, poly::kSeqCh, 60);
    same("1:903C64 ");
    ms(midim::kFreshHeldMs + 1);
    if (kVoiceSlots > 1) {
      midim::kit_switched(1);
      midim::kit_changed(1);
      same("");
      CHECK(midim::voice_notes(1) == 1);
    }
    midim::kit_switched(0);
    same("1:803C00 ");
  }
  // a Pi restart: a track on MIDI, the machine known from its 33, before any report
  {
    midim::reset();
    memset(&v[1].mi, 0, sizeof v[1].mi);
    machine33(4, 50, 1);
    CHECK(v[1].mi);
    machine33(4, 3, 0x7F);
    CHECK(!v[1].mi);
    // a voice on another machine: the change is its next report's
    v[1].fm = true;
    machine33(4, 50, 1);
    CHECK(!v[1].mi);
    v[1].fm = false;
    // a shared voice with a Sidecar machine on its other track: as well
    machine33(3, 3, 0x08 | 7), machine33(2, 50, 7);
    CHECK(!v[7].mi);
    machine33(3, 3, 0x7F), machine33(2, 3, 0x7F);
    machine33(4, 3, 0x7F);
    g.clear();
  }
  rep(0x43, 0, -1, 0, words(kUsbC, 0));
  {
    char b[200];
    CHECK(midim::command("midim", b, sizeof b) && strstr(b, "USBC/1"));
    CHECK(!midim::command("other", b, sizeof b));
  }
  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("midim: PASS\n");
  return 0;
}
