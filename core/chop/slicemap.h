// core/chop/slicemap.h - CHOP's slice map (docs/CHOP.md): up to 64 slices of one sample with their own settings, the
// map's file format (an SFZ the INSTRUMENTS pool takes as an instrument, CHOP's own opcodes beside the standard ones),
// the edits (add / delete / move / merge / split, GRID, TRANSIENT, LAZY), the zero-crossing snap, and the keyed choices
// of the weird tools (SHUFFLE's order, NEIGHBOUR CHANCE's pick). Pure: no card, no globals, no allocation - any core,
// the tests, the format registry (core/fmt) and its fuzz target. Pi and Mac (CORE_MSAMP).
//
// The file (format "slicemap", core/fmt/formats.cpp):
//   v2  the first line "// sidecar slicemap 2 [min=N]", then
//         <control> default_path=           (nothing: sample paths are the card's own, absolute)
//         <global> sc_sha=<64 hex>|- sc_size=<bytes> sc_frames=<n> sc_rate=<hz> sc_ch=<1|2> sc_name=<name>
//         <region> key=<36 + k> offset=<a> end=<b - 1> ... sample=<path>     one a slice, in slice order
//       per region the standard SFZ opcodes other players read (offset, end, transpose, tune, volume, direction,
//       ampeg_attack, ampeg_decay, loop_mode, loop_type, group, off_by) and CHOP's own (sc_nudge sc_stut sc_stutlen
//       sc_stutdec sc_tape sc_tapems sc_nbr); `sample` last on the line (a path may hold spaces). A slice RESAMPLE
//       replaced plays its own sample (its region's sample= another file, offset 0).
//   v1  the sampler's slice maps (core/samp: <name>.sfz / <name>-slices.sfz, no header): <region> sample= key= offset=
//       end= [loop_mode=one_shot]. Upgraded in memory to v2 (default settings) by fmt::load's chain (upgrade_v1).
// Words: a slice is [start, end) in the sample's frames (the file's `end` is inclusive: end - 1). Slices are kept in
// start order and never overlap (an edit that would is clamped); gaps are allowed (a deleted slice leaves one).
#pragma once
#include <stdint.h>

namespace core {
namespace chop {

constexpr int kMaxSlices = 64;
constexpr int kMaxAlts = 7;          // slices RESAMPLE replaced with a sample of their own, a map at most
constexpr int kPathLen = 160;        // lib::kPath
constexpr int kNameLen = 40;
constexpr uint32_t kSnapFrames = 96; // the zero-crossing snap's reach: 2 ms at 48 kHz either way (scaled by the rate)
constexpr uint32_t kMinSlice = 64;   // a slice's frames at least (an edit never makes a shorter one)
constexpr int kKeyBase = 36;         // slice k is key 36 + k in the file (C1 up: the sampler's convention)
constexpr uint16_t kVersion = 2;     // the format's current version

enum Loop : uint8_t { kLoopOff = 0, kLoopFwd, kLoopPing, kLoops };
enum Tape : uint8_t { kTapeOff = 0, kTapeStop, kTapeSpin, kTapes };

// One slice's settings (non-destructive: what plays, never the sample's bytes). Ranges as the editor clamps them.
struct Slice {
  uint32_t start, end;     // frames [start, end) of its sample
  int8_t pitch;            // semitones -24..24
  int8_t fine;             // cents -50..50
  int8_t gain;             // dB -24..12
  uint8_t rev;             // 1: plays backwards
  uint16_t atk_ms;         // 0..2000 (0: 0.5 ms, click-free)
  uint16_t dec_ms;         // 0 hold to the end; else the level falls 60 dB over it
  uint8_t loop;            // Loop (while the note is held)
  uint8_t choke;           // 0 none, 1..4: slices of a group cut each other (the voice's)
  int16_t nudge;           // START NUDGE, frames -2400..2400 (the start moved, the end kept)
  uint8_t stut;            // STUTTER: 1 off, 2..16 the first part played that many times
  uint8_t stut_len;        // the part's length, % of the slice 1..100
  uint8_t stut_dec;        // each repeat's level drop, % 0..90
  uint8_t tape;            // Tape: STOP the pitch falls to zero over the end, SPIN rises from zero at the start
  uint16_t tape_ms;        // 20..4000
  uint8_t nbr;             // NEIGHBOUR CHANCE %: that share of hits plays the previous or the next slice instead
  int8_t alt;              // -1 the map's sample; 0..kMaxAlts-1: alts[alt] (a RESAMPLE'd slice)
};
void slice_defaults(Slice* s, uint32_t start, uint32_t end);

struct Map {
  uint32_t frames;         // the sample's (0 unknown: a map read before its sample)
  uint32_t rate;
  uint8_t ch;
  bool has_sha;
  uint8_t sha[32];         // the sample's content (lib::Ref): what the map is found by
  uint64_t size;           // its bytes
  char sample[kPathLen];   // its path when the map was written (a hint; the sha finds it)
  char name[kNameLen];
  int n;
  Slice s[kMaxSlices];
  int nalt;
  char alt[kMaxAlts][kPathLen];   // RESAMPLE'd slices' own samples (absolute paths)
};
void map_clear(Map* m);
// a slice's frames as it plays (NUDGE applied, clamped inside the sample: at least kMinSlice)
void slice_span(const Map& m, int k, uint32_t* a, uint32_t* b);

// ---- the file
// v2's text of m into out (at most cap - 1 bytes and a NUL): its length, 0 when it does not fit
int write(const Map& m, char* out, int cap);
// v2 (or v1) text -> m; why gets the reason on false. dir: the file's folder (v1's relative sample paths), "" root.
bool read(const char* text, uint32_t n, Map* m, char* why, int cap, const char* dir = "");
// the version the text says (1: no header, an SFZ with regions; 0: not a slice map at all)
int version_of(const char* text, uint32_t n, uint16_t* min_reader = nullptr, uint32_t* body = nullptr);
// fmt's chain v1 -> v2, pure on bytes: false when the v1 text has no slice (a region with offset / end)
bool upgrade_v1(const char* text, uint32_t n, char* out, uint32_t cap, uint32_t* out_n);

// ---- edits (each keeps the order and the no-overlap rule; false: refused, m unchanged)
// the zero crossing (mono sum's sign change) nearest `at` within `reach` frames, else at itself; pcm ch 1 / 2 interleaved
uint32_t snap(const int16_t* pcm, int ch, uint32_t frames, uint32_t at, uint32_t reach = kSnapFrames);
int slice_at(const Map& m, uint32_t frame);          // the slice holding frame, -1 none (a gap)
bool add_marker(Map* m, uint32_t frame);             // a slice starts here: the slice around it split there, or a new
                                                     // one from here to the next slice's start (a gap) / the end
bool del(Map* m, int k);                             // slice k gone (a gap where it was)
bool merge(Map* m, int k);                           // k and k + 1 become one (k's settings)
bool split(Map* m, int k, uint32_t at);              // k split at `at` (the new right half takes k's settings)
bool move_start(Map* m, int k, int32_t frame);       // k's start; a slice touching it on the left follows
bool move_end(Map* m, int k, int32_t frame);         // k's end; a slice touching it on the right follows
void grid(Map* m, int n, const int16_t* pcm, int ch); // n equal slices over the sample (2..64), starts snapped
// Onsets (TRANSIENT): the first difference's energy (an attack's highs) in hops of 256 frames; a hop over thr x the
// mean of the 8 before (and over the floor) starts a slice, refined to the hop's first frame over a quarter of its peak and snapped back to a zero
// crossing; at least 50 ms apart. sens 0..127 (127: the most slices): thr 8.0 .. 1.4. Fed in pieces (a control or card
// core's step at a time): begin, feed until the end, then the starts (the first at 0).
struct Onsets {
  float thr, floor_e, prev;
  float hist[8];
  int nh;
  uint32_t pos;            // frames fed
  uint32_t last;           // the last onset (frames)
  int n;
  uint32_t at[kMaxSlices];
};
void onsets_begin(Onsets* o, int sens);
void onsets_feed(Onsets* o, const int16_t* pcm, int ch, uint32_t frames, uint32_t upto);   // upto: feed to here
void onsets_apply(const Onsets& o, Map* m, const int16_t* pcm, int ch);   // the slices from the starts found

// ---- LAZY (MPC Live's lazy chop): the sample plays from its start; each tap drops a marker where the sample was
// when the player heard it - the playhead at the tap less the latency (the frames from the sound leaving the engine to
// the tap reaching it: the output FIFO and the device's audio path, plus the key's way back), snapped to a zero
// crossing; UNDO takes the last one back; taps past kMaxSlices are refused (counted). STOP ends: the markers become
// the slices (each to the next, the last to the end; nothing before the first).
struct Lazy {
  bool on;
  int n;
  uint32_t at[kMaxSlices];
  uint32_t refused;
};
void lazy_begin(Lazy* l);
// a tap at playhead `played` (frames of the sample played when the tap arrived), latency `lat` frames: the marker's
// frame, or -1 refused (full, or before the start)
int32_t lazy_tap(Lazy* l, uint32_t played, uint32_t lat, const int16_t* pcm, int ch, uint32_t frames);
bool lazy_undo(Lazy* l);
void lazy_apply(const Lazy& l, Map* m);   // the markers as the map's slices (settings reset); none: m unchanged

// ---- the keyed choices (core/seq/rng.h's hash: the same inputs, the same result, on any core)
// SHUFFLE: order[k] = the slice pad / note k plays, a permutation of 0..n-1 from seed (0: the map's own order); the
// same seed gives the same order on every voice and in every build (rng.h's purposes are never renumbered)
void shuffle(uint32_t seed, int n, uint8_t* order);
// NEIGHBOUR CHANCE: hit number `hit` of slice k (n slices) plays k, k - 1 or k + 1: chance_pct % of hits move, half
// each way (the ends move inward)
int neighbour(uint32_t seed, int voice, int k, int n, uint32_t hit, int chance_pct);

// text helpers the editor shares (ms as "12.5MS" / "1.20S"; frames at a rate as ms)
const char* loop_name(int l);   // "OFF" "FWD" "PING"
const char* tape_name(int t);   // "OFF" "STOP" "SPIN"

}  // namespace chop
}  // namespace core
