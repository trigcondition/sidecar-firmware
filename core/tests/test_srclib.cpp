// core/tests/test_srclib.cpp - the source BROWSER (core/lib/srclib.h) and the machines on their POOL lists
// (core/lib/pools.h): a folder of 600 files listed lazily in small steps (sorted, folders first, nothing opened or
// loaded), the virtual folders @INPUTS/ and @BUILT-IN/ (GRANULAR, WAVETABLE; none for MULTISAMPLE), the codes (bank *
// 128 + row) meaning the same row on every machine: GRANULAR on C002 loads and plays it, OFF / empty / missing rows
// silent, a folder filled into a bank, MULTISAMPLE's B001 and WAVETABLE's D009, GRANULAR and DSP SAMPLE on the same
// bank A row, an input on DSP SAMPLE's reach silent there (an empty 14 slot).
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "timing.h"
#include "granular/granular.h"
#include "lib/srclib.h"
#include "lib/pools.h"
#include "pool_util.h"
#include "pool/pcache.h"
#include "lib/library.h"
#include "msamp/msamp.h"
#include "nmach.h"
#include "pool/pool.h"
#include "voices.h"
#include "wavetable/tables.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
static bool lib_step() {   // the card core's (the Pi's core 0): its card time is not core 3's
  host_card_core(0);
  bool d = lib::step();
  host_card_core(3);
  return d;
}
static void idle(int n = 20000) {
  for (int i = 0; i < n; ++i) {
    bool a = nm::step();
    bool b = msamp::step();
    bool c = lib_step();
    if (!a && !b && !c) break;
  }
}
static void tick(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    g_us += 10000;
    host_fake_time(true, g_us);
    idle();
  }
}
static std::string slurp(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static auto tone(double hz) {
  return [hz](int i, int) { return 0.4 * std::sin(2 * M_PI * hz * i / 48000); };
}
// P1..P16 for GRANULAR (SRC first, a plain cloud)
static std::vector<float> P(float src) { return {src, 0, 0, 64, 64, 0, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0}; }
static int find_name(int (*n)(), const char* (*name)(int), const char* want, int from = 0) {
  for (int e = from; e < n(); ++e)
    if (!strcmp(name(e), want)) return e;
  return -1;
}
static int gr_find(const char* want) { return find_name(gr::entries, gr::entry_name, want); }


// An AIFF (big-endian 16-bit) of n frames
static std::vector<uint8_t> aiff16(int n, int rate, double hz) {
  std::vector<uint8_t> b;
  auto be32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); };
  auto be16 = [&](uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); };
  b.insert(b.end(), {'F', 'O', 'R', 'M', 0, 0, 0, 0, 'A', 'I', 'F', 'F', 'C', 'O', 'M', 'M'});
  be32(18), be16(1), be32((uint32_t)n), be16(16);
  // 80-bit extended rate
  int e = 0;
  uint32_t m = (uint32_t)rate;
  while (!(m & 0x80000000u)) m <<= 1, ++e;
  be16((uint16_t)(16383 + 31 - e)), be32(m), be32(0);
  b.insert(b.end(), {'S', 'S', 'N', 'D'});
  be32((uint32_t)(8 + 2 * n)), be32(0), be32(0);
  for (int i = 0; i < n; ++i) be16((uint16_t)(int16_t)std::lround(12000 * std::sin(2 * M_PI * hz * i / rate)));
  uint32_t form = (uint32_t)b.size() - 8;
  for (int i = 0; i < 4; ++i) b[4 + i] = (uint8_t)(form >> (8 * (3 - i)));
  return b;
}

static void setup_card() {
  char tmpl[] = "/tmp/srclib-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/Drums/Kicks' '" + g_card + "/Samples/Big' '" + g_card + "/GRANULAR' '" +
                g_card + "/WAVETABLES' '" + g_card + "/MULTISAMPLE' '" + g_card + "/Plugins' '" + g_card + "/MyStuff' '" +
                g_card + "/Samples/Keys/piano'").c_str()) == 0);
  // 600 small files in one folder (not one is opened by the listing), and two that are not audio
  std::vector<uint8_t> small = wg::wav_bytes(480, 1, 16, 48000, 1, "", tone(440));
  for (int i = 0; i < 600; ++i) {
    char n[64];
    snprintf(n, sizeof n, "/Samples/Big/hit%03d.wav", (i * 7919) % 600);   // written out of order
    wg::write_file(g_card + n, small);
  }
  wg::write_file(g_card + "/Samples/Big/readme.txt", {'x'});
  wg::write_file(g_card + "/Samples/Big/._hit000.wav", {'x'});
  wg::write_file(g_card + "/Samples/Drums/snare.wav", wg::wav_bytes(24000, 1, 16, 48000, 1, "", tone(200)));
  wg::write_file(g_card + "/Samples/Drums/Kicks/kick.wav", wg::wav_bytes(24000, 1, 16, 48000, 1, "", tone(60)));
  wg::write_file(g_card + "/Samples/Drums/clap.aif", aiff16(22050, 44100, 441));
  // 90 s: longer than a source holds
  wg::write_file(g_card + "/Samples/Drums/long.wav", wg::wav_bytes(90 * 48000, 1, 16, 48000, 1, "", tone(100)));
  for (int i = 0; i < 6; ++i)
    wg::write_file(g_card + "/MyStuff/pad" + std::to_string(i) + ".wav", wg::wav_bytes(12000, 1, 16, 48000, 1, "", tone(100 + 50 * i)));
  wg::write_file(g_card + "/GRANULAR/choir.wav", wg::wav_bytes(24000, 1, 16, 48000, 1, "", tone(330)));
  // a 16-frame table of 2048 in /Samples (not /WAVETABLES): WAVETABLE reads it from the library
  wg::write_file(g_card + "/Samples/Keys/table16.wav", wg::wav_bytes(16 * 2048, 1, 16, 48000, 1, "", [](int i, int) {
                   return std::sin(2 * M_PI * (1 + i / 2048) * (i % 2048) / 2048.0) * 0.8;
                 }));
  // an instrument outside /MULTISAMPLE
  wg::write_file(g_card + "/Samples/Keys/piano/c4.wav", wg::wav_bytes(24000, 1, 16, 48000, 1, "", tone(261.63)));
  FILE* f = fopen((g_card + "/Samples/Keys/piano/piano.sfz").c_str(), "w");
  CHECK(f);
  fprintf(f, "<region> sample=c4.wav pitch_keycenter=60\n");
  fclose(f);
  host_set_card(g_card.c_str());
}

// 1. the library: the root's folders (the Sidecar's left out), a folder of 600 files listed in steps, sorted
static void test_listing() {
  CHECK(srclib::cd(srclib::kGr, ""));
  idle();
  CHECK(srclib::listing_done(srclib::kGr));
  const char* n;
  bool dir;
  std::vector<std::string> root;
  for (int i = 0; srclib::item(srclib::kGr, i, &n, &dir, nullptr); ++i) {
    root.push_back(n);
    CHECK(dir);
  }
  // the virtual folders first, then GRANULAR MULTISAMPLE MyStuff Samples WAVETABLES (not Plugins)
  CHECK(root.size() == 7 && root[0] == "@INPUTS" && root[1] == "@BUILT-IN");
  CHECK(root[2] == "GRANULAR" && root[4] == "MyStuff" && root[5] == "Samples");
  char vp[64];
  CHECK(srclib::item_path(srclib::kGr, 0, vp, sizeof vp) && !strcmp(vp, "@INPUTS") && srclib::cd(srclib::kGr, vp));
  CHECK(srclib::listing_done(srclib::kGr) && srclib::count(srclib::kGr) == 10 && srclib::item(srclib::kGr, 3, &n, &dir, nullptr) &&
        !strcmp(n, "SD") && !dir);
  int vt = 0, vi_ = -1;
  CHECK(srclib::item_path(srclib::kGr, 3, vp, sizeof vp) && !strcmp(vp, "@INPUTS/SD") && srclib::special(srclib::kGr, vp, &vt, &vi_) &&
        vt == pools::kInput && vi_ == 3);
  CHECK(srclib::cd(srclib::kWt, "@BUILT-IN") && srclib::count(srclib::kWt) == 6 && srclib::item(srclib::kWt, 5, &n, &dir, nullptr) &&
        !strcmp(n, "SYNC"));
  CHECK(!srclib::cd(srclib::kWt, "@INPUTS") && !srclib::cd(srclib::kMs, "@BUILT-IN"));   // not theirs
  CHECK(srclib::cd(srclib::kWt, "") && srclib::cd(srclib::kGr, ""));
  idle();
  uint32_t loads = gr::stats().loads, steps = srclib::stats().steps, ents = srclib::stats().entries_read;
  // the listing from the library's RAM index: the longest step well under a block's time
  double worst = 0;
  int calls = 0;
  auto list_big = [&] {
    CHECK(srclib::cd(srclib::kGr, "/Samples/Big"));
    worst = 0, calls = 0;
    while (!srclib::listing_done(srclib::kGr) && calls < 1000) {
      auto t0 = std::chrono::steady_clock::now();
      srclib::step();
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      worst = us > worst ? us : worst;
      ++calls;
    }
    printf("test_srclib: 600 files listed in %d steps (%lu entries read), the longest step %.0f us (Mac)\n", calls,
           (unsigned long)(srclib::stats().entries_read - ents), worst);
    return worst < 20000;
  };
  bool fast = list_big();
  CHECK(srclib::listing_done(srclib::kGr) && srclib::count(srclib::kGr) == 600);
  CHECK(calls >= 1 && srclib::stats().steps - steps >= 1);   // (one step: the whole folder from the library's RAM)
  // (wall-clock: up to 3 tries, the folder listed again - tests/timing.h)
  for (int k = 1; !fast && k < kTimingTries; ++k) {
    printf("timing: the srclib listing over its bound (try %d of %d)\n", k, kTimingTries);
    CHECK(srclib::cd(srclib::kGr, ""));
    idle();
    fast = list_big();
    CHECK(srclib::listing_done(srclib::kGr) && srclib::count(srclib::kGr) == 600);
  }
  CHECK(fast);
  for (int i = 0; i < 600; ++i) {
    char want[16];
    snprintf(want, sizeof want, "hit%03d.wav", i);
    CHECK(srclib::item(srclib::kGr, i, &n, &dir, nullptr) && !strcmp(n, want) && !dir);
  }
  idle();
  CHECK(gr::stats().loads == loads);   // nothing loaded by a listing
  // MULTISAMPLE's browser lists .sfz only
  CHECK(srclib::cd(srclib::kMs, "/Samples/Keys/piano"));
  idle();
  CHECK(srclib::count(srclib::kMs) == 1 && srclib::item(srclib::kMs, 0, &n, &dir, nullptr) && !strcmp(n, "piano.sfz"));
  // a folder with folders: the folders first, AIFF listed
  CHECK(srclib::cd(srclib::kGr, "/Samples/Drums"));
  idle();
  CHECK(srclib::count(srclib::kGr) == 4);
  CHECK(srclib::item(srclib::kGr, 0, &n, &dir, nullptr) && !strcmp(n, "Kicks") && dir);
  CHECK(srclib::item(srclib::kGr, 1, &n, &dir, nullptr) && !strcmp(n, "clap.aif") && !dir);
}

// 2. the machines on their pool rows: the code means the same row everywhere
static void test_banks() {
  char out[400];
  // GRANULAR: C002 a file, loaded on use; C000 (OFF), an empty row and a missing file: a trig sounds nothing
  CHECK(pu::put(pools::kSamples, "C001", "/MyStuff/pad0.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "C002", "/MyStuff/pad1.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "H127", "/Samples/Drums/snare.wav") == pools::kOk);
  CHECK(!strcmp(gr::entry_name(2 * 128 + 2), "pad1") && !strcmp(gr::entry_group(2 * 128 + 2), "BANK C"));
  wg::params(kGranMachine, 4, P(2 + 2 / 8.0f));
  tick(300);
  CHECK(gr::voice_entry(4) == 258 && gr::entry_state(258) == gr::kStateRam);
  wg::trig(kGranMachine, 4, 60, 100, P(0 + 2 / 8.0f));
  tick(50);
  CHECK(gr::voice_notes(4) == 0);
  wg::trig(kGranMachine, 4, 60, 100, P(40 + 2 / 8.0f));
  tick(50);
  CHECK(gr::voice_notes(4) == 0);
  wg::trig(kGranMachine, 4, 60, 100, P(2 + 2 / 8.0f));
  tick(50);
  CHECK(gr::voice_notes(4) > 0);
  wg::off(4);
  tick(50);
  // the template's inputs in H001..H010: H004 plays the SD stem (a live source, no file)
  CHECK(!strcmp(gr::entry_name(7 * 128 + 4), "SD") && !strcmp(gr::entry_group(7 * 128 + 4), "LIVE") && !gr::entry_path(7 * 128 + 4)[0]);
  CHECK(gr::entry_state(7 * 128 + 4) == gr::kStateRam);
  // a folder filled into bank E (name order, the first free rows), through the console
  CHECK(nm::command("pool=smp:fill:E:/MyStuff", out, sizeof out) && strstr(out, "filling"));
  for (int i = 0; i < 200 && pools::filling(); ++i) tick(10);
  // (pad0 and pad1 are in C001 / C002 already: they keep their rows, one entry per file)
  CHECK(!strcmp(gr::entry_name(4 * 128 + 1), "pad2") && !strcmp(gr::entry_name(4 * 128 + 4), "pad5"));
  // MULTISAMPLE: INST B001 plays the .sfz, B000 (OFF) has no instrument
  CHECK(pu::put(pools::kInstr, "B001", "/Samples/Keys/piano/piano.sfz") == pools::kOk);
  CHECK(!strcmp(msamp::entry_name(129), "piano"));
  wg::params(kMultiMachine, 2, {1 + 1 / 8.0f, 64, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0});
  idle();
  tick(200);
  CHECK(msamp::voice_slot(2) >= 0);
  wg::params(kMultiMachine, 2, {0 + 1 / 8.0f, 64, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0});
  idle();
  CHECK(msamp::voice_slot(2) < 0);
  // WAVETABLE: D009 by the console; TBL D009 selects it (any WAV is a table)
  CHECK(nm::command("pool=wt:set:D009:/Samples/Keys/table16.wav", out, sizeof out) && strstr(out, "SET"));
  idle();
  wg::params(kWaveMachine, 1, {9 + 3 / 8.0f, 0, 0, 0, 0, 0, 64, 0, 10, 60, 100, 64, 0, 0, 0, 0});
  tick(300);
  CHECK(wt::voice_entry(1) == 3 * 128 + 9 && wt::table_frames(3 * 128 + 9) == 16);
  // GRANULAR and DSP SAMPLE on the same bank A row: A005 is SMP 5 and SRC A005
  CHECK(pu::put(pools::kSamples, "A005", "/Samples/Drums/Kicks/kick.wav") == pools::kOk);
  pool_sync();
  CHECK(npool >= 5 && !strcmp(pool[4].path, "Drums/Kicks/kick.wav"));   // (rui1045: npool counts every bank's rows)
  CHECK(!strcmp(gr::entry_path(5), "/Samples/Drums/Kicks/kick.wav"));
  // an input in bank A: GRANULAR plays it, DSP SAMPLE's slot is empty
  CHECK(pu::put(pools::kSamples, "A006", "@MAIN") == pools::kOk);
  pool_sync();
  CHECK(!strcmp(gr::entry_name(6), "MAIN") && pool[5].path[0] == 0 && pool[5].folder == -1);
  // rui1045: DSP SAMPLE reaches every bank: SMP C007 (P11 = 7 + 32 / 128) is the SAMPLES row C007, pool slot 2 * 127 + 7
  CHECK(pu::put(pools::kSamples, "C007", "/Samples/Keys/table16.wav") == pools::kOk);
  pool_sync();
  int c7 = pool_slot(7 + 32 / 128.0f);
  CHECK(c7 == 2 * kPool + 7 && npool >= c7 && !strcmp(pool[c7 - 1].path, "Keys/table16.wav"));   // (written after the card
                                                                                                    // index: no folder yet)
  CHECK(pool_slot(5.0f) == 5 && pool_slot(0.0f) == 0 && pool_slot(0 + 32 / 128.0f) == 0 && pool_slot(127 + 112 / 128.0f) == 7 * kPool + 127);
  printf("test_srclib: banks: GRANULAR C002 + H004 + E (fill), MULTISAMPLE B001, WAVETABLE D009, DSP SAMPLE A005 + C007\n");
}

// 3. the pool RAM (core/pool/pcache.h): a WAV played by GRANULAR and a member of an instrument is one copy; the RAM
// figure; an unloaded row's audio leaves; the preload follows the used set at a project's (re)load
static void test_shared_ram() {
  FILE* f = fopen((g_card + "/Samples/Drums/Kicks/kick.sfz").c_str(), "w");
  CHECK(f);
  fprintf(f, "<region> sample=kick.wav pitch_keycenter=36\n");
  fclose(f);
  host_card_core(0);
  lib::touched("/Samples/Drums/Kicks/kick.sfz", nullptr, 0);   // (the writer's report: the library runs here)
  host_card_core(3);
  wg::params(kGranMachine, 5, P(5));   // A005: kick (test_banks), whole in the pool RAM
  tick(300);
  CHECK(gr::entry_state(5) == gr::kStateRam);
  pcache::Stats s0 = pcache::stats();
  CHECK(s0.ready >= 1 && pools::resident(pools::kSamples, 5));
  CHECK(pu::put(pools::kInstr, "C001", "/Samples/Drums/Kicks/kick.sfz") == pools::kOk);
  wg::params(kMultiMachine, 3, {1 + 2 / 8.0f, 64, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0});
  tick(300);
  CHECK(msamp::voice_slot(3) >= 0);
  pcache::Stats s1 = pcache::stats();
  CHECK(s1.shared_hits > s0.shared_hits && s1.used == s0.used && s1.ready == s0.ready);   // its head: the same copy
  pools::Ram r = pools::ram();
  CHECK(r.budget > 0 && r.used == s1.used && r.pct <= 100);
  printf("test_srclib: pool RAM %llu of %llu KB (%u %%), tables %llu KB, %u resident\n", (unsigned long long)(r.used >> 10),
         (unsigned long long)(r.budget >> 10), (unsigned)r.pct, (unsigned long long)(r.tables >> 10), (unsigned)r.entries);
  // unloaded from both lists (the voices moved off): the audio leaves the pool RAM
  wg::params(kGranMachine, 5, P(0));
  wg::params(kMultiMachine, 3, {0, 64, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0});
  tick(300);
  char o[200];
  msamp::command("msamp=rescan", o, sizeof o);
  gr::kit_changed(1);
  CHECK(pu::put(pools::kInstr, "C001", "") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A005", "") == pools::kOk);
  tick(300);
  lib::Ref kr;
  lib::ref_for_path("/Samples/Drums/Kicks/kick.wav", &kr);
  CHECK(pcache::find(kr, 0) < 0 || pcache::stats().ready < s1.ready);
  // the preload: rows seen playing come back into RAM when the project is read again (no voice on them)
  CHECK(pu::put(pools::kSamples, "B050", "/Samples/Drums/clap.aif") == pools::kOk);
  pools::seen(pools::kSamples, pools::code(1, 50));
  tick(1500);   // (the used set written)
  snprintf(core::project, sizeof core::project, "%s", "ELSEWHERE");
  for (int i = 0; i < 400 && strcmp(pools::project(), "ELSEWHERE"); ++i) tick(10);
  lib::Ref cr;
  lib::ref_for_path("/Samples/Drums/clap.aif", &cr);
  pcache::forget(cr);
  CHECK(pcache::find(cr, 0) < 0);
  snprintf(core::project, sizeof core::project, "%s", "");
  for (int i = 0; i < 400 && (strcmp(pools::project(), "_default") || !pools::ready()); ++i) tick(10);
  tick(500);
  CHECK(pcache::find(cr, pcache::kWhole) >= 0);   // preloaded
  printf("test_srclib: shared pool RAM, unload, preload ok\n");
}

// ---- the browser's listings from the library's RAM index (Task 6b): never a card read on the control core
struct Want {
  std::string name;
  bool dir;
  uint32_t size;
};
// today's listing of a card folder, from the host's own directory (the oracle): dot names out, the Sidecar's root
// folders out, the machine's file types, folders first, then by name (case ignored)
static std::vector<Want> oracle(int k, const std::string& folder) {
  std::vector<Want> v;
  DIR* d = opendir((g_card + (folder.empty() ? "/" : folder)).c_str());
  CHECK(d);
  for (dirent* e; (e = readdir(d));) {
    std::string n = e->d_name;
    if (n.empty() || n[0] == '.' || n.size() >= 64) continue;
    struct stat st;
    CHECK(stat((g_card + folder + "/" + n).c_str(), &st) == 0);
    bool dir = S_ISDIR(st.st_mode);
    if (dir ? (folder.empty() && srclib::hidden_name(n.c_str())) : !srclib::accepts(k, n.c_str())) continue;
    v.push_back({n, dir, dir ? 0u : (uint32_t)st.st_size});
  }
  closedir(d);
  std::sort(v.begin(), v.end(), [](const Want& a, const Want& b) {
    if (a.dir != b.dir) return a.dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return v;
}
static std::vector<Want> shown(int k) {
  std::vector<Want> v;
  const char* n;
  bool dir;
  uint32_t size;
  int from = srclib::cwd(k)[0] ? 0 : (k == srclib::kGr ? 2 : k == srclib::kWt ? 1 : 0);   // (the virtual folders)
  for (int i = from; srclib::item(k, i, &n, &dir, &size); ++i) v.push_back({n, dir, size});
  return v;
}
static bool same(const std::vector<Want>& a, const std::vector<Want>& b) {
  if (a.size() != b.size()) {
    printf("test_srclib: %zu entries, want %zu\n", a.size(), b.size());
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].name != b[i].name || a[i].dir != b[i].dir || a[i].size != b[i].size) {
      printf("test_srclib: entry %zu '%s' %d %u, want '%s' %d %u\n", i, a[i].name.c_str(), a[i].dir, a[i].size,
             b[i].name.c_str(), b[i].dir, b[i].size);
      return false;
    }
  return true;
}
static void open_listed(int k, const char* folder) {   // a browser into a folder, the control core's steps until listed
  CHECK(srclib::cd(k, folder));
  for (int i = 0; i < 1000 && !srclib::listing_done(k); ++i) srclib::step();
  CHECK(srclib::listing_done(k));
}
static void lib_ready() {
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) {
    g_us += 2500;
    host_fake_time(true, g_us);
    lib_step();
  }
  CHECK(lib::stats().state == lib::kReady);
}

// browse_lists_from_the_library: the library loaded, folders not listed before: three folders (and the root) through
// the browser cost the control core no card time, listed as today (names, sizes, folders first, the empty folders too)
static void browse_lists_from_the_library() {
  lib_ready();
  // the folder view sorted only when the paths changed: the hashing's publishes do not sort
  printf("test_srclib: library publishes %u, folder view sorts %u\n", lib::work().publishes, lib::work().folder_sorts);
  CHECK(lib::work().folder_sorts < lib::work().publishes);
  srclib::reset_for_tests();   // (no listing kept: every folder read anew)
  host_card_model(true);
  uint64_t c3 = host_card_us(3);
  struct {
    int k;
    const char* folder;
  } open[] = {{srclib::kGr, ""}, {srclib::kGr, "/Samples"}, {srclib::kGr, "/Samples/Drums"}, {srclib::kWt, "/Samples/Big"},
              {srclib::kMs, "/Samples/Keys"}, {srclib::kMs, "/Samples/Keys/piano"}};
  for (auto& o : open) {
    open_listed(o.k, o.folder);
    CHECK(same(shown(o.k), oracle(o.k, o.folder)));
  }
  CHECK(srclib::count(srclib::kGr) == 4);   // (Drums: Kicks, clap.aif, long.wav, snare.wav)
  CHECK(!srclib::cd(srclib::kGr, "/Samples/nope") && !srclib::cd(srclib::kGr, "/Samples/Drums/snare.wav"));
  srclib::up(srclib::kGr);
  for (int i = 0; i < 100 && !srclib::listing_done(srclib::kGr); ++i) srclib::step();
  CHECK(!strcmp(srclib::cwd(srclib::kGr), "/Samples") && same(shown(srclib::kGr), oracle(srclib::kGr, "/Samples")));
  printf("test_srclib: %d folders listed from the library, core 3's card time %llu us\n", (int)(sizeof open / sizeof open[0]),
         (unsigned long long)(host_card_us(3) - c3));
  CHECK(host_card_us(3) == c3);
  host_card_model(false);
}

// browse_follows_a_change: a sample added (the writer's lib::touched) is in its folder's listing after the library's
// next generation; before the boot load is done the folder shows as being read, filled once the snapshot comes
static void browse_follows_a_change() {
  open_listed(srclib::kGr, "/Samples/Drums");
  int before = srclib::count(srclib::kGr);
  host_card_model(true);
  uint64_t c3 = host_card_us(3);
  uint32_t g0 = lib::gen(), v0 = srclib::view_gen(srclib::kGr);
  wg::write_file(g_card + "/Samples/Drums/added.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(500)));
  host_card_core(0);
  lib::touched("/Samples/Drums/added.wav", nullptr, 0);
  host_card_core(3);
  for (int i = 0; i < 2000 && lib::gen() == g0; ++i) {
    g_us += 2500;
    host_fake_time(true, g_us);
    lib_step();
  }
  CHECK(lib::gen() != g0);
  for (int i = 0; i < 10; ++i) srclib::step();
  CHECK(srclib::listing_done(srclib::kGr) && srclib::count(srclib::kGr) == before + 1);
  CHECK(srclib::view_gen(srclib::kGr) != v0);
  CHECK(same(shown(srclib::kGr), oracle(srclib::kGr, "/Samples/Drums")));
  CHECK(shown(srclib::kGr)[1].name == "added.wav");
  // a reboot: the library not loaded yet - the browser waits for it, never reads the card itself
  lib::reset_for_tests();
  lib::begin(true);
  srclib::reset_for_tests();
  CHECK(srclib::cd(srclib::kGr, "/Samples/Big"));   // (no snapshot: the folder taken as it is, listed when it comes)
  for (int i = 0; i < 100; ++i) srclib::step();
  CHECK(!srclib::listing_done(srclib::kGr) && srclib::count(srclib::kGr) == 0);
  CHECK(host_card_us(3) == c3);
  lib_ready();
  for (int i = 0; i < 10; ++i) srclib::step();
  CHECK(srclib::listing_done(srclib::kGr) && same(shown(srclib::kGr), oracle(srclib::kGr, "/Samples/Big")));
  CHECK(host_card_us(3) == c3);
  host_card_model(false);
  printf("test_srclib: a change listed at the library's next generation, the boot load waited for\n");
}

// The user, 2026-10-26: POOL > a sample slot > FUNC + YES (REPLACE) on MULTISAMPLE showed READING forever - the pool
// page opens the list's browser where it is (srclib::cwd), and a browser nobody had pointed anywhere since boot had no
// listing attached (nothing lists it). Opening it must list its folder.
static void pool_replace_lists_a_browser_never_opened() {
  srclib::reset_for_tests();   // as after boot: no browser opened yet
  srclib::open(srclib::kMs);   // what the pool page's REPLACE does
  idle();
  for (int i = 0; i < 50 && !srclib::listing_done(srclib::kMs); ++i) srclib::step(), idle(10);
  printf("pool REPLACE on a browser never opened: done %d, %d items\n", srclib::listing_done(srclib::kMs) ? 1 : 0,
         srclib::count(srclib::kMs));
  CHECK(srclib::listing_done(srclib::kMs) && srclib::count(srclib::kMs) > 0);
}
int main() {
  setup_card();
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, g_us);
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  voices_init();
  card_ok = true;
  lib::begin(true);   // the library (the card core's): the browser lists from its RAM index
  nm::begin(true);
  msamp::begin(true);
  pu::steps();
  idle();
  test_listing();
  browse_lists_from_the_library();
  browse_follows_a_change();
  test_banks();
  test_shared_ram();
  pool_replace_lists_a_browser_never_opened();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_srclib: PASS\n");
  return 0;
}
