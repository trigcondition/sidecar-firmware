// core/tests/test_wg_view.cpp - the remote UI's SRC view of GRANULAR and WAVETABLE (core/ui/page_wavegrain.cpp) drawn by
// the page itself (remote_render 1: the voice's machine page) while the machines play: GRANULAR on BELLS with ANIM
// (the source's outline, the head moving, the grains), frozen; WAVETABLE sweeping POS. Writes PNGs (scale 4, the
// Rytm's text in the host's font) to $RUI_OUT (default build/rui_wg): gr_1..3, wt_1..2, and gr_anim/frame-NNNN.png
// (12 frames a second, for a gif). Checks that the picture follows the cloud (the frames differ, the head moves).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "granular/granular.h"
#include "machines.h"
#include "nmach.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "wavetable/wavetable.h"
#include "wg_util.h"
#include "pool_util.h"

using namespace core;
using namespace core::ui;

static uint64_t g_us = 1000000;
static int g_voice = 0;
static Canvas g_c;
static void page_frame(uint8_t fb[kBytes]) {
  CHECK(remote_render(1, g_voice, nullptr, g_c));
  preview(g_c, fb);
}
static bool has_text(const char* t) {   // a text item of the last frame holds t
  for (int i = 0; i < kTextSlots; ++i)
    if (strstr(g_c.txt[i].s, t)) return true;
  return false;
}
static std::string title() { return g_c.txt[kSlotTitle].s; }
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



// t seconds of voice vi's machine rendered (12-frame blocks, the clock on)
static void play(int vi, double t, bool gr_) {
  float o[kBlock];
  for (int b = 0; b < (int)(t * 48000 / kBlock); ++b) {
    for (int i = 0; i < 8 && nm::step(); ++i) {}
    memset(o, 0, sizeof o);
    if (gr_) gr::render_add(vi, o, kBlock);
    else wt::render_add(vi, o, kBlock);
    g_us += 250;
    host_fake_time(true, g_us);
  }
}
static uint32_t frame_hash() {
  uint8_t fb[kBytes];
  page_frame(fb);
  uint32_t h = 2166136261u;
  for (int i = 0; i < kBytes; ++i) h = (h ^ fb[i]) * 16777619u;
  return h;
}

int main() {
  const char* env = getenv("RUI_OUT");
  g_dir = env && *env ? env : "build/rui_wg";
  mkdir("build", 0755);
  mkdir(g_dir.c_str(), 0755);
  mkdir((g_dir + "/gr_anim").c_str(), 0755);
  char tmpl[] = "/tmp/core_wg_view_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples/Sidecar' '" + tmpl + "/MULTISAMPLE' '" + tmpl +
                "/GRANULAR/texture'").c_str()) == 0);
  // GRANULAR's list: two of /GRANULAR (one in a folder) and two takes of the sampler (/Samples/<project>)
  auto tone = [&](const std::string& path, double hz, int frames) {
    wg::write_file(std::string(tmpl) + path, wg::wav_bytes(frames, 1, 16, 48000, 1, "", [hz](int i, int) {
                     return 0.4 * std::sin(2 * M_PI * hz * i / 48000) * std::exp(-i / 30000.0);
                   }));
  };
  tone("/GRANULAR/choir.wav", 330, 24000);
  tone("/GRANULAR/texture/rain.wav", 520, 24000);
  tone("/Samples/Sidecar/SMP001.wav", 110, 24000);
  tone("/Samples/Sidecar/SMP002.wav", 220, 24000);
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  hooks.to_rytm = [](const uint8_t*, int) {};
  CHECK(engine_begin());
  for (int i = 0; i < 4000 && nm::step(); ++i) {}
  // the SAMPLES pool: the inputs A001..A010, PAD BELLS A011 A012... here BELLS at A011 (the old SRC 11), then the files
  pu::clear(pools::kSamples);
  for (int i = 0; i < pools::kInputs; ++i) {
    char r[8], n[16];
    snprintf(r, sizeof r, "A%03d", i + 1), snprintf(n, sizeof n, "@%s", pools::input_name(i));
    CHECK(pu::put(pools::kSamples, r, n) == pools::kOk);
  }
  CHECK(pu::put(pools::kSamples, "A011", "@BELLS") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A012", "/GRANULAR/choir.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A013", "/GRANULAR/texture/rain.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A014", "/Samples/Sidecar/SMP001.wav") == pools::kOk);
  CHECK(pu::put(pools::kSamples, "A015", "/Samples/Sidecar/SMP002.wav") == pools::kOk);
  // GRANULAR on voice 0: BELLS, POS 40, SPRY 64, 60 ms grains at 40 a second, PRND, REV, HANN, ANIM 56 (1BAR, depth .44)
  // P1..P16: SRC POS SPRY SIZE DENS PRND REV WIN ATK REL HOLD ANIM CHD INV SPR VOI
  const float sz = (float)(127 * std::log(0.06 / 0.005) / std::log(200.0)), dn = (float)(127 * std::log(40.0) / std::log(200.0));
  std::vector<float> p = {11, 40, 64, sz, dn, 3, 30, 0, 20, 70, 40, 56, 0, 0, 0, 0};
  g_voice = 0;
  wg::trig(kGranMachine, 0, 60, 110, p);
  play(0, 0.5, true);
  png("gr_1_anim.png");
  uint32_t h0 = frame_hash();
  gr::Viz z0, z1;
  CHECK(gr::viz(0, &z0) && z0.n > 0);
  play(0, 0.4, true);
  png("gr_2_anim_later.png");
  CHECK(gr::viz(0, &z1) && frame_hash() != h0 && std::fabs(z1.head - z0.head) > 0.01f);
  // the animation: 4 s at 12 frames a second
  char name[64];
  for (int f = 0; f < 48; ++f) {
    play(0, 1.0 / 12, true);
    snprintf(name, sizeof name, "gr_anim/frame-%04d.png", f);
    png(name);
  }
  // frozen (HOLD 127), ANIM off, a denser cloud: the head stays
  p[10] = 127, p[11] = 0, p[4] = dn + 20;
  wg::params(kGranMachine, 0, p);
  play(0, 0.5, true);
  png("gr_3_frozen.png");
  // the source list: SRC turned -> the list (names around the value, the groups, the selection inverted); still
  // there 1 s later, gone after kListMs: the page again, the name top left and in the bottom bar
  CHECK(gr::stats().listed == 15);
  CHECK(!strcmp(gr::entry_group(12), "BANK A") && !strcmp(gr::entry_name(13), "rain") && !strcmp(gr::entry_name(15), "SMP002"));
  png("gr_4_page_bells.png");
  CHECK(has_text("BELLS") && title() == "GRANULAR");
  p[0] = 12;
  wg::params(kGranMachine, 0, p);
  png("gr_list_1_choir.png");
  CHECK(title() == "SOURCE" && has_text("A012 CHOIR") && has_text("BELLS") && has_text("BUILT-IN") && has_text("LOADING") &&
        has_text("SRC:CHOIR"));
  p[0] = 15;
  wg::params(kGranMachine, 0, p);
  png("gr_list_2_takes.png");
  CHECK(title() == "SOURCE" && has_text("A015 SMP002") && has_text("A013 RAIN"));
  p[0] = 4;
  wg::params(kGranMachine, 0, p);
  png("gr_list_3_live.png");
  CHECK(has_text("A004 SD") && has_text("LIVE"));
  play(0, 1.0, true);
  png("gr_list_4_later.png");
  CHECK(title() == "SOURCE");
  play(0, 1.2, true);
  png("gr_5_page_sd.png");
  CHECK(title() == "GRANULAR" && has_text("SD") && has_text("SRC:SD"));
  // WAVETABLE on voice 1: BASIC (0: sine .. triangle .. saw .. square), POS 20 then 100
  g_voice = 1;
  std::vector<float> w = {1, 20, 0, 0, 0, 0, 64, 0, 10, 60, 100, 64, 0, 0, 0, 0};   // TBL A001 BASIC
  wg::trig(kWaveMachine, 1, 48, 110, w);
  play(1, 0.3, false);
  png("wt_1_pos20.png");
  uint32_t hw = frame_hash();
  w[1] = 100;
  wg::params(kWaveMachine, 1, w);
  play(1, 0.3, false);
  png("wt_2_pos100.png");
  CHECK(frame_hash() != hw);
  CHECK(has_text("BASIC"));   // the table's name top left
  w[0] = 4;
  wg::params(kWaveMachine, 1, w);
  png("wt_list_1.png");
  CHECK(title() == "TABLE" && has_text("BUILT-IN") && has_text("TBL:"));
  // ---- a missing file: the row reads "!name" in the list and is silent
  g_voice = 0;
  auto rest = [&](int ms) {   // time passing with the view drawn
    uint8_t fb[kBytes];
    for (int t = 0; t < ms; t += 50) {
      for (int i = 0; i < 40 && nm::step(); ++i) {}
      g_us += 50000;
      host_fake_time(true, g_us);
      page_frame(fb);
    }
  };
  rest(2500);   // the WAVETABLE list gone
  CHECK(pu::put(pools::kSamples, "B002", "/Samples/Sidecar/SMP001.wav") == pools::kExisting);   // one entry per file
  p[0] = 15;
  wg::params(kGranMachine, 0, p);
  rest(100);
  CHECK(title() == "SOURCE" && has_text("A015 SMP002"));
  rest(2500);
  png("gr_lib_6_page.png");
  CHECK(title() == "GRANULAR" && has_text("SMP002"));
  system((std::string("rm -rf '") + tmpl + "'").c_str());
  printf("test_wg_view: PASS (%s)\n", g_dir.c_str());
  return 0;
}
