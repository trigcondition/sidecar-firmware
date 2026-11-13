# SAMPLER

A sample from the project's SAMPLES pool, played like a Digitakt sample track: no resonator, one page of knobs, a PLAY
mode that sets the direction and the loop. New in 0.5.0, next to DSP SAMPLE, which stays as it was (resonator, its
SRC and SMPL pages, MOD's seven modes): nothing about a DSP SAMPLE sound changes.

SAMPLER and DSP SAMPLE share one sample engine on the Sidecar: the same pool, the same sample RAM, the same streaming
from the card, the same sample editor.

## On the Rytm (Rytm firmware 0.5.0 and later)

SAMPLER is its own machine in the Rytm's machine list (machine 43). Its reports carry DSP SAMPLE's voice flag (0x20)
and SAMPLER's sound tag (0x29C1, on the wire `53 41`: `core::kSmplTag`, `core/machines.h`), and the Rytm's track list (33) names it by its machine
number, so the Sidecar tells the two apart per voice and per track. A Rytm firmware before 0.5.0 has no SAMPLER.

| Page | A | B | C | D | E | F | G | H |
|---|---|---|---|---|---|---|---|---|
| SRC | SMP | STRT | END | PLAY | TUNE | FINE | LOOP | LEV |
| SMPL | (empty) | | | | | | | |

| Knob | |
|---|---|
| SMP | the sample: a row of the project's SAMPLES pool. **000 is OFF**: the track is silent and loads nothing |
| TUNE | -60 .. +60 semitones (values 4 .. 124, 64 = 0) |
| FINE | fine tuning |
| PLAY | the play mode (below) |
| STRT, END | where the sample starts and ends, absolute (0 .. 120 = the whole file) |
| LOOP | the loop's start for FLP and RLP; 0 = OFF |
| LEV | level |

SAMPLER's knobs use DSP SAMPLE's sample words (word 17 TUNE, 18 FINE, 19 SMP, 20 PLAY, 21 STRT, 22 END, 23 LOOP,
24 LEV). P1 .. P8 are no knobs on SAMPLER. STRT and END sit on B and C, where their values show in the waveform; TUNE and
FINE on E and F, where the display shows their values. The knob order changed during the 0.5.0 test (it was SMP TUNE FINE
PLAY / STRT END LOOP LEV); the words did not, so sounds, locks, LFO and XY targets (P9 .. P16) are unaffected.

### PLAY

Nine equal zones, one a detent:

| Value | PLAY | |
|---|---|---|
| 0 .. 14 | FWD | STRT to END once |
| 15 .. 28 | REV | END to STRT once (backwards) |
| 29 .. 42 | FLP | forwards, then loops |
| 43 .. 56 | RLP | backwards, then loops |
| 57 .. 71 | SLICE | as DSP SAMPLE's SLICE |
| 72 .. 85 | GRAIN | as DSP SAMPLE's GRAIN |
| 86 .. 99 | STRCH | as DSP SAMPLE's STRCH |
| 100 .. 113 | PLUCK | as DSP SAMPLE's PLUCK |
| 114 .. 127 | WAVE | as DSP SAMPLE's WAVE |

For SLICE .. WAVE, STRT, END and LOOP do what DSP SAMPLE's STA, END and LOP do in that mode.

END before STRT turns the direction round: FWD plays from STRT down to END (backwards), REV from END up to STRT
(forwards).

REV and RLP play backwards from the card too, not only from the sample RAM.

### LOOP

- FWD and REV never loop.
- FLP plays STRT to END, then loops from LOOP to END. RLP plays END to STRT, then loops from LOOP down to STRT.
- LOOP 0 (OFF), or a LOOP not strictly between STRT and END: the whole region loops.
- A loop from a loop point plays from the sample RAM: a sample that is not in the sample RAM yet starts when it is
  there (as SLICE does). FWD, REV and a whole-region loop stream from the card at once.
- A sample longer than the sample RAM holds (about 87 s): its loop point is ignored and the whole region loops,
  streamed from the card. Loop points need a sample that fits the sample RAM.
- Without sample RAM (the Sidecar Mini, or `sampleram=0`): FLP and RLP loop the whole region.
- LOOP turned while a note plays: a note playing from the sample RAM follows it at once; a streamed note (a whole-region
  loop) keeps looping the whole region until the next trig.

### SMP OFF

Turn SMP down to 000. In the sample list (the browser), FUNC + NO moves the cursor to OFF and YES confirms the unload
(the sample browser's keys). FUNC + YES on SAMPLER's SRC view with SMP OFF says `000 IS OFF` and picks no free row:
OFF is the track's choice. (DSP SAMPLE keeps its FUNC + YES on OFF: the files for the bank's first free row.)

### Before the Rytm has said

The Sidecar knows a track is SAMPLER from the Rytm's track list (33, after its hello) and a voice from each of its
reports (their sound tag). Before either (a Sidecar restarted while the Rytm plays), a voice plays and shows as what
its last report said; its first report from the Rytm settles it. A report without the tag field says nothing either
way: the voice stays SAMPLER until a report with another tag or another machine. An edit keeps the voice what the
track it edits is (the Rytm's selected track; on a shared voice, RS / CP for one, each track its own machine).
The Rytm's firmware swapped, while the Sidecar keeps running, for one whose reports have no sound-tag field (an image
without the user slots): a voice that was SAMPLER stays SAMPLER until a report on another machine or a Sidecar restart.

### FILL

FILL (UP + RIGHT on the Rytm, docs/FILL.md) p-locks SMP over a SAMPLER or DSP SAMPLE track's trigs: a range of the
SAMPLES pool's rows (empty and OFF rows skipped), in order or at random. Its FROM / TO start on the rows the last quick
load put in (RIGHT in the sample list, docs/SRC-BANKS.md section 8), else on the sample the kit plays.

## Release notes text (0.5.0)

- **New machine: SAMPLER** (with the Rytm firmware 0.5.0): a Digitakt-style sample player next to DSP SAMPLE. One page,
  SMP STRT END PLAY / TUNE FINE LOOP LEV. TUNE is +-60 semitones. PLAY: FWD, REV, FLP, RLP (forwards or backwards, once
  or looped), then SLICE, GRAIN, STRCH, PLUCK, WAVE. LOOP sets the loop's start (0 = OFF: the whole region). SMP 000 =
  OFF. Samples play backwards from the card too.
- DSP SAMPLE is unchanged: its sounds, pages and modes play as in 0.4.x.
- A 0.4.x Sidecar with a 0.5.0 Rytm plays a SAMPLER track as DSP SAMPLE (dry, the old MOD meanings on the same words).
