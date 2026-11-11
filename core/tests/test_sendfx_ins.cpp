// core/tests/test_sendfx_ins.cpp - 0.5.0 send FX as insert FX on the buses (core/fx/master.h, docs/INSERT-FX.md): the send
// TYPEs on B1..B3 under one cap with the master's slots, the voices still refused, every slot's own booking and MIX (no
// read past the master's arrays), the console's refusal, the card line with its MIX and a refused line kept (the file
// locked, not written over), LFOs on M1 / B3 reaching a send TYPE's params, the Rytm page's TYPE list, and the cost.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "fmt/fmt.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/send_fx.h"
#include "lfo.h"
#include "msamp/cardq.h"
#include "store/store.h"
#include "ui/extra.h"
#include "ui/ui.h"

using namespace core;
using Clock = std::chrono::steady_clock;

static std::string g_log;
static void sink(const char* l, int n) { g_log.append(l, (size_t)n), g_log += '\n'; }

static std::string rd(const std::string& p) {
  std::string s;
  if (FILE* f = fopen(p.c_str(), "rb")) {
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
  }
  return s;
}
static void wrs(const std::string& p, const char* t) {
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(t, 1, strlen(t), f);
  fclose(f);
}
static void card_core_turn() {
  host_card_core(0);
  while (cardq::service()) {}
  host_card_core(3);
}
static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save(), card_core_turn();
  CHECK(!project_loading());
}
static void saved() {
  ins::settings_due();
  for (int i = 0; i < 40; ++i) idle_save(), card_core_turn();
}
static const int kB1 = ins::kBus0, kB3 = ins::kBus0 + 2;
static int type_of(int c, int s) { return ins::control.type[c][s]; }
static void mset(int i, int t) { ins::set_type(master::chain(i), master::slot(i), t); }

// which chains take a send TYPE, and the one cap over the master and the buses
static void cap_and_chains() {
  ins::reset();
  CHECK(master::kFxSlots == 4);
  for (int v = 0; v < ins::kVoices; ++v) {   // the voices: never (their chains run on core 1 too)
    ins::set_type(v, 0, ins::kMRings);
    CHECK(type_of(v, 0) == ins::kOff);
  }
  CHECK(master::slot_index(0, 0) < 0 && master::slot_index(ins::kM0 + 1, 1) == 3 && master::slot_index(kB1, 0) == master::kSlots &&
        master::slot_index(kB3, 1) == master::kAllSlots - 1);
  for (int b = 0; b < ins::kBuses; ++b) {   // every bus takes every send TYPE (one at a time here: the cap below)
    for (int t = ins::kTypes; t < ins::kAllTypes; ++t) {
      ins::set_type(ins::kBus0 + b, 1, t);
      CHECK(type_of(ins::kBus0 + b, 1) == t);
    }
    ins::set_type(ins::kBus0 + b, 1, ins::kOff);
  }
  ins::set_type(kB1, 0, ins::kAmp);   // AMP still not a bus's
  CHECK(type_of(kB1, 0) == ins::kOff);
  // four at once, anywhere among the master's 4 and the buses' 6; the fifth refused, its slot keeps what it has
  ins::set_type(kB1, 0, ins::kMRings), ins::set_type(kB3, 0, ins::kMCloud), mset(0, ins::kMSpace), mset(3, ins::kMHalo);
  CHECK(type_of(kB1, 0) == ins::kMRings && type_of(kB3, 0) == ins::kMCloud && master::fx_used() == 4);
  ins::set_type(kB3, 1, ins::kComp);
  ins::set_type(kB3, 1, ins::kMChorus);
  CHECK(type_of(kB3, 1) == ins::kComp);
  mset(1, ins::kMComb);
  CHECK(type_of(ins::kM0, 1) == ins::kOff);
  // each booked slot on its own SendFx slot (two SendFx of two slots each)
  int seen = 0;
  for (int i = 0; i < master::kAllSlots; ++i)
    if (master::control.fx[i] >= 0) {
      CHECK(master::control.fx[i] < master::kFxSlots && !(seen & 1 << master::control.fx[i]));
      seen |= 1 << master::control.fx[i];
    }
  CHECK(seen == 15);
  // a send TYPE replaced by another keeps its room; one cleared frees it
  ins::set_type(kB1, 0, ins::kMHalo);
  CHECK(type_of(kB1, 0) == ins::kMHalo && master::fx_used() == 4);
  ins::set_type(kB1, 0, ins::kFilter);
  CHECK(master::fx_used() == 3);
  ins::set_type(kB3, 1, ins::kMChorus);
  CHECK(type_of(kB3, 1) == ins::kMChorus && master::fx_used() == 4);
  // the UIs' TYPE key: a bus skips AMP and, while full, the send TYPEs; a voice never offers one
  CHECK(master::next_type(master::slot_index(kB1, 1), ins::kRing, 1) == ins::kSat);
  CHECK(master::next_type(master::slot_index(kB1, 1), ins::kSat, 1) == ins::kOff);
  ins::set_type(kB3, 1, ins::kOff);
  CHECK(master::next_type(master::slot_index(kB1, 1), ins::kSat, 1) == ins::kMChorus);
  CHECK(master::next_type(master::slot_index(kB1, 1), ins::kOff, -1) == ins::kMHalo);
  ins::reset();
  CHECK(master::fx_used() == 0);
  printf("test_sendfx_ins: send TYPEs on B1..B3 and the master, 4 at once; the voices refused\n");
}

// the console: a bus slot takes one with its MIX; the cap's refusal says why; a voice is told where they go
static void console() {
  ins::reset();
  char o[400];
  CHECK(ins::command("ins=B3:A:type:rings", o, sizeof o) && strstr(o, "RINGS") && type_of(kB3, 0) == ins::kMRings);
  CHECK(ins::command("ins=B3:A:mix:90", o, sizeof o) && strstr(o, "mix 90"));
  CHECK(master::control.mix[master::slot_index(kB3, 0)] == 90);
  CHECK(ins::command("ins=V2:A:type:cloud", o, sizeof o) && strstr(o, "buses") && type_of(1, 0) == ins::kOff);
  CHECK(ins::command("ins=B1:A:type:cloud", o, sizeof o) && ins::command("ins=B1:B:type:halo", o, sizeof o));
  CHECK(master::command("mfx=1:type:space", o, sizeof o));
  CHECK(master::fx_used() == 4);
  CHECK(ins::command("ins=B2:A:type:chorus", o, sizeof o) && strstr(o, "at most") && strstr(o, "4"));
  CHECK(type_of(ins::kBus0 + 1, 0) == ins::kOff);
  CHECK(master::command("mfx=2:type:comb", o, sizeof o) && strstr(o, "at most") && strstr(o, "buses"));
  CHECK(ins::command("ins=B1:B:mix:300", o, sizeof o) && master::control.mix[master::slot_index(kB1, 1)] == 127);
  CHECK(ins::command("ins=V1:A:mix:30", o, sizeof o) && strstr(o, "ins="));   // a voice slot has no MIX: the help
  // /status: a bus slot's MIX, its labels; the room in use
  static char j[16384];
  int n = ins::json(j, sizeof j);
  CHECK(n > 0 && n < (int)sizeof j);
  CHECK(strstr(j, "{\"n\":\"B3\",\"s\":[{\"t\":\"RINGS\"") && strstr(j, "\"mix\":90") && strstr(j, "\"fxcap\":4,\"fxused\":4"));
  ins::reset();
  printf("test_sendfx_ins: the console takes and refuses them, with the reason\n");
}

// the audio: each slot on its own booking (the bus chains' index is not the master's), MIX 0 dry, 127 the effect alone
// n 12-frame blocks of noise through B1, B3 and the master as core 2 runs them (B1, B3, then the master), each output kept
struct Run3 { std::vector<float> b1, b3, m; };
static Run3 render3(int n, uint32_t seed, float gain = 0.1f) {
  Run3 o;
  uint32_t r = seed;
  for (int b = 0; b < n; ++b) {
    float y3[12][2], y1[12][2], ym[12][2];
    for (int j = 0; j < 12; ++j) {
      r = r * 1664525u + 1013904223u;
      float x = gain * (float)(int32_t)r * (1.0f / 2147483648.0f);
      y3[j][0] = y3[j][1] = y1[j][0] = y1[j][1] = ym[j][0] = ym[j][1] = x;
    }
    ins::engine().ProcessStereo(kB1, y1, 12, ins::control);
    ins::engine().ProcessStereo(kB3, y3, 12, ins::control);
    master::render(ym, 12);
    for (int j = 0; j < 12; ++j)
      o.b1.push_back(y1[j][0]), o.b1.push_back(y1[j][1]), o.b3.push_back(y3[j][0]), o.b3.push_back(y3[j][1]),
          o.m.push_back(ym[j][0]), o.m.push_back(ym[j][1]);
  }
  return o;
}
static double maxdiff(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = ~(size_t)0) {
  double d = 0;
  for (size_t i = from; i < a.size() && i < b.size() && i < to; ++i) d = std::max(d, (double)std::fabs(a[i] - b[i]));
  return d;
}
static double peak(const std::vector<float>& a, size_t from = 0) {
  double p = 0;
  for (size_t i = from; i < a.size(); ++i) p = std::max(p, (double)std::fabs(a[i]));
  return p;
}

// the audio: each slot on its own booking (the bus chains' index is not the master's), MIX 0 dry, 127 the effect alone
static void audio() {
  // four send TYPEs on three chains at once = each chain rendered alone (review: two slots sharing a SendFx slot differ)
  auto setup = [](int which) {   // bit 0 B1 B SPACE, bit 1 B3 RINGS > CLOUD, bit 2 M2 A HALO
    ins::reset();
    if (which & 2) ins::set_type(kB3, 0, ins::kMRings), ins::set_type(kB3, 1, ins::kMCloud);
    if (which & 1) ins::set_type(kB1, 1, ins::kMSpace);
    if (which & 4) mset(2, ins::kMHalo);
    for (int i = 0; i < master::kAllSlots; ++i) master::set_mix(i, 127);
  };
  setup(7);
  Run3 all = render3(4000, 3);
  CHECK(ins::engine().Faults() == 0);
  setup(1);
  Run3 b1 = render3(4000, 3);
  setup(2);
  Run3 b3 = render3(4000, 3);
  setup(4);
  Run3 m = render3(4000, 3);
  printf("test_sendfx_ins: peaks B3 RINGS>CLOUD %.3f, B1 SPACE %.3f, master HALO %.3f; vs alone %.1e %.1e %.1e\n", peak(all.b3),
         peak(all.b1), peak(all.m), maxdiff(all.b3, b3.b3), maxdiff(all.b1, b1.b1), maxdiff(all.m, m.m));
  CHECK(peak(all.b3) > 1e-4 && peak(all.b1) > 1e-4 && peak(all.m) > 1e-4);
  CHECK(maxdiff(all.b3, b3.b3) == 0 && maxdiff(all.b1, b1.b1) == 0 && maxdiff(all.m, m.m) == 0);
  // MIX 0 on a bus slot: the chain passes its input untouched (after the fades)
  ins::reset();
  ins::set_type(kB3, 0, ins::kMRings);
  master::set_mix(master::slot_index(kB3, 0), 0);
  double err = 0;
  for (int b = 0; b < 2000; ++b) {
    float y[12][2], x[12];
    for (int j = 0; j < 12; ++j) x[j] = 0.2f * std::sin(0.03f * (float)(b * 12 + j)), y[j][0] = y[j][1] = x[j];
    ins::engine().ProcessStereo(kB3, y, 12, ins::control);
    if (b > 100)
      for (int j = 0; j < 12; ++j) err = std::max(err, (double)std::fabs(y[j][0] - x[j]));
  }
  CHECK(err < 1e-6);
  // one code path: a bus slot at MIX 127 = the send FX's own return for the same input and knobs
  ins::reset();
  static fx::SendFx* sf = new fx::SendFx(48000.0f);
  sf->Reset();
  static fx::Control c;
  memset((void*)&c, 0, sizeof c);
  c.route = fx::kRouteMain;
  c.knob[0][0] = fx::kRings;
  for (int k = 0; k < 4; ++k) c.knob[0][2 + k] = ins::labels(ins::kMRings).def[k];
  c.send[0][0] = 127;
  ins::set_type(kB3, 1, ins::kMRings);
  master::set_mix(master::slot_index(kB3, 1), 127);
  double d = 0, pk = 0;
  uint32_t r = 7;
  for (int b = 0; b < 24000; ++b) {
    float st[16][fx::kStems] = {}, o[16][2], m[16][2], sl[16][4], y[16][2];
    for (int j = 0; j < 16; ++j) {
      r = r * 1664525u + 1013904223u;
      st[j][0] = 0.05f * (float)(int32_t)r * (1.0f / 2147483648.0f);
      y[j][0] = y[j][1] = st[j][0];
    }
    sf->Process(st, 16, o, c, m, sl);
    ins::engine().ProcessStereo(kB3, y, 16, ins::control);
    if (b > 20000)
      for (int j = 0; j < 16; ++j)
        d = std::max(d, (double)std::fabs(y[j][0] - sl[j][0])), d = std::max(d, (double)std::fabs(y[j][1] - sl[j][1])),
        pk = std::max(pk, (double)std::fabs(sl[j][0]));
  }
  printf("test_sendfx_ins: RINGS on B3 B vs the send FX: max difference %.2e of peak %.4f\n", d, pk);
  CHECK(pk > 1e-4 && d <= 1e-3 * pk + 2e-6);
  ins::reset();
  printf("test_sendfx_ins: bus slots play on their own booking and MIX\n");
}

// review M1: a room freed while its slot still fades out is not shared: the fading slot finishes on it undisturbed
// (bit for bit as alone), the slot that took the room passes its input dry meanwhile, then plays from clean
static void fade_not_shared() {
  auto start = []() {
    ins::reset();
    ins::set_type(kB1, 0, ins::kMCloud);
    master::set_mix(master::slot_index(kB1, 0), 127);
    render3(400, 9, 0.2f);   // 100 ms of CLOUD on B1
    ins::set_type(kB1, 0, ins::kOff);   // B1 A fades out over its 5 ms
  };
  start();
  Run3 alone = render3(80, 11);
  start();
  ins::set_type(kB3, 0, ins::kMRings);   // takes the room B1 A just left (a kit change does this to every room)
  CHECK(master::control.fx[master::slot_index(kB3, 0)] >= 0);
  master::set_mix(master::slot_index(kB3, 0), 127);
  Run3 both = render3(80, 11);
  printf("test_sendfx_ins: B1's fade-out with B3 on its room: %.1e from alone\n", maxdiff(both.b1, alone.b1));
  CHECK(maxdiff(both.b1, alone.b1) == 0);
  // B3 while B1 still fades (5 ms: B1 ends it in block 20, then lets the room go): dry, the noise as it came
  uint32_t r = 11;
  double dry = 0;
  for (int i = 0; i < 19 * 12; ++i) {
    r = r * 1664525u + 1013904223u;
    float x = 0.1f * (float)(int32_t)r * (1.0f / 2147483648.0f);
    dry = std::max(dry, (double)std::fabs(both.b3[2 * i] - x));
  }
  CHECK(dry < 1e-7);
  Run3 later = render3(2000, 13);   // then RINGS plays
  CHECK(peak(later.b3, 2 * 1000 * 12) > 1e-4);
  ins::reset();
  printf("test_sendfx_ins: a room left mid-fade is not shared\n");
}

// review M2: a room taken again never resumes the tail its last user left in the SendFx slot (SPACE's echoes)
static void no_old_tail() {
  for (int same = 0; same < 2; ++same) {   // another slot (M2 A) takes it, or the same slot again
    ins::reset();
    ins::set_type(kB3, 0, ins::kMSpace);
    master::set_mix(master::slot_index(kB3, 0), 127);
    for (int k = 0; k < 4; ++k) ins::set_param(kB3, 0, k, 100);   // long echoes
    render3(2000, 5, 0.3f);
    ins::set_type(kB3, 0, ins::kOff);
    render3(400, 5, 0.0f);   // faded out, the slot off
    if (same) {
      ins::set_type(kB3, 0, ins::kMSpace);
      for (int k = 0; k < 4; ++k) ins::set_param(kB3, 0, k, 100);
    } else {
      mset(2, ins::kMSpace);
      for (int k = 0; k < 4; ++k) ins::set_param(master::chain(2), 0, k, 100);
    }
    master::set_mix(same ? master::slot_index(kB3, 0) : 2, 127);
    Run3 o = render3(4000, 5, 0.0f);   // silence in: anything out is the old tail
    printf("test_sendfx_ins: SPACE taken again (%s): out of silence %.2e\n", same ? "B3 A again" : "M2 A", same ? peak(o.b3) : peak(o.m));
    CHECK((same ? peak(o.b3) : peak(o.m)) < 1e-6);
  }
  ins::reset();
}

// the card: a bus slot's line with its MIX round-trips; a line over the cap is said and its file kept
static void card_lines(const std::string& c) {
  uint8_t msg[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'F', 'I', 0xF7};
  // kit 3: five send TYPEs (an edited file, or a later build's): the fifth refused, said, the file never written over
  const char* five = "# x\nM1 A SPACE 40 50 0 20\nM1 B RINGS 50 0 80 60\nB1 A CLOUD 0 70 40 30 50\nB3 A HALO 70 30 60 0 90\nB3 B CHORUS 50 90 40 0 60\n";
  wrs(c + "/.rytm/ins/SFI/k003.txt", five);
  const char* voice = "# x\nV1 A RINGS 50 0 80 60\nV2 A COMP 50 60 40 20\n";   // a voice slot naming one (option B's line)
  wrs(c + "/.rytm/ins/SFI/k004.txt", voice);
  engine_sysex(msg, (int)sizeof msg);
  loaded();
  CHECK(store::ready());
  ins::kit_changed(0);
  CHECK(master::fx_used() == 0);
  ins::kit_changed(1);
  ins::set_type(kB3, 0, ins::kMRings);
  ins::set_param(kB3, 0, 1, 33);
  master::set_mix(master::slot_index(kB3, 0), 77);
  ins::set_type(kB1, 1, ins::kMCloud);   // MIX left at its default: still written (the line has it whenever the TYPE is a send's)
  saved();
  std::string t = rd(c + "/.rytm/ins/SFI/k002.txt");
  CHECK(t.find("B3 A RINGS 50 33 80 60 77\n") != std::string::npos && t.find("B1 B CLOUD 0 70 40 30 40\n") != std::string::npos);
  ins::kit_changed(0);
  CHECK(type_of(kB3, 0) == ins::kOff && master::fx_used() == 0 && master::control.mix[master::slot_index(kB3, 0)] == master::kMixDef);
  ins::kit_changed(1);
  CHECK(type_of(kB3, 0) == ins::kMRings && ins::param(kB3, 0, 1) == 33 && master::control.mix[master::slot_index(kB3, 0)] == 77);
  CHECK(type_of(kB1, 1) == ins::kMCloud && master::control.mix[master::slot_index(kB1, 1)] == master::kMixDef && master::fx_used() == 2);
  // the kit with five: four play, the fifth is in the log, and an edit there leaves the file as it was
  g_log.clear();
  ins::kit_changed(2);
  CHECK(master::fx_used() == 4 && type_of(kB3, 1) == ins::kOff && type_of(kB3, 0) == ins::kMHalo &&
        master::control.mix[master::slot_index(kB3, 0)] == 90 && master::control.mix[master::slot_index(kB1, 0)] == 50);
  printf("test_sendfx_ins: log: %s", g_log.c_str());
  CHECK(g_log.find("B3 B CHORUS") != std::string::npos && g_log.find("at most") != std::string::npos &&
        g_log.find("not written") != std::string::npos);
  ins::set_param(kB3, 0, 0, 12);
  saved();
  CHECK(rd(c + "/.rytm/ins/SFI/k003.txt") == five);
  // the voice slot's line: not read, said, kept
  ins::kit_changed(3);
  CHECK(type_of(0, 0) == ins::kOff && type_of(1, 0) == ins::kComp);
  ins::set_param(1, 0, 0, 12);
  saved();
  CHECK(rd(c + "/.rytm/ins/SFI/k004.txt") == voice);
  ins::kit_changed(0);
  printf("test_sendfx_ins: card: a bus slot's line and MIX round-trip; a refused line is said and its file kept\n");
}

// review H1: the master's "Clear the chain" (mfx=reset) clears the master's slots only: a bus's send TYPE keeps its
// booking and MIX, a master slot then books a room of its own, the cap holds, and the kit saves and reloads unlocked
static void reset_keeps_buses(const std::string& c) {
  ins::kit_changed(5);
  ins::set_type(kB1, 0, ins::kMRings);
  const int b = master::slot_index(kB1, 0);
  master::set_mix(b, 90);
  const int fb = master::control.fx[b];
  char o[400];
  CHECK(master::command("mfx=1:type:comp", o, sizeof o) && master::command("mfx=reset", o, sizeof o));
  CHECK(type_of(kB1, 0) == ins::kMRings && master::control.fx[b] == fb && fb >= 0 && master::control.mix[b] == 90);
  CHECK(master::fx_used() == 1);
  CHECK(master::command("mfx=1:type:cloud", o, sizeof o) && type_of(ins::kM0, 0) == ins::kMCloud);
  CHECK(master::control.fx[0] >= 0 && master::control.fx[0] != fb && master::fx_used() == 2);
  mset(1, ins::kMSpace), mset(2, ins::kMHalo);
  CHECK(master::fx_used() == 4);
  mset(3, ins::kMComb);
  CHECK(type_of(ins::kM0 + 1, 1) == ins::kOff && master::fx_used() == 4);
  saved();
  g_log.clear();
  ins::kit_changed(0);
  ins::kit_changed(5);
  CHECK(g_log.find("not taken") == std::string::npos && g_log.find("does not read") == std::string::npos);
  CHECK(type_of(kB1, 0) == ins::kMRings && master::control.mix[b] == 90 && master::fx_used() == 4);
  ins::set_param(kB1, 0, 0, 11);   // unlocked: an edit is written
  saved();
  CHECK(rd(c + "/.rytm/ins/SFI/k006.txt").find("B1 A RINGS 11 ") != std::string::npos);
  ins::kit_changed(0);
  // review L2: the boot bench leaves a bus's room alone (it runs only with nothing set anywhere)
  ins::reset();
  ins::set_type(kB3, 0, ins::kMRings);
  uint32_t w = 0;
  CHECK(master::bench(&w) == 0 && master::fx_used() == 1 && type_of(kB3, 0) == ins::kMRings);
  ins::reset();
  printf("test_sendfx_ins: the master's reset leaves the buses' send TYPEs, their rooms and MIX\n");
}

// LFO targets: M1 / M2 reach the master's slots, B3 the bus's, a send TYPE's params among them (the offset on the knob)
static void lfo_targets() {
  lfo::reset();
  ins::reset();
  mset(1, ins::kMRings), ins::set_type(kB3, 0, ins::kMCloud);
  ins::set_param(ins::kM0, 1, 1, 60), ins::set_param(kB3, 0, 2, 60);
  char o[160];
  CHECK(lfo::command("lfo=1:tgt:M1", o, sizeof o) && lfo::command("lfo=1:par:5", o, sizeof o));   // B P2 of M1: slot 2
  CHECK(lfo::command("lfo=1:wav:4", o, sizeof o) && lfo::command("lfo=1:rate:127", o, sizeof o) && lfo::command("lfo=1:dep:20", o, sizeof o));
  CHECK(lfo::command("lfo=2:tgt:B3", o, sizeof o) && lfo::command("lfo=2:par:2", o, sizeof o));   // A P3 of B3
  CHECK(lfo::command("lfo=2:wav:4", o, sizeof o) && lfo::command("lfo=2:rate:127", o, sizeof o) && lfo::command("lfo=2:dep:20", o, sizeof o));
  char pn[8];
  CHECK(std::string(lfo::param_name(lfo::target_from_name("M1"), 5, pn, sizeof pn)) == std::string("B") + ins::labels(ins::kMRings).label[1]);
  uint64_t us = 2000000;
  int mlo = 999, mhi = -1, blo = 999, bhi = -1;
  for (int i = 0; i < 1600; ++i) {
    us += 250, host_fake_time(true, us), lfo::tick(now_us());
    int m = ins::control.play[ins::kM0][1][1], b = ins::control.play[kB3][0][2];
    mlo = std::min(mlo, m), mhi = std::max(mhi, m), blo = std::min(blo, b), bhi = std::max(bhi, b);
  }
  printf("test_sendfx_ins: M1:B2 (RINGS) %d..%d, B3:A3 (CLOUD) %d..%d\n", mlo, mhi, blo, bhi);
  CHECK(mlo <= 45 && mhi >= 75 && ins::param(ins::kM0, 1, 1) == 60);
  CHECK(blo <= 45 && bhi >= 75 && ins::param(kB3, 0, 2) == 60);
  lfo::reset();
  ins::reset();
}

// the Rytm's INSERT page, BUS view: F TYPE runs on into the send TYPEs, H is the MIX of a send TYPE's slot; TRACK view: never
static void rytm_page() {
  ins::reset();
  ui::ins_page_set(true, 0, 2, -1, false, false);   // the BUS view, bus 3, slot A
  ins::set_type(kB3, 0, ins::kSat);
  ui::kInsPage.enc(5, 1, false);
  CHECK(type_of(kB3, 0) == ins::kMChorus);
  ui::kInsPage.enc(7, 5, false);   // H: MIX
  CHECK(master::control.mix[master::slot_index(kB3, 0)] == master::kMixDef + 5 && !ins::control.route3);
  static ui::Canvas cv;
  ui::kInsPage.draw(cv, 3, 9, 0);
  mset(0, ins::kMRings), mset(1, ins::kMRings), ins::set_type(kB1, 0, ins::kMRings);   // the room filled
  CHECK(master::fx_used() == 4);
  ui::ins_page_set(true, 1, 2, -1, false, false);   // bus 3, slot B: F stays off the send TYPEs (SAT the list's end)
  ins::set_type(kB3, 1, ins::kSat);
  ui::kInsPage.enc(5, 1, false);
  CHECK(type_of(kB3, 1) == ins::kSat);
  ui::kInsPage.draw(cv, 3, 9, 0);
  ins::set_type(0, 0, ins::kSat);
  ui::ins_page_set(false, 0, 0, 0, false, false);   // the TRACK view: SAT is the list's end
  ui::kInsPage.enc(5, 1, false);
  CHECK(type_of(0, 0) == ins::kSat);
  ins::reset();
  printf("test_sendfx_ins: the Rytm page's bus TYPE list and MIX\n");
}

// what four send TYPE inserts cost on core 2 (the numbers go to the report)
static void cost() {
  static const struct { const char* name; int t[4]; } kSets[] = {
      {"RINGS x4", {ins::kMRings, ins::kMRings, ins::kMRings, ins::kMRings}},
      {"CLOUD x4", {ins::kMCloud, ins::kMCloud, ins::kMCloud, ins::kMCloud}},
      {"RINGS CLOUD SPACE HALO", {ins::kMRings, ins::kMCloud, ins::kMSpace, ins::kMHalo}},
      {"CHORUS x4", {ins::kMChorus, ins::kMChorus, ins::kMChorus, ins::kMChorus}},
  };
  for (const auto& s : kSets) {
    double one[4] = {};
    for (int n = 1; n <= 4; n *= 2) {
      ins::reset();
      // B1 B, B3 A, B3 B, M1 A: buses and the master as core 2 runs them, every chain each block
      static const int kC[4] = {kB1, kB3, kB3, ins::kM0}, kS[4] = {1, 0, 1, 0};
      for (int i = 0; i < n; ++i) ins::set_type(kC[i], kS[i], s.t[i]), master::set_mix(master::slot_index(kC[i], kS[i]), 64);
      float y[12][2];
      uint32_t r = 5;
      double best = 1e9;
      for (int rep = 0; rep < 3; ++rep) {
        auto t0 = Clock::now();
        for (int b = 0; b < 4000; ++b) {
          float z[12][2], m[12][2];
          for (int j = 0; j < 12; ++j) {
            r = r * 1664525u + 1013904223u;
            y[j][0] = y[j][1] = z[j][0] = z[j][1] = m[j][0] = m[j][1] = 0.1f * (float)(int32_t)r * (1.0f / 2147483648.0f);
          }
          ins::engine().ProcessStereo(kB1, y, 12, ins::control);
          ins::engine().ProcessStereo(kB3, z, 12, ins::control);
          master::render(m, 12);
        }
        best = std::min(best, std::chrono::duration<double>(Clock::now() - t0).count());
      }
      one[n - 1] = best;   // 4000 blocks of 12 = 1 s of audio: the share of a core
    }
    printf("test_sendfx_ins: cost %-24s 1: %.2f %%, 2: %.2f %%, 4: %.2f %% of a Mac core (x 13.2: %.1f %% of a Pi 4 core)\n", s.name,
           100 * one[0], 100 * one[1], 100 * one[3], 100 * one[3] * 13.2);
  }
  ins::reset();
}

int main() {
  char tmpl[] = "/tmp/test_sendfx_ins.XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string c = tmpl;
  host_set_card(tmpl);
  CHECK(mkdir((c + "/NAM").c_str(), 0755) == 0);
  CHECK(mkdir((c + "/Samples").c_str(), 0755) == 0);
  host_set_log_sink(sink);
  host_fake_time(true, 5000000);
  CHECK(engine_begin());
  ins::reset();
  cap_and_chains();
  console();
  audio();
  no_old_tail();
  fade_not_shared();
  card_lines(c);
  reset_keeps_buses(c);
  lfo_targets();
  rytm_page();
  host_fake_time(false, 0);
  cost();
  printf("test_sendfx_ins: PASS\n");
  return 0;
}
