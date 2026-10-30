/* Host build of m7_bench.c, to try the benchmark and its output check before
 * moving to a board. Cycle numbers are host nanoseconds and mean nothing here.
 *
 *   make -C . host_m7_bench            (see the Makefile next to this file)
 *   ./host_m7_bench ../../weights/a2full.namb
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "m7_bench.h"

uint32_t bench_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec);
}

void bench_irq_off(void) {}
void bench_irq_on(void) {}
void bench_cache_flush(void) {}

void bench_print(const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    putchar('\n');
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s capture.namb\n", argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size)
        return 1;
    fclose(f);
    if (m7_bench_load(data, (uint32_t)size) != 0)
        return 1;
    free(data); /* the engine keeps its own copy of the weights */
    m7_bench_run(480000000u, 0);
    return 0;
}
