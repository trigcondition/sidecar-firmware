#pragma once
#include <circle/types.h>
#define DESCRIPTOR_INTERFACE 4
#define DESCRIPTOR_ENDPOINT 5
#define REQUEST_OUT 0
#define REQUEST_TO_INTERFACE 1
#define SET_INTERFACE 11
#define REQUEST_TO_ENDPOINT 2
#define CLEAR_FEATURE 1
#define ENDPOINT_HALT 0
struct __attribute__((packed)) TUSBConfigurationDescriptor { u8 bLength, bDescriptorType; u16 wTotalLength; u8 bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower; };
struct __attribute__((packed)) TUSBInterfaceDescriptor { u8 bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; };
struct __attribute__((packed)) TUSBEndpointDescriptor { u8 bLength, bDescriptorType, bEndpointAddress, bmAttributes; u16 wMaxPacketSize; u8 bInterval; };
struct __attribute__((packed)) TUSBDeviceDescriptor {   // the standard 18 B (usb=mirror hands them on as bytes)
  u8 bLength = 18, bDescriptorType = 1; u16 bcdUSB = 0x200; u8 bDeviceClass = 0, bDeviceSubClass = 0, bDeviceProtocol = 0, bMaxPacketSize0 = 64;
  u16 idVendor = 0, idProduct = 0, bcdDevice = 0; u8 iManufacturer = 0, iProduct = 0, iSerialNumber = 0, bNumConfigurations = 1;
};
struct __attribute__((packed)) TSetupData { u8 bmRequestType, bRequest; u16 wValue, wIndex, wLength; };	// usb=mirror's declarations
#define REQUEST_IN 0x80			// usb=mirror's
#define DESCRIPTOR_STRING 3
