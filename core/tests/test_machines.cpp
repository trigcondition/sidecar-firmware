// core/tests/test_machines.cpp - machines, user slots, ENG zones and parameter bytes as sim/protocol.py defines them.
#include <initializer_list>
#include <stdio.h>
#include "check.h"
#include "machines.h"

int main() {
  using namespace core;
  CHECK(kVoices == 8 && kBlock == 12);
  CHECK(machine_of(0x00 | 3) == kPlaits && machine_of(0x08 | 7) == kJuno && machine_of(0x10) == kDaisy);
  CHECK(machine_of(0x20 | 5) == kDspSample && machine_of(0x40 | 1) == kMd && machine_of(0x48 | 2) == kFm);
  CHECK(user_slot_of(0x50) == 0 && user_slot_of(0x58) == 1 && user_slot_of(0x70) == 4);
  CHECK(user_slot_of(0x48) == -1 && user_slot_of(0x78) == -1 && user_slot_of(0x00) == -1);
  // zone = min((v * n) >> 7, n - 1) on the integer part, clamped to 0..127 (protocol.eng_zone)
  for (int n : {3, 11, 17})
    for (int v = 0; v < 128; ++v) {
      int want = (v * n) >> 7;
      if (want > n - 1) want = n - 1;
      CHECK(eng_zone(v + 0.99f, n) == want);
    }
  CHECK(eng_zone(-5.0f, 17) == 0 && eng_zone(300.0f, 17) == 16);
  const uint8_t want_models[17] = {0, 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20};   // PLAITS_MODELS
  for (int i = 0; i < kPlaitsEngs; ++i) CHECK(kPlaitsModels[i] == want_models[i]);
  const uint8_t raw[4] = {100, 64, 0, 127};   // 100 + 64/128, 0 + 127/128
  float p[2];
  decode_params(raw, 2, p);
  CHECK(p[0] == 100.5f && p[1] == 127.0f / 128.0f);
  printf("test_machines: PASS\n");
  return 0;
}
