// core/tests/test_sdtest.cpp - the SD card test (core/msamp/sdtest.h): the verdict for set speeds (a fast A2 card, the
// Pi's measured card, a slow one, a far too slow one); the runner on a fake card with set speeds (sequential, random
// with jitter, writes stalling the reads): every phase, the numbers near the fake's, the result file on the card, the
// test files removed; cancel; too little free space refused; the console's answers.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "check.h"
#include "msamp/msamp.h"
#include "msamp/sdtest.h"
#include "platform.h"

using namespace core;
using namespace core::sdtest;

static Measured card(float w, float r, float mean32, float max32, float mixed_mean, float mixed_max) {
  Measured m = {};
  m.write_mbps = w, m.read_mbps = r;
  for (int i = 0; i < kSizes; ++i) m.rand[i] = {1000 / mean32, mean32, max32 * 0.8f, max32, mean32};
  m.mixed = {1000 / mixed_mean, mixed_mean, mixed_max * 0.8f, mixed_max, mixed_mean};
  return m;
}

static void test_judge() {
  Verdict a = judge(card(40, 80, 0.8f, 5, 1.2f, 30));   // a fast A2 card
  CHECK(a.level == 2 && a.voices >= 64 && a.voices_rec == 50 && a.streams == 50 && a.ahead_ms == 100);
  CHECK(strstr(a.text, "+ recorder"));
  Verdict p = judge(card(9.2f, 20, 2, 10, 4, 137));   // the Pi's card as the recorder measured it (worst write 137 ms)
  CHECK(p.level == 2 && p.voices == 33 && p.voices_rec == 10 && p.streams == 10 && p.ahead_ms == 235);
  Verdict s = judge(card(3, 8, 8, 60, 12, 400));   // slow: fine alone, not with the recorder
  CHECK(s.level == 1 && s.voices == 8 && s.voices_rec == 0 && s.ahead_ms == 600 && strstr(s.text, "without the recorder"));
  Verdict z = judge(card(1, 2, 20, 300, 40, 900));   // far too slow
  CHECK(z.level == 0 && z.voices < kMinVoices && strstr(z.text, "too slow") && strstr(z.text, "RAM mode"));
  Verdict l = judge(card(40, 80, 0.8f, 500, 1, 500));   // fast but a 500 ms worst read: the ring cannot cover it
  CHECK(l.level == 0 && l.ahead_ms == 600);
}

// ---- a fake card: a clock its operations advance
struct Fake {
  double w_mbps, r_mbps, lat_ms, jitter_ms, stall_ms;   // stall: added to a read now and then while writes run
  uint64_t free_b;
  bool exists[2], open[2];
  uint64_t size[2];
  uint64_t us;
  uint32_t rnd;
  int ops;
} F;
static double fr() { F.rnd = F.rnd * 1664525u + 1013904223u; return (F.rnd >> 8) / 16777216.0; }
static bool f_open(int w, bool write) {
  if (write) F.exists[w] = true, F.size[w] = 0;
  if (!F.exists[w]) return false;
  F.open[w] = true;
  return true;
}
static int f_read(int w, uint64_t off, void*, uint32_t n) {
  ++F.ops;
  if (!F.open[w] || off + n > F.size[w]) return -1;
  double ms = F.lat_ms + F.jitter_ms * fr() + n / (F.r_mbps * 1048.576);
  if (F.open[1] && fr() < 0.05) ms += F.stall_ms;   // the second file is being written: some reads wait for it
  F.us += (uint64_t)(ms * 1000);
  return (int)n;
}
static int f_write(int w, const void*, uint32_t n) {
  ++F.ops;
  if (!F.open[w]) return -1;
  F.size[w] += n;
  F.us += (uint64_t)(n / (F.w_mbps * 1048.576) * 1000);
  return (int)n;
}
static void f_close(int w) { F.open[w] = false; }
static void f_remove(int w) { F.exists[w] = false, F.size[w] = 0; }
static uint64_t f_free() { return F.free_b; }
static uint32_t f_now() { return (uint32_t)F.us; }
static const Io kFake = {f_open, f_read, f_write, f_close, f_remove, f_free, f_now};

static std::string g_dir;
static void test_runner() {
  F = {};
  F.w_mbps = 10, F.r_mbps = 20, F.lat_ms = 0.5, F.jitter_ms = 1.5, F.stall_ms = 120, F.free_b = 8ull << 30, F.rnd = 3;
  set_io(&kFake);
  char o[300];
  CHECK(command("sdtest", false, o, sizeof o) && strstr(o, "not run yet"));
  CHECK(command("sdtest=start", true, o, sizeof o) && strstr(o, "sdtest=start!"));   // busy: refused
  CHECK(state() != kRunning);
  CHECK(command("sdtest=start!", true, o, sizeof o) && state() == kRunning);
  int steps = 0, last = -1;
  bool mono = true;
  while (step()) {
    ++steps;
    int p = progress();
    mono = mono && p >= last;
    last = p;
  }
  CHECK(state() == kDone && mono && progress() == 100);
  CHECK(steps > 8192 + 3 * kRandN + kMixedN);   // 256 MB: 4096 writes and 4096 reads of 64 KB, then the random ones
  const Measured& m = measured();
  CHECK(std::fabs(m.write_mbps - 10) < 0.2 && std::fabs(m.read_mbps - 1 / (0.0005 + 0.00075 + 0.064 / 20.0) * 0.0625) < 1.5);
  CHECK(m.rand[1].mean_ms > 0.5 + 1.5 * 0.3 && m.rand[1].mean_ms < 0.5 + 1.5 * 0.7 + 1.6 && m.rand[1].max_ms < 4);
  CHECK(m.mixed.max_ms > 100 && m.mixed.p50_ms < 4);   // the writes' stalls show only with them
  CHECK(!F.exists[0] && !F.exists[1] && !F.open[0] && !F.open[1]);   // cleaned up
  const Verdict& v = verdict();
  Verdict j = judge(m);
  CHECK(v.level == j.level && v.voices == j.voices && v.ahead_ms == j.ahead_ms && v.level >= 1);
  CHECK(command("sdtest", false, o, sizeof o) && strstr(o, v.text));
  // the result on the card, read back at the next boot
  FILE* fp = fopen((g_dir + "/.rytm/sdtest.txt").c_str(), "r");
  CHECK(fp);
  char t[800] = {0};
  fread(t, 1, sizeof t - 1, fp);
  fclose(fp);
  char want[40];
  snprintf(want, sizeof want, "ahead_ms=%lu\n", (unsigned long)v.ahead_ms);
  CHECK(strstr(t, want) && strstr(t, "streams=") && strstr(t, "verdict="));
  char js[600];
  CHECK(json(js, sizeof js) > 0 && strstr(js, "\"state\":\"done\"") && strstr(js, "\"have\":1"));
  CHECK(!strncmp(line(), "SD ", 3));
  // cancel halfway: files gone
  CHECK(start() && state() == kRunning);
  for (int i = 0; i < 1000; ++i) step();
  CHECK(F.exists[0]);
  cancel();
  CHECK(state() == kCancelled && !F.exists[0] && !F.exists[1]);
  // too little room
  F.free_b = 40ull << 20;
  CHECK(!start() && state() == kFailed && strstr(why(), "64 MB needed"));
  // a small card: a quarter of its free space (but 32 MB at least)
  F.free_b = 200ull << 20;
  CHECK(start());
  int n = 0;
  while (step()) ++n;
  CHECK(state() == kDone && n < 2 * 800 + 1000);
  // a slow card: too slow
  F = {};
  F.w_mbps = 1, F.r_mbps = 2, F.lat_ms = 15, F.jitter_ms = 10, F.stall_ms = 400, F.free_b = 8ull << 30, F.rnd = 5;
  CHECK(start(32));
  while (step()) {}
  CHECK(state() == kDone && verdict().level == 0 && !strcmp(line(), "SD SLOW"));
  // load(): the file's verdict back
  set_io(nullptr);
}

int main() {
  test_judge();
  char tmpl[] = "/tmp/sdtest-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_dir = tmpl;
  host_set_card(tmpl);
  test_runner();
  load();
  CHECK(verdict().level == 0 && verdict().ahead_ms > 0);
  system(("rm -rf '" + g_dir + "'").c_str());
  printf("test_sdtest: PASS\n");
  return 0;
}
