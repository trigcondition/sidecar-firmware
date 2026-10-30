// core/notes.h - a stock Rytm's MIDI notes as the mods' reports (sim/protocol.py): MIDI channel 1..8 plays
// voice 0..7 on the machine chosen for it; note-on -> 0x04 TRIG+PARAMS with the voice's P1..P16, a note-off (or
// velocity 0) of the voice's last note -> 0x11 (an older note's off is ignored, the newer one keeps playing), CC 16..31 -> P1..P16 and a 0x03 PARAMS (sim/obhost.py's CC numbers). Each machine starts
// from defaults that sound (core_golden.py's scenarios): PLAITS VA, JUNO factory patch 0 held, 6-OP FM ROM voice
// 0 held, DAISYSP MODAL, DSP SAMPLE pool slot 1 dry. Shared by the boards (docs/core.md, "MIDI
// notes"): the Pi engine (notes=1, on by default) and the Teensy's voiceloop (:notes=1, off by default). The
// reports go to the board's callback (core::engine_sysex there, after its own counting): the adapter keeps no
// render state, so it runs on the control side only (one instance per board, never per render core).
// Header-only types: Circle-side code includes it too.
#pragma once
#include <stdint.h>

namespace core {

class NoteAdapter {
 public:
  typedef void (*SysExFn)(const uint8_t* d, int len);
  explicit NoteAdapter(SysExFn out);
  bool SetMachine(int voice, const char* name);   // plaits juno daisy dsps grain fm md; false: unknown name or voice
  int Machine(int voice) const { return mach_[voice & 7]; }
  static const char* Name(int flag);
  static const char* Names();   // the names SetMachine knows, space-separated
  void Channel(uint8_t status, uint8_t d1, uint8_t d2);

 private:
  void Defaults(int voice);
  void Send(int cmd, int voice, int note, int vel);
  SysExFn out_;
  uint8_t mach_[8];
  uint8_t p_[8][16];
  int8_t last_[8];   // the note sounding on each voice, -1 none
};

}  // namespace core
