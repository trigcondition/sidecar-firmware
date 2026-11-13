// core/seq/lockfill.cpp - see lockfill.h. Pure; cold.
#include "seq/lockfill.h"

#include <string.h>

#include "platform.h"
#include "seq/rng.h"

namespace core {
namespace lockfill {

namespace {
uint8_t* slot_at(uint8_t* rec, int i) { return rec + kSlotsAt + i * kSlotBytes; }
const uint8_t* slot_at(const uint8_t* rec, int i) { return rec + kSlotsAt + i * kSlotBytes; }
const uint8_t* track_at(const uint8_t* rec, int t) { return rec + 4 + t * kTrackBytes; }
uint8_t* track_at(uint8_t* rec, int t) { return rec + 4 + t * kTrackBytes; }
bool is_fine(const uint8_t* sl) { return sl[0] == kFineTag && sl[1] == kFineTag; }
void free_slot(uint8_t* rec, int i) {   // as the Rytm leaves a free slot: FF FF, the values 00 [measured, rytm-emu]
  uint8_t* sl = slot_at(rec, i);
  sl[0] = sl[1] = 0xFF;
  memset(sl + 2, 0, kSlotBytes - 2);
}
// the first run of n free slots, -1 none
int free_run(const uint8_t* rec, int n) {
  for (int i = 0; i + n <= kSlots; ++i) {
    int k = 0;
    while (k < n && slot_free(rec, i + k)) ++k;
    if (k == n) return i;
  }
  return -1;
}
int free_count(const uint8_t* rec) { return kSlots - slots_used(rec); }
// a free slot at position pos, the used slots from pos to the first free one after it moved one down (pairs stay
// together: a contiguous run moves as a whole). Compacts first when no free slot follows pos. *pos follows the
// compaction. false: none free at all.
bool open_at(uint8_t* rec, int* pos) {
  int f = -1;
  for (int i = *pos; i < kSlots && f < 0; ++i)
    if (slot_free(rec, i)) f = i;
  if (f < 0) {
    if (!free_count(rec)) return false;
    // the slot at *pos - 1 (the one we insert after) keeps its rank among the used slots
    int rank = 0;
    for (int i = 0; i < *pos; ++i) rank += !slot_free(rec, i);
    compact(rec);
    *pos = rank;
    f = kSlots - free_count(rec);
  }
  if (f > *pos) memmove(slot_at(rec, *pos + 1), slot_at(rec, *pos), (size_t)(f - *pos) * kSlotBytes);
  free_slot(rec, *pos);
  return true;
}
}  // namespace

CORE_COLD const char* order_name(int o) {
  static const char* const k[kOrders] = {"UP", "DOWN", "PING", "RND", "SHUF", "SPRD"};
  return o >= 0 && o < kOrders ? k[o] : "?";
}
CORE_COLD const char* trigs_name(int t) {
  static const char* const k[kTrigsModes] = {"ALL", "EMPTY", "LOCKED"};
  return t >= 0 && t < kTrigsModes ? k[t] : "?";
}
CORE_COLD const char* result_text(int r) {
  switch (r) {
    case kOk: return "OK";
    case kNoTrigs: return "NO TRIGS TO FILL";
    case kNoValues: return "NO VALUES IN RANGE";
    case kFull: return "LOCKS FULL (72)";
    case kBadTrack: return "NO SUCH TRACK";
    case kNothing: return "NOTHING TO UNDO";
    case kChanged: return "CHANGED SINCE: NO UNDO";
    default: return "FAILED";
  }
}

CORE_COLD bool trig_on(const uint8_t* rec, int t, int s) {
  const uint8_t* b = track_at(rec, t);
  int bit = s * 14 + 13;   // the 14 trig bits MSB first: bit 0x0001 is the step's last [pi/ui/rytm_pattern.c get14]
  return (b[bit >> 3] >> (7 - (bit & 7))) & 1;
}
CORE_COLD int track_len(const uint8_t* rec, int t) {
  const uint8_t* tail = rec + kTailAt;
  int m = tail[kTailLen] << 8 | tail[kTailLen + 1];
  if (tail[kTailMode] == kModeNormal && m >= 1) return m > kSteps ? kSteps : m;   // NORMAL: the master length rules
  int n = track_at(rec, t)[kLenAt];
  return n >= 1 && n <= kSteps ? n : kSteps;
}
CORE_COLD bool slot_free(const uint8_t* rec, int i) { return slot_at(rec, i)[0] == 0xFF; }
CORE_COLD int slots_used(const uint8_t* rec) {
  int n = 0;
  for (int i = 0; i < kSlots; ++i) n += !slot_free(rec, i);
  return n;
}
CORE_COLD int find_slot(const uint8_t* rec, int type, int t) {
  for (int i = 0; i < kSlots; ++i) {
    const uint8_t* sl = slot_at(rec, i);
    if (sl[0] == type && sl[1] == t && !is_fine(sl)) return i;
  }
  return -1;
}
CORE_COLD bool has_fine(const uint8_t* rec, int i) { return i >= 0 && i + 1 < kSlots && is_fine(slot_at(rec, i + 1)); }

namespace {
// every lock of a in b (one way: an orphan fine half in a is not looked at - same() runs it both ways)
bool same_one(const uint8_t* a, const uint8_t* b) {
  if (memcmp(a, b, kSlotsAt) || memcmp(a + kSlotsAt + kSlots * kSlotBytes, b + kSlotsAt + kSlots * kSlotBytes,
                                       kRecord - kSlotsAt - kSlots * kSlotBytes))
    return false;
  if (slots_used(a) != slots_used(b)) return false;
  for (int i = 0; i < kSlots; ++i) {
    const uint8_t* sa = slot_at(a, i);
    if (slot_free(a, i) || is_fine(sa)) continue;
    int j = find_slot(b, sa[0], sa[1]);
    if (j < 0 || memcmp(sa + 2, slot_at(b, j) + 2, kSteps)) return false;
    bool fa = has_fine(a, i), fb = has_fine(b, j);
    if (fa != fb || (fa && memcmp(slot_at(a, i + 1) + 2, slot_at(b, j + 1) + 2, kSteps))) return false;
  }
  return true;
}
}  // namespace
CORE_COLD bool same(const uint8_t* a, const uint8_t* b) { return same_one(a, b) && same_one(b, a); }

CORE_COLD void compact(uint8_t* rec) {
  int w = 0;
  for (int i = 0; i < kSlots; ++i) {
    if (slot_free(rec, i)) continue;
    if (w != i) memcpy(slot_at(rec, w), slot_at(rec, i), kSlotBytes);
    ++w;
  }
  for (; w < kSlots; ++w) free_slot(rec, w);
}

CORE_COLD uint16_t get(const uint8_t* rec, int t, const Target& g, int s) {
  if (t < 0 || t >= kTracks || s < 0 || s >= kSteps) return kNoValue;
  if (g.where == kVelocity) return track_at(rec, t)[kVelAt + s];
  int i = find_slot(rec, g.type, t);
  if (i < 0) return kNoValue;
  uint8_t c = slot_at(rec, i)[2 + s];
  if (c == kNoValue) return kNoValue;
  uint8_t f = has_fine(rec, i) ? slot_at(rec, i + 1)[2 + s] : 0;
  return value(c, f == kNoValue ? 0 : f);
}
CORE_COLD bool has_value(const uint8_t* rec, int t, const Target& g, int s) { return (get(rec, t, g, s) & 0xFF) != kNoValue; }

CORE_COLD int steps_for(const uint8_t* rec, int t, const Target& g, const Options& o, uint8_t out[kSteps]) {
  if (t < 0 || t >= kTracks) return 0;
  int every = o.every < 1 ? 1 : o.every > 16 ? 16 : o.every, off = o.offset % every, n = 0, k = 0;
  for (int s = 0, len = track_len(rec, t); s < len; ++s) {
    if (!trig_on(rec, t, s)) continue;
    bool v = has_value(rec, t, g, s);
    if ((o.trigs == kEmpty && v) || (o.trigs == kLocked && !v)) continue;
    if (k++ % every == off) out[n++] = (uint8_t)s;
  }
  return n;
}

CORE_COLD void sequence(const uint16_t* set, int nv, int n, const Options& o, uint16_t* out) {
  if (nv <= 0) return;
  uint8_t perm[256];
  int np = nv < 256 ? nv : 256, cycle = -1;
  for (int i = 0; i < n; ++i) {
    int idx = 0;
    switch (o.order) {
      case kDown: idx = nv - 1 - i % nv; break;
      case kPing: {   // up and down, the ends once: 0 1 2 1 0 1 ..
        int p = nv > 1 ? 2 * nv - 2 : 1, q = i % p;
        idx = q < nv ? q : p - q;
        break;
      }
      case kRnd: idx = seq::rng_below(seq::rng_key(o.seed, 0, i, -1, seq::kRngFill, 0), nv); break;
      case kShuf: {   // a new permutation each cycle (Fisher-Yates from the seed and the cycle): each value once first
        if (i / np != cycle) {
          cycle = i / np;
          for (int k = 0; k < np; ++k) perm[k] = (uint8_t)k;
          for (int k = np - 1; k > 0; --k) {
            int j = seq::rng_below(seq::rng_key(o.seed, 1, k, cycle, seq::kRngFill, 1), k + 1);
            uint8_t x = perm[k];
            perm[k] = perm[j], perm[j] = x;
          }
        }
        idx = perm[i % np];
        break;
      }
      case kSprd: idx = n > 1 ? (i * (nv - 1) + (n - 1) / 2) / (n - 1) : 0; break;   // FROM on the first, TO on the last
      default: idx = i % nv; break;
    }
    out[i] = set[idx];
  }
}

CORE_COLD int slots_after(const uint8_t* rec, int t, const Target& g) {
  int u = slots_used(rec);
  if (g.where != kLock) return u;
  int i = find_slot(rec, g.type, t);
  if (i < 0) return u + (g.fine ? 2 : 1);
  return u + (g.fine && !has_fine(rec, i) ? 1 : 0);
}

CORE_COLD int fill(uint8_t* rec, int t, const Target& g, const uint8_t* steps, const uint16_t* vals, int n) {
  if (t < 0 || t >= kTracks) return kBadTrack;
  if (g.where == kVelocity) {
    uint8_t* v = track_at(rec, t) + kVelAt;
    for (int i = 0; i < n; ++i) {
      int c = coarse_of(vals[i]);
      v[steps[i] & 63] = (uint8_t)(c < 1 ? 1 : c);
    }
    return kOk;
  }
  int i = find_slot(rec, g.type, t);
  if (i < 0) {   // a new slot (pair): free slots in a run, else the list compacted
    int need = g.fine ? 2 : 1;
    if (free_count(rec) < need) return kFull;
    i = free_run(rec, need);
    if (i < 0) compact(rec), i = free_run(rec, need);
    if (i < 0) return kFull;
    uint8_t* sl = slot_at(rec, i);
    memset(sl, 0xFF, kSlotBytes);
    sl[0] = g.type, sl[1] = (uint8_t)t;
    if (g.fine) {
      uint8_t* f = slot_at(rec, i + 1);
      memset(f, 0xFF, kSlotBytes);
      f[0] = f[1] = kFineTag;
    }
  } else if (g.fine && !has_fine(rec, i)) {   // a 7-bit lock of this word already: its fine half added after it
    if (!free_count(rec)) return kFull;
    int pos = i + 1;
    if (!open_at(rec, &pos)) return kFull;
    i = pos - 1;
    uint8_t* f = slot_at(rec, pos);
    f[0] = f[1] = kFineTag;
    for (int s = 0; s < kSteps; ++s) f[2 + s] = slot_at(rec, i)[2 + s] == kNoValue ? kNoValue : 0;   // bank A, as it played
  }
  uint8_t* sl = slot_at(rec, i);
  bool fi = has_fine(rec, i);
  for (int k = 0; k < n; ++k) {
    int s = steps[k] & 63;
    sl[2 + s] = (uint8_t)coarse_of(vals[k]);
    if (fi) slot_at(rec, i + 1)[2 + s] = (uint8_t)(g.fine ? fine_of(vals[k]) : 0);
  }
  return kOk;
}

CORE_COLD int clear(uint8_t* rec, int t, const Target& g, const uint8_t* steps, int n) {
  if (t < 0 || t >= kTracks) return kBadTrack;
  if (g.where == kVelocity) {
    for (int k = 0; k < n; ++k) track_at(rec, t)[kVelAt + (steps[k] & 63)] = kNoValue;
    return kOk;
  }
  int i = find_slot(rec, g.type, t);
  if (i < 0) return kOk;
  bool fi = has_fine(rec, i);
  uint8_t* sl = slot_at(rec, i);
  for (int k = 0; k < n; ++k) {
    int s = steps[k] & 63;
    sl[2 + s] = kNoValue;
    if (fi) slot_at(rec, i + 1)[2 + s] = kNoValue;
  }
  for (int s = 0; s < kSteps; ++s)
    if (sl[2 + s] != kNoValue) return kOk;
  free_slot(rec, i);
  if (fi) free_slot(rec, i + 1);
  return kOk;
}

CORE_COLD void save(const uint8_t* rec, int t, const Target& g, Saved* out) {
  *out = Saved();
  out->valid = t >= 0 && t < kTracks;
  if (!out->valid) return;
  out->track = (uint8_t)t, out->g = g;
  if (g.where == kVelocity) {
    memcpy(out->vel, track_at(rec, t) + kVelAt, kSteps);
    return;
  }
  int i = find_slot(rec, g.type, t);
  out->had = i >= 0;
  memset(out->coarse, kNoValue, kSteps), memset(out->finev, kNoValue, kSteps);
  if (i < 0) return;
  memcpy(out->coarse, slot_at(rec, i) + 2, kSteps);
  out->fine = has_fine(rec, i);
  if (out->fine) memcpy(out->finev, slot_at(rec, i + 1) + 2, kSteps);
}

CORE_COLD bool equal(const Saved& a, const Saved& b) {
  if (a.valid != b.valid || a.track != b.track || a.g.where != b.g.where || a.g.type != b.g.type) return false;
  if (a.g.where == kVelocity) return !memcmp(a.vel, b.vel, kSteps);
  return a.had == b.had && a.fine == b.fine && !memcmp(a.coarse, b.coarse, kSteps) && !memcmp(a.finev, b.finev, kSteps);
}

CORE_COLD int restore(uint8_t* rec, const Saved& sv) {
  if (!sv.valid) return kNothing;
  int t = sv.track;
  if (sv.g.where == kVelocity) {
    memcpy(track_at(rec, t) + kVelAt, sv.vel, kSteps);
    return kOk;
  }
  int i = find_slot(rec, sv.g.type, t);
  if (!sv.had) {   // there was none: whatever is there now goes (the fill made it)
    if (i >= 0) {
      bool fi = has_fine(rec, i);
      free_slot(rec, i);
      if (fi) free_slot(rec, i + 1);
    }
    return kOk;
  }
  if (i < 0) {   // gone since (cleared): back as a new slot (pair)
    Target g = sv.g;
    g.fine = sv.fine;
    uint8_t steps[1] = {0};
    uint16_t v[1] = {value(0)};
    int r = fill(rec, t, g, steps, v, 0);   // the slot (pair) made, no step written
    if (r != kOk) return r;
    i = find_slot(rec, sv.g.type, t);
    if (i < 0) return kFull;
  } else if (sv.fine && !has_fine(rec, i)) {
    int pos = i + 1;
    if (!open_at(rec, &pos)) return kFull;
    i = pos - 1;
    uint8_t* f = slot_at(rec, pos);
    f[0] = f[1] = kFineTag;
  } else if (!sv.fine && has_fine(rec, i)) {   // the fill added a fine half: it goes (a free slot where it was)
    free_slot(rec, i + 1);
  }
  memcpy(slot_at(rec, i) + 2, sv.coarse, kSteps);
  if (sv.fine) memcpy(slot_at(rec, i + 1) + 2, sv.finev, kSteps);
  return kOk;
}

}  // namespace lockfill
}  // namespace core
