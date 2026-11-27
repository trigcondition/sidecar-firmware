#pragma once
#define CLOCKHZ 1000000
class CTimer { public: static unsigned long long GetClockTicks64() { return s_now64; } static inline unsigned long long s_now64 = 1; };
