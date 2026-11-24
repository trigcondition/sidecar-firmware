// core/host_link.h - the host's SysEx into the engine: one whole message F0 7D 52 54 <cmd> ... F7 (rytm-mods
// 0021/0023 and the user slots; sim/protocol.py). Moved from teensy/voiceloop/voiceloop.ino's report().
#pragma once
#include <stdint.h>

#include "hooks.h"
#include "pool/pool.h"
#include "slots.h"
#include "voices.h"

namespace core {
// IN L/R source (rytm-mods 0030-fx-in): the Rytm's delay+reverb return on Overbridge IN L/R (channels 10/11) instead of
// the codec inputs. Host -> Rytm 26 <v>: 1 FX return, 0 AUDIO IN, 0x7F ask; Rytm -> host 27 <s>: 1 on, 0 off, sent for
// every 26 and unasked each time the Rytm's USB port comes up (a Rytm resets itself to off on a disconnect).
namespace fxin {
bool wanted();        // the setting: IN L/R carry the FX return (kept in /.rytm/fxin.txt "src=fx|in", default inputs)
int rytm();           // the Rytm's last 27: 1 FX return, 0 inputs, -1 none yet
bool active();        // rytm() == 1: IN L/R are FX_L / FX_R now (recorder names, SCOPE source names)
const char* state();  // "on" "off" "unknown" (no answer yet) "unsupported" (asked 3 s ago, never answered)
void load();          // boot: the setting from the card
bool save();          // the card, if the setting changed (engine.cpp idle_save): true if it wrote
void set(bool on);    // the setting: kept, and sent to the Rytm
void ask();           // 26 7F
// The console's "fxin", "fxin=1|0|?" (the answer in out); false: not an fxin command
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);   // "\"fxin\":{\"want\":1,\"state\":\"on\",\"rytm\":1}"
}  // namespace fxin

void host_sysex(const uint8_t* d, int len);
// params-delta (rytm-mods 0021): a Rytm told so in the host's 1D (core/sysex.h kHostCapsDelta) sends a parameter-only
// report as 56 PARAMS DELTA, only the values that changed since its last report of the voice:
//   F0 7D 52 54 56 01 <voice byte> <m0 P1..P7> <m1 P8..P14> <m2 P15 P16> {<hi> <lo>} x n <tag hi> <tag lo> F7
// (13 + 2n bytes; mask bit i = P(i+1), the values in P order, each as 03's two bytes). The host keeps each voice's last
// 16 values from its 03 / 04 / 43 / 44 and 56s and plays a 56 as the 03 they make: the same state an 03 would leave.
// A 56 for a voice with no such values (this host started after the Rytm's last whole report) is dropped and a resync
// owed: the next 1D asks the Rytm for whole reports again (kHostCapsResync).
int host_slots_caps(uint8_t* m, int n);   // a 1D (13 / 15 bytes) with this host's caps (16), an owed resync taken
bool host_resync_due();                   // a resync is owed: send a 1D soon (its own rate limit: 250 ms)
// The link to the Rytm went down or came up (the Rytm's hello 35, its USB MIDI attached or gone, the Overbridge
// stream started or stopped): every voice silenced (CC 120's ramp, until its next trig) and the send FX's tails
// cleared, as a double STOP does - a note-on whose note-off (or a STOP) was lost across the gap must not hang.
// why: for the log. Rate-limited with all_sound_off (100 ms): an attach's several edges clear once.
void link_reset(const char* why);
void link_lost();   // the Rytm's USB MIDI gone (the platform's edge): what is sent only to a Rytm that listens stops
void sendfx_project();   // the send FX's user-FX slot map for `project` (boot, 0x10); read on the card core, the Rytm told after it
bool sendfx_step();      // each idle pass: that read in, the Rytm told; true when it was
void host_project_changed();   // 2026-10-27: a project switch the slot reports made (core/bundle/prjslot.h): as a 10's
}  // namespace core
