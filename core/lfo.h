// core/lfo.h - the Sidecar's own LFOs (docs/lfo.md): kLfos of them (8 on the Pi and the Mac, 4 on the Sidecar Mini),
// each on one target:
//   V1..V8    a Sidecar voice's P1..P16 (whatever machine plays it, user machines too): modulated in the engine, on
//             top of the Rytm's value (its knob, a p-lock): x.p[k] = base + depth x wave, clamped to 0..127.99, the
//             base the Rytm's last report. A built-in machine reads it at its next block (Plaits interpolates within
//             the block); a user machine gets it as a parameter call, at most every kUserMs.
//   FX1, FX2  the send FX's P2..P5 (core/fx/send_fx.h): fx::control.knob, the base the Rytm's last 0x20.
//   I1..I8, B1..B3  (fx-ins) a voice's / a bus's INSERT slot params (core/fx/insert.h): an offset on top of the knob.
// (1026: the Rytm's own sound parameters as MIDI CC targets are gone: a CC sets the Rytm's stored value, so an LFO on
// one changed the instrument's state. A Rytm-side SysEx modulation path may come later.)
// Waves SINE TRI SAW+ SAW- SQR S&H RND (random, smooth) and DESIGN (16 steps drawn by the user: a value and a shape
// each, HOLD / RAMP to the next / SMOOTH). Rate: free (0.02..20 Hz) or synced to the Rytm's MIDI clock (1/32 .. 8 bars,
// triplets and dotted; with no clock or stopped it runs on at the last tempo). Retrig: FREE, TRIG (the target's trig:
// the voice's 0x04; FX: any trig, a voice's or a Rytm track's note), PLAY (the Rytm's Start).
// Settings per kit (1026; the Rytm's own LFOs are per kit): /.rytm/lfo/<project>/kNNN.txt (kit 1..128, a line per
// LFO), switched when the Rytm names another active kit (kit_changed) or project, written by idle_save(); a kit
// without its own file starts from the project's /.rytm/lfo/<project>.txt (the pre-1026 per-project file), which is
// also what is used while the Rytm has not named a kit (a build without the kit report).
// Everything runs on the control side (where engine_sysex runs), between blocks: tick() from the platform's loop.
#pragma once
#include <stdint.h>

#include "fx/send_fx.h"   // RYTM_SENDFX_LARGE

namespace core {
namespace lfo {

#if RYTM_SENDFX_LARGE
constexpr int kLfos = 8;
#else
constexpr int kLfos = 4;   // the Sidecar Mini
#endif
constexpr int kSteps = 16;
constexpr uint32_t kUserMs = 5;  // a user machine's parameter call at most this often
enum Wave : uint8_t { kSine, kTri, kSawUp, kSawDown, kSquare, kSampleHold, kRandom, kDesign, kWaves };
enum Retrig : uint8_t { kFree, kOnTrig, kOnPlay, kRetrigs };
enum StepShape : uint8_t { kHold, kRamp, kSmooth, kShapes };
// Targets: 0 OFF, 1..8 V1..V8, 9 / 10 FX1 / FX2; fx-ins (CORE_POLY builds): 11..18 I1..I8 the voices' INSERT chains,
// 19..21 B1..B3 the buses' (core/fx/insert.h; params 0..7: slot A P1..P4, slot B P1..P4; ins1112: 8..11 the chain's
// POST EQ HPF / LPF / LOW / HIGH; an offset on the knob / lock); 22..29 O1..O8 the voices' OUT route (core/fx/out.h;
// params 0..2: LEV PAN WID, an offset on the knob).
#if CORE_POLY
constexpr int kTVoice = 1, kTFx = 9, kTIns = 11, kTOut = 22, kTargets = 30;
#else
constexpr int kTVoice = 1, kTFx = 9, kTIns = 11, kTargets = 11;
#endif
constexpr int kDivs = 19;        // synced rates: 1/32 .. 8 bars
constexpr int kFreeRates = 128;  // free rates: 0..127

struct Settings {
  uint8_t target;          // 0..kTargets-1 (above)
  uint8_t param;           // the target's parameter: P1..P16 (0..15), FX P2..P5 (0..3)
  uint8_t wave;            // Wave
  uint8_t rate;            // free: 0..127 (0.02 Hz x 1000^(rate / 127)); synced: 0..kDivs-1
  uint8_t sync;            // 0 free, 1 the Rytm's clock
  uint8_t retrig;          // Retrig
  int8_t depth;            // -64..63: the swing around the base, in parameter steps
  uint8_t base;            // unused since 1026 (the Rytm's CC targets' base; kept in the file's lines)
  int8_t step[kSteps];     // DESIGN: each step's value -64..63
  uint8_t shape[kSteps];   // DESIGN: each step's StepShape
};

const Settings& get(int i);
// The control side's edits (the page, the console): each takes effect at the next tick. False: out of range.
bool set(int i, const Settings& s);
void defaults(Settings* s);
void preset(Settings* s, int which);   // DESIGN presets: 0 RAMP, 1 TRI, 2 STEPS, 3 RANDOM
constexpr int kPresets = 4;
const char* preset_name(int which);

// ---- the engine's hooks (control side)
void tick(uint32_t us);                                 // every pass of the control loop
void clock(uint8_t status, uint16_t spp);               // fx::clock_in's MIDI realtime / SPP (F8 FA FB FC F2)
void midi_in(uint8_t status, uint8_t d1, uint8_t d2);   // the Rytm's channel messages: its notes (an FX target's TRIG)
void voice_params(int vi, float* p, int np, bool trig); // voice_report: base = the report's P1..Pnp, p modulated
uint8_t fx_knob(int slot, int knob, uint8_t v);         // take_sendfx: knob 0..5 of FX slot reported v; returns the value to play
float base_of(int vi, int k, float cur);                // a voice parameter's base (the remote page's edits), cur when unmodulated
void project_changed();                                 // the Rytm's project changed: the LFOs of the new one (card)
void kit_changed(int kit);                              // the Rytm's active kit (0..127): its LFOs (card), when another
int kit();                                              // the kit the settings are of (0..127), -1 unknown
void load();                                            // boot / project or kit change: the kit's file (above)
#if CORE_MSAMP
// 2026-10-26, the RAM store (core/store/store.h): the paths a project's fill reads for the LFOs - the project's file and
// every kit's (128) - of the project now named (core::project). A project load fills the store before load() runs
// (core/pool/pool.h project_fill_step), and load() reads it only, never the card (a kit change cost core 3 40 ms of
// FatFs path operations); a kit change while it fills is only noted. save() puts its text there too while the store is
// its project's.
void want_files();
// the settings in memory are now name's: its files are where save() writes (and, while the store is its, puts) - a
// rename moved the old name's files to name before its switch, the boot names its project before the first fill ends
void rekey(const char* name);
#endif
bool save();                                            // idle_save: the file, a second after the last change; true if it wrote

// ---- names and views (the page, the web)
const char* wave_name(int w);        // "SINE" .. "DESIGN"
const char* retrig_name(int r);      // "FREE" "TRIG" "PLAY"
const char* shape_name(int s);       // "HOLD" "RAMP" "SMTH"
const char* target_name(int t);      // "OFF" "V1".."V8" "FX1" "FX2" (the console, the wire)
const char* target_label(int t);     // the same for the screens: a voice by its tracks ("BD", "CH/OH"), not "V1"
int target_from_name(const char* s); // -1 none
int params_of(int target);           // how many parameters it has
// The parameter's name (<= 5 chars) into b: the voice machine's label (its page's), the FX type's P2..P5 label.
const char* param_name(int target, int param, char* b, int cap);
void rate_name(const Settings& s, char* b, int cap);    // "1/16T", "2BAR", "0.35HZ"
float rate_hz(int rate);
int div_ticks(int div);              // a synced rate's cycle in 24 ppqn ticks
float wave_at(const Settings& s, float ph, uint32_t seed);   // -1..1 at phase ph (S&H / RND: a fixed pattern by seed)
float design_at(const Settings& s, float ph);
float phase(int i);                  // the LFO's phase now, 0..1
// The Sidecar's clock as the LFOs run on it (any core may read it: GRANULAR's ANIM): the position in 24 ppqn ticks
// (mod 2304, locked to the Rytm's song position while it plays, else on at the last tempo) and a tick's length in us.
// False until tick() first ran (tests without the control loop).
bool clock_now(float* ticks, float* tick_us);
float out(int i);                    // its wave now, -1..1
bool active(int i);                  // on a target with a nonzero depth

// ---- the console / web: "lfo" (the state), "lfo=N:KEY:VALUE" (N 1..kLfos; KEY tgt par wav rate sync trig dep base,
// st:I:V:S one DESIGN step, des:<48 hex: per step value + 64 (2) and shape (1)>, pre:ramp|tri|steps|rnd, off).
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);        // "\"lfo\":{\"n\":8,\"kit\":K,\"l\":[...]}" (K 1..128, 0 unknown)

struct Stats { uint32_t ticks, voice_sets, user_calls, fx_sets, restores; };
const Stats& stats();
void reset();                        // tests: defaults, no targets, the clock forgotten

// core/bundle/rename (UX review B1): the unsaved edits due now (the next save() writes them), and whether any wait
void save_due();
bool dirty();
}  // namespace lfo
}  // namespace core
