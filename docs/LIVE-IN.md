# LIVE IN: the Rytm's AUDIO IN through a Sidecar voice

A Sidecar machine that plays the Rytm's **AUDIO IN** (a guitar through a DI, a synth, a mic preamp) through the voice's
own **INSERT** chain (AMP / NAM, DRIVE, EQ, COMP, ...: docs/INSERT-FX.md), its **sends** (SPACE and the other send FX,
the buses) and its **OUT** route (TRK / PRE / POST, stereo: docs/STEREO.md). The use case: a guitar into the Rytm's AUDIO
IN, through an amp capture and the tape echo, on the Rytm's mix and its sequencer's trigs. Pi 4 / Pi 5 and the Mac tests
(`CORE_MSAMP`); the Sidecar Mini (Teensy) has no INSERT chains and no LIVE IN.

Tags as in the other docs: **[measured]** run on the Mac (or the hardware) and recorded; **[code]** read in the source;
**[inferred]** follows from those, not run on the real Rytm; **[open]** to be measured on the real Rytm.

## 1. For players

### Choosing it

Machine flag **0x78** (`core/machines.h` `kLiveMachine`), named **LIVE IN** (short **LIVEIN**). On the Rytm from rytm-mods
rui1051: the machine list's **LIVE IN** (machine 48, after GRANULAR), its knobs labelled as below (section 4). On an older
Rytm build:

```
livein=voice:V        voice V (0..7) plays LIVE IN, whatever built-in Sidecar machine the Rytm reports for its track
livein=voice:V:off    back to the track's own machine
```

Put a Sidecar machine on the track (PLAITS is fine: its SRC page gives P1..P8, its second page P9), then `livein=voice:V`
(the console, or the web console). The voice plays at once: no trig is needed in MON ALW.

### The knobs (the track's SRC page, P1..P8, and P9)

The Rytm's own SRC page still shows the reported machine's labels (the host cannot rename a built-in machine's labels
there); the remote UI's **LIVE IN** page (the SRC view on the Rytm's screen, and the touchscreen's voice page) draws them
as below. The knob positions are the same.

| knob | label | range | what it does |
|---|---|---|---|
| P1 (A) | **INPT** | L / R / L+R / STR (4 zones) | which input: AUDIO IN L, R, their mono sum (L + R) / 2, or STR: the stereo pair (on a PRE / POST route; on TRK, the mono sum) |
| P2 (B) | **GAIN** | -24..+24 dB, 64 = 0 dB | the input's level (into the gate and the INSERT chain) |
| P3 (C) | **GATE** | 0 OFF, 1..127 = -80..-20 dBFS | a noise gate after GAIN: open above the threshold, closing 3 dB under it |
| P4 (D) | **HOLD** | 0..1 s (quadratic) | how long the gate stays open after the input falls under it; it then closes over HOLD / 4 + 5 ms |
| P5 (E) | **MON** | ALW / TRIG (2 zones) | **ALW**: the input passes all the time (an effects lane). **TRIG**: a trig opens an envelope on it (the Octatrack thru-machine way: rhythmic gating) |
| P6 (F) | **ATK** | 0.5 ms..2 s | TRIG's attack |
| P7 (G) | **DEC** | 5 ms..5 s (to -60 dB) | TRIG's decay towards SUS |
| P8 (H) | **SUS** | 0..127 | the level TRIG's envelope holds while the note is held; **0: a one-shot** (ATK + DEC, no note-off needed: the Rytm's trigs) |
| P9 | **REL** | 5 ms..5 s | TRIG's release after the note-off (the Sidecar sequencer's notes, a Rytm trig's 0x11) |

TRIG's trigs: the Rytm's trigs on the track, the Sidecar sequencer's notes (its mono path: E0, a poly config of any
VOICES plays one), the pads bound to the voice. Velocity and pitch are not used. The output level is the route's own:
the Rytm's AMP / VOL on TRK, OUT LEV on PRE / POST, the INSERT AMP's LEV.

**On TRK** the voice goes through the Rytm's track: its filter, overdrive, **AMP envelope** and sends. The Rytm passes a
voice's Overbridge input only while that envelope is open (docs/overbridge-voice-inputs.md: the VCA must be triggered),
so on TRK the Rytm's own AMP page already gates it at the track's trigs: **MON ALW + the track's AMP** (ATK / HLD / DEC)
is the Octatrack-thru style there, and a continuous effects lane needs the AMP held open (a long hold on a trig). **On
PRE / POST** there is no Rytm AMP: MON ALW is a continuous lane, MON TRIG the gate.

### Signal flow

```
AUDIO IN L / R (Overbridge IN 10 / 11) -> INPT -> GAIN -> GATE (HOLD) -> MON (ALW, or TRIG's ATK DEC SUS REL)
  -> INSERT A -> INSERT B (the voice's chain) -> the limiter (-1 dBFS) -> the feedback guard
  -> OUT: TRK (the Rytm's track, mono) / PRE / POST (a pair; STR keeps L and R)
  -> the voice's sends (bus 1 = send FX 1, e.g. SPACE; bus 2; bus 3), and on PRE / POST the track's FX1 / FX2 sends
```

The INSERT chain is mono on TRK and stereo on PRE / POST; the AMP (NAM) runs on (L + R) / 2 and puts its output on both
sides (docs/INSERT-FX.md), so INPT STR through an AMP comes out centred; DRIVE, EQ, FILTER, COMP keep the pair.

### STATUS and the console

The touchscreen's STATUS page shows a LIVE IN voice as **`LIVE -18DB`** (the input's peak after GAIN over the last
0.3 s), **`LIVE GATED`** (the gate is shut), **`LIVE NO IN`** (no AUDIO IN frames in 0.2 s: the Rytm is not streaming)
or **`LIVE MUTED`** (the feedback guard muted it). The console:

```
livein                 the AUDIO IN channels, frames in; per LIVE IN voice: the input level, the gate, every knob's value,
                       the round trip (measured, or the estimate and its parts), underruns, skips, limited blocks,
                       the guard's probes and trips
livein=voice:V[:off]   the override above
livein=measure[:V]     measure the round trip (below); livein=measure:stop
```

## 2. Latency

The round trip from a sound at the Rytm's AUDIO IN jack to the same sound processed at the Rytm's outputs:

```
AUDIO IN ADC -> the Rytm -> USB IN (Overbridge blocks of 7 frames)
  -> the Pi's IN completion (core 0) writes LIVE IN's ring -> the voice's render reads the newest block (core 1 / 2)
  -> the output FIFO (target 96 frames on USB: docs/pi-engine.md "FIFO target") -> USB OUT
  -> the Rytm's voice input (TRK) or its returns (PRE / POST) -> DAC -> the outputs
```

**The estimate** (what `livein` shows before a measure): the Rytm's own OUT -> IN loop, **289 frames** [measured
2026-10-15 on the Rytm: a voice's onset as sent in OUT, back in MAIN on IN, the FIFO not included] + the FIFO target
(**96**) + the read point's mean backlog (0..12 frames in steady state [measured in the Mac model]) = **385..397 frames,
8.0..8.3 ms** [inferred: the Rytm's AUDIO IN -> USB path and its voice input -> output path are assumed to cost what its
voice input -> MAIN -> USB loop costs; both have a converter pair and the Rytm's 32-frame blocks]. In the emulator link
the FIFO is 512 frames (the link's buffer): `livein` there estimates 17.3 ms (289 + 512 + 30), and `sim/livein_e2e.py`
measures **761 frames, 15.9 ms** from a click on the emulated AUDIO IN to the same click in BD's stem after LIVE IN and
the DRIVE (18 of 18 clicks within 1 frame) [measured, rytm-emu; the emulator's own loop is not the Rytm's 289].

What the Sidecar adds over the Rytm itself is the FIFO plus at most a block: the voice reads its input with no extra
buffering (the newest 12 frames each block; a short read pads silence and counts an underrun; a backlog that stays over
24 frames for a second is cut back to the newest block). `fifo=72` (cmdline / console) takes 0.5 ms off at the risk of
underflows (docs/pi-engine.md measured 72 as the smallest all-PASS target).

**Measuring it** (`livein=measure`): with the hardware, the user's steps:

1. Patch the Rytm's **MAIN OUT L** (or the headphones' L) to its **AUDIO IN L** with a cable. Turn the main volume to
   about 9 o'clock (the clicks are -9 dBFS; the input must not clip).
2. Make the voice LIVE IN (`livein=voice:V`) and set its route to **POST** (`out=V1:post` for voice 0: the simplest;
   on TRK the track's AMP must be open, as above). Keep GAIN at 0 dB and the gate OFF; nothing else playing.
3. `livein=measure:V`. The voice's own sound mutes for ~3 s: 0.1 s of the input's floor, then 5 clicks (1 kHz, 2 ms,
   -9 dBFS) 0.5 s apart, each one found again on AUDIO IN (the first sample over 8 x the floor).
4. The log, then `livein`, give the median: `livein: voice V round trip N frames (X.X ms), 5 of 5 clicks`. Three clicks
   at least within 1 ms of each other, else it says the measure failed (and how many came back).
5. Undo the cable before playing (else the guard below will mute the voice).

The measured value is the whole round trip in the voice's own time base (its render frames): the Pi's FIFO, USB both
ways, the Rytm's buffers and converters - what a player hears between the string and the speaker, minus the speaker's
air. It replaces the estimate in `livein` and the STATUS line until the next boot.

## 3. Feedback guard

If the voice's output reaches AUDIO IN (the main out patched back to the input, a mixer's aux loop, an Overbridge
routing that loops), the loop can howl. The guard [measured in the Mac model, `test_livein`]:

- **A hard limiter** on the voice after its INSERT chain, always: -1 dBFS of the voice's return, instant attack, 50 ms
  release. A runaway never goes past it.
- **Detection**: in 50 ms windows, the input (after GAIN) rising window over window (4 in a row, 200 ms) or sitting near
  full scale (over -6 dBFS for 300 ms) while the voice sounds. That alone is also a crescendo or a strum, so it only
  starts a **probe**: the voice's output dips 6 dB for 30 ms (after the limiter); the input is watched in 5 ms windows for
  200 ms. A loop shows the same 30 ms dip, the same distance after the probe (the loop's delay), twice in a row (a second
  probe confirms the first's place: a strum's own dips do not line up). Confirmed: **muted** for 2 s (10 ms fade), then
  back over 0.5 s; the log says `livein: voice V feedback - the input follows the voice ...: muted 2 s`, STATUS says
  `LIVE MUTED`. A loop still there builds up again and is muted again. A probe that finds no loop leaves the voice alone
  for 3 s.
- In the model: a loop at x 2 gain, 420 frames: muted after 0.6 s, the peak held at the limiter's ceiling, 2 trips in
  6 s; a -40 -> -3 dBFS swell and an 8-a-second strum at -1 dBFS: probes, no trip.

**Warning for players:** do not monitor the Rytm's outputs into its own AUDIO IN. The guard stops a howl within a
second, but the first part of it is loud (up to -1 dBFS).

## 4. The Rytm side (rytm-mods rui1051)

Built in rui1051 as described here (machine 48 = LT TEST, its engine entry ext_render, its SRC page MULTISAMPLE's shared
records set up by 0026 for it, page 2 the shared records with REL on P9; the defaults of both pages written at the
choice); `rytm-mods tools/emu/cu_emu.py li` checks the labels, the defaults, the 0x04's voice byte 0x78 and INPT / MON's
zones, `sim/rui_e2e.py chopmach` the list's entry against the real core [measured, rytm-emu 2026-10-24]. The text below
is the design it follows.

- rytm-mods 0021: a machine slot reported with **0x78** in the voice byte (the last free flag; WAVETABLE took 0x28,
  GRANULAR 0x38, MULTISAMPLE 0x30), name `LIVE IN` / short `LVIN`, its SRC knobs P1..P8 INPT GAIN GATE HOLD MON ATK DEC
  SUS (long: Input, Gain, Gate, Hold, Monitor, Attack, Decay, Sustain; looks: list, bipolar, level, dial, list, dial,
  dial, level), page 2 P9 REL (Release), the rest unused; defaults 80 64 0 28 0 0 77 0 / 77. Value texts: INPT L R L+R
  STR (4 zones), MON ALW TRIG (2 zones), GATE OFF at 0 then -80..-20 DB; 0029's stepped list knobs: INPT 4, MON 2.
- Until then the override (`livein=voice:V`) and the host's LIVE IN page: the Rytm's SRC page shows the reported
  machine's labels. (A Rytm-side table blocks the name and the labels: the host cannot change a built-in machine's
  labels on the Rytm; a user slot's could be, but a user machine is a card package with no audio input in the plugin
  ABI.)

## 5. Threads, memory, CPU

- The ring: 2048 frames of float L / R (16 KB, static), one writer (the IN completion on core 0, `bridge::livein_in`,
  only while a voice plays LIVE IN), the count stored with release; each LIVE IN voice its own read point on its render
  core. No allocation, no card I/O anywhere in LIVE IN (tested: 0 `operator new`, 0 card time on the render core over
  4000 blocks with a DRIVE and a PRE route) [measured].
- The control core: the override, the trig / note-off counters, `livein::step()` (the guard's and the measure's log
  lines, from `idle_stream`), the console.
- CPU: one LIVE IN voice (gate, MON TRIG, the guard) **0.06 % of a Mac core** (Apple Silicon, 1 s of audio in 12-frame
  blocks, `test_livein`) [measured]; **Pi 4 ~0.26 %** by the ratio docs/INSERT-FX.md uses (x 4.4) [inferred]; the boot
  bench measures it on the Pi (`bench livein`, a synthetic AUDIO IN; `pi_run.py bench`). Its E0 declaration: mono only
  (VOICES 1 max: a poly config resolves to one voice), 0.04 % declared.
- The INSERT chain's cost is the chain's (an AMP: ~1.5 % of a Pi 4 core, docs/INSERT-FX.md).

## 6. Tests and the demo

- `core/tests/test_livein.cpp` (in `make -C core test`): the declaration (names, mono only, the page, labels and every
  knob text's length, the page drawn through the text-fit check), the override over a reported machine, the signal path
  (a tone through at 0.7 = the voices' headroom, GAIN +-12 dB exact), a pluck through an INSERT AMP (a synthetic
  capture) and a SPACE send (send FX 1 TYPE 5 via bus 1: its echoes after the input stops), INPT L / R / L+R / STR on
  TRK and on POST (the pair, and through the AMP: centred), the gate (closed on -60 dBFS hiss, open on a -20 dBFS tone,
  the hold, then shut), MON TRIG with the sequencer's notes (`inst::seq_on` with VOICES 4: the mono path) and a Rytm
  trig, SUS and the note-off's release, underruns (no stream, half blocks, back), a stalled render's backlog cut back,
  the feedback guard (a x 2 loop muted, a swell and a strum left alone), the measure (a 377-frame loop: 377, a missing
  loop: failed), the estimate, no allocation / card time on the render core, the CPU.
- `make -C core demo-livein OUT=dir [MODEL=file.nam]`: `livein_demo.wav` (19 s, stereo): a synthetic guitar (plucked
  strings at a DI's level) as AUDIO IN -> LIVE IN -> INSERT AMP (the capture MODEL names; else a synthetic one, not an
  amp) -> SPACE: the DI alone, + AMP, + SPACE, then MON TRIG gating it at 16ths.
- `sim/livein_e2e.py` (rytm-emu, `make -C core lib` first): the emulator's AUDIO IN (`dtboot --audio-in`: tones on L / R
  and clicks every 0.25 s) over the Overbridge link into the real core (`sim/poly_live.py --livein 0`, INPT L, a DRIVE
  insert), back into the Rytm on BD's voice input (OUT TRK), BD trigs opening its AMP. Checks: AUDIO IN on the R stream
  (IN 10 / 11; the emulator puts a WAV's full scale at -6 dBFS), the host's voice output (L's tone, not R's, the DRIVE's
  harmonics), LIVE IN's console line, BD's stem carrying the processed input, the round trip from the clicks [measured
  2026-10-24: ALL PASS, 761 frames].
