#pragma once
#define XHCI_EP_MAX_URBS 16
class CXHCIEndpoint {
 public:
  unsigned GetPendingURBs() { return 0; }
  unsigned GetEPState() { return 1; }
};
