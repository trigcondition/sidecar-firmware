// shim_dwc/circle/bcm2835.h - the DWC2's registers at offset 0 of the fake register file; bus addresses are handles
// into the test's table of buffers (test_gadget_ep0.cpp, dwhciregister.h)
#pragma once
#include <circle/types.h>
#define ARM_USB_BASE 0
#define ARM_USB_CORE_BASE ARM_USB_BASE
#define ARM_USB_HOST_BASE (ARM_USB_BASE + 0x400)
#define ARM_USB_DEV_BASE (ARM_USB_BASE + 0x800)
#define ARM_USB_POWER (ARM_USB_BASE + 0xE00)
u32 FakeBusAddress (uintptr p);
#define BUS_ADDRESS(addr) FakeBusAddress ((uintptr) (addr))
