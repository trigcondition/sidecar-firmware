# Vendored: Beads and stmlib (eurorack)

The DSP of Beads, the texture synthesizer by Emilie Gillet, and the parts of stmlib it needs. MIT (every file's
header; `../../LICENSE.beads`, `stmlib/LICENSE`).

| What   | Repository                                   | Commit                                                   |
|--------|----------------------------------------------|----------------------------------------------------------|
| eurorack (Beads) | https://github.com/pichenettes/eurorack | `f4cfa7697095a28d292bff758beb51deb49b5a86` (2026-10-12; Beads' code added in `d8f4739`, the same day) |
| stmlib (its submodule) | https://github.com/pichenettes/stmlib | `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` |

The upstream paths are kept (`beads/...`, `stmlib/...`), so the includes stay as upstream writes them.

## Files

From eurorack, unchanged except `beads/dsp/delay_line.h` (below):

- `beads/dsp/`: `delay_clock.{h,cc}`, `delay_controller.{h,cc}`, `delay_grain.h`, `delay_granular_player.h`,
  `delay_line.h`, `frame.h`, `fx/fx_engine.h`, `fx/reverb.h`, `fx/wow_and_flutter.h`, `grain.h`,
  `grain_envelope.h`, `granular_player.h`, `granular_processor.h`, `limiter.h`, `modulated_parameter.h`,
  `mu_law.{h,cc}`, `multitap_delay.h`, `randomized_parameter.h`, `sample_rate_conversion_filters.h`,
  `sine_oscillator.h`, `tap.h`, `trigger_generator.{h,cc}`, `voice_stealing_envelope.h`, `wavetable_grain.h`,
  `wavetable_granular_player.h`, `wavetable_voice.h`
- `beads/dsp/processor.{h,cc}` and `beads/dsp/synth.{h,cc}`: not compiled. They are kept as the reference
  `cloud_engine.cc` follows (its RenderCloud is Processor::Process, its RenderSynth is Synth::Render).
- `beads/resources.{h,cc}`: the tables (the wavetables, the mu-law table, the SRC filters).

From stmlib, unchanged: `stmlib.h`, `LICENSE`, `dsp/{cosine_oscillator.h, delay_line.h, dsp.h, filter.h,
hysteresis_filter.h, hysteresis_quantizer.h, parameter_interpolator.h, sample_rate_converter.h, units.h, units.cc}`,
`utils/{dsp.h, gate_flags.h, random.h, random.cc}`.

stmlib is built with `-DTEST`, its portable path: without it, it uses Cortex-M instructions (`ssat`, `vsqrt`).

## The one change

`beads/dsp/delay_line.h`, `DelayLine::Init(uint8_t* buffers[])`: with `BEADS_DELAY_LINE_NO_CLEAR_ON_INIT` defined
(the Makefile defines it), Init calls `Reset(false)` instead of `Reset()`, so it does not clear the 768 KB buffer.
CLOUD's engine clears it itself, 64 KB a render call, so a voice's attach and a QUALITY change stay short. Without
the define the file behaves as upstream.

## Updating

Copy the same files from a newer commit, put the change above back, update the commits here and in `../../NOTICE`,
and run `make check`.
