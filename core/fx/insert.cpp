// core/fx/insert.cpp - see insert.h: the values (knobs, p-locks, LFO offsets -> ins::control.play), the names, the
// console, the card (per kit), and the DSP hooks the render cores and the send FX's core call. CORE_POLY builds only
// (the Pi and the Mac).
#include "fx/insert.h"

#include <string.h>

#include "fx/out.h"
#include "fx/send_fx.h"
#include "platform.h"

namespace core {
namespace ins {

Control control;

namespace {

const Labels kLabels[kTypes] = {
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
};
const char* const kChainNames[kChains] = {"V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "B1", "B2", "B3"};

uint8_t g_knob[kChains][kSlots][kParams];
int16_t g_lock[kVoices][kSlots][kParams];   // -1 none
int16_t g_lfo[kChains][kSlots][kParams];
// ins1112, the POST EQ: the knobs as set (0..127, the UI's units) and the LFOs' offsets. No step locks: the lock
// param space is exactly full at kLockParams = 32 (seq.h), so the post EQ takes LFOs and the knob, not p-locks.
uint8_t g_eq[kChains][kEqParams];
int16_t g_eq_lfo[kChains][kEqParams];
const Labels kEqLabels = {"POST EQ", {"HPF", "LPF", "LOW", "HIGH"}, {0, 0, 64, 64}};
Stats g_stats;
bool g_dirty = false;
uint32_t g_dirty_ms = 0;

Engine g_engine(48000.0f);
#ifndef CORE_HEAP_BUFFERS
float g_lines[kLines][kLine];
struct Placer {
  Placer() { g_engine.Place(g_lines); }
} g_placer;
#endif

struct LockInit {   // no step locks at boot (a 0 would be a lock to 0)
  LockInit() {
    for (auto& a : g_lock)
      for (auto& b : a)
        for (int16_t& x : b) x = -1;
    for (auto& a : g_eq)
      for (int k = 0; k < kEqParams; ++k) a[k] = kEqFlat(k);
  }
} g_lock_init;

void changed() { g_dirty = true, g_dirty_ms = now_ms(); }
struct OutHook {   // OUT's settings live in the INSERT file (out.h)
  OutHook() { out::on_change = changed; }
} g_out_hook;

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void refresh(int c, int s, int k) {
  int v = c < kVoices && g_lock[c][s][k] >= 0 ? g_lock[c][s][k] : g_knob[c][s][k];
  control.play[c][s][k] = (uint8_t)clampi(v + g_lfo[c][s][k], 0, 127);
}
bool valid(int c, int s, int k = 0) { return c >= 0 && c < kChains && s >= 0 && s < kSlots && k >= 0 && k < kParams; }
bool eq_valid(int c, int k) { return c >= 0 && c < kChains && k >= 0 && k < kEqParams; }
void eq_refresh(int c, int k) {
  control.peq[c][k] = (int8_t)eq_store(k, clampi((int)g_eq[c][k] + g_eq_lfo[c][k], 0, 127));
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

void clear_settings() {
  for (int c = 0; c < kChains; ++c)
    for (int s = 0; s < kSlots; ++s) {
      control.type[c][s] = kOff;
      for (int k = 0; k < kParams; ++k) g_knob[c][s][k] = 0, refresh(c, s, k);
    }
  for (int c = 0; c < kChains; ++c)
    for (int k = 0; k < kEqParams; ++k) g_eq[c][k] = kEqFlat(k), eq_refresh(c, k);   // ins1112
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b) control.send[v][b] = 0;
  control.route3 = 0;
}

// bus 3's soft limit, as the send FX's pairs (send_fx.h kLimitFx / kLimitMain)
inline float lim(float x, float l) { return fx::soft_limit(x, l); }

float g_send_g[kVoices][kBuses];   // the OUT fill's ramped send gains (core 0)

}  // namespace

// ---------------------------------------------------------------------------------------------------- names

const Labels& labels(int t) { return kLabels[t >= 0 && t < kTypes ? t : 0]; }
const char* type_name(int t) { return t >= 0 && t < kTypes ? kLabels[t].name : "?"; }
int type_from_name(const char* s) {
  for (int t = 0; t < kTypes; ++t)
    if (same_ci(s, kLabels[t].name)) return t;
  if (same_ci(s, "TREM")) return kTrem;
  if (same_ci(s, "RING")) return kRing;
  int v = num(s);
  return v >= 0 && v < kTypes ? v : -1;
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
    default: break;
  }
  sniprintf(b, cap, CORE_STR("%d"), v);
}

void slot_value_text(int c, int s, int k, int v, char* b, int cap) {
  int t = valid(c, s, k) ? control.type[c][s] : kOff;
  if (t == kAmp && k == 0) return amp::knob_text(c, s, v, b, cap);
  value_text(t, k, v, b, cap);
}

// ---------------------------------------------------------------------------------------------------- settings

void set_type(int c, int s, int t, bool keep) {
  if (!valid(c, s) || t < 0 || t >= kTypes) return;
  if (t == kAmp && c >= kVoices) return;   // AMP: the voice chains only (amp.h)
  if (control.type[c][s] == t && keep) return;
  control.type[c][s] = (uint8_t)t;
  if (!keep)
    for (int k = 0; k < kParams; ++k) g_knob[c][s][k] = kLabels[t].def[k];
  for (int k = 0; k < kParams; ++k) refresh(c, s, k);
  ++g_stats.sets;
  changed();
}
void set_param(int c, int s, int k, int v) {
  if (!valid(c, s, k)) return;
  g_knob[c][s][k] = (uint8_t)clampi(v, 0, 127);
  refresh(c, s, k);
  ++g_stats.sets;
  changed();
}
void set_send(int vi, int b, int v) {
  if (vi < 0 || vi >= kVoices || b < 0 || b >= kBuses) return;
  control.send[vi][b] = (uint8_t)clampi(v, 0, 127);
  ++g_stats.sets;
  changed();
}
void set_route3(int post) {
  control.route3 = post ? 1 : 0;
  changed();
}
int param(int c, int s, int k) { return valid(c, s, k) ? g_knob[c][s][k] : 0; }
// ---- ins1112, the POST EQ (end of the chain, after both slots)
const Labels& eq_labels() { return kEqLabels; }
void set_eq(int c, int k, int v) {
  if (!eq_valid(c, k)) return;
  g_eq[c][k] = (uint8_t)clampi(v, 0, 127);
  eq_refresh(c, k);
  ++g_stats.sets;
  changed();
}
int eq_param(int c, int k) { return eq_valid(c, k) ? g_eq[c][k] : 0; }
int eq_playing(int c, int k) { return eq_valid(c, k) ? eq_knob(k, control.peq[c][k]) : 0; }
void eq_lfo_offset(int c, int k, int off) {
  if (!eq_valid(c, k)) return;
  g_eq_lfo[c][k] = (int16_t)clampi(off, -127, 127);
  eq_refresh(c, k);
  ++g_stats.lfo_sets;
}
bool eq_on(int c) {
  if (!eq_valid(c, 0)) return false;
  for (int k = 0; k < kEqParams; ++k)
    if (g_eq[c][k] != kEqFlat(k)) return true;
  return false;
}
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
void lock(int vi, int s, int k, int v) {
  if (vi < 0 || vi >= kVoices || !valid(vi, s, k)) return;
  g_lock[vi][s][k] = (int16_t)(v < 0 ? -1 : clampi(v, 0, 127));
  refresh(vi, s, k);
  ++g_stats.locks;
}
int locked(int c, int s, int k) { return c >= 0 && c < kVoices && valid(c, s, k) ? g_lock[c][s][k] : -1; }
void lfo_offset(int c, int s, int k, int off) {
  if (!valid(c, s, k)) return;
  g_lfo[c][s][k] = (int16_t)clampi(off, -127, 127);
  refresh(c, s, k);
  ++g_stats.lfo_sets;
}
void touch() { changed(); }
bool any_on() {
  for (int c = 0; c < kChains; ++c)
    for (int s = 0; s < kSlots; ++s)
      if (control.type[c][s]) return true;
  return eq_any_on();   // ins1112
}

// ---------------------------------------------------------------------------------------------------- the DSP hooks

Engine& engine() { return g_engine; }

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
void voice_out(int vi, int16_t (*ring)[kVoices], const int* row, int n, int16_t (*ring_r)[kVoices]) {
  if (vi < 0 || vi >= kVoices) return;
  amp::service(vi);   // an AMP capture offered to a slot not playing AMP: taken now
  if (!ring_r) g_engine.MarkMono(vi);
  if (!g_engine.Active(vi, control)) return;
  float x[64], r[64];
  if (n > 64) n = 64;
  for (int i = 0; i < n; ++i) x[i] = ring[row[i]][vi] * (1.0f / 32768.0f);
  if (ring_r) {
    for (int i = 0; i < n; ++i) r[i] = ring_r[row[i]][vi] * (1.0f / 32768.0f);
    g_engine.ProcessVoiceStereo(vi, x, r, n, control);
    for (int i = 0; i < n; ++i) ring[row[i]][vi] = knee16(x[i]), ring_r[row[i]][vi] = knee16(r[i]);
  } else {
    g_engine.ProcessMono(vi, x, n, control);
    for (int i = 0; i < n; ++i) ring[row[i]][vi] = knee16(x[i]);
  }
  __atomic_fetch_add(&g_stats.voice_blocks, 1u, __ATOMIC_RELAXED);
}

bool sends_on() {
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b)
      if (control.send[v][b] || g_send_g[v][b] != 0.0f) return true;
  return false;
}

void voice_sends(const int16_t (*V)[kVoices], int n, float (*out)[kBuses]) {
  for (int i = 0; i < n; ++i) out[i][0] = out[i][1] = out[i][2] = 0.0f;
  if (n <= 0) return;
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b) {
      float t = knob01(control.send[v][b]);
      t *= t;   // squared, as the send FX's sends
      float g0 = g_send_g[v][b];
      if (g0 == 0.0f && t == 0.0f) continue;
      // toward the target over ~5 ms (a block is 7 frames on the Pi): linear within the block
      float g1 = g0 + (t - g0) * fminf(1.0f, (float)n / 240.0f);
      if (fabsf(g1 - t) < 1e-5f) g1 = t;
      g_send_g[v][b] = g1;
      const float dg = (g1 - g0) / (float)n;
      float g = g0;
      for (int i = 0; i < n; ++i) g += dg, out[i][b] += g * (float)V[i][v] * (1.0f / 32768.0f);
    }
}

bool bus3_on() {
  bool send = false;
  for (int v = 0; v < kVoices; ++v) send |= control.send[v][2] != 0;
  return (send && (control.type[kBus0 + 2][0] || control.type[kBus0 + 2][1] || eq_on(kBus0 + 2))) ||
         g_engine.Active(kBus0 + 2, control);
}

void bus3(const float* in, int n, float (*pre)[2], float (*post)[2]) {
  if (!g_engine.Active(kBus0 + 2, control)) return;
  float y[128][2];
  if (n > 128) n = 128;
  for (int i = 0; i < n; ++i) y[i][0] = y[i][1] = in[i];
  g_engine.ProcessStereo(kBus0 + 2, y, n, control);
  bool p = control.route3 != 0;
  float (*d)[2] = p ? post : pre;
  const float l = p ? fx::kLimitMain : fx::kLimitFx;
  if (!d) return;
  for (int i = 0; i < n; ++i) d[i][0] = lim(d[i][0] + y[i][0], l), d[i][1] = lim(d[i][1] + y[i][1], l);
  ++g_stats.bus_blocks;
}

// the send FX's hook (send_fx.h fx::bus_insert): FX slot s's return through bus s's chain
static void bus_hook(int s, float (*y)[2], int n) {
  if (s < 0 || s > 1 || !g_engine.Active(kBus0 + s, control)) return;
  g_engine.ProcessStereo(kBus0 + s, y, n, control);
  ++g_stats.bus_blocks;
}
static struct BusHook {
  BusHook() { fx::bus_insert = bus_hook; }
} g_bus_hook;

// the DELAY lines, then AMP's engine pool (amp.h; on the Mac: its own static memory)
unsigned long heap_bytes() { return sizeof(float) * kLines * kLine + amp::heap_bytes(); }
void heap_place(void* mem) {
  memset(mem, 0, heap_bytes());
  g_engine.Place((float(*)[kLine])mem);
  amp::heap_place((uint8_t*)mem + sizeof(float) * kLines * kLine);
}

// ---------------------------------------------------------------------------------------------------- the card's hooks

bool settings_dirty(uint32_t* since) {
  if (since) *since = g_dirty_ms;
  return g_dirty;
}
static int g_kit_now = -1;
int kit() { return g_kit_now; }
void set_kit(int k) { g_kit_now = k; }
void settings_clean() { g_dirty = false; }
void settings_due() { g_dirty_ms = now_ms() - 1000; }
void clear() {
  clear_settings();
  out::clear();
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s) amp::want_file(c, s, "");
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s)
      for (int k = 0; k < kParams; ++k) g_lock[c][s][k] = -1, refresh(c, s, k);
}

// ---------------------------------------------------------------------------------------------------- console

bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "ins", 3) != 0 || (p[3] && p[3] != '=')) return false;
  if (!p[3]) {
    int n = sniprintf(o, cap, "ins:");
    for (int c = 0; c < kChains && n < cap; ++c) {
      int a = control.type[c][0], b = control.type[c][1];
      bool send = c < kVoices && (control.send[c][0] || control.send[c][1] || control.send[c][2]);
      if (!a && !b && !send && !eq_on(c)) continue;
      int k = sniprintf(o + n, cap - n, CORE_STR(" %s %s/%s%s"), kChainNames[c], kLabels[a].name, kLabels[b].name,
                        eq_on(c) ? "/EQ" : "");
      if (k < 0 || k >= cap - n) break;
      n += k;
      if (send) {
        k = sniprintf(o + n, cap - n, CORE_STR(" send %d/%d/%d"), control.send[c][0], control.send[c][1], control.send[c][2]);
        if (k < 0 || k >= cap - n) break;
        n += k;
      }
      k = sniprintf(o + n, cap - n, ",");
      if (k < 0 || k >= cap - n) break;
      n += k;
    }
    if (n < cap) sniprintf(o + n, cap - n, CORE_STR(" b3 %s, faults %u"), control.route3 ? "post" : "pre",
                           (unsigned)g_engine.Faults());
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
      "ins=C:S:type:T|p1..p4:V|off (C V1..V8 B1..B3, S A|B), ins=C:eq:hpf|lpf|low|high:V|off, ins=Vn:send:B:V (B 1..3), "
      "ins=b3:route:pre|post, ins=reset";
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
    set_send(c, bus, v);
    sniprintf(o, cap, CORE_STR("ins: %s send %d/%d/%d"), kChainNames[c], control.send[c][0], control.send[c][1], control.send[c][2]);
    return true;
  }
  if (same_ci(b, "route") && c == kBus0 + 2) {
    set_route3(same_ci(key, "post"));
    sniprintf(o, cap, CORE_STR("ins: B3 returns %s"), control.route3 ? "POST (0/1)" : "PRE (2/3)");
    return true;
  }
  if (same_ci(b, "eq")) {   // ins1112: ins=V3:eq:hpf:40, ins=b1:eq:lpf:90, ins=V3:eq:off
    static const char* const kK[kEqParams] = {"hpf", "lpf", "low", "high"};
    if (same_ci(key, "off")) {
      for (int k = 0; k < kEqParams; ++k) set_eq(c, k, kEqFlat(k));
    } else {
      int k = -1;
      for (int j = 0; j < kEqParams; ++j)
        if (same_ci(key, kK[j])) k = j;
      int v = num(val);
      if (k < 0 || v < 0) {
        sniprintf(o, cap, "%s", kHelp);
        return true;
      }
      set_eq(c, k, v);
    }
    const Labels& L = eq_labels();
    char v4[kEqParams][8];
    for (int k = 0; k < kEqParams; ++k) eq_value_text(k, eq_param(c, k), v4[k], sizeof v4[k]);
    sniprintf(o, cap, CORE_STR("ins: %s POST EQ %s %s %s %s %s %s %s %s"), kChainNames[c], L.label[0], v4[0], L.label[1],
              v4[1], L.label[2], v4[2], L.label[3], v4[3]);
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
      sniprintf(o, cap, CORE_STR("ins: no type \"%s\" (OFF FILTER CRUSH DRIVE EQ COMP DELAY TREMOLO RINGMOD AMP)"), val);
      return true;
    }
    if (t == kAmp && c >= kVoices) {
      sniprintf(o, cap, "ins: AMP is for the voice chains (V1..V8), not the buses");
      return true;
    }
    set_type(c, s, t);
  } else if ((key[0] == 'p' || key[0] == 'P') && key[1] >= '1' && key[1] <= '4' && !key[2]) {
    int v = num(val);
    if (v < 0) {
      sniprintf(o, cap, "%s", kHelp);
      return true;
    }
    set_param(c, s, key[1] - '1', v);
  } else {
    sniprintf(o, cap, "%s", kHelp);
    return true;
  }
  int t = control.type[c][s];
  char v4[4][8];
  for (int k = 0; k < kParams; ++k) slot_value_text(c, s, k, g_knob[c][s][k], v4[k], sizeof v4[k]);
  sniprintf(o, cap, CORE_STR("ins: %s %c %s %s %s %s %s %s %s %s %s"), kChainNames[c], 'A' + s, kLabels[t].name,
            kLabels[t].label[0], v4[0], kLabels[t].label[1], v4[1], kLabels[t].label[2], v4[2], kLabels[t].label[3], v4[3]);
  return true;
}

int json(char* o, int cap) {
  int n = sniprintf(o, cap, CORE_STR("\"ins\":{\"kit\":%d,\"route3\":%d,\"c\":["), kit() + 1, control.route3);
  for (int c = 0; c < kChains && n < cap; ++c) {
    int k = sniprintf(o + n, cap - n, CORE_STR("%s{\"n\":\"%s\",\"s\":["), c ? "," : "", kChainNames[c]);
    if (k < 0 || k >= cap - n) return cap;
    n += k;
    for (int s = 0; s < kSlots; ++s) {
      int t = control.type[c][s];
      k = sniprintf(o + n, cap - n, CORE_STR("%s{\"t\":\"%s\",\"p\":[%d,%d,%d,%d],\"l\":[%d,%d,%d,%d]}"), s ? "," : "",
                    kLabels[t].name, g_knob[c][s][0], g_knob[c][s][1], g_knob[c][s][2], g_knob[c][s][3],
                    control.play[c][s][0], control.play[c][s][1], control.play[c][s][2], control.play[c][s][3]);
      if (k < 0 || k >= cap - n) return cap;
      n += k;
    }
    k = sniprintf(o + n, cap - n, CORE_STR("],\"peq\":[%d,%d,%d,%d],\"peql\":[%d,%d,%d,%d]"),   // ins1112: set, playing
                  eq_param(c, 0), eq_param(c, 1), eq_param(c, 2), eq_param(c, 3), eq_playing(c, 0), eq_playing(c, 1),
                  eq_playing(c, 2), eq_playing(c, 3));
    if (k < 0 || k >= cap - n) return cap;
    n += k;
    if (c < kVoices)
      k = sniprintf(o + n, cap - n, CORE_STR(",\"send\":[%d,%d,%d]}"), control.send[c][0], control.send[c][1], control.send[c][2]);
    else
      k = sniprintf(o + n, cap - n, "}");
    if (k < 0 || k >= cap - n) return cap;
    n += k;
  }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, "],");
  if (k > 0) n += k;
  if (n < cap) n += out::json(o + n, cap - n);
  k = sniprintf(o + n, n < cap ? cap - n : 0, ",");
  if (k > 0) n += k;
  if (n < cap) n += amp::json(o + n, cap - n);
  k = sniprintf(o + n, n < cap ? cap - n : 0, "}");
  return n + (k > 0 ? k : 0);
}

const Stats& stats() { return g_stats; }

void reset() {
  clear_settings();
  for (int c = 0; c < kChains; ++c)
    for (int s = 0; s < kSlots; ++s)
      for (int k = 0; k < kParams; ++k) {
        if (c < kVoices) g_lock[c][s][k] = -1;
        g_lfo[c][s][k] = 0;
        refresh(c, s, k);
      }
  for (int c = 0; c < kChains; ++c)
    for (int k = 0; k < kEqParams; ++k) g_eq_lfo[c][k] = 0, eq_refresh(c, k);   // ins1112
  for (int v = 0; v < kVoices; ++v)
    for (int b = 0; b < kBuses; ++b) g_send_g[v][b] = 0.0f;
  g_engine.Reset();
  amp::reset();
  g_stats = Stats();
  out::reset();
  g_dirty = false;
}

}  // namespace ins
}  // namespace core
