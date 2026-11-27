# WAVETABLE: a band-limited wavetable synth machine for the Sidecar (Pi)

## In short

- A polyphonic wavetable machine on the Pi engine (`core/wavetable/`, Pi and Mac only: `CORE_MSAMP` builds; the Teensy
  is frozen and has none). Machine flag **0x28** (`core/machines.h` `kWaveMachine`); until the Rytm's 0021 has the
  machine, `wt=voice:V` plays it on voice V whatever built-in machine the Rytm reports.
- Tables: TBL is a row of the project's **WAVETABLES** POOL list (docs/POOLS.md): six built-ins generated here (BASIC
  PWM VOCAL ORGAN FM SYNC, 64 frames each, nothing third-party; the template puts them in A001..A006), or any WAV /
  AIFF on the card (by its hash) in the open format Serum, Vital and others write: N single-cycle frames of 2048
  samples, or the frame size its `clm ` chunk names, or 1024 / 512 / 256, or one cycle of any length (AKWF's 600), or
  any longer WAV cut into 2048-sample frames.
- Every frame is held as 10 mip levels an octave apart (harmonics 512 >> k): a note reads the level whose top harmonic
  stays under 0.6 fs, so nothing aliases audibly at any pitch [measured: a C7 saw's aliases -67 dB under its
  harmonics, against -14 dB reading level 0; C2..C8 all under -67 dB up to 18 kHz].
- Per note: up to 7 unison oscillators (stereo summed to the voice's mono return), **two tables** (A and B, each with
  its own frames and POS, played at once in every lane), a warp (nine: SYNC PWM FM FORMANT QUANT BEND SQUEEZE RM), a sub
  sine, a filter (the DJ-style SVF, low-pass below CUT 64 and high-pass above, or LP / BP / HP at 12 and 24 dB, or a
  ladder) with drive, an amp ADSR and a mod envelope on both POS. A trig is one note: **chords come from a Sidecar poly
  track** (its sequencer notes, the live pads, the keyboards), not from the machine; trigs, the sequencer and the live
  pads / keys all play it.
- Derived in part from Vital by Matt Tytel (GPL-3.0-or-later): the new warps and filters (section 9: the Pi binaries carry
  its licence).
- CPU [measured: Mac; inferred: Pi]: the boot bench's note (3 oscillators, SYNC) ~1.0 % of a Pi 4 core; a 6-note
  chord of 7 oscillators each with the filter ~7.5 %, with table B, BEND and the ladder ~CPU_CHORD_B %; Pi 5 about 2.5x less.

Reference feel: the Elektron Tonverk's wavetable machine (public information only: a wavetable oscillator with
position morphing, warps and unison into a filter); nothing of its code, tables or exact parameter set is used.

## 1. Tables

| source | frames | notes |
|---|---|---|
| built-in BASIC | 64 | sine -> triangle -> saw -> square (their series crossfaded) |
| built-in PWM | 64 | a pulse from 50 % to 4 % |
| built-in VOCAL | 64 | a 110 Hz voice's harmonics under three formants, a -> e -> i -> o -> u |
| built-in ORGAN | 64 | harmonics 1..48 coming in one by one, drawbar-like levels |
| built-in FM | 64 | a sine phase-modulated by its 2nd harmonic, index 0 .. 6 |
| built-in SYNC | 64 | a saw hard-synced at 1..8x |
| a sample row of the WAVETABLES list | up to 256 | any WAV / AIFF the reader takes (below), anywhere on the card; more frames are thinned evenly (first and last kept) |

The WAV reader (`core/wavetable/wav_read.h`, also GRANULAR's): RIFF/WAVE with `fmt ` / `data` / `clm ` anywhere; PCM
8/16/24/32-bit, IEEE float 32/64-bit, `WAVE_FORMAT_EXTENSIBLE`; any channel count (the first two summed); any rate
(a table's frames are cycles: the rate does not matter). A file is at most 8 MB.

Frame size (`tables.h detect_frame_size`, in this order): the `clm ` chunk's `<!>N` when N divides the length; 2048
when it divides; 1024, 512, 256 when one does; a file of at most 8192 samples is one cycle; else 2048 (the rest left
out). A 2048-sample frame keeps its first 512 harmonics (they reach 20 kHz down to 39 Hz).

Each frame's harmonics come from an FFT (any power of two) or a DFT (other lengths); DC is removed; the table is scaled
so its loudest frame peaks at 0.95. Loading runs between blocks (`wt::step()`): one 32 KB card read or one frame's
mip levels a call, so a 256-frame table is in after ~260 steps. A trig while its table loads waits for it (up to
250 ms, else it plays at once on the table the voice had).

### The list (the pools build, 2026-10-22)

TBL (the 14-bit word) is `code = bank * 128 + row` of the WAVETABLES list: 8 banks x 128 rows, row 000 OFF in every
bank, an empty or MISSING row silent. TBL's default is 1 since rui1043 (A001 = BASIC in the template). Rows are filled
from the Rytm (FUNC + YES in every bank: `@BUILT-IN/` and the card's folders; the POOL page), the touchscreen (the
POOL view, FILES, FILL), the web (Files: **+WT**, Fill bank; the Pool tab) or the console (`pool=wt:...`). Card tables
load on use into 4 card slots (least recently used replaced, never one a voice reads); their memory is set aside at
boot and shown apart in the pool RAM figure ("tables"). See docs/POOLS.md.

### Tables from samples (MAKE WT) and encoder B

On a WAVETABLE track's SRC view, **push knob B** (POS: it has no push of its own, and FUNC + push and the other pushes keep
their meaning) to open SAMPLE EDIT: on table A's file when it is a card file, else on the POOL page's SAMPLES list to pick a
sample. In SAMPLE EDIT, **FUNC + YES, MAKE WT** turns the region (START..END) into a table of 16 .. 256 frames (default 64):
a period track with a harmonic analysis per frame and the frames phase-aligned to each other for a pitched sample, windows
made periodic for noise and hits (the screen says `PITCHED` or `SPECTRAL`); the file goes to `/WAVETABLES/<stem>-wt<N>.wav` (a
float32 WAV with the `clm ` 2048 chunk this reader takes), into the WAVETABLES list, onto the track's TBL, and the page goes
back to the SRC view. On the touchscreen the machine view's **EDIT** key does the same. The method, the file and the
limits: docs/SAMPLE-EDIT.md "MAKE WT". A float32 WAV is one the frame-size rules above read as they read any other (2048
when it divides; the `clm ` chunk names it).

## 2. Band limiting

| level k | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| top harmonic | 512 | 256 | 128 | 64 | 32 | 16 | 8 | 4 | 2 | 1 |
| samples | 2048 | 1024 | 512 | 512 | 512 | 512 | 512 | 512 | 512 | 512 |

- A note (each unison oscillator's highest rate, the warp's top rate folded in: SYNC's ratio, PWM's steepest slope,
  FM's 1 + index) reads the first level whose top harmonic x f stays under **0.6 fs** (28.8 kHz): whatever lies over
  Nyquist then aliases above 0.4 fs (19.2 kHz), out of hearing. A note too high even for level 9 is silent.
- Reads are 4-point Hermite with wrap guards stored around each level (no modulo in the loop). Levels 3..9 keep 512
  samples, so the high notes' strong low harmonics are read at 8 samples a harmonic or more and the read's images sit
  near its nulls [measured: with 256-sample levels key 60 aliased at -51 dB, with 512 every key is under -67 dB].
- Memory: 7200 floats (28 KB) a frame; a card slot holds 256 frames (7.4 MB), four slots, the built-ins 11 MB; all
  from the board's high heap (`msamp_ram_alloc`), taken once at boot.

## 3. Parameters

P1..P16 are sound words 9..16 and 17..24 (page 1 A..H, page 2 A..H), each 0..127 with its 1/128 fraction.

| P | name | what | range |
|---|---|---|---|
| 1 | TBL | table A: a row of the WAVETABLES list (A001..A006 the built-ins, then the card's by path) | 14-bit code, OFF = row 000 |
| 2 | POS | the frame position of A (smoothed 10 ms); neighbouring frames crossfaded | 0 first .. 127 last |
| 3 | WARP | the warp, 9 zones (value x 9 >> 7) | OFF SYNC PWM FM FORMANT QUANT BEND SQUEEZE RM |
| 4 | AMT | the warp's amount | per warp, below |
| 5 | UNI | unison: 0 one oscillator; up, pairs fade in (3, 5, 7 oscillators) while the detune opens (both tables) | 0 .. 60 cents (x^1.5) |
| 6 | SUB | a sine an octave down | level 0..0.8 |
| 7 | CUT | DJ: below 64 low-pass, 64 open, above high-pass; 50 % key tracking. The other types: a plain cutoff | DJ: LP 20 Hz .. 20 kHz, HP 20 Hz .. 8 kHz; others 8 .. 136 semitones above C-1 (13 Hz .. 21 kHz) |
| 8 | RES | resonance, and the drive into the filter with it | DJ: 0 .. near self-oscillation, drive 1..4x; others: Vital's laws |
| 9 | ATK | attack (amp and mod envelope) | 1 ms .. 10 s, exponential |
| 10 | DEC | decay to SUS; also the release (a Minimoog's DEC/REL) | 1 ms .. 10 s (-60 dB) |
| 11 | SUS | sustain | 0 .. 1 |
| 12 | ENV | the mod envelope (attack ATK, decay DEC, to 0) onto POS **and POS B** | 64 none, 127 +the whole table, 0 -the whole |
| 13 | TBL B | table B: the same list; row 000 (OFF, the default) is no B | 14-bit code |
| 14 | POS B | the frame position of B | 0 first .. 127 last |
| 15 | MIX | A against B, equal power (cos / sin of 0..90 degrees); with no B, nothing | 0 only A .. 127 only B |
| 16 | FTYPE | the filter, 8 zones (value x 8 >> 7) | DJ LP12 LP24 BP12 BP24 HP12 HP24 LADDER |

The warps (AMT laws; SYNC, PWM and FM keep this engine's, the others are Vital's):

| WARP | AMT 0 .. 127 |
|---|---|
| OFF | none |
| SYNC | the phase read 1x .. 8x faster, restarting each cycle (hard sync) |
| PWM | the cycle's first half stretched: pulse width 50 % .. 4 % |
| FM | phase modulation, index 0 .. 5 rad (AMT squared); the modulator is **table B when B is on**, a sine at the note's rate otherwise |
| FORMANT | sync ratio 1/16 .. 16x (2^((a - 0.5) x 8)), under a half-sine window (a formant, not a sweep) |
| QUANT | the phase quantised into steps: many (AMT 0: none) .. two .. one (AMT 127: the phase stops on the cycle's start, silence: Vital's end of the law; level 0.23 at AMT 60, 0.08 at 110, 0.02 at 120) |
| BEND | the phase bent by a cubic: AMT 64 is the straight phase, below / above push it either way |
| SQUEEZE | the cycle squeezed toward its start (AMT 0) or its middle (AMT 127); AMT 64 is straight. The mip level allows for the squeeze's steepest slope up to 8x the note's rate (a cap: past it the note is brighter than the level, with some aliasing at the extremes of AMT on high notes) |
| RM | the table multiplied in by a window: 1 + (m - 1) x AMT with m the modulator as in FM (table B, or a sine) |

With table B on, the warp acts on A and on B, except FM and RM: there B is the modulator of A, and B is heard unwarped
under MIX. Reading B when MIX is 0 under FM or RM still happens (it modulates), so MIX 0 plus FM is "A modulated by B,
B not heard". B shares the note's unison lanes, rates and pitch, and its own random start phases. B is **wanted** (requested from the card, kept in its slot) only while it can be heard: MIX above 0, or FM / RM; so an old
project's chord words, which read as TBL B rows at MIX 0, claim no card slot, and every voice's A tables load before any
B. The mix is faded: it moves 1/40 a block (10 ms) toward MIX while B is wanted and in, and back to 0 when B goes (B's
table stays 40 ms for that); with TBL B OFF it is exactly A. Under FM / RM B is read at the note's own mip level (plain),
A at the warp's. Old projects' WARP
values land in other zones now (nine zones, not four, see the table); P13..P16 of an old project were
the chord (CHD INV SPR VOI): a CHD value reads as a TBL B row now (CHD 1 = A001 = BASIC as B, MIX 0: nothing audible
until MIX moves; an SPR of 1..2 reads as a MIX of about 0.01, inaudible), INV / SPR are POS B / MIX, VOI an FTYPE zone: they
are all zero on a project without a chord, and those sound as before.

**Old WARP values (0.5.1.1: four zones, zone = (v * 4) >> 7; now nine, zone = (v * 9) >> 7).** The new zones are OFF 0..14,
SYNC 15..28, PWM 29..42, FM 43..56, FORMANT 57..71, QUANT 72..85, BEND 86..99, SQUEEZE 100..113, RM 114..127. Only 0..14 (OFF)
sounds as before; the AMT laws of SYNC, PWM and FM are unchanged.

| old WARP value | old zone | now |
|---|---|---|
| 0..14 | OFF | OFF (same) |
| 15..28 | OFF | SYNC |
| 29..31 | OFF | PWM |
| 32..42 | SYNC | PWM |
| 43..56 | SYNC | FM |
| 57..63 | SYNC | FORMANT |
| 64..71 | PWM | FORMANT |
| 72..85 | PWM | QUANT |
| 86..95 | PWM | BEND |
| 96..99 | FM | BEND |
| 100..113 | FM | SQUEEZE |
| 114..127 | FM | RM |

**Release note (0.5.2):** WAVETABLE v2: old WARP settings above 14 sound different (the table above); chords are gone (a sound
with a chord plays its root); P13..P16 are now TBL B / POS B / MIX / FTYPE (p-locks on them lock those now); the Rytm shows the
old labels for them until its 0.5.2 mod.

The filters (FTYPE): DJ is the engine's own (CUT as before: a project with FTYPE 0 sounds as before, sample for sample
with B off). The others are Vital's: a digital state-variable filter (12 dB, or two stages for 24 dB: LP, BP, HP by blend;
resonance 0.5 .. 16 Q by RES cubed, drive compensated) and its ladder (4 stages, LP). CUT is semitones: 8 + CUT x 128/127
above C-1, plus 50 % key tracking (0.5 semitone a semitone from C4) plus the **filter envelope** (the mod envelope x
FENV x 64 semitones). The oscillators play into them at half scale (Vital's levels: hardTanh's linear range), the level
made up after, so the passband keeps the DJ filter's. The 24 dB HP's first stage is Vital's blend through a resonance-1
stage, so it falls 18 dB / octave below its cutoff (its 24 dB shows above and in the cascade).

Decisions: page 1 is the oscillator and filter (TBL POS WARP AMT UNI SUB CUT RES, as asked); page 2 is the amp envelope
ATK DEC SUS (DEC also the release), the mod envelope's depth ENV onto both POS, and TBL B, POS B, MIX, FTYPE where the
chord was (owner, 2026-11-17: "chords we can already do from a poly track"). The LFO is the Sidecar's own (`core/lfo.h`):
any of P1..P12 can be its target, POS above all (P13..P16 are not LFO targets: the PAR list stops at P12 on a WAVETABLE voice; their labels are TBLB POSB MIX FTYP). Velocity: 0.25 + 0.75 v^2.

**The filter envelope's depth has no knob and no store.** The brief put it on the touchscreen's WAVETABLE page, but the
project keeps no per-track, per-machine store for a touch-only setting (the stores are the LFO / INSERT / XY / scene
files, each its own kind; none takes a machine's number). So it is the engine's `fenv` per voice, 0 at boot, set from
the console (`wt=fenv:V:PERCENT`, -100..100, 100 % = +64 semitones at the envelope's peak) and **not saved**; with 0 the
new filters have no envelope. A store for it (a line in a kit's record) is a decision for a later task.

## 4. Voice

```
note -> 7 phase accumulators (unison detune, own random phases; the centre one starts at 0), once for table A and
        once for table B (B only when it is on and MIX or the warp needs it)
     -> warp of the phase (SYNC / PWM / FM / FORMANT / QUANT / BEND / SQUEEZE / RM; FM and RM modulate A by B's lane,
        or by a sine at the note's rate while B is off)
     -> Hermite read of frames A and B' at the level, each table's frame pair crossfaded by its POS (+ ENV x the mod env)
     -> A x cos(MIX) + B x sin(MIX) -> sum x 1/sqrt(sum w^2) -> DC blocker (4 Hz)
     -> + SUB -> DJ: drive -> SVF (LP / HP)     |  the others: x 0.5 -> SVF 12 / 24 dB or ladder -> x 2
     -> x amp envelope x velocity x 0.5
voice: the notes summed x the headroom gain 1/sqrt(sum of envelopes) (as MULTISAMPLE's, smoothed 3 / 15 ms)
     -> voices.cpp's soft limiter (soft_out) -> the voice's Overbridge return
```

- Notes: 8 a voice, 32 in all; steal: a free note, then the voice's oldest released, then its oldest. A trig releases
  the voice's earlier trig notes (their tails ring) and starts the chord; 0x11 releases them; live pad notes (a bound
  MIDI channel) and the sequencer's (channel 16) are held until their own note-off.
- Cores: the control core posts a note's start / release / kill through a sequence-locked mailbox (as msamp's); the
  render core reads the voice's tables A and B through a hazard slot each (`sel` / `use`, `selb` / `useb`), so a table a
  render core reads (either one) is never reloaded under it, and a card slot a voice's B holds is not given to another
  table. A B that is not in yet is silent (A plays alone) until it loads. No allocation after `begin()`.
- Chords: the live pads' chord detection writes the Rytm track's chord words (a 28 01) for 6-OP FM and the sample
  machines; for WAVETABLE it does not (P13..P16 would be overwritten): the pads' notes play as notes. The sequencer's
  chord steps, the pads and the keyboards reach the voice as `live_on` notes (`poly::seq_on`, `key_on`).

## 5. CPU

`./core/build/render_wavegrain --bench` runs the Pi's boot bench (`pi/engine/bridge.cpp bench`: voice 0, note 48, a
retrigger on 55, 1 s of audio) on the Mac; the Pi 4's measured numbers for the other machines (`docs/pi-engine.md`
"Boot bench") give the ratio, **4.3x** in this run (2026-11-17: the Mac was busy with other builds; the first version
of this table, 2026-10-20, had 8.1..8.4x on a quieter Mac: the Mac's figures are 1.7x those of then for the same code,
the Pi estimates the same) [measured: Mac; inferred: Pi]. A block is 12 frames (250 us); a row is the best of five runs of
40000 blocks, one voice, all notes held.

| case | Mac (% of a core) | Pi 4 1.5 GHz (estimate, x 4.3) | Pi 5 (estimate, ~2.5x less) |
|---|---|---|---|
| bench note (`notes.cpp` wt: 3 osc, SYNC, sub, LP) | 0.18 % | ~0.8 % | ~0.3 % |
| 1 note, 1 osc | 0.07 % | ~0.3 % | ~0.12 % |
| 1 note, 7 osc + FM (sine) + DJ filter (table B off) | 0.27 % | ~1.2 % | ~0.5 % |
| 6-note chord, 7 osc each + FM + DJ filter (B off) | 1.6 % | ~6.9 % (5..11 %) | ~2.8 % |
| 1 note, 7 osc, B on, no warp, LP24 | 0.30 % | ~1.3 % | ~0.5 % |
| 1 note, 7 osc, A only, BEND, LP12 | 0.41 % | ~1.7 % | ~0.7 % |
| 1 note, 7 osc, B on, FM by B, LP24 | 0.56 % | ~2.4 % | ~1.0 % |
| 1 note, 7 osc, B on, BEND, LADDER | 0.77 % | ~3.3 % | ~1.3 % |
| 6-note chord, 7 osc each, B on, no warp, LP24 | 1.7 % | ~7.4 % | ~3.0 % |
| 6-note chord, 7 osc each, B on, BEND, LADDER (the worst case) | 4.5 % | ~19 % | ~7.7 % |

With table B off the engine costs what it did (the same session's old build: 0.07 / 0.26 / 1.52 %: within 5 %, the
noise of a shared Mac; the code paths are the old loops, unchanged); B on costs roughly its own lanes again, and the
new warps' and filters' per-lane work (the phase warps run per lane and sample; the ladder is four saturating stages).
SYNC and OFF with B on use the NEON read for both tables. The worst case, a six-note chord of seven-oscillator
unison on two tables with the ladder, is a fifth of a Pi 4 core.

The Pi's bench runs `wt` and `gr` too (`pi/engine/cores.cpp`, `pi_run.py bench`): its boot log has the measured numbers
once a kernel with this branch boots. Boot: the built-ins take 24 ms on the Mac (~0.2 s on a Pi 4).

## 6. Console and the Rytm side

- `wt` stats; `wt=voice:V` / `wt=voice:V:off` (the override); `wt=list` (the tables in the log); `wt=rescan` (failed
  tables tried again); `wt=pick:V:ROW` (writes the Rytm's TBL through 48); `wt=pickb:V:ROW` (the same for table B: the Rytm's TBLB, P13, 48 with k = 12); `wt=fenv:V:PERCENT` (the filter envelope's
  depth, -100..100, not saved). The list itself: `pool=wt:...` (docs/POOLS.md).
- Rytm mod (rytm-mods 0021 + 0026 + 0029, not in this branch): a machine slot reported with **0x28** in the voice byte
  (one of 0021's free hidden machines: `us_flag_tab` entries that are 0 now, machines 43 / 46..50; e.g. 46 -> 0x28, and
  0026's `mr_flag` the same), name `WAVETABLE` / short `WTBL`. Its SRC knobs P1..P8 TBL POS WARP AMT UNI SUB CUT RES
  (long: Table, Position, Warp, Amount, Unison, Sub Osc, Cutoff, Resonance; looks: dial, dial, list, dial, dial,
  level, bipolar, dial; defaults 1 0 0 0 0 0 64 0 since rui1043, TBL was 0), page 2 ATK DEC SUS ENV (long: Attack,
  Decay, Sustain, Env>Pos; looks: dial, dial, dial, bipolar; defaults 0 64 100 64: as before) **then TBLB POSB MIX FTYP
  in place of CHD INV SPR VOI**. What the mods task has to change, exactly (the Rytm shows the old labels until then):
  - **Names**: P13 `TBLB` (long `Table B`), P14 `POSB` (long `Position B`), P15 `MIX` (long `Mix A-B`), P16 `FTYP` (long
    `Filter Type`). Looks: P13 list-like as P1 (the host's row names: 47 with the bank byte, `OFF` at row 000, `A005`
    for an empty row), P14 dial, P15 dial, P16 list. Defaults **0 0 0 0** (`kPageTwo` in `core/ui/remote.cpp` already
    writes 0 64 100 64 / 0 0 0 0 for WAVETABLE when it is chosen: no change there; 0026's `cu_wtp2k` page-2 record: the
    last four words 0).
  - **P13 is a 14-bit row word** like P1: the bank rides in the value's fraction (`srclib::code_of`: bank * 128 + row,
    `lo_of`), so 0026's TBL handling (the bank byte of 47 / 48, the list view that opens the WAVETABLES list, the
    pick that writes the knob) has to serve P13 as well as P1. The host reads it as it reads P1 (`select_b`). The host's
    side of the TABLE list for P13 (2026-11-24, docs/core.md "The P13 list"): `4A` carries an optional 6th byte (12 = P13)
    and its YES writes P13; the host announces `4A 7D 1` after each hello, only to a Rytm whose hello's caps2 has bit 5 (32);
    the names bank (47) follows whichever of P1 / P13 was touched last. The touchscreen's pick for B is `wt::pick_b`
    (`wt=pickb`): the pool list's middle key picks the target, A or B (every open starts on A); a 2A for P13 on an idle voice
    selects table B as P1's does (`wt::kit_src_b`).
  - **B is silent at MIX 0** unless WARP is FM or RM (`select_b`: it is wanted, loaded and played only while it can be heard):
    a table picked for B at MIX 0 is stored and does not sound. The touchscreen says so after such a pick ("B IS SILENT:
    MIX IS 0", a few seconds).
  - **WARP** (P3), value texts for the **nine zones**, zone = (v * 9) >> 7: `OFF SYNC PWM FM FORMANT QUANT BEND SQUEEZE
    RM` (a four-letter set if the display wants one: `OFF SYNC PWM FM FRMT QNT BEND SQZE RM`). 0029's stepped list
    knob for WARP: 9 zones (was 4).
  - **FTYP** (P16), **eight zones**, zone = (v * 8) >> 7: `DJ LP12 LP24 BP12 BP24 HP12 HP24 LADDER` (`LDR` if short).
    A stepped list knob (0029). The CUT knob (P7) is a bipolar dial for DJ (LP below 64, HP above) and a plain cutoff
    for the others: its text (the Rytm's dial) can stay the same; there is no way for the Rytm to know the type from P7.
  - **Chord**: CHD INV SPR VOI leave WAVETABLE: no chord texts (`cu_chd`...), no chord view, no 28 01 write-back for
    this machine (the Pi does not send one for it, `poly::close_window`). The other machines keep theirs.
  - **MIX** text: 0 `A` .. 127 `B` is enough (the law is equal power), or the plain number.
  - **P13..P16 are not LFO targets** (the LFO's targets stay P1..P12).
  - An old project's chord words (P13..P16 of a WAVETABLE sound) now read as TBL B, POS B, MIX, FTYPE: a sound with
    no chord (all four 0) is as before; one with a chord plays its root alone (B is silent while MIX is 0), and the
    project's WARP value is read in nine zones.

## 7. Files

| part | file |
|---|---|
| WAV reader | `core/wavetable/wav_read.h` |
| harmonics, mip levels, frame sizes, built-ins | `core/wavetable/tables.{h,cpp}` |
| the machine | `core/wavetable/wavetable.{h,cpp}` |
| the note machines' dispatch (MULTISAMPLE, WAVETABLE, GRANULAR) | `core/nmach.h` |
| remote UI page | `core/ui/page_wavegrain.cpp` |
| tests, demo | `core/tests/test_wavetable.cpp`, `core/tests/wg_util.h`, `core/tests/demo/render_wavegrain.cpp` (`make -C core demo-wavegrain OUT=dir`, `--bench`, `--warps DIR`: the new patches' phrases) |

Shared files touched (small hooks): `core/machines.h` (flags), `core/voices.{h,cpp}` (V's `wt` `gr`, report / note-off /
silence / render through `nm::`), `core/poly.cpp` (live / sequencer notes through `nm::`; no chord write-back for a
WAVETABLE voice), `core/engine.cpp` (begin,
idle step), `core/notes.cpp` (`wt` `gr` for the bench and MIDI notes), `core/ui/remote.cpp` (machine names, the
override), `core/ui/page_seq.cpp`, `core/ui/page_gfx.cpp` + `extra.h` (the pages), `core/host/core_c.cpp`,
`core/Makefile`, `pi/engine/Makefile`, `pi/engine/bridge.{h,cpp}` (console, live input), `pi/engine/ob_usb.cpp` (the IN
stream to GRANULAR), `pi/engine/cores.{h,cpp}` (bench), `pi/engine/web.cpp` (rescan), `pi/engine/tests/pi_run.py` and
`test_pi_run.py` (bench machines), `core/tests/test_notes.cpp`.

## 8. Tests [measured]

`core/tests/test_wavetable.cpp` (in `make -C core test`): the reader (16/24/32/8-bit PCM, float, stereo, extensible,
`clm `, a data chunk past the file's end, refusals); frame size detection and thinning; harmonics (FFT and DFT) and
every level (nothing over its top harmonic, the rest kept, the guards); the WAVETABLES list (built-ins and files as rows), a trig waiting for its table, frames loaded as written, `clm ` 256, a 600-sample cycle, a broken
file refused; the morph (a POS sweep's steepest step no more than the steepest frame's own; halfway between two frames
both at half); aliasing (a C7 saw -67 dB vs -14 dB without the levels, C2..C8 under -60 dB); the warps; unison (0.86 of
the fundamental's energy spread off it, the level within 0.6..1.6x); chords and the headroom; live and sequencer notes
and `wt=voice:V`; golden renders (pluck, pad, sync, vocal: rms, mean |x| and zero crossings within 1e-4 / 1e-3 of what
the engine rendered before TBL B, the new warps and filters, with B off and the DJ filter: sample for sample, nine
patches compared as 32-bit floats); the CPU.
Table B and the new warps and filters (the same file): a trig is one note (an old chord word is a TBL B row); a poly
track's chord (`poly::seq_on` MAJ, `key_on` MAJ7: 3 and 7 notes, the headroom of a chord); MIX's law (A's share of the
energy cos^2, the level constant within 3 %, 0 / 32 / 64 / 96 / 127 against a table of single harmonics); POS B; ENV on
both POS; B's aliases (a saw at C2..C8, B alone and mixed: under -60 dB); B's unison; the five new warps change a saw,
BEND 63.5 and SQUEEZE 63.5 are the straight phase; FM / RM by B against by a sine, B plain under them; **every warp x
every filter type x both ends of the knobs x B on and off** (288 renders): finite, under 4, silent after the release; the
new filters against the design (a sine at each key: LP12 LP24 BP12 BP24 HP12 HP24 within 0.1 dB of the digital
response of Vital's blends over 16..25 keys, their slopes 11.8 / 24.3 / 11.9 / 18.2 dB per octave against the design's,
the band-passes' peak at f / fc 0.98, the LP's level at its cutoff 0.50 (Q 1/2) and RES 90 raising it to 2.6, the
ladder -23.5 dB per octave and 0.94 of the level far below its cutoff); the filter envelope (FENV +1: 3x the level at
10..50 ms of a low-cut saw; 0: none); the card slots of two tables (a slot a voice's B reads is not reloaded, goes back once B leaves it).
`core/tests/test_poly.cpp`: the pads' chord on a WAVETABLE voice plays and writes nothing back to the Rytm's track.
`core/tests/test_notes.cpp` plays `wt` through the MIDI note adapter and checks CC 120's silence.

## 9. Licence of the new warps and filters

FORMANT, QUANT, BEND, SQUEEZE, RM and the filters LP / BP / HP 12 / 24 dB and LADDER (`core/wavetable/wavetable.cpp`:
`phase_warp`, `warp_pf`, `filter_targets`, `svf_tick`, `ladder_tick`, the tanh / hardTanh helpers) are derived from Vital
by Matt Tytel (https://github.com/mtytel/vital, commit 636ca0e, GPL-3.0-or-later; Copyright 2013-2019 Matt Tytel), ported
to float C and checked against the original's C++ (its phase warps, digital SVF and ladder). They are named by what
they do; the original's names are not used for anything here. The file as a whole is GPL-3.0-or-later (SPDX line). The Pi binaries are distributed under GPL-3.0-or-later
(Circle is too); NOTICE, THIRD_PARTY_LICENSES and README say so. No wavetable or preset of Vital is included.
