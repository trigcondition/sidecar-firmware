#pragma once
#include <circle/types.h>
class CSidecarGadget {   // no gadget in the test
 public:
  static CSidecarGadget* Get() { return nullptr; }
  void ToRytm(s32 (*)[12], const s16 (*)[8], unsigned, boolean) {}
  void FromRytmBlock(const u8*) {}
  void RytmMIDIConnected(bool) {}
  bool ForRytm(u32*) { return false; }
  bool ForRytmInSysEx() { return false; }
};
