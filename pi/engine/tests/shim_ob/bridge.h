#pragma once
#include <stdint.h>
namespace bridge {
inline uint32_t to_host_dropped() { return 0; }
inline bool samp_wants_voices() { return false; }
inline bool samp_wants_in() { return false; }
inline bool tap_on() { return false; }
inline bool gr_live_on() { return false; }
inline bool livein_on() { return false; }
inline bool cal_on() { return false; }
inline uint64_t g_render_frames = 0;   // seq::frames(): the test sets it before each IN block (the looper's arrival stamp)
inline uint64_t render_frames() { return g_render_frames; }
inline bool sidechain_on() { return false; }
inline void samp_voices(const int16_t (*)[8], unsigned, const int16_t (*)[16], const float (*)[4]) {}
inline void samp_in(const int32_t*, unsigned, unsigned) {}
inline void tap_in(const int32_t*, unsigned, unsigned) {}
inline void gr_live_in(const int32_t*, unsigned, unsigned) {}
inline void livein_in(const int32_t*, unsigned, unsigned) {}
inline void cal_in(const int32_t*, unsigned, unsigned) {}
inline void sidechain_in(const int32_t*, int, int) {}
}
