// core/tests/test_mod.cpp - the modulation layer (core/mod, spec-050 section 1): target ids, names and ranges.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "mod/target.h"

using namespace core;
using namespace core::mod;

static void ids_and_names() {
  CHECK(kind_of(tid(kV, 2, 4)) == kV && index_of(tid(kV, 2, 4)) == 2 && param_of(tid(kV, 2, 4)) == 4);
  char b[16]; Tid t;
  const char* names[] = {"V3:P5", "FX1:P2", "S5:F1", "I3:A2", "B1:E1", "M1:B3", "X2:B1", "MFX:LEV", "MFX:MIX2",
                         "O4:PAN", "T1:SPD", "R1:GATE", "LOOP:BAL", "RYTM:SCN", "RYTM:PERF3"};
  for (const char* n : names) { CHECK(parse(n, &t)); CHECK(!strcmp(name(t, b, sizeof b), n)); }
  CHECK(parse("v3:p5", &t) && t == tid(kV, 2, 4));
  CHECK(!parse("V9:P1", &t) && !parse("FX1:P6", &t) && !parse("Q1:X", &t));
  CHECK(range(tid(kV, 0, 0)).hi > 127.98f && range(tid(kV, 0, 0)).mode == kCont);   // list knobs too: mod never rounds kV
  CHECK(range(tid(kSeq, 0, 1)).mode == kStep && range(tid(kSeq, 0, 2)).mode == kCont);
  // the indices behind the names: FX1:P2 is slot 0 param 0, B1:E1 bus 1's chain (8) param 8, M1:B3 chain 11 param 6
  CHECK(parse("FX1:P2", &t) && t == tid(kFx, 0, 0) && parse("B1:E1", &t) && t == tid(kIns, 8, 8));
  CHECK(parse("M1:B3", &t) && t == tid(kIns, 11, 6) && parse("RYTM:PERF3", &t) && t == tid(kRytm, 0, 3));
  CHECK(!parse("MFX1:LEV", &t) && !parse("V3", &t) && !parse("I9:A1", &t) && !parse("M3:A1", &t) && !parse("V3:P5x", &t));
  CHECK(!valid(0) && !valid(tid(kV, 8, 0)) && !valid(tid(kKinds, 0, 0)) && !name(tid(kV, 8, 0), b, sizeof b)[0]);
  printf("mod: ids and names\n");
}

int main() {
  ids_and_names();
  printf("test_mod: PASS\n");
  return 0;
}
