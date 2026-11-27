// core/device/admit.h - CPU admission of a box's voices at attach (two-box design, decided 2026-11-21, item 8 of the
// project's design alignment; docs/device-profiles.md "Voices and CPU admission").
//
// Each box asks for its type's voices (Profile::voices: the Rytm 8, the A4 4, the Digitakt 8), or the cmdline's
// voices= for its type. When a box attaches, its voices are admitted against the boot bench's costs, scaled to the CPU
// clock setting, so that each render core stays under kLimitBp (80 % of a core) in the worst case:
//   - a voice costs the worst machine in the bench (any voice may play any machine), plus its POST EQ
//   - its INSERT slots are admitted when a type is set (core/fx/insert.h set_cpu_check; decided 2026-11-22), each at its
//     type's bench row (Costs::type_bp) on its voice's core (voice_core), like AMP below: Load::ins, ins_fits. A type with
//     no room plays dry and shows NO CPU (insert.h refused); before, each voice reserved the worst TYPE pair
//   - each box held adds its own send FX, send TYPE inserts, bus and master chains (dry), measured together as one bench
//     row, on the render core that runs its send FX: slot 0's on core 2, slot 1's on core 1 (chain_core; decided
//     2026-11-22: LT1 found core 2 ~95 % with both boxes' chains on it and core 1 ~70 % idle). Two-slot builds run slot 0's
//     master chain on core 3 (master_core; decided 2026-11-22, LT3: the base box's master AMP pair took ~26 % of core 2);
//     the row is still counted whole on core 2 (its master part, dry, ~4 %: an upper bound)
//   - AMP (core/fx/amp.h) is admitted when a slot loads a capture, not reserved at attach (decided 2026-11-22: reserving
//     one for every base-box voice left a Rytm or Digitakt base box over on core 2 even alone): amp_fits() lets an AMP
//     slot play while its render core (amp_core) stays within kLimitBp with the AMPs already playing there (Load::amps);
//     else it plays dry and shows NO CPU (amp::kCpu). AMPs playing when a box attaches count against it: the first ask wins
//   - a box's voices are its voices 0..n-1: voices 0..s-1 on core 1, s..7 on core 2, s its split (split_for: the cmdline's
//     split=, or half the voices its box asks for if fewer: an A4's 4 as 2 + 2, so its voices and the chains share both cores)
// The first box keeps its voices (it was alone: nothing to take them from). The second gets the most that fit beside the
// first one's (never more than it asked for), is told (a popup on its screen), and /status shows the shortfall. A box whose
// partner leaves gets back what it asked for. Nothing here runs per sample; the render cores read the counts (device.h
// admitted) once per voice. Core 3 (two-slot builds: slot 0's master chain there, master_core) is counted for AMP only
// (decided 2026-11-23 after LT6): its control work and slot 0's master chain dry as fixed shares (kC3BaseBp, kC3MasterBp)
// and the AMPs playing there, within kC3LimitBp; core 3 also waits for cores 1 and 2 each block, so its limit is lower.
// No bench (bench=0, the host, the Teensy): nothing measured, so nothing is refused (every box its count; /status says so).
#pragma once
#include <stdint.h>

#include "machines.h"

namespace core {
namespace admit {

constexpr unsigned kLimitBp = 8000;   // 80 % of a render core, in 1/100 % (bp)
// Core 3 (decided 2026-11-23; LT6, project file gates/rebalance-core2-03f19b4/LT6-RESULTS.md, A4 base + Rytm, full load):
// at 1500 MHz with slot 0's master AMP pair core 3's own load was 53.8 % (master chain 40.4 %), no underruns; at 1200 MHz
// 54.7-61 % (master chain 44.7-49.7 %) and 27-82 underruns a minute, core 3's passes 10-19 ms; the pair off at 1200: 28.6 %
// (master chain 10.0 %), no underruns. Its control work (own minus the master chain at 1500: 13.4 %) and slot 0's master
// chain dry (10.0 % at 1200: 8.0 % at 1500) are fixed shares at 1500 MHz, scaled by the clock like the rest; the limit
// keeps the pair at 1500 (21.4 + 2 x 20.1 = 61.6 %) and one AMP of it at 1200 (26.8 + 25.1 = 51.9 %; both: 77 %).
constexpr unsigned kC3BaseBp = 1340, kC3MasterBp = 800, kC3LimitBp = 7000;

// What the bench's rows cost under the two-box load, in % of the row (LT3, 2026-11-22, chains-core1 64fdd7b, 1500 MHz;
// project file gates/chains-core1-64fdd7b/RESULTS-LT3.md). The bench runs each row alone on core 0; under load all four
// cores share the L2, and AMP (NAM's weights) loses most:
//   - AMP: the base box's master pair took 26.3 % of a core against the bench's 2 x 4.02 (slot 0's master chain 30.6 %,
//     slot 1's dry 4.3 %), and 2 voice AMPs a core added 19.5-21.9 points (config B), so one AMP ~10-13 %: 300 %. LT4
//     (2026-11-22, rebalance-core2 81b96b4; project file gates/rebalance-core2-81b96b4/LT4-RESULTS.md): one voice AMP on
//     core 1 beside a Rytm base box measured ~19-20 points (c1 with chains 78.8 % against core1Bp 70.07; beside an A4 base
//     box ~13, as LT3): 500 %, the worst seen, so core 1 is not left at 79 % against the 80 % limit
//   - a voice (machine + INSERT): 6 voices a core measured 26.1-32.8 points against 6 x 3.62: 135 %
//   - a box's chains (the box row, dry): slot 1's Rytm 33.1 % (send FX 28.8 + master 4.3) against the row's 30.28: 110 %
// With them, reserved at attach, the worst case of a Rytm or Digitakt base box was over on core 2 even alone (~125 %: its
// chains, master pair and 4 voice AMPs). Since the rebalance (decided 2026-11-22) AMPs count when they play (amp_fits) and
// slot 0's master chain with its pair runs on core 3.
constexpr unsigned kAmpLoadPct = 500;
constexpr unsigned kVoiceLoadPct = 135;
constexpr unsigned kChainLoadPct = 110;
// The factors in use (reset: the k*LoadPct above; the host tests' synthetic costs set 100 each)
struct LoadPct { unsigned voice, amp, chain; };
void set_load_pct(const LoadPct& p);
const LoadPct& load_pct();

// The boot bench's figures, each in 1/100 % of a core at `mhz` (kernel.cpp LogBench).
struct Costs {
  unsigned mhz;        // the clock the bench ran at; 0: no bench (admission off)
  unsigned voice_bp;   // one voice at the worst machine of the bench (its mean over 1 s)
  unsigned ins_bp;     // one voice's POST EQ (bench ins OFF+OFF / 8; its INSERT types count when set: type_bp)
  unsigned amp_bp;     // one AMP instance (bench amp)
  unsigned fx_bp;      // one box's chains on its render core (chain_core), dry (no AMP): the bench's box chains row (its send FX, bus and
                       // master chains at the worst set the rooms allow, as one run: master.h box_bench). Before LT1
                       // (2026-11-22) the sum of overlapping rows; still that when the boot has no box row. The base box's
                       // master AMP pair is added by core_bp (2 AMPs at kAmpLoadPct), not here (LT3)
  unsigned slot_bp;    // one INSERT slot at the worst TYPE pair (bench ins rows / 16): a type with no row of its own
  uint16_t type_bp[16];   // one voice INSERT slot of type t (bench ins T+T / 16), 0: not measured (slot_bp). AMP: amp_bp
  uint16_t send_bp[6];    // one master send TYPE slot (fx/insert.h kMChorus..kMHalo: stereo already), a quarter of its
                          // bench sfxins "T x4" row; 0: not measured (twice slot_bp)
};
constexpr int kSendType0 = 11, kSendTypes = 6;   // fx/insert.h kMChorus .. kMHalo (device.cpp checks)
unsigned type_bp(int t);   // a voice INSERT slot of type t at the bench (OFF, AMP, a send TYPE: 0)
// One master INSERT slot of type t at the bench: a voice type twice its row (the master chain is stereo), a send TYPE its
// own row (send_bp), OFF / AMP 0 (AMP: amp_fits)
unsigned master_slot_bp(int t);

void set_costs(const Costs& c);
const Costs& costs();
void set_clock(unsigned mhz);   // the CPU clock setting now (cpumhz=); the costs scale by costs().mhz / mhz
unsigned clock();
void set_split(unsigned n);     // split=: voices 0..n-1 of each box on render core 1 (1..7)
unsigned split();
// the split of a box that asks for `asked` voices: split(), or half of asked (rounded up) if that is fewer; 0 asked
// (a free slot): split(). The Pi's cores.cpp renders by it (core 3 puts it in each pass), core_bp counts by it
unsigned split_for(unsigned asked);
bool on() ;                     // a bench to admit against
// admit=report (console, the two-box load test): the costs, core_bp and fit stay, but no box is refused (every box its
// count) so the worst case can be measured against core_bp; admit=enforce (the default) refuses again
void set_enforce(bool on);
bool enforce();

// The render core that runs slot s's send FX and bus chains (the Pi's cores.cpp ChainCore): slot 1's core 1, slot 0's core 2
constexpr int chain_core(int s) { return s == 1 ? 1 : 2; }
// ... and its master chain (cores.cpp MasterCore): slot 1's core 1; slot 0's core 3 on two-slot builds (the control core,
// during its render wait), core 2 on an 8-voice engine
constexpr int master_core(int s) { return s == 1 ? 1 : kVoiceSlots > 1 ? 3 : 2; }

// A load: voices[s] of slot s's box render (its 0..voices-1), held[s] its chains run (on chain_core(s)), asked[s] what it
// asked for (its split: split_for; 0: split()), amps[c] the AMP slots playing on render core c (1, 2; [0] unused: amp_core).
// The base box (AMP's): slot 0 while held, else slot 1 (device.h g_base).
struct Load {
  uint8_t voices[2];
  bool held[2];
  uint8_t asked[2];
  uint8_t amps[4];   // [3]: core 3's (slot 0's master AMPs, master_core)
  uint32_t ins[4];   // the INSERT slots admitted on core c, at the bench (type_bp; [0] unused): 1, 2 the voices' (a voice
                     // on OUT PRE / POST in stereo: twice); 3 slot 0's master chain's (stereo: each slot twice its mono
                     // row; decided 2026-11-23)
};
// The render core of slot b's box's voice chain c (0..7): by its box's split
int voice_core(int b, int c, const Load& l);
// INSERT admission: a voice slot of type t may play on core c beside l (its ins[] those playing): core_bp + the type's cost
// at the voice load factor within kLimitBp. Always: no bench, admit=report, another core.
// mul: 2 for a voice chain in stereo (OUT PRE / POST: ProcessVoiceStereo runs it on L and R, about twice its mono row)
bool ins_fits(int c, const Load& l, int t, int mul = 1);
int base_of(const Load& l);   // the base box's slot in l: 0 while held, else 1
// The core that runs AMP row r (fx/amp.cpp's rows: the base box's voices 0..7, its master chains from master_row on): a voice
// row its voice's (by the base box's split), a master row master_core(the base box)
int amp_core(int row, int master_row, const Load& l);
// AMP on every box's chains (fx/amp.h: each box its own rows): the core that runs box b's chain c, a voice chain by b's own
// split (voice_core), a master chain master_core(b), a bus 0 (AMP plays dry there)
int amp_core_of(int b, int c, int master_row, const Load& l);
// AMP admission: one more AMP slot may play on core c (1, 2 or 3) beside l (its amps[] those playing): core_bp + one AMP at
// its load factor within kLimitBp (core 3: kC3LimitBp). Always: no bench, admit=report or another core.
bool amp_fits(int c, const Load& l);
// Render core 1 or 2's worst load for l, in bp at the clock now, each row at its load factor (kAmpLoadPct and the rest):
// its voices, the chains it runs, the INSERT slots (l.ins[c]) and AMPs (l.amps[c]) admitted on it. Another core: 0.
unsigned core_bp(int core, const Load& l);
// Core 3's load for l on a two-slot build, in bp at the clock now: kC3BaseBp, slot 0's master chain while it is held
// (kC3MasterBp: its POST EQ, measured whole), its INSERT and send TYPEs admitted (l.ins[3], master_slot_bp) at the chain load factor
// and the AMPs playing there (l.amps[3]) at the AMP load factor. An 8-voice build (no core 3 chain): 0.
unsigned core3_bp(const Load& l);
// One more master INSERT slot of type t (stereo) on core 3 beside l within kC3LimitBp. Always: no bench, admit=report,
// an 8-voice build.
bool ins3_fits(const Load& l, int t);
unsigned limit_bp(int core);   // kLimitBp, core 3's kC3LimitBp
// The most voices (<= want, which is also what it asked for: its split) slot s may render beside the other slot's in l, so that every render core this box's voices land
// on stays within kLimitBp. A core already over without this box's voices (by the chains alone) takes none of them.
int fit(int s, int want, const Load& l);
// What slot s gets at attach: fit() while enforcing, else want (0..8)
int admit(int s, int want, const Load& l);

// cmdline voices=: per box type, "rytm:8,a4:2,dt:6" (dt or digitakt). Each count 0..8, at most its profile's (it lowers a
// count: device.cpp voices_asked); false: not understood (nothing changed).
bool parse_override(const char* text);
int override_for(const char* type_short);   // -1: none ("Rytm", "A4", "Digitakt": device::short_name, case ignored)
void reset();                                // the defaults (tests)

}  // namespace admit
}  // namespace core
