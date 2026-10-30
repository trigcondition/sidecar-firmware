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
- Per note: up to 7 unison oscillators (stereo summed to the voice's mono return), a warp (SYNC, PWM, FM), a sub sine,
  a DJ-style SVF (low-pass below CUT 64, high-pass above) with drive, an amp ADSR and a mod envelope on POS. Chords on
  P13..P16 as MULTISAMPLE's / 6-OP FM's; trigs, the Sidecar sequencer and the live pads / keys all play it.
- CPU [measured: Mac; inferred: Pi]: the boot bench's note (3 oscillators, SYNC) ~1.0 % of a Pi 4 core; a 6-note
  chord of 7 oscillators each with the filter ~7.5 %; Pi 5 about 2.5x less.

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
| 1 | TBL | the table: 0..5 built-ins, then the card's by path | min(TBL, count - 1) |
| 2 | POS | the frame position (smoothed 10 ms); neighbouring frames crossfaded | 0 first .. 127 last |
| 3 | WARP | the warp, 4 zones | OFF SYNC PWM FM |
| 4 | AMT | the warp's amount | SYNC 1..8x, PWM width 50 % .. 4 %, FM index 0..5 (squared) |
| 5 | UNI | unison: 0 one oscillator; up, pairs fade in (3, 5, 7 oscillators) while the detune opens | 0 .. 60 cents (x^1.5) |
| 6 | SUB | a sine an octave down | level 0..0.8 |
| 7 | CUT | DJ-style SVF: below 64 low-pass, 64 open, above high-pass; 50 % key tracking | LP 20 Hz .. 20 kHz, HP 20 Hz .. 8 kHz |
| 8 | RES | resonance, and the drive into the filter with it | 0 .. near self-oscillation, drive 1..4x |
| 9 | ATK | attack (amp and mod envelope) | 1 ms .. 10 s, exponential |
| 10 | DEC | decay to SUS; also the release (a Minimoog's DEC/REL) | 1 ms .. 10 s (-60 dB) |
| 11 | SUS | sustain | 0 .. 1 |
| 12 | ENV | the mod envelope (attack ATK, decay DEC, to 0) onto POS | 64 none, 127 +the whole table, 0 -the whole |
| 13..16 | CHD INV SPR VOI | the chord (`core/poly.h`) | as MULTISAMPLE / 6-OP FM |

Decisions: page 1 is the oscillator and filter (TBL POS WARP AMT UNI SUB CUT RES, as asked); page 2 has only four free
knobs before the chord's, so the amp envelope is ATK DEC SUS with DEC also the release, and the fourth is ENV, the
mod envelope's depth onto POS (the wavetable's signature move). The filter has LP and HP on one knob rather than a
type switch (no knob left; one knob also sweeps smoothly through open). The LFO is the Sidecar's own (`core/lfo.h`):
any of P1..P12 can be its target, POS above all. Velocity: 0.25 + 0.75 v^2.

## 4. Voice

```
note -> 7 phase accumulators (unison detune, own random phases; the centre one starts at 0)
     -> warp (SYNC / PWM / FM: a sine modulator at the note's rate) -> Hermite read of frames A and B at the level
     -> crossfade by POS (+ ENV x the mod envelope) -> sum x 1/sqrt(sum w^2) -> DC blocker (4 Hz)
     -> + SUB -> drive -> SVF (LP / HP) -> x amp envelope x velocity x 0.5
voice: the notes summed x the headroom gain 1/sqrt(sum of envelopes) (as MULTISAMPLE's, smoothed 3 / 15 ms)
     -> voices.cpp's soft limiter (soft_out) -> the voice's Overbridge return
```

- Notes: 8 a voice, 32 in all; steal: a free note, then the voice's oldest released, then its oldest. A trig releases
  the voice's earlier trig notes (their tails ring) and starts the chord; 0x11 releases them; live pad notes (a bound
  MIDI channel) and the sequencer's (channel 16) are held until their own note-off.
- Cores: the control core posts a note's start / release / kill through a sequence-locked mailbox (as msamp's); the
  render core reads the voice's table through a hazard slot (`sel` / `use`), so a table a render core reads is never
  reloaded under it. No allocation after `begin()`.

## 5. CPU

`./core/build/render_wavegrain --bench` runs the Pi's boot bench (`pi/engine/bridge.cpp bench`: voice 0, note 48, a
retrigger on 55, 1 s of audio) on the Mac; the Pi 4's measured numbers for the other machines (`docs/pi-engine.md`
"Boot bench") give the ratio, 8.1..8.4x (5.9..12.3x across machines) [measured: Mac 2026-10-20; inferred: Pi].

| case | Mac (% of a core) | Pi 4 1.5 GHz (estimate) | Pi 5 (estimate, ~2.5x) |
|---|---|---|---|
| bench note (`notes.cpp` wt: 3 osc, SYNC, sub, LP) | 0.12 % | ~1.0 % | ~0.4 % |
| 1 note, 1 osc | 0.04 % | ~0.3 % | ~0.15 % |
| 1 note, 7 osc + FM warp + filter | 0.16 % | ~1.3 % | ~0.5 % |
| 6-note chord, 7 osc each + filter | 0.90 % | ~7.5 % (5..11 %) | ~3 % |

The Pi's bench now runs `wt` and `gr` too (`pi/engine/cores.cpp`, `pi_run.py bench`): its boot log has the measured
numbers once a kernel with this branch boots. Boot: the built-ins take 24 ms on the Mac (~0.2 s on a Pi 4).

## 6. Console and the Rytm side

- `wt` stats; `wt=voice:V` / `wt=voice:V:off` (the override); `wt=list` (the tables in the log); `wt=rescan` (failed
  tables tried again); `wt=pick:V:ROW` (writes the Rytm's TBL through 48). The list itself: `pool=wt:...`
  (docs/POOLS.md).
- Rytm mod (rytm-mods 0021 + 0026, not in this branch): a machine slot reported with **0x28** in the voice byte (one
  of 0021's free hidden machines: `us_flag_tab` entries that are 0 now, machines 43 / 46..50; e.g. 46 -> 0x28, and
  0026's `mr_flag` the same), name `WAVETABLE` / short `WTBL`. Its SRC knobs P1..P8 TBL POS WARP AMT UNI SUB CUT RES
  (long: Table, Position, Warp, Amount, Unison, Sub Osc, Cutoff, Resonance; looks: dial, dial, list, dial, dial,
  level, bipolar, dial; defaults 1 0 0 0 0 0 64 0 since rui1043, TBL was 0), page 2 ATK DEC SUS ENV + CHD INV SPR VOI (long: Attack, Decay,
  Sustain, Env>Pos, then as MULTISAMPLE's; looks: dial, dial, dial, bipolar, then 0026's chord texts; defaults 0 64 100
  64 0 0 0 0). Value texts: WARP OFF SYNC PWM FM (4 zones), CHD..VOI as 0026's `cu_chd` etc.; TBL the host's row names
  (47 with the bank byte, `OFF` at row 000, `A005` for an empty row; rui1043: bank A like B..H, default 1). 0029's stepped list
  knobs: WARP 4 zones, the chord knobs as MULTISAMPLE's.

## 7. Files

| part | file |
|---|---|
| WAV reader | `core/wavetable/wav_read.h` |
| harmonics, mip levels, frame sizes, built-ins | `core/wavetable/tables.{h,cpp}` |
| the machine | `core/wavetable/wavetable.{h,cpp}` |
| the note machines' dispatch (MULTISAMPLE, WAVETABLE, GRANULAR) | `core/nmach.h` |
| remote UI page | `core/ui/page_wavegrain.cpp` |
| tests, demo | `core/tests/test_wavetable.cpp`, `core/tests/wg_util.h`, `core/tests/demo/render_wavegrain.cpp` (`make -C core demo-wavegrain OUT=dir`, `--bench`) |

Shared files touched (small hooks): `core/machines.h` (flags), `core/voices.{h,cpp}` (V's `wt` `gr`, report / note-off /
silence / render through `nm::`), `core/poly.cpp` (live / sequencer notes through `nm::`), `core/engine.cpp` (begin,
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
and `wt=voice:V`; golden renders (pluck, pad, sync, vocal: rms and zero crossings within 10 %); the CPU.
`core/tests/test_notes.cpp` plays `wt` through the MIDI note adapter and checks CC 120's silence.
