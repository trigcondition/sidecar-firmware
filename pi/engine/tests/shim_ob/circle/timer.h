#pragma once
#define CLOCKHZ 1000000
#define HZ 100
class CTimer {
 public:
  static CTimer* Get() { static CTimer t; return &t; }
  static unsigned GetClockTicks() { return s_now; }
  static unsigned long long GetClockTicks64() { return s_now64 ? s_now64 : s_now; }
  static inline unsigned long long s_now64 = 0;
  void MsDelay(unsigned) {}
  unsigned GetTicks() { return s_ticks; }   // HZ a second
  static inline unsigned s_ticks = 0;
  static inline unsigned s_now = 0;
};
