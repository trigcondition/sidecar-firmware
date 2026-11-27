// pi/engine/tests/test_wgview_pi.cpp - the display's machine view for WAVETABLE (wgview_pi.cpp Fill) on the core's Mac library:
// table A's row (sel, name) as ever, table B's (TBLB, P13): sel_b the code, name_b its table's name, mix0 whether B is silent for
// lack of MIX (select_b: MIX 0 and the warp neither FM nor RM), and the hash follows each. make -C pi/engine/tests (make -C core
// engines lib first).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "../wgview_pi.h"
#include "check.h"
#include "engine.h"
#include "machines.h"
#include "voices.h"
#include "wavetable/wavetable.h"

using namespace core;

// the display's view state Fill asks for (the real ones are LVGL's, sidecar_wgview.c): bank A, no FILES, no SELECT UNUSED
extern "C" {
int sidecar_wgview_bank(void) { return 0; }
int sidecar_wgview_selecting(void) { return 0; }
int sidecar_wgview_lib_shown(void) { return 0; }
int sidecar_wgview_pool_page_shown(void) { return 0; }
int sidecar_wgview_page_list(void) { return 1; }
int sidecar_wgview_list_id(void) { return 1; }
}

// voice 0 on WAVETABLE (a 0x03 report): TBL a, TBLB b, WARP w, MIX m
static void report(int a, int b, int w, int m) {
  std::vector<uint8_t> r = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(kWaveMachine | 0)};
  int p[16] = {a, 0, w, 0, 0, 0, 64, 0, 0, 127, 127, 64, b, 0, m, 0};
  for (int i = 0; i < 16; ++i) r.push_back((uint8_t)p[i]), r.push_back(0);
  r.push_back(0xf7);
  engine_sysex(r.data(), (int)r.size());
}

int main() {
  char tmpl[] = "/tmp/wgview_pi_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples' '" + tmpl + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  CHECK(engine_begin());
  CHECK(wt::entries() > 3);
  sidecar_wg_view_t o;
  memset(&o, 0, sizeof o);
  const int zfm = 3 * 128 / 9 + 7;   // the middle of WARP's FM zone (9 zones over 0..127)
  report(1, 3, 0, 64);   // A BASIC (A001), B A003, MIX 64
  wgview_pi::Fill(&o, 0);
  CHECK(o.kind == WGV_WAVETABLE && o.voice == 0 && o.sel == 1);
  CHECK(o.sel_b == 3 && !strcmp(o.name_b, wt::entry_name(3)) && !o.mix0);
  unsigned g0 = o.gen;
  report(1, 3, 0, 0);    // MIX 0, the warp OFF: B is picked and silent
  wgview_pi::Fill(&o, 0);
  CHECK(o.sel == 1 && o.sel_b == 3 && o.mix0 && o.gen != g0);
  unsigned g1 = o.gen;
  report(1, 3, zfm, 0);  // FM uses B as its modulator: audible at MIX 0
  wgview_pi::Fill(&o, 0);
  CHECK(!o.mix0 && o.gen != g1);
  report(1, 4, zfm, 0);  // another B: its code and name, the hash
  unsigned g2 = o.gen;
  wgview_pi::Fill(&o, 0);
  CHECK(o.sel_b == 4 && !strcmp(o.name_b, wt::entry_name(4)));
  report(1, 0, 0, 64);   // B OFF (row 000): no name, MIX 64 so not "silent for MIX"
  wgview_pi::Fill(&o, 0);
  CHECK(o.sel == 1 && o.sel_b == 0 && !strcmp(o.name_b, "OFF") && !o.mix0 && o.gen != g2);
  report(2, 0, 0, 64);   // A changes, B stays: sel_b is B's own
  wgview_pi::Fill(&o, 0);
  CHECK(o.sel == 2 && o.sel_b == 0);
  system((std::string("rm -rf '") + tmpl + "'").c_str());
  printf("test_wgview_pi: PASS (sel_b, name_b, mix0 and the hash)\n");
  return 0;
}
