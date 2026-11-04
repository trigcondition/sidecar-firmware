// core/tests/test_api3_compat.cpp - API 1 / API 2 user machines play the same, sample for sample, on an API 3 host
// (docs/plugin-api-3-plan.md, "Proof that API 2 is unchanged"). Every RMPA package built under plugins/ (make -C
// plugins/<p> a64) is assigned to U1 in turn and played on voice 0 through the engine (a 0x04 trig, a 0x03 knob
// change, a 0x11 note off, the blocks rendered by render_voices); the voice's int16 output is hashed (FNV-1a 64) and
// compared with tests/golden/api2_compat.txt, which an API 2 host (origin/dev before API 3) wrote with
// API2_COMPAT_WRITE=1. The same source builds against both hosts: it uses no API 3 name.
// Without built packages it says so and passes (CI has no AArch64 cross compiler); a package missing from the golden
// file is printed, not failed (a new plugin: write the golden file again on the API 2 host).
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "rmp_format.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"
#include "voices.h"

using namespace core;

#if defined(__aarch64__)
// The user machines' heap (um.cpp's weak um_heap: the package buffer, the voices' states) at a fixed address: some plugins
// seed their noise from their state's address (plugins/hush1), so every run, on either host, must place it alike.
extern "C" void* um_heap(uint32_t bytes) {
  static uint8_t* base = nullptr;
  static size_t used = 0;
  const size_t cap = 512u << 20;
  if (!base) {
    void* p = mmap((void*)0x7f0000000000ull, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    CHECK(p != MAP_FAILED);
    base = (uint8_t*)p;
  }
  size_t n = (bytes + 63u) & ~(size_t)63u;
  if (n > cap - used) return nullptr;
  uint8_t* r = base + used;
  used += n;
  return r;
}
static void out(const uint8_t*, int) {}
static void sink(const char*, int) {}
static std::string slurp(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return "";
  std::string b;
  char t[65536];
  size_t n;
  while ((n = fread(t, 1, sizeof t, f)) > 0) b.append(t, n);
  fclose(f);
  return b;
}
static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
static void sysex(std::vector<uint8_t> m) { engine_sysex(m.data(), (int)m.size()); }
static void idle_all() {
  int quiet = 0;
  for (int i = 0; i < 100000; ++i) {
    if (um::idle()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static std::vector<uint8_t> report(uint8_t cmd, int note, const int* p) {   // 0x04 (note >= 0) or 0x03, voice 0 on U1
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, cmd, 0x50};
  if (note >= 0) m.push_back((uint8_t)note), m.push_back(100);
  for (int k = 0; k < 16; ++k) m.push_back((uint8_t)(p[k] >> 7)), m.push_back((uint8_t)(p[k] & 0x7f));
  m.push_back(0xf7);
  return m;
}
static int16_t g_ring[1024][kVoices];
static uint64_t g_t = 1000000;   // the fake clock (host_fake_time): a block is 250 us, so every run times the same
static uint64_t g_mach[16];
// n blocks of voice 0 into h (FNV-1a 64 over its int16 samples); peak: the largest |sample|
static void render(int blocks, uint64_t* h, int* peak) {
  static int w = 0;
  for (int b = 0; b < blocks; ++b) {
    g_t += 250;
    host_fake_time(true, g_t);
    render_voices(g_ring, 1024, w, 0, kVoices, g_mach);
    for (int s = 0; s < kBlock; ++s) {
      int16_t x = g_ring[(w + s) % 1024][0];
      for (int k = 0; k < 2; ++k) *h = (*h ^ (uint8_t)(x >> (8 * k))) * 0x100000001b3ull;
      int a = x < 0 ? -x : x;
      if (a > *peak) *peak = a;
    }
    w = (w + kBlock) % 1024;
  }
}

int main() {
  const char* pdir = getenv("API2_COMPAT_PLUGINS") ? getenv("API2_COMPAT_PLUGINS") : "../plugins";
  std::vector<std::string> pkgs;   // plugins/<p>/<name>.a64.rmp, in name order
  if (DIR* d = opendir(pdir)) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string sub = std::string(pdir) + "/" + e->d_name;
      if (DIR* d2 = opendir(sub.c_str())) {
        while (dirent* f = readdir(d2)) {
          std::string n = f->d_name;
          // (plugins/fault_test faults on purpose: the guard's tests, not a sound)
          if (n.size() > 8 && n.compare(n.size() - 8, 8, ".a64.rmp") == 0 && n.find("fault") == std::string::npos)
            pkgs.push_back(sub + "/" + n);
        }
        closedir(d2);
      }
    }
    closedir(d);
  }
  std::sort(pkgs.begin(), pkgs.end());
  if (pkgs.empty()) {
    printf("test_api3_compat: no RMPA packages under %s (make -C plugins/<p> a64): nothing compared\n", pdir);
    return 0;
  }
  const char* golden = "tests/golden/api2_compat.txt";
  std::map<std::string, std::string> want;
  {
    std::string g = slurp(golden);
    size_t at = 0;
    while (at < g.size()) {
      size_t e = g.find('\n', at);
      if (e == std::string::npos) e = g.size();
      std::string line = g.substr(at, e - at);
      at = e + 1;
      if (line.empty() || line[0] == '#') continue;
      size_t sp = line.find(' ');
      if (sp != std::string::npos) want[line.substr(0, sp)] = line.substr(sp + 1);
    }
  }
  bool write = getenv("API2_COMPAT_WRITE") != nullptr;

  char tmpl[] = "/tmp/core-api3c-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_set_log_sink(sink);
  host_fake_time(true, g_t);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/projects/UMC"));
  put("/.rytm/project", "UMC");
  std::vector<std::string> ids;
  for (size_t i = 0; i < pkgs.size(); ++i) {
    std::string b = slurp(pkgs[i]);
    CHECK(b.size() > sizeof(rmp_header));
    rmp_header h;
    memcpy(&h, b.data(), sizeof h);
    ids.push_back(std::string(h.id, strnlen(h.id, sizeof h.id)));
    char n[32];
    snprintf(n, sizeof n, "/Plugins/p%02zu.rmp", i);
    put(n, b);
  }
  hooks.to_rytm = out;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'U', 'M', 'C', 0xf7});
  idle_all();

  std::string text = "# test_api3_compat's golden file: <package id> <FNV-1a 64 of voice 0> <peak>, from an API 2 host\n";
  int compared = 0, fails = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    char o[200];
    CHECK(um::command(("um=assign:1:" + ids[i]).c_str(), o, sizeof o));
    idle_all();
    if (um::slot_view(0).state != RMP_SLOT_READY) {
      printf("test_api3_compat: %s not READY here (state %d): skipped\n", ids[i].c_str(), um::slot_view(0).state);
      continue;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    int peak = 0;
    int p[16];
    for (int k = 0; k < 16; ++k) p[k] = (40 + 5 * k) << 7;   // a fixed sound, distinct knobs
    sysex(report(0x04, 60, p));
    idle_all();
    render(200, &h, &peak);
    for (int k = 0; k < 16; ++k) p[k] = ((90 - 3 * k) << 7) | 37;   // a knob change with fractions, mid-note
    sysex(report(0x03, -1, p));
    idle_all();
    render(200, &h, &peak);
    sysex({0xf0, 0x7d, 0x52, 0x54, 0x11, 0, 0xf7});   // the gate closed
    idle_all();
    render(200, &h, &peak);
    sysex(report(0x04, 67, p));   // a second note, another pitch
    idle_all();
    render(200, &h, &peak);
    char line[160];
    snprintf(line, sizeof line, "%016llx %d", (unsigned long long)h, peak);
    text += ids[i] + " " + line + "\n";
    auto it = want.find(ids[i]);
    if (write) {
      printf("test_api3_compat: %s %s\n", ids[i].c_str(), line);
    } else if (it == want.end()) {
      printf("test_api3_compat: %s %s: not in %s (a new plugin?)\n", ids[i].c_str(), line, golden);
    } else {
      ++compared;
      if (it->second != line) {
        ++fails;
        fprintf(stderr, "test_api3_compat: %s plays %s, the API 2 host %s\n", ids[i].c_str(), line, it->second.c_str());
      }
    }
    CHECK(um::command("um=clear:1", o, sizeof o));
    idle_all();
  }
  if (write) {
    FILE* f = fopen(golden, "wb");
    CHECK(f);
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    printf("test_api3_compat: wrote %s (%zu packages)\n", golden, ids.size());
    return 0;
  }
  CHECK(fails == 0);
  printf("test_api3_compat: %d packages sound as on the API 2 host: PASS\n", compared);
  return 0;
}
#else
int main() {
  printf("test_api3_compat: RMPA images run on an AArch64 host only: skipped\n");
  return 0;
}
#endif
