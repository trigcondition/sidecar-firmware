// core/tests/test_slot_state.cpp - each slot's project state in its own folder (docs/device-profiles.md "Several devices"):
// slot 0 (the Rytm) and slot 1 (another box) each have their own project, read from and saved into their own state folder,
// alone or side by side. Nothing of one box's project is the other's, and nothing of one is written into the other's folder.
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "granular/granular.h"
#include "lib/library.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "keys.h"
#include "mod/learn.h"
#include "samp/preview.h"
#include "lfo.h"
#include "lib/pools.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/xypad.h"
#include "pool/pool.h"
#include "presets/juno_lib.h"
#include "seq/seq.h"
#include "ui/remote.h"
#include "store/store.h"
#include "um_host.h"
#include "user/um.h"
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
    device::step();
    idle_prepare();
    idle_save();
    idle_stream();
    bool loading = project_loading();
    for (int k = 1; k < kVoiceSlots; ++k) {   // (every slot's fill: project_loading is the slot in hand's)
      device::Scope in(device::g_links[k]);
      loading = loading || project_loading();
    }
    if (i > 50 && idle_settled() && !loading) break;
  }
  idle_flush(false);
}

static void put_wav(const char* path, int frames) {   // 16-bit mono 48 kHz, a ramp
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f), u32(36 + frames * 2), fwrite("WAVEfmt ", 1, 8, f), u32(16), u16(1), u16(1), u32(48000),
      u32(96000), u16(2), u16(16), fwrite("data", 1, 4, f), u32(frames * 2);
  for (int i = 0; i < frames; ++i) u16((uint16_t)(i * 64));
  fclose(f);
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

int main() {
  if (kVoiceSlots < 2) {
    printf("test_slot_state: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-slotstate-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  CHECK(fs.mkdir("/.rytm/juno") && fs.mkdir("/.a4/juno") && fs.mkdir("/.a4/lfo"));
  // the A4's own LFO file: its T1A on its V2's P4 (slot 1's store holds it, slot 1's LFOs load it; slot 0's never see it)
  static const char kA4Lfo[] = "# the A4's\nT1A V2 3 0 100 0 0 20 64\n";
  put("/.a4/lfo/A4P.txt", kA4Lfo);
  // its INSERT / OUT file: V2's slot A FILTER, V2 on POST at LEV 90 (slot 1's chains and OUT, control1)
  CHECK(fs.mkdir("/.a4/ins"));
  put("/.a4/ins/A4P.txt", "V2 A FILTER 80 20 0 0\nOUT V2 POST 90 64 64 0\nB1 A SAT 0 40 64 127\nSFX A 1 70 11 22 33 44\nMFX 0 RET 80 127 127 127 127 0\n");
  // 3(c) step 8: its box settings in its own folder (the Rytm's keys in /.rytm): read at its attach into its own
  put("/.rytm/keys.txt", "mode=selected\nauto=3\nmac=1\n");
  put("/.a4/keys.txt", "mode=channel\nauto=5\nmac=0\n");
  put("/.a4/sendfx.txt", "out=fx\n");
  put("/.a4/fxin.txt", "src=fx\n");
  put("/.a4/preview.txt", "level=-6\n");
  put("/.a4/learn.txt", "#sidecar learn 1\nR 1 1 20 fader\n");
  // each box's JUNO list in its own folder (the A4's project B4: one row; the Rytm's NEW: two)
  put("/.a4/juno/B4.txt", "000102030405060708090a0b0c0d0e0f1011\tA4 ROW\n");
  put("/.rytm/juno/NEW.txt", "000102030405060708090a0b0c0d0e0f1012\tRYTM ONE\n000102030405060708090a0b0c0d0e0f1013\tRYTM TWO\n");
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  put_wav("/Samples/a.wav", 4800);
  put_wav("/Samples/b.wav", 9600);
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  settle();
  CHECK(!strcmp(prj(0).key, "RYP"));   // boot: slot 0's folder; slot 1 has no box yet

  // the A4 attached after the Rytm (slot 1), then the Rytm leaves: a lone A4 in slot 1 (attach order: it does not move), its
  // own project from /.a4; the Rytm's stays in slot 0, saved, until it comes back
  device::rytm_on_usb(true);
  hello_a4();
  // its send FX B set before its project is loaded (its store not filled yet; its file has no SFX B line): kept, not put
  // back to OFF by the load's clear (the QEMU two-box check, 2026-11-26), and written into its file
  {
    device::Scope in(device::g_links[1]);
    char o[256];
    CHECK(!store::ready() && fx::command("sendfx=2:halo", o, sizeof o));
  }
  device::rytm_on_usb(false);
  settle();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[1]);
  printf("  slot 1's send FX B set while it loaded: type %d (HALO %d)\n", fx::control1.knob[1][0], fx::type_from_name("halo"));
  CHECK(fx::control1.knob[1][0] == fx::type_from_name("halo") && fx::control.knob[1][0] == fx::kOff);
  CHECK(strstr(get("/.a4/ins/A4P.txt").c_str(), "\nSFX A 1 70 11 22 33 44\n"));   // (the file's own A line: kept)
  {
    char want[32];
    snprintf(want, sizeof want, "\nSFX B %d ", fx::type_from_name("halo"));
    CHECK(strstr(get("/.a4/ins/A4P.txt").c_str(), want));
  }
  // a TYPE set alone (no other INSERT edit) once it is loaded: its file written (before: only another edit's save took it)
  {
    device::Scope in(device::g_links[1]);
    char o[256];
    CHECK(store::ready() && fx::command("sendfx=1:comb", o, sizeof o));
  }
  settle();
  {
    char want[40];
    snprintf(want, sizeof want, "\nSFX A %d 70 11 22 33 44\n", fx::type_from_name("comb"));
    printf("  slot 1's file after a TYPE alone: %s\n", strstr(get("/.a4/ins/A4P.txt").c_str(), want) ? "SFX A COMB" : "unchanged");
    CHECK(strstr(get("/.a4/ins/A4P.txt").c_str(), want));
    device::Scope in(device::g_links[1]);
    char o[256];
    CHECK(fx::command("sendfx=1:1", o, sizeof o));   // (back to the file's TYPE 1 for the checks below)
  }
  settle();
  printf("  after the A4's hello: slot 0 %s, slot 1 %s\n", prj(0).key, prj(1).key);
  CHECK(!strcmp(prj(1).key, "A4P") && !strcmp(prj(0).key, "RYP"));
  {
    device::Scope in(device::g_links[1]);   // the A4's own messages see its own project
    CHECK(!strcmp(prj().key, "A4P"));
  }
  // its RAM store: filled from its own folder at its hello, apart from slot 0's
  {
    const uint8_t* d;
    uint32_t n;
    {
      device::Scope in(device::g_links[1]);
      CHECK(store::ready() && !project_loading());
      CHECK(store::get("/.rytm/lfo/A4P.txt", &d, &n) == store::kPresent && n == strlen(kA4Lfo) && !memcmp(d, kA4Lfo, n));
    }
    {
      device::Scope in(device::g_links[0]);   // (the lone A4 is the base box now: outside a Scope the slot in hand is 1)
      CHECK(store::ready(0) && store::get("/.rytm/lfo/A4P.txt", &d, &n) == store::kUnknown);   // (slot 0's: not its path)
    }
    printf("  stores: slot 0 %d files, slot 1 %d files\n", store::present(0), store::present(1));
  }
  // its inserts and OUT: in its own chains (control1), slot 0's untouched
  CHECK(ins::control1.type[1][0] == ins::kFilter && ins::control.type[1][0] == ins::kOff);
  CHECK(out::control1.route[1] == out::kPost && out::control1.lev[1] == 90 && out::control.route[1] == out::kTrk);
  CHECK(ins::param(1, 0, 0) == 80);   // (the slot in hand: the lone A4's)
  // 3(c) step 8: its bus, master and send FX settings (its file) and its box settings (read at its attach), slot 0's apart
  CHECK(ins::control1.type[ins::kBus0][0] == ins::kSat && ins::control.type[ins::kBus0][0] == ins::kOff);
  CHECK(master::control1.lev == 80 && master::control.lev == master::kLevUnity);
  printf("  slot 1's send FX A: type %d P1 %d P5 %d, route %d\n", fx::control1.knob[0][0], fx::control1.knob[0][1],
         fx::control1.knob[0][5], fx::control1.route);
  CHECK(fx::control1.knob[0][0] == 1 && fx::control1.knob[0][1] == 70 && fx::control1.knob[0][5] == 44);
  CHECK(fx::control.knob[0][0] == 0 && fx::control1.route == fx::kRouteFx && fx::control.route == fx::kRouteSlot);
  {
    device::Scope in(device::g_links[1]);
    CHECK(keys::auto_channel() == 5 && !keys::computer_on() && fxin::wanted() && preview::level_db() == -6);
    const learn::Route* r;
    CHECK(learn::routes(&r) == 1 && r[0].cc == 20);
  }
  {
    device::Scope in(device::g_links[0]);
    CHECK(keys::auto_channel() == 3 && keys::computer_on() && !fxin::wanted() && preview::level_db() != -6);
    CHECK(learn::routes(nullptr) == 0);
  }
  // its LFOs: loaded from its store when the fill ended; the slot in hand outside a Scope is the lone A4's
  CHECK(lfo::get(0).target == lfo::kTVoice + 1 && lfo::get(0).param == 3 && lfo::get(0).depth == 20);
  {
    device::Scope in(device::g_links[0]);
    CHECK(lfo::get(0).target == 0);   // (the Rytm's project has no LFO file)
  }

  // the Rytm on USB beside it: each keeps its own
  device::rytm_on_usb(true);
  settle();
  CHECK(device::g_base == &device::g_links[0]);
  CHECK(!strcmp(prj(0).key, "RYP") && !strcmp(prj(1).key, "A4P"));

  // each slot's LFOs on its own box's voices: the A4's T1A (V2 P4) moves engine voice kBoxVoices + 1 only, never the
  // Rytm's voice 1 (the same V2 in slot 0's numbering)
  {
    const int va = kBoxVoices + 1;
    v[1].p[3] = 50.0f, v[va].p[3] = 50.0f;
    uint32_t us = 5000000;
    float lo = 50, hi = 50;
    for (int i = 0; i < 400; ++i, us += 2000) {
      mod::tick(us);
      lo = v[va].p[3] < lo ? v[va].p[3] : lo, hi = v[va].p[3] > hi ? v[va].p[3] : hi;
      CHECK(v[1].p[3] == 50.0f);
    }
    printf("  slot 1's T1A on V2 P4: engine voice %d %.2f..%.2f, voice 1 %.2f\n", va, lo, hi, v[1].p[3]);
    CHECK(hi - lo > 10.0f);
    {
      device::Scope in(device::g_links[1]);
      CHECK(mod::find(mod::tid(mod::kV, 1, 3)) != nullptr);   // slot 1's layer has it
    }
    {
      device::Scope in(device::g_links[0]);
      CHECK(mod::find(mod::tid(mod::kV, 1, 3)) == nullptr);   // slot 0's has not
    }
    // a report of the A4's voice moves slot 1's base (the voice's own slot), the value stays modulated around it
    float p[16] = {};
    p[3] = 70.0f;
    mod::voice_params(va, p, 16);
    CHECK(mod::base_of(va, 3, 0) == 70.0f && mod::base_of(1, 3, -1) == -1.0f);
    // slot 1 takes its own voices, chains, OUT, send FX and master (3(c) step 8): an FX knob target moves its own send FX
    // (fx::control1), a bus target its own bus chain; the Rytm's track sends (kSnd) stay slot 0's; its I2 offset lands on
    // its own chain (control1), never slot 0's
    {
      device::Scope in(device::g_links[1]);
      char o[256];
      CHECK(fx::command("sendknob=1:3:40", o, sizeof o) && fx::control1.knob[0][3] == 40 && fx::control.knob[0][3] == 0);
      mod::begin_tick();
      mod::offset(0, mod::tid(mod::kFx, 0, 1), 10.0f, 1.0f);
      mod::offset(0, mod::tid(mod::kIns, 1, 0), 10.0f, 1.0f);
      mod::offset(0, mod::tid(mod::kIns, 8, 0), 10.0f, 1.0f);
      mod::offset(0, mod::tid(mod::kSnd, 0, 0), 10.0f, 1.0f);
      CHECK(mod::find(mod::tid(mod::kFx, 0, 1)) != nullptr && mod::find(mod::tid(mod::kIns, 8, 0)) != nullptr);
      CHECK(mod::find(mod::tid(mod::kSnd, 0, 0)) == nullptr);
      mod::end_tick();
      printf("  slot 1's FX P3 %d (slot 0's %d)\n", fx::control1.knob[0][3], fx::control.knob[0][3]);
      CHECK(fx::control1.knob[0][3] == 50 && fx::control.knob[0][3] == 0);
      CHECK(ins::control1.play[1][0][0] == 90 && ins::control.play[1][0][0] == 0);
      mod::begin_tick();
      mod::end_tick();   // (I2 and the FX knob restored: not live this tick)
      CHECK(ins::control1.play[1][0][0] == 80 && fx::control1.knob[0][3] == 40);
    }
  }

  // each box's POOL lists and the machines' copies of them (option A: nothing shared): the same row holds another file
  // in each slot; GRANULAR / WAVETABLE compose each slot's own list, and the views read the slot in hand's
  {
    lib::rescan();
    settle();
    const int b7 = pools::code(1, 7);
    char o[256];
    CHECK(pools::command("pool=smp:set:B007:/Samples/a.wav", o, sizeof o) && strstr(o, "B007"));
    CHECK(pools::command("pool=wt:set:B007:/Samples/a.wav", o, sizeof o) && strstr(o, "B007"));
    {
      device::Scope in(device::g_links[1]);
      CHECK(pools::command("pool=smp:set:B007:/Samples/b.wav", o, sizeof o) && strstr(o, "B007"));
      CHECK(pools::command("pool=wt:set:B007:/Samples/b.wav", o, sizeof o) && strstr(o, "B007"));
    }
    settle();
    printf("  B007: slot 0 %s / %s, slot 1 %s / %s\n", pools::at(0, pools::kSamples, b7).path,
           pools::at(0, pools::kTables, b7).path, pools::at(1, pools::kSamples, b7).path, pools::at(1, pools::kTables, b7).path);
    CHECK(!strcmp(pools::at(0, pools::kSamples, b7).path, "/Samples/a.wav"));
    CHECK(!strcmp(pools::at(1, pools::kSamples, b7).path, "/Samples/b.wav"));
    CHECK(!strcmp(gr::entry_path(b7), "/Samples/a.wav") && !strcmp(wt::entry_path(b7), "/Samples/a.wav"));
    {
      device::Scope in(device::g_links[1]);
      CHECK(!strcmp(gr::entry_path(b7), "/Samples/b.wav") && !strcmp(wt::entry_path(b7), "/Samples/b.wav"));
      CHECK(pools::find_hash(pools::kSamples, pools::at(0, pools::kSamples, b7).ref) < 0);   // slot 1's list: no a.wav
    }
    CHECK(gr::entry_state_in(0, b7) == gr::kStateCard && gr::entry_state_in(1, b7) == gr::kStateCard);
    // a voice resolves through its own box's list: voice 0 the Rytm's, voice kBoxVoices the A4's
    CHECK(gr::pick(0, b7) && gr::pick(kBoxVoices, b7));
    CHECK(gr::src_entry(0, b7) == b7 && gr::src_entry(kBoxVoices, b7) == b7);
    // a row seen playing on the A4's voice is in the A4's used set only
    pools::seen_in(slot_of_voice(kBoxVoices), pools::kSamples, b7);
    CHECK(pools::used(1, pools::kSamples, b7) && !pools::used(0, pools::kSamples, b7));
  }

  // the Rytm names another project: slot 0's alone, written into /.rytm; /.a4's untouched
  name_project(0, "NEW");
  settle();
  CHECK(!strcmp(prj(0).key, "NEW") && !strcmp(prj(1).key, "A4P"));
  CHECK(get("/.rytm/project") == "NEW" && get("/.a4/project") == "A4P");

  // a project name from the A4's transport is the A4's: slot 1's alone, written into /.a4
  {
    device::Scope in(device::g_links[1]);
    const uint8_t n[] = {'B', '4', 0xf7};
    set_project(n, 3);
  }
  settle();
  CHECK(!strcmp(prj(0).key, "NEW") && !strcmp(prj(1).key, "B4"));
  CHECK(get("/.rytm/project") == "NEW" && get("/.a4/project") == "B4");
  // the JUNO lists: each slot's read from its own folder at its project change, the other's left as it was
  printf("  JUNO lists: slot 0 %d rows (%s), slot 1 %d rows (%s)\n", junolib.listed(0), junolib.slot(0, 0).name,
         junolib.listed(1), junolib.slot(1, 0).name);
  CHECK(junolib.listed(0) == 2 && !strcmp(junolib.slot(0, 1).name, "RYTM TWO"));
  CHECK(junolib.listed(1) == 1 && !strcmp(junolib.slot(1, 0).name, "A4 ROW"));
  {
    device::Scope in(device::g_links[1]);
    CHECK(junolib.listed() == 1);   // the slot in hand's
  }

  // each slot's LFOs at its own project change: the A4's B4 has no LFO file (the defaults); the Rytm's untouched
  {
    device::Scope in(device::g_links[1]);
    CHECK(lfo::get(0).target == 0);
  }
  // both slots on the same project name: each edit lands in its own folder's file, never the other's
  put("/.a4/lfo/NEW.txt", "# the A4's NEW\n");   // (a project with files of its own: no rename ask, as at slot 0)
  {
    device::Scope in(device::g_links[1]);
    const uint8_t n[] = {'N', 'E', 'W', 0xf7};
    set_project(n, 4);
  }
  settle();
  CHECK(!strcmp(prj(0).key, "NEW") && !strcmp(prj(1).key, "NEW"));
  {
    char o[256];
    CHECK(lfo::command("lfo=T2A:dep:11", o, sizeof o));
    CHECK(ins::command("ins=V1:A:type:DRIVE", o, sizeof o) && ins::command("ins=B1:A:type:FILTER", o, sizeof o));
    device::Scope in(device::g_links[1]);
    CHECK(lfo::command("lfo=T2A:dep:-22", o, sizeof o));
    CHECK(ins::command("ins=V1:A:type:CRUSH", o, sizeof o) && out::command("out=V1:lev:77", o, sizeof o));
    // 3(d): slot 1's buses, B3 route and master are its own (its file, control1, master::control1)
    CHECK(ins::command("ins=B2:B:type:SAT", o, sizeof o) && master::command("mfx=lev:90", o, sizeof o));
    ins::set_route3(1);
    CHECK(fx::command("sendfx=2:comb", o, sizeof o) && fx::command("sendfxout=main", o, sizeof o));
    CHECK(keys::command("keys=auto:7", o, sizeof o) && fxin::command("fxin=0", o, sizeof o));
  }
  settle();
  {
    const std::string r = get("/.rytm/lfo/NEW.txt"), a = get("/.a4/lfo/NEW.txt");
    printf("  NEW's LFO files: /.rytm %d bytes, /.a4 %d bytes\n", (int)r.size(), (int)a.size());
    CHECK(strstr(r.c_str(), "\nT2A OFF 0 0 64 0 0 11 ") && !strstr(r.c_str(), " -22 "));
    CHECK(strstr(a.c_str(), "\nT2A OFF 0 0 64 0 0 -22 ") && !strstr(a.c_str(), " 11 "));
    CHECK(lfo::get(lfo::lfo_of(1, 0)).depth == 11 && !lfo::dirty_any());
    device::Scope in(device::g_links[1]);
    CHECK(lfo::get(lfo::lfo_of(1, 0)).depth == -22);
  }
  {
    const std::string r = get("/.rytm/ins/NEW.txt"), a = get("/.a4/ins/NEW.txt");
    printf("  NEW's INSERT files: /.rytm %d bytes, /.a4 %d bytes\n", (int)r.size(), (int)a.size());
    CHECK(strstr(r.c_str(), "\nV1 A DRIVE ") && strstr(r.c_str(), "\nB1 A FILTER ") && !strstr(r.c_str(), "CRUSH"));
    CHECK(strstr(a.c_str(), "\nV1 A CRUSH ") && strstr(a.c_str(), "\nOUT V1 TRK 77 ") && !strstr(a.c_str(), "DRIVE") &&
          !strstr(a.c_str(), "\nB1 ") && !ins::settings_dirty_any());
    CHECK(ins::control.type[0][0] == ins::kDrive && ins::control1.type[0][0] == ins::kCrush);
    printf("  slot 1's bus / master lines:%s%s%s\n", strstr(a.c_str(), "\nB2 B SAT ") ? " B2" : "",
           strstr(a.c_str(), "\nROUTE B3 POST") ? " ROUTE" : "", strstr(a.c_str(), "\nMFX 0 RET 90 ") ? " MFX" : "");
    CHECK(strstr(a.c_str(), "\nB2 B SAT ") && strstr(a.c_str(), "\nROUTE B3 POST") && strstr(a.c_str(), "\nMFX 0 RET 90 "));
    CHECK(!strstr(r.c_str(), " SAT ") && !strstr(r.c_str(), "\nROUTE ") && !strstr(r.c_str(), "\nMFX "));
    CHECK(ins::control1.type[ins::kBus0 + 1][1] == ins::kSat && ins::control.type[ins::kBus0 + 1][1] == ins::kOff);
    CHECK(ins::control1.route3 == 1 && ins::control.route3 == 0);
    CHECK(master::control1.lev == 90 && master::control.lev == master::kLevUnity);
    CHECK(strstr(a.c_str(), "\nSFX B 2 ") && !strstr(r.c_str(), "\nSFX "));   // (slot 0's: the Rytm's kit)
    const std::string sf = get("/.a4/sendfx.txt"), kk = get("/.a4/keys.txt"), fi = get("/.a4/fxin.txt");
    printf("  slot 1's sendfx.txt \"%s\", fxin.txt \"%s\"\n", sf.c_str(), fi.c_str());
    CHECK(sf == "out=main\n" && strstr(kk.c_str(), "auto=7") && fi == "src=in\n");
    CHECK(get("/.rytm/sendfx.txt").empty() && strstr(get("/.rytm/keys.txt").c_str(), "auto=3"));
    CHECK(out::control1.lev[0] == 77 && out::control.lev[0] == 100);
  }
  // each slot's scenes and XY pad: its own locks, A / B and fader in its own folder's files, its morph on its own voices
  {
    char o[400];
    CHECK(scene::command("scene=lock:1:V1:P3:50", o, sizeof o) && strstr(o, "S1 1 lock"));
    CHECK(xypad::command("xy=set:x:1:V1:P5:50", o, sizeof o));
    {
      device::Scope in(device::g_links[1]);
      CHECK(scene::command("scene=lock:2:V2:P4:60", o, sizeof o) && strstr(o, "S2 1 lock") && !strstr(o, "S1 "));
      CHECK(scene::command("scene=ab:2:0", o, sizeof o) && scene::state().a == 1);
      CHECK(xypad::command("xy=hold:1", o, sizeof o) && xypad::cfg().hold && !xypad::cfg().s[0][0].t);
    }
    CHECK(scene::state().a < 0 && !xypad::cfg().hold && xypad::cfg().s[0][0].t);   // (slot 0's untouched)
    settle();
    const std::string r = get("/.rytm/scene/NEW.txt"), a4 = get("/.a4/scene/NEW.txt");
    const std::string rx = get("/.rytm/xy/NEW.txt"), ax = get("/.a4/xy/NEW.txt");
    printf("  NEW's scene files: /.rytm %d bytes, /.a4 %d bytes; xy %d / %d bytes\n", (int)r.size(), (int)a4.size(),
           (int)rx.size(), (int)ax.size());
    CHECK(strstr(r.c_str(), "\nL 1 V1:P3 6400") && !strstr(r.c_str(), "V2:P4"));
    CHECK(strstr(a4.c_str(), "\nL 2 V2:P4 7680") && strstr(a4.c_str(), "\nAB 2 0") && !strstr(a4.c_str(), "V1:P3"));
    CHECK(strstr(rx.c_str(), "V1:P5") && strstr(ax.c_str(), "H 1") && !strstr(ax.c_str(), "V1:P5"));
    CHECK(!scene::dirty_any() && !xypad::dirty_any());
    // slot 1's A (scene 2, V2's P4 at 60) morphs its own voice kBoxVoices + 1 only
    const int va = kBoxVoices + 1;
    v[1].p[3] = 20.0f, v[va].p[3] = 20.0f;
    uint32_t us = 9000000;
    for (int i = 0; i < 100; ++i, us += 2000) mod::tick(us);
    printf("  slot 1's morph: voice %d P4 %.2f, voice 1 P4 %.2f\n", va, v[va].p[3], v[1].p[3]);
    CHECK(v[va].p[3] == 60.0f && v[1].p[3] == 20.0f);
  }

  // each box's user slots U1..U5 (option (a), decided 2026-11-14): its own map in its own folder, its own load slots;
  // the slot in hand's U2 is its own box's (an id not installed: the map saved, the slot MISSING)
  {
    um::begin(host_um_exec());
    auto um_settle = [] {
      for (int i = 0; i < 2000; ++i) {
        bool did = um::idle();
        idle_save();
        if (!did && i > 20) break;
      }
    };
    um_settle();
    char o[200];
    CHECK(um::command("um=put:2:rytm.alpha:1.0.0", o, sizeof o));   // (outside a Scope: the base box, the Rytm)
    {
      device::Scope in(device::g_links[1]);
      CHECK(um::command("um=put:2:a4.bravo:1.0.0", o, sizeof o));
      CHECK(um::command("um=put:4:a4.delta:1.0.0", o, sizeof o));
    }
    um_settle();
    const std::string r = get("/.rytm/slots/NEW.txt"), a = get("/.a4/slots/NEW.txt");
    printf("  user slots: /.rytm %s/.a4 %s", r.c_str(), a.c_str());
    CHECK(strstr(r.c_str(), "U2 rytm.alpha 1.0.0") && !strstr(r.c_str(), "a4."));
    CHECK(strstr(a.c_str(), "U2 a4.bravo 1.0.0") && strstr(a.c_str(), "U4 a4.delta 1.0.0") && !strstr(a.c_str(), "rytm."));
    CHECK(!strcmp(um::slot_view(1).id, "rytm.alpha") && !um::slot_view(3).id[0]);
    {
      device::Scope in(device::g_links[1]);
      CHECK(!strcmp(um::slot_view(1).id, "a4.bravo") && !strcmp(um::slot_view(3).id, "a4.delta"));
    }
    // the A4's project changes: its map read again from its own folder, the Rytm's untouched
    {
      device::Scope in(device::g_links[1]);
      const uint8_t n[] = {'B', '4', 0xf7};
      set_project(n, 3);
    }
    settle();
    um_settle();
    CHECK(!strcmp(um::slot_view(1).id, "rytm.alpha"));
    {
      device::Scope in(device::g_links[1]);
      CHECK(!um::slot_view(1).id[0] || strcmp(um::slot_view(1).id, "a4.bravo"));   // (B4's own map: none, or inherited)
    }
  }

  // the sequencer per slot (3(c) step 7): the A4's own seq track, its own 2E, its 5-byte 2C records playing its own
  // voice (kBoxVoices + track) in its own pass; tracks.txt in its own folder; the Rytm's sequencer untouched
  {
    char o[900];
    const uint32_t on0 = seq::stats().notes_on;   // (outside a Scope: the base box's, the Rytm's)
    {
      device::Scope in(device::g_links[1]);
      CHECK(seq::command("seq=track:1:on", o, sizeof o));
      CHECK(seq::command("seq=step:1:1:60", o, sizeof o) && seq::command("seq=step:1:5:64", o, sizeof o));
      CHECK(seq::owns_track(0) && !seq::owns_track(4));   // (track 5: the A4's FX track, no voice)
    }
    CHECK(!seq::owns_track(0));
    CHECK(seq::owns_voice(kBoxVoices) && !seq::owns_voice(0) && !seq::owns_voice(kBoxVoices + 1));
    g_out.clear(), g_out1.clear();
    seq::tick(now_ms());
    auto mask_of = [](const std::vector<std::vector<uint8_t>>& w) {
      int m = -1;
      for (const auto& x : w)
        if (x.size() > 7 && x[4] == 0x2E) m = x[6] | x[7] << 7;
      return m;
    };
    printf("  seq 2E: to the A4 0x%x, to the Rytm 0x%x\n", mask_of(g_out1), mask_of(g_out));
    CHECK(mask_of(g_out1) == 1 && mask_of(g_out) <= 0);
    // the A4's seq mode on track 1 (2D 08 1 0): seq mode taken, no SEQ page opened (its mod shows host pages only it
    // opened; the remote UI is one still)
    {
      const bool open_was = ui::remote_open();
      const uint8_t sm[] = {0xf0, 0x7d, 0x52, 0x54, 0x2D, 1, 8, 1, 0, 0xf7};
      engine_sysex_link(1, sm, (int)sizeof sm);
      seq::tick(now_ms() + 200);
      device::Scope in(device::g_links[1]);
      printf("  slot 1 seq mode: track %d, the remote page open %d (was %d)\n", seq::seqmode_on(), ui::remote_open(), open_was);
      CHECK(seq::seqmode_on() == 0 && ui::remote_open() == open_was);
    }
    // T374: the A4's held trig and encoder A (31 / 32, as sidecar-a4 sends them): taken by its slot's sequencer and
    // counted there (seq=ui:1); a 31 / 32 that comes before its 08 on is applied (kSeqOpenMs) is refused and counted
    {
      auto k31 = [](int c, int down) {
        const uint8_t m[] = {0xf0, 0x7d, 0x52, 0x54, 0x31, 1, (uint8_t)(c >> 7), (uint8_t)(c & 0x7f), (uint8_t)down, 0xf7};
        engine_sysex_link(1, m, (int)sizeof m);
      };
      auto e32 = [](int e, int d) {
        const uint8_t m[] = {0xf0, 0x7d, 0x52, 0x54, 0x32, 1, (uint8_t)e, (uint8_t)(d + 64), 0, 0xf7};
        engine_sysex_link(1, m, (int)sizeof m);
      };
      const seq::UiIn u0 = seq::ui_in(1), r0 = seq::ui_in(0);
      k31(4, 1);
      for (int i = 0; i < 12; ++i) e32(0, 2);
      k31(4, 0);
      seq::UiIn u = seq::ui_in(1);
      {
        device::Scope in(device::g_links[1]);
        CHECK(seq::command("seq=steps:1", o, sizeof o));
        printf("  slot 1 trig 5 held + A x12: %s\n", o);
        CHECK(strstr(o, " 5:") && !strstr(o, " 5:64"));   // (step 5 kept, its note moved)
        CHECK(seq::command("seq=ui", o, sizeof o));
        printf("  %s\n", o);
      }
      CHECK(u.keys == u0.keys + 2 && u.trig_down == u0.trig_down + 1 && u.trig_used == u0.trig_used + 1 && u.trig_tap == u0.trig_tap);
      CHECK(u.encs == u0.encs + 12 && u.enc_held == u0.enc_held + 12 && u.enc_mode == u0.enc_mode);
      CHECK(seq::ui_in(0).encs == r0.encs && seq::ui_in(0).keys == r0.keys);   // (the Rytm's slot saw none of it)
      // seq mode off (08 0, applied), then on again: the trig and turns sent before the on is applied are refused here
      const uint8_t off[] = {0xf0, 0x7d, 0x52, 0x54, 0x2D, 1, 8, 0, 0x7f, 0xf7};
      const uint8_t on[] = {0xf0, 0x7d, 0x52, 0x54, 0x2D, 1, 8, 1, 0, 0xf7};
      engine_sysex_link(1, off, (int)sizeof off);
      seq::tick(now_ms() + 400);
      engine_sysex_link(1, on, (int)sizeof on);
      k31(8, 1), e32(0, 2), k31(8, 0);
      seq::tick(now_ms() + 400);
      seq::UiIn v = seq::ui_in(1);
      printf("  08 off / on: applied off %u on %u; early 31s refused %u, 32s %u\n", (unsigned)(v.m08_applied_off - u.m08_applied_off),
             (unsigned)(v.m08_applied_on - u.m08_applied_on), (unsigned)(v.key_mode - u.key_mode), (unsigned)(v.enc_mode - u.enc_mode));
      CHECK(v.m08_off == u.m08_off + 1 && v.m08_on == u.m08_on + 1 && v.m08_applied_off == u.m08_applied_off + 1);
      CHECK(v.m08_applied_on == u.m08_applied_on + 1 && v.key_mode == u.key_mode + 2 && v.enc_mode == u.enc_mode + 1);
      device::Scope in(device::g_links[1]);
      CHECK(seq::seqmode_on() == 0);
    }
    // 3(c) step 8: the A4's mutes (2D 01 05) close its own voices' mute gates (out::control1), never the Rytm's
    {
      const uint8_t mu[] = {0xf0, 0x7d, 0x52, 0x54, 0x2D, 1, 5, 0x05, 0, 0xf7};   // tracks 1 and 3
      engine_sysex_link(1, mu, (int)sizeof mu);
      printf("  slot 1 mutes: open %d%d%d%d, slot 0's %d%d%d%d\n", out::control1.open[0], out::control1.open[1],
             out::control1.open[2], out::control1.open[3], out::control.open[0], out::control.open[1], out::control.open[2],
             out::control.open[3]);
      CHECK(!out::control1.open[0] && out::control1.open[1] && !out::control1.open[2] && out::control1.open[3]);
      for (int v = 0; v < 8; ++v) CHECK(out::control.open[v]);
      const uint8_t un[] = {0xf0, 0x7d, 0x52, 0x54, 0x2D, 1, 5, 0, 0, 0xf7};
      engine_sysex_link(1, un, (int)sizeof un);
      CHECK(out::control1.open[0] && out::control1.open[2]);
    }
    settle();
    CHECK(!get("/.a4/seq/B4/tracks.txt").empty());   // (its own folder)
    CHECK(get("/.rytm/seq/B4/tracks.txt").empty());
    // the A4 plays: 2C 01 <blk4> <n> {track step len speed flags} from its link, 32 steps of 6000 frames (two bars:
    // the first step after PLAY comes with its tick, not planned)
    const int side = device::side_slot();
    CHECK(side == 1);
    int64_t h = 0;
    int k = 0;
    for (int b = 0; b < 32 * 6000 / kBlock; ++b, h += kBlock) {
      if (h >= (int64_t)k * 6000 + 300 && k < 32) {   // (300 frames on the way)
        const int64_t f = (int64_t)k * 6000;
        const uint32_t bl = (uint32_t)(f / 32);
        const uint8_t m[] = {0xf0, 0x7d, 0x52, 0x54, 0x2C, 1, (uint8_t)(bl >> 21 & 0x7F), (uint8_t)(bl >> 14 & 0x7F),
                             (uint8_t)(bl >> 7 & 0x7F), (uint8_t)(bl & 0x7F), 1, 0, (uint8_t)(k % 16), 16, 2, 0, 0xf7};
        engine_sysex_link(1, m, (int)sizeof m);
        ++k;
      }
      seq::pre_block();
      seq::pre_block_side(side);
    }
    {
      device::Scope in(device::g_links[1]);
      CHECK(seq::command("seq=played", o, sizeof o));
      printf("  slot 1: %u notes on, %u bad ticks, transport %d; %s\n", (unsigned)seq::stats().notes_on,
             (unsigned)seq::stats().ticks_bad, seq::transport_now(), o);
      CHECK(seq::stats().notes_on >= 3 && strstr(o, ":1:60") && seq::stats().ticks_bad == 0 && seq::transport_now() == seq::kPlay);
    }
    printf("  slot 0: %u notes on, transport %d\n", (unsigned)seq::stats().notes_on, seq::transport_now());
    CHECK(seq::stats().notes_on == on0 && seq::transport_now() == seq::kStop);
  }

  // the A4 unplugged: slot 1 has no box, but keeps its folder, so a write queued for it before it left lands in /.a4
  device::set_out(1, nullptr);
  CHECK(!device::g_links[1].box && device::g_links[1].state && !strcmp(device::g_links[1].state, "/.a4"));
  {
    device::Scope in(device::g_links[1]);
    char b[64];
    CHECK(!strcmp(device::state_path("/.rytm/lfo/A4P.txt", b, sizeof b), "/.a4/lfo/A4P.txt"));
  }

  printf("test_slot_state: PASS\n");
  return 0;
}
