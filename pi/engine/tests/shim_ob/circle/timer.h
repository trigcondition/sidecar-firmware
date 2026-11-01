#pragma once
class CTimer {
 public:
  static CTimer* Get() { static CTimer t; return &t; }
  static unsigned GetClockTicks() { return s_now; }
  void MsDelay(unsigned) {}
  static inline unsigned s_now = 0;
};
