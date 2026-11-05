// core/mod/mod.cpp - see mod.h. Control side only, cold: a tick walks the live entries once a millisecond at most.
// The arithmetic is 0.4.0's lfo.cpp's, expression for expression (spec-050 1.4, 1.9): a kV value
// clampf(base + off, 0, 127.99), written when it moved by 1/256; a kFx value rnd0(clampf(base + off, 0, 127)); an offset
// kind's (int)(off +- 0.5). Never split `e.off += depth * w` across functions (the golden would move).
#include "mod/mod.h"

#include <math.h>
#include <string.h>

#include "fx/send_fx.h"
#include "lfo.h"
#include "platform.h"
#include "voices.h"

namespace core {
namespace mod {

namespace {

Entry g_e[kEntries];
uint32_t g_last_us = 0;
bool g_started = false;
uint8_t g_vdirty = 0;   // voices whose v[vi].p moved: their machines take them (flush_voices)
uint32_t g_user_ms[kVoices];
Stats g_stats;
void (*g_source)(uint32_t us, uint32_t dus) = nullptr;

CORE_COLD float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
CORE_COLD int rnd0(float x) { return (int)(x + 0.5f); }   // x >= 0

CORE_COLD Entry* find_e(Tid t) {
  for (Entry& e : g_e)
    if (e.t == t) return &e;
  return nullptr;
}

// an entry that leaves: its target back to its base (an offset kind's offset to 0)
CORE_COLD void restore(Entry& e) {
  ++g_stats.restores;
  Kind k = kind_of(e.t);
  if (k == kV) {
    put(e.t, e.base);
    g_vdirty |= (uint8_t)(1u << index_of(e.t));
  } else if (is_offset(k)) {
    put(e.t, 0);
  } else {
    put(e.t, e.base);
  }
  e = Entry();
}

}  // namespace

// the send FX's hook into the layer (fx/send_fx.h; its knob_filter is the LFOs', which forwards here), set before main
static struct FxHooks {
  FxHooks() { fx::send_filter = fx_send; }
} g_fx_hooks;

CORE_COLD void set_source_fn(void (*fn)(uint32_t us, uint32_t dus)) { g_source = fn; }

CORE_COLD void begin_tick() {
  for (Entry& e : g_e) e.off = 0, e.live = 0;
}

CORE_COLD void offset(int src, Tid t, float depth, float w) {
  (void)src;
  Entry* e = find_e(t);
  if (!e)
    for (Entry& f : g_e)
      if (!f.t) {   // first free: the order 0.4.0's table filled in, so the writes come in its order
        e = &f;
        e->t = t;
        e->base = base_now(t);
        e->applied = e->base;
        break;
      }
  if (!e) return;   // (full: cannot happen with the LFOs alone)
  e->live = 1;
  e->off += depth * w;
}

CORE_COLD void morph(Tid, float) {}
CORE_COLD void manual(Tid, float) {}
CORE_COLD void fader_moved() {}

CORE_COLD void end_tick() {
  for (Entry& e : g_e) {
    if (!e.t) continue;
    if (!e.live) {
      restore(e);
      continue;
    }
    Kind k = kind_of(e.t);
    if (k == kV) {
      float val = clampf(e.base + e.off, 0.0f, 127.99f);
      if (fabsf(val - e.applied) >= 1.0f / 256.0f) {
        put(e.t, val);
        e.applied = val;
        g_vdirty |= (uint8_t)(1u << index_of(e.t));
        ++g_stats.voice_sets;
      }
    } else if (is_offset(k)) {   // the offset itself (the knob / a step lock is the base, where it is applied)
      int iv = (int)(e.off + (e.off < 0 ? -0.5f : 0.5f));
      if ((float)iv != e.applied) {
        put(e.t, (float)iv);
        e.applied = (float)iv;
        ++g_stats.fx_sets;
      }
    } else {
      int iv = rnd0(clampf(e.base + e.off, 0.0f, 127.0f));
      if ((float)iv != e.applied) {
        put(e.t, (float)iv);
        e.applied = (float)iv;
        ++g_stats.fx_sets;
      }
    }
  }
}

CORE_COLD void flush_voices() {
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
    /* Phase 2: scene::tick(ms); learn::tick(); */
    end_tick();
  }
  flush_voices();
}

CORE_COLD void restore_all() {
  for (Entry& e : g_e)
    if (e.t) restore(e);
  for (int vi = 0; vi < kVoices; ++vi)
    if (g_vdirty >> vi & 1) voice_params_changed(vi);
  g_vdirty = 0;
}

CORE_COLD void voice_params(int vi, float* p, int np) {
#if !CORE_MOD_LFO
  lfo::voice_params(vi, p, np, false);   // the rollback: the LFOs' own table
  return;
#endif
  for (Entry& e : g_e) {
    if (!e.t || kind_of(e.t) != kV || index_of(e.t) != (vi & 7) || param_of(e.t) >= np) continue;
    e.base = p[param_of(e.t)];
    float val = clampf(e.base + e.off, 0.0f, 127.99f);
    p[param_of(e.t)] = val;
    e.applied = val;
  }
}

CORE_COLD uint8_t fx_knob(int slot, int knob, uint8_t val) {
  if (knob < 2 || knob > 5) return val;
  Entry* e = find_e(tid(kFx, slot & 1, knob - 2));
  if (!e) return val;
  e->base = val;
  int iv = rnd0(clampf(e->base + e->off, 0.0f, 127.0f));
  e->applied = (float)iv;
  return (uint8_t)iv;
}

CORE_COLD uint8_t fx_send(int track, int slot, uint8_t val) {
  Entry* e = find_e(tid(kSnd, track, slot & 1));
  if (!e) return val;
  e->base = val;
  int iv = rnd0(clampf(e->base + e->off, 0.0f, 127.0f));
  e->applied = (float)iv;
  return (uint8_t)iv;
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
    for (const Entry& e : g_e) live += e.t != 0;
    int n = sniprintf(o, cap, "mod: %d live", live);
    bool first = true;
    for (const Entry& e : g_e) {
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
    sniprintf(o, cap, "mod: ticks %u voice_sets %u user_calls %u fx_sets %u restores %u worst_us %u", (unsigned)s.ticks,
              (unsigned)s.voice_sets, (unsigned)s.user_calls, (unsigned)s.fx_sets, (unsigned)s.restores,
              (unsigned)s.worst_us);
    return true;
  }
  sniprintf(o, cap, "mod | mod=stats");
  return true;
}

CORE_COLD int json(char* o, int cap) {
  int live = 0;
  for (const Entry& e : g_e) live += e.t != 0;
  const Stats& s = g_stats;
  int n = sniprintf(o, cap, "\"mod\":{\"live\":%d,\"worst_us\":%u,\"ticks\":%u,\"voice_sets\":%u,\"fx_sets\":%u,"
                    "\"restores\":%u}", live, (unsigned)s.worst_us, (unsigned)s.ticks, (unsigned)s.voice_sets,
                    (unsigned)s.fx_sets, (unsigned)s.restores);
  return n < 0 ? 0 : n;
}

CORE_COLD const Entry* find(Tid t) { return t ? find_e(t) : nullptr; }
CORE_COLD const Stats& stats() { return g_stats; }

CORE_COLD void reset() {
  for (Entry& e : g_e) e = Entry();
  g_started = false, g_last_us = 0, g_vdirty = 0;
  for (uint32_t& m : g_user_ms) m = 0;
  g_stats = Stats();
}

}  // namespace mod
}  // namespace core
