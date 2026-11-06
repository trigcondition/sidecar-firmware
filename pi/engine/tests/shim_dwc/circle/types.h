// shim_dwc/circle/types.h - Circle's basic types on the host (test_gadget_ep0)
#pragma once
#include <cstddef>
#include <cstdint>
#include <assert.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef uintptr_t uintptr; typedef int boolean;
#define FALSE 0
#define TRUE 1
