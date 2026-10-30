// core/wavetable/wavetable.cpp - see wavetable.h. Everything but render_add is the control core's and cold.
#include "../platform.h"

#if CORE_MSAMP
#include "wavetable.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "../lib/pools.h"
#include "../lib/srclib.h"
#include "../msamp/cardq.h"
#include "../machines.h"
#include "../ui/remote.h"
#include "../poly.h"
#include "../voices.h"
#include "tables.h"
#include "wav_read.h"

namespace core {
namespace wt {

namespace {

inline uint32_t ld_acq(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline uint32_t ld_rlx(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
inline void st_rel(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline void st_rlx(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

constexpr int kPath = 160;
constexpr int kTables = kBuiltins + kSlots;   // table ids: the built-ins, then the card slots
constexpr int kSine = 1024;                   // the sub's and FM's sine table

// ---- tables
struct Table {
  float* data;             // frames x kFrameFloats
  volatile uint32_t frames;   // 0 while (re)loading
};
Table tables[kTables];
struct Slot {
  int entry;               // the list entry it holds (-1 none)
  bool ready;
  uint32_t used_ms;
  char path[kPath];        // its file (compose finds its entry again by it)
};
Slot slots[kSlots];
// The list: the project's WAVETABLES pool (core/lib/pools.h), entry = code (bank * 128 + row: TBL's 14-bit word); a
// built-in row plays its generated table (Entry::fixed 0..5), a sample row its file (loaded into a card slot on use);
// OFF, empty and missing rows are silent.
using Entry = srclib::Entry;
Entry* list = nullptr;
Entry* lbuf[2] = {nullptr, nullptr};
int nlist = 0;              // srclib::kEntries once composed
uint32_t composed_sig = 0;
volatile uint32_t g_list_gen = 1;
uint8_t failed[srclib::kEntries];
uint8_t why[srclib::kEntries];   // 1 larger than 8 MB, 2 not a WAV / AIFF it reads, 3 cannot open, 4 no frame, 5 read failed
uint16_t nframes[srclib::kEntries];   // its frames once loaded
// a preview (the touchscreen's pick): a short note once the table is in
struct Preview {
  bool wait, on;
  uint32_t ms;
};
Preview pv[kVoices];
constexpr int kPreviewCh = 126;
constexpr uint32_t kPreviewMs = 700, kPreviewWaitMs = 3000;
float sine[kSine + 1];

// ---- notes
struct Start {
  float key;               // the MIDI note (chord note)
  float gain;
  uint32_t rel_gen, kill_gen, seed;
};
struct Note {
  volatile uint32_t seq;   // odd while the control core writes st
  Start st;
  volatile uint32_t rel_n, kill_n;
  volatile uint32_t done;  // the seq of the start that finished (or was killed)
  // the control core's
  bool held, live;
  int8_t ch;
  uint8_t key, mnote;
  uint32_t order;
  // the render core's
  uint32_t r_seq;
  bool r_on, r_rel;
  Start r;
  uint32_t ph[kUni], sub_ph, fm_ph[kUni];
  float env, menv;
  uint8_t stage, mstage;   // amp: 0 attack 1 decay/sustain 2 release; mod: 0 attack 1 decay
  float ic1, ic2;          // the SVF's state
  float dc_x, dc_y;        // the DC blocker's (a warp's asymmetry, PWM's above all, has a mean)
};
Note notes[kVoices][kPerVoice];
uint32_t order_n = 0;

// The voice's knobs (the control core writes them at a report, the render core reads them; floats, a block's tearing
// is harmless)
struct Knobs {
  float pos, amt, uni, sub, cut, res, atk, dec, sus, env;
  int warp;
};
Knobs knobs[kVoices];
float vp[kVoices][16];
volatile uint32_t sel[kVoices];   // the table id voice vi plays (kTables: none); the control core's
volatile uint32_t use[kVoices];   // the table id its render core reads now (kTables: none)
int16_t want[kVoices];            // the list entry it wants (-1 none)
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
bool card_ok = false;
bool begun = false;
uint32_t rnd = 0x9e3779b9u;

// ---- the job (one at a time)
// fix1041: the file's open, reads and close are the card core's (cardq::async; granular.cpp's note): the control core
// never waits on the card. kBuild / kNorm (the table, a frame a step) stay here.
enum Phase : uint8_t { kIdle, kLoadOpen, kLoadRead, kBuild, kNorm, kLoadClose };
struct Job {
  Phase phase;
  cardq::Req rq;               // fix1041: the load's card request
  Phase after;                 // fix1041: the phase after kLoadClose (kBuild, or kIdle after a failure)
  bool rescan_wanted;
  File dir, f;
  char subs[64][40];
  int nsubs, sub;
  int slot, entry;
  uint8_t* raw;            // the file (kFileMax)
  uint32_t size, got;
  wavread::Info a;
  int fs, frames, frame;
  float peak;
  float* x;                // a frame's samples (8192)
  float* c;                // its harmonics (kSpec)
  float* scratch;          // 4 * 8192
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

// the table id an entry plays without a card slot: a built-in's 0..5; -1 a card file
CORE_COLD int fixed_tab(int e) {
  if (!list || e < 0 || e >= nlist) return -1;
  const Entry& x = list[e];
  return x.fixed >= 0 && x.lib == srclib::kLibNone && x.fixed < kBuiltins ? x.fixed : -1;
}

// a table id's hazard: in use while a voice selects it or a render core reads it
CORE_COLD bool table_busy(uint32_t t) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  for (int vi = 0; vi < kVoices; ++vi)
    if (__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST) == t || __atomic_load_n(&use[vi], __ATOMIC_SEQ_CST) == t) return true;
  return false;
}
CORE_COLD int resident(int e) {
  if (e < 0 || e >= nlist) return -1;
  int f = fixed_tab(e);
  if (f >= 0) return f;
  if (!list[e].path[0]) return -1;
  for (int s = 0; s < kSlots; ++s)   // by path: a file in the pool and in its folder is one slot
    if (slots[s].ready && (slots[s].entry == e || (list[e].path[0] && !strcasecmp(slots[s].path, list[e].path)))) return kBuiltins + s;
  return -1;
}
// ---- the voice's selection and knobs (at every report)
CORE_COLD float tsec(float v) {   // 0..127 -> 1 ms .. 10 s, exponential
  v = v < 0 ? 0 : v > 127 ? 127 : v;
  return 0.001f * powf(10.0f, v / 127.0f * 4.0f);
}
CORE_COLD float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
CORE_COLD void set_knobs(int vi) {
  const float* p = vp[vi];
  Knobs& k = knobs[vi];
  k.pos = clamp01(p[1] / 127.0f);
  k.warp = eng_zone(p[2], 4);
  k.amt = clamp01(p[3] / 127.0f);
  k.uni = clamp01(p[4] / 127.0f);
  k.sub = clamp01(p[5] / 127.0f);
  k.cut = p[6] < 0 ? 0 : p[6] > 127.99f ? 127.99f : p[6];
  k.res = clamp01(p[7] / 127.0f);
  k.atk = tsec(p[8]);
  k.dec = tsec(p[9]);
  k.sus = clamp01(p[10] / 127.0f);
  float e = (p[11] - 64.0f) / 63.0f;
  k.env = e < -1 ? -1 : e > 1 ? 1 : e;
}
CORE_COLD void select(int vi) {
  int e = srclib::code_of(vp[vi][0]);   // the pool row: bank * 128 + row
  if (!list || e < 0 || e >= nlist) return;
  if (v[vi].wt || force[vi]) pools::voice_on(pools::kTables, vi, e);
  want[vi] = (int16_t)e;
  if (srclib::silent(list, e)) return;   // OFF / empty / missing: no sound (a trig skips it)
  int t = resident(e);
  if (t >= kBuiltins) slots[t - kBuiltins].used_ms = now_ms();
  if (t >= 0) __atomic_store_n(&sel[vi], (uint32_t)t, __ATOMIC_SEQ_CST);   // else the old table plays on while it loads
  else if (__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST) >= (uint32_t)kTables)
    __atomic_store_n(&sel[vi], 0u, __ATOMIC_SEQ_CST);   // nothing yet: BASIC meanwhile
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
  for (int k = 0; k < kPerVoice; ++k)   // the oldest released
    if (!ns[k].held && (b < 0 || (int32_t)(ns[k].order - ns[b].order) < 0)) b = k;
  if (b < 0)
    for (int k = 0; k < kPerVoice; ++k)
      if (b < 0 || (int32_t)(ns[k].order - ns[b].order) < 0) b = k;
  ++st.steals;
  return b;
}
CORE_COLD void start_note(int vi, int key, int vel, float gain) {
  if (count_active() >= kTotal) {   // the cap in all: the voice's own oldest goes (if it has none, the note is left out)
    bool any = false;
    for (int k = 0; k < kPerVoice; ++k) any = any || !note_free(notes[vi][k]);
    if (!any) return;
  }
  int k = pick(vi);
  Note& n = notes[vi][k];
  Start x;
  x.key = (float)key;
  float v = (vel < 1 ? 1 : vel) / 127.0f;
  x.gain = gain * (0.25f + 0.75f * v * v);
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
  for (int k = 0; k < kPerVoice; ++k)   // the earlier trig notes: their tails go on
    if (!notes[vi][k].live) release_note(notes[vi][k]);
  if (list && want[vi] >= 0 && srclib::silent(list, want[vi])) {   // rui1038: a bank's OFF / empty row
    ++st.trigs;
    return;
  }
  poly::Chord c = poly::chord_of(vp[vi]);
  uint8_t ks[poly::kMaxNotes];
  int nk = poly::expand(note, c, ks);
  for (int i = 0; i < nk; ++i) start_note(vi, ks[i], vel, 1.0f);
  ++st.trigs;
  v[vi].live = true;
  int t = (int)__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST);
  char b[8];
  log(CORE_STR("trig voice %d WAVETABLE %s note %d %s%s%s: %d notes (%d now, peak %d)"), vi,
      t < kTables && want[vi] >= 0 ? list[want[vi]].name : "-", note, c.chd ? poly::kChordNames[c.chd] : "",
      c.chd ? " VOI " : "", c.chd ? poly::value_text(3, c.voi, b, sizeof b) : "", nk, st.active, st.peak);
}

// ---- loading
void compose();
CORE_COLD void job_fail(const char* text, uint8_t code) {
  log(CORE_STR("wt: %s: %s"), slots[job->slot].path, text);
  if (job->entry >= 0) failed[job->entry] = 1, why[job->entry] = code;
  ++st.load_fails;
  slots[job->slot].entry = -1;
  job->after = kIdle;
  job->phase = kLoadClose;   // the card core closes the file (FatFs's lock), then idle
}
CORE_COLD int free_slot() {
  int b = -1;
  for (int s = 0; s < kSlots; ++s) {
    if (!tables[kBuiltins + s].data) continue;
    if (table_busy(kBuiltins + s) && slots[s].ready) continue;
    if (!slots[s].ready) return s;
    if (b < 0 || (int32_t)(slots[s].used_ms - slots[b].used_ms) < 0) b = s;
  }
  return b;
}
CORE_COLD bool start_load() {
  if (!job->raw) return false;   // no memory for card tables
  int e = -1;
  for (int vi = 0; vi < kVoices && e < 0; ++vi)   // a voice's wanted table that is not resident
    if (want[vi] >= 0 && fixed_tab(want[vi]) < 0 && list[want[vi]].path[0] && resident(want[vi]) < 0 && !failed[want[vi]]) e = want[vi];
  if (e < 0) return false;
  int s = free_slot();
  if (s < 0) return false;   // every slot plays: later
  Slot& sl = slots[s];
  sl.ready = false;
  sl.entry = e;
  sniprintf(sl.path, sizeof sl.path, "%s", list[e].path);
  st_rel(&tables[kBuiltins + s].frames, 0);
  job->slot = s, job->entry = e;
  job->rq.state = cardq::kFree;
  job->phase = kLoadOpen;
  return true;
}
CORE_COLD bool step_open() {
  if (!cardq::async(&job->rq, cardq::kOpenReq, &job->f, slots[job->slot].path, 0, nullptr, 0)) return false;
  if (job->rq.got != 1) {
    job_fail("cannot open", 3);
    return true;
  }
  uint64_t size = job->f.fileSize();
  if (size > kFileMax || size < 44) {
    job_fail(size > kFileMax ? "larger than 8 MB (a table file at most)" : "too short", size > kFileMax ? 1 : 2);
    return true;
  }
  job->size = (uint32_t)size, job->got = 0;
  job->phase = kLoadRead;
  return true;
}
CORE_COLD bool step_read() {
  uint32_t n = job->size - job->got;
  if (n > kChunk) n = kChunk;
  if (!cardq::async(&job->rq, cardq::kReadReq, &job->f, nullptr, job->got, job->raw + job->got, n)) return false;
  int r = job->rq.got;
  if (r <= 0) {
    job_fail("read failed", 5);
    return true;
  }
  job->got += (uint32_t)r;
  if (job->got < job->size) return true;
  if (!wavread::parse(job->raw, job->size, &job->a)) {
    job_fail("not a WAV / AIFF this reads (PCM 8/16/24/32, float 32/64)", 2);
    return true;
  }
  job->fs = detect_frame_size(job->a.frames, job->a.clm_frame);
  job->frames = frame_count(job->a.frames, job->fs);
  if (job->frames < 1) {
    job_fail("no frame in it", 4);
    return true;
  }
  job->frame = 0;
  job->peak = 0;
  job->after = kBuild;
  job->phase = kLoadClose;
  return true;
}
CORE_COLD bool step_close() {
  if (!cardq::async(&job->rq, cardq::kCloseReq, &job->f, nullptr, 0, nullptr, 0)) return false;
  job->phase = job->after;
  return true;
}
// one frame's levels
CORE_COLD void step_build() {
  const wavread::Info& a = job->a;
  uint32_t sf = source_frame(a.frames, job->fs, job->frame);
  wavread::to_mono(a, job->raw + a.data_off + (size_t)sf * job->fs * wavread::frame_bytes(a), (uint32_t)job->fs, job->x);
  analyze(job->x, job->fs, job->c, job->scratch);
  float* fr = tables[kBuiltins + job->slot].data + (size_t)job->frame * kFrameFloats;
  synth_frame(job->c, fr, job->scratch);
  float pk = frame_peak(fr);
  if (pk > job->peak) job->peak = pk;
  if (++job->frame >= job->frames) job->frame = 0, job->phase = kNorm;
}
// one frame scaled to the table's peak, then ready
CORE_COLD void step_norm() {
  float g = job->peak > 1e-6f ? 0.95f / job->peak : 1.0f;
  float* fr = tables[kBuiltins + job->slot].data + (size_t)job->frame * kFrameFloats;
  for (int i = 0; i < kFrameFloats; ++i) fr[i] *= g;
  if (++job->frame < job->frames) return;
  Slot& sl = slots[job->slot];
  st_rel(&tables[kBuiltins + job->slot].frames, (uint32_t)job->frames);
  sl.ready = true;
  sl.used_ms = now_ms();
  ++st.loads;
  st.last_frames = (uint32_t)job->frames, st.last_frame_size = (uint32_t)job->fs;
  log(CORE_STR("wt: %s: %d frames of %d samples (%lu Hz, %d-bit, %d ch%s)"), sl.path, job->frames, job->fs,
      (unsigned long)job->a.rate, job->a.bits, job->a.ch, job->a.clm_frame ? ", clm" : "");
  if (job->entry >= 0) nframes[job->entry] = (uint16_t)job->frames;
  job->phase = kIdle;
  for (int vi = 0; vi < kVoices; ++vi) {   // the voices that wanted it: theirs now; a waiting trig plays
    if (want[vi] < 0 || resident(want[vi]) != kBuiltins + job->slot) continue;
    select(vi);
    if (pend[vi].on) {
      pend[vi].on = false;
      if (now_ms() - pend[vi].ms <= kLateMs) trig(vi, pend[vi].note, pend[vi].vel);
      else ++st.late;
    }
  }
}
// The list composed from the WAVETABLES pool (srclib::compose): an entry's state stays with its code while its path is
// the same, the resident tables stay where the list still has them, the voices select again.
CORE_COLD void compose() {
  Entry* old = list;
  Entry* nl = list == lbuf[0] ? lbuf[1] : lbuf[0];
  composed_sig = srclib::pool_sig(srclib::kWt);
  int n = srclib::compose(srclib::kWt, nl);
  for (int i = 0; i < n; ++i)
    if (!old || strcasecmp(old[i].path, nl[i].path)) failed[i] = 0, why[i] = 0, nframes[i] = 0;
  bool loading = job->phase == kLoadOpen || job->phase == kLoadRead || job->phase == kBuild || job->phase == kNorm ||
                 (job->phase == kLoadClose && job->after == kBuild);
  for (int s = 0; s < kSlots; ++s) {
    if (!slots[s].ready && !(loading && job->slot == s)) continue;
    int e = -1;
    for (int i = 0; i < n && e < 0; ++i)
      if (nl[i].path[0] && !strcasecmp(nl[i].path, slots[s].path)) e = i;
    slots[s].entry = e;
    if (loading && job->slot == s) job->entry = e;
    else if (e < 0 && !table_busy(kBuiltins + s)) slots[s].ready = false, st_rel(&tables[kBuiltins + s].frames, 0);
  }
  list = nl;
  nlist = n;
  st.listed = pools::count(pools::kTables);
  __atomic_store_n(&g_list_gen, g_list_gen + 1, __ATOMIC_RELEASE);
  for (int vi = 0; vi < kVoices; ++vi) select(vi);
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

}  // namespace

// ---- the control core's API
CORE_COLD void begin(bool card) {
  card_ok = card;
  if (!begun) {
    begun = true;
    job = new Job();
    job->raw = (uint8_t*)msamp_ram_alloc(kFileMax);   // the big buffers from the board's high heap (platform.h)
    job->x = new float[8192];
    job->c = new float[kSpec];
    job->scratch = new float[4 * 8192];
    lbuf[0] = new Entry[srclib::kEntries];
    lbuf[1] = new Entry[srclib::kEntries];
    list = lbuf[0];
    for (int i = 0; i <= kSine; ++i) sine[i] = sinf(2.0f * 3.14159265f * i / kSine);
    for (int b = 0; b < kBuiltins; ++b) {   // the built-ins: generated now (64 frames each)
      tables[b].data = (float*)msamp_ram_alloc((uint64_t)kBuiltinFrames * kFrameFloats * sizeof(float));
      if (!tables[b].data) continue;   // (no memory: silent)
      float peak = 0;
      for (int f = 0; f < kBuiltinFrames; ++f) {
        builtin_frame(b, f, job->c, job->scratch);
        float* fr = tables[b].data + (size_t)f * kFrameFloats;
        synth_frame(job->c, fr, job->scratch);
        float pk = frame_peak(fr);
        peak = pk > peak ? pk : peak;
      }
      float g = peak > 1e-6f ? 0.95f / peak : 1.0f;
      for (size_t i = 0; i < (size_t)kBuiltinFrames * kFrameFloats; ++i) tables[b].data[i] *= g;
      tables[b].frames = kBuiltinFrames;
    }
    for (int s = 0; s < kSlots; ++s)
      tables[kBuiltins + s].data = (float*)msamp_ram_alloc((uint64_t)kMaxFrames * kFrameFloats * sizeof(float));
    log(CORE_STR("wt: %d built-in tables, %d card slots of %lu KB"), kBuiltins, kSlots,
        (unsigned long)((size_t)kMaxFrames * kFrameFloats * 4 >> 10));
  }
  for (int s = 0; s < kSlots; ++s) slots[s].ready = false, slots[s].entry = -1, slots[s].path[0] = 0, tables[kBuiltins + s].frames = 0;
  composed_sig = 0;
  compose();
  for (int vi = 0; vi < kVoices; ++vi) {
    want[vi] = -1, pend[vi].on = false, pv[vi].wait = pv[vi].on = false;
    sel[vi] = use[vi] = kTables;
    for (int k = 0; k < 16; ++k) vp[vi][k] = k == 2 ? 0 : k == 6 ? 64 : k == 10 ? 100 : k == 11 ? 64 : k == 9 ? 60 : k == 0 ? 1 : 0;   // (TBL A001 before a report: the template's BASIC)
    set_knobs(vi);
  }
  job->phase = kIdle;
}

CORE_COLD void rescan() {   // the card changed: failed files are tried again (the list itself is the pool's)
  if (!job) return;
  memset(failed, 0, sizeof failed);
  memset(why, 0, sizeof why);
}

CORE_COLD bool step() {
  if (!job) return false;
  preview_step();
  switch (job->phase) {
    case kIdle:
      if (srclib::pool_sig(srclib::kWt) != composed_sig) {   // the WAVETABLES pool changed (a row, a state)
        compose();
        return true;
      }
      return start_load();
    case kLoadOpen: return step_open();
    case kLoadRead: return step_read();
    case kLoadClose: return step_close();
    case kBuild: step_build(); return true;
    case kNorm: step_norm(); return true;
  }
  return false;
}

CORE_COLD bool forced(int vi) { return force[vi & 7]; }

CORE_COLD void report(int vi, int note, int vel, const float* p) {
  vi &= 7;
  if (!job) return;
  for (int k = 0; k < 16; ++k) vp[vi][k] = p[k];
  set_knobs(vi);
  select(vi);
  pools::seen(pools::kTables, srclib::code_of(vp[vi][0]));   // the used set
  v[vi].live = true;
  if (note < 0) return;
  int t = resident(want[vi]);
  if (t < 0 && !failed[want[vi]] && !srclib::silent(list, want[vi])) {   // its table loads: the trig waits (kLateMs)
    pend[vi] = {true, (uint8_t)note, (uint8_t)vel, now_ms()};
    ++st.waits;
    return;
  }
  trig(vi, note, vel);
}

CORE_COLD void live_on(int vi, int key, int vel, int ch, int mnote) {
  vi &= 7;
  if (!job || key < 0 || key > 127) return;
  if (__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST) >= (uint32_t)kTables) select(vi);
  cur_live = {true, (int8_t)ch, (uint8_t)mnote};
  start_note(vi, key, vel, 0.6f);   // as poly FM's live notes
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
CORE_COLD const char* entry_group(int e) { return list && e >= 0 && e < nlist ? list[e].grp : ""; }
CORE_COLD const char* entry_path(int e) { return list && e >= 0 && e < nlist ? list[e].path : ""; }
CORE_COLD int entry_lib(int e) { return list && e >= 0 && e < nlist ? list[e].lib : srclib::kLibOff; }
CORE_COLD int entry_state(int e) {   // as gr::entry_state: 0 none 1 card 2 loading 3 RAM 5 failed
  if (!list || e < 0 || e >= nlist || !srclib::playable(list[e])) return 0;
  if (fixed_tab(e) >= 0) return 3;
  if (failed[e]) return 5;
  if (resident(e) >= 0) return 3;
  for (int vi = 0; vi < kVoices; ++vi)
    if (want[vi] == e) return 2;
  return 1;
}
CORE_COLD const char* entry_note(int e, char* b, int cap) {
  if (cap < 1) return "";
  b[0] = 0;
  switch (entry_state(e)) {
    case 5:
      sniprintf(b, cap, "%s", why[e] == 1 ? "TOO BIG: 8 MB MAX" : why[e] == 2 ? "NOT A WAV IT READS" : why[e] == 3 ? "CANNOT OPEN"
                                : why[e] == 4 ? "NO FRAME IN IT" : "FAILED");
      break;
    case 2: sniprintf(b, cap, "LOADING"); break;
    case 3:
      if (fixed_tab(e) < 0 && nframes[e]) sniprintf(b, cap, "%d FRAMES", nframes[e]);
      break;
    default:
      if (list && e >= 0 && e < nlist && list[e].lib == srclib::kLibMissing) sniprintf(b, cap, "MISSING");
      break;
  }
  return b;
}
uint32_t list_gen() { return __atomic_load_n(&g_list_gen, __ATOMIC_ACQUIRE); }
CORE_COLD int src_entry(int vi, int v) { return !list || nlist <= 0 ? -1 : v < 0 ? 0 : v >= nlist ? nlist - 1 : v; }
CORE_COLD bool voice_picked(int) { return false; }   // (no picks over TBL: a choice writes TBL)
CORE_COLD bool pick(int vi, int e) {   // the touchscreen's choice: TBL written on the Rytm (48: the kit keeps it)
  vi &= 7;
  if (!job || !list || e < 0 || e >= nlist) return false;
  ui::remote_param_write(vi, kWaveMachine, 0, e & 127, srclib::lo_of(e));
  vp[vi][0] = (float)(e & 127) + (float)srclib::lo_of(e) / 128.0f;
  select(vi);
  log(CORE_STR("wt: voice %d: TBL %c%03d %s"), vi, 'A' + srclib::bank_of(e), e & 127, entry_name(e));
  return true;
}
CORE_COLD void preview(int vi, int e) {
  vi &= 7;
  if (!job || !list || !(v[vi].wt || force[vi])) return;
  if (e >= 0) {
    if (e >= nlist || !srclib::playable(list[e])) return;
    want[vi] = (int16_t)e;
  }
  pv[vi].wait = true, pv[vi].ms = now_ms();
  if (pv[vi].on) pv[vi].on = false, live_off(vi, kPreviewCh, -1);
}
// ---- the render core's
namespace {
inline float herm(const float* x, float t) {   // x[-1..2]
  float xm1 = x[-1], x0 = x[0], x1 = x[1], x2 = x[2];
  float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, b = c + w + a;
  return ((a * t - b) * t + c) * t + x0;
}
inline float sin01(float p) {   // sin(2 pi p), p in [0, 1)
  float x = p * kSine;
  int i = (int)x;
  i &= kSine - 1;
  float t = x - (float)(int)x;
  return sine[i] + (sine[i + 1] - sine[i]) * t;
}
inline float soft(float x) {   // tanh, rational (|x| <= 3)
  x = x < -3 ? -3 : x > 3 ? 3 : x;
  float x2 = x * x;
  return x * (27 + x2) / (27 + 9 * x2);
}
// the unison's oscillators: detune (of the full spread) and which pair
const float kSpread[kUni] = {0, -0.33f, 0.33f, -0.66f, 0.66f, -1.0f, 1.0f};
constexpr float kDetuneCents = 60.0f;
}  // namespace

static float head_g[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};
static float pos_s[kVoices];   // the render core's smoothed POS (0..1)

void render_add(int vi, float* out, int nfr) {
  if (!begun) return;
  if (nfr > kBlock) {
    for (int d = 0; d < nfr; d += kBlock) render_add(vi, out + d, nfr - d < kBlock ? nfr - d : kBlock);
    return;
  }
  // the table (a hazard slot: the control core never reloads one a render core reads)
  uint32_t t = __atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST);
  for (int i = 0; i < 4; ++i) {
    __atomic_store_n(&use[vi], t, __ATOMIC_SEQ_CST);
    uint32_t t2 = __atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST);
    if (t2 == t) break;
    t = t2;
  }
  uint32_t nf = t < (uint32_t)kTables ? ld_acq(&tables[t].frames) : 0;
  const float* data = nf ? tables[t].data : nullptr;
  const Knobs k = knobs[vi];
  // POS smoothed (10 ms), ramped over the block
  float p0 = pos_s[vi], p1 = p0 + (k.pos - p0) * 0.12f;
  if (fabsf(p1 - k.pos) < 1e-5f) p1 = k.pos;
  pos_s[vi] = p1;
  // the unison: the pairs' weights, the detune
  float w[kUni], cents[kUni];
  int nu = 1;
  float wsum2 = 1;
  w[0] = 1, cents[0] = 0;
  float det = kDetuneCents * k.uni * sqrtf(k.uni);
  for (int j = 1; j < kUni; ++j) {
    float pw = 3 * k.uni - (float)((j - 1) / 2);
    w[j] = pw < 0 ? 0 : pw > 1 ? 1 : pw;
    cents[j] = kSpread[j] * det;
    if (w[j] > 0) nu = j + 1, wsum2 += w[j] * w[j];
  }
  float unorm = 1.0f / sqrtf(wsum2);
  // the warp: its top rate (for the level) and parameters
  float wfac = 1, sync_r = 1, pw_w = 0.5f, fm_i = 0;
  if (k.warp == 1) sync_r = 1 + 7 * k.amt, wfac = sync_r;
  else if (k.warp == 2) pw_w = 0.5f - 0.46f * k.amt, wfac = 0.5f / pw_w;
  else if (k.warp == 3) fm_i = 5.0f * k.amt * k.amt, wfac = 1 + fm_i;
  // the envelopes' rates
  const float sr = 48000.0f;
  float a_inc = 1.0f / (k.atk * sr);
  float dcoef = expf(-6.9078f / (k.dec * sr));   // -60 dB after DEC
  float acc[kBlock] = {0};
  float wk[kPerVoice];
  int nw = 0;
  for (int ni = 0; ni < kPerVoice; ++ni) {
    Note& n = notes[vi][ni];
    uint32_t q = ld_acq(&n.seq);
    if (q != n.r_seq && !(q & 1)) {
      Start c = n.st;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (ld_rlx(&n.seq) == q) {
        n.r_seq = q, n.r = c, n.r_on = true, n.r_rel = false;
        uint32_t s = c.seed;
        n.ph[0] = 0;
        for (int j = 1; j < kUni; ++j) n.ph[j] = s = s * 1664525u + 1013904223u;   // the unison's own phases
        for (int j = 0; j < kUni; ++j) n.fm_ph[j] = n.ph[j];
        n.sub_ph = 0, n.env = 0, n.menv = 0, n.stage = 0, n.mstage = 0, n.ic1 = n.ic2 = 0, n.dc_x = n.dc_y = 0;
      }
    }
    if (!n.r_on) continue;
    if (ld_acq(&n.kill_n) != n.r.kill_gen) {
      n.r_on = false;
      st_rel(&n.done, n.r_seq);
      continue;
    }
    if (!n.r_rel && ld_acq(&n.rel_n) != n.r.rel_gen) n.r_rel = true, n.stage = 2;
    // the oscillators' rates and the level (the highest one's, the warp's rate folded in)
    float inc0 = 440.0f / sr * exp2f((n.r.key - 69.0f) / 12.0f);
    uint32_t iu[kUni];
    float top = 0;
    for (int j = 0; j < nu; ++j) {
      float inc = inc0 * exp2f(cents[j] / 1200.0f);
      top = inc > top ? inc : top;
      iu[j] = (uint32_t)(inc * 4294967296.0f);
    }
    int lv = pick_level(top * wfac);
    uint32_t sub_inc = (uint32_t)(inc0 * 0.5f * 4294967296.0f);
    int lsz = lv < kLevels ? kLevelSize[lv] : 0;
    int loff = lv < kLevels ? level_off(lv) : 0;
    // the filter: DJ-style, 50 % key tracking
    float kt = exp2f((n.r.key - 60.0f) / 24.0f);
    bool hp = k.cut > 64.0f;   // 64: open (LP at 20 kHz)
    float fc = hp ? 20.0f * powf(400.0f, (k.cut - 64.0f) / 64.0f) : 20.0f * powf(1000.0f, k.cut / 64.0f);
    bool bypass = k.res < 0.01f && (hp ? k.cut < 65.0f : k.cut > 62.5f);
    fc *= kt;
    if (fc > 0.45f * sr) fc = 0.45f * sr;
    float g = tanf(3.14159265f * fc / sr), kq = 2.0f - 1.96f * k.res;
    float a1 = 1.0f / (1.0f + g * (g + kq)), a2 = g * a1, a3 = g * a2;
    float drive = k.res * 3.0f, dnorm = 1.0f / sqrtf(1.0f + drive);
    float gain = n.r.gain * 0.5f;
    bool fin = false;
    for (int s = 0; s < nfr; ++s) {
      // the amp envelope
      switch (n.stage) {
        case 0:
          n.env += a_inc;
          if (n.env >= 1) n.env = 1, n.stage = 1;
          break;
        case 1: n.env = k.sus + (n.env - k.sus) * dcoef; break;
        default:
          n.env *= dcoef;
          if (n.env < 1e-4f) fin = true;
          break;
      }
      if (fin) break;
      // the mod envelope (AD)
      if (n.mstage == 0) {
        n.menv += a_inc;
        if (n.menv >= 1) n.menv = 1, n.mstage = 1;
      } else {
        n.menv *= dcoef;
      }
      float y = 0;
      if (data && lsz) {
        float pos = p0 + (p1 - p0) * (float)(s + 1) / (float)nfr + k.env * n.menv;
        pos = pos < 0 ? 0 : pos > 1 ? 1 : pos;
        float fp = pos * (float)(nf - 1);
        uint32_t fa = (uint32_t)fp;
        if (fa > nf - 1) fa = nf - 1;
        uint32_t fb = fa + 1 < nf ? fa + 1 : fa;
        float ft = fp - (float)fa;
        const float* A = data + (size_t)fa * kFrameFloats + loff;
        const float* B = data + (size_t)fb * kFrameFloats + loff;
        float o = 0;
        for (int j = 0; j < nu; ++j) {
          if (w[j] <= 0) {
            n.ph[j] += iu[j];
            continue;
          }
          uint32_t ph = n.ph[j];
          n.ph[j] = ph + iu[j];
          float pf = (float)(ph >> 8) * (1.0f / 16777216.0f);
          switch (k.warp) {
            case 1: pf = pf * sync_r, pf -= (float)(int)pf; break;
            case 2: pf = pf < pw_w ? pf * 0.5f / pw_w : 0.5f + (pf - pw_w) * 0.5f / (1.0f - pw_w); break;
            case 3: {
              uint32_t mp = n.fm_ph[j];
              n.fm_ph[j] = mp + iu[j];
              pf += fm_i * 0.159155f * sin01((float)(mp >> 8) * (1.0f / 16777216.0f));
              pf -= floorf(pf);
              break;
            }
            default: break;
          }
          float x = pf * (float)lsz;
          int i = (int)x;
          float tt = x - (float)i;
          i &= lsz - 1;
          float a = herm(A + i, tt);
          o += w[j] * (a + (herm(B + i, tt) - a) * ft);
        }
        y = o * unorm;
        float d = y - n.dc_x + 0.9995f * n.dc_y;   // the DC blocker (4 Hz)
        n.dc_x = y, n.dc_y = d, y = d;
      }
      if (k.sub > 0) {
        y += k.sub * 0.8f * sin01((float)(n.sub_ph >> 8) * (1.0f / 16777216.0f));
        n.sub_ph += sub_inc;
      }
      if (!bypass) {   // the SVF (Simper's), with the drive in front
        float x0 = drive > 0.01f ? soft(y * (1 + drive)) * dnorm * 1.3f : y;
        float v3 = x0 - n.ic2, v1 = a1 * n.ic1 + a2 * v3, v2 = n.ic2 + a2 * n.ic1 + a3 * v3;
        n.ic1 = 2 * v1 - n.ic1, n.ic2 = 2 * v2 - n.ic2;
        y = hp ? x0 - kq * v1 - v2 : v2;
      }
      acc[s] += y * n.env * gain;
    }
    if (!fin) wk[nw++] = n.env;
    else {
      n.r_on = false;
      st_rel(&n.done, n.r_seq);
    }
  }
  __atomic_store_n(&use[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
  // the headroom (as msamp's / poly FM's): 1/sqrt of the notes' envelopes, smoothed, ramped over the block
  float e = 0;
  for (int j = 0; j < nw; ++j) e += wk[j];
  const float atk = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadAtkMs));
  const float rel = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadRelMs));
  float g0 = head_g[vi], target = e > 1.0f ? 1.0f / sqrtf(e) : 1.0f;
  float g1 = g0 + (target - g0) * (target < g0 ? atk : rel);
  if (fabsf(g1 - target) < 1e-5f) g1 = target;
  head_g[vi] = g1;
  if (!nw) return;
  float dg = (g1 - g0) / (float)nfr;
  for (int s = 0; s < nfr; ++s) out[s] += acc[s] * (g0 + dg * (float)(s + 1));
}

CORE_COLD float headroom(int vi) { return head_g[vi & 7]; }

CORE_COLD bool parse_num(const char** p, int* v) {
  const char* q = *p;
  if (*q < '0' || *q > '9') return false;
  int x = 0;
  while (*q >= '0' && *q <= '9') x = x * 10 + (*q++ - '0');
  *v = x, *p = q;
  return true;
}
CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "wt", 2) != 0 || (p[2] && p[2] != '=')) return false;
  const char* a = p[2] ? p + 3 : "";
  if (!strcmp(a, "rescan")) {
    rescan();
    sniprintf(out, cap, "wt: failed tables tried again");
    return true;
  }
  if (!strncmp(a, "voice:", 6)) {
    int vi = a[6] - '0';
    if (vi < 0 || vi > 7) {
      sniprintf(out, cap, "wt: voice 0..7");
      return true;
    }
    bool on = strcmp(a + 7, ":off") != 0;
    if (force[vi] && !on) silence(vi);
    force[vi] = on;
    sniprintf(out, cap, "wt: voice %d %s", vi, on ? "plays WAVETABLE (any built-in machine the Rytm reports)" : "plays its own machine");
    return true;
  }
  if (!strncmp(a, "pick:", 5)) {   // pick:V:ROW (or a code)
    const char* q = a + 5;
    int vi = q[0] - '0', e = -1;
    const char* r = q[1] == ':' ? q + 2 : "";
    if (vi < 0 || vi > 7 || (!pools::parse_row(r, &e) && !parse_num(&r, &e))) {
      sniprintf(out, cap, "wt: pick:V:ROW (V 0..7, ROW A001..H127)");
      return true;
    }
    if (!pick(vi, e)) sniprintf(out, cap, "wt: no entry %d (%d listed)", e, nlist);
    else sniprintf(out, cap, "wt: voice %d TBL %s", vi, entry_name(want[vi]));
    return true;
  }
  if (!strcmp(a, "list")) {
    for (int i = 0; i < nlist; ++i)
      if (list[i].lib != srclib::kLibHole && list[i].lib != srclib::kLibOff)   // rui1038: banks' empty rows left out
      log(CORE_STR("wt: TBL %c%03d %s [%s]%s%s%s"), 'A' + srclib::bank_of(i), i & 127, list[i].name, list[i].grp, list[i].path[0] ? " (" : " (built in", list[i].path,
          failed[i] ? ") failed" : ")");
    sniprintf(out, cap, "wt: %d tables (the list in the log)", nlist);
    return true;
  }
  if (*a) return false;
  stats();
  int n = sniprintf(out, cap, "wt: %d tables (%d built in), %d/%d card tables resident, %d/%d notes now (peak %d), "
                              "%lu trigs, %lu notes, %lu steals, %lu waits (%lu late), %lu loads (%lu failed)",
                    nlist, kBuiltins, st.resident, kSlots, st.active, kTotal, st.peak, (unsigned long)st.trigs,
                    (unsigned long)st.notes, (unsigned long)st.steals, (unsigned long)st.waits, (unsigned long)st.late,
                    (unsigned long)st.loads, (unsigned long)st.load_fails);
  bool any = false;
  for (int vi = 0; vi < kVoices && n < cap; ++vi)
    if (force[vi]) n += sniprintf(out + n, cap - n, "%s%d", any ? "," : "; forced voices ", vi), any = true;
  return true;
}

const Stats& stats() {
  if (!job) return st;
  st.active = count_active();
  int r = 0;
  for (int s = 0; s < kSlots; ++s) r += slots[s].ready;
  st.resident = r;
  st.listed = pools::count(pools::kTables);
  return st;
}
int voice_entry(int vi) {
  uint32_t t = __atomic_load_n(&sel[vi & 7], __ATOMIC_SEQ_CST);
  if (t >= (uint32_t)kTables) return -1;
  int e = want[vi & 7];
  if (t >= (uint32_t)kBuiltins && e >= 0 && resident(e) == (int)t) return e;
  if (t >= (uint32_t)kBuiltins) return slots[t - kBuiltins].entry;
  if (fixed_tab(e) == (int)t) return e;
  for (int i = 0; i < nlist; ++i)   // a built-in meanwhile (BASIC while a table loads): the row that holds it
    if (fixed_tab(i) == (int)t) return i;
  return -1;
}
uint64_t ram_bytes(uint64_t* capacity) {   // the tables' mip frames in RAM (the pool RAM figure's "tables")
  uint64_t u = 0, c = 0;
  for (int t = 0; t < kTables; ++t) {
    if (!tables[t].data) continue;
    uint64_t f = t < kBuiltins ? (uint64_t)kBuiltinFrames : (uint64_t)kMaxFrames;
    c += f * kFrameFloats * sizeof(float);
    u += (uint64_t)tables[t].frames * kFrameFloats * sizeof(float);
  }
  if (capacity) *capacity = c;
  return u;
}
int table_frames(int e) {
  int t = e >= 0 && e < nlist ? resident(e) : -1;
  return t >= 0 ? (int)tables[t].frames : 0;
}
const float* table_frame(int e, int f) {
  int t = e >= 0 && e < nlist ? resident(e) : -1;
  if (t < 0 || f < 0 || f >= (int)tables[t].frames) return nullptr;
  return tables[t].data + (size_t)f * kFrameFloats;
}
// The display's (racy, drawing only): POS as the voice plays it, the table's frames
CORE_COLD float viz_pos(int vi, int* frames) {
  vi &= 7;
  int e = voice_entry(vi);
  *frames = e >= 0 ? table_frames(e) : 0;
  return pos_s[vi] < 0 ? 0.0f : pos_s[vi] > 1 ? 1.0f : pos_s[vi];
}
CORE_COLD int viz_frame(int vi, float pos, int cols, float* y) {
  int nf = 0;
  viz_pos(vi, &nf);
  int e = voice_entry(vi & 7);
  if (nf <= 0 || cols <= 0) return 0;
  float fp = (pos < 0 ? 0 : pos > 1 ? 1 : pos) * (float)(nf - 1);
  int f0 = (int)fp, f1 = f0 + 1 < nf ? f0 + 1 : f0;
  float t = fp - (float)f0;
  const float *a = table_frame(e, f0), *b = table_frame(e, f1);
  if (!a || !b) return 0;
  a += level_off(0), b += level_off(0);   // level 0: the full 2048 samples
  for (int c = 0; c < cols; ++c) {
    int i = c * kFrame / cols;
    y[c] = a[i] + (b[i] - a[i]) * t;
  }
  return cols;
}

CORE_COLD void reset() {
  for (int vi = 0; vi < kVoices; ++vi) {
    silence(vi);
    force[vi] = false;
    head_g[vi] = 1.0f;
  }
  uint32_t loads = st.loads, fails = st.load_fails;
  memset(&st, 0, sizeof st);
  st.loads = loads, st.load_fails = fails, st.listed = pools::count(pools::kTables);
}

}  // namespace wt
}  // namespace core
#endif
