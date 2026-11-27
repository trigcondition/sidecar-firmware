// core/tests/test_draw.cpp - drawn pages (sdk/rmp/rmp_api3.h RMP3_CAP_DRAW; core/user/um.cpp on_draw, core/ui/remote.cpp
// drawn_frame): a user machine's plugin draws page 0, the Rytm's SRC view of a track that plays it, with real RMPA
// packages run in place on this arm64 Mac (plugins/api3_test drawprobe.c: api3_draw, api3_drawnw; its P1 picks what the
// draw does). Checked:
//   - rows_to_fb: a row-major frame into the canvas layout (a known pattern, the corners);
//   - the SRC view (29 t 1): the draw called with page 0, the 2A's P1..P16 (the fraction too), the voice's state; its
//     frame sent, no text items; redrawn on a changed value, not on the same values or while nothing changes;
//   - return 0: the host's page (for a user machine today the stock one: 3A 0 0), drawn again after;
//   - the hard limit: a draw that never returns is left after RMP3_DRAW_LIMIT_US; the frame is the host's, the slot's
//     sound plays on (not muted), no more draws until the slot loads again (um=reload), then it draws again;
//   - a draw under the limit (~100 us on a Pi 4) is not stopped;
//   - a write past fb's end and one before its start: the slot muted, the frame not used, the host's canvas intact;
//   - a fault: the slot muted, stock, no more draws;
//   - the host's log does nothing inside a draw;
//   - gating: a plugin with a draw but without RMP3_CAP_DRAW in wants (api3_drawnw) and a plugin with no draw
//     (api3_test) are never drawn; a host without a hard limit (Exec::guard_timed null) offers no draw at all.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "host_link.h"
#include "rmp_api3.h"
#include "rmp_slots.h"
#include "ui/remote.h"
#include "ui/ui.h"
#include "um_host.h"
#include "user/um.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

// ---- rows_to_fb: any host
static void rows_test() {
  uint8_t rows[kBytes] = {}, fb[kBytes];
  auto lit = [&](int x, int y) { rows[16 * y + x / 8] |= (uint8_t)(0x80 >> (x % 8)); };
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      if ((x * 7 + y * 3) % 11 == 0 || x == y) lit(x, y);
  lit(127, 0), lit(0, 63), lit(127, 63);
  memset(fb, 0xAA, sizeof fb);
  rows_to_fb(rows, fb);
  Canvas c;
  c.clear();
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      if (rows[16 * y + x / 8] >> (7 - x % 8) & 1) c.pixel(x, y);
  CHECK(memcmp(c.fb, fb, kBytes) == 0);
  CHECK(fb[7] & 1);                         // (0, 0): byte 7, bit 0
  CHECK(fb[7 + 8 * 127] & 1);               // (127, 0)
  CHECK(fb[0] >> 7 & 1);                    // (0, 63): byte 0, bit 7
  CHECK(fb[8 * 127] >> 7 & 1);              // (127, 63)
  printf("test_draw: rows_to_fb matches Canvas::pixel, the corners\n");
}

#if defined(__aarch64__)
static std::vector<std::string> g_log;
static void sink(const char* s, int n) { g_log.emplace_back(s, (size_t)n); }
static bool logged(const char* w) {
  for (auto& l : g_log)
    if (l.find(w) != std::string::npos) return true;
  return false;
}
static std::vector<std::vector<uint8_t>> g_msgs;
static void out(const uint8_t* m, int n) { g_msgs.emplace_back(m, m + n); }
static std::string slurp(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  CHECK(f);
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
  for (int i = 0; i < 200000; ++i) {
    if (um::idle()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static std::string cmd(const std::string& c) {
  char o[220];
  CHECK(um::command(c.c_str(), o, sizeof o));
  idle_all();
  return o;
}
static uint32_t g_ms = 1000;
static void tick(int n) {
  for (int i = 0; i < n; ++i) remote_tick(g_ms += 30);
}
// a 0x04 for voice vi on slot s: P1..P16 at 64 but P1 0, P2 100
static void report(int s, int vi) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x04, (uint8_t)(0x50 + 8 * s + vi), 60, 100};
  for (int k = 0; k < 16; ++k) m.push_back(k == 0 ? 0 : k == 1 ? 100 : 64), m.push_back(0);
  m.push_back(0xf7);
  sysex(m);
  idle_all();
}
// 2A: track t's P1..P16, each value << 7 | fraction
static void src_vals(int t, const std::vector<int>& p14) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, 0, 16};
  for (int k = 0; k < 16; ++k) {
    int v = k < (int)p14.size() ? p14[k] : 0;
    m.push_back((uint8_t)(v >> 7)), m.push_back((uint8_t)(v & 0x7F));
  }
  m.push_back(0xF7);
  sysex(m);
}
static void one_val(int t, int k, int v) {   // 2A of one value (a knob's turn)
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, (uint8_t)t, (uint8_t)k, 1, (uint8_t)v, 0, 0xF7});
}
static void view(int t, int on) { sysex({0xF0, 0x7D, 0x52, 0x54, 0x29, 1, (uint8_t)t, (uint8_t)on, 0xF7}); }
static int px(int x, int y) { return remote_frame()[(7 - y / 8) + 8 * x] >> (y % 8) & 1; }
static int run(int y) {   // the lit pixels from x 0 on row y (a bar's length)
  int n = 0;
  while (n < kW && px(n, y)) ++n;
  return n;
}
static bool pattern_ok() {
  for (int y = 48; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      if (px(x, y) != ((x + 2 * y) % 5 == 0)) return false;
  return true;
}
static int texts() {
  uint8_t t[36 * kTextSlots];
  return sent_texts(t, kTextSlots);
}
static bool stock_asked() {   // 3A 0 0 since g_msgs was cleared: the Rytm's own page again
  for (auto& m : g_msgs)
    if (m.size() == 9 && m[4] == 0x3A && m[6] == 0 && m[7] == 0) return true;
  return false;
}
static um::Exec g_nolimit;   // host_um_exec without guard_timed (an older host's view: no hard limit, no draw)

// The card, the engine, the plugins in U1..U3, the Rytm's 33; nolimit: the host without Exec::guard_timed
static void boot(bool nolimit) {
  char tmpl[] = "/tmp/core-draw-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_set_log_sink(sink);
  host_fake_time(true, 1000000);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/projects/PD"));
  put("/.rytm/project", "PD");
  for (const char* n : {"api3_draw", "api3_drawnw", "api3_test"})
    put(std::string("/Plugins/") + n + ".rmp", slurp(std::string("tests/api3_pkgs/") + n + ".a64.rmp"));
  hooks.to_rytm = ::out;
  CHECK(engine_begin());
  g_nolimit = *host_um_exec();
  g_nolimit.guard_timed = nullptr;
  um::begin(nolimit ? &g_nolimit : host_um_exec());
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'P', 'D', 0xf7});
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  idle_all();
  cmd("um=empty");
  cmd("um=assign:1:rytm.api3_draw");
  cmd("um=assign:2:rytm.api3_drawnw");
  cmd("um=assign:3:rytm.api3_test");
  CHECK(um::slot_view(0).state == RMP_SLOT_READY && um::slot_view(1).state == RMP_SLOT_READY &&
        um::slot_view(2).state == RMP_SLOT_READY);
  CHECK(hooks.user_draw && hooks.user_draw_key);
  // the Rytm's 33: BD (voice 0) on U1, SD (voice 2) on U3, BT (voice 1) on U2, LT (voice 3) on 6-OP FM
  sysex({0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 6, 0, 0x50 | 0, 0, 0x60 | 2, 0, 0, 0, 0, 0, 0x58 | 1, 0, kFm | 3, 0xF7});
  CHECK(remote_track_machine(0) == 0x50 && remote_track_machine(4) == (0x58 | 1) && remote_track_machine(1) == (0x60 | 2));
  report(0, 0);   // voice 0 plays U1: its state exists
}

// ---- a host without a hard limit: no RMP3_CAP_DRAW, so U1's draw is never called (the stock page, as before)
static int nolimit_main() {
  boot(true);
  src_vals(0, {0, 100 << 7});
  view(0, 1);
  tick(4);
  CHECK(remote_stats().user_draws == 0 && remote_stats().src_frames == 0);
  printf("test_draw: a host without guard_timed offers no draw: U1 not drawn, stock\n");
  return 0;
}

int main() {
  rows_test();
  {   // (a child first: um::begin once a process)
    fflush(stdout);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
      const int r = nolimit_main();
      fflush(stdout);
      _exit(r);
    }
    int st = 0;
    CHECK(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  boot(false);

  // ---- the SRC view of BD: page 0, the 2A's values, the state
  std::vector<int> pv = {0, 100 << 7, 5 << 7 | 64, 64 << 7, 20 << 7, 0, 0, 0, 30 << 7, 0, 0, 0, 0, 0, 0, 127 << 7};
  src_vals(0, pv);
  const uint32_t f0 = remote_stats().src_frames;
  view(0, 1);
  tick(3);
  CHECK(remote_stats().page == 4 && remote_stats().user_draws == 1 && remote_stats().user_drawn == 1);
  CHECK(remote_stats().src_frames == f0 + 1);
  CHECK(run(1) == 0 && run(3) == 100 && run(5) == 5 && run(7) == 64 && run(9) == 20 && run(17) == 30 && run(31) == 127);
  CHECK(run(34) == 1 && px(0, 36));            // page 0; a state
  CHECK(pattern_ok() && texts() == 0);
  printf("test_draw: SRC view drawn by U1: page 0, P1..P16, the state, no text items\n");

  // ---- redraws: a knob's 2A; the same value again (none); nothing (none)
  one_val(0, 4, 77);
  tick(2);
  CHECK(remote_stats().user_draws == 2 && run(9) == 77);
  one_val(0, 4, 77);
  tick(4);
  tick(10);
  CHECK(remote_stats().user_draws == 2);
  printf("test_draw: redrawn on a changed value, not on the same value or while nothing changes\n");

  // ---- return 0 (P1 16): the host's page, stock for a user machine (3A 0 0); P1 back: drawn again
  g_msgs.clear();
  one_val(0, 0, 16);
  tick(3);
  CHECK(stock_asked());
  uint32_t f = remote_stats().src_frames;
  one_val(0, 0, 0);
  tick(3);
  CHECK(remote_stats().src_frames > f && run(3) == 100 && pattern_ok());
  printf("test_draw: a draw returning 0 gives the host's (stock) page, drawn again after\n");

  // ---- the host's log inside a draw (P1 112): no line, the frame drawn
  g_log.clear();
  one_val(0, 0, 112);
  tick(3);
  CHECK(!logged("drawprobe:") && run(1) == 112 && pattern_ok());
  printf("test_draw: the host's log does nothing inside a draw\n");

  // ---- under the limit (P1 96, ~100 us on a Pi 4): drawn, not stopped
  g_log.clear();
  one_val(0, 0, 96);
  tick(3);
  CHECK(run(1) == 96 && !logged("draw stopped"));
  printf("test_draw: a draw under the limit is not stopped\n");

  // ---- the hard limit (P1 48: never returns): left at the limit, stock, the slot not muted, no more draws
  g_msgs.clear();
  g_log.clear();
  one_val(0, 0, 48);
  tick(3);
  CHECK(logged("draw stopped at its 250 us limit") && stock_asked() && !um::slot_view(0).muted);
  uint32_t d = remote_stats().user_draws;
  f = remote_stats().src_frames;
  one_val(0, 0, 0);
  tick(4);
  CHECK(remote_stats().user_draws == d && remote_stats().src_frames == f);
  {   // its sound plays on: voice 0 still renders U1 (the engine's render of the voice does not stop)
    float o[kBlock];
    CHECK(hooks.user_render);
    hooks.user_render(0, o);
    CHECK(!um::slot_view(0).muted);
  }
  cmd("um=reload:1");
  CHECK(um::slot_view(0).state == RMP_SLOT_READY);
  report(0, 0);
  src_vals(0, pv);
  tick(4);
  CHECK(remote_stats().user_draws > d && run(3) == 100 && pattern_ok());
  printf("test_draw: a draw that never returns is stopped at the limit; sound on; draws again after a reload\n");

  // ---- writes outside the frame: past its end (P1 64), before its start (P1 80): muted, the frame not used
  for (int mode : {64, 80}) {
    g_msgs.clear();
    g_log.clear();
    one_val(0, 0, mode);
    tick(3);
    CHECK(um::slot_view(0).muted && logged("muted: its draw wrote outside the frame") && stock_asked());
    d = remote_stats().user_draws;
    one_val(0, 0, 0);
    tick(4);
    CHECK(remote_stats().user_draws == d);
    cmd("um=reload:1");
    report(0, 0);
    src_vals(0, pv);
    tick(4);
    CHECK(!um::slot_view(0).muted && run(3) == 100 && pattern_ok());
  }
  printf("test_draw: writes past and before the frame mute the slot, the frame not used\n");

  // ---- a fault (P1 32): muted (the guard), stock, no more draws
  g_msgs.clear();
  g_log.clear();
  one_val(0, 0, 32);
  tick(3);
  CHECK(um::slot_view(0).muted && logged("muted: crashed in draw") && stock_asked());
  d = remote_stats().user_draws;
  one_val(0, 0, 0);
  tick(4);
  CHECK(remote_stats().user_draws == d);
  view(0, 0);
  printf("test_draw: a faulting draw mutes U1, stock, no more draws\n");

  // ---- gating: api3_drawnw (a draw, not in wants) on BT and api3_test (no draw) on SD: never drawn
  for (int t : {4, 1}) {
    d = remote_stats().user_draws, f = remote_stats().src_frames;
    src_vals(t, {0, 100 << 7});
    view(t, 1);
    tick(4);
    CHECK(remote_stats().user_draws == d && remote_stats().src_frames == f);
    view(t, 0);
  }
  printf("test_draw: no draw without RMP3_CAP_DRAW in wants, none without a draw\n");
  printf("test_draw: PASS\n");
  return 0;
}
#else
int main() {
  rows_test();
  printf("test_draw: the packages are AArch64 - only rows_to_fb here\n");
  return 0;
}
#endif
