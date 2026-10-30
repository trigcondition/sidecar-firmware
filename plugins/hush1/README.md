# HUSH1: an SH-101-style mono synth (bass, leads)

A Pi Sidecar user machine (RMPA package `hush1.a64.rmp`, id `hush1.sh101`), shipped on its own, not in the release
build. The DSP is [HUSH ONE](https://github.com/charlesvestal/schwung-hush1)'s (Charles Vestal, MIT: `LICENSE.hush1`;
the source and commit in `NOTICE`): a saw / pulse / sub / noise mixer into a soft-clipping "pushed" mixer, a resonant
4-pole lowpass with drive, a cubic stage soft clip and its feedback law, the envelope amount and keyboard tracking on
upstream's cutoff curve, an ADSR, exponential glide, a slow pitch drift, the VCA's ENV / GATE switch. `hush1.c` lists
the changes for the Sidecar (PolyBLEP oscillators, a prewarped filter that opens to 18 kHz and self-oscillates on its
own, one ADSR for VCF and VCA as on the SH-101, block-rate cutoff and glide, a click-free retrigger; no LFO, presets
or note stack: the Rytm's LFO, trigs and sounds do those).

**Mono and chords:** on a mono track HUSH1 is one state, the SH-101: every note glides from the last one when GLID is
up (the 101's portamento ON). The manifest's `max_voices` 8 still opts into instancing, as NUSAW: on a Sidecar poly
track (SEQ page POLY, or the web page's Poly tracks) each note plays its own state (AUTO: 4 voices), so a chord
works, each instance gliding from its own last note. State 168 B a voice, image 8 KB, declared CPU 75 permille (0.12 %
of a Pi 4 core a voice by the host's conversion; `make check` prints the Mac estimate).

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | SAW | 100 | saw level |
| SRC | P2 | PULS | 0 | pulse level |
| SRC | P3 | PW | 40 | pulse width, 50 % (0) .. 5 % (127); turn it, or modulate it, for PWM |
| SRC | P4 | SUB | 72 | sub square, an octave down |
| SRC | P5 | NOIS | 0 | noise level |
| SRC | P6 | CUT | 34 | cutoff, 30 Hz .. 15 kHz (squared), at C4 |
| SRC | P7 | RES | 44 | resonance; self-oscillates from ~110 |
| SRC | P8 | ENV | 84 | envelope to cutoff |
| MORE | P9 | ATK | 0 | attack, 1 ms .. 10 s |
| MORE | P10 | DEC | 46 | decay |
| MORE | P11 | SUS | 24 | sustain |
| MORE | P12 | REL | 22 | release |
| MORE | P13 | GLID | 0 | glide (portamento) time, off .. ~1.2 s a time constant |
| MORE | P14 | TRK | 64 | keyboard tracking of the cutoff, 0 .. 1 octave an octave |
| MORE | P15 | LEV | 100 | level |
| MORE | P16 | VCA | ENV | ENV: the ADSR shapes the level; GATE: full while the gate is open (the ADSR only on the filter) |

The defaults are a punchy saw + sub bass. Lead: PULS ~70, CUT ~60, SUS up, GLID ~35, TRK ~90. Acid-ish: SUB 0,
RES ~95, DEC ~40, SUS 0, ENV up.

## Build, check, install

```
make a64         # hush1.a64.rmp (aarch64-none-elf-gcc, else the rytm-circle docker image)
make package     # out/: hush1.a64.rmp, LICENSE.txt (ours and HUSH ONE's), NOTICE.txt, SHA256SUMS, BUILD-INFO-plugins.txt
make check       # tests/hush_check.cpp: the package run on this arm64 Mac as the Pi runs it
make demo DEMO=/path/hush1-demo.wav    # tests/demo.py: a bass line, a glide lead, a filter sweep
../../.venv/bin/python ../sc_render.py hush1    # the starter render check (pitch, sustain, release)
```

Install: upload `out/hush1.a64.rmp` on the Pi's web page (http://sidecar.local, Machines / User machines, or Files into
`/Plugins`), or copy it into `/Plugins` on the card; then pick HUSH1 for a user slot (U1..U5) from the Rytm's slot
menu or the web page.
