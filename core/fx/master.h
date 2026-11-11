// core/fx/master.h - the master FX (docs/MASTER-FX.md): one stereo chain after the Rytm's own FX, on the POST return. The
// Rytm's analog mix cannot be replaced from the Pi, only added to: the chain takes what the Sidecar sends back (OUT 0/1,
// and 2/3 moved onto 0/1), not the analog mix. SRC MIX adds the Rytm's stems: the whole mix once its tracks are off ROUTE TO
// MAIN (the Rytm's own setting), panned from the Rytm's report 50 (0030-fx-in); stems still on main stay out of the sum.
//
// One FX code path: the chain is ins::Engine's chains kM0, kM0 + 1 (four slots in a row, then the second's POST EQ), with
// every INSERT TYPE, AMP among them (its own cap, an L/R pair on a slot if asked), and the send FX's types as inserts:
// reached by SendFx::Insert (the same fades, zones and clears), mono in, a MIX knob each. Saved per kit in the INSERT
// file: the slots' and PEQ's lines, one MFX line.
//
// 0.5.0, send FX as insert FX: the bus chains B1..B3 take the send TYPEs too, under one cap with the master's slots
// (kFxSlots, 4 at once, anywhere among the master's 4 slots and the buses' 6). The room is booked here (accept): a slot
// with a send TYPE holds one of the kFxSlots SendFx slots, on two more fx::SendFx (kFxUnits, 5.9 MB each, the send FX's
// own SendFx untouched). Every chain that runs them runs on the send FX's core (the Pi's render core 2: the buses inside
// sendfx_pi::Render, the master in masterfx_pi::Render, one after the other), so the bookings and the SendFx are that
// core's alone. The voice chains stay refused: they run on the core that renders the voice (core 1 for voices 1..4).
// A bus slot's MIX is kept here (mix[] after the master's 4) and saved on its slot line ("B3 A RINGS P1 P2 P3 P4 MIX").
//
// Slots here are 0..3: chain(i) and slot(i) give the insert's chain and slot. ins::set_type() and friends take those, and
// everything that keeps state stays there. CORE_POLY builds only (the Pi and the Mac).
#pragma once
#include <stdint.h>

#include "fx/insert.h"

namespace core {
namespace master {

constexpr int kSlots = ins::kMasters * ins::kSlots;   // 4
constexpr int kBusSlots = ins::kBuses * ins::kSlots;  // 6: B1 A, B1 B .. B3 B
constexpr int kAllSlots = kSlots + kBusSlots;         // 10: what slot_index() numbers, the master's first
// send TYPEs playing at once, the master and the buses together: two SendFx of two slots each. Four because of core 2:
// each costs up to ~6.5 % of a Pi 4 core (CLOUD; RINGS ~5 %), core 2 carries ~40 % before them (voices 5..8, the send FX,
// the buses' and the master's chains), so four is ~25 % more and a fifth and sixth would leave little for heavy voices
constexpr int kFxSlots = 4;
constexpr int kFxUnits = 2;
constexpr int kEqChain = ins::kM0 + ins::kMasters - 1;   // the POST EQ at the end of the chain
inline int chain(int i) { return ins::kM0 + i / ins::kSlots; }
inline int slot(int i) { return i % ins::kSlots; }
// chain c's slot s as a send TYPE slot: 0..3 the master's, 4..9 the buses' (B1 A = 4); -1 a voice's (none)
inline int slot_index(int c, int s) {
  if (s < 0 || s >= ins::kSlots) return -1;
  if (c >= ins::kM0 && c < ins::kChains) return (c - ins::kM0) * ins::kSlots + s;
  if (c >= ins::kBus0 && c < ins::kM0) return kSlots + (c - ins::kBus0) * ins::kSlots + s;
  return -1;
}
inline int index_chain(int i) { return i < kSlots ? chain(i) : ins::kBus0 + (i - kSlots) / ins::kSlots; }
enum Src : uint8_t { kRet = 0, kMix = 1 };
constexpr uint8_t kLevUnity = 100, kMixDef = 40;
constexpr int kStems = 8;                             // IN 2..9: BD SD RS/CP BT LT MT/HT CH/OH CY/CB
// The chain's output: not the ceiling, a bound on what reaches the Pi's limiter (masterfx_pi, core/fx/limiter.h), which holds 0/1
// (full scale there: -10.9 dB into the mix on the real Rytm) while a chain plays. Far over it, +24 dB: linear to 70 %, out_mix.h's knee
constexpr float kOutLimit = 16.0f;
constexpr uint8_t kTrimDef = 6, kTrimMax = 24;        // the stem trim, dB down

// What the render core and core 0 read: byte fields, a torn read cannot happen
struct Control {
  volatile uint8_t byp;            // 1: the chain's output is out, the dry path back (bit for bit once faded)
  volatile uint8_t src;            // Src
  volatile uint8_t lev;            // the chain's output, 0..127, 100 unity
  volatile uint8_t trim;           // SRC MIX: every stem before the sum, dB down, 0..kTrimMax (smoothed: masterfx_pi)
  volatile uint8_t mix[kAllSlots]; // a send TYPE slot's dry / wet, 0..127 (slot_index: the master's, then the buses')
  // core/mod kMfx (MFX:LEV, MFX:MIX1..4): offsets on lev / mix as they play (played_lev / played_mix), never saved
  volatile int8_t mod_lev, mod_mix[kSlots];
  volatile int8_t fx[kAllSlots];   // the SendFx slot (0..kFxSlots-1) a send TYPE holds, -1 none
  // The Rytm's report 50: the stems' pans (0..127, 64 centre, as they play: p-locks and LFOs in) and the stems off ROUTE TO
  // MAIN (a bit a stem). Not saved, not the kit's. known 0: no report (older mods): every stem summed, centred
  volatile uint8_t pan[kStems];
  volatile uint8_t off_main;
  volatile uint8_t known;
};
extern Control control;
inline int played_lev() { int x = control.lev + control.mod_lev; return x < 0 ? 0 : x > 127 ? 127 : x; }
inline int played_mix(int i) {   // (the buses' MIX has no modulation: MFX:MIX1..4 are the master's)
  if (i < 0 || i >= kAllSlots) return 0;
  int x = control.mix[i] + (i < kSlots ? control.mod_mix[i] : 0);
  return x < 0 ? 0 : x > 127 ? 127 : x;
}
void mod_offset(int param, int off);   // core/mod kMfx: param 0 LEV, 1..4 MIX of slot 1..4; -127..127
extern volatile float lim_gain;    // core 0's: the limiter's lowest gain of the last second or so, 1 while it is out
int gr10();                        // ... as its reduction, in tenths of a dB (any core but the audio's: a log)

// ---- the control side (the Pi's core 3)
bool on();                         // a slot has a TYPE, or the POST EQ is set: the chain runs
bool accept(int c, int s, int t);  // ins::set_type's question: chain c's slot s may take TYPE t (the room for send TYPEs is
                                   // booked here; the answer is no past kFxSlots, and on a voice chain: the slot keeps what it has)
int fx_used();                     // send TYPE slots booked now (0..kFxSlots)
void set_byp(int on);
void set_src(int src);
void set_lev(int v);
void set_trim(int db);             // the stem trim, 0 .. -24 (dB)
void set_mix(int i, int v);        // i: slot_index (the master's 0..3, the buses' 4..9)
void set_pair(int i, int on);      // slot i's AMP: an L/R pair
bool pair(int i);
bool send_type(int t);             // t is one of the send TYPEs
int next_type(int i, int t, int d);   // the TYPE after t for slot i (slot_index), d +1 / -1, round the list (the UIs' TYPE
                                      // key: skips what it cannot take: AMP on a bus, a send TYPE without room)
bool room(int i);                  // slot i may take a send TYPE now (it holds one, or one is free)
int full_text(int t, char* b, int cap);   // the cap's refusal: "RINGS: 4 send FX inserts at most, ..."
bool zone_text(int t, int k, int v, char* b, int cap);   // a send TYPE's zone name for knob k at v; false: none, a number
void clear();                      // defaults: nothing set (ins::clear); the Rytm's report stays
void clear_master();               // the master's own part only (mfx=reset): the buses' send TYPE rooms and MIX stay
bool report(const uint8_t* d, int len);   // F0 7D 52 54 50 <8 pans> <route byte's bit 7> <its bits 0..6> F7; false: not this one
void link_lost();                  // the Rytm's USB gone: the report is forgotten until the next
int stems_on_main();               // how many stems are still on main (0 while no report)
void reset();                      // tests: the audio side too
// The console: "mfx" (the state), "mfx=byp:0|1", "mfx=src:ret|mix", "mfx=lev:V", "mfx=trim:DB", "mfx=S:type:T", "mfx=S:p1..p4:V",
// "mfx=S:mix:V", "mfx=S:pair:0|1", "mfx=S:off" (S 1..4), "mfx=eq:hpf|lpf|low|high:V", "mfx=eq:off", "mfx=reset"
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);      // "\"mfx\":{...}" (inside "ins", docs/MASTER-FX.md)
// The card (insert_card.cpp): "MFX BYP SRC LEV MIX1 MIX2 MIX3 MIX4 PAIRS [TRIM]" (words, as "MFX 0 MIX 100 40 40 40 40 3 -12";
// PAIRS a bit per slot, TRIM in dB, left out at its default), written only off the defaults
constexpr int kLineWords = 10;
int line(char* b, int cap);
bool from_line(const char* const* w, int n);

// ---- the audio side
// The render core's: n frames L/R through the chain, LEV and the bound (kOutLimit), in place (below its knee sample-exact).
// y's peaks, in and out, kept for the meters.
void render(float (*y)[2], int n);
void take_peaks(float* in, float* out);   // the largest |sample| since the last call
// The boot bench: a chain without AMP (COMP, EQ, SPACE, CLOUD, the POST EQ) over 1 s in 12-frame blocks through render(), on
// the real engine; core::cycles() ticks, *worst_block the longest block. 0 when a chain or a bus send TYPE is set (nothing
// touched: its clear() would take the buses' rooms).
uint64_t bench(uint32_t* worst_block);
unsigned long heap_bytes();        // the inserts' SendFx (on CORE_HEAP_BUFFERS ports; else built on first use)
void heap_place(void* mem);

}  // namespace master
}  // namespace core
