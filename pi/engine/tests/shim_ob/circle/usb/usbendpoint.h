#pragma once
#include <circle/usb/usb.h>
#include <circle/usb/xhciendpoint.h>
class CUSBDevice;
class CUSBEndpoint {
 public:
  CUSBEndpoint(CUSBDevice*, const TUSBEndpointDescriptor* d = nullptr) : desc(d) {}
  CXHCIEndpoint* GetXHCIEndpoint() { return &x; }
  const TUSBEndpointDescriptor* desc;
  CXHCIEndpoint x;
};
