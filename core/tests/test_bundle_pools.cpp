// core/tests/test_bundle_pools.cpp - bundles on the project's POOL lists (the pools design 6, docs/BUNDLES.md): the
// collector takes a project's samples and instruments from pools::refs() (the current project) or from its list files
// on the card (pools::text::parse, any other project), instruments with their members; the three list files and
// pool-used.txt travel as opaque kind "pool" files with their format in the manifest's "formats"; sha256 + CRC-32 come
// from the library (no HASH read of an indexed file; a stale record is caught by WRITE's check and hashed again); no
// bank A record; the import writes the list files as they are (refs resolve by hash) and an old bundle's per-machine
// pool files where they were (never read).
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "bundle/bundle.h"
#include "bundle/prjslot.h"
#include "fmt/fmt.h"
#include "bundle/projects.h"
#include "bundle/zip.h"
#include "check.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/sha256.h"
#include "lib/srclib.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "pool/pool.h"
#include "wg_util.h"

using namespace core;
using namespace core::bundle;

static std::string g_root, g_card;
static char g_current[24] = "LIVE";
static uint64_t g_us = 50000000;

static void put(const std::string& p, const std::string& s) {
  std::string m = g_card + p;
  CHECK(system(("mkdir -p \"$(dirname '" + m + "')\"").c_str()) == 0);
  FILE* f = fopen(m.c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static std::string get(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "<missing>";
  std::string s;
  char b[65536];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static bool there(const std::string& p) { return get(p) != "<missing>"; }
// 2026-10-27: a delete moves the project to the trash, a job of the control core (core/bundle/prjslot.h): run here
static void trash_jobs() {
  uint64_t t = 50000000000ull;   // (the clock on: a posted write nobody drives here is waited out, kFlushMs)
  host_fake_time(true, t);
  core::prjslot::tick(core::now_ms());
  for (int i = 0; i < 100000 && core::prjslot::busy(); ++i) {
    host_fake_time(true, t += 20000);
    if (cardq::worker()) while (cardq::service()) {}   // (this test's card core: its requests run between the ticks)
    core::fmt::post_step(), core::pools::step();          // (the control core's idle pass: posted writes, the lists' saves)
    core::prjslot::tick(core::now_ms());
  }
  host_fake_time(false, 0);
}
static bool trashed(const char* key, const char* file) {   // a trash entry of key holds file (its trash name)
  for (int id = 1; id < 200; ++id) {
    char e[64];
    snprintf(e, sizeof e, "/.rytm/trash/%06d", id);
    if (get(std::string(e) + "/info.txt").find(std::string("\nkey=") + key + "\n") != std::string::npos)
      return get(std::string(e) + "/" + file) != "<missing>" && !get(std::string(e) + "/" + file).empty();
  }
  return false;
}

static void use_card(const std::string& name) {
  g_card = g_root + "/" + name;
  CHECK(system(("mkdir -p '" + g_card + "'").c_str()) == 0);
  host_set_card(g_card.c_str());
  projects::reset_for_tests();
  projects::load();
}
static std::string wav(int frames, double hz) {
  auto v = wg::wav_bytes(frames, 1, 16, 48000, 1, "", [hz](int i, int) { return 0.3 * std::sin(2 * M_PI * hz * i / 48000); });
  return std::string(v.begin(), v.end());
}
static std::string sha_hex(const std::string& s) {
  uint8_t h[32];
  char x[65];
  sha256(s.data(), s.size(), h);
  hex(h, 32, x);
  return x;
}

// the control core (pools, srclib's browser) and the card core (cardq, the library) in turn; 1 ms a pass
static void run(int ms) {
  for (int i = 0; i < ms; ++i) {
    for (int k = 0; k < 8 && (pools::step() || srclib::step()); ++k) {}
    if (cardq::worker())
      for (int k = 0; k < 8 && cardq::service(); ++k) {}
    lib::step();
    g_us += 1000;
    host_fake_time(true, g_us);
  }
}
static void lib_settle() {
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  CHECK(lib::stats().state == lib::kReady);
}

static IoRule g_rule = {kPiece, kPiece, 0, ""};
static void rules(IoRule* r) { *r = g_rule; }
static void current(char* out, int cap) { snprintf(out, cap, "%s", g_current); }
static void job() {
  for (int i = 0; i < 400000 && busy(); ++i) {
    step();
    g_us += 100;
    host_fake_time(true, g_us);
    uint8_t p = status().phase;
    if (p == kReady || p == kServe || p == kDone || p == kFailed || p == kCancelled) break;
  }
}
static std::string export_card(const char* key, uint32_t epoch) {
  ExportOpts o;
  o.epoch = epoch;
  CHECK(start_export(key, kToCard, o));
  job();
  if (status().phase != kDone) fprintf(stderr, "export: %s %s\n", phase_name(status().phase), status().msg);
  CHECK(status().phase == kDone);
  std::string name = bundle_name();
  finish();
  return "/Bundles/" + name;
}
static std::map<std::string, std::string> unzip(const std::string& z) {
  std::map<std::string, std::string> m;
  ZRead r;
  char err[160];
  CHECK(eocd((const uint8_t*)z.data() + z.size() - 22, 22, z.size(), &r, err, sizeof err));
  std::vector<ZEntry> e(r.n);
  CHECK(central((const uint8_t*)z.data() + r.cd_off, r, e.data(), r.n, err, sizeof err));
  for (int i = 0; i < r.n; ++i) {
    CHECK(local((const uint8_t*)z.data() + e[i].local, (uint32_t)std::min<uint64_t>(kLocalMax, z.size() - e[i].local), &e[i],
                i + 1 < r.n ? e[i + 1].local : r.cd_off, err, sizeof err));
    m[e[i].name] = z.substr(e[i].data, e[i].size);
  }
  return m;
}
static bool has(const std::string& s, const std::string& what) {
  if (s.find(what) != std::string::npos) return true;
  fprintf(stderr, "not found: %s\n", what.c_str());
  return false;
}

// an old bundle (before the pools redesign): its per-machine pool files, "pools" descriptors and "bank_a", made here
// with the container's own writer
struct OldEntry { std::string name, kind, bytes, path; };
static std::vector<OldEntry> g_old;
static std::string g_man;
static bool old_data(void*, int zi, uint32_t off, uint8_t* buf, uint32_t n) {
  const std::string& s = zi == 0 ? g_man : g_old[zi - 1].bytes;
  if (off + n > s.size()) return false;
  memcpy(buf, s.data() + off, n);
  return true;
}
static std::string old_bundle() {
  g_man = "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"rytm_name\":\"OLD\",\"key\":\"OLD\"},"
          "\"pools\":[{\"id\":\"dsp\",\"entry\":\"project/pool.txt\",\"base\":\"/Samples\",\"skip\":0,\"refs\":\"sample\"},"
          "{\"id\":\"granular\",\"entry\":\"project/granular.pool.txt\",\"base\":\"\",\"skip\":0,\"refs\":\"sample\"}],"
          "\"bank_a\":{\"granular\":[[0,\"@LIVE\"],[2,\"/GRANULAR/pad.wav\"]],\"wavetable\":null},\"entries\":[";
  for (size_t i = 0; i < g_old.size(); ++i) {
    const OldEntry& e = g_old[i];
    g_man += std::string(i ? "," : "") + "{\"name\":\"" + e.name + "\",\"kind\":\"" + e.kind + "\",\"size\":" +
             std::to_string(e.bytes.size()) + ",\"sha256\":\"" + sha_hex(e.bytes) + "\"" +
             (e.path.empty() ? "" : ",\"paths\":[\"" + e.path + "\"]") + "}";
  }
  g_man += "]}";
  std::vector<ZEntry> z(g_old.size() + 1);
  memset((void*)z.data(), 0, sizeof(ZEntry) * z.size());
  strcpy(z[0].name, "manifest.json");
  z[0].size = (uint32_t)g_man.size(), z[0].crc = crc32(0, g_man.data(), g_man.size());
  for (size_t i = 0; i < g_old.size(); ++i) {
    strcpy(z[i + 1].name, g_old[i].name.c_str());
    z[i + 1].size = (uint32_t)g_old[i].bytes.size(), z[i + 1].crc = crc32(0, g_old[i].bytes.data(), g_old[i].bytes.size());
  }
  Layout L;
  CHECK(layout(z.data(), (int)z.size(), 0, 0, &L));
  std::string out(L.total, '\0');
  CHECK(render(z.data(), L, 0, (uint8_t*)&out[0], (uint32_t)L.total, old_data, nullptr));
  return out;
}

int main() {
  char tmpl[] = "/tmp/bundle-pools-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_root = tmpl;
  host_fake_time(true, g_us);
  use_card("A");
  card_ok = true;
  // the card: a sample for bank C, a wavetable, an instrument with two members, a sample no list names
  put("/Samples/Banks/c1.wav", wav(48000, 300));
  put("/WAVETABLES/saw.wav", wav(2048, 23.4375));
  put("/MULTISAMPLE/Piano/Piano.sfz", "<region> sample=c3.wav key=48\n<region> sample=c4.wav key=60\n");
  put("/MULTISAMPLE/Piano/c3.wav", wav(48000, 130.8));
  put("/MULTISAMPLE/Piano/c4.wav", wav(48000, 261.6));
  put("/Samples/Other/unused.wav", wav(4800, 50));
  put("/.rytm/granular/LIVE.pool.txt", "/Samples/Other/unused.wav\n");   // an old per-machine pool: not carried
  cardq::reset();
  cardq::set_worker(true);
  lib::begin(true);
  srclib::begin(true);
  pools::begin(true);
  lib_settle();
  snprintf(core::project, sizeof core::project, "%s", "LIVE");
  for (int i = 0; i < 2000 && (!pools::ready() || strcmp(pools::project(), "LIVE")); ++i) run(1);
  CHECK(pools::ready());

  // the lists: C005 a sample, A010 a wavetable, B003 the instrument (its members from the .sfz); C005 seen playing
  int out = -1;
  CHECK(pools::set_file(pools::kSamples, pools::code(2, 5), "/Samples/Banks/c1.wav", &out, pools::kConfirmed) == pools::kOk);
  CHECK(pools::set_file(pools::kTables, pools::code(0, 10), "/WAVETABLES/saw.wav", &out, pools::kConfirmed) == pools::kOk);
  CHECK(pools::set_file(pools::kInstr, pools::code(1, 3), "/MULTISAMPLE/Piano/Piano.sfz", &out, pools::kConfirmed) == pools::kPending);
  for (int i = 0; i < 2000 && pools::pending(); ++i) run(1);
  CHECK(pools::at(pools::kInstr, pools::code(1, 3)).type == pools::kInstrument);
  int nm = 0;
  CHECK(pools::members(pools::kInstr, pools::code(1, 3), &nm) && nm == 2);
  pools::seen(pools::kSamples, pools::code(2, 5));
  for (int i = 0; i < 3000 && pools::saving(); ++i) run(1);
  CHECK(!pools::saving());
  const std::string D = "/.rytm/projects/LIVE/";
  for (const char* f : {"samples.pool.txt", "wavetables.pool.txt", "instruments.pool.txt", "pool-used.txt"}) {
    if (!there(D + f)) fprintf(stderr, "no %s\n", f);
    CHECK(there(D + f));
  }
  CHECK(has(get(D + "samples.pool.txt"), "C005 sample "));

  // ================= the current project: refs() collects; hashes from the library (HASH reads no indexed file)
  Env env{};
  env.rules = rules;
  env.current = current;
  env.version = "0.2.0-test";
  env.board = "mac";
  env.flush_ms = 0;
  set_env(env);
  reset_for_tests();
  const uint32_t kEpoch = 1792677780u;
  uint64_t sample_bytes = 0;
  for (const char* s : {"/Samples/Banks/c1.wav", "/WAVETABLES/saw.wav", "/MULTISAMPLE/Piano/c3.wav", "/MULTISAMPLE/Piano/c4.wav"})
    sample_bytes += get(s).size();
  stats_reset();
  std::string z1 = export_card("LIVE", kEpoch);
  uint64_t read1 = stats().read;
  printf("test_bundle_pools: export read %llu bytes, the samples are %llu\n", (unsigned long long)read1, (unsigned long long)sample_bytes);
  CHECK(read1 < sample_bytes + sample_bytes / 4);   // WRITE reads each once; a HASH pass would read them again
  auto e1 = unzip(get(z1));
  for (const char* n : {"project/samples.pool.txt", "project/wavetables.pool.txt", "project/instruments.pool.txt",
                        "project/pool-used.txt", "files/Samples/Banks/c1.wav", "files/WAVETABLES/saw.wav",
                        "files/MULTISAMPLE/Piano/Piano.sfz", "files/MULTISAMPLE/Piano/c3.wav", "files/MULTISAMPLE/Piano/c4.wav"}) {
    if (!e1.count(n)) fprintf(stderr, "missing entry %s\n", n);
    CHECK(e1.count(n));
  }
  for (auto& kv : e1) {
    CHECK(kv.first.find("unused") == std::string::npos && kv.first.find("granular") == std::string::npos);
    CHECK(kv.first.find("project/pool.txt") == std::string::npos);
  }
  CHECK(e1["project/samples.pool.txt"] == get(D + "samples.pool.txt"));   // byte for byte
  CHECK(e1["project/pool-used.txt"] == get(D + "pool-used.txt"));
  std::string man = e1["manifest.json"];
  CHECK(has(man, "\"name\":\"project/samples.pool.txt\",\"kind\":\"pool\""));
  CHECK(has(man, "\"format\":{\"name\":\"samplepool\",\"version\":1}"));
  CHECK(has(man, "\"format\":{\"name\":\"instpool\",\"version\":1}"));
  CHECK(has(man, "\"format\":{\"name\":\"poolused\",\"version\":1}"));
  CHECK(has(man, "\"samplepool\":1") && has(man, "\"wtpool\":1") && has(man, "\"instpool\":1") && has(man, "\"poolused\":1"));
  CHECK(man.find("bank_a") == std::string::npos && man.find("\"pools\"") == std::string::npos);
  CHECK(has(man, "\"name\":\"files/MULTISAMPLE/Piano/Piano.sfz\",\"kind\":\"sfz\""));
  CHECK(has(man, "\"missing\":[]"));
  for (const char* s : {"/Samples/Banks/c1.wav", "/MULTISAMPLE/Piano/c3.wav", "/MULTISAMPLE/Piano/c4.wav"})   // the library's sha
    CHECK(has(man, std::string("\"name\":\"files") + s + "\",\"kind\":\"sample\",\"size\":" + std::to_string(get(s).size()) +
                       ",\"sha256\":\"" + sha_hex(get(s)) + "\""));

  // ================= another project's lists (not the current one): read from its files on the card; a sample moved
  // since (found by its hash: the bundle carries it where it is now)
  CHECK(rename((g_card + "/Samples/Banks/c1.wav").c_str(), (g_card + "/Samples/Moved.wav").c_str()) == 0);
  lib::moved("/Samples/Banks/c1.wav", "/Samples/Moved.wav");
  lib_settle();
  strcpy(g_current, "OTHER");
  std::string z2 = export_card("LIVE", kEpoch + 60);
  auto e2 = unzip(get(z2));
  CHECK(e2.count("files/Samples/Moved.wav") && !e2.count("files/Samples/Banks/c1.wav"));
  CHECK(e2.count("files/MULTISAMPLE/Piano/c4.wav") && e2.count("files/MULTISAMPLE/Piano/Piano.sfz"));
  CHECK(e2["project/samples.pool.txt"] == e1["project/samples.pool.txt"]);
  CHECK(has(e2["manifest.json"], "\"missing\":[]"));

  // ================= a file changed behind the library's back (same size): WRITE's check refuses the export and the
  // library hashes it again; the next export carries the new content
  {
    std::string c4 = get("/MULTISAMPLE/Piano/c4.wav");
    std::string c4b = c4;
    c4b[c4b.size() - 1] ^= 0x55;
    put("/MULTISAMPLE/Piano/c4.wav", c4b);
    ExportOpts o;
    o.epoch = kEpoch + 120;
    CHECK(start_export("LIVE", kToCard, o));
    job();
    CHECK(status().phase == kFailed && strstr(status().msg, "changed during export"));
    finish();
    lib_settle();
    std::string z3 = export_card("LIVE", kEpoch + 180);
    auto e3 = unzip(get(z3));
    CHECK(e3["files/MULTISAMPLE/Piano/c4.wav"] == c4b);
    CHECK(has(e3["manifest.json"], sha_hex(c4b)));
    put("/MULTISAMPLE/Piano/c4.wav", c4);
    lib::touched("/MULTISAMPLE/Piano/c4.wav", nullptr, 0);
    lib_settle();
  }
  std::string za = get(z1);
  lib::reset_for_tests();

  // ================= import onto an empty card: the list files as they are (no rewriting: refs resolve by hash), the
  // instrument and its members together
  use_card("B");
  put("/Bundles/incoming/live.sidecar.zip", za);
  CHECK(start_verify("/Bundles/incoming/live.sidecar.zip"));
  job();
  CHECK(status().phase == kReady);
  CHECK(start_import(ImportOpts()));
  job();
  if (status().phase != kDone) fprintf(stderr, "import: %s %s\n", phase_name(status().phase), status().msg);
  CHECK(status().phase == kDone);
  finish();
  for (const char* f : {"samples.pool.txt", "wavetables.pool.txt", "instruments.pool.txt", "pool-used.txt"})
    CHECK(get(D + f) == e1[std::string("project/") + f]);
  CHECK(get("/Samples/Banks/c1.wav") == e1["files/Samples/Banks/c1.wav"]);
  const std::string R = "/Samples/Projects/LIVE";
  CHECK(get(R + "/MULTISAMPLE/Piano/Piano.sfz") == e1["files/MULTISAMPLE/Piano/Piano.sfz"]);
  CHECK(get(R + "/MULTISAMPLE/Piano/c3.wav") == e1["files/MULTISAMPLE/Piano/c3.wav"]);
  CHECK(get(R + "/MULTISAMPLE/Piano/c4.wav") == e1["files/MULTISAMPLE/Piano/c4.wav"]);
  CHECK(!there("/.rytm/granular/LIVE.pool.txt"));
  // delete: the project's folder goes with its list files
  strcpy(g_current, "OTHER");
  snprintf(core::project, sizeof core::project, "%s", "OTHER");   // (the Pi's env.current is core::project: as one here)
  CHECK(start_delete("LIVE", "LIVE"));
  job();
  CHECK(status().phase == kDone);
  finish();
  trash_jobs();   // (2026-10-27: into the project trash, never deleted)
  CHECK(trashed("LIVE", "projects!%/samples.pool.txt"));
  CHECK(!there(D + "samples.pool.txt") && !there("/.rytm/projects/LIVE"));
  CHECK(there("/Samples/Banks/c1.wav"));

  // ================= an old bundle: its per-machine pool files written where they were, byte for byte (never read);
  // its "pools" and "bank_a" read past
  use_card("C");
  g_old = {{"project/pool.txt", "pool", "Drums/kick.wav\n", ""},
           {"project/granular.pool.txt", "pool", "/Samples/Drums/kick.wav\n", ""},
           {"project/slots.txt", "project", "U1 sc.test 1.0.0\n", ""},
           {"files/Samples/Drums/kick.wav", "sample", wav(480, 60), "/Samples/Drums/kick.wav"}};
  put("/Bundles/incoming/old.sidecar.zip", old_bundle());
  CHECK(start_verify("/Bundles/incoming/old.sidecar.zip"));
  job();
  if (status().phase != kReady) fprintf(stderr, "old verify: %s %s\n", phase_name(status().phase), status().msg);
  CHECK(status().phase == kReady);
  CHECK(start_import(ImportOpts()));
  job();
  CHECK(status().phase == kDone);
  finish();
  CHECK(get("/.rytm/pools/OLD.txt") == "Drums/kick.wav\n");
  CHECK(get("/.rytm/granular/OLD.pool.txt") == "/Samples/Drums/kick.wav\n");
  CHECK(get("/.rytm/slots/OLD.txt") == "U1 sc.test 1.0.0\n");
  CHECK(get("/Samples/Drums/kick.wav") == g_old[3].bytes);
  CHECK(project_exists("OLD"));

  CHECK(system(("rm -rf '" + g_root + "'").c_str()) == 0);
  printf("test_bundle_pools: PASS\n");
  return 0;
}
