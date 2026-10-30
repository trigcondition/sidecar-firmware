// core/samp/sampler.h - SAMPLING and RESAMPLING (docs/SAMPLING.md): one take from a chosen source into a sample,
// /Samples/<project>/<name>.wav (24-bit 48 kHz), with its slices, and instruments for MULTISAMPLE at once.
//
//   sources  the Rytm's AUDIO IN L/R (Overbridge IN 10/11, stereo), its MAIN L/R (IN 0/1: resampling the whole Rytm,
//            the Sidecar's voices and send FX included, as the Rytm mixes them), one Rytm voice output (IN 2..9: BD,
//            SD, RS/CP, BT, LT, MT/HT, CH/OH, CY/CB; mono), or one Sidecar voice as rendered (0..7, mono; stereo for a
//            voice routed OUT PRE / POST when the take is armed, core/fx/out.h).
//   start    manual (now), threshold (the first frame over thr dBFS), the Rytm's PLAY, or its next bar (MIDI clock: 24
//            a beat, 4/4); a pre-roll (0..1000 ms) is kept from before the start.
//   length   free (until stop, or STOP for a PLAY start; at most max_s), or N bars at the Rytm's tempo (its MIDI clock,
//            measured; bpm= when there is none).
//   process  trim (free takes: the silence before and after, as the auto-sampler's) and normalise (peak -1 dBFS),
//            then the WAV, then the slices: transients (or a grid of N), as /Samples/<project>/<name>.sfz next to
//            the WAV (one region a slice, consecutive keys from C1 = 36: the slicing data, the touch page's later),
//            and two MULTISAMPLE instruments, /MULTISAMPLE/<project>/<name>.sfz (one zone over every key, root C4)
//            and <name>-slices.sfz (the slices): the instrument list rescans, INST selects them at once.
//   DSP SAMPLE lists /Samples from its card index, built at boot: the take is in its list after the next boot (the
//            index sees /Samples changed). GRANULAR (core/granular, when it lands) takes the WAV's path.
//
// Cores (the Pi): command / tick / transport on the control core (core 3: the console, MIDI realtime); in_block /
// voice_block on core 0 (the IN completion, the OUT fill: the producer, never blocks); poll on core 0's loop (the
// card: the take's analysis and its files, 64 KB a call, holding off while cardq::busy(): the streams first). The
// Mac: all on one thread, in that order.
#pragma once
#include <stdint.h>

namespace core {
namespace samp {

enum Src : uint8_t { kSrcIn = 0, kSrcMain, kSrcTrack, kSrcVoice };
enum Start : uint8_t { kManual = 0, kThreshold, kOnPlay, kOnBar };
enum Slices : uint8_t { kNoSlices = 0, kTransients, kGrid };
enum State : int { kIdle = 0, kArmed, kRecording, kCaptured, kWriting, kDone, kFailed };

struct Config {
  Src src = kSrcIn;
  uint8_t sub = 0;            // kSrcTrack: 0..7 (IN 2 + sub); kSrcVoice: 0..7
  Start start = kManual;
  float thr_db = -30.0f;
  uint32_t pre_ms = 50;
  uint8_t bars = 0;           // 0 free
  uint32_t max_s = 30;
  float bpm = 120.0f;         // without the Rytm's clock
  bool trim = true, normalise = true;
  Slices slices = kTransients;
  uint8_t grid_n = 16;
  float sens = 2.5f;          // transients' sensitivity (dsp.h)
  char name[32] = "";         // "" SMP_NNN
  bool fx = false;            // kSrcVoice: the send FX's returns added (CHOP's RESAMPLE with FX: stereo; every voice's
                              // sends are in them)
};

struct Status {
  int state;
  Config cfg;
  uint32_t frames;            // recorded (pre-roll included)
  float peak;                 // the take's peak so far (0..1)
  float level;                // the source's level now (meter, 0..1)
  float bpm;                  // the Rytm's clock's, 0 none
  int slices;
  float gain_db;              // the normalisation applied
  char take[32];              // its name
  char wav[96];               // the written file
  char row[8];                // its SAMPLES pool row once there ("A012"; "" not yet / not added)
  char error[64];
  uint32_t write_ms;
};

// Control (the control core)
void configure(const Config& c);
const Config& config();
bool arm(char* err, int cap);   // manual: starts now; else waits for its start. The capture memory is taken here
                                // the first time (max_s + 1 s of stereo float)
void stop();                    // a take in progress ends now (processed and written); armed: disarmed
void cancel();                  // nothing written
void transport(uint8_t status); // MIDI realtime from the Rytm: FA start, FB continue, FC stop, F8 clock
void tick();                    // every pass: a written take's instruments rescanned (msamp::rescan)
bool command(const char* p, char* out, int cap);   // the console's samp... (docs/SAMPLING.md)
void status(Status* s);         // any core (plain reads)
int json(char* out, int cap);   // /status's "samp":{...} (the web page's sampler; any core)
// The producer (core 0): IN frames, 12 channels a frame (int32, 24-bit in the top bits: Overbridge's), and the
// Sidecar's voices as rendered (int16).
void in_block(const int32_t* f, int n, int stride);
// lr: the L / R planes (Taps::lr); fx: the send FX's returns as sent (FX1 L R, FX2 L R; full scale 1), for Config::fx
void voice_block(const int16_t (*v)[8], int n, const int16_t (*lr)[16] = nullptr, const float (*fx)[4] = nullptr);
bool wants_in();                // armed or recording from an IN source
bool wants_voices();
// The card's side (core 0's loop): one bounded step of the take's processing; true when it did one.
bool poll();
void reset();                   // tests: idle, the config's defaults

// Tests and the auto-sampler: the take's processed audio (after trim / normalise), its slices.
const float* take_audio(uint32_t* frames, int* ch);
int take_slices(const uint32_t** starts);

}  // namespace samp
}  // namespace core
