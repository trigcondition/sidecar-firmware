// core/seq/music.cpp - see seq.h "seqtop": scales and keys (M1). Pure: no card, no clock, no state; cold (edits, plan
// time per note).
#include "seq/seq.h"

#if CORE_POLY
#include <string.h>

#include "platform.h"
#include "poly.h"
#include "seq/rng.h"

namespace core {
namespace seq {

namespace {
// bit k: the semitone k above the key is in the scale. The first five are the touchscreen KEY tool's (CHR MAJ MIN DOR
// PENT, pi/ui/sidecar_seqed.c); the ids are the file's (never renumbered).
const uint16_t kMasks[kScales] = {0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x4A9, 0x5AB, 0xAD5,
                                  0x6B5, 0x56B, 0x9AD, 0xAAD, 0x295, 0x4E9, 0x555};
const char* const kNames[kScales] = {"CHR", "MAJ", "MIN",  "DOR",  "PENT", "PHR",   "LYD",
                                     "MIX", "LOC", "HMIN", "MMIN", "PMAJ", "BLUES", "WHOLE"};
const char* const kKeys[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
// a chord (poly.h's CHD) as scale degrees from its root: the diatonic twin of poly.cpp's semitone table
struct Deg {
  uint8_t n;
  int8_t d[5];
};
const Deg kDeg[poly::kChords] = {
    {1, {0}},          {3, {0, 2, 4}},    {3, {0, 2, 4}},       {3, {0, 1, 4}},       {3, {0, 3, 4}},       {4, {0, 2, 4, 6}},
    {4, {0, 2, 4, 6}}, {4, {0, 2, 4, 6}}, {3, {0, 2, 4}},       {3, {0, 2, 4}},       {4, {0, 2, 4, 5}},    {4, {0, 2, 4, 5}},
    {5, {0, 2, 4, 6, 8}}, {4, {0, 2, 4, 8}}, {5, {0, 2, 4, 6, 8}}, {2, {0, 4}},
};
CORE_COLD int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
CORE_COLD bool same_word(const char* a, const char* b) {
  while (*a && *b && lower(*a) == lower(*b)) ++a, ++b;
  return !*a && !*b;
}
CORE_COLD int clamp7(int p) { return p < 0 ? 0 : p > 127 ? 127 : p; }
}  // namespace

CORE_COLD const char* scale_name(int s) { return s >= 0 && s < kScales ? kNames[s] : "?"; }
CORE_COLD uint16_t scale_mask(int s) { return s > 0 && s < kScales ? kMasks[s] : 0xFFF; }
CORE_COLD int scale_parse(const char* s) {
  if (!s || !*s) return -1;
  if (*s >= '0' && *s <= '9') {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return !*s && v < kScales ? v : -1;
  }
  if (same_word(s, "off") || same_word(s, "chromatic")) return kScaleChr;
  for (int i = 0; i < kScales; ++i)
    if (same_word(s, kNames[i])) return i;
  return -1;
}
CORE_COLD const char* key_name(int k) { return kKeys[((k % 12) + 12) % 12]; }
CORE_COLD int key_parse(const char* s) {
  if (!s || !*s) return -1;
  if (*s >= '0' && *s <= '9') {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return !*s && v < 12 ? v : -1;
  }
  for (int i = 0; i < 12; ++i)
    if (same_word(s, kKeys[i])) return i;
  return -1;
}
CORE_COLD bool in_scale(int pitch, int key, int scale) {
  return scale_mask(scale) >> (((pitch - key) % 12 + 12) % 12) & 1;
}
CORE_COLD int quantise(int pitch, int key, int scale) {
  pitch = clamp7(pitch);
  if (in_scale(pitch, key, scale)) return pitch;
  for (int d = 1; d < 12; ++d) {   // the nearest in the scale; a tie goes down
    if (pitch - d >= 0 && in_scale(pitch - d, key, scale)) return pitch - d;
    if (pitch + d <= 127 && in_scale(pitch + d, key, scale)) return pitch + d;
  }
  return pitch;
}
CORE_COLD int degree_move(int pitch, int key, int scale, int d) {
  int p = quantise(pitch, key, scale);
  if (!scale_mask(scale) || scale_mask(scale) == 0xFFF) return clamp7(p + d);
  int step = d > 0 ? 1 : -1;
  for (int n = d > 0 ? d : -d; n > 0; --n) {
    int q = p + step;
    while (q >= 0 && q <= 127 && !in_scale(q, key, scale)) q += step;
    if (q < 0 || q > 127) break;
    p = q;
  }
  return p;
}
CORE_COLD int diatonic_chord(int root, int chd, int inv, int key, int scale, uint8_t* out) {
  if (chd < 0 || chd >= poly::kChords) chd = 0;
  if (!scale) {   // chromatic: poly.h's chord table, as before
    poly::Chord c = {chd, inv < 0 ? 0 : inv % poly::kInvs, 0, 0};
    return poly::expand(root, c, out);
  }
  const Deg& g = kDeg[chd];
  int t[5], n = 0;
  int r = quantise(root, key, scale);
  for (int i = 0; i < g.n; ++i) t[n++] = degree_move(r, key, scale, g.d[i]);
  int k = inv < 0 ? 0 : inv % poly::kInvs % n;   // the lowest k notes an octave up
  for (int i = 0; i < k; ++i) t[i] += 12;
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && t[j - 1] > t[j]; --j) {
      int x = t[j];
      t[j] = t[j - 1], t[j - 1] = x;
    }
  int m = 0;
  for (int i = 0; i < n; ++i)
    if (t[i] >= 0 && t[i] <= 127 && (m == 0 || out[m - 1] != t[i])) out[m++] = (uint8_t)t[i];
  return m;
}
CORE_COLD void set_chord_in(Step& s, int root, int chd, int inv, int vel, int len, int strum, int key, int scale) {
  if (!scale) {
    set_chord(s, root, chd, inv, vel, len, strum);
    return;
  }
  uint8_t ns[8];
  int k = diatonic_chord(root, chd, inv, key, scale, ns);
  s.n = 0;
  for (int i = 0; i < k && i < kNotes; ++i) s.note[s.n++] = {ns[i], (uint8_t)vel, (uint16_t)(len < 1 ? 1 : len), 0, 0};
  s.chd = (uint8_t)(chd < 0 || chd >= poly::kChords ? 0 : chd), s.inv = (uint8_t)(inv < 0 ? 0 : inv % poly::kInvs);
  s.flags |= 1;
  if (!s.prob) s.prob = 100;
  apply_strum(s, strum);
}

// ---- P4 / M4: ratchets, rolls and the per-step arp
namespace {
const uint8_t kRateNum[kRtRates] = {0, 2, 1, 2, 1, 1, 1, 1, 1}, kRateDen[kRtRates] = {0, 1, 1, 3, 2, 3, 4, 6, 8};
}  // namespace
CORE_COLD const char* rt_mode_name(int m) {
  static const char* const k[kRtModes] = {"CHORD", "WALK", "UP", "DOWN", "UPDN", "RND", "PLAYED", "CONV"};
  return m >= 0 && m < kRtModes ? k[m] : "?";
}
CORE_COLD const char* rt_rate_name(int r) {
  static const char* const k[kRtRates] = {"AUTO", "2", "1", "2/3", "1/2", "1/3", "1/4", "1/6", "1/8"};
  return r >= 0 && r < kRtRates ? k[r] : "?";
}
CORE_COLD const char* vcrv_name(int v) {
  static const char* const k[kVcs] = {"FLAT", "UP", "DOWN", "UPDN"};
  return v >= 0 && v < kVcs ? k[v] : "?";
}
CORE_COLD int rt_expand(const Step& s, const uint8_t* pitch, int key, int scale, bool degrees, uint32_t seed, int track,
                        int idx, int64_t loop, RtNote* out, int cap) {
  const Rtrg& r = s.rt;
  int n = s.n < kNotes ? s.n : kNotes;
  if (!n || cap <= 0) return 0;
  int span = 1, lo = 0;
  for (int k = 0; k < n; ++k) {
    span = s.note[k].len > span ? s.note[k].len : span;
    lo = s.note[k].micro < lo ? s.note[k].micro : lo;
  }
  bool arp = r.mode >= kRtUp && r.mode < kRtModes;
  // the arp's notes: the chord ascending over OCT octaves (PLAYED: as entered), then the mode's order
  int list[kNotes * 4], src[kNotes * 4], nl = 0;
  int oct = r.oct < 1 ? 1 : r.oct > 4 ? 4 : r.oct;
  int ord[kNotes];
  for (int k = 0; k < n; ++k) ord[k] = k;
  if (r.mode != kRtPlayed)
    for (int i = 1; i < n; ++i)
      for (int j = i; j > 0 && pitch[ord[j - 1]] > pitch[ord[j]]; --j) {
        int x = ord[j];
        ord[j] = ord[j - 1], ord[j - 1] = x;
      }
  if (arp)
    for (int o = 0; o < oct; ++o)
      for (int k = 0; k < n; ++k)
        if (pitch[ord[k]] + 12 * o <= 127) list[nl] = pitch[ord[k]] + 12 * o, src[nl++] = ord[k];
  if (arp && nl) {
    int t[kNotes * 4], ts[kNotes * 4], m = 0;
    if (r.mode == kRtDown) {
      for (int i = nl - 1; i >= 0; --i) t[m] = list[i], ts[m++] = src[i];
    } else if (r.mode == kRtUpDn) {
      for (int i = 0; i < nl; ++i) t[m] = list[i], ts[m++] = src[i];
      for (int i = nl - 2; i >= 1; --i) t[m] = list[i], ts[m++] = src[i];
    } else if (r.mode == kRtConv) {   // from the outside in: lowest, highest, 2nd lowest, 2nd highest ..
      for (int a = 0, b = nl - 1; a <= b; ++a, --b) {
        t[m] = list[a], ts[m++] = src[a];
        if (b != a) t[m] = list[b], ts[m++] = src[b];
      }
    } else {
      for (int i = 0; i < nl; ++i) t[m] = list[i], ts[m++] = src[i];
    }
    for (int i = 0; i < m; ++i) list[i] = t[i], src[i] = ts[i];
    nl = m;
  }
  int count = r.cnt >= 2 ? r.cnt : arp ? nl : 1;
  if (count > kRtMax) count = kRtMax;
  if (count < 1) count = 1;
  int rate = r.rate < kRtRates ? r.rate : 0;
  int iv = rate ? kSub * kRateNum[rate] / kRateDen[rate] : span / count;
  if (iv < 1) iv = 1;
  int gate = r.gate >= 1 && r.gate <= 100 ? r.gate : 100;
  int w = 0;
  for (int i = 0; i < count && w < cap; ++i) {
    float f = 1.0f;
    float x = count > 1 ? (float)i / (float)(count - 1) : 0.0f;
    if (r.vcrv == kVcUp) f = 0.4f + 0.6f * x;
    else if (r.vcrv == kVcDown) f = 1.0f - 0.6f * x;
    else if (r.vcrv == kVcUpDn) f = 0.4f + 0.6f * (1.0f - (x * 2 - 1 < 0 ? 1 - x * 2 : x * 2 - 1));
    int shift = r.pcrv * i;
    auto put = [&](int k, int p, int off) {
      if (w >= cap) return;
      int pp = !shift ? p : degrees && scale ? degree_move(p, key, scale, shift) : p + shift;   // (0: as written, never quantised)
      int v = (int)((float)s.note[k].vel * f + 0.5f);
      int l = iv * gate / 100;
      l = s.note[k].len < l ? s.note[k].len : l;
      out[w++] = {off, (uint8_t)clamp7(pp), (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v), (uint16_t)(l < 1 ? 1 : l)};
    };
    if (arp) {
      if (!nl) break;
      int j = r.mode == kRtRnd ? rng_below(rng_key(seed, track, idx, loop, kRngArp, i), nl) : i % nl;
      put(src[j], list[j], lo + i * iv);
    } else if (r.mode == kRtWalk) {
      put(ord[i % n], pitch[ord[i % n]], lo + i * iv);
    } else {
      for (int k = 0; k < n; ++k) put(k, pitch[k], s.note[k].micro + i * iv);
    }
  }
  return w;
}

// ---- R1: random notes
CORE_COLD const char* rnd_mode_name(int m) {
  static const char* const k[kRndModes] = {"OFF", "STEPS", "TRACK"};
  return m >= 0 && m < kRndModes ? k[m] : "?";
}
CORE_COLD const char* weight_name(int w) {
  static const char* const k[kWeights] = {"FLAT", "ROOT", "CHORD", "LOW", "HIGH", "STEP"};
  return w >= 0 && w < kWeights ? k[w] : "?";
}
CORE_COLD uint16_t chord_pcs(const Step& s, int key, int scale) {
  uint8_t x[8];
  int n = 0, low = 127;
  for (int i = 0; i < s.n; ++i) low = s.note[i].pitch < low ? s.note[i].pitch : low;
  if (s.chd && s.n) n = diatonic_chord(low, s.chd, 0, key, scale, x);
  else n = diatonic_chord(60 + ((key % 12) + 12) % 12, 1, 0, key, scale ? scale : kScaleMaj, x);   // the key's triad
  uint16_t m = 0;
  for (int i = 0; i < n; ++i) m |= (uint16_t)(1u << (x[i] % 12));
  return m;
}
CORE_COLD void rnd_notes(int n, uint32_t seed, int track, int idx, int64_t loop, int key, int scale, int low, int oct,
                         int wght, uint16_t chord, const uint8_t* written, uint8_t* out) {
  int cand[64], nc = 0;
  if (oct < 1) oct = 1;
  if (oct > 4) oct = 4;
  for (int p = low; p <= low + 12 * oct && p <= 127 && nc < 64; ++p)
    if (p >= 0 && in_scale(p, key, scale)) cand[nc++] = p;
  bool taken[64] = {false};
  for (int k = 0; k < n; ++k) {
    int w[64], total = 0;
    for (int i = 0; i < nc; ++i) {
      int x = 1;
      int pc = cand[i] % 12;
      switch (wght) {
        case kWRoot: x = pc == ((key % 12) + 12) % 12 ? 3 : 1; break;
        case kWChord: x = chord >> pc & 1 ? 3 : 1; break;
        case kWLow: x = nc - i; break;
        case kWHigh: x = i + 1; break;
        case kWStep: {
          int d = cand[i] - written[k];
          d = d < 0 ? -d : d;
          x = d < 8 ? 8 - d : 1;
          break;
        }
        default: break;
      }
      w[i] = taken[i] ? 0 : x;
      total += w[i];
    }
    if (!total) {   // the range holds fewer notes than the step: the written one
      out[k] = written[k];
      continue;
    }
    int r = rng_below(rng_key(seed, track, idx, loop, kRngNote, k), total);
    int i = 0;
    while (i < nc - 1 && r >= w[i]) r -= w[i++];
    while (i < nc - 1 && !w[i]) ++i;
    taken[i] = true;
    out[k] = (uint8_t)cand[i];
  }
}

}  // namespace seq
}  // namespace core
#endif
