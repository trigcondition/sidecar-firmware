// core/seq/filltargets.h - FILL's per-machine target registry (0.5.0; notes/design sampler-lock-fill 3): what the FILL
// page can write on the selected track's trigs, by the track's machine. The fill engine (lockfill.h) only knows a
// target's storage; this says which targets a machine has, what their values are and how they are stored.
//
//   SAMPLER, DSP SAMPLE      SAMPLE    P11 (lock type 10) + fine: the SAMPLES pool's rows A001..H127 [measured]
//   GRANULAR / WAVETABLE /   SOURCE /  P1 (type 0) + fine: the rows of its pool list (samples / wavetables /
//   MULTISAMPLE              TABLE /   instruments) [measured: bk_emu t_lock]
//                            INST
//   MD                       ENGINE    P9 (type 8): zones(3); TYPE P10 (type 9): zones of the track's ENGINE's types
//   PLAITS                   ENGINE    P1 (type 0): zones(17), the curated models' names
//   DAISYSP                  ENGINE    P1 (type 0): zones(3) MODAL STRING RESON
//   JUNO, 6-OP FM            PRESET    P1 = PAT (type 0): the project's preset list's rows
//   a user machine (U1..U5)  from its knobs (um::fill_knobs): its preset knob (PRESET: its presets 0..n-1), pool knobs
//                            (SAMPLE: pool slot 1..127), list knobs (their long name: zones of their names); at most 4
//   every track              VELOCITY  the track's velocity array: 1..127
// A value set skips what would silence or mean nothing: a pool row that is empty or OFF, an empty preset row (owner
// decision: always skipped in 0.5.0). Names come from where the Rytm's lists get them. Control core (Pi, Mac).
#pragma once
#include <stdint.h>

#include "seq/lockfill.h"
#include "user/um.h"

namespace core {
namespace filltargets {

constexpr int kMax = 5;   // targets a track has at most (the machine's, then VELOCITY)
enum Values : uint8_t { kPool = 0, kZones, kPresets, kUserPresets, kUserZones, kMdEngines, kMdTypes, kRange };
struct Def {
  char label[9];        // "SAMPLE" "ENGINE" "TYPE" "PRESET" "VELOCITY" ..
  lockfill::Target g;
  uint8_t values;       // Values
  int8_t arg;           // kPool: the pools list (pools::List); kPresets: presets kind; kUser*: the slot; kMdTypes: engine
  uint8_t p1;           // kUserZones: the knob's P (1..16)
  int16_t n;            // the domain: index 0..n-1 (kPool: the code bank * 128 + row; kRange: lo + index)
  int16_t lo;           // kRange's first value
  const char* names;    // kZones: '\0'-separated names (n of them)
  uint8_t order;        // the default ORDER (lockfill::Order): UP, VELOCITY RND
};
// the targets of a machine (machines.h numbering: kSmplMachine for SAMPLER, a user slot's flag 0x50..0x70, -1 / 0x7F
// a Rytm machine or unknown: VELOCITY only); md_engine: the MD track's ENGINE zone (TYPE follows it). Returns how many.
int for_machine(int machine, int md_engine, Def* out, int max);
// the selected track t's (its machine from the Rytm's 33, SAMPLER by its name; MD's ENGINE from its P9 as shown)
int for_track(int t, Def* out, int max);
int track_machine(int t);   // as for_machine wants it, -1 unknown / a Rytm machine
// a user machine's targets from its knobs (um::fill_knobs: the preset knob with npresets presets, pool knobs, list
// knobs of 2 names or more), in their order, then VELOCITY; at most max (VELOCITY always kept). slot: its U slot 0..4
int for_knobs(int slot, const um::FillKnob* k, int nk, int npresets, Def* out, int max);
uint16_t encode(const Def& d, int i);   // domain index -> the stored value (lockfill::value)
int index_of(const Def& d, uint16_t stored);   // the other way, -1 none
bool skip(const Def& d, int i);         // an empty / OFF row, an empty preset slot: not in a value set
// the value's name for the page ("C010 KICK", "VA VCF", "JUNO A11", "100"), into b
const char* name(const Def& d, int i, char* b, int cap);
// the next index from i in direction dir (+1 / -1) not skipped, staying in 0..n-1 (i itself when there is none)
int step(const Def& d, int i, int dir);
// the value set FROM .. TO (indexes; TO below FROM: downwards), skipped values left out, into set (max): how many
int value_set(const Def& d, int from, int to, uint16_t* set, int max);

}  // namespace filltargets
}  // namespace core
