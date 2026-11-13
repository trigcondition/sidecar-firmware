// core/tests/small/test_lfo_small.cpp - the Sidecar Mini keeps its 4 LFOs (LFO FINAL, the owner, 2026-11-12): no tracks,
// no A / B, numbered names on the console, the JSON and the file's lines, every target but the Rytm's (0.5.0 owner test).
#include <string.h>

#include "check.h"
#include "core.h"
#include "lfo.h"

static_assert(!RYTM_SENDFX_LARGE, "build with -DRYTM_SENDFX_LARGE=0");

using namespace core;

int main() {
  static_assert(lfo::kLfos == 4 && !lfo::kPerTrack, "the Mini: 4 LFOs, no tracks");
  lfo::reset();
  char b[8], o[2048];
  CHECK(!strcmp(lfo::lfo_name(1, b, sizeof b), "2") && lfo::lfo_index("4") == 3 && lfo::lfo_index("5") < 0);
  CHECK(lfo::lfo_index("T1A") < 0 && lfo::lfo_index("FXB") < 0);
  CHECK(lfo::lfo_of(3, 0) == 3 && lfo::track_of(1) == -1 && lfo::slot_of(1) == 0);
#if CORE_POLY   // 0.5.0 owner test: no LFO on the Rytm, the Mini's either - the list ends before RT1
  for (int i = 0; i < lfo::kLfos; ++i) CHECK(lfo::target_max(i) == lfo::kTRt - 1 && !lfo::target_ok(i, lfo::kTRt));
  CHECK(!lfo::command("lfo=1:tgt:rt1", o, sizeof o) || strstr(o, "cannot"));
#else
  for (int i = 0; i < lfo::kLfos; ++i) CHECK(lfo::target_max(i) == lfo::kTargets - 1);
#endif
  CHECK(lfo::command("lfo=4:tgt:fx1", o, sizeof o) && !strstr(o, "cannot") && strstr(o, "lfo: L4 FX1"));
  CHECK(lfo::command("lfo=5:dep:1", o, sizeof o) && strstr(o, "lfo=N") && !strstr(o, "T1A"));
  CHECK(lfo::command("lfo", o, sizeof o) && strstr(o, "L1 OFF") && strstr(o, "L4 FX1"));
  CHECK(lfo::json(o, sizeof o) > 0 && strstr(o, "\"n\":4,\"pt\":0"));
  puts("test_lfo_small: the Mini keeps 4 LFOs, numbered, no A / B");
  return 0;
}
