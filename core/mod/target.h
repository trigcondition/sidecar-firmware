// core/mod/target.h - the modulation layer's targets (spec-050 1.3): one id for every value a source (the LFOs; later
// scenes, MIDI learn) may move, `kind << 11 | index << 5 | param`, 0 = none, and the kinds' table (core/mod/kinds.cpp):
// whether an id names a target this build has, its range and mode, its name. Names are `<kind><index>:<param>`, the
// indices 1-based: V3:P5 (voice 3's P5), FX1:P2 (send FX 1's P2), S5:F1 (Rytm track 5's send to FX1), I3:A2 / B1:E1 /
// M1:B3 (an INSERT chain: I1..I8 the voices', B1..B3 the buses', M1 M2 the master's; A1..A4 / B1..B4 slot A / B's
// P1..P4, E1..E4 its POST EQ HPF LPF LOW HIGH), X2:B1 (voice 2's send to bus 1), MFX:LEV, MFX:MIX2 (the master's LEV,
// slot 2's MIX), MFX:B3A (bus 3 slot A's send TYPE MIX), O4:PAN (voice 4's OUT LEV PAN WID), T1:SPD (a Sidecar seq track's SPD DIR SWG LEN RNDM RLOW ROCT
// RWGT), R1:GATE (track 1's arpeggiator MODE SPD RNG GATE SWNG), LOOP:BAL (the looper's BAL LEV), RYTM:SCN,
// RYTM:PERF3 (the Rytm's scene and PERF macros), RT3:FRQ / RT1:P2 / RT2:VEL / RFX:TIM (0.5.0, the Rytm's own sound
// parameters: notes/design rytm-param-mod 3.5, 5.1). A name this build does not know does not parse.
// Without CORE_POLY (the Sidecar Mini) only kV and kFx are targets.
#pragma once
#include <stdint.h>

namespace core {
namespace mod {

using Tid = uint16_t;   // kind << 11 | index << 5 | param; 0 = none
enum Kind : uint8_t {
  kNone = 0,
  kV,      // voice vi 0..7, param 0..15 = P1..P16                         base: voice_report, writer: v[vi].p
  kFx,     // send FX slot 0..1, param 0..3 = P2..P5                        base: 0x20 knob, writer: fx::control.knob
  kSnd,    // Rytm track 0..11, param 0..1 = its SEND to FX1 / FX2         base: 0x20 sends, writer: fx::control.send
  kIns,    // insert chain 0..12 (0..7 voices, 8..10 buses, 11..12 master), param 0..7 slot A/B P1..P4, 8..11 POST EQ
  kIsnd,   // voice vi 0..7 -> bus param 0..2 (ins::control.send)
  kMfx,    // param 0 LEV, 1..4 MIX of master slot 1..4, 5..10 the bus slots' (B1A..B3B; master.h lev, mix[slot_index])
  kOut,    // voice vi 0..7, param 0..2 = LEV PAN WID (core/fx/out.h)
  kSeq,    // Rytm track r 0..11 (a Sidecar seq track), param 0 SPD 1 DIR 2 SWG 3 LEN 4 RNDM 5 RLOW 6 ROCT 7 RWGT
  kArp,    // Rytm track r 0..11, param 0..4 = MODE SPD RNG GATE SWNG (core/arp/arp.h Cfg)
  kLoop,   // index 0, param 0 BAL, 1 LEV (spec-050 3.8)
  kRytm,   // index 0, param 0 SCN (Rytm scene 0..12), 1..12 PERF macro 1..12 (spec-050 2.7)
  // 0.5.0 (rytm-param-mod 5.1): the Rytm's own sound parameters, modulated on the Rytm (core/mod/rytm_param.h): the base
  // is the Rytm's (its knob, its p-lock, its scene), never read here; an offset (LFO, XY, MIDI learn) or an override
  // (a seq p-lock, the morph) goes out as SysEx. Names RT1..RT12 (kRs and kRa share them) and RFX
  kRs,     // Rytm track 0..11, param = word 1..24: LFO MUL WAV MOD (DST never) SPD FAD SPH DEP, SYNTH P1..P8, SAMPLE
  kRa,     // Rytm track 0..11, param 0..16 = words 25..41 (FILTER, AMP), 17..20 VM1..4, 21..24 AT1..4, 25 VEL
  kRfx,    // the FX track (index 0), param 0..28 = words 9..37, 29 SPD (5), 30 FAD (6), 31 DEP (8)
  kKinds
};
constexpr Tid tid(Kind k, int index, int param) { return (Tid)(k << 11 | (index & 63) << 5 | (param & 31)); }
inline Kind kind_of(Tid t) { return (Kind)(t >> 11); }
inline int index_of(Tid t) { return t >> 5 & 63; }
inline int param_of(Tid t) { return t & 31; }
enum Mode : uint8_t { kCont = 0, kStep = 1, kTrig = 2, kOff = 3 };   // = RMP3_MOD_* (sdk/rmp/rmp_api3.h)
struct Range { float lo, hi; Mode mode; };

// ---- the kinds' table (core/mod/kinds.cpp)
bool valid(Tid t);                                  // a target this build has
Range range(Tid t);                                 // its value's range and mode ({0, 0, kOff} when not valid). kV is
                                                    // kCont for every knob (mod never rounds a voice's value: a list
                                                    // knob's machine quantises it, as an LFO's on 0.4.0); the offset
                                                    // kinds (kIns, kIsnd, kMfx, kOut) give the offset's -127..127
bool is_offset(Kind k);                             // the writer takes an offset on the knob (kIns kIsnd kMfx kOut;
                                                    // the remote kinds kRs kRa kRfx: an offset on the Rytm's base)
inline bool is_remote(Kind k) { return k == kRs || k == kRa || k == kRfx; }   // the Rytm holds the base (rytm_param.h)
// a remote kind's (t, w) on the wire: track 0..12 (12 the FX track), word 1..41 / 48..56 (VM1..4, AT1..4, VEL); false
// when t is not a valid remote target
bool rytm_word(Tid t, int* track, int* word);
Tid rytm_tid(int track, int word);                  // the reverse (0: none)
// a remote target that takes no offset (a list word: MUL WAV MOD SMP LOP FTYP): overrides only (scenes, p-locks); the
// LFO / XY pickers leave it out
bool override_only(Tid t);
const char* name(Tid t, char* b, int cap);          // "V3:P5"; "" when not valid
bool parse(const char* s, Tid* out);                // a name, any case; false when this build has no such target
// For the screens (2026-11, the owner: "no idea what I'm setting, it just says P1"): the param as the target labels
// it now - a voice's by the machine it plays (core/ui voice_label: PLAITS "HRM", a user machine's manifest), send FX
// and INSERT by their TYPE (lfo::param_name: "CUT", "ACUT", "QHPF"), the others by name ("LEV", "PERF3"); "-" a P the
// machine / TYPE leaves unused. label: "V3:HRM", "FX1:CUT", "I2:ACUT", "MFX:LEV". "" when not valid. Never parsed.
const char* param_label(Tid t, char* b, int cap);
const char* label(Tid t, char* b, int cap);

// ---- the kinds' base readers and writers (control side)
float base_now(Tid t);       // a new entry's base: what plays now (v[vi].p[k], the FX knob, a send; 0 for an offset kind)
float knob_now(Tid t);       // an offset kind's knob (or step lock) its offset is applied on; base_now for the others
void put(Tid t, float v);    // the writer: the value (kV kFx kSnd), the offset rounded by the caller (kIns kIsnd kMfx
                             // kOut); kV only
                             // stores (the caller marks the voice dirty); the kinds without a writer yet: nothing

}  // namespace mod
}  // namespace core
