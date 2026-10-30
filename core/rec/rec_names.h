// core/rec/rec_names.h - what the two recorders share besides the WAV header (rec_wav.h): the sources and their
// file names, the channel lists ("master,bd,+in_l"), the take names (/Recordings/<prefix>_NNN/NN_NAME.wav) and the
// PLAY / STOP arming. The Pi's multitrack recorder (pi/engine/recorder.h) records any of the 25 sources; the
// Sidecar Mini's (core/rec/mini_rec.h) the Overbridge IN ones (0..12). Portable, cold (program flash on the Teensy).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rec {

// Sources (their id is the file's NN): 0 MASTER (IN 0/1, stereo), 1..12 IN 0..11 (mono), 13..20 the Sidecar's voices
// 1..8 (mono; stereo for a voice routed OUT PRE / POST when the take starts, core/fx/out.h), 21/22 FX 1/2 returns
// (stereo), 23 / 24 the PRE / POST pairs of the PRE / POST voices as sent to 2/3 and 0/1 (stereo).
const int kSources = 25, kMaster = 0, kIn = 1, kVoice = 13, kFx = 21, kPrePair = 23, kPostPair = 24;
// the OUT side's channels: 8 voices (L), FX1 L/R, FX2 L/R, PRE L/R, POST L/R; then the voices' R (kSideR + v)
const int kInChannels = 12, kVoices = 8, kSide = kVoices + 8, kSideR = kSide, kSideFrame = kSide + kVoices;
const uint32_t kMaxData = 0xFFF00000u;   // a file's data limit (FAT32: 4 GB - 1, and RIFF's 32-bit sizes)
const uint32_t kInMask = ((1u << kInChannels) - 1) << kIn;   // the 12 IN channels as mono files ("stems")

struct Source { const char* name; uint8_t width; uint8_t side; uint8_t ch; };   // side: from out_block; ch: its first
const Source& source(int id);
// IN L/R (ids 11, 12) carry the Rytm's FX return (rytm-mods 0030-fx-in, core::fxin) while set: named FX_L / FX_R
// (11_FX_L.wav, 12_FX_R.wav). "fx_l" / "fx_r" parse either way.
void set_fxin(bool on);
bool fxin();
// "all", "none", or a comma list of ids or names (case-insensitive: master, main_l, bd, rs-cp, in_l, voice1, fx1, ...);
// "+id"/"-id" add or remove. False: an unknown entry (err says which).
bool parse_list(const char* s, uint32_t* mask, char* err, unsigned errlen);
int list_ids(uint32_t mask, char* out, unsigned cap);   // "0,1,13"
// st: the voices (bit v) recorded in stereo (their OUT PRE / POST at the take's start): one more sample a frame each
unsigned samples_per_frame(uint32_t mask, uint8_t st = 0);
unsigned files(uint32_t mask);
uint32_t bytes_per_second(uint32_t mask, unsigned bits, uint8_t st = 0);

// take names: "<prefix>_NNN" (NNN from 001, one more than the highest of that prefix in /Recordings)
bool valid_prefix(const char* p);                       // [A-Za-z0-9_-], 1..24
int take_number(const char* name, const char* prefix);  // NNN of "<prefix>_NNN", else -1
void take_name(char* out, unsigned cap, const char* prefix, int n);
void take_dir(char* out, unsigned cap, const char* take);           // /Recordings/<take>
void file_path(char* out, unsigned cap, const char* take, int id);  // /Recordings/<take>/<NN>_<NAME>.wav
bool is_wav_name(const char* name);                                  // *.wav / *.WAV

// The Pi's take names (docs/BACKLOG.md 5d): "<slug>_<YYYY-MM-DD_HHMM>" with a wall clock (local time), "_2", "_3" ...
// when that name is on the card already; "<slug>_take-NNN" without one (one more than the highest of that slug).
// The slug is the Rytm project's name: lowercase a-z 0-9, every run of anything else one '-', none at either end,
// at most kSlugMax ("" when nothing is left). Older takes ("take_004") keep their names.
const unsigned kSlugMax = 24;
const unsigned kTakeMax = 64;   // a take's id with its volume ("NVME:" + 24 + 1 + 15 + "_NN") and the NUL
void slug(const char* name, char* out, unsigned cap);
void stamp_name(char* out, unsigned cap, const char* slug, uint32_t local);   // local: seconds since 1970, zone applied
void date_time(uint32_t local, char* out, unsigned cap);                     // "YYYY-MM-DD HH:MM"
void variant(char* out, unsigned cap, const char* base, int k);   // k 1: base, else "<base>_<k>"
int variant_number(const char* name, const char* base);          // 1 base, k "<base>_<k>", else 0 (any case)
void seq_name(char* out, unsigned cap, const char* slug, int n);  // "<slug>_take-NNN"
int seq_number(const char* name, const char* slug);              // NNN of "<slug>_take-NNN" (any case), else -1
// the number to try first: clock (base: the stamped name) one more than the highest variant_number on the card (1:
// the base is free), else one more than the highest seq_number of slug
int next_number(const char (*names)[64], int n, const char* slug, bool clock, const char* base);

// PLAY / STOP while armed (MIDI realtime: FA start, FB continue, FC stop), any core
inline void transport_gate(uint8_t s, bool arm_mode, volatile uint8_t* go) {
  if (s == 0xFA || s == 0xFB) {
    if (arm_mode) *go = 1;
  } else if (s == 0xFC) {
    if (arm_mode) *go = 0;
  }
}

}  // namespace rec
