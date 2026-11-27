// pi/engine/tests/test_rytm_slot1.cpp - the Rytm in either slot (attach order, design 2026-11-21 item 4): its stems pace its
// own slot's send FX and feed its slot's SRC MIX, and its report 50 lands in its slot's master control. A fixed scene (CHORUS
// on FX 1, COMB on FX 2 POST, its track sends, the voices' sends to the buses, a TYPE on bus 3, the master's COMP and a send
// TYPE insert, SRC MIX with the stems and its report) is played once as slot 0 and once as slot 1 through sendfx_pi and
// masterfx_pi as ob_usb.cpp calls them (its stream's device row, core::out::kRytmMk2); every OUT frame hashed (FNV-1a 64):
// the two hashes are equal. Then a box that is not the Rytm (an A4-like row) in slot 0: its voices pace its send FX (frames
// rendered, a return on its pair) and SRC MIX mixes no stems on its stream. 16 voices (two slots).
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

static_assert(CORE_VOICES > 8, "two slots: build with -DCORE_VOICES=16");

static int g_hand = 0, g_rytm = 0;   // the slot in hand (ins / out / fx settings), the Rytm's slot (master::rytm)
static int hand() { return g_hand; }
static int rytm() { return g_rytm; }

static uint64_t g_h;
static void hash(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}

// slot b's settings, as the slot in hand (the same scene in either slot)
static void scene(int b) {
  g_hand = b;
  fx::Control& c = fx::control_of(b);
  c.knob[0][0] = fx::kChorus, c.knob[1][0] = fx::kComb;
  for (int k = 2; k < fx::kKnobs; ++k) c.knob[0][k] = 70, c.knob[1][k] = 50;
  c.knob[1][1] = 100;   // FX 2 POST
  for (int t = 0; t < fx::kTracks; ++t) c.send[t][0] = (uint8_t)(20 + 7 * t), c.send[t][1] = (uint8_t)(90 - 5 * t);
  for (int v = 0; v < ins::kVoices; ++v) ins::control_of(b).send[v][0] = 40, ins::control_of(b).send[v][2] = 60;
  ins::set_type(ins::kBus0 + 2, 0, ins::kSat);
  ins::set_type(ins::kM0, 0, ins::kComp);
  ins::set_type(ins::kM0, 1, ins::kMChorus);
  master::control_of(b).src = master::kMix;
  g_hand = 0;
}

static void report() {   // the Rytm's 50: BD pan 50 off main, CY/CB pan 100 off main, the rest centred on main
  const uint8_t m[16] = {0xF0, 0x7D, 0x52, 0x54, 0x50, 50, 64, 64, 64, 64, 64, 64, 100, 0, (1 << 3) | (1 << 6), 0xF7};
  CHECK(master::report(m, 16));
}

// n blocks of 7 frames for box b on stream dev, as ob_usb.cpp: the IN completion (its stems, when dev is the Rytm's), the OUT fill
// (voices, the send FX return, the master chain), core 2 after the block
static void run(int b, const out::Device& dev, int blocks, bool stems, double* peak = nullptr) {
  for (int k = 0; k < blocks; ++k) {
    int32_t s[7][8] = {};
    int16_t v[7][8] = {};
    for (int f = 0; f < 7; ++f) {
      const double t = (7.0 * k + f) / 48000.0;
      for (int c = 0; c < 8; ++c) s[f][c] = (int32_t)(0.05 * 2147483647.0 * std::sin(6.2831853 * (110.0 + 37.0 * c) * t));
      for (int c = 0; c < 8; ++c) v[f][c] = (int16_t)(3000.0 * std::sin(6.2831853 * (330.0 + 11.0 * c) * t));
    }
    if (stems) {
      if (sendfx_pi::On(b)) sendfx_pi::FromRytm(b, s, 7);
      if (masterfx_pi::WantStems(b)) masterfx_pi::Stems(b, s, 7);
    }
    int32_t o[7][12];
    for (int f = 0; f < 7; ++f)
      for (int c = 0; c < 12; ++c) o[f][c] = (int32_t)(1000 * (c + 1) * (f + 1));
    float x[7][4] = {};
    sendfx_pi::FromVoices(b, dev, v, 7);
    sendfx_pi::AddReturn(b, dev, o, 7, x);
    masterfx_pi::Process(b, dev, o, 7);
    hash(o, sizeof o);
    hash(x, sizeof x);
    if (peak)
      for (int f = 0; f < 7; ++f) *peak = std::fmax(*peak, std::fabs((double)o[f][0] - 1000.0 * (f + 1)));
    sendfx_pi::Render(b);
    masterfx_pi::Render(b);
    core::g_ms += (k % 7 == 0);
  }
}

static uint64_t as_rytm(int b) {
  g_rytm = b;
  scene(b);
  report();
  CHECK(master::control_of(b).known && !master::control_of(1 - b).known);   // its report: its slot's
  CHECK(master::stems_on_main() == 6);
  CHECK(sendfx_pi::On(b) && master::on(b) && masterfx_pi::WantStems(b));
  if (b == 1) CHECK(!masterfx_pi::WantStems(0));   // (slot 0 has no scene yet)
  g_h = 1469598103934665603ull;
  core::g_ms = 0;
  const uint32_t f0 = sendfx_pi::ReadStats(b).frames, o0 = sendfx_pi::ReadStats(1 - b).frames;
  const uint32_t m0 = masterfx_pi::Read(b).frames, n0 = masterfx_pi::Read(1 - b).frames;
  run(b, out::kRytmMk2, 3000, true);
  CHECK(sendfx_pi::ReadStats(b).frames - f0 >= 3000 * 7 - 2 * sendfx_pi::kMaxPerRender);   // its stems paced it
  CHECK(sendfx_pi::ReadStats(1 - b).frames == o0);                                        // the other slot's: untouched
  CHECK(masterfx_pi::Read(b).frames > m0 && masterfx_pi::Read(1 - b).frames == n0);
  master::link_lost();   // the Rytm left: its report is forgotten, in either slot
  CHECK(!master::control_of(0).known && !master::control_of(1).known && master::stems_on_main() == 0);
  return g_h;
}

int main() {
  ins::slot_fn = hand;
  master::rytm_slot_fn = rytm;
  const uint64_t h1 = as_rytm(1);   // the Rytm attached second (an A4 first)
  printf("test_rytm_slot1: the Rytm in slot 1: hash %016llx\n", (unsigned long long)h1);
  const uint64_t h0 = as_rytm(0);   // the Rytm attached first, as ever
  printf("test_rytm_slot1: the Rytm in slot 0: hash %016llx\n", (unsigned long long)h0);
  CHECK(h0 == h1);
  printf("test_rytm_slot1: the Rytm's send FX and SRC MIX play bit for bit the same in slot 1 as in slot 0\n");

  // A box (no stems: an A4-like row) in slot 0 with the same scene (SRC MIX set): its voices pace its send FX (before: its
  // voices waited for stems that never came and nothing rendered), and its master mixes no stems
  static const out::Device kBox = {"box", 6, {0, 1}, {0, 1}, {}, {}, {}, {}, 0, 6, out::kStereoMix};
  g_rytm = 1;   // the Rytm, if any, elsewhere
  const uint32_t f0 = sendfx_pi::ReadStats(0).frames;
  double pk = 0;
  run(0, kBox, 400, false, &pk);
  CHECK(sendfx_pi::ReadStats(0).frames - f0 >= 400 * 7 - 2 * sendfx_pi::kMaxPerRender);
  CHECK(pk > 1000.0);   // its send FX return (and the chain) on its pair
  printf("test_rytm_slot1: a box in slot 0 has its send FX paced by its voices, no stems in its SRC MIX\n");
  printf("test_rytm_slot1: PASS\n");
}
