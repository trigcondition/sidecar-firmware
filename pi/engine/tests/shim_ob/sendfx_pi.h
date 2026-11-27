#pragma once
#include <stdint.h>
namespace core {
namespace out {
struct Device;
}
}  // namespace core
namespace sendfx_pi {
inline bool On() { return false; }
inline bool On(int) { return false; }
inline void FromVoices(const int16_t (*)[8], unsigned, const float (*)[8] = nullptr) {}
inline void FromVoices(int, const int16_t (*)[8], unsigned, const float (*)[8] = nullptr) {}
inline void FromVoices(int, const core::out::Device&, const int16_t (*)[8], unsigned, const float (*)[8] = nullptr) {}
inline void AddReturn(int32_t (*)[12], unsigned, float (*)[4] = nullptr) {}
inline void AddReturn(int, const core::out::Device&, int32_t (*)[12], unsigned, float (*)[4] = nullptr) {}
inline void FromRytm(const int32_t (*)[8], unsigned) {}
inline void FromRytm(int, const int32_t (*)[8], unsigned) {}
}
