// core/mod/mod.cpp - see mod.h. Control side only, cold: a tick walks the live entries once a millisecond at most.
// The arithmetic is 0.4.0's lfo.cpp's, expression for expression (spec-050 1.4, 1.9): a kV value
// clampf(base + off, 0, 127.99), written when it moved by 1/256; a kFx value rnd0(clampf(base + off, 0, 127)); an offset
// kind's (int)(off +- 0.5). Never split `e.off += depth * w` across functions (the golden would move).
#include "mod/mod.h"

#include <math.h>
#include <string.h>

#include "fx/send_fx.h"
#include "lfo.h"
#include "mod/learn.h"
#include "mod/rytm_out.h"
#include "mod/rytm_param.h"
#include "mod/scene.h"
#include "mod/xypad.h"
#include "platform.h"
#include "pool/pool.h"   // prj_slot: the slot in hand (3(c))
#include "ui/remote.h"
#include "voices.h"
#include "device/device.h"

namespace core {
namespace mod {

namespace {

// the entries by id (spec-050 1.4, Phase 2: 128 morph calls a tick): 512 slots, linear probing, entry + 1 (0 empty);
// deletion shifts the run back (no tombstones). The walks still go in array order (the writes' order is 0.4.0's).
constexpr int kIx = 512;
// 3(c) (option A): a layer per voice slot - its entries, their index, its dirty voices - so slot 1's LFOs move slot 1's
// own voices and never an entry of slot 0's. The slot in hand's (L(): device::cur() on the control core); mod::tick
// runs each slot's pass in its Scope; a voice's report names its own slot's (slot_of_voice).
struct Layer {
  Entry e[kEntries];
  // one past the highest entry in use: every walk stops there (allocation is first free, so the LFOs' few entries sit
  // at the bottom and a lookup costs what 0.4.0's 16-entry table did, not kEntries)
  int hi = 0;
  uint8_t vdirty = 0;   // the slot's box voices (0..7) whose v[].p moved: their machines take them (flush_voices)
  uint16_t ix[kIx];
};
Layer g_l[kVoiceSlots];
CORE_COLD Layer& L() { return g_l[prj_slot()]; }
CORE_COLD Layer& L(int s) { return g_l[s > 0 && s < kVoiceSlots ? s : 0]; }
struct Used {
  Entry* b;
  Entry* e;
  Entry* begin() const { return b; }
  Entry* end() const { return e; }
};
Used used(Layer& l = L()) { return {l.e, l.e + l.hi}; }
uint32_t g_last_us = 0;
bool g_started = false;
uint32_t g_user_ms[kVoices];
Stats g_stats;
void (*g_source)(uint32_t us, uint32_t dus) = nullptr;

CORE_COLD float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
CORE_COLD int rnd0(float x) { return (int)(x + 0.5f); }   // x >= 0

CORE_COLD int ix_home(Tid t) { return (int)((uint32_t)t * 40503u >> 7) & (kIx - 1); }
CORE_COLD Entry* find_e(Layer& l, Tid t) {
  if (!t) return nullptr;   // (a free entry's t is 0)
  for (int i = ix_home(t); l.ix[i]; i = (i + 1) & (kIx - 1))
    if (l.e[l.ix[i] - 1].t == t) return &l.e[l.ix[i] - 1];
  return nullptr;
}
CORE_COLD Entry* find_e(Tid t) { return find_e(L(), t); }
CORE_COLD void ix_add(Layer& l, Entry& e) {
  int i = ix_home(e.t);
  while (l.ix[i]) i = (i + 1) & (kIx - 1);
  l.ix[i] = (uint16_t)(&e - l.e + 1);
}
CORE_COLD void ix_del(Layer& l, Entry& e) {
  int i = ix_home(e.t);
  while (l.ix[i] && l.ix[i] != &e - l.e + 1) i = (i + 1) & (kIx - 1);
  if (!l.ix[i]) return;
  l.ix[i] = 0;
  for (int j = (i + 1) & (kIx - 1); l.ix[j]; j = (j + 1) & (kIx - 1)) {
    int h = ix_home(l.e[l.ix[j] - 1].t);   // moves back into the hole unless its home lies in (i, j]
    bool stays = i <= j ? (h > i && h <= j) : (h > i || h <= j);
    if (!stays) l.ix[i] = l.ix[j], l.ix[j] = 0, i = j;
  }
}
// 3(c): the kinds a slot's layer may have. The Rytm's slot's: all of them. Another slot's: its own voices (kV), all of its INSERT
// chains (voices, buses, master), OUT routes and sends, its send FX's knobs (kFx) and its master (kMfx) - its own
// control_of, the slot in hand's settings (core/fx/insert.h, send_fx.h, master.h; 3(c) step 8) - never the Rytm's own
// (its tracks' sends kSnd, its params, scene and the sequencer words). Refused at the source's call (offset / morph): an
// LFO file naming one still loads and saves as it was.
CORE_COLD bool slot_has(Tid t) {
  if (device::rytm_at(prj_slot())) return true;
  const Kind k = kind_of(t);
  return k == kV || k == kOut || k == kIsnd || k == kIns || k == kFx || k == kMfx;   // (3(c) step 8: its own send FX and master too)
}
// a new entry for t: the first free one (the order 0.4.0's table filled in, so the writes come in its order); its
// base what plays now. Null: the table is full
CORE_COLD Entry* alloc(Tid t) {
  Layer& l = L();
  for (Entry& f : l.e)
    if (!f.t) {
      f.t = t;
      f.base = base_now(t);
      // a kind whose writer takes a value (kSeq kArp kLoop kRytm) has no state the layer can read: nothing applied yet,
      // so its first value is always put (a RYTM:SCN lock at 0 goes out; rytm_out's own on-change check decides)
      Kind k = kind_of(t);
      f.applied = k == kSeq || k == kArp || k == kLoop || k == kRytm ? -1.0f : f.base;
      if (&f - l.e >= l.hi) l.hi = (int)(&f - l.e) + 1;
      ix_add(l, f);
      return &f;
    }
  return nullptr;
}
// what a target starts from this tick (spec-050 1.2): a held manual edit, the morph, else its base
CORE_COLD float start_of(const Entry& e) { return e.held ? e.manual : e.morph_on ? e.morph : e.base; }

// an entry that leaves: its target back to its base (an offset kind's offset to 0). The slot in hand's layer
CORE_COLD void restore(Entry& e) {
  Layer& l = L();
  ++g_stats.restores;
  Kind k = kind_of(e.t);
  if (k == kV) {
    put(e.t, e.base);
    l.vdirty |= (uint8_t)(1u << index_of(e.t));
  } else if (is_offset(k)) {
    put(e.t, 0);
  } else if (k == kFx || k == kSnd) {
    put(e.t, e.base);
  } else if (k == kLoop) {
    put(e.t, -1.0f);   // the knob plays again
  }   // (kSeq kArp kRytm: nothing to put back - their writers take a value, not a state; Phases 3, 4)
#if CORE_POLY
  if (k == kRytm) rytm_out_forget(param_of(e.t));   // the Rytm may change it meanwhile: the next lock's value goes out
  if (is_remote(k)) rp_release(e.t), rp_morph_off(e.t);   // (its offset went to 0 above: put; now a release)
#endif
  ix_del(l, e);
  e = Entry();
  while (l.hi && !l.e[l.hi - 1].t) --l.hi;
}

}  // namespace

// the send FX's hook into the layer (fx/send_fx.h; its knob_filter is the LFOs', which forwards here), set before main
static CORE_COLD void edited(uint16_t t, float v) {
  if (v < 0) manual_knob(t);
  else manual(t, v);
}
static struct FxHooks {
  FxHooks() { fx::send_filter = fx_send, fx::manual_edit = edited; }
} g_fx_hooks;

CORE_COLD void set_source_fn(void (*fn)(uint32_t us, uint32_t dus)) { g_source = fn; }

CORE_COLD void begin_tick() {
  for (Entry& e : used()) e.off = 0, e.live = 0, e.morph_on = 0;   // (a free entry's off is 0 already)
}

CORE_COLD void offset(int src, Tid t, float depth, float w) {
  (void)src;
  if (!valid(t) || !slot_has(t)) return;   // (0 or a target this build has not: never into a free entry)
  Entry* e = find_e(t);
  if (!e) e = alloc(t);
  if (!e) {   // full (cannot happen with the LFOs alone): counted, mod=stats
    ++g_stats.dropped;
    return;
  }
  e->live = 1;
  e->off += depth * w;
}

CORE_COLD void morph(Tid t, float value) {
  if (!valid(t) || !slot_has(t)) return;
  Entry* e = find_e(t);
  if (!e) e = alloc(t);
  if (!e) {
    ++g_stats.dropped;
    return;
  }
  e->live = 1, e->morph_on = 1, e->morph = value;
}
#if CORE_POLY
CORE_COLD void morph_parts(Tid t, bool has_a, float a, bool has_b, float b, float f, bool step, bool upper) {
  if (!valid(t) || !is_remote(kind_of(t)) || !slot_has(t)) return;
  Entry* e = find_e(t);
  if (!e) e = alloc(t);
  if (!e) {
    ++g_stats.dropped;
    return;
  }
  e->live = 1, e->morph_on = 1, e->morph = has_b ? b : a;
  if (!step && (ui::remote_caps2() & ui::kCaps2Morph)) {   // P3: the Rytm morphs a continuous word itself (09 / 0A)
    float x = f < 0 ? 0 : f > 1 ? 1 : f;
    rp_fader((uint16_t)(x * 16383.0f + 0.5f));
    rp_morph_set(t, has_a, a, has_b, b);
  } else {
    rp_morph(t, has_a, a, has_b, b, f, step, upper);
  }
  ++g_stats.scene_sets;
}
#endif
CORE_COLD void release(Tid t) {
  Entry* e = find_e(t);
  if (e && !e->live) restore(*e);
}
CORE_COLD float base(Tid t) {
  if (is_offset(kind_of(t))) return knob_now(t);
  const Entry* e = find_e(t);
  return e ? e->base : base_now(t);
}
// spec-050 2.5: a Sidecar-side edit of a target the morph holds wins until the fader next moves (never a report's)
CORE_COLD void manual(Tid t, float v) {
  Entry* e = find_e(t);
  if (!e || !e->morph_on) return;
  e->held = 1, e->manual = v;
}
CORE_COLD void fader_moved() {
  for (Entry& e : used()) e.held = 0;
}
CORE_COLD bool held_any() {
  for (const Entry& e : used())
    if (e.t && e.held) return true;
  return false;
}

CORE_COLD void end_tick() {
  for (Entry& e : used()) {
    if (!e.t) continue;
    if (!e.live) {
      restore(e);
      continue;
    }
    Kind k = kind_of(e.t);
    bool sc = e.held || e.morph_on;   // (counted apart: the LFOs' stats line shows voice_sets / fx_sets)
    float st = start_of(e);           // (e.base unless a morph or a held edit: then 0.4.0's expressions exactly)
    if (k == kV) {
      float val = clampf(st + e.off, 0.0f, 127.99f);
      if (fabsf(val - e.applied) >= 1.0f / 256.0f) {
        put(e.t, val);
        e.applied = val;
        L().vdirty |= (uint8_t)(1u << index_of(e.t));
        ++(sc ? g_stats.scene_sets : g_stats.voice_sets);
      }
#if CORE_POLY
    } else if (is_remote(k)) {   // the Rytm holds the base (rytm_param.h): the offset unrounded, the morph's parts
      if (e.off != e.applied) {
        put(e.t, e.off);
        e.applied = e.off;
        ++g_stats.fx_sets;
      }
      if (!e.morph_on) rp_morph_off(e.t);
#endif
    } else if (is_offset(k)) {   // the offset itself (the knob / a step lock is the base, where it is applied)
      // under a morph or a held edit: the offset that makes the knob play start + off (the writer clamps the sum)
      // (held: the edit is the knob itself, so the offset alone)
      float o = e.morph_on && !e.held ? st + e.off - knob_now(e.t) : e.off;
      int iv = (int)(o + (o < 0 ? -0.5f : 0.5f));
      if ((float)iv != e.applied) {
        put(e.t, (float)iv);
        e.applied = (float)iv;
        ++(sc ? g_stats.scene_sets : g_stats.fx_sets);
      }
    } else if (k == kFx || k == kSnd) {
      int iv = rnd0(clampf(st + e.off, 0.0f, 127.0f));
      if ((float)iv != e.applied) {
        put(e.t, (float)iv);
        e.applied = (float)iv;
        ++(sc ? g_stats.scene_sets : g_stats.fx_sets);
      }
    } else {   // kSeq kArp kLoop kRytm: an integer in the field's range
      Range r = range(e.t);
      int iv = rnd0(clampf(st + e.off, r.lo, r.hi));
      if ((float)iv != e.applied) {
        put(e.t, (float)iv);
        e.applied = (float)iv;
        ++g_stats.scene_sets;
      }
    }
  }
}

static CORE_COLD void flush_body();
CORE_COLD void note_lookup(uint32_t us) {
  if (us > g_stats.lookup_us) g_stats.lookup_us = us;
}

CORE_COLD void flush_voices() {
  uint32_t t0 = now_us();
  flush_body();
  uint32_t d = now_us() - t0;
  if (d > g_stats.flush_us) g_stats.flush_us = d;
}
static CORE_COLD void flush_body() {
  // the voices whose parameters moved: their machines take them (a user machine at most every kUserMs); the slot in
  // hand's (its box voice k is engine voice voice_in(k))
  Layer& l = L();
  uint32_t ms = now_ms();
  for (int k = 0; k < kBoxVoices; ++k) {
    if (!(l.vdirty >> k & 1)) continue;
    const int vi = device::voice_in((uint8_t)k);
    if (v[vi].user >= 0) {
      if (ms - g_user_ms[vi] < kUserMs) continue;
      g_user_ms[vi] = ms;
      ++g_stats.user_calls;
    }
    l.vdirty &= (uint8_t)~(1u << k);
    voice_params_changed(vi);
  }
}
// 3(c): slot s takes part in the tick: device::live (held, saving, or the base)
static CORE_COLD bool slot_on(int s) { return device::live(s); }

static CORE_COLD void tick_body(uint32_t us);
CORE_COLD void tick(uint32_t us) {   // the whole tick timed (spec-050 1.5): stats().worst_us
  uint32_t t0 = now_us();
  tick_body(us);
  uint32_t d = now_us() - t0;
  if (d > g_stats.worst_us) g_stats.worst_us = d;
}
static CORE_COLD void tick_body(uint32_t us) {
#if CORE_POLY
  rp_tick(us);   // the Rytm's own params (rytm_param.h): a p-lock due, the 5 ms grid, the refresh (cheap when idle)
#endif
#if !CORE_MOD_LFO
  if (!g_source) {   // the rollback (core/lfo.h): the LFOs' own path
    lfo::tick(us);
    return;
  }
#endif
  if (!g_started) {
    g_started = true, g_last_us = us;
    return;
  }
  uint32_t dus = us - g_last_us;
  if (dus < 1000) {   // a millisecond at most; user machines waiting for their call go on
    bool any = false;
    for (const Layer& l : g_l) any = any || l.vdirty;
    if (!any) return;
  } else {
    if (dus > 100000) dus = 100000;   // a long stall: the sources skip ahead no more than 100 ms
    g_last_us = us;
    ++g_stats.ticks;
    for (int s = 0; s < kVoiceSlots; ++s) {   // 3(c): each slot's layer in its own pass, in its Scope (slot 0 first)
      if (!slot_on(s)) continue;
      device::Scope in(device::g_links[s]);
      begin_tick();
      if (g_source) g_source(us, dus);   // (tests)
      else lfo::waves(us, dus);         // the LFOs: the clock position, the waves, each one's offset
#if CORE_POLY
      learn::tick();                    // the slot's MIDI-learn routes' offsets (core/mod/learn.h)
      xypad::tick();                    // the slot's XY pad (core/mod/xypad.h)
      scene::tick(now_ms());            // the slot's morph: a value for each lock of A and B (core/mod/scene.h)
#endif
      end_tick();
#if CORE_POLY
      if (device::rytm_at(s)) {   // (the Rytm's: once a tick)
        rp_layer_done();                  // (a kit change's releases are in the layer now: the grid may send)
        rytm_out_tick(now_ms());          // the Rytm's scene / macros whose 5 ms have passed (core/mod/rytm_out.h)
        rp_tick(us);                      // and the Rytm's own params this tick's offsets moved (rytm_param.h)
      }
#endif
    }
  }
  for (int s = 0; s < kVoiceSlots; ++s) {
    if (!g_l[s].vdirty) continue;
    device::Scope in(device::g_links[s]);
    flush_voices();
  }
}

CORE_COLD void restore_all() {   // the slot in hand's
  Layer& l = L();
  for (Entry& e : used(l))
    if (e.t) restore(e);
  for (int k = 0; k < kBoxVoices; ++k)
    if (l.vdirty >> k & 1) voice_params_changed(device::voice_in((uint8_t)k));
  l.vdirty = 0;
#if CORE_POLY
  if (device::rytm_at(prj_slot())) rp_clear_all();   // the Rytm's own params: CLEAR on every track (a project change; rytm_param.h)
#endif
}

CORE_COLD void restore_morphs() {
  for (Entry& e : used())
    if (e.t && (e.morph_on || e.held)) restore(e);
}

CORE_COLD void voice_params(int vi, float* p, int np) {
#if !CORE_MOD_LFO
  lfo::voice_params(vi, p, np, false);   // the rollback: the LFOs' own table
  return;
#endif
  if (vi < 0 || vi >= kVoices) return;
  const int bk = box_voice(vi);   // 3(c): the voice's own slot's layer, its number in its box
  for (Entry& e : used(L(slot_of_voice(vi)))) {
    if (!e.t || kind_of(e.t) != kV || index_of(e.t) != bk || param_of(e.t) >= np) continue;
    e.base = p[param_of(e.t)];
    float val = clampf(start_of(e) + e.off, 0.0f, 127.99f);   // (a morph plays on: the report moves the base only)
    p[param_of(e.t)] = val;
    e.applied = val;
  }
}

CORE_COLD uint8_t fx_knob(int slot, int knob, uint8_t val) {
  if (knob < 2 || knob > 5) return val;
  Entry* e = find_e(L(), tid(kFx, slot & 1, knob - 2));   // (the slot in hand's send FX: its layer, 3(c) step 8)
  if (!e) return val;
  e->base = val;
  int iv = rnd0(clampf(start_of(*e) + e->off, 0.0f, 127.0f));
  e->applied = (float)iv;
  return (uint8_t)iv;
}

CORE_COLD uint8_t fx_send(int track, int slot, uint8_t val) {
  Entry* e = find_e(L(device::rytm_home()), tid(kSnd, track, slot & 1));   // (the Rytm's sends: its slot's layer)
  if (!e) return val;
  e->base = val;
  int iv = rnd0(clampf(start_of(*e) + e->off, 0.0f, 127.0f));
  e->applied = (float)iv;
  return (uint8_t)iv;
}

CORE_COLD uint8_t knob_base(int slot, int knob) {
  slot &= 1;
  if (knob < 0 || knob > 5) return 0;
  const Entry* e = knob >= 2 ? find_e(L(), tid(kFx, slot, knob - 2)) : nullptr;
  return e ? (uint8_t)e->base : fx::cur().knob[slot][knob];
}
CORE_COLD uint8_t send_base(int track, int slot) {
  if (track < 0 || track >= fx::kTracks) return 0;
  const Entry* e = find_e(L(device::rytm_home()), tid(kSnd, track, slot & 1));
  return e ? (uint8_t)e->base : fx::control_of(device::rytm_home()).send[track][slot & 1];   // (the Rytm's: its slot's)
}

CORE_COLD float base_of(int vi, int k, float cur) {
#if !CORE_MOD_LFO
  return lfo::base_of(vi, k, cur);   // the rollback: the LFOs' own table
#endif
  const Entry* e = vi < 0 || vi >= kVoices ? nullptr : find_e(L(slot_of_voice(vi)), tid(kV, box_voice(vi), k & 15));
  return e ? e->base : cur;
}

CORE_COLD void played(int vi, float out[16]) { memcpy(out, (const float*)v[voice_wrap(vi)].p, 16 * sizeof(float)); }

namespace {
// a value for the console: kV with two decimals, the others whole (integer printf only: the Teensy shares this file)
CORE_COLD int fmt_val(char* b, int cap, float x, bool frac, bool sign) {
  int c = (int)(x * 100.0f + (x < 0 ? -0.5f : 0.5f));
  const char* sg = c < 0 ? "-" : sign ? "+" : "";
  int a = c < 0 ? -c : c;
  if (frac) return sniprintf(b, cap, "%s%d.%02d", sg, a / 100, a % 100);
  return sniprintf(b, cap, "%s%d", sg, (a + 50) / 100);
}
}  // namespace

CORE_COLD bool command(const char* p, char* o, int cap) {
#if CORE_POLY
  if (rp_command(p, o, cap)) return true;   // rp, rp=...: the Rytm's own params (rytm_param.h)
#endif
  if (strncmp(p, "mod", 3) != 0 || (p[3] && p[3] != '=')) return false;
  if (cap <= 0) return true;
  o[0] = 0;
  if (!p[3]) {
    int live = 0;
    for (const Entry& e : used()) live += e.t != 0;
    int n = sniprintf(o, cap, "mod: %d live", live);
    bool first = true;
    for (const Entry& e : used()) {
      if (!e.t || n >= cap - 1) continue;
      char nm[16], a[16], b[16], f[16];
      name(e.t, nm, sizeof nm);
      Kind k = kind_of(e.t);
      int w;
      if (is_offset(k)) {
        fmt_val(a, sizeof a, e.applied, false, true);
        w = sniprintf(o + n, cap - n, "%s%s %s (offset)", first ? ": " : ", ", nm, a);
      } else {
        fmt_val(a, sizeof a, e.applied, k == kV, false), fmt_val(b, sizeof b, e.base, k == kV, false);
        fmt_val(f, sizeof f, e.off, false, true);
        w = sniprintf(o + n, cap - n, "%s%s %s (base %s off %s)", first ? ": " : ", ", nm, a, b, f);
      }
      first = false;
      if (w < 0 || w >= cap - n) break;
      n += w;
    }
    return true;
  }
  if (!strcmp(p + 4, "stats")) {
    const Stats& s = g_stats;
    sniprintf(o, cap,
              "mod: ticks %u voice_sets %u user_calls %u fx_sets %u scene_sets %u restores %u dropped %u worst_us %u "
              "flush_us %u lookup_us %u hi %d",
              (unsigned)s.ticks, (unsigned)s.voice_sets, (unsigned)s.user_calls, (unsigned)s.fx_sets,
              (unsigned)s.scene_sets, (unsigned)s.restores, (unsigned)s.dropped, (unsigned)s.worst_us, (unsigned)s.flush_us, (unsigned)s.lookup_us, L().hi);
    return true;
  }
  sniprintf(o, cap, "mod | mod=stats");
  return true;
}

CORE_COLD int json(char* o, int cap) {   // the base box's layer (/status on the Pi's core 0: never the slot in hand)
  const device::Link* b = __atomic_load_n(&device::g_base, __ATOMIC_RELAXED);
  int live = 0;
  for (const Entry& e : used(L(b ? b->index : 0))) live += e.t != 0;
  const Stats& s = g_stats;
  int n = sniprintf(o, cap, "\"mod\":{\"live\":%d,\"worst_us\":%u,\"flush_us\":%u,\"lookup_us\":%u,\"ticks\":%u,"
                    "\"voice_sets\":%u,\"fx_sets\":%u,\"restores\":%u,\"dropped\":%u}", live, (unsigned)s.worst_us,
                    (unsigned)s.flush_us, (unsigned)s.lookup_us, (unsigned)s.ticks, (unsigned)s.voice_sets,
                    (unsigned)s.fx_sets, (unsigned)s.restores, (unsigned)s.dropped);
  if (n < 0) return 0;
#if CORE_POLY
  if (n < cap - 1) {   // ,"rp":{...}: the Rytm's own params (rytm_param.h)
    o[n++] = ',';
    int k = rp_json(o + n, cap - n);
    n = k > 0 && k < cap - n ? n + k : n - 1;
    o[n] = 0;
  }
#endif
  return n;
}

CORE_COLD const Entry* find(Tid t) { return t ? find_e(t) : nullptr; }
CORE_COLD const Stats& stats() { return g_stats; }

CORE_COLD void reset() {
  for (Layer& l : g_l) {   // (every slot's)
    for (Entry& e : l.e) e = Entry();
    memset(l.ix, 0, sizeof l.ix);
    l.hi = 0, l.vdirty = 0;
  }
  g_source = nullptr;
#if CORE_POLY
  rytm_out_reset();
  rp_reset();
#endif
  g_started = false, g_last_us = 0;
  for (uint32_t& m : g_user_ms) m = 0;
  g_stats = Stats();
}

}  // namespace mod
}  // namespace core
