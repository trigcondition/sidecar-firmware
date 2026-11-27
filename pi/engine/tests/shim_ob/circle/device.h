#pragma once
// CDevice's properties only (ob_usb.cpp matches a MIDI device to its box by them)
class CDevice {
 public:
  enum TProperty { PropertyVendor, PropertyProduct, PropertySerialNumber, PropertyFunction, PropertyUnknown };
  virtual ~CDevice() {}
  const char* GetProperty(TProperty p) const { return p < PropertyUnknown ? prop[p] : ""; }
  void SetProperty(TProperty p, const char* s) { if (p < PropertyUnknown) prop[p] = s; }
  const char* prop[PropertyUnknown] = {"", "", "", ""};
};
