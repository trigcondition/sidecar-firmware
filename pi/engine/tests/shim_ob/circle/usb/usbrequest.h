#pragma once
#include <circle/types.h>
class CUSBEndpoint;
class CUSBRequest;
typedef void TURBCompletionRoutine(CUSBRequest*, void*, void*);
class CUSBRequest {
 public:
  CUSBRequest(CUSBEndpoint* e, void* b, unsigned n) : ep(e), buf((u8*)b), len(n) {}
  void SetCompletionRoutine(TURBCompletionRoutine* r, void* p, void* c) { fn = r, param = p, ctx = c; }
  void SetStatus(unsigned s) { status = s; }
  void SetResultLen(unsigned n) { result = n; }
  unsigned GetStatus() const { return status; }
  unsigned GetResultLength() const { return result; }
  CUSBEndpoint* ep; u8* buf; unsigned len;
  TURBCompletionRoutine* fn = nullptr; void* param = nullptr; void* ctx = nullptr;
  unsigned status = 0, result = 0;
};
