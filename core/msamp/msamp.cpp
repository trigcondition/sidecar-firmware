// core/msamp/msamp.cpp - see msamp.h. Everything but render_add is the control core's and cold.
#include "../platform.h"

#if CORE_MSAMP
#include "msamp.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "../fmt/fmt.h"
#include "../lib/library.h"
#include "../lib/pools.h"
#include "../pool/pcache.h"
#include "../pool/stream_budget.h"
#include "../lib/srclib.h"
#include "../machines.h"
#include "../ui/remote.h"
#include "../poly.h"
#include "cardq.h"
#include "text.h"
#include "../voices.h"

namespace core {
namespace msamp {

namespace {

// ---- atomics (the mailbox between the control core and a render core)
inline uint32_t ld_acq(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline uint32_t ld_rlx(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
inline void st_rel(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline void st_rlx(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

inline uint16_t le16(const uint8_t* b) { return (uint16_t)(b[0] | b[1] << 8); }
inline uint32_t le32(const uint8_t* b) { return (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24); }
inline uint16_t be16(const uint8_t* b) { return (uint16_t)(b[0] << 8 | b[1]); }
inline uint32_t be32(const uint8_t* b) { return (uint32_t)((uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]); }

// ---- the audio files
struct Audio {
  uint8_t fmt;      // 1 integer PCM, 3 IEEE float
  uint8_t bits, ch;
  bool be;          // big-endian samples (AIFF)
  float rate;
  uint32_t data_off, frames;
  bool has_loop;
  uint32_t loop_s, loop_e;   // inclusive (a WAV's smpl chunk)
};

// WAV (RIFF/WAVE, fmt / data / smpl anywhere) or AIFF / AIFC (COMM, SSND; NONE, sowt, fl32). A few small reads.
CORE_COLD bool audio_header(File& f, Audio* a) {
  memset(a, 0, sizeof *a);
  uint8_t b[12];
  uint64_t size = f.fileSize();
  if (!f.seekSet(0) || f.read(b, 12) != 12) return false;
  bool wav = !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4);
  bool aiff = !memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4));
  if (!wav && !aiff) return false;
  uint64_t pos = 12;
  bool fmt = false, data = false;
  uint32_t data_len = 0;
  for (int guard = 0; guard < 64 && pos + 8 <= size; ++guard) {
    uint8_t c[48];
    if (!f.seekSet(pos) || f.read(c, 8) != 8) break;
    uint32_t sz = wav ? le32(c + 4) : be32(c + 4);
    uint32_t want = sz < 40 ? sz : 40;
    if (wav && !memcmp(c, "fmt ", 4)) {
      if (sz < 16 || f.read(c + 8, want) != (int)want) return false;
      uint16_t tag = le16(c + 8);
      a->ch = (uint8_t)le16(c + 10);
      a->rate = (float)le32(c + 12);
      a->bits = (uint8_t)le16(c + 22);
      if (tag == 0xFFFE && sz >= 26 + 8) tag = le16(c + 8 + 24);   // extensible: the SubFormat's first two bytes
      a->fmt = tag == 3 ? 3 : tag == 1 ? 1 : 0;
      fmt = true;
    } else if (wav && !memcmp(c, "data", 4)) {
      a->data_off = (uint32_t)pos + 8;
      data_len = sz;
      data = true;
    } else if (wav && !memcmp(c, "smpl", 4) && sz >= 36 + 24) {
      if (f.read(c + 8, 40) != 40) return false;
      // smpl: 36 bytes, then the loops (cue id, type, start, end, fraction, count); the first loop
      uint8_t l[24];
      if (le32(c + 8 + 28) >= 1 && f.seekSet(pos + 8 + 36) && f.read(l, 24) == 24) {
        a->loop_s = le32(l + 8), a->loop_e = le32(l + 12);
        a->has_loop = a->loop_e > a->loop_s;
      }
    } else if (aiff && !memcmp(c, "COMM", 4)) {
      if (sz < 18 || f.read(c + 8, want) != (int)want) return false;
      a->ch = (uint8_t)be16(c + 8);
      a->frames = be32(c + 10);
      a->bits = (uint8_t)be16(c + 14);
      // the rate: an 80-bit extended float
      int e = ((c[16] & 0x7F) << 8 | c[17]) - 16383 - 63;
      uint64_t m = 0;
      for (int i = 0; i < 8; ++i) m = m << 8 | c[18 + i];
      a->rate = (float)ldexp((double)m, e);
      a->fmt = 1, a->be = true;
      if (!memcmp(b + 8, "AIFC", 4) && sz >= 22) {
        if (!memcmp(c + 26, "sowt", 4)) a->be = false;
        else if (!memcmp(c + 26, "fl32", 4) || !memcmp(c + 26, "FL32", 4)) a->fmt = 3;
        else if (memcmp(c + 26, "NONE", 4)) a->fmt = 0;
      }
      fmt = true;
    } else if (aiff && !memcmp(c, "SSND", 4)) {
      if (f.read(c + 8, 8) != 8) return false;
      a->data_off = (uint32_t)pos + 16 + be32(c + 8);
      data_len = sz > 8 ? sz - 8 : 0;
      data = true;
    }
    pos += 8 + (uint64_t)sz + (sz & 1);
  }
  if (!fmt || !data || !a->fmt || a->ch < 1 || a->rate < 1000 || a->rate > 384000) return false;
  if (a->fmt == 1 && a->bits != 8 && a->bits != 16 && a->bits != 24 && a->bits != 32) return false;
  if (a->fmt == 3 && a->bits != 32) return false;
  uint32_t fb = a->ch * (a->bits / 8);
  if ((uint64_t)a->data_off + data_len > size) data_len = (uint32_t)(size > a->data_off ? size - a->data_off : 0);
  uint32_t fr = data_len / fb;
  if (wav || a->frames > fr) a->frames = fr;
  return a->frames > 0;
}

// One sample of channel c in a frame at p (in -1..1 as int16 scale)
inline int32_t sample16(const Audio& a, const uint8_t* p) {
  switch (a.bits) {
    case 8: return ((int32_t)p[0] - 128) << 8;
    case 16: return a.be ? (int16_t)be16(p) : (int16_t)le16(p);
    case 24: {
      int32_t x = a.be ? (p[0] << 24 | p[1] << 16 | p[2] << 8) : (p[2] << 24 | p[1] << 16 | p[0] << 8);
      return (x >> 16) + ((x >> 15) & 1);   // rounded
    }
    default: {
      uint32_t u = a.be ? be32(p) : le32(p);
      if (a.fmt == 3) {
        float x;
        memcpy(&x, &u, 4);
        float y = x * 32767.0f;
        return (int32_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
      }
      return ((int32_t)u >> 16) + (((int32_t)u >> 15) & 1);
    }
  }
}
// frames whole frames of bytes into int16: ch 1 mono (the first two channels summed), ch 2 the first two as L R
// interleaved (a stereo file kept, docs/STEREO.md)
CORE_COLD void convert(const Audio& a, const uint8_t* in, uint32_t frames, int16_t* out, int ch = 1) {
  int bps = a.bits / 8, fb = bps * a.ch;
  if (ch == 2) {
    for (uint32_t i = 0; i < frames; ++i) {
      const uint8_t* p = in + i * fb;
      int32_t x = sample16(a, p), y = a.ch >= 2 ? sample16(a, p + bps) : x;
      out[2 * i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
      out[2 * i + 1] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    return;
  }
  for (uint32_t i = 0; i < frames; ++i) {
    const uint8_t* p = in + i * fb;
    int32_t x = sample16(a, p);
    if (a.ch >= 2) x = (x + sample16(a, p + bps)) >> 1;
    out[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
  }
}
// the layout a sample is held in: its file's channels (2 for 2+), 1 under poolmono (pcache::set_mono)
inline int ram_ch(const Audio& a) { return a.ch >= 2 && !pcache::mono() ? 2 : 1; }

// ---- instruments
struct Smp {
  Audio a;                   // the file's format (streaming reads it again)
  const int16_t* head;       // frames [0, head_n)
  uint32_t head_n;
  const int16_t* res2;       // a loop past the head: frames [ws, we), nullptr none
  uint32_t ws, we;
  uint32_t frames;
  float rate;
  bool ok, has_loop;
  uint32_t loop_s, loop_e;   // inclusive (the file's, else the first region's)
  int8_t part;               // loading: 0 the head, 1 the loop part
  // the pools design 4.1: the head lives in the pool RAM's shared cache (one copy a content: a GRANULAR source or a
  // DSP SAMPLE slot of the same file shares it), the loop part in a private block of the same budget
  int pe;                    // the head's cache entry (-1 none)
  int blk;                   // its block, pinned while the slot holds it (-1 none)
  int priv;                  // the loop part's private block (-1 none)
  bool cached;               // the head was in the cache already (not read here)
  uint8_t ch;                // its head / loop part / ring layout: 1 mono, 2 L R interleaved
};
enum SlotState : uint8_t { kFree, kLoading, kReady };
struct Slot {
  SlotState state;
  bool ram;                  // held whole
  bool forced;               // held whole for the streaming budget (core/pool/stream_budget.h), counted there
  int entry;                 // the list entry it holds (-1 after a rescan: kept only while used)
  char path[kPath];
  Desc d;
  Smp* smp;
  uint16_t* seq;             // round robin counters, one a region
  uint64_t used;             // bytes (its heads, as the cache holds them, and its loop parts)
  uint64_t need_ram;         // every sample whole, as int16 in its layout (a stereo sample: 2x)
  uint32_t used_ms;
};

// The INST list: the project's INSTRUMENTS pool (core/lib/pools.h), entry = code (bank * 128 + row: INST's 14-bit
// word); an instrument row its .sfz (resolved by its hash) with its member samples (resolved one by one: a member found
// elsewhere plays from there, the .sfz unchanged); OFF, empty and missing rows are silent.
using Entry = srclib::Entry;

// ---- notes
struct Start {
  const int16_t* head;
  uint32_t head_n;
  uint8_t ch;                // 1, 2 (head, res2 and ring interleaved L R)
  float pl, pr;              // a stereo voice's (OUT PRE / POST) gains: the region's pan / width / position
  const int16_t* res2;
  uint32_t ws, we;
  int16_t* ring;             // nullptr: nothing streams (the sample is in RAM up to where this note reads)
  uint32_t w0;               // the ring's first path frame for this note
  uint32_t end;              // exclusive
  double pos;
  float inc, gain;
  float wd;                  // the region's width (1 as recorded, 0 mono, -1 swapped)
  uint32_t loop_s, loop_e;   // exclusive end; loop_e > loop_s + 1 when looping
  uint8_t mode;              // 0 none, 1 continuous, 2 sustain (the loop while held)
  bool one_shot;             // a note-off does not release it
  uint32_t att, hold;        // samples
  float dcoef, sus, rcoef;
  uint32_t rel_gen, kill_gen;
};
struct Note {
  // control -> render
  volatile uint32_t seq;     // odd while the control core writes st
  Start st;
  volatile uint32_t rel_n, kill_n;
  volatile uint32_t wgen, wp;   // the stream: wp path frames of start wgen are in the ring
  // render -> control
  volatile uint32_t done;    // the seq of the start that finished (or was killed)
  volatile uint32_t rgen, rd;   // the start rgen reads at file frame rd (the lowest it may still need)
  // the control core's own
  bool held;                 // started and not released
  int8_t slot;
  uint8_t key, vel;
  bool rtrig;                // a release region's note
  bool live;                 // a live pad's note (live_on): its channel's note-off releases it, not a trig
  int8_t ch;
  uint8_t mnote;
  uint32_t order;
  // the render core's own
  uint32_t r_seq;
  bool r_on, r_rel, r_sounded;
  Start r;
  double pos;
  uint8_t stage;             // 0 attack 1 hold 2 decay 3 sustain 4 release
  uint32_t n;                // samples in the stage
  float lvl, ug;             // the envelope; the underrun fade (1 sounding)
};
// A note's stream, the control core's
struct Wr {
  bool on;                   // this start streams
  uint32_t gen;              // the start (its seq)
  int slot, smp;
  uint32_t head_n, ws, we, path_end, wp;
  float fps;                 // frames a second it reads at
  File f;
  bool open;
  int8_t rq;                 // its card request in flight (rqs, cardq.h), -1 none
  bool ending;               // its note ended while a request ran: closed when that completes
};
// The streams' card requests (cardq.h): one a stream at most, kInflight at once; their read buffers
constexpr int kInflight = 8;
struct Rq {
  cardq::Req r;
  int wi;                    // the stream (vi * kPerVoice + k), -1 free
  uint32_t wp, fr, fb;       // a read: the ring position it fills, the frames asked, bytes a frame
  uint8_t buf[kStreamChunk];
};

// path frame <-> file frame: the ring holds the frames after the head, the preloaded loop part left out
inline uint32_t path_of(uint32_t f, uint32_t head_n, uint32_t ws, uint32_t we) {
  if (f < head_n) return 0;
  if (ws < we && f >= ws) return f >= we ? f - head_n - (we - ws) : ws - head_n;
  return f - head_n;
}
inline uint32_t file_of(uint32_t p, uint32_t head_n, uint32_t ws, uint32_t we) {
  uint32_t f = head_n + p;
  return ws < we && f >= ws ? f + (we - ws) : f;
}

Slot slots[kSlots];
Entry* list = nullptr;
Entry* lbuf[2] = {nullptr, nullptr};
int nlist = 0;              // srclib::kEntries once composed
uint32_t composed_sig = 0;
volatile uint32_t g_list_gen = 1;
bool arena = false;          // the pool RAM is there (core/pool/pcache.h)
int16_t* rings = nullptr;    // kVoices * kPerVoice * kRing
uint64_t slot_cap = 0;
Note notes[kVoices][kPerVoice];
Wr* wrs = nullptr;           // [kVoices * kPerVoice]
Rq* rqs = nullptr;           // [kInflight]
uint32_t order_n = 0;
Stats st;
volatile uint32_t g_underruns = 0;
bool card_ok = false;
uint32_t head_ms = kHeadMs, ahead_ms = 300;
int max_streams = 32;

int8_t vslot[kVoices];       // the slot voice vi plays (-1 none)
int16_t want[kVoices];       // the list entry it wants (-1 none)
float vp[kVoices][16];       // its last P1..P16
uint8_t key_vel[kVoices][128];   // the velocity of each key's last note-on (release regions)
bool force[kVoices];
struct Pend {
  bool on;
  uint8_t note, vel;
  uint32_t ms;
};
Pend pend[kVoices];
// Live notes (poly's binding, the Sidecar sequencer's chords: live_on) that came before the voice's instrument was
// ready: started when it is, if within kLateMs, as a trig's (pend); their note-off before that drops them.
struct LivePend {
  bool on;
  uint8_t key, vel, mnote;
  int8_t ch;
  uint32_t ms;
};
LivePend lpend[kVoices][kPerVoice];
uint8_t failed[srclib::kEntries];   // entries that failed to load (until a rescan)
struct LiveTag {
  bool on;
  int8_t ch;
  uint8_t mnote;
};
LiveTag cur_live = {false, 0, 0};   // the live note start_region starts now (live_on)
uint32_t rnd = 22222;

// ---- the job (one at a time)
enum Phase : uint8_t { kIdle, kSfzRead, kHeaders, kSampleOpen, kSampleRead };
struct Job {
  Phase phase;
  // load
  int slot, entry;
  File f;
  char* text;
  uint32_t text_len, text_size;
  int smp;                  // the sample being read
  uint32_t got, want_n, t0;
  int16_t* dst;
  uint8_t buf[kStreamChunk];
  int16_t cv[kStreamChunk];   // (8-bit mono: a sample a byte)
};
Job* job = nullptr;

CORE_COLD const char* slot_name(int s) {
  const char* p = strrchr(slots[s].path, '/');
  return p ? p + 1 : slots[s].path;
}

CORE_COLD bool note_free(const Note& n) { return ld_acq(&n.done) == ld_rlx(&n.seq); }
CORE_COLD bool slot_playing(int s) {
  for (int v = 0; v < kVoices; ++v)
    for (int k = 0; k < kPerVoice; ++k)
      if (notes[v][k].slot == s && !note_free(notes[v][k])) return true;
  return false;
}
CORE_COLD bool slot_selected(int s) {
  for (int v = 0; v < kVoices; ++v)
    if (vslot[v] == s || (want[v] >= 0 && slots[s].entry == want[v])) return true;
  return false;
}
CORE_COLD bool same_file(int s, int entry) {   // a slot holds entry's file (the same resolved .sfz)
  return slots[s].entry == entry || (entry >= 0 && entry < nlist && list[entry].path[0] && !strcasecmp(slots[s].path, list[entry].path));
}
CORE_COLD int count_active() {
  int a = 0;
  for (int v = 0; v < kVoices; ++v)
    for (int k = 0; k < kPerVoice; ++k) a += !note_free(notes[v][k]);
  return a;
}
CORE_COLD int resident(int entry) {
  for (int s = 0; s < kSlots; ++s)
    if (slots[s].state == kReady && same_file(s, entry)) return s;
  return -1;
}
CORE_COLD Wr& wr_of(int vi, int k) { return wrs[vi * kPerVoice + k]; }
// A stream whose note is done: its file closed (a request still running for it: when that completes, stream_step).
CORE_COLD void wr_end(Wr& w) {
  w.on = false;
  if (w.rq >= 0) {
    if (!cardq::cancel(&rqs[w.rq].r)) {
      w.ending = true;
      return;
    }
    rqs[w.rq].wi = -1;
    w.rq = -1;
  }
  if (w.open) w.f.close();
  w.open = false;
  w.ending = false;
}
CORE_COLD void complete(Rq& q);
// The stream's Wr free for a new start: a running request waited for (one card operation; rare - pick() takes a free
// note first).
CORE_COLD void wr_quiesce(Wr& w) {
  wr_end(w);
  if (w.ending && w.rq >= 0) {
    Rq& q = rqs[w.rq];
    cardq::wait(&q.r);
    complete(q);
  }
}
CORE_COLD bool streaming(int vi, int k) {
  Wr& w = wr_of(vi, k);
  if (w.on && note_free(notes[vi][k])) wr_end(w);
  return w.on;
}
CORE_COLD int count_streams() {
  int a = 0;
  for (int v = 0; v < kVoices; ++v)
    for (int k = 0; k < kPerVoice; ++k) a += streaming(v, k);
  return a;
}
CORE_COLD void kill(int vi, int k) {
  Note& n = notes[vi][k];
  n.held = false;
  n.live = false;
  st_rel(&n.kill_n, n.kill_n + 1);
}
// The oldest released streaming note (else the oldest streaming one, when any_held), not (xv, xk); false: none.
CORE_COLD bool steal_stream(bool any_held, int xv, int xk) {
  int bv = -1, bk = -1;
  for (int pass = 0; pass < (any_held ? 2 : 1) && bv < 0; ++pass)
    for (int v = 0; v < kVoices; ++v)
      for (int k = 0; k < kPerVoice; ++k) {
        if ((v == xv && k == xk) || !streaming(v, k) || (pass == 0 && notes[v][k].held)) continue;
        if (bv < 0 || (int32_t)(notes[v][k].order - notes[bv][bk].order) < 0) bv = v, bk = k;
      }
  if (bv < 0) return false;
  kill(bv, bk);
  wr_end(wr_of(bv, bk));
  ++st.bw_steals;
  return true;
}

// ---- starting notes
CORE_COLD float tscale(float v) { return exp2f((v - 64.0f) / 16.0f); }
CORE_COLD float coef(float secs) {   // per sample, -60 dB after secs
  float n = secs * 48000.0f;
  return n < 48 ? expf(-6.9078f / 48.0f) : expf(-6.9078f / n);
}

// The control core writes a start into note n.
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

CORE_COLD void start_region(int vi, int s, const Region& r, int key, int vel, float cg, bool rtrig) {
  const Smp& m = slots[s].smp[r.sample];
  if (!m.ok) return;
  const float* p = vp[vi];
  if (!rtrig && count_active() >= kTotal) {   // the cap in all: the voice's own oldest goes
    bool any = false;
    for (int k = 0; k < kPerVoice; ++k) any = any || !note_free(notes[vi][k]);
    if (!any) return;
  }
  int k = pick(vi);
  Note& n = notes[vi][k];
  Wr& w = wr_of(vi, k);
  wr_quiesce(w);
  Start x;
  memset(&x, 0, sizeof x);
  uint32_t e = r.end == kUnset || r.end + 1 > m.frames ? m.frames : r.end + 1;
  uint32_t off = r.offset < e ? r.offset : e - 1;
  float strt = p[10] < 0 ? 0 : p[10] > 127 ? 127 : p[10];
  double pos = off + (double)(e - off) * (strt / 128.0);
  bool whole = m.head_n >= m.frames;
  if (!whole && strt > 0 && pos > (double)m.head_n - 4) pos = off < m.head_n - 4 ? m.head_n - 4 : off;   // STRT inside the head
  x.pos = pos;
  x.head = m.head, x.head_n = m.head_n, x.res2 = m.res2, x.ws = m.ws, x.we = m.we;
  x.ch = m.ch == 2 ? 2 : 1;
  {   // the region's pan (-100..100), on a stereo voice: a stereo sample's balance, a mono one's equal-power pan
      // (centre = unity, as out.h's PAN); width / position (sfz v2/ARIA): a stereo sample narrowed toward mono
      // and placed
    float pn = r.pan / 100.0f;
    if (x.ch == 2) pn += r.position / 100.0f;
    pn = pn < -1 ? -1 : pn > 1 ? 1 : pn;
    if (x.ch == 2) {
      x.pl = pn > 0 ? 1.0f - pn : 1.0f, x.pr = pn < 0 ? 1.0f + pn : 1.0f;
    } else {
      float th = (pn + 1.0f) * 0.78539816f;
      x.pl = 1.41421356f * cosf(th), x.pr = 1.41421356f * sinf(th);
      if (r.pan == 0) x.pl = x.pr = 1.0f;
    }
    x.wd = r.width / 100.0f;
  }
  x.end = e;
  uint32_t ls = r.loop_start != kUnset ? r.loop_start : m.has_loop ? m.loop_s : off;
  uint32_t le = r.loop_end != kUnset ? r.loop_end + 1 : m.has_loop ? m.loop_e + 1 : e;
  if (le > e) le = e;
  bool can_loop = le > ls + 1;
  int lz = eng_zone(p[11], 3);   // AUTO OFF ON
  uint8_t mode = r.loop_mode;
  if (mode == kLoopAuto) mode = (m.has_loop || r.loop_start != kUnset || r.loop_end != kUnset) ? kLoopContinuous : kNoLoop;
  x.one_shot = mode == kOneShot;
  if (lz == 1) mode = kNoLoop;
  else if (lz == 2 && mode != kLoopSustain) mode = kLoopContinuous;
  x.mode = !can_loop ? 0 : mode == kLoopContinuous ? 1 : mode == kLoopSustain ? 2 : 0;
  bool loop_in_ram = le <= m.head_n || (m.res2 && le <= m.we && (ls >= m.head_n ? ls >= m.ws : m.ws == m.head_n));
  if (x.mode && !whole && !loop_in_ram) {
    x.mode = 0;   // its loop is not preloaded: streamed loops are not played (docs/MULTISAMPLE.md)
    ++st.loops_dropped;
  }
  x.loop_s = ls, x.loop_e = le;
  float semis = (key - r.keycenter) * r.pitch_keytrack / 100.0f + r.transpose + r.tune / 100.0f + (p[8] - 64) +
                (p[9] - 64) / 64.0f;
  x.inc = m.rate / 48000.0f * exp2f(semis / 12.0f);
  float vt = r.amp_veltrack / 100.0f * (p[1] < 0 ? 0 : p[1] > 127 ? 1 : p[1] / 127.0f);
  if (vt < 0) vt = 0;
  float v = (vel < 1 ? 1 : vel) / 127.0f;
  float vg = 1 - vt + vt * v * v;
  x.gain = powf(10.0f, r.volume / 20.0f) * r.amplitude / 100.0f * vg * (p[2] < 0 ? 0 : p[2] / 100.0f) * cg;
  float att = r.attack * tscale(p[4]), hold = r.hold, dec = r.decay * tscale(p[5]), rel = r.release * tscale(p[7]);
  float sus = r.sustain / 100.0f, sv = p[6];
  sus = sv <= 64 ? sus * sv / 64.0f : sus + (1 - sus) * (sv - 64) / 63.0f;
  x.att = (uint32_t)(att > 30 ? 30 * 48000 : att * 48000);
  x.hold = (uint32_t)(hold > 30 ? 30 * 48000 : hold * 48000);
  x.dcoef = dec <= 0 ? 0 : coef(dec);
  x.sus = sus < 0 ? 0 : sus > 1 ? 1 : sus;
  x.rcoef = coef(rel);
  x.rel_gen = n.rel_n;
  x.kill_gen = n.kill_n;
  // the stream: the frames after the head this note can reach (a continuous loop's notes stop at its end)
  uint32_t stream_end = x.mode == 1 ? le : e;
  bool streams = !whole && stream_end > m.head_n;
  if (streams && count_streams() >= max_streams) steal_stream(true, vi, k);
  if (streams) {
    x.ring = rings + (size_t)(vi * kPerVoice + k) * kRing;
    uint32_t f0 = (uint32_t)pos > m.head_n + 2 ? (uint32_t)pos - 2 : m.head_n;
    x.w0 = path_of(f0, m.head_n, m.ws, m.we);
  }
  n.held = !x.one_shot && !rtrig;
  n.slot = (int8_t)s;
  n.key = (uint8_t)key, n.vel = (uint8_t)vel;
  n.rtrig = rtrig;
  n.live = cur_live.on && !rtrig, n.ch = cur_live.ch, n.mnote = cur_live.mnote;
  n.order = ++order_n;
  st_rel(&n.wgen, 0);   // the old start's stream is over
  post(n, x);
  if (streams) {
    w.on = true, w.gen = n.seq, w.slot = s, w.smp = r.sample;
    w.head_n = m.head_n, w.ws = m.ws, w.we = m.we;
    w.path_end = path_of(stream_end, m.head_n, m.ws, m.we);
    w.wp = x.w0;
    w.fps = x.inc * 48000.0f;
    st_rlx(&n.wp, w.wp);
    st_rel(&n.wgen, w.gen);
    int ns = count_streams();
    st.streams = ns;
    if (ns > st.streams_peak) st.streams_peak = ns;
  }
  ++st.notes;
  int a = count_active();
  st.active = a;
  if (a > st.peak) st.peak = a;
}

CORE_COLD float frand() {
  rnd = rnd * 1664525u + 1013904223u;
  return (rnd >> 8) / 16777216.0f;
}

// The regions of slot s for key / vel / trigger; started (layers up to kLayers). The count.
CORE_COLD int play_key(int vi, int s, int key, int vel, uint8_t trig, float cg) {
  Slot& sl = slots[s];
  float r01 = frand();
  int started = 0;
  for (int i = 0; i < sl.d.nregions && started < kLayers; ++i) {
    const Region& r = sl.d.regions[i];
    if (r.trigger != trig || key < r.lokey || key > r.hikey || vel < r.lovel || vel > r.hivel) continue;
    uint16_t c = ++sl.seq[i];
    if (r.seq_length > 1 && (c - 1) % r.seq_length != (uint32_t)(r.seq_position - 1) % r.seq_length) continue;
    if (r01 < r.lorand || r01 >= r.hirand) continue;
    start_region(vi, s, r, key, vel, cg, trig == kTrigRelease);
    ++started;
  }
  return started;
}

CORE_COLD void release_voice(int vi, bool rtrig) {
  int s = vslot[vi];
  uint8_t keys[kPerVoice];
  int nk = 0;
  for (int k = 0; k < kPerVoice; ++k) {
    Note& n = notes[vi][k];
    if (n.live) continue;   // a live pad's: its own note-off releases it
    if (!n.held || note_free(n)) {
      n.held = false;
      continue;
    }
    n.held = false;
    st_rel(&n.rel_n, n.rel_n + 1);
    bool seen = false;
    for (int j = 0; j < nk; ++j) seen = seen || keys[j] == n.key;
    if (!seen) keys[nk++] = n.key;
  }
  if (!rtrig || s < 0 || slots[s].state != kReady) return;
  for (int j = 0; j < nk; ++j) play_key(vi, s, keys[j], key_vel[vi][keys[j]], kTrigRelease, 1.0f);
}

CORE_COLD void trig(int vi, int note, int vel) {
  int s = vslot[vi];
  if (s < 0 || slots[s].state != kReady) return;
  slots[s].used_ms = now_ms();
  release_voice(vi, false);   // the earlier trig notes: their tails go on (no release regions for a retrig)
  poly::Chord c = poly::chord_of(vp[vi]);
  uint8_t ks[poly::kMaxNotes];
  int nk = poly::expand(note, c, ks);
  float cg = 1.0f;   // (a chord's 1/sqrt(n): render_add's headroom, as poly FM's)
  int started = 0;
  for (int i = 0; i < nk; ++i) {
    key_vel[vi][ks[i]] = (uint8_t)vel;
    started += play_key(vi, s, ks[i], vel, kTrigAttack, cg);
  }
  ++st.trigs;
  v[vi].live = true;
  char b[8];
  log(CORE_STR("trig voice %d MULTISAMPLE %s note %d %s%s%s: %d notes, %d keys (%d now, peak %d, %d streaming)"), vi,
      slot_name(s), note, c.chd ? poly::kChordNames[c.chd] : "", c.chd ? " VOI " : "",
      c.chd ? poly::value_text(3, c.voi, b, sizeof b) : "", started, nk, st.active, st.peak, st.streams);
}

// ---- streaming (the control core, between blocks)
// The note's audio ahead of where it reads, in frames (its head's rest counts).
CORE_COLD uint32_t reader_path(int vi, int k, const Wr& w, uint32_t* lead) {
  Note& n = notes[vi][k];
  uint32_t g1 = ld_acq(&n.rgen), rd = ld_acq(&n.rd), g2 = ld_acq(&n.rgen);
  if (g1 != w.gen || g2 != g1) rd = (uint32_t)n.st.pos;   // not started yet: where it will start
  *lead = rd < w.head_n ? w.head_n - rd : 0;
  uint32_t p = path_of(rd, w.head_n, w.ws, w.we);
  return p < n.st.w0 ? n.st.w0 : p;
}
// The note's audio ahead of where it reads, in frames (its head's rest counts).
CORE_COLD uint32_t ahead_frames(int vi, int k, const Wr& w) {
  uint32_t lead, pr = reader_path(vi, k, w, &lead);
  return lead + (w.wp > pr ? w.wp - pr : 0);
}

// A stream request done (cardq.h): an open's file, a read's frames into the note's ring.
CORE_COLD void complete(Rq& q) {
  Wr& w = wrs[q.wi];
  int vi = q.wi / kPerVoice, k = q.wi % kPerVoice;
  w.rq = -1;
  q.wi = -1;
  q.r.state = cardq::kFree;
  if (w.ending) {   // its note ended meanwhile
    if (w.f.isOpen()) w.f.close();
    w.open = w.ending = false;
    return;
  }
  if (q.r.kind == cardq::kOpenReq) {
    w.open = q.r.got == 1;
    if (!w.open) {
      log(CORE_STR("msamp: cannot stream %s"), q.r.path);
      w.path_end = w.wp;   // plays its head only
    }
    return;
  }
  const Smp& m = slots[w.slot].smp[w.smp];
  int got = q.r.got;
  uint32_t whole = got > 0 ? (uint32_t)got / q.fb : 0;
  int16_t* ring = rings + (size_t)(vi * kPerVoice + k) * kRing;
  if (m.ch == 2) {
    convert(m.a, q.buf, whole, job->cv, 2);
    for (uint32_t i = 0; i < whole; ++i) {
      uint32_t p = 2 * ((w.wp + i) & (kRing / 2 - 1));
      ring[p] = job->cv[2 * i], ring[p + 1] = job->cv[2 * i + 1];
    }
  } else {
    convert(m.a, q.buf, whole, job->cv);
    for (uint32_t i = 0; i < whole; ++i) ring[(w.wp + i) & (kRing - 1)] = job->cv[i];
  }
  w.wp += whole;
  if (whole < q.fr) w.path_end = w.wp;   // the file ended early
  st_rel(&notes[vi][k].wp, w.wp);
  ++st.stream_reads;
  st.stream_bytes += (uint64_t)whole * q.fb;
}

// The streams' card work, between blocks: the requests that completed taken in, then one request (an open, or a
// read of up to kStreamChunk) for the neediest stream below the read-ahead that has none in flight. With a card core
// (cardq::worker(): the Pi's core 0) the request runs there and its result comes in on a later pass; without, it runs
// in post() and is taken in at once (one card operation a call, as before the queue). True when it did something.
CORE_COLD bool stream_step() {
  bool did = false;
  uint32_t now_u = now_us();
  for (int i = 0; i < kInflight; ++i) {
    if (rqs[i].wi < 0) continue;
    if (cardq::idle(&rqs[i].r) ||   // done; or waiting too long for a held-up card core: run here
        (now_u - rqs[i].r.posted_us > cardq::kTakeBackUs && cardq::take_back(&rqs[i].r)))
      complete(rqs[i]), did = true;
  }
  int bv = -1, bk = -1;
  float best = 1e9f, all = 1e9f;
  for (int v = 0; v < kVoices; ++v)
    for (int k = 0; k < kPerVoice; ++k) {
      if (!streaming(v, k)) continue;
      Wr& w = wr_of(v, k);
      if (w.wp >= w.path_end) continue;   // all of it in the ring
      float ms = ahead_frames(v, k, w) * 1000.0f / (w.fps > 1 ? w.fps : 1);
      if (ms < all) all = ms;
      if (w.rq >= 0) continue;   // its request is in flight
      if (ms < best) best = ms, bv = v, bk = k;
    }
  // the recorder and uploads hold off while a stream is below half its read-ahead (cardq::busy)
  cardq::set_need(all < 1e8f ? (uint32_t)all : ~0u, ahead_ms / 2);
  if (bv < 0 || best >= (float)ahead_ms) return did;
  int qi = -1;
  for (int i = 0; i < kInflight && qi < 0; ++i)
    if (rqs[i].wi < 0) qi = i;
  if (qi < 0) return did;
  Wr& w = wr_of(bv, bk);
  // bandwidth: a note near dry takes a released tail's reads, and once nearly out, the oldest held one's (at most one a
  // kCriticalMs: a card stall does not empty the voices at once)
  static uint32_t last_steal = 0;
  uint32_t now = now_ms();
  if (best < (float)kCriticalMs && now - last_steal >= kCriticalMs &&
      (steal_stream(false, bv, bk) || (best < kCriticalMs / 3.0f && steal_stream(true, bv, bk))))
    last_steal = now;
  const Smp& m = slots[w.slot].smp[w.smp];
  Rq& q = rqs[qi];
  q.r.f = &w.f;
  if (!w.open) {
    q.r.kind = cardq::kOpenReq;
    strncpy(q.r.path, slots[w.slot].d.samples[w.smp], sizeof q.r.path - 1);
    q.r.path[sizeof q.r.path - 1] = 0;
  } else {
    // how far it may write: the ring's room past the reader, to the path's end, to the loop part's start
    uint32_t lead, rp = reader_path(bv, bk, w, &lead);
    const uint32_t cap = kRing / (m.ch == 2 ? 2 : 1);   // the ring's frames (a stereo sample: half)
    uint32_t room = rp + cap - 64 > w.wp ? rp + cap - 64 - w.wp : 0;
    if (!room) return did;
    uint32_t f = file_of(w.wp, w.head_n, w.ws, w.we);
    uint32_t fb = m.a.ch * (m.a.bits / 8);
    uint32_t fr = kStreamChunk / fb;
    if (fr > room) fr = room;
    if (fr > w.path_end - w.wp) fr = w.path_end - w.wp;
    if (w.ws < w.we && f < w.ws && f + fr > w.ws) fr = w.ws - f;
    if (f + fr > m.frames) fr = f < m.frames ? m.frames - f : 0;
    if (!fr) {
      w.path_end = w.wp;
      return did;
    }
    q.r.kind = cardq::kReadReq;
    q.r.off = (uint64_t)m.a.data_off + (uint64_t)f * fb;
    q.r.buf = q.buf;
    q.r.len = fr * fb;
    q.wp = w.wp, q.fr = fr, q.fb = fb;
  }
  if (!cardq::post(&q.r)) return did;
  q.wi = bv * kPerVoice + bk;
  w.rq = (int8_t)qi;
  if (!cardq::worker()) complete(q);   // it ran in post()
  return true;
}

// ---- loading
// The pool entry's member for the .sfz's sample i: its i-th when the file names agree (the members are the .sfz's
// distinct samples in order), else the member of that file name (an .sfz edited since it went in, a sample left out);
// null none. (A member found elsewhere by its hash keeps its file name.)
CORE_COLD const char* base_name(const char* p) {
  const char* b = strrchr(p, '/');
  return b ? b + 1 : p;
}
CORE_COLD const pools::Member* member_of(const pools::Member* mem, int nm, int i, const char* path) {
  if (!mem) return nullptr;
  const char* b = base_name(path);
  if (i < nm && !strcasecmp(base_name(mem[i].ref.hint), b)) return &mem[i];
  for (int k = 0; k < nm; ++k)
    if (!strcasecmp(base_name(mem[k].ref.hint), b)) return &mem[k];
  return nullptr;
}
// a slot's memory given back: its heads unpinned (they stay in the cache until evicted), its loop parts released
CORE_COLD void slot_release(int s) {
  Slot& sl = slots[s];
  for (int i = 0; sl.smp && i < sl.d.nsamples && i < kMaxSamples; ++i) {
    Smp& m = sl.smp[i];
    if (m.blk >= 0) pcache::unpin(m.blk);
    if (m.priv >= 0) pcache::release(m.priv);
    m.blk = m.priv = m.pe = -1;
  }
  sl.d.nsamples = 0;
  sl.state = kFree;
  if (sl.forced) sl.forced = false, sbudget::forced(-1);
}
CORE_COLD void job_fail(const char* why) {
  Slot& sl = slots[job->slot];
  log(CORE_STR("msamp: %s: %s"), sl.path, why);
  if (job->entry >= 0 && job->entry < srclib::kEntries) failed[job->entry] = 1;
  slot_release(job->slot);
  sl.entry = -1;
  job->f.close();
  job->phase = kIdle;
  ++st.load_fails;
}

CORE_COLD int free_slot() {
  for (int s = 0; s < kSlots; ++s)
    if (slots[s].state == kFree) return s;
  int b = -1;
  for (int s = 0; s < kSlots; ++s) {
    if (slots[s].state != kReady || slot_selected(s) || slot_playing(s)) continue;
    if (b < 0 || (int32_t)(slots[s].used_ms - slots[b].used_ms) < 0) b = s;
  }
  return b;
}

CORE_COLD bool start_load() {
  for (int vi = 0; vi < kVoices; ++vi) {
    int e = want[vi];
    if (e < 0 || e >= nlist || !list[e].path[0] || failed[e] || resident(e) >= 0) continue;
    int s = free_slot();
    if (s < 0) {
      if (failed[e] != 2) log(CORE_STR("msamp: no free instrument slot for %s (%d in use)"), list[e].name, kSlots);
      failed[e] = 2;
      return false;
    }
    Slot& sl = slots[s];
    if (sl.state != kFree) slot_release(s);
    sl.state = kLoading;
    sl.entry = e;
    strcpy(sl.path, list[e].path);
    sl.used = sl.need_ram = 0;
    sl.ram = false;
    job->slot = s, job->entry = e;
    job->text_len = 0;
    job->t0 = now_ms();
    if (!job->f.open(sl.path, kRead)) {
      job_fail("cannot open");
      return true;
    }
    uint64_t size = job->f.fileSize();
    if (size > kSfzMax) {
      job_fail("the .sfz is over 256 KB");
      return true;
    }
    job->text_size = (uint32_t)size;
    job->phase = kSfzRead;
    return true;
  }
  return false;
}

CORE_COLD void loaded() {
  Slot& sl = slots[job->slot];
  int missing = 0;
  for (int i = 0; i < sl.d.nsamples; ++i) missing += !sl.smp[i].ok;
  sl.state = kReady;
  sl.used_ms = now_ms();
  job->phase = kIdle;
  ++st.loads;
  log(CORE_STR("msamp: loaded %s (%s): %d regions, %d samples (%d not loaded), %lu KB in RAM of %lu KB (whole: %lu KB), "
               "%lu ms; %d opcodes ignored%s%s%s, %d regions skipped, %d malformed"),
      sl.path, sl.ram ? "RAM" : "STREAM", sl.d.nregions, sl.d.nsamples, missing, (unsigned long)(sl.used >> 10),
      (unsigned long)(pcache::stats().budget >> 10), (unsigned long)(sl.need_ram >> 10), (unsigned long)(now_ms() - job->t0),
      sl.d.ignored, sl.d.ignored ? " (first: " : "", sl.d.first_ignored, sl.d.ignored ? ")" : "", sl.d.skipped,
      sl.d.malformed);
  uint32_t ms = now_ms();
  for (int vi = 0; vi < kVoices; ++vi) {
    if (want[vi] < 0 || resident(want[vi]) != job->slot) continue;
    vslot[vi] = (int8_t)job->slot;
    if (pend[vi].on) {
      pend[vi].on = false;
      if (ms - pend[vi].ms <= kLateMs) trig(vi, pend[vi].note, pend[vi].vel);
      else ++st.late;
    }
    for (int k = 0; k < kPerVoice; ++k) {   // a sequenced chord's (or the pads') notes that came first
      LivePend& l = lpend[vi][k];
      if (!l.on) continue;
      l.on = false;
      if (ms - l.ms <= kLateMs) live_on(vi, l.key, l.vel, l.ch, l.mnote);
      else ++st.late;
    }
  }
}

// After the headers: RAM or STREAM, and each sample's parts placed: its head in the pool RAM's shared cache (found
// there already - a whole copy plays from RAM, nothing streams - or filled here), its loop part in a private block.
CORE_COLD void plan() {
  Slot& sl = slots[job->slot];
  uint64_t budget = pcache::stats().budget;
  bool want_ram = sl.d.load == 1 || (sl.d.load == 0 && sl.need_ram <= kAutoRam);
  // docs/STEREO.md: a row (or the list) marked "always in RAM"; a stereo instrument the volume cannot stream
  // sbudget::kWantStreams voices of (its widest format, the stream budget of its samples' volume) is held whole
  // (over the budget wins over sidecar_load=stream too: streams the volume cannot carry would run dry; the decision is
  // the load's: a change acts at the instrument's next load)
  bool always = pools::always_ram(pools::kInstr, job->entry), over = false;
  for (int i = 0; i < sl.d.nsamples && !want_ram && !always && !over; ++i) {
    const Smp& m = sl.smp[i];
    if (m.frames && m.a.ch >= 2 && !pcache::mono() && !sbudget::fits(sl.d.samples[i], m.a.rate, 2, m.a.bits))
      over = true;
  }
  if ((always || over) && !want_ram && sl.need_ram <= budget / 2) {
    want_ram = true;
    if (over && !always) {
      sl.forced = true;
      sbudget::forced(1);
    }
    log(CORE_STR("msamp: %s: held in RAM (%s)"), slot_name(job->slot),
        always ? "always in RAM" : "stereo, over the streaming budget of its volume");
  } else if (over) {
    log(CORE_STR("msamp: %s: stereo, over the streaming budget, and too big for the pool RAM: streamed anyway"),
        slot_name(job->slot));
  }
  sl.ram = want_ram && sl.need_ram <= budget / 2;
  if (want_ram && !sl.ram)
    log(CORE_STR("msamp: %s: RAM needs %lu KB, more than half the pool RAM (%lu KB): streamed instead"), slot_name(job->slot),
        (unsigned long)(sl.need_ram >> 10), (unsigned long)(budget >> 10));
  int nm = 0;
  const pools::Member* mem = pools::members(pools::kInstr, job->entry, &nm);
  for (int i = 0; i < sl.d.nsamples; ++i) {
    Smp& m = sl.smp[i];
    m.pe = m.blk = m.priv = -1, m.cached = false;
    if (!m.frames) continue;
    // the loop: the file's, else the first region's on this sample
    if (!m.has_loop)
      for (int r = 0; r < sl.d.nregions; ++r) {
        const Region& g = sl.d.regions[r];
        if (g.sample != i || g.loop_end == kUnset) continue;
        m.loop_s = g.loop_start == kUnset ? g.offset : g.loop_start, m.loop_e = g.loop_end;
        m.has_loop = m.loop_e > m.loop_s && m.loop_e < m.frames;
        break;
      }
    m.ch = (uint8_t)ram_ch(m.a);
    uint32_t hn = (uint32_t)(m.rate * head_ms / 1000.0f);
    if (sl.ram || hn + hn / 4 >= m.frames) hn = m.frames;
    if (hn > pcache::kMaxFrames) hn = pcache::kMaxFrames;
    lib::Ref ref;
    const pools::Member* mi = member_of(mem, nm, i, sl.d.samples[i]);
    if (mi) ref = mi->ref;
    else lib::ref_for_path(sl.d.samples[i], &ref);
    // the cache first: a whole copy (a GRANULAR source, a DSP SAMPLE slot, another instrument) or a long enough head
    int16_t* dst = nullptr;
    int have = -1;
    int pe = pcache::fill_begin(ref, hn, m.frames, m.rate, &dst, &have, m.ch);
    pcache::Info in;
    if (pe < 0 && have >= 0 && pcache::get(have, &in)) {
      m.pe = have, m.blk = in.blk, m.cached = true;
      m.ch = in.ch;   // (the cache's layout: the loop part and the ring follow it)
      pcache::pin(in.blk);
      m.head = in.pcm;
      hn = in.whole ? (in.frames < m.frames ? in.frames : m.frames) : in.frames < hn ? in.frames : hn;
      if (in.whole && in.frames >= m.frames) hn = m.frames;
    } else if (pe >= 0) {
      m.pe = pe;
      m.head = dst;
    } else {
      log(CORE_STR("msamp: %s: %s left out (no room in the pool RAM for %lu KB)"), slot_name(job->slot), sl.d.samples[i],
          (unsigned long)(((uint64_t)hn * 2 * m.ch) >> 10));
      ++st.left_out;
      m.frames = 0;
      continue;
    }
    m.head_n = hn;
    m.ws = m.we = 0;
    m.res2 = nullptr;
    if (hn < m.frames && m.has_loop && m.loop_e + 1 > hn) {
      uint32_t ws = m.loop_s > hn ? m.loop_s : hn, we = m.loop_e + 1;
      void* p2 = nullptr;
      if (we - ws <= kLoopMax && (m.priv = pcache::alloc_private((uint64_t)(we - ws) * 2 * m.ch, &p2)) >= 0)
        m.ws = ws, m.we = we, m.res2 = (const int16_t*)p2;
    }
    sl.used += ((uint64_t)m.head_n * 2 * m.ch + 63 & ~(uint64_t)63) + (uint64_t)(m.we - m.ws) * 2 * m.ch;
  }
}

CORE_COLD void next_sample() {
  Slot& sl = slots[job->slot];
  job->f.close();
  ++job->smp;
  while (job->smp < sl.d.nsamples && (!sl.smp[job->smp].frames || (sl.smp[job->smp].cached && !sl.smp[job->smp].res2))) {
    if (sl.smp[job->smp].cached) sl.smp[job->smp].ok = true;   // its head from the cache, nothing to read
    ++job->smp;
  }
  if (job->smp >= sl.d.nsamples) {
    loaded();
    return;
  }
  sl.smp[job->smp].part = sl.smp[job->smp].cached ? 1 : 0;   // (a cached head: only its loop part)
  job->phase = kSampleOpen;
}

// Every sample's header, one a step: the RAM size, the formats.
CORE_COLD void step_header() {
  Slot& sl = slots[job->slot];
  Smp& m = sl.smp[job->smp];
  memset(&m, 0, sizeof m);
  m.pe = m.blk = m.priv = -1;
  const char* path = sl.d.samples[job->smp];
  File f;
  if (!f.open(path, kRead) || !audio_header(f, &m.a)) {
    log(CORE_STR("msamp: %s: cannot read %s"), slot_name(job->slot), path);
  } else {
    m.frames = m.a.frames;
    m.rate = m.a.rate;
    m.has_loop = m.a.has_loop && m.a.loop_e < m.a.frames;
    m.loop_s = m.a.loop_s, m.loop_e = m.a.loop_e;
    sl.need_ram += ((uint64_t)m.frames * 2 * ram_ch(m.a) + 63) & ~(uint64_t)63;
  }
  f.close();
  if (++job->smp < sl.d.nsamples) return;
  plan();
  job->smp = -1;
  next_sample();
}

CORE_COLD void step_sample_open() {
  Slot& sl = slots[job->slot];
  Smp& m = sl.smp[job->smp];
  if (!job->f.open(sl.d.samples[job->smp], kRead)) {
    log(CORE_STR("msamp: %s: cannot read %s"), slot_name(job->slot), sl.d.samples[job->smp]);
    if (!m.cached && m.pe >= 0) pcache::fill_end(m.pe, 0, false), m.pe = -1;
    m.frames = 0;
    next_sample();
    return;
  }
  uint32_t fb = m.a.ch * (m.a.bits / 8);
  uint32_t first = m.part == 0 ? 0 : m.ws;
  job->want_n = m.part == 0 ? m.head_n : m.we - m.ws;
  job->dst = (int16_t*)(m.part == 0 ? m.head : m.res2);
  job->got = 0;
  job->f.seekSet((uint64_t)m.a.data_off + (uint64_t)first * fb);
  job->phase = kSampleRead;
}

CORE_COLD void step_sample_read() {
  Slot& sl = slots[job->slot];
  Smp& m = sl.smp[job->smp];
  uint32_t fb = m.a.ch * (m.a.bits / 8);
  uint32_t fr = kStreamChunk / fb, left = job->want_n - job->got;   // as a stream's read
  if (fr > left) fr = left;
  int got = job->f.read(job->buf, fr * fb);
  uint32_t whole = got > 0 ? (uint32_t)got / fb : 0;
  convert(m.a, job->buf, whole, job->dst + (size_t)job->got * m.ch, m.ch);
  job->got += whole;
  bool shrt = whole < fr;
  if (!shrt && job->got < job->want_n) return;
  if (shrt) {   // a short file: what was read plays
    if (m.part == 0) m.frames = m.head_n = job->got;
    m.res2 = nullptr, m.ws = m.we = 0;
    if (m.loop_e >= m.frames) m.has_loop = false;
  }
  m.ok = m.frames > 0;
  if (m.part == 0 && !m.cached && m.pe >= 0) {   // the head into the cache (shared from now on), pinned for the slot
    pcache::fill_end(m.pe, job->got, m.ok && job->got >= 2);
    pcache::Info in;
    if (m.ok && pcache::get(m.pe, &in)) m.blk = in.blk, pcache::pin(in.blk);
    else m.ok = false, m.pe = -1;
  }
  if (m.part == 0 && m.res2) {
    m.part = 1;
    job->phase = kSampleOpen;
    job->f.close();
    return;
  }
  if (!m.ok) m.frames = 0;
  next_sample();
}

CORE_COLD void step_sfz() {
  Slot& sl = slots[job->slot];
  uint32_t want_n = job->text_size - job->text_len;
  if (want_n > kChunk) want_n = kChunk;
  int got = want_n ? job->f.read(job->text + job->text_len, want_n) : 0;
  if (got > 0) job->text_len += got;
  if (got > 0 && job->text_len < job->text_size) return;
  job->f.close();
  job->text[job->text_len] = 0;
  char dir[kPath];
  strcpy(dir, sl.path);
  char* sl_ = strrchr(dir, '/');
  if (sl_) *sl_ = 0;
  if (!parse(job->text, (int)job->text_len, dir, &sl.d)) {
    job_fail(sl.d.err);
    return;
  }
  // the pool entry's members (in the .sfz's order of distinct samples): each played from where the library finds it
  // now (a member moved: from there, the .sfz unchanged; missing: as the .sfz says, its regions then silent)
  int nm = 0;
  const pools::Member* mem = pools::members(pools::kInstr, job->entry, &nm);
  for (int i = 0; mem && i < sl.d.nsamples; ++i) {
    const pools::Member* mi = member_of(mem, nm, i, sl.d.samples[i]);
    if (!mi) continue;   // (a sample the entry does not know: as the .sfz says)
    lib::Resolved rv;
    lib::State rs = lib::resolve(mi->ref, &rv);
    if (rs != lib::kMissing && rv.path[0] && strcmp(rv.path, sl.d.samples[i])) {
      log(CORE_STR("msamp: %s: %s played from %s"), slot_name(job->slot), sl.d.samples[i], rv.path);
      sniprintf(sl.d.samples[i], kPath, "%s", rv.path);
    }
  }
  memset(sl.seq, 0, sizeof(uint16_t) * kMaxRegions);
  job->smp = 0;
  job->phase = kHeaders;
}

// The voice's selection from INST (the control core's, at every report): the pool row; OFF / empty / missing: silent
CORE_COLD void select(int vi) {
  int e = srclib::code_of(vp[vi][0]);
  if (v[vi].ms || force[vi]) pools::voice_on(pools::kInstr, vi, e);
  if (!list || e < 0 || e >= nlist || srclib::silent(list, e)) {
    vslot[vi] = -1, want[vi] = -1;
    return;
  }
  want[vi] = (int16_t)e;
  int s = resident(e);
  if (s >= 0) slots[s].used_ms = now_ms();
  vslot[vi] = (int8_t)s;   // -1 while it loads: its notes so far ring on from their own slot
}

// The list composed from the INSTRUMENTS pool (srclib::compose): failed stays with its code while its path is the same,
// the resident instruments stay where the list still has them, the voices select again.
CORE_COLD void compose() {
  Entry* old = list;
  Entry* nl = list == lbuf[0] ? lbuf[1] : lbuf[0];
  composed_sig = srclib::pool_sig(srclib::kMs);
  int n = srclib::compose(srclib::kMs, nl);
  for (int i = 0; i < n; ++i)
    if (!old || strcasecmp(old[i].path, nl[i].path)) failed[i] = 0;
  bool loading = job->phase >= kSfzRead;
  for (int s = 0; s < kSlots; ++s) {
    if (slots[s].state == kFree) continue;
    int e = -1;
    for (int i = 0; i < n && e < 0; ++i)
      if (nl[i].path[0] && !strcasecmp(nl[i].path, slots[s].path)) e = i;
    slots[s].entry = e;
    if (loading && job->slot == s) job->entry = e;
    else if (e < 0 && slots[s].state == kReady && !slot_playing(s)) slot_release(s);
  }
  list = nl;
  nlist = n;
  st.listed = pools::count(pools::kInstr);
  __atomic_store_n(&g_list_gen, g_list_gen + 1, __ATOMIC_RELEASE);
  for (int vi = 0; vi < kVoices; ++vi)
    if (want[vi] >= 0 || v[vi].ms) select(vi);
}

// /.rytm/sdtest.txt's read-ahead and stream count (sdtest.h), if the card was tested
CORE_COLD void load_limits() {
  File f;
  if (!f.open("/.rytm/sdtest.txt", kRead)) return;
  char t[600];
  int n = f.read(t, sizeof t - 1);
  f.close();
  if (n <= 0) return;
  t[n] = 0;
  const char* a = strstr(t, "ahead_ms=");
  const char* s = strstr(t, "streams=");
  if (a && s) set_stream_limits((uint32_t)to_int(a + 9), to_int(s + 8));
}

}  // namespace

CORE_COLD void set_stream_limits(uint32_t a, int n) {
  ahead_ms = a < 100 ? 100 : a > 600 ? 600 : a;
  max_streams = n < 1 ? 1 : n > kTotal ? kTotal : n;
  log(CORE_STR("msamp: streaming read-ahead %lu ms, at most %d notes streaming"), (unsigned long)ahead_ms, max_streams);
}
CORE_COLD void set_head_ms(uint32_t ms) { head_ms = ms < 50 ? 50 : ms > 120000 ? 120000 : ms; }

CORE_COLD void begin(bool card) {
  card_ok = card;
  if (!job) {
    job = new Job();
    job->text = new char[kSfzMax + 1];
    lbuf[0] = new Entry[srclib::kEntries];
    lbuf[1] = new Entry[srclib::kEntries];
    list = lbuf[0];
    wrs = new Wr[kVoices * kPerVoice];
    for (int i = 0; i < kVoices * kPerVoice; ++i) wrs[i].rq = -1, wrs[i].on = wrs[i].open = wrs[i].ending = false;
    rqs = new Rq[kInflight];
    for (int i = 0; i < kInflight; ++i) rqs[i].wi = -1, rqs[i].r.state = cardq::kFree;
    rings = new int16_t[(size_t)kVoices * kPerVoice * kRing];
    for (int s = 0; s < kSlots; ++s) {
      Slot& sl = slots[s];
      sl.d.regions = new Region[kMaxRegions];
      sl.d.max_regions = kMaxRegions;
      sl.d.samples = new char[kMaxSamples][kPath];
      sl.d.max_samples = kMaxSamples;
      sl.smp = new Smp[kMaxSamples];
      sl.seq = new uint16_t[kMaxRegions];
    }
    pcache::begin(pool_ram_budget());   // the pool RAM (the first machine to start takes it)
    arena = pcache::ok();
    slot_cap = pcache::stats().budget;
    st.arena = slot_cap, st.slot_cap = slot_cap;
    for (int s = 0; s < kSlots; ++s)
      for (int i = 0; i < kMaxSamples; ++i) slots[s].smp[i].pe = slots[s].smp[i].blk = slots[s].smp[i].priv = -1;
    log(CORE_STR("msamp: instruments in the pool RAM (%lu MB, shared), %d slots; stream rings %lu KB"),
        (unsigned long)(slot_cap >> 20), kSlots, (unsigned long)((uint64_t)kVoices * kPerVoice * kRing * 2 >> 10));
  }
  for (int s = 0; s < kSlots; ++s) slot_release(s), slots[s].entry = -1;
  for (int vi = 0; vi < kVoices; ++vi) vslot[vi] = -1, want[vi] = -1, pend[vi].on = false;
  composed_sig = 0;
  compose();
  memset(lpend, 0, sizeof lpend);
  for (int vi = 0; vi < kVoices; ++vi)
    for (int k = 0; k < kPerVoice; ++k) notes[vi][k].slot = -1, wr_end(wr_of(vi, k));
  job->phase = kIdle;
  if (card) load_limits();
}

CORE_COLD void rescan() {   // the card changed: failed instruments tried again, the ones not playing read again on use
  if (!job) return;
  memset(failed, 0, sizeof failed);
  for (int s = 0; s < kSlots; ++s)
    if (slots[s].state == kReady && !slot_playing(s)) slot_release(s);
  for (int vi = 0; vi < kVoices; ++vi) vslot[vi] = -1, want[vi] = -1;
}

CORE_COLD bool step() {
  if (!job || !arena) return false;
  if (stream_step()) return true;
  switch (job->phase) {
    case kIdle:
      if (srclib::pool_sig(srclib::kMs) != composed_sig) {   // the INSTRUMENTS pool changed (a row, a state)
        compose();
        return true;
      }
      return start_load();
    case kSfzRead: step_sfz(); return true;
    case kHeaders: step_header(); return true;
    case kSampleOpen: step_sample_open(); return true;
    case kSampleRead: step_sample_read(); return true;
  }
  return false;
}

CORE_COLD bool forced(int vi) { return force[vi & 7]; }

CORE_COLD void report(int vi, int note, int vel, const float* p) {
  vi &= 7;
  for (int k = 0; k < 16; ++k) vp[vi][k] = p[k];
  select(vi);
  pools::seen(pools::kInstr, srclib::code_of(vp[vi][0]));   // the used set
  v[vi].live = true;
  if (note < 0) return;
  int s = vslot[vi];
  if (s >= 0) {
    trig(vi, note, vel);
    return;
  }
  if (!nlist) {
    log(CORE_STR("trig voice %d MULTISAMPLE: no instruments"), vi);
    return;
  }
  if (want[vi] < 0) return;   // rui1038: a bank's OFF / empty row
  pend[vi] = {true, (uint8_t)note, (uint8_t)vel, now_ms()};
  ++st.waits;
  log(CORE_STR("trig voice %d MULTISAMPLE waits for %s"), vi, list[want[vi]].name);
}

CORE_COLD void live_on(int vi, int key, int vel, int ch, int mnote) {
  vi &= 7;
  int s = vslot[vi];
  if (key < 0 || key > 127) return;
  if (s < 0 || slots[s].state != kReady) {   // its instrument loads: the note waits (loaded() starts it)
    if (!nlist || want[vi] < 0) return;
    int f = -1;
    uint32_t now = now_ms();
    for (int k = 0; k < kPerVoice; ++k) {
      LivePend& l = lpend[vi][k];
      if (l.on && l.ch == ch && l.mnote == mnote) f = k;   // the same note again: its newest
      if (f < 0 && (!l.on || now - l.ms > kLateMs)) f = k;
    }
    if (f < 0) return;   // a full queue (16 notes waiting): dropped
    lpend[vi][f] = {true, (uint8_t)key, (uint8_t)vel, (uint8_t)mnote, (int8_t)ch, now};
    ++st.waits;
    return;
  }
  slots[s].used_ms = now_ms();
  key_vel[vi][key] = (uint8_t)vel;
  cur_live = {true, (int8_t)ch, (uint8_t)mnote};
  play_key(vi, s, key, vel, kTrigAttack, 0.6f);   // as poly FM's live notes
  cur_live = {false, 0, 0};
  v[vi].live = true;
}

CORE_COLD void live_off(int vi, int ch, int mnote) {
  vi &= 7;
  for (int k = 0; k < kPerVoice; ++k)   // still waiting for the instrument: dropped
    if (lpend[vi][k].on && lpend[vi][k].ch == ch && (mnote < 0 || lpend[vi][k].mnote == mnote)) lpend[vi][k].on = false;
  uint8_t keys[kPerVoice];
  int nk = 0;
  for (int k = 0; k < kPerVoice; ++k) {
    Note& n = notes[vi][k];
    if (!n.live || n.ch != ch || (mnote >= 0 && n.mnote != mnote)) continue;
    n.live = false;
    if (!n.held || note_free(n)) {
      n.held = false;
      continue;
    }
    n.held = false;
    st_rel(&n.rel_n, n.rel_n + 1);
    bool seen = false;
    for (int j = 0; j < nk; ++j) seen = seen || keys[j] == n.key;
    if (!seen) keys[nk++] = n.key;
  }
  int s = vslot[vi];
  if (vp[vi][3] < 64 || s < 0 || slots[s].state != kReady) return;
  for (int j = 0; j < nk; ++j) play_key(vi, s, keys[j], key_vel[vi][keys[j]], kTrigRelease, 1.0f);
}

CORE_COLD int voice_notes(int vi) {
  int a = 0;
  for (int k = 0; k < kPerVoice; ++k) a += !note_free(notes[vi & 7][k]);
  return a;
}

CORE_COLD int voice_keys(int vi, uint8_t* keys, int max) {
  int n = 0;
  for (int k = 0; k < kPerVoice && n < max; ++k) {
    const Note& x = notes[vi & 7][k];
    if (!note_free(x) && !x.rtrig) keys[n++] = x.key;
  }
  return n;
}

CORE_COLD const char* entry_name(int inst) {
  if (!list || nlist <= 0) return "";
  return list[inst < 0 ? 0 : inst >= nlist ? nlist - 1 : inst].name;
}

CORE_COLD void note_off(int vi) {
  vi &= 7;
  pend[vi].on = false;
  release_voice(vi, vp[vi][3] >= 64);
}

CORE_COLD int entries() { return nlist; }   // every code (bank * 128 + row)
CORE_COLD const char* entry_group(int e) { return list && e >= 0 && e < nlist ? list[e].grp : ""; }
CORE_COLD const char* entry_path(int e) { return list && e >= 0 && e < nlist ? list[e].path : ""; }
CORE_COLD int entry_lib(int e) { return list && e >= 0 && e < nlist ? list[e].lib : srclib::kLibOff; }
CORE_COLD int entry_state(int e) {   // as gr::entry_state: 0 none 1 card 2 loading 3 resident 5 failed
  if (!list || e < 0 || e >= nlist || !srclib::playable(list[e])) return 0;
  if (failed[e] == 1) return 5;
  if (resident(e) >= 0) return 3;
  for (int vi = 0; vi < kVoices; ++vi)
    if (want[vi] == e) return 2;
  return 1;
}
CORE_COLD const char* entry_note(int e, char* b, int cap) {
  if (cap < 1) return "";
  b[0] = 0;
  int st_ = entry_state(e);
  if (st_ == 5) sniprintf(b, cap, "FAILED");
  else if (st_ == 2) sniprintf(b, cap, failed[e] == 2 ? "NO FREE SLOT" : "LOADING");
  else if (st_ == 3) sniprintf(b, cap, "%s", slots[resident(e)].forced ? "RAM (BUDGET)" : slots[resident(e)].ram ? "RAM" : "STREAM");
  else if (!st_ && list && e >= 0 && e < nlist && list[e].lib == srclib::kLibMissing) sniprintf(b, cap, "MISSING");
  return b;
}
uint32_t list_gen() { return __atomic_load_n(&g_list_gen, __ATOMIC_ACQUIRE); }
CORE_COLD int src_entry(int vi, int v) { return !list || nlist <= 0 ? -1 : v < 0 ? 0 : v >= nlist ? nlist - 1 : v; }
CORE_COLD bool voice_picked(int) { return false; }   // (no picks over INST: a choice writes INST)
CORE_COLD bool pick(int vi, int e) {   // the touchscreen's choice: INST written on the Rytm (48: the kit keeps it)
  vi &= 7;
  if (!job || !list || e < 0 || e >= nlist) return false;
  ui::remote_param_write(vi, kMultiMachine, 0, e & 127, srclib::lo_of(e));
  vp[vi][0] = (float)(e & 127) + (float)srclib::lo_of(e) / 128.0f;
  select(vi);
  log(CORE_STR("msamp: voice %d: INST %c%03d %s"), vi, 'A' + srclib::bank_of(e), e & 127, entry_name(e));
  return true;
}

CORE_COLD void silence(int vi) {
  vi &= 7;
  pend[vi].on = false;
  for (int k = 0; k < kPerVoice; ++k) lpend[vi][k].on = false;
  for (int k = 0; k < kPerVoice; ++k) kill(vi, k);
}

// ---- the render core's
namespace {
inline float herm(float xm1, float x0, float x1, float x2, float t) {
  float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, b = c + w + a;
  return ((a * t - b) * t + c) * t + x0;
}
}  // namespace

static float head_g[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};   // the render core's: each voice's headroom gain

static void render_impl(int vi, float* out, float* outr, int nfr);
void render_add(int vi, float* out, int nfr) { render_impl(vi, out, nullptr, nfr); }
// OUT PRE / POST: each region's sample in its two channels (a mono one on both), its pan / width / position applied
void render_add2(int vi, float* l, float* r, int nfr) { render_impl(vi, l, r, nfr); }

static void render_impl(int vi, float* out, float* outr, int nfr) {
  if (!arena) return;
  if (nfr > kBlock) {   // (one block at a time: the headroom's accumulator)
    for (int d = 0; d < nfr; d += kBlock)
      render_impl(vi, out + d, outr ? outr + d : nullptr, nfr - d < kBlock ? nfr - d : kBlock);
    return;
  }
  float acc[kBlock] = {0}, accr[kBlock] = {0};
  float* const dst = out;
  float* const dstr = outr;
  out = acc;
  float wk[kPerVoice];   // the notes' weights (their envelopes), one a key
  uint8_t kk[kPerVoice];
  int nw = 0;
  for (int k = 0; k < kPerVoice; ++k) {
    Note& n = notes[vi][k];
    uint32_t q = ld_acq(&n.seq);
    if (q != n.r_seq && !(q & 1)) {
      Start c = n.st;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (ld_rlx(&n.seq) == q) {
        n.r_seq = q, n.r = c, n.r_on = true, n.r_rel = false, n.r_sounded = false;
        n.pos = c.pos, n.stage = 0, n.n = 0, n.lvl = 0, n.ug = 1;
      }
    }
    if (!n.r_on) continue;
    Start& r = n.r;
    if (ld_acq(&n.kill_n) != r.kill_gen) {
      n.r_on = false;
      st_rel(&n.done, n.r_seq);
      continue;
    }
    // released: from the next block once it has sounded. A note whose note-on and note-off both came before its first
    // render (a very short Rytm trig: its 0x04 and 0x11 in one control pass) was released at level 0 and silent; it
    // now plays its attack for a block first (a minimum gate, as FM / PLAITS sound such a trig). A normal note has
    // sounded by its release: unchanged.
    if (!n.r_rel && !r.one_shot && n.r_sounded && ld_acq(&n.rel_n) != r.rel_gen) n.r_rel = true, n.stage = 4;
    // the stream's frames in the ring now: path [lo, wp)
    uint32_t wp = 0, lo = 0;
    const int ch = r.ch == 2 ? 2 : 1;
    const uint32_t cap = kRing / ch;   // the ring's frames
    if (r.ring) {
      uint32_t g1 = ld_acq(&n.wgen);
      wp = ld_acq(&n.wp);
      uint32_t g2 = ld_acq(&n.wgen);
      if (g1 != n.r_seq || g2 != g1) wp = r.w0;   // nothing yet
      lo = wp > cap ? wp - cap : 0;
      if (lo < r.w0) lo = r.w0;
    }
    const bool two = outr && ch == 2;   // both channels read (a stereo voice, a stereo sample)
    // the width (M/S) and the pan of a stereo voice: L' = a l + b r, R' = b l + a r, x pl / pr
    const float wa = 0.5f * (1.0f + r.wd), wb = 0.5f * (1.0f - r.wd);
    const int16_t* head = r.head;
    int64_t end = r.end;
    bool fin = false, dry = false;
    for (int s = 0; s < nfr; ++s) {
      bool looping = r.mode == 1 || (r.mode == 2 && !n.r_rel);
      int64_t i = (int64_t)n.pos;
      float t = (float)(n.pos - (double)i);
      int64_t len = (int64_t)r.loop_e - r.loop_s;
      float x[4], xr[4] = {0, 0, 0, 0};
      bool miss = false;
      for (int j = 0; j < 4; ++j) {
        int64_t f = i - 1 + j;
        if (looping && f >= r.loop_e) f -= len;
        f = f < 0 ? 0 : f >= end ? end - 1 : f;
        uint32_t u = (uint32_t)f;
        const int16_t* q = nullptr;
        if (u < r.head_n) q = head + (size_t)u * ch;
        else if (r.res2 && u >= r.ws && u < r.we) q = r.res2 + (size_t)(u - r.ws) * ch;
        else if (r.ring) {
          uint32_t p = path_of(u, r.head_n, r.ws, r.we);
          if (p >= lo && p < wp) q = r.ring + (size_t)(p & (cap - 1)) * ch;
        }
        if (!q) {
          x[j] = xr[j] = 0, miss = true;
        } else if (ch == 2) {
          if (two) x[j] = q[0], xr[j] = q[1];
          else x[j] = (float)(((int32_t)q[0] + (int32_t)q[1]) >> 1);   // mono: as the summed load (convert)
        } else {
          x[j] = q[0];
        }
      }
      if (miss && !n.r_sounded) break;   // its start is not in yet: it waits (no time passes for it)
      if (miss) dry = true;
      n.ug += miss ? -1.0f / 48.0f : 1.0f / 48.0f;   // 1 ms fades out and in
      n.ug = n.ug < 0 ? 0 : n.ug > 1 ? 1 : n.ug;
      switch (n.stage) {   // the envelope
        case 0:
          if (n.n >= r.att) { n.stage = 1, n.n = 0, n.lvl = 1; }
          else { n.lvl = (float)(n.n + 1) / (float)r.att; ++n.n; }
          break;
        case 1:
          if (n.n >= r.hold) n.stage = r.dcoef > 0 ? 2 : 3, n.lvl = r.dcoef > 0 ? 1 : r.sus;
          else ++n.n;
          break;
        case 2:
          n.lvl = r.sus + (n.lvl - r.sus) * r.dcoef;
          if (n.lvl - r.sus < 1e-4f) n.stage = 3, n.lvl = r.sus;
          break;
        case 3: n.lvl = r.sus; break;
        default:
          n.lvl *= r.rcoef;
          if (n.lvl < 1e-4f) fin = true;
          break;
      }
      if (fin) break;
      n.r_sounded = true;
      float y = miss ? 0.0f : herm(x[0], x[1], x[2], x[3], t);
      if (outr) {
        float g = (1.0f / 32768.0f) * n.lvl * r.gain * n.ug;
        float yr = two ? (miss ? 0.0f : herm(xr[0], xr[1], xr[2], xr[3], t)) : y;
        float a = two ? wa * y + wb * yr : y, b = two ? wb * y + wa * yr : y;
        out[s] += a * g * r.pl;
        accr[s] += b * g * r.pr;
      } else {
        out[s] += y * (1.0f / 32768.0f) * n.lvl * r.gain * n.ug;
      }
      n.pos += r.inc;
      if (looping && n.pos >= r.loop_e) n.pos -= (double)len;
      else if (n.pos >= (double)end) { fin = true; break; }
    }
    if (dry) __atomic_fetch_add(&g_underruns, 1u, __ATOMIC_RELAXED);
    if (!fin) {   // its weight: the envelope now, the loudest note of its key (a key's layers count once)
      int j = 0;
      while (j < nw && kk[j] != n.key) ++j;
      if (j == nw) kk[nw] = n.key, wk[nw++] = 0;
      if (n.lvl > wk[j]) wk[j] = n.lvl;
    }
    if (r.ring) {   // where it reads now, for the stream's room
      int64_t rd = (int64_t)n.pos - 2;
      st_rlx(&n.rgen, 0);
      st_rel(&n.rd, (uint32_t)(rd < 0 ? 0 : rd));
      st_rel(&n.rgen, n.r_seq);
    }
    if (fin) {
      n.r_on = false;
      st_rel(&n.done, n.r_seq);
    }
  }
  // the headroom (as poly FM's, core/poly.h kHeadAtkMs): 1/sqrt of the keys' envelopes, smoothed, ramped over the block
  float e = 0;
  for (int j = 0; j < nw; ++j) e += wk[j];
  const float atk = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadAtkMs));
  const float rel = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadRelMs));
  float g0 = head_g[vi], target = e > 1.0f ? 1.0f / sqrtf(e) : 1.0f;
  float g1 = g0 + (target - g0) * (target < g0 ? atk : rel);
  if (fabsf(g1 - target) < 1e-5f) g1 = target;   // (settled: exactly)
  head_g[vi] = g1;
  if (!nw) return;
  float dg = (g1 - g0) / (float)nfr;
  for (int s = 0; s < nfr; ++s) dst[s] += acc[s] * (g0 + dg * (float)(s + 1));
  if (dstr)
    for (int s = 0; s < nfr; ++s) dstr[s] += accr[s] * (g0 + dg * (float)(s + 1));
}

CORE_COLD float headroom(int vi) { return head_g[vi & 7]; }

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "msamp", 5) != 0 || (p[5] && p[5] != '=')) return false;
  const char* a = p[5] ? p + 6 : "";
  if (!strcmp(a, "rescan")) {
    rescan();
    sniprintf(out, cap, "msamp: failed instruments tried again");
    return true;
  }
  if (!strncmp(a, "voice:", 6)) {
    int vi = a[6] - '0';
    if (vi < 0 || vi > 7) {
      sniprintf(out, cap, "msamp: voice 0..7");
      return true;
    }
    bool on = strcmp(a + 7, ":off") != 0;
    if (force[vi] && !on) silence(vi);
    force[vi] = on;
    sniprintf(out, cap, "msamp: voice %d %s", vi, on ? "plays MULTISAMPLE (any built-in machine the Rytm reports)" : "plays its own machine");
    return true;
  }
  if (!strncmp(a, "head:", 5)) {
    set_head_ms((uint32_t)to_int(a + 5));
    sniprintf(out, cap, "msamp: heads of %lu ms from the next load", (unsigned long)head_ms);
    return true;
  }
  if (!strncmp(a, "ahead:", 6) || !strncmp(a, "streams:", 8)) {
    if (a[0] == 'a') set_stream_limits((uint32_t)to_int(a + 6), max_streams);
    else set_stream_limits(ahead_ms, to_int(a + 8));
    sniprintf(out, cap, "msamp: read-ahead %lu ms, %d streams at most", (unsigned long)ahead_ms, max_streams);
    return true;
  }
  if (!strncmp(a, "pick:", 5)) {   // pick:V:ROW (or a code)
    const char* q = a + 5;
    int vi = q[0] - '0', e = -1;
    const char* r = q[1] == ':' ? q + 2 : "";
    if (vi < 0 || vi > 7 || (!pools::parse_row(r, &e) && (*r < '0' || *r > '9'))) {
      sniprintf(out, cap, "msamp: pick:V:ROW (V 0..7, ROW A001..H127)");
      return true;
    }
    if (e < 0) e = to_int(r);
    if (!pick(vi, e)) sniprintf(out, cap, "msamp: no entry %d (%d listed)", e, nlist);
    else sniprintf(out, cap, "msamp: voice %d INST %s", vi, entry_name(e));
    return true;
  }
  if (!strcmp(a, "list")) {
    for (int i = 0; i < nlist; ++i)
      if (list[i].lib != srclib::kLibHole && list[i].lib != srclib::kLibOff)   // rui1038: banks' empty rows left out
      log(CORE_STR("msamp: INST %c%03d %s [%s] (%s)%s"), 'A' + srclib::bank_of(i), i & 127, list[i].name, list[i].grp, list[i].path, failed[i] == 1 ? " failed" : "");
    sniprintf(out, cap, "msamp: %d instruments (the list in the log)", nlist);
    return true;
  }
  if (*a) return false;
  stats();
  int n = sniprintf(out, cap, "msamp: %d listed, %d/%d resident (cap %lu MB each), %d/%d notes now (peak %d), %d streaming "
                              "(peak %d, max %d, ahead %lu ms), %lu underruns, %lu bandwidth steals, %lu KB streamed, "
                              "%lu trigs, %lu notes, %lu steals, %lu waits (%lu late), %lu loads (%lu failed, %lu left out), "
                              "%lu saves held by the card core%s;",
                    nlist, st.resident, kSlots, (unsigned long)(slot_cap >> 20), st.active, kTotal, st.peak, st.streams,
                    st.streams_peak, max_streams, (unsigned long)ahead_ms, (unsigned long)st.underruns,
                    (unsigned long)st.bw_steals, (unsigned long)(st.stream_bytes >> 10), (unsigned long)st.trigs,
                    (unsigned long)st.notes, (unsigned long)st.steals, (unsigned long)st.waits, (unsigned long)st.late,
                    (unsigned long)st.loads, (unsigned long)st.load_fails, (unsigned long)st.left_out,
                    (unsigned long)fmt::post_held(), fmt::post_stalled() ? " (one now)" : "");
  for (int s = 0; s < kSlots && n < cap; ++s)
    if (slots[s].state == kReady)
      n += sniprintf(out + n, cap - n, " %s %s %lu KB", slot_name(s), slots[s].ram ? "RAM" : "STREAM",
                     (unsigned long)(slots[s].used >> 10));
  bool any = false;
  for (int vi = 0; vi < kVoices && n < cap; ++vi)
    if (force[vi]) n += sniprintf(out + n, cap - n, "%s%d", any ? "," : "; forced voices ", vi), any = true;
  return true;
}

const Stats& stats() {
  if (!job) return st;   // not begun
  st.active = count_active();
  st.streams = count_streams();
  st.underruns = ld_rlx(&g_underruns);
  int res = 0;
  for (int s = 0; s < kSlots; ++s) res += slots[s].state == kReady;
  st.resident = res;
  return st;
}
const char* list_path(int k) { return k >= 0 && k < nlist ? list[k].path : ""; }
int voice_slot(int vi) { return vslot[vi & 7]; }
const Desc* slot_desc(int s) { return s >= 0 && s < kSlots && slots[s].state == kReady ? &slots[s].d : nullptr; }
uint64_t slot_bytes(int s) { return s >= 0 && s < kSlots ? slots[s].used : 0; }
bool slot_ram(int s) { return s >= 0 && s < kSlots && slots[s].ram; }

CORE_COLD void reset() {
  for (int vi = 0; vi < kVoices; ++vi) {
    silence(vi);
    force[vi] = false;
    head_g[vi] = 1.0f;
    for (int k = 0; k < kPerVoice; ++k) wr_quiesce(wr_of(vi, k));
  }
  uint32_t loads = st.loads, fails = st.load_fails, left = st.left_out;
  memset(&st, 0, sizeof st);
  st_rlx(&g_underruns, 0);
  st.loads = loads, st.load_fails = fails, st.left_out = left;
  st.listed = pools::count(pools::kInstr), st.arena = slot_cap, st.slot_cap = slot_cap;
}

}  // namespace msamp
}  // namespace core
#endif
