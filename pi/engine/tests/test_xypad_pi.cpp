// pi/engine/tests/test_xypad_pi.cpp - the display's XY page view (xypad_pi.cpp) from the XY pad (core/mod/xypad.h) on the
// core's Mac library: FillView's names, depths, HOLD and position, its hash following each and coming back with the
// same state; core 3's publish and core 0's take. make -C pi/engine/tests (make -C core engines lib first).
#include <cstdio>
#include <cstring>

#include "../xypad_pi.h"
#include "check.h"
#include "mod/target.h"
#include "mod/xypad.h"
#include "platform.h"
#include "ui/remote.h"

namespace core { void voices_init(); }   // (core/voices.h needs the engines' headers)

using namespace core;

static uint64_t g_us = 50000000;
static mod::Tid T(const char* n) {
  mod::Tid t = 0;
  CHECK(xypad::parse_target(n, &t));
  return t;
}

int main() {
  host_fake_time(true, g_us);
  xypad::reset();
  voices_init();   // (the voices as at boot: PLAITS, no user slot - the names below are PLAITS')
  sidecar_xy_view_t a, b, e;
  {   // 0.5.0: track 1 BD HARD (the Rytm's machine 0, not a Sidecar machine) from the Rytm's 33
    const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 0, 0x7F, 0xF7};
    ui::remote_sysex(m33, sizeof m33);
    CHECK(ui::remote_track_rytm_machine(0) == 0);
    const uint8_t raw[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 2, 1, 32, 0x7F, 0xF7};   // track 3 SY RAW: 4-letter labels
    ui::remote_sysex(raw, sizeof raw);
    CHECK(ui::remote_track_rytm_machine(2) == 32);
  }
  xypad_pi::FillView(&e, true);
  CHECK(e.valid && !e.hold && !e.active && !e.s[0][0].name[0] && !e.s[1][3].name[0]);
  CHECK(xypad::set(0, 0, T("RYTM:PERF12"), -100) && xypad::set(1, 3, T("MORPH"), 50));
  xypad_pi::FillView(&a, true);   // (the picker open: its tables filled)
  // the RT picker's P1..P8 by the track's Rytm machine (BD HARD: P1 LEV, P2 TUN); another track's not known yet: "P2";
  // a slot's label by name ("RT3:FRQ", "RT1:TUN")
  CHECK(!strcmp(a.rl[0][0], "LEV") && !strcmp(a.rl[0][1], "TUN") && !strcmp(a.rl[1][1], "P2"));
  CHECK(!strcmp(a.rl[2][1], "TUNE") && !strcmp(a.rl[2][2], "DEC2"));   // (the 53-machine table, through mod::param_label)
  {
    sidecar_xy_view_t r;
    CHECK(xypad::set(0, 2, T("RT3:FRQ"), 30) && xypad::set(1, 2, T("RT1:P2"), -30));
    xypad_pi::FillView(&r);
    CHECK(!strcmp(r.s[0][2].name, "RT3:FRQ") && !strcmp(r.s[0][2].label, "RT3:FRQ"));
    CHECK(!strcmp(r.s[1][2].name, "RT1:P2") && !strcmp(r.s[1][2].label, "RT1:TUN"));
    xypad::clear(0, 2), xypad::clear(1, 2);
  }
  CHECK(a.gen != e.gen && !strcmp(a.s[0][0].name, "RYTM:PERF12") && a.s[0][0].depth == -100);
  CHECK(!strcmp(a.s[1][3].name, "MORPH") && a.s[1][3].depth == 50);
  // 2026-11: the screens' names - a slot's label, the picker's tables (voice 1 PLAITS: P2 HRM, P9 FM; INSERT I1's slot
  // A OFF: "A-", its POST EQ "QHPF")
  CHECK(!strcmp(a.s[0][0].label, "RYTM:PERF12") && !strcmp(a.s[1][3].label, "MORPH"));
  CHECK(!strcmp(a.vl[0][1], "HRM") && !strcmp(a.vl[0][8], "FM") && !strcmp(a.il[0][0], "A-") && !strcmp(a.il[0][8], "QHPF"));
  {   // the picker closed: the tables are not built again (about 290 names), the last ones kept; open: built again
    sidecar_xy_view_t c;
    xypad_pi::FillView(&c);
    CHECK(c.gen == a.gen && !strcmp(c.vl[0][1], "HRM") && !memcmp(c.il, a.il, sizeof c.il));
    xypad_pi::FillView(&c, true);
    CHECK(c.gen == a.gen && !strcmp(c.vl[0][1], "HRM"));
  }
  CHECK(xypad::set(0, 1, T("V1:P2"), 25));
  xypad_pi::FillView(&b);
  CHECK(!strcmp(b.s[0][1].name, "V1:P2") && !strcmp(b.s[0][1].label, "V1:HRM"));   // (the id stays: xy=set takes it)
  xypad::clear(0, 1);
  xypad::set_hold(true);
  xypad_pi::FillView(&b);
  CHECK(b.gen != a.gen && b.hold);
  xypad::pos(100, 16383);
  xypad_pi::FillView(&a);
  CHECK(a.gen != b.gen && a.touching && a.active && a.x == 100 && a.y == 16383);
  xypad::up();
  xypad_pi::FillView(&b);
  CHECK(b.gen != a.gen && !b.touching && b.active);   // HOLD on: held
  xypad::set_hold(false);
  xypad::clear(0, 0), xypad::clear(1, 3);
  xypad::reset();
  xypad_pi::FillView(&a);
  CHECK(a.gen == e.gen);   // the same state: the same hash (no redraw)
  // core 3 publishes while wanted (every 50 ms at most), core 0 takes the newest once
  unsigned seen = 0;
  xypad_pi::Tick();
  CHECK(!xypad_pi::View(&a, &seen));   // not wanted: nothing built
  xypad_pi::Want(true);
  g_us += 60000, host_fake_time(true, g_us);
  xypad_pi::Tick();
  CHECK(xypad_pi::View(&a, &seen) && a.valid);
  CHECK(!xypad_pi::View(&a, &seen));
  xypad::set_hold(true);
  xypad_pi::Tick();                    // within 50 ms: not again
  CHECK(!xypad_pi::View(&a, &seen));
  g_us += 60000, host_fake_time(true, g_us);
  xypad_pi::Tick();
  CHECK(xypad_pi::View(&a, &seen) && a.hold);
  // the picker's tables: built while it is open (the first Tick at once), then at most every 250 ms; not while closed
  {
    unsigned nv = xypad_pi::NamesBuilt();
    for (int ms = 0; ms < 1000; ms += 60) {   // the page shown, the picker closed
      g_us += 60000, host_fake_time(true, g_us);
      xypad_pi::Tick();
    }
    CHECK(xypad_pi::NamesBuilt() == nv);
    xypad_pi::Want(true, true);
    g_us += 60000, host_fake_time(true, g_us);
    xypad_pi::Tick();
    CHECK(xypad_pi::NamesBuilt() == nv + 1);
    for (int ms = 0; ms < 1000; ms += 60) {
      g_us += 60000, host_fake_time(true, g_us);
      xypad_pi::Tick();
    }
    CHECK(xypad_pi::NamesBuilt() >= nv + 4 && xypad_pi::NamesBuilt() <= nv + 6);   // (1 s: 4 or 5 more, not 17)
    xypad_pi::Want(true, false);
    nv = xypad_pi::NamesBuilt();
    for (int ms = 0; ms < 1000; ms += 60) {
      g_us += 60000, host_fake_time(true, g_us);
      xypad_pi::Tick();
    }
    CHECK(xypad_pi::NamesBuilt() == nv);
  }
  xypad_pi::Want(false);
  xypad::reset();
  // a drag's lines (the pad's, the morph slider's) are not logged, and do not hold the status push back
  CHECK(xypad_pi::Quiet("xy=pos:100:200") && xypad_pi::Quiet("xy=up") && xypad_pi::Quiet("scene=fader:8000"));
  CHECK(!xypad_pi::Quiet("xy=hold:1") && !xypad_pi::Quiet("xy=set:x:1:MORPH:10") && !xypad_pi::Quiet("xy=upx") &&
        !xypad_pi::Quiet("xy") && !xypad_pi::Quiet("scene=ab:1") && !xypad_pi::Quiet("sendlvl=1:1:2"));
  printf("test_xypad_pi: ok (names, depths, HOLD, the position; the hash; publish / take; the quiet lines)\n");
  return 0;
}
