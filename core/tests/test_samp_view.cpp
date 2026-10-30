// core/tests/test_samp_view.cpp - the remote UI's SAMPLE and AUTO pages (core/ui/page_samp.cpp, page_auto.cpp) in their main states, drawn by
// the page itself (remote_render) with the sampler and the auto-sampler driven through their console commands and
// the IN blocks, as test_sampling.cpp does. Writes the frames as PNGs (scale 4, the Rytm's text in the host's font) to
// $RUI_OUT, default build/rui_samp: 1..5 (SAMPLE: idle with signal, armed with the threshold marker, recording, clipping, a take), 6..11 (AUTO: FM plan, running, done; JUNO plan, running, done with loops), 12 (AUTO, a Rytm track source with its level).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "msamp/cardq.h"
#include "msamp/msamp.h"
#include "samp/autosamp.h"
#include "samp/sampler.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;
using namespace core::ui;

static uint64_t g_us = 1000000;
static int g_page = -1;
static void page_frame(uint8_t fb[kBytes]) {
  static Canvas c;
  CHECK(remote_render(6 + g_page, 0, nullptr, c));
  preview(c, fb);
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


static void block() {
  for (int i = 0; i < 64 && msamp::step(); ++i) {}
  samp::tick();
  samp::asamp_tick();
  for (int i = 0; i < 4 && samp::poll(); ++i) {}
  for (int i = 0; i < 4 && samp::asamp_poll(); ++i) {}
  g_us += 250;
  host_fake_time(true, g_us);
}
// n frames of IN L/R (channels 10, 11): noise at amp, with a decaying hit every 0.25 s
static int g_ch2 = -1;   // a second channel (a Rytm track's) carrying the same signal
static void feed(uint32_t frames, float amp, uint32_t* at) {
  static uint32_t rnd = 5;
  int32_t f[kBlock * 12];
  for (uint32_t i = 0; i < frames; i += kBlock, *at += kBlock) {
    memset(f, 0, sizeof f);
    for (int k = 0; k < kBlock; ++k) {
      rnd = rnd * 1664525u + 1013904223u;
      float nz = ((rnd >> 8) / 8388608.0f - 1.0f), ph = (float)((*at + k) % 12000) / 12000.0f;
      float v = amp * nz * expf(-ph * 18.0f);
      f[k * 12 + 10] = (int32_t)(v * 2147483000.0f), f[k * 12 + 11] = (int32_t)(0.8f * v * 2147483000.0f);
      if (g_ch2 >= 0) f[k * 12 + g_ch2] = (int32_t)(v * 2147483000.0f);
    }
    tap_in(f, kBlock, 12);
    samp::in_block(f, kBlock, 12);
    block();
  }
}
static void cmd(const char* c) {
  char o[600];
  CHECK(samp::command(c, o, sizeof o));
}
static void acmd(const char* c) {
  char o[600];
  CHECK(samp::asamp_command(c, o, sizeof o));
}

int main() {
  const char* env = getenv("RUI_OUT");
  g_dir = env && *env ? env : "build/rui_samp";
  mkdir("build", 0755);
  mkdir(g_dir.c_str(), 0755);
  char tmpl[] = "/tmp/core_samp_view_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples' '" + card + "/MULTISAMPLE'").c_str()) == 0);
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  for (int i = 0; i < 4000 && msamp::step(); ++i) {}
  for (int k = 0; k < extra_pages(); ++k)
    if (!strcmp(extra_page(k).title, "SAMPLE")) g_page = k;
  CHECK(g_page >= 0);
  samp::reset();
  tap_enable(true);
  samp::AStatus as;
  samp::Status s;
  uint32_t at = 0;

  // 1: idle, IN L/R, threshold start, 2 bars, transient slices; the source's level in the meter
  cmd("samp=src:in"), cmd("samp=start:thr:30"), cmd("samp=len:2"), cmd("samp=slices:trans"), cmd("samp=pre:100");
  feed(24000, 0.15f, &at);   // signal on IN before anything is armed
  png("1_idle_signal.png");
  // 2: armed on the threshold, the source quiet under it
  cmd("samp=arm");
  samp::status(&s);
  CHECK(s.state == samp::kArmed);
  feed(24000, 0.01f, &at);
  png("2_armed_thr_marker.png");
  // 3: recording (hits over the threshold), the time and the level, the peak in the footer
  feed(60000, 0.5f, &at);
  samp::status(&s);
  CHECK(s.state == samp::kRecording);
  png("3_recording.png");
  feed(24000, 1.0f, &at);   // the source clips
  png("4_recording_clip.png");
  // 4: the take stopped, processed and written: its name and slices
  cmd("samp=stop");
  feed(480, 0.5f, &at);
  for (int i = 0; i < 400000; ++i) {
    samp::status(&s);
    if (s.state == samp::kDone || s.state == samp::kFailed) break;
    block();
  }
  CHECK(s.state == samp::kDone);
  png("5_take.png");

  // the AUTO page, driven through its own encoders and keys (A..H, YES, A pushed)
  int ga = -1;
  for (int k = 0; k < extra_pages(); ++k)
    if (!strcmp(extra_page(k).title, "AUTO")) ga = k;
  CHECK(ga >= 0);
  const ExtraPage& ap = extra_page(ga);
  auto turn = [&](int e, int d, bool func = false) { ap.enc(e, d, func); };
  auto wait_asamp = [&](int done_at) {
    for (int i = 0; i < 4000000; ++i) {
      samp::asamp_status(&as);
      if (as.state != samp::kATaking && as.state != samp::kAWriting) break;
      if (done_at && as.done >= done_at) break;
      block();
    }
  };
  auto shot = [&](const char* n) { g_page = ga; png(n); };
  // FM: the ROM's patch 3, C2..C6 every 6, 3 layers, 2 round robins, hold 1.5 s
  turn(1, 3), turn(5, 2), turn(6, 1), turn(2, -12);
  shot("6_auto_plan_fm.png");
  CHECK(samp::asamp_plan().nvel == 3 && samp::asamp_plan().rr == 2 && samp::asamp_plan().pat == 3);
  CHECK(ap.key(0x44));
  wait_asamp(14);
  CHECK(as.state == samp::kATaking);
  g_us += 30000000, host_fake_time(true, g_us);   // (the run's 30 s on the page's clock)
  shot("7_auto_running_fm.png");
  wait_asamp(0);
  for (int i = 0; i < 400; ++i) block();
  CHECK(as.state == samp::kADone);
  shot("8_auto_done_fm.png");
  CHECK(ap.key(0x45));   // NO: the plan again
  // JUNO patch 4: the sustained strings; 1 layer, loops on, release 2.5 s
  turn(0, 1);
  turn(1, 4);
  turn(5, -2), turn(6, -1);
  turn(7, 3);
  shot("9_auto_plan_juno.png");
  CHECK(ap.key(0x44));
  wait_asamp(5);
  CHECK(as.state == samp::kATaking);
  g_us += 12000000, host_fake_time(true, g_us);
  shot("10_auto_running_juno.png");
  wait_asamp(0);
  for (int i = 0; i < 400; ++i) block();
  CHECK(as.state == samp::kADone && as.loops >= 1);
  shot("11_auto_done_juno_loops.png");
  CHECK(ap.key(0x70));   // A pushed on DONE: the plan again
  CHECK(ap.key(0x44));   // YES starts a run; A pushed cancels it
  wait_asamp(2);
  CHECK(ap.key(0x70));
  for (int i = 0; i < 4000; ++i) block();
  samp::asamp_status(&as);
  CHECK(as.state == samp::kACancelled);
  // a Rytm track as the source (BD: IN 2): its live level in the bottom bar
  turn(0, 2);
  CHECK(samp::asamp_plan().src == samp::kATrack);
  g_ch2 = 2;
  feed(24000, 0.3f, &at);
  shot("12_auto_plan_track_meter.png");
  // an external synth (EXT): B the MIDI output and channel, the input in the bottom bar
  turn(0, 1);
  CHECK(samp::asamp_plan().src == samp::kAExt);
  shot("13_auto_plan_ext.png");
  printf("test_samp_view: PASS\n");
  system(("rm -rf '" + card + "'").c_str());
  return 0;
}
