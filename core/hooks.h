// core/hooks.h - the platform layer's entry points into the core: where the core's SysEx for the host goes, and
// the user slots U1..U5 (machine plugins: the Teensy's RytmPlugins glue in voiceloop.ino). A board without
// machine plugins leaves every user hook null: U1..U5 are silent and core/slots.cpp answers the Rytm's slot
// messages as EMPTY slots.
#pragma once
#include <stdint.h>

namespace core {

struct Hooks {
  void (*to_rytm)(const uint8_t* m, int n);   // a SysEx F0..F7 for the host; null: dropped
  void (*trig_seen)();                        // a 0x04 trig report arrived (the Teensy's transport guess)
  // ---- user slots (all null without machine plugins)
  // A report for voice vi on user slot `slot` (0..4); was_slot the voice's slot before (-1 none). v[vi].p holds
  // its P1..P16; note -1 for a 0x03; tag the sound tag (0 untagged).
  void (*user_report)(int vi, int slot, int was_slot, int note, int vel, int tag);
  void (*user_leave)(int vi);                 // voice vi plays a built-in machine now
  void (*user_note_off)(int vi);              // 0x11 for a voice on a user slot
  void (*user_render)(int vi, float* out);    // kBlock samples, -1..1
  void (*slot_ask)(const uint8_t* d, int len);      // 0x19 (null: core/slots.cpp)
  void (*slot_assign)(const uint8_t* d, int len);   // 0x1E (null: core/slots.cpp)
  void (*project_seen)();                     // any 0x10 arrived (null: core/slots.cpp's rate-limited 1D)
  void (*project_changed)();                  // 0x10 named another project
  void (*slot_prints)(const uint8_t* d, int len);   // 0x25 SLOT PRINTS: the project's sounds' fingerprints per slot (null: ignored)
  void (*pool_changed)(int line);             // pool line `line` (0..126) changed; -1 the whole pool
  // the Rytm's transport as the mods report it (seq's kTransport state, whatever the Rytm's MIDI TRANSPORT SEND says),
  // as MIDI realtime: 0xFA start, 0xFB continue, 0xFC stop (pause too). The control core; null: ignored. The Pi's
  // recorder arms on it as on the MIDI messages (its gate is idempotent: both may arrive).
  void (*transport)(uint8_t status);
};
extern Hooks hooks;
// True while the core itself calls user_report for a parameter change of its own (voice_params_changed: the Sidecar's
// LFOs, the sequencer's step locks), not for a report from the Rytm: a user machine's glue tells a knob the user moved
// from a modulation (core/user/um.cpp: whose TYPE a slot's labels follow).
extern bool user_report_internal;

uint32_t to_rytm_dropped();                  // messages to_rytm dropped (no hook)
void to_rytm(const uint8_t* m, int n);

}  // namespace core
