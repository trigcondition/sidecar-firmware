/* Linux/AArch64 benchmark for the NAM A2 engines (either model: the .namb
 * file says which; see example/a2_engine.c).
 *
 *   ./a76_bench capture.namb [block]          (default block 48)
 *
 * Processes 10 s of a test signal 7 times and reports the best run:
 * CPU cycles and instructions per sample from perf_event_open (user space
 * only; needs /proc/sys/kernel/perf_event_paranoid <= 2, no perf tool), plus
 * wall-clock time per sample and the share of one core a 48 kHz stream uses.
 * When the counters are unavailable it reports wall-clock time only.
 *
 * Pin to the core you want to measure: `taskset -c 4 ./a76_bench` (on RK3588
 * cores 4-7 are the A76s). See run.sh. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "a2_engine.h"
#include "perfc.h"

#define SECONDS 10

static float sig(long i)
{
    return 0.5f * sinf(i * 0.0131f) * (0.6f + 0.4f * sinf(i * 0.00007f)) + 0.1f * sinf(i * 0.37f);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int load_capture(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc((size_t)size);
    int ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    const char* err = "read error";
    int rc = ok ? a2_load(data, (uint32_t)size, 1, &err) : -1; /* loads and prewarms */
    if (rc != 0)
        fprintf(stderr, "%s: %s\n", path, err);
    free(data);
    return rc;
}

static float sigbuf[48000L * SECONDS];

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s capture.namb [block]\n", argv[0]);
        return 2;
    }
    const int bs = argc > 2 ? atoi(argv[2]) : 48;
    if (bs < 1 || bs > A2_MAX_FRAMES) {
        fprintf(stderr, "block must be 1..%d\n", A2_MAX_FRAMES);
        return 1;
    }
    const long n = 48000L * SECONDS / bs * bs;
    float in[A2_MAX_FRAMES] = {0}, out[A2_MAX_FRAMES];
    for (long i = 0; i < n; i++)
        sigbuf[i] = sig(i);

    if (load_capture(argv[1]) != 0)
        return 1;

    perfc_t pc = perfc_init();
    const int have_pmu = pc.cyc >= 0;
    uint64_t best_c = ~0ull, best_i = 0;
    double best_t = 1e30, peak = 0.0;
    for (int it = 0; it < 7; it++) {
        uint64_t c = 0, ins = 0;
        double t0 = now_s();
        if (have_pmu)
            perfc_start(pc);
        for (long s = 0; s < n; s += bs) {
            for (int i = 0; i < bs; i++)
                in[i] = sigbuf[s + i];
            a2_process(in, out, bs);
            if (it == 0)
                for (int i = 0; i < bs; i++)
                    if (fabsf(out[i]) > peak)
                        peak = fabsf(out[i]);
        }
        if (have_pmu)
            perfc_stop(pc, &c, &ins);
        double t = now_s() - t0;
        if (t < best_t)
            best_t = t;
        if (have_pmu && c < best_c) {
            best_c = c;
            best_i = ins;
        }
    }

    printf("%s, block %d, %d s of audio, best of 7 (output peak %.3f)\n", a2_model_name(a2_model()), bs,
           SECONDS, peak);
    if (have_pmu)
        printf("  %.0f cycles/sample  %.0f instr/sample  IPC %.2f\n", (double)best_c / n,
               (double)best_i / n, (double)best_i / best_c);
    else
        printf("  cycle counters unavailable (perf_event_paranoid > 2?): wall clock only\n");
    printf("  %.1f ns/sample  %.2f%% of one core for a 48 kHz stream  (%.0fx real time)\n",
           best_t / n * 1e9, 100.0 * best_t / SECONDS, SECONDS / best_t);
    return 0;
}
