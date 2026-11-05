// core/mod/mod.h - the modulation layer (spec-050 section 1): every modulated target's entry - its base (what the Rytm,
// the knob or a lock says), the sources' offsets summed this tick, the value written - and the one tick that applies
// them. Sources: the LFOs (core/lfo.cpp, 0..7), the scenes' morph (core/mod/scene.h), later MIDI learn. The value:
//   start = held ? the manual edit : the morph this tick ? the morph's value : base      (spec-050 1.2, 2.5)
//   value = clamp(start + the sum of the offsets, lo, hi)
// written when it moved by 1/256 (kV) or one integer step (every other kind), restored to its base when no source
// refers to it any more. A new entry's base is what plays now; voice_params / fx_knob move a live entry's base only (no
// base is kept for a target nothing modulates). The offset kinds (kIns kIsnd kMfx kOut) write the offset itself on
// their knob, rounded (their base is the knob, read where they are applied). Bit for bit 0.4.0's LFOs: the entries
// keep its floats and its order (array order, first free; core/tests/test_mod_golden.cpp).
// Everything runs on the control side (core 3 on the Pi), between blocks: tick() from the control loop's every pass.
#pragma once
#include <stdint.h>

#include "mod/target.h"

namespace core {
namespace mod {

#if CORE_POLY
constexpr int kEntries = 256;    // live targets: 8 LFOs + 2 scenes x 64 locks + 32 MIDI routes, plus 64 for an A / B
                                 // change (the old scene's entries restored before the new ones are allocated)
#else
constexpr int kEntries = 16;     // the Sidecar Mini: its LFOs only
#endif
constexpr int kSources = 48;     // 0..7 the LFOs, 8..39 MIDI-learn routes, 40..47 reserved (API 3 bus signals)
constexpr uint32_t kUserMs = 5;  // a user machine's parameter call at most this often
struct Entry {
  Tid t;               // 0: a free entry
  uint8_t live;        // a source or the morph refers to it this tick
  uint8_t held;        // spec-050 2.5 (Phase 2)
  float base, off, morph, manual, applied;
  uint8_t morph_on;
};

void tick(uint32_t us);                              // the control loop's entry, every pass: the 1 ms gate (below)
void begin_tick();                                   // offsets to 0, live flags cleared
// a source's offset this tick, summed as e.off += depth * w in ONE expression here: the same multiply-add the LFO
// compiled to on 0.4.0 (fused under -ffp-contract=fast), so the sum is bit for bit 0.4.0's
void offset(int src, Tid t, float depth, float w);
void morph(Tid t, float value);                      // the scene layer's value this tick (an offset kind: the knob's)
void release(Tid t);                                 // an entry no source referred to yet this tick: its base, now
float base(Tid t);                                   // what t plays under a morph: a live entry's base, else what
                                                     // plays now; an offset kind's knob (kinds.cpp knob_now)
void end_tick();                                     // apply: write what changed, restore what left
void flush_voices();                                 // the dirty voices' voice_params_changed, user machines every kUserMs
void restore_all();                                  // every entry back to its base, every dirty voice flushed now
// the base hooks (control side)
void voice_params(int vi, float* p, int np);         // voice_report: a live entry's base from p, p modulated
uint8_t fx_knob(int slot, int knob, uint8_t v);      // fx::knob_filter: the value to play
uint8_t fx_send(int track, int slot, uint8_t v);     // fx::send_filter: a track's send, the value to play
float base_of(int vi, int k, float cur);             // a voice parameter's base, cur when unmodulated
uint8_t knob_base(int slot, int knob);               // send FX knob 0..5 as the kit has it (fx::control.knob plays)
uint8_t send_base(int track, int slot);              // a track's send as the kit has it (fx::control.send plays)
void played(int vi, float out[16]);                  // the voice's values as they play (v[vi].p)
void manual(Tid t, float v);                         // a Sidecar-side manual edit (spec-050 2.5; Task 8)
void fader_moved();                                  // clears every held flag (Task 8)
const Entry* find(Tid t);
// worst_us: the slowest whole tick(); flush_us: the slowest flush_voices (a user machine's params call is in it);
// lookup_us: the slowest base lookup block a caller noted (note_lookup: seq's lock apply, a voice report, the remote
// page's edit); dropped: offsets refused with the table full; scene_sets: writes of a morphed or held entry (not in
// voice_sets / fx_sets, which the LFOs' stats line shows). All since the boot / reset().
struct Stats { uint32_t ticks, voice_sets, user_calls, fx_sets, restores, seq_sets, rytm_out, worst_us, flush_us, lookup_us, dropped, scene_sets; };
void note_lookup(uint32_t us);                       // a caller's base lookups (base_of / voice_params) took us
const Stats& stats();
// the console: "mod" -> "mod: 3 live: V1:P3 66.00 (base 60.00 off +6), FX1:P3 50 (base 40 off +10), I1:A2 +12 (offset)";
// "mod=stats" -> the counts, worst_us, flush_us, lookup_us (Stats) and the entries' high-water mark. False: not "mod..."
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);                        // "\"mod\":{\"live\":N,\"worst_us\":U,\"ticks\":..,...}"
void reset();                                        // tests: no entries, the clock forgotten, the stats zero
// tests: the sources tick() calls in place of the LFOs' (null: the LFOs; reset() clears it)
void set_source_fn(void (*fn)(uint32_t us, uint32_t dus));

}  // namespace mod
}  // namespace core
