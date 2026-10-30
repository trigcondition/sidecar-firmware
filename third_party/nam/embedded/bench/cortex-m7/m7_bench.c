/* Board-agnostic Cortex-M7 benchmark: see m7_bench.h.
 *
 * Report format:
 *
 *   capture: <model>, <n> weights, CRC ok, loaded and prewarmed in <n> cycles
 *   m7_bench: warm, block 48 x 1000
 *   cycles/block: min <n>  p50 <n>  p99 <n>  max <n>
 *   mean <n> cycles/sample = <x>% of a 48 kHz real-time budget at <f> MHz
 *   output: <n> samples checked, max abs err <e> (limit <l>): OK | FAIL
 *
 * Output check: every 1000th output sample is compared with values computed
 * by a host build of the same engines for the captures in weights/
 * (expected.h). The limit is 1e-4 x peak; a correct build is typically
 * within 4e-7. With another capture the timings stay valid but the check
 * reports FAIL; define M7_BENCH_NO_CHECK to skip it.
 */
#include "m7_bench.h"

#include <stdlib.h>
#include <string.h>

#include "a2_engine.h"
#include "namb_reader.h"

#ifndef M7_BENCH_BLOCK
#define M7_BENCH_BLOCK 48
#endif
/* Default: 1 s of audio at 48 kHz (1000 blocks of 48, 3000 of 16), which is
 * also what the expected-output table covers. */
#ifndef M7_BENCH_BLOCKS
#define M7_BENCH_BLOCKS ((48000 + M7_BENCH_BLOCK - 1) / M7_BENCH_BLOCK)
#endif

#if M7_BENCH_BLOCK > A2_MAX_FRAMES
#error "M7_BENCH_BLOCK must be <= NAM_A2LITE_MAX_BUFFER_SIZE and NAM_A2FULL_MAX_BUFFER_SIZE"
#endif

#define M7_BENCH_CHECK_STEP 1000
#if !defined(M7_BENCH_NO_CHECK) && !defined(M7_BENCH_DUMP)
#include "expected.h" /* m7_bench_expected_<model>[], m7_bench_expected_<model>_peak */
#endif

static uint32_t g_cycles[M7_BENCH_BLOCKS];

/* Parabolic sine with a slow amplitude ramp. Float arithmetic only, so host
 * and device inputs are bit-identical. */
static float test_signal(long i)
{
    float p = (float)(i % 126) / 63.0f - 1.0f;
    float s = 4.0f * p * (1.0f - (p < 0.0f ? -p : p));
    float amp = 0.1f + 0.4f * (float)(i % 1000) / 1000.0f;
    return amp * s;
}

static int cmp_u32(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

static uint32_t timer_overhead(void)
{
    uint32_t best = 0xffffffffu;
    for (int i = 0; i < 32; i++) {
        bench_irq_off();
        uint32_t t0 = bench_cycles();
        uint32_t t1 = bench_cycles();
        bench_irq_on();
        if (t1 - t0 < best)
            best = t1 - t0;
    }
    return best;
}

int m7_bench_load(const void* namb, uint32_t size)
{
    const char* err;
    const uint32_t t0 = bench_cycles();
    if (a2_load(namb, size, 1, &err) != 0) {
        bench_print("capture: %s", err);
        return -1;
    }
    const uint32_t t1 = bench_cycles();
    namb_model_t m;
    namb_parse(namb, size, &m);
    bench_print("capture: %s, %lu weights, CRC ok, loaded and prewarmed in %lu cycles",
                a2_model_name(a2_model()), (unsigned long)m.weight_count, (unsigned long)(t1 - t0));
    return 0;
}

void m7_bench_run(uint32_t cpu_hz, int cold)
{
    float in[M7_BENCH_BLOCK], out[M7_BENCH_BLOCK];

    bench_print("m7_bench: %s, %s, block %d x %d", a2_model_name(a2_model()), cold ? "cold" : "warm",
                M7_BENCH_BLOCK, M7_BENCH_BLOCKS);
    a2_reset(); /* reset + prewarm, as an app does after a capture change */

    const uint32_t ovh = timer_overhead();
    int nonfinite = 0;
#if !defined(M7_BENCH_NO_CHECK) && !defined(M7_BENCH_DUMP)
    const float* expected = a2_model() == A2_LITE ? m7_bench_expected_a2lite : m7_bench_expected_a2full;
    const float peak = a2_model() == A2_LITE ? m7_bench_expected_a2lite_peak : m7_bench_expected_a2full_peak;
    const int n_expected = M7_BENCH_EXPECTED_COUNT;
    int n_checked = 0;
    float max_err = 0.0f;
#endif
#if defined(M7_BENCH_DUMP)
    uint32_t peak_bits = 0;
#endif
    long idx = 0;
    uint64_t total = 0;
    for (int b = 0; b < M7_BENCH_BLOCKS; b++) {
        for (int i = 0; i < M7_BENCH_BLOCK; i++)
            in[i] = test_signal(idx + i);
        if (cold)
            bench_cache_flush();
        bench_irq_off();
        uint32_t t0 = bench_cycles();
        a2_process(in, out, M7_BENCH_BLOCK);
        uint32_t t1 = bench_cycles();
        bench_irq_on();
        g_cycles[b] = (t1 - t0) - ovh;
        total += g_cycles[b];
        for (int i = 0; i < M7_BENCH_BLOCK; i++, idx++) {
            uint32_t bits;
            memcpy(&bits, &out[i], sizeof(bits));
            /* Bit test: -ffast-math may assume no NaN/Inf and drop v != v. */
            if ((bits & 0x7f800000u) == 0x7f800000u)
                nonfinite++;
#if defined(M7_BENCH_DUMP)
            if ((bits & 0x7fffffffu) > peak_bits) /* |x| order = bit order for finite floats */
                peak_bits = bits & 0x7fffffffu;
#endif
            if (idx % M7_BENCH_CHECK_STEP != 0)
                continue;
#if defined(M7_BENCH_DUMP)
            bench_print("expected %ld %08lx", idx / M7_BENCH_CHECK_STEP, (unsigned long)bits);
#elif !defined(M7_BENCH_NO_CHECK)
            if (n_checked < n_expected) {
                float e = out[i] - expected[n_checked++];
                e = e < 0.0f ? -e : e;
                if (!(e <= max_err)) /* also catches NaN */
                    max_err = e;
            }
#endif
        }
    }

    qsort(g_cycles, M7_BENCH_BLOCKS, sizeof(g_cycles[0]), cmp_u32);
#define PCT(p) ((unsigned long)g_cycles[((long)(M7_BENCH_BLOCKS - 1) * (p)) / 1000])
    bench_print("cycles/block: min %lu  p50 %lu  p99 %lu  max %lu", PCT(0), PCT(500), PCT(990), PCT(1000));
#undef PCT
    /* Integer arithmetic only, so newlib-nano printf is enough. */
    const unsigned long per_sample = (unsigned long)(total / ((uint64_t)M7_BENCH_BLOCKS * M7_BENCH_BLOCK));
    const unsigned long budget = cpu_hz / 48000u;
    const unsigned long load_x10 = budget ? (unsigned long)((uint64_t)per_sample * 1000u / budget) : 0;
    bench_print("mean %lu cycles/sample = %lu.%lu%% of a 48 kHz real-time budget at %lu MHz", per_sample,
                load_x10 / 10, load_x10 % 10, (unsigned long)(cpu_hz / 1000000u));
#if !defined(M7_BENCH_NO_CHECK) && !defined(M7_BENCH_DUMP)
    /* Errors in units of 1e-9, so printf needs no float support. */
    const float limit = 1e-4f * peak;
    const int ok = nonfinite == 0 && n_checked == n_expected && max_err <= limit;
    bench_print("output: %d samples checked, max abs err %lue-9 (limit %lue-9): %s", n_checked,
                (unsigned long)(max_err * 1e9f), (unsigned long)(limit * 1e9f), ok ? "OK" : "FAIL");
#else
#if defined(M7_BENCH_DUMP)
    bench_print("peak %08lx", (unsigned long)peak_bits);
#endif
    bench_print("output: not checked, %d nonfinite samples", nonfinite);
#endif
}
