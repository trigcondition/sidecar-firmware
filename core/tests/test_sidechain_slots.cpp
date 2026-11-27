// core/tests/test_sidechain_slots.cpp - SIDECHAIN with two boxes (core/fx/sidechain.h, insert.h RunSc): each voice slot's
// chains key from their own box's bus (sc::bus(slot)), so a trig or a key ring of one box never moves the other's stage,
// and each slot's chain plays its own settings (ins::control_of(slot)).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/sidechain.h"

using namespace core;
using ins::Engine;

static const float kSr = 48000.0f;
static const int kB = 16;
static std::vector<float> g_mem[2];

static Engine fresh(int box) {
  Engine e(kSr, box);
  g_mem[box].assign((size_t)ins::kLines * ins::kLine, 0.0f);
  e.Place((float(*)[ins::kLine])g_mem[box].data());
  e.Reset();
  return e;
}
static void clear_buses() {
  for (int s = 0; s < sc::kBuses; ++s) {
    memset((void*)&sc::bus(s), 0, sizeof(sc::Bus));
    sc::bus(s).wanted_trig = ~0u;
  }
}
// PUMP on chain 0 keyed by voice V3's trig (instant attack, a long hold: the gain is 0 for the rest of the run once fired)
static void pump_v3(ins::Control& k) {
  memset((void*)&k, 0, sizeof k);
  k.scmode[0] = sc::kPump, k.scsrc[0] = (uint8_t)(sc::kV0 + 2);
  const uint8_t p[sc::kParams] = {127, 0, 127, 20, 64, 50};
  for (int j = 0; j < sc::kParams; ++j) k.scp[0][j] = p[j];
}
// 1024 frames of 0.5 through chain 0 of `e`, a trig of V3 on bus `trig_bus` at frame 480; the voice ring of both buses kept
// alive; the output's last frame
static float run(Engine& e, const ins::Control& k, int trig_bus) {
  std::vector<float> y(1024, 0.5f);
  float z[kB] = {};
  for (int i = 0; i < 1024; i += kB) {
    for (int s = 0; s < sc::kBuses; ++s) sc::write(s, sc::kStems + 2 + 2, z, kB);
    if (i == 480) sc::trig_voice(trig_bus, 2);
    e.ProcessMono(0, &y[i], kB, k);
  }
  return y[1023];
}

int main() {
  static ins::Control k0, k1;
  pump_v3(k0), pump_v3(k1);
  {   // slot 1's chain fires on its own box's V3, not on slot 0's
    clear_buses();
    Engine e1 = fresh(1);
    CHECK(run(e1, k1, 0) == 0.5f);
    clear_buses();
    e1 = fresh(1);
    CHECK(std::fabs(run(e1, k1, 1)) < 1e-6f);
    printf("sidechain: slot 1's PUMP fires on its own box's V3 trig only\n");
  }
  {   // and slot 0's on slot 0's
    clear_buses();
    Engine e0 = fresh(0);
    CHECK(run(e0, k0, 1) == 0.5f);
    clear_buses();
    e0 = fresh(0);
    CHECK(std::fabs(run(e0, k0, 0)) < 1e-6f);
    printf("sidechain: slot 0's PUMP fires on its own box's V3 trig only\n");
  }
  {   // a key ring written on one bus is missing on the other: DUCK keyed from it leaves the other slot's chain untouched
    clear_buses();
    static ins::Control d;
    memset((void*)&d, 0, sizeof d);
    d.scmode[0] = sc::kDuck, d.scsrc[0] = (uint8_t)(sc::kV0 + 2);
    const uint8_t p[sc::kParams] = {0, 127, 0, 20, 0, 0};   // THR 0, RAT max: any key ducks
    for (int j = 0; j < sc::kParams; ++j) d.scp[0][j] = p[j];
    Engine e1 = fresh(1);
    std::vector<float> y(2048, 0.5f);
    float loud[kB];
    for (int i = 0; i < kB; ++i) loud[i] = 0.9f;
    for (int i = 0; i < 2048; i += kB) {
      sc::write(0, sc::kStems + 2 + 2, loud, kB);   // slot 0's box only
      e1.ProcessMono(0, &y[i], kB, d);
    }
    CHECK(y[2047] == 0.5f);
    printf("sidechain: a key on slot 0's bus is missing for slot 1's DUCK (untouched)\n");
  }
  printf("ALL PASS\n");
  return 0;
}
