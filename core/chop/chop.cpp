// core/chop/chop.cpp - see chop.h. Everything but render_add / render_add2 is the control core's and cold.
#include "../platform.h"

#include "chop.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#if CORE_MSAMP
#include "../fmt/fmt.h"
#include "../fx/out.h"
#include "../lib/library.h"
#include "../lib/pools.h"
#include "../lib/srclib.h"
#include "../livein/livein.h"
#include "../machines.h"
#include "../msamp/cardq.h"
#include "../poly.h"
#include "../pool/pcache.h"
#include "../pool/pool.h"
#include "../samp/sampler.h"
#include "../ui/remote.h"
#include "../voices.h"
#endif

namespace core {
namespace chop {

// ---- the per-slice fields (any build: the editors' texts)
namespace {
struct FieldDef {
  const char* name;   // the Rytm's label
  const char* word;   // the console's
  int lo, hi;
};
const FieldDef kFieldDefs[kFields] = {
    {"PTCH", "pitch", -24, 24}, {"FINE", "fine", -50, 50},    {"GAIN", "gain", -24, 12},  {"REV", "rev", 0, 1},
    {"ATK", "atk", 0, 2000},    {"DEC", "dec", 0, 10000},     {"LOOP", "loop", 0, 2},     {"CHOK", "choke", 0, 4},
    {"NUDG", "nudge", -2400, 2400}, {"STUT", "stut", 1, 16},  {"SLEN", "slen", 1, 100},   {"SDEC", "sdec", 0, 90},
    {"TAPE", "tape", 0, 2},     {"TTIM", "tms", 20, 4000},    {"NBR", "nbr", 0, 100},
};
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

const char* field_name(int f) { return f >= 0 && f < kFields ? kFieldDefs[f].name : "?"; }
int field_of(const char* s) {
  for (int f = 0; f < kFields; ++f)
    if (!strcasecmp(s, kFieldDefs[f].name) || !strcasecmp(s, kFieldDefs[f].word)) return f;
  return -1;
}
void field_range(int f, int* lo, int* hi) {
  *lo = f >= 0 && f < kFields ? kFieldDefs[f].lo : 0;
  *hi = f >= 0 && f < kFields ? kFieldDefs[f].hi : 0;
}
int field_get(const Slice& s, int f) {
  switch (f) {
    case kFPitch: return s.pitch;
    case kFFine: return s.fine;
    case kFGain: return s.gain;
    case kFRev: return s.rev;
    case kFAtk: return s.atk_ms;
    case kFDec: return s.dec_ms;
    case kFLoop: return s.loop;
    case kFChoke: return s.choke;
    case kFNudge: return s.nudge;
    case kFStut: return s.stut;
    case kFSlen: return s.stut_len;
    case kFSdec: return s.stut_dec;
    case kFTape: return s.tape;
    case kFTms: return s.tape_ms;
    case kFNbr: return s.nbr;
    default: return 0;
  }
}
void field_set(Slice* s, int f, int v) {
  if (f < 0 || f >= kFields) return;
  v = clampi(v, kFieldDefs[f].lo, kFieldDefs[f].hi);
  switch (f) {
    case kFPitch: s->pitch = (int8_t)v; break;
    case kFFine: s->fine = (int8_t)v; break;
    case kFGain: s->gain = (int8_t)v; break;
    case kFRev: s->rev = (uint8_t)v; break;
    case kFAtk: s->atk_ms = (uint16_t)v; break;
    case kFDec: s->dec_ms = (uint16_t)v; break;
    case kFLoop: s->loop = (uint8_t)v; break;
    case kFChoke: s->choke = (uint8_t)v; break;
    case kFNudge: s->nudge = (int16_t)v; break;
    case kFStut: s->stut = (uint8_t)v; break;
    case kFSlen: s->stut_len = (uint8_t)v; break;
    case kFSdec: s->stut_dec = (uint8_t)v; break;
    case kFTape: s->tape = (uint8_t)v; break;
    case kFTms: s->tape_ms = (uint16_t)v; break;
    case kFNbr: s->nbr = (uint8_t)v; break;
  }
}
const char* field_text(const Slice& s, int f, uint32_t rate, char* b, int cap) {
  int v = field_get(s, f);
  switch (f) {
    case kFPitch: sniprintf(b, cap, v > 0 ? "+%d" : "%d", v); break;
    case kFFine: sniprintf(b, cap, v > 0 ? "+%dC" : "%dC", v); break;
    case kFGain: sniprintf(b, cap, v > 0 ? "+%dDB" : "%dDB", v); break;
    case kFRev: sniprintf(b, cap, "%s", v ? "REV" : "FWD"); break;
    case kFAtk: case kFDec: case kFTms:
      if (f == kFDec && !v) sniprintf(b, cap, "HOLD");
      else if (v < 1000) sniprintf(b, cap, "%dMS", v);
      else sniprintf(b, cap, "%d.%02dS", v / 1000, (v % 1000) / 10);
      break;
    case kFLoop: sniprintf(b, cap, "%s", loop_name(v)); break;
    case kFChoke: if (v) sniprintf(b, cap, "G%d", v); else sniprintf(b, cap, "OFF"); break;
    case kFNudge: {   // frames as ms (one decimal)
      int r = rate ? (int)rate : 48000;
      int ms10 = (int)((int64_t)v * 10000 / r);
      int a = ms10 < 0 ? -ms10 : ms10;
      sniprintf(b, cap, "%s%d.%dMS", ms10 < 0 ? "-" : ms10 > 0 ? "+" : "", a / 10, a % 10);
      break;
    }
    case kFStut: if (v > 1) sniprintf(b, cap, "X%d", v); else sniprintf(b, cap, "OFF"); break;
    case kFSlen: case kFSdec: case kFNbr: sniprintf(b, cap, "%d%%", v); break;
    case kFTape: sniprintf(b, cap, "%s", tape_name(v)); break;
    default: sniprintf(b, cap, "%d", v); break;
  }
  return b;
}

#if CORE_MSAMP
namespace {

inline uint32_t ld_acq(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline uint32_t ld_rlx(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
inline void st_rel(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline void st_rlx(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

constexpr float kEdge = 48.0f;          // a jump's (a loop's, a stutter's, a one-shot's end) fade, frames: 1 ms
constexpr float kKillFrames = 96.0f;    // a choked / stolen / silenced note's fade: 2 ms
constexpr float kRelFrames = 1440.0f;   // a looped slice's release after its note-off: 30 ms
constexpr uint32_t kMapText = 65536;    // a slice map's file at most (fmt's max_bytes; 64 regions are ~16 KB)
constexpr uint32_t kTransientPiece = 65536;   // TRANSIENT's frames a step
constexpr int kKeyFrames = 96;          // a pad's way to the host (scan + USB MIDI), frames [inferred: 2 ms]

// ---- sources
enum SrcKind : uint8_t { kSrcNone = 0, kSrcFile, kSrcRam };
struct Src {
  uint8_t kind;
  bool ready;
  const int16_t* pcm;
  uint32_t frames, rate;
  uint8_t ch;
  int blk, pe;                  // the pool RAM's block (pinned) and entry (a file)
  lib::Ref ref;                 // what it holds (a file)
  char path[kPathLen];
  uint32_t used_ms;
  int8_t* ov;                   // the overview: min / max a bin (kBins frames), int8
  uint32_t nbins, ov_done;      // bins, bins built
};
Src* srcs = nullptr;            // [kSrcSlots]
int8_t* ov_mem = nullptr;       // [kSrcSlots][2 * (kSlotFrames / kBins + 1)]
constexpr uint32_t kOvBins = kSlotFrames / kBins + 1;

// ---- maps
enum MapState : uint8_t { kMFree = 0, kMFind, kMRead, kMReading, kMReady };
struct MapSlot {
  uint8_t state;
  bool has_sha, ram;            // ram: a use_buffer map (never written)
  uint8_t sha[32];
  char key_path[kPathLen];      // without a sha: the sample's path
  Map m, undo;
  bool has_undo, need_grid, dirty, saved_once, accept;
  uint32_t dirty_ms, gen, used_ms;
  char file[kPathLen];          // where it is written / was read
  int inst;                     // its INSTRUMENTS row (-1 not known)
  int find_code;                // the scan's next INSTRUMENTS code
  char found[kPathLen];         // the scan's best file
  int found_rank;               // 2 a .chop.sfz, 1 a -slices.sfz (v1)
  Onsets on;                    // TRANSIENT in progress
  bool on_busy;
};
MapSlot* maps = nullptr;        // [kMaps]

// ---- notes (the render core reads Start through the sequence lock)
struct Start {
  int8_t src, src2;             // source slots (src2 -1: no MORPH partner)
  uint8_t rev, loop, tape, stut;
  uint8_t choke;
  bool lazy;
  uint32_t a, b, a2, b2;        // frames of the sources
  float inc, gain;
  float stut_len, stut_g;       // a repeat's frames (source), the level a repeat keeps
  float tape_r0;                // the tape ramp's source frames (inc x its time / 2)
  float atk, dec_k;             // the attack's step a frame, the decay's factor a frame (1 hold)
  uint32_t rel_gen, kill_gen;
};
struct Tail {                   // a note cut by a new start on its player: its last read faded out
  const int16_t* pcm;
  uint32_t frames;
  uint8_t ch;
  double pos;
  float inc, g, fade;
  bool on;
};
struct Note {
  volatile uint32_t seq;
  Start st;
  volatile uint32_t rel_n, kill_n;
  volatile uint32_t done;
  // the control core's
  bool held, live;
  int8_t ch;
  uint8_t key, mnote;
  uint32_t order;
  // the render core's
  uint32_t r_seq;
  bool r_on, r_rel, r_kill;
  Start r;
  const int16_t* p1;
  const int16_t* p2;
  uint32_t f1, f2;
  uint8_t c1, c2;
  double u;                     // travelled in the slice (source frames, along its direction)
  int dir;                      // PING's direction (+1 / -1)
  int rep;                      // STUTTER's repeat
  float rep_g, env, rel_k, kill_g;
  uint8_t stage;                // 0 attack 1 on (decaying) 2 release
  Tail tail;
  // review M3: what the render core reads now, published by it as one word - the start it plays (src, src2) and its
  // tail's source, 0xff none (bits 0..7, 8..15, 16..23). A start posted over a busy note leaves the old sound as a tail:
  // the newest post (st) and this word together cover every source a player or a tail can read (a start posted and
  // replaced before the render core saw it is never played)
  volatile uint32_t reads;
};
constexpr uint32_t kReadsNone = 0xFFFFFFu;
inline uint32_t reads_word(int a, int b, int t) {
  return (uint32_t)(a & 0xFF) | (uint32_t)(b & 0xFF) << 8 | (uint32_t)(t & 0xFF) << 16;
}
Note notes[kVoices][kPerVoice];
Map g_before;   // the map before the edit in hand (the control core's: edit_slot, TRANSIENT's landing)
uint32_t order_n = 0;
volatile int32_t g_head[kVoices][kPerVoice];   // the playheads (frames of the note's source; -1 none), for drawing
volatile uint32_t g_lazy_t[kVoices];           // the LAZY player's frames played
volatile float g_morph[kVoices];               // P4 now, 0..1
volatile uint32_t g_blocks = 0;

// ---- the voices (the control core's)
struct Pend {
  bool on;
  uint8_t note, vel;
  bool seq;
  uint32_t ms;
};
struct VS {
  float p[16];
  int code;                     // the SAMPLES row SRC plays (-1 none)
  int src;                      // its source slot (-1 not resident)
  int map;                      // its map slot (-1 none yet)
  int alt_src[kMaxAlts];        // the RESAMPLE'd slices' sources
  bool force, reported;
  Pend pend;
  uint32_t hits[kMaxSlices];    // NEIGHBOUR CHANCE's draws: each slice's hits since the map came
  uint32_t shuf_seed;
  int shuf_n;
  uint8_t order[kMaxSlices];
  int last;                     // the slice the last hit played
  Lazy lazy;
  float lazy_inc;
  int buf_src;                  // use_buffer's slot (-1 none)
  int fail_code;                // the SAMPLES row whose file did not load (not tried again until SRC changes)
  bool pre;                     // a Rytm track on this voice plays CHOP (its 33): its sample loads before a report / trig
};
VS vs[kVoices];
Stats st;
bool card_ok = false, begun = false;
int g_lat = -1;                 // chop=lat:N (-1 the estimate)
struct LiveTag {
  bool on;
  int8_t ch;
  uint8_t mnote;
};
LiveTag cur_live = {false, 0, 0};

// ---- the job (one card operation in flight at a time)
enum JobKind : uint8_t { kJIdle = 0, kJLoad, kJMapRead, kJMapWrite };
struct Job {
  uint8_t kind;
  int slot;                     // kJLoad: the source slot; map jobs: the map slot
  int pe;                       // kJLoad: the pool RAM's entry
  uint8_t io;                   // map read: 0 open 1 read 2 close
  cardq::Req rq;
  File f;
  char path[kPathLen];
  uint32_t got;
};
Job* job = nullptr;
char* text = nullptr;           // [kMapText + 1]: a map's file read / written

// ---- RESAMPLE
struct Rs {
  int state;
  int vi, k;
  bool fx;
  uint32_t until_ms, started_ms;
  char row[8];
  char wav[96];
  int code;
  int map;                      // review M5: the map and the slice's span it was taken from (the answer checks them)
  uint32_t a, b;
};
Rs rs = {kRsIdle, -1, -1, false, 0, 0, "", "", -1, -1, 0, 0};

CORE_COLD bool note_free(const Note& n) { return ld_acq(&n.done) == ld_rlx(&n.seq); }
CORE_COLD int count_active() {
  int a = 0;
  for (int vi = 0; vi < kVoices; ++vi)
    for (int k = 0; k < kPerVoice; ++k) a += !note_free(notes[vi][k]);
  return a;
}
CORE_COLD bool parse_int(const char** p, int* v) {   // [spaces] [-] digits
  const char* q = *p;
  while (*q == ' ') ++q;
  bool neg = *q == '-';
  if (neg) ++q;
  if (*q < '0' || *q > '9') return false;
  int x = 0;
  while (*q >= '0' && *q <= '9') x = x * 10 + (*q++ - '0');
  *v = neg ? -x : x;
  *p = q;
  return true;
}
CORE_COLD float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
CORE_COLD int zone(float p, int n) { return eng_zone(p, n); }
CORE_COLD bool same_ref(const lib::Ref& a, const lib::Ref& b) {
  if (a.has_sha && b.has_sha) return !memcmp(a.sha, b.sha, 32);
  return !a.has_sha && !b.has_sha && a.size == b.size && !strcasecmp(a.hint, b.hint);
}

// ---- sources
CORE_COLD bool src_busy(int s) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  for (int vi = 0; vi < kVoices; ++vi) {
    for (int k = 0; k < kPerVoice; ++k) {
      const Note& n = notes[vi][k];
      if (!note_free(n) && (n.st.src == s || n.st.src2 == s)) return true;
      uint32_t rd = ld_acq(&n.reads);   // M3: the render core's player and tail
      if ((int)(rd & 0xFF) == s || (int)(rd >> 8 & 0xFF) == s || (int)(rd >> 16 & 0xFF) == s) return true;
    }
    if (vs[vi].src == s || vs[vi].buf_src == s) return true;
    for (int a = 0; a < kMaxAlts; ++a)
      if (vs[vi].alt_src[a] == s) return true;
  }
  return false;
}
CORE_COLD void src_drop(int s) {
  Src& x = srcs[s];
  if (x.kind == kSrcFile && x.blk >= 0) pcache::unpin(x.blk);
  x.kind = kSrcNone, x.ready = false, x.blk = -1, x.pe = -1, x.pcm = nullptr, x.frames = 0, x.path[0] = 0;
  x.nbins = x.ov_done = 0;
}
CORE_COLD int src_find(const lib::Ref& r, const char* path) {
  for (int s = 0; s < kSrcSlots; ++s)
    if (srcs[s].kind == kSrcFile && (same_ref(srcs[s].ref, r) || (path && path[0] && !strcasecmp(srcs[s].path, path))))
      return s;
  return -1;
}
CORE_COLD int src_free() {   // an empty slot, else the least recently used one nothing reads
  int b = -1;
  for (int s = 0; s < kSrcSlots; ++s) {
    if (srcs[s].kind == kSrcNone) return s;
    if (src_busy(s) || !srcs[s].ready) continue;
    if (b < 0 || (int32_t)(srcs[s].used_ms - srcs[b].used_ms) < 0) b = s;
  }
  return b;
}
// a source's overview: a piece of its bins (min / max of kBins frames), the rest at later steps
CORE_COLD bool overview_step() {
  for (int s = 0; s < kSrcSlots; ++s) {
    Src& x = srcs[s];
    if (!x.ready || x.ov_done >= x.nbins) continue;
    uint32_t end = x.ov_done + 1024 < x.nbins ? x.ov_done + 1024 : x.nbins;
    for (uint32_t b = x.ov_done; b < end; ++b) {
      int mn = 0, mx = 0;
      uint32_t a = b * kBins, e = a + kBins < x.frames ? a + kBins : x.frames;
      for (uint32_t i = a; i < e; ++i) {
        int v = x.ch == 2 ? ((int)x.pcm[2 * i] + x.pcm[2 * i + 1]) >> 1 : x.pcm[i];
        mn = v < mn ? v : mn, mx = v > mx ? v : mx;
      }
      x.ov[2 * b] = (int8_t)(mn >> 8), x.ov[2 * b + 1] = (int8_t)(mx >> 8);
    }
    x.ov_done = end;
    return true;
  }
  return false;
}
CORE_COLD void src_ready(int s, const int16_t* pcm, uint32_t frames, uint32_t rate, int ch) {
  Src& x = srcs[s];
  x.pcm = pcm, x.frames = frames > kSlotFrames ? kSlotFrames : frames, x.rate = rate ? rate : 48000, x.ch = (uint8_t)(ch == 2 ? 2 : 1);
  x.ov = ov_mem + (size_t)s * 2 * kOvBins;
  x.nbins = (x.frames + kBins - 1) / kBins, x.ov_done = 0;
  x.used_ms = now_ms();
  __atomic_store_n(&x.ready, true, __ATOMIC_RELEASE);
}

// ---- maps
CORE_COLD int map_find_slot(const lib::Ref& r, const char* path) {
  for (int m = 0; m < kMaps; ++m) {
    const MapSlot& x = maps[m];
    if (x.state == kMFree || x.ram) continue;
    if (r.has_sha && x.has_sha && !memcmp(x.sha, r.sha, 32)) return m;
    if (!r.has_sha && !x.has_sha && path && !strcasecmp(x.key_path, path)) return m;
  }
  return -1;
}
CORE_COLD bool map_used(int m) {
  for (int vi = 0; vi < kVoices; ++vi)
    if (vs[vi].map == m) return true;
  return false;
}
CORE_COLD int map_new() {   // a free slot, else the least recently used one no voice holds and nothing waits to write
  int b = -1;
  for (int m = 0; m < kMaps; ++m) {
    if (maps[m].state == kMFree) return m;
    if (map_used(m) || maps[m].dirty || maps[m].on_busy || (job->kind != kJIdle && job->slot == m && job->kind != kJLoad)) continue;
    if (b < 0 || (int32_t)(maps[m].used_ms - maps[b].used_ms) < 0) b = m;
  }
  return b;
}
CORE_COLD void map_touch(int m) {
  MapSlot& x = maps[m];
  ++x.gen;
  x.dirty = !x.ram, x.dirty_ms = now_ms();
  for (int vi = 0; vi < kVoices; ++vi)
    if (vs[vi].map == m) {
      memset(vs[vi].hits, 0, sizeof vs[vi].hits), vs[vi].shuf_n = -1;
      // review M2: an edit may drop or renumber the alternative samples (drop_unused_alts): their sources are found
      // again by path (set_source's pass), never kept by index
      for (int a = 0; a < kMaxAlts; ++a) {
        vs[vi].alt_src[a] = -1;
        for (int k = 0; a < x.m.nalt && k < kSrcSlots; ++k)   // (one already in RAM at once)
          if (srcs[k].kind == kSrcFile && srcs[k].ready && !strcasecmp(srcs[k].path, x.m.alt[a])) vs[vi].alt_src[a] = k;
      }
    }
}
// the sample came into RAM: the map takes its frames (a v1 map, a new one), and GRID 16 when it has no slices yet
CORE_COLD void map_fit(int m, int s) {
  MapSlot& x = maps[m];
  const Src& sr = srcs[s];
  if (!sr.ready || x.state != kMReady) return;
  x.m.frames = sr.frames, x.m.rate = sr.rate, x.m.ch = sr.ch;
  if (x.need_grid || !x.m.n) {
    x.need_grid = false;
    grid(&x.m, kDefaultGrid, sr.pcm, sr.ch);
    ++st.maps_new;
    ++x.gen;
    return;
  }
  bool cut = false;
  for (int k = x.m.n - 1; k >= 0; --k) {   // a map of a longer file (the pool RAM cut it at 87 s): inside it
    Slice& sl = x.m.s[k];
    if (sl.alt >= 0) continue;
    if (sl.start + kMinSlice > sr.frames) { memmove(x.m.s + k, x.m.s + k + 1, sizeof(Slice) * (size_t)(x.m.n - k - 1)), --x.m.n, cut = true; continue; }
    if (sl.end > sr.frames) sl.end = sr.frames, cut = true;
  }
  if (!x.m.n) grid(&x.m, kDefaultGrid, sr.pcm, sr.ch);
  if (cut) log(CORE_STR("chop: %s: the map is longer than the sample in RAM: cut to it"), x.file);
  ++x.gen;
}
CORE_COLD void fill_sample_info(int m, int code) {
  MapSlot& x = maps[m];
  const pools::Entry& e = pools::at(pools::kSamples, code);
  x.m.has_sha = e.ref.has_sha;
  if (e.ref.has_sha) memcpy(x.m.sha, e.ref.sha, 32);
  x.m.size = e.ref.size;
  sniprintf(x.m.sample, sizeof x.m.sample, "%s", e.path[0] ? e.path : e.ref.hint);
  sniprintf(x.m.name, sizeof x.m.name, "%s", e.name);
}
// the voice's map for SRC's sample: held already, else a new slot that looks for its file in the INSTRUMENTS pool
CORE_COLD int map_for(int vi, int code) {
  const pools::Entry& e = pools::at(pools::kSamples, code);
  int m = map_find_slot(e.ref, e.path);
  if (m >= 0) {
    maps[m].used_ms = now_ms();
    return m;
  }
  m = map_new();
  if (m < 0) return -1;
  MapSlot& x = maps[m];
  memset(&x, 0, sizeof x);
  x.has_sha = e.ref.has_sha;
  if (e.ref.has_sha) memcpy(x.sha, e.ref.sha, 32);
  sniprintf(x.key_path, sizeof x.key_path, "%s", e.path);
  map_clear(&x.m);
  fill_sample_info(m, code);
  x.inst = -1, x.find_code = 0, x.found_rank = 0, x.found[0] = 0;
  x.state = kMFind, x.used_ms = now_ms();
  (void)vi;
  return m;
}
CORE_COLD bool ends_with(const char* s, const char* t) {
  size_t a = strlen(s), b = strlen(t);
  return a >= b && !strcasecmp(s + a - b, t);
}
// the INSTRUMENTS pool searched for the sample's map (a piece of the rows a step): CHOP's own .chop.sfz first, else
// the sampler's -slices.sfz (v1)
CORE_COLD bool map_find_step(int m) {
  MapSlot& x = maps[m];
  int end = x.find_code + 128 < pools::kCodes ? x.find_code + 128 : pools::kCodes;
  for (int c = x.find_code; c < end; ++c) {
    const pools::Entry& e = pools::at(pools::kInstr, c);
    if (e.type != pools::kInstrument || !e.path[0]) continue;
    int rank = ends_with(e.path, ".chop.sfz") ? 2 : ends_with(e.path, "-slices.sfz") ? 1 : 0;
    if (rank <= x.found_rank) continue;
    int nm = 0;
    const pools::Member* mem = pools::members(pools::kInstr, c, &nm);
    bool match = false;
    for (int i = 0; mem && i < nm && !match; ++i)
      match = x.has_sha ? mem[i].ref.has_sha && !memcmp(mem[i].ref.sha, x.sha, 32)
                        : !strcasecmp(mem[i].ref.hint, x.key_path);
    if (!match) continue;
    x.found_rank = rank, x.inst = rank == 2 ? c : x.inst;
    sniprintf(x.found, sizeof x.found, "%s", e.path);
  }
  x.find_code = end;
  if (end < pools::kCodes) return true;
  if (x.found[0]) {
    x.state = kMRead;
  } else {
    x.state = kMReady, x.need_grid = true;   // none: GRID 16 once the sample is in (written only when edited)
  }
  return true;
}
CORE_COLD void map_read_done(int m, bool ok, uint32_t n) {
  MapSlot& x = maps[m];
  Map keep = x.m;   // (the sample's facts: the file's may be older)
  bool good = false;
  if (ok && n) {
    char why[96] = "";
    const fmt::Format* f = fmt::find("slicemap");
    static uint8_t up[kMapText + 1];
    fmt::Buf out = {up, 0, kMapText};
    fmt::Info inf;
    fmt::Bytes in = {(const uint8_t*)text, n};
    fmt::Status s = f ? fmt::load(*f, in, &out, &inf) : fmt::kUnreadable;
    if (s == fmt::kOk || s == fmt::kUpgraded || s == fmt::kForeignNewer) {
      up[out.n] = 0;
      char dir[kPathLen];
      sniprintf(dir, sizeof dir, "%s", x.found);
      char* sl = strrchr(dir, '/');
      if (sl) *sl = 0;
      good = read((const char*)up, out.n, &x.m, why, sizeof why, dir);
      if (good && s == fmt::kUpgraded) x.dirty = true, x.dirty_ms = now_ms();   // (the sampler's v1: CHOP's own file next)
    } else {
      sniprintf(why, sizeof why, "%s", inf.why);
    }
    if (!good) log(CORE_STR("chop: %s: not a slice map it reads (%s): GRID 16"), x.found, why);
  }
  if (!good) {
    x.m = keep, x.need_grid = true, ++st.map_fails;
  } else {
    ++st.maps_read;
    if (x.found_rank == 2) sniprintf(x.file, sizeof x.file, "%s", x.found), x.saved_once = true;
    // the sample's own facts over the file's (its hash, its current path)
    x.m.has_sha = keep.has_sha, x.m.size = keep.size;
    if (keep.has_sha) memcpy(x.m.sha, keep.sha, 32);
    if (keep.sample[0]) sniprintf(x.m.sample, sizeof x.m.sample, "%s", keep.sample);
    if (!x.m.name[0]) sniprintf(x.m.name, sizeof x.m.name, "%s", keep.name);
    log(CORE_STR("chop: %s: %d slices"), x.found, x.m.n);
  }
  x.state = kMReady;
  ++x.gen;
}
// where a map is written: /CHOP/<project>/<name>-<sha8>.chop.sfz
CORE_COLD void map_path(const MapSlot& x, char* out, int cap) {
  char nm[24];
  int k = 0;
  for (const char* s = x.m.name[0] ? x.m.name : "SAMPLE"; *s && k < (int)sizeof nm - 1; ++s) {
    char c = *s;
    nm[k++] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ? c : '_';
  }
  nm[k] = 0;
  char h[9] = "nosha000";
  static const char kHex[] = "0123456789abcdef";
  if (x.has_sha)
    for (int i = 0; i < 4; ++i) h[2 * i] = kHex[x.sha[i] >> 4], h[2 * i + 1] = kHex[x.sha[i] & 15];
  const char* key = pools::project();
  char dir[kPathLen];
  if (!fmt::path("slicemap", key && key[0] ? key : "default", 0, dir, sizeof dir)) sniprintf(dir, sizeof dir, "/CHOP/default");
  sniprintf(out, cap, "%s/%s-%s.chop.sfz", dir, nm, h);
}

// ---- the job
CORE_COLD bool start_load() {
  if (!pcache::ok()) return false;
  for (int vi = 0; vi < kVoices; ++vi) {
    VS& x = vs[vi];
    if (!(v[vi].cp || x.force || x.pre) || x.code < 0 || x.src >= 0 || x.buf_src >= 0 || x.code == x.fail_code) continue;
    const pools::Entry& e = pools::at(pools::kSamples, x.code);
    if (e.type != pools::kSample || !pools::playable(e) || !e.path[0]) continue;
    int s = src_find(e.ref, e.path);
    if (s >= 0 && srcs[s].ready) { x.src = s; continue; }
    if (s >= 0) continue;   // (loading)
    s = src_free();
    if (s < 0) return false;
    src_drop(s);
    int pe = pcache::want(e.ref, e.path);
    if (pe < 0) {
      ++st.load_fails, x.fail_code = x.code;
      log(CORE_STR("chop: %s: no room in the pool RAM (or it failed before)"), e.path);
      return true;
    }
    Src& sr = srcs[s];
    sr.kind = kSrcFile, sr.ref = e.ref, sr.pe = pe;
    sniprintf(sr.path, sizeof sr.path, "%s", e.path);
    job->kind = kJLoad, job->slot = s, job->pe = pe;
    return true;
  }
  // a RESAMPLE'd slice's own sample (an alt): by its path
  for (int vi = 0; vi < kVoices; ++vi) {
    VS& x = vs[vi];
    if (x.map < 0 || maps[x.map].state != kMReady) continue;
    const Map& mp = maps[x.map].m;
    for (int a = 0; a < mp.nalt; ++a) {
      if (x.alt_src[a] >= 0) continue;
      lib::Ref r;
      if (!lib::ref_for_path(mp.alt[a], &r)) {
        memset(&r, 0, sizeof r);
        sniprintf(r.hint, sizeof r.hint, "%s", mp.alt[a]);
      }
      int s = src_find(r, mp.alt[a]);
      if (s >= 0 && srcs[s].ready) { x.alt_src[a] = s; continue; }
      if (s >= 0) continue;
      s = src_free();
      if (s < 0) return false;
      src_drop(s);
      int pe = pcache::want(r, mp.alt[a]);
      if (pe < 0) continue;
      Src& sr = srcs[s];
      sr.kind = kSrcFile, sr.ref = r, sr.pe = pe;
      sniprintf(sr.path, sizeof sr.path, "%s", mp.alt[a]);
      job->kind = kJLoad, job->slot = s, job->pe = pe;
      return true;
    }
  }
  return false;
}
CORE_COLD void trig_now(int vi, int note, int vel, bool seq_note);
CORE_COLD bool step_load() {
  Src& sr = srcs[job->slot];
  if (!pcache::is(job->pe, sr.ref)) {   // (the entry moved on)
    src_drop(job->slot);
    job->kind = kJIdle;
    return true;
  }
  pcache::State s = pcache::state(job->pe);
  if (s == pcache::kLoading || (s == pcache::kReady && pcache::busy(job->pe))) return pcache::step();
  pcache::Info in;
  if (s == pcache::kReady && pcache::get(job->pe, &in) && in.whole) {
    pcache::pin(in.blk);
    sr.blk = in.blk;
    src_ready(job->slot, in.pcm, in.frames, (uint32_t)in.rate, in.ch);
    ++st.loads;
    if (in.cut) log(CORE_STR("chop: %s: longer than a source holds: its first %lu s"), sr.path, (unsigned long)(in.frames / (in.rate > 0 ? (uint32_t)in.rate : 48000)));
    for (int vi = 0; vi < kVoices; ++vi) {
      VS& x = vs[vi];
      if (x.code >= 0 && x.src < 0 && x.buf_src < 0) {
        const pools::Entry& e = pools::at(pools::kSamples, x.code);
        if (same_ref(e.ref, sr.ref) || !strcasecmp(e.path, sr.path)) x.src = job->slot;
      }
      if (x.map >= 0)
        for (int a = 0; a < maps[x.map].m.nalt; ++a)
          if (x.alt_src[a] < 0 && !strcasecmp(maps[x.map].m.alt[a], sr.path)) x.alt_src[a] = job->slot;
      if (x.src == job->slot && x.map >= 0) map_fit(x.map, job->slot);
      if (x.pend.on && x.src >= 0 && x.map >= 0 && maps[x.map].state == kMReady) {
        x.pend.on = false;
        if (now_ms() - x.pend.ms <= kLateMs) trig_now(vi, x.pend.note, x.pend.vel, x.pend.seq);
        else ++st.late;
      }
    }
  } else {
    ++st.load_fails;
    log(CORE_STR("chop: %s: not a sample it reads (or no room in the pool RAM)"), sr.path);
    for (int vi = 0; vi < kVoices; ++vi) {
      VS& x = vs[vi];
      if (x.code < 0) continue;
      const pools::Entry& e = pools::at(pools::kSamples, x.code);
      if (same_ref(e.ref, sr.ref) || !strcasecmp(e.path, sr.path)) x.fail_code = x.code, x.pend.on = false;
    }
    src_drop(job->slot);
  }
  job->kind = kJIdle;
  return true;
}
CORE_COLD bool step_map_read() {
  MapSlot& x = maps[job->slot];
  switch (job->io) {
    case 0:
      if (!cardq::async(&job->rq, cardq::kOpenReq, &job->f, job->path, 0, nullptr, 0)) return true;
      if (job->rq.got != 1) {
        job->kind = kJIdle;
        map_read_done(job->slot, false, 0);
        return true;
      }
      job->io = 1, job->rq.state = cardq::kFree;
      return true;
    case 1:
      if (!cardq::async(&job->rq, cardq::kReadReq, &job->f, nullptr, 0, (uint8_t*)text, kMapText)) return true;
      job->got = job->rq.got > 0 ? (uint32_t)job->rq.got : 0;
      job->io = 2, job->rq.state = cardq::kFree;
      return true;
    default:
      if (!cardq::async(&job->rq, cardq::kCloseReq, &job->f, nullptr, 0, nullptr, 0)) return true;
      job->kind = kJIdle;
      text[job->got] = 0;
      map_read_done(job->slot, job->got > 0 && job->got < kMapText, job->got);
      (void)x;
      return true;
  }
}
CORE_COLD bool step_map_write() {
  MapSlot& x = maps[job->slot];
  if (!cardq::async(&job->rq, cardq::kAtomicReq, nullptr, job->path, 0, (uint8_t*)text, job->got)) return true;
  job->kind = kJIdle;
  if (job->rq.got < 0) {
    ++st.map_fails;
    x.dirty = true, x.dirty_ms = now_ms();   // (tried again later)
    log(CORE_STR("chop: cannot write %s"), job->path);
    return true;
  }
  ++st.maps_written;
  lib::hash_now(job->path);   // the library hashes the new file before the rest (its ref, the pool's row)
  if (!x.saved_once || x.inst < 0) {
    int out = -1;
    int r = pools::add_take(pools::kInstr, job->path, &out);
    if ((r == pools::kOk || r == pools::kExisting) && out >= 0) x.inst = out;
    log(CORE_STR("chop: %s written (%d slices), INSTRUMENTS: %s"), job->path, x.m.n, pools::result_text(r));
  } else {
    x.accept = true;   // the row's ref takes the new content once the library has hashed it
  }
  x.saved_once = true;
  return true;
}
CORE_COLD bool start_map_job() {
  for (int m = 0; m < kMaps; ++m) {
    MapSlot& x = maps[m];
    if (x.state == kMFind) return map_find_step(m);
    if (x.state == kMRead) {
      job->kind = kJMapRead, job->slot = m, job->io = 0, job->rq.state = cardq::kFree;
      sniprintf(job->path, sizeof job->path, "%s", x.found);
      x.state = kMReading;   // (map_read_done: kMReady)
      return true;
    }
  }
  if (!card_ok) return false;
  for (int m = 0; m < kMaps; ++m) {
    MapSlot& x = maps[m];
    if (x.state != kMReady || !x.dirty || x.ram || x.on_busy || now_ms() - x.dirty_ms < kSaveMs || !x.m.frames) continue;
    if (!x.file[0]) map_path(x, x.file, sizeof x.file);
    int n = write(x.m, text, (int)kMapText);
    if (n <= 0) {
      x.dirty = false, ++st.map_fails;
      log(CORE_STR("chop: %s: the map does not fit %lu bytes"), x.file, (unsigned long)kMapText);
      continue;
    }
    x.dirty = false;
    job->kind = kJMapWrite, job->slot = m, job->got = (uint32_t)n, job->rq.state = cardq::kFree;
    sniprintf(job->path, sizeof job->path, "%s", x.file);
    return true;
  }
  return false;
}
CORE_COLD bool transient_step() {
  for (int m = 0; m < kMaps; ++m) {
    MapSlot& x = maps[m];
    if (!x.on_busy) continue;
    int s = -1;
    for (int vi = 0; vi < kVoices && s < 0; ++vi)
      if (vs[vi].map == m) s = vs[vi].buf_src >= 0 ? vs[vi].buf_src : vs[vi].src;
    if (s < 0 || !srcs[s].ready) { x.on_busy = false; continue; }
    const Src& sr = srcs[s];
    uint32_t to = x.on.pos + kTransientPiece;
    onsets_feed(&x.on, sr.pcm, sr.ch, sr.frames, to > sr.frames ? sr.frames : to);
    if (x.on.pos + 256 > sr.frames) {
      g_before = x.m;
      onsets_apply(x.on, &x.m, sr.pcm, sr.ch);
      x.on_busy = false;
      if (memcmp(&g_before, &x.m, sizeof x.m)) x.undo = g_before, x.has_undo = true;   // (UNDO: only when it changed)
      map_touch(m);
      log(CORE_STR("chop: TRANSIENT: %d slices"), x.m.n);
    }
    return true;
  }
  return false;
}
// a changed row's content accepted (the map rewritten into its own file: the pool's ref follows)
CORE_COLD void accept_step() {
  for (int m = 0; m < kMaps; ++m) {
    MapSlot& x = maps[m];
    if (!x.accept || x.inst < 0) continue;
    const pools::Entry& e = pools::at(pools::kInstr, x.inst);
    if (e.type != pools::kInstrument || strcasecmp(e.ref.hint, x.file)) { x.accept = false; continue; }
    if (e.st == lib::kChanged) pools::accept_changed(pools::kInstr, x.inst), x.accept = false;
  }
}
CORE_COLD void find_inst_row(int m) {   // the row a map's file went into (add_take's .sfz read finishes later)
  MapSlot& x = maps[m];
  if (x.inst >= 0 || !x.file[0] || !x.saved_once) return;
  for (int c = 0; c < pools::kCodes; ++c) {
    const pools::Entry& e = pools::at(pools::kInstr, c);
    if (e.type == pools::kInstrument && (!strcasecmp(e.ref.hint, x.file) || !strcasecmp(e.path, x.file))) {
      x.inst = c;
      return;
    }
  }
}

// ---- notes
CORE_COLD void post(Note& n, const Start& s) {
  uint32_t q = n.seq;
  st_rlx(&n.seq, q + 1);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  n.st = s;
  st_rel(&n.seq, q + 2);
}
CORE_COLD int pick(int vi) {
  Note* ns = notes[vi];
  for (int k = 0; k < kPerVoice; ++k)
    if (note_free(ns[k])) return k;
  int b = -1;
  for (int k = 0; k < kPerVoice; ++k)
    if (!ns[k].held && (b < 0 || (int32_t)(ns[k].order - ns[b].order) < 0)) b = k;
  if (b < 0)
    for (int k = 0; k < kPerVoice; ++k)
      if (b < 0 || (int32_t)(ns[k].order - ns[b].order) < 0) b = k;
  ++st.steals;
  return b;
}
CORE_COLD void release_note(Note& n) {
  if (!n.held || note_free(n)) {
    n.held = false;
    return;
  }
  n.held = false;
  st_rel(&n.rel_n, n.rel_n + 1);
}
CORE_COLD void kill_note(Note& n) {
  n.held = n.live = false;
  if (!note_free(n)) st_rel(&n.kill_n, n.kill_n + 1);
}
// The knobs' overrides on slice sl (P9..P16: fully down the slice's own)
CORE_COLD void knob_overrides(const float* p, Slice* s, uint32_t rate) {
  int z = zone(p[8], 16);   // STUT: SLC, then 2..16
  if (z > 0) s->stut = (uint8_t)(z + 1);
  if (p[9] >= 1) {          // SLEN: 5 ms .. 1 s as a share of the slice
    float ms = 5.0f * powf(200.0f, clamp01((p[9] - 1) / 126.0f));
    uint32_t len = s->end > s->start ? s->end - s->start : 1;
    int pct = (int)(ms * (float)(rate ? rate : 48000) / 1000.0f * 100.0f / (float)len + 0.5f);
    s->stut_len = (uint8_t)clampi(pct, 1, 100);
  }
  z = zone(p[10], 4);       // TAPE: SLC OFF STOP SPIN
  if (z > 0) s->tape = (uint8_t)(z - 1);
  if (p[11] >= 1) s->tape_ms = (uint16_t)(20.0f * powf(200.0f, clamp01((p[11] - 1) / 126.0f)));
  if (p[12] >= 1) s->atk_ms = (uint16_t)(0.5f + 2000.0f * powf(clamp01((p[12] - 1) / 126.0f), 2.0f));
  if (p[13] >= 1) s->dec_ms = p[13] >= 127 ? 0 : (uint16_t)(10.0f * powf(1000.0f, clamp01((p[13] - 1) / 125.0f)));
  z = zone(p[14], 3);       // DIR: SLC FWD REV
  if (z > 0) s->rev = z == 2;
  z = zone(p[15], 4);       // LOOP: SLC OFF FWD PING
  if (z > 0) s->loop = (uint8_t)(z - 1);
}
// slice k of the voice's map as a note's Start; false: not playable now
CORE_COLD bool make_start(int vi, int k, int vel, bool knobs, Start* o) {
  VS& x = vs[vi];
  if (x.map < 0 || maps[x.map].state != kMReady) return false;
  const Map& mp = maps[x.map].m;
  if (k < 0 || k >= mp.n) return false;
  int main = x.buf_src >= 0 ? x.buf_src : x.src;
  Slice s = mp.s[k];
  int src = s.alt >= 0 ? x.alt_src[(int)s.alt] : main;
  if (src < 0 || !srcs[src].ready) return false;
  const Src& sr = srcs[src];
  uint32_t a, b;
  if (s.alt >= 0) a = 0, b = sr.frames;
  else slice_span(mp, k, &a, &b);
  if (b > sr.frames) b = sr.frames;
  if (a + 2 > b) return false;
  if (knobs) knob_overrides(x.p, &s, sr.rate);
  memset(o, 0, sizeof *o);
  o->src = (int8_t)src, o->src2 = -1;
  o->a = a, o->b = b;
  o->rev = s.rev, o->loop = s.loop, o->tape = s.tape, o->stut = s.stut < 1 ? 1 : s.stut, o->choke = s.choke;
  float tune = knobs ? (float)clampi((int)lroundf((x.p[2] - 64.0f) * 24.0f / 63.0f), -24, 24) : 0.0f;
  float semis = (float)s.pitch + (float)s.fine / 100.0f + tune;
  o->inc = (float)sr.rate / 48000.0f * powf(2.0f, semis / 12.0f);
  float vv = (vel < 1 ? 1 : vel > 127 ? 127 : vel) / 127.0f;
  float sens = knobs ? clamp01(x.p[6] / 127.0f) : 0.5f;
  float lev = knobs ? x.p[7] / 127.0f : 0.8f;
  o->gain = powf(10.0f, (float)s.gain / 20.0f) * (1.0f - sens + sens * vv * vv) * lev;
  uint32_t len = b - a;
  o->stut_len = (float)len * (float)(s.stut_len ? s.stut_len : 25) / 100.0f;
  if (o->stut_len < 64) o->stut_len = 64;
  o->stut_g = 1.0f - (float)s.stut_dec / 100.0f;
  o->tape_r0 = o->inc * (float)s.tape_ms * 48.0f * 0.5f;
  if (o->tape_r0 > (float)len * 0.95f) o->tape_r0 = (float)len * 0.95f;
  float af = (float)(s.atk_ms ? s.atk_ms : 0) * 48.0f;
  o->atk = af < 24.0f ? 1.0f / 24.0f : 1.0f / af;
  o->dec_k = s.dec_ms ? expf(-6.9077553f / ((float)s.dec_ms * 48.0f)) : 1.0f;
  // MORPH's partner: the right-hand neighbour (the last slice: its left one), the same source only
  if (mp.n > 1) {
    int k2 = k + 1 < mp.n ? k + 1 : k - 1;
    const Slice& s2 = mp.s[k2];
    int src2 = s2.alt >= 0 ? x.alt_src[(int)s2.alt] : main;
    if (src2 >= 0 && srcs[src2].ready) {
      uint32_t a2, b2;
      if (s2.alt >= 0) a2 = 0, b2 = srcs[src2].frames;
      else slice_span(mp, k2, &a2, &b2);
      if (b2 > srcs[src2].frames) b2 = srcs[src2].frames;
      if (a2 + 2 <= b2) o->src2 = (int8_t)src2, o->a2 = a2, o->b2 = b2;
    }
  }
  return true;
}
CORE_COLD void start_note(int vi, const Start& s0, bool live) {
  if (count_active() >= kTotal) {
    bool any = false;
    for (int k = 0; k < kPerVoice; ++k) any = any || !note_free(notes[vi][k]);
    if (!any) return;
  }
  if (s0.choke)   // CHOKE: the voice's other notes of the group cut
    for (int k = 0; k < kPerVoice; ++k) {
      Note& n = notes[vi][k];
      if (!note_free(n) && n.st.choke == s0.choke) kill_note(n), ++st.chokes;
    }
  int k = pick(vi);
  Note& n = notes[vi][k];
  Start s = s0;
  s.rel_gen = n.rel_n, s.kill_gen = n.kill_n;
  n.held = true;
  n.live = live && cur_live.on, n.ch = cur_live.ch, n.mnote = cur_live.mnote;
  n.order = ++order_n;
  post(n, s);
  ++st.notes;
  int a = count_active();
  st.active = a;
  if (a > st.peak) st.peak = a;
  v[vi].live = true;
}
// the slice a hit picks, before SHUFFLE / NEIGHBOUR: a sequencer note is the slice; a Rytm trig / pad SLC + (note - 60)
CORE_COLD int hit_slice(int vi, int note, bool seq_note) {
  VS& x = vs[vi];
  if (x.map < 0 || maps[x.map].state != kMReady || maps[x.map].m.n <= 0) return -1;
  int n = maps[x.map].m.n;
  int k = seq_note ? note : zone(x.p[1], 64) + (note - kKeyRoot);
  k %= n;
  return k < 0 ? k + n : k;
}
CORE_COLD int choose(int vi, int k) {
  VS& x = vs[vi];
  const Map& mp = maps[x.map].m;
  uint32_t seed = (uint32_t)zone(x.p[4], 128);
  if (x.shuf_n != mp.n || x.shuf_seed != seed) shuffle(seed, mp.n, x.order), x.shuf_n = mp.n, x.shuf_seed = seed;
  int j = x.order[k];
  int knob = (int)(clamp01(x.p[5] / 127.0f) * 100.0f + 0.5f);
  int own = mp.s[j].nbr;
  int pct = 100 - (100 - own) * (100 - knob) / 100;
  if (pct > 0) {
    int j2 = neighbour(seed ? seed : 0x5EEDu, vi, j, mp.n, x.hits[j]++, pct);
    if (j2 != j) ++st.nbr_moves;
    j = j2;
  }
  return j;
}
CORE_COLD void trig_now(int vi, int note, int vel, bool seq_note) {
  int k = hit_slice(vi, note, seq_note);
  if (k < 0) return;
  int j = choose(vi, k);
  Start s;
  if (!make_start(vi, j, vel, true, &s)) return;
  start_note(vi, s, false);
  vs[vi].last = j;
  ++st.trigs;
  log(CORE_STR("trig voice %d CHOP slice %d of %d (note %d%s)"), vi, j + 1, maps[vs[vi].map].m.n, note, seq_note ? ", seq" : "");
}
// a hit waiting for the sample / its map (within kLateMs), or played now
CORE_COLD bool ready_now(int vi) {
  VS& x = vs[vi];
  int s = x.buf_src >= 0 ? x.buf_src : x.src;
  return s >= 0 && srcs[s].ready && x.map >= 0 && maps[x.map].state == kMReady;
}
CORE_COLD void hit(int vi, int note, int vel, bool seq_note) {
  if (!ready_now(vi)) {
    VS& x = vs[vi];
    if (x.code < 0 && x.buf_src < 0) return;
    x.pend = {true, (uint8_t)note, (uint8_t)vel, seq_note, now_ms()};
    ++st.waits;
    return;
  }
  trig_now(vi, note, vel, seq_note);
}
CORE_COLD void set_source(int vi) {
  VS& x = vs[vi];
  if (x.buf_src >= 0) return;
  int code = srclib::code_of(x.p[0]);
  const pools::Entry& e = pools::at(pools::kSamples, code);
  bool file = e.type == pools::kSample && pools::playable(e) && e.path[0];
  if (v[vi].cp || x.force) pools::voice_on(pools::kSamples, vi, file ? code : -1);
  if (!file) {
    if (x.code >= 0) x.code = -1, x.src = -1, x.map = -1;
    return;
  }
  bool same = x.code == code && x.map >= 0 && (x.src < 0 || !strcasecmp(srcs[x.src].path, e.path));
  if (same) return;
  x.code = code, x.src = -1, x.map = -1, x.fail_code = -1;
  for (int a = 0; a < kMaxAlts; ++a) x.alt_src[a] = -1;
  int s = src_find(e.ref, e.path);
  if (s >= 0 && srcs[s].ready) x.src = s;
  x.map = map_for(vi, code);
  memset(x.hits, 0, sizeof x.hits);
  x.shuf_n = -1;
  if (x.src >= 0 && x.map >= 0) map_fit(x.map, x.src);
}
// The Rytm's tracks on CHOP (its 33) and their SRC (the SRC view's 2A or our own 48): the sample and its map in before
// the first report / trig - a choice in the machine list and a turn of SRC load it (no trig waits kLateMs for it then)
CORE_COLD void preload_step() {
  static uint32_t last = 0;
  uint32_t now = now_ms();
  if (now - last < 100) return;
  last = now;
  bool on[kVoices] = {};
  for (int t = 0; t < 12; ++t) {
    int vb = ui::remote_track_machine(t), vi = ui::remote_track_voice(t);
    if (vb < 0 || vb == 0x7F || (vb & 0x78) != kChopMachine || vi < 0 || vi >= kVoices) continue;
    on[vi] = true;
    VS& x = vs[vi];
    int c14 = ui::remote_track_src14(t);
    if (c14 < 0 || (x.reported && v[vi].cp)) continue;   // (a voice that reports itself: its own SRC)
    int code = srclib::code_of14(c14);
    x.p[0] = (float)(code & 127) + (float)srclib::lo_of(code) / 128.0f;
    set_source(vi);
  }
  for (int vi = 0; vi < kVoices; ++vi) vs[vi].pre = on[vi];
}
CORE_COLD MapSlot* edit_slot(int vi, bool undo) {
  if (vi < 0 || vi >= kVoices || !job) return nullptr;
  VS& x = vs[vi];
  if (x.map < 0 || maps[x.map].state != kMReady) return nullptr;
  MapSlot& m = maps[x.map];
  if (undo) g_before = m.m;   // review minor: UNDO's copy kept only once the edit is made (edited)
  return &m;
}
CORE_COLD void edited(MapSlot* m) {   // an edit made on edit_slot(vi, true)'s map: UNDO takes it back
  m->undo = g_before, m->has_undo = true;
  map_touch((int)(m - maps));
}
CORE_COLD const Src* voice_src(int vi) {
  int s = vs[vi].buf_src >= 0 ? vs[vi].buf_src : vs[vi].src;
  return s >= 0 && srcs[s].ready ? &srcs[s] : nullptr;
}

}  // namespace

// ---- the control core's API
CORE_COLD void begin(bool card) {
  card_ok = card;
  if (!begun) {
    begun = true;
    job = new Job();
    srcs = new Src[kSrcSlots];
    maps = new MapSlot[kMaps];
    text = new char[kMapText + 1];
    ov_mem = (int8_t*)msamp_ram_alloc((uint64_t)kSrcSlots * 2 * kOvBins);   // 512 KB (the board's high heap)
    memset(ov_mem, 0, (size_t)kSrcSlots * 2 * kOvBins);
    for (int s = 0; s < kSrcSlots; ++s) memset(&srcs[s], 0, sizeof(Src)), srcs[s].blk = srcs[s].pe = -1;
    for (int m = 0; m < kMaps; ++m) memset(&maps[m], 0, sizeof(MapSlot));
    for (int vi = 0; vi < kVoices; ++vi)
      for (int k = 0; k < kPerVoice; ++k) st_rel(&notes[vi][k].reads, kReadsNone);
  }
  pcache::begin(pool_ram_budget());
  job->kind = kJIdle;
  for (int vi = 0; vi < kVoices; ++vi) {
    VS& x = vs[vi];
    bool f = x.force;
    memset(&x, 0, sizeof x);
    x.force = f;
    x.code = x.src = x.map = x.buf_src = x.fail_code = -1, x.last = -1, x.shuf_n = -1;
    for (int a = 0; a < kMaxAlts; ++a) x.alt_src[a] = -1;
    x.p[0] = 10, x.p[2] = 64, x.p[6] = 64, x.p[7] = 100;   // (before a report: SRC A010, TUNE 0, VEL 50 %, LEV 100)
    for (int k = 0; k < kPerVoice; ++k) g_head[vi][k] = -1;
  }
}

CORE_COLD bool step() {
  if (!job) return false;
  preload_step();
  accept_step();
  for (int m = 0; m < kMaps; ++m) find_inst_row(m);
  // RESAMPLE: the slice played out (and the FX's tail): the take stopped; the take in the pool: the offer
  if (rs.state == kRsPlaying && (int32_t)(now_ms() - rs.until_ms) >= 0) {
    samp::stop();
    rs.state = kRsWriting;
  }
  if (rs.state == kRsWriting) {
    samp::Status ss;
    samp::status(&ss);
    if (ss.state == samp::kFailed) rs.state = kRsFailed, log(CORE_STR("chop: RESAMPLE failed: %s"), ss.error);
    else if (ss.row[0] && (ss.state == samp::kDone || ss.state == samp::kIdle)) {
      sniprintf(rs.row, sizeof rs.row, "%s", ss.row);
      sniprintf(rs.wav, sizeof rs.wav, "%s", ss.wav);
      rs.state = kRsOffer;
      log(CORE_STR("chop: RESAMPLE of slice %d -> %s (%s): REPLACE? (chop=replace:yes|no)"), rs.k + 1, rs.row, rs.wav);
    } else if (now_ms() - rs.started_ms > 60000) {
      rs.state = kRsFailed;
    }
  }
  for (int vi = 0; vi < kVoices; ++vi)   // a pending hit whose wait ran out
    if (vs[vi].pend.on && now_ms() - vs[vi].pend.ms > kLateMs && !ready_now(vi)) vs[vi].pend.on = false, ++st.late;
  switch (job->kind) {
    case kJLoad: return step_load();
    case kJMapRead: return step_map_read();
    case kJMapWrite: return step_map_write();
    default: break;
  }
  if (start_map_job()) return true;
  for (int m = 0; m < kMaps; ++m)   // a map read: its sample may be in already
    for (int vi = 0; vi < kVoices; ++vi)
      if (vs[vi].map == m && maps[m].state == kMReady && !maps[m].m.frames) {
        const Src* s = voice_src(vi);
        if (s) map_fit(m, (int)(s - srcs));
      }
  for (int vi = 0; vi < kVoices; ++vi) {   // pending hits whose map / sample came
    VS& x = vs[vi];
    if (x.pend.on && ready_now(vi)) {
      x.pend.on = false;
      trig_now(vi, x.pend.note, x.pend.vel, x.pend.seq);
    }
  }
  if (transient_step()) return true;
  if (start_load()) return true;
  if (overview_step()) return true;
  return pcache::step();
}

CORE_COLD bool forced(int vi) { return vs[vi & 7].force; }

CORE_COLD void report(int vi, int note, int vel, const float* p) {
  vi &= 7;
  if (!job) return;
  VS& x = vs[vi];
  for (int k = 0; k < 16; ++k) x.p[k] = p[k];
  g_morph[vi] = clamp01(p[3] / 127.0f);
  x.reported = true;
  v[vi].live = true;
  set_source(vi);
  if (x.code >= 0) pools::seen(pools::kSamples, x.code);
  if (note < 0) return;
  if (x.lazy.on) {   // LAZY: a trig is a tap
    lazy_tap(vi);
    return;
  }
  hit(vi, note, vel, false);
}

CORE_COLD void live_on(int vi, int key, int vel, int ch, int mnote) {
  vi &= 7;
  if (!job || key < 0 || key > 127) return;
  VS& x = vs[vi];
  if (x.code < 0 && x.buf_src < 0) set_source(vi);
  bool seq_note = ch == poly::kSeqCh;
  if (x.lazy.on && !seq_note && ch != kPreviewCh) {
    lazy_tap(vi);
    return;
  }
  if (!ready_now(vi)) {
    hit(vi, key, vel, seq_note);
    return;
  }
  int k = ch == kPreviewCh ? key : hit_slice(vi, key, seq_note);
  if (k < 0) return;
  int j = ch == kPreviewCh ? k : choose(vi, k);
  Start s;
  if (!make_start(vi, j, vel, ch != kPreviewCh, &s)) return;
  cur_live = {true, (int8_t)ch, (uint8_t)mnote};
  start_note(vi, s, true);
  cur_live = {false, 0, 0};
  x.last = j;
  if (ch != kPreviewCh && !seq_note) log(CORE_STR("live voice %d CHOP slice %d (key %d, channel %d)"), vi, j + 1, key, ch + 1);
}

CORE_COLD void live_off(int vi, int ch, int mnote) {
  vi &= 7;
  for (int k = 0; k < kPerVoice; ++k) {
    Note& n = notes[vi][k];
    if (!n.live || n.ch != ch || (mnote >= 0 && n.mnote != mnote)) continue;
    n.live = false;
    release_note(n);
  }
}

CORE_COLD void seq_hit(int vi, int note, int vel) {
  vi &= 7;
  if (!job) return;
  if (vs[vi].code < 0 && vs[vi].buf_src < 0) set_source(vi);
  hit(vi, note, vel, true);
}
CORE_COLD int voice_notes(int vi) {
  int a = 0;
  for (int k = 0; k < kPerVoice; ++k) a += !note_free(notes[vi & 7][k]);
  return a;
}
CORE_COLD void note_off(int vi) {
  vi &= 7;
  vs[vi].pend.on = false;
  for (int k = 0; k < kPerVoice; ++k)
    if (!notes[vi][k].live) release_note(notes[vi][k]);
}
CORE_COLD void silence(int vi) {
  vi &= 7;
  vs[vi].pend.on = false;
  if (vs[vi].lazy.on) vs[vi].lazy.on = false;
  for (int k = 0; k < kPerVoice; ++k) kill_note(notes[vi][k]);
}

// ---- the editors' view
CORE_COLD const Map* map_of(int vi) {
  if (!job || vi < 0 || vi >= kVoices) return nullptr;
  int m = vs[vi].map;
  return m >= 0 && maps[m].state == kMReady ? &maps[m].m : nullptr;
}
CORE_COLD int slices(int vi) {
  const Map* m = map_of(vi);
  return m ? m->n : 0;
}
CORE_COLD int slice_of_hit(int vi, int note, bool seq_note) { return job ? hit_slice(vi & 7, note, seq_note) : -1; }
CORE_COLD int played_slice(int vi) { return vs[vi & 7].last; }
CORE_COLD bool sample_ready(int vi) { return job && voice_src(vi & 7) != nullptr; }
CORE_COLD uint32_t sample_frames(int vi) {
  const Src* s = job ? voice_src(vi & 7) : nullptr;
  return s ? s->frames : 0;
}
CORE_COLD uint32_t sample_rate(int vi) {
  const Src* s = job ? voice_src(vi & 7) : nullptr;
  return s ? s->rate : 48000;
}
CORE_COLD int outline(int vi, uint32_t a, uint32_t b, int cols, float* mn, float* mx) {
  const Src* s = job ? voice_src(vi & 7) : nullptr;
  if (!s || cols <= 0 || b <= a) return 0;
  if (b > s->frames) b = s->frames;
  if (a >= b) return 0;
  double span = (double)(b - a) / cols;
  for (int c = 0; c < cols; ++c) {
    uint32_t i0 = a + (uint32_t)(span * c), i1 = a + (uint32_t)(span * (c + 1));
    if (i1 <= i0) i1 = i0 + 1;
    if (i1 > b) i1 = b;
    int lo = 0, hi = 0;
    if (span >= 2.0 * kBins && s->ov_done >= (i1 + kBins - 1) / kBins) {   // the overview's bins
      for (uint32_t k = i0 / kBins; k < (i1 + kBins - 1) / kBins && k < s->nbins; ++k) {
        int l = s->ov[2 * k] * 256, h = s->ov[2 * k + 1] * 256;
        lo = l < lo ? l : lo, hi = h > hi ? h : hi;
      }
    } else {   // a zoom closer than the bins (or before they are built): the frames, strided to 64 a column at most
      uint32_t stp = (i1 - i0) / 64 + 1;
      for (uint32_t i = i0; i < i1; i += stp) {
        int x = s->ch == 2 ? ((int)s->pcm[2 * i] + s->pcm[2 * i + 1]) >> 1 : s->pcm[i];
        lo = x < lo ? x : lo, hi = x > hi ? x : hi;
      }
    }
    mn[c] = (float)lo / 32768.0f, mx[c] = (float)hi / 32768.0f;
  }
  return cols;
}
CORE_COLD int playheads(int vi, int32_t* pos, int max) {
  int n = 0;
  for (int k = 0; k < kPerVoice && n < max; ++k) {
    int32_t h = g_head[vi & 7][k];
    if (h >= 0) pos[n++] = h;
  }
  return n;
}
CORE_COLD uint32_t map_gen(int vi) {
  int m = job ? vs[vi & 7].map : -1;
  return m >= 0 ? maps[m].gen * 2 + (maps[m].state == kMReady) : 0;
}
CORE_COLD const char* map_file(int vi) {
  int m = job ? vs[vi & 7].map : -1;
  return m >= 0 && maps[m].saved_once ? maps[m].file : "";
}
CORE_COLD int map_row(int vi) {
  int m = job ? vs[vi & 7].map : -1;
  return m >= 0 ? maps[m].inst : -1;
}
CORE_COLD bool map_dirty(int vi) {
  int m = job ? vs[vi & 7].map : -1;
  return m >= 0 && maps[m].dirty;
}

// ---- the edits
CORE_COLD bool edit_grid(int vi, int n) {
  MapSlot* m = edit_slot(vi, true);
  const Src* s = m ? voice_src(vi) : nullptr;
  if (!s || n < 2 || n > kMaxSlices) return false;
  grid(&m->m, n, s->pcm, s->ch);
  edited(m);
  return true;
}
CORE_COLD bool edit_transient(int vi, int sens) {
  MapSlot* m = edit_slot(vi, true);
  if (!m || !voice_src(vi)) return false;
  onsets_begin(&m->on, sens);
  m->on_busy = true;
  return true;
}
CORE_COLD bool transient_busy(int vi) {
  int m = job ? vs[vi & 7].map : -1;
  return m >= 0 && maps[m].on_busy;
}
CORE_COLD bool edit_add(int vi, uint32_t frame) {
  MapSlot* m = edit_slot(vi, true);
  const Src* s = m ? voice_src(vi) : nullptr;
  if (!s) return false;
  uint32_t f = snap(s->pcm, s->ch, s->frames, frame);
  if (!add_marker(&m->m, f)) return false;
  edited(m);
  return true;
}
CORE_COLD bool edit_del(int vi, int k) {
  MapSlot* m = edit_slot(vi, true);
  if (!m || m->m.n <= 1 || !del(&m->m, k)) return false;
  edited(m);
  return true;
}
CORE_COLD bool edit_merge(int vi, int k) {
  MapSlot* m = edit_slot(vi, true);
  if (!m || !merge(&m->m, k)) return false;
  edited(m);
  return true;
}
CORE_COLD bool edit_split(int vi, int k, uint32_t frame) {
  MapSlot* m = edit_slot(vi, true);
  const Src* s = m ? voice_src(vi) : nullptr;
  if (!s || k < 0 || k >= m->m.n) return false;
  if (!frame) frame = m->m.s[k].start + (m->m.s[k].end - m->m.s[k].start) / 2;
  if (!split(&m->m, k, snap(s->pcm, s->ch, s->frames, frame))) return false;
  edited(m);
  return true;
}
CORE_COLD bool edit_start(int vi, int k, int32_t frame) {
  MapSlot* m = edit_slot(vi, true);
  if (!m || !move_start(&m->m, k, frame)) return false;
  edited(m);
  return true;
}
CORE_COLD bool edit_end(int vi, int k, int32_t frame) {
  MapSlot* m = edit_slot(vi, true);
  if (!m || !move_end(&m->m, k, frame)) return false;
  edited(m);
  return true;
}
CORE_COLD bool edit_field(int vi, int k, int f, int value) {
  MapSlot* m = edit_slot(vi, true);
  if (!m || k < 0 || k >= m->m.n || f < 0 || f >= kFields) return false;
  field_set(&m->m.s[k], f, value);
  edited(m);
  return true;
}
CORE_COLD bool edit_undo(int vi) {
  MapSlot* m = edit_slot(vi, false);
  if (!m || !m->has_undo) return false;
  Map t = m->m;
  m->m = m->undo, m->undo = t;   // (UNDO again: the edit back)
  map_touch((int)(m - maps));
  return true;
}

// ---- LAZY
CORE_COLD int latency() {
  if (g_lat >= 0) return g_lat;
  // the sound leaves the engine, waits in the output FIFO, crosses the device to its outputs (about half its own
  // OUT -> IN loop), the player taps, the key comes back (kKeyFrames) [inferred; chop=lat:N sets it]
  return livein::fifo() + out::device().loop_frames / 2 + kKeyFrames;
}
CORE_COLD void set_latency(int frames) { g_lat = frames < 0 ? -1 : frames > 9600 ? 9600 : frames; }
CORE_COLD bool lazy_arm(int vi) {
  vi &= 7;
  const Src* s = job ? voice_src(vi) : nullptr;
  if (!s || vs[vi].map < 0 || maps[vs[vi].map].state != kMReady) return false;
  VS& x = vs[vi];
  for (int k = 0; k < kPerVoice; ++k) kill_note(notes[vi][k]);
  lazy_begin(&x.lazy);
  Start st0;
  memset(&st0, 0, sizeof st0);
  st0.src = (int8_t)(s - srcs), st0.src2 = -1, st0.a = 0, st0.b = s->frames;
  st0.inc = (float)s->rate / 48000.0f, st0.gain = 0.8f, st0.stut = 1, st0.atk = 1.0f / 24.0f, st0.dec_k = 1.0f;
  st0.lazy = true;
  x.lazy_inc = st0.inc;
  st_rlx(&g_lazy_t[vi], 0);
  start_note(vi, st0, false);
  log(CORE_STR("chop: LAZY on voice %d: the sample plays; tap the pads / trigs (latency %d frames)"), vi, latency());
  return true;
}
CORE_COLD int32_t lazy_tap(int vi) {
  vi &= 7;
  VS& x = vs[vi];
  const Src* s = job ? voice_src(vi) : nullptr;
  if (!s || !x.lazy.on) return -1;
  bool playing = false;   // the LAZY player still sounding (a tap after the sample's end is refused)
  for (int k = 0; k < kPerVoice && !playing; ++k) playing = !note_free(notes[vi][k]) && notes[vi][k].st.lazy;
  if (!playing) {
    ++x.lazy.refused, ++st.lazy_refused;
    return -1;
  }
  uint32_t played = ld_acq(&g_lazy_t[vi]);
  uint32_t lat = (uint32_t)((float)latency() * x.lazy_inc + 0.5f);
  int32_t at = chop::lazy_tap(&x.lazy, played, lat, s->pcm, s->ch, s->frames);
  if (at < 0) {
    ++st.lazy_refused;
    if (x.lazy.n >= kMaxSlices) log(CORE_STR("chop: LAZY: 64 slices already: a tap ignored"));
  } else {
    ++st.lazy_taps;
    log(CORE_STR("chop: LAZY tap %d: marker at %ld (played %lu, latency %lu)"), x.lazy.n, (long)at, (unsigned long)played,
        (unsigned long)lat);
  }
  return at;
}
CORE_COLD bool lazy_undo(int vi) { return vs[vi & 7].lazy.on && chop::lazy_undo(&vs[vi & 7].lazy); }
CORE_COLD bool lazy_stop(int vi, bool keep) {
  vi &= 7;
  VS& x = vs[vi];
  if (!x.lazy.on) return false;
  x.lazy.on = false;
  for (int k = 0; k < kPerVoice; ++k) kill_note(notes[vi][k]);
  if (!keep || !x.lazy.n) return true;
  MapSlot* m = edit_slot(vi, true);
  if (!m) return false;
  lazy_apply(x.lazy, &m->m);
  edited(m);
  char b[160];
  int n = 0;
  for (int k = 0; k < m->m.n && n < (int)sizeof b - 12; ++k) n += sniprintf(b + n, sizeof b - n, " %lu", (unsigned long)m->m.s[k].start);
  b[n] = 0;
  log(CORE_STR("chop: LAZY: %d slices at%s"), m->m.n, b);
  return true;
}
CORE_COLD bool lazy_on(int vi) { return job && vs[vi & 7].lazy.on; }
CORE_COLD const Lazy* lazy_state(int vi) { return job ? &vs[vi & 7].lazy : nullptr; }
CORE_COLD uint32_t lazy_playhead(int vi) { return ld_acq(&g_lazy_t[vi & 7]); }
CORE_COLD void audition(int vi, int k) {
  vi &= 7;
  if (!job || vs[vi].lazy.on) return;
  live_off(vi, kPreviewCh, -1);
  live_on(vi, k, 110, kPreviewCh, k);
  for (int j = 0; j < kPerVoice; ++j)   // (an audition plays its slice to its end: no note-off)
    if (notes[vi][j].live && notes[vi][j].ch == kPreviewCh) notes[vi][j].live = false, notes[vi][j].held = false;
}

// ---- RESAMPLE
CORE_COLD bool resample(int vi, int k, bool fx) {
  vi &= 7;
  // review M5: one take at a time - an offer not yet answered (YES / NO) holds the next one off
  if (!job || rs.state == kRsPlaying || rs.state == kRsWriting || rs.state == kRsOffer) return false;
  const Map* mp = map_of(vi);
  const Src* s = voice_src(vi);
  if (!mp || !s || k < 0 || k >= mp->n) return false;
  Start st0;
  if (!make_start(vi, k, 127, true, &st0)) return false;
  const samp::Config was = samp::config();   // review M4: the user's sampler settings, back after the arm
  samp::Config c = was;
  c.src = samp::kSrcVoice, c.sub = (uint8_t)vi, c.start = samp::kManual, c.pre_ms = 0, c.bars = 0;
  c.trim = false, c.normalise = false, c.slices = samp::kNoSlices;
  float secs = (float)(st0.b - st0.a) / (st0.inc > 0.01f ? st0.inc : 0.01f) / 48000.0f;   // its time at its speed
  if (st0.stut > 1) secs += secs * (float)(st0.stut - 1) * 0.5f;
  uint32_t ms = (uint32_t)(secs * 1000.0f) + (fx ? 1500 : 60);
  c.max_s = ms / 1000 + 2;
  sniprintf(c.name, sizeof c.name, "%.20s-S%d", mp->name[0] ? mp->name : "CHOP", k + 1);
  for (char* q = c.name; *q; ++q)
    if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '-')) *q = '_';
  c.fx = fx;
  samp::configure(c);
  char err[64];
  bool armed = samp::arm(err, sizeof err);   // (arm takes its own copy of the settings)
  samp::configure(was);
  if (!armed) {
    log(CORE_STR("chop: RESAMPLE: the sampler said %s"), err);
    return false;
  }
  for (int j = 0; j < kPerVoice; ++j) kill_note(notes[vi][j]);
  start_note(vi, st0, false);
  rs.state = kRsPlaying, rs.vi = vi, rs.k = k, rs.fx = fx, rs.row[0] = rs.wav[0] = 0;
  rs.map = vs[vi].map, rs.a = mp->s[k].start, rs.b = mp->s[k].end;
  rs.started_ms = now_ms(), rs.until_ms = now_ms() + ms;
  ++st.resamples;
  log(CORE_STR("chop: RESAMPLE slice %d of voice %d%s: %lu ms"), k + 1, vi, fx ? " with its send FX" : "", (unsigned long)ms);
  return true;
}
CORE_COLD int resample_state(int* slice, char* row) {
  if (slice) *slice = rs.k;
  if (row) sniprintf(row, 8, "%s", rs.row);
  return rs.state;
}
CORE_COLD bool resample_answer(bool yes) {
  if (rs.state != kRsOffer && rs.state != kRsFailed) return false;
  bool done = false;
  if (yes && rs.state == kRsOffer) {
    // review M5: the take goes to the slice it was taken from - the same map, the slice still where it was
    const VS& x = vs[rs.vi];
    const Map* now = x.map >= 0 ? &maps[x.map].m : nullptr;
    if (x.map != rs.map || !now || rs.k >= now->n || now->s[rs.k].start != rs.a || now->s[rs.k].end != rs.b) {
      log(CORE_STR("chop: RESAMPLE: slice %d changed meanwhile: the take kept as %s, not used"), rs.k + 1, rs.wav);
      rs.state = kRsIdle;
      return false;
    }
    MapSlot* m = edit_slot(rs.vi, true);
    if (m && rs.k >= 0 && rs.k < m->m.n) {
      Map& mp = m->m;
      int a = -1;
      for (int i = 0; i < mp.nalt && a < 0; ++i)
        if (!strcmp(mp.alt[i], rs.wav)) a = i;
      if (a < 0 && mp.nalt < kMaxAlts) a = mp.nalt++, sniprintf(mp.alt[a], kPathLen, "%s", rs.wav);
      if (a >= 0) {
        Slice& sl = mp.s[rs.k];
        sl.alt = (int8_t)a;   // its own sample now: played whole at its settings (pitch, gain, the tools: kept)
        sl.pitch = sl.fine = 0, sl.rev = 0, sl.stut = 1, sl.tape = kTapeOff, sl.nudge = 0;   // (baked into the take)
        vs[rs.vi].alt_src[a] = -1;
        edited(m);
        done = true;
        log(CORE_STR("chop: slice %d now plays %s"), rs.k + 1, rs.wav);
      }
    }
  }
  rs.state = kRsIdle;
  return done || !yes;
}

CORE_COLD bool use_buffer(int vi, const int16_t* pcm, uint32_t frames, int ch, uint32_t rate) {
  vi &= 7;
  if (!job) return false;
  VS& x = vs[vi];
  for (int k = 0; k < kPerVoice; ++k) kill_note(notes[vi][k]);
  if (x.buf_src >= 0) {   // (its notes may still fade: the slot is dropped once nothing reads it, at a later reuse)
    int s = x.buf_src;
    x.buf_src = -1;
    if (!src_busy(s)) src_drop(s);
  }
  if (x.map >= 0 && maps[x.map].ram) maps[x.map].state = kMFree;
  x.map = -1;
  if (!pcm || !frames) {
    x.code = -1;
    set_source(vi);
    return true;
  }
  int s = src_free();
  if (s < 0) return false;
  src_drop(s);
  srcs[s].kind = kSrcRam;
  src_ready(s, pcm, frames, rate, ch);
  x.buf_src = s;
  int m = map_new();
  if (m < 0) return false;
  MapSlot& mp = maps[m];
  memset(&mp, 0, sizeof mp);
  mp.ram = true, mp.state = kMReady, mp.inst = -1, mp.need_grid = true, mp.used_ms = now_ms();
  sniprintf(mp.m.name, sizeof mp.m.name, "BUFFER V%d", vi + 1);
  x.map = m;
  map_fit(m, s);
  memset(x.hits, 0, sizeof x.hits);
  x.shuf_n = -1;
  return true;
}

CORE_COLD const Stats& stats() {
  st.active = count_active();
  return st;
}
CORE_COLD void reset() {
  if (!job) return;
  for (int vi = 0; vi < kVoices; ++vi) {
    silence(vi);
    for (int k = 0; k < kPerVoice; ++k) {
      Note& n = notes[vi][k];
      st_rel(&n.done, ld_rlx(&n.seq));
      n.r_on = false, n.r_seq = n.seq, n.tail.on = false;
      st_rel(&n.reads, kReadsNone);
      g_head[vi][k] = -1;
    }
  }
  for (int s = 0; s < kSrcSlots; ++s) src_drop(s);
  for (int m = 0; m < kMaps; ++m) memset(&maps[m], 0, sizeof(MapSlot));
  memset(&st, 0, sizeof st);
  rs.state = kRsIdle;
  g_lat = -1;
  begin(card_ok);
}
CORE_COLD uint32_t render_allocs_check() { return ld_acq(&g_blocks); }

// ---- the console
CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "chop", 4) || (p[4] && p[4] != '=')) return false;
  if (!job) {
    sniprintf(out, cap, "chop: not started");
    return true;
  }
  if (!p[4]) {
    const Stats& s = stats();
    int n = sniprintf(out, cap, "chop: trigs %lu notes %lu (now %d, peak %d) steals %lu chokes %lu waits %lu late %lu "
                                "loads %lu fails %lu maps read %lu written %lu new %lu fails %lu lazy %lu/%lu nbr %lu "
                                "resample %lu lat %d",
                      (unsigned long)s.trigs, (unsigned long)s.notes, s.active, s.peak, (unsigned long)s.steals,
                      (unsigned long)s.chokes, (unsigned long)s.waits, (unsigned long)s.late, (unsigned long)s.loads,
                      (unsigned long)s.load_fails, (unsigned long)s.maps_read, (unsigned long)s.maps_written,
                      (unsigned long)s.maps_new, (unsigned long)s.map_fails, (unsigned long)s.lazy_taps,
                      (unsigned long)s.lazy_refused, (unsigned long)s.nbr_moves, (unsigned long)s.resamples, latency());
    for (int vi = 0; vi < kVoices && n < cap; ++vi) {
      const Map* m = map_of(vi);
      if (!(v[vi].cp || vs[vi].force) || !m) continue;
      n += sniprintf(out + n, cap - n, "; V%d %s %d slices%s%s", vi + 1, m->name, m->n, map_dirty(vi) ? " (unsaved)" : "",
                     vs[vi].lazy.on ? " LAZY" : "");
    }
    return true;
  }
  const char* a = p + 5;
  int vi = 0, k = 0, x = 0;
  auto word = [&](const char* w) {
    size_t l = strlen(w);
    if (strncmp(a, w, l) || (a[l] && a[l] != ':')) return false;
    a += l + (a[l] == ':');
    return true;
  };
  auto voice = [&]() {
    if (!parse_int(&a, &vi) || vi < 0 || vi > 7) return false;
    if (*a == ':') ++a;
    return true;
  };
  auto num = [&](int* o) {
    if (!parse_int(&a, o)) return false;
    if (*a == ':') ++a;
    return true;
  };
  bool ok = true;
  if (word("voice")) {
    if (!voice()) { sniprintf(out, cap, "chop=voice:V[:off]"); return true; }
    bool off = !strcmp(a, "off");
    vs[vi].force = !off;
    if (off) silence(vi);
    sniprintf(out, cap, "chop: voice %d %s", vi, off ? "back to its own machine" : "plays CHOP");
    return true;
  }
  if (word("lat")) {
    if (!strcmp(a, "auto")) set_latency(-1);
    else if (num(&x)) set_latency(x);
    sniprintf(out, cap, "chop: LAZY latency %d frames%s", latency(), g_lat < 0 ? " (estimate)" : "");
    return true;
  }
  if (word("replace")) {
    bool y = !strcmp(a, "yes");
    ok = resample_answer(y);
    sniprintf(out, cap, "chop: replace %s: %s", y ? "yes" : "no", ok ? "done" : "nothing offered");
    return true;
  }
  if (word("grid")) ok = voice() && num(&x) && edit_grid(vi, x);
  else if (word("trans")) ok = voice() && num(&x) && edit_transient(vi, x);
  else if (word("add")) ok = voice() && num(&x) && edit_add(vi, (uint32_t)x);
  else if (word("del")) ok = voice() && num(&k) && edit_del(vi, k - 1);
  else if (word("merge")) ok = voice() && num(&k) && edit_merge(vi, k - 1);
  else if (word("split")) ok = voice() && num(&k) && (num(&x) || (x = 0, true)) && edit_split(vi, k - 1, (uint32_t)x);
  else if (word("start")) ok = voice() && num(&k) && num(&x) && edit_start(vi, k - 1, x);
  else if (word("end")) ok = voice() && num(&k) && num(&x) && edit_end(vi, k - 1, x);
  else if (word("undo")) ok = voice() && edit_undo(vi);
  else if (word("play")) ok = voice() && num(&k) && (audition(vi, k - 1), true);
  else if (word("set")) {
    ok = voice() && num(&k);
    char fn[12] = "";
    int i = 0;
    while (ok && *a && *a != ':' && i < 11) fn[i++] = *a++;
    fn[i] = 0;
    if (ok && *a == ':') ++a;
    int f = field_of(fn);
    ok = ok && f >= 0 && num(&x) && edit_field(vi, k - 1, f, x);
  } else if (word("lazy")) {
    ok = voice();
    if (ok && !strcmp(a, "arm")) ok = lazy_arm(vi);
    else if (ok && !strcmp(a, "tap")) ok = lazy_tap(vi) >= 0;
    else if (ok && !strcmp(a, "undo")) ok = lazy_undo(vi);
    else if (ok && !strcmp(a, "stop")) ok = lazy_stop(vi, true);
    else if (ok && !strcmp(a, "cancel")) ok = lazy_stop(vi, false);
    else ok = false;
  } else if (word("resample")) {
    ok = voice() && num(&k);
    bool fx = ok && !strcmp(a, "fx");
    ok = ok && resample(vi, k - 1, fx);
  } else if (word("map")) {
    if (!voice()) { sniprintf(out, cap, "chop=map:V"); return true; }
    const Map* m = map_of(vi);
    if (!m) { sniprintf(out, cap, "chop: voice %d has no map yet", vi); return true; }
    int n = sniprintf(out, cap, "chop: V%d %s %lu frames %lu Hz %d ch, %d slices%s%s:", vi + 1, m->name, (unsigned long)m->frames,
                      (unsigned long)m->rate, m->ch, m->n, map_file(vi)[0] ? " in " : "", map_file(vi));
    for (int i = 0; i < m->n && n < cap; ++i)
      n += sniprintf(out + n, cap - n, " %d:%lu-%lu", i + 1, (unsigned long)m->s[i].start, (unsigned long)m->s[i].end);
    return true;
  } else {
    sniprintf(out, cap, "chop: chop=voice:V[:off] grid:V:N trans:V:SENS add:V:FRAME del|merge:V:K split:V:K[:FRAME] "
                        "start|end:V:K:FRAME set:V:K:FIELD:VALUE undo:V play:V:K lazy:V:arm|tap|undo|stop|cancel "
                        "resample:V:K[:fx] replace:yes|no lat:N|auto map:V");
    return true;
  }
  sniprintf(out, cap, "chop: %s", ok ? "ok" : "refused");
  return true;
}

// ---- the render core's
namespace {
inline float rd(const int16_t* p, uint8_t ch, uint32_t frames, double pos, float* r) {
  if (pos < 0) pos = 0;
  uint32_t i = (uint32_t)pos;
  float f = (float)(pos - (double)i);
  uint32_t j = i + 1 < frames ? i + 1 : i;
  if (i >= frames) i = j = frames - 1;
  if (ch == 2) {
    float l = (float)p[2 * i] + f * (float)(p[2 * j] - p[2 * i]);
    *r = (float)p[2 * i + 1] + f * (float)(p[2 * j + 1] - p[2 * i + 1]);
    return l;
  }
  float m = (float)p[i] + f * (float)(p[j] - p[i]);
  *r = m;
  return m;
}
inline float edge(float x, float len) {   // 1 inside, a 1 ms ramp at both ends of a segment (0 outside)
  if (x < 0 || x > len) return 0;
  float a = x / kEdge, b = (len - x) / kEdge;
  float e = a < b ? a : b;
  return e > 1 ? 1 : e;
}
// one note's frames added into l / r (r null: mono, the pair summed); false when it ended
bool render_note(int vi, int k, Note& n, float* l, float* r, int cnt) {
  const Start& s = n.r;
  const float len = (float)(s.b - s.a);
  const float len2 = s.src2 >= 0 ? (float)(s.b2 - s.a2) : 0;
  float m = s.src2 >= 0 ? g_morph[vi] : 0.0f;
  float gm1 = 1.0f, gm2 = 0.0f;
  if (m > 0.0005f) gm1 = cosf(m * 1.5707963f), gm2 = sinf(m * 1.5707963f);
  const float sc = 1.0f / 32768.0f;
  bool alive = true;
  int i = 0;
  for (; i < cnt; ++i) {
    // the envelope
    if (n.r_kill) {
      n.kill_g -= 1.0f / kKillFrames;
      if (n.kill_g <= 0) { alive = false; break; }
    }
    if (n.stage == 0) {
      n.env += s.atk;
      if (n.env >= 1) n.env = 1, n.stage = 1;
    } else if (n.stage == 1) {
      if (s.dec_k < 1) {
        n.env *= s.dec_k;
        if (n.env < 0.001f) { alive = false; break; }
      }
    } else {
      n.env *= n.rel_k;
      if (n.env < 0.0001f) { alive = false; break; }
    }
    // where it reads: u along the slice (STUTTER's repeats over its first part)
    bool last_seg = n.rep >= (int)s.stut - 1;
    float seg = last_seg ? len : (s.stut_len < len ? s.stut_len : len);
    float u = (float)n.u;
    float g = n.r.gain * n.env * n.kill_g * n.rep_g;
    if (s.loop != kLoopPing || !last_seg) g *= edge(u, seg);
    double pos = s.rev ? (double)s.b - 1.0 - n.u : (double)s.a + n.u;
    float xr, xl = rd(n.p1, n.c1, n.f1, pos, &xr);
    if (gm2 > 0) {   // MORPH: the neighbour at the same place (time-aligned starts), equal power
      float yr = 0, yl = 0;
      if (u < len2) {
        double p2 = s.rev ? (double)s.b2 - 1.0 - n.u : (double)s.a2 + n.u;
        float e2 = edge(u, len2);
        yl = rd(n.p2, n.c2, n.f2, p2, &yr) * e2, yr *= e2;
      }
      xl = xl * gm1 + yl * gm2, xr = xr * gm1 + yr * gm2;
    }
    xl *= g * sc, xr *= g * sc;
    if (r) l[i] += xl, r[i] += xr;
    else l[i] += 0.5f * (xl + xr);
    // the speed: TAPE STOP falls to zero over the end, SPIN-UP rises from zero at the start
    float spd = s.inc;
    if (s.tape == kTapeStop && s.loop == kLoopOff && last_seg) {
      float rem = len - u;
      if (rem < s.tape_r0) {
        spd = s.inc * sqrtf(rem > 0 ? rem / s.tape_r0 : 0);
        if (spd < s.inc * 0.004f) { alive = false; ++i; break; }
      }
    } else if (s.tape == kTapeSpin && n.rep == 0 && u < s.tape_r0) {
      float q = sqrtf(u / s.tape_r0);
      spd = s.inc * (q < 0.03f ? 0.03f : q);
    }
    if (s.loop == kLoopPing && last_seg) {
      n.u += n.dir * spd;
      if (n.u >= len - 1) n.u = 2.0 * (len - 1) - n.u, n.dir = -1;
      if (n.u <= 0) n.u = -n.u, n.dir = 1;
      if (n.u > len - 1) n.u = len - 1;
    } else {
      n.u += spd;
      if (n.u >= seg) {
        if (!last_seg) {
          n.u -= seg, ++n.rep, n.rep_g *= s.stut_g;
        } else if (s.loop == kLoopFwd) {
          n.u -= len;
          if (n.u < 0) n.u = 0;
        } else {
          alive = false;
          ++i;
          break;
        }
      }
    }
  }
  int32_t head = alive ? (int32_t)(s.rev ? (double)s.b - 1.0 - n.u : (double)s.a + n.u) : -1;
  __atomic_store_n(&g_head[vi][k], head, __ATOMIC_RELAXED);
  if (s.lazy) st_rel(&g_lazy_t[vi], (uint32_t)n.u);
  return alive;
}
void render_tail(Tail& t, float* l, float* r, int cnt) {
  const float sc = 1.0f / 32768.0f;
  for (int i = 0; i < cnt && t.on; ++i) {
    t.g -= t.fade;
    if (t.g <= 0) { t.on = false; break; }
    float xr, xl = rd(t.pcm, t.ch, t.frames, t.pos, &xr);
    t.pos += t.inc;
    if (t.pos >= t.frames - 1 || t.pos < 0) { t.on = false; break; }
    xl *= t.g * sc, xr *= t.g * sc;
    if (r) l[i] += xl, r[i] += xr;
    else l[i] += 0.5f * (xl + xr);
  }
}
void render_voice(int vi, float* l, float* r, int cnt) {
  if (!srcs) return;
  __atomic_fetch_add(&g_blocks, 1u, __ATOMIC_RELAXED);
  for (int k = 0; k < kPerVoice; ++k) {
    Note& n = notes[vi][k];
    uint32_t q = ld_acq(&n.seq);
    if (q != n.r_seq && !(q & 1)) {   // a new start (through the sequence lock)
      Start s = n.st;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (ld_rlx(&n.seq) == q) {
        uint32_t was = ld_rlx(&n.reads);
        int tsrc = n.tail.on ? (int)(was >> 16 & 0xFF) : 0xFF;
        if (n.r_on && n.p1) {   // the player was busy: its sound fades out as a tail
          Tail& t = n.tail;
          tsrc = (int)(was & 0xFF);
          t.pcm = n.p1, t.ch = n.c1, t.frames = n.f1;
          t.pos = n.r.rev ? (double)n.r.b - 1.0 - n.u : (double)n.r.a + n.u;
          t.inc = n.r.rev ? -n.r.inc : n.r.inc;
          t.g = n.r.gain * n.env * n.kill_g * n.rep_g, t.fade = t.g / kKillFrames, t.on = t.g > 0;
        }
        // M3: what it reads now (written before the player's pointers move on: the control core never sees a gap)
        st_rel(&n.reads, reads_word(s.src, s.src2, n.tail.on ? tsrc : 0xFF));
        n.r = s, n.r_seq = q, n.r_on = true;
        const Src& a = srcs[(int)s.src];
        n.p1 = a.pcm, n.f1 = a.frames, n.c1 = a.ch;
        if (s.src2 >= 0) {
          const Src& b = srcs[(int)s.src2];
          n.p2 = b.pcm, n.f2 = b.frames, n.c2 = b.ch;
        } else {
          n.p2 = nullptr, n.f2 = 0, n.c2 = 1;
        }
        n.u = 0, n.dir = 1, n.rep = 0, n.rep_g = 1, n.env = 0, n.stage = 0, n.kill_g = 1;
        n.r_rel = n.r_kill = false;
        n.rel_k = expf(-9.2103404f / kRelFrames);
        if (!n.p1 || n.r.b <= n.r.a) {   // (nothing to play: its sources read no more)
          n.r_on = false;
          st_rel(&n.reads, (ld_rlx(&n.reads) & 0xFF0000u) | 0xFFFFu);
          st_rel(&n.done, q);
        }
      }
    }
    if (n.tail.on) {
      render_tail(n.tail, l, r, cnt);
      if (!n.tail.on) st_rel(&n.reads, (ld_rlx(&n.reads) & 0xFFFFu) | 0xFF0000u);   // M3: its source free again
    }
    if (!n.r_on) continue;
    if (!n.r_kill && ld_acq(&n.kill_n) != n.r.kill_gen) n.r_kill = true;
    if (!n.r_rel && ld_acq(&n.rel_n) != n.r.rel_gen) {
      n.r_rel = true;
      if (n.r.loop != kLoopOff) n.stage = 2;   // (a one-shot plays on to its end / its decay)
    }
    if (!render_note(vi, k, n, l, r, cnt)) {
      n.r_on = false;
      st_rel(&n.reads, (ld_rlx(&n.reads) & 0xFF0000u) | 0xFFFFu);   // M3: the player reads nothing more
      __atomic_store_n(&g_head[vi][k], -1, __ATOMIC_RELAXED);
      st_rel(&n.done, n.r_seq);
    }
  }
}
}  // namespace

void render_add(int vi, float* out, int n) { render_voice(vi & 7, out, nullptr, n); }
void render_add2(int vi, float* l, float* r, int n) { render_voice(vi & 7, l, r, n); }
#endif

}  // namespace chop
}  // namespace core
