// pi/engine/tests/shim_ob/ - just enough of Circle (and of the kernel's neighbours) to compile the real ob_usb.cpp on the Mac:
// test_ob_usb.cpp drives it as the xHCI would. A fake bus, no hardware model.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef uintptr_t uintptr;
typedef int boolean;
#define FALSE 0
#define TRUE 1
