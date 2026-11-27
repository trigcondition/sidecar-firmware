#pragma once
// Host-test stand-in for pi/engine/teensy_target.h: ob_usb.cpp's USB merge (TCircleIo) asks whether the Teensy relay
// shows the Rytm and takes the relay computer's packets for it. No relay in these tests.
#include <stdint.h>
class CTeensyTarget {
 public:
  static bool ShowsRytm() { return false; }
  static bool ForRytm(uint32_t*) { return false; }
  static bool ForRytmInSysEx() { return false; }
  static void RytmSendFailed() {}
  static void RytmMidiConnected(bool) {}
};
