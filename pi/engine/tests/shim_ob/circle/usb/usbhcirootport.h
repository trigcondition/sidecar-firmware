#pragma once
#include <circle/types.h>
class CUSBHCIRootPort {
 public:
  virtual ~CUSBHCIRootPort() {}
  virtual boolean ReScanDevices() = 0;
  virtual boolean RemoveDevice() { ++removes; return TRUE; }   // (deletes nothing here)
  int removes = 0;
};
