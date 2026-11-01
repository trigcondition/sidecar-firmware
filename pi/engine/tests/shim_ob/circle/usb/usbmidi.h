#pragma once
#include <circle/types.h>
class CUSBMIDIDevice {
 public:
  boolean SendEventPackets(const u8*, unsigned) { return TRUE; }
};
