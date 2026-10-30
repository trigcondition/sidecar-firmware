// core/fx/send_fx.h - the Sidecar's send FX (docs/superpowers/specs/2026-10-16-sidecar-send-fx-research.md, route A):
// SEND FX 1 and 2, each a TYPE and 5 params stored in the Rytm's kit (FX-track words 0, 29, 38..41, so they travel
// with the kit and its patterns); input = the sum over the Rytm's 8 voice stems (Overbridge IN 2..9, post analog
// chain) of stem x the track's send level; stereo out, wet only, returned by the board per FX slot (P1 = ROUTE) on
// host -> Rytm 2/3 (MAIN FX, PRE: through the analog distortion + compressor) or 0/1 (MAIN, POST: clean). Measured
// 2026-10-16 on the real Rytm (OS 1.73, Overbridge mode): OUT 1-2 is added after the analog master distortion and
// compressor, OUT 3-4 enters before them. No firmware mod is involved: the pre/post choice is the choice of pair.
//
// Two halves:
//   fx::Control  the values: per-track sends and each FX's TYPE + params (host link 0x20 SENDFX, or the console's
//                overrides), the route (console, /.rytm/sendfx.txt). Small, in every build; written by the control
//                side, read by the audio side (byte fields: a torn read cannot happen).
//   fx::SendFx   the DSP: header-only, allocation-free (~5.9 MB with SPACE's 0.5 MB tape a slot, RINGS, CLOUDSEED and STUTTER's 2 MiB ring; the board
//                places it; the Teensy's small set: 16.6 KB, 16-bit lines, the small types sharing one block a slot). One instance, used by one core (the Pi:
//                render core 2; the Teensy: the USB host interrupt); never per render core, so RYTM_RENDER_CORES is
//                not involved.
//
// TYPE ids (stable; sim/protocol.py SENDFX_TYPES): 0 off; 1..63 built in (1 CHORUS, 2 COMB, 16 CRUSH, 17 RINGMOD,
// 18 HALO (halo_fx.h) in every build, the Teensy's small set; with RYTM_SENDFX_LARGE (the Pi and the Mac) also 5 SPACE (tape_fx.h), 6 RINGS, 8 CLOUDSEED, 15 STUTTER (a
// master insert, not a send: stutter_fx.h; one slot at a time, FX 1 first, a second is MISSING); the rest reserved
// for planned effects: an older Sidecar plays an id it lacks as silence and reports it MISSING by name; no tag);
// 64..67 the user-FX slots UFX1..UFX4, as the user machines' U1..U5: each project's slot map on the Sidecar card
// (/.rytm/fxslots/<project>.txt) says which user effect a slot holds (fingerprint + name), and the kit stores the
// effect's 14-bit tag (0x20 track 13), so a kit moved to a project or card where the slot holds another effect is
// detected: that FX silent and MISSING (!NAME). No user-effect plugins exist yet: a user slot is at best MISSING.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <new>

// RINGS (rings_fx.h, 72 KB a slot) and CLOUDSEED (cloudseed_fx.h, 1.25 MB a slot): built where RYTM_SENDFX_LARGE is 1,
// by default everywhere but the Teensy (1 MB of RAM in all, no PSRAM required), where the ids stay reserved:
// silent and reported MISSING. -DRYTM_SENDFX_LARGE=0 / =1 overrides. The Teensy plays the small set (CHORUS, COMB,
// CRUSH, RINGMOD) with 16-bit delay lines (RYTM_SENDFX_LINE16, by default where RYTM_SENDFX_LARGE is 0): CHORUS 2048
// samples a slot (42 ms, its full range), COMB 1024 a side (21 ms: 47 Hz at 48 kHz, below its 55 Hz x 1.03 lowest),
// so the knob maps are the Pi's at 48 kHz; at 96 kHz the COMB's lowest TUNE would be clamped to ~94 Hz.
#ifndef RYTM_SENDFX_LARGE
#if defined(TEENSYDUINO) || defined(__IMXRT1062__)
#define RYTM_SENDFX_LARGE 0
#else
#define RYTM_SENDFX_LARGE 1
#endif
#endif
#ifndef RYTM_SENDFX_LINE16
#define RYTM_SENDFX_LINE16 (!RYTM_SENDFX_LARGE)
#endif
// The DSP's code: in the Teensy's program flash (its ITCM is full), inline elsewhere.
#ifndef FX_DSP
#if defined(TEENSYDUINO) || defined(__IMXRT1062__)
#define FX_DSP __attribute__((section(".flashmem.sendfx")))   // .flashmem* (inline: its own name, no section type conflict)
#else
#define FX_DSP
#endif
#endif
#if RYTM_SENDFX_LARGE
#include "fx/cloudseed_fx.h"
#include "fx/rings_fx.h"
#endif
#include "fx/knob_labels.h"
#include "fx/stutter_fx.h"   // its clock word in every build; its Stutter (2 MiB) placed only with RYTM_SENDFX_LARGE

namespace core {
namespace fx {

constexpr int kTracks = 12, kStems = 8, kSlots = 2, kKnobs = 6;   // knob 0 TYPE, 1 P1 (ROUTE), 2..5 the effect's P2..P5
enum Type : uint8_t {
  kOff = 0, kChorus = 1, kComb = 2, kTape = 5, kRings = 6, kCloudSeed = 8, kStutter = 15, kCrush = 16, kRingMod = 17, kHalo = 18,
  kFirstUser = 64
};
// the ids this build's SendFx plays
constexpr bool plays(int t) {
  return t == kChorus || t == kComb || t == kCrush || t == kRingMod || t == kHalo ||
         (RYTM_SENDFX_LARGE && (t == kTape || t == kRings || t == kCloudSeed || t == kStutter));
}
// P1 = ROUTE per FX slot: 0..63 PRE, returned on 2/3 (through the Rytm's analog distortion + compressor; 0 is the
// default, so existing kits sound as before); 64..127 POST, returned on 0/1 (clean, after them). It shares TYPE's
// word. A change crossfades between the pairs over kPanFrames (~8 ms), so it never clicks. STUTTER follows its slot's
// P1 too (its mix on that pair). The console's sendfxout=fx|main forces every slot to one pair (Control::route).
// The effects use P2..P5 (k[0..3] below), all p-lockable.
// A knob's zone that picks an effect's structure (RINGS P3's model, CLOUDSEED P2's character): a change of zone fades
// out, clears and fades in like a TYPE change (p-lockable, so per step: never a click, a ~10..30 ms gap).
// Returns the zone (0..3), or -1 for a type without one; `structure_knob` the P index (2..5) it lives in.
constexpr int structure_knob(int t) { return t == kRings ? 3 : t == kCloudSeed ? 2 : 0; }
inline int structure_of(int t, const volatile uint8_t* knob) {
  int j = structure_knob(t);
  return j ? (knob[j] > 127 ? 127 : knob[j]) >> 5 : -1;
}
// Control::route, the console override: kRouteSlot each slot's P1 decides (the normal path), kRouteFx all on 2/3 (PRE),
// kRouteMain all on 0/1 (POST); an explicit sendfxout=fx|main wins over P1.
enum Route : uint8_t { kRouteSlot = 0, kRouteFx = 1, kRouteMain = 2 };
enum State : uint8_t { kStEmpty = 0, kStReady = 1, kStMissing = 3 };   // 1A's state codes (user slots)
constexpr float kLimitFx = 0.5f;     // -6 dBFS: 2/3 enter the PRE mix at a fixed ~0.16 (research section 1)
constexpr float kLimitMain = 0.25f;  // -12 dBFS, as core::MainOut on MAIN
constexpr int kPanFrames = 384;      // a P1 (ROUTE) change: the crossfade between 2/3 and 0/1, 8 ms
constexpr int kFadeFrames = 480;     // a TYPE change: 10 ms out, the new effect from silence, 10 ms in
constexpr int kClearBytesPerFrame = 2048;   // a left effect is zeroed in pieces: <= 24 KB a 12-frame block

// The track that plays each stem: IN 2..9 = BD SD RS/CP BT LT MT/HT CH/OH CY/CB. A shared voice takes the louder
// of its two tracks' levels (the owner of the sounding trig is not reported; research "Pan and shared voices").
constexpr int8_t kStemTracks[kStems][2] = {{0, -1}, {1, -1}, {2, 3}, {4, -1}, {5, -1}, {6, 7}, {8, 9}, {10, 11}};

struct Control {
  volatile uint8_t send[kTracks][kSlots];   // 0..127: track t's SEND FX 1 (sound index 0 bits 8..14), 2 (bits 1..7)
  // FX k's knobs, 0..127: [0] TYPE, [1..5] P1..P5. FX1 = word 29 hi, 29 lo, 38 hi, 38 lo, 39 hi, 39 lo;
  // FX2 = word 0 hi, 0 lo, 40 hi, 40 lo, 41 hi, 41 lo (TYPE and P1 in the stock-stored words 29 / 0).
  volatile uint8_t knob[kSlots][kKnobs];
  volatile uint8_t route;                   // Route: the override (kRouteSlot = P1 per slot)
  volatile uint32_t messages;               // 0x20 messages taken
  volatile uint8_t kill;                    // bumped by all_sound_off(): the audio side fades out, clears and fades in
  volatile uint32_t clock;                  // the MIDI clock (clock_in; stutter_fx.h ClockWord), for STUTTER
  volatile uint16_t stutter_lag;            // frames the stems trail the MIDI (console stutterlag=; 0 by default)
};
extern Control control;

bool builtin(int id);                        // an id this build plays (1, 2; 6, 8 with RYTM_SENDFX_LARGE)
// The name for id (built in, reserved, or the registry's user effect), at most 10 chars; "OFF" for 0.
const char* type_name(int id, int slot = -1);   // slot: a user id's name as the kit's FX `slot` sees it
int type_from_name(const char* s);           // a built-in name (any case) or a number 0..127; -1 if neither
State state(int slot);                       // FX slot's TYPE: kStEmpty (off), kStReady (playable), kStMissing
bool any_on();                               // an FX slot has a TYPE that plays
// MIDI CC 120 (All Sound Off) from the Rytm (a double STOP sends it on channels 1..13): both FX fade out in ~5 ms,
// are cleared and come back with TYPE and params untouched. Control side; a burst within 100 ms is one request.
// True if this call made the request (the board logs it).
bool all_sound_off();
// MIDI realtime / SPP from the Rytm (control side): F8 tick, FA start, FB continue, FC stop, F2 song position (spp in
// 16ths) -> control.clock, for STUTTER's grid.
void clock_in(uint8_t status, uint16_t spp = 0);
// The LFOs' (core/lfo.cpp sets them; null without): each knob take_sendfx takes passes knob_filter (the value to play,
// an LFO's modulation on top), each clock_in byte goes on to clock_listener.
extern uint8_t (*knob_filter)(int slot, int knob, uint8_t v);
extern void (*clock_listener)(uint8_t status, uint16_t spp);
// The FX buses' insert chains (core/fx/insert.h, CORE_POLY builds; null without): FX slot s's return y[n] (stereo, before
// its TYPE fade and the limits) through bus s's chain, in place. The send FX's core.
extern void (*bus_insert)(int slot, float (*y)[2], int n);
// The slot that is the STUTTER, or -1: FX 1 if both are (FX 2 is then MISSING: one ring, one insert).
inline int stutter_slot(const Control& c) {
  return !plays(kStutter) ? -1 : c.knob[0][0] == kStutter ? 0 : c.knob[1][0] == kStutter ? 1 : -1;
}
// 0x20 SENDFX (sim/protocol.py sendfx()): F0 7D 52 54 20 01 <t> then one hi/lo pair (t 0..11), six (t 12) or the
// two FX tags (t 13: <tag1 hi7> <tag1 lo7> <tag2 hi7> <tag2 lo7>), F7.
// False (and nothing changed) if malformed.
bool take_sendfx(const uint8_t* d, int len);
// 0x21 SENDFX NAMES (the TYPE knob's list: OFF and every playable effect, by id) and 0x22 SENDFX STATE (FX k's
// TYPE, state, name: the Rytm shows !NAME when MISSING) into m (>= 128 bytes); see sim/protocol.py.
int sysex_names(uint8_t* m, bool replace, int total, int first, int count);   // entries first..first+count-1
int names_total();
int sysex_state(uint8_t* m, int slot);
// 0x24 SENDFX LABELS: TYPE id's P2..P5 labels and zones (knob_labels.h) into m (>= 128 bytes); 0 if this build does
// not play id or has no labels for it. labels(id): the table, or null.
const KnobLabels* labels(int id);
int sysex_labels(uint8_t* m, int id);
// A console command without the ':' (sendfx, sendfx=K:TYPE, sendfx=off, sendfxout=fx|main, sendlvl=K:T|all:V,
// sendknob=K:I:V): true if it was one; the answer (one line) in reply. TYPE and knobs hold until the next 0x20 for
// the FX track (a stock Rytm sends none). A route change marks the card file to be written (card_dirty).
bool command(const char* p, char* reply, int max);
extern volatile bool card_dirty;
// The card: /.rytm/sendfx.txt "out=fx|main" (global: card_load at boot), /.rytm/fxslots/<project>.txt (line n =
// UFXn+1: "<tag 4 hex> <name>" or empty; slots_load on a 0x10 project change). card_save writes the route (the
// board's idle step). send_fx_card.cpp (the core's card).
void card_load();
void card_save();
void slots_load(const char* project);
constexpr int kUserSlots = 4;
struct UserFx {
  uint16_t tag;    // 1..0x3FFF; 0 = the slot is empty
  char name[11];
};
extern UserFx user[kUserSlots];
extern volatile uint16_t kit_tag[kSlots];   // the kit's tag for FX k (0x20 track 13; 0 = none reported)
uint16_t fx_tag(const char* name);          // FNV-1a 32 of the name, folded to 14 bits, 0 avoided

// ---- the DSP
struct Smooth {   // a one-pole toward the target, per sample
  float y = 0.0f;
  float Next(float target, float a) { return y += (target - y) * a; }
  // the block's worth at once (the one-pole's n steps ~ n a, at most all the way): y moves to its block-end value;
  // returns the per-sample step of the linear ramp there from the old y (the caller adds it each sample)
  float Ramp(float target, float a, int n) {
    if (y == target) return 0.0f;   // settled (the usual case)
    float y0 = y, an = a * (float)n;
    y += (target - y) * (an > 1.0f ? 1.0f : an);
    if (y - target < 1e-7f && target - y < 1e-7f) y = target;
    return (y - y0) * (1.0f / (float)n);
  }
};
inline float flush(float x) { return (x > -1e-20f && x < 1e-20f) ? 0.0f : x; }
inline float knob01(uint8_t v) { return (float)(v > 127 ? 127 : v) * (1.0f / 127.0f); }
// a soft knee toward lim: ~linear below lim/4, never above lim (u (27 + u^2) / (27 + 9 u^2), |u| <= 3). Branch-free
// (vminnm / vmaxnm on the Teensy); the division measured cheaper there than a division-free polynomial of the ratio
inline float soft_limit(float x, float lim) {
  float u = fminf(3.0f, fmaxf(-3.0f, x * (1.0f / lim)));   // lim is a constant: a multiply
  float y = lim * u * (27.0f + u * u) / (27.0f + 9.0f * u * u);
  return fminf(lim, fmaxf(-lim, y));   // rounding at |u| = 3
}

// soft_limit's series for |x| <= lim / 4 (u^2 <= 1/16): 1 - 8t/27 + 8t^2/81, t = u^2, within 1e-5 of the exact
// curve there; no division (the Teensy's limit loop runs it when a block's peak is that low)
inline float soft_limit_low(float x, float lim) {
  const float t = x * x * (1.0f / (lim * lim));
  return x * (1.0f + t * (-8.0f / 27.0f + t * (8.0f / 81.0f)));
}

// a delay line read with linear interpolation; N a power of 2
template <int N>
struct Line {
  float buf[N];
  uint32_t w = 0;
  void Clear() { memset(buf, 0, sizeof buf), w = 0; }
  void Write(float x) { buf[w++ & (N - 1)] = flush(x); }
  float Read(float d) const {   // d samples ago, clamped to 1..N - 2
    d = fminf((float)(N - 2), fmaxf(1.0f, d));
    float fi = floorf(d);
    uint32_t i = (uint32_t)fi;
    float fr = d - fi;
    float a = buf[(w - i) & (N - 1)], b = buf[(w - i - 1) & (N - 1)];
    return a + (b - a) * fr;
  }
  float ReadAt(uint32_t i, float fr) const {
    float a = buf[(w - i) & (N - 1)], b = buf[(w - i - 1) & (N - 1)];
    return a + (b - a) * fr;
  }
};

// The same, 16-bit (the Teensy: half the RAM). Full scale +-kLine16Fs (the summed sends can pass 1.0), saturated;
// truncated toward zero, so a decaying feedback loop ends at 0 instead of an LSB limit cycle. Noise ~-84 dB re 1.0.
constexpr float kLine16Fs = 2.0f;
template <int N>
struct Line16 {
  int16_t buf[N];
  uint32_t w = 0;
  void Clear() { memset(buf, 0, sizeof buf), w = 0; }
  void Write(float x) {
    buf[w++ & (N - 1)] = (int16_t)fminf(32767.0f, fmaxf(-32767.0f, x * (32767.0f / kLine16Fs)));
  }
  float Read(float d) const {
    d = fminf((float)(N - 2), fmaxf(1.0f, d));
    float fi = floorf(d);
    uint32_t i = (uint32_t)fi;
    float fr = d - fi;
    float a = buf[(w - i) & (N - 1)], b = buf[(w - i - 1) & (N - 1)];
    return (a + (b - a) * fr) * (kLine16Fs / 32767.0f);
  }
  float ReadAt(uint32_t i, float fr) const {   // Read with d = i + fr already split and clamped (a fixed delay)
    float a = buf[(w - i) & (N - 1)], b = buf[(w - i - 1) & (N - 1)];
    return (a + (b - a) * fr) * (kLine16Fs / 32767.0f);
  }
};
#if RYTM_SENDFX_LINE16
template <int N>
using DLine = Line16<N>;
#else
template <int N>
using DLine = Line<N>;
#endif

// The Rytm's labels for P2..P5 (knob_labels.h, 0x24). COMB P3: the decay's polarity, each with its step (0..63).
inline constexpr KnobLabels kChorusLabels = {{"RATE", "DPTH", "DLY", "FDBK"}, nullptr, 0};
inline constexpr Zone kCombZones[] = {{1, 0, 0, 1, 0, "POS"}, {1, 64, 0, 1, 0, "NEG"}};   // continuous: 0..63 inside
inline constexpr KnobLabels kCombLabels = {{"TUNE", "DCY", "DAMP", "SPRD"}, kCombZones, 2};
inline constexpr KnobLabels kCrushLabels = {{"RATE", "BITS", "DRV", "TONE"}, nullptr, 0};
inline constexpr KnobLabels kRingModLabels = {{"FREQ", "SPRD", "SHPE", "LFO"}, nullptr, 0};

// Chorus: two taps on one line, LFOs in quadrature (L, R: full width), optional feedback. P2..P5 (0 = the default;
// k[0..3]): P2 rate 0.2..5 Hz (squared), P3 depth 25..100 % of 6 ms, P4 delay 8..25 ms, P5 feedback 0..0.8. Unity
// level (the old P5 level dropped: the sends set it).
struct Chorus {
  static const int kLine = 2048;   // 42.7 ms (the longest read: 25 + 6 ms)
  DLine<kLine> line;
  float phase = 0.0f, fb_l = 0.0f, fb_r = 0.0f;
  Smooth rate, depth, delay, fb;   // ramped per block (Smooth::Ramp)
  void Clear() {
    line.Clear();
    phase = fb_l = fb_r = 0.0f;
  }
  static float Tri2Sine(float p) {   // p in [0, 1): a parabolic sine, -1..1
    float x = 1.0f - 4.0f * fabsf(p - 0.5f);   // triangle -1..1 (-1 at 0, 1 at 1/2)
    return x * (1.5f - 0.5f * x * x);
  }
  FX_DSP void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float a) {
    const float t_rate = 0.2f + 4.8f * knob01(k[0]) * knob01(k[0]), t_depth = (0.25f + 0.75f * knob01(k[1])) * 0.006f * sr,
                t_delay = (0.008f + 0.017f * knob01(k[2])) * sr, t_fb = 0.8f * knob01(k[3]);
    const float isr = 1.0f / sr;
    float r = rate.y, dp = depth.y, dl = delay.y, f = fb.y;
    const float sr_ = rate.Ramp(t_rate, a, n) * isr, sdp = depth.Ramp(t_depth, a, n), sdl = delay.Ramp(t_delay, a, n),
                sf = fb.Ramp(t_fb, a, n);
    r *= isr;
    // the LFO once a block (0.2..5 Hz: at most 3 % of a cycle in 32 frames), each tap's delay ramped linearly to it
    const float ph0 = phase, d0l = dl + dp * Tri2Sine(ph0), d0r = dl + dp * Tri2Sine(ph0 + 0.25f - floorf(ph0 + 0.25f));
    float ph = ph0 + (r + 0.5f * sr_ * (float)(n + 1)) * (float)n;   // the phase increments' sum over the block
    ph -= floorf(ph);
    const float dl1 = dl + sdl * (float)n, dp1 = dp + sdp * (float)n;
    const float d1l = dl1 + dp1 * Tri2Sine(ph), d1r = dl1 + dp1 * Tri2Sine(ph + 0.25f - floorf(ph + 0.25f));
    const float inv = 1.0f / (float)n, sl = (d1l - d0l) * inv, srr = (d1r - d0r) * inv;
    float yl = fb_l, yr = fb_r, tl = d0l, tr = d0r;
    for (int i = 0; i < n; ++i) {
      f += sf, tl += sl, tr += srr;
      line.Write(in[i] + 0.5f * f * (yl + yr));
      yl = line.Read(tl), yr = line.Read(tr);
      out[i][0] += yl;   // full width, unity level: L and R the two taps
      out[i][1] += yr;
    }
    phase = ph, fb_l = flush(yl), fb_r = flush(yr);
  }
};

// Comb: a feedback comb per side with a one-pole lowpass in the loop, the input scaled by sqrt(1 - fb^2) (white
// noise in = out in RMS). P2..P5 (0 = the default; k[0..3]; the old P5 level dropped: the sends set it):
//   P2 tune 55 Hz * 2^(v/24) (half-semitone steps, 55..2.1 kHz), P3 decay: 0..63 positive, 64..127 negative (an
//   octave down, odd harmonics), T60 = 0.2 s * 40^((v & 63) / 63) (0.2 .. 8 s, log); the loop gain is
//   g = 10^(-3 period / T60), so the ring is as long at every P2 tune (|g| <= 0.9995; at 55 Hz 0.2 s is g 0.53, the
//   old range's low end), P4 damping 0..0.95 (the loop low-pass: shortens the highs), P5 spread (R tuned up to 3 %
//   lower).
struct Comb {
#if RYTM_SENDFX_LINE16
  static const int kLine = 1024;   // 55 Hz x 1.03 = 902 samples at 48 kHz (the Teensy's rate)
#else
  static const int kLine = 2048;   // 55 Hz x 1.03 = 902 samples at 48 kHz, 1050 at 96 kHz
#endif
  DLine<kLine> l, r;
  float lp_l = 0.0f, lp_r = 0.0f;
  Smooth dl, dr, fb, damp;   // ramped per block (Smooth::Ramp)
  static float T60(uint8_t v) { return 0.2f * powf(40.0f, (float)(v & 63) / 63.0f); }   // P2: seconds
  void Clear() {
    l.Clear();
    r.Clear();
    lp_l = lp_r = 0.0f;
    fresh = true;
  }
  bool fresh = true;         // started from silence: the ramped values begin at their targets (not 0: a delay of 0 and no damping)
  int key = -1;              // P2 | P3 << 7 | P5 << 14 of the targets below (their powf / cosf once a change)
  float c_dl = 0.0f, c_dr = 0.0f, c_fb = 0.0f;
  FX_DSP void Targets(const uint8_t* k, float sr) {
    float f0 = 55.0f * exp2f((float)(k[0] > 127 ? 127 : k[0]) / 24.0f);
    float t_dl = sr / f0, t_dr = t_dl * (1.0f + 0.03f * knob01(k[3]));
    if (t_dr > kLine - 2) t_dr = kLine - 2;
    float t60 = T60(k[1]), g60 = powf(10.0f, -3.0f * (t_dl / sr) / t60);
    // the linear interpolation's loss at the sounding f0 each pass (|1 - f + f e^-jw|; ~3 dB/s at 550 Hz) made up,
    // so the fundamental rings for T60
    float fr = t_dl - floorf(t_dl), cw = cosf(6.2831853f * f0 / sr * ((k[1] & 127) >= 64 ? 0.5f : 1.0f));
    g60 /= sqrtf(1.0f - 2.0f * fr * (1.0f - fr) * (1.0f - cw));
    if (g60 > 0.9995f) g60 = 0.9995f;
    c_dl = t_dl, c_dr = t_dr, c_fb = g60 * ((k[1] & 127) >= 64 ? -1.0f : 1.0f);
  }
  FX_DSP void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float a) {
    const int kk = (k[0] & 127) | (k[1] & 127) << 7 | (k[3] & 127) << 14;
    if (kk != key) key = kk, Targets(k, sr);
    const float t_dl = c_dl, t_dr = c_dr;
    const float t_fb = c_fb, t_damp = 0.95f * knob01(k[2]);
    if (fresh) dl.y = t_dl, dr.y = t_dr, fb.y = t_fb, damp.y = t_damp, fresh = false;
    float d1 = dl.y, d2 = dr.y, g = fb.y, dm = damp.y, gi = sqrtf(1.0f - g * g);
    const float s1 = dl.Ramp(t_dl, a, n), s2 = dr.Ramp(t_dr, a, n), sg = fb.Ramp(t_fb, a, n), sd = damp.Ramp(t_damp, a, n);
    const float sgi = (sqrtf(1.0f - fb.y * fb.y) - gi) * (1.0f / (float)n);   // the input's sqrt(1 - g^2), ramped
    float pl = lp_l, pr = lp_r;
    if (s1 == 0.0f && s2 == 0.0f) {   // the delays settled (the usual case): their split once a block
      const float c1 = fminf((float)(kLine - 2), fmaxf(1.0f, d1)), c2 = fminf((float)(kLine - 2), fmaxf(1.0f, d2));
      const float f1 = floorf(c1), f2 = floorf(c2), r1 = c1 - f1, r2 = c2 - f2;
      const uint32_t i1 = (uint32_t)f1, i2 = (uint32_t)f2;
      for (int i = 0; i < n; ++i) {
        g += sg, dm += sd, gi += sgi;
        float x = gi * in[i];
        float yl = l.ReadAt(i1, r1), yr = r.ReadAt(i2, r2);
        pl = yl + (pl - yl) * dm;
        pr = yr + (pr - yr) * dm;
        l.Write(x + g * pl);
        r.Write(x + g * pr);
        out[i][0] += yl;
        out[i][1] += yr;
      }
      lp_l = flush(pl), lp_r = flush(pr);
      return;
    }
    for (int i = 0; i < n; ++i) {
      d1 += s1, d2 += s2, g += sg, dm += sd, gi += sgi;
      float x = gi * in[i];
      float yl = l.Read(d1), yr = r.Read(d2);
      pl = yl + (pl - yl) * dm;
      pr = yr + (pr - yr) * dm;
      l.Write(x + g * pl);
      r.Write(x + g * pr);
      out[i][0] += yl;
      out[i][1] += yr;
    }
    lp_l = flush(pl), lp_r = flush(pr);
  }
};

// a sine, x a phase in [0, 1): 5th-order odd polynomial per quarter (error ~1e-4)
inline float fast_sin(float p) {
  float x = 1.0f - 4.0f * fabsf(p - 0.5f);   // triangle -1..1 (-1 at 0: the sine's phase - 1/4)
  float x2 = x * x;
  return x * (1.5707963f - x2 * (0.6459640f - x2 * 0.0794693f));
}

// Crush: a sample-rate reducer and bitcrusher (no memory). P2..P5 (0 = clean; k[0..3]): P2 rate, the hold
// 1..64 samples (2^(6 v/127): 48 kHz .. 750 Hz; R's hold runs half a period behind L's: width), P3 bits 16..1
// (16 - 15 v/127, continuous: the step 2^(1 - bits)), P4 drive 0..+24 dB before the quantizer (soft-limited at
// +-1, the level made up by 1/sqrt(gain)), P5 tone: a one-pole low-pass after, 18 kHz .. 400 Hz (exp).
struct Crush {
  float ph = 0.0f, ql = 0.0f, qr = 0.0f, lp_l = 0.0f, lp_r = 0.0f;
  bool half = false;
  Smooth step, drive, tone;   // per block: the quantizer and the drive act on held samples only
  int key = -1;
  float c_inc = 1.0f, c_step = 0.0f, c_drive = 1.0f, c_tone = 1.0f;
  void Clear() { ph = ql = qr = lp_l = lp_r = 0.0f, half = false; }
  FX_DSP void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float a) {
    const int kk = (k[0] & 127) | (k[1] & 127) << 7 | (k[2] & 127) << 14 | (k[3] & 127) << 21;
    if (kk != key) {   // the targets' exp2f / expf once a knob change
      key = kk;
      c_inc = exp2f(-6.0f * knob01(k[0]));   // 1 / the hold
      c_step = exp2f(1.0f - (16.0f - 15.0f * knob01(k[1]))), c_drive = exp2f(4.0f * knob01(k[2]));
      const float fc = 18000.0f * exp2f(-5.4919f * knob01(k[3]));   // 18 kHz * (400 / 18000)^v
      c_tone = 1.0f - expf(-6.2831853f * fc / sr);
    }
    const float inc = c_inc, t_step = c_step, t_drive = c_drive, t_tone = c_tone;
    float tn = tone.y;
    const float stn = tone.Ramp(t_tone, a, n);
    step.Ramp(t_step, a, n), drive.Ramp(t_drive, a, n);
    const float st = step.y, ist = 1.0f / st, dr = drive.y, comp = 1.0f / sqrtf(dr);
    float p = ph, yl = lp_l, yr = lp_r, hl = ql, hr = qr;
    bool h = half;
    for (int i = 0; i < n; ++i) {
      tn += stn;
      p += inc;
      if (p >= 0.5f && !h) {   // R: half a hold after L
        h = true;
        hr = st * floorf(soft_limit(in[i] * dr, 1.0f) * comp * ist + 0.5f);
      }
      if (p >= 1.0f) {
        p -= 1.0f, h = false;
        hl = st * floorf(soft_limit(in[i] * dr, 1.0f) * comp * ist + 0.5f);
      }
      yl += (hl - yl) * tn;
      yr += (hr - yr) * tn;
      out[i][0] += yl;
      out[i][1] += yr;
    }
    ph = p, half = h, ql = hl, qr = hr, lp_l = flush(yl), lp_r = flush(yr);
  }
};

// RingMod: the input times a carrier per side (no memory). P2..P5 (0 = the default; k[0..3]): P2 freq 10 Hz *
// 2^(9 v/127) (10 .. 5120 Hz, exp), P3 spread (R's carrier up to a fifth higher: x 1..1.5), P4 shape sine .. near
// square ((1 + s) c / (1 + s |c|), s 0..20), P5 LFO: 0 off, else the carrier swept +-6 semitones by a sine at
// 0.1 .. 10 Hz (squared).
struct RingMod {
  float pl = 0.0f, pr = 0.0f, lfo = 0.0f;
  Smooth freq, ratio, shape, depth;
  int key = -1;
  float c_freq = 10.0f;
  void Clear() { pl = pr = lfo = 0.0f; }
  FX_DSP void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float a) {
    if (k[0] != key) key = k[0], c_freq = 10.0f * exp2f(9.0f * knob01(k[0]));   // exp2f once a change
    const float t_freq = c_freq, t_ratio = 1.0f + 0.5f * knob01(k[1]),
                t_shape = 20.0f * knob01(k[2]) * knob01(k[2]), t_depth = k[3] ? 0.5f : 0.0f;
    const float lrate = 0.1f + 9.9f * knob01(k[3]) * knob01(k[3]);
    // the LFO's sweep per block (2^(depth sin)): one exp2f a block, not a frame
    lfo += lrate * (float)n / sr;
    if (lfo >= 1.0f) lfo -= floorf(lfo);
    depth.Ramp(t_depth, a, n);
    const float dp = depth.y, isr = 1.0f / sr;
    const float sweep = dp > 0.0f ? exp2f(dp * fast_sin(lfo)) : 1.0f;
    float f = freq.y * sweep * isr, rt = ratio.y, sh = shape.y;
    const float sf = freq.Ramp(t_freq, a, n) * sweep * isr, srt = ratio.Ramp(t_ratio, a, n), ssh = shape.Ramp(t_shape, a, n);
    const bool shaped = sh > 0.0f || shape.y > 0.0f;
    float a_l = pl, a_r = pr;
    for (int i = 0; i < n; ++i) {
      f += sf, rt += srt, sh += ssh;
      a_l += f;
      a_l -= floorf(a_l);
      a_r += f * rt;
      a_r -= floorf(a_r);
      float cl = fast_sin(a_l), cr = fast_sin(a_r);
      if (shaped) {
        cl = (1.0f + sh) * cl / (1.0f + sh * fabsf(cl));
        cr = (1.0f + sh) * cr / (1.0f + sh * fabsf(cr));
      }
      out[i][0] += in[i] * cl;
      out[i][1] += in[i] * cr;
    }
    pl = a_l, pr = a_r;
  }
};

}  // namespace fx
}  // namespace core
#include "fx/halo_fx.h"   // HALO: uses Smooth, knob01, fast_sin, flush above
#if RYTM_SENDFX_LARGE
#include "fx/tape_fx.h"   // SPACE: uses Smooth, knob01, fast_sin, flush, soft_limit above and ClockWord
#endif
namespace core {
namespace fx {

// The small types share one block per slot (only one TYPE plays in a slot at a time: the old fades out before the
// new starts): the new one is built and cleared in place when it starts (SendFx::Enter), never marked dirty. HALO
// fits in what CHORUS + COMB + CRUSH + RINGMOD took side by side, so it cost no RAM.
inline constexpr bool small_type(int t) { return t == kChorus || t == kComb || t == kCrush || t == kRingMod || t == kHalo; }
union Small {
  Chorus chorus;
  Comb comb;
  Crush crush;
  RingMod ring;
  Halo halo;
  Small() {}   // nothing built: Enter places the playing one
};
#if RYTM_SENDFX_LINE16
static_assert(sizeof(Halo) <= sizeof(Chorus) + sizeof(Comb) + sizeof(Crush) + sizeof(RingMod),
              "HALO must fit in the small set's old footprint (the Teensy's RAM1)");
#endif

class SendFx {
 public:
  static const int kMaxFrames = 128;   // per Process call
  explicit SendFx(float sample_rate = 48000.0f) : sr_(sample_rate) {
    a_ = 1.0f - expf(-1.0f / (0.010f * sample_rate));   // 10 ms
    Reset();
  }
  void Reset() {
    for (int s = 0; s < kSlots; ++s) {   // the small types: built and cleared when one starts (Enter)
#if RYTM_SENDFX_LARGE
      st_[s].rings.Clear();
      st_[s].cloud.Clear();
      st_[s].tape.Clear();
#endif
      cur_[s] = kOff, zone_[s] = -1, fade_[s] = 0, dirty_[s] = 0;
    }
    for (int v = 0; v < kStems; ++v) gain_[0][v] = gain_[1][v] = Smooth();
    kill_seen_ = 0, kill_[0] = kill_[1] = false, pan_set_ = false;
#if RYTM_SENDFX_LARGE
    stutter_.Clear();
    stut_on_ = false;
#endif
  }
  // n frames (<= kMaxFrames) of the 8 stems (full scale 1.0) -> out[n] L/R (overwritten): the PRE pair (host -> Rytm 2/3,
  // limited kLimitFx) and main[n] L/R (overwritten): the POST pair (0/1, kLimitMain), each slot's effect on the pair its
  // P1 (or c.route) names, crossfading when that changes. STUTTER's insert (unlimited, mono onto both channels) goes
  // after the limit onto its slot's pair. main null: both pairs are summed into out (the single-pair callers).
  // slots (optional, overwritten): each slot's own return before the limit, FX 1 L/R then FX 2 L/R, STUTTER's on its
  // slot (the Pi's recorder: its FX1/FX2 tracks).
  // vsend (optional): the Sidecar voices' own sends into each FX's input, per frame (insert.h voice_sends; 0 none)
  FX_DSP void Process(const float (*stems)[kStems], int n, float (*out)[2], const Control& c, float (*main)[2] = nullptr,
               float (*slots)[4] = nullptr, const float (*vsend)[3] = nullptr) {
    if (n > kMaxFrames) n = kMaxFrames;
    float tg[kSlots][kStems];
    for (int s = 0; s < kSlots; ++s)
      for (int v = 0; v < kStems; ++v) {
        int a = c.send[kStemTracks[v][0]][s], b = kStemTracks[v][1] >= 0 ? c.send[kStemTracks[v][1]][s] : 0;
        float x = knob01((uint8_t)(a > b ? a : b));
        tg[s][v] = x * x;   // squared: the knob's travel in dB-ish steps
      }
    float post_l[kMaxFrames][2];
    float (*post)[2] = main ? main : post_l;
    for (int i = 0; i < n; ++i) out[i][0] = out[i][1] = post[i][0] = post[i][1] = 0.0f;
    if (slots)
      for (int i = 0; i < n; ++i) slots[i][0] = slots[i][1] = slots[i][2] = slots[i][3] = 0.0f;
    float pb[kSlots][kMaxFrames];   // each slot's POST share per frame: 0 = 2/3, 1 = 0/1
    for (int s = 0; s < kSlots; ++s) {
      float tgt = c.route == kRouteFx ? 0.0f : c.route == kRouteMain ? 1.0f : c.knob[s][1] >= 64 ? 1.0f : 0.0f;
      if (!pan_set_) pan_[s] = tgt;   // the first block: on its pair at once, no crossfade from a default
      float pn = pan_[s];
      if (pn == tgt)
        for (int i = 0; i < n; ++i) pb[s][i] = pn;
      else
        for (int i = 0; i < n; ++i) pn += fminf(kPanStep, fmaxf(-kPanStep, tgt - pn)), pb[s][i] = pn;
      pan_[s] = pn;
    }
    pan_set_ = true;
    bool stut = false;   // a slot plays STUTTER (or fades it): gs_ its fade gain
    bool any = false;    // a slot ran its effect this block
    bool upre = false, upost = false;   // the pairs a slot's return reached this block (the other's limit skipped)
    (void)stut;
#if RYTM_SENDFX_LARGE
    for (int i = 0; i < n; ++i) gs_[i] = 0.0f;
#endif
    if (c.kill != kill_seen_) {   // All Sound Off: both FX leave like a TYPE change and come back to the same TYPE
      kill_seen_ = c.kill;
      for (int s = 0; s < kSlots; ++s) kill_[s] = plays(cur_[s]) && cur_[s] != kStutter;
#if RYTM_SENDFX_LARGE
      stutter_.Kill();   // STUTTER: the repeat drops to dry, the mix stays
#endif
    }
    int sslot = stutter_slot(c);
    // the send mixes, both slots in one pass when both (likely) run: the stems loaded once, two independent chains
    float xs[kSlots][kMaxFrames];
    bool have[kSlots] = {false, false};
    {
      int ss[kSlots], ns = 0;
      for (int s = 0; s < kSlots; ++s)
        if ((plays(cur_[s]) && cur_[s] != kStutter) || (plays(c.knob[s][0]) && c.knob[s][0] != kStutter)) ss[ns++] = s;
      if (ns) Mix(stems, n, tg, ss, ns, xs);
      for (int k2 = 0; k2 < ns; ++k2) have[ss[k2]] = true;
      if (vsend)
        for (int k2 = 0; k2 < ns; ++k2)
          for (int i = 0; i < n; ++i) xs[ss[k2]][i] += vsend[i][ss[k2]];
    }
    for (int s = 0; s < kSlots; ++s) {
      uint8_t want = c.knob[s][0];
      if (want == kStutter && s != sslot) want = kOff;   // the other slot has it: this one silent (MISSING)
      int want_zone = structure_of(want, c.knob[s]);
      bool same = want == cur_[s] && want_zone == zone_[s] && !kill_[s];
      if (!same && fade_[s] <= 0) {
        kill_[s] = false;   // faded out (or silent): the left effect to be cleared, the new from clean
        if (plays(cur_[s]) && !small_type(cur_[s])) dirty_[s] |= Bit(cur_[s]);
        cur_[s] = kOff;
        if (!plays(want) || !(dirty_[s] & Bit(want))) {   // else: waits
          cur_[s] = want, zone_[s] = (int8_t)want_zone;
          if (small_type(want)) Enter(s, want);   // from clean, in the shared block
        }
        same = want == cur_[s] && want_zone == zone_[s];
      }
      ClearSome(s, n * kClearBytesPerFrame);
      int t = cur_[s];
      if (!plays(t)) continue;   // off, an effect this build lacks, or waiting for its clear: silence
#if RYTM_SENDFX_LARGE
      if (t == kStutter) {   // the insert: only its fade here, the DSP once below
        for (int i = 0; i < n; ++i) {
          int f = fade_[s] + (same ? 1 : -1);
          fade_[s] = f < 0 ? 0 : f > kFadeFrames ? kFadeFrames : f;
          gs_[i] += (float)fade_[s] * (1.0f / kFadeFrames);
        }
        stut = true;
        continue;
      }
#endif
      if (!have[s]) {   // not foreseen above (a TYPE that just came)
        Mix(stems, n, tg, &s, 1, xs);
        if (vsend)
          for (int i = 0; i < n; ++i) xs[s][i] += vsend[i][s];
      }
      const float* x = xs[s];
      float y[kMaxFrames][2];
      float xf[kMaxFrames];
      for (int i = 0; i < n; ++i) y[i][0] = y[i][1] = 0.0f;
      any = true;
      {   // the TYPE fade: down while another TYPE (or structure zone) waits, up otherwise
        const int step = same ? 1 : kill_[s] ? -2 : -1;   // All Sound Off: out in 5 ms
        int f = fade_[s];
        if (step > 0 && f >= kFadeFrames)   // fully in (the usual case): unity
          for (int i = 0; i < n; ++i) g_[i] = 1.0f;
        else
          for (int i = 0; i < n; ++i) {
            f += step;
            f = f < 0 ? 0 : f > kFadeFrames ? kFadeFrames : f;
            g_[i] = (float)f * (1.0f / kFadeFrames);
          }
        fade_[s] = f;
        if (!(step > 0 && f >= kFadeFrames && g_[n - 1] == 1.0f && g_[0] == 1.0f)) {
          // the input fades with the output: a delay-based effect would else bring the input's first sample out after its
          // delay as a hard onset (the fade-in long over by then)
          for (int i = 0; i < n; ++i) xf[i] = x[i] * g_[i];
          x = xf;
        }
      }
      uint8_t k[kKnobs];
      for (int j = 0; j < kKnobs; ++j) k[j] = c.knob[s][j];
      if (int j = structure_knob(t)) k[j] = (uint8_t)(zone_[s] << 5 | (k[j] & 31));   // the zone playing until its fade-out ends
      switch (t) {
        case kChorus: st_[s].sm.chorus.Process(x, n, y, k + 2, sr_, a_); break;
        case kComb: st_[s].sm.comb.Process(x, n, y, k + 2, sr_, a_); break;
        case kCrush: st_[s].sm.crush.Process(x, n, y, k + 2, sr_, a_); break;
        case kRingMod: st_[s].sm.ring.Process(x, n, y, k + 2, sr_, a_); break;
        case kHalo: st_[s].sm.halo.Process(x, n, y, k + 2, sr_, a_); break;
#if RYTM_SENDFX_LARGE
        case kRings: st_[s].rings.Process(x, n, y, k + 2, sr_, a_); break;
        case kCloudSeed: st_[s].cloud.Process(x, n, y, k + 2, sr_, a_); break;
        case kTape: st_[s].tape.Process(x, n, y, k + 2, sr_, a_, c.clock); break;
#endif
        default: break;
      }
      if (bus_insert) bus_insert(s, y, n);   // the bus's insert chain (core/fx/insert.h)
      upre |= pb[s][0] < 1.0f || pb[s][n - 1] < 1.0f;
      upost |= pb[s][0] > 0.0f || pb[s][n - 1] > 0.0f;
      const float q0 = pb[s][0], q1 = pb[s][n - 1];
      if (slots)   // the slot's own return (the Pi's recorder)
        for (int i = 0; i < n; ++i) slots[i][2 * s] += g_[i] * y[i][0], slots[i][2 * s + 1] += g_[i] * y[i][1];
      if (q0 == q1 && (q0 == 0.0f || q0 == 1.0f)) {   // on one pair the whole block (the usual case)
        float (*d)[2] = q0 == 0.0f ? out : post;
        for (int i = 0; i < n; ++i) d[i][0] += g_[i] * y[i][0], d[i][1] += g_[i] * y[i][1];
      } else
        for (int i = 0; i < n; ++i) {   // crossfading between the pairs
          float l = g_[i] * y[i][0], r = g_[i] * y[i][1], q = pb[s][i];
          out[i][0] += (1.0f - q) * l, out[i][1] += (1.0f - q) * r, post[i][0] += q * l, post[i][1] += q * r;
        }
    }
    if (!any && !stut) return;   // nothing played: both pairs stay the zeros written above
    float ms[kMaxFrames];   // STUTTER's output (mono)
    for (int i = 0; i < n; ++i) ms[i] = 0.0f;
#if RYTM_SENDFX_LARGE
    if (stut && !stut_on_) stutter_.Restart();   // selected: a fresh capture, dry
    stut_on_ = stut;
    if (stut) {
      float x[kMaxFrames], y[kMaxFrames];
      for (int i = 0; i < n; ++i) {   // the plain sum, stem order
        float sum = 0.0f;
        for (int v = 0; v < kStems; ++v) sum += stems[i][v];
        x[i] = sum;
      }
      uint8_t k[4];
      int ks = sslot >= 0 ? sslot : cur_[0] == kStutter ? 0 : 1;   // fading out: the slot that had it
      for (int j = 0; j < 4; ++j) k[j] = c.knob[ks][2 + j];
      stutter_.Process(x, n, y, c.clock, c.stutter_lag, k);
      for (int i = 0; i < n; ++i) {
        float v = gs_[i] == 1.0f ? y[i] : gs_[i] * y[i];
        if (!(v == v) || v > 1e6f || v < -1e6f) v = 0.0f, ++faults_;
        ms[i] = v;
      }
    }
#endif
    int ks = stutter_slot(c);
    if (ks < 0) ks = cur_[0] == kStutter ? 0 : 1;   // fading out: the slot that had it
    // the limits: NaN or a runaway checked once a block on the sums, the peak from the block's largest |x| (the limit
    // is monotonic), each pair's limit only if a slot reached it, in loops of independent chains (the M7 is in order)
    float c0 = 0.0f, c1 = 0.0f, m0 = 0.0f, m1 = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float a0 = fabsf(out[i][0]), a1 = fabsf(out[i][1]), b0 = fabsf(post[i][0]), b1 = fabsf(post[i][1]);
      c0 += a0 + a1, c1 += b0 + b1;
      m0 = fmaxf(m0, fmaxf(a0, a1)), m1 = fmaxf(m1, fmaxf(b0, b1));
    }
    if (!(c0 + c1 < 1e6f)) {   // NaN or a runaway: both effects cleared, start again, silent
      ++faults_;
      for (int s = 0; s < kSlots; ++s) {
        if (plays(cur_[s]) && !small_type(cur_[s])) dirty_[s] |= Bit(cur_[s]);   // a small one: Enter clears it
        cur_[s] = kOff, fade_[s] = 0;
      }
      for (int j2 = 0; j2 < n; ++j2) out[j2][0] = out[j2][1] = post[j2][0] = post[j2][1] = 0.0f;
      if (slots)
        for (int j2 = 0; j2 < n; ++j2) slots[j2][0] = slots[j2][1] = slots[j2][2] = slots[j2][3] = 0.0f;
      return;
    }
    peak_ = fmaxf(peak_, fmaxf(soft_limit(m0, kLimitFx), soft_limit(m1, kLimitMain)));
    if (upre && m0 <= 0.25f * kLimitFx)   // a quiet block: the series, no division
      for (int i = 0; i < n; ++i) out[i][0] = soft_limit_low(out[i][0], kLimitFx), out[i][1] = soft_limit_low(out[i][1], kLimitFx);
    else if (upre)
      for (int i = 0; i < n; ++i) out[i][0] = soft_limit(out[i][0], kLimitFx), out[i][1] = soft_limit(out[i][1], kLimitFx);
    if (upost && m1 <= 0.25f * kLimitMain)
      for (int i = 0; i < n; ++i)
        post[i][0] = soft_limit_low(post[i][0], kLimitMain), post[i][1] = soft_limit_low(post[i][1], kLimitMain);
    else if (upost)
      for (int i = 0; i < n; ++i)
        post[i][0] = soft_limit(post[i][0], kLimitMain), post[i][1] = soft_limit(post[i][1], kLimitMain);
    if (stut)
      for (int i = 0; i < n; ++i) {   // STUTTER's insert, unlimited, onto its slot's pair
        const float q = pb[ks][i], v = ms[i];
        out[i][0] += (1.0f - q) * v, out[i][1] += (1.0f - q) * v;
        post[i][0] += q * v, post[i][1] += q * v;
        if (slots) slots[i][2 * ks] += v, slots[i][2 * ks + 1] += v;
      }
    if (!main)
      for (int i = 0; i < n; ++i) out[i][0] += post[i][0], out[i][1] += post[i][1];
  }
#if RYTM_SENDFX_LARGE
  const Stutter& stutter() const { return stutter_; }
#endif
  float TakePeak() {
    float p = peak_;
    peak_ = 0.0f;
    return p;
  }
  uint32_t Faults() const { return faults_; }
  bool Clearing(int s) const { return dirty_[s] != 0; }   // a left effect still being zeroed

 private:
  static uint32_t Bit(int t) { return 1u << (t & 31); }
  // the send mix of slots ss[0..ns-1] (ns 1 or 2) into xs: each stem's level ramped linearly over the block
  // (Smooth::Ramp); a slot with every level 0 gets zeros
  FX_DSP void Mix(const float (*stems)[kStems], int n, const float (*tg)[kStems], const int* ss, int ns, float (*xs)[kMaxFrames]) {
    float g[kSlots][kStems], dg[kSlots][kStems];
    bool on[kSlots] = {false, false};
    for (int k = 0; k < ns; ++k)
      for (int v = 0; v < kStems; ++v) {
        const int s = ss[k];
        g[k][v] = gain_[s][v].y, dg[k][v] = gain_[s][v].Ramp(tg[s][v], a_, n);
        on[k] |= g[k][v] != 0.0f || gain_[s][v].y != 0.0f;
      }
    if (ns == 2 && on[0] && on[1]) {
      float* x0 = xs[ss[0]];
      float* x1 = xs[ss[1]];
      for (int i = 0; i < n; ++i) {
        float p0 = 0.0f, q0 = 0.0f, p1 = 0.0f, q1 = 0.0f;
#pragma GCC unroll 8
        for (int v = 0; v < kStems; v += 2) {
          const float a = stems[i][v], b = stems[i][v + 1];
          g[0][v] += dg[0][v], g[0][v + 1] += dg[0][v + 1], g[1][v] += dg[1][v], g[1][v + 1] += dg[1][v + 1];
          p0 += a * g[0][v], q0 += b * g[0][v + 1], p1 += a * g[1][v], q1 += b * g[1][v + 1];
        }
        x0[i] = p0 + q0, x1[i] = p1 + q1;
      }
      return;
    }
    for (int k = 0; k < ns; ++k) {
      float* x = xs[ss[k]];
      if (!on[k]) {
        for (int i = 0; i < n; ++i) x[i] = 0.0f;
        continue;
      }
      float* gk = g[k];
      const float* dk = dg[k];
      for (int i = 0; i < n; ++i) {
        float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
#pragma GCC unroll 8
        for (int v = 0; v < kStems; ++v) gk[v] += dk[v], p[v & 3] += stems[i][v] * gk[v];
        x[i] = (p[0] + p[1]) + (p[2] + p[3]);
      }
    }
  }
  // zero one left (dirty) effect of slot s, at most `bytes` this call; never the one playing
  FX_DSP void ClearSome(int s, int bytes) {
    uint32_t d = dirty_[s];
    if (!d) return;
    int t = 0;
    while (!(d & Bit(t))) ++t;
    bool done = true;
    switch (t) {   // the small types are never dirty (Enter clears them)
#if RYTM_SENDFX_LARGE
      case kRings: done = st_[s].rings.ClearStep(bytes); break;
      case kCloudSeed: done = st_[s].cloud.ClearStep(bytes); break;
      case kTape: done = st_[s].tape.ClearStep(bytes); break;
#endif
      default: break;
    }
    if (done) dirty_[s] &= ~Bit(t);
  }
  // the small type t of slot s starts: built in the shared block and cleared (<= 8.3 KB on the Teensy, 16 KB with
  // float lines: one memset, as a left CHORUS's clear was); placement new builds no heap object
  FX_DSP void Enter(int s, int t) {
    Small& u = st_[s].sm;
    switch (t) {
      case kChorus: new (&u.chorus) Chorus(), u.chorus.Clear(); break;
      case kComb: new (&u.comb) Comb(), u.comb.Clear(); break;
      case kCrush: new (&u.crush) Crush(), u.crush.Clear(); break;
      case kRingMod: new (&u.ring) RingMod(), u.ring.Clear(); break;
      case kHalo: new (&u.halo) Halo(), u.halo.Clear(); break;
      default: break;
    }
  }
  struct State {   // every large type kept per slot (a type change needs no construction there); the small ones share
    Small sm;
#if RYTM_SENDFX_LARGE
    RingsFx rings;
    CloudSeedFx cloud;
    TapeFx tape;
#endif
  };
  State st_[kSlots];
#if RYTM_SENDFX_LARGE
  Stutter stutter_;        // one, for the one STUTTER slot
  float gs_[kMaxFrames];
  bool stut_on_ = false;
#endif
  uint8_t kill_seen_ = 0;
  bool kill_[kSlots];      // All Sound Off pending: fading out, then cleared
  uint8_t cur_[kSlots];    // the TYPE playing
  int8_t zone_[kSlots];    // its structure zone (RINGS P3, CLOUDSEED P2), -1 for none
  uint32_t dirty_[kSlots]; // Bit(type): left, not yet zeroed
  int fade_[kSlots];       // 0..kFadeFrames
  bool pan_set_ = false;
  float pan_[kSlots] = {0.0f, 0.0f};   // 0 = PRE (2/3) .. 1 = POST (0/1), per kPanStep
  float g_[kMaxFrames];
  Smooth gain_[kSlots][kStems];
  static constexpr float kPanStep = 1.0f / kPanFrames;
  float sr_, a_, peak_ = 0.0f;
  uint32_t faults_ = 0;
};

}  // namespace fx
}  // namespace core
