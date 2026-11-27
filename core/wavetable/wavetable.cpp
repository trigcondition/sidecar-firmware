// core/wavetable/wavetable.cpp - see wavetable.h. Everything but render_add is the control core's and cold.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// The warps FORMANT QUANT BEND SQUEEZE RM and the filters LP / BP / HP 12 and 24 dB and LADDER (the render core's block
// further down: phase_warp, warp_pf, filter_targets, svf_tick, ladder_tick and their helpers) are derived from Vital by
// Matt Tytel (https://github.com/mtytel/vital, commit 636ca0e; Copyright 2013-2019 Matt Tytel), GPL-3.0-or-later: the
// Pi binaries carry it (NOTICE, THIRD_PARTY_LICENSES). Ported to float C and checked against the original's C++ (its
// phase warps, digital filters and ladder). The rest of this file is ours, GPL-2.0-or-later; as one combined file it is
// GPL-3.0-or-later.
#include "../platform.h"

#if CORE_MSAMP
#include "wavetable.h"
#include "simd.h"

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
// P3 WARP's zones (eng_zone of 9) and P16 FTYPE's (of 8)
enum { kWOff, kWSync, kWPwm, kWFm, kWFormant, kWQuant, kWBend, kWSqueeze, kWRm, kWarps };
enum { kFDj, kFLp12, kFLp24, kFBp12, kFBp24, kFHp12, kFHp24, kFLadder, kFTypes };

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
  uint32_t phb[kUni];      // table B's phases (the same rates as A's)
  float env, menv;
  uint8_t stage, mstage;   // amp: 0 attack 1 decay/sustain 2 release; mod: 0 attack 1 decay
  float ic1, ic2;          // the SVF's state (DJ, and the 12 / 24 dB types' main stage)
  float ic1p, ic2p, lin, ls[4][3];   // the 24 dB types' first stage; the ladder's input and four stages
  float coef;              // the new filters' coefficient at the last block's end (ramped to this block's)
  uint8_t ftype;           // the filter type the memories above belong to (a change clears them)
  bool coef_set;
  float dc_x, dc_y;        // the DC blocker's (a warp's asymmetry, PWM's above all, has a mean)
};
Note notes[kVoices][kPerVoice];
uint32_t order_n = 0;

// The voice's knobs (the control core writes them at a report, the render core reads them; floats, a block's tearing
// is harmless)
struct Knobs {
  float pos, amt, uni, sub, cut, res, atk, dec, sus, env;
  float posb, mix;         // table B's POS and the A / B mix (0 only A .. 1 only B)
  int warp, ftype;         // the warp zone (kWarps), the filter type zone (kFTypes)
};
Knobs knobs[kVoices];
float fenv_amt[kVoices];          // the mod envelope's depth onto CUT of the new filters, -1..1 (not a knob: wt=fenv:V:%)
float vp[kVoices][16];
volatile uint32_t sel[kVoices];   // the table id voice vi plays (kTables: none); the control core's
volatile uint32_t use[kVoices];   // the table id its render core reads now (kTables: none)
volatile uint32_t selb[kVoices];  // table B's (kTables: none: B off)
volatile uint32_t useb[kVoices];
volatile uint32_t bwant[kVoices];   // 1: table B is wanted (a row, and it can be heard): the render core fades it in / out
bool b_off[kVoices];              // B was dropped: its table is kept kFadeMs for the fade out, then selb goes
uint32_t b_off_ms[kVoices];
constexpr uint32_t kFadeMs = 40;
int16_t want[kVoices];            // the list entry it wants (-1 none)
int16_t wantb[kVoices];           // table B's
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
    if (__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST) == t || __atomic_load_n(&use[vi], __ATOMIC_SEQ_CST) == t ||
        __atomic_load_n(&selb[vi], __ATOMIC_SEQ_CST) == t || __atomic_load_n(&useb[vi], __ATOMIC_SEQ_CST) == t)
      return true;
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
  k.warp = eng_zone(p[2], kWarps);
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
  k.posb = clamp01(p[13] / 127.0f);
  k.mix = clamp01(p[14] / 127.0f);
  k.ftype = eng_zone(p[15], kFTypes);
}
CORE_COLD void select_a(int vi) {
  int e = srclib::code_of(vp[vi][0]);   // the pool row: bank * 128 + row
  if (!list || e < 0 || e >= nlist) return;
  if (!(v[vi].wt || force[vi])) return;   // not on WAVETABLE (left it: leave()): no selection, no card slot (a compose)
  pools::voice_on(pools::kTables, vi, e);
  // a table that failed to load, picked again (TBL away and back): tried again (the card was busy, the file is
  // fixed); the same TBL at every report or trig does not reload it
  if (e != want[vi] && failed[e]) failed[e] = 0, why[e] = 0;
  want[vi] = (int16_t)e;
  if (srclib::silent(list, e)) return;   // OFF / empty / missing: no sound (a trig skips it)
  int t = resident(e);
  if (t >= kBuiltins) slots[t - kBuiltins].used_ms = now_ms();
  if (t >= 0) __atomic_store_n(&sel[vi], (uint32_t)t, __ATOMIC_SEQ_CST);   // else the old table plays on while it loads
  else if (__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST) >= (uint32_t)kTables)
    __atomic_store_n(&sel[vi], 0u, __ATOMIC_SEQ_CST);   // nothing yet: BASIC meanwhile
}
// table B (P13): OFF (row 000 of a bank, an empty or missing row) is no B. B is wanted (requested from the card, kept in
// its slot) only while it can be heard: MIX above 0, or FM / RM using it as the modulator (an old project's chord words
// read as TBL B rows at MIX 0: they claim no slot). A B that goes keeps its table kFadeMs more, for the render core's
// fade out (b_fade_step); a table loading leaves the B it had (none: silent until it is in).
CORE_COLD bool b_heard(int vi) {   // MIX above 0, or FM / RM using B as the modulator: B can be heard
  vi &= 7;
  int wz = eng_zone(vp[vi][2], kWarps);
  return vp[vi][14] > 0 || wz == kWFm || wz == kWRm;
}
CORE_COLD void select_b(int vi) {
  int e = srclib::code_of(vp[vi][12]);
  if (!list || e < 0 || e >= nlist) return;
  if (!(v[vi].wt || force[vi])) return;
  bool on = !srclib::silent(list, e) && b_heard(vi);
  if (!on) {
    wantb[vi] = -1;
    __atomic_store_n(&bwant[vi], 0u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&selb[vi], __ATOMIC_SEQ_CST) < (uint32_t)kTables && !b_off[vi]) b_off[vi] = true, b_off_ms[vi] = now_ms();
    return;
  }
  b_off[vi] = false;
  if (e != wantb[vi] && failed[e]) failed[e] = 0, why[e] = 0;
  wantb[vi] = (int16_t)e;
  __atomic_store_n(&bwant[vi], 1u, __ATOMIC_SEQ_CST);
  int t = resident(e);
  if (t >= kBuiltins) slots[t - kBuiltins].used_ms = now_ms();
  if (t >= 0) __atomic_store_n(&selb[vi], (uint32_t)t, __ATOMIC_SEQ_CST);
}
CORE_COLD void b_fade_step() {
  for (int vi = 0; vi < kVoices; ++vi)
    if (b_off[vi] && now_ms() - b_off_ms[vi] >= kFadeMs) {
      b_off[vi] = false;
      if (wantb[vi] < 0) __atomic_store_n(&selb[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
    }
}
CORE_COLD void select(int vi) {
  select_a(vi);
  select_b(vi);
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
  start_note(vi, note, vel, 1.0f);   // one note: chords come from a Sidecar poly track (P13..P16 are TBL B ... FTYPE)
  ++st.trigs;
  v[vi].live = true;
  int t = (int)__atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST);
  log(CORE_STR("trig voice %d WAVETABLE %s note %d (%d now, peak %d)"), vi,
      t < kTables && want[vi] >= 0 ? list[want[vi]].name : "-", note, st.active, st.peak);
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
  for (int pass = 0; pass < 2 && e < 0; ++pass)   // every voice's A first, then the B tables (they never starve an A)
    for (int vi = 0; vi < kVoices && e < 0; ++vi) {
      int w = pass ? wantb[vi] : want[vi];
      if (w >= 0 && fixed_tab(w) < 0 && list[w].path[0] && resident(w) < 0 && !failed[w]) e = w;
    }
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
    bool ma = want[vi] >= 0 && resident(want[vi]) == kBuiltins + job->slot;
    bool mb = wantb[vi] >= 0 && resident(wantb[vi]) == kBuiltins + job->slot;
    if (!ma && !mb) continue;
    select(vi);
    if (ma && pend[vi].on) {
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
    want[vi] = wantb[vi] = -1, pend[vi].on = false, pv[vi].wait = pv[vi].on = false;
    sel[vi] = use[vi] = selb[vi] = useb[vi] = kTables;
    bwant[vi] = 0, b_off[vi] = false;
    fenv_amt[vi] = 0;
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
  b_fade_step();
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

CORE_COLD void set_fenv(int vi, float a) { fenv_amt[vi & 7] = a < -1 ? -1 : a > 1 ? 1 : a; }
CORE_COLD float fenv_get(int vi) { return fenv_amt[vi & 7]; }

CORE_COLD void report(int vi, int note, int vel, const float* p) {
  vi &= 7;
  if (!job) return;
  for (int k = 0; k < 16; ++k) vp[vi][k] = p[k];
  set_knobs(vi);
  select(vi);
  pools::seen(pools::kTables, srclib::code_of(vp[vi][0]));   // the used set
  if (wantb[vi] >= 0 && !srclib::silent(list, wantb[vi])) pools::seen(pools::kTables, wantb[vi]);   // (table B's too)
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

CORE_COLD void leave(int vi) {   // the voice left WAVETABLE: its notes end, its selection drops (its card table may go)
  vi &= 7;
  silence(vi);
  want[vi] = wantb[vi] = -1;
  __atomic_store_n(&sel[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
  __atomic_store_n(&selb[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
  bwant[vi] = 0, b_off[vi] = false;
  fenv_amt[vi] = 0;
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
    if (want[vi] == e || wantb[vi] == e) return 2;
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
CORE_COLD void kit_src(int vi, int e) {
  vi &= 7;
  if (!job || !list || e < 0 || e >= nlist) return;
  vp[vi][0] = (float)(e & 127) + (float)srclib::lo_of(e) / 128.0f;
  select(vi);
  pools::seen(pools::kTables, e);   // (the used set, as a report)
}
// 2026-11-24 TBLB: the touchscreen's choice for table B (P13): the Rytm's TBLB written (48, k = 12: the kit keeps it), table B
// selected (select_b alone: table A is untouched). A row 000 is B off. B is silent at MIX 0 unless the warp is FM or RM (b_silent).
CORE_COLD bool pick_b(int vi, int e) {
  vi &= 7;
  if (!job || !list || e < 0 || e >= nlist) return false;
  ui::remote_param_write(vi, kWaveMachine, 12, e & 127, srclib::lo_of(e));
  vp[vi][12] = (float)(e & 127) + (float)srclib::lo_of(e) / 128.0f;
  select_b(vi);
  log(CORE_STR("wt: voice %d: TBLB %c%03d %s"), vi, 'A' + srclib::bank_of(e), e & 127, entry_name(e));
  return true;
}
CORE_COLD void kit_src_b(int vi, int e) {   // the Rytm's own P13 on an idle voice (kit_src's for table B)
  vi &= 7;
  if (!job || !list || e < 0 || e >= nlist) return;
  vp[vi][12] = (float)(e & 127) + (float)srclib::lo_of(e) / 128.0f;
  if (!b_heard(vi)) return;   // not audible by the voice's last values (they may be another sound's): no load, no "used" mark
  select_b(vi);
  pools::seen(pools::kTables, e);
}
CORE_COLD bool b_open(int vi) { return b_heard(vi & 7); }
CORE_COLD int table_b_code(int vi) { return srclib::code_of(vp[vi & 7][12]); }
CORE_COLD bool b_silent(int vi) {   // a table is set for B and MIX / the warp keep it from being heard
  vi &= 7;
  int e = srclib::code_of(vp[vi][12]);
  return list && e >= 0 && e < nlist && !srclib::silent(list, e) && !b_heard(vi);
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

// ---- the warps and filters below are derived from Vital by Matt Tytel (GPL-3.0-or-later), as a port
// checked against its C++ (NOTICE): syncPhase / quantizePhase / bendPhase / squeezePhase / halfSinWindow / rmWindow and
// their amount laws (synth_oscillator.cpp), the digital SVF and the ladder (filters/), futils' tanh.
constexpr float kPhaseMult = 4294967296.0f, kInvPhaseMult = 1.0f / 4294967296.0f;
constexpr int kMaxSync = 16;
inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   // NaN -> lo
// utils::toInt (truncation; out of range is INT_MIN, through int64 as on the original's x86 build)
inline int32_t to_int(float x) { return (int32_t)(uint32_t)(int64_t)x; }
inline float truncf_(float x) { return (float)(int64_t)x; }
inline float fsin(float ph) {   // futils::sin, phase in [-0.5, 0.5] cycles
  float a = ph * (8.0f - 16.0f * fabsf(ph));
  return a * (0.776f + 0.224f * fabsf(a));
}
inline float ftanh(float v) {   // futils::tanh (a rational fit)
  float a = fabsf(v), s = v * v;
  float num = v * (2.45550750702956f + 2.45550750702956f * a + s * (0.893229853513558f + 0.821226666969744f * a));
  float den = 2.44506634652299f + (2.44506634652299f + s) * fabsf(v + 0.814642734961073f * v * a);
  return num / den;
}
inline float hard_tanh(float v) {   // linear to 0.66, then the fit
  float c = clampf(v, -0.66f, 0.66f);
  if (c == v) return v;
  return c + ftanh((v - c) * (1.0f / 0.34f)) * 0.34f;
}
inline float algebraic_sat(float v) {
  float s = v * v;
  return v - v * s * 0.9f / (s + 3.0f);
}
inline float tan_pw(float x) {   // tan(x) for 0 .. 1.43 (the filters' prewarp), Pade, relative error < 1e-4 there
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}

// A block's warp: the zone and its amount laws (WP::d is Vital's setDistortionValues for the new warps), the old ones'
// own (SYNC 1..8x, PWM 50 % .. 4 %, FM index 0..5)
struct WP {
  int warp;
  float sync_r, pw_w, fm_i, d;
};
// the phase warps of FORMANT QUANT BEND SQUEEZE on Vital's signed 32-bit phase (ph 0 = the frame's start): the phase
// read, *win the window (left as it is when the warp has none)
inline int32_t phase_warp(int warp, int32_t ph, float d, float* win) {
  int32_t dp = ph;
  switch (warp) {
    case kWFormant:
      dp = (int32_t)((uint32_t)to_int((float)(int32_t)((uint32_t)ph + 0x80000000u) * d) * (uint32_t)kMaxSync + 0x80000000u);
      *win = fsin((float)(int32_t)((uint32_t)ph + 0x7FFFFFFFu) * (kInvPhaseMult * 0.5f) + 0.25f);
      break;
    case kWQuant: {
      float np = (float)ph * d * kInvPhaseMult;
      dp = to_int(truncf_(np) / d * kPhaseMult);
    } break;
    case kWBend: {
      float fp = (float)ph * kInvPhaseMult + 0.5f, off = (d - d * d) * 2.0f, fp2 = fp * fp, fp3 = fp * fp2;
      float np = fp3 + (d * 3.0f + off) * (fp2 - fp3) + (d * 3.0f - off) * (fp - fp2 * 2.0f + fp3);
      dp = to_int((np - 0.5f) * kPhaseMult);
    } break;
    case kWSqueeze: {
      float fp = (float)ph, a = fabsf(fp), np;
      if (a > d * (kPhaseMult * 0.25f)) np = kPhaseMult * 0.5f - (kPhaseMult * 0.5f - a) / (2.0f - d);
      else np = a / d;
      dp = to_int(fp > 0.0f ? np : -np);
    } break;
    default: break;
  }
  return dp;
}
// the table's phase (0..1) for an oscillator phase ph; m the modulator (FM: the index's multiplier, RM: the window's)
inline float warp_pf(const WP& w, uint32_t ph, float m, float* win) {
  float pf = (float)(ph >> 8) * (1.0f / 16777216.0f);
  switch (w.warp) {
    case kWSync: pf = pf * w.sync_r, pf -= (float)(int)pf; break;
    case kWPwm: pf = pf < w.pw_w ? pf * 0.5f / w.pw_w : 0.5f + (pf - w.pw_w) * 0.5f / (1.0f - w.pw_w); break;
    case kWFm:
      pf += w.fm_i * 0.159155f * m;
      pf -= floorf(pf);
      break;
    case kWFormant: case kWQuant: case kWBend: case kWSqueeze:
      pf = (float)((uint32_t)phase_warp(w.warp, (int32_t)ph, w.d, win) >> 8) * (1.0f / 16777216.0f);
      break;
    case kWRm: *win = 1.0f + (m - 1.0f) * w.d; break;
    default: break;
  }
  return pf;
}
inline float read2(const float* A, const float* B, float pf, int lsz, float ft) {   // frames A and B morphed by ft
  float x = pf * (float)lsz;
  int i = (int)x;
  float tt = x - (float)i;
  i &= lsz - 1;
  float a = herm(A + i, tt);
  return a + (herm(B + i, tt) - a) * ft;
}
// the table's pair of frames at pos (0..1) and the morph between them
inline void frames_at(const float* data, uint32_t nf, float pos, int loff, const float** A, const float** B, float* ft) {
  pos = pos < 0 ? 0 : pos > 1 ? 1 : pos;
  float fp = pos * (float)(nf - 1);
  uint32_t fa = (uint32_t)fp;
  if (fa > nf - 1) fa = nf - 1;
  uint32_t fb = fa + 1 < nf ? fa + 1 : fa;
  *ft = fp - (float)fa;
  *A = data + (size_t)fa * kFrameFloats + loff;
  *B = data + (size_t)fb * kFrameFloats + loff;
}

// The new filters' block constants (Vital's DigitalSvf::setupFilter, LadderFilter::setupFilter / setStageScales as
// a port's filter_targets): the blend, resonance and drive of the type and RES
struct FT {
  bool two, ladder;    // 24 dB (two stages), the ladder
  float res, drv, b0, b1, b2, sc[5];
};
inline void filter_targets(int ft, float r, FT* o) {
  static const int8_t kBl[kFTypes] = {0, -1, -1, 0, 0, 1, 1, -1};   // pass blend - 1: LP -1, BP 0, HP +1
  float bl = (float)kBl[ft];
  float band = sqrtf(1.0f - bl * bl), low = bl < 0.0f ? -bl : 0.0f, high = bl > 0.0f ? bl : 0.0f;
  o->ladder = ft == kFLadder;
  o->two = ft == kFLp24 || ft == kFBp24 || ft == kFHp24;
  if (!o->ladder) {
    float ra = r * r * r, q = 0.5f + (16.0f - 0.5f) * ra;
    o->res = 1.0f / q;
    o->drv = 1.0f / (ra * 2.0f + 1.0f);
    o->b0 = high, o->b1 = band - o->res * high, o->b2 = low - high;
  } else {
    float ra = sinf(r * 1.5707963f);
    o->res = 0.001f + (4.1f - 0.001f) * ra;
    float dm = sinf(r) + 1.0f;
    o->drv = dm + (1.0f - dm) * high;
    float bp = 1.0f - fabsf(bl);
    o->sc[0] = high, o->sc[1] = -4.0f * high, o->sc[2] = -bp + 6.0f * high;
    o->sc[3] = 2.0f * bp - 4.0f * high, o->sc[4] = low - bp + high;
  }
}
// DigitalSvf::tick (12 dB) or tick24 (a resonance-1 stage, hardTanh, then tick); g = tan(pi cutoff / fs)
inline float svf_tick(Note& n, const FT& f, float x, float g) {
  float g2 = g * g, in = f.drv * x;
  if (f.two) {
    float p0 = 1.0f / (g2 + g + 1.0f), p1 = p0 * g, p2 = p0 * g2;
    float v3 = in - n.ic2p, v1 = p0 * n.ic1p + p1 * v3, v2 = n.ic2p + p1 * n.ic1p + p2 * v3;
    n.ic1p = v1 * 2.0f - n.ic1p, n.ic2p = v2 * 2.0f - n.ic2p;
    in = hard_tanh(f.b0 * in + f.b1 * v1 + f.b2 * v2);
  }
  float c0 = 1.0f / (g2 + g * f.res + 1.0f), c1 = c0 * g, c2 = c0 * g2;
  float v3 = in - n.ic2, v1 = c0 * n.ic1 + c1 * v3, v2 = n.ic2 + c1 * n.ic1 + c2 * v3;
  n.ic1 = v1 * 2.0f - n.ic1, n.ic2 = v2 * 2.0f - n.ic2;
  return hard_tanh(f.b0 * in + f.b1 * v1 + f.b2 * v2);
}
// LadderFilter::tick and the stage-scale mix (24 dB), the stages OnePoleFilter<algebraicSat>; c = tan(pr / (pr + 1)),
// pr = pi cutoff / fs
inline float ladder_tick(Note& n, const FT& f, float x, float c) {
  float g1 = c * 1.66f, g2 = g1 * g1, g3 = g1 * g2;
  float fs = n.ls[3][2] + g1 * n.ls[2][2];
  fs = fs + g2 * n.ls[1][2];
  fs = fs + g3 * n.ls[0][2];
  n.lin = ftanh(x * f.drv - f.res * fs);
  float u = n.lin;
  for (int k = 0; k < 4; ++k) {
    float* q = n.ls[k];
    float delta = c * (u - q[2]);
    q[1] += delta;
    q[0] = algebraic_sat(q[1]);
    q[1] += delta;
    q[2] = algebraic_sat(q[1]);
    u = q[0];
  }
  return f.sc[0] * n.lin + f.sc[1] * n.ls[0][0] + f.sc[2] * n.ls[1][0] + f.sc[3] * n.ls[2][0] + f.sc[4] * n.ls[3][0];
}
inline void clear_filters(Note& n) {
  n.ic1 = n.ic2 = n.ic1p = n.ic2p = n.lin = 0.0f;
  for (int k = 0; k < 4; ++k) n.ls[k][0] = n.ls[k][1] = n.ls[k][2] = 0.0f;
}
// The lanes of a sample for what the fast paths above do not take: the new warps, and table B with PWM / FM / the new
// warps (B warped as A is; under FM and RM B is A's modulator and is read plain). o, ob: A's and B's weighted sums.
__attribute__((noinline)) void lanes_generic(Note& n, int nu, const uint32_t* iu, const float* w, const WP& wp, bool modulated, bool read_b,
                                             int lsz, int lszb, const float* A, const float* B, float ft, const float* Bb, const float* Bc,
                                             float ftb, float* o_out, float* ob_out) {
  const WP plain = {kWOff, 1, 0.5f, 0, 0};
  float o = 0, ob = 0;
  for (int j = 0; j < nu; ++j) {
    uint32_t pa = n.ph[j], pb = n.phb[j], pm = n.fm_ph[j];
    n.ph[j] = pa + iu[j], n.phb[j] = pb + iu[j], n.fm_ph[j] = pm + iu[j];
    if (w[j] <= 0) continue;
    float m = 0, bl = 0;
    if (modulated) {   // B as it is is the modulator (a sine at the note's rate while B is off)
      float wb = 1;
      if (read_b) m = bl = read2(Bb, Bc, warp_pf(plain, pb, 0, &wb), lszb, ftb);
      else m = sin01((float)(pm >> 8) * (1.0f / 16777216.0f));
    } else if (read_b) {   // B warped as A is
      float wb = 1;
      float pfb = warp_pf(wp, pb, 0, &wb);
      bl = wb * read2(Bb, Bc, pfb, lszb, ftb);
    }
    float wa = 1;
    float pfa = warp_pf(wp, pa, m, &wa);
    o += w[j] * wa * read2(A, B, pfa, lsz, ft);
    ob += w[j] * bl;
  }
  *o_out = o, *ob_out = ob;
}
}  // namespace

static float head_g[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};
static float pos_s[kVoices];   // the render core's smoothed POS (0..1)
static float posb_s[kVoices];  // ... and POS B
static float mix_s[kVoices];   // ... and the A / B mix as played (0 until B is in and wanted: it fades in and out)
static float amt_s[kVoices];   // ... and AMT for the new warps (FORMANT .. RM: a step of QUANT's law would click)

void render_add(int vi, float* out, int nfr) {
  if (!begun) return;
  if (nfr > kBlock) {
    for (int d = 0; d < nfr; d += kBlock) render_add(vi, out + d, nfr - d < kBlock ? nfr - d : kBlock);
    return;
  }
  // the tables (a hazard slot each: the control core never reloads one a render core reads)
  uint32_t t = __atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST), tb = __atomic_load_n(&selb[vi], __ATOMIC_SEQ_CST);
  bool held = false;
  for (int i = 0; i < 4 && !held; ++i) {
    __atomic_store_n(&use[vi], t, __ATOMIC_SEQ_CST);
    __atomic_store_n(&useb[vi], tb, __ATOMIC_SEQ_CST);
    uint32_t t2 = __atomic_load_n(&sel[vi], __ATOMIC_SEQ_CST), tb2 = __atomic_load_n(&selb[vi], __ATOMIC_SEQ_CST);
    held = t2 == t && tb2 == tb;
    t = t2, tb = tb2;
  }
  if (!held) {   // the selection kept moving: this block is skipped (nothing is read that use does not name)
    __atomic_store_n(&use[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
    __atomic_store_n(&useb[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
    return;
  }
  uint32_t nf = t < (uint32_t)kTables ? ld_acq(&tables[t].frames) : 0;
  const float* data = nf ? tables[t].data : nullptr;
  uint32_t nfb = tb < (uint32_t)kTables ? ld_acq(&tables[tb].frames) : 0;
  const float* datab = nf && nfb ? tables[tb].data : nullptr;   // B plays with an A
  const bool bon = datab != nullptr;
  const Knobs k = knobs[vi];
  // POS smoothed (10 ms), ramped over the block
  float p0 = pos_s[vi], p1 = p0 + (k.pos - p0) * 0.12f;
  if (fabsf(p1 - k.pos) < 1e-5f) p1 = k.pos;
  pos_s[vi] = p1;
  float q0 = posb_s[vi], q1 = q0 + (k.posb - q0) * 0.12f;
  if (fabsf(q1 - k.posb) < 1e-5f) q1 = k.posb;
  posb_s[vi] = q1;
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
  WP wp = {k.warp, 1, 0.5f, 0, 0};
  float wfac = 1;
  if (k.warp >= kWFormant) {   // the new warps' amount smoothed (a block's steps would click QUANT)
    float a0 = amt_s[vi], a1 = a0 + (k.amt - a0) * 0.12f;
    if (fabsf(a1 - k.amt) < 1e-5f) a1 = k.amt;
    amt_s[vi] = a1;
    float a = a1;
    switch (k.warp) {
      case kWFormant: {   // Vital's law: the sync ratio 1/16 .. 16 (the window is the half sine)
        float ratio = exp2f((a - 0.5f) * 2.0f * 4.0f);
        wp.d = ratio * (1.0f / (float)kMaxSync);
        wfac = ratio < 1 ? 1 : ratio;
        break;
      }
      case kWQuant: {   // 2 steps (AMT 127) .. none (0)
        float d = 1.0f - a;
        d = d * d * d * 0.85f;
        wp.d = exp2f(d * 32.0f + 1.0f);
        break;
      }
      case kWBend: wp.d = a, wfac = 2; break;
      case kWSqueeze: {
        wp.d = a * 2.0f * 0.95f + (1.0f - 0.95f);
        float m = wp.d < 2.0f - wp.d ? wp.d : 2.0f - wp.d;
        wfac = 1.0f / m;
        wfac = wfac > 8.0f ? 8.0f : wfac;
        break;
      }
      default: wp.d = a, wfac = 2; break;   // RM: the window's depth
    }
  } else {
    amt_s[vi] = k.amt;
    if (k.warp == kWSync) wp.sync_r = 1 + 7 * k.amt, wfac = wp.sync_r;
    else if (k.warp == kWPwm) wp.pw_w = 0.5f - 0.46f * k.amt, wfac = 0.5f / wp.pw_w;
    else if (k.warp == kWFm) wp.fm_i = 5.0f * k.amt * k.amt, wfac = 1 + wp.fm_i;
  }
  const bool modulated = k.warp == kWFm || k.warp == kWRm;   // B (or a sine) is the modulator
  // the A / B mix (equal power); B off: A alone, untouched
  // (B is faded: the mix moves 1/40 a block toward MIX while B is wanted and in, toward 0 otherwise; 0 with no B at all)
  const float mx0 = mix_s[vi];
  float mt = bon && __atomic_load_n(&bwant[vi], __ATOMIC_SEQ_CST) ? k.mix : 0.0f;
  float mx1 = mx0 + (mt > mx0 ? (mt - mx0 < 0.025f ? mt - mx0 : 0.025f) : (mx0 - mt < 0.025f ? mt - mx0 : -0.025f));
  if (!bon) mx1 = 0;
  mix_s[vi] = mx1;
  const bool mramp = mx0 != mx1;
  const float ga = bon ? cosf(mx1 * 1.5707963f) : 1.0f, gb = bon ? sinf(mx1 * 1.5707963f) : 0.0f;
  const bool read_b = bon && (mx0 > 1e-4f || mx1 > 1e-4f || modulated);
  // the filter's block constants (the new types)
  FT fl = {};
  if (k.ftype != kFDj) filter_targets(k.ftype, k.res, &fl);
  const float fenv = fenv_amt[vi];
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
        for (int j = 0; j < kUni; ++j) n.phb[j] = s = s * 1664525u + 1013904223u;   // B's own (the same rates)
        n.sub_ph = 0, n.env = 0, n.menv = 0, n.stage = 0, n.mstage = 0, n.ic1 = n.ic2 = 0, n.dc_x = n.dc_y = 0;
        clear_filters(n);
        n.ftype = (uint8_t)k.ftype, n.coef_set = false;
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
    // table B read as the modulator of FM / RM is plain: its own level (the note's, not the warp's)
    int lvb = modulated ? pick_level(top) : lv;
    int lszb = lvb < kLevels ? kLevelSize[lvb] : 0, loffb = lvb < kLevels ? level_off(lvb) : 0;
    // the filter: DJ-style (50 % key tracking), or one of the new types (Vital's cutoff law: 8 .. 136 semitones above
    // C-1 as CUT 0..127, the same key tracking, the mod envelope x FENV x 64 semitones)
    float kt = exp2f((n.r.key - 60.0f) / 24.0f);
    bool hp = k.cut > 64.0f;   // 64: open (LP at 20 kHz)
    bool bypass = false;
    float g = 0, a1 = 0, a2 = 0, a3 = 0, drive = 0, dnorm = 1;
    float coef = 0, d_coef = 0;
    if (n.ftype != k.ftype) {   // a filter type change: what the new type does not share is cleared
      auto family = [](int f) { return f == kFDj ? 0 : f == kFLadder ? 2 : 1; };
      auto two = [](int f) { return f == kFLp24 || f == kFBp24 || f == kFHp24; };
      if (family(n.ftype) != family(k.ftype)) clear_filters(n), n.coef_set = false;
      else if (family(k.ftype) == 1 && two(n.ftype) != two(k.ftype)) n.ic1p = n.ic2p = 0;
      n.ftype = (uint8_t)k.ftype;
    }
    if (k.ftype == kFDj) {
      float fc = hp ? 20.0f * powf(400.0f, (k.cut - 64.0f) / 64.0f) : 20.0f * powf(1000.0f, k.cut / 64.0f);
      bypass = k.res < 0.01f && (hp ? k.cut < 65.0f : k.cut > 62.5f);
      fc *= kt;
      if (fc > 0.45f * sr) fc = 0.45f * sr;
      g = tanf(3.14159265f * fc / sr);
      float kq = 2.0f - 1.96f * k.res;
      a1 = 1.0f / (1.0f + g * (g + kq)), a2 = g * a1, a3 = g * a2;
      drive = k.res * 3.0f, dnorm = 1.0f / sqrtf(1.0f + drive);
    } else {
      float semis = 8.0f + k.cut * (128.0f / 127.0f) + 0.5f * (n.r.key - 60.0f) + 64.0f * fenv * n.menv;
      float r = exp2f(semis * (1.0f / 12.0f)) * (8.1757989f / sr);
      float rmax = fl.ladder ? 20000.0f / sr : 0.45f;
      r = r > rmax ? rmax : r;
      float c;
      if (fl.ladder) {
        float pr = 3.14159265f * r;
        c = tan_pw(pr / (pr + 1.0f));
      } else {
        c = tan_pw(3.14159265f * r);
      }
      float c0 = n.coef_set ? n.coef : c;
      n.coef = c, n.coef_set = true;
      coef = c0, d_coef = (c - c0) / (float)nfr;
    }
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
        const float *A, *B, *Bb, *Bc;
        float ft, ftb = 0;
        frames_at(data, nf, pos, loff, &A, &B, &ft);
        Bb = Bc = nullptr;
        if (bon) {
          float posb = q0 + (q1 - q0) * (float)(s + 1) / (float)nfr + k.env * n.menv;
          frames_at(datab, nfb, posb, loffb, &Bb, &Bc, &ftb);
        }
        float o = 0, ob = 0;
        if (!bon && k.warp <= kWSync && w[nu - 1] > 0) o = simd::wt_unison(n.ph, iu, w, nu, k.warp, wp.sync_r, lsz, A, B, ft);   // (nu: the weights > 0)
        else if (!bon && k.warp <= kWFm) {   // PWM, FM (B off: FM's modulator is a sine at the note's rate)
          const float pw_w = wp.pw_w, fm_i = wp.fm_i;
          for (int j = 0; j < nu; ++j) {
            if (w[j] <= 0) {
              n.ph[j] += iu[j];
              continue;
            }
            uint32_t ph = n.ph[j];
            n.ph[j] = ph + iu[j];
            float pf = (float)(ph >> 8) * (1.0f / 16777216.0f);
            switch (k.warp) {
              case kWPwm: pf = pf < pw_w ? pf * 0.5f / pw_w : 0.5f + (pf - pw_w) * 0.5f / (1.0f - pw_w); break;
              case kWFm: {
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
        } else if (bon && k.warp <= kWSync) {   // table B the same way: its own phases, the same rates
          o = simd::wt_unison(n.ph, iu, w, nu, k.warp, wp.sync_r, lsz, A, B, ft);
          if (read_b) ob = simd::wt_unison(n.phb, iu, w, nu, k.warp, wp.sync_r, lsz, Bb, Bc, ftb);
          else
            for (int j = 0; j < nu; ++j) n.phb[j] += iu[j];
        } else {   // PWM, FM and the new warps (and B)
          lanes_generic(n, nu, iu, w, wp, modulated, read_b, lsz, lszb, A, B, ft, Bb, Bc, ftb, &o, &ob);
        }
        if (bon && mramp) {   // the mix moves over the block
          float ang = (mx0 + (mx1 - mx0) * (float)(s + 1) / (float)nfr) * 1.5707963f;
          y = (cosf(ang) * o + sinf(ang) * ob) * unorm;
        } else {
          y = (bon ? ga * o + gb * ob : o) * unorm;
        }
        float d = y - n.dc_x + 0.9995f * n.dc_y;   // the DC blocker (4 Hz)
        n.dc_x = y, n.dc_y = d, y = d;
      }
      if (k.sub > 0) {
        y += k.sub * 0.8f * sin01((float)(n.sub_ph >> 8) * (1.0f / 16777216.0f));
        n.sub_ph += sub_inc;
      }
      if (k.ftype == kFDj) {
        if (!bypass) {   // the SVF (Simper's), with the drive in front
          float kq = 2.0f - 1.96f * k.res;
          float x0 = drive > 0.01f ? soft(y * (1 + drive)) * dnorm * 1.3f : y;
          float v3 = x0 - n.ic2, v1 = a1 * n.ic1 + a2 * v3, v2 = n.ic2 + a2 * n.ic1 + a3 * v3;
          n.ic1 = 2 * v1 - n.ic1, n.ic2 = 2 * v2 - n.ic2;
          y = hp ? x0 - kq * v1 - v2 : v2;
        }
      } else {
        coef += d_coef;
        // (Vital's oscillators play at half scale into its filters, its hardTanh's linear range: the same here, the
        // level made up after, so the passband keeps the DJ filter's level)
        y = fl.ladder ? ladder_tick(n, fl, 0.5f * y, coef) : svf_tick(n, fl, 0.5f * y, coef);
        if (!(y > -4.0f && y < 4.0f)) clear_filters(n), y = 0.0f;   // never NaN, never runaway
        y *= 2.0f;
      }
      acc[s] += y * n.env * gain;
    }
    if (k.ftype != kFDj) {   // no subnormals in the new filters' memories (a held note on a flat input decays into them)
      auto fl0 = [](float& x) { if (fabsf(x) < 1e-20f) x = 0.0f; };
      fl0(n.ic1), fl0(n.ic2), fl0(n.ic1p), fl0(n.ic2p), fl0(n.lin);
      for (int q = 0; q < 4; ++q) fl0(n.ls[q][0]), fl0(n.ls[q][1]), fl0(n.ls[q][2]);
    }
    if (!fin) wk[nw++] = n.env;
    else {
      n.r_on = false;
      st_rel(&n.done, n.r_seq);
    }
  }
  __atomic_store_n(&use[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
  __atomic_store_n(&useb[vi], (uint32_t)kTables, __ATOMIC_SEQ_CST);
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
  for (int n = 0; *q >= '0' && *q <= '9' && n < 6; ++n) x = x * 10 + (*q++ - '0');   // (six digits at most: no overflow)
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
  if (!strncmp(a, "fenv:", 5)) {   // fenv:V:PERCENT (-100..100): the mod envelope's depth onto CUT of the new filters
    int vi = a[5] - '0', pc = 0;
    const char* q = a[5] && a[6] == ':' ? a + 7 : "";
    bool neg = *q == '-';
    q += neg;
    if (vi < 0 || vi > 7 || !parse_num(&q, &pc) || *q || pc > 100) {
      sniprintf(out, cap, "wt: fenv:V:PERCENT (V 0..7, -100..100)");
      return true;
    }
    if (neg) pc = -pc;
    set_fenv(vi, (float)pc / 100.0f);
    sniprintf(out, cap, "wt: voice %d filter envelope %d %%", vi, pc);
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
  if (!strncmp(a, "pickb:", 6)) {   // pickb:V:ROW (or a code): table B (TBLB, P13)
    const char* q = a + 6;
    int vi = q[0] - '0', e = -1;
    const char* r = q[1] == ':' ? q + 2 : "";
    if (vi < 0 || vi > 7 || (!pools::parse_row(r, &e) && !parse_num(&r, &e))) {
      sniprintf(out, cap, "wt: pickb:V:ROW (V 0..7, ROW A001..H127)");
      return true;
    }
    if (!pick_b(vi, e)) sniprintf(out, cap, "wt: no entry %d (%d listed)", e, nlist);
    else sniprintf(out, cap, "wt: voice %d TBLB %s%s", vi, entry_name(e), b_silent(vi) ? " (silent: MIX 0)" : "");
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
bool denormal_state(int vi) {
  for (const Note& n : notes[vi & 7]) {
    float x[] = {n.ic1, n.ic2, n.ic1p, n.ic2p, n.lin, n.ls[0][0], n.ls[0][1], n.ls[0][2], n.ls[1][0], n.ls[1][1], n.ls[1][2],
                 n.ls[2][0], n.ls[2][1], n.ls[2][2], n.ls[3][0], n.ls[3][1], n.ls[3][2]};
    for (float y : x)
      if (fpclassify(y) == FP_SUBNORMAL) return true;
  }
  return false;
}
int voice_entry_b(int vi) {
  uint32_t t = __atomic_load_n(&selb[vi & 7], __ATOMIC_SEQ_CST);
  int e = wantb[vi & 7];
  if (t >= (uint32_t)kTables || e < 0) return -1;
  return resident(e) == (int)t || fixed_tab(e) == (int)t ? e : -1;
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
  rnd = 0x9e3779b9u;   // (the unison's random phases from the same start: the tests' renders repeat)
  for (int vi = 0; vi < kVoices; ++vi) {
    silence(vi);
    force[vi] = false;
    head_g[vi] = 1.0f;
    mix_s[vi] = 0;
    fenv_amt[vi] = 0;
  }
  uint32_t loads = st.loads, fails = st.load_fails;
  memset(&st, 0, sizeof st);
  st.loads = loads, st.load_fails = fails, st.listed = pools::count(pools::kTables);
}

}  // namespace wt
}  // namespace core
#endif
