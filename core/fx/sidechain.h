// core/fx/sidechain.h - the SIDECHAIN stage of the INSERT chains (docs/INSERT-FX.md "SIDECHAIN"): one gain stage per chain
// (V1..V8, B1..B3, M1 M2), after the chain's two slots and before its POST EQ. The DSP is ins::Engine's (insert.h:
// RunSc); this file is what it and the rest of the firmware share: the modes and sources, the key bus the sources write
// into, the values' names and texts.
//
// MODE  OFF (the default: the stage is skipped, bit for bit, no CPU)
//       PUMP  a trig-driven gain envelope: DPTH ATT HOLD REL SHAP (and THR, the onset level for a source with no trigs)
//       DUCK  the INSERT COMP keyed by the source: THR RAT ATT REL, and the key's band-pass KBAS (high pass, 18 dB/oct) and
//             KWID (low pass, 12 dB/oct, KWID octaves above it); MON plays the filtered key instead of the chain (0.5.2)
// SRC   0..11 a Rytm track BD SD RS CP BT LT MT HT CH OH CY CB (its Overbridge stem, shared by pairs; its trig is the
//       Rytm's note-out on the track's channel), 12 AINL 13 AINR 14 AINLR (AUDIO IN: no trig, a level onset), 15..22
//       V1..V8 (a Sidecar voice's pre-chain signal; its trig is the note that started it).
// The sources are written by whoever has them (the Overbridge IN side, the voices' render, the MIDI in) and read by the
// chain that runs on any core: one writer per ring, a published counter, never a lock. Stems exist only in Overbridge mode:
// a source whose ring does not move is "missing", and the stage does nothing (key 0, no trigs).
#pragma once
#include <math.h>
#include <stdint.h>

#include "platform.h"

namespace core {
namespace sc {

constexpr int kParams = 6;                 // the knobs
enum Mode : uint8_t { kOff = 0, kPump, kDuck, kModes };
constexpr int kTracks = 12, kStems = 8, kVoices = 8;
enum Src : uint8_t { kAinL = 12, kAinR = 13, kAinLR = 14, kV0 = 15, kSrcs = kV0 + kVoices };
// key rings: 0..7 stems, 8 AUDIO IN L, 9 R, 10..17 voices
constexpr int kKeys = kStems + 2 + kVoices;
constexpr int kRing = 256;                 // frames a ring holds (a power of 2)
constexpr int kStale = 1024;               // frames of the reader's own time without a write: the source is missing

// the stem of each Rytm track (IN 2..9 = BD SD RS/CP BT LT MT/HT CH/OH CY/CB)
constexpr uint8_t kTrackStem[kTracks] = {0, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7, 7};
// source -> key ring (-1 none)
constexpr int key_of(int src) {
  return src >= 0 && src < kTracks ? kTrackStem[src] : src == kAinL || src == kAinLR ? kStems : src == kAinR ? kStems + 1
         : src >= kV0 && src < kSrcs ? kStems + 2 + (src - kV0) : -1;
}
// source -> trig counter (0..11 the Rytm tracks, 12..19 the voices; -1 none: AUDIO IN has onsets only)
constexpr int trig_of(int src) { return src >= 0 && src < kTracks ? src : src >= kV0 && src < kSrcs ? kTracks + (src - kV0) : -1; }
constexpr int kTrigs = kTracks + kVoices;

struct Bus {
  float ring[kKeys][kRing];
  volatile uint32_t w[kKeys];          // frames written, ever (a release store after the frames)
  volatile uint32_t ms[kKeys];         // when the last write was (now_ms): the control side's "is it there"
  volatile uint32_t trig[kTrigs];      // trigs, ever (the writer bumps)
  volatile uint32_t wanted;            // key rings some chain reads (bit per ring): a writer skips the others
  volatile uint32_t wanted_trig;
};
// One bus per voice slot (two boxes: each slot's chains key from its own box's stems, AUDIO IN, voices and trigs; slot 0 on a
// one-box build). The calls without a slot are slot 0's.
constexpr int kBuses = 2;
inline Bus g_bus[kBuses];
inline Bus& bus(int s = 0) { return g_bus[(unsigned)s < (unsigned)kBuses ? s : 0]; }

// ---- the writers (any core; one writer per ring)
// One ring's frames (floats), published after they are in
inline void write(int s, int key, const float* x, int n) {
  if (key < 0 || key >= kKeys) return;
  Bus& b = bus(s);
  const uint32_t w = b.w[key];
  float* r = b.ring[key];
  for (int i = 0; i < n; ++i) r[(w + (uint32_t)i) & (kRing - 1)] = x[i];
  b.ms[key] = now_ms();
  __atomic_store_n(&b.w[key], w + (uint32_t)n, __ATOMIC_RELEASE);
}
inline void write(int key, const float* x, int n) { write(0, key, x, n); }
// The Overbridge IN side of slot s's box: n frames, channel c of frame i at f[i * stride + c]. `stem0` the first stem's
// channel (IN 2; -1: a box with no Rytm track stems), `ain` the AUDIO IN L / R channels (the device table: fx/out.h
// Device::audio_in). Only the rings a chain reads.
inline void in_block(int s, const int32_t* f, int n, int stride, int stem0, const uint8_t* ain) {
  const uint32_t want = bus(s).wanted;
  float x[16];
  while (n > 0) {
    const int m = n < 16 ? n : 16;
    for (int k = 0; k < kStems + 2; ++k) {
      if (!(want >> k & 1u) || (k < kStems && stem0 < 0)) continue;
      const int ch = k < kStems ? stem0 + k : ain[k - kStems];
      for (int i = 0; i < m; ++i) x[i] = (float)f[i * stride + ch] * (1.0f / 2147483648.0f);
      write(s, k, x, m);
    }
    f += m * stride, n -= m;
  }
}
inline void in_block(const int32_t* f, int n, int stride, int stem0, const uint8_t* ain) { in_block(0, f, n, stride, stem0, ain); }
// a trig of track t of slot s's box (the Rytm's tracks: its note-out), of box voice vi (0..7) of slot s
inline void trig_track(int s, int t) {
  Bus& b = bus(s);
  if (t >= 0 && t < kTracks && (b.wanted_trig >> t & 1u)) __atomic_fetch_add(&b.trig[t], 1u, __ATOMIC_RELEASE);
}
inline void trig_voice(int s, int vi) {
  Bus& b = bus(s);
  vi &= 7;
  if (b.wanted_trig >> (kTracks + vi) & 1u) __atomic_fetch_add(&b.trig[kTracks + vi], 1u, __ATOMIC_RELEASE);
}
inline void trig_track(int t) { trig_track(0, t); }
inline void trig_voice(int vi) { trig_voice(0, vi); }
inline bool wants_in(int s = 0) { return bus(s).wanted & ((1u << (kStems + 2)) - 1u); }
inline bool wants_voice(int s, int vi) { return bus(s).wanted >> (kStems + 2 + (vi & 7)) & 1u; }
inline bool wants_voice(int vi) { return wants_voice(0, vi); }
// The reader (the stage): the newest m frames of ring `key` into out. False (and zeros) when the ring is missing: no write
// for kStale frames of the reader's own time (*since counts them, *last_w is the counter last seen, ~0 before the first
// look). `late`: frames the stage is behind the ring's newest (a call longer than one chunk; m + late <= 240)
inline bool read(int s, int key, float* out, int m, uint32_t* last_w, uint32_t* since, int late = 0) {
  Bus& b = bus(s);
  const uint32_t w = __atomic_load_n(&b.w[key], __ATOMIC_ACQUIRE);
  if (*last_w == ~0u) *last_w = w, *since = (uint32_t)kStale;   // the first look: a ring that is not seen moving is stale
  if (w != *last_w) *last_w = w, *since = 0;
  else if (*since < 0x40000000u) *since += (uint32_t)m;
  if (w == 0 || *since >= (uint32_t)kStale) {
    for (int i = 0; i < m; ++i) out[i] = 0.0f;
    return false;
  }
  const float* r = b.ring[key];
  for (int i = 0; i < m; ++i) out[i] = r[(w - (uint32_t)late - (uint32_t)m + (uint32_t)i) & (kRing - 1)];
  return true;
}
inline bool read(int key, float* out, int m, uint32_t* last_w, uint32_t* since, int late = 0) { return read(0, key, out, m, last_w, since, late); }

// ---- DUCK's key band-pass (0.5.2): the knobs' scales. KBAS (knob 4): 0 OFF, else 20 Hz .. 19.6 kHz, log (knob 1 .. 127). KWID
// (knob 5): 0 OFF, else 0.5 .. 9.9 octaves (linear in the knob) above KBAS (above 20 Hz while KBAS is OFF)
constexpr float kKbasOct = 9.93709f;   // log2(19600 / 20)
inline float kbas_hz(int v) { return v <= 0 ? 0.0f : 20.0f * exp2f(kKbasOct * (float)((v > 127 ? 127 : v) - 1) / 126.0f); }
inline float kwid_oct(int v) { return v <= 0 ? 0.0f : 0.5f + 9.4f * (float)((v > 127 ? 127 : v) - 1) / 126.0f; }
// an older card's DUCK HPF knob (0 off, else 40 Hz x 2^(6 v / 127), 12 dB/oct) as the nearest KBAS (the same corner)
inline int kbas_from_old_hpf(int v) {
  if (v <= 0) return 0;
  const int k = 1 + (int)(126.0f * log2f(2.0f * exp2f(6.0f * (float)(v > 127 ? 127 : v) / 127.0f)) / kKbasOct + 0.5f);
  return k < 1 ? 1 : k > 127 ? 127 : k;
}

// ---- the control side (insert.cpp)
const char* mode_name(int mode);                 // "OFF" "PUMP" "DUCK"
int mode_from_name(const char* s);               // any case, or 0..2; -1 neither
const char* src_name(int src);                   // "BD".."CB" "AINL" "AINR" "AINLR" "V1".."V8"
int src_from_name(const char* s);                // -1 none
const char* label(int mode, int k);              // "DPTH" "ATT" ... ("-" unused)
uint8_t def(int mode, int k);                    // the knob a mode starts with

}  // namespace sc
}  // namespace core
