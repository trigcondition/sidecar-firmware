// plugins/cloud/cloud.cc - CLOUD, a texture machine for the Pi: an oscillator at the trig's note, recorded into an
// always-running buffer and played back as a granular cloud with feedback, freeze, stereo spread and reverb (the CLOUD
// engine), or Beads' own wavetable grains (the SYNTH engine). The DSP is Beads' (Emilie Gillet, MIT; cloud_engine.h).
//
// Plugin API 3 only (rmp_api3.h): the buffer (768 KB), the reverb (64 KB) and the engine live in the voice's arena
// part, which the 64 KB state cannot hold; the output is stereo on PRE / POST (the mono mix on TRK). A voice state
// holds the knobs and a pointer into the arena. An API 2 host refuses the package at the scan.
//
// The knobs (manifest.json; P1..P8 the SRC page, P9..P16 the second page):
//   P1 TIME  P2 SIZE  P3 DENS  P4 PTCH  P5 SHAP  P6 FDBK  P7 SPRD  P8 MIX
//   P9 RVB   P10 FRZ  P11 QUAL P12 BANK P13 WAVE P14 TRND P15 PRND P16 ENG
// PTCH snaps to Beads' intervals (0, 1, 3, 4, 7, 12, 24 semitones each way) as Beads' pitch pot does; TRND and PRND
// are Beads' attenuverters with nothing patched (the same law: a dead zone at the centre, + smooth, - jumps).
//
// The control calls stay short: params() only maps numbers, voice_attach() places the engine (its buffer is cleared
// 64 KB a render call afterwards, the wet signal faded in once it is), note_on() sets the note and a seed.
// MIT, Copyright 2026 trigcondition and contributors (LICENSE). Beads: Copyright Emilie Gillet (LICENSE.beads).
#include <new>
#include <stdint.h>
#include <string.h>

#include "rmp/rmp_api3.h"
#include "cloud_engine.h"

#ifndef RMP_NATIVE
extern "C" int abs(int x) { return x < 0 ? -x : x; }   // Beads' grains call it; the plugin libc has none
#endif

#ifndef CLOUD_PERMILLE
#define CLOUD_PERMILLE 700
#endif

namespace {

// The arena part of a voice: the engine, then the buffer, then the reverb's memory.
const uint32_t kEngineBytes = (uint32_t)((sizeof(cloud::Engine) + 63u) & ~(size_t)63u);
const uint32_t kArenaBytes = kEngineBytes + (uint32_t)cloud::kBufferBytes + (uint32_t)cloud::kReverbBytes;

struct State {
  cloud::Engine* engine;     // in the arena part; NULL without one (silent)
  cloud::Controls c;
  float p[16];
  int32_t voice;
};
static_assert(sizeof(State) <= 65536, "the state fits the Pi's 64 KB");

// The manifest's defaults (P1..P16)
const float kDefaults[16] = {24, 72, 92, 64, 64, 40, 64, 96, 64, 0, 127, 0, 40, 76, 64, 0};

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Beads' pot laws (beads/cv_reader.h PotChannel, LAW_ATTENUVERTER and LAW_DETUNE), on the knob's 0..1.
inline float attenuverter(float v) {
  v -= 0.5f;
  const float a = fabsf(v) - 0.03f;
  return (2.0f / 0.47f) * v * (a > 0.0f ? a : 0.0f);
}
float detune(float v) {
  float d = 2.0f * v - 1.0f;
  const float sign = d < 0.0f ? -1.0f : 1.0f;
  d = d * sign * 6.0f;
  int i = (int)d;
  if (i > 6) i = 6;
  float f = d - (float)i;
  f = (f - 0.33333333f) * 1.5f;
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  static const float kIntervals[8] = {0.0f, 1.0f, 3.0f, 4.0f, 7.0f, 12.0f, 24.0f, 24.0f};
  const float a = kIntervals[i], b = kIntervals[i + 1];
  const float s = f * f * (3.0f - 2.0f * f);   // SmoothStep
  return (a + (b - a) * s) * sign;
}
inline int enum_of(float v, int n) {   // a knob's 0..127 in n equal zones
  int k = (int)(v * (float)n / 128.0f);
  return k < 0 ? 0 : (k >= n ? n - 1 : k);
}

void map_controls(State* s) {
  const float* p = s->p;
  cloud::Controls& c = s->c;
  c.time = clamp01(p[0] / 127.0f);
  c.size = clamp01(p[1] / 127.0f);
  c.density = clamp01(p[2] / 127.0f);
  c.pitch = detune(clamp01(p[3] / 127.0f));
  c.shape = clamp01(p[4] / 127.0f);
  c.feedback = clamp01(p[5] / 127.0f * 1.005f);
  c.spread = clamp01(p[6] / 127.0f);
  c.dry_wet = clamp01(p[7] / 127.0f * 1.005f);
  c.reverb = clamp01(p[8] / 127.0f * 1.005f);
  c.freeze = p[9] >= 64.0f;
  c.quality = enum_of(p[10], 4);
  c.bank = enum_of(p[11], cloud::kNumBanks);
  c.wave = clamp01(p[12] / 127.0f);
  c.time_random = attenuverter(clamp01(p[13] / 127.0f));
  c.pitch_random = attenuverter(clamp01(p[14] / 127.0f));
  c.engine = enum_of(p[15], 2);
}

// Flush-to-zero while CLOUD renders (AArch64 FPCR.FZ, put back after): Beads' one-pole filters, limiters and reverb
// decay towards zero and would otherwise spend blocks in denormals once a sound has died away.
struct FlushToZero {
#if defined(__aarch64__)
  uint64_t saved;
  FlushToZero() {
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved));
    __asm__ volatile("msr fpcr, %0" : : "r"(saved | (1ull << 24)));
  }
  ~FlushToZero() { __asm__ volatile("msr fpcr, %0" : : "r"(saved)); }
#endif
};

int32_t c_init(const rmp_host* h) {
  (void)h;
  return 0;
}
void c_voice_init(void* st) {
  State* s = (State*)st;
  s->engine = nullptr;
  s->voice = -1;
  memcpy(s->p, kDefaults, sizeof s->p);
  map_controls(s);
}
void c_attach(void* st, const rmp3_voice* v) {
  State* s = (State*)st;
  s->voice = v->voice;
  s->engine = nullptr;
  if (!v->arena || v->arena_bytes < kArenaBytes) return;   // no room: silent
  uint8_t* a = (uint8_t*)v->arena;
  cloud::Engine* e = new (a) cloud::Engine;
  e->Init(a + kEngineBytes, (uint16_t*)(void*)(a + kEngineBytes + cloud::kBufferBytes));
  s->engine = e;
}
void c_params(void* st, const float* p) {
  State* s = (State*)st;
  memcpy(s->p, p, sizeof s->p);
  map_controls(s);
}
void c_note_on(void* st, float note, float velocity) {
  State* s = (State*)st;
  if (s->engine) s->engine->NoteOn(note, velocity);
}
void c_note_off(void* st) {
  State* s = (State*)st;
  if (s->engine) s->engine->NoteOff();
}
void c_render(void* st, const rmp3_block* b) {
  State* s = (State*)st;
  const int32_t n = b->n < 0 ? 0 : (b->n > 16 ? 16 : b->n);
  if (n == 0) return;
  float* l = b->out[0];
  float* r = b->out[1];
  if (!s->engine) {
    for (int32_t i = 0; i < n; ++i) l[i] = 0.0f;
    if (r)
      for (int32_t i = 0; i < n; ++i) r[i] = 0.0f;
    return;
  }
  FlushToZero ftz;
  float rr[16];
  s->engine->Render(s->c, l, r ? r : rr, (size_t)n);
  if (!r)   // a mono route (TRK): the mix of both sides
    for (int32_t i = 0; i < n; ++i) l[i] = 0.5f * (l[i] + rr[i]);
}

const rmp3_plugin kPlugin3 = {
    RMP3_MAGIC, (uint32_t)sizeof(rmp3_plugin), RMP3_MINOR,
    RMP3_KIND_MACHINE,
    RMP3_CAP_ARENA,                             // needs: without an arena it cannot play
    RMP3_CAP_GUARD | RMP3_CAP_STEREO,           // wants
    0, 0,                                       // shared_min, shared_want
    kArenaBytes, kArenaBytes,                   // voice_min, voice_want
    8,                                          // states: the 8 voices (no poly instances)
    0,                                          // latency_frames
    2, 0, 0,                                    // out_channels, in_channels, aux_channels
    RMP3_AT_VOICE,
    0, 0, {0, 0, 0, 0, 0, 0},
    nullptr, nullptr,
    nullptr,                                    // setup: nothing shared
    c_attach, c_render, nullptr,
    {nullptr, nullptr, nullptr, nullptr},
};

}  // namespace

RMP_ENTRY const rmp_plugin rmp_entry = {
    RMP_PLUGIN_MAGIC,
    RMP_API3,                                   // API 3 only: the arena is not optional
    (uint32_t)sizeof(State),
    CLOUD_PERMILLE,
    c_init, c_voice_init, c_note_on, c_note_off, c_params,
    nullptr,                                    // no API 2 render
    {nullptr, nullptr, nullptr, nullptr, RMP_FN(&kPlugin3), nullptr, nullptr, nullptr},
};
