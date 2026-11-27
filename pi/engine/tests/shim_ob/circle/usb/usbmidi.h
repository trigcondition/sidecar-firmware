#pragma once
#include <circle/types.h>
#include <circle/device.h>
class CUSBDevice;
// fail: the next sends that fail, each halting the bulk OUT (a halted endpoint fails every send until reset)
class CUSBMIDIDevice : public CDevice {
 public:
  CUSBDevice* GetUSBDevice() const { return usb; }   // circle-midi-usbdevice.patch
  CUSBDevice* usb = nullptr;
  boolean SendEventPackets(const u8*, unsigned) {
    if (fail > 0) return --fail, halted = true, FALSE;
    if (halted) return FALSE;
    return ++sent, TRUE;
  }
  boolean OutHalted() { return halted; }   // circle-midi-reset2.patch
  boolean ResetOutHalted() {   // circle-midi-reset.patch (v2: FALSE when the state after it is still halted)
    if (!halted) return FALSE;
    ++resets;
    if (!reset_ok) return FALSE;
    halted = false;
    return TRUE;
  }
  int fail = 0, resets = 0;
  unsigned sent = 0;
  bool halted = false, reset_ok = true;
};
