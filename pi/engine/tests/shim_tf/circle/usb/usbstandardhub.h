#pragma once
// the hub as teensy_flash.cpp sees it: its device, and a port poll asked for (circle-hub-poll.patch)
class CUSBDevice;
class CUSBStandardHub {
 public:
  CUSBDevice* GetDevice() { return dev; }
  void RequestPortPoll() { polls++; }
  CUSBDevice* dev = nullptr;
  unsigned polls = 0;
};
