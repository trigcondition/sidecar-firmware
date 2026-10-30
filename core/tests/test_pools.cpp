// core/tests/test_pools.cpp - the project's three POOL lists (core/lib/pools.h; the pools design 3, 4.3, 4.4, 7): the
// template (no migration: the old per-machine pool files are not read), OFF at row 0 of every bank, the entry types
// each list takes, one entry per hash, the list files written by the card core (fmt::write_atomic through cardq) and
// read back the same (the golden files round-trip through the module), a newer / damaged file locked and never
// written, every lib::resolve state on an entry and an instrument's member (moved, missing, a "-" ref filled once
// hashed), refs() with members, the used set and SELECT UNUSED, the P3 guards (a row in use asks, the Rytm playing
// refuses unless forced, one-level undo), a folder fill, an instrument's members read from its .sfz, a project switch.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "fmt/fmt.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/pools_fmt.h"
#include "lib/srclib.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "pool/pool.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
static bool g_play = false;

static std::string slurp(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void put(const std::string& card_path, const std::string& text) {
  std::string d = (g_card + card_path).substr(0, (g_card + card_path).rfind('/'));
  CHECK(system(("mkdir -p '" + d + "'").c_str()) == 0);
  wg::write_file(g_card + card_path, std::vector<uint8_t>(text.begin(), text.end()));
}
// the control core's passes (pools, srclib's browser) with the card core's turn between them; 1 ms a pass
static void run(int ms) {
  for (int i = 0; i < ms; ++i) {
    for (int k = 0; k < 8 && (pools::step() || srclib::step()); ++k) {}
    if (cardq::worker())
      for (int k = 0; k < 8 && cardq::service(); ++k) {}
    g_us += 1000;
    host_fake_time(true, g_us);
  }
}
static void lib_ready() {
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  CHECK(lib::stats().state == lib::kReady);
  run(5);
}
static void open_project(const char* name) {
  snprintf(core::project, sizeof core::project, "%s", name);
  for (int i = 0; i < 2000 && (!pools::ready() || strcmp(pools::project(), name)); ++i) run(1);
  CHECK(pools::ready());
}
static int C(const char* row) {
  int c = -1;
  CHECK(pools::parse_row(row, &c));
  return c;
}
static const pools::Entry& E(int l, const char* row) { return pools::at(l, C(row)); }
static std::string file_of(const char* key, const char* name) { return slurp(g_card + "/.rytm/projects/" + key + "/" + name); }

int main() {
  char tmpl[] = "/tmp/pools-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples/Drums' '" + g_card + "/Samples/Fill' '" + g_card + "/GRANULAR' '" +
                g_card + "/MULTISAMPLE/Piano' '" + g_card + "/.rytm/granular' '" + g_card + "/Moved'").c_str()) == 0);
  auto tone = [](double hz) { return [hz](int i, int) { return 0.4 * std::sin(2 * M_PI * hz * i / 48000); }; };
  wg::write_file(g_card + "/Samples/Drums/kick.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(60)));
  wg::write_file(g_card + "/Samples/Drums/snare.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(200)));
  wg::write_file(g_card + "/Samples/Drums/kick copy.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(60)));   // = kick
  wg::write_file(g_card + "/GRANULAR/choir.wav", wg::wav_bytes(9600, 2, 16, 48000, 1, "", tone(330)));
  for (int i = 0; i < 5; ++i)
    wg::write_file(g_card + "/Samples/Fill/f" + std::to_string(4 - i) + ".wav", wg::wav_bytes(480 + i, 1, 16, 48000, 1, "", tone(100 + i)));
  wg::write_file(g_card + "/MULTISAMPLE/Piano/c3.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(130.8)));
  wg::write_file(g_card + "/MULTISAMPLE/Piano/c4.wav", wg::wav_bytes(4800, 1, 16, 48000, 1, "", tone(261.6)));
  put("/MULTISAMPLE/Piano/Piano.sfz", "<region> sample=c3.wav key=48\n<region> sample=c4.wav key=60\n<region> sample=c3.wav key=49\n");
  put("/.rytm/granular/LIVE.pool.txt", "/GRANULAR/choir.wav\n");   // an old per-machine pool: never read
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  card_ok = true;
  cardq::reset();
  cardq::set_worker(true);   // the Pi: the card core (core 0's loop) runs the list reads and writes
  lib::begin(true);
  srclib::begin(true);
  pools::set_playing_fn([] { return g_play; });
  pools::begin(true);
  lib_ready();

  // ---- the template: a project without list files (the old per-machine pool file ignored)
  open_project("LIVE");
  for (int b = 0; b < pools::kBanks; ++b)
    for (int l = 0; l < pools::kLists; ++l) CHECK(pools::at(l, pools::code(b, 0)).type == pools::kOff);
  for (int i = 0; i < pools::kInputs; ++i) {
    char r[8];
    snprintf(r, sizeof r, "H%03d", i + 1);
    CHECK(E(pools::kSamples, r).type == pools::kInput && E(pools::kSamples, r).id == i);
  }
  CHECK(E(pools::kSamples, "H011").type == pools::kBuiltin && !strcmp(E(pools::kSamples, "H011").name, "PAD"));
  CHECK(E(pools::kSamples, "H012").type == pools::kBuiltin && !strcmp(E(pools::kSamples, "H012").name, "BELLS"));
  CHECK(pools::count(pools::kSamples) == 12);
  for (int i = 0; i < 6; ++i) CHECK(pools::at(pools::kTables, i + 1).type == pools::kBuiltin && pools::at(pools::kTables, i + 1).id == i);
  CHECK(!strcmp(E(pools::kTables, "A001").name, "BASIC") && !strcmp(E(pools::kTables, "A006").name, "SYNC"));
  CHECK(pools::count(pools::kInstr) == 0);
  CHECK(E(pools::kSamples, "A001").type == pools::kEmpty);   // bank A holds no inputs: DSP SAMPLE's reach
  CHECK(file_of("LIVE", "samples.pool.txt").empty());       // the template is not written until an edit
  CHECK(pools::stats().templates >= 3);
  CHECK(pools::playable(E(pools::kSamples, "H001")) && !pools::playable(E(pools::kSamples, "A000")));

  // ---- edits: types, OFF, built-ins, one entry per hash
  int out = -1;
  CHECK(pools::add_file(pools::kSamples, 0, "/Samples/Drums/kick.wav", &out) == pools::kOk && out == C("A001"));
  CHECK(E(pools::kSamples, "A001").type == pools::kSample && !strcmp(E(pools::kSamples, "A001").name, "kick"));
  CHECK(E(pools::kSamples, "A001").ref.has_sha && E(pools::kSamples, "A001").st == lib::kExact);
  CHECK(!strcmp(E(pools::kSamples, "A001").path, "/Samples/Drums/kick.wav"));
  CHECK(pools::add_file(pools::kSamples, 0, "/Samples/Drums/kick.wav", &out) == pools::kExisting && out == C("A001"));
  CHECK(pools::add_file(pools::kSamples, 2, "/Samples/Drums/kick copy.wav", &out) == pools::kExisting && out == C("A001"));   // same hash
  CHECK(pools::set_file(pools::kSamples, C("C005"), "/Samples/Drums/snare.wav", &out) == pools::kOk && out == C("C005"));
  CHECK(pools::set_file(pools::kSamples, C("C000"), "/Samples/Drums/kick.wav", &out) == pools::kErrOff);
  CHECK(pools::set_file(pools::kSamples, C("H001"), "/GRANULAR/choir.wav", &out) == pools::kErrBuiltin);
  CHECK(pools::set_file(pools::kSamples, C("A002"), "/MULTISAMPLE/Piano/Piano.sfz", &out) == pools::kErrType);
  CHECK(pools::set_file(pools::kInstr, C("A002"), "/Samples/Drums/kick.wav", &out) == pools::kErrType);
  CHECK(pools::set_special(pools::kTables, C("A010"), pools::kInput, 0) == pools::kErrType);   // no inputs in wavetables
  CHECK(pools::set_special(pools::kSamples, C("A010"), pools::kBuiltin, 1) == pools::kOk);
  CHECK(E(pools::kSamples, "A010").type == pools::kBuiltin && !strcmp(E(pools::kSamples, "A010").name, "BELLS"));
  CHECK(pools::unload(pools::kSamples, C("A010")) == pools::kOk && E(pools::kSamples, "A010").type == pools::kEmpty);
  CHECK(pools::add_file(pools::kTables, 1, "/GRANULAR/choir.wav", &out) == pools::kOk && out == C("B001"));   // any sample is a table
  CHECK(E(pools::kTables, "B001").ref.has_sha);
  // the saves: by the card core, the list's rows in order, the hash refs
  run(500);
  std::string sp = file_of("LIVE", "samples.pool.txt");
  CHECK(sp.compare(0, 22, "#sidecar samplepool 1\n") == 0);
  CHECK(sp.find("A001 sample ") != std::string::npos && sp.find(" /Samples/Drums/kick.wav\tkick") != std::string::npos);
  CHECK(sp.find("C005 sample ") != std::string::npos && sp.find("H001 input IN\n") != std::string::npos);
  CHECK(sp.find("A001") < sp.find("C005") && sp.find("C005") < sp.find("H001"));
  CHECK(sp.find("A010") == std::string::npos);
  CHECK(file_of("LIVE", "wavetables.pool.txt").find("B001 sample ") != std::string::npos);
  CHECK(file_of("LIVE", "wavetables.pool.txt").find("A001 builtin BASIC\n") != std::string::npos);
  { fmt::Info info;   // what the module writes is what the registry reads
    fmt::Bytes in = {(const uint8_t*)sp.data(), (uint32_t)sp.size()};
    CHECK(fmt::load(*fmt::find("samplepool"), in, nullptr, &info) == fmt::kOk); }
  CHECK(pools::stats().saves >= 2);

  // ---- an instrument: its .sfz read by the card core, its members (distinct samples, in order) as refs
  CHECK(pools::add_file(pools::kInstr, 1, "/MULTISAMPLE/Piano/Piano.sfz", &out) == pools::kPending);
  for (int i = 0; i < 200 && pools::pending(); ++i) run(1);
  CHECK(!pools::pending());
  const pools::Entry& pe = E(pools::kInstr, "B001");
  CHECK(pe.type == pools::kInstrument && !strcmp(pe.name, "Piano") && pe.ref.has_sha);
  int nm = 0;
  const pools::Member* ms = pools::members(pools::kInstr, C("B001"), &nm);
  CHECK(nm == 2 && ms && !strcmp(ms[0].ref.hint, "/MULTISAMPLE/Piano/c3.wav") && !strcmp(ms[1].ref.hint, "/MULTISAMPLE/Piano/c4.wav"));
  CHECK(ms[0].ref.has_sha && ms[0].st == lib::kExact);
  CHECK(pools::add_file(pools::kInstr, 3, "/MULTISAMPLE/Piano/Piano.sfz", &out) == pools::kPending);
  for (int i = 0; i < 200 && pools::pending(); ++i) run(1);
  CHECK(E(pools::kInstr, "D001").type == pools::kEmpty);   // the same .sfz: its row B001 kept, no second entry
  // refs(): every ref, members included
  struct Seen { int n, members; } seen = {0, 0};
  pools::refs(pools::kInstr, [](void* c, int, int, int m, const lib::Ref&) {
    Seen* s = (Seen*)c;
    ++s->n, s->members += m >= 0;
  }, &seen);
  CHECK(seen.n == 3 && seen.members == 2);
  run(500);
  std::string ip = file_of("LIVE", "instruments.pool.txt");
  CHECK(ip.find("B001 instrument ") != std::string::npos && ip.find("\n m ") != std::string::npos &&
        ip.find("/MULTISAMPLE/Piano/c4.wav\n") != std::string::npos);

  // ---- resolution: a member and a sample moved on the card (found by hash), a sample deleted (MISSING)
  CHECK(rename((g_card + "/MULTISAMPLE/Piano/c4.wav").c_str(), (g_card + "/Moved/c4.wav").c_str()) == 0);
  CHECK(rename((g_card + "/Samples/Drums/snare.wav").c_str(), (g_card + "/Moved/snare.wav").c_str()) == 0);
  CHECK(remove((g_card + "/GRANULAR/choir.wav").c_str()) == 0);
  lib::rescan();
  lib_ready();
  run(50);
  CHECK(E(pools::kSamples, "C005").st == lib::kMoved && !strcmp(E(pools::kSamples, "C005").path, "/Moved/snare.wav"));
  ms = pools::members(pools::kInstr, C("B001"), &nm);
  CHECK(ms[1].st == lib::kMoved);
  CHECK(E(pools::kTables, "B001").st == lib::kMissing && !E(pools::kTables, "B001").path[0]);
  CHECK(!pools::playable(E(pools::kTables, "B001")));
  CHECK(E(pools::kSamples, "A001").st == lib::kExact);

  // ---- a "-" ref: a file the library has not hashed yet, filled in once it has, and written at the next save
  wg::write_file(g_card + "/Samples/Drums/new.wav", wg::wav_bytes(2400, 1, 16, 48000, 1, "", tone(500)));
  CHECK(pools::set_file(pools::kSamples, C("A003"), "/Samples/Drums/nope.wav", &out) == pools::kErrPath);   // not on the card
  lib::rescan();
  { lib::Ref r;   // walked, not hashed yet (the walk publishes before it hashes)
    bool k = false;
    for (int i = 0; i < 200000 && !(k = lib::ref_for_path("/Samples/Drums/new.wav", &r) && !r.has_sha); ++i) lib::step();
    CHECK(k); }
  CHECK(pools::set_file(pools::kSamples, C("A003"), "/Samples/Drums/new.wav", &out) == pools::kOk);
  CHECK(!E(pools::kSamples, "A003").ref.has_sha && E(pools::kSamples, "A003").path[0]);   // played by path meanwhile
  run(500);
  CHECK(file_of("LIVE", "samples.pool.txt").find("A003 sample - ") != std::string::npos);
  lib::rescan();
  lib_ready();
  run(500);
  CHECK(E(pools::kSamples, "A003").ref.has_sha && E(pools::kSamples, "A003").st == lib::kExact);
  CHECK(file_of("LIVE", "samples.pool.txt").find("A003 sample - ") == std::string::npos);

  // ---- the used set: seen codes, written a second after a change, SELECT UNUSED
  pools::seen(pools::kSamples, C("A001"));
  pools::seen(pools::kSamples, C("A001"));
  pools::seen(pools::kInstr, C("B001"));
  CHECK(pools::used(pools::kSamples, C("A001")) && pools::used_count(pools::kSamples, C("A001")) == 2);
  CHECK(!pools::used(pools::kSamples, C("C005")));
  int16_t un[64];
  int nu = pools::select_unused(pools::kSamples, un, 64);
  bool has_c005 = false, has_a001 = false;
  for (int i = 0; i < nu; ++i) has_c005 |= un[i] == C("C005"), has_a001 |= un[i] == C("A001");
  CHECK(has_c005 && !has_a001);
  CHECK(strstr(pools::kUnusedWarning, "NOT SEEN PLAYING"));
  run(1200);
  std::string uf = file_of("LIVE", "pool-used.txt");
  CHECK(uf.compare(0, 20, "#sidecar poolused 1\n") == 0 && uf.find("smp A001 2\n") != std::string::npos &&
        uf.find("inst B001 1\n") != std::string::npos);

  // ---- P3: a row in use asks first; while the Rytm plays it is refused unless forced; one-level undo
  v[3].gr = v[4].gr = true;   // two voices on GRANULAR, on C005
  pools::voice_on(pools::kSamples, 3, C("C005"));
  pools::voice_on(pools::kSamples, 4, C("C005"));
  CHECK(pools::users(pools::kSamples, C("C005")) == 2 && pools::in_use(pools::kSamples, C("C005")));
  CHECK(pools::set_file(pools::kSamples, C("C005"), "/Samples/Drums/new.wav", &out) == pools::kExisting);   // in A003
  wg::write_file(g_card + "/Samples/Drums/hat.wav", wg::wav_bytes(1200, 1, 16, 48000, 1, "", tone(900)));
  lib::rescan();
  lib_ready();
  CHECK(pools::set_file(pools::kSamples, C("C005"), "/Samples/Drums/hat.wav", &out) == pools::kErrUsed);
  CHECK(E(pools::kSamples, "C005").path[0] && !strcmp(E(pools::kSamples, "C005").name, "snare"));
  g_play = true;
  CHECK(pools::set_file(pools::kSamples, C("C005"), "/Samples/Drums/hat.wav", &out, pools::kConfirmed) == pools::kErrPlaying);
  CHECK(pools::unload(pools::kSamples, C("C005"), pools::kConfirmed) == pools::kErrPlaying);
  CHECK(pools::set_file(pools::kSamples, C("C006"), "/Samples/Drums/hat.wav", &out) == pools::kOk);   // an empty row: fine
  CHECK(pools::unload(pools::kSamples, C("C006")) == pools::kOk);
  CHECK(pools::set_file(pools::kSamples, C("C005"), "/Samples/Drums/hat.wav", &out, pools::kForced) == pools::kOk);
  g_play = false;
  CHECK(!strcmp(E(pools::kSamples, "C005").name, "hat"));
  CHECK(pools::undo(pools::kSamples) && !strcmp(E(pools::kSamples, "C005").name, "snare"));
  CHECK(!pools::undo(pools::kSamples));   // one level
  CHECK(pools::unload(pools::kSamples, C("A001")) == pools::kErrUsed);   // seen playing
  CHECK(pools::unload(pools::kSamples, C("A001"), pools::kConfirmed) == pools::kOk);
  CHECK(pools::undo(pools::kSamples) && E(pools::kSamples, "A001").type == pools::kSample);
  pools::voice_on(pools::kSamples, 3, -1), pools::voice_on(pools::kSamples, 4, -1);

  // ---- a folder fill: its files in name order into the bank's empty rows (a file already in the list keeps its row)
  CHECK(pools::set_file(pools::kSamples, C("E002"), "/Samples/Fill/f1.wav", &out) == pools::kOk);
  CHECK(srclib::cd(srclib::kGr, "@BUILT-IN"));   // a user browses: a fill never moves that browser
  CHECK(pools::fill(pools::kSamples, 4, "/Samples/Fill") == 0);
  for (int i = 0; i < 2000 && pools::filling(); ++i) run(1);
  CHECK(!pools::filling() && !strcmp(srclib::cwd(srclib::kGr), "@BUILT-IN"));
  CHECK(!strcmp(E(pools::kSamples, "E001").name, "f0") && !strcmp(E(pools::kSamples, "E002").name, "f1") &&
        !strcmp(E(pools::kSamples, "E003").name, "f2") && !strcmp(E(pools::kSamples, "E004").name, "f3") &&
        !strcmp(E(pools::kSamples, "E005").name, "f4") && E(pools::kSamples, "E006").type == pools::kEmpty);
  CHECK(pools::fill(pools::kSamples, 4, "/Samples/nope") == 0);   // (its own browser: a missing folder lists empty)
  for (int i = 0; i < 2000 && pools::filling(); ++i) run(1);
  CHECK(!pools::filling() && E(pools::kSamples, "E006").type == pools::kEmpty);

  // ---- the console
  char o[400];
  CHECK(pools::command("pool=smp:set:B007:/Samples/Drums/hat.wav", o, sizeof o) && strstr(o, "B007"));
  CHECK(E(pools::kSamples, "B007").type == pools::kSample);
  CHECK(pools::command("pool=smp:unload:B007", o, sizeof o) && E(pools::kSamples, "B007").type == pools::kEmpty);
  CHECK(pools::command("pool=smp:set:A000:/Samples/Drums/hat.wav", o, sizeof o) && strstr(o, "000 IS OFF"));
  CHECK(pools::command("pool=smp:builtin:B008:PAD", o, sizeof o) && E(pools::kSamples, "B008").type == pools::kBuiltin);
  CHECK(pools::command("pool=smp:input:B009:MAIN", o, sizeof o) && E(pools::kSamples, "B009").id == 1);
  CHECK(pools::command("pool=wt:input:B009:MAIN", o, sizeof o) && pools::at(pools::kTables, C("B009")).type == pools::kEmpty);
  CHECK(pools::command("pool=smp:unused", o, sizeof o) && strstr(o, "NOT SEEN PLAYING"));
  CHECK(srclib::cd(srclib::kGr, "@INPUTS") && pools::command("pool=smp:assign:B010:2", o, sizeof o) && strstr(o, "SET"));
  CHECK(E(pools::kSamples, "B010").type == pools::kInput && E(pools::kSamples, "B010").id == 2);   // BD
  CHECK(srclib::cd(srclib::kGr, "/Samples/Drums"));
  for (int i = 0; i < 50 && !srclib::listing_done(srclib::kGr); ++i) run(1);
  CHECK(pools::command("pool=smp:assign:B011:0", o, sizeof o) && E(pools::kSamples, "B011").type == pools::kSample);
  CHECK(pools::command("pool=smp:shown:C", o, sizeof o) && pools::shown_bank(pools::kSamples) == 2);
  pools::shown(pools::kSamples, 0);
  CHECK(pools::command("pool", o, sizeof o) && strstr(o, "pool:"));
  CHECK(!pools::command("lib=status", o, sizeof o));
  char js[8192];
  CHECK(pools::bank_json(pools::kSamples, 0, js, sizeof js) > 0 && strstr(js, "\"kick\"") && strstr(js, "\"bank\":0"));
  run(1500);

  // ---- the files read back the same (a second project, then this one again)
  std::string before = file_of("LIVE", "samples.pool.txt");
  open_project("OTHER");
  CHECK(E(pools::kSamples, "A001").type == pools::kEmpty && E(pools::kSamples, "H001").type == pools::kInput);
  CHECK(!pools::used(pools::kSamples, C("A001")));
  open_project("LIVE");
  CHECK(E(pools::kSamples, "A001").type == pools::kSample && !strcmp(E(pools::kSamples, "C005").name, "snare"));
  CHECK(pools::used(pools::kSamples, C("A001")) && pools::used_count(pools::kInstr, C("B001")) == 1);
  CHECK(E(pools::kInstr, "B001").type == pools::kInstrument);
  ms = pools::members(pools::kInstr, C("B001"), &nm);
  CHECK(nm == 2 && ms[1].st == lib::kMoved);
  run(1500);
  CHECK(file_of("LIVE", "samples.pool.txt") == before);   // nothing changed: not written again

  // ---- the golden files through the module: read, then written (an edit and its undo) byte for byte the same
  const char* const gold[][2] = {{"samplepool", "samples.pool.txt"}, {"wtpool", "wavetables.pool.txt"}, {"instpool", "instruments.pool.txt"}};
  std::string gdir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/golden/";
  for (int l = 0; l < 3; ++l) {
    std::string g = slurp(gdir + gold[l][0] + "/v1/live.txt");
    CHECK(!g.empty());
    put(std::string("/.rytm/projects/GOLD/") + gold[l][1], g);
  }
  open_project("GOLD");
  CHECK(E(pools::kSamples, "C127").type == pools::kSample && strlen(E(pools::kSamples, "C127").name) == 39);
  CHECK(E(pools::kSamples, "A001").frames == 103000 && E(pools::kSamples, "A001").ch == 2);
  CHECK(!E(pools::kSamples, "A012").ref.has_sha);
  CHECK(E(pools::kSamples, "A001").st == lib::kChanged && pools::playable(E(pools::kSamples, "A001")));   // other content there
  CHECK(E(pools::kSamples, "C127").st == lib::kMissing && !pools::playable(E(pools::kSamples, "C127")));
  ms = pools::members(pools::kInstr, C("B003"), &nm);
  CHECK(nm == 2 && !ms[1].ref.has_sha);
  for (int l = 0; l < 3; ++l) {
    CHECK(pools::set_special(l == 2 ? pools::kSamples : l, C("G100"), pools::kBuiltin, 0) == (l == 2 ? pools::kOk : pools::kOk));
    CHECK(pools::unload(l == 2 ? pools::kSamples : l, C("G100")) == pools::kOk);
  }
  pools::seen(pools::kInstr, C("B003"));   // (an edit of the instruments list: the used set's, not the list's)
  run(1500);
  for (int l = 0; l < 2; ++l) {
    std::string g = slurp(gdir + gold[l][0] + "/v1/live.txt");
    CHECK(file_of("GOLD", gold[l][1]) == g);
  }
  CHECK(file_of("GOLD", "instruments.pool.txt") == slurp(gdir + "instpool/v1/live.txt"));   // never edited: as it was
  pools::accept_changed(pools::kSamples, C("A001"));   // CHANGED: one tap takes the file's content
  CHECK(E(pools::kSamples, "A001").st == lib::kExact);

  // ---- a newer file: the list locked, never written, no edits; the other lists unaffected
  put("/.rytm/projects/NEWER/samples.pool.txt", "#sidecar samplepool 9\nA001 hologram x\n");
  put("/.rytm/projects/NEWER/wavetables.pool.txt", "garbage\n");
  open_project("NEWER");
  CHECK(pools::locked(pools::kSamples) && strstr(pools::lock_why(pools::kSamples), "newer"));
  CHECK(pools::locked(pools::kTables));   // no header: damaged
  CHECK(!pools::locked(pools::kInstr) && pools::count(pools::kInstr) == 0);
  CHECK(pools::add_file(pools::kSamples, 0, "/Samples/Drums/kick.wav", &out) == pools::kErrLocked);
  CHECK(pools::add_file(pools::kInstr, 0, "/MULTISAMPLE/Piano/Piano.sfz", &out) == pools::kPending);
  for (int i = 0; i < 200 && pools::pending(); ++i) run(1);
  run(1500);
  CHECK(file_of("NEWER", "samples.pool.txt") == "#sidecar samplepool 9\nA001 hologram x\n");
  CHECK(file_of("NEWER", "wavetables.pool.txt") == "garbage\n");
  CHECK(file_of("NEWER", "instruments.pool.txt").find("A001 instrument") != std::string::npos);
  // a foreign-newer file: read, edits in RAM only
  put("/.rytm/projects/FOREIGN/samples.pool.txt", "#sidecar samplepool 2 min=1\nA001 input IN\n");
  open_project("FOREIGN");
  CHECK(pools::read_only(pools::kSamples) && E(pools::kSamples, "A001").type == pools::kInput);
  CHECK(pools::set_special(pools::kSamples, C("A002"), pools::kInput, 1) == pools::kOk);
  run(1500);
  CHECK(file_of("FOREIGN", "samples.pool.txt") == "#sidecar samplepool 2 min=1\nA001 input IN\n");
  // a cut between write_atomic's renames: the .prev is read
  put("/.rytm/projects/CUT/samples.pool.txt.prev", "#sidecar samplepool 1\nA001 input BD\n");
  open_project("CUT");
  CHECK(E(pools::kSamples, "A001").type == pools::kInput && E(pools::kSamples, "A001").id == 2);

  // ---- the review's cases
  open_project("REVIEW");
  {   // H1: the member table compacted in the order of the members' places (an instrument added later at a lower row)
    for (int i = 0; i < 3; ++i) {
      std::string d = "/MULTISAMPLE/I" + std::to_string(i);
      CHECK(system(("mkdir -p '" + g_card + d + "'").c_str()) == 0);
      std::string sfz = "// instrument " + std::to_string(i) + "\n";
      for (int k = 0; k < 4; ++k) {
        std::string w = d + "/s" + std::to_string(k) + ".wav";
        wg::write_file(g_card + w, wg::wav_bytes(480 + 10 * i + k, 1, 16, 48000, 1, "", tone(100 + k)));
        sfz += "<region> sample=s" + std::to_string(k) + ".wav key=" + std::to_string(60 + k) + "\n";
      }
      put(d + "/i.sfz", sfz);
    }
    lib::rescan();
    lib_ready();
    const char* rows[3] = {"C010", "C005", "C001"};   // added in this order: m0 grows while the rows go down
    for (int i = 0; i < 3; ++i) {
      CHECK(pools::set_file(pools::kInstr, C(rows[i]), ("/MULTISAMPLE/I" + std::to_string(i) + "/i.sfz").c_str(), &out, pools::kForced) == pools::kPending);
      for (int k = 0; k < 400 && pools::pending(); ++k) run(1);
    }
    pools::compact_for_tests(pools::kInstr);   // (C001's members last in the table: moved first by row order, before)
    for (int i = 0; i < 3; ++i) {   // each instrument still has its own members, in order
      int n = 0;
      const pools::Member* m = pools::members(pools::kInstr, C(rows[i]), &n);
      if (n != 4) fprintf(stderr, "%s: type %d n %d\n", rows[i], pools::at(pools::kInstr, C(rows[i])).type, n);
      CHECK(n == 4);
      for (int k = 0; k < 4; ++k)
        CHECK(!strcmp(m[k].ref.hint, ("/MULTISAMPLE/I" + std::to_string(i) + "/s" + std::to_string(k) + ".wav").c_str()));
    }
  }
  {   // M9: an instrument while the card job is busy (a list being written): queued, added after
    CHECK(pools::set_special(pools::kSamples, C("D001"), pools::kInput, 0, pools::kForced) == pools::kOk);
    run(250);   // (the save starts: the card job busy for a few passes with the worker on)
    CHECK(pools::add_take(pools::kInstr, "/MULTISAMPLE/I0/i.sfz", &out) == pools::kPending);
    for (int k = 0; k < 2000 && pools::pending(); ++k) run(1);
    int r0 = -1;
    int rr = pools::last_result(&r0);   // (I0 is in C010 already: its row)
    CHECK((rr == pools::kOk || rr == pools::kExisting) && pools::at(pools::kInstr, r0).type == pools::kInstrument);
  }
  {   // M4: a hash filled in while the Rytm plays is not written until it stops; an edit is
    run(1500);
    uint32_t s0 = pools::stats().saves;
    wg::write_file(g_card + "/Samples/Drums/late.wav", wg::wav_bytes(1000, 1, 16, 48000, 1, "", tone(700)));
    lib_ready();
    lib::rescan();
    { lib::Ref r; bool k = false;
      for (int i = 0; i < 200000 && !(k = lib::ref_for_path("/Samples/Drums/late.wav", &r) && !r.has_sha); ++i) lib::step();
      CHECK(k); }
    CHECK(pools::set_file(pools::kSamples, C("D002"), "/Samples/Drums/late.wav", &out, pools::kForced) == pools::kOk);
    run(500);
    CHECK(pools::stats().saves == s0 + 1);   // the edit
    g_play = true;
    lib_ready();
    run(1500);
    CHECK(E(pools::kSamples, "D002").ref.has_sha && pools::stats().saves == s0 + 1);   // filled, not written while playing
    g_play = false;
    run(1500);
    CHECK(pools::stats().saves == s0 + 2);
  }
  {   // L1: undo does not bring back a content that is in another row now
    CHECK(pools::unload(pools::kSamples, C("D002"), pools::kForced) == pools::kOk);
    CHECK(pools::set_file(pools::kSamples, C("D003"), "/Samples/Drums/late.wav", &out, pools::kForced) == pools::kOk);
    CHECK(!pools::undo(pools::kSamples) || pools::find_hash(pools::kSamples, E(pools::kSamples, "D003").ref) == C("D003"));
    CHECK(E(pools::kSamples, "D002").type == pools::kEmpty);
  }
  {   // M12: a voice that left the machine holds no row
    pools::voice_on(pools::kSamples, 6, C("D003"));
    v[6].gr = true;
    CHECK(pools::users(pools::kSamples, C("D003")) == 1);
    v[6].gr = false;
    CHECK(pools::users(pools::kSamples, C("D003")) == 0);
  }
  {   // M1 / M2: a list there but not readable is locked, never the template written over it
    put("/.rytm/projects/BROKEN/samples.pool.txt", "#sidecar samplepool 1\nA001 input IN\n");
    host_fail_open("/.rytm/projects/BROKEN/samples.pool.txt", true);
    open_project("BROKEN");
    CHECK(pools::locked(pools::kSamples) && strstr(pools::lock_why(pools::kSamples), "card error"));
    CHECK(pools::set_special(pools::kSamples, C("A002"), pools::kInput, 1, pools::kForced) == pools::kErrLocked);
    host_fail_open("/.rytm/projects/BROKEN/samples.pool.txt", false);
    run(1500);
    CHECK(file_of("BROKEN", "samples.pool.txt") == "#sidecar samplepool 1\nA001 input IN\n");
  }
  cardq::reset();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_pools: PASS (%u reads, %u saves)\n", pools::stats().reads, pools::stats().saves);
  return 0;
}
