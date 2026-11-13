// core/mod/kinds.cpp - see target.h: the kinds' table (spec-050 1.3, 2.9). Control side only, cold.
#include "mod/target.h"

#include <string.h>

#include "fx/send_fx.h"
#include "lfo.h"
#include "platform.h"
#include "voices.h"
#if CORE_POLY
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "mod/rytm_out.h"
#include "mod/rytm_param.h"
#endif

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
// LEV, the master's slots' MIX, then (0.5.0) the bus slots' send TYPE MIX: B1A = bus 1 slot A's (master::slot_index + 1)
const char* const kMfxP[] = {"LEV", "MIX1", "MIX2", "MIX3", "MIX4", "B1A", "B1B", "B2A", "B2B", "B3A", "B3B"};
const char* const kOutP[] = {"LEV", "PAN", "WID"};
const char* const kSeqP[] = {"SPD", "DIR", "SWG", "LEN", "RNDM", "RLOW", "ROCT", "RWGT"};
const char* const kArpP[] = {"MODE", "SPD", "RNG", "GATE", "SWNG"};
const char* const kLoopP[] = {"BAL", "LEV"};
const char* const kRytmP[] = {"SCN",   "PERF1", "PERF2", "PERF3",  "PERF4",  "PERF5", "PERF6",
                              "PERF7", "PERF8", "PERF9", "PERF10", "PERF11", "PERF12"};
// 0.5.0 the Rytm's own sound parameters (rytm-param-mod 3.5; "" a word never offered: no param there)
const char* const kRsP[] = {"",    "MUL", "WAV", "MOD", "",    "SPD", "FAD", "SPH", "DEP", "P1", "P2",
                            "P3",  "P4",  "P5",  "P6",  "P7",  "P8",  "TUN", "FIN", "SMP", "BR", "STA",
                            "END", "LOP", "LEV"};   // param = the word 1..24 (4 LFO DST: never)
const char* const kRaP[] = {"FENV", "FATK", "FDEC", "FSUS", "FREL", "FRQ", "RES", "FTYP", "ATK",
                            "HLD",  "DEC",  "OVR",  "VOL",  "PAN",  "ACC", "DEL", "REV",  "VM1",
                            "VM2",  "VM3",  "VM4",  "AT1",  "AT2",  "AT3", "AT4", "VEL"};   // words 25..41, 48..56
const char* const kRfxP[] = {"TIM",  "X",    "WID",  "FDB", "DHPF", "DLPF", "DREV", "DVOL", "DOV", "",   "PRE",
                             "RDEC", "RFRQ", "GAI",  "RHPF", "RLPF", "RVOL", "",    "AMT",  "SYM", "",   "THR",
                             "CATK", "CREL", "RAT",  "SEQ", "MUP",  "MIX",  "CVOL", "SPD",  "FAD", "DEP"};   // 9..37, 5 6 8
const Row kRows[kKinds] = {
    {"", 0, 0, nullptr, 0, false},         // kNone
    {"V", 8, 16, nullptr, 1, false},       // kV (core/voices.h kVoices)
    {"FX", 2, 4, nullptr, 2, false},       // kFx
    {"S", 12, 2, kSndP, 0, true},          // kSnd (core/fx/send_fx.h kTracks)
    {"I", 13, 12, kInsP, 0, true},         // kIns (core/fx/insert.h kChains, kParams x kSlots + kEqParams)
    {"X", 8, 3, kIsndP, 0, true},          // kIsnd (ins::kBuses)
    {"MFX", 1, 11, kMfxP, 0, true},        // kMfx (1 + master::kAllSlots)
    {"O", 8, 3, kOutP, 0, true},           // kOut
    {"T", 12, 8, kSeqP, 0, true},          // kSeq (the Rytm's tracks)
    {"R", 12, 5, kArpP, 0, true},          // kArp
    {"LOOP", 1, 2, kLoopP, 0, true},       // kLoop
    {"RYTM", 1, 13, kRytmP, 0, true},      // kRytm
    {"RT", 12, 25, kRsP, 0, true},         // kRs (param = word; 0 none)
    {"RT", 12, 26, kRaP, 0, true},         // kRa
    {"RFX", 1, 32, kRfxP, 0, true},        // kRfx
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
  return index_of(t) < r.indices && param_of(t) < r.params && (!r.pnames || r.pnames[param_of(t)][0]);
}

CORE_COLD bool is_offset(Kind k) { return k == kIns || k == kIsnd || k == kMfx || k == kOut || is_remote(k); }

CORE_COLD bool rytm_word(Tid t, int* track, int* word) {
  if (!valid(t) || !is_remote(kind_of(t))) return false;
  int i = index_of(t), p = param_of(t), w;
  switch (kind_of(t)) {
    case kRs: w = p; break;
    case kRa: w = p <= 16 ? 25 + p : 48 + (p - 17); break;   // 17..24 VM1..4 AT1..4 = 48..55, 25 VEL = 56
    default: w = p <= 28 ? 9 + p : p == 29 ? 5 : p == 30 ? 6 : 8, i = 12; break;
  }
  if (track) *track = i;
  if (word) *word = w;
  return true;
}
CORE_COLD Tid rytm_tid(int track, int w) {
  Tid t = 0;
  if (track == 12) t = w >= 9 && w <= 37 ? tid(kRfx, 0, w - 9) : w == 5 ? tid(kRfx, 0, 29) : w == 6 ? tid(kRfx, 0, 30)
                     : w == 8 ? tid(kRfx, 0, 31) : 0;
  else if (track >= 0 && track < 12) t = w >= 1 && w <= 24 ? tid(kRs, track, w) : w >= 25 && w <= 41 ? tid(kRa, track, w - 25)
                                         : w >= 48 && w <= 56 ? tid(kRa, track, 17 + w - 48) : 0;
  return valid(t) ? t : 0;
}
CORE_COLD bool override_only(Tid t) {
  int tr, w;
  if (!rytm_word(t, &tr, &w)) return false;
  // the FX track: the DEL / REV routing lists (not valid targets anyway); the compressor's ATK REL RAT SEQ (lists)
  if (tr == 12) return w == 18 || w == 26 || (w >= 31 && w <= 34);
  return w == 1 || w == 2 || w == 3 || w == 19 || w == 23 || w == 32;   // MUL WAV MOD, SMP LOP, FTYP
}

CORE_COLD Range range(Tid t) {
  if (!valid(t)) return {0, 0, kOff};
  Kind k = kind_of(t);
  int p = param_of(t);
  if (is_remote(k) && override_only(t)) return {0, 127, kStep};   // a list word: a whole entry, overridden only
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

CORE_COLD const char* param_label(Tid t, char* b, int cap) {
  if (cap <= 0) return b;
  b[0] = 0;
  if (!valid(t)) return b;
  Kind k = kind_of(t);
  int i = index_of(t), p = param_of(t);
  if (k == kV) return lfo::param_name(lfo::kTVoice + i, p, b, cap);
  if (k == kFx) return lfo::param_name(lfo::kTFx + i, p, b, cap);
#if CORE_POLY
  if (k == kIns) return lfo::param_name(i < ins::kM0 ? lfo::kTIns + i : lfo::kTMaster + i - ins::kM0, p, b, cap);
  if (is_remote(k)) return rp_label(t, b, cap);   // the Rytm's own: "FRQ"; P1..P8 by the track's machine ("TUN")
#endif
  const Row& r = kRows[k];
  if (r.pnames) sniprintf(b, cap, "%s", r.pnames[p]);
  else sniprintf(b, cap, "P%d", p + r.pbase);
  return b;
}

CORE_COLD const char* label(Tid t, char* b, int cap) {
  if (cap <= 0) return b;
  b[0] = 0;
  if (!valid(t)) return b;
  char n[24], pl[8];
  name(t, n, sizeof n);
  param_label(t, pl, sizeof pl);
  const char* colon = strchr(n, ':');
  sniprintf(b, cap, "%.*s:%s", colon ? (int)(colon - n) : 0, n, pl);
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

// ---------------------------------------------------------------------------------------------------- base, writer

CORE_COLD float base_now(Tid t) {
  int i = index_of(t), p = param_of(t);
  switch (kind_of(t)) {
    case kV: return v[i & 7].p[p & 15];
    case kFx: return fx::control.knob[i & 1][2 + (p & 3)];
#if CORE_POLY
    case kSnd: return fx::control.send[i % fx::kTracks][p & 1];
#endif
    default: return 0;   // the offset kinds: the offset is on the knob (their base is read where it is applied)
  }
}

CORE_COLD float knob_now(Tid t) {
  int i = index_of(t), p = param_of(t);
  switch (kind_of(t)) {
#if CORE_POLY
    case kIns:   // the step lock when there is one (insert.cpp applies the offset on it), else the knob
      if (p >= 8) return (float)ins::eq_param(i, (p - 8) & 3);
      {
        int l = ins::locked(i, (p >> 2) & 1, p & 3);
        return (float)(l >= 0 ? l : ins::param(i, (p >> 2) & 1, p & 3));
      }
    case kIsnd: return (float)ins::send_knob(i, p);
    case kMfx: return (float)(p ? master::control.mix[(p - 1) % master::kAllSlots] : master::control.lev);
    case kOut: return (float)out::param(i, p % 3);
#endif
    default: return base_now(t);
  }
}

CORE_COLD void put(Tid t, float val) {
  int i = index_of(t), p = param_of(t);
  switch (kind_of(t)) {
    case kV: v[i & 7].p[p & 15] = val; break;
    case kFx: fx::control.knob[i & 1][2 + (p & 3)] = (uint8_t)val; break;
#if CORE_POLY
    case kIns:   // fx-ins: params 0..7 slot A / B P1..P4, 8..11 the chain's POST EQ (ins1112)
      if (p >= 8) ins::eq_mod_offset(i, (p - 8) & 3, (int)val);
      else ins::mod_offset(i, (p >> 2) & 1, p & 3, (int)val);
      break;
    case kOut: out::mod_offset(i, p % 3, (int)val); break;
    case kSnd: fx::control.send[i % fx::kTracks][p & 1] = (uint8_t)val; break;
    case kIsnd: ins::send_offset(i, p, (int)val); break;
    case kMfx: master::mod_offset(p, (int)val); break;
    case kRytm:   // the Rytm's own scene / a PERF macro (spec-050 2.7: never the kit), on change, at most every 5 ms
      if (p == 0) rytm_scene((int)val);
      else rytm_perf(p, (int)val);
      break;
    case kRs:
    case kRa:
    case kRfx:   // the Rytm's own sound parameters: the offset (in steps, unrounded) to the sender (rytm_param.h)
      rp_offset(t, val);
      break;
#endif
    default: break;
  }
}

}  // namespace mod
}  // namespace core
