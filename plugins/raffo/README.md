# RAFFO: a 4-oscillator mono synth with a ladder lowpass (basses, leads)

A Pi Sidecar user machine (RMPA package `raffo.a64.rmp`, id `raffo.mono4`), shipped on its own, not in the release
build. The DSP is [RaffoSynth](https://github.com/nicoroulet/RaffoSynth)'s (Nicolas Roulet, Julian Palladino, MIT:
`LICENSE.raffo`) as the Schwung module [schwung-moog](https://github.com/charlesvestal/schwung-moog) has it (the source
and commits in `NOTICE`): 4 oscillators (triangle, saw, square, 20 % pulse), a Moog-style 4-pole ladder lowpass with
its contour envelope, an amp ADSR, glide while legato. `raffo.c` lists the changes for the Sidecar (PolyBLEP edges,
the ladder to self-oscillation with a soft-clipped feedback, half key follow, block-rate coefficients, no LFO / noise).

**Mono and chords:** RaffoSynth is monophonic: on a mono track a trig while the gate is still on (an overlapping trig,
a slide) glides there without retriggering the envelopes (GLID; GLID 0 jumps). The manifest's `max_voices` 8 opts
into instancing as well: on a Sidecar poly track each note plays its own state (AUTO: 4 voices), so chords work, each
note without glide. State 172 B a voice, image 8 KB, declared CPU 70 permille (0.11 % of a Pi 4 core a voice by the
host's conversion; `make check` prints the Mac estimate, ~65).

The four oscillators: OSC1 (WAV1) the reference, OSC2 and OSC4 a pair detuned +-DTN2 around it (WAV2, level MIX),
OSC3 a sub an octave down (WAV1's wave, level SUB).

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | WAV1 | SAW | OSC1 and OSC3's wave: TRI SAW SQR PLS |
| SRC | P2 | WAV2 | SQR | OSC2 and OSC4's wave |
| SRC | P3 | DTN2 | 72 | OSC2 +, OSC4 - this detune, +-50 cents (64 = 0) |
| SRC | P4 | MIX | 85 | OSC2 + OSC4 level |
| SRC | P5 | SUB | 80 | OSC3 level (an octave down) |
| SRC | P6 | CUT | 44 | ladder cutoff, 20 Hz .. 20 kHz (half key follow from C3) |
| SRC | P7 | RES | 46 | resonance, self-oscillates near 127 |
| SRC | P8 | ENV | 64 | contour: the filter envelope's amount |
| SMPL | P9 | ATK | 0 | attack (amp and filter), 1 ms .. 5 s |
| SMPL | P10 | DEC | 60 | amp decay |
| SMPL | P11 | SUS | 110 | amp sustain |
| SMPL | P12 | REL | 30 | release (amp and filter) |
| SMPL | P13 | FDEC | 30 | filter decay |
| SMPL | P14 | FSUS | 24 | filter sustain |
| SMPL | P15 | GLID | 30 | legato glide time constant, 0 .. 1 s |
| SMPL | P16 | LEV | 100 | level |

Defaults: a fat bass (saw + detuned squares + sub, a plucked contour). Leads: SUB down, CUT / RES up, ATK ~14,
GLID ~46. Acid-ish: MIX and SUB 0, RES ~100, ENV up, FDEC ~25, FSUS 0.

## Build, check, install

```
make a64         # raffo.a64.rmp (aarch64-none-elf-gcc, else the rytm-circle docker image)
make package     # out/: raffo.a64.rmp, LICENSE.txt (ours and RaffoSynth's), NOTICE.txt, SHA256SUMS, BUILD-INFO-plugins.txt
make check       # tests/raffo_check.cpp: the package run on this arm64 Mac as the Pi runs it
make demo DEMO=/path/raffo-demo.wav    # tests/demo.py: a bassline, a gliding lead, a filter sweep
../../.venv/bin/python ../sc_render.py raffo    # the starter render check (pitch, sustain, release)
```

Install: upload `out/raffo.a64.rmp` on the Pi's web page (http://sidecar.local, Machines / User machines, or Files into
`/Plugins`), or copy it into `/Plugins` on the card; then pick RAFFO for a user slot (U1..U5) from the Rytm's slot
menu or the web page. For chords, use a poly track (POLY VOICES, or AUTO: 4).
