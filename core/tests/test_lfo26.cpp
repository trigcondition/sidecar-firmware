// core/tests/test_lfo26.cpp - 0.5.0's LFO FINAL (the owner, 2026-11-12; notes rytm-param-mod.md): 26 LFOs on the Pi,
// two a track (T1..T12 and the FX track: A and B). The count and the names (console, JSON, file); A may target
// anything, B the Sidecar's targets only - never the Rytm (set, the console, the page's TGT list, a file's line); an old
// per-kit file's LFO i becomes track i's A with its target; the Rytm-screen page shows the selected track's pair (the
// Rytm's 49 1 2), FUNC + A steps the track (the FX pair with a 0026 that reports sound tracks only), and the FX track
// itself once a 0026 names it (49 1 2 12). 0.5.0 owner test (2026-11-13): an LFO on a Rytm param made the whole Rytm
// slow, so no LFO, A or B, targets the Rytm any more (RT1..RT12, RFX): refused everywhere, a file's line loads OFF.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp, access

#include "check.h"
#include "timing.h"
#include "core.h"
#include "machines.h"
#include "mod/mod.h"
#include "store/store.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;

static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static std::string g_card;
static void loaded() {
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
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
static void wr_file(const std::string& p, const std::string& text) {
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
}
static void to_project(const char* name) {   // 0x10: the Rytm's project; its files into the store, then its LFOs
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x10};
  for (const char* c = name; *c; ++c) m.push_back((uint8_t)*c);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
  loaded();
}
static void kit(int k) {
  const uint8_t m[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, (uint8_t)k, 0xF7};
  engine_sysex(m, 8);
}
static void saved() {   // a second after the last change: idle_save writes it
  at(g_us + 1100000);
  for (int i = 0; i < 8; ++i) idle_save();
}
static bool cmd(const char* c, std::string* out = nullptr) {
  char o[4096];
  bool ok = lfo::command(c, o, sizeof o) && !strstr(o, "cannot") && !strstr(o, "lfo=N");
  if (out) *out = o;
  return ok;
}
static int target(const char* n) { return lfo::target_from_name(n); }

// ---- the console's listing: all 26 in the Pi's 1400 B buffer (cores.cpp); a smaller buffer is cut with "...", not silently
static void console_listing() {
  lfo::reset();
  for (int i = 0; i < lfo::kLfos; ++i) {
    char c[40];
    snprintf(c, sizeof c, "lfo=%d:tgt:%s", i + 1, lfo::slot_of(i) ? "v2" : "i1");
    CHECK(cmd(c));
    snprintf(c, sizeof c, "lfo=%d:dep:-64", i + 1);
    CHECK(cmd(c));
  }
  char big[1400];
  CHECK(lfo::command("lfo", big, sizeof big));
  printf("lfo26: the console listing of 26 LFOs: %d of 1400 bytes\n", (int)strlen(big));
  CHECK(strstr(big, "FXA I1") && strstr(big, ", FXB V2") && strstr(big, "T12B V2") && !strstr(big, "..."));
  char small[200];
  CHECK(lfo::command("lfo", small, sizeof small));
  CHECK(strlen(small) < sizeof small && strlen(small) > 100 && !strcmp(small + strlen(small) - 3, "..."));
  lfo::reset();
}

// ---- the count and the names
static void count_and_names() {
  lfo::reset();
  static_assert(lfo::kLfos == 26 && lfo::kPerTrack && lfo::kTracks == 13, "the Pi: 26 LFOs, two a track");
  char b[8];
  CHECK(!strcmp(lfo::lfo_name(0, b, sizeof b), "T1A") && !strcmp(lfo::lfo_name(1, b, sizeof b), "T1B"));
  CHECK(!strcmp(lfo::lfo_name(23, b, sizeof b), "T12B") && !strcmp(lfo::lfo_name(24, b, sizeof b), "FXA") &&
        !strcmp(lfo::lfo_name(25, b, sizeof b), "FXB"));
  for (int i = 0; i < lfo::kLfos; ++i) {
    CHECK(lfo::lfo_of(lfo::track_of(i), lfo::slot_of(i)) == i);
    CHECK(lfo::lfo_index(lfo::lfo_name(i, b, sizeof b)) == i);
  }
  CHECK(lfo::lfo_index("t12b") == 23 && lfo::lfo_index("fxa") == 24 && lfo::lfo_index("26") == 25);
  CHECK(lfo::lfo_index("27") < 0 && lfo::lfo_index("T13A") < 0 && lfo::lfo_index("T0A") < 0 && lfo::lfo_index("T3C") < 0);
  CHECK(cmd("lfo=26:dep:5") && lfo::get(25).depth == 5);
  std::string o;
  CHECK(cmd("lfo=FXB:dep:-7", &o) && lfo::get(25).depth == -7 && o.find("lfo: FXB OFF") == 0);
  CHECK(cmd("lfo=t3a:tgt:v2") && lfo::get(4).target == lfo::kTVoice + 1);
  CHECK(!cmd("lfo=27:dep:1", &o) && o.find("lfo=N") != std::string::npos && o.find("T1A..T12B FXA FXB") != std::string::npos);
  CHECK(!cmd("lfo=T13A:dep:1") && !cmd("lfo=FXC:dep:1"));
  CHECK(cmd("lfo", &o) && o.find("lfo: T1A OFF") == 0 && o.find("T3A V2") != std::string::npos);
  static char j[lfo::kJsonCap];
  int n = lfo::json(j, sizeof j);
  CHECK(n > 0 && n < (int)sizeof j && strstr(j, "\"lfo\":{\"n\":26,\"pt\":1,") && strstr(j, "\"full\":0"));
  int cards = 0;
  for (const char* p = j; (p = strstr(p, "{\"tgt\":")); ++p) ++cards;
  CHECK(cards == 26);
  printf("lfo26: 26 LFOs, T1A..T12B FXA FXB on the console, the JSON (%d B) and the file\n", n);
}

// ---- no LFO on the Rytm (0.5.0 owner test): A and B the Sidecar's targets only
static void a_and_b_targets() {
  lfo::reset();
  lfo::Settings s;
  lfo::defaults(&s);
  s.target = (uint8_t)target("RT1"), s.param = 0;
  CHECK(!lfo::set(0, s) && lfo::get(0).target == 0);                // T1A: refused
  CHECK(!lfo::set(1, s) && lfo::get(1).target == 0);                // T1B: refused
  s.target = (uint8_t)target("RFX");
  CHECK(!lfo::set(24, s) && !lfo::set(25, s) && lfo::get(24).target == 0 && lfo::get(25).target == 0);
  for (int i = 0; i < lfo::kLfos; ++i) {
    for (int t = 0; t < lfo::kTargets; ++t) {
      bool rytm = t >= lfo::kTRt && t <= lfo::kTRfx;
      CHECK(lfo::target_ok(i, t) == !rytm);
    }
    CHECK(lfo::target_max(i) == lfo::kTRt - 1);
  }
  // the Sidecar targets: voices, inserts, the master, the send FX, the sends, the OUT routes - A and B alike
  for (const char* t : {"V1", "FX2", "I3", "B2", "O4", "M1", "MFX", "S7", "X8"}) {
    char c[32];
    snprintf(c, sizeof c, "lfo=T5B:tgt:%s", t);
    CHECK(cmd(c) && lfo::get(9).target == target(t));
    snprintf(c, sizeof c, "lfo=T5A:tgt:%s", t);
    CHECK(cmd(c) && lfo::get(8).target == target(t));
  }
  std::string o;
  CHECK(!cmd("lfo=T5B:tgt:rt5", &o) && o.find("cannot") != std::string::npos && lfo::get(9).target == target("X8"));
  CHECK(!cmd("lfo=T5A:tgt:rt5", &o) && o.find("cannot") != std::string::npos && lfo::get(8).target == target("X8"));
  CHECK(!cmd("lfo=2:tgt:rfx") && !cmd("lfo=FXB:tgt:65") && !cmd("lfo=FXA:tgt:rfx") && lfo::get(25).target == 0 &&
        lfo::get(24).target == 0);
  // the TGT lists: A's the same as B's, no RT / RFX entry
  CHECK(lfo::target_count() == lfo::kTRt);
  CHECK(lfo::target_pos(lfo::kTRt) < 0 && lfo::target_pos(lfo::kTRfx) < 0);
  printf("lfo26: no LFO on the Rytm: A and B refuse RT / RFX (set, console, target lists)\n");
}

// ---- a file's line on the Rytm (A or B): it loads OFF, the file is not locked (the next save drops it)
static void b_line_on_the_rytm() {
  const std::string f = g_card + "/.rytm/lfo/BRT/k001.txt";
  const std::string text = "# Sidecar LFOs\nT1A RT1 30 0 64 0 0 20 64\nT1B RT2 30 0 64 0 0 20 64\nT2A V2 3 0 64 0 0 9 64\n";
  wr_file(f, text);
  to_project("BRT");
  kit(0);
  CHECK(lfo::kit() == 0 && lfo::get(2).target == lfo::kTVoice + 1);
  CHECK(lfo::get(0).target == 0 && lfo::get(1).target == 0);   // the A and the B line load OFF
  CHECK(cmd("lfo=T3A:dep:4"));
  saved();
  // not locked: the edit is saved, A and B are OFF in the file, the others kept
  const std::string now = rd_file(f);
  CHECK(now != text && now.find("T3A OFF 0 0 64 0 0 4 64") != std::string::npos && now.find("T1B OFF") != std::string::npos);
  CHECK(now.find("T1B RT") == std::string::npos && now.find("T1A RT") == std::string::npos &&
        now.find("T1A OFF") != std::string::npos && now.find("T2A V2 3 0 64 0 0 9 64") != std::string::npos);
  to_project("BRT");
  kit(0);
  CHECK(lfo::get(0).target == 0 && lfo::get(1).target == 0 && lfo::get(4).depth == 4);
  printf("lfo26: a file's A or B line on the Rytm loads OFF, the file is saved as usual\n");
}

// ---- an old per-kit file: LFO i -> track i's A, its target kept; written back in the new names
static void old_file_migrates() {
  const std::string f = g_card + "/.rytm/lfo/MIG/k001.txt";
  wr_file(f, "# Sidecar LFOs: N TARGET PARAM WAVE RATE SYNC RETRIG DEPTH BASE DESIGN\n"
             "1 V3 2 1 64 0 0 10 64\n"
             "2 FX1 1 4 7 1 2 -12 64\n"
             "5 I2 9 0 30 0 0 22 64\n"
             "8 RT8 30 0 100 0 1 -20 64\n");
  to_project("MIG");
  kit(0);
  CHECK(lfo::kit() == 0);
  CHECK(lfo::get(0).target == lfo::kTVoice + 2 && lfo::get(0).param == 2 && lfo::get(0).depth == 10);   // T1A
  CHECK(lfo::get(2).target == lfo::kTFx && lfo::get(2).wave == 4 && lfo::get(2).sync == 1 && lfo::get(2).retrig == 2 &&
        lfo::get(2).depth == -12);                                                                       // T2A
  CHECK(lfo::get(8).target == target("I2") && lfo::get(8).param == 9 && lfo::get(8).depth == 22);      // T5A
  CHECK(lfo::get(14).target == 0);   // T8A: a Rytm target loads OFF (0.5.0 owner test: no LFO on the Rytm)
  for (int i = 0; i < lfo::kLfos; ++i)
    if (lfo::slot_of(i) == 1 || (i != 0 && i != 2 && i != 8)) CHECK(lfo::get(i).target == 0);   // the rest OFF
  lfo::Settings keep[lfo::kLfos];
  for (int i = 0; i < lfo::kLfos; ++i) keep[i] = lfo::get(i);
  // an edit writes the file in the new names (an older build reads none of its lines: it never writes over it)
  CHECK(cmd("lfo=FXB:tgt:mfx") && cmd("lfo=FXB:dep:3"));
  keep[25] = lfo::get(25);
  saved();
  std::string t = rd_file(f);
  CHECK(t.find("\nT1A V3 2 1 64 0 0 10 64 ") != std::string::npos && t.find("\nT2A FX1 1 4 7 1 2 -12 ") != std::string::npos);
  CHECK(t.find("\nT8A OFF ") != std::string::npos && t.find("RT8") == std::string::npos && t.find("\nFXB MFX 1 ") != std::string::npos);
  CHECK(t.find("\n1 ") == std::string::npos && t.find("\nT12B ") != std::string::npos);
  int lines = 0;
  for (char c : t) lines += c == '\n';
  CHECK(lines == 1 + 26);
  lfo::reset();
  project_fill_begin();   // again from the card: the same 26
  loaded();
  kit(0);
  for (int i = 0; i < lfo::kLfos; ++i) CHECK(!memcmp(&lfo::get(i), &keep[i], sizeof keep[i]));
  printf("lfo26: an old file's LFO i is track i's A (targets kept), saved back as T1A..FXB\n");
}

// ---- the Rytm-screen page: the selected track's pair
static int g_masks_encs = -1;
static std::string g_texts;
static void capture(const uint8_t* d, int len) {
  if (len > 13 && d[4] == 0x3D) g_texts += std::string((const char*)d + 12, (size_t)(len - 13)) + "|";
  if (len >= 10 && d[4] == 0x37) g_masks_encs = d[6] | d[7] << 7;
}
static void rx(std::vector<uint8_t> body) {
  std::vector<uint8_t> f = {0xF0, 0x7D, 0x52, 0x54};
  f.insert(f.end(), body.begin(), body.end());
  f.push_back(0xF7);
  engine_sysex(f.data(), (int)f.size());
}
static uint32_t g_ms = 0;
static void ticks() {
  for (int i = 0; i < 10; ++i) ui::remote_tick(g_ms += 10);
}
static void key(int code) { rx({0x31, 1, (uint8_t)(code >> 7), (uint8_t)(code & 0x7F), 1}), rx({0x31, 1, (uint8_t)(code >> 7), (uint8_t)(code & 0x7F), 0}), ticks(); }
static void enc(int e, int d, bool func = false) {   // 32 <enc> <64 + d> <FUNC held>
  rx({0x32, 1, (uint8_t)e, (uint8_t)(64 + d), (uint8_t)(func ? 1 : 0)});
  ticks();
}
static void select_track(int t) {   // 49 1 2 <track> <machine> <name 12 B>: the Rytm's selected track
  std::vector<uint8_t> b = {0x49, 1, 2, (uint8_t)t, (uint8_t)(t == 12 ? 0x7F : 0)};
  const char* nm = t == 12 ? "FX" : "BD HARD";
  for (int i = 0; i < 12; ++i) b.push_back((uint8_t)(i < (int)strlen(nm) ? nm[i] : 0));
  rx(b);
  ticks();
}
static bool shown(const char* s) { return g_texts.find(std::string("|") + s + "|") != std::string::npos; }
static void page_pair() {
  lfo::reset();
  hooks.to_rytm = capture;
  g_ms = now_ms();
  g_texts = "|";
  rx({0x30, 1, 1, 1, 0});   // UP + LFO (0026: 30 <open> <page 1> 0)
  ticks();
  CHECK(ui::remote_stats().page == 3 && g_masks_encs == 0xFF);
  // no track named yet: T1's pair; A walks A <-> B only
  CHECK(shown("LFO T1 A") && shown("T1 A"));
  g_texts = "|";
  enc(0, 1);
  CHECK(shown("LFO T1 B") && shown("T1 B"));
  enc(0, 5);   // stays on B
  CHECK(lfo::track_of(0) == 0);
  for (int i = 0; i < 20; ++i) enc(1, 30);   // B's TGT: the list ends before RT1
  CHECK(lfo::get(1).target == lfo::kTRt - 1);
  enc(0, -1);
  for (int i = 0; i < 20; ++i) enc(1, 30);   // A's: the same end (no Rytm target, 0.5.0 owner test)
  CHECK(lfo::get(0).target == lfo::kTRt - 1);
  // the Rytm selects track 7: its pair, the same slot (A)
  g_texts = "|";
  select_track(6);
  CHECK(shown("LFO T7 A"));
  key(0x41);   // RIGHT: B
  enc(3, 4);   // D: depth of T7 B
  CHECK(lfo::get(13).depth == 32 + 4 && lfo::get(12).depth == 32);
  g_texts = "|";
  enc(3, 1);
  CHECK(shown("T7 B DEPTH:+37"));
  key(0x40);   // LEFT: A
  // FUNC + A: the track the page shows (the FX pair with today's 0026, which never names track 12)
  g_texts = "|";
  for (int i = 0; i < 6; ++i) enc(0, 1, true);
  CHECK(shown("LFO FX A"));
  enc(4, 1);   // E: FX A's wave
  CHECK(lfo::get(24).wave == 1);
  // the Rytm selects another track: the page follows it again
  g_texts = "|";
  select_track(2);
  CHECK(shown("LFO T3 A"));
  // a 0026 that names the FX track (the proposed 49 1 2 12): the page shows FX's pair; a sound track again after
  g_texts = "|";
  select_track(12);
  CHECK(shown("LFO FX A") && ui::remote_lfo_track(nullptr) == 12);
  uint32_t g0 = 0;
  CHECK(ui::remote_selected_track(&g0) == 2);   // (every other reader keeps the sound track)
  g_texts = "|";
  select_track(4);
  CHECK(shown("LFO T5 A"));
  // the design view's title names the LFO
  g_texts = "|";
  key(0x44);
  CHECK(shown("T5 A DESIGN"));
  key(0x45);
  printf("lfo26: the page shows the selected track's pair (A / B), FUNC + A the track, FX by 49 1 2 12\n");
}

// ---- the owner's test (2026-11-13): LFO A on BD playing SAMPLER (machine 43: page 1 SMP STRT END PLAY / TUNE FINE LOOP
// LEV on P9..P16, P1..P8 unused) listed empty "P1 -".."P8 -" rows, then (the Rytm part) P1..P8 again. The PAR list now
// holds the params the machine names, each once (A's and B's: one list)
static void sampler_par_list() {
  lfo::reset();
  rx({0x33, 1, 0, 1, (uint8_t)kRytmSampler, (uint8_t)kDspSample});   // BD on SAMPLER: DSP SAMPLE's flag, machine 43
  const int v = ui::remote_track_voice(0);
  CHECK(ui::remote_voice_machine(v) == kSmplMachine);
  {   // (one list for A and B since the owner's test)
    lfo::Settings s;
    lfo::defaults(&s);
    CHECK(lfo::target_step(&s, 1) && s.target == lfo::kTVoice + v && !strcmp(lfo::target_label(s.target), "BD"));
    char nm[8];
    CHECK(strcmp(lfo::param_name(s.target, s.param, nm, sizeof nm), "-"));   // TGT lands on a named param
    for (int k = 0; k < 20; ++k) lfo::param_step(&s, -1);
    CHECK(lfo::param_index(s) == 0);
    const int n = lfo::param_count(s);
    std::vector<std::string> names;
    for (int k = 0; k < n; ++k) {
      CHECK(s.target == lfo::kTVoice + v && lfo::param_index(s) == k);
      lfo::param_name(s.target, s.param, nm, sizeof nm);
      CHECK(nm[0] && strcmp(nm, "-"));                         // no empty / generic row
      for (const std::string& x : names) CHECK(x != nm);       // no row twice
      names.push_back(nm);
      lfo::param_step(&s, 1);
    }
    std::string all;
    for (const std::string& x : names) all += x + " ";
    printf("lfo26: SAMPLER's PAR list: %s\n", all.c_str());
    CHECK(n == 8);
  }
  // a stored unnamed param (an older file's) stays the entry it is until PAR leaves it
  lfo::Settings u;
  lfo::defaults(&u);
  u.target = (uint8_t)(lfo::kTVoice + v), u.param = 2;   // P3: unused on SAMPLER
  CHECK(lfo::param_count(u) == 9 && lfo::param_index(u) == 0);
  lfo::param_step(&u, 1);
  CHECK(u.param == 8 && lfo::param_count(u) == 8 && lfo::param_index(u) == 0);   // P9 TUNE, the first named
  // fix round 1 (review M1): the web / console `tgt` lands where the page's TGT does - SAMPLER's first named, not P2
  {
    lfo::reset();
    char c[32], nm[8];
    snprintf(c, sizeof c, "lfo=T1A:tgt:V%d", v + 1);
    CHECK(cmd(c) && lfo::get(0).target == lfo::kTVoice + v);
    lfo::param_name(lfo::get(0).target, lfo::get(0).param, nm, sizeof nm);
    printf("lfo26: tgt:V%d on SAMPLER lands on P%d %s\n", v + 1, lfo::get(0).param + 1, nm);
    CHECK(!strcmp(nm, "TUNE") && lfo::get(0).param == 8);
    lfo::Settings s;
    lfo::defaults(&s);
    CHECK(lfo::target_step(&s, 1) && s.target == lfo::get(0).target && s.param == lfo::get(0).param);   // = the page's
    lfo::reset();
  }
  rx({0x33, 1, 0, 1, 0, 0x7F});   // (BD back on a Rytm machine)
  printf("lfo26: a SAMPLER voice's PAR list: its 8 named params, no empty row, none twice\n");
}

// ---- the cost: every LFO on a target at 20 Hz, the control loop's mod::tick (once a millisecond: the waves, the layer's
// sums and writes, the voices' parameter calls) timed on this Mac; 8 LFOs (0.4.x's count) against 26
static void cost(int n, double* mean, double* worst, double* waves_mean) {
  lfo::reset();
  static const char* const kTgt[26] = {"V1", "I1", "V2", "I2", "V3", "I3", "V4", "I4", "V5", "I5", "V6", "I6", "V7", "I7",
                                       "V8", "I8", "O1", "S9", "O2", "S10", "O3", "S11", "O4", "S12", "FX1", "MFX"};
  for (int i = 0; i < n; ++i) {
    lfo::Settings s;
    lfo::defaults(&s);
    s.target = (uint8_t)target(kTgt[i]), s.param = 2, s.wave = (uint8_t)(i % 7), s.rate = 127, s.depth = 40;
    CHECK(lfo::set(i, s));
  }
  std::vector<double> d;
  double wsum = 0;
  int wn = 0;
  for (int k = 0; k < 6000; ++k) {
    at(g_us + 250);
    auto t0 = std::chrono::steady_clock::now();
    mod::tick((uint32_t)now_us());
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (k % 4 == 0 && k > 400) d.push_back(us);   // the passes that ran the millisecond's work
    if (k % 40 == 0) {   // the LFOs' own share: their waves alone
      auto w0 = std::chrono::steady_clock::now();
      lfo::waves((uint32_t)now_us(), 1000);
      wsum += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count(), ++wn;
    }
  }
  std::sort(d.begin(), d.end());
  double sum = 0;
  for (double x : d) sum += x;
  *mean = sum / (double)d.size(), *worst = d[d.size() * 999 / 1000], *waves_mean = wsum / wn;
  lfo::reset();
}
static void cpu_cost() {
  double m8, w8, v8, m26, w26, v26;
  cost(8, &m8, &w8, &v8);
  cost(26, &m26, &w26, &v26);
  printf("lfo26: CPU on this Mac, mod::tick a millisecond: 8 LFOs mean %.2f us (p99.9 %.2f, waves %.2f), 26 LFOs mean %.2f us "
         "(p99.9 %.2f, waves %.2f): %.3f %% / %.3f %% of a core\n", m8, w8, v8, m26, w26, v26, m8 / 10.0, m26 / 10.0);
  CHECK(timing_ok(m26 < 200));   // (a loose guard on a loaded Mac: the number is the report's)
}

int main() {
  char tmpl[] = "/tmp/core_lfo26_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(tmpl);
  at(g_us);
  engine_begin();
  loaded();
  count_and_names();
  console_listing();
  a_and_b_targets();
  b_line_on_the_rytm();
  old_file_migrates();
  page_pair();
  sampler_par_list();
  cpu_cost();
  printf("test_lfo26: PASS\n");
  return 0;
}
