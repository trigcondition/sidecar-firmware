// plugins/cloud/cloud_engine.h - CLOUD's sound engine: an oscillator recorded into an always-running buffer and
// granulated (the CLOUD engine), or wavetable grains (the SYNTH engine), then the reverb and the limiter.
//
// Adapted from Beads (Emilie Gillet, MIT): beads/dsp/processor.{h,cc} (the cloud: the four quality settings, the
// delay mode, dry/wet, wow and flutter, reverb, limiters) and beads/dsp/synth.{h,cc} (the wavetable grains Beads plays
// when nothing is patched in), with Beads' own DSP classes vendored unchanged under third_party/eurorack (one change
// in delay_line.h, see VENDORED.md). What differs from Beads:
//   - The source is a built-in oscillator (Beads' WavetableVoice) at the trig's note, not an audio input: Plugin
//     API 3 gives a machine no audio input yet. The CLOUD engine always records it (Beads' mono buffer).
//   - Everything runs at 48 kHz. DIGITAL is Beads' own 48 kHz path; CLEAN TAPE and DIRTY TAPE use Beads' own 2:1
//     converters to 24 kHz inside (DIRTY TAPE as Beads builds it without ENABLE_HARDWARE_24K); VINTAGE runs its
//     12-bit buffer at 48 kHz instead of Beads' 32 kHz codec. The mono buffer holds 8 s (DIGITAL), 10.7 s (VINTAGE),
//     21.3 s (CLEAN TAPE) and 32 s (DIRTY TAPE).
//   - The buffer is cleared in pieces from Render (64 KB a call) with the wet signal faded out, never in one call: a
//     voice's attach and a QUALITY change stay short.
//   - The SEED button is latched (grains at DENSITY) and the trig presses it: a note_on taps a grain (DENSITY at
//     the centre) or resyncs the grain clock. The delay mode keeps it latched (TIME sets the delay, no tap tempo).
// MIT. Beads: Copyright 2016-2021 Emilie Gillet (LICENSE.beads). The adaptation: Copyright 2026 trigcondition and
// contributors (LICENSE).
#pragma once
#include <stddef.h>
#include <stdint.h>

// In the order Beads' processor.h and synth.h include them (the headers lean on what came before).
#include "beads/dsp/granular_processor.h"
#include "beads/dsp/modulated_parameter.h"
#include "beads/dsp/fx/reverb.h"
#include "beads/dsp/fx/wow_and_flutter.h"
#include "beads/dsp/delay_controller.h"
#include "beads/dsp/limiter.h"
#include "beads/dsp/trigger_generator.h"
#ifdef TEST   // wavetable_granular_player.h prints on a stolen grain under TEST (stmlib's portable build): not here
#undef TEST
#include "beads/dsp/wavetable_granular_player.h"
#define TEST 1
#else
#include "beads/dsp/wavetable_granular_player.h"
#endif
#include "beads/dsp/wavetable_voice.h"

namespace cloud {

// Beads' buffer: 768 KB (three 256 KB banks), and the reverb's 32768 12-bit words.
const size_t kBankBytes = 262144;
const size_t kNumBufferBanks = 3;
const size_t kBufferBytes = kBankBytes * kNumBufferBanks;
// Beads sizes its delay lines for 768 000 bytes of the 786 432: the rest holds the lines' interpolation tails and the
// freeze tail (sized for the whole buffer, a grain crossing the wrap point reads past its end).
const size_t kDelayBufferSizeBits = 768 * 1000 * 8;
const size_t kReverbWords = 32768;
const size_t kReverbBytes = kReverbWords * sizeof(uint16_t);
const size_t kClearChunk = 65536;     // bytes cleared a Render call
const int kNumBanks = 12;             // the oscillator's wavetable banks (WAV_INTEGRATED_WAVES: 12 x 16 waves)
const float kSampleRate = 48000.0f;

enum Quality { DIRTY_TAPE = 0, CLEAN_TAPE = 1, VINTAGE = 2, DIGITAL = 3 };   // Beads' Processor::Media order
enum EngineKind { ENGINE_CLOUD = 0, ENGINE_SYNTH = 1 };

// The controls, as the knobs give them (cloud.cc maps P1..P16 to these with Beads' pot laws).
struct Controls {
  float time;          // 0..1 position in the buffer (CLOUD), the grains' wave (SYNTH)
  float size;          // 0..1 grain size; under 1/3 reversed; at the top the delay mode (CLOUD)
  float density;       // 0..1: below the centre a regular clock, the centre only trigs, above random
  float pitch;         // semitones, -24..+24, the grains' transposition
  float shape;         // 0..1 grain envelope
  float feedback;      // 0..1 (CLOUD)
  float spread;        // 0..1: random grain envelope and pan (Beads' SHAPE attenuverter, no CV)
  float dry_wet;       // 0..1
  float reverb;        // 0..1
  bool freeze;
  int quality;         // Quality
  int bank;            // 0..kNumBanks-1
  float wave;          // 0..1 the oscillator's wave in its bank
  float time_random;   // -1..1 Beads' TIME attenuverter with nothing patched: + smooth, - jumps
  float pitch_random;  // -1..1 Beads' PITCH attenuverter with nothing patched
  int engine;          // EngineKind
};

class Engine {
 public:
  // buffer: kBufferBytes, reverb: kReverbBytes; neither needs to be zero (Render clears them first).
  void Init(uint8_t* buffer, uint16_t* reverb);
  // A trig: the oscillator's note (semitones, 60 = C4), its level 0..1, and a seed (one grain / a tap).
  void NoteOn(float note, float velocity);
  void NoteOff();
  // n frames (1..16) into l and r (written, not added).
  void Render(const Controls& c, float* l, float* r, size_t n);

  inline bool clearing() const { return clear_pos_ < clear_end_; }
  inline int active_quality() const { return media_; }

 private:
  void StartClear(int media);
  beads::SeedButtonState seed_state() const;
  void Configure(int media);
  void RenderCloud(const Controls& c, const beads::FloatFrame* in, beads::FloatFrame* out, const bool* freeze,
                   size_t n);
  void RenderSynth(const Controls& c, beads::FloatFrame* out, const bool* freeze, size_t n);

  beads::GranularProcessor<beads::RESOLUTION_16_BIT, kDelayBufferSizeBits / (16 * 2), 1> digital_;
  beads::GranularProcessor<beads::RESOLUTION_12_BIT, kDelayBufferSizeBits / (12 * 2), 1> vintage_;
  beads::GranularProcessor<beads::RESOLUTION_12_BIT, kDelayBufferSizeBits / (12 * 2), 2> clean_tape_;
  beads::GranularProcessor<beads::RESOLUTION_8_BIT, kDelayBufferSizeBits / (8 * 2), 2> dirty_tape_;

  beads::TriggerGenerator trigger_generator_;
  beads::DelayController delay_controller_;
  beads::WowAndFlutter wow_and_flutter_;
  beads::Reverb reverb_;
  beads::Limiter pre_reverb_limiter_;
  beads::Limiter output_limiter_;
  beads::WavetableVoice oscillator_;
  beads::WavetableGranularPlayer wavetable_granular_player_;

  uint8_t* buffer_;
  uint16_t* reverb_buffer_;
  uint8_t* banks_[kNumBufferBanks];

  int media_;              // the quality the buffer is configured for, -1 none yet
  int engine_;             // the engine playing (a change fades the wet signal out and in)
  size_t clear_pos_, clear_end_;
  int clear_media_;        // the quality the clear in progress prepares the buffer for
  float filled_;           // 48 kHz frames recorded since the buffer was configured (up to fill_target_)
  float fill_target_;      // the buffer's length in 48 kHz frames
  float record_gain_;      // 0..1: the recording's fade-in after a clear
  float wet_gate_;         // 0..1: the wet signal's fade around a clear or an engine change
  float delay_mode_transition_;
  float dry_, wet_;        // the dry / wet gains, interpolated
  float note_;             // semitones from the oscillator's root (C3, 130.81 Hz)
  float level_;            // the oscillator's level, 0..1 (velocity)
  float env_;              // the oscillator's envelope
  bool gate_;
  int seed_phase_;         // after a trig: 2 the SEED button released this block, 1 pressed, 0 latched
};

}  // namespace cloud
