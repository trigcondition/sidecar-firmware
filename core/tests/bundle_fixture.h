// core/tests/bundle_fixture.h - a small card with one project for the bundle tests that came after test_bundle.cpp
// (test_bundle_usb.cpp: USB sticks; test_bundle_ui.cpp: the PROJECT pages' feed and the Rytm's PROJECT page): Mac
// folders as cards and sticks, the card's state as a tree of sha256s, a run of the job.
#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "bundle/bundle.h"
#include "bundle/projects.h"
#include "lib/sha256.h"
#include "bundle/zip.h"
#include "check.h"
#include "rmp_api.h"
#include "rmp_format.h"
#include "rmp_loader.h"
#include "wg_util.h"

namespace bfx {

using namespace core;
using namespace core::bundle;

inline std::string& root() { static std::string r; return r; }   // the tests' folder: a card or stick per sub-folder
inline std::string& card_dir() { static std::string c; return c; }   // the card in use

inline std::string mac(const std::string& base, const std::string& path) { return base + path; }
inline void put_at(const std::string& base, const std::string& path, const std::string& s) {
  std::string m = base + path;
  system(("mkdir -p \"$(dirname '" + m + "')\"").c_str());
  FILE* f = fopen(m.c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
inline std::string get_at(const std::string& base, const std::string& path) {
  FILE* f = fopen((base + path).c_str(), "rb");
  if (!f) return "<missing>";
  std::string s;
  char b[65536];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
inline bool there_at(const std::string& base, const std::string& path) {
  struct stat st;
  return stat((base + path).c_str(), &st) == 0;
}
inline void put(const std::string& p, const std::string& s) { put_at(card_dir(), p, s); }
inline std::string get(const std::string& p) { return get_at(card_dir(), p); }
inline bool there(const std::string& p) { return there_at(card_dir(), p); }

inline std::string folder(const std::string& name) {   // a fresh sub-folder of the tests' folder
  std::string d = root() + "/" + name;
  system(("rm -rf '" + d + "' && mkdir -p '" + d + "'").c_str());
  return d;
}
inline void use_card(const std::string& name, bool fresh = true) {
  card_dir() = root() + "/" + name;
  if (fresh) folder(name);
  host_set_card(card_dir().c_str());
  projects::reset_for_tests();
  projects::load();
}

// every file and folder below base with its content's sha256
inline void walk(const std::string& m, const std::string& rel, std::map<std::string, std::string>* out) {
  DIR* d = opendir(m.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string full = m + "/" + n, r = rel + "/" + n;
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
inline std::map<std::string, std::string> tree(const std::string& base) {
  std::map<std::string, std::string> t;
  walk(base, "", &t);
  return t;
}
inline std::string diff(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b) {
  std::string s;
  for (auto& kv : a)
    if (!b.count(kv.first) || b.at(kv.first) != kv.second) s += " -" + kv.first;
  for (auto& kv : b)
    if (!a.count(kv.first)) s += " +" + kv.first;
  return s;
}

inline std::string wav(int frames, double hz) {
  auto v = wg::wav_bytes(frames, 1, 16, 48000, 1, "", [hz](int i, int) { return 0.3 * std::sin(2 * M_PI * hz * i / 48000); });
  return std::string(v.begin(), v.end());
}
inline std::string package(const char* id, uint32_t version, uint8_t salt) {
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

// a project's SAMPLES list (core/lib/pools.h, format samplepool v1): the files in bank A from row 1, refs by path (sha
// "-": the library does not run in these tests; the files must be on the card already, for their sizes)
inline std::string samples_list(const std::string& key) { return "/.rytm/projects/" + key + "/samples.pool.txt"; }
inline std::string samples_text(std::initializer_list<const char*> paths) {
  std::string s = "#sidecar samplepool 1\n";
  int row = 1;
  for (const char* p : paths) {
    char line[256];
    snprintf(line, sizeof line, "A%03d sample - %zu %s\n", row++, get(p).size(), p);
    s += line;
  }
  return s;
}

// one project, "LIVE SET": samples big enough for many pieces (about 1.4 MB), a slot map with a package, songs
inline void make_project(const char* key = "LIVE SET") {
  std::string k = key;
  put("/Samples/Live/loop.wav", wav(480000, 110));   // ~960 KB: 30 stick pieces
  put("/Samples/Live/hit.wav", wav(96000, 220));
  put(samples_list(key), samples_text({"/Samples/Live/loop.wav", "/Samples/Live/hit.wav"}));
  put("/.rytm/slots/" + k + ".txt", "U1 sc.live 1.2.0\n");
  put("/.rytm/lfo/" + k + ".txt", "L1 V1 TIM SIN 40 20\n");
  put("/.rytm/seq/" + k + "/p001.sqp", "SQP1\x03" + std::string(59, '\x11'));
  put("/.rytm/songs/" + k + "/take-001.json", "{\"take\":1}");
  put("/Plugins/sc.live-1.2.0.rmp", package("sc.live", 0x010200, 7));
}

inline int& steps() { static int n = 0; return n; }
inline bool settled() {
  int p = status().phase;
  return p == kReady || p == kServe || p == kDone || p == kFailed || p == kCancelled || p == kIdle;
}
inline void run(int max_steps = 200000) {
  for (int i = 0; i < max_steps && busy(); ++i) {
    step();
    ++steps();
    if (settled()) break;
  }
}
// steps until the phase is p (or the job settles): false when it did not get there
inline bool run_to(int p, int max_steps = 200000) {
  for (int i = 0; i < max_steps && busy(); ++i) {
    if (status().phase == p) return true;
    step();
    ++steps();
  }
  return status().phase == p;
}

}  // namespace bfx
