#pragma once
#include <circle/usb/usb.h>
class CUSBEndpoint;
class CUSBRequest;
typedef void TURBCompletionRoutine(CUSBRequest*, void*, void*);
class CUSBRequest {
 public:
  CUSBRequest(CUSBEndpoint* e, void* b, unsigned n, TSetupData* s = nullptr) : ep(e), buf((u8*)b), len(n), setup(s) { ++live(); }
  ~CUSBRequest() { --live(); }
  static unsigned& live() { static unsigned n = 0; return n; }
  void SetCompletionRoutine(TURBCompletionRoutine* r, void* p, void* c) { fn = r, param = p, ctx = c; }
  unsigned GetStatus() const { return status; }
  // the test: the xHCI's completion (status 1 done, 0 an error)
  void Complete(unsigned s) { status = s; fn(this, param, ctx); }
  CUSBEndpoint* ep; u8* buf; unsigned len; TSetupData* setup;
  TURBCompletionRoutine* fn = nullptr; void* param = nullptr; void* ctx = nullptr;
  unsigned status = 0;
};
