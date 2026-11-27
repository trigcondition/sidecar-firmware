#pragma once
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/device.h>
class CUSBFunction : public CDevice {
 public:
  CUSBFunction(CUSBFunction* p) : parent(p) {}
  CUSBFunction(CUSBDevice* d, CUSBHostController* h) : dev(d), host(h) {}
  virtual ~CUSBFunction() {}
  virtual boolean Configure() { return FALSE; }
  CUSBDevice* GetDevice() { return parent ? parent->GetDevice() : dev; }
  CUSBHostController* GetHost() { return parent ? parent->GetHost() : host; }
  CUSBEndpoint* GetEndpoint0() { return nullptr; }
  CUSBFunction* parent = nullptr;
  CUSBDevice* dev = nullptr;
  CUSBHostController* host = nullptr;
};
