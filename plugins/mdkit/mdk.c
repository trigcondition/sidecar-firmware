// plugins/mdkit/mdk.c - one family of mdkit's drum voices (families.h, the Makefile's FAM) as one machine:
// P9 TYPE (a list knob) picks the voice, P1..P8 are that voice's own controls 1:1 (0..127 straight into
// set_control; a voice with fewer leaves the rest inert), P10 LEV the output trim. A trig is the voice's trigger,
// its velocity the hit's level. MD (MDK_FAM_MD: the three families in one machine): P9 ENGINE (FM, PI, TRX) picks the
// family, P10 TYPE the voice among that ENGINE's (zones of 128 / its TYPE count, as the host's RLB2 block shows them),
// P11 LEV; an ENGINE change is a TYPE change (the voice index runs over all 30) and only the voice set up renders. MIT, Copyright 2026 trigcondition and contributors; the voices are mdkit's
// (https://github.com/soejrd/mdkit, CC0 1.0), compiled unchanged but for the PI cymbals' memory window (Makefile).
//
// Rate: mdkit's voices run at their own 44 100 Hz (their arithmetic and timing untouched): render() makes the host's
// 48 kHz from them with a polyphase windowed-sinc resampler (147/160, mdk_fir.h: 32 taps, 160 phases), its history
// the state's tap ring (mdk_tap.h), so a block of n output frames renders the ~n x 0.92 core samples it needs.
// Latency: 16 core samples (0.36 ms).
//
// TYPE: the voices' states share one union. A change of TYPE while the old voice may still ring fades the output out
// over kFade output samples (2 ms) and keeps the new TYPE pending; the next params() or note_on() after the fade (or a
// trig at once) sets the new voice up: its state zeroed and init(), then P1..P8 from the knobs, the history cleared.
// Nothing is allocated (there is no allocator: the package links no host symbols), nothing outside the state written.
//
// Threading: the host calls voice_init, params, note_on and note_off on its control core and render on a render core,
// never at the same time for one voice (the SDK's lockstep: core/user/um.cpp runs a voice's calls between its blocks),
// so the state needs no locks or atomics. setup() (a TYPE change) costs a zeroing of the new voice's state and mdkit's
// init (tests/mdk_check.cpp prints it per TYPE).
#include <stdint.h>
#include <string.h>
#include "rmp/rmp_api.h"
#include "mdkit.h"
#include "families.h"
#include "mdk_tap.h"
#include "mdk_fir.h"

#define MDK_STATE(v) mdkit_##v v;
#define MDK_TABLE(v) &mdkit_voice_##v,
#define MDK_COUNT(v) +1
static const mdkit_voice *const mdk_voices[] = {MDK_VOICES(MDK_TABLE)};
enum { kTypes = 0 MDK_VOICES(MDK_COUNT), kFade = 96, kKnobs = 8 };

typedef struct mdk {
  mdk_tap tap;                  // first: the core samples (the tests read it)
  int32_t type;                 // the voice set up (0..kTypes-1)
  int32_t want;                 // a TYPE waiting for the fade (-1 none)
  int32_t hit;                  // triggered since its set-up (a change then fades)
  int32_t ctl[kKnobs];          // the value last given to set_control (-1: none yet)
  int32_t knob[kKnobs];         // P1..P8 as the host last sent them (-1: not yet)
  uint32_t phase;               // the next output's position: base + phase / MDK_FIR_PHASES core samples
  uint64_t base;
  float level, vel, gain, fade;
#ifdef MDK_ENGINES
  // MD (review 2): a TYPE or ENGINE change that comes with its trig (a p-locked step) cut the ringing voice in one
  // sample. note_on renders the old voice's next kFade output samples first (it fades out over them, as without a
  // trig), and render mixes them under the new hit. MD only: MD-PI/EFM/TRX stay as they were, byte for byte.
  float tail[kFade];
  int32_t tail_n, tail_at;
#endif
  _Alignas(8) union { MDK_VOICES(MDK_STATE) } u;   // (8-aligned: zero8)
} mdk;
_Static_assert(_Alignof(mdk) <= 8, "the host aligns a voice's state to 8 bytes");

// 48 kHz, and blocks the tap ring holds (a block of n needs ~0.92 n new core samples next to the FIR's 32)
static int zone(float f, int n) {
  int z = f >= 0.0f && f <= 127.0f ? (int)f : f > 127.0f ? 127 : 0;   // NaN: 0
  return (z * n) >> 7;
}

#ifdef MDK_ENGINES
// MD: the voice P9 ENGINE and P10 TYPE mean. TYPE keeps its raw value across an ENGINE change and is read in the new
// ENGINE's zones (the same relative place along the list: 0 is BD in every ENGINE, 127 the last), so what the
// Rytm shows (the host's RLB2 count for the ENGINE) and what plays always agree.
#define MDK_N(n) n,
#define MDK_ONE(n) +1
#define MDK_ADD(n) +n
static const uint8_t mdk_count[] = {MDK_ENGINES(MDK_N)};
enum { kEngines = 0 MDK_ENGINES(MDK_ONE), kLevel = 10 };
_Static_assert(0 MDK_ENGINES(MDK_ADD) == kTypes, "the ENGINEs' TYPE counts add up to the voices");
static int type_of(const float *p) {
  int e = zone(p[8], kEngines), b = 0;
  for (int i = 0; i < e; ++i) b += mdk_count[i];
  return b + zone(p[9], mdk_count[e]);
}
#else
enum { kLevel = 9 };
static int type_of(const float *p) { return zone(p[8], kTypes); }   // P9 TYPE
#endif

static int32_t md_init(const rmp_host *h) { return h->sample_rate == 48000.0f && h->max_block >= 1 && h->max_block <= 24 ? 0 : 1; }

// Zeroes n bytes at p (8-aligned: the union in the 8-aligned state), eight at a time: the SDK shim's memset is a byte
// loop, and a set-up runs on the control core (review I2).
static void zero8(void *p, uint32_t n) {
  uint64_t *q = (uint64_t *)p;
  for (uint32_t i = 0; i < n / 8u; ++i) q[i] = 0;
  for (uint32_t i = n & ~7u; i < n; ++i) ((uint8_t *)p)[i] = 0;
}

static void setup(mdk *s, int t) {   // TYPE t from scratch: as mdkit's own render starts a voice (calloc + init)
  const mdkit_voice *v = mdk_voices[t];
  zero8(&s->u, (uint32_t)v->state_bytes);   // its own state only (calloc's), not the family's largest
  v->init(&s->u);
  for (int k = 0; k < kKnobs; ++k) {
    s->ctl[k] = -1;
    if (k < (int)v->control_count && s->knob[k] >= 0) v->set_control(&s->u, (unsigned)k, (unsigned)(s->ctl[k] = s->knob[k]));
  }
  zero8(s->tap.ring, sizeof s->tap.ring);
  s->tap.produced = 0, s->tap.epoch++, s->tap.type = (uint32_t)t;
  s->base = 0, s->phase = 0;
  s->type = t, s->want = -1, s->hit = 0, s->fade = 1.0f;
}

static void md_voice_init(void *st) {
  mdk *s = (mdk *)st;
  s->tap.magic = MDK_TAP_MAGIC, s->tap.ring_mask = MDK_RING - 1;
  for (int k = 0; k < kKnobs; ++k) s->knob[k] = -1;
  s->level = 100.0f / 127.0f, s->vel = 1.0f, s->gain = 0.0f;
  setup(s, 0);
}

static void md_params(void *st, const float *p) {
  mdk *s = (mdk *)st;
  for (int k = 0; k < kKnobs; ++k) {
    float f = p[k];
    s->knob[k] = f >= 0.0f && f <= 127.0f ? (int)f : f > 127.0f ? 127 : 0;
  }
  float lv = p[kLevel];
  s->level = (lv >= 0.0f && lv <= 127.0f ? lv : lv > 127.0f ? 127.0f : 0.0f) / 127.0f;
  int t = type_of(p);
  if (t != s->type) {
    if (!s->hit) { setup(s, t); return; }   // nothing sounding yet: at once
    s->want = t;
  } else if (s->want >= 0) {   // back to the playing TYPE during the fade: it plays on (render fades it back in)
    s->want = -1;
  }
  if (s->want >= 0) {
    if (s->fade <= 0.0f) setup(s, s->want);   // faded out: the new voice now (the control core)
    return;
  }
  const mdkit_voice *v = mdk_voices[s->type];
  for (int k = 0; k < (int)v->control_count && k < kKnobs; ++k)
    if (s->knob[k] != s->ctl[k]) v->set_control(&s->u, (unsigned)k, (unsigned)(s->ctl[k] = s->knob[k]));
}

static void md_render(void *st, float *out, int32_t n);

static void md_note_on(void *st, float note, float vel) {
  (void)note;   // pitch none: PTCH is the voice's own knob
  mdk *s = (mdk *)st;
#ifdef MDK_ENGINES
  if (s->want >= 0 && s->hit && s->fade > 0.0f) {   // the old voice may ring: its fade-out, rendered now (8 blocks)
    // (an older tail still playing is read ahead of where this one is written: it carries into the new one)
    for (int j = 0; j < kFade; j += 12) md_render(s, s->tail + j, kFade - j < 12 ? kFade - j : 12);
    s->tail_n = kFade, s->tail_at = 0;
  }
#endif
  if (s->want >= 0) setup(s, s->want);
  s->vel = vel >= 0.0f && vel <= 1.0f ? vel : vel > 1.0f ? 1.0f : 0.0f;
  s->gain = s->level * s->vel;   // a hit's level at once (its attack is not ramped); LEV moves are smoothed
  mdk_voices[s->type]->trigger(&s->u);
  s->hit = 1;
}

static void md_note_off(void *st) { (void)st; }   // drums: the voice's own decay

static void md_render(void *st, float *out, int32_t n) {
  mdk *s = (mdk *)st;
  const mdkit_voice *v = mdk_voices[s->type];
  // the core samples this block needs: up to the last output's base + MDK_FIR_TAPS - MDK_FIR_LEFT - 1
  uint64_t last = s->base + (s->phase + (uint32_t)(n - 1) * 147u) / MDK_FIR_PHASES;
  uint64_t need = last + (MDK_FIR_TAPS - MDK_FIR_LEFT);
  while (s->tap.produced < need) {
    float tmp[32];
    uint64_t m = need - s->tap.produced;
    if (m > 32) m = 32;
    v->render(&s->u, tmp, (unsigned)m);
    for (uint64_t i = 0; i < m; ++i) s->tap.ring[(s->tap.produced + i) & (MDK_RING - 1)] = tmp[i];
    s->tap.produced += m;
  }
  float target = s->level * s->vel;
  for (int32_t j = 0; j < n; ++j) {
    const float *h = mdk_fir[s->phase];
    uint64_t at = s->base - MDK_FIR_LEFT;   // (wraps below 0 at the start: the ring's zeros)
    float acc = 0.0f;
    for (int k = 0; k < MDK_FIR_TAPS; ++k) acc += s->tap.ring[(at + (uint64_t)k) & (MDK_RING - 1)] * h[k];
    s->gain += (target - s->gain) * 0.02f;   // ~1 ms: a LEV change without a step
    if (s->want >= 0) {
      if ((s->fade -= 1.0f / kFade) < 0.0f) s->fade = 0.0f;
    } else if (s->fade < 1.0f) {
      if ((s->fade += 1.0f / kFade) > 1.0f) s->fade = 1.0f;
    }
    if (s->gain < 1e-20f && target == 0.0f) s->gain = 0.0f;   // (no denormals on the way down)
    out[j] = acc * s->gain * s->fade;
#ifdef MDK_ENGINES
    if (s->tail_at < s->tail_n) out[j] += s->tail[s->tail_at++];
#endif
    s->phase += 147u;
    if (s->phase >= MDK_FIR_PHASES) s->phase -= MDK_FIR_PHASES, s->base++;
  }
}

#ifndef MDK_PERMILLE
#define MDK_PERMILLE 200
#endif
RMP_ENTRY const rmp_plugin rmp_entry = {RMP_PLUGIN_MAGIC, RMP_API, sizeof(mdk), MDK_PERMILLE,
  md_init, md_voice_init, md_note_on, md_note_off, md_params, md_render, {0}};

#ifdef MDK_BUILTIN
// The built-in MD (core/md/md.cpp: voice byte 0x40, this file compiled into the core with MDK_FAM_MD, its rmp_entry
// renamed mdk_builtin_entry by core/engines/setup_engines.sh's wrapper): mdkit's own control count and defaults per
// TYPE (0..29, families.h's order), for the Rytm's labels and the DIGIDRUM migration. Not in any package.
int mdk_builtin_types(void) { return kTypes; }
int mdk_builtin_controls(int t) { return t >= 0 && t < kTypes ? (int)mdk_voices[t]->control_count : 0; }
int mdk_builtin_default(int t, int k) {
  if (t < 0 || t >= kTypes || k < 0 || k >= (int)mdk_voices[t]->control_count) return 0;
  return mdk_voices[t]->control_defaults ? mdk_voices[t]->control_defaults[k] : 64;
}
#endif
