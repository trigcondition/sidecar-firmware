// shim_dwc/circle/usb/gadget/dwusbgadget.h - a gadget for test_gadget_ep0: EP0's callbacks recorded, class / vendor
// requests deferred as the Sidecar's mirror defers them (sidecar_gadget.cpp OnControlRequest)
#pragma once
#include <circle/usb/gadget/dwusbgadgetendpoint.h>
#include <circle/usb/usb.h>
#include <circle/types.h>
#include <vector>
class CDWUSBGadget {
public:
  static const unsigned NumberOfEPs = 7, NumberOfInEPs = 7;
  enum { ControlNotHandled, ControlDeferred };
  void AssignEndpoint (unsigned nEP, CDWUSBGadgetEndpoint *pEP) { m_pEP[nEP] = pEP; }
  void RemoveEndpoint (unsigned nEP, CDWUSBGadgetEndpoint *) { m_pEP[nEP] = nullptr; }
  void CountIsoPending (int) {}
  const void *GetDescriptor (u16, u16, size_t *pLength) { *pLength = sizeof m_Desc; return m_Desc; }
  void SetDeviceAddress (u8) { ++set_address; }
  boolean SetConfiguration (u8) { ++set_config; return TRUE; }
  boolean OnSetInterface (u8, u8) { ++set_interface; return TRUE; }
  int OnGetInterface (u8) { return 0; }
  int OnClassOrVendorRequest (const TSetupData *pSetup, u8 *) { ++class_requests; return pSetup->bmRequestType & REQUEST_IN ? -1 : 0; }
  int OnControlRequest (const TSetupData *pSetup, const u8 *pOutData) {
    requests.push_back (*pSetup);
    if (pOutData && pSetup->wLength) out_data.assign (pOutData, pOutData + pSetup->wLength);
    return defer ? ControlDeferred : ControlNotHandled;
  }
  void OnControlAborted (void) { ++aborted; }
  void FlushTxFIFO (unsigned) {}
  // the gadget's IRQ handler for one EP (CDWUSBGadget is the EPs' friend)
  void OutIrq (unsigned nEP) { m_pEP[nEP]->HandleOutInterrupt (); }
  void InIrq (unsigned nEP) { m_pEP[nEP]->HandleInInterrupt (); }
  CDWUSBGadgetEndpoint *m_pEP[NumberOfEPs + 1] = {};
  u8 m_Desc[18] = {18, 1};
  bool defer = true;
  int aborted = 0, set_interface = 0, set_address = 0, set_config = 0, class_requests = 0;
  std::vector<TSetupData> requests;
  std::vector<u8> out_data;
};
