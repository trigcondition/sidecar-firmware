#pragma once
#include <stdint.h>
struct TRecStub {
  bool wants_in() { return false; }
  bool wants_side() { return false; }
  void in_block(const int32_t (*)[12], unsigned) {}
  void out_block(const int16_t (*)[8], const float (*)[4], unsigned, const int16_t (*)[16], const float (*)[4]) {}
  void set_stereo_voices(uint8_t) {}
};
struct TPlayStub { void mix(int32_t (*)[12], unsigned) {} };
inline TRecStub g_Rec;
inline TPlayStub g_Play;
