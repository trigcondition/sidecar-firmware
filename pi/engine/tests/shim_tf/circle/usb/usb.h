#pragma once
#include <circle/types.h>
struct __attribute__((packed)) TSetupData { u8 bmRequestType, bRequest; u16 wValue, wIndex, wLength; };
struct __attribute__((packed)) TUSBDeviceDescriptor { u8 bLength, bDescriptorType; u16 bcdUSB; u8 c, s, p, mps; u16 idVendor, idProduct, bcdDevice; u8 a, b, d, n; };
