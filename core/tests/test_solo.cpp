// core/tests/test_solo.cpp - fix1049 (hardware 2026-10-23: soloing another Rytm track left the Sidecar voices playing,
// while MUTE silenced them): the Rytm's 2D 0D SOLO (rytm-mods 0026 1049, the soloed tracks as 05 MUTES' mask) silences
// every track not soloed, as MUTE does: the seq's note gate (seq::silenced) and the PRE / POST voices' gates
// (out::control.open, the device table's voice order BD BT SD LT CH/OH MT/HT CY/CB RS/CP); none soloed: the mutes alone.
// An older Rytm never sends 0D (nothing changes); an older host ignores it (seq.cpp's default).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "fx/out.h"
#include "seq/seq.h"

using namespace core;

static void state(uint8_t kind, uint16_t m) {
  std::vector<uint8_t> x = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 0x01, kind, (uint8_t)(m & 0x7F), (uint8_t)(m >> 7 & 0x3F), 0xF7};
  engine_sysex(x.data(), (int)x.size());
}
static int open_mask() {
  int o = 0;
  for (int v = 0; v < 8; ++v) o |= (out::control.open[v] ? 1 : 0) << v;
  return o;
}

int main() {
  char tmpl[] = "/tmp/core_solo_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = [](const uint8_t*, int) {};
  host_fake_time(true, 1000000);
  CHECK(engine_begin());
  CHECK(seq::silenced() == 0 && open_mask() == 0xFF);
  state(seq::kMutes, 1u << 1);   // MUTE + SD: voice 2 (SD) closed
  CHECK(seq::silenced() == 0x002 && open_mask() == 0xFF - (1 << 2));
  state(seq::kSolo, 1u << 0);    // SOLO BD: everything else silent; SD stays muted
  printf("  SOLO BD: silenced 0x%03x, voices open 0x%02x\n", seq::silenced(), open_mask());
  CHECK(seq::silenced() == (0x1FFF & ~1u) && open_mask() == 0x01);
  CHECK(seq::stats().solos == 1);
  state(seq::kSolo, 1u << 0 | 1u << 2);   // + RS: RS/CP's shared voice (7) is closed only when both tracks are silent
  CHECK(seq::silenced() == (0x1FFF & ~5u) && open_mask() == (0x01 | 0x80));
  state(seq::kSolo, 0);          // none soloed: the mutes alone (SD)
  CHECK(seq::silenced() == 0x002 && open_mask() == 0xFF - (1 << 2));
  state(seq::kSolo, 1u << 1);    // SD soloed and muted: the mute wins (nothing heard)
  CHECK(seq::silenced() == 0x1FFF);
  state(seq::kMutes, 0);
  CHECK(seq::silenced() == (0x1FFF & ~2u) && open_mask() == (1 << 2));
  state(seq::kSolo, 0);
  CHECK(seq::silenced() == 0 && open_mask() == 0xFF);
  CHECK(system((std::string("rm -rf '") + tmpl + "'").c_str()) == 0);
  printf("test_solo: OK\n");
  return 0;
}
