// core/tests/test_mod.cpp - the modulation layer (core/mod, spec-050 section 1): target ids, names and ranges; the
// entries, the tick's 1 ms gate and the voice flush, the base hooks (on the fake clock, a test source for the LFOs).
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "mod/mod.h"
#include "mod/target.h"

using namespace core;
using namespace core::mod;

static void ids_and_names() {
  CHECK(kind_of(tid(kV, 2, 4)) == kV && index_of(tid(kV, 2, 4)) == 2 && param_of(tid(kV, 2, 4)) == 4);
  char b[16]; Tid t;
  const char* names[] = {"V3:P5", "FX1:P2", "S5:F1", "I3:A2", "B1:E1", "M1:B3", "X2:B1", "MFX:LEV", "MFX:MIX2",
                         "O4:PAN", "T1:SPD", "R1:GATE", "LOOP:BAL", "RYTM:SCN", "RYTM:PERF3"};
  for (const char* n : names) { CHECK(parse(n, &t)); CHECK(!strcmp(name(t, b, sizeof b), n)); }
  CHECK(parse("v3:p5", &t) && t == tid(kV, 2, 4));
  CHECK(!parse("V9:P1", &t) && !parse("FX1:P6", &t) && !parse("Q1:X", &t));
  CHECK(range(tid(kV, 0, 0)).hi > 127.98f && range(tid(kV, 0, 0)).mode == kCont);   // list knobs too: mod never rounds kV
  CHECK(range(tid(kSeq, 0, 1)).mode == kStep && range(tid(kSeq, 0, 2)).mode == kCont);
  // the indices behind the names: FX1:P2 is slot 0 param 0, B1:E1 bus 1's chain (8) param 8, M1:B3 chain 11 param 6
  CHECK(parse("FX1:P2", &t) && t == tid(kFx, 0, 0) && parse("B1:E1", &t) && t == tid(kIns, 8, 8));
  CHECK(parse("M1:B3", &t) && t == tid(kIns, 11, 6) && parse("RYTM:PERF3", &t) && t == tid(kRytm, 0, 3));
  CHECK(!parse("MFX1:LEV", &t) && !parse("V3", &t) && !parse("I9:A1", &t) && !parse("M3:A1", &t) && !parse("V3:P5x", &t));
  CHECK(!valid(0) && !valid(tid(kV, 8, 0)) && !valid(tid(kKinds, 0, 0)) && !name(tid(kV, 8, 0), b, sizeof b)[0]);
  printf("mod: ids and names\n");
}

// ---- Task 3: the entries, the tick and its gate, the base hooks (a test source in place of the LFOs)
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static std::function<void(uint32_t, uint32_t)> g_src;
static void src_tramp(uint32_t us, uint32_t dus) { if (g_src) g_src(us, dus); }
static int g_internal = 0;
static void count_user(int, int, int, int, int, int) { if (user_report_internal) ++g_internal; }
static void start() {   // no entries, the clock started at g_us (the first call only starts it)
  mod::reset();
  mod::set_source_fn(src_tramp);
  g_src = nullptr;
  at(g_us + 1000000), mod::tick(now_us());
}
static void tick_one(float depth, float w, Tid t) {        // one 1 ms tick with one source
  g_src = [=](uint32_t, uint32_t) { mod::offset(0, t, depth, w); };
  at(g_us + 1000), mod::tick(now_us());
}
static void base_is_what_plays_now() {
  v[0].p[2] = 60;
  start();
  tick_one(10, 0.5f, tid(kV, 0, 2));
  CHECK(v[0].p[2] == 65.0f);                                // base 60 (v.p when the entry was made) + 10 x 0.5
  CHECK(mod::find(tid(kV, 0, 2)) && mod::find(tid(kV, 0, 2))->base == 60.0f && mod::base_of(0, 2, 99) == 60.0f);
  float p[16] = {}; p[2] = 70;
  mod::voice_params(0, p, 16);                              // a report: the live entry's base moves, p modulated
  CHECK(p[2] == 75.0f && mod::base_of(0, 2, 0) == 70.0f);
  float q[16] = {}; q[5] = 30;
  mod::voice_params(0, q, 16);                              // P6 has no entry: untouched, no base kept
  CHECK(q[5] == 30.0f && !mod::find(tid(kV, 0, 5)) && mod::base_of(0, 5, 77) == 77.0f);
  float pl[16];
  mod::played(0, pl);
  CHECK(pl[2] == v[0].p[2]);
  // a send FX knob: the 0x20's value is the base, the modulated one plays
  fx::control.knob[0][3] = 40;
  tick_one(20, 0.5f, tid(kFx, 0, 1));
  CHECK(fx::control.knob[0][3] == 50 && mod::fx_knob(0, 3, 90) == 100 && mod::fx_knob(0, 2, 33) == 33 && mod::fx_knob(0, 0, 3) == 3);
  printf("mod: a new entry's base is what plays now; a report moves a live entry's base only\n");
}
static void two_sources_sum_then_clamp_once() {
  v[0].p[2] = 120;
  start();
  g_src = [](uint32_t, uint32_t) { mod::offset(0, tid(kV, 0, 2), 32, 1); mod::offset(1, tid(kV, 0, 2), -32, 0.5f); };
  at(g_us + 1000), mod::tick(now_us());
  CHECK(v[0].p[2] == 127.99f);                              // 120 + 32 - 16 = 136, clamped once
  printf("mod: two sources summed, then clamped once\n");
}
static void restore_on_leave() {
  v[0].p[2] = 60;
  start();
  tick_one(10, 1, tid(kV, 0, 2));
  CHECK(v[0].p[2] == 70.0f);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());                     // a tick with no source: back to the base
  CHECK(v[0].p[2] == 60.0f && mod::stats().restores == 1 && !mod::find(tid(kV, 0, 2)));
  // an offset kind back to 0: the OUT route's PAN
  out::reset();
  tick_one(20, 1, tid(kOut, 2, 1));
  CHECK(out::control.pan[2] == 84);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(out::control.pan[2] == 64 && mod::stats().restores == 2);
  printf("mod: a target nothing refers to goes back to its base\n");
}
static void sub_ms_passes_only_flush() {
  hooks.user_report = count_user;
  v[0].user = 0, v[0].p[2] = 60;
  start();
  uint64_t t0 = g_us;
  g_internal = 0;
  tick_one(10, 1, tid(kV, 0, 2));                           // dirties the user voice; flushed at once (first call)
  CHECK(g_internal == 1);
  tick_one(10, 0.5f, tid(kV, 0, 2));                        // dirty again, 1 ms later: waits for its 5 ms
  CHECK(g_internal == 1);
  uint32_t ticks = mod::stats().ticks;                      // (the source stays: the same value, nothing new written)
  for (int i = 0; i < 3; ++i) at(g_us + 250), mod::tick(now_us());   // 250 us passes: no tick, no restore, no call yet
  CHECK(mod::stats().ticks == ticks && mod::stats().restores == 0 && g_internal == 1);
  while (g_us + 250 < t0 + 6000) at(g_us + 250), mod::tick(now_us());
  CHECK(g_internal == 1);
  at(g_us + 250), mod::tick(now_us());                      // t0 + 6 ms: 5 ms after the first call
  CHECK(g_internal == 2 && mod::stats().restores == 0);
  v[0].user = -1;
  hooks.user_report = nullptr;
  printf("mod: the passes between ticks flush only\n");
}
static void user_machine_spacing() {
  hooks.user_report = count_user;
  v[0].user = 0, v[0].p[2] = 60;
  start();
  uint64_t t0 = g_us;
  static int n;
  n = 0;
  g_src = [](uint32_t, uint32_t) { mod::offset(0, tid(kV, 0, 2), 20, (float)(++n % 7) / 7.0f); };
  g_internal = 0;
  while (g_us + 250 <= t0 + 21000) at(g_us + 250), mod::tick(now_us());
  printf("mod: a user machine's calls in 21 ms of passes: %d (ticks %u)\n", g_internal, mod::stats().ticks);
  CHECK(g_internal == 5);                                   // t0 + 1, 6, 11, 16, 21 ms
  v[0].user = -1;
  hooks.user_report = nullptr;
}
static void restore_all_flushes_now() {
  hooks.user_report = count_user;
  v[0].user = 0, v[0].p[2] = 60;
  start();
  tick_one(10, 1, tid(kV, 0, 2));
  tick_one(10, 0.5f, tid(kV, 0, 2));                        // dirty, its 5 ms not passed
  g_internal = 0;
  mod::restore_all();
  CHECK(g_internal == 1 && v[0].p[2] == 60.0f && !mod::find(tid(kV, 0, 2)));
  v[0].user = -1;
  hooks.user_report = nullptr;
  printf("mod: restore_all flushes a waiting user machine at once\n");
}

// ---- Task 5: the new writers (sends, master LEV / MIX, voice-to-bus sends) and the master chains' slots
static void master_lev_offset_not_saved() {
  master::set_lev(101);                                     // (100 alone is the default: no line)
  start();
  tick_one(20, 1, tid(kMfx, 0, 0));                         // MFX:LEV +20
  CHECK(master::control.lev == 101 && master::played_lev() == 121);   // the render's gain target 121 / 100
  char b[128];
  CHECK(master::line(b, sizeof b) > 0 && strstr(b, "MFX 0 RET 101 "));   // the knob saved, never the offset
  tick_one(-30, 1, tid(kMfx, 0, 2));                        // MFX:MIX2 -30 (a new tick: LEV left, back)
  CHECK(master::played_lev() == 101 && master::played_mix(1) == master::control.mix[1] - 30 && master::control.mix[1] == 40);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(master::played_mix(1) == 40);
  printf("mod: MFX:LEV / MIX offsets play, the knobs are what is saved\n");
}
static void sendfx_track(int t, int s1, int s2) {           // a 0x20 for Rytm track t: its sends to FX1 / FX2
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x20, 0x01, (uint8_t)t, (uint8_t)s1, (uint8_t)s2, 0xF7};
  CHECK(fx::take_sendfx(m, (int)sizeof m));
}
static void track_send_follows_0x20_base() {
  sendfx_track(0, 20, 0);
  start();
  tick_one(10, 1, tid(kSnd, 0, 0));                         // S1:F1 +10
  CHECK(fx::control.send[0][0] == 30);
  sendfx_track(0, 40, 5);                                   // the kit's send now 40: the base, still modulated
  CHECK(fx::control.send[0][0] == 50 && fx::control.send[0][1] == 5 && mod::find(tid(kSnd, 0, 0))->base == 40.0f);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(fx::control.send[0][0] == 40);
  printf("mod: a track's send follows its 0x20 base under an offset\n");
}
static void master_chain_slot_reachable() {
  ins::reset();
  ins::set_param(ins::kM0, 0, 0, 60);
  start();
  tick_one(15, 1, tid(kIns, 11, 0));                        // M1:A1 +15
  CHECK(ins::control.play[11][0][0] == 75);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(ins::control.play[11][0][0] == 60);
  printf("mod: the master chains' slots are targets (M1:A1)\n");
}
static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
static std::string g_card;
static void isnd_offset_saves_knob() {
  ins::reset();
  ins::set_send(0, 0, 30);
  start();
  tick_one(15, 1, tid(kIsnd, 0, 0));                        // X1:B1 +15 on the knob 30
  CHECK(ins::control.send[0][0] == 45 && ins::send_knob(0, 0) == 30);
  char b[300];
  CHECK(ins::command("ins", b, sizeof b) && strstr(b, "send 30/0/0"));   // the console shows the knob
  for (int i = 0; i < 1100; ++i) tick_one(15, 1, tid(kIsnd, 0, 0));      // a second on: the INSERT file is due
  for (int i = 0; i < 8; ++i) idle_save();
  std::string f;
  FILE* fp = fopen((g_card + "/.rytm/ins/MODT.txt").c_str(), "rb");
  CHECK(fp);
  for (int c; (c = fgetc(fp)) != EOF;) f += (char)c;
  fclose(fp);
  CHECK(f.find("SEND V1 30 0 0") != std::string::npos);
  ins::set_send(0, 0, 50);                                  // the knob moved under the offset
  CHECK(ins::control.send[0][0] == 65);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(ins::control.send[0][0] == 50);
  ins::reset();
  printf("mod: a voice's send to a bus: the offset plays, the INSERT file has the knob\n");
}

// ---- Task 6: the console, the JSON, the tick's cost
static void console_lists_live() {
  v[0].p[2] = 60;
  start();
  tick_one(6, 1, tid(kV, 0, 2));                            // V1:P3 60 + 6
  char o[400];
  CHECK(mod::command("mod", o, sizeof o));
  printf("mod: console: %s\n", o);
  CHECK(strstr(o, "mod: 1 live") && strstr(o, "V1:P3 66.00 (base 60.00 off +6)"));
  CHECK(mod::command("mod=stats", o, sizeof o) && strstr(o, "ticks 1") && strstr(o, "voice_sets 1"));
  CHECK(mod::command("mod=what", o, sizeof o) && strstr(o, "mod | mod=stats"));
  CHECK(!mod::command("modx", o, sizeof o) && !mod::command("lfo", o, sizeof o));
  int n = mod::json(o, sizeof o);
  printf("mod: json %s\n", o);
  CHECK(n > 0 && n < (int)sizeof o && !strncmp(o, "\"mod\":{\"live\":1,\"worst_us\":", 27) && strstr(o, "\"voice_sets\":1"));
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(mod::command("mod", o, sizeof o) && strstr(o, "mod: 0 live"));
}
static void worst_us_covers_tick() {
  start();
  g_src = [](uint32_t, uint32_t) { at(g_us + 300); };       // a source that takes 300 us (the fake clock)
  at(g_us + 1000), mod::tick(now_us());
  printf("mod: worst_us %u\n", mod::stats().worst_us);
  CHECK(mod::stats().worst_us >= 300 && mod::stats().worst_us < 400);
  g_src = nullptr;
}

// ---- review fixes: the walks stop at the high-water mark; a bad id never lands; a full table counts; the bases for
// SONG RECORD (mod::knob_base / send_base, fx::set_*_base through the filters)
static void high_water_and_guards() {
  start();
  char o[400];
  g_src = [](uint32_t, uint32_t) { mod::offset(0, tid(kV, 1, 3), 5, 1); mod::offset(1, tid(kV, 2, 3), 5, 1); };
  at(g_us + 1000), mod::tick(now_us());
  CHECK(mod::command("mod=stats", o, sizeof o) && strstr(o, " hi 2"));
  g_src = [](uint32_t, uint32_t) { mod::offset(1, tid(kV, 2, 3), 5, 1); };   // the first leaves: entry 0 free, hi stays 2
  at(g_us + 1000), mod::tick(now_us());
  CHECK(mod::command("mod=stats", o, sizeof o) && strstr(o, " hi 2"));
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(mod::command("mod=stats", o, sizeof o) && strstr(o, " hi 0") && mod::base_of(2, 3, 9) == 9.0f);
  g_src = [](uint32_t, uint32_t) { mod::offset(0, 0, 5, 1); mod::offset(0, tid(kV, 9, 0), 5, 1); mod::offset(0, tid(kKinds, 0, 0), 5, 1); };
  at(g_us + 1000), mod::tick(now_us());
  CHECK(mod::command("mod", o, sizeof o) && strstr(o, "mod: 0 live") && !mod::find(0));
  g_src = [](uint32_t, uint32_t) {   // more targets than entries: the rest counted, never written
    int n = 0;
    for (int c = 0; c < 13; ++c)
      for (int p = 0; p < 12; ++p) mod::offset(0, tid(kIns, c, p), 1, 1), ++n;
    for (int vi = 0; vi < 8; ++vi)
      for (int p = 0; p < 16; ++p) mod::offset(0, tid(kV, vi, p), 0, 1), ++n;
    for (int o = 0; o < 8; ++o)
      for (int p = 0; p < 3; ++p) mod::offset(0, tid(kOut, o, p), 1, 1), ++n;   // (0.5.0: kEntries 288)
  };
  at(g_us + 1000), mod::tick(now_us());
  CHECK(mod::stats().dropped == (uint32_t)(13 * 12 + 8 * 16 + 8 * 3 - mod::kEntries));
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  ins::reset();
  mod::note_lookup(42);
  CHECK(mod::stats().lookup_us == 42 && mod::json(o, sizeof o) > 0 && strstr(o, "\"lookup_us\":42") && strstr(o, "\"flush_us\":"));
  printf("mod: the walks stop at the high-water mark; bad ids refused; a full table counted\n");
}
static void song_reads_bases() {
  fx::control.knob[0][3] = 40;
  sendfx_track(2, 30, 7);
  start();
  g_src = [](uint32_t, uint32_t) { mod::offset(0, tid(kFx, 0, 1), 20, 1); mod::offset(1, tid(kSnd, 2, 0), 10, 1); };
  at(g_us + 1000), mod::tick(now_us());
  CHECK(fx::control.knob[0][3] == 60 && fx::control.send[2][0] == 40);   // as they play
  CHECK(mod::knob_base(0, 3) == 40 && mod::send_base(2, 0) == 30 && mod::send_base(2, 1) == 7 && mod::knob_base(0, 0) == fx::control.knob[0][0]);
  fx::set_knob_base(0, 3, 50), fx::set_send_base(2, 0, 20);              // a replay's values: the bases, modulated on top
  CHECK(fx::control.knob[0][3] == 70 && fx::control.send[2][0] == 30 && mod::knob_base(0, 3) == 50 && mod::send_base(2, 0) == 20);
  g_src = nullptr;
  at(g_us + 1000), mod::tick(now_us());
  CHECK(fx::control.knob[0][3] == 50 && fx::control.send[2][0] == 20);  // left: the replayed bases, not the old ones
  printf("mod: SONG RECORD's reads are the bases; its replay goes through the filters\n");
}

int main() {
  char tmpl[] = "/tmp/core_mod_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'M', 'O', 'D', 'T', 0xF7};
  engine_sysex(proj, sizeof proj);
  loaded();
  ids_and_names();
  base_is_what_plays_now();
  two_sources_sum_then_clamp_once();
  restore_on_leave();
  sub_ms_passes_only_flush();
  user_machine_spacing();
  restore_all_flushes_now();
  master_lev_offset_not_saved();
  track_send_follows_0x20_base();
  master_chain_slot_reachable();
  isnd_offset_saves_knob();
  console_lists_live();
  worst_us_covers_tick();
  high_water_and_guards();
  song_reads_bases();
  printf("test_mod: PASS\n");
  return 0;
}
