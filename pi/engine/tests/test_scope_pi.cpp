// pi/engine/tests/test_scope_pi.cpp - the SCOPE page's provider (scope_pi.cpp) against a stand-in Overbridge input tap:
// TRACK follows the Rytm's selected track (its voice's stem), a triggered timebase starts on a rising crossing, a sweep
// fills from the left and wraps with its cursor, a source / timebase change starts over, no tap writes: not valid.
#include <math.h>
#include <string.h>

#include "../scope_pi.h"
#include "check.h"
#include "platform.h"
#include "ui/extra.h"
#include "ui/remote.h"

// ---- stand-ins: the tap (core/ui/page_scope.cpp), the selected track (remote.cpp), the clock
static int16_t g_ring[1 << 16][2];
static uint32_t g_wr, g_ms = 1000;
static int g_src = -1, g_track = 0;
namespace core {
uint32_t now_ms() { return g_ms; }
namespace ui {
volatile bool tap_enabled = false;
void tap_enable(bool on) { tap_enabled = on; }
void tap_source(int src) { g_src = src; }
uint32_t tap_write_pos() { return g_wr; }
int tap_read(uint32_t* rd, int16_t (*out)[2], int max) {
  int n = 0;
  for (; *rd != g_wr && n < max; ++*rd, ++n) out[n][0] = g_ring[*rd & 0xFFFF][0], out[n][1] = g_ring[*rd & 0xFFFF][1];
  return n;
}
const char* tap_source_name(int src) {
  static const char* const n[kTapSources] = {"MAIN", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "IN L", "IN R"};
  return n[src < 0 || src >= kTapSources ? 0 : src];
}
int remote_selected_track(uint32_t* gen) {
  if (gen) *gen = 1;
  return g_track;
}
}  // namespace ui
}  // namespace core

// n entries of a sine (period p entries, amplitude a of full scale), each entry a min / max pair around the value
static double g_ph;
static void write_sine(int n, double p, double a) {
  for (int i = 0; i < n; ++i, g_ph += 1.0) {
    int v = (int)(sin(2 * M_PI * g_ph / p) * a * 32767);
    g_ring[g_wr & 0xFFFF][0] = (int16_t)(v - 50), g_ring[g_wr & 0xFFFF][1] = (int16_t)(v + 50);
    ++g_wr;
  }
}
static int mid(const sidecar_scope_view_t& o, int i) { return (o.lo[i] + o.hi[i]) / 2; }

int main() {
  static sidecar_scope_view_t o;
  uint32_t pk[SSC_CH];
  for (int c = 0; c < SSC_CH; ++c) pk[c] = 1000u * (unsigned)(c + 1);
  // TRACK: the Rytm's selected track BD -> source 1; nothing written yet: not valid, no columns
  scope_pi::Fill(&o, -1, 1, pk);
  CHECK(o.src == 1 && g_src == 1 && o.track == 0 && core::ui::tap_enabled);
  CHECK(o.ncols == 0 && o.cursor == -1 && !o.valid);
  CHECK(!strcmp(o.name[3], "RS/CP") && o.peak[11] == 12000);
  // 85 ms (2 entries a column): 600 columns of a sine 41.3 columns long, triggered on a rising crossing
  write_sine(1200, 82.6, 0.5);
  unsigned g0 = o.gen;
  scope_pi::Fill(&o, -1, 1, pk);
  CHECK(o.valid && o.ncols == SSC_COLS && o.cursor == -1 && o.gen != g0);
  CHECK(mid(o, 0) >= -2000 && mid(o, 0) < 6000 && mid(o, 2) > mid(o, 0));   // starts at the crossing, going up
  int peak = 0;
  for (int i = 0; i < o.ncols; ++i) peak = o.hi[i] > peak ? o.hi[i] : peak;
  CHECK(peak > 15000 && peak < 17500);   // the sine's half scale (two entries' min / max)
  // the Rytm selects CP: its voice's stem RS/CP (source 3); the trace starts over
  g_track = 3;
  scope_pi::Fill(&o, -1, 1, pk);
  CHECK(o.src == 3 && g_src == 3 && o.ncols == 0);
  // a chosen channel (IN L) and 341 ms (8 entries a column, a sweep): 100 columns from the left, the cursor after them
  scope_pi::Fill(&o, 9, 3, pk);
  CHECK(o.src == 9 && g_src == 9 && o.tb == 3 && o.ncols == 0 && o.cursor == 0);
  write_sine(800, 400, 0.25);
  scope_pi::Fill(&o, 9, 3, pk);
  CHECK(o.ncols == 100 && o.cursor == 100);
  write_sine(1600, 400, 0.25);
  g0 = o.gen;
  scope_pi::Fill(&o, 9, 3, pk);
  CHECK(o.ncols == SSC_COLS && o.cursor == 300 - SSC_COLS && o.gen != g0);
  // nothing new: the gen stays (the page does not redraw the trace); 600 ms without a write: not valid
  g0 = o.gen;
  scope_pi::Fill(&o, 9, 3, pk);
  CHECK(o.gen == g0 && o.valid);
  g_ms += 600;
  scope_pi::Fill(&o, 9, 3, pk);
  CHECK(!o.valid);
  // TRACK's mapping: every Rytm track to its voice's stem
  static const int kWant[12] = {1, 2, 3, 3, 4, 5, 6, 6, 7, 7, 8, 8};
  for (int t = 0; t < 12; ++t) CHECK(scope_pi::SourceOfTrack(t) == kWant[t]);
  scope_pi::Leave();
  CHECK(!core::ui::tap_enabled);
  printf("test_scope_pi: PASS\n");
  return 0;
}
