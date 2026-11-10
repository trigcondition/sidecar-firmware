// plugins/cloud/cloud_engine.cc - CLOUD's sound engine (cloud_engine.h says what it is and how it differs from
// Beads). Processor::Process and Synth::Render of Beads (Emilie Gillet, MIT) are the model for RenderCloud and
// RenderSynth: the same order, constants and gain laws.
// MIT. Beads: Copyright 2016-2021 Emilie Gillet (LICENSE.beads). The adaptation: Copyright 2026 trigcondition and
// contributors (LICENSE).
#include "cloud_engine.h"

#include <algorithm>

#include "beads/dsp/mu_law.h"
#include "stmlib/dsp/parameter_interpolator.h"

namespace cloud {

using namespace beads;
using namespace stmlib;

namespace {
const float kDelayModeTransitionTime = 1.5f;   // seconds (Beads' processor.cc)
const float kDelayModeFadeTime = 0.75f;
const float kRoot = 48.0f;                     // the oscillator's root note, C3 = 130.81 Hz (kWavetableRootNote)
const float kSourceGain = 0.5f;                // the oscillator into the buffer: Beads' dry level of its own synth
const float kAttack = 1.0f - 0.98966f;         // the oscillator's envelope: ~2 ms up, ~200 ms down after a note off
const float kRelease = 1.0f - 0.999896f;
const float kWetFadeStep = 1.0f / 144.0f;      // the wet signal's fade around a clear / an engine change: 3 ms
const float kRecordFadeStep = 1.0f / 240.0f;   // the recording's fade-in after a clear: 5 ms
// Beads' synth: the bit depth of the continuous voice and of the grains, per quality (Synth::Resolution)
const int kContinuousBitShift[4] = {4, 16, 7, 16};
const int kGrainBitShift[4] = {4, 6, 7, 16};

// Fill n bytes (a multiple of 64, 8-byte aligned) with the byte b, with word stores: the plugin libc's memset goes a
// byte at a time.
void fill_words(uint8_t* p, size_t n, uint8_t b) {
  const uint64_t v = 0x0101010101010101ull * b;
  uint64_t* w = reinterpret_cast<uint64_t*>(p);
  for (size_t i = 0; i < n / 8; i += 8) {
    w[i] = v, w[i + 1] = v, w[i + 2] = v, w[i + 3] = v, w[i + 4] = v, w[i + 5] = v, w[i + 6] = v, w[i + 7] = v;
  }
}
// The freeze tail Beads' delay lines keep at the end of the last bank (256 float frames): zero floats.
const size_t kFreezeTailBytes = 256 * sizeof(FloatFrame);
// The longest line (any quality: 768 000 bytes) with its 4-item interpolation tail stays clear of the freeze tail.
static_assert(kDelayBufferSizeBits / 8 + 8 + kFreezeTailBytes <= kBufferBytes, "the delay lines fit the buffer");
}  // namespace

void Engine::Init(uint8_t* buffer, uint16_t* reverb) {
  buffer_ = buffer;
  reverb_buffer_ = reverb;
  for (size_t i = 0; i < kNumBufferBanks; ++i) banks_[i] = buffer + i * kBankBytes;

  // Init only places the delay lines (BEADS_DELAY_LINE_NO_CLEAR_ON_INIT): the buffer is cleared in pieces below.
  digital_.Init(48000.0f, banks_);
  vintage_.Init(48000.0f, banks_);
  clean_tape_.Init(24000.0f, banks_);
  dirty_tape_.Init(24000.0f, banks_);

  trigger_generator_.Init(kSampleRate);
  delay_controller_.Init(kSampleRate, digital_.max_delay_times_ratio() * 2);
  reverb_.Init(reverb, kSampleRate);           // clears its 64 KB
  wow_and_flutter_.Init(kSampleRate);
  wow_and_flutter_.set_lp(0.6f);               // Beads' 48 kHz codec setting
  pre_reverb_limiter_.Init(1.0f / (kSampleRate * 4.0f), 0.9f);
  output_limiter_.Init(1.0f / (kSampleRate * 6.0f), 0.9f);
  oscillator_.Init(kSampleRate);
  wavetable_granular_player_.Init(kSampleRate);

  media_ = -1;
  engine_ = ENGINE_CLOUD;
  wet_gate_ = 0.0f;
  delay_mode_transition_ = -kDelayModeTransitionTime;
  dry_ = wet_ = 0.0f;
  note_ = 12.0f;
  level_ = 0.0f;
  env_ = 0.0f;
  gate_ = false;
  seed_phase_ = 0;
  filled_ = 0.0f;
  fill_target_ = 1.0f;
  record_gain_ = 0.0f;
  clear_pos_ = clear_end_ = 0;
  clear_media_ = -1;   // the first Render starts the clear, for the quality asked then
}

// The buffer is cleared to the silence of the quality it will hold: zero bytes, except DIRTY TAPE, whose 8-bit
// mu-law silence is Lin2MuLaw(0) (a zero byte there is full scale negative).
void Engine::StartClear(int media) {
  clear_media_ = media;
  clear_pos_ = 0;
  clear_end_ = kBufferBytes;
}

void Engine::NoteOn(float note, float velocity) {
  note_ = note - kRoot;
  level_ = std::min(std::max(velocity, 0.0f), 1.0f);
  gate_ = true;
  seed_phase_ = 2;   // the SEED button released for a block, then pressed for one: Beads' rising edge
}

void Engine::NoteOff() { gate_ = false; }

// Beads' SEED button: latched (LOCKED) as Beads starts, so the grains run at DENSITY; a trig releases it for a block
// and presses it for one, the rising edge that makes a grain (DENSITY at the centre) or resyncs the grain clock.
SeedButtonState Engine::seed_state() const {
  return seed_phase_ == 2 ? SEED_BUTTON_STATE_OFF : (seed_phase_ == 1 ? SEED_BUTTON_STATE_ON : SEED_BUTTON_STATE_LOCKED);
}

// The quality the buffer plays at, once it is cleared: that processor's state starts afresh (Beads' Configure).
void Engine::Configure(int media) {
  media_ = media;
  int max_delay = 0;
  switch (media) {
    case DIGITAL:
      digital_.Init(48000.0f, banks_);
      max_delay = digital_.max_delay_times_ratio();
      break;
    case VINTAGE:
      vintage_.Init(48000.0f, banks_);
      max_delay = vintage_.max_delay_times_ratio();
      break;
    case CLEAN_TAPE:
      clean_tape_.Init(24000.0f, banks_);
      max_delay = clean_tape_.max_delay_times_ratio();
      break;
    default:
      dirty_tape_.Init(24000.0f, banks_);
      max_delay = dirty_tape_.max_delay_times_ratio();
      break;
  }
  delay_controller_.Init(kSampleRate, max_delay * 2);   // mono: the mono delay line is twice as long
  delay_mode_transition_ = -kDelayModeTransitionTime;
  filled_ = 0.0f;
  fill_target_ = float(max_delay) * 2.0f;   // the mono line's length in 48 kHz frames
  record_gain_ = 0.0f;
}

void Engine::RenderCloud(const Controls& c, const FloatFrame* in, FloatFrame* out, const bool* freeze, size_t size) {
  const bool ready = media_ >= 0 && !clearing();
  const float delay_mode = delay_mode_transition_ > 0.0f;
  const float threshold = delay_mode ? 0.98f : 0.99f;
  const float direction = c.size > threshold ? 1.0f : -1.0f;
  delay_mode_transition_ += direction * float(size) / kSampleRate;
  CONSTRAIN(delay_mode_transition_, -kDelayModeTransitionTime, +kDelayModeTransitionTime);

  Parameters p;
  // TIME reaches back only as far as the buffer holds sound: right after a clear (a new voice, a QUALITY change) the
  // grains start near the write head and spread back as it fills (Beads' buffer has always been recording). The
  // delay mode keeps Beads' TIME.
  const float fill = !delay_mode && filled_ < fill_target_ ? filled_ / fill_target_ : 1.0f;
  if (ready && !freeze[0]) filled_ = std::min(fill_target_, filled_ + float(size));
  p.time.base = c.time * fill, p.time.attenuverter = c.time_random, p.time.cv = 0.0f, p.time.has_cv = false;
  p.size.base = c.size, p.size.attenuverter = 0.0f, p.size.cv = 0.0f, p.size.has_cv = false;
  p.shape.base = c.shape, p.shape.attenuverter = c.spread, p.shape.cv = 0.0f, p.shape.has_cv = false;
  p.pitch.base = c.pitch, p.pitch.attenuverter = c.pitch_random, p.pitch.cv = 0.0f, p.pitch.has_cv = false;
  p.monophonic_mode = false;
  // the delay mode keeps SEED latched: its TIME sets the delay (a press would make the trigs a tap tempo)
  const SeedButtonState seed = delay_mode ? SEED_BUTTON_STATE_LOCKED : seed_state();

  TriggerGenerator::Output triggers_buffer[size];
  DelayController::Output taps_buffer[size];
  TriggerGenerator::Output* triggers = &triggers_buffer[0];
  DelayController::Output* taps = &taps_buffer[0];
  if (delay_mode) {
    triggers = NULL;
    delay_controller_.Render(seed, NULL, freeze, c.density, 0.0f, 0.0f, p.time, p.shape, taps, size);
  } else {
    taps = NULL;
    trigger_generator_.Render(seed, NULL, c.density, 0.0f, 0.0f, triggers, size);
  }

  // What is recorded fades in after a clear (5 ms): a grain reading across the moment the recording started hears a
  // swell, not a step from silence to the oscillator mid-wave.
  FloatFrame rec[size];
  for (size_t i = 0; i < size; ++i) {
    if (ready && record_gain_ < 1.0f) record_gain_ = std::min(1.0f, record_gain_ + kRecordFadeStep);
    rec[i] = in[i] * record_gain_;
  }
  FloatFrame dry_signal[size];
  const float feedback = c.feedback;
  if (!ready) {
    for (size_t i = 0; i < size; ++i) out[i] = 0.0f, dry_signal[i] = in[i];
  } else {
    switch (media_) {
      case DIGITAL:
        digital_.Process(triggers, taps, freeze, p, feedback, true, rec, out, size);
        std::copy(&in[0], &in[size], &dry_signal[0]);
        break;
      case VINTAGE:
        vintage_.Process(triggers, taps, freeze, p, feedback, true, rec, out, size);
        for (size_t i = 0; i < size; ++i) {
          dry_signal[i].l = static_cast<int32_t>(in[i].l * 4096.0f) / 4096.0f;
          dry_signal[i].r = static_cast<int32_t>(in[i].r * 4096.0f) / 4096.0f;
        }
        break;
      case CLEAN_TAPE:
        clean_tape_.Process(triggers, taps, freeze, p, feedback, true, rec, out, size);
        std::copy(&in[0], &in[size], &dry_signal[0]);
        break;
      default:
        dirty_tape_.Process(triggers, taps, freeze, p, feedback, true, rec, out, size);
        for (size_t i = 0; i < size; ++i) {
          dry_signal[i].l = MuLaw2Lin(Lin2MuLaw(Clip16(static_cast<int32_t>(in[i].l * 32768.0f))));
          dry_signal[i].r = MuLaw2Lin(Lin2MuLaw(Clip16(static_cast<int32_t>(in[i].r * 32768.0f))));
        }
        break;
    }
  }

  // Fade the wet signal out and in between the granular and the delay mode (Beads), and around a clear.
  const float wet_fade = SmoothStep(std::min(1.0f, fabsf(delay_mode_transition_) / kDelayModeFadeTime));
  const float post_gain_dry = 0.75f;
  const float post_gain_wet = 1.5f;
  const float dry_wet = c.dry_wet;
  const float dry = (1.0f - dry_wet) * (1.0f + dry_wet) * post_gain_dry;
  const float wet = dry_wet * (2.0f - dry_wet) * post_gain_wet * wet_fade;
  ParameterInterpolator dry_modulation(&dry_, dry, size);
  ParameterInterpolator wet_modulation(&wet_, wet, size);
  for (size_t i = 0; i < size; ++i) {
    out[i] = dry_modulation.Next() * dry_signal[i] + wet_modulation.Next() * wet_gate_ * out[i];
  }
  if (media_ == DIRTY_TAPE && ready) wow_and_flutter_.Process(out, size);

  pre_reverb_limiter_.Process(out, size);
  const bool caveman = media_ != DIGITAL;
  const float reverb = c.reverb;
  const float reverb_time = reverb * reverb * reverb;
  reverb_.set_mix(reverb * reverb * 0.9f);
  reverb_.set_time(caveman ? (0.45f + 0.5f * reverb_time) : (0.3f + 0.6f * reverb_time));
  reverb_.set_lp(caveman ? 0.4f : (media_ == VINTAGE ? 0.75f : 0.6f));
  reverb_.Process(out, size);
  output_limiter_.Process(out, size);
}

void Engine::RenderSynth(const Controls& c, FloatFrame* out, const bool* freeze, size_t size) {
  Parameters p;
  p.time.base = c.time, p.time.attenuverter = c.time_random, p.time.cv = 0.0f, p.time.has_cv = false;
  p.size.base = c.size, p.size.attenuverter = 0.0f, p.size.cv = 0.0f, p.size.has_cv = false;
  p.shape.base = c.shape, p.shape.attenuverter = c.spread, p.shape.cv = 0.0f, p.shape.has_cv = false;
  p.pitch.base = note_ + c.pitch, p.pitch.attenuverter = c.pitch_random, p.pitch.cv = 0.0f, p.pitch.has_cv = false;
  p.monophonic_mode = false;
  const SeedButtonState seed = seed_state();
  TriggerGenerator::Output triggers[size];
  trigger_generator_.Render(seed, NULL, c.density, 0.0f, 0.0f, triggers, size);

  const int q = std::min(std::max(c.quality, 0), 3);
  const float post_gain_dry = 0.5f;
  const float post_gain_wet = 1.0f;
  const float dry_wet = c.dry_wet;
  const float dry = (1.0f - dry_wet) * (1.0f + dry_wet) * post_gain_dry;
  const float wet = dry_wet * (2.0f - dry_wet) * post_gain_wet;
  ParameterInterpolator dry_modulation(&dry_, dry, size);
  ParameterInterpolator wet_modulation(&wet_, wet, size);
  // the grains follow the note: its velocity, and its release after a note off (out[] holds the enveloped
  // oscillator; env_level is its envelope at the block's end)
  const float env_level = env_ * level_;
  for (size_t i = 0; i < size; ++i) {
    FloatFrame w;
    w = 0.0f;
    wavetable_granular_player_.Render(c.bank, kGrainBitShift[q], triggers[i], freeze[i], p, &w);
    out[i] = dry_modulation.Next() * out[i] + w * (wet_modulation.Next() * wet_gate_ * env_level);
  }
  pre_reverb_limiter_.Process(out, size);
  const float reverb = c.reverb;
  const float reverb_time = reverb * reverb * reverb;
  reverb_.set_mix(reverb * reverb * 0.8f);
  reverb_.set_time(0.3f + 0.6f * reverb_time);
  reverb_.set_lp(0.6f);
  reverb_.Process(out, size);
  output_limiter_.Process(out, size);
}

void Engine::Render(const Controls& c, float* l, float* r, size_t n) {
  if (n > 16) n = 16;
  const int quality = std::min(std::max(c.quality, 0), 3);
  const int engine = c.engine == ENGINE_SYNTH ? ENGINE_SYNTH : ENGINE_CLOUD;

  // A clear in progress: one piece a call; once done, the buffer is configured for the quality asked now.
  if (clearing()) {
    const size_t k = std::min(kClearChunk, clear_end_ - clear_pos_);
    fill_words(buffer_ + clear_pos_, k, clear_media_ == DIRTY_TAPE ? Lin2MuLaw(0) : 0);
    clear_pos_ += k;
    if (!clearing()) {
      fill_words(buffer_ + kBufferBytes - kFreezeTailBytes, kFreezeTailBytes, 0);
      Configure(clear_media_);   // if QUALITY moved on meanwhile, the check below fades out and clears again
    }
  }
  // What the wet signal waits for: the engine asked for, and in CLOUD its buffer at the quality asked for.
  const bool settled = engine == engine_ && (engine_ == ENGINE_SYNTH || (media_ == quality && !clearing()));
  if (settled) {
    wet_gate_ = std::min(1.0f, wet_gate_ + kWetFadeStep * float(n));
  } else {
    wet_gate_ = std::max(0.0f, wet_gate_ - kWetFadeStep * float(n));
    if (wet_gate_ == 0.0f) {   // faded out: swap the engine, start the clear a quality change needs
      engine_ = engine;
      if (engine_ == ENGINE_CLOUD && media_ != quality && !clearing()) StartClear(quality);
    }
  }

  bool freeze[16];
  for (size_t i = 0; i < n; ++i) freeze[i] = c.freeze;

  // The oscillator (Beads' WavetableVoice) at the note, its envelope and velocity.
  FloatFrame src[16];
  for (size_t i = 0; i < n; ++i) src[i] = 0.0f;
  Parameters op;
  op.time.base = c.wave, op.time.attenuverter = 0.0f, op.time.cv = 0.0f, op.time.has_cv = false;
  op.size = op.time;
  op.shape = op.time;
  op.pitch.base = note_, op.pitch.attenuverter = 0.0f, op.pitch.cv = 0.0f, op.pitch.has_cv = false;
  op.monophonic_mode = false;
  const int bank = std::min(std::max(c.bank, 0), kNumBanks - 1);
  oscillator_.Render(bank, engine_ == ENGINE_SYNTH ? kContinuousBitShift[quality] : 16, freeze, op, src, n);
  const float target = gate_ ? 1.0f : 0.0f;
  const float coefficient = gate_ ? kAttack : kRelease;
  for (size_t i = 0; i < n; ++i) {
    env_ += (target - env_) * coefficient;
    src[i] *= env_ * level_ * (engine_ == ENGINE_CLOUD ? kSourceGain : 1.0f);
  }

  FloatFrame out[16];
  if (engine_ == ENGINE_SYNTH) {
    for (size_t i = 0; i < n; ++i) out[i] = src[i];
    RenderSynth(c, out, freeze, n);
  } else {
    RenderCloud(c, src, out, freeze, n);
  }
  if (seed_phase_ > 0) --seed_phase_;

  for (size_t i = 0; i < n; ++i) {
    float a = out[i].l, b = out[i].r;
    a = (a == a) ? std::min(std::max(a, -1.0f), 1.0f) : 0.0f;   // NaN never leaves (the host clips too)
    b = (b == b) ? std::min(std::max(b, -1.0f), 1.0f) : 0.0f;
    l[i] = fabsf(a) < 1e-20f ? 0.0f : a;
    r[i] = fabsf(b) < 1e-20f ? 0.0f : b;
  }
}

}  // namespace cloud
