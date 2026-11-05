// core/mod/mod.h - the modulation layer (spec-050 section 1): every modulated target's entry - its base (what the Rytm,
// the knob or a lock says), the sources' offsets summed this tick, the value written - and the one tick that applies
// them. Sources: the LFOs (core/lfo.cpp, 0..7), later the scenes' morph and MIDI learn. The value that plays:
//   value = clamp(base + the sum of the offsets, lo, hi)     (Phase 2: the morph or a held manual value for base)
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
void morph(Tid t, float value);                      // the scene layer's value this tick (Phase 2: a stub)
void end_tick();                                     // apply: write what changed, restore what left
void flush_voices();                                 // the dirty voices' voice_params_changed, user machines every kUserMs
void restore_all();                                  // every entry back to its base, every dirty voice flushed now
// the base hooks (control side)
void voice_params(int vi, float* p, int np);         // voice_report: a live entry's base from p, p modulated
uint8_t fx_knob(int slot, int knob, uint8_t v);      // fx::knob_filter: the value to play
uint8_t fx_send(int track, int slot, uint8_t v);     // fx::send_filter: a track's send, the value to play
float base_of(int vi, int k, float cur);             // a voice parameter's base, cur when unmodulated
void played(int vi, float out[16]);                  // the voice's values as they play (v[vi].p)
void manual(Tid t, float v);                         // a Sidecar-side manual edit (spec-050 2.5; Phase 2: a stub)
void fader_moved();                                  // clears every held flag (Phase 2: a stub)
const Entry* find(Tid t);
struct Stats { uint32_t ticks, voice_sets, user_calls, fx_sets, restores, seq_sets, rytm_out, worst_us; };
const Stats& stats();
void reset();                                        // tests: no entries, the clock forgotten, the stats zero
// tests: the sources tick() calls in place of the LFOs' (null: the LFOs)
void set_source_fn(void (*fn)(uint32_t us, uint32_t dus));

}  // namespace mod
}  // namespace core
