// core/tests/test_lfo.cpp - the Sidecar's LFOs (core/lfo.h) on the fake clock: the shapes and the DESIGN steps, free
// rates, sync to a MIDI clock (tempo changes, stop and start, song position), retrig on a trig and on PLAY, depth and
// clamping around a voice's base, the base restored when an LFO is turned off, FX knobs, no Rytm targets (never a CC;
// a Rytm track's note retrigs an FX target), the console and the per-project file,
// and the LFO page through the remote host (its catch mask, the design view's trig keys).
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "store/store.h"
#include "ui/extra.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void run_ms(int ms) {   // the control loop every 250 us
  for (int i = 0; i < ms * 4; ++i) at(g_us + 250), lfo::tick(now_us());
}
static int ccs(int ch, int cc, int* last = nullptr) {
  int n = 0;
  for (auto& m : g_out)
    if (m.size() == 3 && m[0] == (0xB0 | ch) && m[1] == cc) ++n, last ? *last = m[2] : 0;
  return n;
}
static void report(int cmd, int vi, int machine, const float* p, int note = 60) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(machine | vi)};
  if (cmd == 0x04) m.push_back((uint8_t)note), m.push_back(100);
  for (int k = 0; k < 16; ++k) {
    int iv = (int)p[k];
    m.push_back((uint8_t)iv), m.push_back((uint8_t)((p[k] - iv) * 128));
  }
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void set(int i, const char* kv) {
  char c[80], o[160];
  snprintf(c, sizeof c, "lfo=%d:%s", i, kv);
  CHECK(lfo::command(c, o, sizeof o));
  CHECK(!strstr(o, "cannot"));
}
// the card core's turn (with a card worker): every request posted to it run on core 0
static void card_core_turn() {
  host_card_core(0);
  while (cardq::service()) {
  }
  host_card_core(3);
}
static std::string rd_file(const std::string& p) {
  std::string s;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void wr_file(const std::string& p, const char* text) {
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}
// a project load (0x10, the boot): the store filled from the idle pass, then the LFOs loaded from it (Task 4)
static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
static bool fill_store() {   // the project's LFO files into the RAM store (core/store/store.h), as a project load will
  store::clear();
  lfo::want_files();
  store::fill_begin();
  for (int i = 0; i < 100000; ++i)
    if (store::fill_step()) return store::ready();
  return false;
}

// 2026-10-26, the RAM store (phase 1): a kit change reads the store, never the card - kit 6's own file, then (kit 7
// has none) the project's, then kit 6's again - and the control core is charged no card time for it
static void kit_change_reads_ram_only(const std::string& card) {
  wr_file(card + "/.rytm/lfo/STO/k006.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 10 0\n");
  wr_file(card + "/.rytm/lfo/STO.txt", "# Sidecar LFOs\n1 FX1 2 1 64 0 0 -5 0\n");
  uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'S', 'T', 'O', 0xF7};
  engine_sysex(m, (int)sizeof m);   // project STO: its files into the store, then its LFOs from there
  loaded();
  CHECK(store::ready());
  host_card_model(true);
  uint64_t ctl = host_card_us(3);
  lfo::kit_changed(5);   // kit 6: its own file
  CHECK(lfo::kit() == 5 && lfo::get(0).target == lfo::kTVoice + 2 && lfo::get(0).depth == 10);
  lfo::kit_changed(6);   // kit 7: none of its own, the project's
  CHECK(lfo::kit() == 6 && lfo::get(0).target == lfo::kTFx && lfo::get(0).depth == -5);
  lfo::kit_changed(5);
  CHECK(lfo::get(0).target == lfo::kTVoice + 2 && lfo::get(0).depth == 10);
  CHECK(host_card_us(3) == ctl);   // RAM only: no open, no exists, no recover on the control core
  host_card_model(false);
  printf("lfo: a kit change reads the RAM store only (no card time on the control core)\n");
}

// the kit's edit survives a kit change away and back before its card write lands: the save puts its text into the
// store at once; the write itself is posted (with the post slot busy, at a later pass - still dirty until then)
static void edit_then_switch_back(const std::string& card) {
  const std::string f6 = card + "/.rytm/lfo/STO/k006.txt";
  cardq::reset();
  cardq::set_worker(true);   // a card core: nothing reaches the card until its turn
  CHECK(lfo::kit() == 5);
  set(1, "dep:-7");
  lfo::kit_changed(6);
  CHECK(rd_file(f6).find("-7") == std::string::npos);   // not on the card yet
  lfo::kit_changed(5);
  CHECK(lfo::get(0).depth == -7 && lfo::get(0).target == lfo::kTVoice + 2);   // from the store
  card_core_turn();
  for (int i = 0; i < 8; ++i) idle_save(), card_core_turn();
  CHECK(rd_file(f6).find(" -7 ") != std::string::npos && !fmt::post_busy());

  // the post slot busy (another settings file in flight) when the kit changes: the edit waits, its text in the store
  char* t = fmt::post_begin();
  CHECK(t);
  memcpy(t, "x\n", 2);
  CHECK(fmt::post_end("/.rytm/other.txt", 2) && fmt::post_busy());
  set(1, "dep:-9");
  lfo::kit_changed(6);
  CHECK(!lfo::dirty() && fmt::post_busy());   // kit 6's write queued behind it (fmt::post_queue), not lost
  lfo::kit_changed(5);
  CHECK(lfo::get(0).depth == -9);   // from the store, before any card write
  CHECK(rd_file(f6).find(" -9 ") == std::string::npos);
  for (int i = 0; i < 16; ++i) card_core_turn(), idle_save();
  card_core_turn();
  for (int i = 0; i < 4; ++i) idle_save(), card_core_turn();
  CHECK(rd_file(f6).find(" -9 ") != std::string::npos && !lfo::dirty() && !fmt::post_busy());
  cardq::set_worker(false);
  cardq::reset();
  printf("lfo: an edit survives a kit change away and back before its card write lands\n");
}

// a kit without its own file whose default (the project's file) is there but cannot be read: the defaults, and the kit
// is NOT locked - its own file overwrites nothing (as the card path); a kit's own unreadable file still locks it
static void unreadable_default_does_not_lock_the_kit(const std::string& card) {
  const std::string pf = card + "/.rytm/lfo/STO.txt", f10 = card + "/.rytm/lfo/STO/k010.txt";
  const std::string f6 = card + "/.rytm/lfo/STO/k006.txt";
  const std::string p0 = rd_file(pf), k6 = rd_file(f6);
  host_fail_open("/.rytm/lfo/STO.txt", true);   // (one path at a time)
  CHECK(fill_store());
  const uint8_t* d;
  uint32_t n;
  CHECK(store::get("/.rytm/lfo/STO.txt", &d, &n) == store::kLocked);
  lfo::kit_changed(9);   // kit 10: none of its own, the project's cannot be read
  lfo::Settings def;
  lfo::defaults(&def);
  CHECK(lfo::kit() == 9 && lfo::get(0).target == def.target && lfo::get(0).depth == def.depth);
  set(1, "tgt:v6"), set(1, "dep:21");
  at(g_us + 1100000);
  for (int i = 0; i < 8; ++i) idle_save();
  CHECK(rd_file(f10).find("1 V6 ") != std::string::npos && rd_file(pf) == p0);   // saved: its own file, not the default
  host_fail_open("/.rytm/lfo/STO.txt", false);
  host_fail_open("/.rytm/lfo/STO/k006.txt", true);
  CHECK(fill_store());
  CHECK(store::get("/.rytm/lfo/STO/k006.txt", &d, &n) == store::kLocked);
  lfo::kit_changed(5);   // kit 6: its own file there but unreadable - locked, never written over
  set(1, "dep:-30");
  at(g_us + 1100000);
  for (int i = 0; i < 8; ++i) idle_save();
  CHECK(rd_file(f6) == k6);
  host_fail_open("/.rytm/lfo/STO/k006.txt", false);
  project_fill_begin();   // the project's store as the card has it again
  loaded();
  printf("lfo: an unreadable default does not lock a kit without its own file; an unreadable own file does\n");
}

static bool near(float a, float b, float e = 0.02f) { return fabsf(a - b) <= e; }
static bool near_ph(float a, float b, float e) { float d = fabsf(a - b); return d <= e || 1.0f - d <= e; }   // on the circle

int main() {
  char tmpl[] = "/tmp/core_lfo_XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  hooks.to_rytm = out;
  at(g_us);
  engine_begin();
  loaded();
  lfo::reset();

  // ---- shapes
  lfo::Settings s;
  lfo::defaults(&s);
  s.wave = lfo::kSine;
  CHECK(near(lfo::wave_at(s, 0, 1), 0) && near(lfo::wave_at(s, 0.25f, 1), 1) && near(lfo::wave_at(s, 0.75f, 1), -1));
  s.wave = lfo::kTri;
  CHECK(near(lfo::wave_at(s, 0, 1), 0) && near(lfo::wave_at(s, 0.25f, 1), 1) && near(lfo::wave_at(s, 0.5f, 1), 0) &&
        near(lfo::wave_at(s, 0.75f, 1), -1));
  s.wave = lfo::kSawUp;
  CHECK(near(lfo::wave_at(s, 0, 1), -1) && near(lfo::wave_at(s, 0.5f, 1), 0) && near(lfo::wave_at(s, 0.999f, 1), 1, 0.01f));
  s.wave = lfo::kSawDown;
  CHECK(near(lfo::wave_at(s, 0, 1), 1) && near(lfo::wave_at(s, 0.5f, 1), 0));
  s.wave = lfo::kSquare;
  CHECK(lfo::wave_at(s, 0.1f, 1) == 1 && lfo::wave_at(s, 0.6f, 1) == -1);
  for (int w : {lfo::kSampleHold, lfo::kRandom}) {   // the page's picture: within -1..1, S&H flat within a quarter
    s.wave = (uint8_t)w;
    for (int k = 0; k < 100; ++k) CHECK(fabsf(lfo::wave_at(s, k / 100.0f, 7)) <= 1.0f);
  }
  s.wave = lfo::kSampleHold;
  CHECK(lfo::wave_at(s, 0.01f, 7) == lfo::wave_at(s, 0.2f, 7));
  // DESIGN: step 0 = -64 HOLD, step 1 = +64 RAMP to step 2 = 0, step 2 SMOOTH to step 3 = +32
  memset(s.step, 0, sizeof s.step);
  memset(s.shape, 0, sizeof s.shape);
  s.wave = lfo::kDesign;
  s.step[0] = -64, s.shape[0] = lfo::kHold;
  s.step[1] = 63, s.shape[1] = lfo::kRamp;
  s.step[2] = 0, s.shape[2] = lfo::kSmooth;
  s.step[3] = 32;
  float st = 1.0f / 16;
  CHECK(near(lfo::design_at(s, 0.5f * st), -1) && near(lfo::design_at(s, 0.99f * st), -1));          // held
  CHECK(near(lfo::design_at(s, 1.5f * st), 63 / 128.0f, 0.03f));                                      // ramp halfway
  CHECK(near(lfo::design_at(s, 2.5f * st), 0.25f, 0.01f) && lfo::design_at(s, 2.1f * st) < 0.05f);    // smooth: slow start
  CHECK(near(lfo::wave_at(s, 1.5f * st, 0), lfo::design_at(s, 1.5f * st)));
  for (int p = 0; p < lfo::kPresets; ++p) {
    lfo::preset(&s, p);
    for (int k = 0; k < lfo::kSteps; ++k) CHECK(s.step[k] >= -64 && s.step[k] <= 63 && s.shape[k] < lfo::kShapes);
  }
  lfo::preset(&s, 0);
  CHECK(s.step[0] == -64 && s.step[15] == 63 && s.shape[3] == lfo::kRamp);   // RAMP

  // ---- rates
  CHECK(near(lfo::rate_hz(0), 0.02f, 1e-4f) && near(lfo::rate_hz(127), 20.0f, 0.05f));
  char rn[12];
  s.sync = 1, s.rate = 2;
  lfo::rate_name(s, rn, sizeof rn);
  CHECK(!strcmp(rn, "1/16") && lfo::div_ticks(2) == 6 && lfo::div_ticks(lfo::kDivs - 1) == 768);
  s.sync = 0, s.rate = 127;
  lfo::rate_name(s, rn, sizeof rn);
  CHECK(!strcmp(rn, "20HZ"));

  // ---- a free LFO on voice 1 (PLAITS) P2 HRM: depth and clamping around the Rytm's value, the base restored
  float p[16];
  for (float& x : p) x = 64;
  p[1] = 100;
  report(0x03, 0, 0x00, p);
  CHECK(v[0].p[1] == 100);
  set(1, "tgt:v1");
  set(1, "par:1");
  set(1, "wav:4");    // square: +1 for the first half
  set(1, "rate:0");   // 0.02 Hz: the first half lasts 25 s
  set(1, "dep:40");
  run_ms(20);
  CHECK(v[0].p[1] > 127.5f);                       // 100 + 40 clamped to 127.99
  CHECK(near(v[0].patch.harmonics, 127.99f / 127, 0.01f));   // the machine took it (apply)
  p[1] = 50;
  report(0x03, 0, 0x00, p);                        // the Rytm's knob: the new base, still modulated
  CHECK(near(v[0].p[1], 90, 0.01f) && near(lfo::base_of(0, 1, 0), 50));
  set(1, "dep:-63");
  run_ms(5);
  CHECK(v[0].p[1] == 0);                           // 50 - 63 clamped at 0
  set(1, "off");
  run_ms(5);
  CHECK(v[0].p[1] == 50 && near(v[0].patch.harmonics, 50 / 127.0f, 1e-4f));   // back to the base
  CHECK(lfo::base_of(0, 1, 77) == 77);

  // ---- free rate: a sine at rate_hz(r) goes round in 1 / f seconds
  lfo::reset();
  report(0x03, 0, 0x00, p);
  set(1, "tgt:v1"), set(1, "par:2"), set(1, "wav:0"), set(1, "dep:32");
  int r = 0;
  while (lfo::rate_hz(r) < 2.0f) ++r;   // ~2 Hz
  char rr[16];
  snprintf(rr, sizeof rr, "rate:%d", r);
  set(1, rr);
  run_ms(1);
  float ph0 = lfo::phase(0);
  run_ms(100);
  float want = fmodf(ph0 + lfo::rate_hz(r) * 0.1f, 1.0f);
  CHECK(near(lfo::phase(0), want, 0.01f));
  CHECK(near(v[0].p[2], 64 + 32 * lfo::out(0), 0.05f));
  // retrig on the voice's trig (0x04): the phase back to 0
  set(1, "trig:1");
  run_ms(130);
  CHECK(lfo::phase(0) > 0.1f);
  report(0x04, 0, 0x00, p);
  run_ms(1);
  CHECK(lfo::phase(0) < 0.01f);
  report(0x04, 1, 0x00, p);   // another voice's trig: not this one's
  run_ms(50);
  CHECK(lfo::phase(0) > 0.05f);

  // ---- sync to the MIDI clock: 1/4 = 24 ticks, 120 BPM (20833 us a tick), then 60 BPM; stop and start
  lfo::reset();
  set(1, "tgt:v1"), set(1, "par:2"), set(1, "wav:2"), set(1, "sync:1"), set(1, "rate:8"), set(1, "dep:32");
  CHECK(lfo::get(0).sync && lfo::div_ticks(lfo::get(0).rate) == 24);
  fx::clock_in(0xFA);
  auto ticks = [&](int n, uint64_t tick_us) {
    for (int i = 0; i < n; ++i) {
      fx::clock_in(0xF8);
      for (uint64_t t = 0; t < tick_us; t += 250) at(g_us + 250), lfo::tick(now_us());
    }
  };
  ticks(12, 20833);   // ticks 0..11 seen, the 12th's time reached: half a beat
  CHECK(near(lfo::phase(0), 0.5f, 0.03f));
  ticks(12, 20833);
  CHECK(lfo::phase(0) < 0.03f || lfo::phase(0) > 0.97f);
  ticks(6, 41667);    // 60 BPM now: the phase follows the ticks, a quarter beat
  CHECK(near(lfo::phase(0), 0.25f, 0.04f));
  ticks(6, 41667);
  CHECK(near(lfo::phase(0), 0.5f, 0.03f));
  fx::clock_in(0xFC);   // stop: on at the last tempo (60 BPM: 1 s a beat)
  float before = lfo::phase(0);
  run_ms(500);
  CHECK(near_ph(lfo::phase(0), fmodf(before + 0.5f, 1.0f), 0.05f));
  fx::clock_in(0xFA);   // start: the song position from 0
  ticks(6, 41667);
  CHECK(near(lfo::phase(0), 0.25f, 0.04f));
  fx::clock_in(0xF2, 1);   // SPP 1/16 = 6 ticks (then the next tick is 6)
  ticks(1, 41667);
  CHECK(near(lfo::phase(0), 7 / 24.0f, 0.04f));
  // PLAY retrig, free rate: Start puts the phase back to 0
  set(1, "sync:0"), set(1, "trig:2");
  run_ms(200);
  CHECK(lfo::phase(0) > 0.05f);
  fx::clock_in(0xFA);
  run_ms(1);
  CHECK(lfo::phase(0) < 0.01f);

  // ---- FX1's P3: the knob around the kit's value, back to it when off
  lfo::reset();
  {
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x20, 0x01, 12, 0, 0, 1, 0, 30, 90, 0, 0, 0, 0, 0, 0, 0xF7};   // FX1: TYPE 1 (CHORUS), P2 30 P3 90
    CHECK(fx::take_sendfx(m, (int)sizeof m));
    CHECK(fx::control.knob[0][0] == 1 && fx::control.knob[0][3] == 90);
    set(1, "tgt:fx1"), set(1, "par:1"), set(1, "wav:4"), set(1, "rate:0"), set(1, "dep:20");
    run_ms(5);
    CHECK(fx::control.knob[0][3] == 110 && fx::control.knob[0][2] == 30);
    CHECK(fx::take_sendfx(m, (int)sizeof m) && fx::control.knob[0][3] == 110);   // the kit again: still modulated
    char pn[8];
    CHECK(!strcmp(lfo::param_name(lfo::kTFx, 1, pn, sizeof pn), fx::labels(1)->label[1]));
    set(1, "off");
    run_ms(5);
    CHECK(fx::control.knob[0][3] == 90);
  }

  // ---- no Rytm targets (1026): BD..CB / FX are not targets (nothing sent to the Rytm); a Rytm track's note retrigs an
  // FX target set to TRIG
  lfo::reset();
  g_out.clear();
  {
    char o[128];
    CHECK(lfo::command("lfo=1:tgt:bd", o, sizeof o) && strstr(o, "cannot") && lfo::get(0).target == 0);
    CHECK(lfo::target_from_name("BD") < 0 && lfo::target_from_name("FX") < 0 && lfo::kTargets == lfo::kTFx + 2 + 11 + 8);   // fx-ins: I1..I8 B1..B3; O1..O8 (the OUT route)
  }
  set(1, "tgt:fx1"), set(1, "par:0"), set(1, "rate:100"), set(1, "trig:1");
  run_ms(150);
  CHECK(lfo::phase(0) > 0.05f);
  lfo::midi_in(0x90, 0, 100);
  run_ms(1);
  CHECK(lfo::phase(0) < 0.02f);
  for (auto& m : g_out) CHECK(!(m.size() == 3 && (m[0] & 0xF0) == 0xB0));   // never a CC

  // ---- the console, the design hex, the JSON, the per-project file (saved a second after the change, reloaded)
  char o[2048];
  set(2, "tgt:v3"), set(2, "par:4"), set(2, "wav:7"), set(2, "pre:steps"), set(2, "st:5:-20:2"), set(2, "dep:-12");
  CHECK(lfo::get(1).step[5] == -20 && lfo::get(1).shape[5] == lfo::kSmooth && lfo::get(1).wave == lfo::kDesign);
  CHECK(lfo::command("lfo=2:des:000111222333444555666777888999aaabbbcccdddeeefff", o, sizeof o) && !strstr(o, "cannot"));
  CHECK(lfo::get(1).step[0] == -64 && lfo::get(1).shape[0] == 0 && lfo::get(1).step[1] == 17 - 64 &&
        lfo::get(1).shape[1] == 1 && lfo::get(1).step[15] == 63);
  CHECK(lfo::command("lfo=9:dep:1", o, sizeof o) && strstr(o, "lfo=N"));
  CHECK(lfo::command("lfo=1:wat:1", o, sizeof o) && strstr(o, "cannot"));
  CHECK(!lfo::command("lfox", o, sizeof o));
  int n = lfo::json(o, sizeof o);
  CHECK(n > 0 && n < (int)sizeof o && strstr(o, "\"tgt\":\"V3\"") && strstr(o, "\"tgt\":\"FX1\""));
  printf("lfo: json %d B for %d LFOs\n", n, lfo::kLfos);
  CHECK(lfo::command("lfo", o, sizeof o) && strstr(o, "L1 FX1"));
  {
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'L', 'F', 'O', 0xF7};
    engine_sysex(m, (int)sizeof m);   // a project: its (empty) LFOs, ours unsaved -> lost? no: saved first
    loaded();
  }
  CHECK(lfo::get(0).target == 0 && lfo::get(1).target == 0);   // project LFO has none yet
  set(1, "tgt:v2"), set(1, "par:3"), set(1, "dep:7"), set(1, "wav:6");
  set(2, "tgt:fx2"), set(2, "par:2"), set(2, "dep:-9"), set(2, "pre:ramp");
  CHECK(!lfo::save());   // not yet: a second after the last change
  at(g_us + 1100000);
  for (int i = 0; i < 8; ++i) idle_save();
  CHECK(access((std::string(tmpl) + "/.rytm/lfo/LFO.txt").c_str(), 0) == 0);
  lfo::Settings keep0 = lfo::get(0), keep1 = lfo::get(1);
  lfo::reset();
  project_fill_begin();   // the project loaded anew: its file from the card
  loaded();
  CHECK(!memcmp(&lfo::get(0), &keep0, sizeof keep0) && !memcmp(&lfo::get(1), &keep1, sizeof keep1));
  {
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 0xF7};
    engine_sysex(m, (int)sizeof m);
    loaded();
  }
  CHECK(lfo::get(0).target == 0);
  {
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'L', 'F', 'O', 0xF7};
    engine_sysex(m, (int)sizeof m);
    loaded();
  }
  CHECK(!memcmp(&lfo::get(1), &keep1, sizeof keep1) && lfo::get(1).target == lfo::kTFx + 1);   // restored with the project

  // ---- per kit (1026): the Rytm names its active kit (3B, provisional); a kit without its own file starts from the
  // project's (the migration), its edits go to /.rytm/lfo/<project>/kNNN.txt, another kit has its own
  auto kit = [](int k) {
    const uint8_t m[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, (uint8_t)k, 0xF7};
    engine_sysex(m, 8);
  };
  kit(4);
  CHECK(lfo::kit() == 4 && !memcmp(&lfo::get(1), &keep1, sizeof keep1));   // kit 5: the project's file as its default
  set(1, "tgt:v5"), set(1, "dep:-3");
  at(g_us + 1100000);
  for (int i = 0; i < 8; ++i) idle_save();
  CHECK(access((std::string(tmpl) + "/.rytm/lfo/LFO/k005.txt").c_str(), 0) == 0);
  set(3, "tgt:v8");   // unsaved when the kit changes: saved into kit 5's file first
  kit(0);
  CHECK(lfo::kit() == 0 && lfo::get(0).target == keep0.target && lfo::get(2).target == 0);   // kit 1: the project's again
  kit(4);
  CHECK(lfo::get(0).target == lfo::kTVoice + 4 && lfo::get(0).depth == -3 && lfo::get(2).target == lfo::kTVoice + 7);
  {
    char o[2048];
    CHECK(lfo::json(o, sizeof o) > 0 && strstr(o, "\"kit\":5"));
  }
  {
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'O', 'T', 'H', 0xF7};
    engine_sysex(m, (int)sizeof m);   // another project on the same kit: that project's (none)
    loaded();
  }
  CHECK(lfo::kit() == 4 && lfo::get(0).target == 0);

  // ---- 1042 data safety: a kit file with a line this build does not read (a newer Sidecar's target, damage) is never
  // written over: the LFOs change in memory only
  {
    std::string d = std::string(tmpl) + "/.rytm/lfo/BAD";
    CHECK(system(("mkdir -p '" + d + "'").c_str()) == 0);
    const char bad[] = "# Sidecar LFOs\n1 V2 3 SIN 64 0 0 10 0 0\n2 WARP9 1 2 3 4 5 6 7 8 9\n";
    FILE* fp = fopen((d + "/k005.txt").c_str(), "wb");
    fwrite(bad, 1, sizeof bad - 1, fp);
    fclose(fp);
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'B', 'A', 'D', 0xF7};
    engine_sysex(m, (int)sizeof m);   // kit 5 (4) still: BAD/k005.txt
    loaded();
    set(3, "tgt:v4"), set(3, "dep:5");
    at(g_us + 1100000);
    for (int i = 0; i < 8; ++i) idle_save();
    char got[256] = {};
    fp = fopen((d + "/k005.txt").c_str(), "rb");
    size_t n = fp ? fread(got, 1, sizeof got - 1, fp) : 0;
    if (fp) fclose(fp);
    CHECK(n == sizeof bad - 1 && !memcmp(got, bad, n));
    printf("lfo: 1042: an unreadable line locks its file (not written over)\n");
  }

  kit_change_reads_ram_only(tmpl);
  edit_then_switch_back(tmpl);
  unreadable_default_does_not_lock_the_kit(tmpl);

  // ---- the page through the remote host: LEFT x 3 from SIDECAR (SCOPE, then the LFO page: no REC here), its mask
  lfo::reset();
  static int mask_encs = -1;
  static uint32_t mask_trigs = 0;
  static int closes = 0;   // ux1027: 3A 0 0 (the host asks the page closed)
  hooks.to_rytm = [](const uint8_t* d, int len) {
    if (len >= 9 && d[4] == 0x3A && d[7] == 0) ++closes;
    if (len >= 10 && d[4] == 0x37) {
      mask_encs = d[6] | d[7] << 7;
      mask_trigs = 0;
      for (int k = 0; k < 16; ++k)
        if (6 + 3 + k / 7 < len - 1 && (d[9 + k / 7] >> (k % 7) & 1)) mask_trigs |= 1u << k;
    }
  };
  auto rytm = [](uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
    engine_sysex(m, 10);
  };
  uint32_t ms = now_ms();
  auto key = [&](int code) {
    rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 1), rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 0);
    for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  };
  // ux1027 (the user, 2026-10-27): the LFO page is UP + LFO's alone, off the SIDECAR ring (LEFT x 2 from SIDECAR
  // found it before): it replaces the Rytm's screen, as the SRC view does
  rytm(0x30, 0, 1, 0);
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  for (int i = 0; i < 8; ++i) {
    key(0x40);
    CHECK(!(ui::remote_stats().page == 3 && ui::remote_stats().voice == 0));
  }
  rytm(0x30, 0, 0, 3);
  rytm(0x30, 1, 1, 0);   // UP + LFO
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(ui::remote_stats().page == 3 && ui::remote_stats().voice == 0 && mask_encs == 0xFF && mask_trigs == 0);
  rytm(0x32, 1, 64 + 1, 0);    // B +1: V1
  rytm(0x32, 3, 64 + 4, 0);    // D +4: depth 36
  rytm(0x32, 4, 64 + 7, 0);    // E +7: DESIGN
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(lfo::get(0).target == lfo::kTVoice && lfo::get(0).depth == 36 && lfo::get(0).wave == lfo::kDesign);
  key(0x44);   // YES: the design view takes T1..T16 and A..D
  CHECK(mask_encs == 0x0F && mask_trigs == 0xFFFF);
  key(0x04);   // T5: step 5
  rytm(0x32, 1, 64 - 10, 0);   // B -10: its value
  rytm(0x32, 2, 64 + 1, 0);    // C +1: its shape
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  lfo::Settings d0;
  lfo::defaults(&d0);
  CHECK(lfo::get(0).step[4] == d0.step[4] - 10 && lfo::get(0).shape[4] == (d0.shape[4] + 1 > 2 ? 2 : d0.shape[4] + 1));
  key(0x45);   // NO: the main view (not SIDECAR)
  CHECK(ui::remote_stats().page == 3 && mask_encs == 0xFF && mask_trigs == 0);
  closes = 0;
  key(0x45);   // NO again: back where UP + LFO came from - the stock screen (the page asked closed)
  CHECK(closes == 1);
  rytm(0x30, 0, 0, 3);
  // TRK + LFO (rui1026): 30's page byte 1 opens on the LFO page; ux1027: LEFT / RIGHT the LFO before / after (as A),
  // never another page; UP + LFO again on it leaves (as NO), as UP + TRIG leaves the SEQ page
  rytm(0x30, 1, 1, 0);
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(ui::remote_open() && ui::remote_stats().page == 3 && ui::remote_stats().voice == 0 && mask_encs == 0xFF);
  for (int i = 0; i < 6; ++i) key(0x40);   // LEFT: LFO 1 (the first: it stays)
  key(0x41);   // RIGHT: LFO 2, still the LFO page
  CHECK(ui::remote_stats().page == 3 && ui::remote_stats().voice == 0);
  {
    int t0 = lfo::get(1).target;
    rytm(0x32, 1, 64 + 2, 0);   // B +2: LFO 2's target
    for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
    CHECK(lfo::get(1).target != t0 && lfo::get(0).target == lfo::kTVoice);
  }
  key(0x40);   // LEFT: LFO 1 again
  key(0x44), key(0x41), key(0x41);   // the design view: RIGHT the next step (as A), the page stays
  CHECK(ui::remote_stats().page == 3 && mask_trigs == 0xFFFF);
  closes = 0;
  rytm(0x30, 1, 1, 0);   // UP + LFO again on it (in the design view): leaves (the stock screen it came from)
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(closes == 1);
  rytm(0x30, 0, 0, 3);
  rytm(0x30, 1, 1, 0);   // review 5: back on the main view (the design view dropped with the leave), T1..T16 free
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(ui::remote_stats().page == 3 && mask_encs == 0xFF && mask_trigs == 0);
  closes = 0;
  rytm(0x30, 1, 1, 0);
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(closes == 1);
  rytm(0x30, 0, 0, 3);
  // review 3: UP + LFO, then another page (SCOPE), then LFO again another way (the MENU's row): UP + LFO now goes to
  // the LFO page (it is there), it does not leave - the page byte was forgotten at the page change
  rytm(0x30, 1, 1, 0);
  ui::remote_show_page("SCOPE");
  ui::remote_show_page("LFO");
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  closes = 0;
  rytm(0x30, 1, 1, 0);
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(closes == 0 && ui::remote_stats().page == 3 && ui::remote_stats().voice == 0);
  rytm(0x30, 1, 1, 0);   // again: leaves (back to SCOPE, where the LFO page was opened over)
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "SCOPE"));
  rytm(0x30, 0, 0, 3), rytm(0x30, 0, 1, 0);   // TRK + YES: SIDECAR
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(ui::remote_stats().page == 0);
  rytm(0x30, 1, 1, 0);   // UP + LFO over SIDECAR, again: back to SIDECAR (where it came from), not closed
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(ui::remote_stats().page == 3);
  closes = 0;
  rytm(0x30, 1, 1, 0);
  for (int i = 0; i < 10; ++i) ui::remote_tick(ms += 10);
  CHECK(ui::remote_stats().page == 0 && closes == 0);
  printf("test_lfo: PASS\n");
  return 0;
}
