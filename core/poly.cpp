// core/poly.cpp - see poly.h. Per message and per chord: cold; render_add is the render core's.
#include "poly.h"

#include <string.h>

#include "platform.h"

#if CORE_POLY
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "keys.h"
#include "machines.h"
#include "seq/seq.h"
#include "sysex.h"
#include "ui/remote.h"
#include "voices.h"
#if CORE_MSAMP
#include "msamp/msamp.h"
#include "nmach.h"   // WAVETABLE, GRANULAR: live notes as MULTISAMPLE's
#endif
#endif

namespace core {
namespace poly {

const char* const kChordNames[kChords] = {"OFF", "MAJ",  "MIN", "SUS2", "SUS4", "7",  "MAJ7", "MIN7",
                                          "DIM", "AUG",  "6",   "MIN6", "9",    "ADD9", "MIN9", "5"};
namespace {
struct Def {
  uint8_t n;
  int8_t iv[5];
};
const Def kDefs[kChords] = {
    {1, {0}},           {3, {0, 4, 7}},      {3, {0, 3, 7}},         {3, {0, 2, 7}},
    {3, {0, 5, 7}},     {4, {0, 4, 7, 10}},  {4, {0, 4, 7, 11}},     {4, {0, 3, 7, 10}},
    {3, {0, 3, 6}},     {3, {0, 4, 8}},      {4, {0, 4, 7, 9}},      {4, {0, 3, 7, 9}},
    {5, {0, 4, 7, 10, 14}}, {4, {0, 4, 7, 14}}, {5, {0, 3, 7, 10, 14}}, {2, {0, 7}},
};

CORE_COLD int idx(float p, int n) {
  int v = (int)p;
  return v >= 0 && v < n ? v : 0;
}
CORE_COLD void sort_unique(int* t, int* m) {
  for (int i = 1; i < *m; ++i)
    for (int j = i; j > 0 && t[j - 1] > t[j]; --j) {
      int x = t[j];
      t[j] = t[j - 1], t[j - 1] = x;
    }
  int k = 0;
  for (int i = 0; i < *m; ++i)
    if (k == 0 || t[i] != t[k - 1]) t[k++] = t[i];
  *m = k;
}
// The chord's notes from 0 (may be outside 0..127 once a root is added), ascending, distinct; the count.
CORE_COLD int pattern(const Chord& c, int* t) {
  const Def& d = kDefs[c.chd >= 0 && c.chd < kChords ? c.chd : 0];
  int n = d.n;
  if (c.chd == 0) {
    t[0] = 0;
    return 1;
  }
  int b[5];
  for (int i = 0; i < n; ++i) b[i] = d.iv[i];
  int inv = c.inv % n;   // the lowest inv notes an octave up
  for (int i = 0; i < inv; ++i) b[i] += 12;
  int nb = n;
  sort_unique(b, &nb);
  int m = c.voi >= 1 && c.voi <= kVoiMax ? c.voi : n;
  for (int k = 0; k < m; ++k) t[k] = b[k % n] + 12 * (k / n);
  sort_unique(t, &m);
  for (int k = 0; k < m; ++k) t[k] += 12 * ((k * (c.spr + 1)) / m);
  sort_unique(t, &m);
  return m;
}
}  // namespace

CORE_COLD Chord chord_of(const float* p) {
  Chord c;
  c.chd = idx(p[12], kChords);
  c.inv = idx(p[13], kInvs);
  c.spr = idx(p[14], kSprs);
  c.voi = idx(p[15], kVoiMax + 1);
  return c;
}

CORE_COLD int expand(int root, const Chord& c, uint8_t* out) {
  int t[kMaxNotes + 2];
  int m = pattern(c, t), k = 0;
  for (int i = 0; i < m; ++i) {
    int x = root + t[i];
    if (x >= 0 && x <= 127) out[k++] = (uint8_t)x;
  }
  return k;
}

CORE_COLD Detected detect(const uint8_t* notes, int n) {
  int s[16];
  int m = 0;
  for (int i = 0; i < n && m < 16; ++i) s[m++] = notes[i];
  sort_unique(s, &m);
  if (m > kMaxNotes) m = kMaxNotes;   // the lowest six
  Detected d = {m ? s[0] : 60, {0, 0, 0, 0}, true};
  if (m <= 1) return d;
  // exact: tiers of VOI (AUTO, then more notes than the chord, then fewer), then INV, SPR, the chord's number
  for (int tier = 0; tier < 3; ++tier)
    for (int inv = 0; inv < kInvs; ++inv)
      for (int spr = 0; spr < kSprs; ++spr)
        for (int chd = 1; chd < kChords; ++chd) {
          int size = kDefs[chd].n;
          if (inv >= size) continue;
          if ((tier == 0 && m != size) || (tier == 1 && m <= size) || (tier == 2 && m >= size)) continue;
          Chord c = {chd, inv, spr, tier == 0 ? 0 : m};
          int t[kMaxNotes + 2];
          if (pattern(c, t) != m) continue;
          int root = s[0] - t[0];
          bool same = root >= 0 && root <= 127;
          for (int i = 0; same && i < m; ++i) same = s[i] - root == t[i];
          if (same) return {root, c, true};
        }
  // nearest by pitch classes
  int have = 0;
  for (int i = 0; i < m; ++i) have |= 1 << (s[i] % 12);
  int best = -1, best_root = 0, bn = 0, bd = 1;
  for (int chd = 1; chd < kChords; ++chd)
    for (int r = 0; r < 12; ++r) {
      int pcs = 0;
      for (int i = 0; i < kDefs[chd].n; ++i) pcs |= 1 << ((r + kDefs[chd].iv[i]) % 12);
      int inter = __builtin_popcount(pcs & have), uni = __builtin_popcount(pcs | have);
      if (best < 0 || inter * bd > bn * uni) best = chd, best_root = r, bn = inter, bd = uni;
    }
  d.exact = false;
  if (best < 0 || 2 * bn <= bd) return d;   // nothing near: OFF on the lowest
  int bass = s[0], bpc = bass % 12, inv = 0, root = bass - ((bpc - best_root + 12) % 12);
  for (int i = 1; i < kDefs[best].n && i < kInvs; ++i)
    if ((best_root + kDefs[best].iv[i]) % 12 == bpc) inv = i, root = bass - kDefs[best].iv[i];
  int span = s[m - 1] - bass;
  d.root = root < 0 ? root + 12 : root;
  d.c = {best, inv, span < 12 ? 0 : span < 24 ? 1 : 2, m == kDefs[best].n ? 0 : m};
  return d;
}

CORE_COLD const char* value_text(int k, int v, char* buf, int cap) {
  switch (k) {
    case 0: return kChordNames[idx((float)v, kChords)];
    case 1: sniprintf(buf, cap, "INV%d", idx((float)v, kInvs)); return buf;
    case 2: sniprintf(buf, cap, "SPR%d", idx((float)v, kSprs)); return buf;
    default: {
      int n = idx((float)v, kVoiMax + 1);
      if (!n) return "AUTO";
      sniprintf(buf, cap, "%d", n);
      return buf;
    }
  }
}

CORE_COLD int sysex_chord_write(uint8_t* m, int track, int root, const Chord& c) {
  const uint8_t b[] = {0xF0, 0x7D, 0x52, 0x54, kChordCmd, kChordWrite, (uint8_t)(track & 0x7F), (uint8_t)(root & 0x7F),
                       (uint8_t)(c.chd & 0x7F), (uint8_t)(c.inv & 0x7F), (uint8_t)(c.spr & 0x7F), (uint8_t)(c.voi & 0x7F),
                       0xF7};
  memcpy(m, b, sizeof b);
  return (int)sizeof b;
}

#if CORE_POLY
// ---- the pool
namespace {
struct Slot {
  plaits::Voice voice;
  plaits::Patch patch;
  plaits::Modulations mod;
  rytm::FmMachine fm;
  volatile int8_t owner;   // the voice, -1 free (the render core frees a quiet released slot)
  bool held;               // gated: not yet released (NoteOff)
  bool live;               // a MIDI note (its note-off releases it), else a trig's
  int8_t ch;               // live: the MIDI channel and note
  uint8_t midi_note, note;
  uint16_t quiet;          // released: blocks in a row below the floor
  uint32_t order;          // allocation order (the oldest steals first)
  uint32_t released_ms;
  float gain;
  float ref;               // the render core's: its block peak while held (a released note's weight: peak / ref)
  char* scratch;           // its Plaits voice's 16 KB (a re-init: start())
  bool used;               // played a note since its last re-init
};
Slot* slots = nullptr;
float head_g[kVoices];   // the render core's: each voice's pool headroom gain (render_add)
void (*sink)(uint8_t, uint8_t, uint8_t) = nullptr;
uint32_t order_n = 0;
uint8_t vbytes[kVoices][128];
bool vhas[kVoices];
float vknobs[kVoices][8];
Stats st;
namespace {
// 1042 (the CHRO leftover): the Rytm's TRK held (the remote page's caught TRK, its 31) and the auto channel's notes
// taken as that gesture's (bit n: a note-on swallowed, its note-off too)
bool trk_held = false;
uint32_t trk_ms = 0, trk_sw[4] = {};
constexpr uint32_t kTrkMaxMs = 10000;   // a release never reported (a lost message): TRK no longer taken as held
}  // namespace


// live-chord state, per MIDI channel
struct Chan {
  int8_t vi;            // the bound voice, -1 none
  int8_t off;           // the 0x04's note - the MIDI note
  bool direct;          // bound by poly=bind (a keyboard on the host), not by votes: stays bound
  bool chordkeys;       // direct + chord: a key plays the voice's CHD/INV/SPR/VOI chord on it (6-OP FM)
  uint32_t last_ms;     // its last note-on
  uint32_t held[4];     // notes held (bits): sounding, a mono steal's kept notes too
  uint32_t kept[4];     // the notes a mono steal kept (their pads' note-offs left out; poly.h kStealMs)
  int16_t cur;          // the note the Rytm holds now (its last note-on), -1 none
  int16_t prev;         // the note the last note-on displaced (its note-off within kStealMs: a steal), -1 none
  uint32_t prev_ms;     // that note-on's time
  uint32_t g0;          // the gesture's first note-on (a note-on kGestureMs after the one before starts one)
  uint32_t on_ms[128];  // each note's last note-on
  uint8_t since;        // note-ons since the channel was last silent (one: a lone note, not a chord's)
  bool open;            // a chord window
  uint32_t t0;
  uint8_t notes[kMaxNotes], n;   // the window's MIDI notes
};
Chan chans[16];
struct Recent {         // recent note-ons (any channel) and 0x04 trigs (FM voices), for binding
  uint8_t ch, note;
  uint32_t ms;
  bool used;
};
Recent ons[8], trigs[8];   // (trigs: ch is the voice)
int ons_w = 0, trigs_w = 0;
struct Vote {
  int8_t ch, vi, off;
  uint8_t count;
  uint32_t ms;
};
Vote votes[8];
struct Pending {
  bool on;
  uint8_t note, vel;
  uint32_t ms;
};
Pending pend[kVoices];
View views[kVoices];   // what the remote UI's chord view shows (view())

// A voice the live binding plays: 6-OP FM (this pool) or a note machine (MULTISAMPLE, WAVETABLE, GRANULAR: their own
// live notes, core/nmach.h)
CORE_COLD bool is_ms(int vi) {
#if CORE_MSAMP
  return nm::is(vi);
#else
  (void)vi;
  return false;
#endif
}
CORE_COLD bool plays_live(int vi) { return v[vi & 7].fm || is_ms(vi); }

CORE_COLD int count_voice(int vi) {
  int k = 0;
  for (int i = 0; i < kTotal; ++i) k += slots[i].owner == vi;
  return k;
}
CORE_COLD int count_all() {
  int k = 0;
  for (int i = 0; i < kTotal; ++i) k += slots[i].owner >= 0;
  return k;
}
CORE_COLD int oldest(int vi, bool released_only) {
  int b = -1;
  for (int i = 0; i < kTotal; ++i) {
    const Slot& s = slots[i];
    if (s.owner < 0 || (vi >= 0 && s.owner != vi) || (released_only && s.held)) continue;
    if (b < 0 || (int32_t)(s.order - slots[b].order) < 0) b = i;
  }
  return b;
}
CORE_COLD int alloc(int vi, int note) {
  for (int i = 0; i < kTotal; ++i)
    if (slots[i].owner == vi && slots[i].note == note) return i;   // the same pitch: retriggered
  int b = -1;
  if (count_voice(vi) >= kPerVoice) {
    b = oldest(vi, true);
    if (b < 0) b = oldest(vi, false);
  } else {
    for (int i = 0; i < kTotal && b < 0; ++i)
      if (slots[i].owner < 0) b = i;
    if (b < 0) b = oldest(-1, true);
    if (b < 0) b = oldest(-1, false);
  }
  if (b >= 0 && slots[b].owner >= 0) ++st.steals;
  return b;
}
CORE_COLD void start(int vi, int note, int vel, float gain, bool live, int ch, int midi_note) {
  int i = alloc(vi, note);
  if (i < 0) return;
  Slot& s = slots[i];
  // A free slot's Plaits voice stopped rendering when its note went quiet (render_add frees it at -80 dB): its DX7
  // operator envelopes stayed where they were, the modulators' often still mid-release, and its next note started from
  // there (a faster, brighter, louder attack: up to 8 dB in the first 10 ms, measured on ROM voice 10). A never-used
  // slot starts from rest, so a slot's first note was the quieter one. Now every note on a free slot starts from rest
  // (the voice re-initialised: ~6 us on the Mac; control core, between blocks, the slot not rendering): a slot's
  // notes are alike, the first as the later ones. A retrigger of the same pitch or a steal keeps the sounding state.
  if (s.owner < 0 && s.used) {
    stmlib::BufferAllocator allocator(s.scratch, 16384);
    s.voice.Init(&allocator);
    memset(&s.patch, 0, sizeof s.patch);
    memset(&s.mod, 0, sizeof s.mod);
    s.mod.trigger_patched = true;
    s.fm.Init();
  }
  s.used = true;
  s.owner = (int8_t)vi;
  s.held = true;
  s.live = live;
  s.ch = (int8_t)ch;
  s.midi_note = (uint8_t)midi_note;
  s.note = (uint8_t)note;
  s.quiet = 0;
  s.order = ++order_n;
  s.gain = gain;
  s.ref = 0;
  s.fm.SetVoice(vhas[vi] ? vbytes[vi] : nullptr);
  s.fm.SetKnobs(vknobs[vi]);
  s.fm.NoteOn((float)note, (vel < 1 ? 1 : vel) / 127.0f);
  v[vi].live = true;
  ++st.notes;
  if (live) ++st.live_notes;
  int a = count_all();
  st.active = a;
  if (a > st.peak) st.peak = a;
}
CORE_COLD void live_start(int vi, int note, int vel, int ch, int midi_note) {
#if CORE_MSAMP
  if (is_ms(vi)) {
    nm::live_on(vi, note, vel, ch, midi_note);
    ++st.notes, ++st.live_notes;
    return;
  }
#endif
  start(vi, note, vel, 0.6f, true, ch, midi_note);
}
CORE_COLD bool bit(const uint32_t* b, int n) { return b[n >> 5] >> (n & 31) & 1; }
CORE_COLD void emit(uint8_t st, uint8_t d1, uint8_t d2) {
  if (sink) sink(st, d1, d2);
}
CORE_COLD void live_release(int vi, int ch, int midi_note) {   // msamp's live notes (the pool's: release() by slot)
#if CORE_MSAMP
  if (vi >= 0 && is_ms(vi)) nm::live_off(vi, ch, midi_note);
#else
  (void)vi, (void)ch, (void)midi_note;
#endif
}
CORE_COLD void trig_notes_off(int vi) {   // the voice's own trig notes (mono note and chord), for a live pad's takeover
#if CORE_MSAMP
  if (is_ms(vi)) {
    nm::note_off(vi);
    return;
  }
#endif
  fms[vi].NoteOff();
}
CORE_COLD void release(Slot& s) {
  if (!s.held) return;
  s.held = false;
  s.released_ms = now_ms();
  s.fm.NoteOff();
}
// The track for a write-back: the track on voice vi whose machine the Rytm's 33 says is 6-OP FM or MULTISAMPLE, else
// its first.
CORE_COLD int track_of(int vi) {
  int first = -1;
  for (int t = 0; t < 12; ++t) {
    if (ui::remote_track_voice(t) != vi) continue;
    if (first < 0) first = t;
    int vb = ui::remote_track_machine(t);
    if (vb >= 0 && vb != 0x7F && ((vb & 0x78) == kFm || (vb & 0x78) == kMultiMachine)) return t;
  }
  return first;
}
CORE_COLD bool chan_active(const Chan& c, uint32_t ms) {
  return c.vi >= 0 && ((c.held[0] | c.held[1] | c.held[2] | c.held[3]) || ms - c.last_ms < kActiveMs);
}
// The pads are down, or were a moment ago: the chord view keeps their chord against a late 0x04 of theirs
CORE_COLD bool pads_down(const Chan& c, uint32_t ms) {
  return (c.held[0] | c.held[1] | c.held[2] | c.held[3]) || ms - c.last_ms < 400;
}
CORE_COLD int chan_of_voice(int vi) {
  for (int c = 0; c < 16; ++c)
    if (chans[c].vi == vi) return c;
  return -1;
}
CORE_COLD void bind(int ch, int vi, int off) {
  for (int c = 0; c < 16; ++c)
    if (c != ch && chans[c].vi == vi) chans[c].vi = -1;   // one channel a voice
  Chan& c = chans[ch];
  bool was = c.vi == vi && c.off == off;
  c.vi = (int8_t)vi, c.off = (int8_t)off;
  st.bound[ch] = vi;
  if (was) return;
  log(CORE_STR("poly: MIDI channel %d plays voice %d live (note offset %d)"), ch + 1, vi, off);
  // the notes held on the channel now: from the pool (or MULTISAMPLE's live notes), the mono note the Rytm's 0x04s
  // left off
  trig_notes_off(vi);
  for (int n = 0; n < 128; ++n)
    if (c.held[n >> 5] >> (n & 31) & 1) {
      int note = n + off;
      if (note >= 0 && note <= 127) live_start(vi, note, 100, ch, n);
    }
  pend[vi].on = false;
}
// A note-on (ch, n) and a trig (vi, note) close in time: a vote for binding ch -> vi at that offset
CORE_COLD void vote(int ch, int vi, int off, uint32_t ms) {
  int k = -1, free_k = -1, old_k = 0;
  for (int i = 0; i < 8; ++i) {
    Vote& x = votes[i];
    if (x.count && ms - x.ms > 3000) x.count = 0;
    if (x.count && x.ch == ch && x.vi == vi && x.off == off) k = i;
    if (!x.count && free_k < 0) free_k = i;
    if ((int32_t)(x.ms - votes[old_k].ms) < 0) old_k = i;
  }
  if (k < 0) {
    k = free_k >= 0 ? free_k : old_k;
    votes[k] = {(int8_t)ch, (int8_t)vi, (int8_t)off, 0, ms};
  }
  ++votes[k].count;
  votes[k].ms = ms;
  if (votes[k].count >= 2 && !(chans[ch].vi == vi && chans[ch].off == off)) bind(ch, vi, off);
}
CORE_COLD void play_trig(int vi, int note, int vel, const float* p);
// The chord the channel's pads make: every note held now plus the window's (+ the binding's offset), one set - a pad
// that came after the 60 ms window closed is part of the chord while it is held (not a one-note chord of its own).
CORE_COLD int chan_chord(const Chan& c, uint8_t* ns) {
  uint32_t bits[4] = {c.held[0], c.held[1], c.held[2], c.held[3]};
  for (int i = 0; i < c.n; ++i) bits[c.notes[i] >> 5 & 3] |= 1u << (c.notes[i] & 31);
  int k = 0;
  for (int n = 0; n < 128 && k < 16; ++n)
    if (bits[n >> 5] >> (n & 31) & 1) {
      int x = n + c.off;
      ns[k++] = (uint8_t)(x < 0 ? 0 : x > 127 ? 127 : x);
    }
  return k;
}
CORE_COLD Detected chan_view(int ch, int* cnt) {
  uint8_t ns[16];
  *cnt = chan_chord(chans[ch], ns);
  Detected d = detect(ns, *cnt);
  views[chans[ch].vi] = {d.root, d.c, true};   // the chord view: what the pads play
  return d;
}
CORE_COLD void close_window(int ch) {
  Chan& c = chans[ch];
  c.open = false;
  if (c.n < 1 || c.vi < 0 || c.chordkeys) return;   // (chord keys: the voice's own CHD chord, no detection)
  int cnt;
  Detected d = chan_view(ch, &cnt);
  c.n = 0;   // (the window's notes are in the held set while the pads are down)
  if (cnt < 2) return;
  int t = track_of(c.vi);
  if (t < 0) return;
  Chord w = d.c;   // VOI written as the count (AUTO plays the same notes when it is the chord's own)
  uint8_t back[kMaxNotes];
  if (w.chd) w.voi = expand(d.root, d.c, back);
  uint8_t m[16];
  int len = sysex_chord_write(m, t, d.root, w);
  to_rytm(m, len);
  ++st.writes;
  char b[8];
  log(CORE_STR("poly: live chord on voice %d (track %d): root %d %s INV%d SPR%d VOI %s, %d notes%s -> 28 write-back"),
      c.vi, t, d.root, kChordNames[w.chd], w.inv, w.spr, value_text(3, w.voi, b, sizeof b), cnt,
      d.exact ? "" : " (nearest)");
}
}  // namespace

CORE_COLD void init() {
  if (!slots) {
    slots = new Slot[kTotal];
    // Plaits' scratch per note, 16 KB as the voices' (heap: off the Pi's .bss)
    char* mem = new char[kTotal * 16384 + 64];
    char* a = (char*)(((uintptr_t)mem + 63) & ~(uintptr_t)63);
    for (int i = 0; i < kTotal; ++i) {
      Slot& s = slots[i];
      s.scratch = a + i * 16384;
      stmlib::BufferAllocator allocator(s.scratch, 16384);
      s.voice.Init(&allocator);
    }
  }
  for (int i = 0; i < kTotal; ++i) {
    Slot& s = slots[i];
    memset(&s.patch, 0, sizeof s.patch);
    memset(&s.mod, 0, sizeof s.mod);
    s.mod.trigger_patched = true;
    s.fm.Init();
    s.owner = -1;
    s.held = s.live = s.used = false;
  }
  reset();
}

CORE_COLD void selected_voice(int vi) {
  for (int c = 0; c < 16; ++c) {
    Chan& ch = chans[c];
    if (ch.vi < 0 || ch.direct || ch.vi == vi) continue;
    if (slots)
      for (int i = 0; i < kTotal; ++i)
        if (slots[i].owner >= 0 && slots[i].live && slots[i].ch == c) release(slots[i]);
    live_release(ch.vi, c, -1);
    log(CORE_STR("poly: MIDI channel %d unbound (another track selected)"), c + 1);
    ch.vi = -1, st.bound[c] = -1;
  }
  memset(votes, 0, sizeof votes);
}
CORE_COLD void reset() {
  trk_held = false;
  memset(trk_sw, 0, sizeof trk_sw);
  if (slots)
    for (int i = 0; i < kTotal; ++i) slots[i].owner = -1, slots[i].held = false;
  memset(&st, 0, sizeof st);
  for (int c = 0; c < 16; ++c) {
    memset(&chans[c], 0, sizeof chans[c]);
    chans[c].vi = -1;
    chans[c].cur = chans[c].prev = -1;
    st.bound[c] = -1;
  }
  memset(ons, 0, sizeof ons);
  memset(trigs, 0, sizeof trigs);
  memset(votes, 0, sizeof votes);
  memset(pend, 0, sizeof pend);
  for (int vi = 0; vi < kVoices; ++vi) {
    for (int k = 0; k < 8; ++k) vknobs[vi][k] = k == 3 || k == 7 ? 127.0f : k == 6 ? 100.0f : 64.0f;
    views[vi] = {60, {0, 0, 0, 0}, false};
    head_g[vi] = 1.0f;
  }
}

CORE_COLD void set_voice(int vi, const uint8_t* dx7) {
  vi &= 7;
  vhas[vi] = dx7 != nullptr;
  if (dx7) memcpy(vbytes[vi], dx7, 128);
}

CORE_COLD void knobs(int vi, const float* p) {
  vi &= 7;
  for (int k = 0; k < 8; ++k) vknobs[vi][k] = p[k];
  if (!slots) return;
  for (int i = 0; i < kTotal; ++i)
    if (slots[i].owner == vi) slots[i].fm.SetKnobs(p);
}

namespace {
CORE_COLD void play_trig(int vi, int note, int vel, const float* p) {
  Chord c = chord_of(p);
  if (c.chd == 0) {   // OFF: the mono note, as without the pool
    fm_note_on(vi, note, vel);
    return;
  }
  for (int i = 0; i < kTotal; ++i)   // the voice's earlier chord: released, its tails go on
    if (slots[i].owner == vi && !slots[i].live) release(slots[i]);
  fms[vi].NoteOff();
  uint8_t ns[kMaxNotes];
  int n = expand(note, c, ns);
  for (int k = 0; k < n; ++k) start(vi, ns[k], vel, 1.0f, false, -1, 0);   // (1/sqrt(n): render_add's headroom)
  ++st.chords;
  char b[8];
  log(CORE_STR("trig voice %d 6-OP FM entry %d chord %s INV%d SPR%d VOI %s root %d: %d notes, %d/%d in the pool (peak %d, steals %lu)"),
      vi, fm_key[vi] & 0xff, kChordNames[c.chd], c.inv, c.spr, value_text(3, c.voi, b, sizeof b), note, n, st.active,
      kTotal, st.peak, (unsigned long)st.steals);
}
}  // namespace

CORE_COLD bool take_live(int vi, int note, int vel) {
  vi &= 7;
  if (!slots) return false;
  if (keys::take_trig(vi, note)) return true;   // the Rytm's trig for a keyboard note the Pi forwarded (core/keys.h)
  uint32_t ms = now_ms();
  int ch = chan_of_voice(vi);
  if (ch >= 0 && chan_active(chans[ch], ms)) {
    Chan& c = chans[ch];
    for (int i = 0; i < 8; ++i)   // the pad's note-on came first: already playing
      if (!ons[i].used && ons[i].ch == ch && ms - ons[i].ms <= kWindowMs && ons[i].note + c.off == note) {
        ons[i].used = true;
        ++st.swallowed;
        return true;
      }
    pend[vi] = {true, (uint8_t)note, (uint8_t)vel, ms};   // its note-on may still come
    ++st.held;
    return true;
  }
  // not bound (or idle): a vote with each recent note-on
  trigs[trigs_w] = {(uint8_t)vi, (uint8_t)note, ms, false};
  trigs_w = (trigs_w + 1) & 7;
  for (int i = 0; i < 8; ++i)
    if (ons[i].ms && ms - ons[i].ms <= kBindMs) vote(ons[i].ch, vi, note - ons[i].note, ms);
  ch = chan_of_voice(vi);
  if (ch >= 0 && chan_active(chans[ch], ms)) {   // bound by this trig: the pool plays its held notes
    ++st.swallowed;
    return true;
  }
  views[vi].root = note, views[vi].live = false;   // the Rytm's own trig: the chord view's root
  return false;
}

CORE_COLD bool take_trig(int vi, int note, int vel, const float* p) {
  vi &= 7;
  if (!slots) return false;
  if (take_live(vi, note, vel)) return true;
  if (chord_of(p).chd == 0) return false;   // the mono note
  play_trig(vi, note, vel, p);
  return true;
}

CORE_COLD View view(int vi) { return views[vi & 7]; }

CORE_COLD int voice_notes(int vi) {
  if (is_ms(vi)) {
#if CORE_MSAMP
    return nm::voice_notes(vi);
#endif
  }
  return slots ? count_voice(vi & 7) : 0;
}

CORE_COLD void note_off(int vi) {
  vi &= 7;
  if (!slots) return;
  for (int i = 0; i < kTotal; ++i)
    if (slots[i].owner == vi && !slots[i].live) release(slots[i]);
}

CORE_COLD void silence(int vi) {
  vi &= 7;
  if (!slots) return;
  for (int i = 0; i < kTotal; ++i)
    if (slots[i].owner == vi) slots[i].owner = -1, slots[i].held = false;
  pend[vi].on = false;
  st.active = count_all();
}

CORE_COLD bool active(int vi) {
  if (!slots) return false;
  for (int i = 0; i < kTotal; ++i)
    if (slots[i].owner == vi) return true;
  return false;
}

void render_add(int vi, float* out, int n, uint8_t* stage) {
  if (!slots) return;
  // the headroom gain's smoothing per block (one pole, down fast, up slow)
  const float atk = 1.0f - expf(-1000.0f * kBlock / (48000.0f * kHeadAtkMs));
  const float rel = 1.0f - expf(-1000.0f * kBlock / (48000.0f * kHeadRelMs));
  float peak[kTotal] = {0};
  for (int done = 0; done < n; done += kBlock) {
    int size = n - done < kBlock ? n - done : kBlock;
    float acc[kBlock] = {0}, o[kBlock];
    float e = 0;   // the notes' weights
    bool any = false;
    for (int i = 0; i < kTotal; ++i) {
      Slot& s = slots[i];
      if (s.owner != vi) continue;
      any = true;
      s.fm.Render(&s.voice, &s.patch, &s.mod, stage, o, size);
      float pb = 0;
      for (int k = 0; k < size; ++k) {
        float y = o[k] * s.gain;
        acc[k] += y;
        float a = y < 0 ? -y : y;
        if (a > pb) pb = a;
      }
      if (pb > peak[i]) peak[i] = pb;
      if (s.held) {
        if (pb > s.ref) s.ref = pb;
        e += 1.0f;
      } else {
        e += s.ref > 0 ? (pb < s.ref ? pb / s.ref : 1.0f) : 0.0f;
      }
    }
    float g0 = head_g[vi];
    float target = e > 1.0f ? 1.0f / sqrtf(e) : 1.0f;
    float g1 = g0 + (target - g0) * (target < g0 ? atk : rel);
  if (fabsf(g1 - target) < 1e-5f) g1 = target;   // (settled: exactly)
    head_g[vi] = g1;
    if (!any) continue;
    float dg = (g1 - g0) / (float)size;   // a ramp over the block: no step
    for (int k = 0; k < size; ++k) out[done + k] += acc[k] * (g0 + dg * (float)(k + 1));
  }
  for (int i = 0; i < kTotal; ++i) {
    Slot& s = slots[i];
    if (s.owner != vi || s.held) continue;
    s.quiet = peak[i] < 1e-4f ? s.quiet + 1 : 0;
    if (s.quiet >= 40) s.owner = -1;   // released and quiet for 10 ms: free
  }
}

CORE_COLD float headroom(int vi) { return head_g[vi & 7]; }
CORE_COLD int held_notes(int vi) {
  int k = 0;
  for (int i = 0; slots && i < kTotal; ++i) k += slots[i].owner == (vi & 7) && slots[i].held;
  return k;
}
CORE_COLD void set_note_sink(void (*f)(uint8_t, uint8_t, uint8_t)) { sink = f; }

namespace {
CORE_COLD bool silent(const Chan& c) { return !(c.held[0] | c.held[1] | c.held[2] | c.held[3]); }
// Note n of channel ch released: its pool notes / MULTISAMPLE live notes, the sink's note-off
CORE_COLD void release_note(Chan& c, int ch, int n) {
  uint32_t m = ~(1u << (n & 31));
  c.held[n >> 5] &= m, c.kept[n >> 5] &= m;
  if (c.cur == n) c.cur = -1;
  for (int i = 0; i < kTotal; ++i)
    if (slots[i].owner >= 0 && slots[i].live && slots[i].ch == ch && slots[i].midi_note == n) release(slots[i]);
  live_release(c.vi, ch, n);
  emit((uint8_t)(0x80 | ch), (uint8_t)n, 0);
}
CORE_COLD void drop_window_note(Chan& c, int n) {
  int k = 0;
  for (int i = 0; i < c.n; ++i)
    if (c.notes[i] != n) c.notes[k++] = c.notes[i];
  c.n = (uint8_t)k;
}
}  // namespace

namespace {
// ---- fix1040b: a seq track's pad in the normal pad mode (bug 5's pad-trigger gate kept it from the binding, and the
// voice's 0x04 is dropped on a seq track): its 0x04 and its trigger note-on meet within kBindMs, either order
struct SeqPad {
  bool on;
  uint8_t note, vel;
  uint32_t ms;
};
SeqPad spad[kVoices];                 // a 0x04 on a seq track's voice waiting for its pad
uint32_t pad_on_ms[12];               // each track's pad trigger (its note-on)
bool pad_on[12];                      // ... not yet matched with a 0x04
bool pad_played[12];                  // that pad started its voice's note (its release ends it)
CORE_COLD void pad_play(int vi, int note, int vel) {
  if (v[vi].fm) play_trig(vi, note, vel, v[vi].p);
#if CORE_MSAMP
  else if (is_ms(vi)) nm::report(vi, note, vel, v[vi].p);   // MULTISAMPLE / WAVETABLE / GRANULAR (core/nmach.h)
#endif
  else voice_play(vi, note, vel);   // E0: any other machine on a seq track: its own machine, as its trig
}
CORE_COLD void pad_trigger(int t, bool on, uint32_t ms) {
  if (t < 0 || t >= 12) return;
  int vi = ui::remote_track_voice(t);
  if (!on) {
    if (pad_played[t] && vi >= 0) {
      pad_played[t] = false;
      trig_notes_off(vi);
      note_off(vi);
      if (!v[vi & 7].fm && !is_ms(vi & 7)) voice_note_off(vi & 7);   // E0: the other machines' pad note (voice_play)
    }
    pad_on[t] = false;
    return;
  }
  pad_on_ms[t] = ms, pad_on[t] = true;
  pad_played[t] = false;
  if (vi < 0 || !seq::owns_track(t)) return;
  SeqPad& q = spad[vi & 7];
  if (q.on && ms - q.ms <= kBindMs) {   // its 0x04 came first
    q.on = false, pad_on[t] = false;
    pad_played[t] = true;
    ++st.seq_pads;
    pad_play(vi & 7, q.note, q.vel);
  }
}
}  // namespace

CORE_COLD bool seq_pad(int vi, int note, int vel) {
  vi &= 7;
  uint32_t ms = now_ms();
  for (int t = 0; t < 12; ++t)
    if (pad_on[t] && ms - pad_on_ms[t] <= kBindMs && ui::remote_track_voice(t) == vi && seq::owns_track(t)) {
      pad_on[t] = false;
      pad_played[t] = true;
      ++st.seq_pads;
      return true;
    }
  spad[vi] = {true, (uint8_t)note, (uint8_t)vel, ms};
  return false;
}

CORE_COLD bool pad_sounding(int vi) {
  for (int t = 0; t < 12; ++t)
    if (pad_played[t] && ui::remote_track_voice(t) == (vi & 7)) return true;
  return false;
}

CORE_COLD void rytm_trk(bool held) {
  trk_held = held;
  trk_ms = now_ms();
}

CORE_COLD void midi(uint8_t status, uint8_t d1, uint8_t d2) {
  int kind = status & 0xF0, ch = status & 0x0F;
  // 1042: TRK held + a pad on the Rytm selects the pad's track; with the remote page open TRK is the page's (stock gets
  // its press only with the pad: rytm-mods 0026 1040), and stock sends the pad's note on the auto channel (in CHRO the
  // old track's chromatic note) before the selection report (49 02, a UI tick later), so it played once on the old
  // track's bound voice [measured, rytm-emu rui1041 + the real core: seq_e2e.py padsel]. While TRK is held such a note
  // is the selection's: no voice, no binding vote, no step entry or recording; its note-off too. (With the page closed
  // stock has TRK and sends no note for TRK + pad [measured].)
  if ((kind == 0x90 || kind == 0x80) && ch == keys::auto_channel() - 1 && !chans[ch].direct) {
    int n = d1 & 0x7F;
    if (kind == 0x90 && d2 && trk_held && now_ms() - trk_ms <= kTrkMaxMs) {
      trk_sw[n >> 5] |= 1u << (n & 31);
      ++st.trk_pads;
      return;
    }
    if (trk_sw[n >> 5] >> (n & 31) & 1) {   // the swallowed note's note-off
      trk_sw[n >> 5] &= ~(1u << (n & 31));
      return;
    }
  }
  if (!slots) {
    if (kind == 0x90 || kind == 0x80) emit(status, d1, d2);
    return;
  }
  Chan& c = chans[ch];
  if (kind != 0x90 && kind != 0x80) {
    if (kind == 0xB0 && (d1 == 120 || d1 == 123)) {   // all sound / notes off: the channel's held notes released
      memset(c.held, 0, sizeof c.held);
      memset(c.kept, 0, sizeof c.kept);
      c.cur = c.prev = -1, c.since = 0;
      for (int i = 0; i < kTotal; ++i)
        if (slots[i].owner >= 0 && slots[i].live && slots[i].ch == ch) release(slots[i]);
      live_release(c.vi, ch, -1);
    }
    return;
  }
  uint32_t ms = now_ms();
  int n = d1 & 0x7F;
  // rui1040 (hardware report on rui1039, the user's rule: chromatic only in the Rytm's CHRO mode): in the normal pad mode
  // a pad sends note t (0..11, its track) on the auto channel - a trigger of track t, not a pitch - and only CHRO sends
  // the selected track's chromatic notes (12..) there [measured, rytm-emu rui1040: BD / SD / BT pads -> ch 14 notes 0 /
  // 1 / 4; rytm-mods re/subsystems/midi-cc.md, the auto channel's map]. Those triggers never bind the channel to a
  // Sidecar voice, never play one and never reach the sequencer's step entry / recording (sink): before, a channel bound
  // while the seq track was played kept every pad on the 6-OP FM voice after another track was selected.
  if (!c.direct && ch == keys::auto_channel() - 1 && n < 12) {
    pad_trigger(n, kind == 0x90 && d2 != 0, ms);   // fix1040b: a seq track's own pad still plays its voice
    return;
  }
  if (c.vi >= 0 && !c.direct && !plays_live(c.vi)) c.vi = -1, st.bound[ch] = -1;   // the voice left FM / MULTISAMPLE
  if (kind == 0x90 && d2) {
    // the Rytm's mono pads (poly.h kStealMs): the gesture, the note this one displaces
    if (silent(c) || ms - c.last_ms > kGestureMs) c.g0 = ms, c.since = 0;
    c.prev = -1;
    if (!c.direct && c.cur >= 0 && c.cur != n && ms - c.on_ms[c.cur] <= kGestureMs) c.prev = c.cur, c.prev_ms = ms;
    if (bit(c.held, n)) release_note(c, ch, n);   // a pad of the chord again: retriggered
    c.cur = (int16_t)n;
    c.on_ms[n] = ms;
    if (c.since < 255) ++c.since;
    emit(status, d1, d2);
    c.held[n >> 5] |= 1u << (n & 31);
    c.last_ms = ms;
    ons[ons_w] = {(uint8_t)ch, (uint8_t)n, ms, false};
    Recent& me = ons[ons_w];
    ons_w = (ons_w + 1) & 7;
    if (!c.open) c.open = true, c.t0 = ms, c.n = 0;
    if (c.n < kMaxNotes) c.notes[c.n++] = (uint8_t)n;   // (the window closes kGestureMs after its last note-on)
    if (c.vi < 0) {   // unbound: a vote with each recent trig
      for (int i = 0; i < 8; ++i)
        if (trigs[i].ms && !trigs[i].used && ms - trigs[i].ms <= kBindMs) vote(ch, trigs[i].ch, trigs[i].note - n, ms);
      if (c.vi < 0) return;
      me.used = true;   // bound now: bind() played the held notes, this one too
      return;
    }
    int note = n + c.off;
    if (note < 0 || note > 127) return;
    int cnt;
    if (!c.chordkeys) chan_view(ch, &cnt);   // the view follows the pads while they are held
    for (int i = 0; i < 8; ++i)   // the channel was idle and the Rytm's 0x04 for this pad already played: cut to the pool
      if (!trigs[i].used && trigs[i].ms && trigs[i].ch == c.vi && trigs[i].note == note && ms - trigs[i].ms <= kBindMs) {
        trigs[i].used = true;
        trig_notes_off(c.vi);
        note_off(c.vi);
        ++st.swallowed;
      }
    Pending& pd = pend[c.vi];
    if (pd.on && pd.note == note) {   // the Rytm's 0x04 for it came first: swallowed
      pd.on = false;
      me.used = true;
      ++st.swallowed;
    }
    if (c.chordkeys && v[c.vi].fm) {   // direct keys with a chord: this key is the chord's root
      Chord cc = chord_of(v[c.vi].p);
      if (cc.chd) {
        uint8_t ns[kMaxNotes];
        int k = expand(note, cc, ns);
        for (int i = 0; i < k; ++i) start(c.vi, ns[i], d2, 0.6f, true, ch, n);
        return;
      }
    }
    live_start(c.vi, note, d2, ch, n);
    return;
  }
  // a note-off: the Rytm's mono steal (kept, not passed on), or a real one (n and the kept notes of its gesture)
  if (n == c.prev && n != c.cur && ms - c.prev_ms <= kStealMs && bit(c.held, n)) {
    c.kept[n >> 5] |= 1u << (n & 31);
    c.prev = -1;
    ++st.mono_steals;
    return;
  }
  if (n == c.prev) c.prev = -1;
  bool cur_gesture = (int32_t)(c.on_ms[n] - c.g0) >= 0;
  release_note(c, ch, n);
  for (int k = 0; k < 128; ++k)
    if (bit(c.kept, k) && (cur_gesture || (int32_t)(c.on_ms[k] - c.g0) < 0)) release_note(c, ch, k);
  if (silent(c)) {
    if (c.since == 1 && c.open) drop_window_note(c, n);   // a lone note (a melody's): not a chord's
    c.since = 0;
  }
}

CORE_COLD void tick(uint32_t ms) {
  keys::tick(ms);   // the Pi's own keyboards (core/keys.h): their chord windows, the learned selected track
  if (!slots) return;
  for (int vi = 0; vi < kVoices; ++vi) {
    Pending& pd = pend[vi];
    if (pd.on && ms - pd.ms > kHoldMs) {   // no note-on matched it: the Rytm's own trig (its sequencer)
      pd.on = false;
      int pc = chan_of_voice(vi);   // (a pad's own 0x04 that came late is not the sequencer's: the pads' chord stays)
      if (pc < 0 || !pads_down(chans[pc], ms)) views[vi].root = pd.note, views[vi].live = false;
      if (seq::owns_voice(vi)) continue;   // a Sidecar-sequenced track's own trig makes no note (core/seq/seq.h)
      if (v[vi].fm) play_trig(vi, pd.note, pd.vel, v[vi].p);
#if CORE_MSAMP
      else if (is_ms(vi)) nm::report(vi, pd.note, pd.vel, v[vi].p);
#endif
    }
  }
  for (int ch = 0; ch < 16; ++ch)
    if (chans[ch].open && (ms - chans[ch].last_ms > kGestureMs || ms - chans[ch].t0 > 2000)) close_window(ch);
  for (int i = 0; i < kTotal; ++i) {   // a released note that never went quiet (a drone patch): freed after 10 s
    Slot& s = slots[i];
    if (s.owner >= 0 && !s.held && ms - s.released_ms > 10000) s.owner = -1;
  }
  st.active = count_all();
  static uint32_t lim_seen[kVoices], lim_ms;   // the soft limiter at work: logged, at most every 2 s
  if (ms - lim_ms >= 2000) {
    lim_ms = ms;
    for (int vi = 0; vi < kVoices; ++vi) {
      uint32_t l = voice_limited(vi);
      if (l != lim_seen[vi])
        log(CORE_STR("poly: voice %d soft-limited %lu blocks (%lu in all): a chord over full scale"), vi,
            (unsigned long)(l - lim_seen[vi]), (unsigned long)l);
      lim_seen[vi] = l;
    }
  }
}

CORE_COLD bool reply(const uint8_t* d, int len) {
  if (len < 10 || d[4] != kChordCmd || d[5] != kChordReply) return false;
  static const char* const kStatus[] = {"ok", "no recent step", "lock slots full", "not recording / not a Sidecar track"};
  int t = d[6], step = d[7], status = d[8];
  if (status == 0) ++st.replies_ok;
  else ++st.replies_err;
  if (step == 0x7F)
    log(CORE_STR("poly: chord write-back track %d: %s"), t, status < 4 ? kStatus[status] : "status ?");
  else
    log(CORE_STR("poly: chord write-back track %d step %d: %s"), t, step + 1, status < 4 ? kStatus[status] : "status ?");
  return true;
}

CORE_COLD bool seq_on(int vi, int note, int vel) {
  vi &= 7;
  if (!slots || note < 0 || note > 127) return false;
#if CORE_MSAMP
  if (is_ms(vi)) {
    nm::live_on(vi, note, vel, kSeqCh, note);
    ++st.notes;
    return true;
  }
#endif
  if (!v[vi].fm) return false;
  start(vi, note, vel, 0.5f, true, kSeqCh, note);
  return true;
}

CORE_COLD void seq_off(int vi, int note) {
  vi &= 7;
  if (!slots) return;
  for (int i = 0; i < kTotal; ++i) {
    Slot& s = slots[i];
    if (s.owner == vi && s.live && s.ch == kSeqCh && (note < 0 || s.midi_note == note)) release(s);
  }
#if CORE_MSAMP
  if (is_ms(vi)) nm::live_off(vi, kSeqCh, note);
#endif
}

CORE_COLD bool plays_poly(int vi) { return slots && vi >= 0 && vi < kVoices && plays_live(vi); }

CORE_COLD bool key_on(int vi, int note, int vel, int kc) {
  if (!plays_poly(vi) || note < 0 || note > 127) return false;
  live_start(vi, note, vel < 1 ? 1 : vel, kc, note);
  return true;
}

CORE_COLD void key_off(int vi, int note, int kc) {
  if (!slots || vi < 0 || vi >= kVoices) return;
  for (int i = 0; i < kTotal; ++i) {
    Slot& s = slots[i];
    if (s.owner == vi && s.live && s.ch == kc && (note < 0 || s.midi_note == note)) release(s);
  }
  live_release(vi, kc, note);
}

CORE_COLD Detected keys_view(int vi, const uint8_t* notes, int n) {
  Detected d = detect(notes, n);
  if (vi >= 0 && vi < kVoices) views[vi] = {d.root, d.c, true};
  return d;
}

CORE_COLD int bound_voice(int ch, int* off) {
  if (ch < 0 || ch > 15) return -1;
  if (off) *off = chans[ch].off;
  return chans[ch].vi;
}

const Stats& stats() { return st; }

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (!strncmp(p, "poly=bind:", 10)) {   // poly=bind:CH:V[:chord] (CH 1..16, V 0..7) a host keyboard's channel plays voice V; poly=bind:CH:off
    int ch = atoi(p + 10) - 1;
    const char* q = strchr(p + 10, ':');
    if (ch < 0 || ch > 15 || !q) return sniprintf(out, cap, CORE_STR("poly=bind:CH:V|off")), true;
    if (!strcmp(q + 1, "off")) {
      chans[ch].direct = chans[ch].chordkeys = false, chans[ch].vi = -1, st.bound[ch] = -1;
      return sniprintf(out, cap, CORE_STR("poly: channel %d unbound"), ch + 1), true;
    }
    int vi = atoi(q + 1) & 7;
    bind(ch, vi, 0);
    chans[ch].direct = true;
    chans[ch].chordkeys = strstr(q + 1, ":chord") != nullptr;
    return sniprintf(out, cap, CORE_STR("poly: channel %d plays voice %d%s"), ch + 1, vi, chans[ch].chordkeys ? " (a key plays the chord)" : ""), true;
  }
  if (strcmp(p, "poly") != 0) return false;
  int n = sniprintf(out, cap, CORE_STR("poly: %d/%d notes now (peak %d, %d a voice), %lu notes, %lu chords, %lu live, %lu steals, "
                                       "%lu swallowed, %lu held, %lu mono steals, %lu write-backs (%lu ok, %lu refused), %lu seq pads; bound"),
                    st.active, kTotal, st.peak, kPerVoice, (unsigned long)st.notes, (unsigned long)st.chords,
                    (unsigned long)st.live_notes, (unsigned long)st.steals, (unsigned long)st.swallowed,
                    (unsigned long)st.held, (unsigned long)st.mono_steals, (unsigned long)st.writes,
                    (unsigned long)st.replies_ok, (unsigned long)st.replies_err, (unsigned long)st.seq_pads);
  bool any = false;
  for (int c = 0; c < 16 && n < cap; ++c)
    if (chans[c].vi >= 0) n += sniprintf(out + n, cap - n, " ch%d->v%d", c + 1, chans[c].vi), any = true;
  if (!any && n < cap) n += sniprintf(out + n, cap - n, " none");
  any = false;   // the soft limiter's blocks (voices.h kSoftKnee), the voices it touched
  for (int vi = 0; vi < kVoices && n < cap; ++vi)
    if (voice_limited(vi))
      n += sniprintf(out + n, cap - n, "%s v%d:%lu", any ? "" : "; limited", vi, (unsigned long)voice_limited(vi)), any = true;
  return true;
}
#endif

}  // namespace poly
}  // namespace core
