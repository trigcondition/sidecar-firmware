// core/msamp/sfz.h - the SFZ subset MULTISAMPLE reads (docs/MULTISAMPLE.md "2. The subset"). A small parser written
// for this project (no third-party code): text in, regions out, no file access, no allocation (the caller gives the
// arrays). Portable C++17; built only where CORE_MSAMP is set (the Pi, the Mac), never on the Teensy.
//
//   headers   <control> <global> <master> <group> <region>; opcodes inherit control > global > master > group >
//             region. Any other header (<curve> <effect> <midi> <sample> ...) and its opcodes are skipped (counted).
//   opcodes   sample default_path note_offset octave_offset sidecar_load=ram|stream|auto (control; ours); lokey hikey key pitch_keycenter lovel hivel
//             tune transpose pitch_keytrack volume amplitude pan width position amp_veltrack offset end loop_mode (loopmode)
//             loop_start (loopstart) loop_end (loopend) trigger seq_length seq_position lorand hirand ampeg_attack
//             ampeg_hold ampeg_decay ampeg_sustain ampeg_release. Others: ignored, counted, the first one named.
//   syntax    // and /* */ comments; #define $NAME value (substituted in later values); #include is refused
//             (counted, its line skipped); note names c4 = 60, c#4 / db4, octaves -1..9; `sample` (and
//             default_path) runs to the end of its line or the next opcode, so file names may hold spaces; '\'
//             in paths read as '/'.
//   paths     default_path + sample, relative to the .sfz's own folder (dir); a leading '/' is the card's root;
//             "." and ".." resolved (above the root: refused). Every distinct path is one entry of samples[].
#pragma once
#include <stdint.h>

namespace core {
namespace msamp {

constexpr int kPath = 192;            // a card path at most (with its folders)
constexpr uint32_t kUnset = 0xFFFFFFFFu;

enum LoopMode : uint8_t { kLoopAuto = 0, kNoLoop, kOneShot, kLoopContinuous, kLoopSustain };
enum Trigger : uint8_t { kTrigAttack = 0, kTrigRelease };

struct Region {
  int16_t sample;          // samples[] index, -1 none (a region without `sample` is dropped)
  uint8_t lokey, hikey, keycenter, lovel, hivel;
  uint8_t trigger;         // Trigger (first / legato read as attack)
  uint8_t loop_mode;       // LoopMode; kLoopAuto: the file's loop if it has one (continuous), else none
  int16_t transpose;       // semitones
  int16_t pitch_keytrack;  // cents a key (100)
  float tune;              // cents
  float volume;            // dB
  float amplitude;         // % (100)
  float pan;               // -100..100: on a voice routed OUT PRE / POST (docs/STEREO.md) a stereo sample's balance, a
                           // mono one's pan; TRK (the Rytm's mono channel) ignores it
  float width;             // -100..100 (100): a stereo sample's width on PRE / POST (0 mono, -100 swapped)
  float position;          // -100..100 (0): where a stereo sample sits on PRE / POST (with pan)
  float amp_veltrack;      // % (100)
  uint32_t offset;         // first frame (0)
  uint32_t end;            // last frame, inclusive; kUnset: the sample's last
  uint32_t loop_start, loop_end;   // inclusive; kUnset: the file's (its smpl chunk) or the whole sample
  uint16_t seq_length, seq_position;   // round robin (1, 1)
  float lorand, hirand;    // 0, 1
  float attack, hold, decay, sustain, release;   // ampeg_*: s, s, s, % (100), s
};

struct Desc {
  // the caller's arrays and their sizes
  Region* regions;
  int max_regions;
  char (*samples)[kPath];
  int max_samples;
  // out
  int nregions, nsamples;
  int ignored;             // opcodes not in the subset (and those under skipped headers)
  int skipped;             // regions dropped: no sample, a key/velocity range that is empty, end=-1, a full table
  int malformed;           // tokens that are no opcode (no '='), unknown headers, bad numbers, #include
  int load;                // <control> sidecar_load: 0 auto, 1 ram, 2 stream (our own opcode; other players ignore it)
  char first_ignored[32];  // the first ignored opcode's name ("" none)
  char err[96];            // why parse() returned false
};

// Parse len bytes of SFZ text; dir is the .sfz's folder on the card ("/MULTISAMPLE/Piano", no trailing '/').
// False (d->err says why) when no region is left.
bool parse(const char* text, int len, const char* dir, Desc* d);
// "c4" "C#3" "db-1" "60" -> 0..127 (note_offset / octave_offset not applied); -1 not a note
int note_value(const char* s);
// a + "/" + b with "." and ".." resolved, '\' as '/', no doubled '/'; b absolute ('/'...) replaces a. False: too
// long, or above the root.
bool join_path(const char* a, const char* b, char* out, int cap);

}  // namespace msamp
}  // namespace core
