// core/granular/granular.cpp - see granular.h. Everything but render_add and live_in is the control core's and cold.
#include "../platform.h"

#if CORE_MSAMP
#include "granular.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "../fx/out.h"
#include "../lfo.h"
#include "../lib/pools.h"
#include "../pool/pcache.h"
#include "../lib/srclib.h"
#include "../msamp/cardq.h"
#include "../pool/pool.h"
#include "../poly.h"
#include "../samp/out.h"
#include "../voices.h"
#include "../machines.h"
#include "../ui/remote.h"
#include "../wavetable/wav_read.h"

namespace core {
namespace gr {

namespace {

inline uint32_t ld_acq(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline uint32_t ld_rlx(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
inline void st_rel(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline void st_rlx(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

constexpr int kPath = 160;
constexpr int kFixed = kLive + kBuiltins;     // entries before the card's
constexpr int kSrcIds = kFixed + kSlots;      // source ids: live 0..9, built-ins 10..11, card slots 12..
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr int kWin = 512, kWins = 4;
// Click-free edges (docs/GRANULAR.md "Clicks"): every grain fades in and out over at least kEdge samples (2 ms, half
// the grain when it is shorter) on top of its window; a grain whose note is stolen or killed, or whose source is
// switched, fades out over kOrphan (3 ms) instead of stopping; the live ring's seams (a thaw, a new source) fade over
// kSeam; the note's envelope is smoothed over ~0.7 ms (no corner at the release's start).
constexpr float kEdge = 96.0f, kOrphan = 144.0f, kEnvSmooth = 0.03f;
constexpr uint32_t kSeam = 96;
constexpr uint32_t kBuiltinFrames = 4 * 48000;

// ---- sources
struct Src {
  int16_t* data;
  volatile uint32_t frames;   // 0 while (re)loading
  float rate;
  volatile uint8_t ch;        // 1 mono, 2 stereo interleaved (the pool RAM's layout)
};
Src srcs[kSrcIds];            // live ids unused (their rings are per voice)
struct Slot {
  int entry;
  bool ready;
  uint32_t used_ms;
  char path[kPath];            // the file it holds (a rescan keeps it when the list still has it, by path and size)
  uint32_t size;
  int blk;                     // its pool RAM block (core/pool/pcache.h: the shared sample cache), pinned (-1 none)
};
Slot slots[kSlots];
// The list: the project's SAMPLES pool (core/lib/pools.h), entry = code (bank * 128 + row: SRC's 14-bit word). An
// input row plays its live source (Entry::fixed 0..9), a built-in PAD / BELLS (fixed 0, 1: source ids 10, 11), a sample
// row its resolved file (loaded into a card slot on use); OFF, empty and missing rows are silent.
using Entry = srclib::Entry;   // grp as shown: "BANK A".."BANK H", "LIVE", "BUILT-IN"
Entry* list = nullptr;         // the list the voices and displays read (one of lbuf[2]: compose swaps them)
Entry* lbuf[2] = {nullptr, nullptr};
int nlist = 0;                 // srclib::kEntries once composed
uint32_t composed_sig = 0;     // pools::sig the list was composed with
volatile uint32_t g_list_gen = 1;   // a new list (the displays copy it again)
uint8_t failed[srclib::kEntries];
uint8_t why[srclib::kEntries];    // entry_note's: 0 none, 1 cut to the cache's kMaxFrames, 2 not a WAV, 5 no room
uint32_t secs10[srclib::kEntries];   // its length in 0.1 s once loaded (0 unknown)
bool reported[kVoices];           // a report (the Rytm's SRC) came for the voice
// a preview (the list's audition): a short cloud of the entry on the voice
struct Preview {
  bool wait, on;
  uint32_t ms;
};
Preview pv[kVoices];
bool preview_enabled = true;
constexpr int kPreviewCh = 126;        // its live note's channel tag (no MIDI channel uses it)
constexpr uint32_t kPreviewMs = 900, kPreviewWaitMs = 3000;
float win[kWins][kWin + 1];

// ---- the live capture (one writer: live_in)
int16_t (*capture)[kLive] = nullptr;   // [kCap][kLive]
volatile uint32_t cap_w = 0;           // frames written in all
volatile bool want_live = false;
volatile uint32_t g_live_frames = 0;

// ---- notes
struct Start {
  float key, gain;
  uint32_t rel_gen, kill_gen, seed;
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
  bool r_on, r_rel;
  Start r;
  float env, envs;             // the envelope, and as played (smoothed)
  uint8_t stage;               // 0 attack 1 held 2 release
  double scan;                 // card: the playhead's offset from POS (source frames)
  float next;                  // samples to the next grain
  uint32_t rnd;
};
Note notes[kVoices][kPerVoice];
uint32_t order_n = 0;

struct Grain {
  double pos;                  // source frame (live: absolute ring frame)
  float inc, wph, winc, gain;
  float ek;                    // the edge fade's slope: min(wph, 1 - wph) x ek < 1 inside the fades
  float oenv, ofade, odec;     // an orphan (odec > 0): its note's envelope when it was cut off, fading 1 -> 0
  uint8_t note, win;
  uint16_t s0;                 // the block's sample it starts at (its first block)
  bool on, old;                // old: reads the voice's previous source (a switch: fading out)
  float pl, pr;                // a stereo voice's (OUT PRE / POST) pan gains for this grain (centre 1, 1)
};
Grain* grains = nullptr;       // [kVoices][kMaxGrains]
volatile uint32_t g_grains[kVoices];   // sounding now, per voice (the render core's)
volatile uint32_t g_started = 0, g_capped = 0;
int grain_cap = kDefaultCap;

// the voice's live ring (the render core's)
float* rings = nullptr;        // [kVoices][kRing]
uint32_t ring_w[kVoices];      // frames written
uint32_t cap_rd[kVoices];      // where it reads the capture
int ring_src[kVoices];         // the live source it records (-1 none)
float fbb[kVoices][kBlock];    // its last block (feedback)

struct Knobs {
  int src;                     // the entry
  float pos, spray_s, spr, size_s, dens, prnd, rev, atk, rel, fb;
  float scan;                  // card: the playhead's speed (1 = 1x; HOLD slows it, 0 frozen)
  float anim, anim_beats;      // ANIM: its depth 0..1, its cycle in beats (0 off)
  int win;
  bool freeze;
};
Knobs knobs[kVoices];
float vp[kVoices][16];
volatile uint32_t sel[kVoices];   // the source id voice vi plays (kNone: none)
volatile uint32_t use[kVoices];
volatile uint32_t prev[kVoices];  // the source a switch fades out of (its grains still read it; kNone: none)
// the render core's: what a voice's fading grains read after a switch
struct OldSrc {
  const int16_t* data;
  uint32_t len;
  bool live, any;
  uint8_t ch;
};
OldSrc old_src[kVoices];
// the live ring's seams (the render core's)
bool was_frozen[kVoices];
uint32_t seam_in[kVoices];     // new frames still to fade in
// ANIM (the render core's): the clock position it runs on (24 ppqn ticks), the head it put the cloud at (0..1)
double anim_ticks[kVoices];
volatile float g_head[kVoices];
volatile float g_back[kVoices];    // live: the head's distance back (0..1 of what the ring holds)
int16_t want[kVoices];
bool force[kVoices];
struct Pend {
  bool on;
  uint8_t note, vel;
  uint32_t ms;
};
Pend pend[kVoices];
struct LiveTag {
  bool on;
  int8_t ch;
  uint8_t mnote;
};
LiveTag cur_live = {false, 0, 0};
Stats st;
bool card_ok = false, begun = false;
uint32_t rnd = 0x2545f491u;

CORE_COLD float anim_beats(float v) {
  if (v < 1) return 0;
  int z = (int)v / 16;
  return 32.0f / (float)(1 << (z > 7 ? 7 : z));   // 32 16 8 4 2 1 1/2 1/4 beats
}

// ---- the job: a source's audio comes from the pool RAM (core/pool/pcache.h): one decoded copy a content, shared
// with DSP SAMPLE and MULTISAMPLE; its load (through the card core, never this core's time) is the cache's. A slot here
// is a pinned view of a cache block, swapped in when no voice reads the slot (free_slot): one store of its frames.
enum Phase : uint8_t { kIdle, kLoadWait };
struct Job {
  Phase phase;
  int entry;                   // the list entry waited for
  int pe;                      // its cache entry
};
Job* job = nullptr;

CORE_COLD bool note_free(const Note& n) { return ld_acq(&n.done) == ld_rlx(&n.seq); }
CORE_COLD int count_active() {
  int a = 0;
  for (int vi = 0; vi < kVoices; ++vi)
    for (int k = 0; k < kPerVoice; ++k) a += !note_free(notes[vi][k]);
  return a;
}
CORE_COLD uint32_t frand_u() { return rnd = rnd * 1664525u + 1013904223u; }

// the source id an entry plays without a card slot: an input's live source 0..9, a built-in's 10, 11; -1 a card file
CORE_COLD int fixed_src(int e) {
  if (!list || e < 0 || e >= nlist) return -1;
  const Entry& x = list[e];
  if (x.fixed < 0 || x.lib != srclib::kLibNone) return -1;
  return x.type == pools::kInput ? x.fixed : x.fixed < kBuiltins ? kLive + x.fixed : -1;
}
CORE_COLD bool is_live(int e) {
  int f = fixed_src(e);
  return f >= 0 && f < kLive;
}
CORE_COLD bool src_busy(uint32_t id) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  for (int vi = 0; vi < kVoices; ++vi)
    if (__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST) == id || __atomic_load_n(&use[vi], __ATOMIC_SEQ_CST) == id ||
        __atomic_load_n(&prev[vi], __ATOMIC_SEQ_CST) == id)
      return true;
  return false;
}
CORE_COLD int resident(int e) {
  if (e < 0 || e >= nlist) return -1;
  int f = fixed_src(e);
  if (f >= 0) return f;
  if (!list[e].path[0]) return -1;
  for (int s = 0; s < kSlots; ++s)   // by path: the same file listed twice (the pool and its folder) is one slot
    if (slots[s].ready && (slots[s].entry == e || (list[e].path[0] && !strcasecmp(slots[s].path, list[e].path)))) return kFixed + s;
  return -1;
}
CORE_COLD float tsec(float v) {
  v = v < 0 ? 0 : v > 127 ? 127 : v;
  return 0.001f * powf(10.0f, v / 127.0f * 4.0f);
}
CORE_COLD float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

CORE_COLD bool parse_int(const char** p, int* v) {   // [spaces] [-] digits (no strtol on the Pi)
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
CORE_COLD void set_knobs(int vi) {
  const float* p = vp[vi];
  Knobs& k = knobs[vi];
  int e = srclib::code_of(p[0]);   // the pool row: bank * 128 + row (an OFF / empty / missing row: silent, a trig skips it)
  k.src = e;
  if (v[vi].gr || force[vi]) pools::voice_on(pools::kSamples, vi, e);
  k.pos = clamp01(p[1] / 127.0f);
  float s = clamp01(p[2] / 127.0f);
  k.spray_s = 2.0f * s * s;
  k.spr = s;
  k.size_s = 0.005f * powf(200.0f, clamp01(p[3] / 127.0f));
  k.dens = powf(200.0f, clamp01(p[4] / 127.0f));
  k.prnd = 12.0f * clamp01(p[5] / 127.0f);
  k.rev = clamp01(p[6] / 127.0f);
  k.win = eng_zone(p[7], kWins);
  k.atk = tsec(p[8]);
  k.rel = tsec(p[9]);
  // P11 HOLD: 0..119 live - the feedback 0 .. 0.95; card - the playhead's speed 1x .. 0; 120..127 FREEZE
  k.freeze = p[10] >= kHoldFreeze;
  float hold = k.freeze ? 1.0f : clamp01(p[10] / (float)(kHoldFreeze - 1));
  k.fb = k.freeze ? 0.0f : 0.95f * hold;
  k.scan = k.freeze ? 0.0f : 1.0f - hold;
  // P12 ANIM: 0 off; its depth grows with the knob, its cycle steps from 8 bars to 1/16 (synced to the clock)
  k.anim = clamp01(p[11] / 127.0f);
  k.anim_beats = anim_beats(p[11]);
  bool any = false;
  for (int i = 0; i < kVoices; ++i) any = any || (is_live(knobs[i].src) && (v[i].gr || force[i]));
  want_live = any;
}
CORE_COLD void select(int vi) {
  int e = knobs[vi].src;
  want[vi] = (int16_t)e;
  int t = resident(e);
  if (t >= kFixed) slots[t - kFixed].used_ms = now_ms();
  uint32_t was = __atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST);   // switched away from: still fading out (free_slot)
  if (t >= 0 && was != (uint32_t)t && was >= (uint32_t)kFixed && was < (uint32_t)kSrcIds) slots[was - kFixed].used_ms = now_ms();
  if (t >= 0) __atomic_store_n(&sel[vi], (uint32_t)t, __ATOMIC_SEQ_CST);
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
  int b = -1;
  for (int k = 0; k < kPerVoice; ++k)
    if (note_free(ns[k])) return k;
  for (int k = 0; k < kPerVoice; ++k)
    if (!ns[k].held && (b < 0 || (int32_t)(ns[k].order - ns[b].order) < 0)) b = k;
  if (b < 0)
    for (int k = 0; k < kPerVoice; ++k)
      if (b < 0 || (int32_t)(ns[k].order - ns[b].order) < 0) b = k;
  ++st.steals;
  return b;
}
CORE_COLD void start_note(int vi, int key, int vel, float gain) {
  if (count_active() >= kTotal) {
    bool any = false;
    for (int k = 0; k < kPerVoice; ++k) any = any || !note_free(notes[vi][k]);
    if (!any) return;
  }
  int k = pick(vi);
  Note& n = notes[vi][k];
  Start x;
  x.key = (float)key;
  float vv = (vel < 1 ? 1 : vel) / 127.0f;
  x.gain = gain * (0.25f + 0.75f * vv * vv);
  x.rel_gen = n.rel_n;
  x.kill_gen = n.kill_n;
  x.seed = frand_u();
  n.held = true;
  n.live = cur_live.on, n.ch = cur_live.ch, n.mnote = cur_live.mnote;
  n.key = (uint8_t)key;
  n.order = ++order_n;
  post(n, x);
  ++st.notes;
  int a = count_active();
  st.active = a;
  if (a > st.peak) st.peak = a;
}
CORE_COLD void release_note(Note& n) {
  if (!n.held || note_free(n)) {
    n.held = false;
    return;
  }
  n.held = false;
  st_rel(&n.rel_n, n.rel_n + 1);
}
CORE_COLD void trig(int vi, int note, int vel) {
  for (int k = 0; k < kPerVoice; ++k)
    if (!notes[vi][k].live) release_note(notes[vi][k]);
  if (list && srclib::silent(list, knobs[vi].src)) {   // rui1038: a bank's OFF / empty row
    ++st.trigs;
    return;
  }
  poly::Chord c = poly::chord_of(vp[vi]);
  uint8_t ks[poly::kMaxNotes];
  int nk = poly::expand(note, c, ks);
  for (int i = 0; i < nk; ++i) start_note(vi, ks[i], vel, 1.0f);
  ++st.trigs;
  v[vi].live = true;
  char b[8];
  log(CORE_STR("trig voice %d GRANULAR %s note %d %s%s%s: %d notes (%d now, peak %d)"), vi, list[knobs[vi].src].name,
      note, c.chd ? poly::kChordNames[c.chd] : "", c.chd ? " VOI " : "", c.chd ? poly::value_text(3, c.voi, b, sizeof b) : "",
      nk, st.active, st.peak);
}

// ---- loading
CORE_COLD bool loading_slot(int) { return false; }   // (a load fills the cache, never a slot)
CORE_COLD int free_slot() {
  int b = -1;
  for (int s = 0; s < kSlots; ++s) {
    if (slots[s].ready && src_busy(kFixed + s)) continue;
    if (slots[s].ready && now_ms() - slots[s].used_ms < kRecentMs) continue;   // a switch's grains may still read it
    if (!slots[s].ready) return s;
    if (b < 0 || (int32_t)(slots[s].used_ms - slots[b].used_ms) < 0) b = s;
  }
  return b;
}
CORE_COLD void slot_drop(int s) {   // its view gone (no voice reads it), its cache block unpinned
  slots[s].ready = false;
  st_rel(&srcs[kFixed + s].frames, 0);
  if (slots[s].blk >= 0) pcache::unpin(slots[s].blk), slots[s].blk = -1;
}
// The cache has e whole: a slot views it, the voices waiting for it select it (and a waiting trig plays)
CORE_COLD bool take_in(int e, const pcache::Info& in) {
  int s = free_slot();
  if (s < 0) return false;
  slot_drop(s);
  pcache::pin(in.blk);
  Slot& sl = slots[s];
  sl.blk = in.blk, sl.entry = e, sl.size = list[e].size;
  sniprintf(sl.path, sizeof sl.path, "%s", list[e].path);
  Src& sr = srcs[kFixed + s];
  sr.data = (int16_t*)in.pcm;
  sr.rate = in.rate;
  sr.ch = in.ch;
  st_rel(&sr.frames, in.frames);
  sl.ready = true, sl.used_ms = now_ms();
  why[e] = in.cut ? 1 : 0;
  secs10[e] = (uint32_t)((uint64_t)in.frames * 10 / (in.rate > 0 ? (uint32_t)in.rate : 48000));
  pools::note_info(pools::kSamples, e, in.total, in.fch ? in.fch : in.ch, (uint32_t)in.rate);
  ++st.loads;
  if (in.cut)
    log(CORE_STR("gr: %s: longer than a source holds (%lu frames): its first %lu s only"), sl.path,
        (unsigned long)pcache::kMaxFrames, (unsigned long)(in.frames / (in.rate > 0 ? (uint32_t)in.rate : 48000)));
  for (int vi = 0; vi < kVoices; ++vi) {
    if (want[vi] < 0 || resident(want[vi]) != kFixed + s) continue;
    select(vi);
    if (pend[vi].on) {
      pend[vi].on = false;
      if (now_ms() - pend[vi].ms <= kLateMs) trig(vi, pend[vi].note, pend[vi].vel);
      else ++st.late;
    }
  }
  return true;
}
CORE_COLD bool start_load() {
  if (!pcache::ok()) return false;   // no pool RAM: card sources off
  int e = -1;
  for (int vi = 0; vi < kVoices && e < 0; ++vi)
    if (want[vi] >= 0 && fixed_src(want[vi]) < 0 && list[want[vi]].path[0] && resident(want[vi]) < 0 && !failed[want[vi]]) e = want[vi];
  if (e < 0) return false;
  int pe = pcache::want(pools::at(pools::kSamples, e).ref, list[e].path);
  if (pe < 0) return false;   // (the cache's queue full: tried again at a later step)
  job->entry = e, job->pe = pe, job->phase = kLoadWait;
  return true;
}
CORE_COLD bool step_wait() {
  int e = job->entry;
  if (e < 0 || e >= nlist || !pcache::is(job->pe, pools::at(pools::kSamples, e).ref)) {   // the row or the entry moved on
    job->phase = kIdle;
    return true;
  }
  pcache::State s = pcache::state(job->pe);
  if (s == pcache::kLoading || (s == pcache::kReady && pcache::busy(job->pe))) return false;   // (a head grows to whole)
  pcache::Info in;
  if (s == pcache::kReady && pcache::get(job->pe, &in) && in.whole) {
    if (e >= 0 && e < nlist && take_in(e, in)) job->phase = kIdle;
    else if (e < 0 || e >= nlist) job->phase = kIdle;
    return true;   // (no free slot yet: again at the next step)
  }
  if (e >= 0) {
    failed[e] = 1, why[e] = pcache::no_room(job->pe) ? 5 : 2;
    ++st.load_fails;
  }
  job->phase = kIdle;
  return true;
}
// The list composed from the SAMPLES pool (srclib::compose): what an entry knew (failed, why, its length) stays with its
// code while its path is the same; the resident samples stay where the list still has them (same path and size); a
// loading sample's entry found again; the voices' sources picked again.
CORE_COLD void compose() {
  Entry* old = list;
  Entry* nl = list == lbuf[0] ? lbuf[1] : lbuf[0];
  composed_sig = srclib::pool_sig(srclib::kGr);
  int n = srclib::compose(srclib::kGr, nl);
  for (int i = 0; i < n; ++i)
    if (!old || strcasecmp(old[i].path, nl[i].path)) failed[i] = 0, why[i] = 0, secs10[i] = 0;
  for (int s = 0; s < kSlots; ++s) {
    if (!slots[s].ready && !loading_slot(s)) continue;
    int e = -1;
    for (int i = 0; i < n && e < 0; ++i)
      if (nl[i].path[0] && !strcasecmp(nl[i].path, slots[s].path)) e = i;
    if (e >= 0 && slots[s].size && nl[e].size && slots[s].size != nl[e].size) e = -1;   // the file changed
    slots[s].entry = e;
    if (loading_slot(s)) job->entry = e;
    else if (e < 0 && !src_busy(kFixed + s)) slot_drop(s);
  }
  list = nl;   // (the displays read the list racily: the old buffer stays as it was until the next compose)
  nlist = n;
  st.listed = pools::count(pools::kSamples);
  __atomic_store_n(&g_list_gen, g_list_gen + 1, __ATOMIC_RELEASE);
  for (int vi = 0; vi < kVoices; ++vi) set_knobs(vi), select(vi);
}
// ---- the built-in sources (generated here): both pitched at C4 (key 60 plays them as they are)
CORE_COLD void gen_builtin(int b, int16_t* out, float* d) {
  const float f0 = 261.6256f, sr = 48000.0f, tw = 6.2831853f;
  for (uint32_t i = 0; i < kBuiltinFrames; ++i) d[i] = 0;
  if (b == 0) {   // PAD: two detuned band-limited saws, their brightness swelling and falling over the 4 s
    const int kH = 24;
    float xr[2][kH], xi[2][kH], cr[2][kH], ci[2][kH];   // a rotation a sample per partial
    for (int c = 0; c < 2; ++c)
      for (int h = 0; h < kH; ++h) {
        float w = tw * f0 * (c ? 1.004f : 0.996f) * (h + 1) / sr;
        xr[c][h] = 1, xi[c][h] = 0, cr[c][h] = cosf(w), ci[c][h] = sinf(w);
      }
    for (uint32_t i0 = 0; i0 < kBuiltinFrames; i0 += 64) {
      float bright = 0.5f - 0.5f * cosf(tw * (float)i0 / sr / 4.0f);   // 0 -> 1 -> 0 over the 4 s
      float a[kH];
      for (int h = 0; h < kH; ++h) a[h] = expf(-(float)h / (2.0f + 10.0f * bright)) / (h + 1);
      for (int c = 0; c < 2; ++c)
        for (int h = 0; h < kH; ++h) {
          float r = xr[c][h], q = xi[c][h];
          for (uint32_t i = i0; i < i0 + 64 && i < kBuiltinFrames; ++i) {
            d[i] += a[h] * q;
            float nr = r * cr[c][h] - q * ci[c][h];
            q = r * ci[c][h] + q * cr[c][h], r = nr;
          }
          float m = 1.0f / sqrtf(r * r + q * q);   // renormalised
          xr[c][h] = r * m, xi[c][h] = q * m;
        }
    }
  } else {   // BELLS: eight strikes of a bell's inharmonic partials, each ringing out
    const float kRatio[5] = {1.0f, 2.756f, 5.404f, 8.933f, 13.34f}, kAmp[5] = {1.0f, 0.6f, 0.4f, 0.25f, 0.15f};
    for (uint32_t i = 0; i < kBuiltinFrames; ++i) {
      float t = (float)i / sr, ts = fmodf(t, 0.5f);
      for (int k = 0; k < 5; ++k) d[i] += kAmp[k] * expf(-ts * (1.5f + 2.0f * k)) * sinf(tw * f0 * kRatio[k] * t);
    }
  }
  float pk = 0;   // to 0.9 of full scale
  for (uint32_t i = 0; i < kBuiltinFrames; ++i) pk = fabsf(d[i]) > pk ? fabsf(d[i]) : pk;
  float g = pk > 0 ? 0.9f * 32767.0f / pk : 0.0f;
  for (uint32_t i = 0; i < kBuiltinFrames; ++i) out[i] = (int16_t)(d[i] * g);
}

}  // namespace

// ---- the control core's API
CORE_COLD void begin(bool card) {
  card_ok = card;
  if (!begun) {
    begun = true;
    job = new Job();
    lbuf[0] = new Entry[srclib::kEntries];
    lbuf[1] = new Entry[srclib::kEntries];
    list = lbuf[0];
    grains = new Grain[kVoices * kMaxGrains];
    memset(grains, 0, sizeof(Grain) * kVoices * kMaxGrains);
    rings = (float*)msamp_ram_alloc((uint64_t)kVoices * kRing * sizeof(float));   // the board's high heap (platform.h)
    memset(rings, 0, sizeof(float) * kVoices * kRing);
    capture = new int16_t[kCap][kLive];
    memset(capture, 0, sizeof(int16_t) * kCap * kLive);
    float* tmp = new float[kBuiltinFrames];
    for (int b = 0; b < kBuiltins; ++b) {
      srcs[kLive + b].data = new int16_t[kBuiltinFrames];
      gen_builtin(b, srcs[kLive + b].data, tmp);
      srcs[kLive + b].rate = 48000.0f;
      srcs[kLive + b].ch = 1;
      srcs[kLive + b].frames = kBuiltinFrames;
    }
    delete[] tmp;
    for (int s = 0; s < kSlots; ++s) slots[s].blk = -1;
    for (int w = 0; w < kWins; ++w)
      for (int i = 0; i <= kWin; ++i) {
        float x = (float)i / kWin, y;
        if (w == 0) y = 0.5f - 0.5f * cosf(6.2831853f * x);                          // HANN
        else if (w == 1) y = 1.0f - fabsf(2 * x - 1);                                // TRI
        else if (w == 2) y = x < 0.25f ? 0.5f - 0.5f * cosf(6.2831853f * 2 * x)       // TUKEY: flat half
                           : x > 0.75f ? 0.5f - 0.5f * cosf(6.2831853f * 2 * (1 - x)) : 1.0f;
        else y = x < 0.05f ? 0.5f - 0.5f * cosf(3.14159265f * x / 0.05f)              // PERC: a raised-cosine
                           : expf(-5.0f * (x - 0.05f) / 0.95f);                      //   attack, then a fall
        win[w][i] = i == kWin ? 0 : y;
      }
    log(CORE_STR("gr: %d card sources at once (the pool RAM's), live rings %lu KB a voice"), kSlots,
        (unsigned long)(kRing * 4 >> 10));
  }
  composed_sig = 0;
  compose();
  pcache::begin(pool_ram_budget());   // (the first machine to start takes the pool RAM; later calls change nothing)
  for (int s = 0; s < kSlots; ++s) slot_drop(s), slots[s].entry = -1;
  for (int vi = 0; vi < kVoices; ++vi) {
    want[vi] = -1, pend[vi].on = false;
    reported[vi] = false, pv[vi].wait = pv[vi].on = false;
    sel[vi] = use[vi] = prev[vi] = kNone;
    ring_src[vi] = -1;
    for (int k = 0; k < 16; ++k) vp[vi][k] = 0;
    vp[vi][0] = 11 + (7 << 4) / 128.0f, vp[vi][3] = 64, vp[vi][4] = 64;   // (before a report: H011, the template's PAD)
    set_knobs(vi);
  }
  job->phase = kIdle;
}

CORE_COLD void rescan() {   // the card changed: failed files are tried again (the list itself is the pool's)
  if (!job) return;
  memset(failed, 0, sizeof failed);
  memset(why, 0, sizeof why);
}

CORE_COLD void preview_step() {
  uint32_t now = now_ms();
  for (int vi = 0; vi < kVoices; ++vi) {
    Preview& q = pv[vi];
    if (q.on && now - q.ms >= kPreviewMs) q.on = false, live_off(vi, kPreviewCh, -1);
    if (!q.wait) continue;
    int e = want[vi];
    if (e < 0 || failed[e] || now - q.ms >= kPreviewWaitMs) { q.wait = false; continue; }
    if (resident(e) < 0) continue;
    q.wait = false;
    select(vi);
    live_off(vi, kPreviewCh, -1);
    live_on(vi, 60, 100, kPreviewCh, 60);
    q.on = true, q.ms = now;
  }
}

CORE_COLD bool step() {
  if (!job) return false;
  preview_step();
  switch (job->phase) {
    case kIdle:
      if (srclib::pool_sig(srclib::kGr) != composed_sig) {   // the SAMPLES pool changed (a row, a state): listed again
        compose();
        return true;
      }
      return start_load() || pcache::step();
    case kLoadWait: return step_wait() || pcache::step();
  }
  return false;
}

CORE_COLD bool forced(int vi) { return force[vi & 7]; }

CORE_COLD void report(int vi, int note, int vel, const float* p) {
  vi &= 7;
  if (!job) return;
  for (int k = 0; k < 16; ++k) vp[vi][k] = p[k];
  v[vi].live = true;
  reported[vi] = true;
  set_knobs(vi);
  select(vi);
  pools::seen(pools::kSamples, knobs[vi].src);   // the used set (SELECT UNUSED, the preload, P3)
  if (note < 0) return;
  if (!srclib::silent(list, want[vi]) && resident(want[vi]) < 0 && !failed[want[vi]]) {
    pend[vi] = {true, (uint8_t)note, (uint8_t)vel, now_ms()};
    ++st.waits;
    return;
  }
  trig(vi, note, vel);
}

CORE_COLD void live_on(int vi, int key, int vel, int ch, int mnote) {
  vi &= 7;
  if (!job || key < 0 || key > 127) return;
  if (__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST) == kNone) set_knobs(vi), select(vi);
  cur_live = {true, (int8_t)ch, (uint8_t)mnote};
  start_note(vi, key, vel, 0.6f);
  cur_live = {false, 0, 0};
  v[vi].live = true;
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

CORE_COLD int voice_notes(int vi) {
  int a = 0;
  for (int k = 0; k < kPerVoice; ++k) a += !note_free(notes[vi & 7][k]);
  return a;
}

CORE_COLD void note_off(int vi) {
  vi &= 7;
  pend[vi].on = false;
  for (int k = 0; k < kPerVoice; ++k)
    if (!notes[vi][k].live) release_note(notes[vi][k]);
}

CORE_COLD void silence(int vi) {
  vi &= 7;
  pend[vi].on = false;
  for (int k = 0; k < kPerVoice; ++k) {
    Note& n = notes[vi][k];
    n.held = n.live = false;
    st_rel(&n.kill_n, n.kill_n + 1);
  }
}

CORE_COLD const char* entry_name(int e) {
  if (!list || nlist <= 0) return "";
  return list[e < 0 ? 0 : e >= nlist ? nlist - 1 : e].name;
}
CORE_COLD int entries() { return nlist; }   // every code (bank * 128 + row)
CORE_COLD const char* entry_group(int e) {
  if (!list || e < 0 || e >= nlist) return "";
  return list[e].grp;
}
CORE_COLD const char* entry_path(int e) {
  if (!list || e < 0 || e >= nlist) return "";
  return list[e].path;
}
CORE_COLD int entry_state(int e) {
  if (!list || e < 0 || e >= nlist || !srclib::playable(list[e])) return kStateNone;
  if (fixed_src(e) >= 0) return kStateRam;
  if (failed[e]) return kStateFailed;
  if (resident(e) >= 0) return why[e] == 1 ? kStateCut : kStateRam;
  for (int vi = 0; vi < kVoices; ++vi)
    if (want[vi] == e) return kStateLoading;
  return kStateCard;
}
CORE_COLD const char* entry_note(int e, char* b, int cap_) {
  if (!list || e < 0 || e >= nlist || cap_ < 1) return "";
  b[0] = 0;
  switch (entry_state(e)) {
    case kStateFailed:
      sniprintf(b, cap_, "%s", why[e] == 2 ? "NOT A WAV IT READS" : why[e] == 5 ? "NO ROOM IN THE POOL RAM" : "FAILED");
      break;
    case kStateCut: sniprintf(b, cap_, "TOO LONG: FIRST %lu S", (unsigned long)(secs10[e] / 10)); break;
    case kStateLoading: sniprintf(b, cap_, "LOADING"); break;
    case kStateNone:
      if (list[e].lib == srclib::kLibMissing) sniprintf(b, cap_, "MISSING");
      break;
    case kStateRam:
      if (fixed_src(e) < 0 && secs10[e]) sniprintf(b, cap_, "%lu.%lu S", (unsigned long)(secs10[e] / 10), (unsigned long)(secs10[e] % 10));
      break;
    default: break;
  }
  return b;
}
uint32_t list_gen() { return __atomic_load_n(&g_list_gen, __ATOMIC_ACQUIRE); }
CORE_COLD int src_entry(int vi, int v) {   // the code is the entry
  if (!list || nlist <= 0) return -1;
  return v < 0 ? 0 : v >= nlist ? nlist - 1 : v;
}
CORE_COLD bool voice_picked(int) { return false; }   // (no picks over SRC: a choice writes SRC)
// The touchscreen's / the web's choice: SRC written on the Rytm (48, as a knob turn: the kit keeps it, its report then
// names the row); the voice takes it at once (the report follows)
CORE_COLD bool pick(int vi, int e) {
  vi &= 7;
  if (!job || !list || e < 0 || e >= nlist) return false;
  ui::remote_param_write(vi, kGranMachine, 0, e & 127, srclib::lo_of(e));
  vp[vi][0] = (float)(e & 127) + (float)srclib::lo_of(e) / 128.0f;
  set_knobs(vi);
  select(vi);
  log(CORE_STR("gr: voice %d: SRC %c%03d %s"), vi, 'A' + srclib::bank_of(e), e & 127, entry_name(e));
  return true;
}
CORE_COLD int entry_lib(int e) { return list && e >= 0 && e < nlist ? list[e].lib : srclib::kLibOff; }
CORE_COLD void preview(int vi, int e) {
  vi &= 7;
  if (!job || !list || !preview_enabled || !(v[vi].gr || force[vi])) return;
  if (e >= 0) {   // the entry the Rytm's list shows (its SRC; the voice's report follows at its next trig)
    if (e >= nlist || !srclib::playable(list[e])) return;
    knobs[vi].src = e;
    select(vi);
  }
  pv[vi].wait = true, pv[vi].ms = now_ms();
  if (pv[vi].on) pv[vi].on = false, live_off(vi, kPreviewCh, -1);
}
CORE_COLD void kit_changed(int) {}

// The IN completion's: the ten live sources into the capture ring
void live_in(const int32_t* f, int n, int stride) {
  if (!capture) return;
  uint32_t w = ld_rlx(&cap_w);
  for (int i = 0; i < n; ++i, f += stride) {
    int16_t* c = capture[(w + i) & (kCap - 1)];
    c[0] = (int16_t)(((f[10] >> 1) + (f[11] >> 1)) >> 16);   // IN: AUDIO IN L + R
    c[1] = (int16_t)(((f[0] >> 1) + (f[1] >> 1)) >> 16);     // MAIN
    for (int k = 0; k < 8; ++k) c[2 + k] = (int16_t)(f[2 + k] >> 16);
  }
  st_rel(&cap_w, w + (uint32_t)n);
  st_rlx(&g_live_frames, ld_rlx(&g_live_frames) + (uint32_t)n);
}
bool live_wanted() { return want_live; }

// ---- the render core's
namespace {
inline float soft(float x) {
  x = x < -3 ? -3 : x > 3 ? 3 : x;
  float x2 = x * x;
  return x * (27 + x2) / (27 + 9 * x2);
}
inline uint32_t lcg(uint32_t& s) { return s = s * 1664525u + 1013904223u; }
inline float u01(uint32_t& s) { return (float)(lcg(s) >> 8) * (1.0f / 16777216.0f); }

// A seam in the voice's ring (a thaw, a new source, the first block): the frames before it fade out over kSeam, the
// new ones fade in, so a grain reading across it dips instead of stepping.
void seam(int vi) {
  float* r = rings + (size_t)vi * kRing;
  uint32_t w = ring_w[vi], n = w < kSeam ? w : kSeam;
  for (uint32_t j = 0; j < n; ++j) {   // j frames before the seam: the gain rises back to 1 going back in time
    float u = (float)(j + 1) / (float)(kSeam + 1);
    r[(w - 1 - j) & (kRing - 1)] *= u * u * (3.0f - 2.0f * u);
  }
  seam_in[vi] = kSeam;
}

// The voice's ring takes what the capture got since the last block (its source, plus the feedback), unless frozen.
void fill_ring(int vi, int src, bool frozen, float fb) {
  uint32_t cw = ld_acq(&cap_w);
  if (ring_src[vi] != src) {   // a new source (or the first block): no backlog
    ring_src[vi] = src;
    cap_rd[vi] = cw;
    seam(vi);
    return;
  }
  uint32_t rd = cap_rd[vi];
  if (cw - rd > kCap) rd = cw - kCap;   // (fell behind: the oldest lost)
  cap_rd[vi] = cw;
  if (frozen) {
    was_frozen[vi] = true;
    return;
  }
  if (was_frozen[vi]) was_frozen[vi] = false, seam(vi);
  float* r = rings + (size_t)vi * kRing;
  uint32_t w = ring_w[vi];
  for (uint32_t j = 0; rd != cw; ++rd, ++j) {
    float x = capture[rd & (kCap - 1)][src] * (1.0f / 32768.0f);
    if (fb > 0) x = soft(x + fb * fbb[vi][j % kBlock]);
    if (seam_in[vi]) {
      float u = 1.0f - (float)seam_in[vi] / (float)(kSeam + 1);
      x *= u * u * (3.0f - 2.0f * u);
      --seam_in[vi];
    }
    r[w & (kRing - 1)] = x;
    ++w;
  }
  ring_w[vi] = w;
}

// One grain's samples into acc from s0 on: interpolated from the ring (LIVE) or the int16 data (CH 1, or 2
// interleaved: summed to mono unless accr), x its window, its edge fades, its gain and its note's envelope (an orphan:
// its own fade). accr (a stereo voice): L into acc x the grain's pl, R into accr x pr (a mono source on both). False:
// it ended.
template <bool LIVE, int CH = 1>
inline bool play_grain(Grain& g, const float* ring, const int16_t* data, uint32_t len, const float* eb, float* acc,
                       int nfr, float* accr = nullptr) {
  const float* wt = win[g.win];
  double pos = g.pos;
  float wph = g.wph, ofade = g.ofade;
  const bool orphan = g.odec > 0;
  int s = g.s0;
  g.s0 = 0;
  for (; s < nfr && wph < 1.0f; ++s) {
    float a, b, t, ar = 0.0f, br = 0.0f;
    if (LIVE) {
      int64_t i = (int64_t)pos;
      t = (float)(pos - (double)i);
      a = ring[(uint32_t)i & (kRing - 1)], b = ring[(uint32_t)(i + 1) & (kRing - 1)];
      ar = a, br = b;
    } else {
      uint32_t i = (uint32_t)pos;
      t = (float)(pos - (double)i);
      uint32_t i1 = i + 1 >= len ? 0 : i + 1;
      if (CH == 2 && accr) {
        a = data[2 * i] * (1.0f / 32768.0f), b = data[2 * i1] * (1.0f / 32768.0f);
        ar = data[2 * i + 1] * (1.0f / 32768.0f), br = data[2 * i1 + 1] * (1.0f / 32768.0f);
      } else if (CH == 2) {
        a = pcache::mono_at(data, 2, i) * (1.0f / 32768.0f), b = pcache::mono_at(data, 2, i1) * (1.0f / 32768.0f);
      } else {
        a = data[i] * (1.0f / 32768.0f), b = data[i1] * (1.0f / 32768.0f);
        ar = a, br = b;
      }
    }
    float x = wph * kWin;
    int wi = (int)x;
    float wv = wt[wi] + (wt[wi + 1] - wt[wi]) * (x - (float)wi);
    float e = (wph < 1.0f - wph ? wph : 1.0f - wph) * g.ek;   // the edge fades (smoothstep)
    if (e < 1.0f) wv *= e * e * (3.0f - 2.0f * e);
    float m;
    if (orphan) {
      m = g.oenv * ofade * ofade * (3.0f - 2.0f * ofade);
      ofade -= g.odec;
      if (ofade <= 0) {
        wph = 1.0f;
        break;
      }
    } else {
      m = eb[s];
    }
    if (accr) {
      float gm = wv * g.gain * m;
      acc[s] += (a + (b - a) * t) * gm * g.pl;
      accr[s] += (ar + (br - ar) * t) * gm * g.pr;
    } else {
      acc[s] += (a + (b - a) * t) * wv * g.gain * m;
    }
    pos += g.inc;
    if (!LIVE) {
      if (pos >= len) pos -= len;
      else if (pos < 0) pos += len;
    }
    wph += g.winc;
  }
  g.pos = pos, g.wph = wph, g.ofade = ofade;
  return wph < 1.0f;
}
inline void orphan(Grain& g, float env) {
  if (g.odec > 0) return;
  g.oenv = env, g.ofade = 1.0f, g.odec = 1.0f / kOrphan;
}
}  // namespace

static float head_g[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};
static uint32_t pan_rnd[kVoices] = {0x9e3779b9u, 2, 3, 4, 5, 6, 7, 8};   // the stereo grains' pans (their own stream)

// a grain's read of the data at its channels (CH: the source's), into acc (and accr: a stereo voice)
static inline bool play_data(Grain& g, const int16_t* data, uint32_t len, int ch, const float* eb, float* acc, int nfr,
                             float* accr) {
  return ch == 2 ? play_grain<false, 2>(g, nullptr, data, len, eb, acc, nfr, accr)
                 : play_grain<false, 1>(g, nullptr, data, len, eb, acc, nfr, accr);
}
static void render_impl(int vi, float* out, float* outr, int nfr);
void render_add(int vi, float* out, int nfr) { render_impl(vi, out, nullptr, nfr); }
// OUT PRE / POST: the cloud in stereo - a stereo source's two channels, and each grain at a random pan within
// +-(SPRY x WID) (WID 64: SPRY's own spread; a mono source spreads too)
void render_add2(int vi, float* l, float* r, int nfr) { render_impl(vi, l, r, nfr); }

static void render_impl(int vi, float* out, float* outr, int nfr) {
  if (!begun) return;
  if (nfr > kBlock) {
    for (int d = 0; d < nfr; d += kBlock)
      render_impl(vi, out + d, outr ? outr + d : nullptr, nfr - d < kBlock ? nfr - d : kBlock);
    return;
  }
  uint32_t sid = __atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST);
  for (int i = 0; i < 4; ++i) {
    __atomic_store_n(&use[vi], sid, __ATOMIC_SEQ_CST);
    uint32_t s2 = __atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST);
    if (s2 == sid) break;
    sid = s2;
  }
  const Knobs k = knobs[vi];
  Grain* G = grains + vi * kMaxGrains;
  static uint32_t last_sid[kVoices] = {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
  static const int16_t* last_data[kVoices];
  static uint32_t last_len[kVoices];
  static uint8_t last_ch[kVoices];
  if (sid != last_sid[vi]) {   // another source: the grains reading the old one fade out on it (prev: kept for them)
    uint32_t was = last_sid[vi];
    OldSrc& o = old_src[vi];
    o = {last_data[vi], last_len[vi], was < (uint32_t)kLive, false, last_ch[vi]};
    if (was != kNone && (o.live || o.data)) {
      __atomic_store_n(&prev[vi], was, __ATOMIC_SEQ_CST);
      for (int gi = 0; gi < kMaxGrains; ++gi) {
        Grain& g = G[gi];
        if (!g.on) continue;
        if (g.old) {   // a second switch while the first still fades: those go now
          g.on = false;
          continue;
        }
        g.old = true, o.any = true;
        orphan(g, notes[vi][g.note].envs);
      }
    }
    last_sid[vi] = sid;
  }
  bool live = sid < (uint32_t)kLive && rings;
  if (live) fill_ring(vi, (int)sid, k.freeze, k.fb);
  else ring_src[vi] = -1;
  const int16_t* data = nullptr;
  uint32_t len = 0;
  float srate = 48000.0f;
  int dch = 1;
  if (!live && sid < (uint32_t)kSrcIds) {
    len = ld_acq(&srcs[sid].frames);
    data = len ? srcs[sid].data : nullptr;
    srate = srcs[sid].rate;
    dch = srcs[sid].ch == 2 ? 2 : 1;
  }
  last_data[vi] = data, last_len[vi] = len, last_ch[vi] = (uint8_t)dch;
  // a stereo voice's pan spread: SPRY x WID (out.h; WID 64 = SPRY's own)
  float pan_amt = 0.0f;
  if (outr) {
    float w = out::control.wid[vi] * (1.0f / 64.0f);
    pan_amt = clamp01(k.spr * w);
  }
  const float* ring = rings + (size_t)vi * kRing;
  uint32_t rw = ring_w[vi];
  uint32_t filled = rw < kRing ? rw : kRing;
  const float sr = 48000.0f;
  // ANIM: the cloud's head, size, density and spray moved by a quasi-periodic wave on the clock (or its own at the
  // last tempo when there is no control loop: tests)
  float a_pos = 0, a_size = 1, a_dens = 1, a_spray = 0;
  {
    float tp, tus;
    if (lfo::clock_now(&tp, &tus)) anim_ticks[vi] = tp;
    else anim_ticks[vi] = fmod(anim_ticks[vi] + nfr * (1e6 / 48000.0) / (tus > 1 ? tus : 20833.3f), 2304.0);
    if (k.anim_beats > 0) {
      double per = k.anim_beats * 24.0;
      float ph = (float)(fmod(anim_ticks[vi], per) / per) * 6.2831853f;
      float d = k.anim;
      a_pos = d * (0.8f * sinf(ph) + 0.2f * sinf(3 * ph + 0.4f));   // -d..d
      a_size = exp2f(0.6f * d * sinf(2 * ph + 1.3f));
      a_dens = exp2f(0.4f * d * sinf(3 * ph + 0.4f));
      a_spray = 0.15f * d;
    }
  }
  float spray_s = k.spray_s, spr = k.spr;
  if (a_spray > 0) {
    float s = clamp01(sqrtf(spray_s * 0.5f) + a_spray);
    spray_s = 2.0f * s * s, spr = s;
  }
  // the notes: their envelopes for this block, their new grains
  float envb[kPerVoice][kBlock];
  bool on[kPerVoice];
  float a_inc = 1.0f / (k.atk * sr), rcoef = expf(-6.9078f / (k.rel * sr));
  float size_s = k.size_s * a_size;
  float size_f = size_s * sr;
  float dens = k.dens * a_dens;
  float interval = sr / dens;
  // the cloud's level kept even: 1 / sqrt(overlap), and 1.6 for the windows' mean power (Hann's 3/8)
  float ggain = 1.6f / sqrtf(dens * size_s > 1 ? dens * size_s : 1.0f);
  int ng = (int)ld_rlx(&g_grains[vi]);
  float wk[kPerVoice];
  int nw = 0;
  float head = -1, back = -1;
  for (int ni = 0; ni < kPerVoice; ++ni) {
    Note& n = notes[vi][ni];
    on[ni] = false;
    uint32_t q = ld_acq(&n.seq);
    if (q != n.r_seq && !(q & 1)) {
      Start c = n.st;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (ld_rlx(&n.seq) == q) {
        if (n.r_on)   // a steal: the old note's grains fade out
          for (int gi = 0; gi < kMaxGrains; ++gi)
            if (G[gi].on && !G[gi].old && G[gi].note == ni) orphan(G[gi], n.envs);
        n.r_seq = q, n.r = c, n.r_on = true, n.r_rel = false;
        n.env = n.envs = 0, n.stage = 0, n.scan = 0, n.next = 0, n.rnd = c.seed | 1;
      }
    }
    if (!n.r_on) continue;
    if (ld_acq(&n.kill_n) != n.r.kill_gen) {   // killed: its grains fade out
      for (int gi = 0; gi < kMaxGrains; ++gi)
        if (G[gi].on && !G[gi].old && G[gi].note == ni) orphan(G[gi], n.envs);
      n.r_on = false;
      st_rel(&n.done, n.r_seq);
      continue;
    }
    if (!n.r_rel && ld_acq(&n.rel_n) != n.r.rel_gen) n.r_rel = true, n.stage = 2;
    bool fin = false;
    for (int s = 0; s < nfr; ++s) {
      if (n.stage == 0) {
        n.env += a_inc;
        if (n.env >= 1) n.env = 1, n.stage = 1;
      } else if (n.stage == 2) {
        n.env *= rcoef;
        if (n.env < 1e-4f && n.envs < 2e-4f) fin = true;
      }
      n.envs += (n.env - n.envs) * kEnvSmooth;
      envb[ni][s] = n.envs;
    }
    if (fin) {
      n.r_on = false;
      st_rel(&n.done, n.r_seq);
      continue;
    }
    on[ni] = true;
    wk[nw++] = n.envs;
    // new grains in this block (none once the note is nearly out)
    while (n.next < (float)nfr) {
      int s0 = (int)n.next;
      n.next += interval * (1.0f + (u01(n.rnd) - 0.5f) * spr);
      if (n.stage == 2 && n.env < 1e-3f) continue;
      if (!data && !live) continue;
      if (ng >= grain_cap) {
        __atomic_fetch_add(&g_capped, 1u, __ATOMIC_RELAXED);
        continue;
      }
      float semis = n.r.key - 60.0f + (k.prnd > 0 ? (2 * u01(n.rnd) - 1) * k.prnd : 0.0f);
      float rate = exp2f(semis / 12.0f) * srate / sr;
      bool rev = u01(n.rnd) < k.rev;
      float lsrc = size_f * rate;   // source frames the grain covers
      double pos;
      if (live) {
        if ((float)filled < lsrc + size_f + 512) continue;   // not enough recorded yet
        // behind the write head; ahead of the oldest frame by the grain's length always (a thaw restarts the writes)
        double newest = (double)rw - 3;
        double oldest = (double)rw - filled + 256 + size_f + 64;
        double room = newest - oldest - lsrc;
        if (room < 0) continue;
        double hb = k.pos + (a_pos + k.anim) * 0.5;   // ANIM: further back by up to its depth (of the room)
        hb = hb < 0 ? 0 : hb > 1 ? 1 : hb;
        double d = hb * room + spray_s * sr * u01(n.rnd);
        if (d > room) d = room;
        back = (float)hb;
        double top = newest - d;
        pos = rev ? top : top - lsrc;
      } else {
        double span = 0.25 * len < 4.0 * srate ? 0.25 * len : 4.0 * srate;   // ANIM: +- a quarter, 4 s at most
        double hc = (double)k.pos * len + n.scan + a_pos * span;   // the head (without the spray)
        double c = hc + (spray_s > 0 ? (2 * u01(n.rnd) - 1) * spray_s * srate : 0.0f);
        c = fmod(c, (double)len);
        if (c < 0) c += len;
        hc = fmod(hc, (double)len);
        head = (float)((hc < 0 ? hc + len : hc) / len);
        pos = rev ? c + lsrc * 0.5 : c - lsrc * 0.5;
        if ((double)len > lsrc + 4) {   // inside the sample: no grain reads across its end
          double lo = rev ? lsrc + 1 : 0, hi = rev ? len - 2 : len - 2 - lsrc;
          pos = pos < lo ? lo : pos > hi ? hi : pos;
        } else {
          pos = fmod(pos, (double)len);
          if (pos < 0) pos += len;
        }
      }
      int gi = 0;
      while (gi < kMaxGrains && G[gi].on) ++gi;
      if (gi == kMaxGrains) continue;
      Grain& g = G[gi];
      g.pos = pos, g.inc = rev ? -rate : rate;
      float gs = size_f < 16 ? 16 : size_f;
      g.wph = 0, g.winc = 1.0f / gs;
      g.ek = 1.0f / (g.winc * (kEdge < 0.5f * gs ? kEdge : 0.5f * gs));
      g.oenv = g.ofade = g.odec = 0;
      g.gain = n.r.gain * ggain;
      g.note = (uint8_t)ni, g.win = (uint8_t)k.win, g.s0 = (uint16_t)s0, g.on = true, g.old = false;
      g.pl = g.pr = 1.0f;
      if (outr && pan_amt > 0.0f) {   // (the voice's own random stream: a TRK voice's grains draw exactly as before)
        float pn = (2.0f * u01(pan_rnd[vi]) - 1.0f) * pan_amt;
        float th = (pn + 1.0f) * 0.78539816f;
        g.pl = 1.41421356f * cosf(th), g.pr = 1.41421356f * sinf(th);
      }
      ++ng;
      __atomic_fetch_add(&g_started, 1u, __ATOMIC_RELAXED);
    }
    n.next -= (float)nfr;
    if (!live) n.scan += nfr * (double)srate / sr * k.scan;   // the playhead scans on (HOLD slows it, FREEZE stops)
    if (len && n.scan >= len) n.scan -= len;
  }
  if (head >= 0) g_head[vi] = head;
  else if (!live && len && !nw)   // no note: where the next would start
    g_head[vi] = (float)fmod(k.pos + a_pos * (0.25 < 4.0 * srate / len ? 0.25 : 4.0 * srate / len) + 1.0, 1.0);
  if (back >= 0) g_back[vi] = back;
  // the grains
  float acc[kBlock] = {0}, accr[kBlock] = {0};
  float* ar = outr ? accr : nullptr;
  int live_g = 0;
  const OldSrc o = old_src[vi];
  bool old_left = false;
  for (int gi = 0; gi < kMaxGrains; ++gi) {
    Grain& g = G[gi];
    if (!g.on) continue;
    if (g.odec <= 0 && !on[g.note]) {   // its note ended (quiet by now)
      g.on = false;
      continue;
    }
    const float* eb = envb[g.note];
    bool more;
    if (g.old) more = o.live ? play_grain<true>(g, ring, nullptr, 0, eb, acc, nfr, ar)
                     : o.data ? play_data(g, o.data, o.len, o.ch, eb, acc, nfr, ar) : false;
    else if (live) more = play_grain<true>(g, ring, nullptr, 0, eb, acc, nfr, ar);
    else if (data) more = play_data(g, data, len, dch, eb, acc, nfr, ar);
    else more = false;
    if (!more) g.on = false;
    else ++live_g, old_left = old_left || g.old;
  }
  if (!old_left && __atomic_load_n(&prev[vi], __ATOMIC_RELAXED) != kNone) __atomic_store_n(&prev[vi], kNone, __ATOMIC_SEQ_CST);
  st_rlx(&g_grains[vi], (uint32_t)live_g);
  __atomic_store_n(&use[vi], kNone, __ATOMIC_SEQ_CST);
  float e = 0;
  for (int j = 0; j < nw; ++j) e += wk[j];
  const float atk = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadAtkMs));
  const float rel = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadRelMs));
  float g0 = head_g[vi], target = e > 1.0f ? 1.0f / sqrtf(e) : 1.0f;
  float g1 = g0 + (target - g0) * (target < g0 ? atk : rel);
  if (fabsf(g1 - target) < 1e-5f) g1 = target;
  head_g[vi] = g1;
  float dg = (g1 - g0) / (float)nfr;
  for (int s = 0; s < nfr; ++s) {
    float gs = g0 + dg * (float)(s + 1);
    float y = acc[s] * gs;
    if (outr) {
      float yr = accr[s] * gs;
      outr[s] += yr;
      fbb[vi][s] = 0.5f * (y + yr);
    } else {
      fbb[vi][s] = y;
    }
    out[s] += y;
  }
  for (int s = nfr; s < kBlock; ++s) fbb[vi][s] = 0;
}

CORE_COLD float headroom(int vi) { return head_g[vi & 7]; }

CORE_COLD const char* hold_text(int v, char* b, int cap_) {
  if (v >= kHoldFreeze) sniprintf(b, cap_, "FRZ");
  else sniprintf(b, cap_, "%d", (v < 0 ? 0 : v) * 99 / (kHoldFreeze - 1));
  return b;
}
CORE_COLD const char* anim_text(int v, char* b, int cap_) {
  static const char* const kCyc[8] = {"8BAR", "4BAR", "2BAR", "1BAR", "1/2", "1/4", "1/8", "1/16"};
  int z = v / 16;
  sniprintf(b, cap_, "%s", v < 1 ? "OFF" : kCyc[z > 7 ? 7 : z]);
  return b;
}

// The display's: read while the render core writes (floats and flags; a torn frame draws a grain a block off)
CORE_COLD bool viz(int vi, Viz* o) {
  vi &= 7;
  o->n = 0;
  o->entry = -1;
  if (!begun) return false;
  uint32_t sid = __atomic_load_n(&sel[vi], __ATOMIC_RELAXED);
  if (sid == kNone || sid >= (uint32_t)kSrcIds) return false;
  const Knobs& k = knobs[vi];
  o->entry = voice_entry(vi);
  o->live = sid < (uint32_t)kLive;
  o->frozen = k.freeze;
  o->anim = k.anim_beats > 0 ? k.anim : 0.0f;
  uint32_t len = o->live ? 0 : ld_rlx(&srcs[sid].frames);
  uint32_t rw = ring_w[vi], filled = rw < kRing ? rw : kRing;
  o->head = o->live ? 1.0f - g_back[vi] * 0.97f : g_head[vi];   // live: the room is ~97 % of what the ring holds
  const Grain* G = grains + vi * kMaxGrains;
  for (int gi = 0; gi < kMaxGrains; ++gi) {
    const Grain& g = G[gi];
    if (!g.on || g.old) continue;
    float x;
    if (o->live) {
      if (!filled) continue;
      double back = (double)rw - g.pos;
      x = (float)(1.0 - back / (double)filled);
    } else {
      if (!len) continue;
      x = (float)(g.pos / (double)len);
    }
    if (x < 0 || x > 1) continue;
    float w = g.wph < 0 ? 0 : g.wph > 1 ? 1 : g.wph;
    float a = win[g.win][(int)(w * kWin)] * (g.odec > 0 ? g.oenv * g.ofade : notes[vi][g.note].envs);
    o->pos[o->n] = x;
    o->amp[o->n] = a < 0 ? 0 : a > 1 ? 1 : a;
    o->rev[o->n] = g.inc < 0;
    ++o->n;
  }
  return true;
}
CORE_COLD int viz_wave(int vi, int cols, float* mn, float* mx) {
  vi &= 7;
  if (!begun || cols <= 0) return 0;
  uint32_t sid = __atomic_load_n(&sel[vi], __ATOMIC_RELAXED);
  if (sid == kNone || sid >= (uint32_t)kSrcIds) return 0;
  bool live = sid < (uint32_t)kLive;
  const float* r = rings + (size_t)vi * kRing;
  uint32_t rw = ring_w[vi], filled = rw < kRing ? rw : kRing;
  uint32_t len = live ? filled : ld_rlx(&srcs[sid].frames);
  const int16_t* d = live ? nullptr : srcs[sid].data;
  const int dc = srcs[sid].ch == 2 ? 2 : 1;
  if (!len || (!live && !d)) return 0;
  constexpr uint32_t kPeek = 48;   // samples looked at a column (an outline, not every frame: the cost stays flat)
  for (int c = 0; c < cols; ++c) {
    uint32_t a = (uint32_t)((uint64_t)len * c / cols), b = (uint32_t)((uint64_t)len * (c + 1) / cols);
    if (b <= a) b = a + 1;
    uint32_t step = (b - a) / kPeek ? (b - a) / kPeek : 1;
    float lo = 1, hi = -1;
    for (uint32_t i = a; i < b; i += step) {
      float y = live ? r[(rw - filled + i) & (kRing - 1)] : pcache::mono_at(d, dc, i < len ? i : len - 1) * (1.0f / 32768.0f);
      lo = y < lo ? y : lo, hi = y > hi ? y : hi;
    }
    mn[c] = lo, mx[c] = hi;
  }
  return cols;
}

CORE_COLD bool command(const char* p, char* out, int cap_) {
  if (strncmp(p, "gr", 2) != 0 || (p[2] && p[2] != '=')) return false;
  const char* a = p[2] ? p + 3 : "";
  if (!strcmp(a, "rescan")) {
    rescan();
    sniprintf(out, cap_, "gr: failed sources tried again");
    return true;
  }
  if (!strncmp(a, "voice:", 6)) {
    int vi = a[6] - '0';
    if (vi < 0 || vi > 7) {
      sniprintf(out, cap_, "gr: voice 0..7");
      return true;
    }
    bool on = strcmp(a + 7, ":off") != 0;
    if (force[vi] && !on) silence(vi);
    force[vi] = on;
    sniprintf(out, cap_, "gr: voice %d %s", vi, on ? "plays GRANULAR (any built-in machine the Rytm reports)" : "plays its own machine");
    return true;
  }
  if (!strncmp(a, "pick:", 5) || !strncmp(a, "preview:", 8)) {   // pick:V:E / pick:V:off, preview:V[:E]
    bool pk = a[1] == 'i';
    const char* q = a + (pk ? 5 : 8);
    int vi = q[0] - '0', e = -1;
    if (vi < 0 || vi > 7 || (q[1] && q[1] != ':')) {
      sniprintf(out, cap_, "gr: %s:V:E (V 0..7)", pk ? "pick" : "preview");
      return true;
    }
    const char* r = q[1] ? q + 2 : "";
    if (*r && strcmp(r, "off") && !pools::parse_row(r, &e) && !parse_int(&r, &e)) {
      sniprintf(out, cap_, "gr: the entry: a row (A001..H127) or a code (gr=list)");
      return true;
    }
    if (!pk) {
      preview(vi, e);
      sniprintf(out, cap_, "gr: voice %d previews %s", vi, entry_name(e >= 0 ? e : knobs[vi].src));
      return true;
    }
    if (!pick(vi, e)) sniprintf(out, cap_, "gr: no entry %d (%d listed)", e, nlist);
    else {
      if (e >= 0) preview(vi, -1);
      sniprintf(out, cap_, "gr: voice %d SRC %s", vi, entry_name(knobs[vi].src));
    }
    return true;
  }
  if (!strncmp(a, "audition:", 9)) {
    preview_enabled = strcmp(a + 9, "off") && strcmp(a + 9, "0");
    sniprintf(out, cap_, "gr: list previews %s", preview_enabled ? "on" : "off");
    return true;
  }
  if (!strncmp(a, "grains:", 7)) {
    int n = 0;
    for (const char* q = a + 7; *q >= '0' && *q <= '9'; ++q) n = n * 10 + (*q - '0');
    set_cap(n);
    sniprintf(out, cap_, "gr: at most %d grains a voice", grain_cap);
    return true;
  }
  if (!strcmp(a, "list")) {
    for (int i = 0; i < nlist; ++i)
      if (list[i].lib != srclib::kLibHole && list[i].lib != srclib::kLibOff)   // empty and OFF rows left out
      log(CORE_STR("gr: SRC %c%03d %s [%s]%s%s%s"), 'A' + srclib::bank_of(i), i & 127, list[i].name, list[i].grp, list[i].path[0] ? " (" : "",
          list[i].path, list[i].path[0] ? (failed[i] ? ") failed" : ")") : "");
    sniprintf(out, cap_, "gr: %d sources (the list in the log)", nlist);
    return true;
  }
  if (*a) return false;
  stats();
  int n = sniprintf(out, cap_, "gr: %d sources (%d live, %d built in), %d/%d card samples resident, %d/%d notes now "
                               "(peak %d), %d grains now (peak %d, cap %d a voice), %lu grains, %lu capped, %lu live "
                               "frames, %lu trigs, %lu notes, %lu steals, %lu waits (%lu late), %lu loads (%lu failed)",
                    nlist, kLive, kBuiltins, st.resident, kSlots, st.active, kTotal, st.peak, st.grains_now,
                    st.grains_peak, grain_cap, (unsigned long)st.grains, (unsigned long)st.capped,
                    (unsigned long)st.live_frames, (unsigned long)st.trigs, (unsigned long)st.notes,
                    (unsigned long)st.steals, (unsigned long)st.waits, (unsigned long)st.late, (unsigned long)st.loads,
                    (unsigned long)st.load_fails);
  bool any = false;
  for (int vi = 0; vi < kVoices && n < cap_; ++vi)
    if (force[vi]) n += sniprintf(out + n, cap_ - n, "%s%d", any ? "," : "; forced voices ", vi), any = true;
  return true;
}

const Stats& stats() {
  if (!job) return st;
  st.active = count_active();
  int r = 0;
  for (int s = 0; s < kSlots; ++s) r += slots[s].ready;
  st.resident = r;
  st.listed = pools::count(pools::kSamples);
  st.grains = ld_rlx(&g_started);
  st.capped = ld_rlx(&g_capped);
  st.live_frames = ld_rlx(&g_live_frames);
  int g = 0;
  for (int vi = 0; vi < kVoices; ++vi) g += (int)ld_rlx(&g_grains[vi]);
  st.grains_now = g;
  if (g > st.grains_peak) st.grains_peak = g;
  return st;
}
void set_cap(int n) { grain_cap = n < 1 ? 1 : n > kMaxGrains ? kMaxGrains : n; }
int cap() { return grain_cap; }
int voice_grains(int vi) { return (int)ld_rlx(&g_grains[vi & 7]); }
uint32_t ring_written(int vi) { return ring_w[vi & 7]; }
float ring_at(int vi, uint32_t f) { return rings ? rings[(size_t)(vi & 7) * kRing + (f & (kRing - 1))] : 0.0f; }
int voice_entry(int vi) {
  uint32_t t = __atomic_load_n(&sel[vi & 7], __ATOMIC_SEQ_CST);
  if (t == kNone) return -1;
  int e = knobs[vi & 7].src;   // (a file listed twice: the entry the voice chose)
  if (e >= 0 && e < nlist && resident(e) == (int)t) return e;
  return t < (uint32_t)kFixed ? -1 : slots[t - kFixed].entry;
}
CORE_COLD void reset() {
  for (int vi = 0; vi < kVoices; ++vi) {
    silence(vi);
    force[vi] = false;
    head_g[vi] = 1.0f;
  }
  uint32_t loads = st.loads, fails = st.load_fails;
  memset(&st, 0, sizeof st);
  st_rlx(&g_started, 0), st_rlx(&g_capped, 0);
  st.loads = loads, st.load_fails = fails, st.listed = pools::count(pools::kSamples);
}

}  // namespace gr
}  // namespace core
#endif
