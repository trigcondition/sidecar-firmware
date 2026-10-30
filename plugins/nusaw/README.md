# NUSAW: a supersaw user machine (pads, stabs)

A Pi Sidecar user machine (RMPA package `nusaw.a64.rmp`, id `nusaw.supersaw`), shipped on its own, not in the release
build. The DSP is [NuSaw](https://github.com/saustin2010/mpc-vst-nusaw)'s (Charles Vestal, MIT: `LICENSE.nusaw`; the
source and commit in `NOTICE`): 7 PolyBLEP saws (a centre and 3 detuned pairs, 1:3:6 spacing, NuSaw's piecewise detune
curve up to 10 % of f0), the centre-anchored mix (RMS normalised), random phases at a fresh note, slow per-saw drift,
a sine sub an octave down, a DC blocker, a 2-pole TPT lowpass with its own ADSR, an ADSR amp. `nusaw.c` lists the
changes for the Sidecar (mono, one note a state, block-rate coefficients, Q at most 12, a click-free retrigger).

**Chords:** the manifest's `max_voices` 8 opts into instancing: on a Sidecar poly track (SEQ page POLY, or the web
page's Poly tracks) each note plays its own state (AUTO: 4 voices). State 184 B a voice, image 8 KB, declared CPU
55 permille (0.09 % of a Pi 4 core a voice by the host's conversion; `make check` prints the Mac estimate).

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | DETN | 38 | detune (NuSaw's curve: fine to ~50, wide above) |
| SRC | P2 | MIX | 89 | centre .. sides (the side saws' level) |
| SRC | P3 | CUT | 72 | lowpass cutoff, 20 Hz .. 20 kHz |
| SRC | P4 | RES | 13 | resonance, Q 0.5 .. 12 |
| SRC | P5 | ENV | 57 | filter envelope amount, 0 .. 8 octaves |
| SRC | P6 | ATK | 23 | amp attack, 1 ms .. 10 s (23: ~5 ms) |
| SRC | P7 | REL | 83 | amp (and filter) release |
| SRC | P8 | SUB | 16 | sine sub, an octave down |
| SMPL | P9 | TUN | 64 | +-12 semitones |
| SMPL | P10 | DEC | 64 | amp decay |
| SMPL | P11 | SUS | 102 | amp sustain |
| SMPL | P12 | FDEC | 70 | filter decay |
| SMPL | P13 | FSUS | 38 | filter sustain |
| SMPL | P14 | VEL | 64 | velocity amount |
| SMPL | P15 | LEV | 100 | level |

Pads: ATK and REL up (70 / 100), ENV down. Stabs: the defaults, REL ~55.

## Build, check, install

```
make a64         # nusaw.a64.rmp (aarch64-none-elf-gcc, else the rytm-circle docker image)
make package     # out/: nusaw.a64.rmp, LICENSE.txt (ours and NuSaw's), NOTICE.txt, SHA256SUMS, BUILD-INFO-plugins.txt
make check       # tests/nsaw_check.cpp: the package run on this arm64 Mac as the Pi runs it
make demo DEMO=/path/nusaw-demo.wav    # tests/demo.py: a pad and two stab lines
../../.venv/bin/python ../sc_render.py nusaw    # the starter render check (pitch, sustain, release)
```

Install: upload `out/nusaw.a64.rmp` on the Pi's web page (http://sidecar.local, Machines / User machines, or Files into
`/Plugins`), or copy it into `/Plugins` on the card; then pick NUSAW for a user slot (U1..U5) from the Rytm's slot
menu or the web page. For chords, set the track's POLY VOICES (or leave AUTO: 4).
