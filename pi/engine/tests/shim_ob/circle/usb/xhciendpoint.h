#pragma once
#include <deque>
#define XHCI_EP_MAX_URBS 16
class CUSBRequest;
class CXHCIEndpoint {
 public:
  unsigned GetPendingURBs() { return ring ? (unsigned)ring->size() : 0; }
  unsigned GetEPState() { return state; }
  unsigned GetLastErrorCode() const { return 4; }
  bool IsValid() { return s_valid; }
  // a halted endpoint: Reset Endpoint + Set TR Dequeue to the enqueue TRB (every TD still queued is skipped), then
  // circle-xhci-dropurbs.patch's DropURBs forgets their URBs
  // as Circle's: TRUE whatever the commands gave (it returns !code); a reset that did not take leaves the state at 2
  bool ResetFromHalted() { ++resets; if (reset_ok) state = 3; return true; }
  unsigned DropURBs() { unsigned n = GetPendingURBs(); if (ring) ring->clear(); return n; }
  static inline bool s_valid = true;   // false: the controller refused the endpoint
  unsigned state = 1;                  // 1 running, 2 halted, 3 stopped
  bool reset_ok = true;   // false: the commands fail, the endpoint stays halted
  int resets = 0;
  std::deque<CUSBRequest*>* ring = nullptr;   // the test's: the host's queue of this endpoint
};
