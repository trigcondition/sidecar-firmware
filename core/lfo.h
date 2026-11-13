// core/lfo.h - the Sidecar's own LFOs (docs/lfo.md): kLfos of them (26 on the Pi and the Mac - two a Rytm track, A and
// B - and 4 on the Sidecar Mini), each on one target:
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
#include "mod/target.h"

// 0.5.0 (spec-050 1.1): the LFOs are a source of the modulation layer (core/mod): core/mod owns their targets, their
// bases and the writes. CORE_MOD_LFO 0 keeps 0.4.0's own write path compiled (the rollback; removed in Phase 2).
#ifndef CORE_MOD_LFO
#define CORE_MOD_LFO 1
#endif

namespace core {
namespace lfo {

// 0.5.0 (the owner, LFO FINAL 2026-11-12): 26 on the Pi (and the Mac), 2 a track - T1..T12 and the FX track - LFO i
// being track i / 2's A (even i) or B (odd i). 0.5.0 owner test (2026-11-13): A and B alike target the Sidecar's own
// only (the voices, inserts, the master, the send FX, the sends, the OUT routes): never RT1..RT12 / RFX, so no LFO uses
// the Rytm link (set, the console, the page, the web refuse it; a file's line on one loads OFF): an LFO on a Rytm param
// made the whole Rytm slow. The Mini keeps its 4 (no tracks, no A / B).
#if RYTM_SENDFX_LARGE
constexpr int kLfos = 26;
constexpr bool kPerTrack = true;
#else
constexpr int kLfos = 4;   // the Sidecar Mini
constexpr bool kPerTrack = false;
#endif
constexpr int kTracks = 13;   // T1..T12, FX (kPerTrack)
// LFO i's track (0..11 T1..T12, 12 FX; -1 on the Mini) and slot (0 A, 1 B); the LFO of a track's slot
constexpr int track_of(int i) { return kPerTrack ? i / 2 : -1; }
constexpr int slot_of(int i) { return kPerTrack ? i % 2 : 0; }
constexpr int lfo_of(int track, int slot) { return kPerTrack ? 2 * track + (slot & 1) : track; }
constexpr int kSteps = 16;
constexpr uint32_t kUserMs = 5;  // a user machine's parameter call at most this often
enum Wave : uint8_t { kSine, kTri, kSawUp, kSawDown, kSquare, kSampleHold, kRandom, kDesign, kWaves };
enum Retrig : uint8_t { kFree, kOnTrig, kOnPlay, kRetrigs };
enum StepShape : uint8_t { kHold, kRamp, kSmooth, kShapes };
// Targets: 0 OFF, 1..8 V1..V8, 9 / 10 FX1 / FX2; fx-ins (CORE_POLY builds): 11..18 I1..I8 the voices' INSERT chains,
// 19..21 B1..B3 the buses' (core/fx/insert.h; params 0..7: slot A P1..P4, slot B P1..P4; ins1112: 8..11 the chain's
// POST EQ HPF / LPF / LOW / HIGH; an offset on the knob / lock); 22..29 O1..O8 the voices' OUT route (core/fx/out.h;
// params 0..2: LEV PAN WID, an offset on the knob). 0.5.0 (spec-050 1.6): 30 31 M1 M2 the master's two chains (params as
// an INSERT chain's), 32 MFX the master's LEV and MIX1..4 (0..4), the bus slots' send TYPE MIX B1A..B3B (5..10), 33..44 S1..S12 a Rytm track's sends to FX1 / FX2
// (0..1: F1 F2), 45..52 X1..X8 a voice's sends to the buses (0..2: B1..B3); all through core/mod (an LFO file naming
// one is locked on 0.4.0, as any line it does not read). 0.5.0 (rytm-param-mod): 53..64 RT1..RT12 the Rytm tracks' own
// sound parameters and 65 RFX its FX track's (core/mod/rytm_param.h): params the words an offset may move (kRtWords /
// kRfxWords in lfo.cpp; the file names the word); since the owner's test no LFO may take them (target_ok): the ids,
// names and words stay for the files and the other layer users.
#if CORE_POLY
constexpr int kTVoice = 1, kTFx = 9, kTIns = 11, kTOut = 22, kTMaster = 30, kTMfx = 32, kTSnd = 33, kTIsnd = 45,
              kTRt = 53, kTRfx = 65, kTargets = 66;
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
// The control side's edits (the page, the console): each takes effect at the next tick. False: out of range, or a
// target LFO i may not have (target_ok).
bool set(int i, const Settings& s);
// LFO i may have target t: any but a Rytm target (RT1..RT12, RFX; 0.5.0 owner test)
bool target_ok(int i, int t);
// the highest target LFO i may have (the page's TGT list ends there: before RT1)
int target_max(int i);
// LFO i's name: "T3A", "FXB" (kPerTrack; the console, the file), "3" on the Mini; -1 from lfo_index: not a name
const char* lfo_name(int i, char* b, int cap);
int lfo_index(const char* s, const char** end = nullptr);   // "T3A" / "fxb" / "5" (N 1..kLfos) -> 0..kLfos-1, -1 none
// 0.5.0 (LFO FINAL): the Rytm link is saturated (core/mod/rytm_param.h rp_saturated) while an LFO targets the Rytm: the
// page, the touchscreen and the web say "RYTM LINK FULL: LFOs update slower" (releases and p-locks keep priority)
bool link_full();
void defaults(Settings* s);
void preset(Settings* s, int which);   // DESIGN presets: 0 RAMP, 1 TRI, 2 STEPS, 3 RANDOM
constexpr int kPresets = 4;
const char* preset_name(int which);

// ---- the engine's hooks (control side)
// every pass of the control loop. 0.5.0: a forwarder to mod::tick (core/mod/mod.h) for one release (removed in 0.6.0):
// the platforms call mod::tick; tests, the emulator lib (core_c_lfo_tick) and older callers keep meaning the same
void tick(uint32_t us);
void waves(uint32_t us, uint32_t dus);                  // mod::tick's source: the clock position, each LFO's offset
mod::Tid tid_of(int target, int param);                 // an LFO target's id on the layer (0: OFF)
void clock(uint8_t status, uint16_t spp);               // fx::clock_in's MIDI realtime / SPP (F8 FA FB FC F2)
void midi_in(uint8_t status, uint8_t d1, uint8_t d2);   // the Rytm's channel messages: its notes (an FX target's TRIG)
void voice_params(int vi, float* p, int np, bool trig); // voice_report: the bases (mod::voice_params), a trig's retrig
uint8_t fx_knob(int slot, int knob, uint8_t v);         // take_sendfx: knob 0..5 of FX slot reported v (mod::fx_knob)
float base_of(int vi, int k, float cur);                // mod::base_of (the callers use it directly)
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
const char* target_label(int t);     // the same for the screens: a voice by its track ("BD", "CH"), not "V1"; RT1..RT12 the same
// DISPLAY ORDER of the TGT list (the page's knob; 0.5.0), a mapping only: the stored target id + param (Settings, files,
// the wire) keep their meaning. On the Rytm the list is OFF, the 8 Sidecar voices (each ONCE, labelled by the track it
// is shown under - the first track of its voice that plays a Sidecar machine, else the first - and in that track's
// order), then ids 9..52 (FX1 .. X8); no RT1..RT12, no RFX (0.5.0 owner test: A's list is B's, one list). Behind a foreign box (its own voice names) the list is the id order. A target's PAR list is its own
// params; a voice's only those its machine names (ui::voice_label; the stored one kept while stored; a machine naming
// none: all 16), and TGT lands on its P2, else its first named. Every step moves, no entry repeats, every entry is reachable.
int target_count();                     // list positions
int target_pos(int target);             // the list position of a stored target (-1: out of range)
bool target_step(Settings* s, int d);   // TGT turned d positions: s.target and s.param of the entry landed on (false: at an end)
int param_count(const Settings& s);     // the PAR list's length of s's target entry
int param_index(const Settings& s);     // s's place in it
void param_step(Settings* s, int d);    // PAR turned d: s.target / s.param of the entry landed on (clamped)
int target_from_name(const char* s); // -1 none
int params_of(int target);           // how many parameters it has
// The parameter's name (<= 5 chars) into b: the voice machine's label (ui::voice_label), the FX type's P2..P5 label;
// "-" (an INSERT slot's "A-") for one its machine / TYPE leaves unused.
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
// Plugin API 3's transport (core/user/um.cpp pre_block): the clock as clock() keeps it - running (FA / FB .. FC), ticking
// (an F8 seen), the latest tick's song position (24 ppqn since Start or an F2, -1 none) and when it came, the tempo, the
// position tick() keeps (clock_now's), and counts of Start / Continue, Stop and song position messages since boot.
struct ClockState { bool run, ticking; int32_t pos; uint32_t last_us; float tick_us, tp; uint32_t starts, stops, jumps; };
void clock_state(ClockState* out);
float out(int i);                    // its wave now, -1..1
bool active(int i);                  // on a target with a nonzero depth

// ---- the console / web: "lfo" (the state), "lfo=N:KEY:VALUE" (N 1..kLfos, or the LFO's name: T3A, FXB; KEY tgt par wav
// rate sync trig dep base,
// st:I:V:S one DESIGN step, des:<48 hex: per step value + 64 (2) and shape (1)>, pre:ramp|tri|steps|rnd, off).
bool command(const char* p, char* out, int cap);
// "\"lfo\":{\"n\":26,\"pt\":1,\"kit\":K,\"full\":0,\"trk\":T,\"l\":[...]}" (K 1..128, 0 unknown; pt 1: two a track, l[i] track
// i / 2's A or B; full: link_full; trk: the Rytm's selected track 0..12 (12 FX), -1 none named - the web follows it)
int json(char* out, int cap);
// The /status buffer json() writes into on the Pi (pi/engine/kernel.cpp): every LFO with its params' 16 names ("pns"),
// sized for 26 LFOs (the owner, 2026-11: 2 per Rytm track T1..T12 and 2 for the FX track; test_param_names checks it)
constexpr int kJsonCap = 12288;

struct Stats { uint32_t ticks, voice_sets, user_calls, fx_sets, restores; };
const Stats& stats();
void reset();                        // tests: defaults, no targets, the clock forgotten

// core/bundle/rename (UX review B1): the unsaved edits due now (the next save() writes them), and whether any wait
void save_due();
bool dirty();
}  // namespace lfo
}  // namespace core
