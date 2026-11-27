#pragma once
#include <circle/usb/usbfunction.h>
#include <circle/timer.h>
class CUSBStandardHub : public CUSBFunction {
 public:
  CUSBStandardHub(CUSBDevice* d) : CUSBFunction(d, nullptr) {}
  boolean ReScanDevices() { ++rescans; return TRUE; }
  int rescans = 0;
  // remove_ok FALSE: the port not disabled, remove_us after (the request's 3 s timeout, or a quick refusal)
  boolean RemoveDeviceAt(unsigned i) { removed_at = (int) i; ++removes; CTimer::s_now += remove_us; return remove_ok; }
  int removes = 0, removed_at = -1;
  boolean remove_ok = TRUE;
  unsigned remove_us = 0;
};
