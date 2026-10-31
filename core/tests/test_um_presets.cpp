// core/tests/test_um_presets.cpp - the user machines' presets with their card work on the card queue (core/user/um.cpp,
// 7z 2026-10-30): /PRESETS/<id>/ scanned by requests (the folder's open, its entries 8 a kDirReq, the close) and a
// preset loaded by requests (kOpenAtReq by its dir index, a read of 4096 B at most, the close). Without a card core
// (each request at once, as the Mac's tests always ran) and with one (cardq::service() run between the control core's
// steps, on the card model: no card time on the control core). Checked: the list is the old scan's (openNext and
// rmp_preset_add on the control core, here in the test), a missing folder or a plain file of its name gives none, a load
// writes its values back (1C), one cut at 4096 B still loads, a decision made while a load is under way replaces it (the
// older one's values never applied), a scan abandoned mid-folder (um=presets again) ends with the same list, and every
// open the requests made is closed. The package is made here: an RMPA image of a few AArch64 instructions (arm64 Mac).
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "msamp/cardq.h"
#include "rmp_api.h"
#include "rmp_format.h"
#include "rmp_loader.h"
#include "rmp_presets.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"

using namespace core;

#if defined(__aarch64__)
static const char* kId = "rytm.cqtest";
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static std::vector<std::string> g_log;
static void sink(const char* s, int n) { g_log.emplace_back(s, (size_t)n); }

static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}

// An RMPA package: the entry table, init (returns 0), an empty voice_init / note_on / note_off / params, a render of
// zeros; P1 PRE (the preset knob), P2 and P3 preset-role knobs; native presets ".tp" (no load_entry).
static std::string package() {
  std::vector<uint8_t> img(8192, 0);
  auto w32 = [&](size_t o, uint32_t v) { memcpy(&img[o], &v, 4); };
  auto w64 = [&](size_t o, uint64_t v) { memcpy(&img[o], &v, 8); };
  w32(0, RMP_PLUGIN_MAGIC), w32(4, 2), w32(8, 64), w32(12, 10);
  const uint32_t init[] = {0x52800000, 0xd65f03c0};   // mov w0, #0; ret
  const uint32_t ret[] = {0xd65f03c0};
  // cmp w2, #0; b.le +16; str wzr, [x1], #4; sub w2, w2, #1; b -16; ret
  const uint32_t render[] = {0x7100005f, 0x5400008d, 0xb800443f, 0x51000442, 0x17fffffc, 0xd65f03c0};
  memcpy(&img[0x100], init, sizeof init);
  memcpy(&img[0x110], ret, sizeof ret);
  memcpy(&img[0x120], render, sizeof render);
  const uint64_t fn[] = {0x100, 0x110, 0x110, 0x110, 0x110, 0x120};   // init voice_init note_on note_off params render
  std::string relocs;
  for (int k = 0; k < 6; ++k) {
    w64(16 + 8 * k, fn[k]);
    rmp_reloc r = {(uint32_t)(16 + 8 * k) | (uint32_t)RMP_REL_ABS64 << 28, 0};
    relocs.append((const char*)&r, sizeof r);
  }
  rmp_manifest_head3 h3;
  memset(&h3, 0, sizeof h3);
  memcpy(h3.name, "CQ TEST", 7), memcpy(h3.short_name, "CQT", 3), memcpy(h3.licence, "MIT", 3);
  h3.knob_count = 3;
  memcpy(h3.preset_ext, "tp", 2);
  rmp_knob3 k[3];
  memset(k, 0, sizeof k);
  const char* names[3][2] = {{"Preset", "PRE"}, {"Level", "LEV"}, {"Tone", "TON"}};
  for (int i = 0; i < 3; ++i) {
    k[i].p = (uint8_t)(i + 1), k[i].min = 0, k[i].max = 127, k[i].def = 0;
    k[i].text = i == 0 ? RMP_TEXT_PRESET : RMP_TEXT_NUMBER;
    k[i].role = i == 0 ? RMP_ROLE_FREE : RMP_ROLE_PRESET;
    memcpy(k[i].long_name, names[i][0], strlen(names[i][0]));
    memcpy(k[i].short_name, names[i][1], 3);
  }
  std::string man = std::string((const char*)&h3, sizeof h3) + std::string((const char*)k, sizeof k);
  std::string body = std::string(img.begin(), img.end()) + relocs + man;
  body.append((4 - body.size() % 4) % 4, '\0');
  rmp_header h;
  memset(&h, 0, sizeof h);
  h.magic = RMP_FILE_MAGIC_A64, h.format = 3, h.api = 2;
  snprintf(h.id, sizeof h.id, "%s", kId);
  h.version = h.compatible_from = 1u << 16;
  h.image_size = 8192, h.entry_offset = 0, h.reloc_count = 6, h.manifest_size = (uint32_t)man.size();
  uint32_t ds = 4096;
  memcpy(h.reserved, &ds, 4);
  h.crc32 = ~rmp_crc32_update(0xFFFFFFFFu, (const uint8_t*)body.data(), (uint32_t)body.size());
  return std::string((const char*)&h, sizeof h) + body;
}

static bool g_worker = false;
static uint64_t g_card3 = 0;   // card time charged to the control core inside um::idle
static bool step() {
  host_card_core(3);
  uint64_t c = host_card_us(3);
  bool did = um::idle();
  g_card3 += host_card_us(3) - c;
  return did;
}
static bool serve_one() {
  host_card_core(0);
  bool r = cardq::service() != nullptr;
  host_card_core(3);
  return r;
}
static void idle_all() {   // the work until none is left (a card core's requests served between the steps)
  int quiet = 0;
  for (int i = 0; i < 100000; ++i) {
    bool did = step();
    bool served = false;
    while (g_worker && serve_one()) served = true;
    if (did || served || cardq::pending()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static void sysex(std::vector<uint8_t> m) {
  engine_sysex(m.data(), (int)m.size());
}
static void report(int pre, int p2, int p3) {   // 0x03 for voice 0 on U1 (P values 0..127)
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, 0x50};
  for (int k = 0; k < 16; ++k) {
    int v = k == 0 ? pre : k == 1 ? p2 : k == 2 ? p3 : 64;
    m.push_back((uint8_t)v), m.push_back(0);
  }
  m.push_back(0xf7);
  sysex(m);
}
static std::vector<std::string> names() {   // U1's preset names (19 ASK presets, page 0: 1B 0x20)
  g_out.clear();
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x19, 0x01, 1, 0, 2, 0, 0xf7});
  idle_all();
  std::vector<std::string> r;
  int total = -1;
  for (auto& m : g_out) {
    if (m.size() < 12 || m[4] != 0x1b || m[7] != 0x20) continue;
    total = m[10];
    for (size_t i = 11; i + 14 <= m.size(); i += 14) {
      std::string n((const char*)&m[i + 1], 13);
      n.resize(strnlen(n.c_str(), 13));
      if ((size_t)m[i] >= r.size()) r.resize(m[i] + 1);
      r[m[i]] = n;
    }
  }
  CHECK(total >= 0 && (int)r.size() == total);
  return r;
}
static std::vector<std::pair<int, int>> knobs() {   // the 1C pairs sent since g_out was cleared (P index, value)
  std::vector<std::pair<int, int>> r;
  for (auto& m : g_out)
    if (m.size() > 9 && m[4] == 0x1c)
      for (int k = 0; k < m[8]; ++k) r.push_back({m[9 + 2 * k], m[10 + 2 * k]});
  std::sort(r.begin(), r.end());
  return r;
}
static int logged(const std::string& s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}
static std::vector<std::string> old_scan() {   // the scan as it ran on the control core before (openNext, 4 a step)
  static rmp_preset_names nm;
  static rmp_preset_index ix;
  rmp_preset_names_init(&nm);
  rmp_preset_index_init(&ix);
  rmp_preset_scan_begin(&nm, &ix, 0);
  File d, e;
  CHECK(d.open((std::string("/PRESETS/") + kId).c_str(), kRead) && d.isDir());
  while (e.openNext(&d, kRead)) {
    char n[128];
    bool dir = e.isDir();
    uint32_t idx = e.dirIndex();
    if (!e.getName(n, sizeof n)) n[0] = 0;
    e.close();
    if (!dir && n[0] && n[0] != '.' && idx <= 0xFFFFu) rmp_preset_add(&nm, &ix, n, (uint16_t)idx, "tp");
  }
  d.close();
  rmp_preset_scan_end(&nm, &ix);
  std::vector<std::string> r;
  for (int i = 0; i < nm.n; ++i) r.push_back(nm.name[i]);
  return r;
}
static uint32_t ran(cardq::Kind k) { return cardq::stats().ran[k]; }
static uint32_t opens() { return ran(cardq::kOpenReq) + ran(cardq::kOpenAtReq); }

int main() {
  char tmpl[] = "/tmp/core-umpre-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_set_log_sink(sink);
  Fs fs;
  std::string dir = std::string("/PRESETS/") + kId;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/projects/UMP") && fs.mkdir(dir.c_str()));
  put("/.rytm/project", "UMP");
  put("/Plugins/cqtest.rmp", package());
  // 11 generic presets (two kDirReq batches), junk the scan leaves out, one over 4096 B
  const char* pre[] = {"Bass", "Bell", "Brass", "Drone", "Keys", "Lead", "Organ", "Pad", "Pluck", "Strings"};
  for (int i = 0; i < 10; ++i) put(dir + "/" + pre[i] + ".txt", "P2 " + std::to_string(10 + i) + "\nP3 " + std::to_string(40 + i) + "\n");
  put(dir + "/Big.txt", "P2 77\nP3 66\n" + std::string(5000, '\n'));
  put(dir + "/._Bass.txt", "P2 1\n");
  put(dir + "/readme.md", "not a preset\n");
  CHECK(fs.mkdir((dir + "/sub").c_str()));
  auto vals = [&](const std::string& n) -> std::vector<std::pair<int, int>> {   // a preset's 1C pairs
    if (n == "Big") return {{1, 77}, {2, 66}};
    for (int i = 0; i < 10; ++i)
      if (n == pre[i]) return {{1, 10 + i}, {2, 40 + i}};
    CHECK(!"no such preset");
    return {};
  };
  const std::vector<std::string> want = old_scan();
  CHECK(want.size() == 11 && want[0] == "Bass" && want[2] == "Big");

  hooks.to_rytm = out;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'U', 'M', 'P', 0xf7});
  idle_all();
  char o[200];
  CHECK(um::command((std::string("um=assign:1:") + kId).c_str(), o, sizeof o));
  idle_all();
  CHECK(um::slot_view(0).state == RMP_SLOT_READY);

  // ---- no card core: every request at once
  CHECK(names() == want);
  CHECK(logged(std::string("um: presets U1 ") + kId + ": 11") == 1);
  g_out.clear();
  report(0, 64, 64);   // the sound's first report: PRE 0 loads, written back
  idle_all();
  CHECK(knobs() == vals(want[0]));
  CHECK(logged("um: preset voice 0 U1 0 Bass (written back") == 1);
  g_out.clear();
  report(2, 10, 40);   // Big: cut at 4096 B, its first lines still read
  idle_all();
  CHECK(knobs() == vals("Big"));
  CHECK(logged("um: preset voice 0 U1 2 Big (written back, cut at 4096 B") == 1);
  // a scan abandoned after its first batch: the list read again from the start, the same
  uint32_t d0 = ran(cardq::kDirReq);
  CHECK(um::command("um=presets", o, sizeof o));
  for (int i = 0; i < 1000 && ran(cardq::kDirReq) == d0; ++i) step();
  CHECK(ran(cardq::kDirReq) == d0 + 1);
  CHECK(um::command("um=presets", o, sizeof o));
  CHECK(names() == want);
  CHECK(logged(std::string("um: presets U1 ") + kId + ": 11") == 2);   // (the abandoned one not logged)

  // ---- a card core (core 0's service()): the control core posts, never touches the card
  host_card_model(true);
  g_worker = true;
  cardq::set_worker(true);
  uint32_t o0 = opens(), c0 = ran(cardq::kCloseReq);
  g_card3 = 0;
  CHECK(um::command("um=presets", o, sizeof o));
  CHECK(names() == want);
  CHECK(logged(std::string("um: presets U1 ") + kId + ": 11") == 3);
  g_out.clear();
  report(4, 77, 66);
  step();   // the load's open posted, nothing applied while the card core has not run it
  CHECK(cardq::pending() == 1 && knobs().empty());
  idle_all();
  CHECK(knobs() == vals(want[4]));
  CHECK(logged(std::string("um: preset voice 0 U1 4 ") + want[4] + " (written back") == 1);
  // a decision while a load is under way: the older one's values never applied, the newer one loaded
  auto v4 = vals(want[4]);
  g_out.clear();
  report(5, v4[0].second, v4[1].second);
  step();
  CHECK(cardq::pending() == 1);
  serve_one();   // its open done; the read posted at the next step
  step();
  report(6, v4[0].second, v4[1].second);
  idle_all();
  CHECK(knobs() == vals(want[6]));
  CHECK(logged(std::string("um: preset voice 0 U1 5 ")) == 0);
  CHECK(logged(std::string("um: preset voice 0 U1 6 ") + want[6] + " (written back") == 1);
  // a scan abandoned with its second batch posted: the batch runs to its end, the folder closed, the list again
  d0 = ran(cardq::kDirReq);
  CHECK(um::command("um=presets", o, sizeof o));
  for (int i = 0; i < 1000 && !(ran(cardq::kDirReq) == d0 + 1 && cardq::pending()); ++i) {
    step();
    if (ran(cardq::kDirReq) == d0 + 1 && cardq::pending()) break;
    serve_one();
  }
  CHECK(ran(cardq::kDirReq) == d0 + 1 && cardq::pending() == 1);
  CHECK(um::command("um=presets", o, sizeof o));
  CHECK(names() == want);
  CHECK(logged(std::string("um: presets U1 ") + kId + ": 11") == 4);
  CHECK(opens() - o0 == ran(cardq::kCloseReq) - c0);   // every folder and preset opened was closed
  // no folder (its open fails: nothing to close); a plain file of its name (opened: closed): no presets
  CHECK(fs.rename(dir.c_str(), "/PRESETS/away"));
  o0 = ran(cardq::kOpenReq), c0 = ran(cardq::kCloseReq);
  CHECK(um::command("um=presets", o, sizeof o));
  CHECK(names().empty());
  CHECK(ran(cardq::kOpenReq) == o0 + 1 && ran(cardq::kCloseReq) == c0);
  put(dir, "a file");
  CHECK(um::command("um=presets", o, sizeof o));
  CHECK(names().empty());
  CHECK(ran(cardq::kOpenReq) == o0 + 2 && ran(cardq::kCloseReq) == c0 + 1);
  CHECK(logged(std::string("um: presets U1 ") + kId) == 4);
  CHECK(g_card3 == 0 && host_card_us(0) > 0);   // the control core never waited for the card; the card core did the work
  g_worker = false;
  cardq::set_worker(false);
  CHECK(fs.remove(dir.c_str()) && fs.rename("/PRESETS/away", dir.c_str()));
  CHECK(um::command("um=presets", o, sizeof o));
  CHECK(names() == want);
  printf("test_um_presets: PASS\n");
  return 0;
}
#else
int main() {
  printf("test_um_presets: PASS (an AArch64 package: arm64 only)\n");
  return 0;
}
#endif
