// plugins/mdkit/mdk_tap.h - the head of an mdkit machine's voice state: the core-rate (44.1 kHz) samples the voice
// rendered, before the resampler - its history, and what the host tests (tests/mdk_check.cpp) compare bit for bit
// with mdkit's own render. MIT, Copyright 2026 trigcondition and contributors.
#pragma once
#include <stdint.h>
#define MDK_TAP_MAGIC 0x4B54444Du   /* "MDTK" */
#define MDK_RING 64u                /* a power of two: core samples kept (the FIR's 32 + a block's ~12 + margin) */
typedef struct mdk_tap {
  uint32_t magic;            /* MDK_TAP_MAGIC */
  uint32_t ring_mask;        /* MDK_RING - 1 */
  uint64_t produced;         /* core samples rendered since the voice's TYPE was last set up (ring[i & mask] is i) */
  uint32_t epoch;            /* bumped at each TYPE set-up (the ring restarts from zeros) */
  uint32_t type;             /* the TYPE playing */
  float ring[MDK_RING];
} mdk_tap;
