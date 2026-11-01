#pragma once
class CKernelOptions {
 public:
  static CKernelOptions* Get() { static CKernelOptions o; return &o; }
  unsigned GetAppOptionDecimal(const char*, unsigned d) { return d; }
  const char* GetAppOptionString(const char*, const char* d) { return d; }
};
