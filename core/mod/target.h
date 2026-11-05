// core/mod/target.h - the modulation layer's targets (spec-050 1.3): one id for every value a source (the LFOs; later
// scenes, MIDI learn) may move, `kind << 11 | index << 5 | param`, 0 = none, and the kinds' table (core/mod/kinds.cpp):
// whether an id names a target this build has, its range and mode, its name. Names are `<kind><index>:<param>`, the
// indices 1-based: V3:P5 (voice 3's P5), FX1:P2 (send FX 1's P2), S5:F1 (Rytm track 5's send to FX1), I3:A2 / B1:E1 /
// M1:B3 (an INSERT chain: I1..I8 the voices', B1..B3 the buses', M1 M2 the master's; A1..A4 / B1..B4 slot A / B's
// P1..P4, E1..E4 its POST EQ HPF LPF LOW HIGH), X2:B1 (voice 2's send to bus 1), MFX:LEV, MFX:MIX2 (the master's LEV,
// slot 2's MIX), O4:PAN (voice 4's OUT LEV PAN WID), T1:SPD (a Sidecar seq track's SPD DIR SWG LEN RNDM RLOW ROCT
// RWGT), R1:GATE (track 1's arpeggiator MODE SPD RNG GATE SWNG), LOOP:BAL (the looper's BAL LEV), RYTM:SCN,
// RYTM:PERF3 (the Rytm's scene and PERF macros). A name this build does not know does not parse.
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
  kMfx,    // master: param 0 LEV, 1..4 MIX of master slot 1..4 (core/fx/master.h Control lev, mix[])
  kOut,    // voice vi 0..7, param 0..2 = LEV PAN WID (core/fx/out.h)
  kSeq,    // Rytm track r 0..11 (a Sidecar seq track), param 0 SPD 1 DIR 2 SWG 3 LEN 4 RNDM 5 RLOW 6 ROCT 7 RWGT
  kArp,    // Rytm track r 0..11, param 0..4 = MODE SPD RNG GATE SWNG (core/arp/arp.h Cfg)
  kLoop,   // index 0, param 0 BAL, 1 LEV (spec-050 3.8)
  kRytm,   // index 0, param 0 SCN (Rytm scene 0..12), 1..12 PERF macro 1..12 (spec-050 2.7)
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
bool is_offset(Kind k);                             // the writer takes an offset on the knob (kIns kIsnd kMfx kOut)
const char* name(Tid t, char* b, int cap);          // "V3:P5"; "" when not valid
bool parse(const char* s, Tid* out);                // a name, any case; false when this build has no such target

// ---- the kinds' base readers and writers (control side)
float base_now(Tid t);       // a new entry's base: what plays now (v[vi].p[k], the FX knob; 0 for an offset kind)
void put(Tid t, float v);    // the writer: the value (kV kFx), the offset rounded by the caller (kIns kOut); kV only
                             // stores (the caller marks the voice dirty); the kinds without a writer yet: nothing

}  // namespace mod
}  // namespace core
