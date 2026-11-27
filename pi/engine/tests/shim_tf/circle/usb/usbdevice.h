#pragma once
#include <circle/usb/usb.h>
class CUSBStandardHub;
class CUSBHCIRootPort;
class CUSBDevice {
 public:
  const TUSBDeviceDescriptor* GetDeviceDescriptor() const { return &desc; }
  CUSBStandardHub* GetParentHub() const { return hub; }		// the hub the device hangs off (0: a root port)
  CUSBHCIRootPort* GetParentRootPort() const { return root; }	// the root port it hangs off directly
  TUSBDeviceDescriptor desc = {};
  CUSBStandardHub* hub = nullptr;
  CUSBHCIRootPort* root = nullptr;
};
