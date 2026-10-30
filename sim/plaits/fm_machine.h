// The 6-OP FM machine's voice (rytm-mods 0021 machine 40, voice flag 0x48;
// sim/protocol.py), for the simulator (plaits_c.cc) and the Teensy (copied
// into its Plaits library by teensy/setup_plaits_lib.sh): Plaits' six-op FM
// engine (DX7 voices; MIT, (c) Emilie Gillet) through a plaits::Voice of its
// own, playing one DX7 voice (128 packed bytes) at a time.
//
// P1..P8 PAT TIM MOR DEC LPG TUN LEV LEN (0..127.99). PAT is the host's: it
// picks the voice (SetVoice). TIM: Plaits' TIMBRE, the modulators' levels
// (64 = the voice as stored); MOR: Plaits' MORPH, the envelopes' rates (64 =
// as stored, Plaits' centre); DEC, LPG: Plaits' low-pass gate after the voice,
// decay and colour, pinged by each trig - DEC 127 leaves it out (the voice's
// own envelopes alone, so a held note sustains); TUN semitones from 64 (as
// PLAITS); LEV the output level; LEN the gate: 127 holds until NoteOff (the
// Rytm's note-off 0x11, or the next trig), lower a timed gate of
// 20 ms * 2^(LEN/14) (as JUNO's), released at its end or at NoteOff,
// whichever comes first. The velocity reaches the voice's operators as the
// DX7's (their velocity sensitivity).
//
// Loading a voice reloads the engine's bank (32 copies of the voice through
// user_data.h's fm_user_bank; on the Teensy, RYTM_FM_SINGLE_PATCH, one copy
// and the engine unpacks only it - teensy/setup_plaits_lib.sh): the next
// block of the new note is silent while the engine sets the voice up
// (Plaits' own 0.5 ms blank on a patch change). The voice alternates between
// the engine's patches 0 and 1, so the previous note rings out on its own
// patch (the engine's other voice) instead of being cut: no click on a PAT
// p-lock (builds with teensy/setup_plaits_lib.sh's patched six-op engine; the
// simulator's unpatched vendor build still cuts it). The first load after Init,
// Leave or a silent stretch (an empty slot) unloads both, as stock Plaits.
#pragma once
#include <math.h>
#include <string.h>

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/engine/engine.h"
#include "plaits/dsp/envelope.h"
#include "plaits/dsp/fx/low_pass_gate.h"
#include "plaits/dsp/voice.h"
#include "plaits/user_data.h"

namespace rytm {

#ifndef RYTM_FM_SINGLE_PATCH
#define RYTM_FM_SINGLE_PATCH 0
#endif

class FmMachine {
 public:
  enum { kVoiceBytes = 128, kEngine = 2, kCopies = RYTM_FM_SINGLE_PATCH ? 1 : 32,
         kStageBytes = kCopies * kVoiceBytes };

  void Init() {
    memset(v_, 0, sizeof v_);
    for (int i = 0; i < 8; ++i) p_[i] = 64.0f;
    p_[3] = 127.0f;
    p_[6] = 100.0f;
    p_[7] = 127.0f;
    has_ = gate_ = regap_ = released_ = false;
    reload_ = true;
    note_ = 60.0f;
    velocity_ = 0.8f;
    held_ = gate_len_ = 0;
    lpg_wait_ = 0;
    lpg_.Init();
    lpgate_.Init();
    slot_ = 0;
    played_ = noted_ = false;
    fresh_ = true;
  }

  // The DX7 voice to play (128 packed bytes, as in a 32-voice bank), or
  // nullptr: silence (an empty slot).
  void SetVoice(const uint8_t* v) {
    if (!v) {
      has_ = false;
      return;
    }
    if (has_ && memcmp(v, v_, kVoiceBytes) == 0) return;
    memcpy(v_, v, kVoiceBytes);
    has_ = true;
    reload_ = true;
  }
  bool has_voice() const { return has_; }

  // P1..P8.
  void SetKnobs(const float* p) {
    for (int i = 0; i < 8; ++i) p_[i] = p[i];
  }

  void NoteOn(float note, float velocity) {
    // a trig on a held gate, or right after a NoteOff no render has seen yet (a note-off and the next note-on
    // back to back, as a full-length step or the Pi's self-test sends them): low for a block, so the engine
    // sees an edge
    regap_ = gate_ || released_;
    gate_ = true;
    noted_ = true;   // Render: a note on the patch loaded for it (played_)
    note_ = note;
    velocity_ = velocity < 0.0f ? 0.0f : velocity > 1.0f ? 1.0f : velocity;
    held_ = 0;
    float k = p_[7];
    gate_len_ = k >= 127.0f ? 0 : (uint32_t)(48000.0f * 0.02f * exp2f(k / 14.0f));
    lpg_wait_ = plaits::kTriggerDelay + 1;   // the engine hears the trigger this late
  }

  void NoteOff() {
    gate_ = false;
    released_ = true;
  }

  // The voice leaves the machine: its Plaits voice reloads its ROM bank on
  // its next render and takes its level unpatched again (PLAITS' use).
  void Leave(plaits::Voice* voice, plaits::Modulations* mod) {
    voice->ReloadUserData();
    mod->level_patched = false;
    reload_ = true;
    gate_ = false;
    fresh_ = true;
  }

  // n samples (any n) into out, -1..1 before LEV. stage: kStageBytes of
  // scratch for a reload (shared by the voices one core renders: only used within a call; one per core,
  // as plaits::fm_user_bank, with RYTM_RENDER_CORES).
  void Render(plaits::Voice* voice, plaits::Patch* patch, plaits::Modulations* mod, uint8_t* stage, float* out,
              int n) {
    released_ = false;   // this render sees the gate as it is now
    if (!has_) {
      memset(out, 0, n * sizeof(float));
      fresh_ = true;   // the engine's voices froze mid-note: the next load unloads them all
      return;
    }
    const int kB = (int)plaits::kBlockSize;
    for (int done = 0; done < n;) {
      int size = n - done < kB ? n - done : kB;
      patch->engine = kEngine;
      // the patch the voice is in: 0 or 1 (slot_; with 32 copies, both are this voice) - 1.5 / 32 / 1.02 is patch 1
      // to the engine's quantizer, from either side
      patch->harmonics = slot_ ? 1.5f / (32.0f * 1.02f) : 0.0f;
      patch->timbre = p_[1] / 128.0f;
      patch->morph = p_[2] / 128.0f;
      patch->decay = p_[3] / 127.0f;
      patch->lpg_colour = p_[4] / 127.0f;
      patch->frequency_modulation_amount = 0.0f;
      patch->timbre_modulation_amount = 0.0f;
      patch->morph_modulation_amount = 0.0f;
      patch->note = note_ + roundf(p_[5] - 64.0f);
      mod->trigger_patched = true;
      mod->level_patched = true;
      mod->level = velocity_;
      mod->trigger = gate_ && !regap_ ? 1.0f : 0.0f;
      regap_ = false;
      if (gate_ && gate_len_) {
        held_ += size;
        if (held_ >= gate_len_) gate_ = false;
      }
      bool reload = reload_;
      if (reload) {
        // the other patch, once a note has played this one: that note rings out on it while the new one starts
        if (played_) slot_ ^= 1, played_ = false;
        patch->harmonics = slot_ ? 1.5f / (32.0f * 1.02f) : 0.0f;
        for (int i = 0; i < kCopies; ++i) memcpy(stage + i * kVoiceBytes, v_, kVoiceBytes);
        plaits::fm_user_bank() = stage;
        // value 2 (bit 1): unload every engine voice - the first load after Init / Leave / silence (a note frozen in the
        // engine since must not resume), as stock Plaits does on every load
        plaits::fm_user_slot() = slot_ | (fresh_ ? 2 : 0);
        fresh_ = false;
        voice->ReloadUserData();
      }
      if (noted_) played_ = true, noted_ = false;   // a note on this patch (a NoteOn, not a held gate)
      plaits::Voice::Frame f[plaits::kMaxBlockSize];
      voice->Render(*patch, *mod, f, size);
      if (reload) {
        plaits::fm_user_bank() = NULL;
        reload_ = false;
      }
      float* o = out + done;
      for (int s = 0; s < size; ++s) o[s] = f[s].out / 32768.0f;
      if (p_[3] < 127.0f) {   // Plaits' LPG, pinged by the trig (DEC 127: none)
        if (lpg_wait_ && --lpg_wait_ == 0) lpg_.Trigger();
        const float decay = p_[3] / 127.0f, hf = p_[4] / 127.0f;
        const float short_decay = (200.0f * kB) / plaits::kSampleRate * stmlib::SemitonesToRatio(-96.0f * decay);
        const float decay_tail = (20.0f * kB) / plaits::kSampleRate *
                                     stmlib::SemitonesToRatio(-72.0f * decay + 12.0f * hf) - short_decay;
        const float attack = plaits::NoteToFrequency(patch->note) * float(kB) * 2.0f;
        lpg_.ProcessPing(attack, short_decay, decay_tail, hf);
        lpgate_.Process(lpg_.gain(), lpg_.frequency(), lpg_.hf_bleed(), o, size);
      } else if (lpg_wait_) {
        --lpg_wait_;
      }
      const float lev = p_[6] / 127.0f;
      for (int s = 0; s < size; ++s) o[s] *= lev;
      done += size;
    }
  }

 private:
  uint8_t v_[kVoiceBytes];
  float p_[8];
  bool has_, gate_, regap_, reload_;
  int slot_ = 0;          // the engine patch (0 / 1) the voice was last loaded into
  bool played_ = false;   // a note played on slot_ since (its voice would be cut by a reload into slot_)
  bool noted_ = false;    // a NoteOn the render has not seen yet
  bool fresh_ = true;     // the next load unloads every engine voice (after Init, Leave, a silent stretch)
  bool released_;   // a NoteOff since the last render
  float note_, velocity_;
  uint32_t held_, gate_len_;
  int lpg_wait_;
  plaits::LPGEnvelope lpg_;
  plaits::LowPassGate lpgate_;
};

}  // namespace rytm
