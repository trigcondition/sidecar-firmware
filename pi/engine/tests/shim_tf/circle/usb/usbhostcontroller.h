#pragma once
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <deque>
class CUSBHostController {
 public:
  boolean SubmitAsyncRequest(CUSBRequest* u) { if (refuse) return FALSE; ep0.push_back(u); return TRUE; }
  std::deque<CUSBRequest*> ep0;
  bool refuse = false;
};
