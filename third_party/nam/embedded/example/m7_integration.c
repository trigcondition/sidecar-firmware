/*
 * Cortex-M7 integration sketch: the engine inside an audio callback.
 *
 * Board-agnostic. Replace the "your HAL" declarations with your codec/DMA
 * driver and your flash layout. Compile both engines, example/a2_engine.c,
 * namb/namb_reader.c and this file with (GCC):
 *
 *   -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O2 -ffast-math
 *   -DNAM_A2LITE_MAX_BUFFER_SIZE=48 -DNAM_A2FULL_MAX_BUFFER_SIZE=48
 *                           (your largest callback size; for 16-frame
 *                            callbacks use 16, and the a2lite-b16 engine)
 *   '-DA2_HOT=__attribute__((section(".dtcmram_bss")))'
 *                           (instance + capture in DTCM, for a2_engine.c)
 *
 * Use the same NAM_*_MAX_BUFFER_SIZE in every file that includes an engine
 * header. This sketch supports both models through a2_engine.c; a product
 * with one model can call that engine directly instead (see its header and
 * a2_engine.c). See the README, "Cortex-M7 integration", for placement.
 */
#include <stdint.h>

#include "a2_engine.h"

/* ---- Your HAL (declarations so this file compiles) ----------------------- */
void audio_start(void (*cb)(const float* in, float* out, int n));
void audio_mute(int on); /* while muted, the callback must not run a2_process */
void log_error(const char* msg);
/* Where a capture lives: e.g. a partition of memory-mapped QSPI flash, written
 * at manufacture, over the air, or copied from an SD card. 4-byte aligned. */
extern const uint8_t capture_partition[];
extern const uint32_t capture_partition_size;
/* -------------------------------------------------------------------------- */

static void enable_ftz(void)
{
    /* Flush-to-zero + default NaN. Denormals are slow on the M7 FPU and occur
     * in decaying signals. FPSCR applies to this context; FPDSCR (0xE000EF3C)
     * is copied into FPSCR for each new exception context, i.e. the audio
     * interrupt. Set both. */
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
    *(volatile uint32_t*)0xE000EF3Cu |= (1u << 24) | (1u << 25);
}

/* Switch to another capture, of either model, while running. */
int switch_capture(const void* namb, uint32_t size)
{
    const char* err;
    audio_mute(1); /* a2_process must not run during the load */
    int rc = a2_load(namb, size, 1, &err); /* validates, loads, resets, prewarms */
    if (rc != 0)
        log_error(err); /* every check runs before anything is written, so the
                           previous capture is still loaded and prewarmed */
    audio_mute(0);
    return rc;
}

/* Audio callback: mono float in/out, n <= A2_MAX_FRAMES. Any n works,
 * including sizes that change between calls; there is no runtime check, so
 * a larger n overruns the ring buffers. */
static void audio_callback(const float* in, float* out, int n)
{
    a2_process(in, out, n);
}

int main(void)
{
    /* Enable I-cache and D-cache in your startup code (CMSIS:
     * SCB_EnableICache(); SCB_EnableDCache();). The engine relies on them for
     * code in flash and ring buffers in AXI SRAM. */
    enable_ftz();

    /* Once at boot: load the capture (this also prewarms it: ~50 ms for
     * A2-Lite, ~250 ms for A2-Full at 480 MHz). */
    const char* err;
    if (a2_load(capture_partition, capture_partition_size, 1, &err) != 0) {
        log_error(err);
        for (;;) {
        } /* no valid capture: do not start audio (or fall back to bypass) */
    }

    audio_start(audio_callback);
    for (;;) {
        /* To change captures: switch_capture(). To clear the state after a
         * bypass: mute, a2_reset(), unmute. None of these may run
         * concurrently with a2_process. */
    }
}
