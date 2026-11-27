// core/arp/arp.cpp - see arp.h. Per note, per message, one pass of compares per block while a voice runs: cold.
#include "arp/arp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CORE_POLY
#include "core.h"
#include "hooks.h"
#include "inst.h"
#include "machines.h"
#if CORE_MSAMP
#include "nmach.h"
#endif
#include "platform.h"
#include "poly.h"
#include "ui/remote.h"
#include "voices.h"

namespace core {
namespace arp {
namespace {

constexpr int kDefSpeed = 3, kDefGate = 1;      // the fields' stored 0: 1/16, 50 %
constexpr double kInterval[kSpeeds] = {4.0, 2.0, 4.0 / 3.0, 1.0, 2.0 / 3.0, 0.5};   // 1/16ths
constexpr double kSwingFrac[kSwings] = {0.0, 0.16, 0.32};                            // swing / 50 - 1 of an interval, seq.h's rule
constexpr int kGatePct[kGates] = {25, 50, 100};
constexpr int kSwingPct[kSwings] = {50, 58, 66};
constexpr int kMinGate = 96;                    // frames: 2 ms
constexpr int kCatchMax = 384;                  // frames after a grid point that still count as on it (8 ms, plus the latency)

struct Held {
  uint8_t note, vel, src, key;
};
struct Vo {
  Held h[kHeld];
  int n;
  bool run;              // in `running`: holding, or its last note playing out
  bool started;          // its grid placed
  bool clocked;          // on the Rytm's clock (else free)
  uint8_t speed;         // the speed it was placed at
  int64_t next;          // the grid point due
  double free_o;         // free running: grid point 0
  int pos;               // notes played since the hold began
  double last_f;         // the grid point played last
  double o_seen;         // the clock's origin at the last look
  uint32_t trig_ms;      // the last trig's time: a kit change does not end a trig this fresh
  int note;              // the note sounding, -1 none
  int64_t off_at;
  bool seq_cfg;          // the notes came from the sequencer: its poly config
  seq::PolyCfg pcfg;
  uint16_t sig;          // the machines on the voice's tracks at the last look (machines())
  uint32_t plain[4];     // pitches the sequencer played past the arp under a lock of MODE OFF: the first off of that pitch goes to the machine (plain_off)
};
struct Lk {
  uint8_t mask, val[seq::kArpLocks];
  int8_t track;
  int64_t until;
};
static_assert(seq::kArpLockMax[0] == kModes - 1 && seq::kArpLockMax[1] == kSpeeds - 1 && seq::kArpLockMax[2] == kRanges - 1 &&
              seq::kArpLockMax[3] == kGates - 1 && seq::kArpLockMax[4] == kSwings - 1, "seq.h's arp lock ranges");
Vo vo[kVoices];
Lk lk[kVoices];
uint16_t code[12];
uint8_t running = 0;   // bit vi
Stats st;
uint32_t rnd = 0x2545F491u;
uint32_t check_ms = 0;
uint16_t pins_sent = 0xFFFF;

CORE_COLD int rand_below(int n) {
  rnd ^= rnd << 13, rnd ^= rnd >> 17, rnd ^= rnd << 5;
  return (int)((rnd >> 8) % (uint32_t)n);
}
CORE_COLD bool sidecar_track(int t) {
  int vb = ui::remote_track_machine(t);
  return vb != 0x7F;   // (-1: not reported yet, a test's or a first report's: the voice's reports say it is ours)
}
CORE_COLD bool code_on(int c) { return c < kCodes && c % kModes != kOff; }   // (past the table: decode's 0, OFF)
CORE_COLD int code_of(int vi) {
  for (int t = 0; t < 12; ++t)
    if (ui::remote_track_voice(t) == vi && code_on(code[t]) && sidecar_track(t)) return code[t];
  return 0;
}
// The voice's settings at frame h: the code, a lock laid over it. A lock past its step is gone here
CORE_COLD Cfg cfg_at(int vi, int64_t h) {
  Lk& l = lk[vi];
  if (l.mask && h >= l.until) l.mask = 0;
  int cc = code_of(vi);
  if (!l.mask) return decode(cc);
  Cfg c = decode(cc ? cc : code[l.track]);   // (the sound's OFF: its other values still its own)
  if (l.mask & 1) c.mode = l.val[0];
  if (l.mask & 2) c.speed = l.val[1];
  if (l.mask & 4) c.range = l.val[2];
  if (l.mask & 8) c.gate = l.val[3];
  if (l.mask & 16) c.swing = l.val[4];
  return c;
}
CORE_COLD void wake(int vi) {
  if (!vo[vi].run) {
    vo[vi].run = true;
    vo[vi].started = false;
    vo[vi].pos = 0;
    running |= (uint8_t)(1u << vi);
    ++st.running;
    ++st.chords;
  }
}
CORE_COLD void sleep(int vi) {
  if (vo[vi].run) {
    vo[vi].run = false;
    running &= (uint8_t)~(1u << vi);
    --st.running;
  }
}
CORE_COLD void add(int vi, int note, int vel, int src, int key) {
  Vo& x = vo[vi];
  for (int i = 0; i < x.n; ++i)
    if (x.h[i].src == src && x.h[i].key == key && x.h[i].note == note) {
      x.h[i].vel = (uint8_t)vel;
      wake(vi);
      return;
    }
  if (x.n == kHeld) memmove(x.h, x.h + 1, sizeof(Held) * (size_t)--x.n);
  x.h[x.n++] = {(uint8_t)note, (uint8_t)vel, (uint8_t)src, (uint8_t)key};
  wake(vi);
}
// Held notes matching are gone; how many
CORE_COLD int drop(int vi, int src, int key, int note) {
  Vo& x = vo[vi];
  int k = 0, gone = 0;
  for (int i = 0; i < x.n; ++i) {
    const Held& h = x.h[i];
    if (h.src == src && (key < 0 || h.key == key) && (note < 0 || h.note == note)) ++gone;
    else x.h[k++] = h;
  }
  x.n = k;
  return gone;
}
CORE_COLD void sound_off(int vi) {
  Vo& x = vo[vi];
  if (!x.run || x.note < 0) return;
  inst::play_off(vi, x.note);
  x.note = -1;
}
// The pattern's list: the held pitches once each, ascending or (PLAYED) as played; the count
CORE_COLD int list_of(const Vo& x, int mode, uint8_t* pit, uint8_t* vel) {
  int n = 0;
  for (int i = 0; i < x.n; ++i) {
    int j = 0;
    while (j < n && pit[j] != x.h[i].note) ++j;
    if (j < n) continue;
    if (mode != kPlayed)
      for (j = n; j > 0 && pit[j - 1] > x.h[i].note; --j) pit[j] = pit[j - 1], vel[j] = vel[j - 1];
    pit[j] = x.h[i].note, vel[j] = x.h[i].vel;
    ++n;
  }
  return n;
}
// MODE OFF by a lock, the sound's on: the note ends, the hold rests, placed again when the lock goes
CORE_COLD void rest(int vi) {
  sound_off(vi);
  vo[vi].started = false;
}
// The note at position pos of the pattern over c.range octaves
CORE_COLD int pick(int pos, int n, const Cfg& c, int* idx) {
  int m = n * (c.range + 1), i;
  switch (c.mode) {
    case kDown: i = m - 1 - pos % m; break;
    case kUpDown: {
      int p = m > 1 ? 2 * m - 2 : 1, k = pos % p;
      i = k < m ? k : p - k;
      break;
    }
    case kRandom: i = rand_below(m); break;
    default: i = pos % m; break;
  }
  *idx = i % n;
  return i / n;   // the octave
}
CORE_COLD double grid(double o, double g, int64_t n, double sw) { return o + (double)n * g + (n & 1 ? sw * g : 0); }
// The frames of a 1/16 now
CORE_COLD double per16() {
  double origin, per;
  int lat;
  if (seq::beat_map(&origin, &per, &lat)) return per;
  return seq::step_frames(seq::tempo() > 1 ? seq::tempo() : 120.0, 2);
}

CORE_COLD void play(int vi, const Cfg& c) {
  Vo& x = vo[vi];
  uint8_t pit[kHeld], vel[kHeld];
  int n = list_of(x, c.mode, pit, vel), idx = 0;
  int oct = pick(x.pos++, n, c, &idx);
  int note = pit[idx] + 12 * oct;
  while (note > 127) note -= 12;
  seq::PolyCfg mono = {};
  mono.voices = 1;   // one at a time: a track that is no poly track plays on its own voice
  if (inst::play_on(vi, note, vel[idx], x.seq_cfg ? x.pcfg : mono)) voice_wake(vi), x.note = note;
  ++st.notes;
}
// A release that empties the hold before it played a note (off the grid, a slow SPD, a short LEN): its first note now, not silence
CORE_COLD int release(int vi, int src, int key, int note) {
  Vo& x = vo[vi];
  Held save[kHeld];
  int was = x.n;
  memcpy(save, x.h, sizeof save);
  int gone = drop(vi, src, key, note);
  if (!gone || x.n || !x.run || x.pos || x.note >= 0) return gone;
  Cfg c = cfg_at(vi, (int64_t)seq::frames());
  if (c.mode == kOff) return gone;
  memcpy(x.h, save, sizeof save);
  x.n = was;
  play(vi, c);
  x.n = 0;
  int64_t h = (int64_t)seq::frames(), len = (int64_t)(kInterval[c.speed] * per16() * kGatePct[c.gate] / 100.0);
  x.off_at = h + (len > kMinGate ? len : kMinGate);
  ++st.released;
  return gone;
}
}  // namespace

CORE_COLD Cfg decode(int v) {
  if (v < 0 || v >= kCodes) v = 0;
  Cfg c;
  c.mode = (uint8_t)(v % kModes), v /= kModes;
  c.speed = (uint8_t)((v % kSpeeds + kDefSpeed) % kSpeeds), v /= kSpeeds;
  c.range = (uint8_t)(v % kRanges), v /= kRanges;
  c.gate = (uint8_t)((v % kGates + kDefGate) % kGates), v /= kGates;
  c.swing = (uint8_t)(v % kSwings);
  return c;
}
CORE_COLD int encode(const Cfg& c) {
  int v = c.swing % kSwings;
  v = v * kGates + (c.gate + kGates - kDefGate) % kGates;
  v = v * kRanges + c.range % kRanges;
  v = v * kSpeeds + (c.speed + kSpeeds - kDefSpeed) % kSpeeds;
  return v * kModes + c.mode % kModes;
}
CORE_COLD const char* mode_name(int m) {
  static const char* const k[kModes] = {"OFF", "UP", "DOWN", "UPDN", "RND", "PLAY"};
  return k[m >= 0 && m < kModes ? m : 0];
}
CORE_COLD const char* speed_name(int s) {
  static const char* const k[kSpeeds] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
  return k[s >= 0 && s < kSpeeds ? s : kDefSpeed];
}
CORE_COLD int gate_pct(int g) { return kGatePct[g >= 0 && g < kGates ? g : kDefGate]; }
CORE_COLD int swing_pct(int s) { return kSwingPct[s >= 0 && s < kSwings ? s : 0]; }

CORE_COLD static void changed(int t) {   // track t's code moved: its voice runs by it, or stops
  int vi = ui::remote_track_voice(t);
  if (vi >= 0 && vo[vi].run && !on(vi)) end(vi);
  check_ms = 0;   // the pins and the clock at the next pass
}
CORE_COLD bool sysex(const uint8_t* d, int len) {
  if (len < 10 || d[4] != kCmd) return false;
  if (d[5] == 1 && len == 10 && d[6] < 12 && !(d[7] & 0x80) && d[8] < 8) {
    int c = d[7] | d[8] << 7;
    ++st.reports;
    if (c != code[d[6]]) code[d[6]] = (uint16_t)c, changed(d[6]);
  }
  return true;
}
CORE_COLD void set(int t, const Cfg& c) {
  if (t < 0 || t >= 12) return;
  int v = encode(c);
  code[t] = (uint16_t)v;
  changed(t);
  uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, kCmd, 0x01, (uint8_t)t, (uint8_t)(v & 0x7F), (uint8_t)(v >> 7), 0xF7};
  to_rytm(m, 10);
  ++st.edits;
}
CORE_COLD Cfg get(int t) { return decode(t >= 0 && t < 12 ? code[t] : 0); }
CORE_COLD bool on(int vi) { return vi >= 0 && vi < kVoices && code_of(vi) != 0; }

CORE_COLD bool seq_on(int vi, int note, int vel, const seq::PolyCfg& cfg) {
  if (vi >= 0 && vi < kVoices && lk[vi].mask) {
    Cfg c = cfg_at(vi, (int64_t)seq::frames());
    if (c.mode == kOff) {
      if (lk[vi].mask & 1) vo[vi].plain[note >> 5 & 3] |= 1u << (note & 31);
      if (vo[vi].run && !on(vi)) end(vi);   // (its lock gone just now: before the note it could end)
      return false;
    }
    if (!on(vi) && vo[vi].note != note) inst::play_off(vi, note);   // locked on: a plain note of that pitch gives way (one off comes for both)
  } else if (!on(vi)) {
    return false;
  }
  vo[vi].seq_cfg = true, vo[vi].pcfg = cfg;
  add(vi, note, vel, kLiveSeq, note);
  return true;
}
CORE_COLD bool seq_off(int vi, int note) {
  if (vi < 0 || vi >= kVoices) return false;
  if (note < 0) {
    memset(vo[vi].plain, 0, sizeof vo[vi].plain);
    return drop(vi, kLiveSeq, -1, -1), false;
  }
  vo[vi].plain[note >> 5 & 3] &= ~(1u << (note & 31));
  return release(vi, kLiveSeq, note, note) > 0;
}
CORE_COLD void plain_off(int vi, int note) {
  if (vi < 0 || vi >= kVoices || note < 0) return;
  uint32_t& w = vo[vi].plain[note >> 5 & 3], bit = 1u << (note & 31);
  if (!(w & bit)) return;
  w &= ~bit;
  inst::play_off(vi, note);
}
CORE_COLD bool live_on(int vi, int note, int vel, int ch, int key) {
  if (!on(vi)) return false;
  if (!vo[vi].n) vo[vi].seq_cfg = false;
  add(vi, note, vel, (uint8_t)ch, (uint8_t)key);
  return true;
}
CORE_COLD bool live_off(int vi, int ch, int key) {
  if (vi < 0 || vi >= kVoices) return false;
  if (key < 0) return drop(vi, ch, -1, -1), false;
  return release(vi, ch, key, -1) > 0;
}
CORE_COLD bool trig(int vi, int note, int vel, const float* p) {
  vi &= 7;
  if (!on(vi)) return false;
  drop(vi, kLiveTrig, -1, -1);   // mono on the voice, as a trig is
  if (!vo[vi].n) vo[vi].seq_cfg = false;
  const V& x = v[vi];
  bool chords = x.fm;
#if CORE_MSAMP
  if (int m = nm::of(vi))   // the note machine (forced ones too, before the voice's first report): chord words on P13..P16 only
    chords = m == kMultiMachine || m == kGranMachine;   // (WAVETABLE's are TBL B ... FTYPE)
#endif
  uint8_t ns[poly::kMaxNotes] = {(uint8_t)note};
  int k = chords ? poly::expand(note, poly::chord_of(p), ns) : 1;
  for (int i = 0; i < k; ++i) add(vi, ns[i], vel, kLiveTrig, ns[i]);
  vo[vi].pos = 0, vo[vi].started = false;   // a trig is a new chord, even over one still held
  vo[vi].trig_ms = now_ms();
  return true;
}
CORE_COLD int held(int vi) { return vo[vi & 7].n; }
CORE_COLD bool trig_off(int vi) { return vi >= 0 && vi < kVoices && release(vi, kLiveTrig, -1, -1) > 0; }

CORE_COLD void lock(int vi, int track, int mask, const uint8_t* val, int64_t until) {
  if (vi < 0 || vi >= kVoices) return;
  Lk& l = lk[vi];
  l.mask = (uint8_t)(mask & 31), l.track = (int8_t)(track >= 0 && track < 12 ? track : 0), l.until = until;
  for (int i = 0; i < seq::kArpLocks; ++i) l.val[i] = (uint8_t)(val[i] <= seq::kArpLockMax[i] ? val[i] : seq::kArpLockMax[i]);
  if (vo[vi].run && cfg_at(vi, (int64_t)seq::frames()).mode == kOff) {   // now, not at the block: the step's own notes come next
    if (on(vi)) rest(vi);
    else end(vi);
  }
}
CORE_COLD int locked(int vi) { return vi >= 0 && vi < kVoices && (int64_t)seq::frames() < lk[vi].until ? lk[vi].mask : 0; }

void block(int64_t h) {
  if (!running) return;
  uint32_t t0 = now_us();
  double origin = 0, per = 0;
  int lat = 0;
  bool clk = seq::beat_map(&origin, &per, &lat);
  for (int vi = 0; vi < kVoices; ++vi) {
    if (!(running >> vi & 1)) continue;
    Vo& x = vo[vi];
    if (x.note >= 0 && h + kBlock / 2 >= x.off_at) sound_off(vi);
    if (!x.n) {   // the hold is over; the note plays out its gate
      if (x.note < 0) sleep(vi);
      x.started = false, x.pos = 0;
      continue;
    }
    Cfg c = cfg_at(vi, h);
    if (c.mode == kOff) {
      if (on(vi)) rest(vi);
      else end(vi);
      continue;
    }
    double p = clk ? per : seq::step_frames(seq::tempo() > 1 ? seq::tempo() : 120.0, 2);
    double g = kInterval[c.speed] * p, sw = kSwingFrac[c.swing];
    bool placed = x.started && x.clocked == clk && x.speed == c.speed;
    if (!placed) {
      x.started = true, x.clocked = clk, x.speed = c.speed;
      // free: from now, or one interval after the last note (a move from the clock, a new speed: no double)
      double from = x.pos && x.last_f + g > (double)h ? x.last_f + g : (double)h;
      x.free_o = from - g, x.next = 0;
    }
    double f, f1;
    if (clk) {
      double catch_f = lat + kCatchMax;
      if (catch_f > g / 2) catch_f = g / 2;
      f = grid(origin, g, x.next, sw);
      bool moved = !placed || origin - x.o_seen > g / 4 || x.o_seen - origin > g / 4;   // a pattern switch, a jump, a tempo
      x.o_seen = origin;
      if (x.pos == 0 || moved || f > (double)h + 2.5 * g || f < (double)h - g) {
        // the next point, or one just passed; never one played already (notes go `latency` early, a switch's new clock comes after)
        double q = ((double)h - origin) / g;   // (floored by hand: libm's floor is in no bare-metal path here)
        int64_t n = (int64_t)q - (q < 0 ? 2 : 1);
        while (grid(origin, g, n, sw) < (double)h - catch_f || (x.pos && grid(origin, g, n, sw) < x.last_f + g / 2)) ++n;
        x.next = n;
      }
      f = grid(origin, g, x.next, sw), f1 = grid(origin, g, x.next + 1, sw);
    } else {   // free: one interval on from the last, at the tempo now
      f = x.free_o + g + (x.next & 1 ? sw * g : 0), f1 = x.free_o + 2 * g + (x.next & 1 ? 0 : sw * g);
    }
    // MODE locked on, the sound's OFF: no point left in the step (a swung or late note, a slow SPD): its first note now, or the hold ends unheard
    if (!x.pos && lk[vi].mask && !on(vi) && f >= (double)lk[vi].until) f = (double)h;
    if ((double)h + kBlock / 2 < f) continue;
    sound_off(vi);   // (a gate of 100 %, or a late one)
    if ((double)h - f > kBlock) ++st.late;
    play(vi, c);
    int64_t off = (int64_t)(f + (f1 - f) * kGatePct[c.gate] / 100.0);
    x.off_at = off > h + kMinGate ? off : h + kMinGate;
    x.last_f = f;
    x.free_o += g;
    ++x.next;
  }
  uint32_t us = now_us() - t0;
  if (us > st.worst_us) st.worst_us = us;
}

CORE_COLD void stop() {
  for (int vi = 0; vi < kVoices; ++vi) {
    Vo& x = vo[vi];
    if (!x.run) continue;
    drop(vi, kLiveTrig, -1, -1);
    drop(vi, kLiveSeq, -1, -1);
    if (!x.n) sound_off(vi), sleep(vi);
    x.started = false;   // what stays (keys, pads) runs on free
    x.seq_cfg = false;
  }
}
CORE_COLD void end(int vi) {
  if (vi < 0 || vi >= kVoices) return;
  sound_off(vi);
  vo[vi].n = 0;
  sleep(vi);
}
CORE_COLD void kit_changed() {
  uint32_t ms = now_ms();
  for (int vi = 0; vi < kVoices; ++vi) {
    Vo& x = vo[vi];
    bool fresh = false;
    for (int i = 0; i < x.n; ++i) fresh = fresh || (x.h[i].src == kLiveTrig && ms - x.trig_ms <= kFreshMs);
    if (!fresh) {
      end(vi);
      continue;
    }
    int k = 0;   // the new kit's first trig: the 3B comes up to a UI tick after it. Only it stays
    for (int i = 0; i < x.n; ++i)
      if (x.h[i].src == kLiveTrig) x.h[k++] = x.h[i];
    x.n = k;
  }
}
CORE_COLD void link_lost() {
  for (int vi = 0; vi < kVoices; ++vi) end(vi);
}
CORE_COLD void machines() {
  for (int vi = 0; vi < kVoices; ++vi) {
    uint16_t sig = 0;
    for (int t = 0; t < 12; ++t)
      if (ui::remote_track_voice(t) == vi) sig = (uint16_t)(sig * 31 + (uint16_t)(ui::remote_track_machine(t) + 1));
    if (sig != vo[vi].sig) {
      vo[vi].sig = sig;
      end(vi);
    }
  }
}
CORE_COLD bool busy(int vi) { return vi >= 0 && vi < kVoices && vo[vi].run; }

CORE_COLD void tick(uint32_t ms) {
  if ((int32_t)(ms - check_ms) < 200 && check_ms) return;
  check_ms = ms | 1;
  uint16_t pins = 0;
  for (int t = 0; t < 12; ++t) {
    if (!code_on(code[t]) || !sidecar_track(t)) continue;
    int vi = ui::remote_track_voice(t);
    bool mi = false;
#if CORE_MSAMP
    mi = vi >= 0 && v[vi].mi;
#endif
    pins |= (uint16_t)((mi ? 0x1000u : 1u) << (mi ? 0 : t));   // bit 12: the MIDI machine needs a clock, no VCA
  }
  if (pins != pins_sent) pins_sent = pins, seq::set_arp_tracks((uint16_t)(pins & 0xFFF), pins != 0);
}

CORE_COLD const Stats& stats() { return st; }

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "arp", 3) != 0 || (p[3] && p[3] != '=')) return false;
  if (!p[3]) {
    int n = sniprintf(out, cap, CORE_STR("arp: %d running, %lu notes (%lu late, %lu at a release), %lu holds, %lu reports, %lu edits, a block's worst %lu us; tracks"),
                      st.running, (unsigned long)st.notes, (unsigned long)st.late, (unsigned long)st.released, (unsigned long)st.chords,
                      (unsigned long)st.reports, (unsigned long)st.edits, (unsigned long)st.worst_us);
    for (int t = 0; t < 12 && n < cap; ++t) n += sniprintf(out + n, cap - n, " %d", code[t]);
    return true;
  }
  int t = atoi(p + 4);
  const char* e = strchr(p + 4, ':');
  if (t < 1 || t > 12 || !e) return sniprintf(out, cap, CORE_STR("arp=T:mode=0..5,spd=0..5,rng=0..2,gate=0..2,swng=0..2")), true;
  Cfg c = get((int)t - 1);
  for (const char* q = e + 1; *q;) {
    const char* eq = strchr(q, '=');
    if (!eq) return sniprintf(out, cap, CORE_STR("arp: bad pair")), true;
    int val = atoi(eq + 1);
    if (!strncmp(q, "mode", 4)) c.mode = (uint8_t)(val % kModes);
    else if (!strncmp(q, "spd", 3)) c.speed = (uint8_t)(val % kSpeeds);
    else if (!strncmp(q, "rng", 3)) c.range = (uint8_t)(val % kRanges);
    else if (!strncmp(q, "gate", 4)) c.gate = (uint8_t)(val % kGates);
    else if (!strncmp(q, "swng", 4)) c.swing = (uint8_t)(val % kSwings);
    else return sniprintf(out, cap, CORE_STR("arp: bad pair")), true;
    q = strchr(eq, ',');
    if (!q) break;
    ++q;
  }
  set((int)t - 1, c);
  sniprintf(out, cap, CORE_STR("arp %d: %s %s %d oct, gate %d, swing %d, code %d"), t, mode_name(c.mode), speed_name(c.speed), c.range + 1,
            gate_pct(c.gate), swing_pct(c.swing), encode(c));
  return true;
}

CORE_COLD void reset() {
  memset(vo, 0, sizeof vo);
  memset(lk, 0, sizeof lk);
  for (Vo& x : vo) x.note = -1;
  memset(code, 0, sizeof code);
  memset(&st, 0, sizeof st);
  running = 0, check_ms = 0, pins_sent = 0xFFFF;
}

}  // namespace arp
}  // namespace core
#endif
