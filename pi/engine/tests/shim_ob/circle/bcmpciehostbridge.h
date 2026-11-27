#pragma once
#include <map>
#include "types.h"
// circle-pcie-config.patch: the test fills s_cfg[bus][offset] (an empty map: no host bridge) and counts the writes
class CBcmPCIeHostBridge {
 public:
  static inline std::map<unsigned, u32> s_cfg[2];
  static inline int s_writes = 0;
  static inline bool s_write_ok = true;   // false: the write does not take
  static u32 ConfigRead(unsigned bus, unsigned slot, unsigned func, unsigned where) {
    if (s_cfg[0].empty() || bus > 1 || slot || func || where > 0xffc) return ~0U;
    auto it = s_cfg[bus].find(where & ~3U);
    return it == s_cfg[bus].end() ? 0 : it->second;
  }
  static bool ConfigWrite(unsigned bus, unsigned slot, unsigned func, unsigned where, u32 v) {
    if (s_cfg[0].empty() || bus > 1 || slot || func || where > 0xffc) return false;
    ++s_writes;
    if (s_write_ok) s_cfg[bus][where & ~3U] = (s_cfg[bus][where & ~3U] & 0xffff0000u) | (v & 0xffff);
    return true;
  }
  static unsigned FindCapability(unsigned bus, unsigned slot, unsigned func, unsigned id) {
    if (!((ConfigRead(bus, slot, func, 4) >> 16) & 0x10)) return 0;
    unsigned p = ConfigRead(bus, slot, func, 0x34) & 0xfc;
    for (unsigned n = 0; p >= 0x40 && n < 48; n++) {
      u32 h = ConfigRead(bus, slot, func, p);
      if ((h & 0xff) == id) return p;
      p = (h >> 8) & 0xfc;
    }
    return 0;
  }
};
