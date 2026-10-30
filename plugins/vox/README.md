# VOX: a sung / spoken vocal tract user machine

A Pi Sidecar user machine (RMPA package `vox.a64.rmp`, id `vox.trombone`), shipped on its own, not in the release
build. The DSP is [Pink Trombone](https://dood.al/pinktrombone/)'s (Neil Thapen, version 1.1, MIT:
`LICENSE.pinktrombone`; the source in `NOTICE`): the glottis (the Liljencrants-Fant waveform shaped by the tenseness,
aspiration noise, vibrato and the natural pitch / tenseness wobble) into a 44-segment waveguide tract at twice the
sample rate with the 28-segment nasal tract at the velum, the tongue's rest shape, a constriction (stops, fricative
noise, plosive transients), the diameters slewing to their targets (no zipper when a knob or an LFO moves them).
`vox.c` lists the changes for the Sidecar (knobs for the touches, an envelope, the note as the pitch, a glide,
32-bit floats, a release to exact silence, a NaN guard).

**Chords:** the manifest's `max_voices` 8 opts into instancing: on a Sidecar poly track (SEQ page POLY, or the web
page's Poly tracks) each note plays its own state (AUTO: 4 voices). State 1608 B a voice, image 16 KB, declared CPU
180 permille (the Mac estimate). Measured on a Pi 4 (2026-10-30, the user's unit): 5,629 ns a 12-frame block, about 2.3 % of a core a voice, so 8 voices fit easily; `make check`
prints the Mac estimate.

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | VOWL | 4 | tongue position, back (a, o) .. front (e, i), within what TONG allows |
| SRC | P2 | TONG | 33 | tongue diameter: high (i, u: VOWL's full range) .. low (open; VOWL matters less) |
| SRC | P3 | CONS | 96 | constriction position, pharynx .. lips (~58 g/k, ~96 d/t/n, 127 b/p/m) |
| SRC | P4 | CDIA | 127 | constriction diameter: 0..~40 closed (a stop), ~44..62 fricative noise, above an approximant, 127 none |
| SRC | P5 | TENS | 94 | tenseness: 0 a whisper, breathy .. pressed |
| SRC | P6 | BRTH | 64 | aspiration (64: Pink Trombone's) |
| SRC | P7 | VIBR | 21 | vibrato depth (21: Pink Trombone's 0.5 %) |
| SRC | P8 | NASL | 0 | velum: closed .. open (m, n with a closure; nasal vowels) |
| SMPL | P9 | ATK | 68 | voicing attack, 1 ms .. 2 s (68: ~60 ms) |
| SMPL | P10 | REL | 74 | release, 5 ms .. 4 s (74: ~0.25 s) |
| SMPL | P11 | GLID | 16 | glide (portamento) time constant, 3 ms .. 1.5 s |
| SMPL | P12 | WOBL | 32 | the natural pitch / tenseness wobble (64: Pink Trombone's) |
| SMPL | P13 | VRAT | 64 | vibrato rate, 2 .. 10 Hz |
| SMPL | P14 | VEL | 64 | velocity amount |
| SMPL | P15 | LEV | 100 | level |

The defaults sing "aah". "ee": VOWL 127, TONG 0. "oo": VOWL 0, TONG 0 (rounder with CONS 127, CDIA ~75). Syllables:
p-lock or LFO CDIA to 0 for a few steps at a CONS (with NASL up for m / n); "s": CONS ~100, CDIA ~52.

## Build, check, install

```
make a64         # vox.a64.rmp (aarch64-none-elf-gcc, else the rytm-circle docker image)
make package     # out/: vox.a64.rmp, LICENSE.txt (ours and Pink Trombone's), NOTICE.txt, SHA256SUMS, BUILD-INFO-plugins.txt
make check       # tests/vox_check.cpp: the package run on this arm64 Mac as the Pi runs it
make demo DEMO=/path/vox-demo.wav    # tests/demo.py: vowels, a chord, syllables, a glide
../../.venv/bin/python ../sc_render.py vox    # the starter render check (pitch, sustain, release)
```

Install: upload `out/vox.a64.rmp` on the Pi's web page (http://sidecar.local, Machines / User machines, or Files into
`/Plugins`), or copy it into `/Plugins` on the card; then pick VOX for a user slot (U1..U5) from the Rytm's slot
menu or the web page. For chords, set the track's POLY VOICES (or leave AUTO: 4).
