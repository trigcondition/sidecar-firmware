// core/mod/rytm_param.cpp - see rytm_param.h. Control side only, cold.
#include "mod/rytm_param.h"

#if CORE_POLY
#include <string.h>

#include "hooks.h"
#include "mod/rytm_out.h"
#include "platform.h"
#include "ui/remote.h"

namespace core {
namespace mod {

namespace {

constexpr int16_t kNone = INT16_MIN;
struct E {
  uint8_t t = 0xFF, w = 0;   // t 0xFF: free
  float off = 0;             // the offset wanted (steps)
  bool rel = false;          // the layer let go (rp_release): its 0 a release, not a crossing
  int16_t off_sent = kNone;  // 1/64 step
  uint32_t off_ms = 0;       // when it went
  uint32_t chg_ms = 0;       // when it went as a change (not a refresh), and the gap between its last two changes:
  uint32_t chg_gap = 0;      // its own update interval (the ramp; 0 none yet)
  bool lock = false;         // a seq p-lock (lock_v, 1/128 step)
  uint16_t lock_v = 0;
  bool morph = false, has_a = false, has_b = false, step = false, upper = false;
  float a = 0, b = 0, f = 0;
  uint16_t ovr = 0;          // composed: the override wanted
  uint8_t mix = 0;
  int16_t ovr_sent = -1, mix_sent = -1;   // -1: none sent since the hello
  uint32_t ovr_ms = 0;
  bool due = false;          // a p-lock change: at due_us, a jump, outside the grid
  uint32_t due_us = 0;
};
E g_e[kRpEntries];
int g_caps = 0;
bool g_link = false;       // a hello came since boot / the USB went
int g_kit = -1;
uint16_t g_clear = 0;      // tracks owed a 06
uint32_t g_last_us = 0, g_off_us = 0, g_ovr_us = 0;   // the tick's clock; the last 04 / 05 sent
bool g_started = false;
bool g_ovr_first = false;  // which grid op goes first when both are due (they take turns: neither starves the other)
bool g_hold = false;       // a kit / project change: no grid send until the layer's next end_tick (rp_layer_done)
uint32_t g_hold_us = 0;
float g_credit = 0;        // the cap's bytes left (floored: a p-lock burst's debt is paid in at most 100 ms)
uint32_t g_lead_us = kRpLeadUs;
uint32_t g_win_ms = 0, g_win_bytes = 0, g_bps = 0;   // bytes in the current second; the last whole second's
uint32_t g_said_ms = 0;
bool g_said_any = false;
uint8_t g_status[8];
int g_status_n = 0;
RpStats g_st;
// rp_saturated: a backlog of changed offsets at the grid's slots since g_bl_ms (g_bl), the last slot that found it kSatMs
// on end (g_sat_ms), the tick's time
bool g_bl = false, g_sat = false;
uint32_t g_bl_ms = 0, g_sat_ms = 0, g_now_ms = 0;
uint32_t g_ms_rem = 0;   // the us not yet in g_now_ms: a ms counter that runs on across the 32-bit us wrap (us / 1000 jumps back)

CORE_COLD int off64(float steps) {
  float x = steps * 64.0f;
  int v = (int)(x + (x < 0 ? -0.5f : 0.5f));
  return v < -8192 ? -8192 : v > 8191 ? 8191 : v;
}
CORE_COLD E* find(int t, int w) {
  for (E& e : g_e)
    if (e.t == t && e.w == w) return &e;
  return nullptr;
}
CORE_COLD E* get(int t, int w) {
  if (E* e = find(t, w)) return e;
  for (E& e : g_e)
    if (e.t == 0xFF) {
      e = E();
      e.t = (uint8_t)t, e.w = (uint8_t)w;
      return &e;
    }
  ++g_st.dropped;
  return nullptr;
}
// the override the parts make now (rytm-param-mod 5.3; a p-lock's value is the base the morph blends from)
CORE_COLD void compose(E& e) {
  if (e.morph) {
    float base = e.lock ? (float)e.lock_v / 128.0f : 0;
    bool a_ok = e.has_a || e.lock, b_ok = e.has_b || e.lock;
    float a = e.has_a ? e.a : base, b = e.has_b ? e.b : base, v;
    int mix = 127;
    if (a_ok && b_ok) v = e.step ? (e.upper ? b : a) : a + (b - a) * e.f;
    else if (a_ok) v = a, mix = e.step ? (e.upper ? 0 : 127) : (int)((1.0f - e.f) * 127.0f + 0.5f);   // A only
    else v = b, mix = e.step ? (e.upper ? 127 : 0) : (int)(e.f * 127.0f + 0.5f);                      // B only
    int iv = (int)(v * 128.0f + 0.5f);
    e.ovr = (uint16_t)(iv < 0 ? 0 : iv > 16383 ? 16383 : iv), e.mix = (uint8_t)(mix < 0 ? 0 : mix > 127 ? 127 : mix);
  } else if (e.lock) {
    e.ovr = e.lock_v, e.mix = 127;
  } else {
    e.mix = 0;   // released (the value is kept: a release record carries the last one)
  }
}
CORE_COLD bool active(const E& e) { return rp_word_active(e.t, e.w); }
CORE_COLD int want_off(const E& e) { return active(e) ? off64(e.off) : 0; }
CORE_COLD int want_mix(const E& e) { return active(e) ? e.mix : 0; }
CORE_COLD void maybe_free(E& e) {
  if (e.t == 0xFF || e.off != 0 || e.lock || e.morph || e.due) return;
  if ((e.off_sent != kNone && e.off_sent != 0) || e.mix_sent > 0) return;
  e = E();
}

CORE_COLD void count(int bytes) {
  g_credit -= (float)bytes;
  if (g_credit < -(float)kBytesPerS / 10.0f) g_credit = -(float)kBytesPerS / 10.0f;
  g_win_bytes += (uint32_t)bytes;
  g_st.bytes += (uint32_t)bytes;
  ++g_st.msgs;
}
// every field is made 7-bit where it is computed; a byte above 0x7f here is a bug: counted (bad), logged, not sent
// (false: refused as bad; the caller then books nothing as sent)
CORE_COLD bool send(const uint8_t* b, int n) {   // F0 7D 52 54 5A 01 b... F7
  uint8_t m[136] = {0xF0, 0x7D, 0x52, 0x54, kHostCmd, 1};
  int k = 6;
  for (int i = 0; i < n; ++i)
    if (b[i] > 0x7F || n > (int)sizeof m - 7) {
      ++g_st.bad;
      log(CORE_STR("rytm params: a bad message (op %d, byte %d = %d) not sent"), b[0], i, b[i]);
      return false;
    }
  for (int i = 0; i < n; ++i) m[k++] = b[i];
  m[k++] = 0xF7;
  to_rytm(m, k);
  count(k);
  return true;
}
CORE_COLD int blocks(uint32_t us) {   // a ramp in Rytm blocks (32 frames at 48 kHz: 666.7 us)
  int b = (int)((us * 3u + 1000u) / 2000u);
  return b < 1 ? 1 : b > 127 ? 127 : b;
}
CORE_COLD bool ready() { return g_link && (g_caps & kCapsParamMod) && g_kit >= 0; }
CORE_COLD void forget_sent() {
  for (E& e : g_e) {
    if (e.t == 0xFF) continue;
    e.off_sent = kNone, e.ovr_sent = e.mix_sent = -1;
    e.due = false;   // a pending p-lock goes with the grid (its due time may be far gone: a long link loss)
    maybe_free(e);
  }
}

// the 06 owed
CORE_COLD void send_clear() {
  if (!g_clear) return;
  const uint8_t b[5] = {kOpClear, (uint8_t)g_kit, 0, (uint8_t)(g_clear & 0x7F), (uint8_t)(g_clear >> 7 & 0x3F)};
  send(b, 5);
  ++g_st.clears;
  g_clear = 0;
}
// up to n records chosen: the lowest pass first (pick: 0 none, 1.. by priority), oldest first within a pass. Each
// entry's pick runs once (a candidate list), then a partial selection: O(128 + n x candidates) integer compares.
// *changed: the candidates below the refresh pass (the records that changed), for the ramp's rounds
struct Cand { E* e; uint8_t pass; uint32_t at; };
template <class Pick>
CORE_COLD int choose(E** out, int n, Pick pick, uint32_t E::*at, int refresh_pass, int* changed) {
  Cand c[kRpEntries];
  int m = 0, ch = 0;
  for (E& e : g_e) {
    if (e.t == 0xFF) continue;
    int p = pick(e);
    if (!p) continue;
    c[m++] = {&e, (uint8_t)p, e.*at};
    ch += p < refresh_pass;
  }
  int k = 0;
  for (; k < n && k < m; ++k) {
    int b = k;
    for (int i = k + 1; i < m; ++i)
      if (c[i].pass < c[b].pass || (c[i].pass == c[b].pass && (int32_t)(c[i].at - c[b].at) < 0)) b = i;
    Cand x = c[k];
    c[k] = c[b], c[b] = x;
    out[k] = c[k].e;
  }
  if (changed) *changed = ch;
  return k;
}
// the ramp of a grid send (rytm-param-mod 1.2, 2.4): until the record's next update, so a moving value is a line on the
// Rytm, not a staircase. The next send: the grid, or the cap's wait for this message's bytes; times the rounds the
// changed records need (more than a message holds: each one comes every rounds-th message); at least the records' own
// last update interval (a slow LFO moves 1/16 step every 20 ms or more; one that started now: the grid)
CORE_COLD int grid_ramp(int bytes, int rounds, uint32_t own_us) {
  float wait_us = ((float)bytes - g_credit) * (1e6f / (float)kBytesPerS);
  uint32_t r = (wait_us > (float)kGridUs ? (uint32_t)wait_us : kGridUs) * (uint32_t)(rounds < 1 ? 1 : rounds);
  return blocks(own_us > r ? own_us : r);
}
// an offset record's pass at the grid (0 none): 1 a release (back to 0: before the changes, LFO FINAL), 2 a change, 3 a refresh
CORE_COLD int off_pass(const E& e, uint32_t ms) {
  int v = want_off(e);
  if (e.off_sent == kNone) return v != 0 ? 2 : 0;
  if (v == 0 && e.off_sent != 0 && e.rel) return 1;
  int d = v - e.off_sent;
  if (v == 0 && e.off_sent != 0) return 2;
  if (d >= kOffMinMove || d <= -kOffMinMove) return 2;
  return e.off_sent != 0 && ms - e.off_ms >= kRefreshMs ? 3 : 0;
}
// an override record's grid pass (0 none; not a due p-lock's): 1 a release (mix to 0), 2 a change, 3 a refresh
CORE_COLD int ovr_pass(const E& e, uint32_t ms) {
  if (e.due) return 0;
  int mix = want_mix(e);
  if (mix != e.mix_sent) return mix == 0 ? (e.mix_sent > 0 ? 1 : 0) : 2;
  if (mix && e.ovr != e.ovr_sent) return 2;
  return e.mix_sent > 0 && ms - e.ovr_ms >= kRefreshMs ? 3 : 0;
}
// the records waiting at a pass at most `upto` (offsets or overrides): the releases (1), the changes (2)
template <class Pass>
CORE_COLD int waiting(Pass pass, int upto) {
  int n = 0;
  for (const E& e : g_e) {
    if (e.t == 0xFF) continue;
    int p = pass(e);
    n += p && p <= upto;
  }
  return n;
}
// rel_only: the release cut's message (past a spent credit): the releases alone, never a change or a refresh
CORE_COLD bool send_offsets(uint32_t us, uint32_t ms, bool rel_only) {
  E* r[kRpMaxOffsets];
  int changed = 0;
  int n = choose(r, kRpMaxOffsets, [ms, rel_only](const E& e) {
    int p = off_pass(e, ms);
    return rel_only && p != 1 ? 0 : p;
  }, &E::off_ms, 3, &changed);
  if (!n) return false;
  uint32_t own_us = 0;   // the changed records' own last update interval (the shortest; one idle past the refresh: none)
  bool any = false;
  for (int i = 0; i < n; ++i) {
    const E& e = *r[i];
    int v = want_off(e);   // (a release, back to 0: at the grid's pace, never stretched)
    if (e.off_sent == kNone || v == e.off_sent || v == 0 || !e.chg_gap || ms - e.chg_ms >= kRefreshMs) continue;
    uint32_t d = (ms - e.chg_ms < e.chg_gap ? ms - e.chg_ms : e.chg_gap) * 1000u;   // (a start after a pause: no ramp)
    if (!any || d < own_us) own_us = d, any = true;
  }
  int rounds = (changed + kRpMaxOffsets - 1) / kRpMaxOffsets;
  uint8_t b[4 + 4 * kRpMaxOffsets] = {kOpOffset, (uint8_t)g_kit, (uint8_t)grid_ramp(11 + 4 * n, rounds, own_us), (uint8_t)n};
  for (int i = 0; i < n; ++i) {
    int v = want_off(*r[i]);
    uint16_t u = (uint16_t)(v < 0 ? v + 0x4000 : v);   // 14-bit two's complement (v -8192..8191)
    b[4 + 4 * i] = r[i]->t, b[5 + 4 * i] = r[i]->w, b[6 + 4 * i] = (uint8_t)(u >> 7), b[7 + 4 * i] = (uint8_t)(u & 0x7F);
  }
  g_off_us = us;   // (a bad message: tried again at the grid's pace, not every call)
  if (!send(b, 4 + 4 * n)) return false;
  for (int i = 0; i < n; ++i) {
    int v = want_off(*r[i]);
    if (v != r[i]->off_sent) {
      E& e = *r[i];
      e.chg_gap = e.off_sent != kNone && ms - e.chg_ms < kRefreshMs ? ms - e.chg_ms : 0;
      e.chg_ms = ms;
    }
    r[i]->off_sent = (int16_t)v, r[i]->off_ms = ms;
  }
  g_st.offsets += (uint32_t)n;
  for (int i = 0; i < n; ++i) maybe_free(*r[i]);
  return true;
}
// overrides: due (the p-locks whose time came: jump) or the grid's (changed, a refresh)
// the grid's passes: 1 a release (mix to 0: never left on the Rytm), 2 a change, 3 a refresh
CORE_COLD bool send_overrides(uint32_t us, uint32_t ms, bool due_only, bool rel_only = false) {
  E* r[kRpMaxOverrides];
  int changed = 0;
  int n = choose(r, kRpMaxOverrides, [us, ms, due_only, rel_only](const E& e) {
    if (e.due) return due_only && (!e.due_us || (int32_t)(us - e.due_us) >= 0) ? 1 : 0;   // (0: at once)
    if (due_only) return 0;
    int p = ovr_pass(e, ms);
    return rel_only && p != 1 ? 0 : p;
  }, &E::ovr_ms, 3, &changed);
  if (!n) return false;
  int rounds = (changed + kRpMaxOverrides - 1) / kRpMaxOverrides;
  uint8_t b[4 + 5 * kRpMaxOverrides] = {kOpOverride, (uint8_t)g_kit, (uint8_t)(due_only ? 0 : grid_ramp(11 + 5 * n, rounds, 0)),
                                        (uint8_t)n};
  for (int i = 0; i < n; ++i) {
    E& e = *r[i];
    int mix = want_mix(e);
    b[4 + 5 * i] = e.t, b[5 + 5 * i] = (uint8_t)(e.w | (due_only ? 0x40 : 0));
    b[6 + 5 * i] = (uint8_t)(e.ovr >> 7 & 0x7F), b[7 + 5 * i] = (uint8_t)(e.ovr & 0x7F), b[8 + 5 * i] = (uint8_t)mix;
  }
  if (!due_only) g_ovr_us = us;
  if (!send(b, 4 + 5 * n)) return false;
  for (int i = 0; i < n; ++i) {
    E& e = *r[i];
    e.ovr_sent = (int16_t)e.ovr, e.mix_sent = (int16_t)want_mix(e), e.ovr_ms = ms, e.due = false;
  }
  g_st.overrides += (uint32_t)n;
  for (int i = 0; i < n; ++i) maybe_free(*r[i]);
  return true;
}

CORE_COLD const char* op_name(int op) {
  return op == kOpOffset ? "OFFSET" : op == kOpOverride ? "OVERRIDE" : op == kOpClear ? "CLEAR" : "STATUS";
}

// the machine table (rytm-param-mod M0d [measured, ROM 1.73]): the kit's machine byte (the 33's) is the index m 0..52;
// words 9..16 of machine m are the records of the table at 0x413db5fc + 4 x (8m + w - 9), labelled as the Rytm's synth
// page labels them. "" an empty slot ("--"); 27 DISABLE, 34 BT IMPROVED, 43 FUNC. GEN. have no synth page. The "2"
// variants (35..42) have their own order (TUN before LEV)
constexpr int kMachines = 53;
const char kMl[kMachines][8][5] = {
    {"LEV", "TUN", "DEC", "HLD", "SWT", "SNP", "WAV", "TIC"},   //  0 BD HARD
    {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "WAV", "TRA"},   //  1 BD CLASSIC
    {"LEV", "TUN", "DEC", "SWD", "TIC", "NOD", "NOL", "SWT"},   //  2 SD HARD
    {"LEV", "TUN", "DEC", "DET", "SNP", "NOD", "NOL", "BAL"},   //  3 SD CLASSIC
    {"LEV", "TUN", "DEC", "SWD", "TIC", "NOL", "SYM", "SWT"},   //  4 RS HARD
    {"LEV", "T1", "DEC", "BAL", "T2", "SYM", "NOL", "TIC"},   //  5 RS CLASSIC
    {"LEV", "TON", "NOD", "NUM", "RAT", "NOL", "RND", "CPD"},   //  6 CP CLASSIC
    {"LEV", "TUN", "DEC", "", "NOL", "SNP", "SWD", ""},   //  7 BT CLASSIC
    {"LEV", "TUN", "DEC", "SWD", "SWT", "NOD", "NOL", "TON"},   //  8 XT CLASSIC
    {"LEV", "TUN", "DEC", "COL", "", "", "", ""},   //  9 CH CLASSIC
    {"LEV", "TUN", "DEC", "COL", "", "", "", ""},   // 10 OH CLASSIC
    {"LEV", "TUN", "DEC", "COL", "TON", "", "", ""},   // 11 CY CLASSIC
    {"LEV", "TUN", "DEC", "DET", "PW1", "PW2", "", ""},   // 12 CB CLASSIC
    {"LEV", "TUN", "DEC", "FMA", "SWT", "FMS", "FMD", "FMT"},   // 13 BD FM
    {"LEV", "TUN", "DEC", "FMT", "FMD", "NOD", "NOL", "FMA"},   // 14 SD FM
    {"LEV", "LPF", "DEC", "HPF", "LPQ", "ATK", "SWT", "SWD"},   // 15 UT NOISE
    {"LEV", "ATK", "DEC", "HLD", "SHP", "REP", "NUM", "POL"},   // 16 UT IMPULSE
    {"LEV", "TUN", "DEC", "", "", "", "", ""},   // 17 CH METALLIC
    {"LEV", "TUN", "DEC", "", "", "", "", ""},   // 18 OH METALLIC
    {"LEV", "TUN", "DEC", "TON", "TRD", "", "", ""},   // 19 CY METALLIC
    {"LEV", "TUN", "DEC", "DET", "PW1", "PW2", "", ""},   // 20 CB METALLIC
    {"LEV", "TUN", "DEC", "TYP", "MOD", "SWT", "SWD", "TIC"},   // 21 BD PLASTIC
    {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "DUS", "CLK"},   // 22 BD SILKY
    {"LEV", "TUN", "BDY", "DEC", "BAL", "LPF", "HPF", "RES"},   // 23 SD NATURAL
    {"LEV", "TUN", "DEC", "TON", "TRD", "RST", "NOL", "NOD"},   // 24 HH BASIC
    {"LEV", "TUN", "DEC", "TYP", "HIT", "C1", "C2", "C3"},   // 25 CY RIDE
    {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "WAV", "TIC"},   // 26 BD SHARP
    {"", "", "", "", "", "", "", ""},   // 27 DISABLE
    {"LEV", "TUN", "DEC", "DET", "DEC", "BAL", "BND", "CFG"},   // 28 SY DUAL VCO
    {"LEV", "TUN", "DEC", "OF2", "OF3", "OF4", "WAV", "SPD"},   // 29 SY CHIP
    {"LEV", "TUN", "DEC", "HLD", "SWT", "SWD", "WAV", "IMP"},   // 30 BD ACOUSTIC
    {"LEV", "TUN", "BDY", "NOD", "NOL", "HLD", "SWD", "IMP"},   // 31 SD ACOUSTIC
    {"LEV", "TUNE", "DEC2", "DET", "NLEV", "WAV1", "WAV2", "BAL"},   // 32 SY RAW
    {"LEV", "OSC1", "DEC", "OSC2", "OSC3", "OSC4", "OSC5", "OSC6"},   // 33 HH LAB
    {"", "", "", "", "", "", "", ""},   // 34 BT IMPROVED
    {"TUN", "LEV", "HLD", "DEC", "SWT", "SNP", "WAV", "TIC"},   // 35 BD HARD 2
    {"TUN", "LEV", "HLD", "DEC", "SWT", "SWD", "WAV", "TRA"},   // 36 BD CLASSIC 2
    {"SWD", "DEC", "LEV", "TIC", "NOD", "NOL", "TUN", "SWT"},   // 37 SD HARD 2
    {"TUN", "BAL", "DET", "DEC", "NOD", "SNP", "NOL", "LEV"},   // 38 SD CLASSIC 2
    {"SWD", "DEC", "LEV", "TIC", "NOL", "SYM", "TUN", "SWT"},   // 39 RS HARD 2
    {"BAL", "DEC", "T1", "T2", "LEV", "TIC", "NOL", "SYM"},   // 40 RS CLASSIC 2
    {"TUN", "LEV", "FMA", "DEC", "SWT", "FMS", "FMD", "FMT"},   // 41 BD FM 2
    {"TUN", "FMA", "FMT", "DEC", "NOD", "FMD", "NOL", "LEV"},   // 42 RS FM 2
    {"", "", "", "", "", "", "", ""},   // 43 FUNC. GEN.
    {"TU1", "TU2", "LE1", "LE2", "WF1", "WF2", "FM", "RM"},   // 44 BD TEST
    {"TU1", "TU2", "LE1", "LE2", "TUN", "DEC", "WAV", "FRO"},   // 45 SD TEST
    {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"},   // 46 RS TEST
    {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"},   // 47 BT TEST
    {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"},   // 48 LT TEST
    {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"},   // 49 MT TEST
    {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"},   // 50 HH TEST
    {"TUN", "SEL", "50", "500", "80", "800", "8000", "VCA"},   // 51 CY TEST
    {"LEV", "TUN", "TUN", "DEC", "SNP", "NOL", "PLV", "PLN"},   // 52 CB TEST
};

}  // namespace

// ---------------------------------------------------------------------------------------------------- the sources

CORE_COLD void rp_offset(Tid t, float steps) {
  int tr, w;
  if (!rytm_word(t, &tr, &w)) return;
  if (override_only(t)) steps = 0;   // a list word takes no offset (the pickers do not offer it; a file might)
  E* e = steps != 0 ? get(tr, w) : find(tr, w);
  if (!e) return;
  e->off = steps;
  if (steps != 0) e->rel = false;
  maybe_free(*e);
}
CORE_COLD void rp_release(Tid t) {
  int tr, w;
  if (!rytm_word(t, &tr, &w)) return;
  E* e = find(tr, w);
  if (!e) return;
  e->off = 0, e->rel = true;
  maybe_free(*e);
}
CORE_COLD void rp_lock(int track, int word, int v, uint32_t due_us) {
  E* e = get(track, word);
  if (!e) return;
  v = v < 0 ? 0 : v > 16383 ? 16383 : v;
  if (e->lock && e->lock_v == v) return;   // held (a re-apply, a slide's tick): its jump is out or due already
  e->lock = true, e->lock_v = (uint16_t)v;
  compose(*e);
  e->due = true, e->due_us = due_us;
}
CORE_COLD void rp_unlock(int track, int word, uint32_t due_us) {
  E* e = find(track, word);
  if (!e || !e->lock) return;
  e->lock = false;
  compose(*e);
  if (e->mix_sent > 0 || e->morph) {
    e->due = true, e->due_us = due_us;
  } else {   // never sent: nothing to take back
    e->due = false;
    maybe_free(*e);
  }
}
CORE_COLD void rp_morph(Tid t, bool has_a, float a, bool has_b, float b, float f, bool step, bool upper) {
  int tr, w;
  if (!rytm_word(t, &tr, &w) || (w >= 48 && w <= 55)) return;   // (VM / AT: offsets only)
  E* e = get(tr, w);
  if (!e) return;
  e->morph = true, e->has_a = has_a, e->a = a, e->has_b = has_b, e->b = b, e->f = f, e->step = step, e->upper = upper;
  compose(*e);
}
CORE_COLD void rp_morph_off(Tid t) {
  int tr, w;
  if (!rytm_word(t, &tr, &w)) return;
  E* e = find(tr, w);
  if (!e || !e->morph) return;
  e->morph = false;
  compose(*e);
  maybe_free(*e);
}

// ---------------------------------------------------------------------------------------------------- the link

CORE_COLD void rp_hello(int caps) {
  g_caps = caps, g_link = true, g_clear = 0;
  forget_sent();   // the Rytm cleared every entry at its hello
}
CORE_COLD void rp_kit(int kit) {
  if (kit < 0 || kit > 127 || kit == g_kit) return;
  bool had = g_kit >= 0;
  g_kit = kit;
  if (!had) return;
  g_clear = 0x1FFF;   // the Rytm frees its entries at a kit change itself; the 06 under the new kit says so too
  forget_sent();
  // the sources release (the XY pad, the scenes) or reload (the LFOs) for the new kit, and the layer applies that at its
  // next end_tick: until then no grid send (else the old kit's offsets would go out once under the new kit's byte)
  g_hold = true, g_hold_us = g_last_us;
}
CORE_COLD void rp_project() {
  g_clear = 0x1FFF;
  forget_sent();
  g_hold = true, g_hold_us = g_last_us;
}
CORE_COLD void rp_layer_done() { g_hold = false; }
CORE_COLD void rp_link_lost() {
  g_link = false, g_clear = 0;
  forget_sent();
}
CORE_COLD void rp_clear_all() {
  for (E& e : g_e)
    if (e.t != 0xFF) e = E();
  g_clear = 0x1FFF;
}
CORE_COLD void rp_ack(int op, int status) {
  if (!status) return;
  if (status == 2) ++g_st.full;
  else ++g_st.refused;
  uint32_t ms = now_ms();
  if (!g_said_any || ms - g_said_ms >= 1000) {
    g_said_any = true, g_said_ms = ms;
    log(CORE_STR("rytm params: the Rytm %s op %d %s (status %d)"), status == 2 ? "has no slot left for" : "refused", op,
        op_name(op), status);
  }
}
CORE_COLD void rp_status(const uint8_t* b, int n) {
  g_status_n = n < 0 ? 0 : n > 7 ? 7 : n;
  if (g_status_n) memcpy(g_status, b, (size_t)g_status_n);
}

CORE_COLD void rp_tick(uint32_t us) {
  if (!g_started) g_started = true, g_last_us = us, g_off_us = g_ovr_us = us - 2 * kGridUs, g_now_ms = us / 1000u, g_ms_rem = us % 1000u;
  uint32_t dus = us - g_last_us;
  g_last_us = us;
  g_ms_rem += dus % 1000u, g_now_ms += dus / 1000u + g_ms_rem / 1000u, g_ms_rem %= 1000u;
  g_credit += (float)dus * ((float)kBytesPerS * 1e-6f);
  if (g_credit > (float)kBytesPerS / 10.0f) g_credit = (float)kBytesPerS / 10.0f;
  uint32_t ms = g_now_ms;
  if (ms - g_win_ms >= 1000) g_bps = g_win_bytes, g_win_bytes = 0, g_win_ms = ms;
  if (!ready()) return;
  send_clear();
  if (g_hold && us - g_hold_us < 4 * kGridUs) return;   // (a build without the layer's tick: 20 ms at most)
  g_hold = false;
  // the p-locks due now: a jump each at the step's time, outside the grid and its credit check (their bytes count:
  // the debt, floored at 100 ms of the cap, holds the grid's next sends; the average stays under kBytesPerS)
  send_overrides(us, ms, true);
  // the grid: one 04 and one 05 at most every kGridUs while the credit lasts; when both are due they take turns going
  // first, so the offsets never starve the overrides (a release among them) or the other way round
  bool off_due = us - g_off_us >= kGridUs, ovr_due = us - g_ovr_us >= kGridUs;
  const bool off_slot = off_due;
  // LFO FINAL: a release waits for no credit and no turn - an override's mix to 0 first, then an offset back to 0 (each
  // at its own op's grid slot: at most one 04 and one 05 every kGridUs still; the bytes a debt, as a p-lock's)
  if (ovr_due && g_credit < 0 && waiting([ms](const E& e) { return ovr_pass(e, ms); }, 1) && send_overrides(us, ms, false, true))
    ovr_due = false, g_ovr_first = false, ++g_st.rel_cut;
  if (off_due && g_credit < 0 && waiting([ms](const E& e) { return off_pass(e, ms); }, 1) && send_offsets(us, ms, true))
    off_due = false, g_ovr_first = true, ++g_st.rel_cut;
  const bool ovr_first = g_ovr_first;   // (this call's order: the flag flips for the next call, not within this one)
  for (int i = 0; i < 2 && g_credit >= 0; ++i) {
    bool ovr = (i == 0) == ovr_first;
    if (ovr ? ovr_due && send_overrides(us, ms, false) : off_due && send_offsets(us, ms, false)) g_ovr_first = !ovr;
  }
  // saturated (rp_saturated): at the offsets' grid slot, changed records still waiting (the credit spent, or more than
  // one message holds) - kSatMs on end
  if (off_slot) {
    if (waiting([ms](const E& e) { return off_pass(e, ms); }, 2)) {
      if (!g_bl) g_bl = true, g_bl_ms = ms;
      if (ms - g_bl_ms >= kSatMs) {
        if (g_sat && ms - g_sat_ms < kSatHoldMs) g_st.sat_ms += ms - g_sat_ms;
        g_sat = true, g_sat_ms = ms;
      }
    } else {
      g_bl = false;
    }
  }
}

// ---------------------------------------------------------------------------------------------------- availability

CORE_COLD uint32_t rp_lead_us() { return g_lead_us; }
CORE_COLD bool rp_on() { return g_link && (g_caps & kCapsParamMod); }
CORE_COLD bool rp_saturated() { return ready() && g_sat && g_now_ms - g_sat_ms < kSatHoldMs; }
CORE_COLD const char* rp_machine_label(int machine, int word) {
  if (word < 9 || word > 16 || machine < 0 || machine >= kMachines) return nullptr;
  return kMl[machine][word - 9];
}
CORE_COLD bool rp_word_active(int track, int word) {
  if (!rp_on() || track < 0 || track > 12) return false;
  if (track == 12) return true;   // (M0b [measured]: the compressor reads its words 30..37 from the effective array)
  if (word >= 9 && word <= 24) {
    int vb = ui::remote_track_machine(track);
    if (vb >= 0 && vb != 0x7F) return false;   // a Sidecar machine's track: its P1..P16 (the V targets do that job)
    if (word <= 16) {
      const char* l = rp_machine_label(ui::remote_track_rytm_machine(track), word);
      if (l && !l[0]) return false;   // the machine's slot is empty
    }
  }
  return true;
}
CORE_COLD bool rp_active(Tid t) {
  int tr, w;
  return rytm_word(t, &tr, &w) && rp_word_active(tr, w);
}
CORE_COLD const char* rp_label(Tid t, char* b, int cap) {
  if (cap <= 0) return b;
  b[0] = 0;
  int tr, w;
  if (!rytm_word(t, &tr, &w)) return b;
  if (tr < 12 && w >= 9 && w <= 16) {
    const char* l = rp_machine_label(ui::remote_track_rytm_machine(tr), w);
    if (l && l[0]) {
      sniprintf(b, cap, "%s", l);
      return b;
    }
  }
  char nm[16];
  name(t, nm, sizeof nm);
  const char* c = strchr(nm, ':');
  sniprintf(b, cap, "%s", c ? c + 1 : nm);
  return b;
}

// ---------------------------------------------------------------------------------------------------- console

CORE_COLD bool rp_command(const char* p, char* o, int cap) {
  if (strncmp(p, "rp", 2) != 0 || (p[2] && p[2] != '=')) return false;
  if (cap <= 0) return true;
  o[0] = 0;
  if (p[2] == '=') {
    const char* a = p + 3;
    if (!strcmp(a, "status")) {
      if (ready()) {
        const uint8_t b[1] = {kOpStatus};
        send(b, 1);
      }
      sniprintf(o, cap, "rp: status %s", ready() ? "asked (2D 01 11)" : "not asked: the Rytm has no param modulation");
      return true;
    }
    if (!strcmp(a, "clear")) {   // every entry forgotten and 06 every track: a source sends again when it moves
      rp_clear_all();
      sniprintf(o, cap, "rp: CLEAR every track, every entry forgotten");
      return true;
    }
    if (!strncmp(a, "lead:", 5)) {
      int v = 0;
      for (const char* q = a + 5; *q >= '0' && *q <= '9'; ++q) v = v * 10 + (*q - '0');
      if (v > 50000) v = 50000;
      g_lead_us = (uint32_t)v;
      sniprintf(o, cap, "rp: p-lock lead %u us", (unsigned)g_lead_us);
      return true;
    }
    if (strcmp(a, "list") != 0) {
      sniprintf(o, cap, "rp | rp=list | rp=status | rp=clear (all forgotten: a source resends when it moves) | rp=lead:US");
      return true;
    }
    int n = sniprintf(o, cap, "rp: %d live", rp_live());
    for (const E& e : g_e) {
      if (e.t == 0xFF || n >= cap - 1) continue;
      char nm[16];
      name(rytm_tid(e.t, e.w), nm, sizeof nm);
      int v = want_off(e);
      int k = sniprintf(o + n, cap - n, ", %s off %s%d/64 ovr %u mix %d%s%s", nm, v < 0 ? "-" : "+", v < 0 ? -v : v,
                        (unsigned)e.ovr, want_mix(e), e.lock ? " lock" : "", active(e) ? "" : " (inactive)");
      if (k < 0 || k >= cap - n) break;
      n += k;
    }
    return true;
  }
  int n = sniprintf(o, cap,
                    "rp: %s%s, kit %d, %d live, %u B/s, sent %u offsets %u overrides %u clears in %u msgs, refused %u, "
                    "full %u, dropped %u, bad %u, lead %u us",
                    !g_link ? "no link" : !(g_caps & kCapsParamMod) ? "the Rytm has no param modulation (needs Rytm mod 0.5)" : "on",
                    rp_saturated() ? " (SATURATED: the LFOs update slower)" : "", g_kit < 0 ? -1 : g_kit + 1, rp_live(), (unsigned)g_bps, (unsigned)g_st.offsets, (unsigned)g_st.overrides,
                    (unsigned)g_st.clears, (unsigned)g_st.msgs, (unsigned)g_st.refused, (unsigned)g_st.full,
                    (unsigned)g_st.dropped, (unsigned)g_st.bad, (unsigned)g_lead_us);
  if (g_status_n >= 7 && n < cap - 1)
    sniprintf(o + n, cap - n, "; Rytm: %d slots, ring drops %d, kit drops %d, timeouts %d, flags %d", g_status[0],
              g_status[1] << 7 | g_status[2], g_status[3] << 7 | g_status[4], g_status[5], g_status[6]);
  return true;
}
CORE_COLD int rp_json(char* o, int cap) {
  int n = sniprintf(o, cap, "\"rp\":{\"on\":%d,\"live\":%d,\"bps\":%u,\"refused\":%u,\"full\":%u,\"bad\":%u,\"sent\":%u,\"sat\":%d}",
                    rp_on() ? 1 : 0, rp_live(), (unsigned)g_bps, (unsigned)g_st.refused, (unsigned)g_st.full,
                    (unsigned)g_st.bad, (unsigned)g_st.msgs, rp_saturated() ? 1 : 0);
  return n < 0 ? 0 : n;
}
CORE_COLD const RpStats& rp_stats() { return g_st; }
CORE_COLD bool rp_view(int track, int word, RpView* out) {
  const E* e = find(track, word);
  if (!e) return false;
  if (out) *out = {e->off_sent, e->ovr, (uint8_t)want_mix(*e), e->lock, e->morph};
  return true;
}
CORE_COLD int rp_live() {
  int n = 0;
  for (const E& e : g_e) n += e.t != 0xFF;
  return n;
}
CORE_COLD void rp_reset() {
  for (E& e : g_e) e = E();
  g_caps = 0, g_link = false, g_kit = -1, g_clear = 0;
  g_started = false, g_credit = 0, g_lead_us = kRpLeadUs, g_ovr_first = false, g_hold = false;
  g_win_ms = g_win_bytes = g_bps = 0, g_said_any = false, g_status_n = 0;
  g_bl = g_sat = false, g_bl_ms = g_sat_ms = g_now_ms = g_ms_rem = 0;
  g_st = RpStats();
}

}  // namespace mod
}  // namespace core
#endif
