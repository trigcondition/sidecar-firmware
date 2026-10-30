# NAM A2 engines for Cortex-M7 and Cortex-A76

Dependency-free C engines for NAM **A2-Lite** (3 channels) and **A2-Full**
(8 channels) WaveNet captures, generated per target:

- **Cortex-M7**: register-tiled kernels in generated Thumb-2/FPv5 assembly.
- **Cortex-A76**: NEON kernels with generated AArch64 tap loops.

Each engine is one `.c` + `.h` pair with its own prefix (`nam_a2lite_*`,
`nam_a2full_*`), so a product can link both models. The engines need only
`<math.h>`, `<string.h>` and `<stdint.h>`: no C++, no Eigen, no JSON parser
and no allocation. They keep no state of their own: you own the memory, so
you choose where each part goes and can run as many instances as you like.

**No weights are compiled into the engines.** Each engine runs any capture
of its model, loaded at run time from a `.namb` file (the compact binary NAM
format); a small C99 reader is included. See [Weights](#weights).

Outputs match NeuralAmpModelerCore to an ESR of about −134 dB; see
[Accuracy](#accuracy). Upgrading from the 1.x engines (the tarball
releases)? See [MIGRATION.md](MIGRATION.md); [CHANGELOG.md](CHANGELOG.md) lists
the changes per release.

| Target | A2-Lite | A2-Full | Compared with |
|---|---:|---:|---|
| Cortex-A76 | **447** cycles/sample (2.9x faster) | **1,981** cycles/sample (3.2x faster) | NeuralAmpModelerCore A2 fast path: 1,293 / 6,308 |
| Cortex-M7 | **4,059** cycles/sample (41% load) | **18,972** cycles/sample | 1.x engines, same board: 4,076 / 19,262 |

Block size 48, measured on hardware: Cortex-A76 on an Orange Pi 5 (RK3588S),
Cortex-M7 on a Daisy Seed (STM32H750, 480 MHz), each with this repository's
benchmark. See [Performance](#performance) for 16-frame blocks and details.

## Contents

```
engines/
  cortex-m7/a2lite/      nam_a2lite.{c,h}   default
  cortex-m7/a2lite-b16/  nam_a2lite.{c,h}   tuned for 16-frame blocks
  cortex-m7/a2full/      nam_a2full.{c,h}   default
  cortex-a76/a2lite/     nam_a2lite.{c,h}   default
  cortex-a76/a2full/     nam_a2full.{c,h}   default
example/
  a2_engine.{c,h}     one engine slot for either model, chosen from the .namb file
  host_example.c      load a .namb, selftest against the reference, render a file
  m7_integration.c    Cortex-M7 audio-callback integration sketch
  Makefile
namb/
  namb_reader.{c,h}   .namb reader: validate, check shape, zero-copy weight pointer
weights/
  a2lite.namb         example A2-Lite capture (7,980 bytes)
  a2full.namb         example A2-Full capture (49,080 bytes)
bench/
  cortex-m7/          board-agnostic DWT benchmark + Daisy Seed (STM32H750) port
  cortex-a76/         Linux benchmark (perf_event cycle counters) + run script
test/
  golden_input.raw            2 s of test input, mono float32, 48 kHz
  golden_a2lite_output.raw    NeuralAmpModelerCore output for that input,
                              with the capture in weights/
  golden_a2full_output.raw
test/m7emu/           runs the Cortex-M7 assembly under emulation against the references
VERSION, MANIFEST.json   release version, code generator version and settings
```

## Quick start

On any host with a C compiler:

```sh
cd example
make check-all
```

This builds both engines of each target for the host and runs both captures
in `weights/` against the NeuralAmpModelerCore references: at block sizes 48,
32, 16, 1 and the maximum, with block sizes that change on every call, with
the default `NAM_*_MAX_BUFFER_SIZE` and with 16, and on the A76 engines with
three block hints. Every line should say `ok`:

```
== cortex-m7
../weights/a2lite.namb: A2-Lite capture
  block 48         max abs err 2.38e-07  ESR  -135.1 dB  ok
  ...
selftest: 96000 frames vs NeuralAmpModelerCore reference: PASS
```

On an AArch64 host (including Apple Silicon), the A76 engines run their NEON
assembly; on other hosts they build a portable scalar fallback. On a host,
the M7 engines run C tiles with the same accumulation order as the M7
assembly. Every combination passes.

To process your own audio (headerless mono float32 at 48 kHz) with any
capture, of either model:

```sh
make                                  # A76 engines; TARGET=cortex-m7 for the M7 ones
./host_example render ../weights/a2lite.namb my_di.raw my_out.raw
./host_example render other_capture.namb my_di.raw my_out.raw   # no rebuild
```

## API

Shown for A2-Lite; A2-Full is the same with `nam_a2full_` / `NAM_A2FULL_`.

```c
#include "nam_a2lite.h"

/* You own all of the engine's memory: three structs of fixed size. */
static nam_a2lite_weights_t weights;  /* a capture; shared by any number of instances */
static nam_a2lite_t         inst;     /* one per running engine: small, hot */
static nam_a2lite_state_t   state;    /* one per running engine: the ring buffers */

nam_a2lite_load_weights(&weights, w, n);  /* the capture; see Weights. 0 = loaded,
                                             -1 = wrong weight count, nothing written */
nam_a2lite_init(&inst, &weights, &state); /* bind and clear */
nam_a2lite_prewarm(&inst);                /* NAM_A2LITE_PREWARM_SAMPLES of silence */

const float* in[1]  = { input };
float*       out[1] = { output };
nam_a2lite_process(&inst, in, out, n);    /* any n in 1..NAM_A2LITE_MAX_BUFFER_SIZE */

nam_a2lite_reset(&inst);                  /* clear the state, e.g. after a mute or bypass; */
nam_a2lite_prewarm(&inst);                /* prewarm again afterwards */
nam_a2lite_set_weights(&inst, &other);    /* switch captures, then reset + prewarm */
```

| Struct | Size, A2-Lite / A2-Full | What |
|---|---|---|
| `nam_a2*_weights_t` | 7.3 KB / 47.5 KB | the capture, packed for the kernels; read-only while processing |
| `nam_a2*_t` | M7: 1.9 KB / 3.1 KB, A76: 1.0 KB / 2.2 KB, at max block 48 | head buffer, scratch, per-layer call descriptors |
| `nam_a2*_state_t` | 100 KB / 267 KB at max block 48 | ring buffers |

| Constant | Value |
|---|---|
| `NAM_A2*_MAX_BUFFER_SIZE` | 64 by default. The largest `n` that `process` accepts. Override it with `-D` (see below). |
| `NAM_A2*_PREWARM_SAMPLES` | 6347, the A2 receptive field plus one (NeuralAmpModelerCore's prewarm) |
| `NAM_A2*_IN_CHANNELS`, `NAM_A2*_OUT_CHANNELS` | 1, 1 |
| `NAM_A2*_SAMPLE_RATE` | 48000 |

Notes on the API:

- **Memory.** The structs are complete types: declare them static, place them
  in a section, or put them in a union with another engine's. Never on the
  stack (the state is large). See [Cortex-M7 integration](#cortex-m7-integration).
- **Instances.** Each running engine needs its own `nam_a2*_t` and
  `nam_a2*_state_t`. Instances can share one `weights` struct, of either
  model, and do not interact: a stereo pair, a dual amp, or A2-Lite and
  A2-Full side by side all work.
- **Prewarm.** After `init` or `reset`, `prewarm` runs
  `NAM_A2*_PREWARM_SAMPLES` of silence through the instance so the first real
  block has no start-up transient, as NeuralAmpModelerCore does. It is not
  real-time: about 54 ms (A2-Lite) or 254 ms (A2-Full) of processing on a
  480 MHz M7, under 2 ms on an A76. It is separate from `init` because the capture must be loaded
  first.
- **Block size.** `n` can change from call to call. Nothing checks it at run
  time: if `n` is larger than `NAM_A2*_MAX_BUFFER_SIZE`, the engine writes
  past its buffers.
- **`NAM_A2*_MAX_BUFFER_SIZE`.** Set it to your largest callback size, e.g.
  `-DNAM_A2LITE_MAX_BUFFER_SIZE=48`; it sizes the structs. Use the same value
  in every file that includes the engine's header.
- **Threading.** `load_weights` (into weights an instance is using), `init`,
  `set_weights`, `reset` and `prewarm` must not run concurrently with
  `process` on the same instance. `process` is real-time safe: it does no
  allocation, takes no locks and makes no system calls. Different instances
  can run on different threads.
- **Both models in one product.** `example/a2_engine.{c,h}` is a ready-made
  slot that loads either model from its `.namb` file and overlays the two
  engines' memory, so it costs as much as A2-Full alone.

### Block size

Two settings depend on your callback size. Neither affects the output; both
affect speed or memory.

- **`NAM_A2*_MAX_BUFFER_SIZE`** (compile time): the largest callback, as
  above. Smaller values save state memory.
- **Tile shapes**, chosen for your usual callback size:
  - **Cortex-A76:** one engine per model, with the tile shapes for every
    block size inside it. Set `-DNAM_A2LITE_BLOCK_HINT=n` /
    `-DNAM_A2FULL_BLOCK_HINT=n` (default 48) when compiling the engine's `.c`
    file; only the matching kernels are compiled.
  - **Cortex-M7:** one build per tuning, so code size stays minimal. The
    default engines are tuned for 48-frame blocks. Where a different tuning
    is faster for 16-frame blocks, it is shipped as `<model>-b16/` (same API,
    weights and output); where it would be the same code, it is not shipped.
    For 16-frame callbacks, build with the `-b16` engine if there is one and
    `-DNAM_A2*_MAX_BUFFER_SIZE=16`.

Time spent outside the engine also grows when the callback gets smaller: the
audio interrupt and any per-callback work in your code run 3x as often at 16
as at 48. To tell the two apart, time `process` alone with the cycle
counter, as `bench/cortex-m7` does.

### What is fixed at generation time

| | |
|---|---|
| Architecture | A2-Lite (3 ch) or A2-Full (8 ch): 23 layers, kernel sizes 6 and 15, dilations 1 to 239, LeakyReLU (slope 0.01), head kernel 16 |
| Weights | **Not fixed**: loaded at run time. A2-Lite takes 1,871 floats; A2-Full takes 12,146. |
| Precision | float32 |
| Sample rate | 48 kHz |

## Weights

Captures ship as **`.namb`** files: the compact binary NAM format, specified
and tooled in
[tone-3000/nam-binary-loader](https://github.com/tone-3000/nam-binary-loader).
A `.namb` holds the model config and the weights in one CRC-protected file,
about 81% smaller than the `.nam` JSON: 7,980 bytes for A2-Lite and 49,080
bytes for A2-Full. Firmware can check that the file has the right shape before
it loads anything.

`namb/namb_reader.{c,h}` reads it. It is C99, has no dependencies beyond
`<stdint.h>`, `<stddef.h>` and `<string.h>`, does no allocation, and is about
0.8 KB of Cortex-M7 code.

```c
namb_model_t m;
namb_status_t s = namb_parse(data, size, &m);       /* container + header  */
if (s == NAMB_OK) s = namb_verify_crc(data, size);  /* corrupt flash?      */
if (s == NAMB_OK) s = namb_check_a2full(&m);        /* or namb_check_a2lite */
if (s != NAMB_OK) { log(namb_status_str(s)); return; }

nam_a2full_load_weights(&weights, m.weights, (int)m.weight_count);
```

- **Zero-copy parse.** `m.weights` points into your buffer, so a `.namb` in
  memory-mapped flash needs no RAM buffer. The buffer must be 4-byte aligned:
  otherwise `namb_parse` returns `NAMB_ERR_ALIGN`.
- **Copy into the engine.** `load_weights` copies the weights into your
  `weights` struct, repacking them into the kernels' layout. The source
  buffer can be freed or overwritten afterwards.
- **Which model?** `namb_check_a2lite` / `namb_check_a2full` tell the two
  apart; `example/a2_engine.c` uses them to pick the engine.
- **Cost.** `namb_parse` and the shape checks are fixed-cost.
  `namb_verify_crc` reads the whole file: run it at boot or after a flash
  write, not on every capture switch.
- **Errors.** A wrong-shape file is rejected before anything is written, and
  `load_weights` rejects a wrong weight count before writing. A failed load
  therefore leaves the previous capture intact.

**Switching captures while running.** Stop calling `process` (mute), load
the new capture, `reset`, `prewarm`, and unmute. `switch_capture()` in
`example/m7_integration.c` shows the sequence. To switch without a gap,
load into a second `weights` struct and second instance while the first
keeps running, then swap.

**Making a `.namb`.** Convert any A2-Lite or A2-Full `.nam` with the
`nam2namb` tool from
[tone-3000/nam-binary-loader](https://github.com/tone-3000/nam-binary-loader):

```sh
nam2namb my_capture.nam my_capture.namb
```

## Cortex-M7 integration

See `example/m7_integration.c`. It loads a capture of either model from
memory-mapped flash at boot, runs it from an audio callback, and switches
captures. It compiles as-is with `arm-none-eabi-gcc`.

**Compiler.** Use GCC; we tested Arm GNU Toolchain 10.3. Recommended flags:

```
-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O2 -ffast-math
```

The assembly kernels are selected automatically when the target is ARMv7E-M
with FMA and Thumb-2. They use `VMAXNM`, which Cortex-M4F does not have. To
build for M4F, or for any core without FPv5, add `-DNAM_M7_ASM=0`: this
selects the C tiles, which give the same output but run slower. `fpv5-sp-d16`
also works, because the engine uses single precision only.

**Memory placement.** This has the largest effect on M7 performance after the
kernels themselves. You place each struct; the engines need no linker
attributes of their own.

| What | Size, A2-Lite / A2-Full | Where it should go |
|---|---|---|
| `nam_a2*_weights_t` + `nam_a2*_t` (capture, head buffer, scratch, descriptors) | 9.2 KB / 50.6 KB at max block 48 | **DTCM**, e.g. `static nam_a2lite_t inst __attribute__((section(".dtcmram_bss")));` (`A2_HOT` in `a2_engine.c`) |
| `nam_a2*_state_t` (ring buffers) | 100 KB / 267 KB at max block 48 | AXI SRAM with D-cache on, or DTCM if it fits (`A2_STATE`) |
| Engine code (no constant data) | 12.4 KB / 22.2 KB, of which the weight loader is 4.3 KB / 5.4 KB | **ITCM** if you can; otherwise flash with I-cache on. `bench/cortex-m7/daisy_seed/itcm_section.lds` shows how. The loader runs only at load time and can stay in flash |
| The `.namb` capture | 7.8 KB / 47.9 KB | Anywhere readable: flash, SD card, a RAM buffer. Read only during `load_weights` |

At the default max block of 64 the state is 109 KB / 290 KB. A product with
both models and `a2_engine.c` needs the A2-Full sizes once, not the sum.

**FPU set-up.** Enable **flush-to-zero and default-NaN** in both FPSCR and
FPDSCR before audio starts. `m7_integration.c` shows how. Denormals are slow
on the M7 FPU, and decaying signals produce them. Also enable the I-cache and
D-cache.

## Cortex-A76 integration

The engines are plain C99 with GCC/Clang inline assembly. We measured them on
Linux:

```sh
gcc -O3 -ffast-math -mcpu=cortex-a76 -DNAM_A2FULL_BLOCK_HINT=48 \
    -DNAM_A2FULL_MAX_BUFFER_SIZE=48 -c nam_a2full.c namb_reader.c
```

- The scheduled assembly tap loops are on by default on AArch64.
  `-DNAM_A64_ASM=0` selects the NEON intrinsics path, which gives bit-identical
  output and runs somewhat slower.
- Set the block hint (see [Block size](#block-size)) to your usual callback
  size; any value works.
- The engines run on any ARMv8-A core, but their tile shapes and instruction
  schedule are tuned for the A76. If you also ship on in-order cores such as
  the Cortex-A55, ask us for the A55-tuned build.
- Call `process` from your audio thread. If you can, pin that thread to an
  A76 core: on RK3588, cores 4 to 7 are the A76 cores.

## Performance

All figures are cycles per sample, measured with this repository's
benchmarks (`bench/`), best of several runs. The 1.x engines were measured
the same way on the same boards.

### Cortex-M7 (Daisy Seed)

STM32H750 at 480 MHz, GCC 10.3, `-O2 -ffast-math`, engine code in ITCM,
instance and capture in DTCM, ring buffers in AXI SRAM (the `daisy_seed`
port). At 48 kHz the real-time budget is 10,000 cycles/sample. Warm: caches
as an audio loop leaves them; cold: caches flushed before every block.

| Model, block | 1.x, warm / cold | **2.0**, warm / cold | Real-time load (warm) |
|---|---:|---:|---:|
| A2-Lite, 48 | 4,076 / 4,113 | **4,059 / 4,109** | 41% |
| A2-Lite, 16 (`a2lite-b16`) | 4,437 / 4,645 | **4,403 / 4,634** | 44% |
| A2-Full, 48 | 19,262 / 19,303 | **18,972 / 19,014** | 190% |
| A2-Full, 16 | 19,754 / 19,870 | **19,399 / 19,521** | 194% |

The benchmark also reports per-block p50, p99 and max; for A2-Lite, the p99
block is 2.4% above the mean at block 48 and 7.6% at block 16. A2-Full is
FMA-bound: it
needs 11,768 multiply-accumulates per sample, and the M7 FPU issues one per
cycle, so it does not run in real time on a single 480 MHz Cortex-M7.

### Cortex-A76 (Orange Pi 5)

RK3588S, Cortex-A76 at 2.3 GHz, Linux, GCC 13, `-O3 -ffast-math
-mcpu=cortex-a76`, pinned to an A76 core, block hint and max block equal to
the block size. User-space cycles from `perf_event_open`, best of 7 runs over
10 s of audio.

| Model, block | NeuralAmpModelerCore A2 fast path | 1.x | **2.0** | Share of one A76 core at 48 kHz |
|---|---:|---:|---:|---:|
| A2-Lite, 48 | 1,293 | 441 | **447** | 0.9% |
| A2-Lite, 16 | | 512 | **519** | 1.1% |
| A2-Full, 48 | 6,308 | 2,019 | **1,981** | 4.1% |
| A2-Full, 16 | | 2,155 | **2,157** | 4.5% |

Run-to-run variation on this board is about ±1%. The A2-Lite engine is about
1% slower than in 1.x: the 1.x engines had their
buffers at fixed addresses, which GCC specialized the kernels on.

### Running the benchmarks

**Cortex-A76.** Run this on the board, with `perf_event_paranoid` at 2 or
less:

```sh
cd bench/cortex-a76
./run.sh                 # both models, pinned to core 4, block 48
BLOCK=16 ./run.sh        # block 16 (also builds the engines for it)
CORE=0 BLOCK=128 ./run.sh
```

Without the cycle counters, the script reports wall-clock time only.

**Cortex-M7.** `bench/cortex-m7/m7_bench.c` is board-agnostic. To port it to a
board, implement five hooks: `DWT->CYCCNT`, interrupts off and on, cache flush
and print. Then call `m7_bench_load(namb, size)` once with the capture, of
either model, and `m7_bench_run(cpu_hz, cold)` for each run.

- The benchmark times 1 s of audio (1000 blocks of 48, or 3000 of 16) and
  reports the min, p50, p99 and max cycles per block, plus the mean cycles
  per sample and the real-time load.
- It also compares the output with expected values for the captures in
  `weights/` (`expected.h`), and prints OK or FAIL.
- `cold` flushes the caches before every block. It gives an upper bound for a
  callback whose caches were evicted by other work.

`daisy_seed/` is a complete port for the Electrosmith Daisy Seed (STM32H750
at 480 MHz) using libDaisy. It embeds `weights/<model>.namb` in QSPI flash
unchanged, and validates and loads it at boot (`NAMB=` selects another
capture). It needs only the USB cable:

```sh
cd bench/cortex-m7/daisy_seed
make MODEL=a2lite LIBDAISY_DIR=/path/to/libDaisy GCC_PATH=/path/to/arm/bin
make MODEL=a2lite BLOCK=16 ...    # instead: 16-frame blocks
make program-dfu        # with the Daisy bootloader waiting for DFU
screen /dev/cu.usbmodem* 115200   # type W (warm) or C (cold)
```

To try the benchmark and its output check on a host first: `cd bench/cortex-m7
&& make && ./host_m7_bench ../../weights/a2full.namb`. It times nothing
meaningful.

## Accuracy

The reference is NeuralAmpModelerCore's generic WaveNet, running the same
captures as `weights/` (from their `.nam` files), fed with
`test/golden_input.raw` at block 48 and prewarmed the same way. The engines
load the captures from `.namb`.

| Engine | Max abs error | ESR |
|---|---:|---:|
| A2-Lite (M7 and A76) | 2.4e-7 | −135.1 dB |
| A2-Full (M7 and A76) | 4.2e-7 | −133.7 dB |

The output peak is about 0.6, so these errors are at float32 rounding level.

Other checks:

- The Cortex-M7 assembly is checked against the same references under
  emulation: `make -C test/m7emu check` builds the engines for the M7 and runs
  them in Unicorn (an emulated Cortex-M7 with its FPU) over the whole test
  input, for every M7 tuning, at block sizes 48, 16 and varying. It gives the
  errors above. It needs `arm-none-eabi-gcc` with newlib and
  `pip install unicorn pyelftools`. On a board, `bench/cortex-m7` also checks
  its output.
- On AArch64, the assembly tap loops are bit-identical to the intrinsics path.
- Results are the same for every block size, block hint and
  `NAM_A2*_MAX_BUFFER_SIZE` we tried, including block sizes that change on
  every call, and for several instances of one or both models running
  interleaved.

Continuous integration (`.github/workflows/ci.yml`) runs `make check-all` on
x86-64 Linux and on AArch64 (macOS, NEON code), the Cortex-M7 emulation test,
and `-Werror` builds for the Cortex-M7 and Cortex-M4F, for every release tag.

## Version

This is release 2.0.0 (see `VERSION` and [CHANGELOG.md](CHANGELOG.md)).
The engines are generated by TONE3000's nam2c code generator; `MANIFEST.json`
records the generator version and settings used. Releases are tagged
(`v2.0.0`, ...); pin one in your build.

Questions and bug reports: open an issue in this repository, or, if you
prefer to keep it private, contact us through your usual channel.

## License

MIT; see [LICENSE](LICENSE). Third-party material is listed in
[NOTICE](NOTICE).
