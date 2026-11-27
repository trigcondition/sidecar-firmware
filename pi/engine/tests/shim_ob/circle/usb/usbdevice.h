#pragma once
#include <circle/usb/usb.h>
#include <circle/usb/usbhcirootport.h>
#include <circle/timer.h>
class CUSBStandardHub;
enum { USBSpeedLow, USBSpeedFull, USBSpeedHigh, USBSpeedSuper, USBSpeedUnknown };
class CUSBDevice {
 public:
  const TUSBConfigurationDescriptor* GetConfigurationDescriptor() { return (const TUSBConfigurationDescriptor*)config; }
  const TUSBDeviceDescriptor* GetDeviceDescriptor() const { return &desc; }
  unsigned GetSpeed() { return speed; }
  CUSBHCIRootPort* GetParentRootPort() const { return root; }
  CUSBStandardHub* GetParentHub() const { return hub; }
  unsigned GetParentHubPortIndex() const { return hub_port; }
  // (the test deletes the function itself). remove_ok FALSE: the hub did not disable the port, remove_us after (its
  // request's 3 s timeout, or a quick refusal)
  boolean RemoveDevice() { ++removes; CTimer::s_now += remove_us; return remove_ok; }
  int removes = 0;
  boolean remove_ok = TRUE;
  unsigned remove_us = 0;
  CUSBStandardHub* hub = nullptr;
  unsigned hub_port = 0;
  unsigned speed = USBSpeedHigh;
  CUSBHCIRootPort* root = nullptr;
  const u8* config = nullptr;
  TUSBDeviceDescriptor desc = {};
};
