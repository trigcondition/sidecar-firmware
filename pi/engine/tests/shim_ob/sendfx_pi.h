#pragma once
#include <stdint.h>
namespace sendfx_pi {
inline bool On() { return false; }
inline void FromVoices(const int16_t (*)[8], unsigned, const float (*)[8] = nullptr) {}
inline void AddReturn(int32_t (*)[12], unsigned, float (*)[4] = nullptr) {}
inline void FromRytm(const int32_t (*)[8], unsigned) {}
}
