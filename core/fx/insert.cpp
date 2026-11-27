// core/fx/insert.cpp - see insert.h: the values (knobs, p-locks, LFO offsets -> ins::control.play), the names, the
// console, the card (per kit), and the DSP hooks the render cores and the send FX's core call. CORE_POLY builds only
// (the Pi and the Mac).
#include "fx/insert.h"

#include <string.h>

#include "device/device.h"   // rytm_home: the SIDECHAIN settings are the Rytm's slot's
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "mod/target.h"
#include "platform.h"

namespace core {
namespace ins {

Control control;
Control control1;   // zero: every chain OFF, no post EQ, no sends (insert.h: slot 1's)
int (*slot_fn)() = nullptr;
bool (*rytm_fn)(int) = nullptr;
void (*send_insert)(int, int, int, const float*, const float*, const float*, float*, float*, int) = nullptr;

namespace {

#define FXL(x) {fx::x.label[0], fx::x.label[1], fx::x.label[2], fx::x.label[3]}
const Labels kLabels[kAllTypes] = {
    {"OFF", {"", "", "", ""}, {0, 0, 0, 0}},
    {"FILTER", {"CUT", "RES", "MODE", "DRV"}, {80, 20, 0, 0}},
    {"CRUSH", {"RATE", "BITS", "DRV", "MIX"}, {40, 80, 0, 127}},
    {"DRIVE", {"DRV", "SHPE", "TONE", "MIX"}, {50, 0, 30, 127}},
    {"EQ", {"LOW", "MID", "HIGH", "FREQ"}, {64, 64, 64, 64}},
    {"COMP", {"THR", "RAT", "SPD", "GAIN"}, {50, 60, 40, 20}},
    {"DELAY", {"TIME", "FDBK", "MOD", "MIX"}, {90, 40, 30, 64}},
    {"TREMOLO", {"RATE", "DPTH", "SHPE", "PAN"}, {64, 90, 0, 0}},
    {"RINGMOD", {"FREQ", "SHPE", "MOD", "MIX"}, {64, 0, 0, 127}},
    {"AMP", {"MODL", "GAIN", "TONE", "LEV"}, {0, 64, 64, 109}},   // core/fx/amp.h
    {"SAT", {"CHAR", "DRV", "TONE", "MIX"}, {0, 40, 64, 127}},    // core/fx/sat_fx.h
    // the master's: the send FX's knobs P2..P5, their labels (core/fx/master.h)
    {"CHORUS", FXL(kChorusLabels), {50, 90, 40, 0}},
    {"COMB", FXL(kCombLabels), {40, 30, 40, 20}},
#if RYTM_SENDFX_LARGE
    {"SPACE", FXL(kTapeLabels), {40, 50, 0, 20}},
    {"RINGS", FXL(kRingsLabels), {50, 0, 80, 60}},
    {"CLOUD", FXL(kCloudSeedLabels), {0, 70, 40, 30}},
#else
    {"SPACE", {"", "", "", ""}, {0, 0, 0, 0}},
    {"RINGS", {"", "", "", ""}, {0, 0, 0, 0}},
    {"CLOUD", {"", "", "", ""}, {0, 0, 0, 0}},
#endif
    {"HALO", FXL(kHaloLabels), {70, 30, 60, 0}},
};
#undef FXL
const char* const kChainNames[kChains] = {"V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "B1", "B2", "B3", "M1", "M2"};

// 3(c) (option A): each voice slot's settings as set - its chains' knobs, locks and offsets, their post EQs and sends,
// its dirty mark and kit - playing through control_of(slot). Since 3(d) every chain is the slot's own: its voice chains,
// its buses, its master chains and B3's route (each box its send FX and master), set and saved in its own file.
struct Set {
  uint8_t knob[kChains][kSlots][kParams];
  int16_t lock[kVoices][kSlots][kParams];   // -1 none
  int16_t lfo[kChains][kSlots][kParams];
  // ins1112, the POST EQ: the knobs as set (0..127, the UI's units) and the LFOs' offsets. No step locks: the lock
  // param space is exactly full at kLockParams = 32 (seq.h), so the post EQ takes LFOs and the knob, not p-locks.
  uint8_t eq[kChains][kEqParams];
  // sfx-basewidth: the WIDTH last set through set_eq_bw (eq_bw keeps it while it still gives the LPF). 127 is "none": it
  // reads the same as no memory. Any other write of the HPF / LPF, and a clear, forgets it, so a kit's HPF / LPF always
  // read as the same WIDTH whatever came before (it is not stored: the kit file keeps the PEQ line only).
  uint8_t bw_width[kChains];
  int16_t eq_lfo[kChains][kEqParams];
  uint8_t send_knob[kVoices][kBuses];   // the sends as set (control.send: as they play, + send_off)
  int16_t send_off[kVoices][kBuses];    // core/mod kIsnd's offsets
  // an AMP slot's capture as the file names it (a slot that is not out::g_base_slot keeps it here: AMP's state is the
  // base box's chains', amp.h; its file keeps the line)
  char amp_file[kVoices][kSlots][amp::kNameMax];
  bool dirty;
  uint32_t dirty_ms;
  int kit_now;
};
Set g_set[kVoiceSlots];
int g_force = -1;   // reset(): the slot worked on, whatever is in hand
int hand() {
  int sl = g_force >= 0 ? g_force : slot_fn ? slot_fn() : 0;
  return sl > 0 && sl < kVoiceSlots ? sl : 0;
}
int sl_of(int) { return hand(); }   // chain c's settings: the slot in hand's (every chain, 3(d))
Set& S(int c) { return g_set[sl_of(c)]; }
Control& K(int c) { return control_of(sl_of(c)); }
const Labels kEqLabels = {"POST EQ", {"HPF", "LPF", "LOW", "HIGH"}, {0, 0, 64, 64}};
Stats g_stats;

Engine g_engine(48000.0f);
#if CORE_VOICES > 8
// slot 1's chains (its voices 8..15, its buses and its master: phase 3(d))
Engine g_engine1(48000.0f, 1);
#endif
#ifndef CORE_HEAP_BUFFERS
float g_lines[kLines][kLine];
#if CORE_VOICES > 8
float g_lines1[kLines][kLine];
#endif
struct Placer {
  Placer() {
    g_engine.Place(g_lines);
#if CORE_VOICES > 8
    g_engine1.Place(g_lines1);
#endif
  }
} g_placer;
#endif

struct LockInit {   // no step locks at boot (a 0 would be a lock to 0)
  LockInit() {
    for (Set& t : g_set) {
      for (auto& a : t.lock)
        for (auto& b : a)
          for (int16_t& x : b) x = -1;
      for (auto& a : t.eq)
        for (int k = 0; k < kEqParams; ++k) a[k] = kEqFlat(k);
      t.kit_now = -1;
    }
  }
} g_lock_init;

void mark(int sl) { g_set[sl].dirty = true, g_set[sl].dirty_ms = now_ms(); }
void changed(int c) { mark(sl_of(c)); }   // chain c's settings changed: its slot's file
void changed() { mark(hand()); }          // the slot in hand's (OUT, a touch)
struct OutHook {   // OUT's settings live in the INSERT file (out.h)
  OutHook() { out::on_change = changed; }
} g_out_hook;

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void refresh(int c, int s, int k) {
  const Set& t = S(c);
  int v = c < kVoices && t.lock[c][s][k] >= 0 ? t.lock[c][s][k] : t.knob[c][s][k];
  K(c).play[c][s][k] = (uint8_t)clampi(v + t.lfo[c][s][k], 0, 127);
}
bool valid(int c, int s, int k = 0) { return c >= 0 && c < kChains && s >= 0 && s < kSlots && k >= 0 && k < kParams; }
bool eq_valid(int c, int k) { return c >= 0 && c < kChains && k >= 0 && k < kEqParams; }
void send_refresh(int vi, int b) {
  K(vi).send[vi][b] = (uint8_t)clampi(S(vi).send_knob[vi][b] + S(vi).send_off[vi][b], 0, 127);
}
void eq_refresh(int c, int k) {
  K(c).peq[c][k] = (int8_t)eq_store(k, clampi((int)S(c).eq[c][k] + S(c).eq_lfo[c][k], 0, 127));
}

bool same_ci(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}
int num(const char* p) {   // a decimal number, -1 none
  bool neg = *p == '-';
  if (neg) ++p;
  if (*p < '0' || *p > '9') return neg ? -2 : -1;
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  return neg ? -v : v;
}
// a field of "a:b:c" up to ':' or the end into b
const char* field(const char* p, char* b, int cap) {
  int n = 0;
  while (*p && *p != ':') {
    if (n < cap - 1) b[n++] = *p;
    ++p;
  }
  b[n] = 0;
  return *p == ':' ? p + 1 : p;
}

// Hz as "20", "950", "1.6K", "12K" (integers only: the Pi's printf has no float)
void hz_text(float hz, char* b, int cap) {
  int h = (int)(hz + 0.5f);
  if (h < 1000) sniprintf(b, cap, CORE_STR("%d"), h);
  else if (h < 10000) sniprintf(b, cap, CORE_STR("%d.%dK"), h / 1000, (h % 1000) / 100);
  else sniprintf(b, cap, CORE_STR("%dK"), (h + 500) / 1000);
}
float exp2c(float x) {   // 2^x, |x| < 30 (cold: the names)
  return fast_exp2(x);
}

void sc_clear();
// the slot in hand's settings: its chains, sends, B3 route and master (3(d): each slot all of its own)
void clear_settings() {
  const int nc = kChains;
  for (int c = 0; c < nc; ++c)
    for (int s = 0; s < kSlots; ++s) {
      K(c).type[c][s] = kOff;
      for (int k = 0; k < kParams; ++k) S(c).knob[c][s][k] = 0, refresh(c, s, k);
    }
  for (int c = 0; c < nc; ++c)
    for (int k = 0; k < kEqParams; ++k) S(c).eq[c][k] = kEqFlat(k), eq_refresh(c, k);   // ins1112
  for (int c = 0; c < nc; ++c) S(c).bw_width[c] = 127;   // sfx-basewidth: no WIDTH remembered
  sc_clear();   // 0.5.0 SIDECHAIN (the slot's own)
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b) S(v).send_knob[v][b] = 0, send_refresh(v, b);
  control_of(hand()).route3 = 0;
  master::clear();   // (the slot in hand's: master.cpp asks slot_in_hand)
  // 3(c) step 8: a box's send FX are its file's (SFX lines) unless the slot is a Rytm's (its kit's 0x20s, in either slot: two
  // Rytms, design 2026-11-26). An A4 or Digitakt in slot 0 too
  if (rytm_fn ? !rytm_fn(hand()) : hand() != 0) {
    fx::Control& f = fx::control_of(hand());
    for (int s = 0; s < fx::kSlots; ++s)
      for (int k = 0; k < fx::kKnobs; ++k) f.knob[s][k] = 0;
  }
}

// bus 3's soft limit, as the send FX's pairs (send_fx.h kLimitFx / kLimitMain)
inline float lim(float x, float l) { return fx::soft_limit(x, l); }

float g_send_g[2][kVoices][kBuses];   // the OUT fill's ramped send gains (core 0), each box's

}  // namespace

// ---------------------------------------------------------------------------------------------------- names

const Labels& labels(int t) { return kLabels[t >= 0 && t < kAllTypes ? t : 0]; }
const char* type_name(int t) { return t >= 0 && t < kAllTypes ? kLabels[t].name : "?"; }
int type_from_name(const char* s) {
  for (int t = 0; t < kAllTypes; ++t)
    if (same_ci(s, kLabels[t].name)) return t;
  if (same_ci(s, "TREM")) return kTrem;
  if (same_ci(s, "RING")) return kRing;
  int v = num(s);
  return v >= 0 && v < kAllTypes ? v : -1;
}
const char* chain_name(int c) { return c >= 0 && c < kChains ? kChainNames[c] : "?"; }
int chain_from_name(const char* s) {
  for (int c = 0; c < kChains; ++c)
    if (same_ci(s, kChainNames[c])) return c;
  return -1;
}

void value_text(int t, int k, int v, char* b, int cap) {
  v = clampi(v, 0, 127);
  const float p = (float)v / 127.0f;
  switch (t) {
    case kFilter:
      if (k == 0) return hz_text(20.0f * exp2c(10.0f * p), b, cap);
      if (k == 2) {
        static const char* const m[5] = {"LP", "LP>BP", "BP", "BP>HP", "HP"};
        int z = v < 8 ? 0 : v < 56 ? 1 : v < 72 ? 2 : v < 120 ? 3 : 4;
        sniprintf(b, cap, "%s", m[z]);
        return;
      }
      if (k == 3) {
        sniprintf(b, cap, CORE_STR("+%dDB"), (v * 24 + 63) / 127);
        return;
      }
      break;
    case kCrush:
      if (k == 1) {
        sniprintf(b, cap, CORE_STR("%dBIT"), 16 - (v * 14 + 63) / 127);
        return;
      }
      break;
    case kDrive:
      if (k == 0) {
        sniprintf(b, cap, CORE_STR("+%dDB"), (v * 30 + 63) / 127);
        return;
      }
      if (k == 2) return hz_text(18000.0f * exp2c(-4.17f * p), b, cap);
      break;
    case kEq:
      if (k < 3) {
        int d = v - 64, db10 = d * (d > 0 ? 150 : 150) / (d > 0 ? 63 : 64);   // tenths of a dB
        if (db10 == 0) sniprintf(b, cap, "0DB");
        else sniprintf(b, cap, CORE_STR("%c%d.%dDB"), db10 < 0 ? '-' : '+', (db10 < 0 ? -db10 : db10) / 10,
                       (db10 < 0 ? -db10 : db10) % 10);
        return;
      }
      return hz_text(200.0f * exp2c(5.32f * p), b, cap);
    case kComp:
      if (k == 0) {
        sniprintf(b, cap, CORE_STR("-%dDB"), (v * 40 + 63) / 127);
        return;
      }
      if (k == 1) {
        int r10 = (int)(10.0f * (1.0f + 19.0f * p * p) + 0.5f);
        sniprintf(b, cap, CORE_STR("%d.%d:1"), r10 / 10, r10 % 10);
        return;
      }
      if (k == 3) {
        sniprintf(b, cap, CORE_STR("+%dDB"), (v * 24 + 63) / 127);
        return;
      }
      break;
    case kDelay:
      if (k == 0) {
        int ms10 = (int)(10.0f * exp2c(6.3219f * p) + 0.5f);   // 80^p ms
        if (ms10 < 100) sniprintf(b, cap, CORE_STR("%d.%dMS"), ms10 / 10, ms10 % 10);
        else sniprintf(b, cap, CORE_STR("%dMS"), (ms10 + 5) / 10);
        return;
      }
      break;
    case kTrem:
      if (k == 0) {
        int c = (int)(100.0f * 0.1f * exp2c(7.6439f * p) + 0.5f);   // 200^p
        if (c >= 1000) sniprintf(b, cap, CORE_STR("%dHZ"), (c + 50) / 100);
        else sniprintf(b, cap, CORE_STR("%d.%02dHZ"), c / 100, c % 100);
        return;
      }
      break;
    case kRing:
      if (k == 0) return hz_text(10.0f * exp2c(8.9658f * p), b, cap);   // 500^p
      break;
    case kAmp: {   // MODL: the capture the knob picks (slot_value_text adds the slot's state); GAIN / TONE / LEV in dB
      if (k == 0) {
        int i = amp::model_for_knob(v);
        sniprintf(b, cap, "%s", i >= 0 ? amp::model_short(i) : amp::status_short(amp::kNoFiles));
        return;
      }
      int db = k == 1 ? (v * 36 + 63) / 127 - 18 : k == 2 ? ((v - 64) * 6 + (v >= 64 ? 31 : -31)) / 63 : (v * 42 + 63) / 127 - 36;
      if (db == 0) sniprintf(b, cap, k == 2 ? "FLAT" : "0DB");
      else sniprintf(b, cap, CORE_STR("%c%dDB"), db < 0 ? '-' : '+', db < 0 ? -db : db);
      return;
    }
    case kSat:
      if (k == 0) {
        sniprintf(b, cap, "%s", satfx::char_name(satfx::char_of(v)));
        return;
      }
      if (k == 1) {
        sniprintf(b, cap, CORE_STR("+%dDB"), (v * 24 + 63) / 127);
        return;
      }
      if (k == 2) {
        int db = ((v - 64) * 6 + (v >= 64 ? 31 : -31)) / 63;
        if (!db) sniprintf(b, cap, "FLAT");
        else sniprintf(b, cap, CORE_STR("%c%dDB"), db < 0 ? '-' : '+', db < 0 ? -db : db);
        return;
      }
      break;
    default: break;
  }
  if (t >= kTypes && master::zone_text(t, k, v, b, cap)) return;   // a send TYPE's zones (MODAL 12, 1/16)
  sniprintf(b, cap, CORE_STR("%d"), v);
}

void slot_value_text(int c, int s, int k, int v, char* b, int cap) {
  int t = valid(c, s, k) ? K(c).type[c][s] : kOff;
  if (t == kAmp && k == 0) return amp::knob_text(hand(), c, s, v, b, cap);
  value_text(t, k, v, b, cap);
}

// ---------------------------------------------------------------------------------------------------- settings

static bool (*g_cpu_fits)(int box, int c, int s, int t) = nullptr;
void set_cpu_check(bool (*fits)(int box, int c, int s, int t)) { g_cpu_fits = fits; }
// (the measured guard's mute shows the same NO CPU: fx_guard.h, decided 2026-11-23)
bool refused(int c, int s) {
  return valid(c, s) && (K(c).refused[c][s] || fx::guard::ins_muted(slot_in_hand(), c, s)) && K(c).type[c][s] != kOff;
}
int refused_count(int box) {
  if (box < 0 || box >= kVoiceSlots) return 0;
  const Control& k = control_of(box);
  int n = 0;
  for (int c = 0; c < kChains; ++c)
    for (int s = 0; s < kSlots; ++s) n += k.refused[c][s] && k.type[c][s] != kOff;
  return n;
}
CORE_COLD bool shed(int box, int c, int s) {
  if (box < 0 || box >= kVoiceSlots || c < 0 || c >= kChains || (c >= kBus0 && c < kM0) || s < 0 || s >= kSlots) return false;
  Control& k = control_of(box);
  const int t = k.type[c][s];
  if (t == kOff || t == kAmp || k.refused[c][s]) return false;
  k.refused[c][s] = 1;   // (the render core reads it each block: dry from the next)
  ++g_stats.refused;
  return true;
}
CORE_COLD int regrant() {
  int n = 0;
  if (!g_cpu_fits) return 0;
  for (int b = 0; b < kVoiceSlots; ++b) {
    Control& k = control_of(b);
    for (int c = 0; c < kChains; ++c)
      for (int s = 0; s < kSlots; ++s) {
        if (!k.refused[c][s]) continue;
        const int t = k.type[c][s];
        if (t == kOff || t == kAmp || g_cpu_fits(b, c, s, t)) k.refused[c][s] = 0, ++n;   // (a refused slot is not counted)
      }
  }
  return n;
}
void set_type(int c, int s, int t, bool keep) {
  if (!valid(c, s) || t < 0 || t >= kAllTypes) return;
  if (t == kAmp && c >= kVoices && c < kM0) return;   // AMP: the voice chains and the master (amp.h)
  if (t >= kTypes && c < kBus0) return;               // a send TYPE: the buses' and the master's chains (core 2's) only
  if (c >= kBus0 && !master::accept(c, s, t)) return;   // no room for another send TYPE (master.h kFxSlots)
  if (K(c).type[c][s] == t && keep) return;
  if (c < kVoices || c >= kM0) {   // CPU admission: the type's place on its render core first (refused: it plays dry, NO CPU)
    const bool ok = t == kOff || t == kAmp || !g_cpu_fits || g_cpu_fits(slot_in_hand(), c, s, t);
    if (!ok && !K(c).refused[c][s]) ++g_stats.refused;
    K(c).refused[c][s] = !ok;
    __atomic_thread_fence(__ATOMIC_RELEASE);   // (before the type: the render core never plays one not admitted)
  }
  K(c).type[c][s] = (uint8_t)t;
  fx::guard::clear(fx::guard::ins_unit(slot_in_hand(), c, s));   // set again: the measured guard's mute released (fx_guard.h)
  if (!keep)
    for (int k = 0; k < kParams; ++k) S(c).knob[c][s][k] = kLabels[t].def[k];
  for (int k = 0; k < kParams; ++k) refresh(c, s, k);
  ++g_stats.sets;
  changed(c);
}
void set_param(int c, int s, int k, int v) {
  if (!valid(c, s, k)) return;
  S(c).knob[c][s][k] = (uint8_t)clampi(v, 0, 127);
  refresh(c, s, k);
  ++g_stats.sets;
  changed(c);
}
void set_send(int vi, int b, int v) {
  if (vi < 0 || vi >= kVoices || b < 0 || b >= kBuses) return;
  S(vi).send_knob[vi][b] = (uint8_t)clampi(v, 0, 127);
  send_refresh(vi, b);
  ++g_stats.sets;
  changed(vi);
}
void send_offset(int vi, int b, int off) {
  if (vi < 0 || vi >= kVoices || b < 0 || b >= kBuses) return;
  S(vi).send_off[vi][b] = (int16_t)clampi(off, -127, 127);
  send_refresh(vi, b);
  ++g_stats.lfo_sets;
}
int send_knob(int vi, int b) { return vi >= 0 && vi < kVoices && b >= 0 && b < kBuses ? S(vi).send_knob[vi][b] : 0; }
void set_route3(int post) {
  control_of(hand()).route3 = post ? 1 : 0;
  mark(hand());
}
int param(int c, int s, int k) { return valid(c, s, k) ? S(c).knob[c][s][k] : 0; }
void set_param_in(int slot, int c, int s, int k, int v) {
  const int was = g_force;
  g_force = slot >= 0 && slot < kVoiceSlots ? slot : 0;
  set_param(c, s, k, v);
  g_force = was;
}
int param_in(int slot, int c, int s, int k) {
  return valid(c, s, k) ? g_set[slot > 0 && slot < kVoiceSlots ? slot : 0].knob[c][s][k] : 0;
}
// ---- ins1112, the POST EQ (end of the chain, after both slots)
const Labels& eq_labels() { return kEqLabels; }
static void eq_write(int c, int k, int v) {
  S(c).eq[c][k] = (uint8_t)clampi(v, 0, 127);
  eq_refresh(c, k);
  ++g_stats.sets;
  changed(c);
}
void set_eq(int c, int k, int v) {
  if (!eq_valid(c, k)) return;
  if (k < 2) S(c).bw_width[c] = 127;   // the HPF / LPF set as themselves: WIDTH is what they give
  eq_write(c, k, v);
}
int eq_param(int c, int k) { return eq_valid(c, k) ? S(c).eq[c][k] : 0; }
static int bw_lpf(int base, int width) { return base + width >= 127 ? 0 : 127 - base - width; }
void set_eq_bw(int c, int base, int width) {
  if (!eq_valid(c, 0)) return;
  base = clampi(base, 0, 127), width = clampi(width, 0, 127);
  eq_write(c, 0, base);
  eq_write(c, 1, bw_lpf(base, width));
  S(c).bw_width[c] = (uint8_t)width;
}
void eq_bw(int c, int* base, int* width) {
  int b = eq_param(c, 0), l = eq_param(c, 1);
  *base = b;
  if (eq_valid(c, 0) && bw_lpf(b, S(c).bw_width[c]) == l) *width = S(c).bw_width[c];
  else *width = l ? clampi(127 - b - l, 0, 127) : 127;
}
int eq_playing(int c, int k) { return eq_valid(c, k) ? eq_knob(k, K(c).peq[c][k]) : 0; }
void eq_mod_offset(int c, int k, int off) {
  if (!eq_valid(c, k)) return;
  S(c).eq_lfo[c][k] = (int16_t)clampi(off, -127, 127);
  eq_refresh(c, k);
  ++g_stats.lfo_sets;
}
bool eq_on(int c) { return eq_on_in(hand(), c); }
bool eq_on_in(int slot, int c) {   // (any core: names its slot, never asks the slot in hand)
  if (!eq_valid(c, 0) || slot < 0 || slot >= kVoiceSlots) return false;
  for (int k = 0; k < kEqParams; ++k)
    if (g_set[slot].eq[c][k] != kEqFlat(k)) return true;
  return false;
}
int slot_in_hand() { return hand(); }
bool eq_any_on() {
  for (int c = 0; c < kChains; ++c)
    if (eq_on(c)) return true;
  return false;
}
// HPF / LPF as Hz ("OFF" at 0: the knob does nothing), the shelves as dB ("FLAT" at 64)
void eq_value_text(int k, int v, char* b, int cap) {
  v = clampi(v, 0, 127);
  if (k < 2) {
    if (!v) return (void)sniprintf(b, cap, "OFF");
    float p = (float)v / 127.0f;
    return hz_text(k == 0 ? 20.0f * exp2c(10.0f * p) : 20000.0f * exp2c(-10.0f * p), b, cap);
  }
  int d = v - 64;
  if (!d) return (void)sniprintf(b, cap, "FLAT");
  int db10 = (int)(d * (d > 0 ? 150.0f / 63.0f : 150.0f / 64.0f) + (d > 0 ? 0.5f : -0.5f));
  int a = db10 < 0 ? -db10 : db10;   // ("%+d" would print "+0.5DB" for -0.5 dB: the sign is carried by hand)
  sniprintf(b, cap, CORE_STR("%s%d.%dDB"), db10 < 0 ? "-" : "+", a / 10, a % 10);
}
// ---- 0.5.0 SIDECHAIN (fx/sidechain.h): the stage's settings. The mode and the source are lists (never modulated), the six knobs
// take the LFOs' offsets (core/mod kIns params 12..17). No p-locks: the step-lock space is full (as the POST EQ's).
}  // namespace ins
namespace sc {
namespace {
const char* const kModeNames[kModes] = {"OFF", "PUMP", "DUCK"};
const char* const kSrcNames[kSrcs] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB",
                                      "AINL", "AINR", "AINLR", "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8"};
const char* const kLabelsM[kModes][kParams] = {{"-", "-", "-", "-", "-", "-"},
                                              {"DPTH", "ATT", "HOLD", "REL", "SHAP", "THR"},
                                              {"THR", "RAT", "ATT", "REL", "KBAS", "KWID"}};
const uint8_t kDefs[kModes][kParams] = {{0, 0, 0, 0, 0, 0}, {90, 0, 20, 60, 40, 50}, {50, 70, 10, 50, 0, 0}};
bool same_c(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}
// "4.0MS" / "120MS" / "1.2S" from milliseconds x 10
void ms_text(int ms10, char* b, int cap) {
  if (ms10 < 1000) sniprintf(b, cap, CORE_STR("%d.%dMS"), ms10 / 10, ms10 % 10);
  else if (ms10 < 10000) sniprintf(b, cap, CORE_STR("%dMS"), (ms10 + 5) / 10);
  else sniprintf(b, cap, CORE_STR("%d.%dS"), ms10 / 10000, (ms10 % 10000) / 1000);
}
}  // namespace
const char* mode_name(int m) { return m >= 0 && m < kModes ? kModeNames[m] : "?"; }
int mode_from_name(const char* s) {
  for (int m = 0; m < kModes; ++m)
    if (same_c(s, kModeNames[m])) return m;
  if (s[0] >= '0' && s[0] <= '9' && !s[1] && s[0] - '0' < kModes) return s[0] - '0';
  return -1;
}
const char* src_name(int s) { return s >= 0 && s < kSrcs ? kSrcNames[s] : "?"; }
int src_from_name(const char* s) {
  for (int i = 0; i < kSrcs; ++i)
    if (same_c(s, kSrcNames[i])) return i;
  if (same_c(s, "AIN")) return kAinLR;
  return -1;
}
const char* label(int m, int k) { return m >= 0 && m < kModes && k >= 0 && k < kParams ? kLabelsM[m][k] : "-"; }
uint8_t def(int m, int k) { return m >= 0 && m < kModes && k >= 0 && k < kParams ? kDefs[m][k] : 0; }
}  // namespace sc
namespace ins {

namespace {
void sc_vt(int m, int k, int v, char* b, int cap) {
  v = v < 0 ? 0 : v > 127 ? 127 : v;
  const float p = (float)v / 127.0f;
  if (m == sc::kPump) {
    switch (k) {
      case 0: return (void)sniprintf(b, cap, CORE_STR("%d%%"), (int)(100.0f * p + 0.5f));
      case 1: return v ? sc::ms_text((int)(300.0f * p * p + 0.5f), b, cap) : (void)sniprintf(b, cap, "INST");
      case 2: return sc::ms_text((int)(2500.0f * p * p + 0.5f), b, cap);
      case 3: return sc::ms_text((int)((10.0f + 1490.0f * p * p) * 10.0f + 0.5f), b, cap);
      case 4:
        if (v == 64) return (void)sniprintf(b, cap, "LIN");
        return (void)sniprintf(b, cap, CORE_STR("%c%d"), v < 64 ? '-' : '+', v < 64 ? 64 - v : v - 64);
      case 5: return (void)sniprintf(b, cap, CORE_STR("-%dDB"), (int)(60.0f * (1.0f - p) + 0.5f));
      default: break;
    }
  } else if (m == sc::kDuck) {
    switch (k) {
      case 0: return (void)sniprintf(b, cap, CORE_STR("-%dDB"), (v * 40 + 63) / 127);
      case 1: {
        int r10 = (int)(10.0f * (1.0f + 19.0f * p * p) + 0.5f);
        return (void)sniprintf(b, cap, CORE_STR("%d.%d:1"), r10 / 10, r10 % 10);
      }
      case 2: return sc::ms_text((int)((0.3f + 30.0f * p * p) * 10.0f + 0.5f), b, cap);
      case 3: return sc::ms_text((int)((40.0f + 600.0f * p) * 10.0f + 0.5f), b, cap);
      case 4: {   // KBAS
        if (!v) return (void)sniprintf(b, cap, "OFF");
        const int h = (int)(sc::kbas_hz(v) + 0.5f);
        if (h < 1000) sniprintf(b, cap, CORE_STR("%dHZ"), h);
        else sniprintf(b, cap, CORE_STR("%d.%dK"), h / 1000, (h % 1000) / 100);
        return;
      }
      case 5: {   // KWID, octaves
        if (!v) return (void)sniprintf(b, cap, "OFF");
        const int o10 = (int)(sc::kwid_oct(v) * 10.0f + 0.5f);
        return (void)sniprintf(b, cap, CORE_STR("%d.%d OCT"), o10 / 10, o10 % 10);
      }
      default: break;
    }
  }
  sniprintf(b, cap, "-");
}
// 0.5.0 SIDECHAIN: each slot's own (its chains, keyed from its own box's bus: sc::bus(slot)); the settings below are the
// slot in hand's, as the chains' (hand(): reset's g_force, else the slot in hand)
static Control& SC() { return control_of(hand()); }
uint8_t g_scp_[kVoiceSlots][kChains][sc::kParams];
#define g_scp g_scp_[hand()]
uint32_t g_mon_until_[kVoiceSlots][kChains];   // the web's MON lease (ms; 0: none), each slot's
#define g_mon_until g_mon_until_[hand()]
int16_t g_sc_lfo_[kVoiceSlots][kChains][sc::kParams];
#define g_sc_lfo g_sc_lfo_[hand()]
bool sc_valid(int c) { return c >= 0 && c < kChains; }
void sc_refresh(int c, int k) {
  SC().scp[c][k] = (uint8_t)clampi((int)g_scp[c][k] + g_sc_lfo[c][k], 0, 127);
}
// which key rings and trigs a chain reads: the writers skip the others
void sc_wanted() {
  uint32_t keys = 0, trigs = 0;
  for (int c = 0; c < kChains; ++c) {
    if (!SC().scmode[c]) continue;
    const int s = SC().scsrc[c], key = sc::key_of(s), tg = sc::trig_of(s);
    if (key >= 0) keys |= 1u << key;
    if (s == sc::kAinLR) keys |= 1u << (sc::kStems + 1);
    if (tg >= 0) trigs |= 1u << tg;
  }
  sc::bus(hand()).wanted = keys, sc::bus(hand()).wanted_trig = trigs;
}
void sc_clear() {
  for (int c = 0; c < kChains; ++c) {
    SC().scmode[c] = sc::kOff, SC().scsrc[c] = 0, SC().scmon[c] = 0, g_mon_until[c] = 0;
    for (int k = 0; k < sc::kParams; ++k) g_scp[c][k] = 0, sc_refresh(c, k);
  }
  sc_wanted();
}
}  // namespace

void sc_value_text(int mode, int k, int v, char* b, int cap) { sc_vt(mode, k, v, b, cap); }
void set_sc_mode(int c, int m, bool keep) {
  if (!sc_valid(c) || m < 0 || m >= sc::kModes) return;
  if (SC().scmode[c] == m) return;   // the mode it is in: nothing reloaded, the knobs stay
  if (!keep)
    for (int k = 0; k < sc::kParams; ++k) g_scp[c][k] = sc::def(m, k);
  SC().scmode[c] = (uint8_t)m;
  SC().scmon[c] = 0, g_mon_until[c] = 0;   // 0.5.2: MON ends with a mode change
  for (int k = 0; k < sc::kParams; ++k) sc_refresh(c, k);
  sc_wanted();
  ++g_stats.sets;
  changed();
}
void set_sc_src(int c, int s) {
  if (!sc_valid(c) || s < 0 || s >= sc::kSrcs) return;
  if (SC().scsrc[c] != s) SC().scmon[c] = 0, g_mon_until[c] = 0;   // 0.5.2: MON ends with a source change
  SC().scsrc[c] = (uint8_t)s;
  sc_wanted();
  ++g_stats.sets;
  changed();
}
void set_sc_param(int c, int k, int v) {
  if (!sc_valid(c) || k < 0 || k >= sc::kParams) return;
  g_scp[c][k] = (uint8_t)clampi(v, 0, 127);
  sc_refresh(c, k);
  ++g_stats.sets;
  changed();
}
// 0.5.2 MON: DUCK plays its filtered key instead of the chain's output. Per chain, never saved; only on a DUCK stage
void set_sc_mon(int c, bool on) {   // the Rytm's T1: no lease
  if (!sc_valid(c)) return;
  const uint8_t v = on && SC().scmode[c] == sc::kDuck ? 1 : 0;
  g_mon_until[c] = 0;
  if (SC().scmon[c] == v) return;
  SC().scmon[c] = v;
  ++g_stats.sets;
}
// the web's MON (the console line): it lapses 3 s after the last one unless the page sends it again (a closed tab, a lost link)
void set_sc_mon_lease(int c, bool on) {
  set_sc_mon(c, on);
  if (on && sc_mon(c)) g_mon_until[c] = now_ms() + 3000u | 1u;
}
void renew_sc_mon(int c) {   // the web's renew-only line: extends a lease that is running, never switches MON on
  if (sc_valid(c) && SC().scmon[c] && g_mon_until[c]) g_mon_until[c] = now_ms() + 3000u | 1u;
}
void sc_mon_tick() {   // from save(), every idle pass
  for (int c = 0; c < kChains; ++c)
    if (g_mon_until[c] && (int32_t)(now_ms() - g_mon_until[c]) > 0) g_mon_until[c] = 0, SC().scmon[c] = 0;
}
bool sc_mon(int c) { return sc_valid(c) && SC().scmon[c] != 0; }
void sc_mon_off_all() {
  for (int c = 0; c < kChains; ++c) SC().scmon[c] = 0, g_mon_until[c] = 0;
}
int sc_mode(int c) { return sc_valid(c) ? SC().scmode[c] : 0; }
int sc_src(int c) { return sc_valid(c) ? SC().scsrc[c] : 0; }
int sc_param(int c, int k) { return sc_valid(c) && k >= 0 && k < sc::kParams ? g_scp[c][k] : 0; }
int sc_playing(int c, int k) { return sc_valid(c) && k >= 0 && k < sc::kParams ? SC().scp[c][k] : 0; }
void sc_mod_offset(int c, int k, int off) {
  if (!sc_valid(c) || k < 0 || k >= sc::kParams) return;
  g_sc_lfo[c][k] = (int16_t)clampi(off, -127, 127);
  sc_refresh(c, k);
  ++g_stats.lfo_sets;
}
bool sc_on(int c) { return sc_valid(c) && SC().scmode[c] != sc::kOff; }
bool sc_any_on() {
  for (int c = 0; c < kChains; ++c)
    if (SC().scmode[c]) return true;
  return false;
}
bool sc_status(int c, char* why, int cap) {
  sniprintf(why, cap, "OK");
  if (!sc_on(c)) return sniprintf(why, cap, "OFF"), true;
  if (c >= kBus0 && c < kBus0 + 2 && fx::control_of(hand()).knob[c - kBus0][0] == fx::kOff)   // bus 1 / 2's chain runs inside its send FX's return
    return sniprintf(why, cap, "SEND FX OFF"), false;
  const int key = sc::key_of(SC().scsrc[c]);
  if (key < 0) return sniprintf(why, cap, "NO SRC"), false;
  if (SC().scmode[c] == sc::kPump && sc::trig_of(SC().scsrc[c]) >= 0) return true;   // PUMP on a track runs on its MIDI trigs
  if (key < sc::kStems + 2) {   // the Overbridge IN side's: stems, AUDIO IN. A voice's ring is written by its own render
    const sc::Bus& b = sc::bus(hand());
    if (!b.w[key] || now_ms() - b.ms[key] > 300) return sniprintf(why, cap, key < sc::kStems ? "NO STEMS" : "NO AUDIO IN"), false;
  }
  return true;
}

void lock(int vi, int s, int k, int v) {
  if (vi < 0 || vi >= kVoices || !valid(vi, s, k)) return;
  S(vi).lock[vi][s][k] = (int16_t)(v < 0 ? -1 : clampi(v, 0, 127));
  refresh(vi, s, k);
  ++g_stats.locks;
}
int locked(int c, int s, int k) { return c >= 0 && c < kVoices && valid(c, s, k) ? S(c).lock[c][s][k] : -1; }
void mod_offset(int c, int s, int k, int off) {
  if (!valid(c, s, k)) return;
  S(c).lfo[c][s][k] = (int16_t)clampi(off, -127, 127);
  refresh(c, s, k);
  ++g_stats.lfo_sets;
}
void touch() { changed(); }
bool any_on() {
  for (int c = 0; c < kChains; ++c)
    for (int s = 0; s < kSlots; ++s)
      if (K(c).type[c][s]) return true;
  return eq_any_on() || sc_any_on();   // ins1112; 0.5.0 SIDECHAIN
}

// ---------------------------------------------------------------------------------------------------- the DSP hooks

Engine& engine(int slot) {
#if CORE_VOICES > 8
  if (slot) return g_engine1;
#else
  (void)slot;
#endif
  return g_engine;
}

// the poly voices' soft limiter (voices.h kSoftKnee / kSoftCeil): an EQ boost or a DRIVE never hard-clips
static inline int16_t knee16(float y) {
  const float knee = 0.8f, ceil_ = 0.98f;
  float a = y < 0 ? -y : y;
  if (a > knee) {
    a = knee + (ceil_ - knee) * tanhf((a - knee) / (ceil_ - knee));
    y = y < 0 ? -a : a;
  }
  float q = y * 32767.0f;
  return (int16_t)(q > 32767.0f ? 32767 : q < -32768.0f ? -32768 : q);
}
void voice_out(int vi, int16_t (*ring)[core::kVoices], const int* row, int n, int16_t (*ring_r)[core::kVoices], int slot) {
  if (vi < 0 || vi >= core::kVoices) return;
  const int c = box_voice(vi);   // its box voice's chain, in its slot's engine
  Engine& e = engine(slot);
  const Control& k = control_of(slot);
  e.SetAmp(true);              // AMP on every box's chains (amp.h: each box its own rows)
  amp::service(slot, c);       // an AMP capture offered to a slot not playing AMP: taken now
  if (!ring_r) e.MarkMono(c);
  if (sc::wants_voice(slot, c)) {   // 0.5.0: a SIDECHAIN source of its slot's chains: this voice as it was rendered, before its own chain
    float kb[64];
    const int m = n > 64 ? 64 : n;
    for (int i = 0; i < m; ++i) kb[i] = ring[row[i]][vi] * (1.0f / 32768.0f);
    if (ring_r)
      for (int i = 0; i < m; ++i) kb[i] = 0.5f * (kb[i] + ring_r[row[i]][vi] * (1.0f / 32768.0f));
    sc::write(slot, sc::kStems + 2 + c, kb, m);
  }
  if (!e.Active(c, k)) return;
  float x[64], r[64];
  if (n > 64) n = 64;
  for (int i = 0; i < n; ++i) x[i] = ring[row[i]][vi] * (1.0f / 32768.0f);
  if (ring_r) {
    for (int i = 0; i < n; ++i) r[i] = ring_r[row[i]][vi] * (1.0f / 32768.0f);
    e.ProcessVoiceStereo(c, x, r, n, k);
    for (int i = 0; i < n; ++i) ring[row[i]][vi] = knee16(x[i]), ring_r[row[i]][vi] = knee16(r[i]);
  } else {
    e.ProcessMono(c, x, n, k);
    for (int i = 0; i < n; ++i) ring[row[i]][vi] = knee16(x[i]);
  }
  __atomic_fetch_add(&g_stats.voice_blocks, 1u, __ATOMIC_RELAXED);
}

bool sends_on() { return sends_on(0); }

bool sends_on(int box) {
  const Control& k = control_of(box);
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b)
      if (k.send[v][b] || g_send_g[box & 1][v][b] != 0.0f) return true;
  return false;
}

void voice_sends(const int16_t (*V)[kVoices], int n, float (*out)[kBuses]) { voice_sends(0, V, n, out); }

void voice_sends(int box, const int16_t (*V)[kVoices], int n, float (*out)[kBuses]) {
  for (int i = 0; i < n; ++i) out[i][0] = out[i][1] = out[i][2] = 0.0f;
  if (n <= 0) return;
  const Control& k = control_of(box);
  float (*gs)[kBuses] = g_send_g[box & 1];
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b) {
      float t = knob01(k.send[v][b]);
      t *= t;   // squared, as the send FX's sends
      float g0 = gs[v][b];
      if (g0 == 0.0f && t == 0.0f) continue;
      // toward the target over ~5 ms (a block is 7 frames on the Pi): linear within the block
      float g1 = g0 + (t - g0) * fminf(1.0f, (float)n / 240.0f);
      if (fabsf(g1 - t) < 1e-5f) g1 = t;
      gs[v][b] = g1;
      const float dg = (g1 - g0) / (float)n;
      float g = g0;
      for (int i = 0; i < n; ++i) g += dg, out[i][b] += g * (float)V[i][v] * (1.0f / 32768.0f);
    }
}

bool bus3_on() { return bus3_on(0); }

bool bus3_on(int box) {
  const Control& k = control_of(box);
  bool send = false;
  for (int v = 0; v < kVoices; ++v) send |= k.send[v][2] != 0;
  return (send && (k.type[kBus0 + 2][0] || k.type[kBus0 + 2][1] || eq_on_in(box, kBus0 + 2))) ||   // (box's own knobs)
         engine(box).Active(kBus0 + 2, k);
}

void bus3(const float* in, int n, float (*pre)[2], float (*post)[2], float* mono) { bus3(0, in, n, pre, post, mono); }

void bus3(int box, const float* in, int n, float (*pre)[2], float (*post)[2], float* mono) {
  Engine& e = engine(box);
  const Control& k = control_of(box);
  if (!e.Active(kBus0 + 2, k)) return;
  float y[128][2];
  if (n > 128) n = 128;
  for (int i = 0; i < n; ++i) y[i][0] = y[i][1] = in[i];
  e.ProcessStereo(kBus0 + 2, y, n, k);
  if (mono)
    for (int i = 0; i < n; ++i) mono[i] = 0.5f * (y[i][0] + y[i][1]);
  bool p = k.route3 != 0;
  float (*d)[2] = p ? post : pre;
  const float l = p ? fx::kLimitMain : fx::kLimitFx;
  if (!d) return;
  for (int i = 0; i < n; ++i) d[i][0] = lim(d[i][0] + y[i][0], l), d[i][1] = lim(d[i][1] + y[i][1], l);
  ++g_stats.bus_blocks;
}

// the send FX's hook (send_fx.h fx::bus_insert): FX slot s's return through bus s's chain
static void bus_hook(const fx::Control& fc, int s, float (*y)[2], int n) {
  const int box = fx::box_of(fc);
  Engine& e = engine(box);
  const Control& k = control_of(box);
  if (s < 0 || s > 1 || !e.Active(kBus0 + s, k)) return;
  e.ProcessStereo(kBus0 + s, y, n, k);
  ++g_stats.bus_blocks;
}
static struct BusHook {
  BusHook() { fx::bus_insert = bus_hook; }
} g_bus_hook;

// the DELAY lines, then AMP's engine pool (amp.h; on the Mac: its own static memory)
// (and on 16 voices slot 1's DELAY lines right after slot 0's: both line arrays at their alignment)
static const unsigned long kLinesBytes = sizeof(float) * kLines * kLine;
static const unsigned long kSideBytes = kVoiceSlots > 1 ? kLinesBytes : 0;
unsigned long heap_bytes() { return kLinesBytes + kSideBytes + amp::heap_bytes() + master::heap_bytes(); }
void heap_place(void* mem) {
  memset(mem, 0, heap_bytes());
  g_engine.Place((float(*)[kLine])mem);
#if CORE_VOICES > 8
  g_engine1.Place((float(*)[kLine])((uint8_t*)mem + kLinesBytes));
#endif
  amp::heap_place((uint8_t*)mem + kLinesBytes + kSideBytes);
  master::heap_place((uint8_t*)mem + kLinesBytes + kSideBytes + amp::heap_bytes());
}

// ---------------------------------------------------------------------------------------------------- the card's hooks

bool settings_dirty(uint32_t* since) {   // the slot in hand's
  const Set& t = g_set[hand()];
  if (since) *since = t.dirty_ms;
  return t.dirty;
}
bool settings_dirty_any() {
  for (const Set& t : g_set)
    if (t.dirty) return true;
  return false;
}
int kit() { return g_set[hand()].kit_now; }
void set_kit(int k) { g_set[hand()].kit_now = k; }
void settings_clean() { g_set[hand()].dirty = false; }
void settings_due() {   // every slot's (a restart or a power-off is coming)
  for (Set& t : g_set) t.dirty_ms = now_ms() - 1000;
}
// AMP plays on every box's chains (decided 2026-11-26): the slot in hand's own rows (amp.h)
void want_amp(int c, int s, const char* file) {
  if (c < 0 || c >= kChains || s < 0 || s >= kSlots) return;
  if (c < kVoices) sniprintf(S(c).amp_file[c][s], amp::kNameMax, "%s", file ? file : "");
  amp::want_file(hand(), c, s, file);
}
const char* amp_file(int c, int s) {
  if (c < 0 || c >= kChains || s < 0 || s >= kSlots) return "";
  return amp::slot_file(hand(), c, s);
}
void clear() {
  clear_settings();
  out::clear();
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s) want_amp(c, s, "");
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s)
      for (int k = 0; k < kParams; ++k) S(c).lock[c][s][k] = -1, refresh(c, s, k);
}

// ---------------------------------------------------------------------------------------------------- console

bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "ins", 3) != 0 || (p[3] && p[3] != '=')) return false;
  if (!p[3]) {
    int n = sniprintf(o, cap, "ins:");
    for (int c = 0; c < kChains && n < cap; ++c) {
      int a = K(c).type[c][0], b = K(c).type[c][1];
      bool send = c < kVoices && (K(c).send[c][0] || K(c).send[c][1] || K(c).send[c][2]);   // as it plays (a knob 0 + an offset is heard)
      if (!a && !b && !send && !eq_on(c)) continue;
      int k = sniprintf(o + n, cap - n, CORE_STR(" %s %s/%s%s"), kChainNames[c], kLabels[a].name, kLabels[b].name,
                        eq_on(c) ? "/EQ" : "");
      if (k < 0 || k >= cap - n) break;
      n += k;
      if (send) {
        k = sniprintf(o + n, cap - n, CORE_STR(" send %d/%d/%d"), send_knob(c, 0), send_knob(c, 1), send_knob(c, 2));
        if (k < 0 || k >= cap - n) break;
        n += k;
      }
      k = sniprintf(o + n, cap - n, ",");
      if (k < 0 || k >= cap - n) break;
      n += k;
    }
    if (n < cap) sniprintf(o + n, cap - n, CORE_STR(" b3 %s, faults %u"), control_of(hand()).route3 ? "post" : "pre",
                           (unsigned)engine(hand()).Faults());
    return true;
  }
  const char* q = p + 4;
  if (same_ci(q, "reset")) {
    clear_settings();
    changed();
    sniprintf(o, cap, "ins: every chain off, no sends, the post EQs flat");
    return true;
  }
  char a[8], b[8], key[8], val[12];
  q = field(q, a, sizeof a);
  q = field(q, b, sizeof b);
  q = field(q, key, sizeof key);
  field(q, val, sizeof val);
  int c = chain_from_name(a);
  static const char kHelp[] =
      "ins=C:S:type:T|p1..p4:V|off (C V1..V8 B1..B3, S A|B), ins=Bn:S:mix:V (a send TYPE's), ins=C:eq:hpf|lpf|low|high|base|width:V|off, "
      "ins=C:sc:mode:off|pump|duck, ins=C:sc:src:bd..cb|ainl|ainr|ainlr|v1..v8, ins=C:sc:p1..p6:V, ins=C:sc:mon:on|off|renew (DUCK: hear the filtered key; C V1..V8 B1..B3 M1 M2), "
      "ins=Vn:send:B:V (B 1..3), ins=b3:route:pre|post, ins=reset";
  if (c < 0) {
    sniprintf(o, cap, "%s", kHelp);
    return true;
  }
  if (same_ci(b, "send") && c < kVoices) {
    int bus = num(key) - 1, v = num(val);
    if (bus < 0 || bus >= kBuses || v < 0) {
      sniprintf(o, cap, "%s", kHelp);
      return true;
    }
    set_send(c, bus, v), fx::edited(mod::tid(mod::kIsnd, c, bus));
    sniprintf(o, cap, CORE_STR("ins: %s send %d/%d/%d"), kChainNames[c], send_knob(c, 0), send_knob(c, 1), send_knob(c, 2));
    return true;
  }
  if (same_ci(b, "route") && c == kBus0 + 2) {
    set_route3(same_ci(key, "post"));
    sniprintf(o, cap, CORE_STR("ins: B3 returns %s"), control_of(hand()).route3 ? "POST (0/1)" : "PRE (2/3)");
    return true;
  }
  if (same_ci(b, "eq")) {   // ins1112: ins=V3:eq:hpf:40, ins=b1:eq:lpf:90, ins=V3:eq:off; 0.5.0 ins=b1:eq:base:40, :width:90
    static const char* const kK[kEqParams] = {"hpf", "lpf", "low", "high"};
    if (same_ci(key, "off")) {
      for (int k = 0; k < kEqParams; ++k) set_eq(c, k, kEqFlat(k));
    } else if (same_ci(key, "base") || same_ci(key, "width")) {   // sfx-basewidth: HPF and LPF as BASE / WIDTH
      int v = num(val), bs, w;
      if (v < 0) {
        sniprintf(o, cap, "%s", kHelp);
        return true;
      }
      eq_bw(c, &bs, &w);
      if (same_ci(key, "base")) bs = v;
      else w = v;
      set_eq_bw(c, bs, w), fx::edited(mod::tid(mod::kIns, c, 8)), fx::edited(mod::tid(mod::kIns, c, 9));
    } else {
      int k = -1;
      for (int j = 0; j < kEqParams; ++j)
        if (same_ci(key, kK[j])) k = j;
      int v = num(val);
      if (k < 0 || v < 0) {
        sniprintf(o, cap, "%s", kHelp);
        return true;
      }
      set_eq(c, k, v), fx::edited(mod::tid(mod::kIns, c, 8 + k));
    }
    const Labels& L = eq_labels();
    char v4[kEqParams][8];
    for (int k = 0; k < kEqParams; ++k) eq_value_text(k, eq_param(c, k), v4[k], sizeof v4[k]);
    int bs, w;
    eq_bw(c, &bs, &w);
    sniprintf(o, cap, CORE_STR("ins: %s POST EQ %s %s %s %s %s %s %s %s (BASE %d WIDTH %d)"), kChainNames[c], L.label[0],
              v4[0], L.label[1], v4[1], L.label[2], v4[2], L.label[3], v4[3], bs, w);
    return true;
  }
  if (same_ci(b, "sc")) {   // 0.5.0 SIDECHAIN: ins=V2:sc:mode:pump|duck|off, :src:bd|ainl|ainlr|v3.., :p1..p6:V, :off
    if (same_ci(key, "off")) {
      set_sc_mode(c, sc::kOff);
    } else if (same_ci(key, "mode")) {
      int m = sc::mode_from_name(val);
      if (m < 0) {
        sniprintf(o, cap, "%s", kHelp);
        return true;
      }
      set_sc_mode(c, m);
    } else if (same_ci(key, "src")) {
      int sr = sc::src_from_name(val);
      if (sr < 0) {
        sniprintf(o, cap, "ins: no source \"%s\" (BD SD RS CP BT LT MT HT CH OH CY CB AINL AINR AINLR V1..V8)", val);
        return true;
      }
      set_sc_src(c, sr);
    } else if (same_ci(key, "mon") && same_ci(val, "renew")) {
      renew_sc_mon(c);
    } else if (same_ci(key, "mon")) {
      if (!same_ci(val, "on") && !same_ci(val, "off") && !same_ci(val, "1") && !same_ci(val, "0")) {
        sniprintf(o, cap, "%s", kHelp);
        return true;
      }
      set_sc_mon_lease(c, same_ci(val, "on") || same_ci(val, "1"));
    } else if ((key[0] == 'p' || key[0] == 'P') && key[1] >= '1' && key[1] <= '0' + sc::kParams && !key[2] && num(val) >= 0) {
      set_sc_param(c, key[1] - '1', num(val)), fx::edited(mod::tid(mod::kIns, c, 12 + key[1] - '1'));
    } else {
      sniprintf(o, cap, "%s", kHelp);
      return true;
    }
    const int m = sc_mode(c);
    char v6[sc::kParams][8], why[16];
    for (int k = 0; k < sc::kParams; ++k) sc_value_text(m, k, sc_param(c, k), v6[k], sizeof v6[k]);
    sc_status(c, why, sizeof why);
    sniprintf(o, cap, CORE_STR("ins: %s SIDECHAIN %s src %s %s %s %s %s %s %s %s %s %s %s %s %s (%s)%s"), kChainNames[c], sc::mode_name(m),
              sc::src_name(sc_src(c)), sc::label(m, 0), v6[0], sc::label(m, 1), v6[1], sc::label(m, 2), v6[2], sc::label(m, 3), v6[3],
              sc::label(m, 4), v6[4], sc::label(m, 5), v6[5], why, sc_mon(c) ? " MON" : "");
    return true;
  }
  int s = same_ci(b, "A") || same_ci(b, "1") ? 0 : same_ci(b, "B") || same_ci(b, "2") ? 1 : -1;
  if (s < 0) {
    sniprintf(o, cap, "%s", kHelp);
    return true;
  }
  if (same_ci(key, "off")) {
    set_type(c, s, kOff);
  } else if (same_ci(key, "type")) {
    int t = type_from_name(val);
    if (t < 0) {
      sniprintf(o, cap, CORE_STR("ins: no type \"%s\" (OFF FILTER CRUSH DRIVE EQ COMP DELAY TREMOLO RINGMOD AMP SAT; buses: CHORUS COMB SPACE RINGS CLOUD HALO)"), val);
      return true;
    }
    if (t == kAmp && c >= kVoices && c < kM0) {
      sniprintf(o, cap, "ins: AMP is for the voice chains (V1..V8) and the master, not the buses");
      return true;
    }
    if (t >= kTypes && c < kBus0) {
      sniprintf(o, cap, "ins: %s is for the buses (B1..B3) and the master (mfx=), not a voice", kLabels[t].name);
      return true;
    }
    set_type(c, s, t);
    if (K(c).type[c][s] != t) {   // the send TYPEs' room is full (master.h kFxSlots)
      int n = sniprintf(o, cap, CORE_STR("ins: %s %c: "), kChainNames[c], 'A' + s);
      master::full_text(t, o + n, cap - n);
      return true;
    }
  } else if ((key[0] == 'p' || key[0] == 'P') && key[1] >= '1' && key[1] <= '4' && !key[2]) {
    int v = num(val);
    if (v < 0) {
      sniprintf(o, cap, "%s", kHelp);
      return true;
    }
    set_param(c, s, key[1] - '1', v), fx::edited(mod::tid(mod::kIns, c, s * 4 + key[1] - '1'));
  } else if (same_ci(key, "mix") && master::slot_index(c, s) >= 0 && master::send_type(K(c).type[c][s]) && num(val) >= 0) {
    // a bus's or the master's slot holding a send TYPE (else the help: no MIX to set); the master's an edit as mfx=S:mix:
    const int i = master::slot_index(c, s);
    master::set_mix(i, num(val));
    if (i < master::kSlots) fx::edited(mod::tid(mod::kMfx, 0, 1 + i));
  } else {
    sniprintf(o, cap, "%s", kHelp);
    return true;
  }
  int t = K(c).type[c][s];
  char v4[4][8];
  for (int k = 0; k < kParams; ++k) slot_value_text(c, s, k, S(c).knob[c][s][k], v4[k], sizeof v4[k]);
  int n = sniprintf(o, cap, CORE_STR("ins: %s %c %s %s %s %s %s %s %s %s %s"), kChainNames[c], 'A' + s, kLabels[t].name,
                    kLabels[t].label[0], v4[0], kLabels[t].label[1], v4[1], kLabels[t].label[2], v4[2], kLabels[t].label[3], v4[3]);
  if (master::send_type(t) && n > 0 && n < cap) sniprintf(o + n, cap - n, CORE_STR(" mix %d"), master::cur().mix[master::slot_index(c, s)]);
  return true;
}

int json(char* o, int cap) {
  int n = sniprintf(o, cap, CORE_STR("\"ins\":{\"kit\":%d,\"route3\":%d,\"c\":["), kit() + 1, control_of(hand()).route3);
  for (int c = 0; c < kM0 && n < cap; ++c) {   // (the master's chains: "mfx", below)
    int k = sniprintf(o + n, cap - n, CORE_STR("%s{\"n\":\"%s\",\"s\":["), c ? "," : "", kChainNames[c]);
    if (k < 0 || k >= cap - n) return cap;
    n += k;
    for (int s = 0; s < kSlots; ++s) {
      int t = K(c).type[c][s];
      k = sniprintf(o + n, cap - n, CORE_STR("%s{\"t\":\"%s\",\"p\":[%d,%d,%d,%d],\"l\":[%d,%d,%d,%d]"), s ? "," : "",
                    kLabels[t].name, S(c).knob[c][s][0], S(c).knob[c][s][1], S(c).knob[c][s][2], S(c).knob[c][s][3],
                    K(c).play[c][s][0], K(c).play[c][s][1], K(c).play[c][s][2], K(c).play[c][s][3]);
      if (k < 0 || k >= cap - n) return cap;
      n += k;
      if (master::send_type(t)) {   // 0.5.0: a bus's send TYPE: its MIX
        k = sniprintf(o + n, cap - n, CORE_STR(",\"mix\":%d"), master::cur().mix[master::slot_index(c, s)]);
        if (k < 0 || k >= cap - n) return cap;
        n += k;
      }
      if (t) {   // the TYPE's knobs' names (a send TYPE's since 0.5.0; every TYPE's since 2026-11: the web names P1..P4)
        const Labels& L = kLabels[t];
        k = sniprintf(o + n, cap - n, CORE_STR(",\"n4\":[\"%s\",\"%s\",\"%s\",\"%s\"]"), L.label[0], L.label[1], L.label[2],
                      L.label[3]);
        if (k < 0 || k >= cap - n) return cap;
        n += k;
      }
      k = sniprintf(o + n, cap - n, "}");
      if (k < 0 || k >= cap - n) return cap;
      n += k;
    }
    k = sniprintf(o + n, cap - n, CORE_STR("],\"peq\":[%d,%d,%d,%d],\"peql\":[%d,%d,%d,%d]"),   // ins1112: set, playing
                  eq_param(c, 0), eq_param(c, 1), eq_param(c, 2), eq_param(c, 3), eq_playing(c, 0), eq_playing(c, 1),
                  eq_playing(c, 2), eq_playing(c, 3));
    if (k < 0 || k >= cap - n) return cap;
    n += k;
    if (c < kVoices)
      k = sniprintf(o + n, cap - n, CORE_STR(",\"send\":[%d,%d,%d]}"), send_knob(c, 0), send_knob(c, 1), send_knob(c, 2));
    else if (c < kBus0 + 2) {   // sfx-basewidth: the send FX return filter as BASE / WIDTH (the same two knobs as HPF / LPF)
      int bs, w;
      eq_bw(c, &bs, &w);
      k = sniprintf(o + n, cap - n, CORE_STR(",\"bw\":[%d,%d]}"), bs, w);
    } else
      k = sniprintf(o + n, cap - n, "}");
    if (k < 0 || k >= cap - n) return cap;
    n += k;
  }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, "],");
  if (k > 0) n += k;
  // 0.5.0 SIDECHAIN: every chain (V1..V8 B1..B3 M1 M2): off as {"m":"OFF"}; else its mode, source, knobs (set / playing), names, status
  k = sniprintf(o + n, n < cap ? cap - n : 0, "\"sc\":[");
  if (k > 0) n += k;
  for (int c = 0; c < kChains && n < cap; ++c) {
    if (!sc_on(c)) {
      k = sniprintf(o + n, cap - n, CORE_STR("%s{\"m\":\"OFF\"}"), c ? "," : "");
    } else {
      const int m = sc_mode(c);
      char why[16];
      sc_status(c, why, sizeof why);
      k = sniprintf(o + n, cap - n,
                    CORE_STR("%s{\"m\":\"%s\",\"s\":\"%s\",\"p\":[%d,%d,%d,%d,%d,%d],\"l\":[%d,%d,%d,%d,%d,%d],\"n\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],\"st\":\"%s\",\"mon\":%d}"),
                    c ? "," : "", sc::mode_name(m), sc::src_name(sc_src(c)), sc_param(c, 0), sc_param(c, 1), sc_param(c, 2),
                    sc_param(c, 3), sc_param(c, 4), sc_param(c, 5), sc_playing(c, 0), sc_playing(c, 1), sc_playing(c, 2),
                    sc_playing(c, 3), sc_playing(c, 4), sc_playing(c, 5), sc::label(m, 0), sc::label(m, 1), sc::label(m, 2),
                    sc::label(m, 3), sc::label(m, 4), sc::label(m, 5), why, sc_mon(c) ? 1 : 0);
    }
    if (k < 0 || k >= cap - n) return cap;
    n += k;
  }
  k = sniprintf(o + n, n < cap ? cap - n : 0, "],");
  if (k > 0) n += k;
  if (n < cap) n += out::json(o + n, cap - n);
  k = sniprintf(o + n, n < cap ? cap - n : 0, ",");
  if (k > 0) n += k;
  if (n < cap) n += amp::json(o + n, cap - n);
  k = sniprintf(o + n, n < cap ? cap - n : 0, ",");
  if (k > 0) n += k;
  if (n < cap) n += master::json(o + n, cap - n);
  k = sniprintf(o + n, n < cap ? cap - n : 0, "}");
  return n + (k > 0 ? k : 0);
}

const Stats& stats() { return g_stats; }

void reset() {
  for (int sl = kVoiceSlots - 1; sl >= 0; --sl) {   // every slot's (slot 0's last: its buses and master too)
    g_force = sl;
    clear_settings();
    const int nc = kChains;
    for (int c = 0; c < nc; ++c)
      for (int s = 0; s < kSlots; ++s)
        for (int k = 0; k < kParams; ++k) {
          if (c < kVoices) S(c).lock[c][s][k] = -1;
          S(c).lfo[c][s][k] = 0;
          refresh(c, s, k);
        }
    for (int c = 0; c < nc; ++c)
      for (int k = 0; k < kEqParams; ++k) S(c).eq_lfo[c][k] = 0, eq_refresh(c, k);   // ins1112
    for (int v = 0; v < kVoices; ++v)
      for (int b = 0; b < kBuses; ++b) S(v).send_off[v][b] = 0, send_refresh(v, b);
    memset(S(0).amp_file, 0, sizeof S(0).amp_file);
    S(0).dirty = false, S(0).kit_now = -1;
    for (int c = 0; c < kChains; ++c)
      for (int k = 0; k < sc::kParams; ++k) g_sc_lfo[c][k] = 0, sc_refresh(c, k);   // 0.5.0 SIDECHAIN (each slot's)
  }
  g_force = -1;
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b) g_send_g[0][v][b] = g_send_g[1][v][b] = 0.0f;
  g_engine.Reset();
  memset((void*)control.refused, 0, sizeof control.refused);
#if CORE_VOICES > 8
  memset((void*)&control1, 0, sizeof control1);
  g_engine1.Reset();
#endif
  amp::reset();
  master::reset();
  g_stats = Stats();
  out::reset();
}

}  // namespace ins
}  // namespace core
