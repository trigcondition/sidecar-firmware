// pi/engine/tests/test_slotfx_ref.cpp - two links, phase 3(d): slot 0's send FX and master chain bit for bit. A fixed
// scene (CHORUS on FX 1, COMB on FX 2, the Rytm's sends, the voices' sends to the buses, a TYPE on bus 3, the master's COMP
// and a send TYPE insert, SRC MIX with the stems) through sendfx_pi and masterfx_pi with box 0's calls only, every OUT frame
// hashed (FNV-1a 64). The hash printed and compared with the one the code before the per-slot change gave
// (kRef, recorded at 9354abb). Then, on 16 voices, the same with a TYPE set in slot 1's settings and slot 1's instances run:
// slot 0's hash unchanged.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "check.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "masterfx_pi.h"
#include "sendfx_pi.h"

namespace core {
uint32_t cycles() { static uint32_t c; return c += 1000; }
uint32_t cycles_per_second() { return 54000000; }
uint32_t g_ms;
uint32_t now_ms() { return g_ms; }
void log(const char*, ...) {}
}  // namespace core
extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap) { return vsnprintf(out, n, fmt, ap); }
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r;
}

using namespace core;

static uint64_t g_h = 1469598103934665603ull;
static void hash(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}

static void scene() {
  fx::control.knob[0][0] = fx::kChorus, fx::control.knob[1][0] = fx::kComb;
  for (int k = 2; k < fx::kKnobs; ++k) fx::control.knob[0][k] = 70, fx::control.knob[1][k] = 50;
  fx::control.knob[1][1] = 100;   // FX 2 POST
  for (int t = 0; t < fx::kTracks; ++t) fx::control.send[t][0] = (uint8_t)(20 + 7 * t), fx::control.send[t][1] = (uint8_t)(90 - 5 * t);
  for (int v = 0; v < ins::kVoices; ++v) ins::control.send[v][0] = 40, ins::control.send[v][2] = 60;
  ins::set_type(ins::kBus0 + 2, 0, ins::kSat);
  ins::set_type(ins::kM0, 0, ins::kComp);
  ins::set_type(ins::kM0, 1, ins::kMChorus);
  master::control.src = master::kMix;
}

#if CORE_VOICES > 8 && !defined(SLOTFX_BASE)   // (SLOTFX_BASE: built on the code before the change, for its hash)
// slot 1 (an A4-like row: no PRE pair, tracks on 2..5): CHORUS on its FX 1, its voices sending to bus 1, a COMP on its master
static const core::out::Device kBox = {"box", 6, {0, 1}, {0, 1}};
static bool g_box1 = false;
static double g_peak1 = 0, g_track1 = 0;   // its return's peak on 0/1; frames that moved its tracks 2..5
static void scene1() {
  fx::control1.knob[0][0] = fx::kChorus;
  for (int k = 2; k < fx::kKnobs; ++k) fx::control1.knob[0][k] = 70;
  for (int v = 0; v < ins::kVoices; ++v) ins::control1.send[v][0] = 100;
  ins::control1.type[ins::kM0][0] = ins::kComp;
  for (int k = 0; k < ins::kParams; ++k) ins::control1.play[ins::kM0][0][k] = ins::labels(ins::kComp).def[k];
}
static void box1_block(int k) {
  int16_t v[7][8] = {};
  for (int f = 0; f < 7; ++f)
    for (int c = 0; c < 8; ++c) v[f][c] = (int16_t)(4000.0 * std::sin(6.2831853 * (220.0 + 5.0 * c) * (7.0 * k + f) / 48000.0));
  int32_t o[7][12] = {};
  for (int f = 0; f < 7; ++f)
    for (int c = 2; c < 6; ++c) o[f][c] = 1234 * c;
  sendfx_pi::FromVoices(1, v, 7);
  sendfx_pi::AddReturn(1, kBox, o, 7);
  masterfx_pi::Process(1, kBox, o, 7);
  for (int f = 0; f < 7; ++f) {
    g_peak1 = std::fmax(g_peak1, std::fabs((double)o[f][0]));
    for (int c = 2; c < 6; ++c) g_track1 += o[f][c] != 1234 * c;
  }
  sendfx_pi::Render(1);
  masterfx_pi::Render(1);
}
#endif

// n blocks of 7 frames: stems (two sines), voices (a third), the OUT fill's channels; render after each block
static void run(int blocks) {
  for (int k = 0; k < blocks; ++k) {
    int32_t s[7][8] = {};
    int16_t v[7][8] = {};
    for (int f = 0; f < 7; ++f) {
      const double t = (7.0 * k + f) / 48000.0;
      for (int c = 0; c < 8; ++c) s[f][c] = (int32_t)(0.05 * 2147483647.0 * std::sin(6.2831853 * (110.0 + 37.0 * c) * t));
      for (int c = 0; c < 8; ++c) v[f][c] = (int16_t)(3000.0 * std::sin(6.2831853 * (330.0 + 11.0 * c) * t));
    }
    if (sendfx_pi::On()) sendfx_pi::FromRytm(s, 7);
    if (masterfx_pi::WantStems()) masterfx_pi::Stems(s, 7);
    int32_t o[7][12];
    for (int f = 0; f < 7; ++f)
      for (int c = 0; c < 12; ++c) o[f][c] = (int32_t)(1000 * (c + 1) * (f + 1));
    float x[7][4] = {};
    sendfx_pi::FromVoices(v, 7);
    sendfx_pi::AddReturn(o, 7, x);
    masterfx_pi::Process(o, 7);
    hash(o, sizeof o);
    hash(x, sizeof x);
    sendfx_pi::Render();
    masterfx_pi::Render();
#if CORE_VOICES > 8 && !defined(SLOTFX_BASE)   // (SLOTFX_BASE: built on the code before the change, for its hash)
    if (g_box1) box1_block(k);   // slot 1's instances in between: slot 0's frames must not change
#endif
    core::g_ms += (k % 7 == 0);
  }
}

int main(int argc, char** argv) {
  if (!getenv("SLOTFX_NOSCENE")) scene();   // (the scene off: a different hash, so the FX did play)
#if CORE_VOICES > 8 && !defined(SLOTFX_BASE)   // (SLOTFX_BASE: built on the code before the change, for its hash)
  // SLOTFX_BOX1: slot 1 plays beside it (its own return non-zero on 0/1, its tracks untouched, a user TYPE in its settings
  // MISSING); slot 0's hash must still be the reference
  if (getenv("SLOTFX_BOX1")) {
    g_box1 = true;
    scene1();
    CHECK(fx::state(fx::control1, 0) == fx::kStReady && sendfx_pi::On(1) && core::master::on(1));
  }
#endif
  run(4000);
#if CORE_VOICES > 8 && !defined(SLOTFX_BASE)   // (SLOTFX_BASE: built on the code before the change, for its hash)
  if (g_box1) {
    printf("slot 1 return peak %.0f, track frames moved %.0f\n", g_peak1, g_track1);
    CHECK(g_peak1 > 1e6 && g_track1 == 0);
    fx::control1.knob[1][0] = fx::kFirstUser;
    CHECK(fx::state(fx::control1, 1) == fx::kStMissing);
  }
#endif
  printf("slot 0 hash %016llx\n", (unsigned long long)g_h);
  const char* ref = argc > 1 ? argv[1] : getenv("SLOTFX_REF");
  if (ref) {
    char b[32];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)g_h);
    CHECK(strcmp(b, ref) == 0);
    printf("matches the reference\n");
  }
  return 0;
}
