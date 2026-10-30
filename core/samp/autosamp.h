// core/samp/autosamp.h - the AUTO-SAMPLER (docs/MULTISAMPLE.md §7, docs/SAMPLING.md): plays notes on a source,
// records each, trims, normalises once for the instrument, finds loops, and writes WAV + .sfz into
// /MULTISAMPLE/<name>/ (the instrument list rescans: INST picks it at once).
//
//   sources  (c) the Sidecar's own voices, rendered offline, faster than real time, on a private engine instance (the
//                live voices untouched): 6-OP FM (a DX7 voice: the ROM's or the card's FM list, by PAT) or JUNO (a
//                factory patch or the card's JUNO list), with P1..P8 as given, or a copy of what Sidecar voice V plays
//                now (its machine, its knobs, its DX7 voice / JUNO patch). Each take: a fresh voice, note on, hold,
//                note off, the release until 100 ms under -80 dBFS (or tail_ms).
//            (b) the Rytm's own tracks: a note on the track's MIDI channel through the host link (core::to_rytm, 3-byte
//                channel messages, as the LFOs' CCs), that track's Overbridge output recorded (IN 2..9, mono: BD SD
//                RS/CP BT LT MT/HT CH/OH CY/CB, so RS and CP share IN 4 ...), in real time. The 12 tracks BD..CB, the
//                channel the track's (the Rytm's default: track N on channel N). chro: the notes on the auto channel
//                play the active track chromatically [inferred: the Rytm manual's AUTO channel; not run]; else every
//                take is the track's own pitch (velocity layers and round robins only).
//            (a) an external synth: its notes on a MIDI output (kPortUsb: the computer on the Sidecar's USB-C MIDI,
//                which routes it on; kPortDev: the first USB MIDI device on the Pi's USB-A ports; kPortRytm: the
//                Rytm's USB MIDI, reaching a synth only if the Rytm passes it on [inferred: not known to]), its audio
//                into the Rytm's AUDIO IN L/R recorded from Overbridge IN 10/11: L, R or both (a stereo instrument), in
//                real time, as (b).
//   plan     notes lo..hi every step semitones, 1..8 velocity layers, 1..4 round robins, the hold (note length) and
//            the release capture (tail), the gap between takes (Rytm), loop finding on/off, the latency (Rytm: auto =
//            the onset of the first take at the top velocity; then one cut for every take), a name.
//   output   /MULTISAMPLE/<name>/<name>.sfz and samples/<name>_<C4>_v<vel>_rr<n>.wav (24-bit mono 48 kHz, stereo for EXT
//            L+R; a smpl loop
//            for sustained notes): zones split halfway between the sampled notes, velocity layers between the
//            velocities, round robins as seq_length / seq_position, loop_sustain with the loop found, ampeg_release
//            the takes' release.
//   cores    command / tick on the control core (tick sends the Rytm's notes on time and asks for the rescan); poll on
//            the card core (core 0's loop on the Pi: the offline renders, 100 ms of audio a call, and the files);
//            in_block on core 0 (the IN completion: a Rytm take's frames). The Mac: one thread.
#pragma once
#include <stdint.h>

namespace core {
namespace samp {

enum ASrc : uint8_t { kAFm = 0, kAJuno, kAVoice, kATrack, kAExt };
constexpr int kASources = 5;   // the source kinds in the AUTO page's SRC order (kAFm .. kAExt)
constexpr int kATracks = 12;   // the Rytm's tracks BD SD RS CP BT LT MT HT CH OH CY CB
enum APort : uint8_t { kPortUsb = 0, kPortDev, kPortRytm };   // kAExt: the MIDI output
constexpr int kAPorts = 3;
enum AIn : uint8_t { kInLR = 0, kInL, kInR };                  // kAExt: AUDIO IN L+R (stereo), L, R
int track_stem(int track);    // a Rytm track's Overbridge output 0..7 (IN 2 + it): BD SD RS/CP BT LT MT/HT CH/OH CY/CB
enum AState : int { kAIdle = 0, kATaking, kAWriting, kADone, kAFailed, kACancelled };

struct Plan {
  ASrc src = kAFm;
  int pat = 0;                // kAFm / kAJuno: the list entry (PAT)
  int voice = 0;              // kAVoice: the Sidecar voice 0..7 to copy
  int track = 0;              // kATrack: the Rytm's track 0..11 (BD .. CB), recorded from its stem (track_stem)
  int midi_ch = 0;            // kATrack, kAExt: the channel the notes go to (0..15)
  bool chro = false;          // kATrack: the notes play the track chromatically (the auto channel) [inferred]
  int lo = 36, hi = 96, step = 6;
  int nvel = 1;
  uint8_t vels[8] = {100};
  int rr = 1;
  uint32_t hold_ms = 1500, tail_ms = 2500, gap_ms = 150;
  bool loop = true;
  int lat_ms = -1;            // -1: measured (kATrack); offline: 0
  float knobs[8] = {0, 64, 64, 127, 64, 64, 100, 127};   // kAFm / kAJuno: P1..P8 (P1 is pat; JUNO: the patch's own)
  char name[32] = "";         // "" from the source ("FM_E_PIANO_1", "JUNO_A11", "BD")
  uint8_t port = kPortUsb;    // kAExt: the MIDI output (APort)
  uint8_t in = kInLR;         // kAExt: the input (AIn)
};

struct AStatus {
  int state;
  int done, total;            // takes
  int written;
  char label[24];             // "C4 v100 3/24"
  char name[32];
  char sfz[112];
  char error[64];
  float lat_ms;               // the latency used (kATrack)
  float speed;                // offline: audio seconds rendered a second (x real time)
  int loops;                  // takes with a loop
  float gain_db;
  uint32_t kb;                // the samples' size (WAV bytes / 1024), set when done
  int zones, layers, rr;      // the plan run: notes, velocity layers, round robins
  bool offline;               // the run renders offline (the Sidecar's engines: silent, faster than real time)
};

bool asamp_start(const Plan& p, char* err, int cap);   // the control core's
void asamp_cancel();          // stops after the current take; the files written so far removed, no .sfz
bool asamp_poll();            // the card core's: one bounded step; true when it did one
void asamp_tick();            // the control core's, every pass
void asamp_in_block(const int32_t* f, int n, int stride);   // core 0: IN frames (a Rytm take)
bool asamp_wants_in();
void asamp_status(AStatus* s);
bool asamp_command(const char* p, char* out, int cap);   // the console's asamp... (docs/SAMPLING.md)
const Plan& asamp_plan();
void asamp_configure(const Plan& p);   // the plan the console's asamp= edits (the remote UI's AUTO page)
int asamp_entries(ASrc s);             // the source's list entries: FM (the card's list, else the ROM's), JUNO 128, VOICE 8,
                                       // TRACK 12, EXT 16 x the ports (port * 16 + channel)
// The platform's MIDI outputs for kAExt (the Pi: cores.cpp): send m (n bytes, one channel message) on port; n 0: is the
// port there on this build. Not set: only kPortRytm (core::to_rytm).
typedef bool (*AMidiOut)(int port, const uint8_t* m, int n);
void asamp_set_midi_out(AMidiOut f);
const char* asamp_port_name(int port);   // "USB", "DEV", "RYTM"
// the source's name ("SOLID BASS", "A11 Brass", "VOICE 3", "BD", "USB 1") and the instrument's name the plan makes ("" -> from the source)
void asamp_names(const Plan& p, char* src, int scap, char* inst, int icap);
int asamp_json(char* out, int cap);   // /status's "asamp":{...} (any core)
void asamp_reset();           // tests

}  // namespace samp
}  // namespace core
