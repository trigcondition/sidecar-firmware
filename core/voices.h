// core/voices.h - the eight voices (moved from teensy/voiceloop/voiceloop.ino): their machines' state, what a
// report does to a voice, one block of each voice's audio, DSP SAMPLE's card work between requests. Module state,
// one engine per program, as before. Voice handling is sim/obhost.py's (sim/protocol.py's docstring).
#pragma once
#include "platform.h"   // first: the Teensy port defines PROGMEM, which puts JUNO's factory table (juno_patches.h) in flash

#include <DaisySP.h>
#include <Juno.h>
#include <Plaits.h>
#include <Sampler.h>

#include "engines/stream_voice.h"
#include "hooks.h"
#include "machines.h"
#include "pool/pool.h"
#include "heap_buffers.h"
#include "pool/sample_ram_pool.h"

namespace core {

struct V {
  plaits::Voice voice;
  plaits::Patch patch;
  plaits::Modulations mod;
  float p[16];         // P1..P16, 0..127.99 (P9..P16: EXT page 2)
  float target;        // the note glide moves toward
  bool live;
  bool trig;
  float note, level;
  bool daisy;          // DAISYSP or DSP SAMPLE (else PLAITS)
  bool dsps;           // DSP SAMPLE: P5/P6 are MIX/FB; its sample streams from the card (the GRAIN machine too)
  bool smpl;           // SAMPLER (machines.h kSmplTag on a 0x20 report): dsps with the Digitakt-style PLAY / LOOP, no resonator
  bool grain;          // the GRAIN machine (machines.h kGrainMachine): dsps in GRAIN mode, P12 SPRY, no resonator
  bool md;             // MD (machines.h kMd, 0x40; core/md/md.h): the built-in mdkit drums (silent without CORE_MD)
  bool juno;           // JUNO
  bool fm;             // 6-OP FM: x.voice plays through fms[] (fm_machine.h)
  int8_t user;         // a user slot U1..U5 (0..4) plays the voice (hooks.user_*); -1 none
#if CORE_MSAMP
  bool ms;             // MULTISAMPLE (core/msamp/msamp.h): its notes render in msamp::render_add
  bool wt, gr;         // WAVETABLE, GRANULAR (core/nmach.h): their notes render in wt:: / gr::render_add
  bool li;             // LIVE IN (core/livein/livein.h): the device's AUDIO IN through the voice's chain
  bool cp;             // CHOP (core/chop/chop.h, a note machine of core/nmach.h): its slices render in chop::render_add
  bool mi;             // MIDI (core/midim/midim.h, a note machine of core/nmach.h): sends MIDI, never live
#endif
};

extern V v[kVoices];
extern uint16_t g_dsp_head[kVoices];
int dsp_playhead(int vi);   // DSP SAMPLE: where the voice reads, 0..65534 of the file; -1 none (racy, for drawing)
extern rytm::DaisyEngine daisy[kVoices];
extern juno::Voice junos[kVoices];
#ifdef CORE_HEAP_BUFFERS   // heap_buffers.h: placed at boot
extern char (*shared)[16384];
#else
extern char shared[kVoices][16384];   // Plaits' scratch; a voice on a user slot keeps its plugin state here
#endif
extern rytm::FmMachine fms[kVoices];
extern uint8_t fm_stage[RYTM_RENDER_CORES][rytm::FmMachine::kStageBytes];   // per render core (plaits::fm_core())
// DSP SAMPLE: the streams, their buffers, the prepares between requests
extern rytm::StreamVoice streams[kVoices];
#ifdef CORE_HEAP_BUFFERS   // heap_buffers.h: placed at boot
extern uint8_t (*stream_heads)[rytm::StreamVoice::kHead];
extern uint8_t (*stream_rings_a)[rytm::StreamVoice::kRingBytes];
extern uint8_t (*stream_rings_b)[rytm::StreamVoice::kRingBytes];
#else
extern uint8_t stream_heads[kVoices][rytm::StreamVoice::kHead];
extern uint8_t stream_rings_a[4][rytm::StreamVoice::kRingBytes];
extern uint8_t stream_rings_b[4][rytm::StreamVoice::kRingBytes];
#endif
extern int prepared[kVoices][3];   // folder, file, STA
extern float trig_note[kVoices];
extern int want_prep[kVoices][4];
extern bool prep_pending[kVoices], trig_pending[kVoices];
extern float pending_note[kVoices];
extern uint32_t trig_wait_t[kVoices], max_trig_wait_us;
extern uint32_t refills, refill_kb, max_refill_us;
// the sample RAM (pool/sample_ram_pool.h); psram null without one
extern rytm::SampleRamPool* psram;
extern rytm::SampleModes* engines;
extern bool from_ram[kVoices];
extern int ram_pin[kVoices][2];
// 6-OP FM and JUNO
extern int fm_gen, fm_key[kVoices], fm_pat[kVoices];
extern int juno_pat[kVoices], juno_key[kVoices];
extern uint8_t juno_kb[kVoices][18], juno_hold[kVoices][6];
extern uint32_t juno_hold_t[kVoices];
extern bool juno_holding[kVoices];

int plaits_model(const V& x);
int daisy_model(const V& x);
int dsp_mode(const V& x);
// SAMPLER's PLAY (x.smpl; docs/SAMPLER.md): 9 equal zones FWD REV FLP RLP SLICE GRAIN STRCH PLUCK WAVE, LOOP (P15) the
// loop's start (0 OFF: STA..END whole). DSP SAMPLE's MOD (smpl false): its 7 zones ONE LOOP SLICE GRAIN STRCH PLUCK WAVE
// and LOP's meanings, as before 0.5.0. The words are the same in both (word 20 PLAY / MOD, 23 LOOP / LOP).
constexpr int kDspsPlays = 9;   // SAMPLER's PLAY zones (equal: zone = value * 9 / 128)
enum DspsPlay { kPlayFwd, kPlayRev, kPlayFlp, kPlayRlp, kPlaySlice, kPlayGrain, kPlayStrch, kPlayPluck, kPlayWave };
int dsps_play_zone(bool smpl, float play);           // SAMPLER's zone (DspsPlay) of a PLAY value; -1 for DSP SAMPLE
int dsps_mode_of(bool smpl, float play);             // the engine's mode (rytm::SampleModes::Mode) a PLAY / MOD value plays
const char* dsps_play_name(bool smpl, float play);   // "FWD" .. "WAVE" (DSP SAMPLE: "ONE" .. "WAVE")
void apply(V& x);
void ram_release(int vi);
void prepare_stream(int vi);
void trigger_stream(int vi, int note);
#if CORE_MSAMP
// The sample editor's play buffer (core/samp/sedit.h) in place of the sample RAM's copy: voice vi, triggering pool
// slot `slot` (pool_slot), plays pcm (frames + a guard frame, ch interleaved, at rate) from its next trig on; fa < 0:
// the track's STA..END, else the frames [fa, fb). Null clears it. A note already sounding keeps its own buffer.
struct DspOverride {
  const int16_t* pcm;
  uint32_t frames;
  float rate;
  uint8_t ch;
  int slot;
  int32_t fa, fb;
};
void dsps_override(int vi, const DspOverride* o);
// A voice playing from RAM reads [lo, hi) now (its engine's pcm in it); the control core's, between blocks
bool dsps_reads(const int16_t* lo, const int16_t* hi);
// The voices not playing that still hold a pcm in [lo, hi) let it go (their next trig sets their sample again)
void dsps_detach_idle(const int16_t* lo, const int16_t* hi);
// Voice vi plays its override's frames [a, b) once (ONE, no loop), as a trig at note 60; nothing without an override
void dsps_preview(int vi, uint32_t a, uint32_t b);
#endif
bool prepare_pending();
void fm_select(int vi, int pat);
void fm_note_on(int vi, int note, int vel);   // 6-OP FM's mono note (voice_report; core/poly.cpp)
void fm_relist();
int juno_pick(int pat, const uint8_t** b, const char** name);
void juno_select(int vi, int pat, bool sync, bool force);
void juno_sound_pat(int vi, int pat);   // the sound's PAT, as the Rytm wrote it (the bank list's commit)
void juno_relist();
bool refill_neediest(uint32_t below);
bool pool_line_open(int line);   // a pool line's file (open_on_card): false while the card core opens it
bool prepare_waiting();   // a prepare not ended (a head on the card core)
void streams_step();   // each pass: the DSP SAMPLE streams' finished requests, what they need of the card core (cardq::set_need)

// Boot: the sample RAM (after the card) and the voices (after the project: pool_load resets prepared[]).
void sample_ram_init();
bool dsp_streams_playing();           // a DSP SAMPLE stream plays now (it reads the card on the control core)
extern uint32_t ram_late;             // trigs of a RAM-only mode whose sample came too late (voices.cpp kRamLateMs)
void voices_init();
// A 0x04 / 0x03 report (d: the whole F0..F7, len bytes) and a 0x11 note-off.
void voice_report(int cmd, const uint8_t* d, int len);
// polyuser-4 F-b1: a 33 names voice vi's Sidecar machine (vb): the voice takes it while no 0x03 / 0x04 has come yet
void voice_machine_hint(int vi, uint8_t vb);
void voice_note_off(int vi);
void voice_params_changed(int vi);   // x.p changed on the control side (core/lfo.h): the machine takes it
void voice_silence(int vi);   // MIDI CC 120: note-off and the output ramped out in 5 ms, silent until the next trig
void voice_wake(int vi);      // the sequencer's note: the voice rendering and its output back (as a trig does)
// E0 (core/inst.h): a note on voice vi's own machine as a Rytm trig plays it (the sequencer's mono path: VOICES 1, DSP
// SAMPLE, a mono user machine; a seq track's own pad).
void voice_play(int vi, int note, int vel);
// One block (kBlock frames) of voices v0..v1-1 into ring rows (w + s) % ring_len, column = voice, int16; the
// cycles each machine took added to mach[k] (kM*); returns the block's cycles. Voices v0..v1-1 only: several cores
// may render disjoint ranges at once (pi/engine) when the build sets RYTM_RENDER_CORES to the core count and the
// platform defines rytm_render_core() (the state voices share is per core then: the 6-OP FM stage and
// plaits::fm_user_bank, GRAIN's budget, stmlib's Random, rand(); tested by core/tests/mc/), each core with its own
// mach array (two cores with voices of one machine race on mach[k]), and nothing else runs meanwhile (engine.h:
// steps 1, 2 and 4 between blocks).
// ring_r (CORE_POLY builds; the Pi engine): each voice's R in the same rows - a voice on OUT PRE / POST (core/fx/out.h)
// renders a stereo pair (the sample machines in stereo, the others L = R) and runs its INSERT chain in stereo; a voice
// on TRK renders mono as before, its R = L. st (optional, [kVoices]): st[v] 1 when voice v's pair this block is a real
// stereo pair (L != R somewhere), for the output stage's PAN law (one byte a voice: cores render disjoint voices).
// Null: mono only, as before (the Teensy, the tests).
enum { kMFm, kMJuno, kMDsps, kMDaisy, kMMd, kMPlaits, kMUser, kMachines };
uint32_t render_voices(int16_t (*ring)[kVoices], int ring_len, int w, int v0, int v1, uint64_t* mach,
                       int16_t (*ring_r)[kVoices] = nullptr, uint8_t* st = nullptr);
// DSP SAMPLE's DEC on PRE / POST (core/fx/out.h): its trig / note-off counters (the control core bumps, the render
// core follows)
void dsps_env_trig(int vi);
void dsps_env_off(int vi);
// The poly voices' output (6-OP FM with its pool, MULTISAMPLE; CORE_POLY builds): a soft limiter at the Overbridge
// return, linear up to kSoftKnee of full scale, above it a tanh knee towards kSoftCeil (never reached), so a chord's
// overs never hard-clip. The blocks it touched on voice vi (the render core's count; 0 without CORE_POLY).
constexpr float kSoftKnee = 0.8f, kSoftCeil = 0.98f;
uint32_t voice_limited(int vi);
// The auto-sampler's copy of voice vi's sound (core/samp/autosamp.h): its machine (machine 0x48 6-OP FM / 0x08 JUNO; others: false),
// P1..P16 now, and the DX7 voice (128 packed bytes) or JUNO patch (the 106's 18 bytes, ext: the card's list, else
// the factory patch PAT picks) it plays. The control core's.
struct VoiceSound {
  int machine;
  float p[16];
  uint8_t dx7[128];
  bool has_dx7;
  uint8_t juno[18];
  bool juno_ext;
  char name[24];
};
bool voice_sound(int vi, VoiceSound* out);

}  // namespace core
