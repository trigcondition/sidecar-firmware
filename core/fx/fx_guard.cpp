// core/fx/fx_guard.cpp - the measured-time guard of every effect (fx_guard.h; decided 2026-11-23)
#include "fx/fx_guard.h"

#include <string.h>

#include "fx/insert.h"
#include "fx/send_fx.h"
#include "platform.h"

#ifndef RYTM_RENDER_CORES
#define RYTM_RENDER_CORES 1
#endif
#if RYTM_RENDER_CORES > 1
extern "C" int rytm_render_core();
#endif

namespace core {
namespace fx {
namespace guard {

static_assert(kInsChains == ins::kChains && kInsSlots == ins::kSlots, "fx_guard.h's insert shape");
static_assert(kSendUnits == kSlots, "fx_guard.h's send slots");
static_assert(kUser == kBoxes * kUserSlots, "fx_guard.h's user effects (um.cpp kFx)");
static_assert(kUnits <= 64, "one bit a unit");

void (*user_mute)(int u, uint32_t us, int core) = nullptr;
bool (*user_state)(int u, const char** why, const char** id) = nullptr;

namespace {

enum Kind : uint8_t { kNone = 0, kHuge = 1, kSlow = 2, kDearest = 3, kForced = 4, kLate = 5 };
enum Mode : uint8_t { kModeOff = 0, kModeOn = 1, kModeReport = 2 };

// each core's own (only that core writes it, but core_done's caller and the host tests)
struct Row {
  uint64_t touched;              // units noted since the last core_done
  uint64_t win;                  // units with time in the hot run's window
  uint32_t blk_ticks[kBuiltin];  // this block's
  uint16_t blk_frames[kBuiltin];
  uint32_t user_us[kUser];
  uint32_t win_us[kUnits];       // the hot run's sums (the dearest's pick)
  uint8_t over[kBuiltin];        // blocks over the budget in a row while hot
  uint8_t hot_run;               // hot blocks in a row
  uint8_t settle;                // blocks left after a mute on this core with the dearest rule off (kSettle)
  uint8_t late_run;              // late blocks in a row with no effect over its budget (kLateRun)
  uint64_t ew;                   // units with time in ew_us
  uint32_t ew_us[kUnits];        // each unit's time over the last ~16 blocks on this core, decaying (a late core's dearest)
  // the late log (late_report): late blocks since the last report, and why each that muted nothing did not
  uint32_t n_late, n_muted, n_run, n_settle, n_floor, n_other;
  uint8_t run_max;               // the longest late_run seen
  int8_t cand;                   // the late dearest last seen (before the floor) + 1 (0: none yet), its time a block
  uint32_t cand_us;
};
Row g_row[kCores];

volatile uint8_t g_m[kBuiltin];            // 1: muted, 2: muted with the short fade (kLate) (the render side reads it)
volatile uint64_t g_news = 0;              // muted (or would be: report) since the last poll, bit unit
volatile uint64_t g_would = 0;             // report mode: what would have been muted (logged once)
struct Rec { uint8_t kind, core, type, worded; uint32_t us; };
Rec g_rec[kUnits];                          // the mute's facts (render side writes before g_m / g_news)
char g_why[kBuiltin][72];                   // the control core's words
volatile uint32_t g_slow[kBuiltin];        // test: extra us inside the unit's call
uint32_t g_budget_us = 120;                // API 3's per call budget, a 12-frame block
volatile uint8_t g_mode = kModeOn;
volatile bool g_fifo_late = true;   // the engine's FIFO under a block counts as late (fxguard=fifo:on|off)
Stats g_stats;
int g_host_core = 0;

inline uint32_t to_us(uint32_t ticks) {
  const uint32_t cps = cycles_per_second();
  return cps ? (uint32_t)((uint64_t)ticks * 1000000u / cps) : ticks;
}
inline uint64_t bit(int u) { return 1ull << u; }
uint32_t num(const char* p) {
  uint32_t v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0');
  return v;
}

const char* kind_text(uint8_t k) {
  return k == kHuge ? "huge" : k == kSlow ? "slow" : k == kDearest ? "dearest" : k == kForced ? "forced" : k == kLate ? "late" : "-";
}

// a unit's place: "V1 A" .. "M2 B", "FX1", "FX2", "UFX1".."UFX4"
void where(int u, char* b, int cap) {
  if (u >= kBuiltin) {
    sniprintf(b, cap, "UFX%d", (u - kBuiltin) % kUserSlots + 1);
    return;
  }
  const int k = u % kPerBox;
  if (k >= kInsUnits) sniprintf(b, cap, "FX%d", k - kInsUnits + 1);
  else sniprintf(b, cap, "%s %c", ins::chain_name(k / kInsSlots), 'A' + k % kInsSlots);
}
int box_of_unit(int u) { return u >= kBuiltin ? (u - kBuiltin) / kUserSlots : u / kPerBox; }
int unit_from(int box, const char* at) {   // "V1:A" / "V1 A" / "FX1" / "fx2"
  if (box < 0 || box >= kBoxes || !at) return -1;
  if ((at[0] == 'f' || at[0] == 'F') && (at[1] == 'x' || at[1] == 'X') && (at[2] == '1' || at[2] == '2'))
    return send_unit(box, at[2] - '1');
  char n[4] = {0};
  int i = 0;
  while (at[i] && at[i] != ':' && at[i] != ' ' && i < 3) n[i] = at[i], ++i;
  const int c = ins::chain_from_name(n);
  if (c < 0 || (at[i] != ':' && at[i] != ' ')) return -1;
  const char s = at[i + 1];
  if (s != 'A' && s != 'a' && s != 'B' && s != 'b') return -1;
  return ins_unit(box, c, s == 'A' || s == 'a' ? 0 : 1);
}

// the render side: unit u muted (or, in report mode, noted)
void mute(int u, uint32_t us, uint8_t kind, int core) {
  Rec& r = g_rec[u];
  if (u < kBuiltin && g_m[u]) return;
  if (g_mode == kModeOff) return;
  r.kind = kind, r.core = (uint8_t)core, r.us = us ? us : 1, r.worded = 0;
  r.type = (uint8_t)(u < kBuiltin ? type_now(u) : 0);
  if (g_mode == kModeReport) {
    __atomic_or_fetch(&g_would, bit(u), __ATOMIC_RELEASE);
    return;
  }
  if (u >= kBuiltin) {   // a user effect: um.cpp's mute (its why, its fade back in when reloaded)
    if (user_mute) user_mute(u - kBuiltin, us, core);
    __atomic_or_fetch(&g_news, bit(u), __ATOMIC_RELEASE);
    return;
  }
  for (Row& w : g_row) w.over[u] = 0;
  __atomic_store_n(&g_m[u], (uint8_t)(kind == kLate ? 2 : 1), __ATOMIC_RELEASE);
  __atomic_or_fetch(&g_news, bit(u), __ATOMIC_RELEASE);
}
bool user_muted(int k) {
  const char* w = nullptr;
  const char* id = nullptr;
  return user_state && user_state(k, &w, &id);
}

}  // namespace

int this_core() {
#if RYTM_RENDER_CORES > 1
  return rytm_render_core() & (kCores - 1);
#else
  return g_host_core;
#endif
}
void set_core(int c) { g_host_core = c & (kCores - 1); }

uint32_t (*g_clock)() = nullptr;
uint32_t g_spun = 0;   // (a test clock's: spin_us's waits added to it, so a test sees only the slowness it asked for)
uint32_t stamp() { return g_clock ? g_clock() + g_spun : cycles(); }
void set_clock(uint32_t (*clock)()) { g_clock = clock; }
void note(int unit, uint32_t ticks, int frames) {
  if (unit < 0 || unit >= kBuiltin) return;
  Row& r = g_row[this_core()];
  r.blk_ticks[unit] += ticks;
  r.blk_frames[unit] = (uint16_t)(r.blk_frames[unit] + (frames > 0 ? frames : 0));
  r.touched |= bit(unit);
}
void note_user(int u, uint32_t us) {
  if (u < 0 || u >= kUser) return;
  Row& r = g_row[this_core()];
  r.user_us[u] += us;
  r.touched |= bit(user_unit(u));
}

// After a mute on a core, its dearest rule waits kSettle blocks: the muted slot is still fading out (5 ms, ~20 blocks,
// its time still counted) and the core is still hot from it, so without the wait one slot over its budget took the next
// dearest with it in the same block, and a third 16 blocks later (LT12 prep, 2026-11-24). Per-slot rules go on.
constexpr uint8_t kSettle = 2 * kOverMax;
constexpr uint32_t kLateFloorUs = 8;   // a late core's dearest (none over budget) only from this mean a block (LT12k)
constexpr uint8_t kLateRun = 4;   // late blocks in a row (1 ms) before a late core's dearest goes, none over its budget
void core_done(int core, bool hot, bool late) {
  Row& r = g_row[core & (kCores - 1)];
  bool muted_here = false;   // (a mute in this call: the dearest rule waits)
  // a late block (the core past a whole block: the FIFO drains): this block's effect to mute at once, the one most over
  // its budget, else the dearest in the block (decided 2026-11-24 after LT12b: waiting kOverMax blocks and a 5 ms fade
  // while the slot still costs its time underran for ~18 ms, heard as a loud hit)
  int late_over = -1, late_most = -1;
  uint32_t late_over_by = 0, late_us = 0, late_most_us = 0;
  uint64_t t = r.touched;
  r.touched = 0;
  while (t) {
    const int u = __builtin_ctzll(t);
    t &= t - 1;
    uint32_t us;
    if (u < kBuiltin) {
      us = to_us(r.blk_ticks[u]);
      const uint32_t fr = r.blk_frames[u];
      r.blk_ticks[u] = 0, r.blk_frames[u] = 0;
      if (!g_m[u]) {
        const uint32_t b = g_budget_us * (fr > 12 ? fr : 12) / 12u;
        if (us >= kHugeUs) {
          ++g_stats.huge;
          mute(u, us, kHuge, core);
          muted_here = true;
        } else if (hot && us > b) {
          if (r.over[u] < 255) ++r.over[u];
          if (r.over[u] >= kOverMax) {
            ++g_stats.slow;
            r.over[u] = 0;
            mute(u, us, kSlow, core);
            muted_here = true;
          }
        } else {
          r.over[u] = 0;
        }
        if (!g_m[u] && us > b && us - b > late_over_by) late_over_by = us - b, late_over = u, late_us = us;
      }
    } else {
      us = r.user_us[u - kBuiltin];
      r.user_us[u - kBuiltin] = 0;
    }
    r.ew_us[u] += us, r.ew |= bit(u);
    if (hot) r.win_us[u] += us, r.win |= bit(u);
  }
  // a late core's dearest, none over its budget: the most time over the last ~16 blocks (ew_us, each block's time
  // added, then 1/16 off), not this block's alone. Two boxes take turns on the same core (the base box's pass, then
  // the side stage's), so one block holds one box's effects only: LT12j (2026-11-25) took a 2 us A4 amp in the A4's
  // pass while the Rytm's 80 us effects were in the other pass.
  for (uint64_t w = r.ew; w; w &= w - 1) {
    const int u = __builtin_ctzll(w);
    const bool gone = u < kBuiltin ? g_m[u] != 0 : user_muted(u - kBuiltin);
    if (!gone && r.ew_us[u] > late_most_us) late_most_us = r.ew_us[u], late_most = u;
    r.ew_us[u] -= (r.ew_us[u] + 15) >> 4;
    if (!r.ew_us[u]) r.ew &= ~bit(u);
  }
  late_most_us = (late_most_us + 15) >> 4;   // (about its time a block, for the log)
  if (late && late_most >= 0) r.cand = (int8_t)(late_most + 1), r.cand_us = late_most_us;
  // under kLateFloorUs a block muting it frees nothing (LT12k, 2026-11-25: a FIFO dip muted 1-5 us SAT inserts
  // while the two slowed slots ran 12 and 21 us; at 1200 MHz one harmless dip took four of them)
  if (late_most_us < kLateFloorUs) late_most = -1;
  // (an effect over its budget in a late block: at once; none over: kLateRun late blocks in a row first, so one block
  // made late by something else, a burst of notes, never takes an effect)
  r.late_run = late && !muted_here && !r.settle && late_over < 0 ? (uint8_t)(r.late_run < 255 ? r.late_run + 1 : 255) : 0;
  if (r.late_run > r.run_max) r.run_max = r.late_run;
  if (late) {   // (the late log: what this late block did, and if nothing, why)
    ++r.n_late;
    if (muted_here) ++r.n_other;
    else if (r.settle) ++r.n_settle;
    else if (late_over < 0 && late_most < 0) ++r.n_floor;
    else if (late_over < 0 && r.late_run < kLateRun) ++r.n_run;
  }
  if (late && !muted_here && !r.settle && (late_over >= 0 || (late_most >= 0 && r.late_run >= kLateRun))) {
    ++g_stats.late;
    ++r.n_muted;
    if (late_over >= 0) mute(late_over, late_us, kLate, core);
    else mute(late_most, late_most_us, kLate, core);
    muted_here = true;
    r.late_run = 0;
  }
  if (muted_here) r.settle = kSettle;
  if (!hot || r.settle) {
    if (r.settle) --r.settle;
    r.hot_run = 0;
    for (uint64_t w = r.win; w; w &= w - 1) r.win_us[__builtin_ctzll(w)] = 0;
    r.win = 0;
    return;
  }
  if (++r.hot_run < kOverMax) return;
  // a core hot kOverMax blocks in a row: the dearest effect on it over them
  int best = -1;
  uint32_t most = 0;
  for (uint64_t w = r.win; w; w &= w - 1) {
    const int u = __builtin_ctzll(w);
    const bool gone = u < kBuiltin ? g_m[u] != 0 : user_muted(u - kBuiltin);
    if (!gone && r.win_us[u] > most) most = r.win_us[u], best = u;
    r.win_us[u] = 0;
  }
  r.win = 0, r.hot_run = 0;
  if (best >= 0) {
    ++g_stats.dearest;
    mute(best, most / kOverMax, kDearest, core);   // (its mean a block over the run)
    r.settle = kSettle;
  }
}

bool muted(int unit) { return unit >= 0 && unit < kBuiltin && __atomic_load_n(&g_m[unit], __ATOMIC_RELAXED); }
bool short_fade(int unit) { return unit >= 0 && unit < kBuiltin && __atomic_load_n(&g_m[unit], __ATOMIC_RELAXED) == 2; }
uint32_t slow_us(int unit) { return unit >= 0 && unit < kBuiltin ? g_slow[unit] : 0; }
void spin_us(uint32_t us) {
  const uint32_t t0 = cycles(), n = (uint32_t)((uint64_t)us * cycles_per_second() / 1000000u);   // (the CPU-time counter: as note's)
  if (g_clock) {   // (a test's clock: no real wait, the time counted as spent)
    g_spun += n;
    return;
  }
  while (cycles() - t0 < n) {
  }
}

// ---- the control core

int type_now(int u) {
  if (u < 0 || u >= kBuiltin) return 0;
  const int box = u / kPerBox, k = u % kPerBox;
  if (box >= kVoiceSlots && k < kInsUnits) return 0;   // (an 8-voice build: one box's chains)
  if (k >= kInsUnits) return control_of(box).knob[k - kInsUnits][0];
  return ins::control_of(box).type[k / kInsSlots][k % kInsSlots];
}
const char* type_label(int u, int type) {
  if (u >= kBuiltin) return "";
  return u % kPerBox >= kInsUnits ? fx::type_name(type) : ins::type_name(type);
}

void poll() {
  const uint64_t news = __atomic_exchange_n(&g_news, 0ull, __ATOMIC_ACQ_REL);
  for (uint64_t w = news; w; w &= w - 1) {
    const int u = __builtin_ctzll(w);
    const Rec& r = g_rec[u];
    char at[12];
    where(u, at, sizeof at);
    ++g_stats.mutes;
    if (u >= kBuiltin) {
      ++g_stats.user_mutes;
      log(CORE_STR("fxguard: box %d %s muted: dearest on core %d while hot (%u us a block)"), box_of_unit(u), at, (int)r.core,
          (unsigned)r.us);
      continue;
    }
    char* y = g_why[u];
    if (r.kind == kHuge)
      sniprintf(y, sizeof g_why[u], "muted: ran %u us in one block (over %u us)", (unsigned)r.us, (unsigned)kHugeUs);
    else if (r.kind == kSlow)
      sniprintf(y, sizeof g_why[u], "muted: ran %u us, over its budget (%u us a block) on hot core %d", (unsigned)r.us,
               (unsigned)g_budget_us, (int)r.core);
    else if (r.kind == kDearest)
      sniprintf(y, sizeof g_why[u], "muted: ran %u us, dearest on core %d while hot", (unsigned)r.us, (int)r.core);
    else if (r.kind == kLate)
      sniprintf(y, sizeof g_why[u], "muted: ran %u us in a block that made core %d late", (unsigned)r.us, (int)r.core);
    else
      sniprintf(y, sizeof g_why[u], "muted: by the console (test)");
    log(CORE_STR("fxguard: box %d %s %s %s"), box_of_unit(u), at, type_label(u, r.type), y);
  }
  const uint64_t would = __atomic_exchange_n(&g_would, 0ull, __ATOMIC_ACQ_REL);
  for (uint64_t w = would; w; w &= w - 1) {
    const int u = __builtin_ctzll(w);
    const Rec& r = g_rec[u];
    char at[12];
    where(u, at, sizeof at);
    log(CORE_STR("fxguard: report: box %d %s would be muted (%s, %u us, core %d)"), box_of_unit(u), at, kind_text(r.kind),
        (unsigned)r.us, (int)r.core);
  }
  // the slot set again since its mute (another TYPE, OFF, a kit): released, it fades in (the insert's and send FX's own fades)
  for (int u = 0; u < kBuiltin; ++u)
    if (g_m[u] && type_now(u) != g_rec[u].type) clear(u);
}

void clear(int u) {
  if (u < 0 || u >= kBuiltin || !g_m[u]) return;
  for (Row& w : g_row) w.over[u] = 0;
  g_why[u][0] = 0;
  __atomic_store_n(&g_m[u], (uint8_t)0, __ATOMIC_RELEASE);
  ++g_stats.released;
  char at[12];
  where(u, at, sizeof at);
  log(CORE_STR("fxguard: box %d %s released (set again)"), box_of_unit(u), at);
}
void clear_box(int box) {
  if (box < 0 || box >= kBoxes) return;
  for (int k = 0; k < kPerBox; ++k) clear(box * kPerBox + k);
}
int muted_count(int box) {
  int n = 0;
  for (int k = 0; k < kPerBox && box >= 0 && box < kBoxes; ++k) n += g_m[box * kPerBox + k] != 0;
  return n;
}
const char* why(int u) { return u >= 0 && u < kBuiltin && g_m[u] ? g_why[u] : ""; }

namespace {
int jstr(char* o, int cap, int n, const char* s) {   // a JSON string (quotes and backslashes escaped)
  if (n < cap) o[n++] = '"';
  for (; *s && n < cap - 2; ++s) {
    if (*s == '"' || *s == '\\') o[n++] = '\\';
    o[n++] = *s >= ' ' ? *s : ' ';
  }
  if (n < cap) o[n++] = '"';
  return n;
}
int jput(char* o, int cap, int n, const char* s) {
  while (*s && n < cap) o[n++] = *s++;
  return n;
}
}  // namespace

int json(char* o, int cap) {
  if (cap <= 0) return 0;
  int n = jput(o, cap, 0, "\"fxMuted\":[");
  bool first = true;
  char t[32];
  for (int u = 0; u < kUnits; ++u) {
    const char* w = "";
    const char* ty = "";
    if (u < kBuiltin) {
      if (!g_m[u]) continue;
      w = g_why[u][0] ? g_why[u] : "muted";
      ty = type_label(u, g_rec[u].type);
    } else {
      const char* id = "";
      if (!user_state || !user_state(u - kBuiltin, &w, &id)) continue;
      ty = id ? id : "";
      if (!w || !w[0]) w = "muted";
    }
    if (!first) n = jput(o, cap, n, ",");
    first = false;
    sniprintf(t, sizeof t, "{\"box\":%d,\"at\":", box_of_unit(u));
    n = jput(o, cap, n, t);
    where(u, t, sizeof t);
    n = jstr(o, cap, n, t);
    n = jput(o, cap, n, ",\"type\":");
    n = jstr(o, cap, n, ty);
    n = jput(o, cap, n, ",\"user\":");
    n = jput(o, cap, n, u >= kBuiltin ? "true" : "false");
    n = jput(o, cap, n, ",\"why\":");
    n = jstr(o, cap, n, w);
    n = jput(o, cap, n, "}");
  }
  n = jput(o, cap, n, "],\"fxGuard\":");
  sniprintf(t, sizeof t, "{\"mode\":\"%s\",", g_mode == kModeOn ? "on" : g_mode == kModeReport ? "report" : "off");
  n = jput(o, cap, n, t);
  sniprintf(t, sizeof t, "\"budgetUs\":%u,", (unsigned)g_budget_us);
  n = jput(o, cap, n, t);
  sniprintf(t, sizeof t, "\"mutes\":%u}", (unsigned)g_stats.mutes);
  n = jput(o, cap, n, t);
  if (n >= cap) n = cap - 1;
  o[n] = 0;
  return n;
}

void set_budget_us(uint32_t us) { g_budget_us = us < 5 ? 5 : us > kHugeUs ? kHugeUs : us; }
uint32_t budget_us() { return g_budget_us; }
void set_enabled(bool on) { g_mode = on ? kModeOn : kModeOff; }
bool enabled() { return g_mode == kModeOn; }
bool fifo_late() { return g_fifo_late; }

bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "fxguard", 7) != 0 || (p[7] && p[7] != '=')) return false;
  const char* a = p[7] ? p + 8 : "";
  if (!a[0]) {
    int n = sniprintf(out, cap, "fxguard: %s, budget %u us a block, huge %u us; mutes %u (huge %u slow %u dearest %u late %u user %u), released %u, muted now %d + %d",
                     g_mode == kModeOn ? "on" : g_mode == kModeReport ? "report" : "off", (unsigned)g_budget_us,
                     (unsigned)kHugeUs, (unsigned)g_stats.mutes, (unsigned)g_stats.huge, (unsigned)g_stats.slow,
                     (unsigned)g_stats.dearest, (unsigned)g_stats.late, (unsigned)g_stats.user_mutes, (unsigned)g_stats.released, muted_count(0),
                     muted_count(1));
    for (int u = 0; u < kBuiltin && n < cap - 40; ++u)
      if (g_m[u]) {
        char at[12];
        where(u, at, sizeof at);
        n += sniprintf(out + n, cap - n, "; box %d %s %s", box_of_unit(u), at, g_why[u]);
      }
    return true;
  }
  if (!strcmp(a, "on") || !strcmp(a, "off") || !strcmp(a, "report")) {
    g_mode = a[1] == 'n' ? kModeOn : a[0] == 'r' ? kModeReport : kModeOff;
    sniprintf(out, cap, "fxguard: %s", a);
    return true;
  }
  if (!strncmp(a, "budget:", 7)) {
    set_budget_us((uint32_t)num(a + 7));
    sniprintf(out, cap, "fxguard: budget %u us a block", (unsigned)g_budget_us);
    return true;
  }
  if (!strcmp(a, "fifo:on") || !strcmp(a, "fifo:off")) {
    g_fifo_late = a[6] == 'n';
    sniprintf(out, cap, "fxguard: the FIFO under a block counts as late: %s", g_fifo_late ? "on" : "off");
    return true;
  }
  if (!strcmp(a, "clear")) {
    for (int u = 0; u < kBuiltin; ++u) clear(u), g_slow[u] = 0;
    sniprintf(out, cap, "fxguard: cleared");
    return true;
  }
  if (!strncmp(a, "mute:", 5) || !strncmp(a, "slow:", 5)) {   // <box>:<at>[:<us>]
    const bool slow = a[0] == 's';
    const int box = a[5] - '0';
    const char* at = a[6] == ':' ? a + 7 : "";
    const int u = unit_from(box, at);
    if (u < 0) {
      sniprintf(out, cap, "fxguard: which? (%s<box>:<V1..V8|B1..B3|M1|M2>:<A|B>%s, or FX1 / FX2)", slow ? "slow:" : "mute:",
               slow ? ":<us>" : "");
      return true;
    }
    char at2[12];
    where(u, at2, sizeof at2);
    if (slow) {
      const char* c = strrchr(at, ':');
      const uint32_t us = c && c != at + 2 ? (uint32_t)num(c + 1) : 0;
      g_slow[u] = us > 5000 ? 5000 : us;
      sniprintf(out, cap, "fxguard: box %d %s runs %u us slower (test)", box, at2, (unsigned)g_slow[u]);
    } else {
      const uint8_t m = g_mode;
      g_mode = kModeOn;
      mute(u, 1, kForced, 3);
      g_mode = m;
      sniprintf(out, cap, "fxguard: box %d %s muted (test)", box, at2);
    }
    return true;
  }
  sniprintf(out, cap, "fxguard: fxguard, fxguard=on|off|report|budget:<us>|fifo:on|off|clear|mute:<box>:<at>|slow:<box>:<at>:<us>");
  return true;
}

int late_report(char* out, int cap) {
  int n = 0;
  if (cap > 0) out[0] = 0;
  for (int c = 0; c < kCores && n < cap; ++c) {
    Row& r = g_row[c];
    const uint32_t late = __atomic_exchange_n(&r.n_late, 0u, __ATOMIC_RELAXED);
    const uint32_t muted = __atomic_exchange_n(&r.n_muted, 0u, __ATOMIC_RELAXED);
    const uint32_t run = __atomic_exchange_n(&r.n_run, 0u, __ATOMIC_RELAXED);
    const uint32_t settle = __atomic_exchange_n(&r.n_settle, 0u, __ATOMIC_RELAXED);
    const uint32_t floor = __atomic_exchange_n(&r.n_floor, 0u, __ATOMIC_RELAXED);
    const uint32_t other = __atomic_exchange_n(&r.n_other, 0u, __ATOMIC_RELAXED);
    const unsigned run_max = __atomic_exchange_n(&r.run_max, (uint8_t)0, __ATOMIC_RELAXED);
    if (!late) continue;
    char at[12] = "-";
    const int u = r.cand - 1;
    if (u >= 0 && u < kUnits) where(u, at, sizeof at);
    n += sniprintf(out + n, cap - n, "%sc%d late %u muted %u, none: run<%u %u settle %u floor %u other %u, run max %u, dearest box %d %s %u us",
                   n ? " | " : "", c, (unsigned)late, (unsigned)muted, (unsigned)kLateRun, (unsigned)run, (unsigned)settle,
                   (unsigned)floor, (unsigned)other, run_max, u >= 0 ? box_of_unit(u) : -1, at, (unsigned)r.cand_us);
    if (n >= cap) n = cap - 1;
  }
  return n;
}

const Stats& stats() { return g_stats; }
void reset() {
  memset(g_row, 0, sizeof g_row);
  memset(g_rec, 0, sizeof g_rec);
  memset(g_why, 0, sizeof g_why);
  for (int u = 0; u < kBuiltin; ++u) g_m[u] = 0, g_slow[u] = 0;
  g_news = 0, g_would = 0;
  g_budget_us = 120, g_mode = kModeOn;
  g_stats = Stats();
}

}  // namespace guard
}  // namespace fx
}  // namespace core
