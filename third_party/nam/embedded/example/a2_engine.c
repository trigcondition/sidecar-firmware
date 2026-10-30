/* a2_engine: see a2_engine.h. */
#include "a2_engine.h"

#include <string.h>

#include "namb_reader.h"

#ifndef A2_HOT
#define A2_HOT
#endif
#ifndef A2_STATE
#define A2_STATE
#endif

/* Each union is as large as its A2-Full member; the two models share it. */
static union {
    nam_a2lite_t lite;
    nam_a2full_t full;
} g_inst A2_HOT;
static union {
    nam_a2lite_weights_t lite;
    nam_a2full_weights_t full;
} g_weights A2_HOT;
static union {
    nam_a2lite_state_t lite;
    nam_a2full_state_t full;
} g_state A2_STATE;
static a2_model_t g_model = A2_NONE;

int a2_load(const void* namb, uint32_t size, int verify_crc, const char** err)
{
    namb_model_t m;
    namb_status_t s = namb_parse(namb, size, &m);
    if (s == NAMB_OK && verify_crc)
        s = namb_verify_crc(namb, size); /* reads the whole file: at boot or after a write */
    a2_model_t model = A2_NONE;
    if (s == NAMB_OK) {
        if (namb_check_a2lite(&m) == NAMB_OK)
            model = A2_LITE;
        else if ((s = namb_check_a2full(&m)) == NAMB_OK)
            model = A2_FULL;
    }
    if (model == A2_NONE) {
        if (err)
            *err = namb_status_str(s);
        return -1;
    }
    /* Every check has passed, so the load below cannot fail: nothing was
     * written before this point. */
    g_model = A2_NONE;
    if (model == A2_LITE) {
        nam_a2lite_load_weights(&g_weights.lite, m.weights, (int)m.weight_count);
        nam_a2lite_init(&g_inst.lite, &g_weights.lite, &g_state.lite);
    } else {
        nam_a2full_load_weights(&g_weights.full, m.weights, (int)m.weight_count);
        nam_a2full_init(&g_inst.full, &g_weights.full, &g_state.full);
    }
    g_model = model;
    a2_reset();
    return 0;
}

void a2_reset(void)
{
    if (g_model == A2_LITE) {
        nam_a2lite_reset(&g_inst.lite);
        nam_a2lite_prewarm(&g_inst.lite);
    } else if (g_model == A2_FULL) {
        nam_a2full_reset(&g_inst.full);
        nam_a2full_prewarm(&g_inst.full);
    }
}

void a2_process(const float* in, float* out, int n)
{
    const float* ip[1] = {in};
    float* op[1] = {out};
    if (g_model == A2_LITE)
        nam_a2lite_process(&g_inst.lite, ip, op, n);
    else if (g_model == A2_FULL)
        nam_a2full_process(&g_inst.full, ip, op, n);
    else
        memset(out, 0, (size_t)n * sizeof(float));
}

a2_model_t a2_model(void) { return g_model; }

const char* a2_model_name(a2_model_t m)
{
    return m == A2_LITE ? "A2-Lite" : m == A2_FULL ? "A2-Full" : "none";
}
