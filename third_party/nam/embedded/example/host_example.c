/*
 * Host example for the NAM A2 engines.
 *
 * Builds against the two engines of one target (see Makefile) and runs
 * either model: the capture's .namb file says which.
 *
 *   ./host_example selftest ../weights/a2full.namb ../test/golden_input.raw \
 *                           ../test/golden_a2full_output.raw
 *   ./host_example render ../weights/a2lite.namb in.raw out.raw [block]
 *
 * Audio files are headerless mono float32, 48 kHz, native endianness.
 * The engine calls are in a2_engine.c.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2_engine.h"

static void* read_file(const char* path, long* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc((size_t)*size + 1); /* malloc'd memory is 4-byte aligned, as .namb needs */
    if (!data || fread(data, 1, (size_t)*size, f) != (size_t)*size) {
        fprintf(stderr, "%s: read error\n", path);
        exit(1);
    }
    fclose(f);
    return data;
}

static int load_capture(const char* path)
{
    long size;
    void* data = read_file(path, &size);
    const char* err = 0;
    int rc = a2_load(data, (uint32_t)size, 1, &err); /* loads, resets and prewarms */
    if (rc != 0)
        fprintf(stderr, "%s: %s\n", path, err);
    else
        printf("%s: %s capture\n", path, a2_model_name(a2_model()));
    free(data); /* the engine keeps its own copy of the weights */
    return rc;
}

/* Process x[0..n) in blocks. block > 0: fixed size; block == 0: sizes that
 * change on every call (1..A2_MAX_FRAMES), as some hosts do. */
static void run(const float* x, float* y, long n, int block)
{
    unsigned rng = 12345u;
    for (long s = 0; s < n;) {
        int k = block;
        if (k == 0) {
            rng = rng * 1103515245u + 12345u;
            k = 1 + (int)((rng >> 16) % A2_MAX_FRAMES);
        }
        if (k > n - s)
            k = (int)(n - s);
        a2_process(x + s, y + s, k);
        s += k;
    }
}

static int selftest(const char* in_path, const char* ref_path)
{
    long nb, nrb;
    float* x = read_file(in_path, &nb);
    float* ref = read_file(ref_path, &nrb);
    long n = nb / (long)sizeof(float);
    if (nrb != nb) {
        fprintf(stderr, "length mismatch: %ld vs %ld bytes\n", nb, nrb);
        return 1;
    }
    float* y = malloc((size_t)nb);
    double sig = 0.0, peak = 0.0;
    for (long i = 0; i < n; i++) {
        sig += (double)ref[i] * ref[i];
        if (fabs(ref[i]) > peak)
            peak = fabs(ref[i]);
    }

    const int blocks[] = {48, 32, 16, 1, A2_MAX_FRAMES, 0};
    int fail = 0;
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        if (blocks[b] > A2_MAX_FRAMES)
            continue; /* never pass more than A2_MAX_FRAMES frames */
        a2_reset(); /* reset + prewarm: start each run from the same state */
        run(x, y, n, blocks[b]);
        double err = 0.0, maxe = 0.0;
        for (long i = 0; i < n; i++) {
            double e = (double)y[i] - ref[i];
            err += e * e;
            if (fabs(e) > maxe)
                maxe = fabs(e);
        }
        double esr_db = 10.0 * log10(err / sig + 1e-300);
        int ok = maxe <= 1e-4 * peak && isfinite(maxe);
        fail |= !ok;
        char label[32];
        if (blocks[b])
            snprintf(label, sizeof(label), "block %d", blocks[b]);
        else
            snprintf(label, sizeof(label), "varying blocks");
        printf("  %-16s max abs err %.2e  ESR %7.1f dB  %s\n", label, maxe, esr_db, ok ? "ok" : "FAIL");
    }
    printf("selftest: %ld frames vs NeuralAmpModelerCore reference: %s\n", n, fail ? "FAIL" : "PASS");
    free(x);
    free(ref);
    free(y);
    return fail;
}

static int render(const char* in_path, const char* out_path, int block)
{
    long nb;
    float* x = read_file(in_path, &nb);
    long n = nb / (long)sizeof(float);
    float* y = malloc((size_t)nb + 1);
    run(x, y, n, block);
    FILE* f = fopen(out_path, "wb");
    if (!f) {
        perror(out_path);
        return 1;
    }
    fwrite(y, sizeof(float), (size_t)n, f);
    fclose(f);
    printf("rendered %ld frames to %s\n", n, out_path);
    free(x);
    free(y);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 5 && !strcmp(argv[1], "selftest")) {
        if (load_capture(argv[2]) != 0)
            return 1;
        return selftest(argv[3], argv[4]);
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "render")) {
        int block = argc == 6 ? atoi(argv[5]) : 48;
        if (block < 1 || block > A2_MAX_FRAMES) {
            fprintf(stderr, "block must be 1..%d\n", A2_MAX_FRAMES);
            return 1;
        }
        if (load_capture(argv[2]) != 0)
            return 1;
        return render(argv[3], argv[4], block);
    }
    fprintf(stderr,
            "usage: %s selftest capture.namb golden_input.raw golden_output.raw\n"
            "       %s render capture.namb in.raw out.raw [block]\n",
            argv[0], argv[0]);
    return 2;
}
