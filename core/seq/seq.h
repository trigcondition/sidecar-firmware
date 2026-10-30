// core/seq/seq.h - the Sidecar sequencer (docs/SEQUENCER.md, MVP): the host's own poly note patterns on the Rytm
// tracks it marks, played in time with the Rytm's step clock, edited on the Rytm's trig keys / encoders / LEDs and a
// remote SEQ page. Pi and Mac only (CORE_POLY builds; the Teensy, frozen at 0.1.0, has none).
//
// Wire (sim/protocol.py "The Sidecar sequencer", the reference; rytm-mods 0026 "seq" the Rytm's half):
//   Rytm -> host  2C 01 <blk4> <n> {<track> <step> <len> <speed>} x n   step ticks of one audio block
//                 2D 01 <kind> ...   01 TRANSPORT, 02 PATTERN, 03 SWITCH, 05 MUTES, 06 REC, 07 TEMPO, 08 SEQMODE, 09 POS;
//                                    seq-p2 (rytm-mods 1033): 04 SCALE, 0A EDIT, 0B DIRTY, 0C FILL
//   host -> Rytm  2E 01 <mask lo7> <mask hi5> <flags> [0 0 <pin lo7> <pin hi5>]   the seq tracks (sent again after each
//                                                        hello 35); the pin part only when it says more (1033)
//                 2F 01 <p1..p4>                         the PAGE LEDs (page 1..4); the trig LEDs are 34 (core/ui/remote.h)
//                 46 01 <silent lo7> <silent hi5>        the pinned tracks with nothing sounding (1033: VCA closed)
//
// seq-p2: the Rytm's copy / paste / clear / undo of patterns, tracks, pages and trigs (2D 0A) are applied to the host's
// data with its own clipboard and one undo per level, as stock does (stock's undo function reports whether it undid);
// SCALE moves a seq track's own length; the queued pattern's first steps are planned ahead at the predicted switch
// (the change / master length from 2D 04); swing per track (seq=swing); the latency calibrated from a click's round
// trip through the Rytm (seq=cal; automatic once when a pinned seq track exists and the Rytm's stream comes); the pin's
// silence (46). The Rytm's own pattern for the touch view: core/seq/rytm_dump.h.
//
// Model: per seq track (up to kTracks of the Rytm's 12 at once) a pattern of up to kMaxPages x 16 steps; a step holds up
// to kNotes notes, each its own pitch, velocity, length and micro offset in 1/kSub of a step (-kSub..kSub: a note may
// start up to a step early or late, so broken chords, strums and loose timing are plain data). A step also keeps the
// CHD / INV / strum it was entered with (the encoders re-expand from them, core/poly.h's chord table).
//
// Time: the Rytm is master. Its step ticks carry the audio block they fired in (blk4, 32 frames each); the host keeps
// (1) the offset between the Rytm's frames and its own render frames (the smallest arrival offset over a window: the
// USB path's jitter only ever delays), (2) per track a phase-locked step clock (anchor + period from the tempo and
// the track's speed, the anchor pulled toward each tick, re-anchored on a jump). Every note is planned from that clock
// before its step's tick arrives and started `latency` frames early, at the render block it falls in (12 frames,
// 0.25 ms): it reaches the Rytm on the frame of the Rytm's own step, not one round trip late. The first step after
// PLAY cannot be early (nothing says when PLAY comes): it starts on its tick, counted as late.
//
// Storage: /.rytm/seq/<project>/pNNN.sqp per pattern (versioned sparse binary, sim/protocol.py sqp_pack), the seq tracks
// in /.rytm/seq/<project>/tracks.txt. Loaded on a pattern change (prefetched when the change is queued), saved a second
// after the last edit and at STOP.
//
// Threads: everything on the control side (the Pi's core 3), between render blocks: sysex() from host_sysex, midi()
// beside poly::midi, tick() from the platform's loop, pre_block() once before each render block, idle() among the
// card jobs (idle_save).
#pragma once
#include <stdint.h>

namespace core {
namespace seq {

constexpr int kStepsPerPage = 16, kMaxPages = 8, kMaxSteps = kStepsPerPage * kMaxPages;
constexpr int kNotes = 8;      // notes a step
constexpr int kSub = 96;       // sub-step units a step (lengths, micro offsets)
constexpr int kTracks = 4;     // seq tracks at once
constexpr int kRytmTracks = 12;
constexpr uint8_t kTickCmd = 0x2C, kStateCmd = 0x2D, kMaskCmd = 0x2E, kPageLedsCmd = 0x2F;
enum : uint8_t { kTransport = 1, kPattern = 2, kSwitch = 3, kScale = 4, kMutes = 5, kRec = 6, kTempo = 7, kSeqMode = 8,
                 kPos = 9, kEdit = 10, kDirty = 11, kFill = 12, kSolo = 13 };   // kSolo: fix1049 (rytm-mods 0026 1049)
// 2D 0A EDIT's ops (rytm-mods 0026 "1033"): the stock edit functions the Rytm ran (src 0) or seq mode's own gesture
// (src 1: a host trig / PAGE held + FUNC + RECORD / STOP / PLAY)
enum : uint8_t { kPCopy = 1, kPPaste, kPClear, kPUndo, kTCopy, kTPaste, kTClear, kTUndo, kGCopy, kGPaste, kGClear, kGUndo,
                 kRCopy, kRPaste, kRClear,
                 // 1042: the PATTERN / TRACK menus' RELOAD and SAVE (FUNC + SCNE, FUNC + CHRO)
                 kPReload, kPSave, kTReload, kTSave };
constexpr uint8_t kSilentCmd = 0x46;   // host -> Rytm 46 01 <silent lo7> <hi5>: pinned tracks with nothing sounding
enum : uint8_t { kStop = 0, kPlay = 1, kPause = 2 };
constexpr uint8_t kNone = 0x7F;
constexpr int kBlkFrames = 32;   // frames of a Rytm audio block (blk4's unit)

// ---- the pattern model (pattern.cpp; pure: no card, no clock)
struct Note {
  uint8_t pitch, vel;
  uint16_t len;    // 1/kSub step, >= 1
  int8_t micro;    // -kSub..kSub: from the step's start
  uint8_t fresh;   // recorded ahead of its step's planning this pass: skipped once (no double with the monitoring)
};
// seq-ux (1036): parameter locks, per step. param: 0..15 the track's Sidecar machine P1..P16 (its SRC page A..H =
// P1..P8, its SMPL page = P9..P16: the voice's x.p), kLockFx + 4 s + i: send FX s's P(2 + i) (fx::control.knob[s][2 + i],
// FX1 / FX2 P2..P5). value: 0021's 14-bit encoding, the integer part << 7 | the fraction in 1/128 (0..16383; the FX
// knobs' are integers << 7). A step with locks and no notes is a lock trig (Elektron's trigless lock).
// fx-ins: kLockIns + 4 s + k: the track's voice's INSERT slot s (A / B) P(k + 1) (core/fx/insert.h; integers << 7). An
// older Sidecar drops those locks when it reads the file (param >= its kLockParams).
constexpr int kLocks = 12;          // locks a step
constexpr int kLockFx = 16, kLockIns = 24, kLockParams = 32;
struct Lock {
  uint8_t param;
  uint8_t cond;     // seqtop V1: the lock's own condition (the COND knob's code; 0 always: see lock_cond_ok)
  uint16_t value;
};
// seqtop P4 / M4: a step's ratchet / roll / arp. cnt 0 / 1 off (CHORD / WALK), 2..kRtMax repeats; the ARP modes play
// the chord's notes one at a time over OCT octaves (cnt 0 / 1: one pass), each at RATE (AUTO: the step's longest note /
// the count), GATE % of the interval long (0: 100), the velocity along VCRV, the pitch moved PCRV per repeat
// (semitones; scale degrees with the pattern's scale and QNT on). Expanded into note events when the step is planned
// (rt_expand); the RND arp draws from E1's keyed RNG (kRngArp). The ids are the file's (never renumbered).
enum : uint8_t { kRtChord = 0, kRtWalk, kRtUp, kRtDown, kRtUpDn, kRtRnd, kRtPlayed, kRtConv, kRtModes };   // >= kRtUp: ARP
enum : uint8_t { kVcFlat = 0, kVcUp, kVcDown, kVcUpDn, kVcs };
constexpr int kRtRates = 9;    // 0 AUTO, then 2 1 2/3 1/2 1/3 1/4 1/6 1/8 steps
constexpr int kRtMax = 16;     // repeats / arp notes a step
struct Rtrg {
  uint8_t cnt, rate, vcrv;
  int8_t pcrv;
  uint8_t mode, oct, gate;
};
inline bool rt_on(const Rtrg& r) { return r.cnt >= 2 || r.mode >= kRtUp; }
// seqtop H1: a step's two FX columns {command, value}, the trackers' effect columns. Plan-time commands act on the step
// when it is planned: RTG n (a quick ratchet: n repeats, when the step has none of its own), DLY n (the whole step n/96
// late), CUT n (every note cut after n/96), CHN p (this column's chance for the OTHER column's command), RND r (each
// note moved at random within +-r: scale degrees with the pattern's scale and QNT on, else semitones), RNV r (velocity
// +-r), OCT n (+-4 octaves), TRS n (+-24: degrees / semitones as RND), FIL p (with FILL held, page p's step plays
// instead). Order commands (V3) change which step comes next, through the step map (walk_step): JMP n (after this step,
// step n), JRN w (a random jump within +-w steps; 0 anywhere), LOP n x (loop the last n steps x more times, then on),
// SKP n (skip the next n), DIR d (from here, direction d until the loop's end). A loop (what A:B counts) stays one
// cycle of the track's direction; at each loop's start the walk begins again (step 1, the track's direction). Random
// commands draw from E1's keyed RNG (kRngFx / kRngJump). The ids are the file's (never renumbered).
enum : uint8_t { kFxNone = 0, kFxRtg, kFxDly, kFxCut, kFxChn, kFxRnd, kFxRnv, kFxOct, kFxTrs, kFxJmp, kFxJrn, kFxLop, kFxSkp,
                 kFxDir, kFxFil, kFxCmds };
struct Fx {
  uint8_t cmd, val;   // val: OCT / TRS as int8; LOP (x - 1) << 4 | (n - 1)
};
inline bool fx_order(int c) { return c == kFxJmp || c == kFxJrn || c == kFxLop || c == kFxSkp || c == kFxDir; }
const char* fx_name(int c);                       // "---" "RTG" "DLY" ...
int fx_parse(const char* cmd, const char* val, Fx* out);   // a command and its value ("LOP" "4x2", "DIR" "rev", "OCT" "-1"): ok 1
const char* fx_text(const Fx& f, char* b, int cap);        // "JMP 9", "LOP 4x2", "OCT -1"
void fx_range(int c, int* lo, int* hi);                   // rui1050: command c's values (OCT / TRS as int8; LOP packed)
struct Step {
  uint8_t n;        // notes 0..kNotes (0: empty)
  uint8_t flags;    // bit 0 on (1), bit 1 muted; rui1040: bits 2..7 the trig condition (kCond*, cond_of)
  uint8_t chd, inv, prob;
  int8_t strum;     // 1/kSub a note, the CHD entry's broken chord (0: together)
  uint8_t nl;       // locks 0..kLocks
  uint8_t xf;       // seqtop: kXf* (R1: kXfRnd its notes random)
  Rtrg rt;          // seqtop P4 / M4 (v4 block kBlkRtrg)
  uint32_t slide;   // seqtop X1: bit p - this step's lock of param p slides to the param's next lock (v4 block kBlkSlide)
  Fx fx[2];         // seqtop H1 (v4 block kBlkFx)
  Note note[kNotes];
  Lock lock[kLocks];
};
// rui1040: Elektron-style trig conditions (the stock Rytm's set), evaluated per step by the planner: FILL / !FILL (the
// Rytm's FILL key, 2D 0C), PRE / !PRE (this track's last condition result), NEI / !NEI (the track before's), 1ST / !1ST
// (the first loop since PLAY or a pattern switch), A:B (true on loop A of every B, loops counted from PLAY / the switch).
// A step's probability (prob, 1..100) is a condition too; PRE / NEI do not set the result, every other condition does.
enum : int { kCondNone = 0, kCondFill, kCondNotFill, kCondPre, kCondNotPre, kCondNei, kCondNotNei, kCond1st, kCondNot1st,
             kCondAB = 9, kConds = kCondAB + 35 };   // A:B for B 2..8, A 1..B: 35
inline int cond_of(const Step& s) { int c = s.flags >> 2; return c < kConds ? c : kCondNone; }
inline void cond_set(Step& s, int c) { s.flags = (uint8_t)((s.flags & 3) | (c > 0 && c < kConds ? c : 0) << 2); }
inline bool cond_ab(int c, int* a, int* b) {   // A:B of a condition code (false: not one)
  if (c < kCondAB || c >= kConds) return false;
  int i = c - kCondAB, bb = 2;
  while (i >= bb) i -= bb, ++bb;
  *a = i + 1, *b = bb;
  return true;
}
// the trig view's COND knob: 0 none (100%), 1..8 FILL .. !1ST, 9..43 1:2 .. 8:8, 44..142 the probability 1..99 %
constexpr int kCondKnob = kConds + 99;
int cond_knob(const Step& s);
void cond_knob_set(Step& s, int v);
const char* cond_text(const Step& s, char* b, int cap);
// the planner's condition test of step s on Rytm track r at its loop iter (updates PRE's / NEI's result)
bool cond_pass(const Step& s, int r, int64_t iter);
// seqtop V1: a lock's condition - the COND knob's codes without PRE / NEI (a lock never sets or reads them): 0 none,
// FILL / !FILL, 1ST / !1ST, A:B, the probability 1..99 %. A lock whose condition fails is left out of its step's set
// (its param back to its base on that step, as a step that does not lock it); the probability draws from E1's keyed
// RNG (kRngLock, the param as its k).
bool lock_cond_ok(int c);                                // a code a lock may hold
// seqtop X1: a sliding lock glides from its value to the value of the param's next lock (the next step in play order
// - direction and jumps included - that locks it, within one loop), reaching it at that step's start; without one it
// holds. The ramp runs on the control side (pre_block, every 4 render blocks: 1 ms), through the same path as a lock.
const char* lock_cond_text(int c, char* b, int cap);     // "---" "FILL" "1:4" "30%"
int lock_cond_parse(const char* s);                       // "fill" "!1st" "1:4" "30%" or a code; -1 none
// E0 (docs/SEQ-IDEAS.md, docs/POLYPHONY.md "E0"): a seq track's poly config, modelled on the Analog Four MKII's POLY
// CONFIG (VOICES, ALLOCATION RESET / ROTATE / REASSIGN, UNISON with its DETUNE and PAN SPREAD) [public, recalled +
// Elektronauts]. All zero = AUTO: the machine plays as it did before E0 (its own default count and allocation; the
// 6-OP FM / MULTISAMPLE / WAVETABLE / GRANULAR pools exactly as before). Device-agnostic: the names are the A4's, so
// the concept carries over when the Analog Four is supported.
enum : uint8_t { kAllocReassign = 0, kAllocRotate = 1, kAllocReset = 2, kAllocs = 3 };
constexpr int kMaxVoicesCfg = 8, kMaxUnison = 8, kPolyGroups = 4;
struct PolyCfg {
  uint8_t voices;   // 0 AUTO (the machine's default), 1..8; 1 = mono: a new note retriggers / chokes the old
  uint8_t alloc;    // kAlloc*: REASSIGN (the same pitch's instance again, tails kept), ROTATE (round robin), RESET
                    // (always from the first instance: the most aggressive steal, short stabs)
  uint8_t unison;   // 0 / 1 off; 2..8 instances stacked on each note (the notes at once: voices / unison, >= 1)
  uint8_t detune;   // 0..127: the stack's spread in pitch, 127 = +-50 cents at its edges
  uint8_t spread;   // 0..127: the stack's pan spread (a voice on OUT PRE / POST; mono on TRK)
  uint8_t group;    // 0 none; 1..kPolyGroups: a poly group across tracks (data model only: docs/POLYPHONY.md "E0")
};
inline bool poly_default(const PolyCfg& c) {
  return !c.voices && !c.alloc && c.unison <= 1 && !c.detune && !c.spread && !c.group;
}
// seqtop P2 (docs/SEQ-IDEAS.md P2): a track's own speed, a ratio of the Rytm track's step clock (which already runs at
// the Rytm track's SCALE). 0 FOLLOW = 1:1 (as before). Elektron's set (1/8 .. 2 with 3/4 and 3/2) and the free ratios
// 2/3, 5/4, 7/8 (decided 2026-10-22, Q8). The ids are the file's: never renumbered (new ones appended).
enum : uint8_t { kSpdFollow = 0, kSpd1_8, kSpd1_4, kSpd1_2, kSpd3_4, kSpd3_2, kSpd2, kSpd2_3, kSpd5_4, kSpd7_8, kSpeeds };
struct Ratio {
  int num, den;   // Sidecar steps per Rytm step = num / den
};
Ratio speed_ratio(int spd);          // the ratio of id spd (an unknown id: 1:1)
const char* speed_name(int spd);     // "FOLLOW" "1/8" .. "7/8"
bool speed_elektron(int spd);        // one of Elektron's set (the knob's main range; the others on FUNC: Q8)
int speed_parse(const char* s);      // "follow" / "1/2" / "3/2" / "2" ... -> the id, -1 none
int speed_step(int spd, int d, bool func);   // the knob: d steps through Elektron's set (func: every ratio, by size)
// seqtop P1: the play directions' names ("FWD" "REV" "PEND" "PING" "RND" "BROWN"; kDir* in core/seq/rng.h)
const char* dir_name(int dir);
int dir_parse(const char* s);        // "fwd" / "REV" ... -> the id, -1 none
// seqtop M1 (core/seq/music.cpp): a pattern's key and scale. QNT OFF: an edit aid only (the touchscreen's rows, R1's
// random notes stay in it); EDIT: the NOTE knob moves by degree (FUNC: by semitone) and CHD builds diatonic chords on
// the scale (CHD MAJ / MIN / DIM / AUG: the triad on that degree, 7 / MAJ7 / MIN7: its seventh chord ...); PLAY: also
// every note quantised to the scale when it plays. The scale ids are the file's (never renumbered).
enum : uint8_t { kScaleChr = 0, kScaleMaj, kScaleMin, kScaleDor, kScalePent, kScalePhr, kScaleLyd, kScaleMix, kScaleLoc,
                 kScaleHmin, kScaleMmin, kScalePmaj, kScaleBlues, kScaleWhole, kScales };
enum : uint8_t { kQntOff = 0, kQntEdit = 1, kQntPlay = 2, kQnts = 3 };
const char* scale_name(int s);            // "CHR" "MAJ" "MIN" "DOR" "PENT" "PHR" "LYD" "MIX" "LOC" "HMIN" ..
int scale_parse(const char* s);           // a name (any case) or its id; "off": CHR; -1 none
uint16_t scale_mask(int s);               // bit k: the semitone k above the key is in it (CHR / unknown: 0xFFF)
const char* key_name(int k);              // "C" .. "B"
int key_parse(const char* s);             // "C" "F#" (any case) or 0..11; -1 none
bool in_scale(int pitch, int key, int scale);
int quantise(int pitch, int key, int scale);            // the nearest pitch in the scale (a tie: down)
int degree_move(int pitch, int key, int scale, int d);   // d scale degrees up / down (from the pitch quantised)
// The chord chd (poly.h's CHD) with inversion inv on root, diatonic in the scale (CHR: poly.h's own); its notes, count
int diatonic_chord(int root, int chd, int inv, int key, int scale, uint8_t* out);
// seqtop R1: random notes in the pattern's scale (CHR without one), from the seed. A track's mode: OFF, STEPS (the steps
// flagged kXfRnd), TRACK (every step with notes: their pitches ignored, rhythm / velocity / length / micro kept). A
// step's n notes are drawn without repeats from LOW .. LOW + 12 x OCT in the scale, weighted: FLAT, ROOT (the key 3x),
// CHORD (the step's CHD, else the key's triad, 3x), LOW / HIGH (a slope toward one end), STEP (near the written note).
// The draw is E1's keyed RNG (kRngNote; the seed, the track, the step, the loop): a replan, a predicted switch, a reload
// play the same notes; "same every loop" repeats one line.
enum : uint8_t { kRndOff = 0, kRndSteps, kRndTrack, kRndModes };
enum : uint8_t { kWFlat = 0, kWRoot, kWChord, kWLow, kWHigh, kWStep, kWeights };
constexpr uint8_t kXfRnd = 1;
constexpr uint8_t kRndLowDef = 48, kRndOctDef = 2;
// R1b: SEED MOD - the seed moved by a modulation, in whole seeds (seed + offset, offset 0..RANGE-1), so a sweep walks
// known variations. The source: a machine param of the track's voice (src 1..16: P1..P16 as it plays, after the Rytm's
// scenes / PERF locks and the Sidecar's LFOs - a slot the machine leaves free, Q9), sampled when the QUANT unit begins
// (STEP / LOOP / BAR; LOOP by default, Q10) with a little hysteresis at each boundary. AFFECTS: NOTES (R1's notes, the
// FX column's random notes / velocities, the arp's RND), +CHANCE (also probabilities and conditions' chances), +ORDER
// (also RND / BROWN and random jumps: sampled per loop), ALL. Written notes never move.
enum : uint8_t { kSqStep = 0, kSqLoop, kSqBar, kSqs };
enum : uint8_t { kAffNotes = 0, kAffChance, kAffOrder, kAffAll, kAffs };
constexpr uint8_t kSmodRangeDef = 16;
const char* rnd_mode_name(int m);   // "OFF" "STEPS" "TRACK"
const char* weight_name(int w);     // "FLAT" "ROOT" "CHORD" "LOW" "HIGH" "STEP"
// R1: n distinct random pitches into out (the k-th from rng_key(seed, track, idx, loop, kRngNote, k)); written: the
// step's own pitches (STEP's centre, the fallback when the range holds fewer than n); chord: CHORD's pitch classes
void rnd_notes(int n, uint32_t seed, int track, int idx, int64_t loop, int key, int scale, int low, int oct, int wght,
               uint16_t chord, const uint8_t* written, uint8_t* out);
// P4 / M4: a step's repeats as notes (off: from the step's start, 1/kSub step; the step's pitches as they play - R1's
// - in pitch[]); the count written (at most cap). seed..loop: the RND arp's draw. names: "CHORD" "WALK" "UP" ...
struct RtNote {
  int32_t off;
  uint8_t pitch, vel;
  uint16_t len;
};
int rt_expand(const Step& s, const uint8_t* pitch, int key, int scale, bool degrees, uint32_t seed, int track, int idx,
              int64_t loop, RtNote* out, int cap);
const char* rt_mode_name(int m);
const char* rt_rate_name(int r);    // "AUTO" "2" "1" "2/3" "1/2" "1/3" "1/4" "1/6" "1/8"
const char* vcrv_name(int v);       // "FLAT" "UP" "DOWN" "UPDN"
// the pitch classes (bit 0 = C) of a step's chord (its CHD on its lowest note, diatonic in the scale), else the key's triad
uint16_t chord_pcs(const Step& s, int key, int scale);
// E1: a track's seed. own 0: it follows the pattern's seed (Pattern::seed / seed_fixed); 1: its own fixed seed.
// same 1: the loop index is left out of the hash ("same every loop": a random line repeats like a written one).
struct Track {
  int8_t rytm;          // 0..11, -1 a free slot
  uint8_t hint;         // the machine (voice byte & 0x78), 0x7F unknown
  // poly per pattern (2026-10-26): the track is a poly track in this pattern (the v4 block kBlkOn; a file without it:
  // a slot holding steps is on, an empty one off). seq's mask is derived from it (seq.cpp remask)
  uint8_t on;
  uint16_t length;      // 0 AUTO (the Rytm track's length), else 1..kMaxSteps
  uint8_t dnote, dvel;  // defaults: a tap's note when nothing was played yet, its velocity
  uint16_t dlen;        // a new note's length (1/kSub step)
  uint8_t swing;        // 0 / 50 straight; 51..80: every second step late by (swing / 50 - 1) of a step (the file's byte)
  PolyCfg poly;         // E0 (v4 block kBlkPoly)
  uint8_t seed_own, seed_same;   // E1 (v4 block kBlkTrackSeed)
  uint16_t seed;
  uint8_t speed;        // seqtop P2: kSpd* (v4 block kBlkSpeed)
  uint8_t dir;          // seqtop P1: the play direction, kDir* (core/seq/rng.h; v4 block kBlkDir)
  uint8_t rnd_mode, rnd_low, rnd_oct, rnd_wght;      // seqtop R1 (v4 block kBlkRnd, with the steps' kXfRnd)
  uint8_t smod_src, smod_range, smod_quant, smod_aff;   // seqtop R1b (v4 block kBlkSmod)
  Step step[kMaxSteps];
};
// E2: the v4 file's tagged blocks (docs/SEQUENCER.md "E2"). A block this build does not know is kept verbatim and
// written back on save (keep[]); a per-step one is dropped (and counted) when its track's steps changed since the load.
constexpr int kKeepBytes = 1024;
// seqtop's blocks change how a pattern plays, so they are written required (an older v4 Sidecar locks the file rather
// than play it without them: Q1's rule)
enum : uint8_t { kBlkSeed = 1, kBlkTrackSeed = 2, kBlkPoly = 3, kBlkSpeed = 4, kBlkDir = 5, kBlkScale = 6, kBlkRnd = 7,
                 kBlkSmod = 8, kBlkLockCond = 9, kBlkRtrg = 10, kBlkSlide = 11, kBlkFx = 12,
                 kBlkOn = 13 };   // per pattern poly: <track> <on 0|1>, not required, written only where the rule above
                                  // (steps: on) does not already say it
enum : uint8_t { kBlkRequired = 1, kBlkPerStep = 2, kBlkTrack = 4 };   // a block's flags; kBlkTrack: payload[0] its Rytm track
struct Pattern {
  int index;            // 0..127 (A01 = 0), -1 none
  Track t[kTracks];
  uint16_t seed;        // E1: 0..9999
  uint8_t seed_fixed;   // 0 FREE (a new seed at each PLAY; the default), 1 fixed (the same every PLAY, saved)
  uint8_t key, scale, qnt;   // seqtop M1: the key (0 C .. 11 B), kScale* (0 CHR: none), kQnt* (v4 block kBlkScale)
  uint16_t nkeep;       // E2: the unknown blocks kept, keep[0..nkeep) (each with its 4-byte head)
  uint8_t keep[kKeepBytes];
  int8_t keep_rytm[kTracks];     // the tracks' step sections as loaded (crc32 of their bytes), for the per-step blocks
  uint32_t keep_crc[kTracks];
};
void clear(Pattern& p, int index);
Track* track_of(Pattern& p, int rytm, bool create);   // the slot of Rytm track rytm (created when asked and one is free)
void track_init(Track& t, int rytm);                   // an empty track of Rytm track rytm, every default
const Track* track_of(const Pattern& p, int rytm);
int used_steps(const Track& t);
// A note into a step: the same pitch replaced, else appended (dropped when the step is full); false when dropped.
bool add_note(Step& s, const Note& n);
bool remove_pitch(Step& s, int pitch);
// The step's notes = the chord chd / inv on root (core/poly.h expand, VOI AUTO), vel / len for each, strum applied.
void set_chord(Step& s, int root, int chd, int inv, int vel, int len, int strum);
// A broken chord: the notes' micro offsets 0, |strum|, 2 |strum| ... from the lowest (strum < 0: the highest first).
void apply_strum(Step& s, int strum);
// seqtop M1: set_chord in the pattern's scale (CHR: set_chord)
void set_chord_in(Step& s, int root, int chd, int inv, int vel, int len, int strum, int key, int scale);
// Steps of src from `from` copied over dst's from `to` (n of them, clipped to kMaxSteps); a track's steps cleared.
void copy_steps(const Track& src, int from, Track& dst, int to, int n);
void clear_steps(Track& t, int from, int n);
bool step_used(const Step& s);   // the step holds anything at all (a note, a lock, any seqtop extra)
// seq1112: a LENGTH change from from_len to a longer to_len fills the pages it reveals the Rytm's way: step i takes
// step i % from_len (1 -> 2 pages duplicates page 1, 2 -> 4 copies pages 1-2 into 3-4), whole Step structs, so the
// locks, conditions, ratchets, FX columns and slides come along. A revealed page that already holds steps (hidden
// by an earlier shortening) is left untouched. set_length() calls it; shortening does nothing at all.
void grow_pages(Track& t, int from_len, int to_len);
// seq-ux: a step's locks. lock_of: the value of param's lock, -1 none; set_lock replaces or appends (false: full);
// clear_lock removes one (false: none); a step's every lock: s.nl = 0.
int lock_of(const Step& s, int param);
bool set_lock(Step& s, int param, int value);
bool clear_lock(Step& s, int param);
bool has_locks(const Track& t);   // a step of t has a lock
// The file (sim/protocol.py sqp_pack / sqp_unpack): bytes written (0: cap too small); false: not a valid file (p empty).
// seqtop (core/seq/xcmd.cpp): a console line of the pattern model's own (seq=spd / dir / key / qnt / quantise / rnd /
// rndstep / smod / rtrg / fx / lcond / slide) applied to p. 1: done (*track the Rytm track it changed, -1 the pattern's);
// -1: refused (out: the usage); 0: not one of these.
int command_x(Pattern& p, const char* line, int* track, char* out, int cap);
int pack(const Pattern& p, uint8_t* out, int cap);
bool unpack(Pattern& p, int index, const uint8_t* d, int len);
uint32_t crc32(const uint8_t* d, int n);
int pack_bound();   // the largest pack() can write
int pack_version();  // 1042: the newest file version this build reads (a file above it is a newer Sidecar's)
// E2: why the last unpack failed: kUnpackNewer (a version above pack_version, or a required block this build does not
// know: a newer Sidecar's, locked), kUnpackDamaged (CRC, cut short, bad layout), 0 none. pack_dropped: per-step
// blocks the last pack left out (their track's steps had changed).
enum : int { kUnpackOk = 0, kUnpackNewer = 1, kUnpackDamaged = 2 };
int unpack_error();
int pack_dropped();
int unpack_unkept();   // E2: unknown blocks the last unpack could not keep (past kKeepBytes): lost at the next save

// ---- the clock follower (clock.cpp; pure)
// Rytm frames (blk x 32, unwrapped from 28 bits) -> host render frames. offset: the smallest (arrival - stamp) over
// the last kWindow samples; a sample far below it (a new link) or every sample far above it for kWindow in a row (the
// path got slower) resets the window.
struct Follower {
  static constexpr int kWindow = 24;
  int64_t win[kWindow];
  int n = 0, w = 0;
  int64_t offset = 0;      // host frame - Rytm frame at the fastest arrival
  bool valid = false;
  int64_t last_blk = -1;   // unwrapped
  void reset();
  int64_t unwrap(uint32_t blk28);                  // a stamp's block, unwrapped against the last
  void sample(int64_t host_arrival, int64_t rytm_frame);
  int64_t to_host(int64_t rytm_frame) const { return rytm_frame + offset; }
  int64_t to_rytm(int64_t host_frame) const { return host_frame - offset; }
};
// One track's steps: abs (a step count since PLAY, never wrapping) at Rytm frame anchor + (abs - anchor_abs) x period.
struct StepClock {
  bool on = false;          // anchored (a tick since PLAY / continue)
  int64_t abs = 0;          // the last tick's step count
  int last_step = -1;       // the Rytm's step at it, its length, speed index
  int rytm_len = 16, speed = 2;
  double anchor = 0;        // Rytm frame of anchor_abs
  int64_t anchor_abs = 0;
  double period = 0;        // frames a step; 0 unknown (from the ticks until a tempo is known)
  double last_frame = 0;
  int jumps = 0;            // re-anchors
  void reset() { *this = StepClock(); }
  // A tick of step `step` (0..len-1) at Rytm frame f; tempo_period: frames a 1x step at the reported tempo (0
  // unknown). restart: the first tick after PLAY / a pattern switch (abs = its step: 0, or a jump's position). Returns the step count advanced (0 the
  // first, 1 normally, more after dropped ticks).
  int tick(int step, int len, int speed, double f, double tempo_period, bool restart);
  double frame_of(double abs_step) const { return anchor + (abs_step - (double)anchor_abs) * period; }
  double step_at(double f) const { return period > 0 ? (double)anchor_abs + (f - anchor) / period : (double)abs; }
};
double speed_mult(int speed);                     // index 0..6 -> 2 1.5 1 0.75 0.5 0.25 0.125
double step_frames(double bpm, int speed);        // frames of one step at bpm and that speed (48 kHz, 16ths)

#if CORE_POLY
// ---- the engine (seq.cpp)
void init();                                   // boot: the pattern buffers (heap)
void reset();                                  // tests: everything stopped and empty, the latency default
bool sysex(const uint8_t* d, int len);         // a whole 2C / 2D from the Rytm (true: it was one)
void hello();                                  // the Rytm's hello (35): 2E again, a resync asked
// Clock-only tracks in the 2E (ticked in 2C, nothing else; core/song: BD while SONG RECORD is armed or replaying)
void set_clock_tracks(uint16_t mask);
void midi(uint8_t st, uint8_t d1, uint8_t d2); // a channel message (the Rytm's pads, a host keyboard): poly::midi's note sink (init)
// A note heard on voice vi (pitch as the voice plays it): what midi() records and step-enters, for a source that knows
// its voice and track (core/keys.h: a keyboard on the Pi; track -1: the seq track on vi shown first, else the first)
void note(int vi, bool on, int pitch, int vel, int track);
bool owns_track(int t);                        // Rytm track t (0..11) is a seq track (the mask)
int seqmode_on();                              // the Rytm's seq mode (2D 08): its track 0..11, -1 off
bool rytm_live_rec();                          // the Rytm records live (2D 06 REC on, 2D 01 PLAY)
void pre_block();                              // before each render block (kBlock frames): due notes started / ended
void tick(uint32_t ms);                        // the control loop: LEDs, the page's state, the debounced save
bool idle();                                   // a card job (load / prefetch / save): true if it did one
void project_changed();                        // 0x10 named another project (pool.h project)
void project_leaving();                        // 1042: before pool.cpp takes a new name: the old project's edits saved
#if CORE_MSAMP
// 2026-10-26, the RAM store (phase 2): the files a project load reads into the store for the sequencer (pool.h project's
// tracks.txt, its 128 patterns' .sqp and 1042's saved states .sqs); project_changed runs once it is full (pool.h
// project_fill_step), and the sequencer reads its files from there only, never the card. rekey: the sequencer's state
// is name's from now on (a rename moved its files: core/bundle/rename.h)
void want_files();
void rekey(const char* name);
// a project's store begins to fill (pool.h project_fill_begin): until project_changed its tracks.txt settings and the
// patterns loaded meanwhile are not editable (refused with that reason)
void loading_begin();
#endif
int pattern_locked(int index);                       // 1042: pattern index's file is locked (1 newer, 2 damaged), 0 not
bool owns_voice(int vi);                       // a seq track plays on voice vi now: its 0x04s make no note
bool any_tracks();                             // a seq track is set (the mask)
bool shown_wanted();                           // the SEQ page was asked for (seq=show, the Rytm's seq mode)
uint64_t frames();                             // render frames so far (pre_block's count)
// The Rytm's trig keys / PAGE / encoders while seq mode is on or the SEQ page shows (remote.cpp routes 31 / 32 here
// first): true when taken.
bool ui_key(int code, bool down, bool page_shown);
bool ui_enc(int e, int delta, bool func, bool page_shown);
bool leds(uint8_t c[16]);                      // the trig LEDs now (palette); false: none (not a seq track shown)
void page_leds(uint8_t c[4]);                  // the 4 PAGE LEDs (palette)
bool command(const char* p, char* out, int cap);   // the console's "seq", "seq=track:T:on|off", "seq=lat:FRAMES", ...
// Latency calibration (seq=cal:T): a click rendered into voice vi's output (render_voices calls cal_render for every
// voice after its own sound), found again in the Rytm -> host Overbridge stream of that voice (the platform feeds
// cal_audio with the voice's channel as it arrives, `arrival` = frames() then). latency = arrival - render: the round
// trip, which is what the planner's "start early" must cover (the MIDI tick path the follower measures is the same USB
// IN the audio comes back on). cal_wanted: a click is on its way or due (the platform feeds cal_audio only then).
// render_voices, after its voices v0..v1-1 of the block pre_block just counted: the click into its rows (hot: one
// compare when idle). cal_audio: the 8 voice stems of the Rytm -> host stream in its channel order (BD SD RS/CP BT LT
// MT/HT CH/OH CY/CB, frame f's at s + f * stride, full scale 2^31), n frames that arrived just now (frames() = arrival).
void cal_render(int16_t (*ring)[8], const int* rows, int v0, int v1);
void cal_audio(const int32_t* s, int n, int stride, int64_t arrival);
bool cal_wanted();

// The SEQ page's view (core/ui/page_seq.cpp) and the tests'
struct View {
  int track;              // the Rytm track shown (0..11)
  bool seq;               // it is a seq track
  bool seqmode;           // the Rytm's seq mode (GRID REC on a seq track)
  int page, npages, length;   // the edited page, the pages, the steps (AUTO resolved)
  int play_step;          // the playing step (-1 stopped / not this track)
  double play_pos;        // the playhead in steps (interpolated), -1 none
  int held;               // the held trig (0..15 of the page), -1 none
  int held_mask;          // every held trig
  bool page_held;
  int transport;          // kStop / kPlay / kPause
  bool rec_live, rec_grid;
  int pattern, queued;    // 0..127, -1
  double bpm;
  int quant;              // 0 OFF, 1 1/16
  bool follow;
  int sel;                // the held step's note selection (0 ALL, 1..n)
  int touched;            // the encoder turned last (-1 none)
  int latency;            // frames
  bool loaded;            // the pattern's file was read (or is new)
  int locked;             // 1042: the pattern's file did not load: 1 a newer Sidecar's version, 2 damaged / unreadable;
                          // its edits stay in memory, the file is never written (0: editable); 3 (the RAM store) its
                          // project is still loading: an edit is refused at once
  bool loading;           // the project's store still fills: its track settings' edits are refused (the RAM store)
  const Track* t;         // the track's data (null: none)
  int rytm_len, rytm_speed;   // the Rytm track's SCALE (2D 04 / its ticks), -1 unknown
  bool fill;              // the FILL key held (2D 0C)
  int swing;              // the host's swing on this track, 50..80 (% of a step pair; 50 straight)
  bool clip_track, clip_pattern;   // a track / pattern is on the host's clipboard
  // seq-ux (1036)
  int prompt;             // kPrompt*: the page asks (YES / NO), 0 none
  int prompt_track;       // the track it asks about
  int prompt_n;           // kPromptRestore: the kept notes (within the track's length)
  int prompt_locks;       // kPromptRestore: the kept steps with locks only
  bool prompt_undo;       // kPromptRestore: NO's clear goes to an undo history (UndoHooks set)
  int prompt_why;         // kPromptExplain: 0 an analog machine, 2 JUNO (mono), 3 another mono Sidecar machine
  int prompt_sel;         // kPromptMenu: the row (0 BACK, 1 TURN OFF, 2 LENGTH, 3 QUANTISE)
  bool off_hint;          // R2: B was turned down on a poly track (the footer says how to turn it off)
  int lock_page;          // trigs held + a page key: kLp* (the held steps' locks), -1 none
  bool view_opened;       // the Rytm's TRK + TRIG opened the page (NO closes it; rui1036's FUNC + LFO)
  bool trigview;          // rui1040: the TRIG key's view (the step's / the track's trig values), else the notes
  // rui1050 (the Rytm side of seqtop): the TRIG view's page, TRIG cycling it - no trig held kTvDefaults (NOTE VEL LEN
  // SWNG DIR SPD TLEN) / kTvScale (KEY SCL QNT) / kTvRandom (SEED RNG WGHT MODE SRNG SQNT SAFF); trigs held kSvTrig
  // (NOTE VEL LEN MICR COND STRM CHD INV) / kSvRtrg (RATE CNT VCRV PCRV MODE OCT GATE) / kSvFx (CMD1 VAL1 CMD2 VAL2);
  // 0 the notes (trigview false)
  int tpage;
  int cond_e;             // the lock page's knob whose COND is edited (a long push; its label gets a ?), -1 none
  bool func;              // FUNC held (the SEQ page catches it: 31 0x80, the Rytm's FUNC on both sides)
  const char* msg;        // a transient footer (PRINT RND's result, PLAY MODE's value), null none
};
enum : int { kTvNotes = 0, kTvDefaults, kTvScale, kTvRandom, kTvPages };
enum : int { kSvNotes = 0, kSvTrig, kSvRtrg, kSvFx, kSvPages };
// UX review S2: kPromptMenu, the poly track's own menu (DOWN on its SEQ page; YES before ux1027): BACK / TURN OFF / LENGTH / QUANTISE,
// UP / DOWN and YES (prompt_sel the row); TURN OFF then asks (kPromptOff). B turned down no longer asks (R2).
// rui1050: + PLAY MODE (the track's direction: YES / RIGHT the next, LEFT the one before; the menu stays) and PRINT RND
// (YES: the random notes of the loop playing, else the first, written into the steps: seq=print).
// fix1028 (the user's "Ask on make"): kPromptRestore - the make prompt's YES on a track with kept notes (an earlier
// TURN OFF) asks RESTORE n NOTES? (prompt_n): YES keeps them, NO clears them as an edit (a level in the UndoHooks
// history: FUNC + YES on the SEQ page, the touchscreen's UNDO bring them back). No kept notes: not asked.
enum : int { kPromptNone = 0, kPromptMake, kPromptExplain, kPromptOff, kPromptMenu, kPromptRestore };
enum : int { kMenuBack = 0, kMenuOff, kMenuLen, kMenuQnt, kMenuDir, kMenuPrint };
constexpr int kMenuRows = 6;
constexpr uint32_t kLongPushMs = 400;   // rui1050: a lock page's encoder held this long: its COND
enum : int { kLpTrig = 0, kLpSrc, kLpSmpl, kLpFltr, kLpAmp, kLpLfo, kLpFx, kLpIns };   // TRIG SRC SMPL FLTR AMP LFO FX; INS:
                                                                                      // FX pressed again (fx-ins)
View view();
// UX review T1, any core (two plain reads, drawing only): the active pattern (-1 none yet) and the transport (kStop ..)
int pattern_now();
int transport_now();
// seq-ux: the lock page's encoder e -> its param (-1 none); the held steps' lock of param (the first held step with
// one; -1 none); the value a new lock starts from (14 bits); TRK + TRIG's request for track r's SEQ page (the Rytm's
// 30 page 0x10 + r: the make prompt when r is no seq track); the page closed; the requests so far (the Pi's touch
// editor shows *track from each new one).
int lock_param(int page, int e);
bool pattern_seed(int* seed);   // rui1050: the pattern's seed (true: fixed, *seed it; false: FREE)
void pattern_scale(int* key, int* scale, int* qnt);
int held_lock_cond(int param, bool* slide);   // rui1050: the held steps' lock of param: its condition (*slide), -1 none
int seed_slot_of(int r);   // rui1050: track r's machine's SEED MOD slot (P1..P16), -1 none
int seed_offset(int r);    // rui1050: SEED MOD's offset now (the footer's 41+3), 0 none
int held_lock(int param);
int lock_base(int param);
void view_request(int r);
void page_closed();
// rui1040: the Rytm's selected track moved (49 02): another seq track - the SEQ page follows it; a track that is no seq
// track - the page the SEQ view opened (TRK + TRIG, seq mode) closes, so its trig keys and pads are stock's again
void selected_track(int t);
uint32_t view_requests(int* track);
// seq-ux: the voice's P1..P16 from the Rytm (voice_report, before the LFOs): a param a step locked keeps its lock
// (the report is its new base)
void params_in(int vi, float* p, int np);
double play_pos(int rytm);   // the playhead of Rytm track rytm in its steps (interpolated), -1 not playing
// U1 NOTE EDIT (docs/SEQ-IDEAS.md U1, docs/SEQUENCER.md "Built: NOTE EDIT"): on a poly track's SEQ page the DATA
// encoder (LEVEL, 0026's encoder 8) walks a highlight through every note of the track in play order (step, then pitch
// upward); A..E edit it or the selection (the DATA push, key 0x78). The state for the page, the LEDs and the touchscreen:
// the highlighted note (idx -1 none: no notes), its place among the notes DATA walks now (pos 1..count; a held step:
// its chord), its place in its own step (chord_i 1..chord_n), the selected notes, a counter that moves on every change
// of any of it (the touchscreen's redraw), held: the scope step (-1 the whole pattern).
struct NoteEdit {
  bool on;
  int idx, pitch;
  int pos, count;
  int chord_i, chord_n;
  int sel;
  int held;
  uint32_t gen;
};
NoteEdit note_edit();
bool note_selected(int idx, int pitch);   // the note (step idx, pitch) of the shown track is in NOTE EDIT's selection
// The targets' values for the page: A POS (step idx) B NUDG (micro) C LEN D PTCH E VEL, the lowest and the highest
// over the targets (the selection, else the highlighted note); false: none
bool note_edit_range(int e, int* lo, int* hi);
bool data_wanted();   // the SEQ page catches DATA and its push now (the track shown is a poly track)
bool yes_wanted();    // ux1027: the SEQ page catches YES now (a prompt, FUNC or a trig held); else YES is the Rytm's
bool updown_wanted(); // updown1029: the SEQ page catches UP / DOWN now (a prompt or FUNC held); else they are the Rytm's (CHRO's octave)
// NOTE EDIT's undo (review round 1): the platform's history (the Pi: pi/ui/seq_touch's, the touchscreen's UNDO too, so
// one history holds both), save before a gesture changes track rytm of pattern (one level a gesture: a knob's detents
// within kNeUndoMs are one); undo (the Rytm's FUNC + YES in NOTE EDIT) puts the newest level of that track into t,
// false: none. Null: no undo (the footer says so).
struct UndoHooks {
  void (*save)(int rytm, int pattern, const Track& t);
  bool (*undo)(int rytm, int pattern, Track& t);
  void (*clear)();   // review round 2: a project change / load - the history is the old project's
};
void set_undo_hooks(const UndoHooks* h);
bool shown_poly();   // fix1028 review: the SEQ page's track is a poly track now (its keys, its trig LEDs)
constexpr uint32_t kNeUndoMs = 1000;
// review rounds 2 / 3 (N3): the platform's UNDO or REDO of track rytm is about to run (the touchscreen's seqed=undo /
// redo): when the track's content changed outside NOTE EDIT since its last gesture, the state after that gesture
// becomes a level first, so the UNDO takes back the outside edit alone (REDO brings it back); the snapshot ends there.
// level_taken: the platform took a level of its own (a touch edit) on the current pattern: that state is covered.
void undo_prepare(int rytm);
void level_taken(int rytm);
uint32_t note_edit_work();   // tests: the last move's work (targets checked over the shifts tried)
// the touchscreen moved a note (seqed=move): NOTE EDIT's highlight / selection go with it on Rytm track rytm
void note_moved(int rytm, int from_idx, int from_pitch, int to_idx, int to_pitch);
void view_set(int track, int page, int held, bool follow);   // tests and the renderer
struct Stats {
  uint32_t ticks, ticks_bad, states, switches, notes_on, notes_off, late, late_frames_max, cancelled, dropped_queue;
  uint32_t recorded, loads, prefetched, saves, save_fails, edits, resyncs, masks;
  uint32_t load_fails, save_refused;
  uint32_t mask_early;        // per pattern poly: 2Es that carried the predicted next pattern's mask ahead of its switch
  uint32_t loading_refused;   // edits refused while the project's store filled (the RAM store: Task 5 fix round 1)
  uint32_t saved, reloads;   // 1042: the Rytm's SAVE PATTERN / TRACK written as pNNN.sqs; RELOADs followed   // 1042: files that did not load (locked); saves refused on a locked pattern
  int64_t offset;
  int32_t err_last;       // the last tick's phase error (frames, before the pull)
  int32_t err_max;
  uint32_t dropped_rytm;  // 2D 09's count
  uint32_t edits_rytm, undos, scales, dirties, silents, predicted, predict_ok, predict_miss, cals, cal_fails;
  int32_t cal_last;       // the last calibration's round trip (frames), -1 none
  uint32_t locks_applied, locks_set;   // seq-ux: lock sets applied at their steps; locks written by the knobs
  uint32_t previews;      // rui1037: trigs held + YES, the held steps played (step preview)
  uint32_t solos;         // fix1049: 2D 0D SOLO reports taken
};
uint16_t silenced();      // fix1049: the Rytm tracks not heard now: muted (2D 05), or not soloed while any is (2D 0D)
const Stats& stats();
// tests: the events waiting (frame, on 1 / off 0, voice, note)
int pending(int64_t* frames, uint8_t* on, uint8_t* voice, uint8_t* note, int max);
void set_latency(int frames);
// Edits from elsewhere on the control side (tests, the Pi's touch editor): the current pattern / one track's data (null:
// none and not created), then touched() (the LEDs, the SEQ page and the debounced save follow; stats().edits counts it).
// The pointers stay valid until the next pattern change (load or switch): fetch them again for each edit.
Pattern& edit_pattern();
Track* edit_track(int rytm, bool create);
// The Pi's touch editor (pi/engine/seqed_pi.cpp track_for): Rytm track rytm's slot only while it is a seq track now (in
// the mask: its slot on, its machine a Sidecar one or not reported); nullptr otherwise - an off or released slot is
// never edited from there (its edits would be stored and saved, and never played)
Track* edit_seq_track(int rytm, bool create);
// E0: the poly config Rytm track rytm plays with now (its pattern's, else the track's default: seq=poly)
PolyCfg poly_of(int rytm);
// E0: the seq tracks' poly configs for the web page's /status: "\"poly\":[{\"t\":T,\"v\":..,\"a\":..,\"u\":..,\"d\":..,
// \"s\":..,\"g\":..,\"own\":0|1,\"info\":\"PLAITS 4 INST ..\"}]" (T 1..12; own: the pattern's own, else the default)
int poly_json(char* out, int cap);
// seqtop: the pattern's and the seq tracks' seqtop settings for the web page's /status, steps with extras sparse:
// "\"seqx\":{\"key\":K,\"scale\":S,\"qnt\":Q,\"seed\":N,\"fixed\":0|1,\"t\":[{\"t\":T,\"spd\":..,\"dir\":..,
// \"rnd\":[mode,low,oct,wght],\"smod\":[src,range,quant,aff],\"off\":N,\"x\":[{\"s\":S,\"rt\":[7],\"fx\":[[c,v],[c,v]],
// \"r\":1,\"lc\":[[param,cond]..],\"sl\":[param..]}..]}..]}" (T, S 1-based; 0: cap too small)
int seqx_json(char* out, int cap);
void touched();
#endif

}  // namespace seq
}  // namespace core
