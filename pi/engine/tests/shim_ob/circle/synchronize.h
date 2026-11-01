#pragma once
#include <circle/types.h>
#define IRQ_LEVEL 1
inline void EnterCritical(unsigned = 0) {}
inline void LeaveCritical() {}
inline void InvalidateDataCacheRange(uintptr, size_t) {}
