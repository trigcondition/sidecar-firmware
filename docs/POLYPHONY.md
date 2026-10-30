# Polyphony: the Rytm's sequencer playing chords on Sidecar voices (research, 2026-10-19)

Question: can the Analog Rytm MKII's own sequencer play polyphonic Sidecar voices (chords, several notes a step on
one track) with a Digitone-like UI, and can the Rytm's pattern storage hold that data? No hardware. Every claim is tagged:

- **[measured]**: run in rytm-emu (OS 1.73 + rytm-mods `rui1027` = `sidecar-1027`, base `sections/us_rui1027.snap`,
  card `us_rui1027.img`, USB host simulated with `--usb-host --usb-midi-real`), or read in the disassembly by our earlier
  RE work (rytm-mods `re/symbols.toml`, `re/subsystems/*.md`, `mods/0021-ext`, `mods/0025-sendfx-report`).
- **[inferred]**: follows from measured facts, not checked directly.
- **[public]**: from public manuals, docs or source (Elektron manuals, MCL docs and source). Not checked against a device.

The emulator scripts are throwaway (session scratchpad). The runs and their results are listed in "Measurements".

## Answer in short

- **Storage.** A Rytm step holds one note: one byte in the track's 64-byte NOTE array [measured]. There are no spare
  per-step bytes in the pattern record (641 B a track, every per-step array in use) [measured + inferred]. The pattern
  size (13101 B) is fixed in the project's memory map and on the card [measured], so growing it is not realistic.
- **What does fit:** **p-locks**. Any sound parameter of the track can be locked on any step. A Sidecar machine's
  P1..P16 (sound words 9..16 and 17..24) are ordinary lockable parameters [measured, 0021/0025]. The cost is one of
  the pattern's **72 lock slots** for each (track, parameter), whatever the number of steps [measured + manual]. So
  **a chord can live in the Rytm pattern as a p-lockable CHORD parameter** (for example type, inversion and voicing),
  stored, copied and recalled by stock code, with no format change.
- **Trig report.** The trig report (0x04) gives the host the voice, one note, velocity, P1..P16 and the sound tag
  [measured]. The host can then expand the chord itself, given the CHORD/INV/... values in P1..P16.
- **Live.** Chromatic mode (CHRO) on the pads is monophonic per track. Each new pad retriggers the one voice, and the
  held-note slot keeps only the newest note [measured]. Live recording of a held chord stores **one** note, the last
  pad [measured]. The pads' own MIDI out **does** send every note-on and note-off [measured], so a host can play live
  chords from the pads today. It just cannot record them in the Rytm.
- **Voices.** The Pi has the CPU for many more voices than 8. The worst machine (6-OP FM) costs 1.22 % of a core per
  voice, which is about 65 voices a core [measured on the Pi, `docs/pi-engine.md`]. The Teensy does not: 8 voices take
  46 % on average and 83 % with all MODAL [measured, `docs/measure-teensy-plaits.md`].
- **Built (section 6, 2026-10-19):** the host half on the Pi: 6-OP FM's P13..P16 = CHD INV SPR VOI, a per-voice
  pool (6 a voice, 24 in all), live pad chords played at once and written back by 0x28.
- **Recommendation (section 4):** add a host-side **chord layer** driven by p-lockable **CHORD params on the Sidecar
  machine**, with per-track poly voice allocation on the Pi. The Rytm stores everything. The mods only need a CHORD
  page (records and value texts). Teensy (Sidecar Mini) gets at most a small shared pool, or no poly at all.

## 1. Rytm pattern and kit storage

### Per step [measured]

The live track block (`TrackPattern`, 917 B, `pattern_track` = pattern + 0x70 + 0x60 t accessor) has a 16-bit trig
word at +0x00 and 64-byte arrays at +0x80 note, +0xc0 velocity, +0x100 length and +0x140 microtiming. In each array,
-1 means "the track default" (+0x2c0/+0x2c1/+0x2c2). `trig_fire` reads exactly these (`re/symbols.toml`). Trig word
bits: 0x01 trig, 0x04 lock (trigless), 0x08 accent, 0x10 swing, 0x20 slide, 0x80/0x100/0x200/0x400 SYN/SMP/ENV/LFO
with explicit bits 0x800/0x1000/0x2000/0x40.

The stored track record (`pattern_track_save`/`load`, 641 B a track; bank pattern i at 0x405c8f38 + 13101 (i + 1),
track t at + 4 + 641 t). This is the measured layout of A01 BD after a live recording:

| offset | size | content | evidence |
|---|---|---|---|
| +0x000 | 112 | trig words, 14 bits a step, packed | step 4/8 bits changed by the recording [measured] |
| +0x070 | 64 | NOTE: `0x80 \| MIDI note`, 0xff = default | 0xc2/0xc4 (66/68) on steps 4/8 [measured] |
| +0x0b0 | 64 | velocity, 0xff = default | 0x7f on the recorded steps [measured] |
| +0x0f0 | 64 | length, 0xff = default | 0x19 [measured] |
| +0x130 | 64 | microtiming, 6-bit two's complement + 2 flag bits (0xc0 = 0) | the microtiming pack into a 6-bit field (`re/symbols.toml`) [measured]; the flags' meaning [inferred, public libanalogrytm: condition bits] |
| +0x170, +0x1b0 | 2 x 64 | 0xae / 0xe9 on every step: retrig length / rate [inferred] | |
| +0x1f0 | 64 | 0 on every step: probably the trig condition/probability [inferred] | |
| +0x230 | 7 | track defaults: note 0x3c, velocity 0x64, length 0x0a, flags 03 80, 0x10, 00 | [measured values, names inferred] |
| +0x237 | 64 | 0xff on every step: per-step sound lock (pool sound) [inferred] | |
| +0x277 | 1 | speed 0..6 (bits 3..6 carry nothing: load maps > 6 to 2) | [measured, symbols] |
| +0x279 | 1 | bit 7 euclid on, bits 0..1 Bool Operator, **bits 2..6 ignored by stock load** (0002 uses bit 2) | [measured] |
| +0x27a..+0x27f | 6 | euclid PL1 PL2 RO1 RO2 TRO and OP | [measured] |

Manual (OS 1.72): NOT is -24..+24 per trig and is overridden by chromatic live recording. VEL 1..127, LEN, PRB, and
TRC (trig condition) by conditional lock [public]. **One note per step, no room for a second** [measured]. The few
spare bits are per track, not per step: +0x279 bits 2..6 (partly used by mods) and +0x277 bits 3..6.

### P-locks [measured]

- **Live:** a dense int16 array, track * 2837 + step * 44 + sound index, -1 for none (`lock_pool_set` 0x400ac670).
  Every step can lock every one of the 42 sound words. At trig time, `sound_apply_step_locks` overlays them on a copy
  of the sound.
- **Stored:** after the 13 tracks (4 + 13 x 641 = 8337 B), the pattern has **72 lock slots of 66 B**: `{type, track,
  64 values}` (0xff none). A `0x80 0x80` continuation slot carries the fine byte. One slot holds one parameter of one
  track across all 64 steps. After the slots comes a 12-byte pattern tail (length, scale, kit; in use, not spare).
  Total 13101 B (`tools/emu/sfx_emu.py`, 0025 design).
- **Which params:** the 42 sound words of a voice track: machine words 9..16 (= a Sidecar machine's P1..P8), sample
  words 17..24 (= P9..P16, page 2), plus filter, amp, LFO and so on. The trig params NOTE/VEL/LEN have their own
  per-step arrays and use no lock slot.
- **Cost:** 72 different (track, parameter) pairs per pattern. A 16-bit value (fine) costs 2. The manual says the
  same: "Up to 72 different parameters can be locked in a pattern" [public].
- **Mod lock types work.** 0025 stores FX words 38..41 as its own lock types 42..49. Stock skips them at load and
  drops them at its next full rebuild: harmless loss, no corruption [measured, 0025 design]. A mod can therefore add
  its own per-step data inside the stock format, at the cost of lock slots.

### The sound (kit) [measured]

The stored sound record is 0xa2 B: 48 words, of which slots 42..47 are spare (stock zeroes them on save). The live
sound is 0x98 B with three unexplained bytes at +0x6d..+0x6f. Mod 0013 keeps a value there through load, save, kit
and pool, copy/paste and power cycles (`re/subsystems/sound-storage.md`). These are not lockable per step.

### Could an existing field carry extra notes?

| field | per step? | stored by stock? | verdict |
|---|---|---|---|
| Sidecar machine P1..P16 as CHORD / INV / SPREAD or NOTE2..4 offsets | yes, via p-lock | yes (pattern + kit) | **the way**: costs knobs and lock slots, nothing else |
| Unused sound words of the track (filter/amp/LFO words 25..41) | yes, via p-lock | yes | no: those drive the analog voice the Sidecar audio plays through |
| Spare sound slots 42..47 / live bytes +0x6d..+0x6f | no (per sound) | 0013's detours | per-sound chord table, not per step |
| A mod lock type (as 0025's 42..49) | yes | yes; stock load skips it, a stock save drops it | possible, but no live index to land in (all 42 used): needs a side array and hooks |
| Per-step sound lock (+0x237) | yes | yes | a whole pool sound per step: possible as a "chord preset" selector, but heavy |
| MIDI-track-like storage | - | - | the Rytm has no MIDI tracks (unlike the A4/Digitakt) |

## 2. What the Rytm sends on a trig, and the live side

### The 0x04 trig report [measured: `mods/0021-ext/ext.c`, `sim/protocol.py`, runs below]

`F0 7D 52 54 04 <voice byte> <note> <vel> <P1..P16 as 14-bit pairs> <tag hi> <tag lo> F7` (43 B). It is sent from the
voice's engine call (`ext_render`) when the firmware zeroes the voice's state word, which it does on every trig.

| field | content |
|---|---|
| voice byte | physical voice 0..7 in bits 0..2, machine flag in 0x78 (PLAITS 0x00 ... U1..U5 0x50..0x70) |
| note | the engine's pitch argument >> 16: 60 for the track default; the step's NOTE / chromatic pad added (60, 62, 68, 61 measured) |
| vel | the owner track's velocity |
| P1..P16 | the parameter block the engine gets, **after** p-locks and the LFO: so locked CHORD params arrive with their step |
| tag | the owner track's sound tag (user-slot plugin fingerprint) |
| (none) | no track index (only the voice), no step number, no length: the gate end arrives as **0x11 NOTE_OFF** `<voice>` (LEN elapsed, pad released, STOP) |

0x03 PARAMS sends P1..P16 changes between trigs (knob, LFO, slide). Within one audio block (0.67 ms), the latest trig
wins: `pend_trig` keeps one note [measured: three pads pressed in the same block gave one 0x04].

### Chromatic mode and live recording [measured, runs `chro3`, `rec`, `pb`]

- **CHRO, three pads held, overlapping** (BD, RS, CH 50 M apart, held 300 M): three 0x04s on voice 0 (notes 60, 62, 68).
  The held-note slot `gate[0][0]` (0x8000fe58) read 0x3c, then 0x3e, then 0x44: **only the newest note**. One
  NOTE_OFF came when all were released. The Rytm itself is monophonic per track. A held chord is three retriggers of
  one voice.
- **Pad MIDI out:** the same presses sent `9D 24 7F`, `9D 26 7F`, `9D 2C 7F` and then three `8D xx 00` on USB MIDI
  (auto channel, notes 36 + offset). **The full chord, with note-offs, reaches a host over MIDI.**
- **Live recording** (CHRO, RECORD + PLAY, the chord pressed at one instant, then a single pad): two steps were
  written, notes 0x80|68 (the chord's last pad, CH) and 0x80|66. Playback gave one 0x04 per step. **A held chord
  records as one note.**
- The sequencer itself sends no MIDI notes for its trigs (0021 design, measured with every MIDI setting). Hence the
  0x04 report.

### CHRO's pad range [measured 2026-10-29, rytm-emu, rui1052 build, BD on 6-OP FM; runs `up`, `dn`, `recx`, `midiin`]

UP / DOWN in CHRO move the 12 pads (BD .. CB a semitone apart) by **4 semitones a press**, not an octave [measured:
a 20 ms and a 300 ms press each +4; held 1 s, the key's repeat reached the top]. Kept when CHRO is left and entered
again. 3 presses up, 6 down from the default:

| shift | pad MIDI (auto ch.) BD .. CB | 0x04 note BD .. CB | our voices (0x04 = MIDI pitch, C4 = 60, TUN 0) |
|---|---|---|---|
| lowest (DOWN x 6, -24) | 12 .. 23 | 36 .. 47 | C2 .. B2 (65 .. 123 Hz) |
| default | 36 .. 47 | 60 .. 71 | C4 .. B4 |
| highest (UP x 3, +12) | 48 .. 59 | 72 .. 83 | C5 .. B5 (523 .. 988 Hz) |

- The pads' whole reach: 0x04 36 .. 83 (MIDI 12 .. 59), 4 octaves; the 0x04 is the MIDI note + 24, which is the
  binding's offset (poly.cpp `c.off`), so a bound channel's live notes and the Sidecar sequencer's recorded steps are
  36 .. 83 too. The auto channel's notes in: 12 .. 59 play chromatically (0x04 36 .. 83), 60 and up nothing [measured,
  as re/subsystems/midi-cc.md on the device].
- Live recorded at both ends (Rytm sequencer): steps stored 0x24 / 0x53 and played back as 0x04 36 / 83 [measured].
  The manual's NOTE -24 .. +24 is 0x04 36 .. 84 [inferred]: the pads stay inside it, so NOTE does not narrow them; any
  Rytm trig's 0x04 note is 36 .. 84.
- 6-OP FM / PLAITS / JUNO / MULTISAMPLE take the 0x04 note as a MIDI pitch (60 = C4 = 261.6 Hz), plus the machine's
  TUN. Their own range is the MIDI range: the pads miss C0 .. B1 under and C6 and up over.

## 3. Design options

How the voice audio returns: the host sends each voice on its Overbridge input. That input plays through the voice's
own analog filter, overdrive and **amp envelope** [measured, 0021]. N notes on one track are summed into one channel
and shaped by one analog amp. A new trig restarts that envelope, so the tails of earlier notes are re-enveloped
[inferred from the measured DISABLE behaviour]. For real overlapping poly: put the Rytm AMP on a long hold (or HLD
AUTO with a long decay) and let the host's per-note envelopes do the shaping. Alternatively, route the poly track to
MAIN (`core/main_out.h`: the Overbridge MAIN inputs, which skip the analog voice and its filter).

### (a) Chord in p-lockable machine params (recommended)

The Sidecar machine gets a CHORD page: **CHD** (type: off, maj, min, sus2, sus4, 7, m7, maj7, dim, aug, 6, m6, 9, add9,
5th, oct, user...), **INV** (inversion 0..3), **SPR** (spread/voicing: close, drop2, open, +oct), **VOI** (notes 1..4)
and optionally **STR** (strum ms), all as P-words.

- Every value is locked per step by stock code. Stored in the pattern (lock slots) and in the kit (the track default).
  It travels with copy/paste of steps, tracks and patterns, reload, save, power-off and Transfer, all stock paths.
  Lock cost: 1 to 3 of 72 slots per chord track.
- The 0x04 already carries the value: **no new message**. The host expands note + CHD/INV/SPR into N notes and
  allocates N voices. Length: all notes end on 0x11.
- **Where the knobs come from:** each machine's P1..P16 are full (PLAITS uses all 16; user-slot plugins define their
  own). Options:
  1. a third page with 8 more records (0021 already rewrites records for page 2, and RS/BT/LT TEST records are
     re-owned). Needs 8 more sound words, and there are none left in the 42 (all mapped). So no.
  2. **reserve page 2 slots P13..P16 (sample words 21..24) for CHORD on every Sidecar machine.** This is the cheapest
     route: PLAITS' VEL/GLD/VOL and the like move or go, and a plugin's manifest declares at most 12 own params when
     it wants chords. **Recommended.**
  3. replace a few of the analog voice's FLTR/AMP words: no, they shape the audio.
- **UI:** the stock page shows CHD/INV/SPR/VOI as knobs with value texts (0021's enum looks: "MIN7", "INV2"). The remote
  SRC view (0026) can draw a Digitone-like chord diagram: the notes on a mini keyboard, from the same values. Hold a
  TRIG and turn CHD for a per-step chord, exactly like any p-lock. The trig's NOTE sets the root.
- Effort: mods small (records, value texts, the page); host medium (chord table, voice allocator, per-track voice
  pool, envelopes). Risk: low. No format change, stock-compatible (stock loads it, ignores the meaning).

### (b) Host voice allocation: one Rytm track drives N Sidecar voices

This is needed by (a), (c) and (d) anyway. Per Rytm track, a pool of N engine instances, allocated on 0x04 and
released on 0x11/timeout. Steal order as MCL's: same pitch first, then a free voice, then the oldest. Mono machines
(DIGIDRUM) stay single.

- **Pi:** budget from the boot bench, mean % of a core per voice [measured]: PLAITS 0.73 (109 a core at 80 %), JUNO
  0.54, 6-OP FM 1.22 (65), DAISYSP 0.97, DSP SAMPLE 0.47 [`docs/pi-engine.md`]. Two render cores: **8 tracks x 4-6
  notes is easy** (48 FM voices = 58 % of one core). The guards stay: per-voice budget 25 % and the mute after 3 overs.
  Memory: Plaits needs 16 KB of scratch per voice (`shared[]`) and is heap-placed on the Pi: fine.
- **Teensy 4.1 (Sidecar Mini):** 8 voices = 46 % average, 83 % all-MODAL; `shared[8][16384]` already 128 KB
  [measured]. At most a global pool of +4 cheap voices (JUNO/VA) when PSRAM is present; default **no poly** (the
  memory rule: everything must work on a stock Teensy).
- Release tails across steps are only correct with the analog AMP held open (above) or out=MAIN.

### (c) Recording live chords from the pads in CHRO mode into p-locks (a mod)

The pads' note-ons all arrive at the firmware (they go out as MIDI) [measured], but the recorder keeps the last
[measured]. A mod could, while RECORD is on in CHRO mode on a Sidecar track, collect the notes held within a window
(about 30 ms) around the recorded step, compute (root, CHD, INV) and write the CHD/INV locks on that step through
`track_lock_set` (0x400c0520, the stock lock writer) [measured address; the recorder hook not located].
- Effort: high (find the live-record path, chord detection on the ColdFire, hold timing). Risk: medium (UI-task timing,
  undo). Only chords in the chord table can be recorded; other note sets become the nearest chord.
- A cheaper variant needs no Rytm mod: the **host** sees the pad MIDI (all notes) and the 0x04 of the recorded step.
  It detects the chord and writes the CHD/INV values back as a p-lock through a new message, "lock P13/P14 on step s
  of track t". That needs a mod handler calling `track_lock_set` from the UI task (as 0021's 1C knob write-back does
  for kit values). Effort medium.

### (d) External MIDI keyboard into Sidecar, recorded on the Rytm

A keyboard on the Sidecar host plays the poly track live (the host allocates). Recording: as (c), the host turns
chords into CHD/INV p-locks through the same write-back. Without that, the Rytm's own MIDI in (track channel) records
one note per step, as the pads do [inferred from the same recorder].

### (e) Sidecar's own sequencer for poly/chord tracks, synced to the Rytm (the MCL model)

The Sidecar runs note tracks (piano roll, N notes a step, its own lengths), clocked by the Rytm's MIDI clock and
transport (TRANSPORT SEND is on in our base). The Rytm acts as controller/UI through the remote pages (0026).

| | p-locks in the Rytm (a) | own sequencer (e) |
|---|---|---|
| saved with the Rytm project | yes, stock | no: a side file on the Sidecar SD, keyed by project/pattern (bundles carry it) |
| pattern change, chains, songs, fills, conditions, swing, micro timing, euclid | all stock | must be mirrored (pattern-change reports, song position) or reimplemented |
| copy/paste/clear on the Rytm | stock, data follows | invisible to the Sidecar |
| timing | the trig itself, sample-tight in the voice's block | MIDI clock at 24 PPQN + jitter; pattern switches need reports |
| arbitrary voicings, many notes, independent lengths | limited to the chord table (and NOTE2..4 offsets) | free |
| works without a Sidecar attached | the pattern keeps the data | data lives only on the Sidecar |
| effort | small mods + host chord layer | large (sequencer, editor UI on remote pages, sync, storage) |

Verdict: (e) only as a later, separate "Sidecar note track" for things the Rytm cannot express (long legato lines,
arps already planned in PLAN 5c). Chords on Rytm trigs: (a).

### (f) Extend the Rytm's pattern format itself (Digitone-style extra notes per trig)

- **Spare bytes:** none per step [measured]. All eight 64-byte arrays and the trig words are in use. The bit-7 flags
  in NOTE and microtiming are written by stock (0xc2, 0xc0) [measured]. The only spare bits are per track (+0x279 bits
  2..6, +0x277 bits 3..6) [measured, symbols], and some are already used by mods. The 12-byte pattern tail holds values
  in every pattern [measured, A01..A04]. Not done: a marker write and save/reload. With no candidate per-step field,
  there was nothing worth marking. Stock `pattern_track_load/save` convert field by field, so anything they do not map
  is dropped on the next save [measured, symbols].
- **Growing the structure: not feasible.** Patterns sit at fixed strides (bank pattern i at 0x405c8f38 + 13101 (i+1))
  in a project image that power-off writes raw to fixed card slots (0x7a000/0x7c000) [measured]. Kits are copied as
  fixed 0x9e3-byte blocks [measured]. Transfer/SysEx dumps and a stock OS expect the stock sizes [inferred]. A larger
  record would move every later pattern, and every OS routine that indexes it would need patching. There are hundreds
  of such sites, not enumerated. Backward compatibility with a stock OS would be lost.
- **Side file** (the Sidecar stores per project/pattern/track/step chords on its SD, like `/.rytm/slots`, carried in
  the project bundles, spec `docs/superpowers/specs/2026-10-17-project-bundles-design.md`). Keys: the Rytm project name
  (0x10 sends it), the pattern (would need a pattern-change report; 0x2B/33 carry track and machine, not pattern
  [inferred from the remote-UI messages]) and the step (not in 0x04: needs a new field). It goes stale when the user
  copies, pastes, clears, rotates or bakes euclid on the Rytm, moves patterns, or edits on a stock OS. None of these
  paths has a mod hook, and they are not even located [measured: only the microtiming batch path is traced]. Each
  would need its own detour.
  - Hybrid that keeps it correct: put only a **CHORD index p-lock** in the Rytm pattern (1 slot) and keep the *table*
    of voicings (up to 128 arbitrary note sets) per project in the side file/bundle. The index travels with every stock
    copy. The table is content that does not depend on position. Lost table = chords fall back to the built-in table.
- **Verdict:** format extension no; pure side file no (desync); p-lock CHORD (a), optionally indexing a per-project
  user chord table, yes.

### How others do it [public]

- **Digitone** (from its manual and Elektron's public docs, not verified here): one Digitone track is polyphonic. A
  VOICE menu reserves voices per track (8 in total), with steal and legato. KEYBOARD mode plays chords, and grid/live
  recording stores several notes on one trig (newer OS; Digitone II up to 4 notes a trig, edited on the NOTE page).
  Chords are trig data, not p-locks. The format was built for it.
- **MCL on MegaCommand with a Machinedrum** (jmamma/MCL, BSD-3-Clause; docs at jmamma.github.io/MCL; source read
  for behaviour only, nothing copied):
  - **MCL is the sequencer.** Its MD step tracks (16, up to 64 steps, speeds, conditions, micro timing, locks) are
    played by MCL over MIDI. Trigs go as note-ons on the MD's base channel, using a parallel-trig message (a bitmask of
    tracks as note-ons on base channel + 1/+2; needs the MCL-supporting MD OS "X" versions, the docs ask for X.13).
    P-locks go as CCs (`setTrackParam`) right before the trig. A small SysEx syncs track length/speed. The MD's own
    pattern memory is used only to import patterns. MCL keeps its patterns on the MegaCommand's SD: per project two
    grids (X/Y) of 16 slots x 128 rows, one slot = one track's sequence + sound (kit) state, loaded quantised per
    track. Clock: MCL's internal clock or MIDI/USB in, and it sends clock/transport.
  - **Poly on the MD:** the Polyphony page groups MD tracks (each to one of 16 MIDI channel groups) as the voices of
    one instrument. The allocator (`PtcVoiceRouter`) picks, per note, an active voice with the same pitch, then a free
    one with that pitch, then the oldest. Only pitch-capable machines are used. Synthesis params are linked across the
    group. MCL maps MIDI notes to each machine's PTC value (the MD has no notes, only a pitch parameter, locked per
    step: `set_track_pitch` = lock param 0). A chord is therefore **spread over several MD tracks**: one trig and one PTC
    lock on each voice track (recording "into the selected poly voice tracks"). External/PianoRoll tracks (6, up to
    128 steps) hold real polyphonic notes and can drive an MD voice group (CHANNEL MD1..MD16). Chords are entered by
    holding them on a MIDI keyboard + YES. An arpeggiator sits on the chromatic page.
  - The MD's own MIDI machine tracks carry chords as **NOTE + N2/N3 offsets (64 = none)**. MCL sends the three
    note-ons. This is exactly option (a)'s "offset params" model.
  - Lessons: (1) poly by **track grouping** works but eats tracks; on the Rytm a Sidecar track already owns host
    voices, so grouping is not needed. (2) **Offset/chord params are the proven in-format chord**. (3) An external
    sequencer gives full poly but owns the storage: the MCL user's data lives on the MegaCommand, not in the MD.

## 4. Recommendation

The smallest useful version, in order:

1. **Host (Pi): per-track poly voices + chord layer.** Voice pool per Rytm track (default 4, max 6). Expand 0x04's
   note by CHD/INV/SPR/VOI (P13..P16). Release all on 0x11. Steal as MCL (same pitch, free, oldest). Machines: PLAITS,
   JUNO, 6-OP FM, user slots that declare `poly`. Sum into the track's voice channel. Recommend AMP HLD long, or
   out=MAIN for the track. Teensy Mini: off (or +4 pooled voices with PSRAM).
2. **Mods: the CHORD knobs.** On every Sidecar machine, page 2's last four records become CHD INV SPR VOI, with enum
   value texts (0021's `us_text` enum look; the chord names in a 16-entry table). Lockable as any page-2 word: per-step
   chords by holding a TRIG and turning CHD. Nothing new on the wire.
3. **UI on the Rytm.** SMPL (page 2) shows `CHORD`: knobs E..H = CHD INV SPR VOI. The remote SRC view (0026) on that
   page draws the chord on a one-octave keyboard (root = the trig's NOTE) and the note names. TRIG page NOTE stays the
   root.
4. **Later:** (c-host) the host detects pad/keyboard chords during live recording and writes CHD/INV p-locks back (a
   new "lock this step" message, a UI-task handler on `track_lock_set`). Then an optional per-project user chord table
   in the bundle, indexed by CHD values above the built-ins.

What changes where:

| | mods (rytm-mods) | host (rytm-teensy) |
|---|---|---|
| step 1 | none | voice pools per track, chord table, allocator, AMP/MAIN guidance |
| step 2 | records 21..24 of page 2 renamed CHD INV SPR VOI on the Sidecar machines; value texts; plugin manifests limited to 12 own params when `poly` | manifests: a `poly` flag, P13..P16 reserved |
| step 3 | 0026 page id for the chord view | remote page drawing |
| step 4 | lock write-back handler (UI task, `track_lock_set`) | chord detection from pad MIDI / keyboard |

**Updated (section 5):** for real polyphony, combine this with the user's design: the analog voice pinned open
(amp records held at 127, filter depth 0, VCA de-velocitised, by a detour on the transport entry 0x4007a538), and
**the stock AMP/FLTR words used as the per-note digital envelopes**, sent in a 59-byte 0x04. Stock UI, stored and
p-lockable, true per-note envelopes and overlapping tails. A remote page is optional. The order is in 5.5.

Open risks: the analog amp envelope re-triggering held tails (to listen to on hardware); the 72-slot lock budget with
heavy p-lock users; the trig coalescing within one audio block (one 0x04 per voice per block, measured), harmless
for chords since they come from one trig.

## 5. Real polyphony: analog chain pinned open, per-note envelopes on the host (user design, 2026-10-19)

The design: on a Sidecar poly track, a mod pins the analog voice's AMP stage open and its filter envelope to zero, so
the analog voice becomes a static path (overdrive, filter at FRQ/RES, VCA at level and pan) for the summed digital
notes. Every digital note gets its own envelope on the host. Chords are stored as CHD/INV/SPR/VOI p-locks (sections
3a and 4).

### 5.1 Where the AMP and FLTR values go [measured]

**Sound words** (parameter records' container index, dumped from the ROM table at 0x401bf918 in rytm-emu):

| page | words |
|---|---|
| FLTR | 25 ENV, 26 ATK, 27 DEC, 28 SUS, 29 REL, 30 FRQ, 31 RES, 32 TYP |
| AMP | 33 ATK, 34 HLD, 35 DEC, 36 OVR, 37 VOL, 38 PAN, 39 ACC, 40 DEL, 41 REV |
| (others) | 1..8 LFO, 9..16 machine (P1..P8), 17..24 sample (P9..P16), 0 SEND FX (0025) |

They sit in the track's 42-word block. The trig loop copies it from the kit (0x8000e5b0 + 84 t) and p-locks are
overlaid on it. The LFO block then makes the modulated copy at 0x8000f7ba + 84 t. The machine routine gets `params` =
word 9 of that modulated block (`re/subsystems/voice-controls.md`).

**The control frame** (984 B at 0x800063c0, sent as 492 SPI words once a block). In 1.73 the audio interrupt runs, in
this order (disassembly of stock 1.73 MAIN OS, 0x40121200..0x40121500):

| order | call | writes |
|---|---|---|
| 1 | machine routines (dispatch 0x401211d6), incl. 0021's `ext_render` | machine CVs; 0x04/0x03 reports |
| 2 | `voice_output_mix` 0x4010ea2c (call 0x4012122e) | the voices' DAC channels (Sidecar audio enters here) |
| 3 | filter consumer 0x4010d090 (call 0x4012123a) | +58+2v FRQ, +74+2v RES, type bits, **+296+8v filter-envelope record** |
| 4 | amp consumer 0x401098d4 (call 0x40121246) | **+10/+26+2v VCA L/R**, +42+2v overdrive, **+168/+232+8v amp-envelope records** |
| 5 | FX, `fx_generate` 0x4010e650 (call 0x4012139a), then 0006/0025's hook site 0x401213a0 | FX |
| 6 | transport 0x4007a538 (call 0x40121464) | the frame to the voice boards |

So `ext_render` (step 1) **cannot** override the envelope words: steps 3 and 4 rewrite them afterwards. The override
belongs between step 4 and step 6. Options:
- the existing hook at 0x401213a0 (0025 uses it). That block driver also has a second path (0x4011fe08, not hooked;
  0025 design), whose blocks would go out un-overridden;
- **better, the transport's entry 0x4007a538**, which every path calls with the frame. Its callers in 1.73 are not
  enumerated; 1.74 has seven `jsr 0x4007a5a4`. Patching it on the frame before the copy covers all paths. The mod
  writes only the words of voices whose owner track plays a Sidecar poly machine, so other tracks are untouched.

**What the records do on a hit** (BD on PLAITS, record words sampled every 200 k instructions, about 1 ms):

| AMP setting | trig block | while held | after the gate (LEN) closes |
|---|---|---|---|
| default (ATK 0, HLD 0, DEC INF) | `4000007f 00000000` (start command 0x4000, target 127, rate 0) | `0000007f 00000000` | `00000000 00000000` (an all-zero record: no motion) |
| ATK 0, HLD 70, DEC INF | `4000007f 00000000` | `0000007f 00000000` | `0000007f 00000000`, stays |

The L and R records (+168, +232) are identical. The filter record for ENV 64 (the default) has depth word 0
(`00000000 001d01a2`): ENV = 64 already means no filter envelope.

**The VCA words carry the trig velocity:** +10/+26 for voice 0 read 0x0cc4 after a velocity-32 note and 0x32a9 after
velocity 127. 0x0cc4/0x32a9 = 0.2525 and 32/127 = 0.252, so it is linear [measured]. The words are latched per trig
and stay constant through the hit [measured, also rytm-emu RYTM-PORT]. So **on a poly track the last note's velocity
would scale every ringing note**. The mod must pin the VCA to the velocity-127 value (word x 127 / vel), and the host
applies each note's velocity.

**The override, per poly voice v, once a block before transport** [design; the record semantics are inferred from the
measured values]:
1. amp records +168+8v and +232+8v: `0000007f 00000000` (at 127, rate 0, no start command). On the first block of a
   newly poly voice, one `4000007f` to open it. Afterwards, every retrig's start command is masked, so the analog VCA
   never restarts from its start level under the ringing notes;
2. filter record +296+8v: depth word 0 (no analog filter envelope), whatever the FLTR ENV value;
3. VCA +10/+26+2v: scaled back to velocity 127 (level x pan stay the user's, so VOL, PAN, fades and the PERF/scene
   level still act).

About 30 instructions a voice a block. The trig's velocity per track is at 0x800067c4 + 2 t (0020, measured).

### 5.2 UX: the Rytm's own AMP/FLTR pages as the digital envelope

Yes, and this is better than a new page. The words stay where they are: in the kit, p-lockable, shown on the stock
AMP/FLTR pages, saved and copied by stock. The mod only stops them reaching the analog boards (5.1); the host plays
them per note.

- **The words the report needs:** 25 ENV, 26..29 filter A/D/S/R, 33 ATK, 34 HLD, 35 DEC: **8 words, +16 B**. The 0x04
  grows from 43 to **59 B**. Optionally add 39 ACC (accent) for 61 B. `ext_render` already has the modulated block
  (`params` is word 9, so word k is `params[k - 9]`): p-locks and the LFO land on their step for free. 0x03 gets the
  same words (55 B) for knob turns between trigs. The host tells the formats apart by length, as it does for 39/41/43
  today.
- **Rate:** 96 trigs a second (8 tracks of 16ths at 180 BPM) x 59 B = 5.7 KB/s. 0x03 is limited to one per 16 blocks
  per voice (about 94/s), at most 5.2 KB/s per voice under a running LFO. The USB MIDI sender's ring is 4 KB and is
  drained per USB frame; 0021 already sends 41-43 B per trig. This fits, but the 0x03 worst case with 8 LFO'd voices
  (about 40 KB/s) should be rate-limited further (the 0x20 reporter's `maxb` pattern) [inferred; measure on hardware].
- **The mapping on the host** (a Rytm-style envelope, not a full ADSR):
  - digital amp = AMP ATK (attack), HLD (hold; the step's LEN gate when HLD follows the gate) and DEC (decay to 0;
    INF = until the note-off plus a short release);
  - digital filter envelope = FLTR ATK/DEC/SUS/REL with ENV as bipolar depth. This is a true ADSR per note, applied
    to a per-note digital filter or to the machine's timbre (PLAITS TIMBRE/MORPH, JUNO VCF).
  - The amp has no sustain level on the Rytm. Where a machine needs an amp ADSR, the host can take S from a page-2
    word or reuse FLTR SUS (a choice to make).
- **A remote page is still useful, but not required:**
  - the stock labels stay right (ATK/HLD/DEC, ATK/DEC/SUS/REL/ENV); the stock envelope graphic is right too;
  - the remote SRC view (0026) can show "DIGITAL ENV", the per-note voice count, the chord, and a digital filter type
    if the host adds one. The stock TYP stays the analog filter's type.

### 5.3 What stays analog

- **Per track, on the sum:** the voice's overdrive (OVR, +42), the analog filter at FRQ/RES/TYP (+58/+74 and the type
  bits, written every block by the filter consumer, static with depth 0), and level/pan/sends (VCA, DEL/REV). FRQ and
  RES still act normally and the LFO can still move them. Only the per-note envelope parts go digital. [measured:
  the consumer writes FRQ/RES every block whatever the record; the depth-0 record is the stock ENV-64 case.]
- **Musically:** a static filter on a chord is how most poly synths with a global filter behave (a paraphonic or
  "filter on the bus" sound, like a Juno's VCF with the envelope at 0). Per-note brightness comes from the digital
  filter envelope or the machine's timbre. A resonant analog filter on a dense chord gives intermodulation through the
  overdrive. The OVR default should be 0 on poly tracks. [inferred]
- **The analog filter envelope** (FLTR ENV) can no longer move the analog filter on a poly track, because its values
  now mean the digital envelope. If someone wants both, that is a later per-track switch.

### 5.4 Risks

| risk | assessment | mitigation |
|---|---|---|
| clicks when pinning | each trig sends a start command (0x4000 in the record) [measured]. Whether the hardware jumps to the start level (0) for a block and then back up is not visible from the CPU [inferred risk] | mask the start command on retrigs (5.1 step 1); open once; change poly on/off only while silent |
| noise floor with the VCA held open | the voice's analog noise, the DAC noise and any DC offset pass continuously, even with no notes [inferred]; the hiss is that of an open voice at its level | the host reports "voice silent" (a new 2-byte message after the last tail), and the mod then closes the VCA with a stock decay record; or a timeout derived from DEC/REL |
| shared voices (CH/OH 4, MT/HT 5, CY/CB 6, RS/CP 7) | one physical voice, two tracks; a trig on the other track takes the voice (OWNER changes, 0021) and its stock envelope would be masked if the voice is still "poly" | apply the override only while the voice's current owner track is a poly Sidecar track. Recommend poly on BD, SD, BT, LT (voices 0..3, unshared) |
| choke | a voice cannot choke itself any more (that is the point); stock choke between shared tracks still switches the owner | as above |
| mute / fade | mute stops trigs; ringing digital notes continue [inferred]; VOL and fades still act through the VCA (we keep level x pan) | the host releases a track's notes on mute if 0026/0021 report mutes (not reported today: a small addition) |
| p-locked AMP words on a poly track | they reach the host on their step (good). A stock OS or a plain machine plays them as analog envelopes again (a portable sound) | none needed |
| LFO on AMP/FLTR words | the host gets the modulated values (per block in 0x03) | rate-limit 0x03 |
| 0x04 size | 59 B per trig | fits (5.2) |

### 5.5 Effort and order

1. **Host (Pi) poly engine:** voice pools, digital AHD amp + filter ADSR per note, chord expansion, steal. Medium,
   about 3-5 days. Can be tested now with the current 0x04 (envelopes from defaults).
2. **Mod A, the report:** 0x04/0x03 + 8 words (`ext_render`, a few lines; the host parses by length). Small, 1 day,
   emulator-testable (the report bytes).
3. **Mod B, the analog pin:** a detour on the transport entry 0x4007a538 (1.73): for each poly voice, records open,
   filter depth 0, VCA de-velocitised; poly = the owner track's machine is a Sidecar machine and its POLY knob (VOI >
   1) is set. Medium, 2-3 days with emulator checks (frame words before/after, other voices untouched, shared voices),
   then **hardware listening is mandatory** (clicks, noise floor, the record semantics).
4. **Mod C, the CHD/INV/SPR/VOI page-2 knobs** (section 4 step 2). Small to medium.
5. **The "voice silent" message + VCA close** (noise floor), after a hardware listen shows it is needed.
6. Remote view extras (DIGITAL ENV, chord diagram). Optional.

Order: 1 then 2 give audible poly with Rytm envelope values right away (with AMP set by hand to ATK 0 / DEC INF and
ENV 64, which is the stock setting the mod later enforces). Then 3, then 4, then 5/6.

### 5.6 Built: Mod B (rytm-mods rui1033seq2, 2026-10-20; emulator only)

The pin as designed in 5.1, at the transport entry 0x4007a538 (rytm-mods 0026 design.md "1033"), with three changes from
the plan after measuring: the records are stock's own ATK-0 encodings (`4000007f b0000000|rate` once, then
`0000007f 30000000|rate`: the firmware clears the mode byte after each send, so the sampled `0000007f 00000000` above
was a cleared record); the VCA is de-velocitised through the velocity word (0x800067c4 + 2t set to 0x7f00 after the frame
went out, the trig block's VCA words rescaled), because an empty track's velocity word is 0 and its VCA too; the voice
silent message is 46 (2 bytes: a track mask), closing with stock's decay record at DEC 40. Poly tracks are pinned by
2E's pin mask (seq=pin:T:on). Measured in rytm-emu: an empty BD on 6-OP FM with a sine into its Overbridge input is
silent stock and heard pinned (RMS 0.075); the VCA as sent stays at the velocity-127 value through velocity-32 and -64
trigs; the 0x04 still reports velocity 32. rytm-emu's analog model now reads the frame as sent (its voice_frame_sent
hook, branch seq-p2). Hardware listening is still mandatory (5.4).

## 6. Built: poly FM proof of concept (Pi only, branch `poly-fm`, 2026-10-19)

Sections 4.1 and 4.4 (host half), on the 6-OP FM machine only, Pi only (the Teensy stays at 0.1.0: `CORE_POLY` is not
defined there and the pool is compiled out). Code: `core/poly.h` / `core/poly.cpp` (chords, pool, live chords),
hooks in `core/voices.cpp` and `core/host_link.cpp`, the Pi's MIDI and console in `pi/engine/cores.cpp` and
`bridge.cpp`. Tests: `core/tests/test_poly.cpp`, `sim/tests/test_core_poly.py`. Nothing here ran on hardware yet.

### 6.1 The chord params: FM's page 2, P13..P16

6-OP FM used only P1..P8 (`sim/plaits/fm_machine.h`; P9..P16 were "unused" in `sim/protocol.py`), so nothing of FM is
lost: **P13..P16 (SMPL page knobs E..H, sound words 21..24) = CHD INV SPR VOI**. P9..P12 stay free. Each param is its
7-bit value used as a direct index; a value beyond the range reads as the first entry, because an old FM sound carries
the stock sample words there (STA 0, END 120, LOP 0, LEV 100), which then read OFF / 0 / 0 / AUTO: old kits keep
playing mono, unchanged.

| param | values | meaning |
|---|---|---|
| CHD (P13, word 21) | 0 OFF, 1 MAJ, 2 MIN, 3 SUS2, 4 SUS4, 5 7, 6 MAJ7, 7 MIN7, 8 DIM, 9 AUG, 10 6, 11 MIN6, 12 9, 13 ADD9, 14 MIN9, 15 5 | the chord on the trig's NOTE (the root); OFF = the mono voice as before; >= 16 OFF |
| INV (P14, word 22) | 0..3 | the lowest INV notes of the chord an octave up (INV % notes); > 3 = 0 |
| SPR (P15, word 23) | 0..2 | spread: note k of m raised 12 x (k (SPR + 1) / m) octaves, so the voicing opens over SPR + 1 octaves; > 2 = 0 |
| VOI (P16, word 24) | 0 AUTO, 1..6 | notes: AUTO the chord's own; fewer: the top dropped; more: the (inverted) chord again an octave up; > 6 = AUTO |

Intervals: MAJ 0 4 7, MIN 0 3 7, SUS2 0 2 7, SUS4 0 5 7, 7 0 4 7 10, MAJ7 0 4 7 11, MIN7 0 3 7 10, DIM 0 3 6, AUG 0 4 8,
6 0 4 7 9, MIN6 0 3 7 9, 9 0 4 7 10 14, ADD9 0 4 7 14, MIN9 0 3 7 10 14, 5 0 7. Order: inversion on the chord, then VOI,
then SPR; the result sorted, repeats dropped, notes outside 0..127 dropped. Value texts: `OFF` `MAJ7` ..., `INV2`,
`SPR1`, `AUTO` / `4` (`poly::value_text`, `sim/protocol.py` `chord_value_text`). The Rytm side of the texts and knobs
(records 21..24 of FM's page 2, list looks with max 15/3/2/6) is the rytm-mods poly-chord mod's: FM is a built-in
machine whose labels live in 0021, not a manifest. The host's table is sent to it as above.

### 6.2 The pool

- Per voice (= the track's Overbridge return channel: tracks sharing a voice, RS/CP, CH/OH, ..., share it) up to **6
  notes, 24 in all**. Each note is its own Plaits voice + `rytm::FmMachine` (its own DX7 envelopes and LEN gate, the
  voice's PAT and P1..P8), 16 KB of Plaits scratch each (384 KB, heap at boot: not in the kernel's .bss).
- A 0x04 with CHD on: the voice's earlier chord notes and its mono note get a note-off (their tails go on), the chord's
  notes start, each at 1/sqrt(n) gain, summed into the voice's channel after its mono note, exactly where the mono FM
  voice went. 0x11 releases the trig notes; LEN < 127 ends each note's gate on its own. A slot is free again once
  released and below -80 dB for 10 ms (or 10 s after its release).
- Steal: the same pitch on the voice (retrig), a free slot, the voice's oldest (released first) when it has 6, else
  the oldest released anywhere, then the oldest. A stolen note is cut (no fade: a click is possible at 24).
- CPU: 6-OP FM is 0.5 to 1.2 % of a core a note (`docs/pi-engine.md`), so 24 pool notes + 8 mono voices are at most
  ~39 % of one core, split over the two render cores; a voice with 6 notes is ~8 % of a core, well under the guard's
  25 % a voice. Each chord trig logs `trig voice V 6-OP FM entry E chord MAJ INV0 SPR0 VOI AUTO root R: n notes, a/24
  in the pool (peak p, steals s)`; the console's `:poly` prints the totals and the live bindings.
- CC 120, the link reset (Rytm hello, USB re-attach) and a voice leaving FM empty its pool at once.

### 6.3 Live chords (host half of recording)

The host does not know RECORD (no report carries it), so it treats every pad chord on a bound channel the same; the
Rytm decides (it writes only while recording, and answers so).

- **Binding.** The Rytm's pads send every note-on/off on USB MIDI (CHRO: the auto channel, note = the 0x04's note - 24
  on a default track [measured, run `chro3`]), while its voice sends one 0x04 a pad. A MIDI channel is bound to an FM
  voice when two note-ons each come within 30 ms of an 0x04 of that voice with the same offset (0x04 note - MIDI note;
  learnt, not assumed). A simultaneous chord may give only one 0x04 (one per audio block), so binding can take a second
  chord. Binding moves when the user plays another FM track; a voice leaving FM unbinds.
- **Monitoring.** A bound channel's notes play on the voice's pool at once, as held (note-off releases), at 0.6 gain.
- **Mono pads (steal).** The Rytm's chromatic pads are mono per track [measured 2026-10-20, rui1031]: a second pad
  sends the first one's note-off right after its own note-on (on A, on B, off A, on C, off B, ... and only the last
  pad's real off). A note-off for the channel's previous note within 5 ms (`kStealMs`) after another note-on, when that
  note-on came within 100 ms (`kGestureMs`) of the one before, is a steal: the note keeps sounding and stays in the
  chord. The next real note-off releases the group (its gesture's notes; a pad 100+ ms after the last starts a new
  gesture, and the old group goes with its pad's off). A melody whose notes do not overlap sends real offs before the
  next on: never a steal. Channels bound by `poly=bind` (a host keyboard: real offs) are left alone. Counted
  (`mono steals` in `:poly`). The sequencer's recorder (seq-mvp) takes the notes through `poly::set_note_sink`: the
  note-ons at their own times, the group's offs at its release.
- **Gain staging.** Every pool note at its own gain (a trig's 1, a live pad's 0.6: one note as loud as before), the
  voice's notes summed times a headroom gain 1/sqrt(max(1, E)), E the notes' weights (a held note 1, a released one
  its level over its level while held; MULTISAMPLE: each key's envelope), smoothed (3 ms down, 15 ms up, ramped over
  each block); then a soft limiter at the Overbridge return (linear to 0.8 of full scale, a tanh knee towards 0.98):
  a chord never hard-clips. The limited blocks are counted per voice (`limited vN:` in `:poly`, logged every 2 s).
- **No double sound.** While the channel is active (a note held, or one in the last 1.5 s), the voice's own 0x04s are
  swallowed when the matching note-on came (within 60 ms); an 0x04 that comes first is held 8 ms for its note-on, and
  played (mono, or a chord when that step has CHD) if none comes: the sequencer's own trigs still play during live
  play, 8 ms late. When the channel was idle and the 0x04 of a pad already played mono, its note-on cuts that note to
  the pool.
- **Chord window.** The note-ons of a gesture are one chord: the window closes 100 ms (`kGestureMs`) after its last
  note-on (a strum of pads 80 ms apart is one chord). A lone note released before the next note-on (a melody's) leaves
  the window. At the window's end, 2+ notes are detected and written back (one note: nothing is sent).
- **Detection** (`poly::detect`): exact first, the simplest reading that `expand` turns back into exactly the held notes
  (tiers: VOI AUTO, then doubled notes, then fewer notes; then INV, SPR, CHD order: C E G A is C6, not Am7 INV1). A set
  no chord reproduces: the chord whose pitch classes share the most (Jaccard > 1/2), INV from the bass, SPR from the
  span, VOI the count; else OFF on the lowest note. Every table chord round-trips (tested over all CHD/INV/SPR/VOI).

### 6.4 The 0x28 write-back protocol

Its own id (first proposed as 0x3C beside 0026's keepalive; moved, as 0x30..0x3F are all 0026's): 0x28 is in no mod
handler, and 0022's router passes it to 0026 (the poly-chord mod). Any other length than 13 is refused by the Rytm:

```
host -> Rytm  F0 7D 52 54 28 01 <track 0..11> <root> <CHD> <INV> <SPR> <VOI> F7     13 bytes
Rytm -> host  F0 7D 52 54 28 02 <track> <step 0..63 | 7F> <status> F7              10 bytes
              status 0 ok, 1 no recent step, 2 lock slots full, 3 not recording / not a Sidecar track / track > 11
              (step 7F); the Rytm writes value << 8 into sound words 21..24 and root as the step's NOTE
```

- root = the trig NOTE in the 0x04's unit (60 = default; the Rytm stores it as 0x80 | note [measured: stored 0xc2 plays
  as 0x04 note 66]). CHD/INV/SPR/VOI = the p-lock values for P13..P16 (6.1). VOI is sent as the note count.
- track: the track on the voice whose machine the Rytm's 0x33 says is 6-OP FM, else the voice's first track
  (`kTrackVoice`).
- The host takes `28 02` in `host_link.cpp` (`poly::reply`) and logs it: `poly: chord write-back track 8 step
  5: ok`. Counted in `:poly`.

### 6.5 How-to (test with a Pi running this kernel and a Rytm with the poly-chord mod)

1. On a track, pick the **6-OP FM** machine (SRC page). Choose a voice with PAT.
2. **AMP page: ATK 0, HLD max, DEC INF. FLTR page: ENV 64** (the analog amp/filter is shared by all the summed notes;
   with these it stays open and the FM notes' own envelopes shape each note). The analog-pin mod comes later.
3. **SMPL page (page 2): knob E = CHD.** Turn it from OFF to e.g. MAJ7: every trig on the track now plays a chord on
   its NOTE. F = INV, G = SPR, H = VOI.
4. **Per-step chords:** hold a trig key and turn CHD (or INV / SPR / VOI): a p-lock, stored with the pattern.
5. **Live:** CHRO on, play chords on the pads: you hear them polyphonically at once (the first press or two bind the
   pads to the track; the console says `poly: MIDI channel 14 plays voice 4 live`). With RECORD + PLAY, each chord
   is written to the step it lands on as NOTE + CHD/INV/SPR/VOI locks (the Rytm's mod; the console logs its answer).
6. `:poly` on the Pi's console: notes now, peak, steals, write-backs and their answers.

**On the Mac (no Pi):** `make -C core lib`, then `.venv/bin/python sim/poly_live.py [--wav OUT.wav]` is the host for rytm-emu's
`--ob-link` (window: `DTEMU_USB_MIDI_REAL=1 rytm FW.syx --card C.img --snapshot S --usb-host --ob-link`; headless: `dtboot ... --usb-host
--usb-midi-real --ob-link ADDR`): the real core renders the voices, the Rytm's USB MIDI goes in (pad notes to `poly::midi`), the core's
`28 01` goes back. Run end to end 2026-10-20: CHRO + RECORD + PLAY, pads BD+BT+HT+CB 10 ms apart -> `28 01 00 3c 06 00 00 04`,
`28 02 00 07 00`, step 8 stored as NOTE 60 + locks CHD 6 / INV 0 / SPR 0 / VOI 4, the next loops play it as MAJ7.

Known limits: one MIDI channel a voice; chords only from the 15-entry table (other sets become the nearest); the
Rytm's analog amp/filter still re-triggers on every trig unless set as in step 2; the 8 ms hold for sequencer trigs
while pads are being played; no end-to-end run against rytm-emu yet (needs the poly-chord mod's build).

## 7. E0: every Sidecar machine on a poly track (seqx, 2026-10-22; host only)

docs/SEQ-IDEAS.md E0, built on branch `seqx`; the details in docs/SEQUENCER.md "Built: seqx". What it changes here:

- **Option (b) built, for the sequencer's notes:** a track's poly config (VOICES, ALLOCATION, UNISON, the Analog Four's
  POLY CONFIG) picks where its notes go - the machine's own pool (6-OP FM, the note machines: VOICES caps the notes held),
  the voice's own machine (one instance: mono, retrigger / choke), or `core/inst.h`'s **instance pool** (PLAITS,
  DAISYSP, JUNO, user machines that opt in; DIGIDRUM was one until 2026-10-29, MD, its successor, is mono): 32 instances of 35 KB, each its own engine state, rendered on the
  voice's render core into its channel, so the guard's per-voice budget covers them. The Rytm's own trigs on those
  voices stay mono, as before (a seq track's own trig makes no note).
- **The budget check** replaces this section's estimate with a rule: a voice's instances and its own machine at their
  worst warm block (1.8 % of a core each [measured, docs/pi-engine.md]) under 20 % of a core (80 % of the guard's 25 %):
  every built-in fits 8; a heavy user machine is cut (`CLAMPED`, `CUT` on the pages).
- **Not built (later):** ALLOCATION and UNISON on the machines with their own pools (6-OP FM, MULTISAMPLE, WAVETABLE,
  GRANULAR take VOICES only: their pools allocate themselves, and WAVETABLE has a unison of its own); **poly groups**
  across tracks (GROUP is stored, not played: several tracks pooled into one instrument needs the tracks' voices summed
  on one channel or a note router, and the Rytm side's menu); DSP SAMPLE / GRAIN with more than one note (one card stream
  a voice; instances would need a stream each); the Rytm's own trigs on instances (the POLY config is the sequencer's);
  per-instance pan on a voice routed to TRK (mono: SPREAD needs OUT PRE / POST).

## Measurements (rytm-emu, 2026-10-19) [measured]

Base `sections/us_rui1027.snap` (clock 7400 M), firmware `fw/AR173_us_rui1027.syx`, a copy of `us_rui1027.img`.
dtboot with `--usb-host --usb-midi-real`, USB out via `DTBOOT_USB_OUT`.

| run | what | result |
|---|---|---|
| walk + yes | SRC SRC, DOWN x 23, YES | BD machine byte 0x8000ea00 = 0x2c (44 PLAITS) |
| padD | BD pad twice | 0x04 `(voice 0, note 60, vel 127)` x 2, 0x11 x 2 |
| chro3 | CHRO; BD, RS, CH pads at +0/+50/+100 M, held 300 M; SD later | 0x04 notes 60, 62, 68, then 61; 2 NOTE_OFF; pad MIDI `9D 24/26/2C 7F` then `8D .. 00`, `9D 25`; gate slot 0 0x3c, 0x3e, 0x44, -1 |
| rec | CHRO, RECORD + PLAY, BD+RS+CH at one instant, MT later, STOP | one 0x04 (68) for the chord; stored A01 BD NOTE[4] = 0xc2, NOTE[8] = 0xc4, VEL 0x7f, LEN 0x19; live block 0x407dd8e3 (+0x80 notes 0x42/0x44) |
| envdef / envinf | BD hits, AMP default vs ATK 0 HLD 70 DEC INF; frame +168/+232 (records), +10 (VCA) every 200 k | records in 5.1 |
| velv | auto-channel MIDI notes, velocity 32 then 127 | VCA 0x0cc4 / 0x32a9 (linear) |
| params | the ROM parameter table's container indices | the word map in 5.1 |
| pb | PLAY the recorded pattern | 0x04 notes 66, 68 alternating: one note per step |
| up / dn | (rui1052) CHRO, then UP (DOWN) x 8, BD and CB after each | 0x04 60/71, 64/75, 68/79, 72/83 then 72/83; down 56/67 .. 36/47 then 36/47; pad MIDI 24 under |
| recx | CHRO, DOWN x 7, RECORD + PLAY, BD; UP x 10, CB | live notes 0x24 (step 3), 0x53 (step 15); 0x04 36, 83 each loop |
| midiin | auto channel notes 12, 13, 36, 59, 60, 70 | 0x04 36, 37, 60, 83; none for 60, 70 |
| layout | dump of A01..A04 stored records | layout table above; header `00000005`, tails in use |

Not measured: the analog amp's effect on summed poly tails (an audio check), the marker survival of per-track spare
bits through Transfer/SysEx, and any of this on a real Rytm.
