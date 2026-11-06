// shim_dwc/circle/usb/dwhciregister.h - CDWHCIRegister on a fake register file: reads and writes go through the
// test's core model (test_gadget_ep0.cpp: write-1-to-clear interrupt registers, EP enable / disable, the global OUT NAK)
#pragma once
#include <circle/usb/dwhci.h>
#include <circle/types.h>
u32 FakeRead (u32 nAddress);
void FakeWrite (u32 nAddress, u32 nValue);
class CDWHCIRegister {
public:
  CDWHCIRegister (u32 nAddress) : m_nAddress (nAddress), m_nBuffer (0), m_bValid (false) {}
  CDWHCIRegister (u32 nAddress, u32 nValue) : m_nAddress (nAddress), m_nBuffer (nValue), m_bValid (true) {}
  u32 Read (void) { m_nBuffer = FakeRead (m_nAddress); m_bValid = true; return m_nBuffer; }
  void Write (void) { FakeWrite (m_nAddress, m_nBuffer); }
  u32 Get (void) const { return m_nBuffer; }
  void Set (u32 nValue) { m_nBuffer = nValue; m_bValid = true; }
  void And (u32 nMask) { m_nBuffer &= nMask; }
  void Or (u32 nMask) { m_nBuffer |= nMask; }
  void ClearBit (unsigned n) { m_nBuffer &= ~(1u << n); }
  void SetBit (unsigned n) { m_nBuffer |= 1u << n; }
  boolean IsSet (u32 nMask) const { return (m_nBuffer & nMask) != 0; }
private:
  u32 m_nAddress, m_nBuffer;
  bool m_bValid;
};
