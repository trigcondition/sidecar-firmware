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
  // circle-xhci-stopep.patch: Stop Endpoint, then DropURBs (a stream restarted on the Pi, attach_gate.h)
  unsigned StopAndDrop() { ++stops; state = 3; if (s_on_stop) s_on_stop(this); return DropURBs(); }
  int stops = 0;
  static inline void (*s_on_stop)(CXHCIEndpoint*) = nullptr;   // the test's: its host forgets this endpoint's URBs
  // circle-xhci-iv1count.patch: the test sets s_iv1 / s_iv1_busy / s_iv1_list
  static unsigned GetPeriodicIv1(unsigned* busy = nullptr, char* list = nullptr, unsigned size = 0) {
    if (busy) *busy = s_iv1_busy;
    if (list && size) { unsigned n = 0; for (; s_iv1_list[n] && n + 1 < size; n++) list[n] = s_iv1_list[n]; list[n] = 0; }
    return s_iv1;
  }
  static inline unsigned s_iv1 = 0, s_iv1_busy = 0;
  static inline const char* s_iv1_list = "";
  static inline bool s_valid = true;   // false: the controller refused the endpoint
  unsigned state = 1;                  // 1 running, 2 halted, 3 stopped
  bool reset_ok = true;   // false: the commands fail, the endpoint stays halted
  int resets = 0;
  std::deque<CUSBRequest*>* ring = nullptr;   // the test's: the host's queue of this endpoint
};
