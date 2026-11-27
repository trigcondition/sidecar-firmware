// core/tests/test_same_type.cpp - two boxes of one type (design decided 2026-11-26, docs/device-profiles.md "Two boxes of one
// type"): a box is its type and its DeviceUID. Two Rytms with different UIDs take a slot each, in either order, each with its
// own folder /.rytm/<uid>/; the first one to attach after the update inherits the old /.rytm layout (moved, power-cut safe);
// a box back takes its own slot; a box whose UID is not known uses /.rytm/last alone, and waits beside one of its type unless
// its transport's USB names its slot.
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "host_link.h"
#include "lfo.h"
#include "pool/pool.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }

// the card's own paths (no box's folder in between)
static void put(const char* path, const char* text) {
  device::RawPath raw;
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  fputs(text, f);
  fclose(f);
}
static std::string get(const char* path) {
  device::RawPath raw;
  FILE* f = fopen(host_path(path).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}
static bool there(const char* path) {
  device::RawPath raw;
  return access(host_path(path).c_str(), F_OK) == 0;
}

static void settle() {
  idle_flush(true);
  for (int i = 0; i < 4000; ++i) {
    device::step(idle_settled());
    idle_prepare();
    idle_save();
    idle_stream();
    bool loading = false;
    for (int k = 0; k < kVoiceSlots; ++k) {
      device::Scope in(device::g_links[k]);
      loading = loading || project_loading();
    }
    if (i > 50 && idle_settled() && !loading && !device::leaving()) break;
  }
  idle_flush(false);
}
static void send(int t, const std::vector<uint8_t>& m) { engine_sysex_link(t, m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static void rytm35(int t) { send(t, sidecar({0x35, 1, 0, 100, 3})); }
static std::string key(int s) { return prj(s).key; }
static int lfo_depth(int s) {
  device::Scope in(device::g_links[s]);
  return lfo::get(0).target ? lfo::get(0).depth : 0;
}
static void set_depth(int s, int d) {
  device::Scope in(device::g_links[s]);
  char cmd[32], o[256];
  snprintf(cmd, sizeof cmd, "lfo=1:dep:%d", d);
  CHECK(lfo::command(cmd, o, sizeof o));
}
static void attach(uint32_t uid) {   // a Rytm's USB enumerates with its UID (CObUsb's vendor request), taken at the next pass
  CHECK(device::usb_attach(&device::rytm(), uid) >= 0);
  device::step(true);
}
static int slot_of(uint32_t uid) { return device::slot_of_box(&device::rytm(), uid); }
// a box's answer to the MIDI RPC DeviceUID: F0 00 20 3C 10 00, 8-to-7 (seq, its answer id, 83, the UID MSB first)
static std::vector<uint8_t> uid_answer(uint32_t uid) {
  const uint8_t m[9] = {0, 0, 1, 2, 0x83, (uint8_t)(uid >> 24), (uint8_t)(uid >> 16), (uint8_t)(uid >> 8), (uint8_t)uid};
  std::vector<uint8_t> r = {0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00};
  for (int i = 0; i < 9; i += 7) {
    uint8_t top = 0;
    for (int k = 0; k < 7 && i + k < 9; ++k) top |= (uint8_t)((m[i + k] >> 7) << (6 - k));
    r.push_back(top);
    for (int k = 0; k < 7 && i + k < 9; ++k) r.push_back(m[i + k] & 0x7f);
  }
  r.push_back(0xF7);
  return r;
}
static bool is_uid_request(const std::vector<uint8_t>& m) {
  return m.size() == 13 && m[0] == 0xF0 && m[1] == 0 && m[2] == 0x20 && m[3] == 0x3C && m[4] == 0x10 && m[11] == 0x03;
}
static int requests(const std::vector<std::vector<uint8_t>>& o) {
  int n = 0;
  for (const auto& m : o) n += is_uid_request(m);
  return n;
}
static void wait_ms(uint32_t ms) { const uint32_t t0 = now_ms(); while (now_ms() - t0 < ms) {} }

constexpr uint32_t kA = 0x1234abcd, kB = 0x9f003fa2;

int main() {
  if (kVoiceSlots < 2) {
    printf("test_same_type: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  {   // the UID's folder name
    char u[9];
    device::uid_text(kB, u);
    uint32_t back = 0;
    CHECK(!strcmp(u, "9f003fa2") && device::uid_parse(u, &back) && back == kB);
    CHECK(!device::uid_parse("9f003fa", &back) && !device::uid_parse("9f003fa2x", &back) && !device::uid_parse("lastxxxx", &back));
  }
  char tmpl[] = "/tmp/core-sametype-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.rytm/lfo") && fs.mkdir("/.a4"));
  // a card from before this build: the one Rytm's state in /.rytm itself
  put("/.rytm/project", "OLD");
  put("/.rytm/lfo/OLD.txt", "# the Rytm's\nT1A V2 3 0 100 0 0 20 64\n");
  put("/.rytm/keys.txt", "# keys\n");
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  settle();
  // boot: no UID known yet, no /.rytm/last: the presumed Rytm in /.rytm as before, nothing moved
  CHECK(device::g_links[0].presumed && !device::g_links[0].dir[0] && !device::g_links[0].state && key(0) == "OLD");
  CHECK(there("/.rytm/project") && !there("/.rytm/last"));

  // ---- the first Rytm's USB, UID A: it is the presumed Rytm (slot 0); the old layout moves into /.rytm/1234abcd, `last` A
  attach(kA);
  CHECK(slot_of(kA) == 0 && !device::g_links[0].presumed && device::g_links[0].uid == kA);
  CHECK(!strcmp(device::g_links[0].dir, "/.rytm/1234abcd") && device::g_links[0].state == device::g_links[0].dir);
  CHECK(get("/.rytm/1234abcd/project") == "OLD" && there("/.rytm/1234abcd/lfo/OLD.txt") && there("/.rytm/1234abcd/keys.txt"));
  CHECK(!there("/.rytm/project") && !there("/.rytm/lfo") && !there("/.rytm/1234abcd/.moving"));
  CHECK(get("/.rytm/last") == "1234abcd");
  CHECK(get("/.rytm/1234abcd/name.txt") == "Rytm abcd" && !strcmp(device::g_links[0].label, "Rytm abcd"));
  settle();
  CHECK(key(0) == "OLD" && lfo_depth(0) == 20);   // read again from its UID folder: the same project
  printf("  first Rytm (UID A): slot 0, the old /.rytm moved into %s, named \"%s\"\n", device::g_links[0].dir,
         device::g_links[0].label);

  // ---- a second Rytm, UID B: slot 1, its own folder from the defaults; both are Rytms, each its own machinery
  attach(kB);
  CHECK(slot_of(kB) == 1 && slot_of(kA) == 0 && device::boxes_of(&device::rytm()) == 2);
  CHECK(device::rytm_at(0) && device::rytm_at(1) && device::side_slot() == 1);
  CHECK(!strcmp(device::g_links[1].dir, "/.rytm/9f003fa2") && get("/.rytm/9f003fa2/name.txt") == "Rytm 3fa2");
  CHECK(get("/.rytm/last") == "9f003fa2");
  settle();
  CHECK(key(1) == "" && lfo_depth(1) == 0 && key(0) == "OLD" && lfo_depth(0) == 20);
  {   // rytm_home is the Rytm in hand
    device::Scope in(device::g_links[1]);
    CHECK(device::rytm_home() == 1);
  }
  {
    device::Scope in(device::g_links[0]);
    CHECK(device::rytm_home() == 0);
  }
  // each Rytm's MIDI: the Pi's USB says which transport is part of which box; a 35 on either goes to its own slot
  device::set_midi_slot(0, 0), device::set_midi_slot(1, 1);
  device::step(true);
  rytm35(1);
  CHECK(device::route_of(1) == 1 && device::route_of(0) == 0);
  {
    device::RawPath raw;
    CHECK(fs.mkdir("/.rytm/9f003fa2/lfo"));
  }
  put("/.rytm/9f003fa2/lfo/B1.txt", "# the second Rytm's\nT1A V3 2 0 100 0 0 11 64\n");
  send(1, sidecar({0x10, 'B', '1'}));   // the second Rytm's project name: its slot's only
  CHECK(key(1) == "B1" && key(0) == "OLD");
  settle();
  CHECK(lfo_depth(1) == 11);
  set_depth(1, 44);
  settle();
  CHECK(lfo_depth(1) == 44 && lfo_depth(0) == 20);
  CHECK(get("/.rytm/9f003fa2/project") == "B1" && get("/.rytm/9f003fa2/lfo/B1.txt").find(" 44 ") != std::string::npos);
  CHECK(get("/.rytm/1234abcd/lfo/OLD.txt").find(" 44 ") == std::string::npos);
  printf("  second Rytm (UID B): slot 1 in %s; its project and edits in its own folder\n", device::g_links[1].dir);

  // ---- the console's selector: the second of a type, or its UID
  {
    const char* r = nullptr;
    CHECK(device::box_prefix("box=rytm:lfo", &r) == 0 && device::box_prefix("box=rytm2:lfo", &r) == 1);
    CHECK(device::box_prefix("box=3fa2:lfo", &r) == 1 && device::box_prefix("box=1234abcd:lfo", &r) == 0 && !strcmp(r, "lfo"));
    CHECK(device::box_prefix("box=rytm3:lfo", &r) == -2);
  }
  // ---- its name, set by the user
  CHECK(device::set_label(1, "Live rig  ") && !strcmp(device::g_links[1].label, "Live rig") &&
        get("/.rytm/9f003fa2/name.txt") == "Live rig");
  CHECK(!device::set_label(1, "   "));

  // ---- a box whose UID is not known, beside two Rytms: it waits; on a transport its USB names, that slot
  CHECK(device::claim(&device::rytm(), 0) == -1);
  CHECK(device::claim(&device::rytm(), 0, 1) == 1);
  CHECK(device::usb_attach(&device::rytm(), 0) == -1);   // (the driver claims no stream for it)
  // a third Rytm with its own UID: no slot
  CHECK(device::usb_attach(&device::rytm(), 0x55555555) == -1);

  // ---- a replug: B drops off the bus and comes back while its slot is still saving: its own slot, its own folder
  set_depth(1, 45);
  device::usb_detach(1);
  device::step(false);
  CHECK(!device::held(1) && device::g_links[1].parting && device::g_links[1].was_uid == kB);
  CHECK(device::usb_attach(&device::rytm(), kA) == 0);   // (A, still there: its own slot)
  CHECK(device::usb_attach(&device::rytm(), kB) == 1);
  device::step(true);
  CHECK(slot_of(kB) == 1 && slot_of(kA) == 0);
  settle();
  CHECK(lfo_depth(1) == 45 && key(1) == "B1");
  printf("  replug: B back in slot 1 with its edit; a UID-less Rytm waits beside two, or takes its transport's slot\n");

  // ---- both leave; B attaches first now: slot 0 is B's, in B's folder; A after it slot 1 in A's
  device::usb_detach(0), device::usb_detach(1);
  device::set_midi_slot(0, -1), device::set_midi_slot(1, -1);
  settle();
  CHECK(!device::held(0) && !device::held(1));
  attach(kB);
  attach(kA);
  CHECK(slot_of(kB) == 0 && slot_of(kA) == 1);
  CHECK(!strcmp(device::g_links[0].dir, "/.rytm/9f003fa2") && !strcmp(device::g_links[1].dir, "/.rytm/1234abcd"));
  settle();
  CHECK(key(0) == "B1" && lfo_depth(0) == 45 && key(1) == "OLD" && lfo_depth(1) == 20);
  CHECK(!strcmp(device::g_links[0].label, "Live rig"));
  printf("  B then A: slot 0 %s (%s), slot 1 %s (%s)\n", key(0).c_str(), device::g_links[0].label, key(1).c_str(),
         device::g_links[1].label);

  // ---- one Rytm alone, UID not known (a hello's, a box past its USB's timeout): the folder /.rytm/last names
  device::usb_detach(0), device::usb_detach(1);
  settle();
  CHECK(get("/.rytm/last") == "1234abcd");
  rytm35(0);
  CHECK(device::rytm_slot() == 0 && device::g_links[0].uid == 0 && !strcmp(device::g_links[0].dir, "/.rytm/1234abcd"));
  settle();
  CHECK(key(0) == "OLD");
  // its UID then (the presumed Rytm's case too): the same folder, nothing read again
  attach(kA);
  CHECK(device::g_links[0].uid == kA && !strcmp(device::g_links[0].dir, "/.rytm/1234abcd"));
  // and the A4 beside it: its own type, its own folder by its UID; no A4 layout to move
  CHECK(device::usb_attach(&device::kA4, 0x0a4a4a4a) == 1);
  device::step(true);
  CHECK(!strcmp(device::g_links[1].dir, "/.a4/0a4a4a4a") && get("/.a4/last") == "0a4a4a4a" &&
        get("/.a4/0a4a4a4a/name.txt") == "A4 4a4a");
  printf("  a Rytm with no UID uses /.rytm/last; an A4 beside it in /.a4/<uid>\n");

  // ---- the MIDI RPC: its framing, and the fallback when a box's USB gave no UID
  {
    uint8_t m[16];
    const int n = device::uid_request(0x0102, m);
    const uint8_t want[] = {0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x03, 0xF7};
    CHECK(n == (int)sizeof want && !memcmp(m, want, sizeof want));
    uint32_t u = 0;
    const std::vector<uint8_t> a = uid_answer(kB);
    CHECK(device::uid_answer(a.data(), (int)a.size(), &u) && u == kB);
    const std::vector<uint8_t> a2 = uid_answer(0x80ff7f01);   // top bits in the encoding
    CHECK(device::uid_answer(a2.data(), (int)a2.size(), &u) && u == 0x80ff7f01);
    CHECK(!device::uid_answer(m, n, &u));   // (a request is no answer)
  }
  // A on USB-C with its UID; another Rytm says 35 on transport 1, no USB naming its slot: it waits, its UID asked there
  device::usb_detach(0), device::usb_detach(1);
  settle();
  attach(kA);
  CHECK(slot_of(kA) == 0 && device::route_of(0) == 0);
  g_out1.clear();
  rytm35(1);
  CHECK(device::route_of(1) < 0 && device::asking(1) && slot_of(kB) < 0);
  device::step(true);
  CHECK(requests(g_out1) == 1 && requests(g_out) == 0);   // asked on its own transport only
  wait_ms(device::kAskMs + 5);
  device::step(true);
  CHECK(requests(g_out1) == 2);
  send(1, uid_answer(kB));   // its answer: its own slot, its own folder, transport 1 its route
  CHECK(!device::asking(1) && slot_of(kB) == 1 && device::route_of(1) == 1 && !strcmp(device::g_links[1].dir, "/.rytm/9f003fa2"));
  // a frozen box is never asked
  device::usb_detach(1);
  settle();
  device::g_frozen = [](int s) { return s == 1; };
  device::set_midi_slot(1, 1);
  CHECK(device::usb_attach(&device::rytm(), 0) == -1);   // (beside A, no UID: no stream for it)
  g_out1.clear();
  rytm35(1);
  CHECK(device::asking(1));
  for (int i = 0; i < 3; ++i) wait_ms(device::kAskMs + 5), device::step(true);
  CHECK(requests(g_out1) == 0);   // (the slot its USB names is frozen: nothing sent)
  device::g_frozen = nullptr;
  wait_ms(device::kAskForMs);
  device::step(true);
  CHECK(!device::asking(1) && !device::held(1));   // no answer in 2 s: it waits
  device::set_midi_slot(1, -1);
  // alone, the USB gave no UID: it takes `last`'s folder and its UID is asked; the answer is the same box: nothing moves
  device::usb_detach(0);
  settle();
  g_out.clear();
  CHECK(device::usb_attach(&device::rytm(), 0) == 0);
  device::step(true);
  CHECK(device::g_links[0].uid == 0 && !strcmp(device::g_links[0].dir, "/.rytm/9f003fa2") && device::asking(0));
  device::step(true);
  CHECK(requests(g_out) == 1);
  send(0, uid_answer(kA));   // another Rytm than `last` said: its own folder
  CHECK(device::g_links[0].uid == kA && !strcmp(device::g_links[0].dir, "/.rytm/1234abcd") && !device::asking(0));
  printf("  MIDI RPC fallback: asked on the box's own transport, never a frozen one; an answer claims or names its slot\n");

  // ---- a power cut in the middle of the move: the boot after it finishes it, nothing lost
  {
    char t2[] = "/tmp/core-sametype-cut-XXXXXX";
    CHECK(mkdtemp(t2));
    host_set_card(t2);
    device::reset();
    {
      device::RawPath raw;
      CHECK(fs.mkdir("/.rytm") && fs.mkdir("/.rytm/lfo") && fs.mkdir("/.rytm/kits"));
    }
    put("/.rytm/project", "CUT");
    put("/.rytm/lfo/CUT.txt", "x\n");
    put("/.rytm/kits/k.txt", "k\n");
    put("/.rytm/keys.txt", "k\n");
    const uint64_t c0 = host_changes();
    host_power_cut(5);   // the UID folder, its marker (open, write), two renames: then the power goes
    CHECK(device::usb_attach(&device::rytm(), kA) == 0);
    device::step(true);
    host_power_cut(-1);
    (void)c0;
    CHECK(there("/.rytm/1234abcd/.moving"));
    int left = 0;
    for (const char* p : {"/.rytm/project", "/.rytm/lfo", "/.rytm/kits", "/.rytm/keys.txt"}) left += there(p);
    CHECK(left == 2);
    printf("  power cut mid-move: marker left, %d of 4 entries still in /.rytm\n", left);
    device::reset();   // the reboot
    device::boot();
    CHECK(!there("/.rytm/1234abcd/.moving"));
    for (const char* p : {"/.rytm/project", "/.rytm/lfo", "/.rytm/kits", "/.rytm/keys.txt"}) CHECK(!there(p));
    CHECK(get("/.rytm/1234abcd/project") == "CUT" && there("/.rytm/1234abcd/lfo/CUT.txt") && there("/.rytm/1234abcd/kits/k.txt"));
    // `last` was not written before the cut: the presumed Rytm stays in /.rytm until its UID comes (then its folder)
    CHECK(!device::g_links[0].dir[0]);
    attach(kA);
    CHECK(!strcmp(device::g_links[0].dir, "/.rytm/1234abcd") && get("/.rytm/last") == "1234abcd");
    // the next boot: the presumed Rytm in /.rytm/last's folder from the first file on
    device::reset();
    device::boot();
    CHECK(device::g_links[0].presumed && !strcmp(device::g_links[0].dir, "/.rytm/1234abcd"));
    printf("  the boot after it finished the move; the next boot presumes /.rytm/last's folder\n");
  }

  printf("test_same_type: PASS (two Rytms by UID in either order, each its own folder; the old layout moved into the first, "
         "power-cut safe; replug, UID-less waits, names, selector)\n");
  return 0;
}
