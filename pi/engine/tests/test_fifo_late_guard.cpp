// fifo_late.h with the measured FX guard (core/fx/fx_guard.cpp): LT13 P5's shape (2026-11-25, T509) on the host.
// Two boxes take turns: a base pass (the A4, slot 0) then a side pass (the Rytm, slot 1), each on cores 1 and 2. The Rytm's
// V1 A and V2 A (core 1, side passes) are slowed by 20 and 40 us, the cycle goes past a block, and the FIFOs drain to the
// floor and stay there (the control loop cannot refill them). On the unit the mute came 15.2 s late: the FIFO pinned low
// gave one dip and no hold, and the late core flipped between 1 and 2 pass by pass. Here the guard must mute V2 A within
// 20 ms of the overload, exactly one effect, and the FIFOs must climb back.
//
// The FIFO model: each pass drains 12 frames for every 250 us of the slower core's time plus core 3's own, and adds the
// block it rendered (12 frames); the level starts at the target. A muted slot costs nothing from its next block.
#include <stdio.h>
#include <string.h>

#include <cstdarg>
#include <string>

#include "check.h"
#include "fifo_late.h"
#include "fx/fx_guard.h"

static std::string g_log;
namespace core {
uint32_t cycles() { static uint32_t c; return c += 1000; }
uint32_t cycles_per_second() { return 54000000; }
uint32_t g_ms;
uint32_t now_ms() { return g_ms; }
void log(const char* fmt, ...) {
  char b[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  g_log += b;
  g_log += "\n";
}
}  // namespace core
extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap) { return vsnprintf(out, n, fmt, ap); }
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r;
}

namespace g = core::fx::guard;

static const uint32_t kBlock = 12, kTarget = 96;
static const uint32_t kTicksUs = 54;   // cycles_per_second / 1e6

struct Fx { int unit; int core; bool side; uint32_t us; };

struct Result { int mutes; int first_mute_pass; int muted_unit; uint32_t under; uint32_t level_end; uint32_t fits; };

// cores.cpp's gate (fifo_late.h counts, decided 2026-11-26 after VA1): a FIFO-late pass counts for its core only at or
// over admission's limit for its time a block, or after a real underrun. g_gate off: the guard as before (the shape's
// precondition). g_stall_every: every so many passes a long control pass (g_stall_us) and 22 passes later a shorter one
// (g_stall2_us), rendering nothing while both FIFOs play on: two dips (VA1: pass max 2-3 ms in normal play, 7 dips in the
// mute's second).
static bool g_gate = true;
static int g_stall_every = 0;
static uint32_t g_stall_us = 0, g_stall2_us = 0;
static uint32_t stall(int p) {
  if (!g_stall_every || p < 2 * (int)fifo_late::State::kGrace) return 0;
  const int k = p % g_stall_every;
  return k == 0 ? g_stall_us : k == 22 ? g_stall2_us : 0;
}

// passes: base and side alternate (pass 0 base). slow_at: the pass the slow commands land. Per pass kind, each core's
// own work outside the listed effects (voices, other chains), us.
static Result run(int passes, int slow_at, const uint32_t own[2][3], const Fx* fx, int nfx, uint32_t c3_us,
                  uint32_t slow_v1, uint32_t slow_v2, int v1, int v2) {
  g::reset();
  fifo_late::Pair fl;
  uint64_t last[2][3] = {};
  double level[2] = {kTarget, kTarget};
  Result r = {0, -1, -1, 0, 0, 0};
  uint32_t late_seen = 0;
  fifo_late::Load load[3];
  fifo_late::Under under;
  // the grace and a quiet window first
  for (int p = 0; p < passes; ++p) {
    const bool side = p & 1;
    const uint32_t lvl = (uint32_t)level[side];
    const uint32_t nl = g::stats().late;
    if (nl != late_seen) late_seen = nl, fl.rearm();
    const bool late = fl.step(side, lvl, 1, kBlock, true, true);
    for (int c = 1; c <= 2; ++c) load[c].step(last[0][c] + last[1][c]);
    under.step(r.under);
    unsigned lc = late ? fifo_late::blame(last[0][1], last[1][1], last[0][2], last[1][2]) : 0;
    if (lc && g_gate && !fifo_late::counts(load[lc].bp(250), 8000, under.recent())) lc = 0, ++r.fits;
    uint32_t t[3] = {0, own[side][1], own[side][2]};
    for (int c = 1; c <= 2; ++c) {
      g::set_core(c);
      for (int i = 0; i < nfx; ++i) {
        const Fx& f = fx[i];
        if (f.core != c || f.side != side || g::muted(f.unit)) continue;
        uint32_t us = f.us;
        if (p >= slow_at && f.unit == v1) us += slow_v1;
        if (p >= slow_at && f.unit == v2) us += slow_v2;
        g::note(f.unit, us * kTicksUs, 12);
        t[c] += us;
      }
      last[side][c] = t[c];
      g::core_done(c, t[c] >= 200, lc == (unsigned)c);
    }
    // the FIFO: drained by this pass's time (the slower core, then core 3's own), refilled by the block
    const uint32_t pass_us = (t[1] > t[2] ? t[1] : t[2]) + c3_us + stall(p);
    for (int k = 0; k < 2; ++k) {
      level[k] -= 12.0 * pass_us / 250.0;   // (each FIFO plays 12 frames every 250 us)
      if (k == (int)side) level[k] += 12;   // (the block this pass rendered)
      if (level[k] < 0) level[k] = 0, ++r.under;
      if (level[k] > kTarget) level[k] = kTarget;
    }
    if (r.first_mute_pass < 0 && g::stats().late) r.first_mute_pass = p;
  }
  for (int u = 0; u < g::kBuiltin; ++u)
    if (g::muted(u)) r.muted_unit = u, ++r.mutes;
  r.level_end = (uint32_t)level[1];
  g::poll();
  return r;
}

int main() {
  // the boxes: box 0 the A4 (base), box 1 the Rytm (side)
  const int v1a = g::ins_unit(1, 0, 0), v2a = g::ins_unit(1, 1, 0);
  const Fx fx[] = {
      {g::ins_unit(0, 0, 0), 1, false, 3},   // A4 V1 A, core 1
      {g::ins_unit(0, 2, 0), 2, false, 4},   // A4 V3 A, core 2
      {v1a, 1, true, 6},                     // Rytm V1 A SAT, core 1
      {v2a, 1, true, 5},                     // Rytm V2 A SAT, core 1
      {g::ins_unit(1, 4, 0), 2, true, 3},    // Rytm V5 A, core 2
  };
  // own work a pass: base (A4) heavier on core 2, side (Rytm) heavier on core 1 (the flip LT13 showed)
  // (quiet: 221 us a cycle; slowed: 281 us, an overload the FIFO cannot ride out; V2 A muted: 236 us, it climbs back)
  const uint32_t own[2][3] = {{0, 70, 86}, {0, 80, 50}};
  const uint32_t c3 = 20;   // core 3's own after the render wait
  const int kSlowAt = 2 * (int)fifo_late::State::kGrace + 400, kPasses = kSlowAt + 8000 * 2;   // ~2 s after the slow
  {  // without the slow: never a mute, never an underrun
    Result r = run(kPasses, 1 << 30, own, fx, 5, c3, 0, 0, v1a, v2a);
    CHECK(r.mutes == 0 && r.under == 0);
    printf("test_fifo_late_guard: no slow: %d mutes, %u underruns\n", r.mutes, r.under);
  }
  {  // LT13 P5: V1 A +20 us, V2 A +40 us
    Result r = run(kPasses, kSlowAt, own, fx, 5, c3, 20, 40, v1a, v2a);
    const double ms = r.first_mute_pass < 0 ? -1 : (r.first_mute_pass - kSlowAt) * 0.125;   // (two passes a 250 us cycle)
    printf("test_fifo_late_guard: LT13 P5 shape: %d mute(s), first %.1f ms after the slow, unit %d (V2 A %d), "
           "%u underrun passes, side FIFO %u at the end\n", r.mutes, ms, r.muted_unit, v2a, r.under, r.level_end);
    CHECK(r.mutes == 1);
    CHECK(r.muted_unit == v2a);
    CHECK(ms >= 0 && ms < 20);
    CHECK(r.level_end >= 2 * kBlock);
  }
  {  // LT13 P5 with the gate: core 1 at ~90 % a block once slowed, over the limit: the same one mute, no later
    Result r = run(kPasses, kSlowAt, own, fx, 5, c3, 20, 40, v1a, v2a);
    CHECK(r.mutes == 1 && r.muted_unit == v2a && r.first_mute_pass >= kSlowAt && (r.first_mute_pass - kSlowAt) * 0.125 < 20);
    g_gate = false;
    Result o = run(kPasses, kSlowAt, own, fx, 5, c3, 20, 40, v1a, v2a);
    g_gate = true;
    CHECK(o.mutes == 1 && o.first_mute_pass == r.first_mute_pass && o.under == r.under);
    printf("test_fifo_late_guard: LT13 P5 gated: first mute at the same pass as ungated (%d), %u underrun passes both\n",
           r.first_mute_pass - kSlowAt, r.under);
  }
  {  // VA1 (2026-11-26, T515) at 1200 MHz, admitted 79.77 %: core 1 measures ~73 % a block (the Rytm's FX1 CLOUDSEED
     // 18 us in its passes, ~9 us a pass as the guard counts it); two long control passes dip both FIFOs under a block,
     // no underrun. Ungated the guard muted FX1 (VA1's three mutes); gated nothing goes, nothing underruns.
    const int fx1 = g::send_unit(1, 0);
    const Fx va1[] = {
        {g::ins_unit(0, 0, 0), 1, false, 3},   // A4 V1 A, core 1
        {g::ins_unit(0, 2, 0), 2, false, 4},   // A4 V3 A, core 2
        {v1a, 1, true, 6},                     // Rytm V1 A SAT, core 1
        {v2a, 1, true, 5},                     // Rytm V2 A SAT, core 1
        {fx1, 1, true, 18},                    // Rytm FX1 CLOUDSEED, core 1 (slot 1's send FX)
        {g::ins_unit(1, 4, 0), 2, true, 3},    // Rytm V5 A, core 2
    };
    const uint32_t own1[2][3] = {{0, 70, 60}, {0, 80, 50}};   // core 1: 73 + 109 = 182 us a block (73 %)
    const uint32_t c3v = 5;                                   // (core 3's own: the cycle 192 us, the FIFOs refill)
    g_stall_every = 400, g_stall_us = 1900, g_stall2_us = 600;   // 91 frames of the 96 gone, then 29: under a block, not empty
    g_gate = false;
    Result o = run(kPasses, 1 << 30, own1, va1, 6, c3v, 0, 0, v1a, v2a);
    g_gate = true;
    printf("test_fifo_late_guard: VA1 shape ungated: %d mute(s) (unit %d, FX1 %d), %u underrun passes\n", o.mutes, o.muted_unit, fx1, o.under);
    CHECK(o.under == 0);
    CHECK(o.mutes >= 1);   // (the precondition: the old rule mutes in this shape)
    Result r = run(kPasses, 1 << 30, own1, va1, 6, c3v, 0, 0, v1a, v2a);
    CHECK(r.under == 0 && r.mutes == 0 && r.fits > 0);
    printf("test_fifo_late_guard: VA1 shape: ungated %d mute(s), gated %d (%u passes fit), 0 underruns\n", o.mutes, r.mutes, r.fits);
    g_stall_us = 2300;   // 110 frames: the FIFO runs empty (VA1 O1 P1's +4): a real underrun still mutes
    Result u = run(kPasses, 1 << 30, own1, va1, 6, c3v, 0, 0, v1a, v2a);
    CHECK(u.under > 0 && u.mutes >= 1);
    printf("test_fifo_late_guard: VA1 shape with underruns: %d mute(s), %u underrun passes\n", u.mutes, u.under);
    g_stall_every = 0, g_stall_us = 0, g_stall2_us = 0;
  }
  {  // blame: the cycle, not the last pass (LT13: the last pass's longer core flipped between 1 and 2)
    CHECK(fifo_late::blame(90, 160, 120, 70) == 1);   // core 1 longer over the cycle, core 2 in the base pass
    CHECK(fifo_late::blame(120, 0, 90, 0) == 1);      // one box: its pass alone
    CHECK(fifo_late::blame(90, 0, 120, 0) == 2);
  }
  printf("test_fifo_late_guard: PASS\n");
  return 0;
}
