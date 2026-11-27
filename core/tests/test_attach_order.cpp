// core/tests/test_attach_order.cpp - slots by attach order, folders by box type (design decided 2026-11-21,
// docs/device-profiles.md "Several devices"): the first box to attach is slot 0, the second slot 1, any two different box
// types in either order (an A4 and a Digitakt with no Rytm at all; the Rytm in slot 1). A box that leaves frees its slot, its
// voices stop and its edits land in its own folder; the other box stays where it is; a box that comes back loads from its
// folder. The Rytm's own machinery follows its slot, not slot 0.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "bundle/rename.h"
#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "device/digitakt.h"
#include "engine.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "lfo.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "voices.h"
#include "wavetable/wavetable.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }

static void put(const char* path, const char* text) {
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  fputs(text, f);
  fclose(f);
}
static std::string get(const char* path) {
  FILE* f = fopen(host_path(path).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}

// the idle passes until nothing waits: a box's project read, the saves (idle_flush: no second's wait for an edit to settle)
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
static int hello(int t, uint16_t id) {   // its answer's status
  std::vector<std::vector<uint8_t>>& o = t ? g_out1 : g_out;
  o.clear();
  send(t, sidecar({device::kCmdHello, 1, (uint8_t)(id >> 7), (uint8_t)(id & 0x7f), 1}));
  return o.empty() ? -1 : o.back()[8];
}
static void rytm35(int t) { send(t, sidecar({0x35, 1, 0, 100, 3})); }
// a 0x04 trig on the box's voice k: a PLAITS sound, P1..P16 (the Rytm's and the A4's own report)
static std::vector<uint8_t> trig(int k, int note) {
  static const uint8_t pl[16] = {93, 64, 0, 0, 127, 0, 64, 127, 64, 64, 0, 64, 64, 0, 0, 100};
  std::vector<uint8_t> t = {0x04, (uint8_t)(kPlaits | k), (uint8_t)note, 100};
  for (int i = 0; i < 16; ++i) t.push_back(pl[i]), t.push_back(0);
  return sidecar(t);
}
static std::string key(int s) { return prj(s).key; }
static constexpr int kRows = kBlock * 100;
static int16_t ring[kRows][kVoices];
static double rms(int vi) {   // voice vi over kRows frames rendered now
  uint64_t mach[kMachines] = {0};
  memset(ring, 0, sizeof ring);
  for (int b = 0; b < kRows / kBlock; ++b) render_voices(ring, kRows, b * kBlock, 0, kVoices, mach);
  double s = 0;
  for (int i = kRows / 2; i < kRows; ++i) s += (double)ring[i][vi] * ring[i][vi];
  return std::sqrt(s / (kRows / 2));
}
static std::string sfx_line(char ab, const char* type) {   // "\nSFX B <type id> " (its file's line for that FX)
  char b[40];
  snprintf(b, sizeof b, "\nSFX %c %d ", ab, fx::type_from_name(type));
  return b;
}
static int lfo_depth(int s) {   // slot s's T1A's depth, 0 when it has no target (a project without an LFO file)
  device::Scope in(device::g_links[s]);
  return lfo::get(0).target ? lfo::get(0).depth : 0;
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_attach_order: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-attach-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4") && fs.mkdir("/.dt"));
  CHECK(fs.mkdir("/.a4/lfo") && fs.mkdir("/.dt/lfo"));
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  put("/.dt/project", "DTP");
  put("/.a4/lfo/A4P.txt", "# the A4's\nT1A V2 3 0 100 0 0 20 64\n");
  put("/.dt/lfo/DTP.txt", "# the Digitakt's\nT1A V3 2 0 100 0 0 11 64\n");
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  settle();
  // boot: slot 0 presumed the Rytm's (its project read), nothing on transport 1
  CHECK(device::rytm_slot() == 0 && device::g_links[0].presumed && key(0) == "RYP");
  CHECK(device::route_of(0) == 0 && device::route_of(1) < 0 && !device::held(1));
  CHECK(device::rytm_route(0) == 0 && device::rytm_route(1) < 0);   // its raw MIDI taps: transport 0's

  // ---- an A4 and a Digitakt, no Rytm: the A4 says hello first (on USB-C), so it is slot 0 in /.a4; the Digitakt slot 1
  CHECK(hello(0, device::kIdA4) == device::kOk);
  CHECK(device::slot_held_by(&device::kA4) == 0 && device::rytm_slot() < 0 && !device::rytm_at(0));
  CHECK(!strcmp(device::g_links[0].state, "/.a4") && device::route_of(0) == 0);
  settle();
  CHECK(key(0) == "A4P" && lfo_depth(0) == 20);   // its project and LFOs from its own folder, in slot 0
  CHECK(hello(1, device::kIdDigitakt) == device::kOk);
  CHECK(device::slot_held_by(&device::kDigitakt) == 1 && device::route_of(1) == 1 && device::side_slot() == 1);
  settle();
  CHECK(key(1) == "DTP" && lfo_depth(1) == 11 && key(0) == "A4P" && lfo_depth(0) == 20);
  CHECK(device::rytm_route(0) < 0 && device::rytm_route(1) < 0);   // no Rytm: its taps read neither transport
  CHECK(device::g_base == &device::g_links[0] && device::voice_base(device::g_links[1]) == kBoxVoices);
  printf("  A4 then Digitakt: slot 0 %s (%s), slot 1 %s (%s)\n", key(0).c_str(), device::g_links[0].state, key(1).c_str(),
         device::g_links[1].state);

  // ---- the console's box selector (design point 8): box=S: by slot or by the box's type, whatever its attach order
  {
    const char* r = nullptr;
    CHECK(device::box_prefix("wt=list", &r) == -1 && !strcmp(r, "wt=list"));   // no selector: the base box's, as ever
    CHECK(device::box_prefix("box=a4:wt=voice:1", &r) == 0 && !strcmp(r, "wt=voice:1"));
    CHECK(device::box_prefix("box=dt:wt=voice:1", &r) == 1 && !strcmp(r, "wt=voice:1"));
    CHECK(device::box_prefix("box=1:lfo", &r) == 1 && !strcmp(r, "lfo"));
    CHECK(device::box_prefix("box=rytm:lfo", &r) == -2);   // no Rytm attached
    CHECK(device::box_prefix("box=7:lfo", &r) == -2 && device::box_prefix("box=a4:", &r) == -2 && device::box_prefix("box=", &r) == -2);
    CHECK(!strcmp(device::type_word(&device::kDigitakt), "dt") && !strcmp(device::type_word(&device::rytm()), "rytm"));
    // a voice form takes the box's voice V in the slot in hand: the Digitakt's voice 2 is engine voice 10, the A4's voice 2 is 2
    char o[200];
    {
      device::Scope in(device::g_links[device::box_prefix("box=dt:x", &r)]);
      CHECK(wt::command("wt=voice:2", o, sizeof o) && wt::forced(kBoxVoices + 2) && !wt::forced(2));
      CHECK(wt::command("wt=voice:2:off", o, sizeof o) && !wt::forced(kBoxVoices + 2));
    }
    CHECK(wt::command("wt=voice:2", o, sizeof o) && wt::forced(2) && !wt::forced(kBoxVoices + 2));   // the base box (the A4)
    CHECK(wt::command("wt=voice:2:off", o, sizeof o) && !wt::forced(2));
  }
  // ---- a box's project name is only its name: no Rytm project-slot step and no rename question on the Rytm's PROJECT page
  //      (the A4's project has files, a new name has none: on the Rytm that asks RENAMED FROM?, on the A4 it just switches)
  {
    device::Scope in(device::g_links[0]);
    const uint8_t nm[] = {'A', '4', 'N', 0xf7};
    set_project(nm, (int)sizeof nm);
    CHECK(key(0) == "A4N" && rename::state() == rename::kIdle);
    const uint8_t back[] = {'A', '4', 'P', 0xf7};
    set_project(back, (int)sizeof back);
    CHECK(key(0) == "A4P" && rename::state() == rename::kIdle);
  }
  settle();
  CHECK(key(0) == "A4P" && lfo_depth(0) == 20 && key(1) == "DTP");
  printf("  box=a4: / box=dt: select the box; voice forms take its own voices; its project names skip the Rytm's steps\n");
  // a third box finds no slot: it is told to wait (a Rytm's USB as well: nothing taken)
  CHECK(device::claim(&device::rytm()) == -1 && device::rytm_slot() < 0);
  CHECK(device::usb_attach(&device::rytm()) == -1);
  // the A4's voice 1 is engine voice 1 (its slot 0)
  voices_init();
  send(0, trig(1, 60));
  CHECK(v[1].live && !v[kBoxVoices + 1].live && rms(1) > 100);
  // the same box's hello again: its slot, kOk
  CHECK(hello(0, device::kIdA4) == device::kOk && device::slot_held_by(&device::kA4) == 0);
  // the same type twice: its hello again is its slot (two of a type by their UIDs: test_same_type.cpp)

  // ---- an A4 in slot 0 (it attached first) keeps its send FX in its own INSERT file, as a box in slot 1 does (before: slot 0's
  //      FX were taken for the Rytm's kit's, so an A4 there never wrote them and a replug or reboot brought back OFF)
  {
    device::Scope in(device::g_links[0]);
    char o[256];
    CHECK(fx::command("sendfx=2:halo", o, sizeof o) && fx::control.knob[1][0] == fx::type_from_name("halo"));
  }
  settle();
  printf("  slot 0's A4 FX B set: its file %s\n", get("/.a4/ins/A4P.txt").find(sfx_line('B', "halo")) != std::string::npos ? "has SFX B HALO" : "has no SFX B");
  CHECK(get("/.a4/ins/A4P.txt").find(sfx_line('B', "halo")) != std::string::npos);
  CHECK(fx::control1.knob[1][0] == fx::kOff);   // (the Digitakt's in slot 1: untouched)

  // ---- the A4 leaves with an edit unsaved: its voices stop, its edit lands in /.a4, the Digitakt stays in slot 1
  {
    device::Scope in(device::g_links[0]);
    char o[256];
    CHECK(lfo::command("lfo=1:dep:33", o, sizeof o) && lfo::get(0).depth == 33);
    CHECK(fx::command("sendfx=1:cloudseed", o, sizeof o));   // (its FX A too: saved as it leaves)
  }
  device::usb_detach(0);   // (its USB device gone: the driver's call, taken at the control core's next pass)
  device::step(false);
  CHECK(!device::held(0) && device::g_links[0].parting && device::leaving() && rms(1) == 0);
  CHECK(device::slot_held_by(&device::kDigitakt) == 1 && device::route_of(1) == 1 && device::route_of(0) < 0);
  CHECK(device::g_base == &device::g_links[1] && device::side_slot() == -1);   // the Digitakt alone: the base box
  // while its edits are not saved, slot 0 is the A4's still: a Rytm's 35 now has no slot (it says it again)
  rytm35(0);
  CHECK(device::rytm_slot() < 0 && device::route_of(0) < 0);
  settle();
  CHECK(!device::g_links[0].parting && !device::leaving());
  CHECK(get("/.a4/lfo/A4P.txt").find(" 33 ") != std::string::npos);
  CHECK(get("/.dt/lfo/DTP.txt").find(" 33 ") == std::string::npos && get("/.rytm/lfo/A4P.txt").empty());
  printf("  the A4 left: its edit saved in /.a4, the Digitakt still slot 1\n");

  // ---- a Rytm on USB-C now: the lowest free slot, 0, in /.rytm, its project read again; the Digitakt does not move
  rytm35(0);
  CHECK(device::rytm_slot() == 0 && device::rytm_at(0) && device::route_of(0) == 0 && !device::g_links[0].state);
  CHECK(device::slot_held_by(&device::kDigitakt) == 1 && device::route_of(1) == 1 && device::g_base == &device::g_links[0]);
  settle();
  CHECK(key(0) == "RYP" && lfo_depth(0) == 0 && key(1) == "DTP" && lfo_depth(1) == 11);
  CHECK(get("/.a4/ins/A4P.txt").find(sfx_line('A', "cloudseed")) != std::string::npos);   // the A4's, saved as it left
  CHECK(get("/.rytm/ins/RYP.txt").find("SFX") == std::string::npos);   // the Rytm's FX are its kit's: never SFX lines

  // ---- the Rytm leaves, the A4 comes back on USB-C: slot 0 again, its project and its saved edit from /.a4
  device::rytm_on_usb(false);
  settle();
  CHECK(!device::held(0) && device::slot_held_by(&device::kDigitakt) == 1);
  CHECK(hello(0, device::kIdA4) == device::kOk && device::slot_held_by(&device::kA4) == 0);
  settle();
  CHECK(key(0) == "A4P" && lfo_depth(0) == 33);
  printf("  the A4 back: slot 0 %s, its LFO depth %d (saved before it left), its FX A %s B %s\n", key(0).c_str(), lfo_depth(0),
         fx::type_name(fx::control.knob[0][0]), fx::type_name(fx::control.knob[1][0]));
  CHECK(fx::control.knob[0][0] == fx::type_from_name("cloudseed") && fx::control.knob[1][0] == fx::type_from_name("halo"));

  // ---- the other order: both leave, the Digitakt attaches first (slot 0), then the A4 (slot 1)
  device::set_out(1, nullptr);   // the relay's box gone (the Digitakt)
  device::usb_detach(0);         // the A4's USB gone
  settle();
  CHECK(!device::held(0) && !device::held(1));
  device::set_out(1, out1);
  CHECK(hello(1, device::kIdDigitakt) == device::kOk && device::slot_held_by(&device::kDigitakt) == 0);
  CHECK(device::route_of(1) == 0 && device::route_of(0) < 0);
  CHECK(hello(0, device::kIdA4) == device::kOk && device::slot_held_by(&device::kA4) == 1 && device::route_of(0) == 1);
  settle();
  CHECK(key(0) == "DTP" && lfo_depth(0) == 11 && key(1) == "A4P" && lfo_depth(1) == 33);
  CHECK(!strcmp(device::g_links[0].state, "/.dt") && !strcmp(device::g_links[1].state, "/.a4"));
  voices_init();
  send(0, trig(1, 60));   // the A4 on USB-C, in slot 1: engine voice 8 + 1
  CHECK(v[kBoxVoices + 1].live && !v[1].live);
  printf("  Digitakt then A4: slot 0 %s, slot 1 %s\n", key(0).c_str(), key(1).c_str());

  // ---- the Rytm in slot 1: the A4 leaves, a Rytm takes its place on USB-C; its machinery follows its slot
  device::usb_detach(1);
  settle();
  rytm35(0);
  CHECK(device::rytm_slot() == 1 && device::rytm_at(1) && !device::rytm_at(0) && device::rytm_home() == 1);
  CHECK(device::slot_held_by(&device::kDigitakt) == 0 && device::route_of(0) == 1 && device::route_of(1) == 0);
  CHECK(device::rytm_route(0) == 1 && device::rytm_route(1) < 0);   // its taps (pattern dumps, FILL): its transport, its slot
  settle();
  CHECK(key(1) == "RYP" && key(0) == "DTP" && !device::g_links[1].state);
  // its report 50 (the stems' pans, who is off ROUTE TO MAIN) lands in its slot's master control, for its SRC MIX
  master::link_lost();
  send(0, sidecar({0x50, 50, 64, 64, 64, 64, 64, 64, 64, 0, 1 << 3}));
  CHECK(master::control_of(1).known && !master::control_of(0).known && &master::rytm() == &master::control_of(1));
  CHECK(master::control_of(1).pan[0] == 50 && master::stems_on_main() == 7);
  voices_init();
  send(0, trig(3, 60));   // its voice 3: engine voice 8 + 3, its slot's
  CHECK(v[kBoxVoices + 3].live && !v[3].live);
  {   // the Rytm's own machinery is its slot's: the Digitakt's slot is no Rytm's
    device::Scope in(device::g_links[0]);
    CHECK(!device::rytm_at(device::cur().index));
  }
  // a project name (0x10) from it moves its slot's project only
  send(0, sidecar({0x10, 'R', '2'}));
  CHECK(key(1) == "R2" && key(0) == "DTP");
  printf("  the Rytm in slot 1: slot 0 %s, slot 1 %s; its voices 8..15\n", key(0).c_str(), key(1).c_str());

  // ---- a box on a transport replaced by another box on the same transport: the one before left (its slot saving)
  device::set_out(1, out1);
  CHECK(hello(1, device::kIdA4) == device::kWait || device::slot_held_by(&device::kA4) >= 0);   // (no free slot while the Digitakt's saves land)
  settle();
  CHECK(device::slot_held_by(&device::kDigitakt) < 0);
  CHECK(hello(1, device::kIdA4) == device::kOk && device::slot_held_by(&device::kA4) == 0 && device::rytm_slot() == 1);
  settle();
  CHECK(key(0) == "A4P" && key(1) == "R2");
  CHECK(device::rytm_route(0) == 1 && device::rytm_route(1) < 0);   // the A4 on transport 1: not the Rytm's

  // ---- the Rytm in slot 1 sends its kit's send FX (0x20): they are its slot's, never the A4's in slot 0 (before: every 0x20
  //      wrote slot 0's settings and tags, and the 0x22 answers read them, whichever slot the Rytm held)
  {
    device::Scope in(device::g_links[0]);
    char o[256];
    CHECK(fx::command("sendfx=1:cloudseed", o, sizeof o) && fx::command("sendfx=2:crush", o, sizeof o));
    CHECK(fx::command("sendlvl=1:3:90", o, sizeof o));   // (a send of the A4's own)
  }
  settle();
  uint8_t a4_knob[fx::kSlots][fx::kKnobs], a4_send[fx::kTracks][fx::kSlots];
  for (int s = 0; s < fx::kSlots; ++s)
    for (int k = 0; k < fx::kKnobs; ++k) a4_knob[s][k] = fx::control.knob[s][k];
  for (int t = 0; t < fx::kTracks; ++t)
    for (int s = 0; s < fx::kSlots; ++s) a4_send[t][s] = fx::control.send[t][s];
  const uint16_t a4_tag0 = fx::kit_tag[0], a4_tag1 = fx::kit_tag[1];
  g_out.clear();
  send(0, sidecar({0x20, 1, 12, fx::kHalo, 70, fx::kChorus, 40, 1, 2, 3, 4, 5, 6, 7, 8}));   // track 12: FX 1 CHORUS, FX 2 HALO
  send(0, sidecar({0x20, 1, 3, 111, 22}));                                                    // track 4's sends
  send(0, sidecar({0x20, 1, 13, 0x12, 0x34, 0x05, 0x06}));                                    // the kit's tags
  printf("  the Rytm in slot 1 sends FX %s / %s: slot 1 has %s / %s, the A4's slot 0 keeps %s / %s\n", fx::type_name(fx::kChorus),
         fx::type_name(fx::kHalo), fx::type_name(fx::control1.knob[0][0]), fx::type_name(fx::control1.knob[1][0]),
         fx::type_name(fx::control.knob[0][0]), fx::type_name(fx::control.knob[1][0]));
  CHECK(fx::control1.knob[0][0] == fx::kChorus && fx::control1.knob[1][0] == fx::kHalo);
  CHECK(fx::control1.send[3][0] == 111 && fx::control1.send[3][1] == 22);
  CHECK(fx::kit_tag1[0] == (0x12 << 7 | 0x34) && fx::kit_tag1[1] == (0x05 << 7 | 0x06));
  CHECK(fx::control.knob[0][0] == fx::type_from_name("cloudseed") && fx::control.knob[1][0] == fx::type_from_name("crush"));
  for (int s = 0; s < fx::kSlots; ++s)
    for (int k = 0; k < fx::kKnobs; ++k) CHECK(fx::control.knob[s][k] == a4_knob[s][k]);
  for (int t = 0; t < fx::kTracks; ++t)
    for (int s = 0; s < fx::kSlots; ++s) CHECK(fx::control.send[t][s] == a4_send[t][s]);
  CHECK(fx::kit_tag[0] == a4_tag0 && fx::kit_tag[1] == a4_tag1);
  {   // its 0x22 answers (to the Rytm) are its own slot's FX: the last for each FX (its first 0x20 sent the states before)
    int last[2] = {-1, -1};
    for (const auto& m : g_out)
      if (m.size() > 8 && m[4] == 0x22 && m[5] == 1 && m[6] < 2) last[m[6]] = m[7];
    CHECK(last[0] == fx::kChorus && last[1] == fx::kHalo);
  }
  settle();
  CHECK(get("/.a4/ins/A4P.txt").find(sfx_line('A', "cloudseed")) != std::string::npos);   // the A4's file: its own FX still
  CHECK(get("/.a4/ins/A4P.txt").find(sfx_line('B', "crush")) != std::string::npos);

  // ---- slot 0 and slot 1 the same (decided 2026-11-26): the Rytm in slot 1 beside the A4 in slot 0 gets what it got in slot 0
  //      - its project's user-FX map (fxslots) in its own slot's names, never the A4's; its 0x21 list names them, to it alone
  //      - the 2 s BASE / WIDTH refresh (sfx-basewidth) to its own link (before: the idle step ran outside any Scope, so it
  //        went to the base box's, the A4's)
  //      - the PRE / POST trims the slot in hand's (the console's box=), trimpre= / trimpost= every slot's
  {
    char p[96];
    {
      device::Scope in(device::g_links[1]);
      const char* d = device::state_path("/.rytm/fxslots", p, sizeof p);
      fs.mkdir(d);
      put(device::state_path("/.rytm/fxslots/R3.txt", p, sizeof p), "0123 MYVERB\n");
    }
    const fx::UserFx a4u = fx::user[0];
    g_out.clear(), g_out1.clear();
    send(0, sidecar({0x10, 'R', '3'}));   // the Rytm's project: its own map
    send(0, sidecar({0x23}));             // its ASK: the list, labels, states and BASE / WIDTH (it listens)
    settle();
    CHECK(key(1) == "R3" && key(0) == "A4P");
    printf("  the Rytm in slot 1, project R3: its UFX1 %04x %s; the A4's UFX1 %04x '%s'\n", fx::user1[0].tag, fx::user1[0].name,
           fx::user[0].tag, fx::user[0].name);
    CHECK(fx::user1[0].tag == 0x123 && !strcmp(fx::user1[0].name, "MYVERB"));
    CHECK(fx::user[0].tag == a4u.tag && !strcmp(fx::user[0].name, a4u.name));
    auto names_has = [](const std::vector<std::vector<uint8_t>>& o, const char* n) {
      for (const auto& m : o)
        if (m.size() > 6 && m[4] == 0x21 && std::string(m.begin(), m.end()).find(n) != std::string::npos) return true;
      return false;
    };
    CHECK(names_has(g_out, "MYVERB") && !names_has(g_out1, "MYVERB"));
    auto bw = [](const std::vector<std::vector<uint8_t>>& o) {
      int n = 0;
      for (const auto& m : o) n += m.size() == 10 && m[4] == 0x22 && m[5] == 2;
      return n;
    };
    g_out.clear(), g_out1.clear();
    const uint32_t t0 = now_ms();
    while (now_ms() - t0 < 2300) idle_prepare();   // past one 2 s refresh
    printf("  BASE / WIDTH refresh in 2.3 s: %d to the Rytm (slot 1), %d to the A4 (slot 0)\n", bw(g_out), bw(g_out1));
    CHECK(bw(g_out) >= 2 && bw(g_out1) == 0);
    {
      device::Scope in(device::g_links[1]);
      out::set_trim(0, -30);
    }
    CHECK(out::control1.trim10[0] == -30 && out::control.trim10[0] == 0);
    out::set_trim_all(1, 15);
    CHECK(out::control1.trim10[1] == 15 && out::control.trim10[1] == 15);
    out::set_trim_all(0, 0), out::set_trim_all(1, 0);
  }

  // ---- the Pi's own messages (the boot self-check's trig, the bench) are no box speaking: slot 0 stays presumed, and an
  // A4 whose USB comes first still takes slot 0 (the QEMU gate's a4-rytm order, 2026-11-21)
  device::reset();
  device::set_out(1, out1);
  CHECK(device::g_links[0].presumed && device::rytm_slot() == 0);
  {
    const std::vector<uint8_t> m = trig(0, 48);
    engine_sysex_self(m.data(), (int)m.size());
  }
  CHECK(device::g_links[0].presumed);
  CHECK(device::usb_attach(&device::kA4) == 0);
  device::step(true);
  CHECK(device::slot_held_by(&device::kA4) == 0 && device::rytm_slot() < 0 && !device::g_links[0].presumed);
  CHECK(device::usb_attach(&device::rytm()) == 1);
  device::step(true);
  CHECK(device::rytm_slot() == 1 && device::slot_held_by(&device::kA4) == 0);
  printf("  self-check before any box: slot 0 still presumed; the A4's USB first takes slot 0, the Rytm's slot 1\n");

  // ---- a stock box (no hello) whose USB MIDI the Pi matches only after its stream took the slot: its transport routed then
  // (unit run CK2, T521: the A4 on link1 had no route, its clock dropped)
  CHECK(device::route_of(1) < 0);
  CHECK(engine_realtime_link(1, 0xF8, 0) < 0);   // (dropped: no slot)
  device::set_midi_slot(1, 0);
  device::step(true);
  CHECK(device::route_of(1) == 0 && engine_realtime_link(1, 0xF8, 0) == 0);
  device::step(true);
  CHECK(device::route_of(1) == 0);
  device::set_midi_slot(1, -1);
  printf("  a stock box's USB MIDI matched after its stream: its transport routed, its clock reaches its slot\n");

  // ---- a Rytm whose USB enumerates at boot, on the slot 0 presumed the Rytm's: its USB confirms it, so an A4 after it takes slot 1
  // (the QEMU gate's rytm-a4 order, 2026-11-21)
  device::reset();
  device::set_out(1, out1);
  CHECK(device::g_links[0].presumed);
  CHECK(device::usb_attach(&device::rytm()) == 0);
  device::step(true);
  CHECK(device::rytm_slot() == 0 && !device::g_links[0].presumed && device::route_of(0) == 0);
  CHECK(device::usb_attach(&device::kA4) == 1);
  device::step(true);
  CHECK(device::rytm_slot() == 0 && device::slot_held_by(&device::kA4) == 1);
  printf("  the Rytm's USB at boot confirms slot 0; the A4's USB after it takes slot 1\n");

  // ---- a box that drops off the bus and comes back while its slot is still saving takes that same slot back, not the
  // lowest free one (a nudged cable, the RA1 re-enumeration: unit run AO1, 2026-11-21)
  CHECK(device::rytm_slot() == 0 && device::slot_held_by(&device::kA4) == 1);
  device::release(0);                          // the Rytm gone: slot 0 free
  CHECK(!device::held(0));
  device::usb_detach(1);                       // the A4 drops off the bus...
  device::step(false);                         // (its saves not landed yet: slot 1 parting)
  CHECK(device::g_links[1].parting && device::g_links[1].was == &device::kA4);
  CHECK(device::usb_attach(&device::kA4) == 1);   // ...and comes back at once: its own slot, not slot 0
  device::step(true);
  CHECK(device::slot_held_by(&device::kA4) == 1 && !device::held(0));
  printf("  a box back while its slot is still saving takes the same slot\n");

  // ---- the hold: a slot whose box left stays its box's for g_hold_ms after its saves land (slothold=, unit run AO2: a box
  // knocked off the hub came back 0.2 s later, its saves already done, and took the other slot)
  device::g_hold_ms = 600000;                  // (long: the test's own steps decide when it ends)
  CHECK(device::slot_held_by(&device::kA4) == 1 && !device::held(0));
  device::usb_detach(1);                       // the A4 drops off the bus
  device::step(true);                          // its saves land at once
  CHECK(!device::held(1) && !device::g_links[1].parting && device::g_links[1].holding);
  CHECK(device::usb_attach(&device::rytm()) == 0);   // another box: not its slot (slot 0, free)
  device::step(true);
  CHECK(device::rytm_slot() == 0 && device::g_links[1].holding);
  CHECK(device::usb_attach(&device::kDigitakt) == -1);   // a third: nothing (slot 1 still the A4's)
  CHECK(device::usb_attach(&device::kA4) == 1);   // the A4 back: its own slot
  device::step(true);
  CHECK(device::slot_held_by(&device::kA4) == 1 && !device::g_links[1].holding);
  device::usb_detach(1);                       // and gone for good: past the hold the slot is free for any box
  device::step(true);
  CHECK(device::g_links[1].holding);
  device::g_hold_ms = 1;
  { const uint32_t t0 = now_ms(); while (now_ms() - t0 < 3) {} }
  device::step(true);
  CHECK(!device::g_links[1].holding && device::usb_attach(&device::kDigitakt) == 1);
  device::step(true);
  CHECK(device::slot_held_by(&device::kDigitakt) == 1);
  device::g_hold_ms = 0;
  printf("  the hold: a box's slot kept for it after its saves; another box takes the other slot or waits\n");

  printf("test_attach_order: PASS (slots by attach order, folders by type: A4 + Digitakt either order, the Rytm in slot 1, "
         "a box leaving saves into its folder and frees its slot, the other box never moves, a box back loads its folder)\n");
  return 0;
}
