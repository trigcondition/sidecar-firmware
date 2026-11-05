// core/mod/kinds.cpp - see target.h: the kinds' table (spec-050 1.3, 2.9). Control side only, cold.
#include "mod/target.h"

#include <string.h>

#include "platform.h"

namespace core {
namespace mod {

namespace {

// one row a kind: its name's prefix (kIns: per chain, below), how many indices and params, the params' names
struct Row {
  const char* prefix;   // "" none (kNone)
  uint8_t indices;      // 1: a single one, named without an index ("MFX:LEV")
  uint8_t params;
  const char* const* pnames;   // null: "P<n>" from pbase
  uint8_t pbase;               // the first param's number in "P<n>" names (kV P1.., kFx P2..)
  bool poly;                   // a CORE_POLY kind (the Sidecar Mini has kV and kFx only)
};
const char* const kSndP[] = {"F1", "F2"};
const char* const kInsP[] = {"A1", "A2", "A3", "A4", "B1", "B2", "B3", "B4", "E1", "E2", "E3", "E4"};
const char* const kIsndP[] = {"B1", "B2", "B3"};
const char* const kMfxP[] = {"LEV", "MIX1", "MIX2", "MIX3", "MIX4"};
const char* const kOutP[] = {"LEV", "PAN", "WID"};
const char* const kSeqP[] = {"SPD", "DIR", "SWG", "LEN", "RNDM", "RLOW", "ROCT", "RWGT"};
const char* const kArpP[] = {"MODE", "SPD", "RNG", "GATE", "SWNG"};
const char* const kLoopP[] = {"BAL", "LEV"};
const char* const kRytmP[] = {"SCN",   "PERF1", "PERF2", "PERF3",  "PERF4",  "PERF5", "PERF6",
                              "PERF7", "PERF8", "PERF9", "PERF10", "PERF11", "PERF12"};
const Row kRows[kKinds] = {
    {"", 0, 0, nullptr, 0, false},         // kNone
    {"V", 8, 16, nullptr, 1, false},       // kV (core/voices.h kVoices)
    {"FX", 2, 4, nullptr, 2, false},       // kFx
    {"S", 12, 2, kSndP, 0, true},          // kSnd (core/fx/send_fx.h kTracks)
    {"I", 13, 12, kInsP, 0, true},         // kIns (core/fx/insert.h kChains, kParams x kSlots + kEqParams)
    {"X", 8, 3, kIsndP, 0, true},          // kIsnd (ins::kBuses)
    {"MFX", 1, 5, kMfxP, 0, true},         // kMfx
    {"O", 8, 3, kOutP, 0, true},           // kOut
    {"T", 12, 8, kSeqP, 0, true},          // kSeq (the Rytm's tracks)
    {"R", 12, 5, kArpP, 0, true},          // kArp
    {"LOOP", 1, 2, kLoopP, 0, true},       // kLoop
    {"RYTM", 1, 13, kRytmP, 0, true},      // kRytm
};
// kIns's chains by name: I1..I8 the voices', B1..B3 the buses', M1 M2 the master's
struct Chains { const char* prefix; uint8_t first, n; };
const Chains kChainNames[3] = {{"I", 0, 8}, {"B", 8, 3}, {"M", 11, 2}};
// kSeq's ranges (core/seq/seq.h kSpeeds, core/seq/rng.h kDirs, kMaxSteps, kRndModes, kWeights) and kArp's
// (core/seq/seq.h kArpLockMax)
const Range kSeqR[8] = {{0, 9, kStep}, {0, 5, kStep}, {50, 80, kCont}, {1, 128, kStep},
                        {0, 2, kStep}, {0, 127, kCont}, {1, 4, kCont}, {0, 5, kStep}};
const uint8_t kArpTop[5] = {5, 5, 2, 2, 2};

CORE_COLD char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
CORE_COLD bool same(const char* a, int na, const char* b) {   // a[0..na) against b, any case
  int i = 0;
  for (; i < na && b[i]; ++i)
    if (up(a[i]) != b[i]) return false;
  return i == na && !b[i];
}

}  // namespace

CORE_COLD bool valid(Tid t) {
  Kind k = kind_of(t);
  if (k == kNone || k >= kKinds) return false;
  const Row& r = kRows[k];
#if !CORE_POLY
  if (r.poly) return false;
#endif
  return index_of(t) < r.indices && param_of(t) < r.params;
}

CORE_COLD bool is_offset(Kind k) { return k == kIns || k == kIsnd || k == kMfx || k == kOut; }

CORE_COLD Range range(Tid t) {
  if (!valid(t)) return {0, 0, kOff};
  Kind k = kind_of(t);
  int p = param_of(t);
  if (is_offset(k)) return {-127, 127, kCont};
  switch (k) {
    case kV: return {0, 127.99f, kCont};
    case kSeq: return kSeqR[p];
    case kArp: return {0, (float)kArpTop[p], kStep};
    case kRytm: return p ? Range{0, 127, kCont} : Range{0, 12, kStep};
    default: return {0, 127, kCont};   // kFx kSnd kLoop
  }
}

CORE_COLD const char* name(Tid t, char* b, int cap) {
  if (cap <= 0) return b;
  b[0] = 0;
  if (!valid(t)) return b;
  Kind k = kind_of(t);
  const Row& r = kRows[k];
  int i = index_of(t), p = param_of(t);
  char pn[8];
  if (r.pnames) sniprintf(pn, sizeof pn, "%s", r.pnames[p]);
  else sniprintf(pn, sizeof pn, "P%d", p + r.pbase);
  if (k == kIns) {
    for (const Chains& c : kChainNames)
      if (i >= c.first && i < c.first + c.n) sniprintf(b, cap, "%s%d:%s", c.prefix, i - c.first + 1, pn);
  } else if (r.indices == 1) {
    sniprintf(b, cap, "%s:%s", r.prefix, pn);
  } else {
    sniprintf(b, cap, "%s%d:%s", r.prefix, i + 1, pn);
  }
  return b;
}

CORE_COLD bool parse(const char* s, Tid* out) {
  if (!s) return false;
  const char* colon = strchr(s, ':');
  if (!colon) return false;
  int nl = 0;   // the prefix's letters
  while (s + nl < colon && ((s[nl] >= 'a' && s[nl] <= 'z') || (s[nl] >= 'A' && s[nl] <= 'Z'))) ++nl;
  int idx = 0, nd = 0;
  while (s + nl + nd < colon && s[nl + nd] >= '0' && s[nl + nd] <= '9' && nd < 3) idx = idx * 10 + (s[nl + nd] - '0'), ++nd;
  if (s + nl + nd != colon || !nl) return false;
  const char* ps = colon + 1;
  int pl = (int)strlen(ps);
  for (int k = kV; k < kKinds; ++k) {
    const Row& r = kRows[k];
    int index = -1;
    if (k == kIns) {
      for (const Chains& c : kChainNames)
        if (nd && same(s, nl, c.prefix) && idx >= 1 && idx <= c.n) index = c.first + idx - 1;
    } else if (same(s, nl, r.prefix)) {
      if (r.indices == 1) index = nd ? -1 : 0;
      else if (nd && idx >= 1 && idx <= r.indices) index = idx - 1;
    }
    if (index < 0) continue;
    for (int p = 0; p < r.params; ++p) {
      char pn[8];
      if (r.pnames) sniprintf(pn, sizeof pn, "%s", r.pnames[p]);
      else sniprintf(pn, sizeof pn, "P%d", p + r.pbase);
      if (!same(ps, pl, pn)) continue;
      Tid t = tid((Kind)k, index, p);
      if (!valid(t)) return false;
      *out = t;
      return true;
    }
  }
  return false;
}

}  // namespace mod
}  // namespace core
