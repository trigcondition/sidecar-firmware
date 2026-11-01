#pragma once
#include <circle/usb/usb.h>
enum { USBSpeedHigh = 2 };
class CUSBDevice {
 public:
  const TUSBConfigurationDescriptor* GetConfigurationDescriptor() { return (const TUSBConfigurationDescriptor*)config; }
  const TUSBDeviceDescriptor* GetDeviceDescriptor() const { return &desc; }
  unsigned GetSpeed() { return USBSpeedHigh; }
  const u8* config = nullptr;
  TUSBDeviceDescriptor desc = {};
};
