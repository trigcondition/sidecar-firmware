// core/fx/master.cpp - see master.h.
#include "fx/master.h"

#include <string.h>

#include <new>

#include "fx/out_mix.h"
#include "fx/send_fx.h"
#include "mod/target.h"
#include "platform.h"

namespace core {
namespace master {

Control control;
volatile float lim_gain = 1.0f;

namespace {

// the inserts' SendFx: booking f plays on g_fx[f / fx::kSlots]'s slot f % fx::kSlots
static_assert(kFxSlots == kFxUnits * fx::kSlots, "two slots a SendFx");
fx::SendFx* volatile g_fx[kFxUnits] = {};
fx::Control g_ctl[kFxUnits];          // their knobs: only knob[][] (from the slots), kill and clock (copied)
float g_mix[kAllSlots];               // the render core's: each slot's MIX, smoothed
bool g_mix_set[kAllSlots];
int8_t g_run_fx[kAllSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int8_t g_play[kAllSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // the room each slot played on its last chunk
int16_t g_waited[kAllSlots];          // the frames a slot has waited for its room (the render core's, as these two)
// the longest wait for a room's last user to fade out: its fade and as much again (a holder frozen meanwhile, bus 1 / 2
// with its send FX off, plays no more and is let go then), counted in the waiting slot's own frames
constexpr int kWaitFrames = 2 * ins::kFadeFrames;
volatile float g_peak_in, g_peak_out;
float g_lev = 1.0f;

constexpr uint8_t kSendId[ins::kAllTypes - ins::kTypes] = {fx::kChorus, fx::kComb, fx::kTape, fx::kRings, fx::kCloudSeed, fx::kHalo};

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
bool same_ci(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}
int num(const char* p) {
  if (*p < '0' || *p > '9') return -1;
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  return v;
}
const char* field(const char* p, char* b, int cap) {
  int n = 0;
  while (*p && *p != ':') {
    if (n < cap - 1) b[n++] = *p;
    ++p;
  }
  b[n] = 0;
  return *p == ':' ? p + 1 : p;
}
// a trim's word: 0 or -N (dB); 1: not one
int trim_word(const char* p) {
  if (*p == '-') return num(p + 1) >= 0 ? -num(p + 1) : 1;
  return num(p) == 0 ? 0 : 1;
}
void book(int i, int f) { control.fx[i] = (int8_t)f; }
int free_fx() {
  for (int f = 0; f < kFxSlots; ++f) {
    bool used = false;
    for (int i = 0; i < kAllSlots; ++i) used |= control.fx[i] == f;
    if (!used) return f;
  }
  return -1;
}
void sfx() {
#ifndef CORE_HEAP_BUFFERS
  for (int u = 0; u < kFxUnits; ++u)
    if (!g_fx[u]) g_fx[u] = new fx::SendFx(48000.0f);
#endif
}

// room f played on its last chunk by a slot other than i that has left it (fading out after its TYPE went, a kit
// change). A slot waiting for its own room plays none, so two slots whose rooms swap never wait for each other.
bool held_by_other(int f, int i) {
  for (int k = 0; k < kAllSlots; ++k)
    if (k != i && g_play[k] == f && control.fx[k] != f) return true;
  return false;
}

// the slot's send TYPE over m <= ins::kChunk frames: the sum in, the return out, MIX the crossfade (127: only the return)
void send_run(int c, int s, const float* p, const float* x, const float* r, float* y0, float* y1, int m) {
  // the TYPE playing and the SendFx slot it plays on, also while the slot fades out after the TYPE was changed or cleared
  // (a voice chain never gets here: accept refuses it, and slot_index's -1 is no slot of these arrays)
  const int i = slot_index(c, s), t = ins::engine().slot(c, s).cur;
  float in[ins::kChunk], w[ins::kChunk][2];
  if (i < 0) {
    for (int j = 0; j < m; ++j) y0[j] = x[j];
    if (r)
      for (int j = 0; j < m; ++j) y1[j] = r[j];
    return;
  }
  const int want = control.fx[i];
  bool wait = false;
  if (want >= 0 && want < kFxSlots && g_run_fx[i] != want) {   // a room newly taken
    if (held_by_other(want, i) && g_waited[i] < kWaitFrames) {
      wait = true;   // its last user still fades out on it: this slot passes its input dry until that is done
      g_waited[i] = (int16_t)(g_waited[i] + m);
    } else {         // from clean, never the last user's tail; a frozen last user lets it go (dry when it runs again)
      for (int k = 0; k < kAllSlots; ++k)
        if (g_run_fx[k] == want) g_run_fx[k] = -1, g_play[k] = -1;
      g_run_fx[i] = (int8_t)want;
      if (g_fx[want / fx::kSlots]) g_fx[want / fx::kSlots]->Drop(want % fx::kSlots);
      if (g_waited[i]) g_mix[i] = 0.0f, g_mix_set[i] = true;   // it waited dry: MIX fades in from there (the one-pole below), no step
    }
  }
  if (!wait) g_waited[i] = 0;
  const int f = wait ? -1 : g_run_fx[i], u = f / fx::kSlots, fs = f % fx::kSlots;
  for (int j = 0; j < m; ++j) in[j] = r ? 0.5f * (x[j] + r[j]) : x[j];
  if (f >= 0 && f < kFxSlots && g_fx[u]) {
    fx::Control& k = g_ctl[u];
    k.knob[fs][0] = t >= ins::kTypes && t < ins::kAllTypes ? kSendId[t - ins::kTypes] : (uint8_t)fx::kOff;
    k.knob[fs][1] = 0;
    for (int q = 0; q < ins::kParams; ++q) k.knob[fs][2 + q] = (uint8_t)(p[q] * 127.0f + 0.5f);
    k.kill = fx::control.kill;
    k.clock = fx::control.clock;
    g_fx[u]->Insert(fs, in, m, w, k);
    g_play[i] = (int8_t)f;
  } else {
    g_play[i] = -1;   // no room to play on (waiting, or let go): the input as it is, so MIX changes nothing
    for (int j = 0; j < m; ++j) w[j][0] = x[j], w[j][1] = r ? r[j] : x[j];
  }
  const float tgt = ins::knob01((uint8_t)played_mix(i));   // the knob + core/mod's offset
  if (!g_mix_set[i]) g_mix[i] = tgt, g_mix_set[i] = true;
  const float a = g_mix[i], b = a + (tgt - a) * ((float)m / ((float)m + 144.0f));   // 3 ms a chunk, as a slot's params
  g_mix[i] = fabsf(b - tgt) < 1e-4f ? tgt : b;
  const float d = (g_mix[i] - a) / (float)m;
  float q = a;
  for (int j = 0; j < m; ++j) {
    q += d;
    y0[j] = x[j] + q * (w[j][0] - x[j]);
    if (r) y1[j] = r[j] + q * (w[j][1] - r[j]);
  }
  // the room left and the fade-out done (this chunk ended dry): the slot holds nothing; taken again, it starts from clean
  if (want < 0 && ins::engine().slot(c, s).feed1 <= 0.0f) g_run_fx[i] = -1, g_play[i] = -1;
}

struct Hook {
  Hook() {
    ins::send_insert = send_run;
    for (int i = 0; i < kAllSlots; ++i) control.mix[i] = kMixDef, control.fx[i] = -1;
    control.lev = kLevUnity, control.trim = kTrimDef;
  }
} g_hook;

}  // namespace

bool send_type(int t) { return t >= ins::kTypes && t < ins::kAllTypes; }

bool on() {
  for (int i = 0; i < kSlots; ++i)
    if (ins::control.type[chain(i)][slot(i)]) return true;
  for (int c = ins::kM0; c < ins::kChains; ++c)
    if (ins::peq_set(ins::control, c)) return true;
  return false;
}

int fx_used() {
  int n = 0;
  for (int i = 0; i < kAllSlots; ++i) n += control.fx[i] >= 0;
  return n;
}

bool room(int i) { return i >= 0 && i < kAllSlots && (control.fx[i] >= 0 || free_fx() >= 0); }

int full_text(int t, char* b, int cap) {
  return sniprintf(b, cap, CORE_STR("%s: %d send FX inserts at most, the master and the buses together (%d in use)"),
                   ins::type_name(t), kFxSlots, fx_used());
}

bool accept(int c, int s, int t) {
  const int i = slot_index(c, s);
  if (i < 0) return !send_type(t);   // a voice chain: never a send TYPE (core 1 renders voices 1..4)
  if (!send_type(t)) {
    if (control.fx[i] >= 0) book(i, -1);
    return true;
  }
  if (control.fx[i] >= 0) return true;   // it holds one already: the new TYPE follows it there
  int f = free_fx();
  if (f < 0) return false;
  book(i, f);
#ifndef CORE_HEAP_BUFFERS
  sfx();
#endif
  return true;
}

void set_byp(int v) {
  control.byp = v ? 1 : 0;
  ins::touch();
}
void set_src(int v) {
  control.src = v == kMix ? kMix : kRet;
  ins::touch();
}
void set_lev(int v) {
  control.lev = (uint8_t)clampi(v, 0, 127);
  ins::touch();
}
void mod_offset(int param, int off) {   // never saved, no touch: the knobs are the file's
  int8_t o = (int8_t)clampi(off, -127, 127);
  if (param == 0) control.mod_lev = o;
  else if (param >= 1 && param <= kSlots) control.mod_mix[param - 1] = o;
}
void set_trim(int db) {
  control.trim = (uint8_t)clampi(-db, 0, kTrimMax);
  ins::touch();
}
void set_mix(int i, int v) {
  if (i < 0 || i >= kAllSlots) return;
  control.mix[i] = (uint8_t)clampi(v, 0, 127);
  ins::touch();
}
void set_pair(int i, int v) {
  if (i < 0 || i >= kSlots) return;
  ins::control.mpair[i / ins::kSlots][slot(i)] = v ? 1 : 0;
  ins::touch();
}
bool pair(int i) { return i >= 0 && i < kSlots && ins::control.mpair[i / ins::kSlots][slot(i)]; }

int next_type(int i, int t, int d) {
  const int n = ins::kAllTypes;
  const bool bus = i >= kSlots;
  for (int k = 1; k < n; ++k) {
    const int v = (t + d * k + n * 2) % n;
    if (bus && v == ins::kAmp) continue;
    if (!send_type(v) || room(i)) return v;
  }
  return t;
}

bool zone_text(int t, int k, int v, char* b, int cap) {
  if (!send_type(t) || k < 0 || k > 3) return false;
  const fx::KnobLabels* l = fx::labels(kSendId[t - ins::kTypes]);
  if (!l) return false;
  const fx::Zone* z = nullptr;
  for (int j = 0; j < l->nzones; ++j)
    if (l->zones[j].knob == k && l->zones[j].lo <= v) z = &l->zones[j];
  if (!z) return false;
  if (z->show) sniprintf(b, cap, CORE_STR("%s %d"), z->name, v - z->lo + z->base);
  else sniprintf(b, cap, "%s", z->name);
  return true;
}

void clear_master() {
  control.byp = 0, control.src = kRet, control.lev = kLevUnity, control.trim = kTrimDef;
  for (int i = 0; i < kSlots; ++i) control.mix[i] = kMixDef, control.fx[i] = -1;
  for (int c = 0; c < ins::kMasters; ++c)
    for (int s = 0; s < ins::kSlots; ++s) ins::control.mpair[c][s] = 0;
}
void clear() {
  clear_master();
  for (int i = kSlots; i < kAllSlots; ++i) control.mix[i] = kMixDef, control.fx[i] = -1;
}

// The Rytm's route byte 0x80006466, a set bit: that voice off the main bus. Packed from physical voices 7,6,5,4,0,1,2,3, MSB
// first (sidecar-mods re/subsystems/compressor-hardware.md); here each stem's bit
static constexpr uint8_t kRouteBit[kStems] = {3, 1, 7, 2, 0, 5, 4, 6};

bool report(const uint8_t* d, int len) {
  if (len < 16 || d[4] != 0x50 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return false;
  const int route = (d[13] & 1) << 7 | (d[14] & 0x7F);
  int off = 0;
  for (int v = 0; v < kStems; ++v) {
    control.pan[v] = d[5 + v] & 0x7F;
    if ((route >> kRouteBit[v]) & 1) off |= 1 << v;
  }
  control.off_main = (uint8_t)off;
  control.known = 1;
  return true;
}

void link_lost() { control.known = 0; }

int stems_on_main() {
  if (!control.known) return 0;
  int n = 0;
  for (int v = 0; v < kStems; ++v) n += !((control.off_main >> v) & 1);
  return n;
}

void reset() {
  clear();
  control.mod_lev = 0;
  for (int i = 0; i < kSlots; ++i) control.mod_mix[i] = 0;
  for (int u = 0; u < kFxUnits; ++u)
    if (g_fx[u]) g_fx[u]->Reset();
  memset((void*)g_ctl, 0, sizeof g_ctl);
  for (int i = 0; i < kAllSlots; ++i) g_mix_set[i] = false, g_run_fx[i] = -1, g_play[i] = -1, g_waited[i] = 0;
  g_peak_in = g_peak_out = 0.0f;
  g_lev = 1.0f;
}

// ---------------------------------------------------------------------------------------------------- the audio side

void render(float (*y)[2], int n) {
  float pi = g_peak_in, po = g_peak_out;
  for (int i = 0; i < n; ++i) {
    float a = fabsf(y[i][0]), b = fabsf(y[i][1]);
    pi = fmaxf(pi, fmaxf(a, b));
  }
  amp::service_master();
  ins::Engine& e = ins::engine();
  for (int c = ins::kM0; c < ins::kChains; ++c)
    if (e.Active(c, ins::control)) e.ProcessStereo(c, y, n, ins::control);
  const float t = (float)played_lev() * (1.0f / kLevUnity), g0 = g_lev;   // the knob + core/mod's offset
  float g1 = g0 + (t - g0) * ((float)n / ((float)n + 144.0f));
  if (fabsf(g1 - t) < 1e-4f) g1 = t;
  g_lev = g1;
  const float dg = (g1 - g0) / (float)n;
  float g = g0;
  for (int i = 0; i < n; ++i) {
    g += dg;
    float l = out::Mixer::Limit(y[i][0] * g, kOutLimit), r = out::Mixer::Limit(y[i][1] * g, kOutLimit);
    y[i][0] = l, y[i][1] = r;
    po = fmaxf(po, fmaxf(fabsf(l), fabsf(r)));
  }
  g_peak_in = pi, g_peak_out = po;
}

int gr10() {
  const float g = lim_gain;
  return g < 1.0f ? (int)(-200.0f * log10f(g) + 0.5f) : 0;
}

void take_peaks(float* in, float* outp) {
  *in = g_peak_in, *outp = g_peak_out;
  g_peak_in = g_peak_out = 0.0f;
}

uint64_t bench(uint32_t* worst_block) {
  *worst_block = 0;
  if (on() || fx_used()) return 0;   // (the boot's, before a kit: nothing set anywhere, so the clear() below takes nothing)
#ifndef CORE_HEAP_BUFFERS
  sfx();
#endif
  if (!g_fx[0] || !g_fx[1]) return 0;
  static const uint8_t kT[kSlots] = {ins::kComp, ins::kEq, ins::kMSpace, ins::kMCloud};
  for (int i = 0; i < kSlots; ++i) {
    ins::control.type[chain(i)][slot(i)] = kT[i];
    for (int k = 0; k < ins::kParams; ++k) ins::control.play[chain(i)][slot(i)][k] = ins::labels(kT[i]).def[k];
  }
  control.fx[2] = 0, control.fx[3] = 1, control.mix[2] = control.mix[3] = 127;
  for (int k = 0; k < ins::kEqParams; ++k) ins::control.peq[kEqChain][k] = (int8_t)ins::eq_store(k, k < 2 ? 20 : 70);
  float y[12][2];
  uint64_t sum = 0;
  uint32_t worst = 0;
  for (int n = 0; n < 4000; ++n) {
    for (int i = 0; i < 12; ++i) {   // (the test signal outside the timing)
      float a = sinf(0.05f * (float)(n * 12 + i)), b = sinf(0.031f * (float)(n * 12 + i));
      y[i][0] = 0.2f * a + 0.1f * b, y[i][1] = 0.2f * a - 0.1f * b;
    }
    uint32_t t0 = cycles();
    render(y, 12);
    uint32_t t = cycles() - t0;
    sum += t;
    if (t > worst) worst = t;
  }
  *worst_block = worst;
  // every byte back to nothing, the fades and the left effects' clears run out (no file marked: the bytes were set directly)
  for (int i = 0; i < kSlots; ++i)
    for (int k = 0; k < ins::kParams; ++k) ins::control.type[chain(i)][slot(i)] = 0, ins::control.play[chain(i)][slot(i)][k] = 0;
  for (int k = 0; k < ins::kEqParams; ++k) ins::control.peq[kEqChain][k] = 0;
  for (int n = 0; n < 200; ++n) {
    memset(y, 0, sizeof y);
    render(y, 12);
  }
  clear();
  for (int u = 0; u < kFxUnits; ++u) g_fx[u]->Reset();
  return sum;
}

static const uint8_t kSfxSets[2][4] = {{ins::kMRings, ins::kMCloud, ins::kMSpace, ins::kMHalo},
                                       {ins::kMCloud, ins::kMCloud, ins::kMCloud, ins::kMCloud}};
const char* sfx_bench_name(int set) { return set == 0 ? "RINGS CLOUD SPACE HALO" : set == 1 ? "CLOUD x4" : nullptr; }

uint64_t sfx_bench(int set, uint32_t* worst_block) {
  *worst_block = 0;
  if (set < 0 || set > 1) return 0;
  for (int c = ins::kBus0; c < ins::kChains; ++c)   // the buses' and the master's slots all OFF, no room taken
    for (int s = 0; s < ins::kSlots; ++s)
      if (ins::control.type[c][s] != ins::kOff) return 0;
  if (fx_used()) return 0;
#ifndef CORE_HEAP_BUFFERS
  sfx();
#endif
  if (!g_fx[0] || !g_fx[1]) return 0;
  static ins::Control ic;   // (static: the boot stack is small)
  static Control mc;
  memcpy(&ic, (const void*)&ins::control, sizeof ic), memcpy(&mc, (const void*)&control, sizeof mc);
  static const int kC[4] = {ins::kBus0, ins::kBus0 + 1, ins::kBus0 + 2, ins::kM0};
  for (int i = 0; i < 4; ++i) {   // the bytes set directly (no file marked), rooms 0..3
    const int t = kSfxSets[set][i], j = slot_index(kC[i], 0);
    ins::control.type[kC[i]][0] = (uint8_t)t;
    for (int k = 0; k < ins::kParams; ++k) ins::control.play[kC[i]][0][k] = ins::labels(t).def[k];
    control.fx[j] = (int8_t)i, control.mix[j] = 64;
  }
  float b12[2][12][2], in3[12], pre[12][2], post[12][2], y[12][2];
  uint64_t sum = 0;
  uint32_t worst = 0;
  for (int n = 0; n < 4000; ++n) {
    for (int i = 0; i < 12; ++i) {   // (the test signal outside the timing)
      const float a = sinf(0.05f * (float)(n * 12 + i)), b = sinf(0.031f * (float)(n * 12 + i));
      b12[0][i][0] = b12[1][i][1] = 0.2f * a, b12[0][i][1] = b12[1][i][0] = 0.1f * b;
      in3[i] = 0.15f * (a + b), y[i][0] = 0.2f * a + 0.1f * b, y[i][1] = 0.2f * a - 0.1f * b;
    }
    memset(pre, 0, sizeof pre), memset(post, 0, sizeof post);
    const uint32_t t0 = cycles();
    if (fx::bus_insert) fx::bus_insert(0, b12[0], 12), fx::bus_insert(1, b12[1], 12);
    ins::bus3(in3, 12, pre, post);
    render(y, 12);
    const uint32_t t = cycles() - t0;
    sum += t;
    if (t > worst) worst = t;
  }
  *worst_block = worst;
  // every byte as it was, the fades run out on silence, the render side and the rooms back to nothing
  memcpy((void*)&ins::control, &ic, sizeof ic), memcpy((void*)&control, &mc, sizeof mc);
  for (int n = 0; n < 200; ++n) {
    memset(b12, 0, sizeof b12), memset(in3, 0, sizeof in3), memset(y, 0, sizeof y);
    if (fx::bus_insert) fx::bus_insert(0, b12[0], 12), fx::bus_insert(1, b12[1], 12);
    ins::bus3(in3, 12, pre, post);
    render(y, 12);
  }
  for (int i = 0; i < kAllSlots; ++i) g_mix_set[i] = false, g_run_fx[i] = -1, g_play[i] = -1, g_waited[i] = 0;
  for (int u = 0; u < kFxUnits; ++u) g_fx[u]->Reset();
  return sum;
}

static constexpr unsigned long kUnitBytes = ((unsigned long)sizeof(fx::SendFx) + 63) & ~63ul;
unsigned long heap_bytes() {
#ifdef CORE_HEAP_BUFFERS
  return kFxUnits * kUnitBytes + 64;
#else
  return 0;
#endif
}
void heap_place(void* mem) {
#ifdef CORE_HEAP_BUFFERS
  uintptr_t a = ((uintptr_t)mem + 63) & ~(uintptr_t)63;
  for (int u = 0; u < kFxUnits; ++u) g_fx[u] = new ((void*)(a + u * kUnitBytes)) fx::SendFx(48000.0f);
#else
  (void)mem;
#endif
}

// ---------------------------------------------------------------------------------------------------- console, json, card

static int slot_line(int i, char* o, int cap) {
  const int c = chain(i), s = slot(i), t = ins::control.type[c][s];
  const ins::Labels& L = ins::labels(t);
  int n = sniprintf(o, cap, CORE_STR(" %d %s"), i + 1, L.name);
  if (t == ins::kAmp && pair(i)) n += sniprintf(o + n, n < cap ? cap - n : 0, " L/R");
  if (send_type(t)) n += sniprintf(o + n, n < cap ? cap - n : 0, CORE_STR(" mix %d"), control.mix[i]);
  return n;
}

bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "mfx", 3) != 0 || (p[3] && p[3] != '=') || !strcmp(p, "mfx=stats")) return false;   // (stats: the board's)
  static const char kHelp[] =
      "mfx=byp:0|1, mfx=src:ret|mix, mfx=lev:V, mfx=trim:0..-24, mfx=S:type:T|p1..p4:V|mix:V|pair:0|1|off (S 1..4), mfx=eq:hpf|lpf|low|high:V|off, mfx=reset";
  if (p[3]) {
    const char* q = p + 4;
    char a[8], b[10], v[12];
    q = field(q, a, sizeof a);
    q = field(q, b, sizeof b);
    field(q, v, sizeof v);
    int i = a[0] >= '1' && a[0] < '1' + kSlots && !a[1] ? a[0] - '1' : -1;
    if (same_ci(a, "byp")) set_byp(num(b) > 0);
    else if (same_ci(a, "src")) {
      if (!same_ci(b, "ret") && !same_ci(b, "mix")) return sniprintf(o, cap, "%s", kHelp), true;
      set_src(same_ci(b, "mix") ? kMix : kRet);
    } else if (same_ci(a, "lev") && num(b) >= 0) set_lev(num(b)), fx::edited(mod::tid(mod::kMfx, 0, 0));
    else if (same_ci(a, "trim") && trim_word(b) <= 0) set_trim(trim_word(b));
    else if (same_ci(a, "reset")) {
      for (int k = 0; k < kSlots; ++k) ins::set_type(chain(k), slot(k), ins::kOff);
      for (int k = 0; k < ins::kEqParams; ++k) ins::set_eq(kEqChain, k, ins::kEqFlat(k));
      clear_master();   // the master's own: the buses' send TYPEs keep their rooms and MIX
      ins::touch();
    } else if (same_ci(a, "eq")) {
      static const char* const kK[ins::kEqParams] = {"hpf", "lpf", "low", "high"};
      if (same_ci(b, "off")) {
        for (int k = 0; k < ins::kEqParams; ++k) ins::set_eq(kEqChain, k, ins::kEqFlat(k));
      } else {
        int k = -1;
        for (int j = 0; j < ins::kEqParams; ++j)
          if (same_ci(b, kK[j])) k = j;
        if (k < 0 || num(v) < 0) return sniprintf(o, cap, "%s", kHelp), true;
        ins::set_eq(kEqChain, k, num(v)), fx::edited(mod::tid(mod::kIns, kEqChain, 8 + k));
      }
    } else if (i >= 0) {
      const int c = chain(i), s = slot(i);
      if (same_ci(b, "off")) ins::set_type(c, s, ins::kOff);
      else if (same_ci(b, "type")) {
        int t = ins::type_from_name(v);
        if (t < 0) return sniprintf(o, cap, "mfx: no type \"%s\"", v), true;
        ins::set_type(c, s, t);
        if (ins::control.type[c][s] != t) {
          int n = sniprintf(o, cap, "mfx: ");
          return full_text(t, o + n, cap - n), true;
        }
      } else if (same_ci(b, "mix") && num(v) >= 0) set_mix(i, num(v)), fx::edited(mod::tid(mod::kMfx, 0, 1 + i));
      else if (same_ci(b, "pair")) set_pair(i, num(v) > 0);
      else if ((b[0] == 'p' || b[0] == 'P') && b[1] >= '1' && b[1] <= '4' && !b[2] && num(v) >= 0)
        ins::set_param(c, s, b[1] - '1', num(v)), fx::edited(mod::tid(mod::kIns, c, s * 4 + b[1] - '1'));
      else return sniprintf(o, cap, "%s", kHelp), true;
    } else {
      return sniprintf(o, cap, "%s", kHelp), true;
    }
  }
  int n = sniprintf(o, cap, CORE_STR("mfx: %s, src %s, lev %d, trim %d:"), control.byp ? "BYPASS" : "on", control.src == kMix ? "MIX" : "RET", control.lev,
                    -(int)control.trim);
  for (int i = 0; i < kSlots && n < cap; ++i)
    if (ins::control.type[chain(i)][slot(i)]) n += slot_line(i, o + n, cap - n);
  if (n < cap && ins::eq_on(kEqChain)) n += sniprintf(o + n, cap - n, " EQ");
  return true;
}

int json(char* o, int cap) {
  const int gr = gr10();
  int n = sniprintf(o, cap, CORE_STR("\"mfx\":{\"byp\":%d,\"src\":\"%s\",\"lev\":%d,\"trim\":%d,\"gr\":%d.%d,\"on\":%d,\"s\":["), control.byp,
                    control.src == kMix ? "mix" : "ret", control.lev, -(int)control.trim, gr / 10, gr % 10, on() ? 1 : 0);
  for (int i = 0; i < kSlots && n < cap; ++i) {
    const int c = chain(i), s = slot(i), t = ins::control.type[c][s];
    const ins::Labels& L = ins::labels(t);
    char v[ins::kParams][8] = {};
    if (t)
      for (int j = 0; j < ins::kParams; ++j) ins::slot_value_text(c, s, j, ins::param(c, s, j), v[j], sizeof v[j]);
    int k = sniprintf(o + n, cap - n, CORE_STR("%s{\"t\":\"%s\",\"p\":[%d,%d,%d,%d],\"l\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"v\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"mix\":%d,\"pair\":%d,\"send\":%d}"),
                      i ? "," : "", ins::type_name(t), ins::param(c, s, 0), ins::param(c, s, 1), ins::param(c, s, 2), ins::param(c, s, 3),
                      L.label[0], L.label[1], L.label[2], L.label[3], v[0], v[1], v[2], v[3], control.mix[i], pair(i) ? 1 : 0, send_type(t) ? 1 : 0);
    if (k < 0 || k >= cap - n) return cap;
    n += k;
  }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, CORE_STR("],\"peq\":[%d,%d,%d,%d],\"fxcap\":%d,\"fxused\":%d,\"rep\":%d,\"onmain\":%d}"),
                    ins::eq_param(kEqChain, 0), ins::eq_param(kEqChain, 1), ins::eq_param(kEqChain, 2), ins::eq_param(kEqChain, 3),
                    kFxSlots, fx_used(), control.known, stems_on_main());
  return n + (k > 0 ? k : 0);
}

int line(char* b, int cap) {
  int pairs = 0;
  bool def = !control.byp && control.src == kRet && control.lev == kLevUnity && control.trim == kTrimDef;
  for (int i = 0; i < kSlots; ++i) {
    if (pair(i)) pairs |= 1 << i, def = false;
    def = def && control.mix[i] == kMixDef;
  }
  if (def) return 0;
  int n = sniprintf(b, cap, CORE_STR("MFX %d %s %d %d %d %d %d %d"), control.byp, control.src == kMix ? "MIX" : "RET", control.lev,
                    control.mix[0], control.mix[1], control.mix[2], control.mix[3], pairs);
  if (control.trim != kTrimDef) n += sniprintf(b + n, cap - n, CORE_STR(" %d"), -(int)control.trim);
  return n + sniprintf(b + n, cap - n, "\n");
}

bool from_line(const char* const* w, int n) {
  if (n > 1 && num(w[1]) >= 0) set_byp(num(w[1]));
  if (n > 2) set_src(same_ci(w[2], "MIX") ? kMix : kRet);
  if (n > 3 && num(w[3]) >= 0) set_lev(num(w[3]));
  for (int i = 0; i < kSlots && 4 + i < n; ++i)
    if (num(w[4 + i]) >= 0) set_mix(i, num(w[4 + i]));
  if (n > 8 && num(w[8]) >= 0)
    for (int i = 0; i < kSlots; ++i) set_pair(i, (num(w[8]) >> i) & 1);
  if (n > 9 && trim_word(w[9]) <= 0) set_trim(trim_word(w[9]));
  return true;
}

}  // namespace master
}  // namespace core
