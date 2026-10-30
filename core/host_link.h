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
// The link to the Rytm went down or came up (the Rytm's hello 35, its USB MIDI attached or gone, the Overbridge
// stream started or stopped): every voice silenced (CC 120's ramp, until its next trig) and the send FX's tails
// cleared, as a double STOP does - a note-on whose note-off (or a STOP) was lost across the gap must not hang.
// why: for the log. Rate-limited with all_sound_off (100 ms): an attach's several edges clear once.
void link_reset(const char* why);
void sendfx_project();   // the send FX's user-FX slot map for `project` (boot, 0x10)
void host_project_changed();   // 2026-10-27: a project switch the slot reports made (core/bundle/prjslot.h): as a 10's
}  // namespace core
