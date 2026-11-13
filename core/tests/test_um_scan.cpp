// core/tests/test_um_scan.cpp - the user machines' card work with a card core (core/user/um.cpp, 7z 2026-10-31): the
// /Plugins scan a request a step (the folder's open, its entries 8 a kDirReq, each package's open, reads of 32 KB, the
// close; the CRC 8 KB a step; the entry table, the manifest and the name table kept from those reads), the slot map's
// read (kSmallReq), the slot map's and a preset's writes (fmt::post_queue), a load job abandoned without a wait. Without
// a card core (each request at once) and with one (cardq::service() run between the control core's steps, on the card
// model: no card time on the control core). Checked: the result (the web's JSON, the Rytm's list, a list knob's names, the
// log's lines) is the old scan's - kGolden was captured from the scan as it ran on the control core, before this test's
// change to um.cpp - a second rescan abandons the pass at every step of it and ends with the same list, one request at a
// time, every open closed; a package replaced during a scan; a package's file gone; the map of a project read a few
// passes later with every change of it landing in the right project. The packages are made here (arm64 Mac).
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "rmp_api.h"
#include "rmp_format.h"
#include "rmp_loader.h"
#include "rmp_proto.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"
#include "voices.h"

using namespace core;

#if defined(__aarch64__)
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

struct Opt {
  uint32_t version = 1u << 16;
  uint32_t compat = 0;                // 0: the version
  uint32_t img = 8192;                // the image's bytes
  uint32_t names = 0;                 // a list knob on P2 with this many names
  bool labels = false;                // an RLB1 block: P3's label follows P2's list
  uint32_t magic = RMP_FILE_MAGIC_A64;
  uint32_t state = 64;                // the entry table's
  uint32_t table = RMP_PLUGIN_MAGIC;  // the entry table's magic
  uint32_t api = 2, format = 3;
  bool bad_crc = false;
  uint32_t man_cut = 0;               // this much cut off the manifest (too short to read)
  uint32_t fill = 0;                  // what the image's free bytes hold (another package: another CRC)
};

// An RMPA package (as test_um_presets.cpp's): the entry table, init (returns 0), empty calls, a render of zeros.
static std::string package(const char* id, const Opt& o) {
  std::vector<uint8_t> img(o.img, (uint8_t)o.fill);
  memset(img.data(), 0, 0x200);   // (the entry table's optional calls: none)
  auto w32 = [&](size_t a, uint32_t v) { memcpy(&img[a], &v, 4); };
  auto w64 = [&](size_t a, uint64_t v) { memcpy(&img[a], &v, 8); };
  w32(0, o.table), w32(4, 2), w32(8, o.state), w32(12, 10);
  const uint32_t init[] = {0x52800000, 0xd65f03c0};
  const uint32_t ret[] = {0xd65f03c0};
  const uint32_t render[] = {0x7100005f, 0x5400008d, 0xb800443f, 0x51000442, 0x17fffffc, 0xd65f03c0};
  memcpy(&img[0x100], init, sizeof init);
  memcpy(&img[0x110], ret, sizeof ret);
  memcpy(&img[0x120], render, sizeof render);
  const uint64_t fn[] = {0x100, 0x110, 0x110, 0x110, 0x110, 0x120};
  std::string relocs;
  for (int k = 0; k < 6; ++k) {
    w64(16 + 8 * k, fn[k]);
    rmp_reloc r = {(uint32_t)(16 + 8 * k) | (uint32_t)RMP_REL_ABS64 << 28, 0};
    relocs.append((const char*)&r, sizeof r);
  }
  rmp_manifest_head3 h3;
  memset(&h3, 0, sizeof h3);
  memcpy(h3.name, "SCAN", 4), memcpy(h3.short_name, "SCN", 3), memcpy(h3.licence, "MIT", 3);
  const char* dot = strrchr(id, '.');
  snprintf(h3.name, sizeof h3.name, "%.9s", dot ? dot + 1 : id);
  int nk = o.names ? 3 : 2;
  h3.knob_count = (uint8_t)nk;
  rmp_knob3 k[3];
  memset(k, 0, sizeof k);
  const char* names[3][2] = {{"Level", "LEV"}, {"Mode", "MOD"}, {"Tone", "TON"}};
  for (int i = 0; i < 3; ++i) {
    k[i].p = (uint8_t)(i + 1), k[i].min = 0, k[i].max = 127, k[i].def = 10;
    k[i].text = RMP_TEXT_NUMBER, k[i].role = RMP_ROLE_FREE;
    memcpy(k[i].long_name, names[i][0], strlen(names[i][0]));
    memcpy(k[i].short_name, names[i][1], 3);
  }
  if (o.names) k[1].look = RMP_LOOK_LIST, k[1].text = RMP_TEXT_LIST, k[1].enum_first = 0, k[1].enum_count = (uint8_t)o.names;
  std::string man = std::string((const char*)&h3, sizeof h3) + std::string((const char*)k, sizeof(rmp_knob3) * (size_t)nk);
  for (uint32_t i = 0; i < o.names; ++i) {
    char n[RMP_ENUM_NAME_BYTES + 1];
    snprintf(n, sizeof n, "N%u", i);
    man.append(n, RMP_ENUM_NAME_BYTES);
  }
  if (o.labels) {
    std::string b;
    b.push_back(2), b.push_back((char)o.names), b.push_back(1), b.push_back(0), b.push_back(3);
    for (uint32_t i = 0; i < o.names; ++i) {
      char l[RMP_LABEL_BYTES + 1];
      memset(l, 0, sizeof l);
      snprintf(l, sizeof l, "Lab%u", i);
      b.append(l, RMP_LABEL_BYTES);
    }
    uint16_t len = (uint16_t)b.size();
    uint32_t mg = RMP_LABELS_MAGIC;
    b.append((const char*)&len, 2), b.append((const char*)&mg, 4);
    man += b;
  }
  uint32_t ms = (uint32_t)man.size() - o.man_cut;
  if (o.man_cut) man.resize(ms);
  std::string body = std::string(img.begin(), img.end()) + relocs + man;
  body.append((4 - body.size() % 4) % 4, '\0');
  rmp_header h;
  memset(&h, 0, sizeof h);
  h.magic = o.magic, h.format = o.format, h.api = o.api;
  snprintf(h.id, sizeof h.id, "%s", id);
  h.version = o.version, h.compatible_from = o.compat ? o.compat : o.version;
  h.image_size = o.img, h.entry_offset = 0, h.reloc_count = 6, h.manifest_size = ms;
  uint32_t ds = 4096;
  memcpy(h.reserved, &ds, 4);
  h.crc32 = ~rmp_crc32_update(0xFFFFFFFFu, (const uint8_t*)body.data(), (uint32_t)body.size());
  if (o.bad_crc) h.crc32 ^= 1;
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
static void posts() {   // idle_save's part: the posted writes
  fmt::post_step();
  while (g_worker && fmt::post_busy()) serve_one(), fmt::post_step();
}
static void idle_all() {   // the work until none is left
  int quiet = 0;
  for (int i = 0; i < 400000; ++i) {
    bool did = step();
    bool served = false;
    while (g_worker && serve_one()) served = true;
    posts();
    if (did || served || cardq::pending()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static void sysex(std::vector<uint8_t> m) { engine_sysex(m.data(), (int)m.size()); }
static uint32_t ran(cardq::Kind k) { return cardq::stats().ran[k]; }
static uint32_t opens() { return ran(cardq::kOpenReq); }

static std::string json() {
  um::publish_now();
  static char b[20000];
  int n = um::json_copy(b, sizeof b);
  CHECK(n > 0);
  std::string s(b, (size_t)n), r;
  // (the generation starts at a random number)
  size_t at = s.find("\"gen\":");
  CHECK(at != std::string::npos);
  size_t e = at + 6;
  while (e < s.size() && s[e] >= '0' && s[e] <= '9') ++e;
  return s.substr(0, at) + s.substr(e);
}
static std::string list() {   // the Rytm's installed list (19 ASK installed: 1B)
  g_out.clear();
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x19, 0x01, 0, 0, 3, 0, 0xf7});
  std::string r;
  for (auto& m : g_out)
    if (m.size() > 12 && m[4] == 0x1b) r.append((const char*)m.data() + 10, m.size() - 10);   // (without the generation)
  return r;
}
static std::string enums() {   // U1's P2 names (19 ASK enum, knob index 1)
  g_out.clear();
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x19, 0x01, 1, 0, 1, 1, 0xf7});
  std::string r;
  for (auto& m : g_out)
    if (m.size() > 12 && m[4] == 0x1b) r.append((const char*)m.data() + 10, m.size() - 10);
  return r;
}
static int logged(const std::string& s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}
static std::string scan_log() {   // the log's um lines (the generation's number and the image's address taken out)
  std::string r;
  for (auto l : g_log) {
    if (l.rfind("um: ", 0)) continue;
    size_t a = l.find("(generation "), b = l.find(" at 0x");
    if (a != std::string::npos) l = l.substr(0, a) + l.substr(l.find(')', a));
    if (b != std::string::npos) l = l.substr(0, b) + l.substr(l.find(',', b));
    r += l + "\n";
  }
  return r;
}
static uint32_t fnv(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}
static bool exists(const std::string& p) { return access(host_path(p.c_str()).c_str(), F_OK) == 0; }
static std::string slurp(const std::string& p) {
  std::string r;
  FILE* f = fopen(host_path(p.c_str()).c_str(), "rb");
  if (!f) return r;
  char b[512];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) r.append(b, n);
  fclose(f);
  return r;
}
static void plug(const char* name, const std::string& body) { put(std::string("/Plugins/") + name, body); }
static std::string plugins(const std::string& j) { return j.substr(0, j.find("{\"t\":\"slot\"")); }   // the JSON's packages
static void to_project(const char* n) {   // pool.cpp's switch_to, the user machines' part: its hook on the control core
  sniprintf(project, sizeof project, "%s", n);
  hooks.project_changed();
}
// A pass's steps with the card core one request behind the control core at most
static void lockstep(int n) {
  for (int i = 0; i < n; ++i) {
    step();
    CHECK(cardq::pending() <= 1);
    serve_one();
  }
}

// The fixture: every way a file in /Plugins ends.
static void fixture() {
  Opt big;
  big.img = 300 * 1024, big.names = 36, big.labels = true, big.version = 1u << 16 | 2u << 8;
  plug("a_good.rmp", package("rytm.scana", Opt()));
  plug("b_big.rmp", package("rytm.scanb", big));
  big.version = 1u << 16, big.img = 40000 & ~4095u;
  plug("c_older.rmp", package("rytm.scanb", big));
  plug("d_dup.rmp", package("rytm.scana", Opt()));
  Opt o;
  o.bad_crc = true;
  plug("e_crc.rmp", package("rytm.scane", o));
  std::string cut = package("rytm.scanf", Opt());
  cut.resize(cut.size() - 100);
  plug("f_cut.rmp", cut);
  plug("g_junk.rmp", std::string(500, 'x'));
  plug("h_short.rmp", "tiny");
  o = Opt(), o.magic = RMP_FILE_MAGIC;
  plug("i_thumb.rmp", package("rytm.scani", o));
  o = Opt(), o.api = 99;
  plug("j_api.rmp", package("rytm.scanj", o));
  o = Opt(), o.format = 99;
  plug("k_fmt.rmp", package("rytm.scank", o));
  o = Opt(), o.state = 1u << 20;
  plug("l_state.rmp", package("rytm.scanl", o));
  o = Opt(), o.table = 0x12345678;
  plug("m_table.rmp", package("rytm.scanm", o));
  o = Opt(), o.man_cut = 60;
  plug("n_man.rmp", package("rytm.scann", o));
  plug("UPPER.RMP", package("rytm.scanu", Opt()));
  plug("o_empty.rmp", "");
  put("/Plugins/._hidden.rmp", package("rytm.hidden", Opt()));
  put("/Plugins/readme.txt", "not a package");
  put(std::string("/Plugins/") + std::string(70, 'z') + ".rmp", package("rytm.toolong", Opt()));   // cut at 63: no .rmp, left out
  Fs fs;
  CHECK(fs.mkdir("/Plugins/sub.rmp"));
  for (int i = 0; i < 18; ++i) {   // 34 in all: two not listed
    char n[32], id[32];
    snprintf(n, sizeof n, "p%02d.rmp", i);
    snprintf(id, sizeof id, "rytm.plug%02d", i);
    plug(n, package(id, Opt()));
  }
}

// Plugin API 3 (2026-11) added the READY slots' "muted", "late", "api", "arena", the JSON's "plugpool" and the begin
// line's arena pool: the old scan's values were 0xa68a7eb6 / 0xb8835fb2, and these are the same JSON and log with only
// those added (compared field by field when they changed). 2026-11 (the UFX picker): the "plugins" row's "ufx_slots":4
// (the web page's Effects section) moved it to 0x1a04e040; nothing else changed (the log's hash is the same).
static const uint32_t kGolden = 0x1a04e040;      // fnv(json + list + enums) of the old scan
static const uint32_t kGoldenLog = 0x0f5d3fc0;   // fnv(its um log lines)

int main() {
  char tmpl[] = "/tmp/core-umscan-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_set_log_sink(sink);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/projects/UMS"));
  put("/.rytm/project", "UMS");
  fixture();
  CHECK(fs.mkdir("/.rytm/slots"));

  hooks.to_rytm = ::out;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'U', 'M', 'S', 0xf7});
  idle_all();
  CHECK(logged("um: 32 packages in /Plugins") == 1);
  CHECK(logged("(more not listed)") == 1);
  char o[200];
  CHECK(um::command("um=assign:1:rytm.scanb", o, sizeof o));
  idle_all();
  CHECK(um::slot_view(0).state == RMP_SLOT_READY);
  {   // hooks.user_label (the LFO, MORPH and XY names on core 0): the slot's short labels from the cache um's idle builds
    char b[8];
    const int8_t u = v[0].user;
    v[0].user = 0;
    CHECK(hooks.user_label && hooks.user_label(0, 0, b, sizeof b) && !strcmp(b, "LEV"));
    CHECK(hooks.user_label(0, 1, b, sizeof b) && !strcmp(b, "MOD"));
    CHECK(hooks.user_label(0, 15, b, sizeof b) && !strcmp(b, ""));   // an unused knob: empty
    CHECK(!hooks.user_label(0, 16, b, sizeof b) && !hooks.user_label(-1, 0, b, sizeof b));
    v[0].user = 3;   // an empty slot: no names
    CHECK(!hooks.user_label(0, 0, b, sizeof b));
    v[0].user = -1;
    CHECK(!hooks.user_label(0, 0, b, sizeof b));
    v[0].user = u;
  }

  // ---- no card core: the old scan's result
  const std::string j0 = json(), l0 = list(), e0 = enums();
  printf("old: %d names, %u bytes of JSON\n", (int)e0.size(), (unsigned)j0.size());
  const uint32_t h0 = fnv(j0 + l0 + e0), g0 = fnv(scan_log());
  printf("kGolden 0x%08x kGoldenLog 0x%08x\n", h0, g0);
  CHECK(h0 == kGolden && g0 == kGoldenLog);
  CHECK(j0.find("\"why\":\"CRC wrong (damaged file)\"") != std::string::npos);
  CHECK(j0.find("\"why\":\"duplicate id (the same version twice)\"") != std::string::npos);
  CHECK(j0.find("\"older\":true") != std::string::npos);
  CHECK(!e0.empty());

  // ---- the scan on the card model, no card core: all of its card time where um::idle runs
  host_card_model(true);
  CHECK(um::command("um=rescan", o, sizeof o));
  uint32_t steps = 0;
  while (um::idle() && steps < 100000) ++steps;
  printf("the scan, no card core: %u steps, %.1f ms of card time (model)\n", steps, (double)host_card_us(3) / 1000.0);
  CHECK(json() == j0 && list() == l0);

  // ---- a card core: the same result, no card time on the control core, a request at a time
  host_card_model(true);
  g_worker = true;
  cardq::set_worker(true);
  uint32_t o0 = opens(), c0 = ran(cardq::kCloseReq), p0 = cardq::stats().posted, r0 = ran(cardq::kReadReq);
  int done = logged("um: 32 packages in /Plugins");
  g_card3 = 0;
  CHECK(um::command("um=rescan", o, sizeof o));
  step();
  CHECK(cardq::pending() == 1);   // the folder's open posted: nothing goes on until it has run
  for (int i = 0; i < 20; ++i) step();
  CHECK(cardq::pending() == 1 && ran(cardq::kOpenReq) == o0);
  int pass = 21;
  uint32_t slowest = 0;
  while (logged("um: 32 packages in /Plugins") == done) {
    uint32_t t0 = now_us();
    step();
    if (now_us() - t0 > slowest) slowest = now_us() - t0;
    CHECK(cardq::pending() <= 1);
    serve_one();
    CHECK(++pass < 100000);
  }
  idle_all();
  CHECK(json() == j0 && list() == l0 && enums() == e0);
  CHECK(opens() - o0 == ran(cardq::kCloseReq) - c0);
  CHECK(g_card3 == 0 && host_card_us(0) > 0);
  printf("the scan, a card core: %d steps, %u requests (%u reads), %.1f ms of card time (model), none on the control core, slowest step %u us here\n",
         pass, cardq::stats().posted - p0, ran(cardq::kReadReq) - r0, (double)host_card_us(0) / 1000.0, slowest);

  // ---- a second rescan abandons the pass at any step, with its request posted or run: the same list, every open closed
  for (int k = 0; k < pass - 30; k += 1) {
    o0 = opens(), c0 = ran(cardq::kCloseReq), done = logged("um: 32 packages in /Plugins");
    CHECK(um::command("um=rescan", o, sizeof o));
    lockstep(k / 2);   // (a step a request or an 8 KB of CRC: the odd ones leave a request posted, the even none)
    if (k & 1) step();
    CHECK(cardq::pending() <= 1);
    CHECK(um::command("um=rescan", o, sizeof o));
    idle_all();
    CHECK(logged("um: 32 packages in /Plugins") == done + 1);   // (the abandoned pass published nothing)
    CHECK(plugins(json()) == plugins(j0));
    CHECK(opens() - o0 == ran(cardq::kCloseReq) - c0);
    k += 6;   // (the pass waits 150 ms for the rescans to end: every seventh step, odd and even)
  }
  CHECK(json() == j0);

  // ---- a package replaced during a scan (an upload over it): damaged for that pass, as before; its rescan finds the new one
  auto reads_to_big = [&]() {   // to b_big's first read done (UPPER and a_good before it, then one read each)
    uint32_t r = ran(cardq::kReadReq);
    CHECK(um::command("um=rescan", o, sizeof o));
    for (int i = 0; i < 1000 && ran(cardq::kReadReq) < r + 3; ++i) lockstep(1);
    CHECK(ran(cardq::kReadReq) == r + 3);
  };
  Opt nb;
  nb.img = 300 * 1024, nb.names = 36, nb.labels = true, nb.version = 1u << 16 | 3u << 8, nb.compat = 1u << 16, nb.fill = 7;
  std::string orig = slurp("/Plugins/b_big.rmp");
  reads_to_big();
  put("/Plugins/b_big.rmp", package("rytm.scanb", nb));
  idle_all();
  CHECK(json().find("CRC wrong (damaged file)\",\"file\":\"/Plugins/b_big.rmp") != std::string::npos);
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  CHECK(json().find("\"id\":\"rytm.scanb\",\"ver\":\"1.3.0\"") != std::string::npos);
  CHECK(um::slot_view(0).state == RMP_SLOT_READY);
  // ... and with the upload's rescan coming in the middle of the pass: only the new file's result is ever published
  done = logged("um: 32 packages in /Plugins");
  reads_to_big();
  put("/Plugins/b_big.rmp", orig);
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  CHECK(logged("um: 32 packages in /Plugins") == done + 1);
  CHECK(plugins(json()) == plugins(j0));

  // ---- gone between the listing and its open: "unreadable", as before; a folder that is not there: no packages
  {
    std::string keep = slurp("/Plugins/a_good.rmp");
    c0 = ran(cardq::kCloseReq);
    CHECK(um::command("um=rescan", o, sizeof o));
    for (int i = 0; i < 1000 && ran(cardq::kCloseReq) == c0; ++i) lockstep(1);
    CHECK(fs.remove("/Plugins/a_good.rmp"));
    idle_all();
    CHECK(json().find("\"why\":\"unreadable\"") != std::string::npos);
    put("/Plugins/a_good.rmp", keep);
    CHECK(fs.rename("/Plugins", "/Pluginz"));
    CHECK(um::command("um=rescan", o, sizeof o));
    idle_all();
    CHECK(logged("um: 0 packages in /Plugins") == 1);
    CHECK(fs.rename("/Pluginz", "/Plugins"));
    CHECK(um::command("um=rescan", o, sizeof o));
    idle_all();
    CHECK(um::slot_view(0).state == RMP_SLOT_READY);
  }
  const std::string ups = "/.rytm/slots/UMS.txt", umq = "/.rytm/slots/UMQ.txt";

  // ---- a load job abandoned by a project change with its read posted: no wait, no close here, the same load after it
  CHECK(fs.mkdir("/PRESETS") && fs.mkdir("/PRESETS/rytm.scanb"));   // (its presets' folder opened and closed: the count below)
  CHECK(um::command("um=rescan", o, sizeof o));   // (a package moved: U1 again)
  put("/Plugins/b_big.rmp", package("rytm.scanb", nb));
  CHECK(um::command("um=rescan", o, sizeof o));
  o0 = opens(), c0 = ran(cardq::kCloseReq);
  for (uint32_t t = now_ms(); now_ms() - t < 3000 && um::slot_view(0).state != RMP_SLOT_LOADING;) lockstep(1);   // (the 150 ms)
  CHECK(um::slot_view(0).state == RMP_SLOT_LOADING);
  lockstep(3);
  for (int i = 0; i < 4 && !cardq::pending(); ++i) step();   // (a step takes a read, the next posts one)
  CHECK(cardq::pending() == 1);
  uint64_t c3 = host_card_us(3);
  to_project("UMS");
  CHECK(host_card_us(3) == c3);
  idle_all();
  CHECK(um::slot_view(0).state == RMP_SLOT_READY);
  CHECK(opens() - o0 == ran(cardq::kCloseReq) - c0);
  CHECK(fs.remove("/PRESETS/rytm.scanb") && fs.remove("/PRESETS"));

  // ---- the map's read: the old project's until all three files are here, a request each, none of it on the control core
  uint32_t m0 = ran(cardq::kSmallReq);
  c3 = host_card_us(3);
  to_project("UMQ");
  CHECK(host_card_us(3) == c3);
  CHECK(!strcmp(um::slot_view(0).id, "rytm.scanb"));
  step();
  CHECK(cardq::pending() == 1 && !strcmp(um::slot_view(0).id, "rytm.scanb"));
  CHECK(logged("um: project UMQ, map /.rytm/slots/UMQ.txt (none: fresh)") == 0);
  idle_all();
  CHECK(ran(cardq::kSmallReq) == m0 + 3);   // (the map, the notes, the effects' file)
  CHECK(logged("um: project UMQ, map /.rytm/slots/UMQ.txt (none: fresh)") == 1);
  CHECK(!um::slot_view(0).id[0]);
  // the map's write: posted, written on the card core
  c3 = host_card_us(3);
  CHECK(um::command("um=assign:1:rytm.scana", o, sizeof o));
  CHECK(host_card_us(3) == c3);
  CHECK(!exists(umq));
  idle_all();
  CHECK(slurp(umq).find("rytm.scana") != std::string::npos);
  CHECK(host_card_us(3) == c3);
  // back: the old project's map as it was
  to_project("UMS");
  idle_all();
  CHECK(!strcmp(um::slot_view(0).id, "rytm.scanb") && um::slot_view(0).state == RMP_SLOT_READY);
  CHECK(g_card3 == 0);
  // a change of the map within the read's passes waits for it, from idle(): no wait here, whatever core 0 does, and it lands
  // in the new project, never in the old one's file
  std::string ums0 = slurp(ups);
  to_project("UMQ");
  uint32_t pend = (uint32_t)cardq::pending();
  c3 = host_card_us(3);
  CHECK(um::command("um=assign:2:rytm.plug03", o, sizeof o) && strstr(o, "after the project's map"));
  CHECK(um::command("um=assign:5:rytm.nothere", o, sizeof o));
  CHECK(!strcmp(um::slot_view(0).id, "rytm.scanb") && !um::slot_view(1).id[0]);
  CHECK((uint32_t)cardq::pending() == pend && host_card_us(3) == c3);
  // the Rytm's 1E right after the project change: held the same way, answered once the map is here
  {
    uint8_t gen = um::store_gen();
    g_out.clear();
    sysex({0xf0, 0x7d, 0x52, 0x54, 0x1e, 0x01, 3, RMP_OP_ASSIGN, 0, (uint8_t)(gen & 0x7f), 0xf7});   // U3 = the list's first
    CHECK(host_card_us(3) == c3 && !um::slot_view(2).id[0]);
    for (auto& m : g_out) CHECK(m.size() < 15 || m[4] != 0x1d);   // (no answer yet)
  }
  idle_all();
  CHECK(slurp(ups) == ums0);
  CHECK(slurp(umq).find("rytm.scana") != std::string::npos && slurp(umq).find("rytm.plug03") != std::string::npos);
  CHECK(!strcmp(um::slot_view(0).id, "rytm.scana") && !strcmp(um::slot_view(1).id, "rytm.plug03"));
  CHECK(!strcmp(um::slot_view(2).id, "rytm.scanu"));   // (UPPER.RMP: the Rytm's list's first)
  CHECK(logged("um: rytm.nothere is not installed") == 1);
  {
    bool ans = false;
    for (auto& m : g_out) ans = ans || (m.size() == 15 && m[4] == 0x1d && m[12] == 3);
    CHECK(ans);
  }
  CHECK(um::command("um=clear:3", o, sizeof o));
  idle_all();
  // left and come back to at once: the map read after its save landed, not before
  CHECK(um::command("um=assign:3:rytm.plug04", o, sizeof o));
  CHECK(um::command("um=assign:4:rytm.plug05", o, sizeof o));   // (the second map's write queued behind the first's)
  to_project("UMS");
  to_project("UMQ");
  idle_all();
  CHECK(!strcmp(um::slot_view(2).id, "rytm.plug04") && !strcmp(um::slot_view(3).id, "rytm.plug05"));
  CHECK(slurp(umq).find("rytm.plug05") != std::string::npos);
  // a project change while the map's read runs: the newer project's map, one log line
  to_project("UMS");
  step();
  CHECK(cardq::pending() == 1);
  to_project("UMQ");
  idle_all();
  CHECK(!strcmp(um::slot_view(0).id, "rytm.scana"));
  // a map that does not parse: kept, not written; a cut between write_atomic's renames: the .prev put back by the card core
  put("/.rytm/slots/UMK.txt", "this is not a slot map\n");
  to_project("UMK");
  idle_all();
  CHECK(logged("/.rytm/slots/UMK.txt is not a slot map this build reads") == 1);
  CHECK(um::command("um=assign:1:rytm.scana", o, sizeof o));
  idle_all();
  CHECK(slurp("/.rytm/slots/UMK.txt") == "this is not a slot map\n");
  put("/.rytm/slots/UMR.txt.prev", slurp(umq));
  to_project("UMR");
  idle_all();
  CHECK(!strcmp(um::slot_view(0).id, "rytm.scana") && exists("/.rytm/slots/UMR.txt"));

  // ---- a generic preset saved: posted, its folders made by the card core
  to_project("UMS");
  idle_all();
  {
    std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, 0x50};
    for (int k = 0; k < 16; ++k) m.push_back(64), m.push_back(0);
    m.push_back(0xf7);
    sysex(m);
  }
  idle_all();
  c3 = host_card_us(3);
  CHECK(um::command("um=save:1:Keep", o, sizeof o) && strstr(o, "saved"));
  CHECK(host_card_us(3) == c3);
  idle_all();
  CHECK(slurp("/PRESETS/rytm.scanb/Keep.txt").find("P1 64") != std::string::npos);
  CHECK(host_card_us(3) == c3);
  CHECK(g_card3 == 0);

  // ---- no card core again: the old path, the same list
  g_worker = false;
  cardq::set_worker(false);
  host_card_model(false);
  put("/Plugins/b_big.rmp", orig);
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  CHECK(plugins(json()) == plugins(j0));

  // ---- a box with a folder of its own (the A4's /.a4): the web tab's assign lands there, and its report's voice byte plays the slot
  {
    device::g_link.state = "/.a4";
    CHECK(um::command("um=assign:2:rytm.scanb", o, sizeof o));
    idle_all();
    CHECK(um::slot_view(1).state == RMP_SLOT_READY);
    CHECK(fs.exists("/.a4/slots/UMS.txt") && !slurp("/.a4/slots/UMS.txt").empty());
    device::reset();
    CHECK(device::kA4.features & device::kUserSlots);
  }
  // ---- a clock 24.8+ days into the uptime (a Pi up for weeks, a Mac): the first rescan after boot waits its 150 ms, not forever
  // (g_scan_at started as 0 and now_ms() - 0 read as in the future from 2^31 ms on, so the scan never began)
  host_fake_time(true, 3000000000000ull);   // 3.0e9 ms, past 2^31
  idle_all();
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  CHECK(um::slot_view(0).state == RMP_SLOT_READY);
  host_fake_time(true, 3000000000000ull + 150000ull * 15);
  CHECK(um::command("um=rescan", o, sizeof o));
  idle_all();
  host_fake_time(false, 0);
  printf("test_um_scan: PASS\n");
  return 0;
}
#else
int main() {
  printf("test_um_scan: PASS (an AArch64 package: arm64 only)\n");
  return 0;
}
#endif
