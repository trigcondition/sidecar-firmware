// core/chop/chop.h - CHOP, a sample-chopping machine for the Sidecar's voices (docs/CHOP.md; the design:
// docs/superpowers/specs/2026-10-24-chop-machine-design.md). One sample cut into up to 64 slices, each with its own
// settings (core/chop/slicemap.h), played by the Rytm's trigs (knob SLC, p-lockable), its pads in CHRO (a 12-slice
// window from SLC), the Sidecar sequencer's notes (note 0 = slice 1: the piano roll is a slice lane) and the pads /
// keys bound to the voice; poly through its own pool of slice players (VOICES up to 8, E0: core/inst_alloc.h). Pi and
// Mac only (CORE_MSAMP).
//
//   source   SRC (P1, the 14-bit word bank * 128 + row) is a row of the project's SAMPLES pool (core/lib/pools.h), its
//            file whole in the pool RAM (core/pool/pcache.h: loaded through the card core, never on the render or the
//            control core's time; a slice reads anywhere) as int16 at its own rate, mono or stereo, at most 87 s. A
//            slice source is a FILE (that) or a RAM buffer (use_buffer: the tests, and the LIVE source of phase 2 -
//            record trigs filling a buffer - which needs no second engine or editor). A trig while the sample loads
//            plays when it is in, within kLateMs.
//   map      the slices are the sample's slice map (slicemap.h), found by the sample's sha256 in the INSTRUMENTS pool
//            (an .sfz instrument whose member is the sample: CHOP's own /CHOP/<project>/<name>-<sha8>.chop.sfz, else
//            the sampler's <name>-slices.sfz, read as v1 and upgraded), read on the card core; none: GRID 16, not
//            written until it is edited. An edit is written 1 s after the last one (cardq's atomic write: never on
//            this core) and the file goes into the INSTRUMENTS pool once (a bundle carries it with its sample).
//   playing  a hit picks slice SLC + (note - 60) (the Rytm's trigs: note 60 unless a NOTE lock; the pads in CHRO
//            60..71), or the note itself (the sequencer: note 0 = slice 1), modulo the slices; then SHUFFLE (SEED,
//            P5: a keyed permutation, 0 the map's order) and NEIGHBOUR CHANCE (the slice's own % and NBR, P6: a keyed
//            draw per hit, repeatable). The knobs P9..P16 override the slice's settings for that hit (fully down: the
//            slice's own), so a p-lock stutters or tape-stops one trig.
//   tools    STUTTER (the first part repeated N times, its length and decay), TAPE STOP / SPIN-UP (the pitch falls to
//            zero over the end / rises from zero at the start), MORPH (P4: an equal-power crossfade with the right-hand
//            neighbour, time-aligned starts, live while it plays), SHUFFLE, NEIGHBOUR CHANCE, RESAMPLE SLICE (the slice
//            played once on its voice and taken after its INSERT chain - and with FX the send FX's returns - by the
//            sampler (core/samp: written on the card core into /Samples/<project>/ and the SAMPLES pool), then offered
//            as the slice's own sample).
//   chopping GRID 2..64, TRANSIENT (onsets, sensitivity 0..127, in pieces on the control core), MANUAL (add / delete /
//            move / merge / split, a slice's start and end), LAZY (MPC Live's: the sample plays from the start, each
//            pad / trig / screen tap drops a marker where the player heard the sample - the latency subtracted -
//            snapped to a zero crossing within 2 ms; UNDO, STOP). One level of UNDO for every edit.
//   cores    report / note_off / live_on / live_off / silence / step / the edits / command: the control core (core 3
//            on the Pi); render_add / render_add2: the voice's render core (notes through a sequence-locked mailbox as
//            GRANULAR's; the sources never dropped while a note reads them). No allocation after begin(), none on the
//            render core, no card I/O anywhere here (cardq requests, taken back at later steps).
//
// The params (P1..P16 = sound words 9..16, 17..24; "SLC" = the slice's own setting):
//   P1 SRC   the sample (a SAMPLES pool row)                P2 SLC   the slice the Rytm's trigs play (1..64, p-lockable)
//   P3 TUNE  -24..+24 semitones on every slice             P4 MRPH  MORPH with the right-hand neighbour, 0..100 %
//   P5 SEED  SHUFFLE: 0 OFF (the map's order), 1..127      P6 NBR   NEIGHBOUR CHANCE on top of the slices' own, 0..100 %
//   P7 VEL   velocity's share of the level, 0..100 %       P8 LEV   the level
//   P9 STUT  SLC, 2..16 repeats                            P10 SLEN SLC, the repeated part 5 ms .. 1 s
//   P11 TAPE SLC, OFF, STOP, SPIN                          P12 TTIM SLC, 20 ms .. 4 s
//   P13 ATK  SLC, 0.5 ms .. 2 s                            P14 DEC  SLC, 10 ms .. 10 s, HOLD (fully up)
//   P15 DIR  SLC, FWD, REV                                 P16 LOOP SLC, OFF, FWD, PING
#pragma once
#include <stdint.h>

#include "slicemap.h"

namespace core {
namespace chop {

constexpr int kPerVoice = 8;          // slice players a voice (VOICES' most)
constexpr int kTotal = 32;            // in all
constexpr int kSrcSlots = 16;         // sources viewed at once (pinned pool RAM blocks): samples, RESAMPLE'd slices
constexpr int kMaps = 10;             // slice maps held (a voice's, and recent ones)
constexpr uint32_t kSlotFrames = 1u << 22;   // a source's frames at most (pcache::kMaxFrames: 87 s at 48 kHz)
constexpr uint32_t kLateMs = 250;
constexpr uint32_t kSaveMs = 1000;    // an edit is written this long after the last one
constexpr int kDefaultGrid = 16;      // a sample without a map: GRID 16
constexpr int kKeyRoot = 60;          // a Rytm trig's / CHRO pad's note that plays SLC itself
constexpr int kPreviewCh = 125;       // the editor's auditions (a live note's channel tag no MIDI channel uses)
constexpr int kBins = 256;            // the overview's frames a bin (min / max pairs, for the editors' waveforms)
enum Field : int { kFPitch = 0, kFFine, kFGain, kFRev, kFAtk, kFDec, kFLoop, kFChoke, kFNudge, kFStut, kFSlen, kFSdec,
                   kFTape, kFTms, kFNbr, kFields };
const char* field_name(int f);        // "PTCH" "FINE" "GAIN" "REV" "ATK" "DEC" "LOOP" "CHOK" "NUDG" "STUT" "SLEN" "SDEC"
                                      // "TAPE" "TTIM" "NBR" (<= 4 characters: the Rytm's labels)
int field_of(const char* s);          // by name (any case) or "pitch" "fine" ..; -1 none
int field_get(const Slice& s, int f);
void field_set(Slice* s, int f, int v);   // clamped to the field's range
void field_range(int f, int* lo, int* hi);
const char* field_text(const Slice& s, int f, uint32_t rate, char* b, int cap);   // "+3" "-12C" "OFF" "25%" "300MS"

#if CORE_MSAMP
void begin(bool card);
bool step();                          // one bounded job (a load, a map read / write, a TRANSIENT piece); true: did one
bool forced(int vi);                  // chop=voice:V
void report(int vi, int note, int vel, const float* p);
void note_off(int vi);
void silence(int vi);
void render_add(int vi, float* out, int n);
void render_add2(int vi, float* l, float* r, int n);   // OUT PRE / POST: the pair (docs/STEREO.md)
void live_on(int vi, int key, int vel, int ch, int mnote);   // ch poly::kSeqCh: the sequencer's (note = slice)
void live_off(int vi, int ch, int mnote);
int voice_notes(int vi);
void seq_hit(int vi, int note, int vel);   // the sequencer's mono path (voices.cpp voice_play): note = slice
bool command(const char* p, char* out, int cap);

// ---- the slices as the editors see them (the control core; tests): the voice's map, null while it has none yet
const Map* map_of(int vi);
int slices(int vi);
int slice_of_hit(int vi, int note, bool seq_note);   // the slice a hit plays before SHUFFLE / NEIGHBOUR (tests)
int played_slice(int vi);                            // the slice the voice's last hit played (-1 none)
bool sample_ready(int vi);                           // its sample is in RAM
uint32_t sample_frames(int vi);
uint32_t sample_rate(int vi);
// the sample's outline over frames [a, b) in cols columns of min / max (-1..1); 0 when not ready
int outline(int vi, uint32_t a, uint32_t b, int cols, float* mn, float* mx);
// the playheads of the voice's notes now (frames of the map's sample; -1 none), at most max; racy, for drawing
int playheads(int vi, int32_t* pos, int max);
uint32_t map_gen(int vi);                            // changes with every edit of the voice's map (the views redraw)
const char* map_file(int vi);                        // where it is written ("" not yet)
int map_row(int vi);                                 // its INSTRUMENTS row's code (-1 not in the pool yet)
bool map_dirty(int vi);

// ---- the edits (the control core): each takes one level of UNDO first; false refused (the map unchanged)
bool edit_grid(int vi, int n);
bool edit_transient(int vi, int sens);               // started: done over the next steps (transient_busy)
bool transient_busy(int vi);
bool edit_add(int vi, uint32_t frame);
bool edit_del(int vi, int k);
bool edit_merge(int vi, int k);
bool edit_split(int vi, int k, uint32_t frame);      // frame 0: the middle
bool edit_start(int vi, int k, int32_t frame);
bool edit_end(int vi, int k, int32_t frame);
bool edit_field(int vi, int k, int field, int value);
bool edit_undo(int vi);
// LAZY: the sample plays from its start on the voice; taps (pads, trigs, the screen) drop markers; STOP keeps them
bool lazy_arm(int vi);
int32_t lazy_tap(int vi);                            // the marker's frame, -1 refused
bool lazy_undo(int vi);
bool lazy_stop(int vi, bool keep);
bool lazy_on(int vi);
const Lazy* lazy_state(int vi);
uint32_t lazy_playhead(int vi);                      // frames of the sample played since the arm (racy)
void set_latency(int frames);                        // chop=lat:N; -1: the estimate (the FIFO + the device's loop)
int latency();                                       // frames a LAZY tap is moved back
void audition(int vi, int k);                        // slice k once at its own settings (the voice's knobs)
// RESAMPLE SLICE: slice k played once on voice vi, taken after its INSERT chain (fx: and the send FX's returns) by the
// sampler; when the take is in the SAMPLES pool the offer waits (resample_offer); resample_answer(yes) makes it the
// slice's own sample
enum RsState : int { kRsIdle = 0, kRsPlaying, kRsWriting, kRsOffer, kRsFailed };
bool resample(int vi, int k, bool fx);
int resample_state(int* slice = nullptr, char* row = nullptr);   // row: "A012" once in the pool
bool resample_answer(bool yes);
// a RAM buffer as the voice's source (the tests; phase 2's LIVE): pcm stays the caller's while the voice uses it;
// null: back to SRC's sample. A fresh map (GRID 16), never written to the card.
bool use_buffer(int vi, const int16_t* pcm, uint32_t frames, int ch, uint32_t rate);

struct Stats {
  uint32_t trigs, notes, steals, chokes, waits, late, loads, load_fails;
  uint32_t maps_read, maps_written, map_fails, maps_new;
  uint32_t lazy_taps, lazy_refused, nbr_moves, resamples;
  int active, peak;
};
const Stats& stats();
void reset();                         // tests: every note gone, the maps forgotten, the sources dropped
uint32_t render_allocs_check();       // tests: blocks rendered (render_add / render_add2 calls), for the no-allocation check
#endif

}  // namespace chop
}  // namespace core
