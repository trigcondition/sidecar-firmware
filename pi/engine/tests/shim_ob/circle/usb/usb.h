#pragma once
#include <circle/types.h>
#define DESCRIPTOR_INTERFACE 4
#define DESCRIPTOR_ENDPOINT 5
#define REQUEST_OUT 0
#define REQUEST_TO_INTERFACE 1
#define SET_INTERFACE 11
struct __attribute__((packed)) TUSBConfigurationDescriptor { u8 bLength, bDescriptorType; u16 wTotalLength; u8 bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower; };
struct __attribute__((packed)) TUSBInterfaceDescriptor { u8 bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; };
struct __attribute__((packed)) TUSBEndpointDescriptor { u8 bLength, bDescriptorType, bEndpointAddress, bmAttributes; u16 wMaxPacketSize; u8 bInterval; };
struct TUSBDeviceDescriptor { u16 idVendor, idProduct; };
