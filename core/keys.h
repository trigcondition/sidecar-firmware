// core/keys.h - keyboards on the Sidecar itself (keys-in): USB MIDI keyboards in the Pi's USB-A ports and the computer
// on the Sidecar's USB-C MIDI port, as polyphonic note sources. The Rytm plays one note a track (CHRO), so a keyboard
// through the Rytm reaches the host only as its mono trigs; through the Pi every note-on / note-off is real.
//
// Routing (no setup, mode "selected", the default): the notes play the Rytm's selected track. When its machine is a
// Sidecar poly machine (poly::plays_poly: 6-OP FM, MULTISAMPLE, WAVETABLE, GRANULAR) they play the Sidecar voice directly (poly::key_on, each
// note its own until its note-off); otherwise (an analog track, a mono Sidecar machine) they go to the Rytm on its
// auto channel (kAutoCh, settable), which plays its selected track - never both. Which track is selected, from what the
// Rytm already says (rytm-mods 0026 has no report of it):
//   1. the stock SRC page of a Sidecar track shows (29 t 1, ui::remote_src_track): t;
//   2. the Rytm's seq mode (2D 08 on a track, seq::seqmode_on): that track;
//   3. learned: while neither says, the notes go to the Rytm's auto channel and the Rytm's own trig (0x04) for a forwarded
//      note-on within kEchoMs names the voice it played: that track is the selected one (the 0x04 swallowed, the held
//      keys started on the voice). Forgotten kLearnMs after the last key (and when 1. changes), then learned again.
// Mode "channel": MIDI channel N plays Rytm track N (1..12), 13..16 go to the Rytm as they are. The computer's notes
// always route by channel, except on the auto channel (the selected track): a DAW sequencing the Rytm's track channels
// keeps working, now polyphonic on Sidecar tracks.
// Sustain (CC 64): note-offs held while down, for the Sidecar's notes and the forwarded ones. Pitch bend, other CCs,
// aftertouch, program changes: to the Rytm on the routed channel (the knobs edit the track; the Sidecar's engines have
// no pitch bend). CC 120 / 123: the source's notes on that channel released now (and forwarded).
// Recording: each note goes to the Sidecar sequencer as heard (seq::note: live record, step entry). While the Rytm records
// live (2D 06 + PLAY) on a Sidecar track that is not a seq track, the gesture's first note also goes to the Rytm, so it
// records a trig (its 0x04 swallowed: no double), and a chord of 2+ notes (kGestureMs apart at most) is written back to
// that step (28 01, as a pad chord's, core/poly.h).
// Threads: the control side (the Pi's core 3), as poly; midi() from the platform's queue, tick() from poly::tick.
#pragma once
#include <stdint.h>

namespace core {
namespace keys {

constexpr int kSrcs = 5;                 // 0..3 the USB-A keyboards (in plug order), 4 the computer (USB-C)
constexpr int kComputer = 4;
constexpr int kAutoCh = 14;              // the Rytm's auto channel (1..16; its default) unless set
constexpr uint32_t kEchoMs = 30;         // a forwarded note-on -> the Rytm's 0x04 for it
constexpr uint32_t kLearnMs = 4000;      // a learned selection forgotten this long after the last key
enum Mode : uint8_t { kSelected = 0, kChannel = 1 };
enum Route : uint8_t { kNone = 0, kPoly = 1, kRytm = 2, kProbe = 3 };   // the last note's: Sidecar voice, the Rytm, learning

#if CORE_POLY
// The platform's: a channel message for the Rytm (the Pi: core 0 sends it on the Rytm's USB MIDI); nullptr drops them
void set_forward(void (*f)(uint8_t st, uint8_t d1, uint8_t d2));
void midi(int src, uint8_t st, uint8_t d1, uint8_t d2);   // a channel message from source src
void tick(uint32_t ms);                                    // poly::tick's: the gesture windows, the learned track's age
bool take_trig(int vi, int note);                          // poly::take_live's: a 0x04 for a forwarded note: true swallowed
void source(int src, bool present, const char* name);      // a keyboard plugged in / out (the computer: its MIDI port)
bool computer_on();                                        // the computer's notes come here (else straight to the Rytm)
int selected();                                            // the selected track as known now (0..11), -1 unknown
int auto_channel();                                        // the Rytm's auto channel 1..16 (keys=auto:N, default 14)

struct View {
  int src;               // the last source played (-1 none yet)
  const char* name;      // its name ("" unknown)
  int track;             // its target track (0..11), -1 the Rytm's selected (unknown)
  int voice;             // the Sidecar voice playing it, -1 none
  Route route;
  int held;              // keys held now (every source)
  int present;           // sources present (bits)
  Mode mode;
};
View view();
const char* line(char* b, int cap);   // the remote SIDECAR page's bottom bar: "KEYS MPKMINI > SD POLY", null: no source

struct Stats {
  uint32_t notes, poly, forwarded, probes, learned, swallowed, writes, sustained, dropped;
};
const Stats& stats();
// The settings: /.rytm/keys.txt "mode=selected|channel", "auto=1..16", "mac=1|0" (load at boot, save from idle_save)
void load();
bool save();
// The console's "keys" (the status), "keys=selected|channel", "keys=auto:N", "keys=mac:on|off"
bool command(const char* p, char* out, int cap);
void reset();   // tests: every key up, nothing learned, the defaults, no sources
#endif

}  // namespace keys
}  // namespace core
