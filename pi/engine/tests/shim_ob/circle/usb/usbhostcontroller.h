#pragma once
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <deque>
#include <functional>
#include <vector>
struct TCtl { u8 type, req; u16 value, index; };
class CUSBHostController {
 public:
  int ControlMessage(CUSBEndpoint*, u8 t, u8 r, u16 v, u16 i, void* b, unsigned n) {
    if (t == 0xC0) return vendor.push_back({t, r, v, i}), vendor_in ? vendor_in(r, (u8*)b, n) : -1;   // a vendor IN (ObReadUid's UID): kept apart
    ctl.push_back({t, r, v, i});
    return 0;
  }
  // usb=mirror's string descriptors: what get_string answers (< 0: the box STALLs)
  int GetDescriptor(CUSBEndpoint*, u8 type, u8 index, void* buf, unsigned n, u8, u16 lang) {
    return get_string ? get_string(type, index, lang, (u8*)buf, n) : -1;
  }
  // control URBs (no endpoint descriptor: EP0) to ep0, the rest by direction; refuse: the controller's submit fails
  boolean SubmitAsyncRequest(CUSBRequest* u) {
    if (refuse) return FALSE;
    (!u->ep || !u->ep->desc ? ep0 : (u->ep->desc->bEndpointAddress & 0x80) ? in : out).push_back(u);
    return TRUE;
  }
  std::vector<TCtl> ctl, vendor;
  std::deque<CUSBRequest*> in, out, ep0;
  std::function<int(u8, u8, u16, u8*, unsigned)> get_string;
  std::function<int(u8, u8*, unsigned)> vendor_in;   // null: the box STALLs it (no UID)
  bool refuse = false;
};
