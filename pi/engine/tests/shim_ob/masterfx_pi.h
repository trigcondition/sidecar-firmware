#pragma once
#include <stdint.h>
namespace core {
namespace out {
struct Device;
}
}  // namespace core
namespace masterfx_pi {
inline void (*g_process_hook)(int32_t (*)[12], unsigned) = nullptr;   // a test's marker on the master chain's place in the OUT fill
inline void Process(int32_t (*F)[12], unsigned n) { if (g_process_hook) g_process_hook(F, n); }
inline void Process(int, const core::out::Device&, int32_t (*F)[12], unsigned n) { if (g_process_hook) g_process_hook(F, n); }
inline bool WantStems() { return false; }
inline bool WantStems(int) { return false; }
inline void Stems(const int32_t (*)[8], unsigned) {}
inline void Stems(int, const int32_t (*)[8], unsigned) {}
inline void Reset() {}
inline void Reset(int) {}
}
