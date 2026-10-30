// core/inst_alloc.h - E0 (docs/SEQ-IDEAS.md, docs/POLYPHONY.md "E0"): what each Sidecar machine declares for the
// sequencer's poly tracks (its default and largest VOICES, how a note sets its pitch, its cost per instance), the voice
// budget check, and the allocator of a track's instances (the Analog Four's ALLOCATION modes, UNISON's stack). Pure: no
// engine, no card, no clock - the tests and any core. The instances themselves (their engines' state and rendering):
// core/inst.h.
//
// Words (device-agnostic, the A4's): a track has VOICES notes at once; with UNISON u each note is a "lane" of u
// instances (detuned and panned across the stack), so a track uses voices x u instances, capped at kMaxInst; the notes
// at once are then lanes = instances / u (>= 1).
#pragma once
#include <stdint.h>

#include "seq/seq.h"   // PolyCfg, kAlloc*

namespace core {
namespace inst {

constexpr int kMaxInst = 8;   // instances one voice (one track's poly config) uses at most
// How a machine takes a note's pitch (declared per machine):
enum : uint8_t {
  kPitchNote = 0,   // the note is the pitch (+ the machine's own TUN / FIN): PLAITS, JUNO, DAISYSP, DSP SAMPLE, FM, ...
  // 1 was kPitchTune (DIGIDRUM's PTCH: the note offset a TUNE-like knob), gone with DIGIDRUM 2026-10-29
  kPitchNone = 2,   // the note is ignored (a fixed-pitch drum: a user machine that says so)
};
struct Decl {
  int machine;        // the voice byte's machine (machines.h), a user slot's flag for user machines
  uint8_t def_voices; // VOICES at AUTO: drums 1, tonal machines their count before E0 (the note machines' pools) or 4
  uint8_t max_voices; // the largest VOICES (1: mono only - DSP SAMPLE streams one sample per voice from the card)
  uint8_t pitch;      // kPitch*
  bool own_pool;      // the machine has its own note pool (6-OP FM, MULTISAMPLE, WAVETABLE, GRANULAR): VOICES caps the
                      // notes held at once there; ALLOCATION / UNISON are the instance pool's (below)
  uint16_t cpu_mean;  // % of a Pi 4 core per instance x 100, the bench's mean [measured: docs/pi-engine.md, task7-bench]
  uint16_t cpu_peak;  // the same for the budget check, a block's worst case: the built-ins' worst warm block (1.8 %,
                      // cores 1-3) [measured, docs/pi-engine.md]; a user machine 2.5 x its declared mean [inferred]
};
// The built-in machine m (voice byte & 0x78; kPlaits for any unknown flag, as voices.cpp plays it). null: a user slot
// (its Decl comes from the package: user_decl).
const Decl* builtin(int m);
// A user machine's Decl from its package (rmp_format.h's header flags: max_voices, pitch; the entry table's cpu
// permille of a Teensy voice); old packages (no flags): mono, pitch from the note. AUTO: 1 for a drum (pitch none),
// else min(4, max). The declared cost on the Pi 4: permille / 1000 of a Teensy voice budget (1/8 of the Teensy) [inferred]
// over the Teensy -> Pi 4 speed ratio 7.9 (PLAITS: 46 % / 8 voices on the Teensy, 0.73 % on the Pi) [measured, both].

Decl user_decl(int machine, uint32_t header_flags, uint32_t cpu_permille, uint32_t state_bytes);


// The voice budget (pi/engine/guard.h: 25 % of a core a voice, three blocks over it mute the voice): instances are
// held to kBudgetPct (80 % of it) by their declared peak cost, the voice's own machine included.
constexpr int kBudgetPct100 = 2000;   // 20.00 %
// The instances voice cfg resolves to: VOICES (AUTO: def_voices) x UNISON, clamped to the machine's max, kMaxInst and
// the budget. lanes: the notes at once (instances / unison, >= 1); unison: the stack actually used; clamped: the budget
// or the machine's max cut it.
struct Resolved {
  int instances, lanes, unison;
  bool clamped;
  int cpu100;   // the instances' declared mean cost (% x 100), for STATUS
};
Resolved resolve(const seq::PolyCfg& cfg, const Decl& d);

// ---- the allocator: one voice's lanes. The caller owns the state; times are the caller's counter (any increasing
// number: the note order). A lane is idle (free and quiet), released (its note off, its tail sounding) or held.
struct Lane {
  int8_t note;      // the pitch it plays / played last (-1 never)
  bool held;        // gated: note on, not yet off
  bool sounding;    // something audible (held, or a tail above the quiet floor: the renderer clears it)
  uint32_t order;   // when its note started (the oldest steals first)
  float level;      // the renderer's last block peak (the quietest released tail steals first)
};
struct Alloc {
  int lanes;        // in use, 1..kMaxInst
  int rot;          // ROTATE's cursor: the lane after the last one used
  uint32_t clock;   // the next note's order
  Lane lane[kMaxInst];
  uint32_t steals;  // held notes cut for a new one
};
void reset(Alloc& a, int lanes);
// A note on: the lane it plays on (already marked held with this note). *stole: the lane cut a held note (1) or a
// sounding tail (2), 0 neither. Rules (all modes): the same pitch held -> that lane again (retrigger, no double); one
// lane (VOICES 1) -> that lane, retriggered (mono: the new note chokes the old). Then by mode:
//   REASSIGN: the lane that played this pitch last if it is not held; else an idle lane, the cursor's first; else the
//             quietest released tail; else the oldest held note.
//   ROTATE:   from the cursor, the first lane not held (a tail is cut: the classic round robin); all held: the oldest.
//   RESET:    the lowest lane not held (always from the first: the most aggressive); all held: the oldest.
int note_on(Alloc& a, const seq::PolyCfg& cfg, int note, int* stole);
// A note off: the held lane with that pitch released (-1: none held it)
int note_off(Alloc& a, int note);
// The console's / the pages' words for a config: "auto" (all AUTO) or KEY=VALUE pairs separated by ',' - voices=0..8
// (0 AUTO), alloc=reassign|rotate|reset (or 0..2), unison=0..8, detune=0..127, spread=0..127, group=0..4 (the keys'
// first letters are enough) - applied over *c. false: a bad pair (err says which; *c may be partly changed).
bool parse_cfg(const char* kv, seq::PolyCfg* c, char* err, int cap);
// "VOICES 4 ROTATE UNISON 2 ..." and, with a machine (r non-null), " -> PLAITS: 8 instances, 4 notes at once (5.84 %)"
int describe(const seq::PolyCfg& c, const Resolved* r, int machine, char* out, int cap);
// The short form for the touchscreen's POLY panel: "PLAITS 4 INST 4 NOTES 2.92%" (+ " CUT" when clamped)
int describe_short(const Resolved& r, int machine, char* out, int cap);
const char* alloc_name(int a);   // "REASSIGN" "ROTATE" "RESET"

// UNISON: instance k (0..u-1) of a lane's stack: its pitch offset (cents) and pan (-1 left .. 1 right). One instance:
// 0, 0. detune 127 = +-50 cents at the stack's edges, spread 127 = hard left / right.
void unison_offset(const seq::PolyCfg& cfg, int u, int k, float* cents, float* pan);

}  // namespace inst
}  // namespace core
