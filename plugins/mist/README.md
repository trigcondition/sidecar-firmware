# MIST: a granular texture machine (clouds, smears, frozen pads, wavetable grains)

A Pi Sidecar user machine (RMPA package `mist.a64.rmp`, id `mist.texture`), shipped on its own, not in the release
build. The DSP is Beads', the texture synthesizer by Emilie Gillet (MIT: `LICENSE.beads`; the source and commits in
`NOTICE`, the vendored files in `third_party/eurorack/VENDORED.md`).

**What it does.** A built-in oscillator (Beads' own wavetable voice, 12 banks of 16 waves) plays the trig's note and
is recorded, all the time, into a buffer of several seconds. Grains read that buffer back: short or long, forwards or
reversed, transposed, panned across the stereo field, fed back into the buffer, and sent through Beads' reverb. FRZ
stops the recording, and the grains go on playing what the buffer holds after the note is gone. The SYNTH engine
(ENG) is what Beads plays when nothing is patched in: wavetable grains at the note, with no buffer.

**Why an oscillator.** Plugin API 3 gives a machine no audio input yet (`docs/plugin-api-3-plan.md`, part 9). With
one, MIST could granulate the track's own Rytm audio, as Beads does with its input; the engine already takes any
mono source.

**Pitch:** the trig's note plays the oscillator (C3 = note 48 is the wavetables' root), so the cloud follows the
note. PTCH transposes the grains on top of that.

**Notes:** a trig restarts the oscillator's envelope (2 ms up; about 200 ms down after the note off) and presses
Beads' SEED button: with DENS at the centre each trig makes one grain; elsewhere it resyncs the grain clock. The
cloud keeps sounding while the buffer holds sound: after a note off it fades as the recording does, unless FRZ is on.

**Memory:** the buffer (768 KB), the reverb (64 KB) and the engine (31 KB) live in the voice's arena part, 883 KB a
voice; the 64 KB voice state holds only the knobs. A voice that switches to MIST clears the buffer in pieces over the
first 3 ms (twelve render calls), so the attach stays short (about 13 us measured on the Mac). A QUAL change fades
the wet signal out, clears the buffer for the new format and fades back in: a gap of a few ms.

## The knobs

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | TIME | 24 | where the grains read in the buffer: left is now, right is the oldest sound. In the delay mode, the delay time |
| SRC | P2 | SIZE | 72 | grain size. Below a third the grains play reversed (shortest at the far left); fully right is the delay mode (no grains, a tape delay with the buffer) |
| SRC | P3 | DENS | 92 | left of the centre a steady grain clock (faster to the left), the centre only the trigs, right of it random grains (denser to the right) |
| SRC | P4 | PTCH | 64 | the grains' transposition, snapping to 0, 1, 3, 4, 7, 12 and 24 semitones each way (Beads' pitch knob) |
| SRC | P5 | SHAP | 64 | the grain envelope: square, decay, bell, reversed from left to right |
| SRC | P6 | FDBK | 40 | how much of the grains goes back into the buffer |
| SRC | P7 | SPRD | 64 | random grain shapes and a wider stereo spread |
| SRC | P8 | MIX | 96 | dry oscillator to cloud |
| MORE | P9 | RVB | 64 | Beads' reverb, more and longer to the right |
| MORE | P10 | FRZ | OFF | ON stops the recording: the grains play what the buffer holds (silence if it holds nothing yet: a voice just switched to MIST, or right after a QUAL change) |
| MORE | P11 | QUAL | DIGITAL | the buffer's format, as Beads' quality setting: LOFI (8-bit mu-law at 24 kHz with wow and flutter, the longest buffer), TAPE (12-bit at 24 kHz), VINTAGE (12-bit at 48 kHz), DIGITAL (16-bit at 48 kHz, 8 s). LOFI, TAPE and VINTAGE use a darker reverb |
| MORE | P12 | BANK | SINE | the oscillator's wavetable bank: SINE, TRISTK, ORGAN, TRISAW, BURST, FORMANT, DIGI, WAVES, FM 1..4 |
| MORE | P13 | WAVE | 40 | the wave in the bank |
| MORE | P14 | TRND | 76 | random grain position: right of the centre smooth drift, left of it jumps (Beads' TIME attenuverter with nothing patched) |
| MORE | P15 | PRND | 64 | random grain pitch, the same law |
| MORE | P16 | ENG | CLOUD | CLOUD (the buffer and its grains) or SYNTH (Beads' wavetable grains: TIME picks the grains' wave, QUAL their bit depth) |

TRND and PRND have a small dead zone around the centre, as Beads' attenuverters do.

## Build and test

```
make a64            # mist.a64.rmp (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
make package        # out/: the package, LICENSE.txt, NOTICE.txt, SHA256SUMS, BUILD-INFO-plugins.txt
make check PY=python3   # the package run on an arm64 Mac as the Pi runs it (tests/mist_check.cpp)
```

`make check` renders WAVs into `../build-wav/mist-*.wav` (a held note, freeze, maximum feedback in every format,
a note change, the SYNTH engine, QUAL changes mid-note, random knobs) and checks: no NaN, no denormals, nothing
written outside the voice's state or arena, the pitch following the note, freeze holding, feedback staying bounded,
no burst or click at a QUAL or ENG change, the control calls' time and the CPU per block.

MIT, Copyright 2026 trigcondition and contributors (LICENSE). Beads: Copyright 2016-2021 Emilie Gillet.
