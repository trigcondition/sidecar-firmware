// shim_dwc/circle/synchronize.h - no caches on the host
#pragma once
#include <circle/types.h>
#define DMA_BUFFER(type, name, num) alignas (64) type name[num]
#define CACHE_ALIGN alignas (64)
inline void CleanAndInvalidateDataCacheRange (uintptr, size_t) {}
inline void InvalidateDataCacheRange (uintptr, size_t) {}
inline void CleanDataCacheRange (uintptr, size_t) {}
inline void DataMemBarrier (void) {}
inline void EnterCritical (unsigned = 0) {}
inline void LeaveCritical (void) {}
#define IRQ_LEVEL 1
