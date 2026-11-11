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
#include "mod/scene.h"
#include "platform.h"
#include "voices.h"

namespace core {
namespace mod {

namespace {

Entry g_e[kEntries];
// one past the highest entry in use: every walk stops there (allocation is first free, so the LFOs' few entries sit at
// the bottom and a lookup costs what 0.4.0's 16-entry table did, not kEntries)
int g_hi = 0;
struct Used {
  Entry* b;
  Entry* e;
  Entry* begin() const { return b; }
  Entry* end() const { return e; }
};
Used used() { return {g_e, g_e + g_hi}; }
uint32_t g_last_us = 0;
bool g_started = false;
uint8_t g_vdirty = 0;   // voices whose v[vi].p moved: their machines take them (flush_voices)
uint32_t g_user_ms[kVoices];
Stats g_stats;
void (*g_source)(uint32_t us, uint32_t dus) = nullptr;

CORE_COLD float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
CORE_COLD int rnd0(float x) { return (int)(x + 0.5f); }   // x >= 0

// the entries by id (spec-050 1.4, Phase 2: 128 morph calls a tick): 512 slots, linear probing, entry + 1 (0 empty);
// deletion shifts the run back (no tombstones). The walks still go in array order (the writes' order is 0.4.0's).
constexpr int kIx = 512;
uint16_t g_ix[kIx];
CORE_COLD int ix_home(Tid t) { return (int)((uint32_t)t * 40503u >> 7) & (kIx - 1); }
CORE_COLD Entry* find_e(Tid t) {
  if (!t) return nullptr;   // (a free entry's t is 0)
  for (int i = ix_home(t); g_ix[i]; i = (i + 1) & (kIx - 1))
    if (g_e[g_ix[i] - 1].t == t) return &g_e[g_ix[i] - 1];
  return nullptr;
}
CORE_COLD void ix_add(Entry& e) {
  int i = ix_home(e.t);
  while (g_ix[i]) i = (i + 1) & (kIx - 1);
  g_ix[i] = (uint16_t)(&e - g_e + 1);
}
CORE_COLD void ix_del(Entry& e) {
  int i = ix_home(e.t);
  while (g_ix[i] && g_ix[i] != &e - g_e + 1) i = (i + 1) & (kIx - 1);
  if (!g_ix[i]) return;
  g_ix[i] = 0;
  for (int j = (i + 1) & (kIx - 1); g_ix[j]; j = (j + 1) & (kIx - 1)) {
    int h = ix_home(g_e[g_ix[j] - 1].t);   // moves back into the hole unless its home lies in (i, j]
    bool stays = i <= j ? (h > i && h <= j) : (h > i || h <= j);
    if (!stays) g_ix[i] = g_ix[j], g_ix[j] = 0, i = j;
  }
}
// a new entry for t: the first free one (the order 0.4.0's table filled in, so the writes come in its order); its
// base what plays now. Null: the table is full
CORE_COLD Entry* alloc(Tid t) {
  for (Entry& f : g_e)
    if (!f.t) {
      f.t = t;
      f.base = base_now(t);
      // a kind whose writer takes a value (kSeq kArp kLoop kRytm) has no state the layer can read: nothing applied yet,
      // so its first value is always put (a RYTM:SCN lock at 0 goes out; rytm_out's own on-change check decides)
      Kind k = kind_of(t);
      f.applied = k == kSeq || k == kArp || k == kLoop || k == kRytm ? -1.0f : f.base;
      if (&f - g_e >= g_hi) g_hi = (int)(&f - g_e) + 1;
      ix_add(f);
      return &f;
    }
  return nullptr;
}
// what a target starts from this tick (spec-050 1.2): a held manual edit, the morph, else its base
CORE_COLD float start_of(const Entry& e) { return e.held ? e.manual : e.morph_on ? e.morph : e.base; }

// an entry that leaves: its target back to its base (an offset kind's offset to 0)
CORE_COLD void restore(Entry& e) {
  ++g_stats.restores;
  Kind k = kind_of(e.t);
  if (k == kV) {
    put(e.t, e.base);
    g_vdirty |= (uint8_t)(1u << index_of(e.t));
  } else if (is_offset(k)) {
    put(e.t, 0);
  } else if (k == kFx || k == kSnd) {
    put(e.t, e.base);
  }   // (kSeq kArp kLoop kRytm: nothing to put back - their writers take a value, not a state; Phases 3, 4)
#if CORE_POLY
  if (k == kRytm) rytm_out_forget(param_of(e.t));   // the Rytm may change it meanwhile: the next lock's value goes out
#endif
  ix_del(e);
  e = Entry();
  while (g_hi && !g_e[g_hi - 1].t) --g_hi;
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
  if (!valid(t)) return;   // (0 or a target this build has not: never into a free entry)
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
  if (!valid(t)) return;
  Entry* e = find_e(t);
  if (!e) e = alloc(t);
  if (!e) {
    ++g_stats.dropped;
    return;
  }
  e->live = 1, e->morph_on = 1, e->morph = value;
}
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
        g_vdirty |= (uint8_t)(1u << index_of(e.t));
        ++(sc ? g_stats.scene_sets : g_stats.voice_sets);
      }
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

static CORE_COLD void tick_body(uint32_t us);
CORE_COLD void tick(uint32_t us) {   // the whole tick timed (spec-050 1.5): stats().worst_us
  uint32_t t0 = now_us();
  tick_body(us);
  uint32_t d = now_us() - t0;
  if (d > g_stats.worst_us) g_stats.worst_us = d;
}
static CORE_COLD void tick_body(uint32_t us) {
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
    if (!g_vdirty) return;
  } else {
    if (dus > 100000) dus = 100000;   // a long stall: the sources skip ahead no more than 100 ms
    g_last_us = us;
    ++g_stats.ticks;
    begin_tick();
    if (g_source) g_source(us, dus);   // (tests)
    else lfo::waves(us, dus);         // the LFOs: the clock position, the waves, each one's offset
#if CORE_POLY
    learn::tick();                    // the MIDI-learn routes' offsets (core/mod/learn.h)
    scene::tick(now_ms());            // the morph: a value for each lock of A and B (core/mod/scene.h)
#endif
    end_tick();
#if CORE_POLY
    rytm_out_tick(now_ms());          // the Rytm's scene / macros whose 5 ms have passed (core/mod/rytm_out.h)
#endif
  }
  flush_voices();
}

CORE_COLD void restore_all() {
  for (Entry& e : used())
    if (e.t) restore(e);
  for (int vi = 0; vi < kVoices; ++vi)
    if (g_vdirty >> vi & 1) voice_params_changed(vi);
  g_vdirty = 0;
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
  for (Entry& e : used()) {
    if (!e.t || kind_of(e.t) != kV || index_of(e.t) != (vi & 7) || param_of(e.t) >= np) continue;
    e.base = p[param_of(e.t)];
    float val = clampf(start_of(e) + e.off, 0.0f, 127.99f);   // (a morph plays on: the report moves the base only)
    p[param_of(e.t)] = val;
    e.applied = val;
  }
}

CORE_COLD uint8_t fx_knob(int slot, int knob, uint8_t val) {
  if (knob < 2 || knob > 5) return val;
  Entry* e = find_e(tid(kFx, slot & 1, knob - 2));
  if (!e) return val;
  e->base = val;
  int iv = rnd0(clampf(start_of(*e) + e->off, 0.0f, 127.0f));
  e->applied = (float)iv;
  return (uint8_t)iv;
}

CORE_COLD uint8_t fx_send(int track, int slot, uint8_t val) {
  Entry* e = find_e(tid(kSnd, track, slot & 1));
  if (!e) return val;
  e->base = val;
  int iv = rnd0(clampf(start_of(*e) + e->off, 0.0f, 127.0f));
  e->applied = (float)iv;
  return (uint8_t)iv;
}

CORE_COLD uint8_t knob_base(int slot, int knob) {
  slot &= 1;
  if (knob < 0 || knob > 5) return 0;
  const Entry* e = knob >= 2 ? find_e(tid(kFx, slot, knob - 2)) : nullptr;
  return e ? (uint8_t)e->base : fx::control.knob[slot][knob];
}
CORE_COLD uint8_t send_base(int track, int slot) {
  if (track < 0 || track >= fx::kTracks) return 0;
  const Entry* e = find_e(tid(kSnd, track, slot & 1));
  return e ? (uint8_t)e->base : fx::control.send[track][slot & 1];
}

CORE_COLD float base_of(int vi, int k, float cur) {
#if !CORE_MOD_LFO
  return lfo::base_of(vi, k, cur);   // the rollback: the LFOs' own table
#endif
  const Entry* e = find_e(tid(kV, vi & 7, k & 15));
  return e ? e->base : cur;
}

CORE_COLD void played(int vi, float out[16]) { memcpy(out, (const float*)v[vi & 7].p, 16 * sizeof(float)); }

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
              (unsigned)s.scene_sets, (unsigned)s.restores, (unsigned)s.dropped, (unsigned)s.worst_us, (unsigned)s.flush_us, (unsigned)s.lookup_us, g_hi);
    return true;
  }
  sniprintf(o, cap, "mod | mod=stats");
  return true;
}

CORE_COLD int json(char* o, int cap) {
  int live = 0;
  for (const Entry& e : used()) live += e.t != 0;
  const Stats& s = g_stats;
  int n = sniprintf(o, cap, "\"mod\":{\"live\":%d,\"worst_us\":%u,\"flush_us\":%u,\"lookup_us\":%u,\"ticks\":%u,"
                    "\"voice_sets\":%u,\"fx_sets\":%u,\"restores\":%u,\"dropped\":%u}", live, (unsigned)s.worst_us,
                    (unsigned)s.flush_us, (unsigned)s.lookup_us, (unsigned)s.ticks, (unsigned)s.voice_sets,
                    (unsigned)s.fx_sets, (unsigned)s.restores, (unsigned)s.dropped);
  return n < 0 ? 0 : n;
}

CORE_COLD const Entry* find(Tid t) { return t ? find_e(t) : nullptr; }
CORE_COLD const Stats& stats() { return g_stats; }

CORE_COLD void reset() {
  for (Entry& e : g_e) e = Entry();
  memset(g_ix, 0, sizeof g_ix);
  g_hi = 0, g_source = nullptr;
#if CORE_POLY
  rytm_out_reset();
#endif
  g_started = false, g_last_us = 0, g_vdirty = 0;
  for (uint32_t& m : g_user_ms) m = 0;
  g_stats = Stats();
}

}  // namespace mod
}  // namespace core
