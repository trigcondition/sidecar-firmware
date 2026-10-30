# CHOP: a sample-chopping machine for the Sidecar's voices

A Sidecar machine that cuts one sample into up to **64 slices** and plays them from the Rytm's trigs, its pads in CHRO,
the Sidecar sequencer's notes and the pads / keys bound to the voice; every slice has its own settings (pitch, gain,
reverse, envelope, loop, choke group, start nudge, stutter, tape stop, neighbour chance), non-destructive. Pi 4 / Pi 5
and the Mac tests (`CORE_MSAMP`); the Sidecar Mini (Teensy) has no CHOP. The design:
`docs/superpowers/specs/2026-10-24-chop-machine-design.md`. Clean-room: the Octatrack's slice editor, the MPC Live's lazy
chop and chop screen and the Dirtywave M8's slice list are the models for behaviour only (their manuals and use), none
of their code.

Tags as in the other docs: **[measured]** run on the Mac (or in rytm-emu) and recorded; **[code]** read in the source;
**[inferred]** follows from those, not run on the real Rytm; **[open]** to be measured on the real Rytm.

## 1. For players

### Choosing it

Machine flag **0x18** (`core/machines.h` `kChopMachine`; 0x18 was the GRAIN placeholder no Rytm ever sent), named
**CHOP**. On the Rytm (rytm-mods rui1051): the machine list's **CHOP** (machine 49, after LIVE IN), on every track.
Without rui1051: `chop=voice:V` on the console makes voice V play CHOP whatever built-in machine the Rytm reports.

### The sample

**SRC** (P1) is a row of the project's **SAMPLES** pool (docs/POOLS.md), the same list GRANULAR and DSP SAMPLE use:
turn SRC (its bank list on the Rytm, knob A on the SRC view), **FUNC + YES** on a row for the card's files. The sample is
held whole in the pool RAM (one copy a content, shared with the other machines), at most 87 s (longer: its first 87 s,
logged). It loads as soon as the Rytm's machine table names CHOP on a track and its SRC is known (the SRC view), so the
first trig does not wait; a trig while it still loads plays when it is in, within 250 ms.

### The knobs

| knob | label | what it does |
|---|---|---|
| P1 (A) | **SRC** | the sample (a SAMPLES row) |
| P2 (B) | **SLC** | the slice the Rytm's trigs play: 1..64 in 64 zones (p-lockable: a slice a step) |
| P3 (C) | **TUNE** | -24..+24 semitones on every slice |
| P4 (D) | **MRPH** | MORPH: the played slice crossfaded with its right-hand neighbour (equal power, the starts aligned), live while it plays |
| P5 (E) | **SEED** | SHUFFLE: 0 OFF (the map's order), 1..127 a seed: which slice a pad / note / trig plays is permuted (the same seed, the same order); a scene / PERF lock on SEED varies it live |
| P6 (F) | **NBR** | NEIGHBOUR CHANCE on top of the slices' own: that share of hits plays the previous or the next slice instead (repeatable) |
| P7 (G) | **VEL** | velocity's share of the level |
| P8 (H) | **LEV** | the level |
| P9 | **STUT** | fully down: the slice's own; then x2..x16 (16 zones) |
| P10 | **SLEN** | the stutter's repeated part, 5 ms .. 1 s (down: the slice's) |
| P11 | **TAPE** | SLC / OFF / STOP / SPIN (4 zones) |
| P12 | **TTIM** | the tape's time, 20 ms .. 4 s (down: the slice's) |
| P13 | **ATK** | 0.5 ms .. 2 s (down: the slice's) |
| P14 | **DEC** | 10 ms .. 10 s, fully up HOLD (down: the slice's) |
| P15 | **DIR** | SLC / FWD / REV |
| P16 | **LOOP** | SLC / OFF / FWD / PING |

P9..P16 override the slice's own setting for the hit (fully down: the slice's), so a p-lock stutters, tape-stops or
reverses one trig. On rui1051 the Rytm writes their defaults (all down) when CHOP is chosen.

### Which slice plays

- **The Rytm's trigs and pads:** slice **SLC + (note - 60)**: a plain trig plays SLC; in **CHRO** the 12 pads (notes 60..71)
  play a 12-slice window from SLC, the octave keys move it by 12 (the bank switch); a trig with a NOTE lock plays an
  offset from SLC. Modulo the slices.
- **The Sidecar sequencer (a poly track):** the step's note **is** the slice, **note 0 = slice 1** (the piano roll is a
  slice lane), modulo the slices; conditions, ratchets and locks per step as on any poly track. Its own pool of slice
  players: VOICES up to 8 (E0, `core/inst_alloc.h`).
- Then SHUFFLE (SEED) and NEIGHBOUR CHANCE pick the slice that sounds.
- A one-shot slice plays to its end (a note-off does not cut it); a looped one loops while held and releases over 30 ms.

### Per-slice settings (non-destructive)

| field | range | |
|---|---|---|
| PTCH / FINE | -24..+24 semitones / -50..+50 cents | |
| GAIN | -24..+12 dB | |
| REV | FWD / REV | |
| ATK / DEC | 0..2000 ms / 0 (HOLD) .. 10 s to -60 dB | |
| LOOP | OFF / FWD / PING | while held |
| CHOK | OFF, G1..G4 | slices in a group cut each other (the voice's; a 2 ms fade) |
| NUDG | -50..+50 ms | START NUDGE: the start moved, the end kept |
| STUT / SLEN / SDEC | x1 (off) .. x16 / 1..100 % of the slice / 0..90 % | STUTTER: the first part repeated, each repeat that much quieter, then the rest |
| TAPE / TTIM | OFF / STOP / SPIN, 20 ms .. 4 s | TAPE STOP: the pitch falls to zero over the slice's end (it lasts longer); SPIN-UP: rises from zero at its start |
| NBR | 0..100 % | NEIGHBOUR CHANCE of this slice |

### Chopping

- **GRID** 2..64 equal slices, each start moved to the nearest zero crossing within 2 ms. A sample without a map gets
  GRID 16 (not written to the card until it is edited).
- **TRANSIENT** with a sensitivity (0..127): onsets in the first difference's energy (an attack's highs), 5.3 ms hops, a
  hop over 1.4..8 x the mean of the 8 before starts a slice, refined to the attack and back to its zero crossing; 50 ms
  apart at least. Runs in 64K-frame pieces on the control core (no core waits for it).
- **MANUAL**: ADD (a marker: the slice around it split there), DELETE (a gap where it was), MERGE (with the next), SPLIT,
  a slice's START and END (a touching neighbour follows). **UNDO**: one level for every edit (UNDO again: the edit back).
- **LAZY** (the MPC Live's): arm - the sample plays from its start; each pad / trig / screen tap drops a marker where the
  player **heard** the sample: the playhead at the tap **less the latency**, snapped to a zero crossing within 2 ms; UNDO
  takes the last back; taps past 64 (and double taps within 64 frames, and taps after the sample ended) are refused and
  counted; STOP keeps the markers as the slices (each to the next, the last to the end), CANCEL drops them. The latency:
  the output FIFO + half the device's own OUT -> IN loop (the way out to its outputs) + 96 frames for the key's way back:
  **336 frames, 7.0 ms** with the USB FIFO of 96 and the Rytm's 289-frame loop [inferred; `chop=lat:N` sets it, the
  editor's G knob too]. In rytm-emu, four pad taps 300 ms apart came out as markers 13985 / 14467 / 14316 frames apart
  (14400 each) [measured, rui_e2e choplazy].

### On the Rytm (rui1051)

**The SRC view** (the Rytm's SRC page on a CHOP track): the sample's outline with its slices, the slice SLC plays
inverted, the playheads, `SLICE 4/16 NAME` over SRC SLC TUNE MRPH; SEED NBR VEL LEV below. A **push of encoder B..H** on
it opens the editor (A's push stays the source list's).

**The editor** (page **CHOP**; only from its SRC view's push - not in the SIDECAR MENU since rui1051 (d9f03f3: a page of the selected
track opens from that track's keys); off the LEFT / RIGHT ring, no page dots) after the Octatrack's
slice editor: the outline of the selected slice and its neighbours, the markers, the selection inverted, the playheads.
**PAGE** steps through three views:

| view | A | B | C | D | E | F | G | H |
|---|---|---|---|---|---|---|---|---|
| EDIT | SLC (select) | STRT | END | ZOOM (ALL NEAR SLICE START END) | GRID n | SENS | LAT | RSMP (DRY / +FX) |
| EDIT pushes | ADD | DEL | MERGE | UNDO | GRID | TRANSIENT | LAZY (again: STOP) | RESAMPLE |
| SLICE 1 | PTCH | FINE | GAIN | REV | ATK | DEC | LOOP | CHOK |
| SLICE 2 | NUDG | STUT | SLEN | SDEC | TAPE | TTIM | NBR | RSET (push: SHUFFLE back to the map's order) |

STRT / END move a pixel of the view a detent, **FUNC** held: a frame. **YES** auditions the slice; **LEFT / RIGHT** the
slice before / after; **T1..T16** select and audition the slices of the trig bank (their LEDs: the bank's slices, the
selected one orange), **DOWN** the next bank of 16 (4 banks: 64 slices); **NO** leaves. LAZY armed: the pads, the trigs
and T1..T16 tap, push D undoes the last marker, **YES** keeps (STOP), **NO** drops them. A RESAMPLE's offer: **YES**
replaces the slice, **NO** keeps it. The KEYS page lists these (`core/ui/page_keys.h`, KEYS rows only: UP's map is one
screen of 18 rows and a push on SRC is no UP combo at all - see docs/UI-THEMES.md "The device's shortcuts"). Renders:
`core/build/rui_chop/*.png` (`test_chop`).

### On the touchscreen (page 17, CHOP)

A CHOP track's VOICE page (UP + T4 on the Rytm, the VOICE key) after the MPC Live's chop screen and the M8's slice list:

- **the waveform**, large: the markers with their numbers, the selected slice lit, the playheads; **drag a marker** to
  move it (sent at the release), tap a slice to select it, drag elsewhere to pan, **ZOOM - / +** around the selection
  (the whole sample down to 1/64 of it); LAZY: the markers so far and the sample's playhead.
- **16 pads** with bank tabs **A..D** (slices 1..16 .. 49..64): a tap selects and auditions; LAZY: a tap is a tap.
- **the slice list** (the M8's): the slice, its start and length in ms, and the settings in columns (OLED one, CLASSIC
  two, three on 1024x600), the cursor's cell framed; **arrows** move the cursor (row, column), **- / +** edit it (x10 on
  the long ranges).
- **keys**: LAZY (again: STOP), TAP, UNDO, GRID (4 8 16 32), TRANS, ADD (split the selection), DEL, RSMP; an offer:
  REPLACE / KEEP.

Pinch-zoom is not built: the Pi's touch driver reports one point (ZOOM keys and the drag instead). Both sizes and both
themes are shot by `make -C pi/ui/host shots-all` (`*-chop*.png`).

### The weird tools

1. **SHUFFLE** (SEED, P5): a keyed permutation (core/seq/rng.h's hash, purpose `kRngShuffle`): the same seed gives the
   same order on every voice and in every build; 0 (RSET) the map's own order.
2. **STUTTER** per slice (and P9 / P10 per hit): the first part repeated N times with its decay, then the rest.
3. **TAPE STOP / SPIN-UP** per slice (and P11 / P12): the speed follows a linear ramp to / from zero (the read position's
   square root law), so a stop lasts half its time longer and lands at the slice's end.
4. **RESAMPLE SLICE** (the editor's push H, `chop=resample:V:K[:fx]`): the slice is played once on its voice and taken
   **after its INSERT chain** (AMP included) by the sampler (`core/samp`, source "Sidecar voice"), with **+FX** the send
   FX's returns added (SPACE and the others; every voice's sends are in them: play the slice alone), written on the card
   core into `/Samples/<project>/<name>-S<k>.wav` and the SAMPLES pool, then offered: REPLACE makes it the slice's own
   sample (its region in the map names that file; the pitch / reverse / stutter / tape it baked in are reset).
5. **MORPH** (P4): the neighbour read at the same place, equal-power gains cos / sin of MRPH x 90 degrees, live.
6. **NEIGHBOUR CHANCE** per slice and NBR (P6): `1 - (1 - slice) (1 - knob)`, a keyed draw per hit (`kRngNbr`; seed
   SEED, else a fixed one): the same hits after a map change pick the same neighbours.

## 2. The slice map

Stored in the **INSTRUMENTS** pool, found by the **sample's sha256**: an `.sfz` instrument whose member is the sample.

- **CHOP's own:** `/CHOP/<project>/<name>-<sha8>.chop.sfz` (format **slicemap v2**, `core/fmt/formats.cpp`,
  `core/chop/slicemap.h`). Written through the card queue (`fmt::write_atomic` on the card core) a second after the last
  edit; the first write puts it into the INSTRUMENTS pool (`pools::add_take`), later writes accept the row's new content.
  A **bundle carries it** with its sample (the INSTRUMENTS row and its members, docs/BUNDLES.md); `.sqp` patterns and
  kits refer to the sample's row (SRC), the map follows the sample's hash.
- **The sampler's slice maps** (`<take>-slices.sfz`, docs/SAMPLING.md) are **slicemap v1**: found the same way (CHOP's own
  file first), read through the format registry's upgrade chain v1 -> v2, then written as CHOP's own file (the
  sampler's is left as it is).

The file is an SFZ other players read (one region a slice: `offset`, `end`, `key` 36.., `transpose`, `tune`, `volume`,
`direction`, `ampeg_attack` / `ampeg_decay`, `loop_mode` / `loop_type`, `group` / `off_by` for the choke groups), with
CHOP's own opcodes beside them (`sc_nudge sc_stut sc_stutlen sc_stutdec sc_tape sc_tapems sc_nbr`, a RESAMPLE'd slice's
`sc_from` / `sc_to`), and `<global> sc_sha= sc_size= sc_frames= sc_rate= sc_ch= sc_name= sample=`:

```
// sidecar slicemap 2
// CHOP's slices of one sample (docs/CHOP.md): a region a slice; sc_* opcodes are CHOP's, other players skip them
<control> default_path=
<global> sc_sha=9f2c...e1a0 sc_size=768044 sc_frames=192000 sc_rate=48000 sc_ch=2 sc_name=AMEN BREAK sample=/Samples/LIVE/amen.wav
<region> key=36 offset=0 end=23999 loop_mode=one_shot
<region> key=37 offset=24000 end=47999 transpose=3 ... group=2 off_by=2 sc_stut=4 sc_stutlen=25 sc_stutdec=20 sc_tape=stop sc_tapems=400
<region> key=38 offset=0 sc_from=48000 sc_to=96000 loop_mode=one_shot sample=/Samples/LIVE/AMEN_BREAK-S3.wav
```

A newer file is refused (locked) unless its `min=` lets this build read it (read, never written back); a damaged one
gives GRID 16 and a log line.

## 3. Phase 2: the LIVE source (not built)

The engine takes a **slice source** that is either a FILE (the SAMPLES row's sample in the pool RAM) or a **RAM buffer**
(`chop::use_buffer(voice, pcm, frames, ch, rate)`: the tests and the demo use it; a fresh map, never written). Phase 2's
record trigs fill such a buffer; the editor, the tools and the players need nothing else.

## 4. Threads, memory, CPU

- **The render core** (the voice's): `chop::render_add` / `render_add2` read the slices' starts from a sequence-locked
  mailbox per player (as GRANULAR's notes) and the pinned pool RAM; a player cut by a new start fades as a tail (2 ms).
  **No allocation, no card time** [measured: 0 `operator new`, 0 us of card time over 4400 blocks of 8 players,
  `test_chop`].
- **The control core:** reports, notes, the edits, TRANSIENT's pieces, LAZY, the map's find / read / write requests
  (cardq: never this core's card time [measured: 0 us on the control core over the map and RESAMPLE tests, the card
  core's 244 ms]), the console.
- **Memory:** the overviews 512 KB (the board's high heap), the maps ~80 KB, the sources the pool RAM's.
- **CPU:** 8 slice players with MORPH on (16 reads a frame) **0.205 % of a Mac core, 0.026 % a player** [measured,
  `test_chop`]; Pi 4 ~0.9 % / 0.11 % by the x 4.4 ratio docs/INSERT-FX.md uses [inferred]; the boot bench measures `chop`
  (8 looped players on a synthetic break, MORPH half way) on the Pi (`pi_run.py bench`) [open]. E0 declares 0.30 % an
  instance, VOICES 8 at AUTO.

## 5. Console

```
chop                                 stats, each CHOP voice's map
chop=voice:V[:off]                   the override (until the Rytm's list has CHOP)
chop=map:V                           the slices' frames
chop=grid:V:N  chop=trans:V:SENS     GRID 2..64, TRANSIENT 0..127
chop=add:V:FRAME  chop=del:V:K  chop=merge:V:K  chop=split:V:K[:FRAME]  chop=start|end:V:K:FRAME  chop=undo:V
chop=set:V:K:FIELD:VALUE             FIELD: PTCH FINE GAIN REV ATK DEC LOOP CHOK NUDG STUT SLEN SDEC TAPE TTIM NBR
chop=play:V:K                        audition
chop=lazy:V:arm|tap|undo|stop|cancel
chop=resample:V:K[:fx]  chop=replace:yes|no
chop=lat:N|auto                      LAZY's latency (frames)
```

## 6. Tests and the demo

- `core/tests/test_chop.cpp` (in `make -C core test`) [measured]: the file (v2 written and read back byte for byte, the
  sampler's v1 upgraded, newer / foreign-newer / damaged / NUL, msamp's SFZ parser reads the regions, 4000 mutations),
  the edits (the snap, GRID, add / split / delete / merge / move, the 64 limit, NUDGE), TRANSIENT on a synthetic break
  (9 of 9 hits within 3 ms at sens 64), LAZY with simulated taps (7 ms late with up to 1.5 ms of jitter: each marker
  within the jitter + 2 ms, the double tap, UNDO, 64), the machine on a RAM buffer (the slice of a trig / a CHRO pad /
  a sequencer note, PITCH, GAIN -6 dB, DECAY, ATTACK, REVERSE, LOOP FWD / PING and the release, CHOKE, SHUFFLE, NBR 50 %:
  102 of 200 hits moved, the same hits twice, MORPH 0 / 64 / 127, STUTTER (repeat 2 = repeat 1), TAPE STOP (15525 frames
  for 12000, 167 Hz at the end of a 700 Hz tone), SPIN-UP, the knobs' overrides, UNDO), LAZY on the engine, a card
  sample (the trig that waits, the map written into /CHOP/LIVE and the INSTRUMENTS pool, found again by its hash after a
  reset, the sampler's v1 map found and upgraded), RESAMPLE through a DRIVE insert (the take in the SAMPLES pool, the
  slice plays it), the render core's allocations and card time, the CPU, the Rytm pages (PNGs, the text-fit check).
- `core/tests/test_fmt.cpp`: slicemap's golden files (`core/tests/golden/slicemap`, `make_golden.py`), the fuzz target
  covers it (`make -C core fuzz-fmt`).
- `pi/ui/host` shots: `*-chop`, `-chop-zoom`, `-chop-lazy`, `-chop-offer`, `-chop-bank-b`, `-chop-none`, both themes,
  both sizes, with the commands the controls send checked.
- `sim/rui_e2e.py chopmach chopchro choplazy` (rytm-emu, rui1051, the real core) [measured 2026-10-24]: CHOP from the
  Rytm's machine list (its reports 0x18, the core's trig), LIVE IN one up (0x78); CHRO pads BD SD RS play slices 1 2 3;
  PUSH B on the SRC view opens the CHOP editor, PUSH G arms LAZY, four BD taps 300 ms apart give four slices spaced as
  the taps, YES keeps them.
- `make -C core demo-chop OUT=dir`: `chop_demo.wav` (24.5 s): a synthetic break chopped by LAZY (22 taps, the worst
  marker 1.5 ms from its hit), replayed in a new order, SHUFFLE (SEED 7, 19), STUTTER, TAPE STOP / SPIN-UP, MORPH swept.

## 7. Deviations from the design

- The machine flag is **0x18** (the spec said "a new machine id": 0x78 went to LIVE IN, 0x18 was the only flag left;
  GRAIN's placeholder no Rytm ever sent).
- The sample is always **whole in the pool RAM** (not streamed): a slice reads anywhere; 87 s at most (as GRANULAR).
- **RESAMPLE** plays the slice once in real time on its voice and takes it after the INSERT chain (the sampler's Sidecar
  voice source) instead of an offline render on core 0: the AMP's state is per chain and cannot be run twice at once;
  the +FX take includes every voice's sends.
- TRANSIENT runs in pieces on the control core, not on core 0.
- No pinch-zoom on the touchscreen (one-point touch): ZOOM keys and a drag.
- P9..P16 are per-hit overrides of the slice's settings (the spec named the per-slice settings, not the knobs).
