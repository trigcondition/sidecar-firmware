// core/granular/granular.h - the GRANULAR machine (docs/GRANULAR.md; it supersedes the GRAIN placeholder, machines.h
// kGrainMachine): clouds of short windowed grains read from a source at a position, with spray, size, density,
// per-grain random pitch, reverse probability, four window shapes, freeze and feedback; polyphonic (each note its own
// grain stream at its pitch, 60 = the source's own) with the chord params on P13..P16 (core/poly.h), playable by
// trigs, the Sidecar sequencer and the live pads / keys. Pi and Mac only (CORE_MSAMP).
//
//   sources  SRC (P1, the 14-bit word bank * 128 + row) is a row of the project's SAMPLES pool (core/lib/pools.h,
//            docs/POOLS.md): an input row plays its live source - the Rytm's Overbridge IN stream as it arrives at the
//            host (IN = AUDIO IN L+R, MAIN L+R, the voice stems BD SD RS-CP BT LT MT-HT CH-OH CY-CB; the stream the
//            recorder and the send FX tap, live_in from the IN completion), written into the voice's own ring (kRing
//            frames, 5.5 s) while it is not frozen; a built-in row PAD / BELLS (generated here, 4 s each); a sample
//            row its file (resolved by its hash: core/lib/library.h), whole in the pool RAM (core/pool/pcache.h: a grain
//            reads anywhere; one copy a content, shared with DSP SAMPLE and MULTISAMPLE) as int16 mono at its own rate,
//            at most kSlotFrames (87 s at 48 kHz), kSlots viewed at once. Row 0 of a bank (OFF), an empty row and a
//            missing file are silent. The template puts the inputs and PAD / BELLS in H001..H012. A trig while its
//            sample loads plays when it is ready, within kLateMs.
//   grains   per voice at most `cap` at once (gr=grains:N, default kDefaultCap, up to kMaxGrains): a grain that would
//            exceed it is left out (counted). Each grain: linear interpolation, its window from a table, its pitch
//            2^((key - 60 + random) / 12) x the source's rate / 48000, forward or reversed. The cloud's level is
//            kept even: each grain at 1 / sqrt(overlap) (overlap = DENS x SIZE).
//   cores    report / note_off / silence / step / command: the control core; render_add: the voice's render core
//            (notes through a sequence-locked mailbox as msamp's; the source slot through a hazard slot); live_in:
//            the IN completion's core (one writer, a ring of all ten live sources; each voice's render core copies
//            its source from it). No allocation after begin().
//
// The params (P1..P16 = sound words 9..16, 17..24):
//   P1 SRC   the source, as above (a SAMPLES pool row)
//   P2 POS   card: where the playhead starts (0 = the start, 127 = the end); LIVE: how far back the grains read (0 = the
//            newest audio, 127 = 5 s ago)
//   P3 SPRY  the spray: each grain's position jittered back by up to 2 s (squared), and its start time by up to +-50 %
//   P4 SIZE  5 ms .. 1 s (exponential)       P5 DENS  1 .. 200 grains a second (exponential)
//   P6 PRND  per-grain random pitch, up to +-12 semitones
//   P7 REV   the probability a grain plays reversed (0 .. 100 %)
//   P8 WIN   zones: HANN TRI TUKEY PERC
//   P9 ATK  P10 REL  the note's envelope (1 ms .. 10 s; full while held)
//   P11 HOLD 0..119: card - the playhead's speed, 1x at 0 (the sample time-stretched at the note's pitch) slowing to
//            0 at 119; LIVE - the feedback, the voice's output into its ring (0 .. 0.95, soft-clipped); 120..127 FREEZE:
//            card - the playhead stops at POS; LIVE - the ring stops recording (the grains play what it holds)
//   P12 ANIM the cloud animated (Tonverk-like): 0 off; the depth grows with the knob, the cycle steps every 16 from
//            8 bars down to 1/16 (8BAR 4BAR 2BAR 1BAR 1/2 1/4 1/8 1/16), on the Sidecar's clock (the Rytm's MIDI clock:
//            locked to its song position while it plays). It moves the head (card: +-a quarter of the sample, 4 s at
//            most; LIVE: further back, up to the depth x the ring), the size (+-0.6 octave), the density (+-0.4
//            octave) and adds spray, each on its own harmonic of the cycle.
//   P13..P16 CHD INV SPR VOI (core/poly.h)
// Until a rytm-mods 0021 change gives the Rytm the machine, `gr=voice:V` makes voice V play GRANULAR whatever
// built-in machine the Rytm reports for it.
#pragma once
#include <stdint.h>

namespace core {
namespace gr {

constexpr int kLive = 10;                 // live sources (SRC 0..9)
constexpr int kBuiltins = 2;              // generated sources (SRC 10, 11)
constexpr int kSlots = 8;                 // card sources viewed at once (each a pinned block of the pool RAM)
constexpr uint32_t kSlotFrames = 1u << 22;   // a card sample's frames at most (87 s at 48 kHz; pcache::kMaxFrames)
constexpr uint32_t kRing = 1u << 18;      // a voice's live ring (5.46 s)
constexpr uint32_t kCap = 1u << 13;       // the live sources' capture ring (170 ms)
constexpr int kPerVoice = 6, kTotal = 24;
constexpr int kMaxGrains = 128, kDefaultCap = 64;
constexpr uint32_t kChunk = 32768;
constexpr uint32_t kLateMs = 250;
constexpr int kHoldFreeze = 120;          // P11 HOLD at or above: FREEZE
constexpr uint32_t kRecentMs = 100;       // a card slot switched away from is not reloaded this soon (its grains fade)

#if CORE_MSAMP
void begin(bool card);
bool step();
void rescan();
bool forced(int vi);                       // gr=voice:V
void report(int vi, int note, int vel, const float* p);
void note_off(int vi);
void silence(int vi);
void render_add(int vi, float* out, int n);
void render_add2(int vi, float* l, float* r, int n);   // OUT PRE / POST: the pair (docs/STEREO.md)
void live_on(int vi, int key, int vel, int ch, int mnote);
void live_off(int vi, int ch, int mnote);
int voice_notes(int vi);
float headroom(int vi);
const char* entry_name(int e);
int entries();
// The source list as the displays show it: entry e = code e (bank * 128 + row of the SAMPLES pool), its group ("BANK
// A".."BANK H", "LIVE", "BUILT-IN"), its path ("" an input / built-in / empty row), its state and a note ("TOO LONG:
// FIRST 87 S", "LOADING", "NOT A WAV IT READS", "MISSING", its length once in RAM). list_gen changes with every new list
// (any core: a display copies it then). Racy reads on other cores: names may be mid-compose for a moment.
enum : int { kStateNone, kStateCard, kStateLoading, kStateRam, kStateCut, kStateFailed };
const char* entry_group(int e);
const char* entry_path(int e);
int entry_state(int e);
const char* entry_note(int e, char* b, int cap);
uint32_t list_gen();
int src_entry(int vi, int v);   // the entry SRC value v plays (the code itself)
// A choice (the Pi's touchscreen list; "gr=pick:V:C005"): the Rytm's SRC of the voice's track written (48, as a knob
// turn: the kit keeps it). False: no such entry.
bool pick(int vi, int e);
bool voice_picked(int vi);    // (always false: no picks over SRC)
// A short preview of entry e (< 0: the voice's own) on voice vi: a ~0.9 s note at key 60 with the voice's knobs, when
// the sample is in RAM (loaded first). The Rytm's SRC list after a turn settles, the touchscreen's pick.
void preview(int vi, int e);
void kit_changed(int kit);
int entry_lib(int e);         // srclib::Lib (kLibNone plays; OFF / empty / missing)
// The Overbridge IN stream (the IN completion, one core): n frames, channel c of frame i at f[i * stride + c] (int32
// full scale), the Rytm's 12 channels first (MAIN L/R, the 8 voice stems, AUDIO IN L/R).
void live_in(const int32_t* f, int n, int stride);
bool live_wanted();                        // a voice reads a live source now (the IN completion may skip live_in else)
bool command(const char* p, char* out, int cap);   // "gr", "gr=voice:V[:off]", "gr=list", "gr=rescan", "gr=grains:N",
                                                   // "gr=pick:V:ROW", "gr=preview:V[:ROW]", "gr=audition:on|off"
const char* hold_text(int v, char* b, int cap);    // P11's value text: "0".."99", "FRZ"
const char* anim_text(int v, char* b, int cap);    // P12's: "OFF", then its cycle "8BAR" .. "1/16"

// The display's view of a voice's cloud (any core; a racy snapshot, for drawing only): the source, where the head is,
// each sounding grain's position and level. Positions 0..1: card - of the sample; live - of what the ring holds, 0 the
// oldest, 1 the newest (the write head).
struct Viz {
  int entry;                  // the list entry (-1 none)
  bool live, frozen;
  float head;                 // POS (+ ANIM, + the scan) now, 0..1
  float anim;                 // ANIM's depth 0..1 (0 off)
  int n;                      // grains
  float pos[kMaxGrains], amp[kMaxGrains];   // amp 0..1
  uint8_t rev[kMaxGrains];
};
bool viz(int vi, Viz* o);     // false: voice vi has no GRANULAR source now
// The source's outline: cols columns of min / max (-1..1) over the same 0..1 axis as Viz (live: the ring, newest right)
int viz_wave(int vi, int cols, float* mn, float* mx);

struct Stats {
  int listed, resident, active, peak;
  uint32_t trigs, notes, steals, waits, late, loads, load_fails;
  uint32_t grains, capped;                 // grains started, grains left out at the cap
  int grains_now, grains_peak;
  uint32_t live_frames;                    // frames live_in took
};
const Stats& stats();
// tests
void set_cap(int n);
int cap();
int voice_grains(int vi);                  // the render core's grains sounding on vi now
uint32_t ring_written(int vi);             // frames voice vi's ring took (its write head)
float ring_at(int vi, uint32_t frame);     // the ring's frame (absolute, as ring_written counts)
int voice_entry(int vi);
void reset();
#endif

}  // namespace gr
}  // namespace core
