#pragma once
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <deque>
#include <vector>
struct TCtl { u8 type, req; u16 value, index; };
class CUSBHostController {
 public:
  int ControlMessage(CUSBEndpoint*, u8 t, u8 r, u16 v, u16 i, void*, unsigned) { ctl.push_back({t, r, v, i}); return 0; }
  boolean SubmitAsyncRequest(CUSBRequest* u) { (u->ep->desc && (u->ep->desc->bEndpointAddress & 0x80) ? in : out).push_back(u); return TRUE; }
  std::vector<TCtl> ctl;
  std::deque<CUSBRequest*> in, out;
};
