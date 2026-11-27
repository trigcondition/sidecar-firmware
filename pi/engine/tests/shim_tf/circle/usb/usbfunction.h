#pragma once
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbhostcontroller.h>
class CUSBFunction {
 public:
  CUSBFunction(CUSBFunction* p) : parent(p) {}
  CUSBFunction(CUSBDevice* d, CUSBHostController* h, CUSBEndpoint* e) : dev(d), host(h), ep0(e) {}
  virtual ~CUSBFunction() {}
  virtual boolean Configure() { return FALSE; }
  CUSBDevice* GetDevice() { return parent ? parent->GetDevice() : dev; }
  CUSBHostController* GetHost() { return parent ? parent->GetHost() : host; }
  CUSBEndpoint* GetEndpoint0() { return parent ? parent->GetEndpoint0() : ep0; }
  CUSBFunction* parent = nullptr; CUSBDevice* dev = nullptr; CUSBHostController* host = nullptr; CUSBEndpoint* ep0 = nullptr;
};
