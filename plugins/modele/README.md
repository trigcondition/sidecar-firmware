# MODEL E: a three-oscillator voice with a ladder filter (basses, leads)

A Pi Sidecar user machine (RMPA package `modele.a64.rmp`, id `modele.voice`), version 1.1.0, in the release's plugins
zip from 0.4.1. The oscillators are [ModelDigi](https://github.com/simonjohansson/ModelDigi)'s bank (GPL-2.0-or-later, vendored
unchanged in `third_party/modeldigi`, the commit in `NOTICE`): three oscillators with edge-smoothed waves, oscillator
3 as a modulator (taken before its level, so it can modulate in silence), analog drift and the mixer's saturation.
ModelDigi stops at the oscillators; `modele.c` adds the rest of a playable voice: a 4-pole ladder (no unit delay in its
loop, prewarped, so the resonance and the self-oscillation stay on the cutoff; the differential pair's soft clip at
its input), a filter and a loudness contour, glide, noise.

**Licence: the whole package is GPL-2.0-or-later** (`LICENSE`, `NOTICE`), unlike the other plugins, because it builds
ModelDigi's code in. `make package` puts the complete source next to the binary (`out/modele-source.tar.gz`); whoever
passes the package on passes that on too.

**Mono and chords:** on a mono track MODEL E is one state: every note glides from the last one when the glide is up,
the contours rise from where they are and nothing clicks. The manifest's `max_voices` 8 opts into instancing: on a
Sidecar poly track each note plays its own state (AUTO: 4 voices). State 340 B a voice, image 20 KB, declared CPU 180
permille, the same as VOX: see `make check`'s cpu lines.

## The 16 knobs

The Model D has more controls than 16, so some share a knob:

| Page | P | Knob | Default | |
|---|---|---|---|---|
| SRC | P1 | OSC1 | 8 SAW | range + wave: all 36 pairs (below) |
| SRC | P2 | OSC2 | 8 SAW | range + wave |
| SRC | P3 | OSC3 | 16 SQR | range + wave; LO is the modulation range |
| SRC | P4 | TUN2 | +6 | oscillator 2's tuning: fine to +-1 semitone at +-21, then 1 semitone per 7 steps, +-7 at the ends |
| SRC | P5 | TUN3 | 0 | oscillator 3's, the same; 5 times wider with CONTROL off |
| SRC | P6 | MIX | 0 | osc 1 and 2 full at the centre; left osc 2 falls away, right osc 1; the last 10 % of the travel fades the other too (both silent at the ends: osc 3 or noise alone, or the filter alone) |
| SRC | P7 | O3NZ | +40 | right of the centre oscillator 3's level, left noise (one or the other). The modulation does not depend on it |
| SRC | P8 | MOD | 0 | modulation by oscillator 3 of the other two, up to 1.5 octaves. **Right: osc 3 follows the keys** and modulates itself too (CONTROL on). **Left: osc 3 free** of the keys (CONTROL off). For a free drone without modulation: MOD -1 |
| MORE | P9 | CUT | 44 | cutoff, 20 Hz .. 20 kHz, at C4 |
| MORE | P10 | EMPH | 44 | emphasis; self-oscillates from ~102 |
| MORE | P11 | CONT | 72 | contour amount, up to 7 octaves |
| MORE | P12 | G<>A | 0 | left of the centre glide (to 1.5 s a time constant), right attack (to 10 s, both contours). One or the other |
| MORE | P13 | DEC | 66 | decay, both contours, 4 ms .. 10 s to -40 dB |
| MORE | P14 | SUS | 64 | one knob for two sustains: up to the centre the loudness rises to full (filter contour 0), above the centre the filter contour's sustain rises. 0 a pluck, 64 a bass, 127 an organ |
| MORE | P15 | MODE | K2 AN | below |
| MORE | P16 | LEV | 100 | level |

Each list holds all 36 range + wave pairs, in the order LO 32' 16' 8' 4' 2', each with TRI T/S SAW SQR WID NAR (OSC3: TRI RSW
SAW SQR WID NAR): names `LO TRI` .. `2 NAR`, one detent a pair, zone = value x 36 / 128.

**On an older Rytm** (the 0.3.x mods cache 16 names; this needs the 0.4.0 Sidecar on the Pi, whose list limit is 36): the Pi
serves that Rytm 16 names, every 2.25th pair, each the pair its stored value really plays, and the knob's long name reads
`Osc 1 16/36`: what it shows is what plays, the other 20 pairs cannot be reached until the Rytm's mods are updated. A value
stored by the new mods off those 16 pairs plays its own pair, and an old Rytm's screen names the nearest below. An older Pi
(limit 16) refuses the package: it is RMPA format 5, which says so.

T/S is the triangle / saw mix, RSW the reverse saw, WID and NAR the two rectangular pulses. 8' plays the note's pitch.

MODE: **K0 .. K3** the cutoff's keyboard tracking (0, 1/3, 2/3, 1 octave an octave, from C4), **AN** Analog on (the
drift and the mixer's saturation), **R** the DECAY switch on (the release is the decay time; off about 6 ms).

### Patches

No preset files, as the other plugins; the Rytm's sound pool holds them. Starting from the defaults (a fat, slightly
detuned bass):

- Rubbery bass: OSC1 16 SAW, OSC3 8 SQR, CUT 36, EMPH 70, CONT 92, DEC 54, SUS 48.
- Lead: OSC2 8 SQR, OSC3 4 SAW, TUN2 +5, O3NZ +32, CUT 62, EMPH 62, CONT 38, DEC 80, SUS 92, MODE K3 AN R, G<>A -28.
- Vibrato: OSC3 LO TRI, O3NZ 0, TUN3 +50, MOD -18 (left: free of the keys).
- Trill: OSC3 LO SQR, O3NZ 0, TUN3 +36, MOD +32.
- Self-oscillating lead (the filter alone): MIX -64, O3NZ 0, CUT 58, EMPH 127, CONT 0, SUS 127, MODE K3 AN.

Not there: the Model D's filter modulation (osc 3 / noise on the cutoff; use the Rytm's LFO on CUT), noise as a
modulator, separate filter and loudness times (16 knobs).

## Build, check, install

```
make a64         # modele.a64.rmp (aarch64-none-elf-gcc, else the rytm-circle docker image)
make package     # out/: modele.a64.rmp, LICENSE.txt, NOTICE.txt, modele-source.tar.gz, SHA256SUMS, BUILD-INFO-plugins.txt
make check       # tests/md_check.cpp: the package run on this arm64 Mac as the Pi runs it
make demo DEMO=/path/dir    # tests/demo.py: bass, lead with glide, filter sweep, osc 3 modulation (four WAVs)
../../.venv/bin/python ../sc_render.py modele    # the starter render check (pitch, sustain, release)
```

Install: upload `out/modele.a64.rmp` on the Pi's web page (http://sidecar.local, Machines / User machines, or Files
into `/Plugins`), or copy it into `/Plugins` on the card; then pick MODEL E for a user slot (U1..U5) from the Rytm's
slot menu or the web page.

In a release's plugins zip (the docs repo's `scripts/package_pi.sh`): the package, `modele-LICENSE.txt`,
`modele-NOTICE.txt` and `modele-source.tar.gz` beside it, outside the zip's MIT `LICENSE.txt`.
