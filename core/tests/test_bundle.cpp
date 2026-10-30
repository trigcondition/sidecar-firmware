// core/tests/test_bundle.cpp - bundles phase 1 (docs/BUNDLES.md): the collector, export to the card and to the
// browser (serve), import onto an empty card (round-trip identity), onto the same card (dedupe: no sample bytes, the
// new name "K 2"), onto a card with a different file at a sample's path (relocated, untouched) and a package conflict,
// a damaged bundle (refused, the card unchanged), a power cut after every change of an import (the boot's recovery
// leaves the card as before or as after), the 4 GiB refusal, delete (project files only).
//
// BUNDLE_OUT=<dir>: the bundles made here are left there for sim/tests/test_bundle.py (Python's zipfile reads them).
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bundle/bundle.h"
#include "bundle/prjslot.h"
#include "lib/pools.h"
#include "fmt/fmt.h"
#include "bundle/projects.h"
#include "lib/sha256.h"
#include "bundle/zip.h"
#include "check.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "lib/library.h"
#include "project_key.h"
#include "rmp_api.h"
#include "rmp_format.h"
#include "rmp_loader.h"
#include "wg_util.h"

using namespace core;
using namespace core::bundle;

static std::string g_root;   // the tests' folder: a card per sub-folder
static std::string g_card;   // the card in use
static char g_current[24] = "AR PERC";

// ---- files on the Mac
static void put(const std::string& card_path, const std::string& s) {
  std::string m = g_card + card_path;
  system(("mkdir -p \"$(dirname '" + m + "')\"").c_str());
  FILE* f = fopen(m.c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static std::string get(const std::string& card_path) {
  FILE* f = fopen((g_card + card_path).c_str(), "rb");
  if (!f) return "<missing>";
  std::string s;
  char b[65536];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
// 2026-10-27: a delete moves the project to the trash, a job of the control core (core/bundle/prjslot.h): run here
static void trash_jobs() {
  uint64_t t = 50000000000ull;   // (the clock on: a posted write nobody drives here is waited out, kFlushMs)
  host_fake_time(true, t);
  core::prjslot::tick(core::now_ms());
  for (int i = 0; i < 100000 && core::prjslot::busy(); ++i) host_fake_time(true, t += 20000), core::fmt::post_step(), core::pools::step(), core::prjslot::tick(core::now_ms());
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
static bool there(const std::string& card_path) {
  struct stat st;
  return stat((g_card + card_path).c_str(), &st) == 0;
}
static void use_card(const std::string& name) {
  g_card = g_root + "/" + name;
  system(("mkdir -p '" + g_card + "'").c_str());
  host_set_card(g_card.c_str());
  projects::reset_for_tests();   // each card its own project index
  projects::load();
}
static void clone_card(const std::string& from, const std::string& to) {
  system(("rm -rf '" + g_root + "/" + to + "' && cp -R '" + g_root + "/" + from + "' '" + g_root + "/" + to + "'").c_str());
}
// every file and folder below the card, with its content's sha256: the card's state ("before" / "after")
static void walk(const std::string& mac, const std::string& rel, std::map<std::string, std::string>* out) {
  DIR* d = opendir(mac.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string full = mac + "/" + n, r = rel + "/" + n;
    struct stat st;
    stat(full.c_str(), &st);
    if (S_ISDIR(st.st_mode)) {
      (*out)[r + "/"] = "dir";
      walk(full, r, out);
    } else {
      FILE* f = fopen(full.c_str(), "rb");
      Sha256 s;
      s.init();
      char b[65536];
      for (size_t k; (k = fread(b, 1, sizeof b, f)) > 0;) s.update(b, k);
      fclose(f);
      uint8_t h[32];
      s.final(h);
      char x[65];
      hex(h, 32, x);
      (*out)[r] = x;
    }
  }
  closedir(d);
}
static std::map<std::string, std::string> tree() {
  std::map<std::string, std::string> t;
  walk(g_card, "", &t);
  return t;
}
static std::string diff(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b) {
  std::string s;
  for (auto& kv : a)
    if (!b.count(kv.first) || b.at(kv.first) != kv.second) s += " -" + kv.first;
  for (auto& kv : b)
    if (!a.count(kv.first)) s += " +" + kv.first;
  return s;
}

// ---- the fixture: one project of every kind (the design's inventory, rows 1-15)
static std::string wav(int frames, double hz) {
  auto v = wg::wav_bytes(frames, 1, 16, 48000, 1, "", [hz](int i, int) { return 0.3 * std::sin(2 * M_PI * hz * i / 48000); });
  return std::string(v.begin(), v.end());
}
static std::string package(const char* id, uint32_t version, uint8_t salt) {
  std::string body(8192, '\0');
  uint32_t magic = RMP_PLUGIN_MAGIC, api = RMP_API;
  memcpy(&body[0], &magic, 4);
  memcpy(&body[4], &api, 4);
  body[100] = (char)salt;
  rmp_header h;
  memset(&h, 0, sizeof h);
  h.magic = RMP_FILE_MAGIC_A64, h.format = RMP_FORMAT, h.api = RMP_API;
  snprintf(h.id, sizeof h.id, "%s", id);
  h.version = version, h.image_size = 8192, h.entry_offset = 0;
  uint32_t ds = 4096;
  memcpy(h.reserved, &ds, 4);
  h.crc32 = ~rmp_crc32_update(0xFFFFFFFFu, (const uint8_t*)body.data(), (uint32_t)body.size());
  return std::string((const char*)&h, sizeof h) + body;
}
static std::string sqp(int version) {   // a pattern file's head ("SQP1", the version): the bytes are carried as they are
  std::string s = "SQP1";
  s += (char)version;
  s += std::string(59, '\x11');
  return s;
}
// a pool list file (core/lib/pools.h; formats samplepool / wtpool / instpool v1): rows -> "sample" refs (the file's
// sha256, "-" for one not on the card), an instrument's members indented under it
static std::string ref_of(const std::string& p);
static std::string pool_list(const char* format, std::vector<std::pair<std::string, std::string>> rows) {
  std::string s = std::string("#sidecar ") + format + " 1\n";
  for (auto& r : rows) {
    if (r.first == " m") s += " m " + ref_of(r.second) + "\n";
    else s += r.first + (strcmp(format, "instpool") ? " sample " : " instrument ") + ref_of(r.second) + "\n";
  }
  return s;
}
static std::string list_of(const char* key, const char* name = "samples") {
  return std::string("/.rytm/projects/") + key + "/" + name + ".pool.txt";
}
static std::string ref_of(const std::string& p) {
  std::string b = get(p);
  if (b == "<missing>") return "- 100 " + p;
  uint8_t h[32];
  char x[65];
  sha256(b.data(), b.size(), h);
  hex(h, 32, x);
  return std::string(x) + " " + std::to_string(b.size()) + " " + p;
}
static void fixture() {
  put("/Samples/Drums/kick.wav", wav(4800, 60));
  put("/Samples/Drums/kick copy.wav", get("/Samples/Drums/kick.wav"));   // the same content at two paths
  put("/Samples/Drums/snare.wav", wav(9600, 200));
  put("/Samples/Banks/b1.wav", wav(2400, 300));
  put("/Samples/Abs/d4.wav", wav(2400, 293));
  put("/Samples/Other/unused.wav", wav(100, 50));   // no pool names it: not in the bundle
  put("/GRANULAR/pad.wav", wav(48000, 110));
  put("/GRANULAR/sub/air.wav", wav(24000, 880));
  put("/WAVETABLES/saw.wav", wav(2048, 23.4375));
  put("/MULTISAMPLE/Piano/Piano.sfz", "<control> default_path=samples/\n<region> sample=c4.wav key=60\n"
                                      "<region> sample=/Samples/Abs/d4.wav key=62\n");
  put("/MULTISAMPLE/Piano/samples/c4.wav", wav(4800, 261.6));
  put("/Samples/AR_PERC/SMP_001.wav", wav(1200, 440));   // the sampler's takes: SMP_001 unused, SMP_002 in the pool
  put("/Samples/AR_PERC/SMP_002.wav", wav(1300, 450));
  // the project's pool lists (the pools design 3.3): samples (a missing one, the same content at two paths, a take,
  // the machines' old folders, bank B), a wavetable, an instrument with its members (one named absolutely)
  put(list_of("AR PERC"), pool_list("samplepool", {{"A001", "/Samples/Drums/kick.wav"}, {"A002", "/Samples/Drums/snare.wav"},
                                                   {"A003", "/Samples/Drums/gone.wav"}, {"A004", "/Samples/Drums/kick copy.wav"},
                                                   {"A005", "/Samples/AR_PERC/SMP_002.wav"}, {"A006", "/GRANULAR/pad.wav"},
                                                   {"A007", "/GRANULAR/sub/air.wav"}, {"B001", "/Samples/Banks/b1.wav"}}) +
                       "H001 input IN\nH011 builtin PAD\n");
  put(list_of("AR PERC", "wavetables"), pool_list("wtpool", {{"A010", "/WAVETABLES/saw.wav"}}) + "A001 builtin BASIC\n");
  put(list_of("AR PERC", "instruments"), pool_list("instpool", {{"B003", "/MULTISAMPLE/Piano/Piano.sfz"},
                                                               {" m", "/MULTISAMPLE/Piano/samples/c4.wav"},
                                                               {" m", "/Samples/Abs/d4.wav"}}));
  put("/.rytm/projects/AR PERC/pool-used.txt", "#sidecar poolused 1\nsmp A001 3\n");
  // the per-machine pools before the redesign: never read, so not carried
  put("/.rytm/pools/AR PERC.txt", "Other/unused.wav\n");
  put("/.rytm/granular/AR PERC.pool.txt", "/Samples/Other/unused.wav\n");
  put("/.rytm/slots/AR PERC.txt", "U1 sc.test 1.0.0\n");
  put("/.rytm/slots/AR PERC.inf", "U1 saved 0 1829 sc.test 1.0.0 TEST\n");
  put("/.rytm/fxslots/AR PERC.txt", "004d SHIMMER\n");
  put("/.rytm/juno/AR PERC.txt", "00112233445566778899aabbccddeeff0011\tBRASS 1\n");
  put("/.rytm/fm/AR PERC.txt", "00112233445566778899aabbccddeeff\tEPIANO\n");
  put("/.rytm/lfo/AR PERC.txt", "L1 V1 TIM SIN 40 20\n");
  put("/.rytm/lfo/AR PERC/k001.txt", "L1 V2 TIM TRI 30 10\n");
  put("/.rytm/ins/AR PERC.txt", "V1 A FILTER 80 20 0 0\n");
  put("/.rytm/ins/AR PERC/k002.txt", "V2 A DRIVE 40 0 0 0\n");
  put("/.rytm/seq/AR PERC/p001.sqp", sqp(3));
  put("/.rytm/seq/AR PERC/p002.sqp", sqp(2));
  put("/.rytm/seq/AR PERC/tracks.txt", "T1 V1\n");
  put("/.rytm/seq/AR PERC/p001.sqs", sqp(3));   // 1042: the saved state, the previous working copy
  put("/.rytm/seq/AR PERC/p001.prv", sqp(3));
  put("/.rytm/songs/AR PERC/take-001.json", "{\"take\":1}");
  put("/.rytm/songs/AR PERC/take-002.jsonl", "{\"journal\":1}\n");   // an open journal: left out
  put("/.rytm/songs/AR PERC/.backup-1.json", "{}");                  // a backup: left out
  put("/Plugins/sc.test-1.0.0.rmp", package("sc.test", 0x010000, 1));
  put("/Plugins/other.rmp", package("sc.other", 0x020000, 2));
  put("/Bundles/Rytm/AR PERC.arprj", std::string(5000, 'R'));
  put("/.rytm/sendfx.txt", "out=fx\n");
  put("/.rytm/wifi.txt", "secret=1\n");   // never in a bundle
}

// ---- the environment: full rate, the current project
static IoRule g_rule = {kPiece, kPiece, 0, ""};
static void rules(IoRule* r) { *r = g_rule; }
static void current(char* out, int cap) { snprintf(out, cap, "%s", g_current); }
static int g_steps = 0;
static void run(int max_steps = 200000) {
  for (int i = 0; i < max_steps && busy(); ++i) {
    step();
    ++g_steps;
    if (status().phase == kReady || status().phase == kServe || status().phase == kDone || status().phase == kFailed ||
        status().phase == kCancelled)
      break;
  }
}
static std::string plan() {
  static char b[1 << 16];
  int n = plan_json(b, sizeof b);
  return std::string(b, n > 0 ? n : 0);
}
static std::string export_card(const char* key, uint32_t epoch, bool takes = true) {
  ExportOpts o;
  o.epoch = epoch, o.unused_takes = takes;
  CHECK(start_export(key, kToCard, o));
  run();
  if (status().phase != kDone) fprintf(stderr, "export: %s %s\n", phase_name(status().phase), status().msg);
  CHECK(status().phase == kDone);
  std::string name = bundle_name();
  finish();
  return "/Bundles/" + name;
}
static void keep(const std::string& card_path, const char* as) {
  const char* out = getenv("BUNDLE_OUT");
  if (!out) return;
  system(("mkdir -p '" + std::string(out) + "' && cp '" + g_card + card_path + "' '" + out + "/" + as + "'").c_str());
}
static bool verify(const std::string& zip) {
  CHECK(start_verify(zip.c_str()));
  run();
  return status().phase == kReady;
}
static void import(const ImportOpts& o) {
  CHECK(start_import(o));
  run();
  if (status().phase != kDone) fprintf(stderr, "import: %s %s\n", phase_name(status().phase), status().msg);
  CHECK(status().phase == kDone);
}
// ---- slot mode (2026-10-29: a card keyed by the Rytm's slots, core/bundle/prjslot.h): the card migrated, and the
// Rytm's slot list as rui1052 sends it (4B 08 x 16, then 4B 09 with the count)
static void slot_card(bool on) {
  prjslot::reset();
  if (!on) return;
  put("/.rytm/prjslot.txt", "# test\nmode=slot\ntrash_next=1\n");
  prjslot::load();
  CHECK(prjslot::migrated());
}
static void rytm_list(const std::map<int, std::string>& names) {
  int occ = 0;
  for (int first = 1; first <= prjslot::kSlots; first += 8) {
    std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x4B, 0x01, 0x08, (uint8_t)(first >> 7), (uint8_t)(first & 0x7F), 8};
    for (int k = first; k < first + 8; ++k) {
      auto it = names.find(k);
      m.push_back(it != names.end() ? 1 : 0);
      occ += it != names.end();
      for (int c = 0; c < 15; ++c) m.push_back(it != names.end() && c < (int)it->second.size() ? (uint8_t)it->second[c] : 0);
    }
    m.push_back(0xF7);
    prjslot::sysex(m.data(), (int)m.size());
  }
  uint8_t end[] = {0xF0, 0x7D, 0x52, 0x54, 0x4B, 0x01, 0x09, (uint8_t)(occ >> 7), (uint8_t)(occ & 0x7F), 0, 0, 0xF7};
  prjslot::sysex(end, (int)sizeof end);
}
static int plan_slot() {   // the plan's target slot (0: none in the plan)
  std::string p = plan();
  size_t m = p.find("\"mode\":\"");
  size_t at = m == std::string::npos ? m : p.find('"', m + 8) + 1;   // ("mode":"new","slot":N)
  return at == std::string::npos || p.compare(at, 8, ",\"slot\":") ? 0 : atoi(p.c_str() + at + 8);
}

// the zip as the reader sees it: entry -> bytes
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

int main() {
  char tmpl[] = "/tmp/bundle-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_root = tmpl;
  host_fake_time(true, 1000000);
  cardq::reset();
  Env env{};
  env.rules = rules;
  env.current = current;
  env.version = "0.2.0-test";
  env.board = "mac";
  env.flush_ms = 0;
  set_env(env);
  reset_for_tests();
  projects::reset_for_tests();
  const uint32_t kEpoch = 1792677780u;   // 2026-10-22 14:03:00 UTC

  // ================= export (card A)
  use_card("A");
  card_ok = true;
  fixture();
  std::string zA = export_card("AR PERC", kEpoch);
  CHECK(zA == "/Bundles/AR PERC 20261022-1403.sidecar.zip");
  CHECK(!there(zA + ".part"));
  std::string za = get(zA);
  keep(zA, "ar-perc.sidecar.zip");
  auto ents = unzip(za);
  // the layout: manifest first, project files under their fixed names, samples by card path, the package, the Rytm file
  CHECK(ents.begin()->first != "" && unzip(za).count("manifest.json"));
  for (const char* n : {"project/samples.pool.txt", "project/wavetables.pool.txt", "project/instruments.pool.txt",
                        "project/pool-used.txt", "project/slots.txt", "project/slots.inf", "project/fxslots.txt",
                        "project/juno.txt", "project/fm.txt", "project/lfo.txt", "project/lfo/k001.txt", "project/ins.txt",
                        "project/ins/k002.txt", "project/seq/p001.sqp", "project/seq/p002.sqp", "project/seq/tracks.txt",
                        "project/songs/take-001.json", "files/Samples/Drums/kick.wav", "files/Samples/Drums/snare.wav",
                        "files/Samples/Banks/b1.wav", "files/Samples/Abs/d4.wav", "files/GRANULAR/pad.wav",
                        "files/GRANULAR/sub/air.wav", "files/WAVETABLES/saw.wav", "files/MULTISAMPLE/Piano/Piano.sfz",
                        "files/MULTISAMPLE/Piano/samples/c4.wav", "files/Samples/AR_PERC/SMP_001.wav",
                        "files/Samples/AR_PERC/SMP_002.wav", "plugins/sc.test-1.0.0.a64.rmp", "rytm/AR PERC.arprj"}) {
    if (!ents.count(n)) fprintf(stderr, "missing entry %s\n", n);
    CHECK(ents.count(n));
  }
  // left out: the copy (stored once), the journal, the backup, secrets, globals, unreferenced samples, other packages
  for (const char* n : {"files/Samples/Drums/kick copy.wav", "project/songs/take-002.jsonl", "project/songs/.backup-1.json",
                        "files/Samples/Other/unused.wav", "plugins/sc.other-2.0.0.a64.rmp", "project/pool.txt",
                        "project/granular.pool.txt"})
    CHECK(!ents.count(n));
  for (auto& kv : ents) CHECK(kv.first.find("wifi") == std::string::npos && kv.first.find("sendfx") == std::string::npos);
  CHECK(ents["project/samples.pool.txt"] == get(list_of("AR PERC")));   // byte for byte
  CHECK(ents["files/Samples/Drums/kick.wav"] == get("/Samples/Drums/kick.wav"));
  CHECK(ents["plugins/sc.test-1.0.0.a64.rmp"] == get("/Plugins/sc.test-1.0.0.rmp"));
  std::string man = ents["manifest.json"];
  CHECK(za.compare(4096, 2, "{\"") == 0);   // the manifest's data at the first boundary
  CHECK(man.find("\"paths\":[\"/Samples/Drums/kick.wav\",\"/Samples/Drums/kick copy.wav\"]") != std::string::npos);
  CHECK(man.find("\"missing\":[\"/Samples/Drums/gone.wav\"]") != std::string::npos);
  CHECK(man.find("\"name\":\"files/MULTISAMPLE/Piano/Piano.sfz\",\"kind\":\"sfz\"") != std::string::npos);
  CHECK(man.find("\"abs_refs\":true") != std::string::npos);
  CHECK(man.find("\"name\":\"files/Samples/AR_PERC/SMP_001.wav\",\"kind\":\"take\"") != std::string::npos);
  CHECK(man.find("\"name\":\"files/Samples/AR_PERC/SMP_002.wav\",\"kind\":\"sample\"") != std::string::npos);
  CHECK(man.find("\"format\":{\"name\":\"sqp\",\"version\":3}") != std::string::npos);
  CHECK(man.find("\"format\":{\"name\":\"sqp\",\"version\":2}") != std::string::npos);
  CHECK(man.find("\"name\":\"project/seq/p001.sqs\",\"kind\":\"seq\"") != std::string::npos);
  CHECK(man.find("\"format\":{\"name\":\"prv\",\"version\":3}") != std::string::npos);
  CHECK(man.find("\"slots\":[{\"slot\":1,\"id\":\"sc.test\",\"ver\":\"1.0.0\",\"name\":\"TEST\",\"fp\":1829,\"packages\":{\"a64\":\"plugins/sc.test-1.0.0.a64.rmp\"}}]") != std::string::npos);
  CHECK(man.find("\"prints\":[{\"slot\":1,\"fp\":1829,\"n\":0}]") != std::string::npos);
  CHECK(man.find("\"fxprints\":[{\"slot\":1,\"fp\":77,\"name\":\"SHIMMER\"}]") != std::string::npos);
  CHECK(man.find("bank_a") == std::string::npos && man.find("\"pools\"") == std::string::npos);   // (pools design 6)
  CHECK(man.find("\"name\":\"project/samples.pool.txt\",\"kind\":\"pool\"") != std::string::npos);
  CHECK(man.find("\"format\":{\"name\":\"wtpool\",\"version\":1}") != std::string::npos);
  CHECK(man.find("\"slots\":1,\"samplepool\":1,\"wtpool\":1,\"instpool\":1,\"poolused\":1}") != std::string::npos);
  CHECK(man.find("\"sendfx_route\":\"fx\"") != std::string::npos);
  CHECK(man.find("\"created\":\"2026-10-22T14:03:00Z\"") != std::string::npos);
  CHECK(man.find("\"rytm_file\":{\"name\":\"rytm/AR PERC.arprj\",\"size\":5000") != std::string::npos);
  // every sample's sha256 as listed
  {
    uint8_t h[32];
    char x[65];
    sha256(ents["files/GRANULAR/pad.wav"].data(), ents["files/GRANULAR/pad.wav"].size(), h);
    hex(h, 32, x);
    CHECK(man.find(std::string("\"name\":\"files/GRANULAR/pad.wav\",\"kind\":\"sample\",\"size\":") +
                   std::to_string(ents["files/GRANULAR/pad.wav"].size()) + ",\"sha256\":\"" + x + "\"") != std::string::npos);
  }
  printf("test_bundle: AR PERC: %zu entries, %zu bytes, %d steps\n", ents.size(), za.size(), g_steps);

  // ---- the same export for the browser: any range served equals the card's file
  {
    ExportOpts o;
    o.epoch = kEpoch;
    CHECK(start_export("AR PERC", kToBrowser, o));
    run();
    CHECK(status().phase == kServe && status().bytes == za.size());
    std::string got;
    std::vector<uint8_t> b(kPiece);
    for (uint64_t off = 0; off < za.size();) {
      uint32_t n = 0;
      CHECK(serve(off, b.data(), (uint32_t)std::min<uint64_t>(65536, za.size() - off), &n) && n > 0);
      got.append((const char*)b.data(), n);
      off += n;
    }
    CHECK(got == za);
    uint32_t n;
    CHECK(serve(4093, b.data(), 7, &n) && n == 7 && !memcmp(b.data(), za.data() + 4093, 7));
    CHECK(!serve(za.size(), b.data(), 1, &n));
    // a file changed while served: refused ("changed during export")
    put("/Samples/Drums/snare.wav", wav(9601, 200));
    bool bad = false;
    for (uint64_t off = 0; off < za.size() && !bad; off += 65536) bad = !serve(off, b.data(), (uint32_t)std::min<uint64_t>(65536, za.size() - off), &n);
    CHECK(bad && status().phase == kFailed && strstr(status().msg, "changed during export"));
    finish();
    put("/Samples/Drums/snare.wav", wav(9600, 200));
  }
  // ---- without the unused sampler takes; busy while a job runs
  {
    std::string z2 = export_card("AR PERC", kEpoch + 60, false);
    auto e2 = unzip(get(z2));
    CHECK(!e2.count("files/Samples/AR_PERC/SMP_001.wav") && e2.count("files/Samples/AR_PERC/SMP_002.wav"));
    system(("rm '" + g_card + z2 + "'").c_str());
    ExportOpts o;
    CHECK(start_export("AR PERC", kToBrowser, o));
    CHECK(!start_export("AR PERC", kToCard, o) && !start_verify(zA.c_str()));   // one job at a time
    cancel();
    run();
    finish();
    CHECK(!start_export("NO SUCH", kToCard, o));
  }
  // ---- a song take recording blocks the export of the current project
  {
    env.song_recording = [] { return true; };
    set_env(env);
    ExportOpts o;
    CHECK(start_export("AR PERC", kToCard, o));
    run();
    CHECK(status().phase == kFailed && strstr(status().msg, "song"));
    finish();
    env.song_recording = nullptr;
    set_env(env);
  }

  // the source card's files, to compare with
  use_card("A");
  std::map<std::string, std::string> a_files;
  for (const char* s : {"/Samples/Drums/kick.wav", "/Samples/Drums/kick copy.wav", "/Samples/Drums/snare.wav", "/Samples/Banks/b1.wav",
                        "/Samples/Abs/d4.wav", "/Samples/AR_PERC/SMP_001.wav", "/Samples/AR_PERC/SMP_002.wav", "/GRANULAR/pad.wav",
                        "/GRANULAR/sub/air.wav", "/WAVETABLES/saw.wav", "/MULTISAMPLE/Piano/Piano.sfz", "/MULTISAMPLE/Piano/samples/c4.wav",
                        "/.rytm/projects/AR PERC/samples.pool.txt", "/.rytm/projects/AR PERC/wavetables.pool.txt",
                        "/.rytm/projects/AR PERC/instruments.pool.txt", "/.rytm/projects/AR PERC/pool-used.txt", "/.rytm/seq/AR PERC/p001.sqp",
                        "/.rytm/lfo/AR PERC/k001.txt", "/.rytm/slots/AR PERC.txt", "/Plugins/sc.test-1.0.0.rmp"})
    a_files[s] = get(s);
  // ================= import onto an empty card (B): every file back, the scanned folders' files under /Samples/Projects
  use_card("B");
  put("/Bundles/incoming/ar-perc.sidecar.zip", za);
  auto before_b = tree();
  CHECK(verify("/Bundles/incoming/ar-perc.sidecar.zip"));
  CHECK(tree() == before_b);   // VERIFY and PLAN write nothing
  std::string p = plan();
  CHECK(p.find("\"name\":\"AR PERC\"") != std::string::npos);
  CHECK(p.find("\"machines\":[{\"slot\":1,\"id\":\"sc.test\",\"ver\":\"1.0.0\",\"name\":\"TEST\",\"action\":\"install\"") != std::string::npos);
  CHECK(p.find("Elektron Transfer") != std::string::npos);
  g_current[0] = 0;
  import(ImportOpts());
  const std::string R = "/Samples/Projects/AR PERC";
  for (const char* s : {"/Samples/Drums/kick.wav", "/Samples/Drums/kick copy.wav", "/Samples/Drums/snare.wav", "/Samples/Banks/b1.wav",
                        "/Samples/Abs/d4.wav", "/Samples/AR_PERC/SMP_001.wav", "/Samples/AR_PERC/SMP_002.wav",
                        "/.rytm/seq/AR PERC/p001.sqp", "/.rytm/lfo/AR PERC/k001.txt", "/.rytm/slots/AR PERC.txt"})
    CHECK(get(s) == a_files[s]);
  for (const char* s : {"/GRANULAR/pad.wav", "/GRANULAR/sub/air.wav", "/WAVETABLES/saw.wav", "/MULTISAMPLE/Piano/samples/c4.wav"}) {
    CHECK(!there(s));
    CHECK(get(R + s) == a_files[s]);
  }
  CHECK(get("/Plugins/sc.test-1.0.0.rmp") == a_files["/Plugins/sc.test-1.0.0.rmp"]);
  CHECK(get("/Bundles/Rytm/AR PERC.arprj") == std::string(5000, 'R'));
  // the pool lists as they were, byte for byte (no path rewriting: their refs resolve by hash, the hints are refreshed
  // at the next save); the old per-machine pools not carried
  for (const char* s : {"/.rytm/projects/AR PERC/samples.pool.txt", "/.rytm/projects/AR PERC/wavetables.pool.txt",
                        "/.rytm/projects/AR PERC/instruments.pool.txt", "/.rytm/projects/AR PERC/pool-used.txt"})
    CHECK(get(s) == a_files[s]);
  CHECK(!there("/.rytm/pools/AR PERC.txt") && !there("/.rytm/granular/AR PERC.pool.txt"));
  // the instrument moved whole with the samples it names relatively; its absolutely named sample was free at its own
  // path (written there), so the .sfz is as it was
  CHECK(get(R + "/MULTISAMPLE/Piano/Piano.sfz") == a_files["/MULTISAMPLE/Piano/Piano.sfz"]);
  CHECK(get("/Samples/Abs/d4.wav") == a_files["/Samples/Abs/d4.wav"]);
  CHECK(!there("/Bundles/.staging") || tree().size());   // (the staging folder is gone: below)
  for (auto& kv : tree()) CHECK(kv.first.find("/Bundles/.staging/") == std::string::npos);
  // the index: imported, not yet named by the Rytm (the control core applies the request)
  for (int i = 0; i < 4; ++i) projects::save();
  CHECK(get("/.rytm/projects.txt").find("AR PERC\tAR PERC\t0\t-\ti\n") != std::string::npos);
  CHECK(plan().find("save it on the Rytm as AR PERC") != std::string::npos);
  finish();
  // export again from B: every sample's content the same (relocated ones too: the lists' refs found by their hash in
  // the library)
  {
    g_current[0] = 0;
    lib::reset_for_tests();
    lib::begin(true);
    for (int i = 0; i < 100000 && (lib::step() || lib::busy()); ++i) {}
    std::string zB = export_card("AR PERC", kEpoch);
    auto eb = unzip(get(zB));
    std::multiset<std::string> sa, sb;
    auto sfz = [](const std::string& n) { return n.size() > 4 && n.compare(n.size() - 4, 4, ".sfz") == 0; };
    for (auto& kv : ents) if (!kv.first.compare(0, 6, "files/") && !sfz(kv.first)) sa.insert(kv.second);
    for (auto& kv : eb) if (!kv.first.compare(0, 6, "files/") && !sfz(kv.first)) sb.insert(kv.second);
    // B has no unused /GRANULAR etc. but holds the same samples; the copy at two paths is stored once again
    CHECK(sa == sb);
    CHECK(eb["project/seq/p001.sqp"] == ents["project/seq/p001.sqp"]);
    CHECK(eb.count("files/Samples/Projects/AR PERC/GRANULAR/pad.wav") && !eb.count("files/GRANULAR/pad.wav"));
    system(("rm '" + g_card + zB + "'").c_str());
    lib::reset_for_tests();
  }
  // a project with nothing in the scanned folders: export -> import on an empty card -> export: the same bundle, byte
  // for byte
  {
    use_card("A");
    put(list_of("LIVE SET"), pool_list("samplepool", {{"A001", "/Samples/Drums/snare.wav"}, {"C001", "/Samples/Banks/b1.wav"}}));
    put("/.rytm/seq/LIVE SET/p001.sqp", sqp(3));
    g_current[0] = 0;   // not the Rytm's project
    std::string z1 = export_card("LIVE SET", kEpoch);
    std::string b1 = get(z1);
    keep(z1, "live-set.sidecar.zip");
    use_card("E");
    put("/Bundles/incoming/l.sidecar.zip", b1);
    put("/.rytm/sendfx.txt", "out=fx\n");   // the same global setting (recorded as information)
    CHECK(verify("/Bundles/incoming/l.sidecar.zip"));
    import(ImportOpts());
    finish();
    std::string z2 = export_card("LIVE SET", kEpoch);
    CHECK(get(z2) == b1);
  }

  // ================= the same card: dedupe (no sample bytes), the name "AR PERC 2", the package reused
  use_card("A");
  g_current[0] = 0;
  put("/Bundles/incoming/again.sidecar.zip", za);
  CHECK(verify("/Bundles/incoming/again.sidecar.zip"));
  p = plan();
  CHECK(p.find("\"name\":\"AR PERC 2\"") != std::string::npos);
  CHECK(p.find("\"new_sample_bytes\":0,") != std::string::npos);
  CHECK(p.find("\"action\":\"reuse\"") != std::string::npos);
  auto sam_before = tree();
  import(ImportOpts());
  finish();
  auto sam_after = tree();
  for (auto& kv : sam_after)   // nothing new outside the new project's files, the index and the bundles
    if (!sam_before.count(kv.first))
      CHECK(kv.first.find("AR PERC 2") != std::string::npos || kv.first.find("/.rytm/bundles") == 0 ||
            kv.first.find("/.rytm/projects.txt") == 0 || kv.first.find("/.rytm/seq/") == 0 || kv.first.find("/.rytm/lfo/") == 0 ||
            kv.first.find("/.rytm/ins/") == 0 || kv.first.find("/.rytm/songs/") == 0);
  for (auto& kv : sam_before) CHECK(sam_after.count(kv.first) && sam_after[kv.first] == kv.second);   // nothing changed
  CHECK(get(list_of("AR PERC 2")) == get(list_of("AR PERC")));   // as it was
  CHECK(get("/.rytm/seq/AR PERC 2/p001.sqp") == get("/.rytm/seq/AR PERC/p001.sqp"));
  CHECK(get("/Bundles/Rytm/AR PERC 2.arprj") == std::string(5000, 'R'));
  // and again: "AR PERC 3"
  CHECK(verify("/Bundles/incoming/again.sidecar.zip"));
  CHECK(plan().find("\"name\":\"AR PERC 3\"") != std::string::npos);
  cancel();
  run();
  finish();

  // ================= conflicts (card C): a different file at a sample's path, the same package with another sha
  use_card("C");
  put("/Samples/Drums/kick.wav", "not the kick");
  put("/Plugins/sc.test-1.0.0.rmp", package("sc.test", 0x010000, 9));
  put("/Bundles/incoming/c.sidecar.zip", za);
  CHECK(verify("/Bundles/incoming/c.sidecar.zip"));
  p = plan();
  CHECK(p.find("\"action\":\"conflict\"") != std::string::npos);
  import(ImportOpts());
  finish();
  CHECK(get("/Samples/Drums/kick.wav") == "not the kick");   // never touched
  CHECK(get("/Samples/Projects/AR PERC/Samples/Drums/kick.wav") == a_files["/Samples/Drums/kick.wav"]);
  CHECK(get("/Samples/Drums/kick copy.wav") == a_files["/Samples/Drums/kick.wav"]);   // that path was free
  CHECK(get(list_of("AR PERC")) == a_files[list_of("AR PERC")]);   // (the relocated kick: found by its hash)
  CHECK(get("/Plugins/sc.test-1.0.0.rmp") == package("sc.test", 0x010000, 9));   // the installed one stays
  // packages left out (the summary's untick): nothing in /Plugins
  use_card("D");
  put("/Bundles/incoming/d.sidecar.zip", za);
  CHECK(verify("/Bundles/incoming/d.sidecar.zip"));
  ImportOpts nop;
  nop.packages = false;
  import(nop);
  finish();
  CHECK(!there("/Plugins/sc.test-1.0.0.rmp") && there("/.rytm/slots/AR PERC.txt"));

  // ================= the library is the dedupe index (core/lib, docs/LIBRARY.md; was /.rytm/bundles/imported.txt): a
  // file written by an import is found by its hash at its new path (read again before it is reused)
  use_card("C");
  lib::reset_for_tests();
  lib::begin(true);
  for (int i = 0; i < 100000 && (lib::step() || lib::busy()); ++i) {}   // (the card core: the walk, every file hashed)
  CHECK(lib::stats().state == lib::kReady && lib::stats().hashed > 0);
  put("/Bundles/incoming/c2.sidecar.zip", za);
  CHECK(verify("/Bundles/incoming/c2.sidecar.zip"));
  p = plan();
  CHECK(p.find("\"name\":\"AR PERC 2\"") != std::string::npos);
  import(ImportOpts());
  finish();
  // the kick found by its hash: the copy in its own folder ("kick copy.wav", the library's rule for a moved file:
  // the hint's folder first), the rest as the first import placed it
  CHECK(get(list_of("AR PERC 2")) == a_files[list_of("AR PERC")]);
  CHECK(!there("/Samples/Projects/AR PERC 2/Samples") && !there("/Samples/Projects/AR PERC 2/GRANULAR"));
  CHECK(!there("/.rytm/bundles/imported.txt"));
  // an import's files go into the library with the hash its STAGE read back: nothing read again
  use_card("E");
  lib::reset_for_tests();
  lib::begin(true);
  for (int i = 0; i < 100000 && (lib::step() || lib::busy()); ++i) {}
  put("/Bundles/incoming/e.sidecar.zip", za);
  CHECK(verify("/Bundles/incoming/e.sidecar.zip"));
  uint64_t lib_read = lib::work().read;
  import(ImportOpts());
  finish();
  for (int i = 0; i < 100000 && (lib::step() || lib::busy()); ++i) {}
  CHECK(lib::work().read == lib_read);
  {
    lib::Ref r;
    CHECK(lib::ref_for_path("/Samples/Drums/kick.wav", &r) && r.has_sha);
    uint8_t h[32];
    std::string k = get("/Samples/Drums/kick.wav");
    sha256(k.data(), k.size(), h);
    CHECK(!memcmp(r.sha, h, 32) && r.size == k.size());
    lib::Resolved o;
    CHECK(lib::resolve(r, &o) == lib::kExact);
  }
  lib::reset_for_tests();

  // ================= damage: one flipped byte in each region refused, the card unchanged
  use_card("F");
  {
    auto e = unzip(za);
    ZRead r;
    char err[160];
    CHECK(eocd((const uint8_t*)za.data() + za.size() - 22, 22, za.size(), &r, err, sizeof err));
    size_t mpos = za.find("\"format\":\"sidecar-bundle\"");
    size_t dpos = za.find(e["files/Samples/Drums/snare.wav"].substr(0, 64)) + 100;
    size_t ppos = za.find(e["plugins/sc.test-1.0.0.a64.rmp"].substr(0, 32)) + 50;
    for (size_t at : {mpos + 3, (size_t)(za.find("project/samples.pool.txt") + 2), dpos, ppos, (size_t)r.cd_off + 50, za.size() - 6}) {
      std::string bad = za;
      bad[at] ^= 0x20;
      put("/Bundles/incoming/bad.sidecar.zip", bad);
      auto t0 = tree();
      CHECK(!verify("/Bundles/incoming/bad.sidecar.zip"));
      CHECK(status().phase == kFailed && status().msg[0]);
      finish();
      CHECK(tree() == t0);
    }
  }

  // ================= a power cut after every change of an import: the recovery leaves "before" or "after"
  {
    use_card("G0");
    put("/Samples/Drums/kick.wav", "different");   // a relocation and a write at the original path both happen
    put("/Bundles/incoming/g.sidecar.zip", za);
    clone_card("G0", "G1");
    use_card("G1");
    CHECK(verify("/Bundles/incoming/g.sidecar.zip"));
    uint64_t c0 = host_changes();
    import(ImportOpts());
    finish();
    uint64_t changes = host_changes() - c0;
    auto after = tree();
    use_card("G0");
    auto before = tree();
    printf("test_bundle: an import of %zu entries: %llu changes to the card, a cut after each\n", ents.size(), (unsigned long long)changes);
    int n_before = 0, n_after = 0;
    for (uint64_t k = 0; k <= changes; k += (k < 40 || changes - k < 40) ? 1 : 7) {
      clone_card("G0", "Gk");
      use_card("Gk");
      CHECK(verify("/Bundles/incoming/g.sidecar.zip"));
      host_power_cut((int64_t)k);
      CHECK(start_import(ImportOpts()));
      run();
      host_power_cut(-1);
      reset_for_tests();   // the reboot
      recover();
      auto t = tree();
      bool b = t == before, a = t == after;
      if (!a && !b) fprintf(stderr, "cut after %llu: neither (vs after:%s) (vs before:%s)\n", (unsigned long long)k, diff(after, t).c_str(), diff(before, t).c_str());
      CHECK(a || b);
      n_before += b, n_after += a;
    }
    printf("test_bundle: cuts: %d left the card as before, %d as after\n", n_before, n_after);
    CHECK(n_before > 0 && n_after > 0);
  }

  // ================= the same in replace mode: the old project deleted, the new one written - a cut anywhere, then the
  // boot's roll-forward, never loses the new files (a replayed delete must not remove a renamed file)
  {
    use_card("K0");
    fixture();
    put(list_of("AR PERC"), pool_list("samplepool", {{"A002", "/Samples/Drums/snare.wav"}}));   // the project here: replaced
    put("/.rytm/seq/AR PERC/p009.sqp", sqp(3));              // a file the bundle does not have: deleted
    put("/Bundles/incoming/g.sidecar.zip", za);
    clone_card("K0", "K1");
    use_card("K1");
    CHECK(verify("/Bundles/incoming/g.sidecar.zip"));
    ImportOpts rep;
    rep.mode = kReplace;
    g_current[0] = 0;
    uint64_t c0 = host_changes();
    import(rep);
    finish();
    uint64_t changes = host_changes() - c0;
    CHECK(get(list_of("AR PERC")) == a_files[list_of("AR PERC")] && !there("/.rytm/seq/AR PERC/p009.sqp"));
    CHECK(tree().size() > 0);
    bool backup = false;
    for (auto& kv : tree()) backup |= kv.first.find("/Bundles/.backup/AR PERC ") == 0;
    CHECK(backup);
    auto after = tree();
    use_card("K0");
    auto before = tree();
    int nb = 0, na = 0;
    for (uint64_t k = 0; k <= changes; k += (k < 30 || changes - k < 60) ? 1 : 9) {
      clone_card("K0", "Kk");
      use_card("Kk");
      CHECK(verify("/Bundles/incoming/g.sidecar.zip"));
      host_power_cut((int64_t)k);
      CHECK(start_import(rep));
      run();
      host_power_cut(-1);
      reset_for_tests();
      recover();
      auto t = tree();
      // the backup is written before the staging (an export): a cut may leave it (or its .part) - not the project
      for (auto it = t.begin(); it != t.end();) it = it->first.find("/Bundles/.backup") == 0 ? t.erase(it) : std::next(it);
      auto a2 = after, b2 = before;
      for (auto* m : {&a2, &b2})
        for (auto it = m->begin(); it != m->end();) it = it->first.find("/Bundles/.backup") == 0 ? m->erase(it) : std::next(it);
      bool b = t == b2, a = t == a2;
      if (!a && !b) fprintf(stderr, "replace, cut after %llu: neither (vs after:%s) (vs before:%s)\n", (unsigned long long)k, diff(a2, t).c_str(), diff(b2, t).c_str());
      CHECK(a || b);
      nb += b, na += a;
    }
    printf("test_bundle: replace: %llu changes; cuts: %d as before, %d as after\n", (unsigned long long)changes, nb, na);
    CHECK(nb > 0 && na > 0);
  }

  // ================= untrusted names: a key that is not a key, a path named twice
  {
    use_card("A");
    CHECK(!start_delete("../Samples", "../Samples") && !start_delete("..", ".."));
    ExportOpts o;
    CHECK(!start_export("../../Samples", kToCard, o));
  }

  // ================= over 4 GiB: refused with what takes the space, nothing written
  {
    use_card("H");
    for (auto& [n, sz] : std::vector<std::pair<std::string, off_t>>{{"/Samples/big1.wav", 3000000000ll}, {"/Samples/big2.wav", 1500000000ll}}) {
      put(n, "");
      FILE* f = fopen((g_card + n).c_str(), "wb");
      CHECK(f && ftruncate(fileno(f), sz) == 0);   // sparse: never read
      fclose(f);
    }
    put(list_of("BIG"), pool_list("samplepool", {{"A001", "/Samples/big1.wav"}, {"A002", "/Samples/big2.wav"}}));
    auto t0 = tree();
    ExportOpts o;
    CHECK(start_export("BIG", kToCard, o));
    run();
    CHECK(status().phase == kFailed && strstr(status().msg, "4 GB") && strstr(status().msg, "big1.wav"));
    printf("test_bundle: over 4 GiB: %s\n", status().msg);
    finish();
    CHECK(tree() == t0);
  }

  // ================= delete: project files only, samples stay; never the current project; the confirm must match
  {
    use_card("A");
    strcpy(g_current, "AR PERC");
    CHECK(!start_delete("AR PERC", "AR PERC") && strstr(status().msg, "current"));
    CHECK(!start_delete("AR PERC 2", "AR PERC"));
    CHECK(start_delete("AR PERC 2", "AR PERC 2"));
    run();
    CHECK(status().phase == kDone);
    finish();
    trash_jobs();   // (2026-10-27: into the project trash, never deleted)
    CHECK(trashed("AR PERC 2", "projects!%/samples.pool.txt") || trashed("AR PERC 2", "seq!%"));
    CHECK(!there(list_of("AR PERC 2")) && !there("/.rytm/projects/AR PERC 2") && !there("/.rytm/seq/AR PERC 2") &&
          !there("/.rytm/lfo/AR PERC 2"));
    CHECK(there(list_of("AR PERC")) && there("/Samples/Drums/kick.wav") && there("/GRANULAR/pad.wav"));
    CHECK(!project_exists("AR PERC 2") && project_exists("AR PERC"));
    static char js[1 << 16];
    int n = projects_json(js, sizeof js);
    CHECK(n > 0 && strstr(js, "\"key\":\"AR PERC\"") && strstr(js, "\"key\":\"LIVE SET\"") && !strstr(js, "AR PERC 2"));
    n = bundles_json("/Bundles", js, sizeof js);
    CHECK(n > 0 && strstr(js, "AR PERC 20261022-1403.sidecar.zip"));
  }

  // ================= the boot recovery on its own: an incomplete staging folder goes, a complete commit rolls forward
  {
    use_card("R");
    put("/Bundles/.staging/7/Samples/x.wav", "x");
    put("/Bundles/.staging/8/Samples/y.wav", "y");
    put("/Bundles/.staging/8/commit.txt", "R /Bundles/.staging/8/Samples/y.wav /Samples/y.wav\nEND\n");
    put("/Bundles/.staging/9/commit.txt", "R /Bundles/.staging/9/a /Samples/a\n");   // no END: incomplete
    recover();
    CHECK(!there("/Bundles/.staging/7") && !there("/Bundles/.staging/8") && !there("/Bundles/.staging/9"));
    CHECK(get("/Samples/y.wav") == "y" && !there("/Samples/x.wav") && !there("/Samples/a"));
  }

  // ================= slot mode (2026-10-29): the import goes to a slot of the Rytm's, every project file under @NNN
  {
    // the bundles: AR PERC with its Rytm file (za), and one without (zn)
    use_card("SN");
    fixture();
    unlink((g_card + "/Bundles/Rytm/AR PERC.arprj").c_str());
    g_current[0] = 0;
    std::string zn = get(export_card("AR PERC", kEpoch));
    // name mode (a card not migrated): as before - no slot in the plan, the name key
    use_card("S0");
    slot_card(false);
    put("/Bundles/incoming/a.sidecar.zip", za);
    CHECK(verify("/Bundles/incoming/a.sidecar.zip"));
    CHECK(plan().find("\"slots\":[") == std::string::npos && plan_slot() == 0 && plan().find("\"key\":\"AR PERC\"") != std::string::npos);
    cancel(), finish();

    // the default: the first empty slot (no Rytm list: no Sidecar files, no name in the index)
    use_card("S1");
    slot_card(true);
    put("/Bundles/incoming/a.sidecar.zip", za);
    put("/Bundles/incoming/n.sidecar.zip", zn);
    put("/.rytm/lfo/@001.txt", "L1 V1 TIM SIN 40 20\n");   // slot 1: the Sidecar's files
    CHECK(verify("/Bundles/incoming/a.sidecar.zip"));
    std::string p = plan();
    CHECK(plan_slot() == 2 && p.find("\"key\":\"@002\"") != std::string::npos && p.find("\"slots\":[[\"\",1,0],[\"\",0,0],") != std::string::npos);
    CHECK(p.find("Send AR PERC.arprj to the Rytm's slot 002 with Elektron Transfer, then load slot 002 on the Rytm.") != std::string::npos);
    cancel(), finish();
    // the Rytm's list: slots 2..4 hold other projects -> 5; a slot named as the bundle (any case) -> that one
    rytm_list({{2, "OTHER"}, {3, "LIVE"}, {4, "JAM"}});
    CHECK(verify("/Bundles/incoming/a.sidecar.zip") && plan_slot() == 5 && plan().find("\"rytm_list\":true") != std::string::npos);
    cancel(), finish();
    rytm_list({{2, "OTHER"}, {9, "ar perc"}, {12, "AR PERC"}});
    CHECK(verify("/Bundles/incoming/n.sidecar.zip") && plan_slot() == 9);
    p = plan();
    CHECK(p.find("\"name\":\"ar perc\"") != std::string::npos);   // (the Rytm's name for the slot)
    CHECK(p.find("only the Sidecar's side, no Rytm project file. Slot 009 on the Rytm holds ar perc: load it on the Rytm.") != std::string::npos);
    // the import into slot 9: every project file under @009, none under the name; the index names it as the Rytm does
    import(ImportOpts());
    CHECK(get("/.rytm/seq/@009/p001.sqp") == a_files["/.rytm/seq/AR PERC/p001.sqp"] && get("/.rytm/slots/@009.txt") == a_files["/.rytm/slots/AR PERC.txt"]);
    CHECK(get(list_of("@009")) == a_files[list_of("AR PERC")] && there("/.rytm/lfo/@009/k001.txt"));
    CHECK(!there("/.rytm/seq/AR PERC") && !there(list_of("AR PERC")) && !there("/.rytm/lfo/AR PERC.txt"));
    CHECK(there("/Samples/Projects/@009/GRANULAR/pad.wav"));
    CHECK(strstr(status().msg, "imported to slot 009 (ar perc)"));
    finish();
    for (int i = 0; i < 4; ++i) projects::save();
    CHECK(get("/.rytm/projects.txt").find("@009\tar perc\t") != std::string::npos);
    // another slot asked for (the web page's slot=): an empty slot with no Rytm name takes the page's name
    CHECK(verify("/Bundles/incoming/n.sidecar.zip"));
    ImportOpts o;
    o.slot = 47;
    snprintf(o.name, sizeof o.name, "MY PERC");
    import(o);
    CHECK(get("/.rytm/seq/@047/p001.sqp") == a_files["/.rytm/seq/AR PERC/p001.sqp"] && plan_slot() == 47);
    CHECK(plan().find("put the Rytm project into slot 047 yourself") != std::string::npos);
    finish();
    for (int i = 0; i < 4; ++i) projects::save();
    CHECK(get("/.rytm/projects.txt").find("@047\tMY PERC\t") != std::string::npos);
    // the bundle's own slot first: a bundle exported from @047 ("MY PERC") goes back to 47 when the Rytm's 47 is MY PERC,
    // even with the name in a slot before it; to the named slot when 47 holds another project
    std::string zs = get(export_card("@047", kEpoch));
    use_card("S2");
    slot_card(true);
    put("/Bundles/incoming/s.sidecar.zip", zs);
    rytm_list({{5, "MY PERC"}, {47, "MY PERC"}});
    CHECK(verify("/Bundles/incoming/s.sidecar.zip") && plan_slot() == 47);
    cancel(), finish();
    rytm_list({{5, "MY PERC"}, {47, "SOMETHING"}});
    CHECK(verify("/Bundles/incoming/s.sidecar.zip") && plan_slot() == 5);
    cancel(), finish();
    printf("test_bundle: slot mode: the defaults (first empty, the Rytm's name, the bundle's own slot) and slot= work\n");

    // a slot with Sidecar files: refused without replace (READY stays); replace moves them to the project trash (one
    // entry, why=import), restorable; never the Rytm's current slot
    use_card("S1");
    slot_card(true);
    put("/.rytm/seq/@047/p009.sqp", sqp(3));   // a file the bundle does not have
    CHECK(verify("/Bundles/incoming/a.sidecar.zip"));
    o = ImportOpts();
    o.slot = 47;
    CHECK(!start_import(o) && strstr(status().msg, "slot 047 has Sidecar files") && status().phase == kReady);
    o.mode = kReplace;
    snprintf(g_current, sizeof g_current, "@047");
    CHECK(!start_import(o) && strstr(status().msg, "@047 is the Rytm's current project: switch the Rytm to another project to replace it"));
    CHECK(status().phase == kReady && there("/.rytm/seq/@047/p009.sqp"));
    g_current[0] = 0;
    import(o);
    CHECK(strstr(status().msg, "its old Sidecar files are in the project trash"));
    finish();
    CHECK(get("/.rytm/seq/@047/p001.sqp") == a_files["/.rytm/seq/AR PERC/p001.sqp"] && !there("/.rytm/seq/@047/p009.sqp"));
    for (auto& kv : tree()) CHECK(kv.first.find("/.backup") == std::string::npos);   // (the trash is the backup)
    CHECK(trashed("@047", "seq!%/p009.sqp") && trashed("@047", "seq!%/p001.sqp") && trashed("@047", "lfo!%/k001.txt"));
    CHECK(get("/.rytm/trash/000001/info.txt").find("\nwhy=import\n") != std::string::npos);
    static prjslot::TrashRow rows[8];
    CHECK(prjslot::trash_rows(rows, 8) == 1 && rows[0].id == 1 && !strcmp(rows[0].key, "@047") && !strcmp(rows[0].name, "MY PERC"));
    // restored (to another slot: 47 holds the import now)
    CHECK(prjslot::request_restore(1, "@012", false));
    trash_jobs();
    CHECK(get("/.rytm/seq/@012/p009.sqp") == sqp(3) && there("/.rytm/seq/@012/p001.sqp") && !there("/.rytm/trash/000001"));
    printf("test_bundle: slot mode: replace into the trash (%s), restored to @012\n", rows[0].why);
  }
  // ================= review 2026-10-29: an empty per-project folder is no files (the import goes in); every slot taken: no
  // default (picked); the control core's jobs held through an import's commit; a boot roll-forward never replaces a file
  // whose move to the trash did not complete
  {
    use_card("SE");
    slot_card(true);
    put("/Bundles/incoming/a.sidecar.zip", za);
    system(("mkdir -p '" + g_card + "/.rytm/seq/@001' '" + g_card + "/.rytm/kits/@001/sub/deeper' '" + g_card + "/.rytm/projects/@001'").c_str());
    put("/.rytm/songs/@001/.DS_Store", "x");   // (folders with only empty folders or dot files below them: no files)
    CHECK(verify("/Bundles/incoming/a.sidecar.zip") && plan_slot() == 1 && plan().find("\"clash\":false") != std::string::npos);
    import(ImportOpts());
    CHECK(get("/.rytm/seq/@001/p001.sqp") == a_files["/.rytm/seq/AR PERC/p001.sqp"] && get(list_of("@001")) == a_files[list_of("AR PERC")]);
    finish();
    // replace into a slot with files and an empty folder: the folder stays (the new files in it), the files to the trash
    system(("mkdir -p '" + g_card + "/.rytm/kits/@002'").c_str());
    put("/.rytm/lfo/@002.txt", "old lfo");
    CHECK(verify("/Bundles/incoming/a.sidecar.zip"));
    ImportOpts o;
    o.slot = 2, o.mode = kReplace;
    import(o);
    finish();
    CHECK(get("/.rytm/lfo/@002.txt") == "L1 V1 TIM SIN 40 20\n" && get("/.rytm/trash/000001/lfo!%.txt") == "old lfo" && trashed("@002", "lfo!%.txt") && there("/.rytm/kits/@002"));

    // every slot taken on the Rytm: no default - refused until a slot is picked
    std::map<int, std::string> all;
    for (int k = 1; k <= prjslot::kSlots; ++k) all[k] = "P" + std::to_string(k);
    rytm_list(all);
    CHECK(verify("/Bundles/incoming/a.sidecar.zip") && plan_slot() == 0 && plan().find("pick the slot") != std::string::npos);
    PlanInfo pi;
    CHECK(plan_info(&pi) && pi.pick && pi.slot);
    CHECK(!start_import(ImportOpts()) && strstr(status().msg, "pick the slot on the web page") && status().phase == kReady);
    // a refused pick (the current slot; a slot with files, no replace) stays a pick: a bare import is still refused
    snprintf(g_current, sizeof g_current, "@003");
    o = ImportOpts();
    o.slot = 3;
    CHECK(!start_import(o) && strstr(status().msg, "current project"));
    g_current[0] = 0;
    o.slot = 1;   // (slot 1: the first import's files)
    CHECK(!start_import(o) && strstr(status().msg, "slot 001 has Sidecar files"));
    CHECK(!start_import(ImportOpts()) && strstr(status().msg, "pick the slot on the web page") && plan_slot() == 0);
    CHECK(plan_info(&pi) && pi.pick);
    o = ImportOpts();
    o.slot = 9;
    import(o);
    CHECK(get("/.rytm/seq/@009/p001.sqp") == a_files["/.rytm/seq/AR PERC/p001.sqp"] && strstr(status().msg, "slot 009 (P9)"));
    finish();

    // the hold: no prjslot job starts while held (an import's commit); the one queued runs once released
    prjslot::hold_jobs(true);
    CHECK(prjslot::request_trash("@009", "web", "P9"));
    trash_jobs();
    CHECK(there("/.rytm/seq/@009/p001.sqp") && prjslot::busy());
    prjslot::hold_jobs(false);
    trash_jobs();
    CHECK(!there("/.rytm/seq/@009") && trashed("@009", "seq!%/p001.sqp"));
    // an import waits for a running / queued job before its commit, and releases the hold when done or failed
    CHECK(prjslot::request_trash("@001", "web", "AR PERC"));
    CHECK(verify("/Bundles/incoming/a.sidecar.zip"));
    o = ImportOpts();
    o.slot = 20;
    CHECK(start_import(o));
    run(3000);
    CHECK(status().phase != kDone && !there("/.rytm/seq/@020"));   // (held at its commit's check)
    {   // the two cores taking turns: the trash job runs (the hold let go while it is queued), then the import commits
      uint64_t t = 60000000000ull;
      for (int i = 0; i < 400000 && status().phase != kDone && status().phase != kFailed; ++i) {
        step();
        host_fake_time(true, t += 20000);
        fmt::post_step(), pools::step(), prjslot::tick(now_ms());
      }
      host_fake_time(false, 0);
    }
    CHECK(status().phase == kDone && there("/.rytm/seq/@020/p001.sqp") && !there("/.rytm/seq/@001"));
    finish();
    CHECK(prjslot::request_trash("@020", "web", "X"));
    trash_jobs();
    CHECK(!there("/.rytm/seq/@020") && !prjslot::busy());   // (released)

    // the boot's roll-forward: an M that cannot complete (the trash entry's folder is a file) keeps the old file and
    // leaves the import's files for that path out; the rest goes on
    use_card("SR");
    put("/.rytm/lfo/@047.txt", "old lfo");
    put("/.rytm/seq/@047/p001.sqp", "old seq");
    put("/.rytm/trash/000009", "not a folder");
    put("/Bundles/.staging/5/f0000", "new lfo");
    put("/Bundles/.staging/5/f0001", "new seq");
    put("/Bundles/.staging/5/f0002", "new slots");
    put("/Bundles/.staging/5/commit.txt", "M /.rytm/lfo/@047.txt /.rytm/trash/000009/lfo!%.txt\nM /.rytm/seq/@047 /.rytm/trash/000009/seq!%\n"
                                          "R /Bundles/.staging/5/f0000 /.rytm/lfo/@047.txt\nR /Bundles/.staging/5/f0001 /.rytm/seq/@047/p001.sqp\n"
                                          "R /Bundles/.staging/5/f0002 /.rytm/slots/@047.txt\nEND\n");
    recover();
    CHECK(get("/.rytm/lfo/@047.txt") == "old lfo" && get("/.rytm/seq/@047/p001.sqp") == "old seq" && get("/.rytm/slots/@047.txt") == "new slots");
    CHECK(!there("/Bundles/.staging"));
    printf("test_bundle: slot mode review: empty folders, no default when all taken, the hold, an M that failed at boot\n");
    slot_card(false);
  }
  // ================= slot mode's replace cut after every change: the old files (in their slot) or the whole new project
  // with the old in the trash - never a half (the trash moves are lines of the same commit.txt)
  {
    use_card("SK0");
    slot_card(true);
    fixture();
    for (const char* f : {"/.rytm/lfo/@047.txt", "/.rytm/slots/@047.txt", "/.rytm/seq/@047/p001.sqp", "/.rytm/seq/@047/p009.sqp",
                          "/.rytm/projects/@047/samples.pool.txt", "/.rytm/songs/@047/take-001.json"})
      put(f, std::string("old ") + f);
    put("/Bundles/incoming/g.sidecar.zip", za);
    clone_card("SK0", "SK1");
    use_card("SK1");
    slot_card(true);
    CHECK(verify("/Bundles/incoming/g.sidecar.zip"));
    ImportOpts rep;
    rep.mode = kReplace, rep.slot = 47;
    g_current[0] = 0;
    uint64_t c0 = host_changes();
    import(rep);
    finish();
    uint64_t changes = host_changes() - c0;
    CHECK(get("/.rytm/seq/@047/p001.sqp") == a_files["/.rytm/seq/AR PERC/p001.sqp"] && !there("/.rytm/seq/@047/p009.sqp"));
    CHECK(get("/.rytm/trash/000001/seq!%/p009.sqp") == "old /.rytm/seq/@047/p009.sqp");
    auto after = tree();
    use_card("SK0");
    auto before = tree();
    int nb = 0, na = 0;
    for (uint64_t k = 0; k <= changes; k += (k < 30 || changes - k < 60) ? 1 : 7) {
      clone_card("SK0", "SKk");
      use_card("SKk");
      slot_card(true);
      CHECK(verify("/Bundles/incoming/g.sidecar.zip"));
      host_power_cut((int64_t)k);
      CHECK(start_import(rep));
      run();
      host_power_cut(-1);
      reset_for_tests();
      recover();
      auto t = tree();
      bool b = t == before, a = t == after;
      if (!a && !b) fprintf(stderr, "slot replace, cut after %llu: neither (vs after:%s) (vs before:%s)\n", (unsigned long long)k, diff(after, t).c_str(), diff(before, t).c_str());
      CHECK(a || b);
      nb += b, na += a;
    }
    printf("test_bundle: slot replace: %llu changes; cuts: %d as before, %d as after\n", (unsigned long long)changes, nb, na);
    CHECK(nb > 0 && na > 0);
    slot_card(false);
  }

  system(("rm -rf '" + g_root + "'").c_str());
  printf("test_bundle: PASS\n");
  return 0;
}
