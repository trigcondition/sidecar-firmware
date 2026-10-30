// core/seq/pattern.cpp - see seq.h: the pattern model and its file (sim/protocol.py sqp_pack / sqp_unpack). Cold.
#include "seq/seq.h"

#if CORE_POLY
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "poly.h"
#include "seq/rng.h"

namespace core {
namespace seq {

CORE_COLD void clear(Pattern& p, int index) {
  memset(&p, 0, sizeof p);
  p.index = index;
  for (Track& t : p.t) t.rytm = -1;
  memset(p.keep_rytm, -1, sizeof p.keep_rytm);
}

CORE_COLD Track* track_of(Pattern& p, int rytm, bool create) {
  for (Track& t : p.t)
    if (t.rytm == rytm) return &t;
  if (!create || rytm < 0 || rytm >= kRytmTracks) return nullptr;
  for (Track& t : p.t)
    if (t.rytm < 0) {
      track_init(t, rytm);
      return &t;
    }
  return nullptr;
}
CORE_COLD void track_init(Track& t, int rytm) {
  memset(&t, 0, sizeof t);
  t.rytm = (int8_t)rytm, t.hint = 0x7F, t.dnote = 60, t.dvel = 100, t.dlen = kSub / 4 * 3;
  t.rnd_low = kRndLowDef, t.rnd_oct = kRndOctDef;              // seqtop R1
  t.smod_range = kSmodRangeDef, t.smod_quant = kSqLoop;        // seqtop R1b (Q10: LOOP)
}
CORE_COLD const Track* track_of(const Pattern& p, int rytm) {
  for (const Track& t : p.t)
    if (t.rytm == rytm) return &t;
  return nullptr;
}

CORE_COLD void copy_steps(const Track& src, int from, Track& dst, int to, int n) {
  for (int i = 0; i < n; ++i) {
    int a = from + i, b = to + i;
    if (a < 0 || b < 0 || a >= kMaxSteps || b >= kMaxSteps) continue;
    dst.step[b] = src.step[a];
  }
}
CORE_COLD void clear_steps(Track& t, int from, int n) {
  for (int i = from; i < from + n; ++i)
    if (i >= 0 && i < kMaxSteps) memset(&t.step[i], 0, sizeof t.step[i]);
}

// A step that holds anything at all: not just notes and locks, every seqtop extra too (a condition, a ratchet, an
// FX column, a slide, the random flag). grow_pages uses it to tell a never-used page from one an earlier
// shortening hid, so the compare is against a zero Step rather than a list of fields that could grow.
CORE_COLD bool step_used(const Step& s) {
  static const Step kNone = {};
  return memcmp(&s, &kNone, sizeof s) != 0;
}

// seq1112: LENGTH grows -> the new pages the Rytm's way. Each newly revealed step takes the pattern as it stood
// (step i <- step i % from_len), so 1 -> 2 pages duplicates page 1 and 2 -> 4 copies pages 1-2 into 3-4; whole
// Step structs are assigned, so the locks, their conditions, the trig conditions, ratchets, FX columns, slides and
// the random flag come with the copy.
// A page the growth reveals that **already holds steps** is left exactly as it is: shortening only hides steps
// (length is a play limit, every one of kMaxSteps is always stored), so growing back must give them back rather
// than paint over them. The test is per page and over the steps the growth actually reveals.
CORE_COLD void grow_pages(Track& t, int from_len, int to_len) {
  if (from_len < 1 || from_len > kMaxSteps || to_len <= from_len) return;
  if (to_len > kMaxSteps) to_len = kMaxSteps;
  for (int base = from_len / kStepsPerPage * kStepsPerPage; base < to_len; base += kStepsPerPage) {
    int a = base < from_len ? from_len : base;                                   // the page's first new step
    int b = base + kStepsPerPage < to_len ? base + kStepsPerPage : to_len;       // past its last
    if (a >= b) continue;
    bool fresh = true;
    for (int i = a; i < b && fresh; ++i) fresh = !step_used(t.step[i]);
    if (!fresh) continue;   // hidden by an earlier shortening: revealed as it was
    for (int i = a; i < b; ++i) t.step[i] = t.step[i % from_len];
  }
}

CORE_COLD int used_steps(const Track& t) {
  int n = 0;
  for (const Step& s : t.step) n += s.n > 0 || s.nl > 0;
  return n;
}

CORE_COLD int lock_of(const Step& s, int param) {
  for (int i = 0; i < s.nl && i < kLocks; ++i)
    if (s.lock[i].param == param) return s.lock[i].value;
  return -1;
}
CORE_COLD bool set_lock(Step& s, int param, int value) {
  if (param < 0 || param >= kLockParams) return false;
  value = value < 0 ? 0 : value > 16383 ? 16383 : value;
  for (int i = 0; i < s.nl && i < kLocks; ++i)
    if (s.lock[i].param == param) {
      s.lock[i].value = (uint16_t)value;
      return true;
    }
  if (s.nl >= kLocks) return false;
  s.lock[s.nl++] = {(uint8_t)param, 0, (uint16_t)value};   // (a new lock: always; an existing one keeps its condition)
  if (!s.prob) s.prob = 100;
  return true;
}
CORE_COLD bool clear_lock(Step& s, int param) {
  for (int i = 0; i < s.nl && i < kLocks; ++i)
    if (s.lock[i].param == param) {
      for (int j = i + 1; j < s.nl; ++j) s.lock[j - 1] = s.lock[j];
      --s.nl;
      s.slide &= ~(1u << (param & 31));   // seqtop X1: its slide goes with it
      return true;
    }
  return false;
}
CORE_COLD bool has_locks(const Track& t) {
  for (const Step& s : t.step)
    if (s.nl) return true;
  return false;
}

CORE_COLD bool add_note(Step& s, const Note& n) {
  for (int i = 0; i < s.n; ++i)
    if (s.note[i].pitch == n.pitch) {
      s.note[i] = n;
      return true;
    }
  if (s.n >= kNotes) return false;
  s.note[s.n++] = n;
  s.flags |= 1;
  if (!s.prob) s.prob = 100;
  return true;
}

CORE_COLD bool remove_pitch(Step& s, int pitch) {
  for (int i = 0; i < s.n; ++i)
    if (s.note[i].pitch == pitch) {
      for (int j = i + 1; j < s.n; ++j) s.note[j - 1] = s.note[j];
      --s.n;
      return true;
    }
  return false;
}

CORE_COLD void apply_strum(Step& s, int strum) {
  if (strum < -kSub) strum = -kSub;
  if (strum > kSub) strum = kSub;
  s.strum = (int8_t)strum;
  int a = strum < 0 ? -strum : strum;
  for (int i = 0; i < s.n; ++i) {   // rank by pitch: 0 the lowest (strum < 0: the highest)
    int k = 0;
    for (int j = 0; j < s.n; ++j)
      if (strum >= 0 ? s.note[j].pitch < s.note[i].pitch : s.note[j].pitch > s.note[i].pitch) ++k;
    int m = k * a;
    s.note[i].micro = (int8_t)(m > kSub ? kSub : m);
  }
}

CORE_COLD void set_chord(Step& s, int root, int chd, int inv, int vel, int len, int strum) {
  uint8_t ns[poly::kMaxNotes];
  poly::Chord c = {chd < 0 || chd >= poly::kChords ? 0 : chd, inv < 0 ? 0 : inv % poly::kInvs, 0, 0};
  int k = poly::expand(root, c, ns);
  s.n = 0;
  for (int i = 0; i < k && i < kNotes; ++i) s.note[s.n++] = {ns[i], (uint8_t)vel, (uint16_t)(len < 1 ? 1 : len), 0, 0};
  s.chd = (uint8_t)c.chd, s.inv = (uint8_t)c.inv;
  s.flags |= 1;
  if (!s.prob) s.prob = 100;
  apply_strum(s, strum);
}

// ---- seqtop P2: the speed ratios (ids are the file's)
namespace {
struct SpeedDef {
  uint8_t num, den, elektron;
  const char* name;
};
const SpeedDef kSpeedDefs[kSpeeds] = {{1, 1, 1, "FOLLOW"}, {1, 8, 1, "1/8"}, {1, 4, 1, "1/4"}, {1, 2, 1, "1/2"},
                                      {3, 4, 1, "3/4"},    {3, 2, 1, "3/2"}, {2, 1, 1, "2"},   {2, 3, 0, "2/3"},
                                      {5, 4, 0, "5/4"},    {7, 8, 0, "7/8"}};
// the knob's order, slowest first (FOLLOW is 1:1)
const uint8_t kSpeedOrder[kSpeeds] = {kSpd1_8, kSpd1_4, kSpd1_2, kSpd2_3, kSpd3_4, kSpd7_8, kSpdFollow, kSpd5_4, kSpd3_2, kSpd2};
}  // namespace
CORE_COLD Ratio speed_ratio(int spd) {
  if (spd <= 0 || spd >= kSpeeds) return {1, 1};
  return {kSpeedDefs[spd].num, kSpeedDefs[spd].den};
}
CORE_COLD const char* speed_name(int spd) { return spd >= 0 && spd < kSpeeds ? kSpeedDefs[spd].name : "?"; }
CORE_COLD bool speed_elektron(int spd) { return spd >= 0 && spd < kSpeeds && kSpeedDefs[spd].elektron; }
CORE_COLD int speed_parse(const char* s) {
  if (!s) return -1;
  if (!strcmp(s, "follow") || !strcmp(s, "FOLLOW") || !strcmp(s, "1") || !strcmp(s, "1/1")) return kSpdFollow;
  for (int i = 1; i < kSpeeds; ++i)
    if (!strcmp(s, kSpeedDefs[i].name)) return i;
  return -1;
}
CORE_COLD int speed_step(int spd, int d, bool func) {
  int at = 6;   // FOLLOW
  for (int i = 0; i < kSpeeds; ++i)
    if (kSpeedOrder[i] == spd) at = i;
  int dir = d > 0 ? 1 : -1;
  for (int n = d > 0 ? d : -d; n > 0; --n) {
    int i = at + dir;
    while (i >= 0 && i < kSpeeds && !func && !kSpeedDefs[kSpeedOrder[i]].elektron) i += dir;
    if (i < 0 || i >= kSpeeds) break;
    at = i;
  }
  return kSpeedOrder[at];
}

// ---- seqtop H1: the FX column's commands
namespace {
const char* const kFxNames[kFxCmds] = {"---", "RTG", "DLY", "CUT", "CHN", "RND", "RNV", "OCT", "TRS", "JMP", "JRN", "LOP", "SKP", "DIR", "FIL"};
CORE_COLD int fx_lo(int c) { return c == kFxOct ? -4 : c == kFxTrs ? -24 : c == kFxDir ? 0 : c == kFxJrn || c == kFxChn ? 0 : 1; }
CORE_COLD int fx_hi(int c) {
  switch (c) {
    case kFxRtg: return kRtMax;
    case kFxDly: case kFxCut: return kSub;
    case kFxChn: return 100;
    case kFxRnd: case kFxTrs: return 24;
    case kFxRnv: return 127;
    case kFxOct: return 4;
    case kFxJmp: return kMaxSteps;
    case kFxJrn: return 64;
    case kFxSkp: return 16;
    case kFxDir: return kDirs - 1;
    case kFxFil: return kMaxPages;
    default: return 255;
  }
}
}  // namespace
CORE_COLD const char* fx_name(int c) { return c >= 0 && c < kFxCmds ? kFxNames[c] : "?"; }
CORE_COLD void fx_range(int c, int* lo, int* hi) {   // rui1050: the Rytm's FX page's VAL knob
  *lo = fx_lo(c), *hi = c == kFxNone ? 0 : c == kFxLop ? 255 : fx_hi(c);
}
static CORE_COLD bool fx_valid(const Fx& f) {   // a command this build knows with a value in its range
  if (f.cmd >= kFxCmds) return false;
  if (f.cmd == kFxNone) return true;
  if (f.cmd == kFxLop) return true;
  int v = f.cmd == kFxOct || f.cmd == kFxTrs ? (int8_t)f.val : f.val;
  return v >= fx_lo(f.cmd) && v <= fx_hi(f.cmd);
}
CORE_COLD int fx_parse(const char* cmd, const char* val, Fx* out) {
  int c = -1;
  for (int i = 0; i < kFxCmds; ++i) {
    const char* a = cmd;
    const char* b = kFxNames[i];
    while (*a && *b && (*a == *b || *a == *b + 32)) ++a, ++b;
    if (!*a && !*b) c = i;
  }
  if (c < 0) return 0;
  if (c == kFxNone) return *out = {0, 0}, 1;
  if (!val) return 0;
  if (c == kFxLop) {   // "NxX": loop the last N (1..16) steps X (1..16) more times
    int n = atoi(val);
    const char* x = strchr(val, 'x');
    int k = x ? atoi(x + 1) : 1;
    if (n < 1 || n > 16 || k < 1 || k > 16) return 0;
    return *out = {(uint8_t)c, (uint8_t)((k - 1) << 4 | (n - 1))}, 1;
  }
  int v;
  if (c == kFxDir) {
    v = dir_parse(val);
    if (v < 0) return 0;
  } else {
    if (!((*val >= '0' && *val <= '9') || *val == '-' || *val == '+')) return 0;
    v = atoi(val);
  }
  if (v < fx_lo(c) || v > fx_hi(c)) return 0;
  return *out = {(uint8_t)c, (uint8_t)v}, 1;
}
CORE_COLD const char* fx_text(const Fx& f, char* b, int cap) {
  if (!f.cmd || f.cmd >= kFxCmds) sniprintf(b, cap, "---");
  else if (f.cmd == kFxLop) sniprintf(b, cap, "LOP %dx%d", (f.val & 15) + 1, (f.val >> 4) + 1);
  else if (f.cmd == kFxDir) sniprintf(b, cap, "DIR %s", dir_name(f.val));
  else if (f.cmd == kFxOct || f.cmd == kFxTrs) sniprintf(b, cap, "%s %+d", kFxNames[f.cmd], (int8_t)f.val);
  else sniprintf(b, cap, "%s %d", kFxNames[f.cmd], f.val);
  return b;
}

// ---- seqtop V1: a lock's condition (the COND knob's codes without PRE / NEI)
CORE_COLD bool lock_cond_ok(int c) {
  return c == kCondNone || c == kCondFill || c == kCondNotFill || c == kCond1st || c == kCondNot1st ||
         (c >= kCondAB && c < kCondKnob);
}
CORE_COLD const char* lock_cond_text(int c, char* b, int cap) {
  int a = 0, bb = 0;
  if (cond_ab(c, &a, &bb)) sniprintf(b, cap, "%d:%d", a, bb);
  else if (c >= kConds && c < kCondKnob) sniprintf(b, cap, "%d%%", c - kConds + 1);
  else sniprintf(b, cap, "%s", c == kCondFill ? "FILL" : c == kCondNotFill ? "!FILL" : c == kCond1st ? "1ST" : c == kCondNot1st ? "!1ST" : "---");
  return b;
}
CORE_COLD int lock_cond_parse(const char* s) {
  if (!s || !*s) return -1;
  int n = 0;
  char w[8];
  for (; s[n] && n < 7; ++n) w[n] = (char)(s[n] >= 'a' && s[n] <= 'z' ? s[n] - 32 : s[n]);
  w[n] = 0;
  if (s[n]) return -1;
  if (!strcmp(w, "---") || !strcmp(w, "NONE") || !strcmp(w, "OFF")) return kCondNone;
  if (!strcmp(w, "FILL")) return kCondFill;
  if (!strcmp(w, "!FILL")) return kCondNotFill;
  if (!strcmp(w, "1ST")) return kCond1st;
  if (!strcmp(w, "!1ST")) return kCondNot1st;
  int a = 0, k = 0;
  while (w[k] >= '0' && w[k] <= '9') a = a * 10 + (w[k++] - '0');
  if (!k) return -1;
  if (w[k] == '%' && !w[k + 1]) return a >= 1 && a <= 99 ? kConds + a - 1 : a >= 100 ? kCondNone : -1;
  if (w[k] == ':') {
    int b = 0, j = k + 1;
    while (w[j] >= '0' && w[j] <= '9') b = b * 10 + (w[j++] - '0');
    if (w[j] || b < 2 || b > 8 || a < 1 || a > b) return -1;
    for (int c = kCondAB; c < kConds; ++c) {
      int x, y;
      if (cond_ab(c, &x, &y) && x == a && y == b) return c;
    }
    return -1;
  }
  if (!w[k]) return lock_cond_ok(a) ? a : -1;
  return -1;
}

// ---- seqtop P1: the directions' names
namespace {
const char* const kDirNames[kDirs] = {"FWD", "REV", "PEND", "PING", "RND", "BROWN"};
}
CORE_COLD const char* dir_name(int dir) { return dir >= 0 && dir < kDirs ? kDirNames[dir] : "?"; }
CORE_COLD int dir_parse(const char* s) {
  if (!s) return -1;
  for (int i = 0; i < kDirs; ++i) {
    const char* n = kDirNames[i];
    int k = 0;
    while (n[k] && s[k] && (s[k] == n[k] || s[k] == n[k] + 32)) ++k;
    if (!n[k] && !s[k]) return i;
  }
  return -1;
}

CORE_COLD uint32_t crc32(const uint8_t* d, int n) {
  uint32_t c = 0xFFFFFFFFu;
  for (int i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

namespace {
// v2 (seq-ux, 1036): a lock is 3 bytes {param, value lo, value hi} (14 bits); v1 wrote {param, value} (never any:
// its writer had no locks) and is read as before, a v1 lock's value taken as the integer part (<< 7)
// v4 (E2, seqx): v3 + an optional trailer of tagged blocks between the last track and END: 'X' 'B' <u16 count>, then
// per block <u8 id> <u8 flags> <u16 len> <payload>. Written only when a block is (a fixed seed, a track's seed, a poly
// config, a kept unknown block): every other pattern stays v2 / v3 and older Sidecars keep reading it; a v4 file locks
// on them (fix1042's rule: never played without its data, never saved over).
constexpr uint8_t kVersion = 4;   // rui1040: v3 = flags bits 2..7 the trig condition (written only when a step has one)
constexpr int kHeader = 12, kTrackHead = 11, kStepHead = 8, kNoteBytes = 5, kLockBytes = 3, kBlockHead = 4;
// the known blocks' payloads (bytes after the head)
constexpr int kSeedBytes = 3, kTrackSeedBytes = 5, kPolyBytes = 7;
constexpr int kSpeedBytes = 2;   // seqtop P2: <track> <speed id>
constexpr int kDirBytes = 2;     // seqtop P1: <track> <direction id>
constexpr int kOnBytes = 2;      // per pattern poly: <track> <on 0|1>
constexpr int kScaleBytes = 3;   // seqtop M1: <key> <scale> <qnt> (the pattern's)
constexpr int kRndBytes = 21;    // seqtop R1: <track> <mode> <low> <oct> <weight> <16 bytes: the steps' kXfRnd>
constexpr int kSmodBytes = 5;    // seqtop R1b: <track> <src> <range> <quant> <affects>
constexpr int kLockCondEntry = 3;   // seqtop V1: <track>, then per conditional lock <step> <param> <cond>
constexpr int kRtrgEntry = 8;       // seqtop P4 / M4: <track>, then per step <step> <cnt> <rate> <vcrv> <pcrv> <mode> <oct> <gate>
constexpr int kSlideEntry = 2;      // seqtop X1: <track>, then per sliding lock <step> <param>
constexpr int kFxEntry = 5;         // seqtop H1: <track>, then per step <step> <cmd 1> <val 1> <cmd 2> <val 2>
int g_unpack_err = 0, g_dropped = 0, g_unkept = 0;
struct W {
  uint8_t* p;
  int n, cap;
  bool ok;
  void b(int v) { if (n < cap) p[n++] = (uint8_t)v; else ok = false; }
  void h(int v) { b(v & 0xFF), b(v >> 8 & 0xFF); }
};
struct R {
  const uint8_t* p;
  int n, len;
  bool ok;
  int b() { if (n < len) return p[n++]; ok = false; return 0; }
  int h() { int lo = b(); return lo | b() << 8; }
};
// the blocks this build writes for p (into w; count: how many)
CORE_COLD void known_blocks(const Pattern& p, W& w, int* count) {
  if (p.seed_fixed || p.seed) {
    w.b(kBlkSeed), w.b(0), w.h(kSeedBytes), w.b(p.seed_fixed ? 1 : 0), w.h(p.seed);
    ++*count;
  }
  bool rkey = false;   // (the key alone matters only to R1's ROOT weight: review - a KEY tap on CHR made a v4 file)
  for (const Track& t : p.t) rkey |= t.rytm >= 0 && t.rnd_mode && t.rnd_wght == kWRoot;
  if (p.scale || p.qnt || (p.key && rkey)) {   // seqtop M1: required only when it changes what plays (QNT PLAY)
    w.b(kBlkScale), w.b(p.qnt == kQntPlay ? kBlkRequired : 0), w.h(kScaleBytes), w.b(p.key), w.b(p.scale), w.b(p.qnt);
    ++*count;
  }
  for (const Track& t : p.t) {
    if (t.rytm < 0) continue;
    // per pattern poly: the slot's on flag, where the old-file rule (steps: on, none: off) would read it otherwise
    if ((t.on != 0) != (used_steps(t) > 0)) {
      w.b(kBlkOn), w.b(kBlkTrack), w.h(kOnBytes), w.b(t.rytm), w.b(t.on ? 1 : 0);
      ++*count;
    }
    if (t.seed_own || t.seed_same || t.seed) {
      w.b(kBlkTrackSeed), w.b(kBlkTrack), w.h(kTrackSeedBytes), w.b(t.rytm), w.b(t.seed_own ? 1 : 0), w.h(t.seed),
          w.b(t.seed_same ? 1 : 0);
      ++*count;
    }
    if (!poly_default(t.poly)) {
      const PolyCfg& c = t.poly;
      w.b(kBlkPoly), w.b(kBlkTrack), w.h(kPolyBytes), w.b(t.rytm), w.b(c.voices), w.b(c.alloc), w.b(c.unison),
          w.b(c.detune), w.b(c.spread), w.b(c.group);
      ++*count;
    }
    if (t.speed) {   // seqtop P2
      w.b(kBlkSpeed), w.b(kBlkTrack | kBlkRequired), w.h(kSpeedBytes), w.b(t.rytm), w.b(t.speed);
      ++*count;
    }
    if (t.dir) {   // seqtop P1
      w.b(kBlkDir), w.b(kBlkTrack | kBlkRequired), w.h(kDirBytes), w.b(t.rytm), w.b(t.dir);
      ++*count;
    }
    uint8_t rm[16] = {0};   // seqtop R1: the random steps (with notes)
    bool any = false;
    for (int i = 0; i < kMaxSteps; ++i)
      if ((t.step[i].xf & kXfRnd) && t.step[i].n) rm[i / 8] |= (uint8_t)(1u << (i % 8)), any = true;
    if (any || t.rnd_mode || t.rnd_low != kRndLowDef || t.rnd_oct != kRndOctDef || t.rnd_wght) {
      w.b(kBlkRnd), w.b(kBlkTrack | kBlkRequired | kBlkPerStep), w.h(kRndBytes), w.b(t.rytm), w.b(t.rnd_mode),
          w.b(t.rnd_low), w.b(t.rnd_oct), w.b(t.rnd_wght);
      for (int i = 0; i < 16; ++i) w.b(rm[i]);
      ++*count;
    }
    int nlc = 0;   // seqtop V1: the conditional locks (of steps that are stored: notes or locks)
    for (int i = 0; i < kMaxSteps; ++i)
      for (int k = 0; k < t.step[i].nl && k < kLocks; ++k) nlc += t.step[i].lock[k].cond != 0;
    if (nlc) {
      w.b(kBlkLockCond), w.b(kBlkTrack | kBlkRequired | kBlkPerStep), w.h(1 + kLockCondEntry * nlc), w.b(t.rytm);
      for (int i = 0; i < kMaxSteps; ++i)
        for (int k = 0; k < t.step[i].nl && k < kLocks; ++k)
          if (t.step[i].lock[k].cond) w.b(i), w.b(t.step[i].lock[k].param), w.b(t.step[i].lock[k].cond);
      ++*count;
    }
    int nsl = 0;   // seqtop X1: the sliding locks (a bit of a param the step does not lock: nothing)
    for (int i = 0; i < kMaxSteps; ++i)
      for (int k = 0; k < t.step[i].nl && k < kLocks; ++k) nsl += t.step[i].slide >> (t.step[i].lock[k].param & 31) & 1;
    if (nsl) {
      w.b(kBlkSlide), w.b(kBlkTrack | kBlkRequired | kBlkPerStep), w.h(1 + kSlideEntry * nsl), w.b(t.rytm);
      for (int i = 0; i < kMaxSteps; ++i)
        for (int k = 0; k < t.step[i].nl && k < kLocks; ++k)
          if (t.step[i].slide >> (t.step[i].lock[k].param & 31) & 1) w.b(i), w.b(t.step[i].lock[k].param);
      ++*count;
    }
    int nfx = 0;   // seqtop H1: the steps with a command (any step: a jump may sit on an empty one)
    for (int i = 0; i < kMaxSteps; ++i) nfx += t.step[i].fx[0].cmd || t.step[i].fx[1].cmd;
    if (nfx) {
      w.b(kBlkFx), w.b(kBlkTrack | kBlkRequired | kBlkPerStep), w.h(1 + kFxEntry * nfx), w.b(t.rytm);
      for (int i = 0; i < kMaxSteps; ++i) {
        const Step& s = t.step[i];
        if (s.fx[0].cmd || s.fx[1].cmd) w.b(i), w.b(s.fx[0].cmd), w.b(s.fx[0].val), w.b(s.fx[1].cmd), w.b(s.fx[1].val);
      }
      ++*count;
    }
    int nrt = 0;   // seqtop P4 / M4: the steps (with notes) with a ratchet / arp
    for (int i = 0; i < kMaxSteps; ++i) nrt += t.step[i].n && rt_on(t.step[i].rt);
    if (nrt) {
      w.b(kBlkRtrg), w.b(kBlkTrack | kBlkRequired | kBlkPerStep), w.h(1 + kRtrgEntry * nrt), w.b(t.rytm);
      for (int i = 0; i < kMaxSteps; ++i) {
        const Rtrg& x = t.step[i].rt;
        if (t.step[i].n && rt_on(x)) w.b(i), w.b(x.cnt), w.b(x.rate), w.b(x.vcrv), w.b((uint8_t)x.pcrv), w.b(x.mode), w.b(x.oct), w.b(x.gate);
      }
      ++*count;
    }
    if (t.smod_src || t.smod_range != kSmodRangeDef || t.smod_quant != kSqLoop || t.smod_aff) {   // seqtop R1b
      w.b(kBlkSmod), w.b(kBlkTrack | kBlkRequired), w.h(kSmodBytes), w.b(t.rytm), w.b(t.smod_src), w.b(t.smod_range),
          w.b(t.smod_quant), w.b(t.smod_aff);
      ++*count;
    }
  }
}
CORE_COLD bool has_track(const Pattern& p, int rytm) {
  if (rytm < 0) return false;
  for (const Track& t : p.t)
    if (t.rytm == rytm) return true;
  return false;
}
CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

CORE_COLD int pack_bound() {
  return kHeader + kTracks * (kTrackHead + kMaxSteps * (kStepHead + kNotes * kNoteBytes + 1 + kLocks * kLockBytes)) +
         4 + (kBlockHead + kSeedBytes) + (kBlockHead + kScaleBytes) + kTracks * (3 * kBlockHead + kTrackSeedBytes + kPolyBytes + kOnBytes) +
         kTracks * (4 * kBlockHead + kSpeedBytes + kDirBytes + kRndBytes + kSmodBytes) +
         kTracks * (kBlockHead + 1 + kMaxSteps * kLocks * kLockCondEntry) + kTracks * (kBlockHead + 1 + kMaxSteps * kRtrgEntry) +
         kTracks * (kBlockHead + 1 + kMaxSteps * kLocks * kSlideEntry) + kTracks * (kBlockHead + 1 + kMaxSteps * kFxEntry) +
         kKeepBytes + 4;
}

CORE_COLD int pack(const Pattern& p, uint8_t* out, int cap) {
  W w = {out, 0, cap, true};
  g_dropped = 0;
  int nt = 0;
  for (const Track& t : p.t) nt += t.rytm >= 0;
  int ver = 2;   // rui1040: a file without conditions stays version 2 (older Sidecars read it)
  for (const Track& t : p.t)
    for (int i = 0; t.rytm >= 0 && i < kMaxSteps; ++i)
      if ((t.step[i].n || t.step[i].nl) && cond_of(t.step[i])) ver = 3;
  w.b('S'), w.b('Q'), w.b('P'), w.b('1'), w.b(ver), w.b(nt), w.h(0), w.h(0), w.h(0);   // crc below
  int8_t sec_rytm[kTracks];
  uint32_t sec_crc[kTracks];
  int nsec = 0;
  for (const Track& t : p.t) {
    if (t.rytm < 0) continue;
    w.b(t.rytm), w.b(t.hint), w.h(t.length), w.b(0), w.b(t.swing), w.b(t.dnote), w.b(t.dvel), w.b(t.dlen / 4);
    w.h(used_steps(t));
    int s0 = w.n;
    for (int i = 0; i < kMaxSteps; ++i) {
      const Step& s = t.step[i];
      if (!s.n && !s.nl) continue;
      w.h(i), w.b(s.flags), w.b((uint8_t)s.strum), w.b(s.chd), w.b(s.inv), w.b(s.prob ? s.prob : 100), w.b(s.n);
      for (int k = 0; k < s.n; ++k) {
        const Note& n = s.note[k];
        w.b(n.pitch), w.b(n.vel), w.h(n.len), w.b((uint8_t)n.micro);
      }
      int nl = s.nl > kLocks ? kLocks : s.nl;
      w.b(nl);
      for (int k = 0; k < nl; ++k) w.b(s.lock[k].param), w.h(s.lock[k].value & 0x3FFF);
    }
    if (w.ok && nsec < kTracks) sec_rytm[nsec] = t.rytm, sec_crc[nsec++] = crc32(out + s0, w.n - s0);
  }
  // E2: the blocks (known ones, then the kept unknown ones still valid); none: no trailer, the version as before
  int tr = w.n, count = 0;
  w.b('X'), w.b('B'), w.h(0);
  known_blocks(p, w, &count);
  for (int k = 0; k + kBlockHead <= p.nkeep;) {
    const uint8_t* b = p.keep + k;
    int len = b[2] | b[3] << 8, all = kBlockHead + len;
    if (k + all > p.nkeep) break;
    k += all;
    if (b[1] & kBlkTrack) {
      int r = len ? b[4] : -1;
      if (!has_track(p, r)) { ++g_dropped; continue; }   // its track is gone
      if (b[1] & kBlkPerStep) {   // its steps: as loaded, else it no longer describes them
        bool same = false;
        for (int i = 0; i < kTracks; ++i)
          if (p.keep_rytm[i] == r)
            for (int j = 0; j < nsec; ++j) same |= sec_rytm[j] == r && sec_crc[j] == p.keep_crc[i];
        if (!same) { ++g_dropped; continue; }
      }
    }
    for (int i = 0; i < all; ++i) w.b(b[i]);
    ++count;
  }
  if (count && w.ok) {
    out[tr + 2] = (uint8_t)(count & 0xFF), out[tr + 3] = (uint8_t)(count >> 8);
    out[4] = kVersion;
  } else {
    w.n = tr;   // no blocks: no trailer
  }
  w.b('E'), w.b('N'), w.b('D'), w.b(0);
  if (!w.ok) return 0;
  uint32_t c = crc32(out + kHeader, w.n - kHeader);
  for (int k = 0; k < 4; ++k) out[8 + k] = (uint8_t)(c >> (8 * k));
  return w.n;
}

int pack_version() { return kVersion; }
int unpack_error() { return g_unpack_err; }
int pack_dropped() { return g_dropped; }
int unpack_unkept() { return g_unkept; }

CORE_COLD bool unpack(Pattern& p, int index, const uint8_t* d, int len) {
  clear(p, index);
  g_unpack_err = kUnpackDamaged;
  g_unkept = 0;
  if (len < kHeader + 4 || memcmp(d, "SQP1", 4) || d[4] < 1) return false;
  if (d[4] > kVersion) {
    g_unpack_err = kUnpackNewer;
    return false;
  }
  int ver = d[4];
  uint32_t c = (uint32_t)d[8] | (uint32_t)d[9] << 8 | (uint32_t)d[10] << 16 | (uint32_t)d[11] << 24;
  if (crc32(d + kHeader, len - kHeader) != c || memcmp(d + len - 4, "END", 4)) return false;
  R r = {d, kHeader, len - 4, true};
  int nt = d[5];
  int nsec = 0;
  for (int i = 0; i < nt && r.ok; ++i) {
    int rytm = r.b(), hint = r.b(), length = r.h();
    r.b();
    int swing = r.b();
    int dn = r.b(), dv = r.b(), dl = r.b() * 4, ns = r.h();
    Track* t = rytm < kRytmTracks ? track_of(p, rytm, true) : nullptr;
    if (t) {
      t->hint = (uint8_t)hint, t->length = (uint16_t)(length > kMaxSteps ? kMaxSteps : length);
      t->swing = (uint8_t)(swing >= 50 && swing <= 80 ? swing : 0);
      t->dnote = (uint8_t)(dn & 0x7F ? dn & 0x7F : 60), t->dvel = (uint8_t)(dv & 0x7F), t->dlen = (uint16_t)(dl ? dl : kSub / 4 * 3);
    }
    int s0 = r.n;
    for (int k = 0; k < ns && r.ok; ++k) {
      int idx = r.h();
      Step tmp = {};
      tmp.flags = (uint8_t)r.b(), tmp.strum = (int8_t)r.b(), tmp.chd = (uint8_t)r.b(), tmp.inv = (uint8_t)r.b();
      if (ver < 3 || (tmp.flags >> 2) >= kConds) tmp.flags &= 3;   // rui1040: the condition bits only from v3
      tmp.prob = (uint8_t)r.b();
      int nn = r.b();
      for (int j = 0; j < nn && r.ok; ++j) {
        Note n = {};
        n.pitch = (uint8_t)(r.b() & 0x7F), n.vel = (uint8_t)(r.b() & 0x7F), n.len = (uint16_t)r.h(), n.micro = (int8_t)r.b();
        if (n.len < 1) n.len = 1;
        if (n.micro < -kSub) n.micro = -kSub;
        if (n.micro > kSub) n.micro = kSub;
        if (j < kNotes) tmp.note[tmp.n++] = n;
      }
      int locks = r.b();
      for (int j = 0; j < locks && r.ok; ++j) {
        int prm = r.b(), val = ver >= 2 ? r.h() : r.b() << 7;
        if (prm < kLockParams && tmp.nl < kLocks) tmp.lock[tmp.nl++] = {(uint8_t)prm, 0, (uint16_t)(val & 0x3FFF)};
      }
      if (!tmp.n && !tmp.nl) continue;   // (an empty step: nothing to keep)
      if (t && idx < kMaxSteps) t->step[idx] = tmp;
    }
    if (r.ok && t && nsec < kTracks) p.keep_rytm[nsec] = (int8_t)rytm, p.keep_crc[nsec++] = crc32(d + s0, r.n - s0);
  }
  // per pattern poly: a file without kBlkOn (every one saved before it): a slot that holds steps is on, an empty one off
  for (Track& t : p.t)
    if (t.rytm >= 0) t.on = used_steps(t) > 0 ? 1 : 0;
  // E2: v4's blocks (a v1..v3 file has none: anything before END there was never read, as before)
  if (r.ok && ver >= 4 && r.n + 4 <= r.len && d[r.n] == 'X' && d[r.n + 1] == 'B') {
    r.n += 2;
    int nb = r.h();
    for (int k = 0; k < nb && r.ok; ++k) {
      int id = r.b(), fl = r.b(), bl = r.h(), b0 = r.n;
      if (!r.ok || b0 + bl > r.len) { r.ok = false; break; }
      const uint8_t* q = d + b0;
      Track* t = nullptr;
      if ((fl & kBlkTrack) && bl && q[0] < kRytmTracks) t = track_of(p, q[0], false);
      bool known = id == kBlkSeed || id == kBlkTrackSeed || id == kBlkPoly || id == kBlkSpeed || id == kBlkDir ||
                   id == kBlkScale || id == kBlkRnd || id == kBlkSmod || id == kBlkLockCond || id == kBlkRtrg ||
                   id == kBlkSlide || id == kBlkFx || id == kBlkOn;
      int need = id == kBlkSeed ? kSeedBytes : id == kBlkTrackSeed ? kTrackSeedBytes : id == kBlkPoly ? kPolyBytes
               : id == kBlkSpeed ? kSpeedBytes : id == kBlkDir ? kDirBytes : id == kBlkScale ? kScaleBytes
               : id == kBlkRnd ? kRndBytes : id == kBlkSmod ? kSmodBytes : id == kBlkOn ? kOnBytes : 1;
      if (known && bl < need) { r.ok = false; break; }   // (a later build may make a block longer, never shorter)
      if (id == kBlkSeed) {
        p.seed_fixed = q[0] ? 1 : 0, p.seed = (uint16_t)clampi(q[1] | q[2] << 8, 0, 9999);
      } else if (id == kBlkTrackSeed) {
        if (t) t->seed_own = q[1] ? 1 : 0, t->seed = (uint16_t)clampi(q[2] | q[3] << 8, 0, 9999), t->seed_same = q[4] ? 1 : 0;
      } else if (id == kBlkPoly) {
        if (t) {
          PolyCfg& pc = t->poly;
          pc.voices = (uint8_t)clampi(q[1], 0, kMaxVoicesCfg), pc.alloc = (uint8_t)(q[2] < kAllocs ? q[2] : 0);
          pc.unison = (uint8_t)clampi(q[3], 0, kMaxUnison), pc.detune = (uint8_t)clampi(q[4], 0, 127);
          pc.spread = (uint8_t)clampi(q[5], 0, 127), pc.group = (uint8_t)clampi(q[6], 0, kPolyGroups);
        }
      } else if (id == kBlkOn) {   // per pattern poly
        if (t) t->on = q[1] ? 1 : 0;
      } else if (id == kBlkScale) {   // seqtop M1; a scale / QNT this build does not know: a newer Sidecar's
        if (q[0] >= 12 || q[1] >= kScales || q[2] >= kQnts) {   // (review: values beyond this build's lock, never clamped)
          clear(p, index);
          g_unpack_err = kUnpackNewer;
          return false;
        }
        p.key = q[0], p.scale = q[1], p.qnt = q[2];
      } else if (id == kBlkRnd) {   // seqtop R1; a mode / weight this build does not know: a newer Sidecar's
        if (q[1] >= kRndModes || q[2] > 127 || q[3] < 1 || q[3] > 4 || q[4] >= kWeights) {
          clear(p, index);
          g_unpack_err = kUnpackNewer;
          return false;
        }
        if (t) {
          t->rnd_mode = q[1], t->rnd_low = q[2], t->rnd_oct = q[3], t->rnd_wght = q[4];
          for (int i = 0; i < kMaxSteps; ++i)
            if (q[5 + i / 8] >> (i % 8) & 1) t->step[i].xf |= kXfRnd;
        }
      } else if (id == kBlkLockCond) {   // seqtop V1: <track> {<step> <param> <cond>}..; a code this build does not
        for (int e = 1; e + kLockCondEntry <= bl; e += kLockCondEntry) {   // allow: a newer Sidecar's (locked)
          if (!lock_cond_ok(q[e + 2])) {
            clear(p, index);
            g_unpack_err = kUnpackNewer;
            return false;
          }
          if (t && q[e] < kMaxSteps)
            for (int k = 0; k < t->step[q[e]].nl; ++k)
              if (t->step[q[e]].lock[k].param == q[e + 1]) t->step[q[e]].lock[k].cond = q[e + 2];
        }
      } else if (id == kBlkFx) {   // seqtop H1; a command this build does not know (or a value beyond it): a newer Sidecar's
        for (int e = 1; e + kFxEntry <= bl; e += kFxEntry) {
          Fx a = {q[e + 1], q[e + 2]}, b = {q[e + 3], q[e + 4]};
          if (!fx_valid(a) || !fx_valid(b)) {
            clear(p, index);
            g_unpack_err = kUnpackNewer;
            return false;
          }
          if (t && q[e] < kMaxSteps) t->step[q[e]].fx[0] = a, t->step[q[e]].fx[1] = b;
        }
      } else if (id == kBlkSlide) {   // seqtop X1: <track> {<step> <param>}..
        for (int e = 1; e + kSlideEntry <= bl; e += kSlideEntry)
          if (t && q[e] < kMaxSteps && q[e + 1] < kLockParams && lock_of(t->step[q[e]], q[e + 1]) >= 0)   // (a lock that is
            t->step[q[e]].slide |= 1u << q[e + 1];                                                       // there)
      } else if (id == kBlkRtrg) {   // seqtop P4 / M4; a mode / rate / curve this build does not know: a newer Sidecar's
        for (int e = 1; e + kRtrgEntry <= bl; e += kRtrgEntry) {
          const uint8_t* x = q + e;
          if (x[1] > kRtMax || x[2] >= kRtRates || x[3] >= kVcs || (int8_t)x[4] < -24 || (int8_t)x[4] > 24 || x[5] >= kRtModes ||
              x[6] > 4 || x[7] > 100) {
            clear(p, index);
            g_unpack_err = kUnpackNewer;
            return false;
          }
          if (t && x[0] < kMaxSteps)
            t->step[x[0]].rt = {x[1], x[2], x[3], (int8_t)x[4], x[5], x[6], x[7]};
        }
      } else if (id == kBlkSmod) {   // seqtop R1b
        if (q[1] > 16 || q[2] < 2 || q[2] > 64 || q[3] >= kSqs || q[4] >= kAffs) {
          clear(p, index);
          g_unpack_err = kUnpackNewer;
          return false;
        }
        if (t) t->smod_src = q[1], t->smod_range = q[2], t->smod_quant = q[3], t->smod_aff = q[4];
      } else if (id == kBlkSpeed || id == kBlkDir) {   // seqtop P2 / P1; a value this build does not know: a newer
        if (q[1] >= (id == kBlkSpeed ? (int)kSpeeds : (int)kDirs)) {      // Sidecar's (locked)
          clear(p, index);
          g_unpack_err = kUnpackNewer;
          return false;
        }
        if (t && id == kBlkSpeed) t->speed = (uint8_t)q[1];
        if (t && id == kBlkDir) t->dir = (uint8_t)q[1];
      } else if (fl & kBlkRequired) {   // a newer Sidecar's block that changes how the pattern plays: locked
        clear(p, index);
        g_unpack_err = kUnpackNewer;
        return false;
      } else if (p.nkeep + kBlockHead + bl <= kKeepBytes) {   // kept verbatim for the next save
        memcpy(p.keep + p.nkeep, d + b0 - kBlockHead, (size_t)(kBlockHead + bl));
        p.nkeep = (uint16_t)(p.nkeep + kBlockHead + bl);
      } else {
        ++g_unkept;   // (no room: lost at the next save - counted, the caller logs it)
      }
      r.n = b0 + bl;
    }
  }
  if (r.ok && r.n != r.len && ver >= 4) r.ok = false;   // v4: nothing unexplained before END
  if (!r.ok) {
    clear(p, index);
    return false;
  }
  g_unpack_err = kUnpackOk;
  return true;
}

}  // namespace seq
}  // namespace core
#endif
