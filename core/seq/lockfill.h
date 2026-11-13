// core/seq/lockfill.h - FILL (0.5.0, the owner's "p-lock all the trigs of that track with those samples, in order or
// at random", notes/design sampler-lock-fill): the pure part. It works on the Rytm's pattern record as a SysEx pattern
// dump carries it (13101 B unpacked: 13 tracks x 641 B at +4, 72 lock slots of 66 B at +0x2091, a 12-byte tail;
// docs/POLYPHONY.md, pi/ui/rytm_pattern.h) and changes ONLY what one fill target of one track owns:
//   a lock target   the (lock type, track) slot of the pattern's lock list and, for a banked value (a pool row), the
//                   fine slot {0x80 0x80 ..} right after it [measured, rytm-emu: SMP = type 10 + a fine slot, coarse =
//                   the row's index, fine = bank << 4; 0xFF = no lock on that step]. Lock types 0..15 are a Sidecar
//                   track's P1..P16 (sound words 9..24).
//   a step target   a per-step array of the track's record (0.5.0: VELOCITY at +0x0b0, 1..127, 0xFF the default).
// Nothing is decoded and encoded again (pi/ui rytm_pattern_encode is lossy by design): every other byte of the record
// stays as it was. A new slot pair takes two adjacent free slots; when the free slots are not adjacent the list is
// compacted (the used 66-byte slots moved up, in order: still nothing else touched). The platform's pattern writer
// (core/seq/patwrite.h) reads, patches with this, writes and verifies. Pure: any core, the tests.
#pragma once
#include <stdint.h>

namespace core {
namespace lockfill {

constexpr int kRecord = 13101;
constexpr int kTracks = 12;          // the voice tracks (record 12 is the FX track: never filled)
constexpr int kTrackBytes = 641;
constexpr int kSteps = 64;
constexpr int kSlots = 72;
constexpr int kSlotBytes = 66;
constexpr int kSlotsAt = 0x2091;     // 4 + 13 x 641
constexpr int kVelAt = 0x0b0;        // a track record's velocity array [measured]
constexpr int kLenAt = 0x235;        // the track's length, 1..64 [measured, rytm-emu 0.5.0: 16 / 64 / 18 as set; +0x276 is
                                     // the last byte of the 64 sound-lock bytes at +0x237]
// the pattern's 12-byte tail [measured, rytm-emu fix round: 0026's SCALE report (2D 04 7F) read what was written here;
// public libanalogrytm agrees]: +0 master length (big-endian), +2 master change, +4 kit, +5 swing, +6 mode (0 NORMAL,
// 1 ADVANCED: per-track lengths), +7 master speed, +8 quantize, +9 BPM x 120 (big-endian)
constexpr int kTailAt = kRecord - 12, kTailLen = 0, kTailMode = 6;
constexpr uint8_t kModeNormal = 0;
constexpr uint8_t kNoValue = 0xFF;   // a step without a lock / at the track's default
constexpr uint8_t kFineTag = 0x80;   // a fine slot's type and track bytes

enum Where : uint8_t { kLock = 0, kVelocity = 1 };
// What a fill writes: a lock slot (type 0..15 = P1..P16; fine: a banked value with its fine slot) or a step array
struct Target {
  uint8_t where;   // Where
  uint8_t type;    // kLock: the lock type
  bool fine;       // kLock: the value has a fine half (bank << 4)
};
// A value as it is stored: the coarse byte (a lock's 0..127, a velocity 1..127) and the fine byte (bank << 4)
inline uint16_t value(int coarse, int fine = 0) { return (uint16_t)((coarse & 0x7F) | (fine & 0x7F) << 8); }
inline int coarse_of(uint16_t v) { return v & 0x7F; }
inline int fine_of(uint16_t v) { return (v >> 8) & 0x7F; }

enum Order : uint8_t { kUp = 0, kDown, kPing, kRnd, kShuf, kSprd, kOrders };
enum Trigs : uint8_t { kAll = 0, kEmpty, kLocked, kTrigsModes };
const char* order_name(int o);   // "UP" "DOWN" "PING" "RND" "SHUF" "SPRD"
const char* trigs_name(int t);   // "ALL" "EMPTY" "LOCKED"
struct Options {
  uint8_t order = kUp;
  uint8_t trigs = kAll;
  uint8_t every = 1;     // 1..16, counted over the trigs the TRIGS filter leaves
  uint8_t offset = 0;    // 0..every - 1
  uint16_t seed = 0;     // 0..999 (RND, SHUF): the same seed, the same result
};

enum Result : int { kOk = 0, kNoTrigs = -1, kNoValues = -2, kFull = -3, kBadTrack = -4, kNothing = -5,
                   kChanged = -6 };   // (the undo: the target changed since the fill: CHANGED SINCE: NO UNDO)
const char* result_text(int r);   // "LOCKS FULL (72)" ...

// ---- reading the record
bool trig_on(const uint8_t* rec, int t, int s);   // step s of track t has a trig (its trig bit 0x0001)
int track_len(const uint8_t* rec, int t);         // 1..64: NORMAL mode the master length (at most 64), else the
                                                  // track's (64 when the byte is not a length)
int find_slot(const uint8_t* rec, int type, int t);   // the slot of (type, track t), -1 none
bool has_fine(const uint8_t* rec, int slot);       // the slot after it is its fine half
int slots_used(const uint8_t* rec);                // slots not free (fine halves count: 0..72)
// The target's stored value on step s (kNoValue in the low byte: none). A lock without a fine half reads fine 0.
uint16_t get(const uint8_t* rec, int t, const Target& g, int s);
bool has_value(const uint8_t* rec, int t, const Target& g, int s);

// ---- what a fill does
// The steps the fill writes, in order (at most 64): track t's trigs within its length (trigless steps and steps past the
// length are left alone), those the TRIGS filter keeps (EMPTY: no value of the target yet, LOCKED: one), then every
// EVERY-th of those from OFFSET. Returns how many.
int steps_for(const uint8_t* rec, int t, const Target& g, const Options& o, uint8_t out[kSteps]);
// n values out of the value set set[0..nv-1] (FROM .. TO, as the page lists them) in the order o (seeded)
void sequence(const uint16_t* set, int nv, int n, const Options& o, uint16_t* out);
// The slots a fill of track t's target would leave in use (the page's LOCK TRK 14/72): slots_used + what it adds
int slots_after(const uint8_t* rec, int t, const Target& g);
// Writes vals[i] on steps[i] (i < n). kOk, or kFull (72 slots: nothing changed), kBadTrack.
int fill(uint8_t* rec, int t, const Target& g, const uint8_t* steps, const uint16_t* vals, int n);
// The target's value off those steps (a lock: 0xFF in both halves; a step array: 0xFF, the track's default). A lock
// slot left with no value on any step is freed, its fine half too.
int clear(uint8_t* rec, int t, const Target& g, const uint8_t* steps, int n);

// ---- undo (one level a fill): the target's whole state on track t before the fill, put back later into the pattern
// as it is then (re-read: what was edited since, on this track's other parameters or other tracks, stays)
struct Saved {
  bool valid = false;
  uint8_t track = 0;
  Target g = {};
  bool had = false;            // kLock: the slot existed
  bool fine = false;           //        with a fine half
  uint8_t coarse[kSteps] = {};
  uint8_t finev[kSteps] = {};
  uint8_t vel[kSteps] = {};    // kVelocity
};
void save(const uint8_t* rec, int t, const Target& g, Saved* out);
int restore(uint8_t* rec, const Saved& s);   // kOk, or kFull (the slot must come back and there is no room)
bool equal(const Saved& a, const Saved& b);   // the same target with the same state

// ---- the verify: the Rytm keeps its lock list in its own order (a written list comes back re-sorted [measured,
// rytm-emu 0.5.0: a new slot pair after the other slots read back between them]; free slots' bytes past the first two
// are not kept either). Equal: every byte outside the slot list, and the same locks - each (type, track) slot with the
// same 64 values and the same fine half (or none) - in whatever order.
bool same(const uint8_t* a, const uint8_t* b);

// ---- slot list primitives (tests)
bool slot_free(const uint8_t* rec, int slot);
void compact(uint8_t* rec);   // used slots moved up in order, the free ones at the end

}  // namespace lockfill
}  // namespace core
