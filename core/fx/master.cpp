// core/fx/master.cpp - see master.h.
#include "fx/master.h"

#include <string.h>

#include <new>

#include "fx/out.h"
#include "fx/out_mix.h"
#include "fx/send_fx.h"
#include "mod/target.h"
#include "platform.h"

namespace core {
namespace master {

Control control;
Control control1;   // slot 1's (master.h control_of)
volatile float lim_gain = 1.0f;
volatile float lim_gain1 = 1.0f;

namespace {

// the inserts' SendFx: booking f plays on g_fx[f / fx::kSlots]'s slot f % fx::kSlots
static_assert(kFxSlots == kFxUnits * fx::kSlots, "two slots a SendFx");
// Each box its own (phase 3(d)): rooms, SendFx units, the render core's smoothing and meters. Box 0's are what a lone
// Rytm always had; box 1's (16-voice builds) serve slot 1's chains (ins::engine(1)) and its settings (control1).
struct Box {
  fx::SendFx* volatile fx[kFxUnits] = {};
  fx::Control ctl[kFxUnits];          // their knobs: only knob[][] (from the slots), kill and clock (copied)
  float mix[kAllSlots];               // the render core's: each slot's MIX, smoothed
  bool mix_set[kAllSlots];
  int8_t run_fx[kAllSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  int8_t play[kAllSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // the room each slot played on its last chunk
  int16_t waited[kAllSlots];          // the frames a slot has waited for its room (the render core's, as these two)
  volatile float peak_in, peak_out;
  float lev = 1.0f;
};
Box g_box[kBoxes];
inline Box& box_at(int box) { return g_box[box > 0 && box < kBoxes ? box : 0]; }
// the longest wait for a room's last user to fade out: its fade and as much again (a holder frozen meanwhile, bus 1 / 2
// with its send FX off, plays no more and is let go then), counted in the waiting slot's own frames
constexpr int kWaitFrames = 2 * ins::kFadeFrames;

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
// 3(d): the settings code (book .. clear, the line, the console, json) works on the slot in hand's (ins::slot_in_hand:
// prj_slot on the control core, each slot in turn in ins::reset); report / stems_on_main are the Rytm's (its slot's: rytm())
Control& M() { return control_of(ins::slot_in_hand()); }
ins::Control& IK() { return ins::control_of(ins::slot_in_hand()); }
void book(int i, int f) { M().fx[i] = (int8_t)f; }
int free_fx() {
  for (int f = 0; f < kFxSlots; ++f) {
    bool used = false;
    for (int i = 0; i < kAllSlots; ++i) used |= M().fx[i] == f;
    if (!used) return f;
  }
  return -1;
}
void sfx() {
#ifndef CORE_HEAP_BUFFERS
  for (Box& b : g_box)
    for (int u = 0; u < kFxUnits; ++u)
      if (!b.fx[u]) b.fx[u] = new fx::SendFx(48000.0f);
#endif
}

// room f played on its last chunk by a slot other than i that has left it (fading out after its TYPE went, a kit
// change). A slot waiting for its own room plays none, so two slots whose rooms swap never wait for each other.
bool held_by_other(const Box& B, const Control& M, int f, int i) {
  for (int k = 0; k < kAllSlots; ++k)
    if (k != i && B.play[k] == f && M.fx[k] != f) return true;
  return false;
}

// the slot's send TYPE over m <= ins::kChunk frames: the sum in, the return out, MIX the crossfade (127: only the return)
void send_run(int box, int c, int s, const float* p, const float* x, const float* r, float* y0, float* y1, int m) {
  // the TYPE playing and the SendFx slot it plays on, also while the slot fades out after the TYPE was changed or cleared
  // (a voice chain never gets here: accept refuses it, and slot_index's -1 is no slot of these arrays)
  Box& B = box_at(box);
  const Control& M = control_of(box);   // box's rooms and MIX (its settings)
  const int i = slot_index(c, s), t = ins::engine(box).slot(c, s).cur;
  float in[ins::kChunk], w[ins::kChunk][2];
  if (i < 0) {
    for (int j = 0; j < m; ++j) y0[j] = x[j];
    if (r)
      for (int j = 0; j < m; ++j) y1[j] = r[j];
    return;
  }
  const int want = M.fx[i];
  bool wait = false;
  if (want >= 0 && want < kFxSlots && B.run_fx[i] != want) {   // a room newly taken
    if (held_by_other(B, M, want, i) && B.waited[i] < kWaitFrames) {
      wait = true;   // its last user still fades out on it: this slot passes its input dry until that is done
      B.waited[i] = (int16_t)(B.waited[i] + m);
    } else {         // from clean, never the last user's tail; a frozen last user lets it go (dry when it runs again)
      for (int k = 0; k < kAllSlots; ++k)
        if (B.run_fx[k] == want) B.run_fx[k] = -1, B.play[k] = -1;
      B.run_fx[i] = (int8_t)want;
      if (B.fx[want / fx::kSlots]) B.fx[want / fx::kSlots]->Drop(want % fx::kSlots);
      if (B.waited[i]) B.mix[i] = 0.0f, B.mix_set[i] = true;   // it waited dry: MIX fades in from there (the one-pole below), no step
    }
  }
  if (!wait) B.waited[i] = 0;
  const int f = wait ? -1 : B.run_fx[i], u = f / fx::kSlots, fs = f % fx::kSlots;
  for (int j = 0; j < m; ++j) in[j] = r ? 0.5f * (x[j] + r[j]) : x[j];
  if (f >= 0 && f < kFxSlots && B.fx[u]) {
    fx::Control& k = B.ctl[u];
    k.knob[fs][0] = t >= ins::kTypes && t < ins::kAllTypes ? kSendId[t - ins::kTypes] : (uint8_t)fx::kOff;
    k.knob[fs][1] = 0;
    for (int q = 0; q < ins::kParams; ++q) k.knob[fs][2 + q] = (uint8_t)(p[q] * 127.0f + 0.5f);
    k.kill = fx::control_of(box).kill;
    k.clock = fx::control_of(box).clock;
    B.fx[u]->Insert(fs, in, m, w, k);
    B.play[i] = (int8_t)f;
  } else {
    B.play[i] = -1;   // no room to play on (waiting, or let go): the input as it is, so MIX changes nothing
    for (int j = 0; j < m; ++j) w[j][0] = x[j], w[j][1] = r ? r[j] : x[j];
  }
  const float tgt = ins::knob01((uint8_t)played_mix(M, i));   // the knob + core/mod's offset
  if (!B.mix_set[i]) B.mix[i] = tgt, B.mix_set[i] = true;
  const float a = B.mix[i], b = a + (tgt - a) * ((float)m / ((float)m + 144.0f));   // 3 ms a chunk, as a slot's params
  B.mix[i] = fabsf(b - tgt) < 1e-4f ? tgt : b;
  const float d = (B.mix[i] - a) / (float)m;
  float q = a;
  for (int j = 0; j < m; ++j) {
    q += d;
    y0[j] = x[j] + q * (w[j][0] - x[j]);
    if (r) y1[j] = r[j] + q * (w[j][1] - r[j]);
  }
  // the room left and the fade-out done (this chunk ended dry): the slot holds nothing; taken again, it starts from clean
  if (want < 0 && ins::engine(box).slot(c, s).feed1 <= 0.0f) B.run_fx[i] = -1, B.play[i] = -1;
}

struct Hook {
  Hook() {
    ins::send_insert = send_run;
    Control* const both[2] = {&control, &control1};
    for (Control* m : both) {
      for (int i = 0; i < kAllSlots; ++i) m->mix[i] = kMixDef, m->fx[i] = -1;
      m->lev = kLevUnity, m->trim = kTrimDef;
    }
  }
} g_hook;

}  // namespace

Control& cur() { return M(); }

bool send_type(int t) { return t >= ins::kTypes && t < ins::kAllTypes; }

bool on() { return on(0); }

bool on(int box) {
  const ins::Control& k = ins::control_of(box);
  for (int i = 0; i < kSlots; ++i)
    if (k.type[chain(i)][slot(i)]) return true;
  for (int c = ins::kM0; c < ins::kChains; ++c)
    if (ins::peq_set(k, c) || k.scmode[c]) return true;   // (0.5.0: a SIDECHAIN stage too)
  return false;
}

int fx_used() {
  int n = 0;
  for (int i = 0; i < kAllSlots; ++i) n += M().fx[i] >= 0;
  return n;
}

bool room(int i) { return i >= 0 && i < kAllSlots && (M().fx[i] >= 0 || free_fx() >= 0); }

int full_text(int t, char* b, int cap) {
  return sniprintf(b, cap, CORE_STR("%s: %d send FX inserts at most, the master and the buses together (%d in use)"),
                   ins::type_name(t), kFxSlots, fx_used());
}

bool accept(int c, int s, int t) {
  const int i = slot_index(c, s);
  if (i < 0) return !send_type(t);   // a voice chain: never a send TYPE (core 1 renders voices 1..4)
  if (!send_type(t)) {
    if (M().fx[i] >= 0) book(i, -1);
    return true;
  }
  if (M().fx[i] >= 0) return true;   // it holds one already: the new TYPE follows it there
  int f = free_fx();
  if (f < 0) return false;
  book(i, f);
#ifndef CORE_HEAP_BUFFERS
  sfx();
#endif
  return true;
}

void set_byp(int v) {
  M().byp = v ? 1 : 0;
  ins::touch();
}
void set_src(int v) {
  M().src = v == kMix ? kMix : kRet;
  ins::touch();
}
void set_lev(int v) {
  M().lev = (uint8_t)clampi(v, 0, 127);
  ins::touch();
}
void mod_offset(int param, int off) {   // never saved, no touch: the knobs are the file's
  int8_t o = (int8_t)clampi(off, -127, 127);
  if (param == 0) M().mod_lev = o;
  else if (param >= 1 && param <= kAllSlots) M().mod_mix[param - 1] = o;
}
void set_trim(int db) {
  M().trim = (uint8_t)clampi(-db, 0, kTrimMax);
  ins::touch();
}
void set_mix(int i, int v) {
  if (i < 0 || i >= kAllSlots) return;
  M().mix[i] = (uint8_t)clampi(v, 0, 127);
  ins::touch();
}
void set_pair(int i, int v) {
  if (i < 0 || i >= kSlots) return;
  IK().mpair[i / ins::kSlots][slot(i)] = v ? 1 : 0;
  ins::touch();
}
bool pair(int i) { return i >= 0 && i < kSlots && IK().mpair[i / ins::kSlots][slot(i)]; }

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
  M().byp = 0, M().src = kRet, M().lev = kLevUnity, M().trim = kTrimDef;
  for (int i = 0; i < kSlots; ++i) M().mix[i] = kMixDef, M().fx[i] = -1;
  for (int c = 0; c < ins::kMasters; ++c)
    for (int s = 0; s < ins::kSlots; ++s) IK().mpair[c][s] = 0;
}
void clear() {
  clear_master();
  for (int i = kSlots; i < kAllSlots; ++i) M().mix[i] = kMixDef, M().fx[i] = -1;
}

// The Rytm's route byte 0x80006466, a set bit: that voice off the main bus. Packed from physical voices 7,6,5,4,0,1,2,3, MSB
// first (sidecar-mods re/subsystems/compressor-hardware.md); here each stem's bit
static constexpr uint8_t kRouteBit[kStems] = {3, 1, 7, 2, 0, 5, 4, 6};

int (*rytm_slot_fn)() = nullptr;
Control& rytm() { return control_of(rytm_slot_fn ? rytm_slot_fn() : 0); }

bool report(const uint8_t* d, int len) {
  if (len < 16 || d[4] != 0x50 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return false;
  const int route = (d[13] & 1) << 7 | (d[14] & 0x7F);
  int off = 0;
  Control& c = rytm();
  for (int v = 0; v < kStems; ++v) {
    c.pan[v] = d[5 + v] & 0x7F;
    if ((route >> kRouteBit[v]) & 1) off |= 1 << v;
  }
  c.off_main = (uint8_t)off;
  c.known = 1;
  return true;
}

void link_lost() {   // (both slots': it may have left its slot already; no other box reports)
  for (int b = 0; b < kBoxes; ++b) control_of(b).known = 0;
}

int stems_on_main() {
  const Control& control = rytm();
  if (!control.known) return 0;
  int n = 0;
  for (int v = 0; v < kStems; ++v) n += !((control.off_main >> v) & 1);
  return n;
}

void reset() {
  for (int b = 0; b < kBoxes; ++b) {   // every slot's settings (the slot in hand's are cleared by ins::reset's pass too)
    Control& m = control_of(b);
    m.byp = 0, m.src = kRet, m.lev = kLevUnity, m.trim = kTrimDef, m.mod_lev = 0;
    for (int i = 0; i < kAllSlots; ++i) m.mix[i] = kMixDef, m.fx[i] = -1, m.mod_mix[i] = 0;
    for (int c = 0; c < ins::kMasters; ++c)
      for (int s = 0; s < ins::kSlots; ++s) ins::control_of(b).mpair[c][s] = 0;
  }
  for (Box& B : g_box) {
    for (int u = 0; u < kFxUnits; ++u)
      if (B.fx[u]) B.fx[u]->Reset();
    memset((void*)B.ctl, 0, sizeof B.ctl);
    for (int i = 0; i < kAllSlots; ++i) B.mix_set[i] = false, B.run_fx[i] = -1, B.play[i] = -1, B.waited[i] = 0;
    B.peak_in = B.peak_out = 0.0f;
    B.lev = 1.0f;
  }
}

// ---------------------------------------------------------------------------------------------------- the audio side

void render(float (*y)[2], int n) { render(0, y, n); }

void render(int box, float (*y)[2], int n) {
  Box& B = box_at(box);
  float pi = B.peak_in, po = B.peak_out;
  for (int i = 0; i < n; ++i) {
    float a = fabsf(y[i][0]), b = fabsf(y[i][1]);
    pi = fmaxf(pi, fmaxf(a, b));
  }
  amp::service_master(box);   // AMP: this box's own master rows (amp.h: each box its own)
  ins::Engine& e = ins::engine(box);
  const ins::Control& k = ins::control_of(box);
  for (int c = ins::kM0; c < ins::kChains; ++c)
    if (e.Active(c, k)) e.ProcessStereo(c, y, n, k);
  const float t = (float)played_lev(control_of(box)) * (1.0f / kLevUnity), g0 = B.lev;   // the knob + core/mod's offset
  float g1 = g0 + (t - g0) * ((float)n / ((float)n + 144.0f));
  if (fabsf(g1 - t) < 1e-4f) g1 = t;
  B.lev = g1;
  const float dg = (g1 - g0) / (float)n;
  float g = g0;
  for (int i = 0; i < n; ++i) {
    g += dg;
    float l = out::Mixer::Limit(y[i][0] * g, kOutLimit), r = out::Mixer::Limit(y[i][1] * g, kOutLimit);
    y[i][0] = l, y[i][1] = r;
    po = fmaxf(po, fmaxf(fabsf(l), fabsf(r)));
  }
  B.peak_in = pi, B.peak_out = po;
}

int gr10() { return gr10(0); }

int gr10(int box) {
  const float g = lim_gain_of(box);
  return g < 1.0f ? (int)(-200.0f * log10f(g) + 0.5f) : 0;
}

void take_peaks(float* in, float* outp) { take_peaks(0, in, outp); }

void take_peaks(int box, float* in, float* outp) {
  Box& B = box_at(box);
  *in = B.peak_in, *outp = B.peak_out;
  B.peak_in = B.peak_out = 0.0f;
}

// The benches' writes into the controls, each byte's value before: given back only where it still holds what the bench
// wrote, so nothing written meanwhile by anyone else is undone (no snapshot of whole structs)
struct BenchUndo {
  volatile uint8_t* at[64];
  uint8_t was[64], put[64];
  int n = 0;
  void set(volatile uint8_t* p, uint8_t v) {
    if (n < 64) at[n] = p, was[n] = *p, put[n] = v, ++n;
    *p = v;
  }
  void set(volatile int8_t* p, int8_t v) { set((volatile uint8_t*)p, (uint8_t)v); }
  void undo() {
    for (int i = n - 1; i >= 0; --i)
      if (*at[i] == put[i]) *at[i] = was[i];
    n = 0;
  }
};
// the benches' common end: silence until the fades and the left effects' clears ran out, the render side's rooms and
// smoothing back to nothing, both insert SendFx cleared, the meters' peaks dropped (the bench's signal is no sound's)
template <typename Silence>
void bench_end(Silence silence) {
  for (int n = 0; n < 200; ++n) silence();
  Box& B = g_box[0];   // the boot benches run on box 0's chains
  for (int i = 0; i < kAllSlots; ++i) B.mix_set[i] = false, B.run_fx[i] = -1, B.play[i] = -1, B.waited[i] = 0;
  for (int u = 0; u < kFxUnits; ++u) B.fx[u]->Reset();
  B.peak_in = B.peak_out = 0.0f;
}

uint64_t bench(uint32_t* worst_block) {
  *worst_block = 0;
  if (on() || fx_used()) return 0;   // the boot's: nothing set (see master.h: never while another core may load a project)
#ifndef CORE_HEAP_BUFFERS
  sfx();
#endif
  if (!g_box[0].fx[0] || !g_box[0].fx[1]) return 0;
  static const uint8_t kT[kSlots] = {ins::kComp, ins::kEq, ins::kMSpace, ins::kMCloud};
  static BenchUndo u;
  for (int i = 0; i < kSlots; ++i) {
    u.set(&ins::control.type[chain(i)][slot(i)], kT[i]);
    for (int k = 0; k < ins::kParams; ++k) u.set(&ins::control.play[chain(i)][slot(i)][k], ins::labels(kT[i]).def[k]);
  }
  u.set(&control.fx[2], 0), u.set(&control.fx[3], 1), u.set(&control.mix[2], 127), u.set(&control.mix[3], 127);
  for (int k = 0; k < ins::kEqParams; ++k) u.set(&ins::control.peq[kEqChain][k], (int8_t)ins::eq_store(k, k < 2 ? 20 : 70));
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
  u.undo();   // (no file marked: the bytes were set directly)
  bench_end([&]() { memset(y, 0, sizeof y), render(y, 12); });
  return sum;
}

// set 0 a mix; 1.. one send TYPE x4 each (CPU admission's master send TYPE rows, admit.h send_bp: CLOUD first, as before)
static const uint8_t kSfxSets[][4] = {{ins::kMRings, ins::kMCloud, ins::kMSpace, ins::kMHalo},
                                      {ins::kMCloud, ins::kMCloud, ins::kMCloud, ins::kMCloud},
                                      {ins::kMChorus, ins::kMChorus, ins::kMChorus, ins::kMChorus},
                                      {ins::kMComb, ins::kMComb, ins::kMComb, ins::kMComb},
                                      {ins::kMSpace, ins::kMSpace, ins::kMSpace, ins::kMSpace},
                                      {ins::kMRings, ins::kMRings, ins::kMRings, ins::kMRings},
                                      {ins::kMHalo, ins::kMHalo, ins::kMHalo, ins::kMHalo}};
constexpr int kSfxSetCount = (int)(sizeof kSfxSets / sizeof kSfxSets[0]);
const char* sfx_bench_name(int set) {
  static const char* const kName[kSfxSetCount] = {"RINGS CLOUD SPACE HALO", "CLOUD x4", "CHORUS x4", "COMB x4",
                                                  "SPACE x4", "RINGS x4", "HALO x4"};
  return set >= 0 && set < kSfxSetCount ? kName[set] : nullptr;
}
int sfx_bench_type(int set) {
  if (set < 0 || set >= kSfxSetCount) return -1;
  for (int i = 1; i < 4; ++i)
    if (kSfxSets[set][i] != kSfxSets[set][0]) return -1;
  return kSfxSets[set][0];
}

uint64_t sfx_bench(int set, uint32_t* worst_block) {
  *worst_block = 0;
  if (set < 0 || set >= kSfxSetCount) return 0;
  for (int c = ins::kBus0; c < ins::kChains; ++c)   // the buses' and the master's slots all OFF, no room taken
    for (int s = 0; s < ins::kSlots; ++s)
      if (ins::control.type[c][s] != ins::kOff) return 0;
  if (fx_used()) return 0;
#ifndef CORE_HEAP_BUFFERS
  sfx();
#endif
  if (!g_box[0].fx[0] || !g_box[0].fx[1]) return 0;
  static BenchUndo u;
  static const int kC[4] = {ins::kBus0, ins::kBus0 + 1, ins::kBus0 + 2, ins::kM0};
  for (int i = 0; i < 4; ++i) {   // the bytes set directly (no file marked), rooms 0..3
    const int t = kSfxSets[set][i], j = slot_index(kC[i], 0);
    u.set(&ins::control.type[kC[i]][0], (uint8_t)t);
    for (int k = 0; k < ins::kParams; ++k) u.set(&ins::control.play[kC[i]][0][k], ins::labels(t).def[k]);
    u.set(&control.fx[j], (int8_t)i), u.set(&control.mix[j], 64);
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
    if (fx::bus_insert) fx::bus_insert(fx::control, 0, b12[0], 12), fx::bus_insert(fx::control, 1, b12[1], 12);
    ins::bus3(in3, 12, pre, post);
    render(y, 12);
    const uint32_t t = cycles() - t0;
    sum += t;
    if (t > worst) worst = t;
  }
  *worst_block = worst;
  u.undo();
  bench_end([&]() {
    memset(b12, 0, sizeof b12), memset(in3, 0, sizeof in3), memset(y, 0, sizeof y);
    if (fx::bus_insert) fx::bus_insert(fx::control, 0, b12[0], 12), fx::bus_insert(fx::control, 1, b12[1], 12);
    ins::bus3(in3, 12, pre, post);
    render(y, 12);
  });
  return sum;
}

// One box's chains at their worst set that can play together, as one row (LT1, 2026-11-22: the rows above overlap, and their
// sum stood for a box's chains at ~6x what they took). Measured as core 2 runs them for a box (pi/engine/cores.cpp: sendfx_pi
// then masterfx_pi): its two send FX at CLOUDSEED with every track sent to both (SendFx::Process, which runs buses 1 and 2's
// chains through fx::bus_insert), bus 3's chain (ins::bus3), then the master chain (render) on the main pair. The inserts
// are LT1's config A, the costliest set the rooms allow: B1..B3 A and M1 CLOUD (the four rooms), B1..B3 B and M4 SAT, the
// POST EQ on. M2 and M3 stay OFF: AMP needs a capture, which the boot has not loaded; the caller adds its AMP pair from the
// amp row. send: the box's send FX (sendfx_pi's on the Pi), left Reset.
uint64_t box_bench(fx::SendFx* send, uint32_t* worst_block) {
  *worst_block = 0;
  if (!send) return 0;
  for (int c = ins::kBus0; c < ins::kChains; ++c)   // the buses' and the master's slots all OFF, no room taken
    for (int s = 0; s < ins::kSlots; ++s)
      if (ins::control.type[c][s] != ins::kOff) return 0;
  if (fx_used() || fx::any_on(fx::control)) return 0;
#ifndef CORE_HEAP_BUFFERS
  sfx();
#endif
  if (!g_box[0].fx[0] || !g_box[0].fx[1]) return 0;
  static BenchUndo u;
  struct Set { int c, s, t; };
  static const Set kSet[8] = {{ins::kBus0, 0, ins::kMCloud},     {ins::kBus0 + 1, 0, ins::kMCloud}, {ins::kBus0 + 2, 0, ins::kMCloud},
                              {ins::kM0, 0, ins::kMCloud},       {ins::kBus0, 1, ins::kSat},        {ins::kBus0 + 1, 1, ins::kSat},
                              {ins::kBus0 + 2, 1, ins::kSat},    {ins::kM0 + 1, 1, ins::kSat}};
  for (int i = 0, room = 0; i < 8; ++i) {   // the bytes set directly (no file marked), rooms 0..3 for the send TYPEs
    const Set& e = kSet[i];
    u.set(&ins::control.type[e.c][e.s], (uint8_t)e.t);
    for (int k = 0; k < ins::kParams; ++k) u.set(&ins::control.play[e.c][e.s][k], ins::labels(e.t).def[k]);
    if (send_type(e.t)) {
      const int j = slot_index(e.c, e.s);
      u.set(&control.fx[j], (int8_t)room++), u.set(&control.mix[j], 64);
    }
  }
  for (int k = 0; k < ins::kEqParams; ++k) u.set(&ins::control.peq[kEqChain][k], (int8_t)ins::eq_store(k, k < 2 ? 20 : 70));
  static fx::Control fc;   // the send FX's knobs: not the box's (box_of: any other Control is box 0's), so nothing to give back
  memset((void*)&fc, 0, sizeof fc);
  for (int k = 0; k < fx::kSlots; ++k) fc.knob[k][0] = fx::kCloudSeed, fc.knob[k][1] = 100, fc.knob[k][2] = fc.knob[k][3] = 80, fc.knob[k][4] = fc.knob[k][5] = 64;
  for (int t = 0; t < fx::kTracks; ++t) fc.send[t][0] = fc.send[t][1] = 127;
  float st[12][fx::kStems], pre[12][2], post[12][2], in3[12], y[12][2];
  uint64_t sum = 0;
  uint32_t worst = 0;
  for (int n = 0; n < 4000; ++n) {
    for (int i = 0; i < 12; ++i) {   // (the test signal outside the timing)
      const float a = sinf(0.05f * (float)(n * 12 + i)), b = sinf(0.031f * (float)(n * 12 + i));
      for (int v = 0; v < fx::kStems; ++v) st[i][v] = 0.1f * (v & 1 ? a : b);
      in3[i] = 0.15f * (a + b), y[i][0] = 0.2f * a + 0.1f * b, y[i][1] = 0.2f * a - 0.1f * b;
    }
    const uint32_t t0 = cycles();
    send->Process(st, 12, pre, fc, post);
    ins::bus3(in3, 12, pre, post);
    for (int i = 0; i < 12; ++i) y[i][0] += post[i][0], y[i][1] += post[i][1];
    render(y, 12);
    const uint32_t t = cycles() - t0;
    sum += t;
    if (t > worst) worst = t;
  }
  *worst_block = worst;
  u.undo();
  bench_end([&]() {
    memset(in3, 0, sizeof in3), memset(y, 0, sizeof y), memset(pre, 0, sizeof pre), memset(post, 0, sizeof post);
    ins::bus3(in3, 12, pre, post);
    render(y, 12);
  });
  send->Reset();
  return sum;
}

static constexpr unsigned long kUnitBytes = ((unsigned long)sizeof(fx::SendFx) + 63) & ~63ul;
unsigned long heap_bytes() {
#ifdef CORE_HEAP_BUFFERS
  return kBoxes * kFxUnits * kUnitBytes + 64;   // box 0's units, then box 1's
#else
  return 0;
#endif
}
void heap_place(void* mem) {
#ifdef CORE_HEAP_BUFFERS
  uintptr_t a = ((uintptr_t)mem + 63) & ~(uintptr_t)63;
  for (int b = 0; b < kBoxes; ++b)
    for (int u = 0; u < kFxUnits; ++u) g_box[b].fx[u] = new ((void*)(a + (b * kFxUnits + u) * kUnitBytes)) fx::SendFx(48000.0f);
#else
  (void)mem;
#endif
}

// ---------------------------------------------------------------------------------------------------- console, json, card

static int slot_line(int i, char* o, int cap) {
  const int c = chain(i), s = slot(i), t = IK().type[c][s];
  const ins::Labels& L = ins::labels(t);
  int n = sniprintf(o, cap, CORE_STR(" %d %s"), i + 1, L.name);
  if (t == ins::kAmp && pair(i)) n += sniprintf(o + n, n < cap ? cap - n : 0, " L/R");
  if (send_type(t)) n += sniprintf(o + n, n < cap ? cap - n : 0, CORE_STR(" mix %d"), M().mix[i]);
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
        if (IK().type[c][s] != t) {
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
  int n = sniprintf(o, cap, CORE_STR("mfx: %s, src %s, lev %d, trim %d:"), M().byp ? "BYPASS" : "on", M().src == kMix ? "MIX" : "RET", M().lev,
                    -(int)M().trim);
  for (int i = 0; i < kSlots && n < cap; ++i)
    if (IK().type[chain(i)][slot(i)]) n += slot_line(i, o + n, cap - n);
  if (n < cap && ins::eq_on(kEqChain)) n += sniprintf(o + n, cap - n, " EQ");
  return true;
}

int json(char* o, int cap) {
  const int gr = gr10(ins::slot_in_hand());
  int n = sniprintf(o, cap, CORE_STR("\"mfx\":{\"byp\":%d,\"src\":\"%s\",\"lev\":%d,\"trim\":%d,\"gr\":%d.%d,\"on\":%d,\"s\":["), M().byp,
                    M().src == kMix ? "mix" : "ret", M().lev, -(int)M().trim, gr / 10, gr % 10, on(ins::slot_in_hand()) ? 1 : 0);
  for (int i = 0; i < kSlots && n < cap; ++i) {
    const int c = chain(i), s = slot(i), t = IK().type[c][s];
    const ins::Labels& L = ins::labels(t);
    char v[ins::kParams][8] = {};
    if (t)
      for (int j = 0; j < ins::kParams; ++j) ins::slot_value_text(c, s, j, ins::param(c, s, j), v[j], sizeof v[j]);
    int k = sniprintf(o + n, cap - n, CORE_STR("%s{\"t\":\"%s\",\"p\":[%d,%d,%d,%d],\"l\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"v\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"mix\":%d,\"pair\":%d,\"send\":%d}"),
                      i ? "," : "", ins::type_name(t), ins::param(c, s, 0), ins::param(c, s, 1), ins::param(c, s, 2), ins::param(c, s, 3),
                      L.label[0], L.label[1], L.label[2], L.label[3], v[0], v[1], v[2], v[3], M().mix[i], pair(i) ? 1 : 0, send_type(t) ? 1 : 0);
    if (k < 0 || k >= cap - n) return cap;
    n += k;
  }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, CORE_STR("],\"peq\":[%d,%d,%d,%d],\"fxcap\":%d,\"fxused\":%d,\"rep\":%d,\"onmain\":%d}"),
                    ins::eq_param(kEqChain, 0), ins::eq_param(kEqChain, 1), ins::eq_param(kEqChain, 2), ins::eq_param(kEqChain, 3),
                    kFxSlots, fx_used(), rytm().known, stems_on_main());
  return n + (k > 0 ? k : 0);
}

int line(char* b, int cap) {
  int pairs = 0;
  bool def = !M().byp && M().src == kRet && M().lev == kLevUnity && M().trim == kTrimDef;
  for (int i = 0; i < kSlots; ++i) {
    if (pair(i)) pairs |= 1 << i, def = false;
    def = def && M().mix[i] == kMixDef;
  }
  if (def) return 0;
  int n = sniprintf(b, cap, CORE_STR("MFX %d %s %d %d %d %d %d %d"), M().byp, M().src == kMix ? "MIX" : "RET", M().lev,
                    M().mix[0], M().mix[1], M().mix[2], M().mix[3], pairs);
  if (M().trim != kTrimDef) n += sniprintf(b + n, cap - n, CORE_STR(" %d"), -(int)M().trim);
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
