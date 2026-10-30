// core/machines.h - what a report's voice byte names, and the knobs' zones (sim/protocol.py's docstring and
// eng_zone; rytm-mods 0021). Moved from teensy/voiceloop/voiceloop.ino (kVoices, kBlock, eng_zone, the curated
// Plaits list).
#pragma once
#include <stdint.h>

namespace core {

constexpr int kVoices = 8;    // the Rytm's physical voices
constexpr int kBlock = 12;    // frames a render call makes (plaits::kBlockSize; voices.cpp checks it)

// The machine: voice byte & 0x78, matched exactly; anything else plays as PLAITS.
enum Machine : int { kPlaits = 0x00, kJuno = 0x08, kDaisy = 0x10, kDspSample = 0x20, kFm = 0x48 };
// 0x40: MD, the built-in mdkit drum machine (core/md/md.h: ENGINE FM / PI / TRX + TYPE, the MD user machine's voices),
// since 2026-10-29 in place of DIGIDRUM (soejrd/digidrum's TRX / EFM voices) on the same flag: 0021's machine 42.
// A report with DIGIDRUM's old page plays as MD (md::knobs_of maps its ENG). Pi and Mac (CORE_MD); the Teensy has no
// MD, a voice reported with it is silent there.
constexpr int kMd = 0x40;
// GRAIN as its own machine (1026): DSP SAMPLE's GRAIN mode with DSP SAMPLE's words (P9..P16 TUN FIN SMP SPRY POS SIZE
// DENS LEV: MOD's word is SPRY, the spray), no resonator. NOT on the Rytm: rui1026's 0021 had no room for the machine,
// so no Rytm ever sent its flag; GRAIN is DSP SAMPLE's MOD = GRAIN there, and GRANULAR (0x38) superseded it. Its old
// placeholder flag 0x18 is CHOP's since 2026-10-24 (the last free one); GRAIN keeps a value no voice byte can carry
// (machine_of masks 0x78), so its code stays and nothing selects it.
constexpr int kGrainMachine = 0x98;
// CHOP (core/chop/chop.h, docs/CHOP.md): a sample cut into up to 64 slices, Pi and Mac only. On the Rytm from rui1051
// (0021's machine 49, MT TEST); `chop=voice:V` plays it on voice V whatever built-in machine is reported.
constexpr int kChopMachine = 0x18;
constexpr float kGrainModValue = 64.0f;   // a MOD value in GRAIN's zone (rytm::SampleModes::Zone(64, 7) == kGrain)
// MULTISAMPLE (core/msamp/msamp.h, docs/MULTISAMPLE.md): Pi and Mac only. NOT on the Rytm yet (a 0021 change): no
// Rytm sends this flag; until one does, `msamp=voice:V` plays it on voice V whatever built-in machine is reported.
constexpr int kMultiMachine = 0x30;
// WAVETABLE (core/wavetable/wavetable.h, docs/WAVETABLE.md) and GRANULAR (core/granular/granular.h, docs/GRANULAR.md):
// Pi and Mac only, NOT on the Rytm yet (a 0021 change: the machine list's entries and labels); until then
// `wt=voice:V` / `gr=voice:V` play them on voice V whatever built-in machine is reported. GRANULAR supersedes GRAIN.
constexpr int kWaveMachine = 0x28;
constexpr int kGranMachine = 0x38;
// LIVE IN (core/livein/livein.h, docs/LIVE-IN.md): the device's AUDIO IN through the voice's INSERT chain and route. Pi
// and Mac only; on the Rytm from rui1051 (0021's machine 48, LT TEST); `livein=voice:V` plays it on voice V whatever
// built-in machine is reported.
constexpr int kLiveMachine = 0x78;
inline int machine_of(uint8_t voice_byte) { return voice_byte & 0x78; }
// The user slots U1..U5 (flags 0x50 0x58 0x60 0x68 0x70) -> 0..4; -1 not a user slot (rmp_slot_of_flag's rule).
inline int user_slot_of(int m) { return (m >= 0x50 && m <= 0x70 && !(m & 7)) ? (m - 0x50) >> 3 : -1; }

// The machines' names for the user, one table for every surface (UX review T3): short (at most 6 characters: the
// Rytm's SIDECAR page, the touchscreen's HOME tiles and Settings) and long (the Rytm's machine list's own words where
// it has room: "MULTISAMP" there, a firmware field; prompts, VOICE's title). m: the voice byte's machine (& 0x78).
inline const char* machine_short(int m) {
  switch (m) {
    case kPlaits: return "PLAITS";
    case kJuno: return "JUNO";
    case kDaisy: return "DAISY";
    case kDspSample: return "DSPS";
    case kFm: return "FM";
    case kMd: return "MD";
    case kGrainMachine: return "GRAIN";
    case kMultiMachine: return "MULTI";
    case kWaveMachine: return "WAVE";
    case kGranMachine: return "GRAN";
    case kLiveMachine: return "LIVEIN";
    case kChopMachine: return "CHOP";
    default: {
      static const char* const u[5] = {"U1", "U2", "U3", "U4", "U5"};
      int s = user_slot_of(m);
      return s >= 0 ? u[s] : "?";
    }
  }
}
inline const char* machine_long(int m) {
  switch (m) {
    case kPlaits: return "PLAITS";
    case kJuno: return "JUNO";
    case kDaisy: return "DAISYSP";
    case kDspSample: return "DSP SAMPLE";
    case kFm: return "6-OP FM";
    case kMd: return "MD";
    case kGrainMachine: return "GRAIN";
    case kMultiMachine: return "MULTISAMPLE";
    case kWaveMachine: return "WAVETABLE";
    case kGranMachine: return "GRANULAR";
    case kLiveMachine: return "LIVE IN";
    case kChopMachine: return "CHOP";
    default: {
      static const char* const u[5] = {"USER 1", "USER 2", "USER 3", "USER 4", "USER 5"};
      int s = user_slot_of(m);
      return s >= 0 ? u[s] : "?";
    }
  }
}

// The curated ENG lists (rytm-mods 0021, 2026-10-09): the knob's value v (P's integer part, 0..127) in N equal
// zones, zone (v * N) >> 7, at most N - 1. PLAITS: 17 of Plaits' models (NOISE, the 6-OP FMs and the drums left
// out); DAISYSP and DSP SAMPLE: MODAL STRING RESON.
inline int eng_zone(float p, int n) {
  int v = (int)p;
  v = v < 0 ? 0 : v > 127 ? 127 : v;
  int z = (v * n) >> 7;
  return z >= n ? n - 1 : z;
}
constexpr int kPlaitsEngs = 17;
constexpr uint8_t kPlaitsModels[kPlaitsEngs] = {0, 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20};

// A report's P1..Pn: two bytes each, the value 0..127 then its fraction in 1/128 steps.
inline void decode_params(const uint8_t* raw, int np, float* p) {
  for (int k = 0; k < np; ++k) p[k] = raw[2 * k] + raw[2 * k + 1] / 128.0f;
}

}  // namespace core
