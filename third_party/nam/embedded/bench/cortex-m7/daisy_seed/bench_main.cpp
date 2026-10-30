// Daisy Seed (STM32H750, Cortex-M7 @ 480 MHz) port of m7_bench.
//
// Needs only the Daisy and its USB cable. Open the USB serial port
// (e.g. `screen /dev/cu.usbmodem* 115200`) and type:
//
//   W  benchmark, warm caches       C  benchmark, caches flushed before every block
//   D  reboot into the Daisy bootloader (to flash again without buttons)
//
// The capture (.namb) is embedded in QSPI flash as a binary blob (the
// .incbin below). The firmware validates it and loads it once at boot, as a
// product would load a capture from its flash partition.
//
// Memory placement, as recommended in the README:
//   - engine code in ITCM (copied from QSPI flash at boot, itcm_section.lds)
//   - the engine instance and the capture (A2_HOT) in DTCM
//   - the ring buffers (nam_a2*_state_t) in AXI SRAM behind the D-cache

#include "daisy_seed.h"

#include <cstdarg>

extern "C"
{
#include "m7_bench.h"
}

using namespace daisy;

// The .namb file, verbatim, in read-only memory-mapped QSPI flash.
// BENCH_NAMB is its path, set by the Makefile.
__asm__(".section .rodata.bench_namb,\"a\",%progbits\n"
        ".balign 4\n"
        "bench_namb:\n"
        ".incbin \"" BENCH_NAMB "\"\n"
        "bench_namb_end:\n"
        ".previous\n");
extern "C" const uint8_t bench_namb[], bench_namb_end[];

static DaisySeed     hw;
static volatile char g_cmd = 0;

static void OnUsbReceive(uint8_t* buf, uint32_t* len)
{
    for(uint32_t i = 0; i < *len; i++)
        if(buf[i] == 'W' || buf[i] == 'C' || buf[i] == 'D')
            g_cmd = (char)buf[i];
}

extern "C" uint32_t bench_cycles(void) { return DWT->CYCCNT; }
extern "C" void     bench_irq_off(void) { __disable_irq(); }
extern "C" void     bench_irq_on(void) { __enable_irq(); }

extern "C" void bench_cache_flush(void)
{
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();
}

extern "C" void bench_print(const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    Logger<LOGGER_INTERNAL>::PrintLineV(fmt, va);
    va_end(va);
}

// Defined by the linker script the Makefile derives from libDaisy's QSPI script.
extern "C" uint32_t _sitcmram_text, _itcmram_text_start, _itcmram_text_end;

static void CopyCodeToItcm()
{
    const volatile uint32_t* src = &_sitcmram_text;
    volatile uint32_t*       dst = &_itcmram_text_start;
    while(dst < &_itcmram_text_end)
        *dst++ = *src++;
    __DSB();
    __ISB();
}

int main(void)
{
    CopyCodeToItcm();
    hw.Init(true); // 480 MHz

    // Flush-to-zero + default NaN. FPDSCR (0xE000EF3C) is the default FPSCR
    // for new FP contexts, i.e. interrupt handlers such as the audio callback.
    uint32_t fpscr = __get_FPSCR();
    __set_FPSCR(fpscr | (1U << 24) | (1U << 25));
    *reinterpret_cast<volatile uint32_t*>(0xE000EF3C) |= (1U << 24) | (1U << 25);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    hw.StartLog(false);
    hw.usb_handle.SetReceiveCallback(OnUsbReceive, UsbHandle::FS_INTERNAL);

    // Print happens later too (W/C), since nobody may be listening yet.
    const uint32_t namb_size = (uint32_t)(bench_namb_end - bench_namb);
    const bool     loaded    = m7_bench_load(bench_namb, namb_size) == 0;

    uint32_t last_blink = System::GetNow();
    bool     led        = false;
    for(;;)
    {
        char c = g_cmd;
        if(c)
        {
            g_cmd = 0;
            hw.SetLed(true);
            if((c == 'W' || c == 'C') && !loaded)
                m7_bench_load(bench_namb, namb_size); // prints why it failed
            else if(c == 'W' || c == 'C')
                m7_bench_run(System::GetSysClkFreq(), c == 'C');
            else
            {
                bench_print("rebooting into the Daisy bootloader");
                System::Delay(100);
                System::ResetToBootloader(System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
            }
        }
        if(System::GetNow() - last_blink > 500) // slow blink: waiting for a command
        {
            last_blink = System::GetNow();
            led        = !led;
            hw.SetLed(led);
        }
    }
}
