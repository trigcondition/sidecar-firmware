#pragma once
#include <circle/types.h>
#include <circle/usb/usb.h>
class CUSBEndpoint;
class CUSBRequest;
typedef void TURBCompletionRoutine(CUSBRequest*, void*, void*);
class CUSBRequest {
 public:
  CUSBRequest(CUSBEndpoint* e, void* b, unsigned n, TSetupData* s = nullptr) : ep(e), buf((u8*)b), len(n), setup(s) {
    if (++live() > peak()) peak() = live();
  }
  ~CUSBRequest() { --live(); }
  // circle-usb-urb-free.patch's count (here every instance: the tests make none on the stack)
  static unsigned& live() { static unsigned n = 0; return n; }
  static unsigned& peak() { static unsigned n = 0; return n; }
  static unsigned GetHeapInstances() { return live(); }
  static unsigned GetHeapInstancesPeak() { return peak(); }
  void SetCompletionRoutine(TURBCompletionRoutine* r, void* p, void* c) { fn = r, param = p, ctx = c; }
  void SetStatus(unsigned s) { status = s; }
  void SetResultLen(unsigned n) { result = n; }
  unsigned GetStatus() const { return status; }
  unsigned GetResultLength() const { return result; }
  TSetupData* GetSetupData() { return setup; }
  CUSBEndpoint* ep; u8* buf; unsigned len;
  TSetupData* setup;
  TURBCompletionRoutine* fn = nullptr; void* param = nullptr; void* ctx = nullptr;
  unsigned status = 0, result = 0;
};
