#pragma once
#include <stdint.h>
namespace bridge {
inline bool samp_wants_voices() { return false; }
inline bool samp_wants_in() { return false; }
inline bool tap_on() { return false; }
inline bool gr_live_on() { return false; }
inline bool livein_on() { return false; }
inline bool cal_on() { return false; }
inline void samp_voices(const int16_t (*)[8], unsigned, const int16_t (*)[16], const float (*)[4]) {}
inline void samp_in(const int32_t*, unsigned, unsigned) {}
inline void tap_in(const int32_t*, unsigned, unsigned) {}
inline void gr_live_in(const int32_t*, unsigned, unsigned) {}
inline void livein_in(const int32_t*, unsigned, unsigned) {}
inline void cal_in(const int32_t*, unsigned, unsigned) {}
}
