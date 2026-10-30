/*
 * a2_engine: one NAM A2 engine slot that runs either an A2-Lite or an A2-Full
 * capture, chosen from the .namb file at load time.
 *
 * Example code built on the engines' own API (nam_a2lite_*, nam_a2full_*),
 * for products that support both models. It links both engines and overlays
 * their memory: only one model runs at a time, so the slot costs as much
 * memory as A2-Full alone. Copy and adapt it; for a single model, call that
 * engine's API directly (see its header).
 *
 * Memory, placed with these macros (default: ordinary .bss):
 *   A2_HOT    the instance and the capture (8-51 KB): fast memory, e.g. DTCM
 *   A2_STATE  the ring buffers (100-290 KB): e.g. AXI SRAM behind the D-cache
 *
 * Not thread-safe: a2_load and a2_reset must not run concurrently with
 * a2_process.
 */
#ifndef A2_ENGINE_H
#define A2_ENGINE_H

#include <stdint.h>

#include "nam_a2full.h"
#include "nam_a2lite.h"

/* Largest num_frames a2_process accepts: the smaller of the two engines'
 * NAM_A2LITE_MAX_BUFFER_SIZE / NAM_A2FULL_MAX_BUFFER_SIZE. */
#if NAM_A2LITE_MAX_BUFFER_SIZE < NAM_A2FULL_MAX_BUFFER_SIZE
#define A2_MAX_FRAMES NAM_A2LITE_MAX_BUFFER_SIZE
#else
#define A2_MAX_FRAMES NAM_A2FULL_MAX_BUFFER_SIZE
#endif

typedef enum { A2_NONE = 0, A2_LITE, A2_FULL } a2_model_t;

/* Validate a .namb capture (container, optionally the CRC, A2 shape), load it
 * into the engine of its model, then reset and prewarm. namb must be 4-byte
 * aligned and can be memory-mapped flash; it is not needed afterwards.
 * Returns 0, or -1 with *err set and the previous capture still loaded.
 * Not real-time: the prewarm alone is ~6,347 samples of work. */
int a2_load(const void* namb, uint32_t size, int verify_crc, const char** err);

/* Clear the signal state and prewarm, e.g. after a mute or a bypass. */
void a2_reset(void);

/* Process n <= A2_MAX_FRAMES mono samples; in and out may not overlap.
 * Outputs silence while no capture is loaded. */
void a2_process(const float* in, float* out, int n);

a2_model_t a2_model(void);
const char* a2_model_name(a2_model_t m);

#endif
