#pragma once
// the relay's driver as teensy_flash.cpp sees it: is it on the link, the bootloader request, its log
#include <string.h>
class CUSBDevice;
class CTeensyTarget {
 public:
  static CTeensyTarget* Get() { return up ? (CTeensyTarget*)&up : nullptr; }
  static void RequestBootloader() { boot_requests++; }
  static void RequestLog() { log_requests++; }
  static unsigned LogReads() { return log_reads; }
  static unsigned Log(char* out, unsigned cap) { unsigned n = (unsigned)strlen(log) < cap ? (unsigned)strlen(log) : cap; memcpy(out, log, n); return n; }
  CUSBDevice* GetDevice() { return dev; }	// the relay's device (its hub: the port poll while it moves)
  static inline CUSBDevice* dev = nullptr;
  static inline bool up = false;
  static inline unsigned boot_requests = 0, log_requests = 0, log_reads = 0;
  static inline const char* log = "";
};
