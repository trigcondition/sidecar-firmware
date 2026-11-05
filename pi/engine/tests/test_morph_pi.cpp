// pi/engine/tests/test_morph_pi.cpp - the display's MORPH page view (morph_pi.cpp) from the scenes (core/mod/scene.h) on
// the core's Mac library: FillView's hash changes with a lock (and comes back with the same state), the fader, A / B,
// the selection; the rows from the list's scroll with their names, values and slider ranges; core 3's publish and
// core 0's take. make -C pi/engine/tests (make -C core engines lib first).
#include <cstdio>
#include <cstring>

#include "../morph_pi.h"
#include "check.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "platform.h"

using namespace core;

static uint64_t g_us = 50000000;
static mod::Tid T(const char* n) {
  mod::Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}

int main() {
  host_fake_time(true, g_us);
  scene::reset();
  sidecar_morph_view_t a, b;
  // fill_view_hash_changes_on_lock
  morph_pi::FillView(&a, 0);
  CHECK(a.valid && a.nrows == 0 && a.a == -1 && a.b == -1 && a.sel == 0);
  CHECK(scene::lock(0, T("V1:P3"), 6400));
  morph_pi::FillView(&b, 0);
  CHECK(b.gen != a.gen && b.nlocks[0] == 1 && b.nrows == 1 && !strcmp(b.row[0].name, "V1:P3") && !strcmp(b.row[0].value, "50"));
  CHECK(b.row[0].lo == 0 && b.row[0].hi == 127 && b.row[0].v == 50);
  CHECK(scene::lock(0, T("V1:P3"), 6464));   // 50.5: the value text and the hash follow
  morph_pi::FillView(&a, 0);
  CHECK(a.gen != b.gen && !strcmp(a.row[0].value, "50.5"));
  scene::unlock(0, T("V1:P3"));
  morph_pi::FillView(&a, 0);
  sidecar_morph_view_t e;
  scene::reset();
  morph_pi::FillView(&e, 0);
  CHECK(a.gen == e.gen);                     // the same state: the same hash (no redraw)
  scene::set_fader(8192, scene::kConsole);
  morph_pi::FillView(&b, 0);
  CHECK(b.gen != e.gen && b.fader == 8192);
  scene::set_ab(2, 5);
  morph_pi::FillView(&a, 0);
  CHECK(a.gen != b.gen && a.a == 2 && a.b == 5);
  // the rows from the scroll; a stepped Rytm target's range
  CHECK(scene::lock(3, T("RYTM:SCN"), 5 * 128));
  for (int k = 0; k < 20; ++k) CHECK(scene::lock(3, mod::tid(mod::kV, k % 8, k / 8), (uint16_t)(k * 128)));
  scene::set_sel(3);
  morph_pi::FillView(&a, 0);
  CHECK(a.sel == 3 && a.nlocks[3] == 21 && a.nrows == SMO_ROWS && !strcmp(a.row[0].name, "RYTM:SCN") && !strcmp(a.row[0].value, "5"));
  CHECK(a.row[0].lo == 0 && a.row[0].hi == 12 && a.row[0].v == 5 * 127 / 12);
  morph_pi::FillView(&b, 10);
  CHECK(b.top == 10 && b.nrows == 11 && !strcmp(b.row[0].name, b.row[0].name) && b.gen != a.gen);
  char n[12];
  mod::name(scene::state().s[3].l[10].t, n, sizeof n);
  CHECK(!strcmp(b.row[0].name, n));
  scene::set_edit(true);
  morph_pi::FillView(&a, 10);
  CHECK(a.edit && a.gen != b.gen);
  scene::set_edit(false);
  // core 3 publishes while wanted (every 50 ms at most), core 0 takes the newest once
  unsigned seen = 0;
  morph_pi::Tick();
  CHECK(!morph_pi::View(&a, &seen));         // not wanted: nothing built
  morph_pi::Want(true, 0);
  g_us += 60000, host_fake_time(true, g_us);
  morph_pi::Tick();
  CHECK(morph_pi::View(&a, &seen) && a.sel == 3 && a.top == 0);
  CHECK(!morph_pi::View(&a, &seen));
  morph_pi::Want(true, 5);
  morph_pi::Tick();                          // within 50 ms: not again
  CHECK(!morph_pi::View(&a, &seen));
  g_us += 60000, host_fake_time(true, g_us);
  morph_pi::Tick();
  CHECK(morph_pi::View(&a, &seen) && a.top == 5);
  morph_pi::Want(false, 0);
  scene::reset();
  printf("test_morph_pi: ok (the hash follows a lock, the fader, A / B, EDIT; the rows from the scroll; publish / take)\n");
  return 0;
}
