// core/lfo.cpp - see lfo.h. Control side only, cold (program flash on the Teensy): a tick is a few dozen float ops
// an LFO, once a millisecond at most.
#include "lfo.h"

#include <math.h>
#include <string.h>

#include "fmt/fmt.h"
#include "hooks.h"
#include "mod/mod.h"
#include "platform.h"
#include "pool/pool.h"
#if CORE_MSAMP
#include "bundle/rename.h"
#include "store/store.h"
#endif
#include "voice_name.h"
#include "voices.h"
#if RYTM_SENDFX_LARGE
#include "ui/remote.h"   // remote_lfo_track: the JSON's "trk" (the web follows the Rytm's selected track)
#endif
#if CORE_POLY
#include "fx/insert.h"
#include "fx/out.h"
#include "mod/rytm_param.h"
#include "device/device.h"
#include "ui/remote.h"
#endif

namespace core {
namespace ui {
const char* voice_label(int vi, int k, char* b, int cap);   // page_lfo.cpp: voice vi's P(k+1) as the Rytm labels it, "" none
}
namespace lfo {

#ifdef PROGMEM
#define UI_ROM_LFO PROGMEM   // the Teensy: tables in program flash
#else
#define UI_ROM_LFO
#endif

namespace {

// ---- tables
UI_ROM_LFO const char kTargetNames[kTargets][5] = {"OFF", "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "FX1", "FX2"
#if CORE_POLY
                                                  , "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8", "B1", "B2", "B3",
                                                  "O1", "O2", "O3", "O4", "O5", "O6", "O7", "O8",
                                                  "M1", "M2", "MFX", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8",
                                                  "S9", "S10", "S11", "S12", "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8",
                                                  "RT1", "RT2", "RT3", "RT4", "RT5", "RT6", "RT7", "RT8", "RT9", "RT10",
                                                  "RT11", "RT12", "RFX"
#endif
};
#if CORE_POLY
// 0.5.0: the Rytm words an RT / RFX LFO may move (rytm-param-mod 3.1, 3.3: offsets; no list word, no LFO DST), in the
// page's order: LFO, SYNTH P1..P8, SAMPLE, FILTER, AMP, the velocity routes, VEL
UI_ROM_LFO const uint8_t kRtWords[] = {5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, 24, 25, 26, 27, 28,
                                       29, 30, 31, 33, 34, 35, 36, 37, 38, 39, 40, 41, 48, 49, 50, 51, 52, 53, 54, 55, 56};
// (M0b: the compressor's THR MUP MIX CVOL appended: an older file's index is unchanged; the file keeps the word anyway)
UI_ROM_LFO const uint8_t kRfxWords[] = {5,  6,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 19,
                                        20, 21, 22, 23, 24, 25, 27, 28, 30, 35, 36, 37};
constexpr int kRtN = (int)sizeof kRtWords, kRfxN = (int)sizeof kRfxWords;
#endif
UI_ROM_LFO const char kWaveNames[kWaves][7] = {"SINE", "TRI", "SAW+", "SAW-", "SQR", "S&H", "RND", "DESIGN"};
UI_ROM_LFO const char kRetrigNames[kRetrigs][5] = {"FREE", "TRIG", "PLAY"};
UI_ROM_LFO const char kShapeNames[kShapes][5] = {"HOLD", "RAMP", "SMTH"};
UI_ROM_LFO const char kPresetNames[kPresets][6] = {"RAMP", "TRI", "STEPS", "RND"};
// synced rates: the cycle in 24 ppqn ticks, shortest first; all divide kWrap
UI_ROM_LFO const uint16_t kDivTicks[kDivs] = {3, 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 144, 192, 384, 768};
UI_ROM_LFO const char kDivNames[kDivs][6] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8",  "1/4T",
                                             "1/8D", "1/4",   "1/2T", "1/4D", "1/2",   "1BART", "1/2D",
                                             "1BAR", "1BARD", "2BAR", "4BAR", "8BAR"};
constexpr float kWrap = 2304.0f;   // 2^8 x 9 ticks: every synced cycle divides it

// ---- state
Settings g_set[kLfos];
struct Run {
  float ph = 0, anchor = 0, a = 0, b = 0, w = 0;
  uint32_t rng = 1;
  bool retrig = false, fresh = true;
};
Run g_run[kLfos];

#if !CORE_MOD_LFO
// CORE_MOD_LFO 0 (the rollback, removed in Phase 2): the targets' table of 0.4.0, the LFOs writing their targets
// themselves. Else core/mod owns the targets (spec-050 1.1): the LFOs hand it one offset each a tick (waves()).
struct Tgt {
  uint8_t target = 0, param = 0;
  uint8_t state = 0;   // 0 free, 1 live
  bool live_now = false;
  float base = 0, off = 0, applied = 0;
};
constexpr int kMaxT = 2 * kLfos;
Tgt g_t[kMaxT];
uint32_t g_last_us = 0;
bool g_started = false;
uint8_t g_vdirty = 0;
uint32_t g_user_ms[kVoices];
#endif

struct Clock {
  bool run = false, ticking = false;
  int32_t pos = -1;           // the latest tick's song position (24 ppqn) since Start, -1 none yet
  uint32_t last_us = 0;       // when the latest tick came
  float tick_us = 20833.3f;   // the tempo: 120 BPM until measured
  float tp = 0;               // the clock position now, in ticks mod kWrap
  bool start = false;         // a Start since the last tick (PLAY retrigs)
  uint32_t starts = 0, stops = 0, jumps = 0;   // FA / FB, FC, F2 since boot (clock_state: API 3's transport flags)
};
Clock g_clk;

bool g_dirty = false;
uint32_t g_dirty_ms = 0;
Stats g_stats;

CORE_COLD float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
// no libm here: on the Teensy its functions land in ITCM (fast RAM, full); these are a few multiplies
CORE_COLD float flo(float x) { int i = (int)x; return (float)(i - (x < (float)i ? 1 : 0)); }
CORE_COLD float wrap1(float x) { return x - flo(x); }                     // into 0..1
CORE_COLD float modp(float x, float d) { return x - d * flo(x / d); }     // into 0..d
CORE_COLD int rnd0(float x) { return (int)(x + 0.5f); }                    // x >= 0
CORE_COLD float smooth(float f) { return f * f * (3.0f - 2.0f * f); }
CORE_COLD float sin2pi(float ph) {   // sin(2 pi ph), error < 0.001
  float x = 2.0f * wrap1(ph) - 1.0f;   // sin(2 pi ph) = -sin(pi x)
  float y = 4.0f * x * (1.0f - fabsf(x));
  return -(0.225f * (y * fabsf(y) - y) + y);
}
CORE_COLD float exp2p(float x) {   // 2^x for 0 <= x < 16, error < 0.02 %
  int n = (int)x;
  float f = x - (float)n;
  return (float)(1u << n) * (1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f))));
}
CORE_COLD uint32_t xorshift(uint32_t& s) {
  s ^= s << 13, s ^= s >> 17, s ^= s << 5;
  return s;
}
CORE_COLD float rnd(uint32_t& s) { return (float)(xorshift(s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

CORE_COLD bool is_voice(int t) { return t >= kTVoice && t < kTVoice + kVoices; }
CORE_COLD bool is_fx(int t) { return t == kTFx || t == kTFx + 1; }
// MFX's params: LEV, the master's 4 slots' MIX, the 6 bus slots' (core/fx/master.h slot_index + 1 = core/mod's kMfx param)
constexpr int kMfxMaster = 4, kMfxParams = 11;
#if CORE_POLY
CORE_COLD bool is_ins(int t) { return t >= kTIns && t < kTOut; }   // fx-ins: never true without CORE_POLY
CORE_COLD bool is_out(int t) { return t >= kTOut && t < kTMaster; }   // the OUT route's LEV PAN WID (core/fx/out.h)
CORE_COLD bool is_mchain(int t) { return t == kTMaster || t == kTMaster + 1; }   // 0.5.0: M1 M2 (insert chains 11, 12)
CORE_COLD bool is_mfx(int t) { return t == kTMfx; }                               // LEV, MIX1..4, B1A..B3B
CORE_COLD bool is_snd(int t) { return t >= kTSnd && t < kTSnd + 12; }             // a Rytm track's FX1 / FX2 send
CORE_COLD bool is_isnd(int t) { return t >= kTIsnd && t < kTIsnd + 8; }           // a voice's sends to the buses
CORE_COLD bool is_rt(int t) { return t >= kTRt && t <= kTRfx; }                    // 0.5.0: the Rytm's own params
// an RT / RFX LFO's param (its list's index) <-> the Rytm word (-1: none)
CORE_COLD int rt_word(int t, int p) {
  if (!is_rt(t) || p < 0) return -1;
  return t == kTRfx ? (p < kRfxN ? kRfxWords[p] : -1) : (p < kRtN ? kRtWords[p] : -1);
}
CORE_COLD int rt_param(int t, int w) {
  for (int p = 0, n = t == kTRfx ? kRfxN : kRtN; is_rt(t) && p < n; ++p)
    if (rt_word(t, p) == w) return p;
  return -1;
}
#else
CORE_COLD bool is_ins(int t) { return t >= kTIns && t < kTargets; }
CORE_COLD bool is_out(int) { return false; }
CORE_COLD bool is_mchain(int) { return false; }
CORE_COLD bool is_mfx(int) { return false; }
CORE_COLD bool is_snd(int) { return false; }
CORE_COLD bool is_isnd(int) { return false; }
CORE_COLD bool is_rt(int) { return false; }
CORE_COLD int rt_word(int, int) { return -1; }
CORE_COLD int rt_param(int, int) { return -1; }
#endif

CORE_COLD void changed() { g_dirty = true, g_dirty_ms = now_ms(); }

#if !CORE_MOD_LFO
CORE_COLD Tgt* find(int target, int param) {
  for (Tgt& e : g_t)
    if (e.state && e.target == target && e.param == param) return &e;
  return nullptr;
}

CORE_COLD float current(int target, int param) {   // a new target's base: what plays now
  if (is_voice(target)) return v[target - kTVoice].p[param & 15];
  if (is_ins(target) || is_out(target)) return 0;   // an insert's / OUT's LFO is an offset (mod_offset), its base the knob
  return fx::control.knob[target - kTFx][2 + (param & 3)];
}
#endif

CORE_COLD void retrig(int i) {
  Run& r = g_run[i];
  if (g_set[i].sync) r.anchor = g_clk.tp;
  else r.ph = 0;
  r.fresh = true;
}

CORE_COLD float shaped(const Settings& s, float ph, Run& r, bool cycle) {
  float w;
  switch (s.wave) {
    case kSine: w = sin2pi(ph); break;
    case kTri: w = ph < 0.25f ? 4 * ph : ph < 0.75f ? 2 - 4 * ph : 4 * ph - 4; break;
    case kSawUp: w = 2 * ph - 1; break;
    case kSawDown: w = 1 - 2 * ph; break;
    case kSquare: w = ph < 0.5f ? 1.0f : -1.0f; break;
    case kSampleHold:
      if (cycle) r.a = rnd(r.rng);
      w = r.a;
      break;
    case kRandom:
      if (cycle) r.a = r.b, r.b = rnd(r.rng);
      w = r.a + (r.b - r.a) * smooth(ph);
      break;
    default: w = design_at(s, ph); break;
  }
  return w;
}

CORE_COLD void sanitize(Settings& s) {
  if (s.target >= kTargets) s.target = 0;   // (a Rytm target: set / from_line refuse it first, target_ok)
  int np = params_of(s.target);
  if (np && s.param >= np) s.param = (uint8_t)(np - 1);
  if (s.wave >= kWaves) s.wave = kSine;
  s.sync = s.sync ? 1 : 0;
  if (s.sync && s.rate >= kDivs) s.rate = kDivs - 1;
  if (s.rate >= kFreeRates) s.rate = kFreeRates - 1;
  if (s.retrig >= kRetrigs) s.retrig = kFree;
  if (s.depth < -64) s.depth = -64;
  if (s.base > 127) s.base = 127;
  for (int k = 0; k < kSteps; ++k) {
    if (s.step[k] < -64) s.step[k] = -64;
    if (s.shape[k] >= kShapes) s.shape[k] = kHold;
  }
}

// a decimal integer at p (an optional sign), *end after it (p when none): newlib's strtol / atoi sit in the Teensy's ITCM
CORE_COLD long num(const char* p, const char** end = nullptr) {
  const char* q = p;
  bool neg = *q == '-';
  if (*q == '-' || *q == '+') ++q;
  const char* d = q;
  long v = 0;
  while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
  if (end) *end = q == d ? p : q;
  return neg ? -v : v;
}

CORE_COLD int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
CORE_COLD void design_hex(const Settings& s, char* b) {   // 48 chars + NUL
  static UI_ROM_LFO const char kH[] = "0123456789abcdef";
  for (int k = 0; k < kSteps; ++k) {
    int v = s.step[k] + 64;
    b[3 * k] = kH[v >> 4], b[3 * k + 1] = kH[v & 15], b[3 * k + 2] = kH[s.shape[k] & 15];
  }
  b[3 * kSteps] = 0;
}
CORE_COLD bool design_from_hex(Settings& s, const char* h) {
  if (strlen(h) < 3 * kSteps) return false;
  for (int k = 0; k < 3 * kSteps; ++k)
    if (hexv(h[k]) < 0) return false;
  for (int k = 0; k < kSteps; ++k) {
    int v = hexv(h[3 * k]) << 4 | hexv(h[3 * k + 1]);
    s.step[k] = (int8_t)(v > 127 ? 63 : v - 64);
    s.shape[k] = (uint8_t)(hexv(h[3 * k + 2]) % kShapes);
  }
  return true;
}

// The settings' file: "/.rytm/lfo/<project>/kNNN.txt" for kit NNN (1..128) of the project they are of (g_proj, g_kit),
// "/.rytm/lfo/<project>.txt" (the project's own, from before 1026: every kit's default) while the Rytm has not named
// its kit (kit < 0), with pool_file's safe name.
char g_proj[24] = "";
int g_kit = -1;        // the kit the settings are of (0..127), -1 unknown
int g_rytm_kit = -1;   // the Rytm's active kit as it last said (kit_changed)
CORE_COLD void name_of(const char* proj, char* fn, int cap, int kit) {
  char n[kKeyMax];
  project_key(proj, n, sizeof n);
  if (kit < 0) sniprintf(fn, cap, CORE_STR("/.rytm/lfo/%s.txt"), n);
  else if (kit < 128) sniprintf(fn, cap, CORE_STR("/.rytm/lfo/%s/k%03d.txt"), n, kit + 1);
  else sniprintf(fn, cap, CORE_STR("/.rytm/lfo/%s"), n);   // kit 128: the project's folder
}
CORE_COLD void file_name(char* fn, int cap, int kit) { name_of(g_proj, fn, cap, kit); }

// one LFO as the file's line: NAME TARGET PARAM WAVE RATE SYNC RETRIG DEPTH BASE DESIGN; NAME the LFO's (lfo_name: "T3A",
// "FXB" on the Pi from 0.5.0's 26 - an older build reads no such line, so it never writes over the file; the Mini's "N")
CORE_COLD int to_line(int i, char* b, int cap) {
  const Settings& s = g_set[i];
  char h[3 * kSteps + 1], nm[6];
  design_hex(s, h);
  int pw = is_rt(s.target) ? rt_word(s.target, s.param) : s.param;   // RT / RFX: the Rytm's word, not the list's index
  return sniprintf(b, cap, CORE_STR("%s %s %d %d %d %d %d %d %d %s\n"), lfo_name(i, nm, sizeof nm), target_name(s.target),
                   pw, s.wave, s.rate, s.sync, s.retrig, s.depth, s.base, h);
}
// A line's LFO: its name (T3A, FXB), or a number - the Mini's LFO N, or on the Pi a file from before 0.5.0's 26 (8 LFOs,
// numbered): old LFO N becomes track N's A, its target kept (LFO FINAL: the owner's migration). -1: not one of ours
CORE_COLD int line_lfo(const char* p) {
  if (*p >= '0' && *p <= '9') {
    int n = (int)num(p);
    if (!kPerTrack) return n >= 1 && n <= kLfos ? n - 1 : -1;
    return n >= 1 && n <= kTracks ? lfo_of(n - 1, 0) : -1;
  }
  const char* e;
  int i = lfo_index(p, &e);
  return i >= 0 && (*e == ' ' || !*e) ? i : -1;
}
CORE_COLD bool from_line(const char* b) {
  char tn[8] = {0}, h[3 * kSteps + 2] = {0};
  int ti = 0;
  const char* p = b;
  const int li = line_lfo(p);
  if (li < 0) return false;
  while (*p && *p != ' ') ++p;
  while (*p == ' ') ++p;
  while (*p && *p != ' ' && ti < 7) tn[ti++] = *p++;
  int t = target_from_name(tn);
  if (t < 0) return false;
  if (!target_ok(li, t)) {   // an LFO on a Rytm target: it loads OFF (the line is read, the file not locked; the next save drops it)
    if (!is_rt(t)) return false;
    defaults(&g_set[li]);
    log(CORE_STR("lfo: line %.4s: an LFO on the Rytm is not allowed, loaded OFF"), b);
    return true;
  }
  long f[7];
  for (int k = 0; k < 7; ++k) {
    while (*p == ' ') ++p;
    if (!*p) return false;
    const char* e;
    f[k] = num(p, &e);
    if (e == p) return false;
    p = e;
  }
  while (*p == ' ') ++p;
  strncpy(h, p, sizeof h - 1);
  Settings s;
  defaults(&s);
  if (is_rt(t)) {   // the Rytm's word (a word this build's list lacks: the line is not read, the file locks)
    int q = rt_param(t, (int)f[0]);
    if (q < 0) return false;
    f[0] = q;
  }
  s.target = (uint8_t)t, s.param = (uint8_t)f[0], s.wave = (uint8_t)f[1], s.rate = (uint8_t)f[2], s.sync = (uint8_t)f[3];
  s.retrig = (uint8_t)f[4], s.depth = (int8_t)(f[5] < -64 ? -64 : f[5] > 63 ? 63 : f[5]), s.base = (uint8_t)(f[6] & 127);
  design_from_hex(s, h);
  sanitize(s);
  g_set[li] = s;
  return true;
}

#if !CORE_MOD_LFO
// the targets the LFOs leave: back to their bases
CORE_COLD void restore(Tgt& e) {
  ++g_stats.restores;
  if (is_voice(e.target)) {
    int vi = e.target - kTVoice;
    v[vi].p[e.param & 15] = e.base;
    g_vdirty |= (uint8_t)(1u << vi);
  } else if (is_fx(e.target)) {
    fx::control.knob[e.target - kTFx][2 + (e.param & 3)] = (uint8_t)e.base;
  }
#if CORE_POLY
  else if (is_ins(e.target) && e.param >= 8) ins::eq_mod_offset(e.target - kTIns, (e.param - 8) & 3, 0);
  else if (is_ins(e.target)) ins::mod_offset(e.target - kTIns, (e.param >> 2) & 1, e.param & 3, 0);
  else if (is_out(e.target)) out::mod_offset(e.target - kTOut, e.param % 3, 0);
#endif
  e.state = 0;
}
#endif

}  // namespace

// the send FX's hooks into the LFOs (fx/send_fx.h), set before main
static struct FxHooks {
  FxHooks() { fx::knob_filter = fx_knob, fx::clock_listener = clock; }
} g_fx_hooks;

// ---------------------------------------------------------------------------------------------------- settings

CORE_COLD void defaults(Settings* s) {
  memset(s, 0, sizeof *s);
  s->rate = 64;   // ~0.6 Hz
  s->depth = 32;
  s->base = 64;
  preset(s, 1);   // a triangle in the design
}

CORE_COLD const char* preset_name(int w) { return w >= 0 && w < kPresets ? kPresetNames[w] : ""; }

CORE_COLD void preset(Settings* s, int which) {
  uint32_t seed = 0x2545F491u ^ (uint32_t)(uintptr_t)s;
  for (int k = 0; k < kSteps; ++k) {
    int v;
    uint8_t sh;
    switch (which) {
      case 0: v = -64 + k * 127 / (kSteps - 1); sh = kRamp; break;              // RAMP: up, one cycle
      case 1: v = k < 8 ? -64 + k * 16 : 64 - (k - 8) * 16; sh = kRamp; break;  // TRI
      case 2: v = -48 + (k & 3) * 32; sh = kHold; break;                         // STEPS: four a cycle, up
      default: v = (int)(rnd(seed) * 63.0f); sh = kSmooth; break;                // RND: smooth random steps
    }
    s->step[k] = (int8_t)(v < -64 ? -64 : v > 63 ? 63 : v);
    s->shape[k] = sh;
  }
}

CORE_COLD const Settings& get(int i) { return g_set[i < 0 ? 0 : i >= kLfos ? kLfos - 1 : i]; }

// 0.5.0 owner test (2026-11-13): an LFO on a Rytm param (PAN) made the whole Rytm slow - PLAY, the remote UI - so no
// LFO, A or B, uses the Rytm link: the XY pad, the scenes and MIDI learn keep it (core/mod/rytm_param.h)
CORE_COLD bool target_ok(int i, int t) {
  if (i < 0 || i >= kLfos || t < 0 || t >= kTargets) return false;
  return !is_rt(t);
}
CORE_COLD int target_max(int) {
#if CORE_POLY
  return kTRt - 1;   // (the Rytm's targets are the list's last: the list ends before RT1)
#else
  return kTargets - 1;
#endif
}
CORE_COLD const char* lfo_name(int i, char* b, int cap) {
  if (!kPerTrack) sniprintf(b, cap, "%d", i + 1);
  else if (track_of(i) == 12) sniprintf(b, cap, "FX%c", 'A' + slot_of(i));
  else sniprintf(b, cap, CORE_STR("T%d%c"), track_of(i) + 1, 'A' + slot_of(i));
  return b;
}
CORE_COLD int lfo_index(const char* s, const char** end) {
  const char* p = s;
  int i = -1;
  if (*p >= '0' && *p <= '9') {
    int n = (int)num(p, &p);
    i = n >= 1 && n <= kLfos ? n - 1 : -1;
  } else if (kPerTrack) {
    int t = -1;
    if ((p[0] == 'T' || p[0] == 't') && p[1] >= '1' && p[1] <= '9') {
      t = (int)num(p + 1, &p) - 1;
      if (t > 11) t = -1;
    } else if ((p[0] == 'F' || p[0] == 'f') && (p[1] == 'X' || p[1] == 'x')) {
      t = 12, p += 2;
    }
    char c = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
    if (t >= 0 && (c == 'A' || c == 'B')) i = lfo_of(t, c - 'A'), ++p;
  }
  if (end) *end = i < 0 ? s : p;
  return i;
}
CORE_COLD bool link_full() {
#if CORE_POLY
  if (!mod::rp_saturated()) return false;
  for (int i = 0; i < kLfos; ++i)
    if (is_rt(g_set[i].target) && g_set[i].depth) return true;   // (the LFOs' warning: one of them on the link)
#endif
  return false;
}

CORE_COLD bool set(int i, const Settings& s) {
  if (i < 0 || i >= kLfos || !target_ok(i, s.target)) return false;
  Settings n = s;
  sanitize(n);
  Settings& o = g_set[i];
  bool sync_changed = o.sync != n.sync;
  if (memcmp(&o, &n, sizeof n) != 0) changed();
  o = n;
  if (sync_changed) retrig(i);
  return true;
}

CORE_COLD const char* wave_name(int w) { return w >= 0 && w < kWaves ? kWaveNames[w] : "?"; }
CORE_COLD const char* retrig_name(int r) { return r >= 0 && r < kRetrigs ? kRetrigNames[r] : "?"; }
CORE_COLD const char* shape_name(int s) { return s >= 0 && s < kShapes ? kShapeNames[s] : "?"; }
CORE_COLD const char* target_name(int t) { return t >= 0 && t < kTargets ? kTargetNames[t] : "?"; }
#if CORE_POLY
// ---- the TGT / PAR display order (lfo.h): a mapping over the stored ids, nothing stored changes
static UI_ROM_LFO const char kRytmTrack[12][3] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
constexpr int kTrN = 12;
static CORE_COLD bool tracks_view() { return !device::box_screen(); }
static CORE_COLD bool sidecar_on(int t, bool unknown_is_yes) {   // track t plays a Sidecar machine (before any 33: unknown)
  int m = ui::remote_track_machine(t);
  return m < 0 ? unknown_is_yes : m != 0x7F;
}
// A voice-shared pair (RS/CP, MT/HT, CH/OH, CY/CB) shows its voice under ONE track of the pair: the first that plays a
// Sidecar machine (not yet known counts as one). A pair with no Sidecar track has no owner: its first track.
static CORE_COLD int voice_owner(int v) {
  for (int t = 0; t < kTrN; ++t)
    if (ui::remote_track_voice(t) == v && sidecar_on(t, true)) return t;
  return -1;
}
static CORE_COLD int track_of_voice(int v) {   // the track a stored voice target is shown under (lfo.h)
  const int o = voice_owner(v);
  if (o >= 0) return o;
  for (int t = 0; t < kTrN; ++t)
    if (ui::remote_track_voice(t) == v) return t;
  return 0;
}
#endif
// the list's voice entries (lfo.h): the 8 voices ranked by the track each is shown under (ties: the voice number)
static CORE_COLD int b_rank(int v) {
  int r = 0;
  const int tv = track_of_voice(v);
  for (int o = 0; o < 8; ++o)
    if (o != v) {
      const int to = track_of_voice(o);
      if (to < tv || (to == tv && o < v)) ++r;
    }
  return r;
}
static CORE_COLD int b_voice_at(int r) {
  for (int v = 0; v < 8; ++v)
    if (b_rank(v) == r) return v;
  return r;
}
CORE_COLD const char* target_label(int t) {
#if CORE_POLY
  if (tracks_view()) {
    if (is_voice(t)) return kRytmTrack[track_of_voice(t - kTVoice)];
    if (t >= kTRt && t < kTRt + kTrN) return kRytmTrack[t - kTRt];
  }
#endif
  return t >= kTVoice && t < kTVoice + 8 ? voice_name(t - kTVoice) : target_name(t);
}
// 0.5.0 owner test: no LFO targets the Rytm (target_ok), so A's list is B's: one list
CORE_COLD int target_count() {
#if CORE_POLY
  return kTRt;   // OFF, V1..V8, ids 9..52
#else
  return kTargets;
#endif
}
CORE_COLD int target_pos(int t) {
  if (t < 0 || t >= kTargets) return -1;
#if CORE_POLY
  return is_rt(t) ? -1 : is_voice(t) && tracks_view() ? 1 + b_rank(t - kTVoice) : t;
#else
  return t;
#endif
}
// A voice's PAR list: the params its machine names (ui::voice_label), plus a stored param it does not name while it is
// the stored one (an older file's); a machine that names none (not known yet, a user machine without labels): all 16.
// 0.5.0 owner test: SAMPLER names P9..P16 only - its list had eight empty "P1 -".."P8 -" rows.
static CORE_COLD uint32_t voice_params(int t, int keep) {
  uint32_t m = 0;
  for (int p = 0; p < 16; ++p) {
    char b[8];
    if (ui::voice_label(t - kTVoice, p, b, sizeof b)[0]) m |= 1u << p;
  }
  if (!m) return 0xFFFF;
  return keep >= 0 && keep < 16 ? m | 1u << keep : m;
}
static CORE_COLD int bits_below(uint32_t m, int p) {
  int n = 0;
  for (int k = 0; k < p; ++k) n += (int)(m >> k & 1);
  return n;
}
static CORE_COLD int nth_bit(uint32_t m, int i) {
  for (int k = 0; k < 16; ++k)
    if ((m >> k & 1) && i-- == 0) return k;
  return 0;
}
// a voice target's first param (TGT, the console / web `tgt`): P2 as before when its machine names it, else its first named
static CORE_COLD int voice_landing(int t) {
  const uint32_t m = voice_params(t, -1);
  return m >> 1 & 1 ? 1 : nth_bit(m, 0);
}
CORE_COLD bool target_step(Settings* s, int d) {
  const int pos = target_pos(s->target);
  int np = pos + d;
  const int n = target_count();
  np = np < 0 ? 0 : np >= n ? n - 1 : np;
  if (pos < 0 || np == pos) return false;
  int t = np;   // (the positions are the ids, the voices permuted among 1..8 by the track they are shown under)
#if CORE_POLY
  if (np >= kTVoice && np < kTFx && tracks_view()) t = kTVoice + b_voice_at(np - kTVoice);
#endif
  s->target = (uint8_t)t, s->param = (uint8_t)(is_voice(t) ? voice_landing(t) : 0);
  return true;
}
CORE_COLD int param_count(const Settings& s) {
  if (is_voice(s.target)) return bits_below(voice_params(s.target, s.param & 15), 16);
  return params_of(s.target) ? params_of(s.target) : 1;
}
CORE_COLD int param_index(const Settings& s) {
  if (is_voice(s.target)) return bits_below(voice_params(s.target, s.param & 15), s.param & 15);
  return s.param;
}
CORE_COLD void param_step(Settings* s, int d) {
  const int n = param_count(*s);
  int i = param_index(*s) + d;
  i = i < 0 ? 0 : i >= n ? n - 1 : i;
  s->param = (uint8_t)(is_voice(s->target) ? nth_bit(voice_params(s->target, s->param & 15), i) : i);
}
CORE_COLD int target_from_name(const char* s) {
  for (int t = 0; t < kTargets; ++t) {
    const char* n = kTargetNames[t];
    bool same = true;
    for (int k = 0; n[k] || s[k]; ++k) {
      char a = s[k] >= 'a' && s[k] <= 'z' ? (char)(s[k] - 32) : s[k];
      if (a != n[k]) { same = false; break; }
    }
    if (same) return t;
  }
  return -1;
}
CORE_COLD int params_of(int t) {
  if (is_voice(t)) return 16;
  if (is_fx(t)) return 4;
  if (is_ins(t) || is_mchain(t)) return 8 + ins::kEqParams;   // ins1112: 8..11 the chain's POST EQ
  if (is_out(t) || is_isnd(t)) return 3;
  if (is_mfx(t)) return kMfxParams;   // LEV, MIX1..4, B1A..B3B (the bus slots' send TYPE MIX, 0.5.0)
  if (is_snd(t)) return 2;
#if CORE_POLY
  if (is_rt(t)) return t == kTRfx ? kRfxN : kRtN;
#endif
  return 0;
}
CORE_COLD const char* param_name(int t, int p, char* b, int cap) {
  b[0] = 0;
  if (is_voice(t)) {
    ui::voice_label(t - kTVoice, p & 15, b, cap);
    if (!b[0]) sniprintf(b, cap, "-");   // a P the voice's machine leaves unused
  } else if (is_fx(t)) {
    const fx::KnobLabels* l = fx::labels(fx::control.knob[t - kTFx][0]);
    if (l && l->label[p & 3] && *l->label[p & 3]) sniprintf(b, cap, "%s", l->label[p & 3]);
    else sniprintf(b, cap, "-");   // a knob the TYPE leaves unused
#if CORE_POLY
  } else if ((is_ins(t) || is_mchain(t)) && p >= 8) {   // ins1112: the chain's POST EQ, "QHPF" (Q: after the slots)
    sniprintf(b, cap, CORE_STR("Q%.3s"), ins::eq_labels().label[(p - 8) & 3]);
  } else if (is_ins(t) || is_mchain(t)) {   // "ACUT": the slot's letter and the TYPE's label of the param
    int ty = ins::control.type[is_ins(t) ? t - kTIns : ins::kM0 + t - kTMaster][(p >> 2) & 1];
    const char* l = ins::labels(ty).label[p & 3];
    if (ty && l[0]) sniprintf(b, cap, CORE_STR("%c%.4s"), 'A' + ((p >> 2) & 1), l);
    else sniprintf(b, cap, CORE_STR("%c-"), 'A' + ((p >> 2) & 1));   // the slot OFF (or a knob its TYPE leaves unused)
  } else if (is_out(t)) {   // LEV PAN WID
    static const char* const kO[3] = {"LEV", "PAN", "WID"};
    sniprintf(b, cap, "%s", kO[p % 3]);
  } else if (is_mfx(t)) {   // 0.5.0: the master's LEV, its slots' MIX
    const int q = p % kMfxParams;
    if (q > kMfxMaster) sniprintf(b, cap, CORE_STR("B%d%c"), (q - 1 - kMfxMaster) / 2 + 1, 'A' + (q - 1 - kMfxMaster) % 2);
    else if (q) sniprintf(b, cap, CORE_STR("MIX%d"), q);
    else sniprintf(b, cap, "LEV");
  } else if (is_snd(t)) {   // a track's send to FX1 / FX2
    sniprintf(b, cap, CORE_STR("F%d"), (p & 1) + 1);
  } else if (is_isnd(t)) {   // a voice's send to bus 1..3
    sniprintf(b, cap, CORE_STR("B%d"), p % 3 + 1);
  } else if (is_rt(t)) {   // 0.5.0: the Rytm's own param ("FRQ"; P1..P8 the track's machine's label: "TUN")
    mod::rp_label(tid_of(t, p), b, cap);
#endif
  } else {
    sniprintf(b, cap, "-");
  }
  return b;
}
CORE_COLD float rate_hz(int rate) {   // 0.02 Hz x 1000^(rate / 127)
  return 0.02f * exp2p(9.9657843f * (float)(rate < 0 ? 0 : rate > 127 ? 127 : rate) / 127.0f);
}
CORE_COLD int div_ticks(int d) { return kDivTicks[d < 0 ? 0 : d >= kDivs ? kDivs - 1 : d]; }
CORE_COLD void rate_name(const Settings& s, char* b, int cap) {
  if (s.sync) {
    sniprintf(b, cap, "%s", kDivNames[s.rate < kDivs ? s.rate : kDivs - 1]);
    return;
  }
  int c = (int)(rate_hz(s.rate) * 100.0f + 0.5f);   // centi-Hz (integer printf only)
  if (c >= 1000) sniprintf(b, cap, CORE_STR("%dHZ"), (c + 50) / 100);
  else sniprintf(b, cap, CORE_STR("%d.%02dHZ"), c / 100, c % 100);
}

CORE_COLD float design_at(const Settings& s, float ph) {
  float x = clampf(ph, 0.0f, 0.99999f) * kSteps;
  int k = (int)x;
  float f = x - (float)k;
  float a = s.step[k] / 64.0f, b = s.step[(k + 1) % kSteps] / 64.0f;
  switch (s.shape[k]) {
    case kRamp: return a + (b - a) * f;
    case kSmooth: return a + (b - a) * f * f * (3.0f - 2.0f * f);
    default: return a;
  }
}

CORE_COLD float wave_at(const Settings& s, float ph, uint32_t seed) {
  if (s.wave == kSampleHold || s.wave == kRandom) {   // a fixed picture: 4 values a cycle
    uint32_t r = seed | 1;
    float vals[5];
    for (float& x : vals) x = rnd(r);
    vals[4] = vals[0];
    float x = clampf(ph, 0.0f, 0.99999f) * 4;
    int k = (int)x;
    float f = x - k;
    return s.wave == kSampleHold ? vals[k] : vals[k] + (vals[k + 1] - vals[k]) * smooth(f);
  }
  Run r;
  return shaped(s, ph, r, false);
}

CORE_COLD float phase(int i) { return g_run[i < 0 ? 0 : i >= kLfos ? kLfos - 1 : i].ph; }
CORE_COLD float out(int i) { return g_run[i < 0 ? 0 : i >= kLfos ? kLfos - 1 : i].w; }
CORE_COLD bool active(int i) { return i >= 0 && i < kLfos && g_set[i].target && g_set[i].depth; }
CORE_COLD const Stats& stats() {
#if CORE_MOD_LFO
  const mod::Stats& m = mod::stats();   // the layer's counts under the LFOs' names (the "lfo" line unchanged)
  g_stats.ticks = m.ticks, g_stats.voice_sets = m.voice_sets, g_stats.user_calls = m.user_calls;
  g_stats.fx_sets = m.fx_sets, g_stats.restores = m.restores;
#endif
  return g_stats;
}

// ---------------------------------------------------------------------------------------------------- the engine

bool clock_now(float* ticks, float* tick_us) {
  *ticks = g_clk.tp;
  *tick_us = g_clk.tick_us;
#if CORE_MOD_LFO
  return mod::stats().ticks != 0;
#else
  return g_stats.ticks != 0;
#endif
}
void clock_state(ClockState* o) {
  const Clock& c = g_clk;
  *o = ClockState{c.run, c.ticking, c.pos, c.last_us, c.tick_us, c.tp, c.starts, c.stops, c.jumps};
}

CORE_COLD void clock(uint8_t status, uint16_t spp) {
  Clock& c = g_clk;
  uint32_t now = now_us();
  switch (status) {
    case 0xF8: {
      if (c.ticking) {
        uint32_t d = now - c.last_us;
        if (d > 1000 && d < 250000) c.tick_us += (c.tick_us > 0 ? ((float)d - c.tick_us) * 0.25f : (float)d);
      }
      c.ticking = true;
      c.last_us = now;
      if (c.run) ++c.pos;
      break;
    }
    case 0xFA:
      c.run = true, c.pos = -1, c.start = true, ++c.starts;
      for (int i = 0; i < kLfos; ++i)
        if (g_set[i].retrig == kOnPlay) g_run[i].retrig = true;
      break;
    case 0xFB: c.run = true, ++c.starts; break;
    case 0xFC: c.run = false, ++c.stops; break;
    case 0xF2: c.pos = (int32_t)spp * 6 - 1, ++c.jumps; break;
    default: break;
  }
}

CORE_COLD void midi_in(uint8_t st, uint8_t d1, uint8_t d2) {
  (void)d1;
  if ((st & 0x0F) > 12 || (st & 0xF0) != 0x90 || !d2) return;
  for (int i = 0; i < kLfos; ++i)   // a trig on one of the Rytm's tracks (its note out): the FX targets' TRIG
    if (g_set[i].retrig == kOnTrig && is_fx(g_set[i].target)) g_run[i].retrig = true;
#if CORE_POLY
  for (int i = 0; i < kLfos; ++i)   // 0.5.0: an RT target's TRIG: its track's note (the track's channel 1..12), RFX any
    if (g_set[i].retrig == kOnTrig && is_rt(g_set[i].target) &&
        (g_set[i].target == kTRfx || g_set[i].target - kTRt == (st & 0x0F)))
      g_run[i].retrig = true;
#endif
}

CORE_COLD mod::Tid tid_of(int t, int p) {
  if (is_voice(t)) return mod::tid(mod::kV, t - kTVoice, p & 15);
  if (is_fx(t)) return mod::tid(mod::kFx, t - kTFx, p & 3);
  if (is_ins(t)) return mod::tid(mod::kIns, t - kTIns, p);   // I1..I8 B1..B3: chains 0..10; params 0..11
  if (is_out(t)) return mod::tid(mod::kOut, t - kTOut, p % 3);
  if (is_mchain(t)) return mod::tid(mod::kIns, ins::kM0 + t - kTMaster, p);   // 0.5.0: M1 M2, chains 11 12
  if (is_mfx(t)) return mod::tid(mod::kMfx, 0, p % kMfxParams);
  if (is_snd(t)) return mod::tid(mod::kSnd, t - kTSnd, p & 1);
  if (is_isnd(t)) return mod::tid(mod::kIsnd, t - kTIsnd, p % 3);
#if CORE_POLY
  if (is_rt(t)) return mod::rytm_tid(t == kTRfx ? 12 : t - kTRt, rt_word(t, p));
#endif
  return 0;
}

#if CORE_MOD_LFO
CORE_COLD void voice_params(int vi, float* p, int np, bool trig) {
  int t = kTVoice + (vi & 7);
  mod::voice_params(vi, p, np);   // the bases (core/mod)
  if (trig)
    for (int i = 0; i < kLfos; ++i)
      if (g_set[i].retrig == kOnTrig && (g_set[i].target == t || is_fx(g_set[i].target))) g_run[i].retrig = true;
}
CORE_COLD uint8_t fx_knob(int slot, int knob, uint8_t val) { return mod::fx_knob(slot, knob, val); }
CORE_COLD float base_of(int vi, int k, float cur) { return mod::base_of(vi, k, cur); }
CORE_COLD void tick(uint32_t us) { mod::tick(us); }

// mod::tick's source, once a millisecond at most (us now, dus since the last, at most 100 ms): the clock position and
// the waves, each live LFO's offset handed to the layer
CORE_COLD void waves(uint32_t us, uint32_t dus) {
  float dt = (float)dus * 1e-6f;
  // the clock position: locked to the song position while the Rytm plays and ticks, else on at the last tempo
  Clock& c = g_clk;
  uint32_t since = us - c.last_us;
  bool locked = c.run && c.ticking && since < 250000;
  if (locked && c.pos < 0) c.tp = 0;
  else if (locked) c.tp = (float)(c.pos % (int32_t)kWrap) + clampf((float)since / c.tick_us, 0.0f, 0.999f);
  else c.tp = modp(c.tp + (float)dus / c.tick_us, kWrap);
  if (c.start) c.start = false;

  for (int i = 0; i < kLfos; ++i) {
    Settings& s = g_set[i];
    Run& r = g_run[i];
    if (r.retrig) r.retrig = false, retrig(i);
    float ph;
    if (s.sync) {
      float d = (float)div_ticks(s.rate);
      float x = s.retrig == kFree ? c.tp : c.tp - r.anchor;
      if (x < 0) x += kWrap;
      ph = modp(x, d) / d;
    } else {
      ph = wrap1(r.ph + rate_hz(s.rate) * dt);
    }
    bool cycle = r.fresh || ph < r.ph;
    r.fresh = false;
    r.ph = ph;
    r.w = shaped(s, ph, r, cycle);
    if (!s.target) continue;
    mod::offset(i, tid_of(s.target, s.param), (float)s.depth, r.w);   // never pre-multiplied here (spec-050 1.4)
  }
}
#else
CORE_COLD void waves(uint32_t, uint32_t) {}

CORE_COLD void voice_params(int vi, float* p, int np, bool trig) {
  int t = kTVoice + (vi & 7);
  for (Tgt& e : g_t) {
    if (e.state != 1 || e.target != t || e.param >= np) continue;
    e.base = p[e.param];
    float val = clampf(e.base + e.off, 0.0f, 127.99f);
    p[e.param] = val;
    e.applied = val;
  }
  if (trig)
    for (int i = 0; i < kLfos; ++i)
      if (g_set[i].retrig == kOnTrig && (g_set[i].target == t || is_fx(g_set[i].target))) g_run[i].retrig = true;
}

CORE_COLD uint8_t fx_knob(int slot, int knob, uint8_t val) {
  if (knob < 2 || knob > 5) return val;
  Tgt* e = find(kTFx + (slot & 1), knob - 2);
  if (!e || e->state != 1) return val;
  e->base = val;
  int iv = rnd0(clampf(e->base + e->off, 0.0f, 127.0f));
  e->applied = (float)iv;
  return (uint8_t)iv;
}

CORE_COLD float base_of(int vi, int k, float cur) {
  Tgt* e = find(kTVoice + (vi & 7), k & 15);
  return e && e->state == 1 ? e->base : cur;
}

CORE_COLD void tick(uint32_t us) {
  if (!g_started) {
    g_started = true, g_last_us = us;
    return;
  }
  uint32_t dus = us - g_last_us;
  if (dus < 1000) {   // a millisecond at most; user machines waiting for their call go on
    if (!g_vdirty) return;
  } else {
    if (dus > 100000) dus = 100000;   // a long stall: the LFOs skip ahead no more than 100 ms
    g_last_us = us;
    float dt = (float)dus * 1e-6f;
    ++g_stats.ticks;
    // the clock position: locked to the song position while the Rytm plays and ticks, else on at the last tempo
    Clock& c = g_clk;
    uint32_t since = us - c.last_us;
    bool locked = c.run && c.ticking && since < 250000;
    if (locked && c.pos < 0) c.tp = 0;
    else if (locked) c.tp = (float)(c.pos % (int32_t)kWrap) + clampf((float)since / c.tick_us, 0.0f, 0.999f);
    else c.tp = modp(c.tp + (float)dus / c.tick_us, kWrap);
    if (c.start) c.start = false;

    for (Tgt& e : g_t) e.off = 0, e.live_now = false;
    for (int i = 0; i < kLfos; ++i) {
      Settings& s = g_set[i];
      Run& r = g_run[i];
      if (r.retrig) r.retrig = false, retrig(i);
      float ph;
      if (s.sync) {
        float d = (float)div_ticks(s.rate);
        float x = s.retrig == kFree ? c.tp : c.tp - r.anchor;
        if (x < 0) x += kWrap;
        ph = modp(x, d) / d;
      } else {
        ph = wrap1(r.ph + rate_hz(s.rate) * dt);
      }
      bool cycle = r.fresh || ph < r.ph;
      r.fresh = false;
      r.ph = ph;
      r.w = shaped(s, ph, r, cycle);
      if (!s.target) continue;
#if CORE_POLY
      if (s.target >= kTMaster) continue;   // (0.5.0's new targets: the layer's only, not this rollback path)
#endif
      Tgt* e = find(s.target, s.param);
      if (!e)
        for (Tgt& f : g_t)
          if (!f.state) {
            e = &f;
            e->target = s.target, e->param = s.param, e->state = 1;
            e->base = current(s.target, s.param);
            e->applied = e->base;
            break;
          }
      if (!e) continue;   // (cannot happen: twice as many entries as LFOs)
      e->live_now = true;
      e->off += (float)s.depth * r.w;
    }
    for (Tgt& e : g_t) {
      if (!e.state) continue;
      if (!e.live_now) {
        restore(e);
        continue;
      }
      if (is_voice(e.target)) {
        int vi = e.target - kTVoice;
        float val = clampf(e.base + e.off, 0.0f, 127.99f);
        if (fabsf(val - e.applied) >= 1.0f / 256.0f) {
          v[vi].p[e.param & 15] = val;
          e.applied = val;
          g_vdirty |= (uint8_t)(1u << vi);
          ++g_stats.voice_sets;
        }
#if CORE_POLY
      } else if (is_ins(e.target)) {   // fx-ins: the offset itself (the knob / a step lock is the base, insert.cpp)
        int iv = (int)(e.off + (e.off < 0 ? -0.5f : 0.5f));
        if ((float)iv != e.applied) {
          if (e.param >= 8) ins::eq_mod_offset(e.target - kTIns, (e.param - 8) & 3, iv);   // ins1112: the POST EQ
          else ins::mod_offset(e.target - kTIns, (e.param >> 2) & 1, e.param & 3, iv);
          e.applied = (float)iv;
          ++g_stats.fx_sets;
        }
      } else if (is_out(e.target)) {   // the OUT route's LEV PAN WID: the offset on the knob (out.cpp)
        int iv = (int)(e.off + (e.off < 0 ? -0.5f : 0.5f));
        if ((float)iv != e.applied) {
          out::mod_offset(e.target - kTOut, e.param % 3, iv);
          e.applied = (float)iv;
          ++g_stats.fx_sets;
        }
#endif
      } else {
        int iv = rnd0(clampf(e.base + e.off, 0.0f, 127.0f));
        if ((float)iv != e.applied) {
          fx::control.knob[e.target - kTFx][2 + (e.param & 3)] = (uint8_t)iv;
          e.applied = (float)iv;
          ++g_stats.fx_sets;
        }
      }
    }
  }
  // the voices whose parameters moved: their machines take them (a user machine at most every kUserMs)
  uint32_t ms = now_ms();
  for (int vi = 0; vi < kVoices; ++vi) {
    if (!(g_vdirty >> vi & 1)) continue;
    if (v[vi].user >= 0) {
      if (ms - g_user_ms[vi] < kUserMs) continue;
      g_user_ms[vi] = ms;
      ++g_stats.user_calls;
    }
    g_vdirty &= (uint8_t)~(1u << vi);
    voice_params_changed(vi);
  }
}
#endif

// ---------------------------------------------------------------------------------------------------- the card

// 1042 (data safety): the file save() writes (the kit's) had a line this build does not read, or is there but could not be
// opened: never written over (before: the next edit rewrote it with defaults in place of those LFOs)
bool g_locked = false;
#if CORE_MSAMP
CORE_COLD void rekey(const char* name) { sniprintf(g_proj, sizeof g_proj, "%s", name ? name : ""); }
CORE_COLD void want_files() {
  char fn[80];
  for (int k = -1; k < 128; ++k) name_of(project, fn, sizeof fn, k), store::want(fn);
}
#endif

CORE_COLD void load() {
  g_locked = false;
  for (Settings& s : g_set) defaults(&s);
  for (Run& r : g_run) r = Run();
  for (int i = 0; i < kLfos; ++i) g_run[i].rng = 0x9E3779B9u * (uint32_t)(i + 1);
  g_dirty = false;
  strcpy(g_proj, project);   // the settings are this project's and the Rytm's kit's from now on
  g_kit = g_rytm_kit;
  char fn[80], b[96];
  int n = 0, bad = 0;
  auto take = [&](int k) {
    if (k == 0 || b[0] == '#') return;   // a blank line or a comment (0.3.0: a NUL-led line is neither)
    if (from_line(b)) ++n;
    else ++bad;
  };
  auto done = [&](bool own) {
    if (bad && own) g_locked = true;
    log(CORE_STR("lfo: %d from %s%s"), n, fn, bad ? (own ? " (lines it does not read: the file is kept, not written)" : " (lines it does not read)") : "");
  };
  file_name(fn, sizeof fn, g_kit);
#if CORE_MSAMP
  {   // 2026-10-26: the RAM store (a project load fills it before this runs): no card call on this core
    const uint8_t* d;
    uint32_t len, at = 0;
    bool own = true;   // the file read is the one save() writes
    store::State st = store::get(fn, &d, &len);
    if (st == store::kAbsent && g_kit >= 0) {   // a kit without its own file: the project's (from before 1026)
      file_name(fn, sizeof fn, -1), own = false;
      st = store::get(fn, &d, &len);
    }
    if (st == store::kAbsent) return;   // none: the defaults
    if (st != store::kPresent) {   // there but unreadable (kLocked), or never filled (kUnknown): never taken for absent
      // (as the card path: only the kit's own file is never written over; the project's one, the default, is not the
      // file save() writes, so a kit without its own starts from the defaults and may save them)
      if (own) g_locked = true;
      log(CORE_STR("lfo: %s %s: the defaults%s"), fn, st == store::kLocked ? "cannot be read" : "not in the store",
          own ? ", nothing written" : "");
      return;
    }
    for (int k; (k = fmt::next_line(d, len, &at, b, sizeof b)) >= 0;) take(k);
    done(own);
  }
#else
  File f;
  fmt::post_wait(fn);   // 0.3.0: a posted (or queued) save of this very file lands first (another kit's is not in the way)
  fmt::recover(fn);     // a power cut between write_atomic's renames: the previous file back, a stray .tmp removed
  bool own = true;   // the file read is the one save() writes
  bool opened = f.open(fn, kRead);
  if (!opened) {   // a kit without its own file: the project's (from before 1026) is its default
    if (cards.sd().exists(fn)) g_locked = true;
    if (g_kit < 0) return;
    file_name(fn, sizeof fn, -1);
    fmt::post_wait(fn), fmt::recover(fn);
    own = false;
    if (!f.open(fn, kRead)) return;
  }
  fmt::LineReader rd(f);   // 2026-10-26: buffered, not a card read a byte (40 ms on core 3 at a kit change)
  for (int k; (k = rd.line(b, sizeof b)) >= 0;) take(k);
  f.close();
  done(own);
#endif
}

CORE_COLD void save_due() { if (g_dirty) g_dirty_ms = now_ms() - 1000; }
CORE_COLD bool dirty() { return g_dirty; }

// 0.3.0 power-cut safety: posted to the card core as one atomic write (fmt.h post_begin / post_end), so a cut leaves
// the file whole - old or new - and core 3 does no card I/O for it (the mkdirs went too: write_atomic makes the
// folders where it runs).
//
// 2026-10-26, the RAM store: the text is also put into the store (write-through), so a kit change back reads the edit
// from RAM before the card has it. queue (a kit change's save): no wait for the post slot - fmt::post_queue holds a copy
// behind the write in flight.
#if CORE_MSAMP
static char g_text[fmt::kPostMax];   // the text built (the post slot may be busy: post_queue copies it)
#endif
static CORE_COLD bool save_text(bool queue) {
  if (!g_dirty || now_ms() - g_dirty_ms < 1000) return false;
  if (g_locked) {   // 1042
    g_dirty = false;
    log(CORE_STR("lfo: not saved: its file has lines this build does not read (kept as it is)"));
    return true;
  }
#if CORE_MSAMP
  char* t = g_text;
#else
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
#endif
  char fn[80], b[96];
  file_name(fn, sizeof fn, g_kit);
  static UI_ROM_LFO const char kHead[] = "# Sidecar LFOs: NAME TARGET PARAM WAVE RATE SYNC RETRIG DEPTH BASE DESIGN\n";
  uint32_t n = sizeof kHead - 1;
  memcpy(t, kHead, n);
  for (int i = 0; i < kLfos; ++i) {
    int k = to_line(i, b, sizeof b);
    if (k <= 0 || n + (uint32_t)k > fmt::kPostMax) {   // (cannot happen at kLfos lines: never a half file)
      log(CORE_STR("lfo: %s not written: its text does not fit (kept as it is)"), fn);
      g_dirty = false;
      return true;
    }
    memcpy(t + n, b, (size_t)k), n += (uint32_t)k;
  }
#if CORE_MSAMP
  // the store is the project's now named (filled, or filling: the fill keeps this newer text); an old project's file
  // saved while the next one's store fills is not put in it
  if (!strcmp(g_proj, project)) store::put(fn, (const uint8_t*)t, n);
  // (a held slot, phase 3: queued too, so the edit is on its way rather than dirty for the whole hold)
  if (queue || fmt::post_stalled()) {
    if (!fmt::post_queue(fn, t, n)) log(CORE_STR("lfo: %s not written to the card (refused or out of memory: in RAM only)"), fn);
  } else {
    char* p = fmt::post_begin();
    if (!p) return false;   // another settings write is in flight: at a later pass (still dirty)
    memcpy(p, t, n);
    if (!fmt::post_end(fn, n)) return false;   // not posted: still dirty, again at a later pass
  }
#else
  if (!fmt::post_end(fn, n)) return false;   // not posted: still dirty, again at a later pass
#endif
  g_dirty = false;
  return true;
}
CORE_COLD bool save() { return save_text(false); }

CORE_COLD void project_changed() {
  // the old project's (or kit's) targets: voices and FX back to their bases, every dirty voice flushed
#if CORE_MOD_LFO
  mod::restore_all();
#else
  for (Tgt& e : g_t)
    if (e.state) restore(e);
  for (int vi = 0; vi < kVoices; ++vi)
    if (g_vdirty >> vi & 1) voice_params_changed(vi);
  g_vdirty = 0;
#endif
  // the old one's unsaved edits first, into its own file - never deferred past the load that clears g_dirty
#if CORE_MSAMP
  // 2026-10-26: its write posted or queued (no wait on the card), its text into the store when the store is its
  if (g_dirty) g_dirty_ms = now_ms() - 1000, save_text(true);
#else
  if (g_dirty) fmt::post_wait(), g_dirty_ms = now_ms() - 1000, save();   // post_wait frees the slot for this save
#endif
  load();
}

CORE_COLD void kit_changed(int kit) {
  if (kit < 0 || kit > 127) return;
  g_rytm_kit = kit;
#if CORE_MSAMP
  // a project load's store still filling: its load (project_fill_step) takes this kit. Likewise while a rename moves
  // the old key's files (final review): a save now would queue a write under the old name that lands after the
  // move (idle_save holds the writes meanwhile) - a stray file, the edit missing from the renamed project; the
  // switch that follows the move loads this kit, its project_changed saving the old kit's edits under the new name
  if (!store::ready() || rename::moving()) return;
#endif
  if (kit != g_kit) project_changed();   // the same switch: the old kit's edits saved, the new kit's LFOs loaded
}
CORE_COLD int kit() { return g_kit; }

// ---------------------------------------------------------------------------------------------------- console

CORE_COLD bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "lfo", 3) != 0 || (p[3] && p[3] != '=')) return false;
  if (!p[3]) {
    int n = 0;
    for (int i = 0; i < kLfos; ++i) {
      const Settings& s = g_set[i];
      char pn[8], rn[8], nm[6];
      param_name(s.target, s.param, pn, sizeof pn);
      rate_name(s, rn, sizeof rn);
      int k = sniprintf(o + n, cap - n, CORE_STR("%s%s%s %s %s %s %s %s%d"), i ? ", " : "lfo: ", kPerTrack ? "" : "L",
                        lfo_name(i, nm, sizeof nm), target_name(s.target), s.target ? pn : "", wave_name(s.wave), rn,
                        s.depth < 0 ? "-" : "+", s.depth < 0 ? -s.depth : s.depth);
      if (k < 0 || k >= cap - n) {   // (cut: the listing says so, not silently)
        if (cap - n > 4) strcpy(o + n, "...");
        else if (n > 3) strcpy(o + n - 3, "...");
        break;
      }
      n += k;
    }
    return true;
  }
  const char* a = p + 4;
  const char* ie;
  int i = lfo_index(a, &ie);
  const char* key = strchr(a, ':');
  if (i < 0 || ie == a || *ie != ':' || !key) {
    if (kPerTrack)
      sniprintf(o, cap, CORE_STR("lfo=N:KEY:VALUE (N 1..%d or T1A..T12B FXA FXB; tgt par wav rate sync trig dep base st:I:V:S des:HEX pre:NAME off)"), kLfos);
    else
      sniprintf(o, cap, CORE_STR("lfo=N:KEY:VALUE (N 1..%d; tgt par wav rate sync trig dep base st:I:V:S des:HEX pre:NAME off)"), kLfos);
    return true;
  }
  ++key;
  const char* val = strchr(key, ':');
  Settings s = g_set[i];
  bool ok = true;
  if (!strncmp(key, "off", 3)) {
    s.target = 0;
  } else if (!val) {
    ok = false;
  } else {
    ++val;
    int x = (int)num(val);
    int kl = (int)(val - 1 - key);
    if (kl == 3 && !strncmp(key, "tgt", 3)) {
      int t = target_from_name(val);
      if (t < 0 && val[0] >= '0' && val[0] <= '9') t = x;
      if (t < 0 || t >= kTargets || !target_ok(i, t)) ok = false;   // (no Rytm target: the owner's test)
      else if (t != s.target) s.target = (uint8_t)t, s.param = (uint8_t)(is_voice(t) ? voice_landing(t) : is_fx(t) ? 0 : 1);
    } else if (kl == 3 && !strncmp(key, "par", 3)) s.param = (uint8_t)x;
    else if (kl == 3 && !strncmp(key, "wav", 3)) s.wave = (uint8_t)x;
    else if (kl == 4 && !strncmp(key, "rate", 4)) s.rate = (uint8_t)(x < 0 ? 0 : x);
    else if (kl == 4 && !strncmp(key, "sync", 4)) {
      if ((x ? 1 : 0) != s.sync) s.rate = x ? 8 : 64;   // 1/4, ~0.6 Hz
      s.sync = (uint8_t)(x ? 1 : 0);
    } else if (kl == 4 && !strncmp(key, "trig", 4)) s.retrig = (uint8_t)x;
    else if (kl == 3 && !strncmp(key, "dep", 3)) s.depth = (int8_t)(x < -64 ? -64 : x > 63 ? 63 : x);
    else if (kl == 4 && !strncmp(key, "base", 4)) s.base = (uint8_t)(x < 0 ? 0 : x > 127 ? 127 : x);
    else if (kl == 2 && !strncmp(key, "st", 2)) {   // st:I:V:S
      const char* c2 = strchr(val, ':');
      const char* c3 = c2 ? strchr(c2 + 1, ':') : nullptr;
      if (x < 0 || x >= kSteps || !c2) ok = false;
      else {
        int sv = (int)num(c2 + 1);
        s.step[x] = (int8_t)(sv < -64 ? -64 : sv > 63 ? 63 : sv);
        if (c3) s.shape[x] = (uint8_t)(num(c3 + 1) % kShapes);
      }
    } else if (kl == 3 && !strncmp(key, "des", 3)) ok = design_from_hex(s, val);
    else if (kl == 3 && !strncmp(key, "pre", 3)) {
      int w = -1;
      if (!strcmp(val, "ramp")) w = 0;
      else if (!strcmp(val, "tri")) w = 1;
      else if (!strcmp(val, "steps")) w = 2;
      else if (!strcmp(val, "rnd") || !strcmp(val, "random")) w = 3;
      if (w < 0) ok = false;
      else preset(&s, w), s.wave = kDesign;
    } else ok = false;
  }
  if (!ok) {
    sniprintf(o, cap, CORE_STR("lfo: cannot \"%s\""), p);
    return true;
  }
  set(i, s);
  const Settings& n = g_set[i];
  char pn[8], rn[8], nm[6];
  param_name(n.target, n.param, pn, sizeof pn);
  rate_name(n, rn, sizeof rn);
  sniprintf(o, cap, CORE_STR("lfo: %s%s %s %s %s %s %s %s%d base %d"), kPerTrack ? "" : "L", lfo_name(i, nm, sizeof nm),
            target_name(n.target), n.target ? pn : "-", wave_name(n.wave), rn, retrig_name(n.retrig), n.depth < 0 ? "-" : "+",
            n.depth < 0 ? -n.depth : n.depth, n.base);
  return true;
}

CORE_COLD int json(char* o, int cap) {
#if RYTM_SENDFX_LARGE
  const int trk = ui::remote_lfo_track(nullptr);   // the Rytm's selected track (12 FX), -1 none named yet
#else
  const int trk = -1;
#endif
  int n = sniprintf(o, cap, CORE_STR("\"lfo\":{\"n\":%d,\"pt\":%d,\"kit\":%d,\"full\":%d,\"trk\":%d,\"l\":["), kLfos,
                    kPerTrack ? 1 : 0, g_kit < 0 ? 0 : g_kit + 1, link_full() ? 1 : 0, trk);
  for (int i = 0; i < kLfos && n < cap; ++i) {
    const Settings& s = g_set[i];
    char pn[8], rn[8], h[3 * kSteps + 1];
    param_name(s.target, s.param, pn, sizeof pn);
    rate_name(s, rn, sizeof rn);
    design_hex(s, h);
    int k = sniprintf(o + n, cap - n,
                      CORE_STR("%s{\"tgt\":\"%s\",\"par\":%d,\"pn\":\"%s\",\"np\":%d,\"wav\":%d,\"rate\":%d,\"rn\":\"%s\","
                               "\"sync\":%d,\"trig\":%d,\"dep\":%d,\"base\":%d,\"des\":\"%s\",\"ph\":%d}"),
                      i ? "," : "", target_name(s.target), s.param, pn, params_of(s.target), s.wave, s.rate, rn, s.sync,
                      s.retrig, s.depth, s.base, h, (int)(g_run[i].ph * 1000));
    if (k < 0 || k >= cap - n) return cap;
    n += k;
#if CORE_POLY   // (the Pi: the Mini's console line has no room for them)
    // the names of every param of the target, as param_name gives them ("-" unused): the web's PAR list (pns, 2026-11)
    n -= 1;   // (inside the LFO's object: its closing brace again below)
    for (int q = 0, np = params_of(s.target); q <= np; ++q) {
      if (q < np) param_name(s.target, q, pn, sizeof pn);
      k = q == np ? sniprintf(o + n, cap - n, "%s]}", np ? "" : ",\"pns\":[")
                  : sniprintf(o + n, cap - n, CORE_STR("%s\"%s\""), q ? "," : ",\"pns\":[", pn);
      if (k < 0 || k >= cap - n) return cap;
      n += k;
    }
#endif
  }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, "]}");
  return n + (k > 0 ? k : 0);
}

CORE_COLD void reset() {
  for (Settings& s : g_set) defaults(&s);
  for (int i = 0; i < kLfos; ++i) g_run[i] = Run(), g_run[i].rng = 0x9E3779B9u * (uint32_t)(i + 1);
  g_clk = Clock();
  g_dirty = false, g_kit = g_rytm_kit = -1;
  g_stats = Stats();
#if CORE_MOD_LFO
  mod::reset();   // the targets, the tick's clock, the voice flush, the counts: the layer's
#else
  for (Tgt& e : g_t) e = Tgt();
  g_started = false, g_vdirty = 0;
  for (uint32_t& m : g_user_ms) m = 0;
#endif
}

}  // namespace lfo
}  // namespace core
