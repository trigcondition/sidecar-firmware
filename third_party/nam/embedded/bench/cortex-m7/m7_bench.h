/* Board-agnostic Cortex-M7 benchmark for the generated NAM engines.
 *
 * Your board port supplies the five hooks below, calls m7_bench_load() once
 * with the capture (.namb) and then m7_bench_run().
 * See daisy_seed/bench_main.cpp for a complete port (STM32H750). */
#ifndef M7_BENCH_H
#define M7_BENCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform hooks. */
uint32_t bench_cycles(void);            /* free-running cycle counter (DWT->CYCCNT) */
void bench_irq_off(void);               /* wrapped around each timed block */
void bench_irq_on(void);
void bench_cache_flush(void);           /* clean+invalidate D-cache, invalidate I-cache */
void bench_print(const char* fmt, ...); /* one line; the hook appends the newline */

/* Validates a .namb capture (container, CRC, A2 shape) and loads it into the
 * engine of its model (A2-Lite or A2-Full, through example/a2_engine.c).
 * namb must be 4-byte aligned; it can be memory-mapped flash. Prints the
 * result and returns 0 on success. */
int m7_bench_load(const void* namb, uint32_t size);

/* Times M7_BENCH_BLOCKS blocks of M7_BENCH_BLOCK frames and prints a report.
 * cpu_hz: core clock, used for the real-time load figure.
 * cold != 0 flushes the caches before every block (worst case for an audio
 * callback whose caches were trashed by other work). */
void m7_bench_run(uint32_t cpu_hz, int cold);

#ifdef __cplusplus
}
#endif

#endif
