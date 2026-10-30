# Migrating from 1.x

1.x is the engine package shipped as a tarball (`nam2c_engines_m7_a76`,
`nam_model.{c,h}` per engine). The engines' processing is the same in 2.0,
and the outputs are identical. What
changed is who owns the memory: the engines no longer have file-scope
statics, so you can run several instances and place every buffer yourself.

## Names

Each engine now has its own prefix, so both models can be linked into one
program.

| Previous | Now (A2-Lite; A2-Full is `nam_a2full_` / `NAM_A2FULL_`) |
|---|---|
| `nam_model.c`, `nam_model.h` | `nam_a2lite.c`, `nam_a2lite.h` |
| `nam_state_t` | `nam_a2lite_state_t` (ring buffers), plus `nam_a2lite_t` and `nam_a2lite_weights_t` |
| `NAM_MAX_BUFFER_SIZE` | `NAM_A2LITE_MAX_BUFFER_SIZE` |
| `NAM_IN_CHANNELS`, ... | `NAM_A2LITE_IN_CHANNELS`, ... |
| `NAM_DTCM` | gone: you place the structs (see below) |

## Calls

```c
/* Previous */
static nam_state_t state;
nam_init(&state);
nam_load_weights(w, n);
/* prewarm: your own loop of 6347 samples of silence */
nam_process(&state, in, out, n);
nam_reset(&state);

/* Now */
static nam_a2lite_weights_t weights;
static nam_a2lite_t         inst;
static nam_a2lite_state_t   state;
nam_a2lite_load_weights(&weights, w, n);
nam_a2lite_init(&inst, &weights, &state);
nam_a2lite_prewarm(&inst);                 /* generated; NAM_A2LITE_PREWARM_SAMPLES */
nam_a2lite_process(&inst, in, out, n);
nam_a2lite_reset(&inst);
nam_a2lite_prewarm(&inst);
```

`nam_a2lite_set_weights(&inst, &other_weights)` switches an instance to
another capture (then reset and prewarm). One `weights` struct can serve any
number of instances.

## Memory placement (Cortex-M7)

Previously the weights, head buffers and scratch were statics in the section
named by `-DNAM_DTCM=...`, and you placed `nam_state_t`. Now you place all
three structs:

```c
#define DTCM __attribute__((section(".dtcmram_bss")))
static nam_a2lite_weights_t weights DTCM;  /* was the NAM_DTCM weights */
static nam_a2lite_t         inst    DTCM;  /* was the NAM_DTCM head buffers and scratch */
static nam_a2lite_state_t   state;         /* AXI SRAM, as before */
```

The total sizes are about the same as before (see the README's placement
table). Products with both models can overlay the two engines' structs in
unions, as `example/a2_engine.c` does, and need the A2-Full sizes only once.

## Block size

- **Cortex-A76:** there is no `-b16` engine any more. One engine per model
  holds the tile shapes for every block size; select them with
  `-DNAM_A2LITE_BLOCK_HINT=16` (default 48) when compiling the engine.
- **Cortex-M7:** unchanged, except that a `-b16` directory ships only where
  it differs from the default engine: `cortex-m7/a2lite-b16/`. For A2-Full
  the 16-frame tuning is the same code as the default engine, so use
  `cortex-m7/a2full/` for any block size.

## Performance

On the Daisy Seed (Cortex-M7) the 2.0 engines are 0.4-1.8% faster than the
1.x ones. On the Cortex-A76, A2-Full is unchanged within measurement
noise and A2-Lite is about 1% slower. Details in the README.
