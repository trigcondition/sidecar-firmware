// core/tests/test_pool_page.cpp - the remote UI's POOL page (core/ui/page_pool.cpp; the pools design 5, the UX review's
// P1 and P3) drawn by the page itself on a card with the three lists: the tabs, the RAM figure, the rows and their
// states; SELECT, UNUSED (the warning), UNLOAD with P3 (a row in use asks first, refused while the Rytm plays), REPLACE
// through the list's browser (one entry per file, the ask again), UNDO. PNGs (scale 4, the Rytm's text in the host's
// font) to $RUI_OUT (default build/rui_pool).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "nmach.h"
#include "pool_util.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;
using namespace core::ui;

static Canvas g_c;
static int g_which = -1;
static bool g_play = false;
static void page_frame(uint8_t fb[kBytes]) {
  CHECK(remote_render(g_which, 0, nullptr, g_c));
  preview(g_c, fb);
}
static bool has_text(const char* t) {
  for (int i = 0; i < kTextSlots; ++i)
    if (strstr(g_c.txt[i].s, t)) return true;
  return false;
}
static void frame() {
  uint8_t fb[kBytes];
  page_frame(fb);
}
// ---- a PNG of the sent frame with its text (sim/rui_render.py's colours, scale 4, the pixel grid)
static uint32_t crc32_of(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
static std::vector<uint8_t> zstore(const std::vector<uint8_t>& raw) {   // a zlib stream of stored deflate blocks
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t at = 0; at < raw.size() || at == 0;) {
    size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8)), z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (!n) break;
  }
  for (uint8_t x : raw) a = (a + x) % 65521, b = (b + a) % 65521;
  uint32_t ad = b << 16 | a;
  for (int i = 3; i >= 0; --i) z.push_back((uint8_t)(ad >> (8 * i)));
  return z;
}
static void put_be32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
static void chunk(std::vector<uint8_t>& f, const char* t, const std::vector<uint8_t>& d) {
  put_be32(f, (uint32_t)d.size());
  std::vector<uint8_t> td(t, t + 4);
  td.insert(td.end(), d.begin(), d.end());
  f.insert(f.end(), td.begin(), td.end());
  put_be32(f, crc32_of(td.data(), td.size()));
}
static std::string g_dir;
static void png(const char* name) {
  uint8_t fb[kBytes];
  page_frame(fb);
  const int s = 4, w = 128 * s, h = 64 * s;
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      int X = x / s, Y = y / s;
      bool on = fb[(7 - Y / 8) + 8 * X] >> (Y % 8) & 1;
      int c[3] = {on ? 232 : 12, on ? 238 : 14, on ? 255 : 22};
      bool grid = x % s == s - 1 || y % s == s - 1;
      for (int k = 0; k < 3; ++k) raw.push_back((uint8_t)(grid ? c[k] * 8 / 10 : c[k]));
    }
  }
  std::vector<uint8_t> z = zstore(raw);
  std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, ih;
  put_be32(ih, w), put_be32(ih, h);
  ih.insert(ih.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ih);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  std::string path = g_dir + "/" + name;
  FILE* fp = fopen(path.c_str(), "wb");
  CHECK(fp);
  fwrite(f.data(), 1, f.size(), fp);
  fclose(fp);
  printf("  %s\n", path.c_str());
}




static const ExtraPage& page() { return extra_page(g_which - 6); }
static void key(int code) { page().key(code); }
static void enc(int e, int d) { page().enc(e, d, false); }

int main() {
  const char* env = getenv("RUI_OUT");
  g_dir = env && *env ? env : "build/rui_pool";
  mkdir("build", 0755);
  mkdir(g_dir.c_str(), 0755);
  char tmpl[] = "/tmp/core_pool_page_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples/Kit' '" + card + "/MULTISAMPLE'").c_str()) == 0);
  auto tone = [](double hz) { return [hz](int i, int) { return 0.4 * std::sin(2 * M_PI * hz * i / 48000); }; };
  const char* names[] = {"kick", "snare", "hat", "clap", "tom"};
  for (int i = 0; i < 5; ++i) wg::write_file(card + "/Samples/Kit/" + names[i] + ".wav", wg::wav_bytes(24000 + i, 1, 16, 48000, 1, "", tone(100 + 80 * i)));
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  host_fake_time(true, 1000000);
  hooks.to_rytm = [](const uint8_t*, int) {};
  CHECK(engine_begin());
  pools::set_playing_fn([] { return g_play; });
  pu::steps();
  for (int i = 0; i < extra_pages(); ++i)
    if (!strcmp(extra_page(i).title, "POOL")) g_which = 6 + i;
  CHECK(g_which >= 6);
  for (int i = 0; i < 4; ++i) {
    char r[8];
    snprintf(r, sizeof r, "A%03d", i + 1);
    CHECK(pu::put(pools::kSamples, r, ("/Samples/Kit/" + std::string(names[i]) + ".wav").c_str()) == pools::kOk);
  }
  pools::seen(pools::kSamples, 1);   // kick played in this project
  v[2].gr = true;   // a GRANULAR voice on snare now
  pools::voice_on(pools::kSamples, 2, 2);
  pools::seen(pools::kSamples, 2);   // (and seen playing)
  pool_page_reset();
  pool_page_set(0, 0, 1, 0);
  frame();
  png("pool_1_samples.png");
  CHECK(has_text("SMP ") && has_text("A001 KICK") && has_text("A002 SNARE") && has_text("USED") && has_text("D: SELECT"));
  CHECK(has_text("MB") && has_text("%"));   // the RAM figure
  // the tables tab: the template's built-ins
  enc(0, 1);
  frame();
  png("pool_2_tables.png");
  CHECK(has_text("TBL ") && has_text("A001 BASIC") && has_text("BUILT-IN"));
  enc(0, 1);
  frame();
  png("pool_3_instr.png");
  CHECK(has_text("INST "));
  enc(0, -2);
  // UNUSED: the rows not seen playing (hat, clap; kick seen, snare has a voice)
  pool_page_set(0, 0, 1, 1);
  key(0x44);
  frame();
  png("pool_4_unused.png");
  CHECK(pool_page_selected() == 2 && has_text("2 NOT SEEN PLAYING"));
  // UNLOAD the selection: nothing in use among them: done at once
  pool_page_set(0, 0, 1, 4);
  key(0x44);
  frame();
  CHECK(has_text("2 UNLOADED") && pools::at(pools::kSamples, 3).type == pools::kEmpty && pools::at(pools::kSamples, 4).type == pools::kEmpty);
  // UNDO: the last row back
  pool_page_set(0, 0, 1, 6);
  key(0x44);
  CHECK(pools::at(pools::kSamples, 4).type == pools::kSample);
  // P3: UNLOAD the cursor row kick (seen playing): asks, the second YES unloads; while the Rytm plays: refused
  pool_page_set(0, 0, 1, 4);
  g_play = true;
  key(0x44);
  frame();
  png("pool_5_playing.png");
  CHECK(has_text("1 IN USE: STOP FIRST") && pools::at(pools::kSamples, 1).type == pools::kSample);
  g_play = false;
  key(0x44);
  frame();
  png("pool_6_ask.png");
  CHECK(has_text("1 IN USE: YES AGAIN") && pools::at(pools::kSamples, 1).type == pools::kSample);
  key(0x44);
  CHECK(pools::at(pools::kSamples, 1).type == pools::kEmpty);
  // REPLACE A002 (a voice on it): the browser, the file asks first, then set; a file already in the list: its row
  pool_page_set(0, 0, 2, 5);
  key(0x44);
  CHECK(pool_page_browsing());
  // the browser lists from the library's RAM index (the card core's on the Pi): the library loaded here
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  CHECK(srclib::cd(srclib::kGr, "/Samples/Kit"));
  for (int i = 0; i < 100 && !srclib::listing_done(srclib::kGr); ++i) nm::step();
  frame();
  png("pool_7_replace.png");
  CHECK(has_text("A002 /SAMPLES/KIT") || has_text("A002 ..") || has_text("A002"));
  int tom = -1;
  for (int i = 0; i < srclib::count(srclib::kGr); ++i) {
    const char* n;
    if (srclib::item(srclib::kGr, i, &n, nullptr, nullptr) && !strcmp(n, "tom.wav")) tom = i;
  }
  CHECK(tom >= 0);
  pool_page_set(0, 0, 2, 5);
  key(0x44);   // (the browser again, its cursor to tom)
  for (int i = 0; i < tom; ++i) key(0x43);
  key(0x44);
  frame();
  CHECK(has_text("IN USE: YES AGAIN") && pool_page_browsing());
  key(0x44);
  CHECK(!pool_page_browsing() && !strcmp(pools::at(pools::kSamples, 2).name, "tom"));
  frame();
  png("pool_8_replaced.png");
  srclib::cd(srclib::kGr, "");
  system(("rm -rf '" + card + "'").c_str());
  printf("test_pool_page: PASS (%s)\n", g_dir.c_str());
  return 0;
}
