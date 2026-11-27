// core/wavetable/wavetable.h - the WAVETABLE machine (docs/WAVETABLE.md): a band-limited wavetable oscillator (mip
// levels an octave apart, tables.h), frame position with a smooth morph between neighbouring frames, a second table B
// played with it in every unison lane, a warp (nine), unison up to 7 oscillators summed to the voice's mono return, a sub
// oscillator, a filter (the DJ-style SVF, LP below CUT 64 and HP above, or one of seven others) with drive, an amp ADSR
// and a mod envelope on POS; polyphonic: a trig is one note, chords come from a Sidecar poly track (core/poly.h: the
// sequencer's notes, the live pads, the keyboards), all of them playable. Pi and Mac only (CORE_MSAMP).
//
//   tables   TBL (the 14-bit word bank * 128 + row) is a row of the project's WAVETABLES pool (core/lib/pools.h,
//            docs/POOLS.md): a built-in row (tables.h: BASIC PWM VOCAL ORGAN FM SYNC, 64 frames each, generated at
//            begin; the template puts them in A001..A006) or a sample row (any WAV / AIFF, resolved by its hash). A file: kMaxFrames frames at most (more thinned evenly) of 2048 samples (the open format: Serum, Vital,
//            ...), or the size a 'clm ' chunk says, or 1024 / 512 / 256, or one single cycle of any length up to
//            8192 (tables.h detect_frame_size). Loaded between blocks (step(): one read of kChunk, or one frame's mip
//            levels, a call) into one of kSlots slots; least recently used is replaced. A trig while its table loads
//            plays when it is ready, within kLateMs (else at once on the table the voice had).
//   cores    report / note_off / silence / step / command: the control core; render_add: the voice's render core
//            (a note's start / release / kill through a sequence-locked mailbox, as msamp's; the voice's table
//            through a hazard slot, so a table in use is never reloaded under it). No allocation after begin().
//
// The params (P1..P16 = sound words 9..16, 17..24):
//   P1 TBL   the WAVETABLES pool row (OFF, empty and missing rows are silent)
//   P2 POS   the frame position, 0 = the first frame, 127 = the last; morphs between neighbours
//   P3 WARP  zones: OFF SYNC PWM FM FORMANT QUANT BEND SQUEEZE RM (9 zones)   P4 AMT   the warp's amount
//            (SYNC 1..8x, PWM 50 % .. 4 %, FM index 0..5: the engine's own laws; FORMANT 1/16..16x sync with the half-sine
//            window, QUANT steps, BEND, SQUEEZE, RM depth: Vital's; FM and RM take B as the modulator when B is on, a
//            sine at the note's rate otherwise)
//   P5 UNI   unison: 0 one oscillator; up: pairs fade in (3, 5, 7 oscillators) and the detune opens (up to 60 cents)
//   P6 SUB   a sine an octave down, its level            P7 CUT   0..63 low-pass (20 Hz .. 20 kHz), 64 open, 65..127
//            high-pass (20 Hz .. 8 kHz), 50 % key tracking   P8 RES  resonance, and the drive into the filter with it
//   P9 ATK  P10 DEC  P11 SUS  the amp envelope (1 ms .. 10 s; DEC is also the release, as a Minimoog's)
//   P12 ENV  the mod envelope (attack ATK, decay DEC, to 0) onto POS (and POS B): 64 none, 127 +the whole table, 0 -the
//            whole
//   P13 TBL B  table B (as TBL; row 000 OFF: no B, the default)     P14 POS B  its frame position
//   P15 MIX    0 only A .. 127 only B, equal power                  P16 FTYPE  the filter: DJ LP12 LP24 BP12 BP24 HP12
//            HP24 LADDER (8 zones; DJ the default). The others: CUT 0..127 is 8 + CUT x 128/127 semitones above C-1 (Vital's
//            law) + 50 % key tracking + the filter envelope; RES as Vital's; the drive comes with RES
//   The filter envelope (the mod envelope onto CUT of the new types, -64 .. +64 semitones) has no knob: set_fenv / wt=fenv:
// The Rytm side (the machine, its labels) is a rytm-mods 0021 change; until it exists `wt=voice:V` makes voice V
// play WAVETABLE whatever built-in machine the Rytm reports for it (as msamp=voice:V).
#pragma once
#include <stdint.h>

namespace core {
namespace wt {

constexpr int kSlots = 4;            // card tables resident at once
constexpr int kPerVoice = 8, kTotal = 32, kUni = 7;
constexpr uint32_t kChunk = 32768;   // a card read while loading
constexpr uint32_t kFileMax = 8u << 20;   // a table file at most
constexpr uint32_t kLateMs = 250;

#if CORE_MSAMP
void begin(bool card);               // boot (engine_begin): the built-ins generated, the card's list queued
bool step();                         // one bounded card job; true when it did one
void rescan();                       // the card changed: failed tables are tried again
bool forced(int vi);                 // wt=voice:V
// The filter envelope's depth (-1..1): the mod envelope onto CUT of the new filter types (0 = none; 64 semitones at 1).
// A touchscreen-only setting in the brief, but the project has no per-track store for one: it is the engine's, set from
// the console (wt=fenv:V:PERCENT), not saved, 0 at boot and when the voice leaves WAVETABLE.
void set_fenv(int vi, float a);
float fenv_get(int vi);
// voices.cpp's hooks for a WAVETABLE voice vi (note -1: a 0x03 PARAMS)
void report(int vi, int note, int vel, const float* p);
void note_off(int vi);
void silence(int vi);
void leave(int vi);                  // the voice left WAVETABLE: silenced, its table no longer selected (a card slot freed)
void render_add(int vi, float* out, int n);   // the render core's: the voice's notes added into out (-1..1)
void live_on(int vi, int key, int vel, int ch, int mnote);
void live_off(int vi, int ch, int mnote);
int voice_notes(int vi);
float headroom(int vi);              // tests: the voice's headroom gain now (as msamp's)
const char* entry_name(int e);       // "" none
int entries();
bool command(const char* p, char* out, int cap);   // "wt", "wt=voice:V[:off]", "wt=list", "wt=rescan", "wt=pick:V:ROW", "wt=pickb:V:ROW"
// The TBL list as the displays show it (as gr's): entry e = code e of the WAVETABLES pool. entry_state: 0 none (OFF,
// empty, missing) 1 on the card 2 loading 3 in RAM 5 failed; entry_note: "LOADING", "TOO BIG: 8 MB MAX", "NOT A WAV IT
// READS", "MISSING", "<n> FRAMES". list_gen changes with every new list.
const char* entry_group(int e);
const char* entry_path(int e);
int entry_lib(int e);
int entry_state(int e);
const char* entry_note(int e, char* b, int cap);
uint32_t list_gen();
int src_entry(int vi, int v);        // the entry TBL v plays (the code itself)
// A choice (the touchscreen): the Rytm's TBL of the voice's track written (48: the kit keeps it)
bool pick(int vi, int e);
bool voice_picked(int vi);           // (always false)
// The kit's TBL known while the voice is idle (the SRC view's 2A: a pick in the Rytm's list, a turn; its 0x03 reports
// come only while it plays): the voice selects entry e as at its next report, so its table loads and the pages show it
// with no trig (0.5.0, owner 2026-11-15). The Rytm's own value: nothing is written back.
void kit_src(int vi, int e);
// 2026-11-24 TBLB: the same for table B (P13). pick_b writes only P13 (48 k = 12) and selects table B (A untouched; row 000 is B off);
// kit_src_b is the Rytm's own P13 on an idle voice (the SRC view's 2A). B is silent at MIX 0 unless the warp is FM or RM: b_open
// says whether MIX / the warp let it be heard, b_silent whether a table is set for B and they do not (the touchscreen's note);
// table_b_code the code P13 holds.
bool pick_b(int vi, int e);
void kit_src_b(int vi, int e);
bool b_open(int vi);
bool b_silent(int vi);
int table_b_code(int vi);
void preview(int vi, int e);         // a ~0.7 s note at C4 once the table is in (e < 0: the voice's own)

struct Stats {
  int listed, resident, active, peak;
  uint32_t trigs, notes, steals, waits, late, loads, load_fails;
  uint32_t last_frames, last_frame_size;   // the last table loaded: its frames, the file's frame size
};
const Stats& stats();
// tests
int voice_entry(int vi);             // the list entry voice vi plays now (-1 none)
int voice_entry_b(int vi);           // the list entry its table B plays now (-1 none: B off)
bool denormal_state(int vi);         // a subnormal float in a note's filter memories (tests)
int table_frames(int e);             // a resident entry's frames (0: not resident)
uint64_t ram_bytes(uint64_t* capacity);   // the tables' frames in RAM now (and the memory set aside for them)
const float* table_frame(int e, int f);   // its frame f (kFrameFloats floats), nullptr not resident
void reset();                        // every note free, forced voices off, the list kept
// the displays (any core, racy: drawing only): POS as voice vi plays it (0..1) and its table's frames (0: none); the
// table at position pos (0..1, morphed) as cols samples of its frame (-1..1 about)
float viz_pos(int vi, int* frames);
int viz_frame(int vi, float pos, int cols, float* y);
#endif

}  // namespace wt
}  // namespace core
