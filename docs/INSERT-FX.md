# INSERT effects and FX buses (fx-ins, the Pi)

Roadmap item 2 for the Sidecar (Pi; the Mac build for tests): per-voice **INSERT** chains on the Sidecar's own voices,
before they go back into the Rytm's analog chain, and Tonverk-style **FX buses** built on the existing send FX. The
Sidecar Mini (Teensy) has none of it (everything is under `CORE_POLY`).

## Signal flow

```
Sidecar voice v (render core) -> INSERT A -> INSERT B -> POST EQ -> soft limiter -> Overbridge return v (mono, the Rytm's track)
                                                                      \-> send v->bus1 / v->bus2 / v->bus3 (OUT fill, core 0)
Rytm stems x track sends (as before) + voice sends -> send FX 1 -> bus 1 chain (A, B) -> POST EQ -> PRE 2/3 or POST 0/1 (FX1 P1)
Rytm stems x track sends (as before) + voice sends -> send FX 2 -> bus 2 chain (A, B) -> POST EQ -> PRE 2/3 or POST 0/1 (FX2 P1)
                                     voice sends only -> (dry)  -> bus 3 chain (A, B) -> POST EQ -> PRE 2/3 or POST 0/1 (ROUTE)
```

- Buses 1 and 2 **are** send FX 1 and 2: their TYPE and P1..P5 stay the Rytm's kit words, the stems' track sends and
  PRE/POST routing are unchanged. A kit with no insert settings sounds exactly as before (tested bit for bit:
  `test_insert` "the existing routing untouched").
- Bus 3 has no send FX of its own: a dry bus of the voices' sends through its two slots (a bus compressor, a shared
  filter, a stereo delay + auto-pan), returned PRE or POST as its ROUTE says.
- A voice's sends are taken after its chain (post-insert, pre-Rytm): the bus hears the inserted voice, not the Rytm's
  analog processing of it (that still reaches the send FX through the Rytm's own track sends, as before).
- **The OUT route** (docs/STEREO.md): a voice routed OUT **PRE** or **POST** skips its Rytm track and goes onto 2/3 or 0/1
  as a stereo pair; its chain then runs in stereo (`Engine::ProcessVoiceStereo`: each TYPE's stereo form, as the bus
  chains', the R side's state copied from L at the switch; a second DELAY line per voice slot, +256 KB), its bus sends
  are taken from the pair's mono sum, and it stands in for its silent Rytm stem in the send FX (the track's FX1 / FX2
  sends keep working). Tested: a voice's stereo chain = the bus chain for every TYPE (`test_insert`).

## POST EQ (ins1112): the stage after the inserts

Every chain - each of the eight voices and each of the three buses - ends in a **post EQ**: a high-pass, a low-pass
and a low and a high shelf, run **after both insert slots, AMP among them**, and before anything downstream. On a
voice that is before the soft limiter (`knee16`) and before the Overbridge return, so it is what the Rytm's track and
what the FX buses both hear from that voice. On **buses 1 and 2** - which *are* send FX 1 and 2 (the top of this
file) - it is the **send FX's return filter**: it sits on the slot's wet return, after the bus's own insert chain and
before the TYPE fade, the PRE / POST crossfade and the limits, which is where stock's DELAY and REVERB put their HPF
and LPF. Bus 3's dry return gets one too.

| Knob | Label | Range | Shown |
|---|---|---|---|
| A | HPF | **0 = off**, else 20 Hz .. ~20 kHz (20 x 2^(10 v), 12 dB/oct, Q 0.707) | `OFF`, `420`, `6.2K` |
| B | LPF | **0 = off**, else ~20 kHz .. 20 Hz (20k x 2^(-10 v), 12 dB/oct, Q 0.707) | `OFF`, `9.4K`, `180` |
| C | LOW | a low shelf at **250 Hz**, 64 flat, -15 .. +15 dB (0 is the lowest cut, not flat) | `FLAT`, `+3.0DB`, `-7.1DB` |
| D | HIGH | a high shelf at **4 kHz**, 64 flat, -15 .. +15 dB (0 is the lowest cut, not flat) | `FLAT`, `+6.4DB` |

HPF and LPF read as **amounts**, the way the Rytm's delay reads its two: turned down they do nothing. That is not
cosmetic - it is what makes the whole stage cost nothing and change nothing when it is not used: **a chain whose four
knobs are at their flat values is passed through bit for bit** (`Control::peq` is zero, `Engine::RunEq` returns at
once, `Engine::Active` stays false, and `test_insert` still asserts the untouched routing byte for byte). The shelves
are stored as the knob less 64 for the same reason, so a zeroed `Control` - a reset, a bench, a test - is a chain with
no post EQ at all.

**Click-free.** Engaging and releasing crossfade dry to filtered over `kFadeFrames` (5 ms), the same fade a TYPE change
takes, so the step from "no filter" to "a 21 Hz high pass" at the first notch of the HPF knob cannot click; the four
knobs are smoothed per 16-frame chunk with the slots' 3 ms one-pole, so a knob turn or an LFO never zippers, and the
coefficients (`tanf` / `powf`) are recomputed only when a smoothed knob actually moved. While the stage is releasing
its knobs are frozen, so a filter never sweeps on its way out. A NaN or a runaway clears it and passes the chain dry
(counted in `ins` `faults`, as a slot's).

The filters are the same A. Simper TPT forms the INSERT FILTER and EQ types use (`SvfK`, `svf`), so they are stable at
any setting and safe to modulate; the shelves use the same +-15 dB `A = 10^(dB/40)` scaling and are mathematically
transparent at 64. A stereo voice chain (OUT PRE / POST) runs the post EQ per side, and the R side's state is copied
from L at the mono -> stereo handover, as the slots' is.

**Control.** The knob, and an **LFO** on top (targets `I1..I8` / `B1..B3`, params **8..11** = HPF LPF LOW HIGH,
named `QHPF`...). **No p-locks**: the step-lock param space is exactly full at `kLockParams = 32` (16 machine + 8 send
FX + 8 INSERT) and `Step::slide` is a 32-bit mask indexed by the param, so a 33rd param is a file-format change. Said
plainly in "Left open" below.

**UI.** The Rytm's INSERT page: **RIGHT** opens the EQ view from any of its views (LEFT or NO back; RIGHT again does
nothing: ux1027, the page has no LEFT / RIGHT siblings), A..D the four knobs, G the bus / FUNC + G the track, YES swaps the voice's EQ
for the bus's. Its strip is `[A][B] > [POST EQ] > OUT` - where the stage really is. The touchscreen's INSERT FX page
shows the voice's post EQ on its hint line and marks a bus chain that has one (`B1 CHORUS > FILTER > - > POST EQ PRE`); it
has no dials for it (the page has no room, see "Left open"). Console: `ins=V3:eq:hpf:40`, `ins=b1:eq:lpf:90`,
`ins=V3:eq:off`; `ins` and `/status` `"ins"` carry `"peq"` per chain.

**Card.** One line per chain that is off its flat defaults, in the kit file beside the slot lines:
`PEQ V1 40 0 64 70` (HPF LPF LOW HIGH). A missing word keeps that knob flat. As with every other line kind, a build
that does not know `PEQ` treats the file as locked and keeps it as it is rather than dropping the line (ticket 1042).

## Effects (core/fx/insert.h)

| id | TYPE | P1 | P2 | P3 | P4 |
|---|---|---|---|---|---|
| 1 | FILTER | CUT 20 Hz..20 kHz (exp) | RES Q 0.5..20 | MODE LP > BP > HP (continuous morph) | DRV 0..+24 dB |
| 2 | CRUSH | RATE hold 1..64 | BITS 16..2 | DRV 0..+18 dB | MIX |
| 3 | DRIVE | DRV 0..+30 dB | SHPE soft sat > sine fold | TONE 18 k..1 kHz LP | MIX |
| 4 | EQ | LOW shelf 250 Hz +-15 dB | MID bell +-15 dB | HIGH shelf 4 kHz +-15 dB | FREQ bell 200 Hz..8 kHz |
| 5 | COMP | THR 0..-40 dB | RAT 1..20 | SPD att 0.3..30 ms / rel 40..640 ms | GAIN 0..+24 dB |
| 6 | DELAY | TIME 1..80 ms (glides) | FDBK 0..0.92 | MOD 0.7 Hz sweep up to 4 ms | MIX |
| 7 | TREMOLO | RATE 0.1..20 Hz | DPTH | SHPE sine > square | PAN (R phase offset; mono chains sum it) |
| 8 | RINGMOD | FREQ 10 Hz..5 kHz | SHPE sine > square | MOD 0.3 Hz +-1 oct | MIX |
| 9 | AMP | MODL a NAM A2-Lite capture of `/NAM/` | GAIN -18..+18 dB into it | TONE tilt +-6 dB at 650 Hz | LEV -36..+6 dB |

FILTER and EQ are TPT state-variable filters (A. Simper's forms): stable for any setting, modulation-friendly. COMP is a
peak detector with a 6 dB soft knee in the log domain (fast log2/exp2). DELAY's line is 4096 samples (85 ms) a channel.
Choosing a TYPE loads its defaults (EQ flat, a moderate filter, ...).

**Click-free:** a TYPE change crossfades the slot's wet share to dry over 5 ms, starts the new effect from a clean state
and fades it in (OFF is that fade's dry end); DELAY's line takes its input faded in too (a cleared line would else
return its first sample as a hard onset). Params are smoothed per block (3 ms), so p-lock jumps do not zipper. A NaN
or runaway clears the slot (counted, `ins` shows `faults`).

## AMP (TYPE 9): NAM captures on a voice

A Neural Amp Modeler capture on a Sidecar voice's INSERT slot (`core/fx/amp.h`, `amp.cpp`), run by TONE3000's NAM
A2-Lite engine (`third_party/nam/embedded`, MIT; the Cortex-A76 build: NEON assembly on the Pi 4's Cortex-A72 and on an
Apple Silicon Mac). **A2-Lite only** (the user's decision, 2026-10-23): A2-Full, NAM's A1 WaveNets ("Standard", "Lite",
"Feather", "Nano") and LSTMs are refused with **NOTLITE** ("not an A2-Lite model: LITE MODELS ONLY" on the console and
in the log). TONE3000 trains an A2-Lite version of the captures it hosts: download that one.

| Knob | Label | Range | Shown |
|---|---|---|---|
| P1 | MODL | the files of `/NAM/` (sorted by name, case ignored), spread over the knob (n files: 128 / n values each) | the file's short name (`PLEXICR`), or why it does not play |
| P2 | GAIN | -18..+18 dB into the capture (64 = 0 dB) | `+6DB` |
| P3 | TONE | a tilt after it around 650 Hz, -6..+6 dB (64 = flat) | `FLAT`, `-3DB` |
| P4 | LEV | the output, -36..+6 dB (109 = 0 dB) | `0DB` |

Every knob takes p-locks and LFOs as the other types' do. A MODL change (the knob, a p-lock, an LFO) **loads
asynchronously**: the slot keeps playing what it played (another capture, or dry) until the new capture is ready, then
crossfades to it over 10 ms; a lock that changes the capture every step lags by its load (a few ms on the Mac; on the
Pi est. 10-30 ms [inferred]). A capture used before is kept in its engine and comes back without a read (a reset and a
prewarm).

**Files.** `/NAM/*.namb` (TONE3000's binary NAM, 7,980 bytes for A2-Lite; CRC-checked) or `/NAM/*.nam` (NAM's JSON:
`"architecture": "WaveNet"`, one layer array of 3 channels, exactly 1,871 finite weights, `"sample_rate"` 48000 or
none). **TONE3000's A2 downloads** (`*_a2.nam`, ~300 KB) are a `"SlimmableContainer"` holding an A2-Lite and an A2-Full
WaveNet: the Lite submodel is taken, as is (checked with a CC BY capture from TONE3000, not shipped). At most 64 files
of names up to 47 characters; a file over 512 KB is refused (NOTLITE). The short name:
the name without its extension, upper case, spaces and `_` `-` left out, 7 characters (`Plexi Crunch.namb` -> `PLEXICR`):
name the files so the first 7 letters tell them apart. `amp=rescan` lists `/NAM/` again after files were copied.
`nam2namb` (third_party/nam/nam-binary-loader/tools) converts a `.nam` to `.namb` on a computer; not needed.

**The knob's texts** (P1, 7 characters; the Rytm's INSERT page and the touchscreen's INSERT FX page):

| Text | Meaning |
|---|---|
| `PLEXICR` | the capture plays |
| `PLEXIC~` | that capture is loading |
| `LOADING` | `/NAM/` is being listed |
| `NO NAM` | no `.nam` / `.namb` in `/NAM/` |
| `NOTLITE` | not an A2-Lite model: LITE MODELS ONLY |
| `BADFILE` | damaged, cut short, not a NAM model, a weight not finite |
| `NOT 48K` | a capture at another rate: refused (no resampler; the Sidecar runs at 48 kHz, as A2 does) |
| `MAX 6` | 6 AMP slots play already (the cap below) |
| `NO RAM` | the engines' memory was not placed |

A slot that cannot play passes the voice through TONE and LEV (dry). The card's file keeps the capture by name
(`AMP V1 A Plexi Crunch.namb` after the slot's line), so the knob follows the file when others are added; a file no
longer there is logged and the knob keeps its value.

**Where.** The voice chains only (V1..V8): a bus chain does not take AMP (the Rytm's TYPE stops at RINGMOD there, the
console says so). AMP is mono: a stereo voice chain (OUT PRE / POST) runs the capture once on (L + R) / 2 and puts it on
both sides. GAIN goes into the capture, then a 5 Hz DC block, TONE and LEV. The capture's own output for silence
(measured after its prewarm) is taken off: silence in, silence out (tested: 0.0 from the start, < 1e-5 one second
after a note; no denormals).

**Threads.** The control core (the Pi's core 3, `idle_stream` after the note machines) lists `/NAM/` (8 entries a
step), posts the file's open / 16 KB reads / close to the card core (`cardq::async`: core 3 never waits on the card),
parses (a `.namb` at once, a `.nam` 4 KB of JSON a step), loads the weights and prewarms (6,347 samples, 512 a step),
then offers the ready engine to the slot. The voice's render core takes the offer at its next chunk and crossfades; the
old engine goes back to the pool once the fade is over (`done == offer`, release / acquire). An offer to a slot that is
not playing AMP is taken at the next block (`amp::service` in `voice_out`). Nothing is allocated or read on the render
cores (tested: no `operator new`, no card time charged to the render core).

**Memory.** A pool of 7 engines (6 + one for a change's crossfade), each 93 KB (weights 7.5 KB, instance 0.5 KB, ring
state 85 KB at a 16-frame maximum block), and the file buffer (512 KB): 1.16 MB (1.35 MB with the cap at 8), placed with the INSERT chains' heap
buffers at boot on the Pi (`ins::heap_bytes`), static on the Mac.

**CPU and the cap.** One A2-Lite instance, 1 s of audio in 12-frame blocks (`test_amp`, `amp::bench`): **0.33 % of a
Mac core** (Apple Silicon, the same NEON code as the Pi). The Pi 4 estimate by the boot bench's PLAITS ratio (x 4.4):
**1.4 %**; by TONE3000's A76 figure (1.1 % at a 16 block, 2.3 GHz) scaled to the A72 at 1.5 GHz with its lower IPC:
**2.5-3.5 %** [inferred]. **Measured** on the Pi 4 (the boot bench, 2026-10-23): **3.98 % of a core an instance, worst
block 5.5 %** (8 would have been 31.8 %). So **at most 6 AMP slots play at once** (`amp::kMaxRunning`, was 8 when it
was planned with 3 %): 23.9 % of a core spread over the two render cores (voices 1-4 on core 1, 5-8 on core 2), at most
two a voice (8 % of the 20 % a voice may take). A seventh shows `MAX 6` and plays dry until one is turned off. The Pi's
boot bench measures it: `bench amp A2-LITE: 1 instance x.xx % of a core, worst block y % -> cap 6 = z %` (core 0, after
the `bench ins` lines).

**Console / web.** `amp` (the captures, how many play, each AMP slot's file and state in words), `amp=rescan`;
`ins=V1:A:type:amp`, `ins=V1:A:p1:40`. `/status` `"ins"` carries `"amp":{"models":N,"running":R,"cap":6,"slots":[{"c":"V1",
"s":"A","file":"...","st":"PLAYING|LOADING|NOTLITE|..."}]}`.

**Not done (later).** A cab IR after the amp (a short WAV in `/NAM/IR/`, <= 2048 taps: a partitioned convolution of its
own; TONE3000's "Amp + Cab" captures already include the cab, an "Amp Head" capture wants one); A2-Full (10-15 % a
Pi 4 core [inferred]); a resampler for 44.1 / 96 kHz captures; a send FX AMP.

**Tests.** `core/tests/test_amp.cpp`: the loader (a synthetic A2-Lite capture as `.namb` and `.nam`: read bit for bit,
also a JSON byte a step; cuts, a flipped bit, 100 garbage files, A2-Full, 16 channels, 1,870 weights, LSTM, 44.1 kHz,
NaN refused), the engine against TONE3000's Cortex-M7 C reference on three synthetic captures (identical) and
NeuralAmpModelerCore's test capture against its golden output (max error 2.4e-7, ESR -135 dB), the slot on a card
folder (listing, the async load, dry while loading, the crossfade without a click, a MODL change, reuse, BADFILE /
NOTLITE, the cap, silence, noise at every knob corner, the texts and the INSERT page's text fit, the card's AMP line,
`/status`; TONE3000's SlimmableContainer). Demos: `make -C core demo-amp OUT=dir [MODEL=capture.nam]` writes
`amp_demo.wav` (a guitar-like line) and `amp_kick_demo.wav` (a 909-style kick, synthesised, four on the floor: dry;
-> AMP; -> AMP with GAIN up; -> AMP -> SPACE H1 slapback; H123 synced 1/8 with REPEAT and AGE; a dub throw). Without
MODEL capture A is SYNTHETIC (random weights, not an amp: no amp colour); with MODEL a real A2-Lite capture.

## Control: knob, p-lock, LFO

`ins::control.play` is what the DSP reads: the knob, or a step's p-lock in its place, plus the LFO's offset.

- **p-locks** (seq-ux infrastructure): params 24..31 lock the track's voice's INSERT A P1..P4 / B P1..P4
  (`seq::kLockIns`; `sim/protocol.py SEQ_LOCK_INS`). On the Rytm: hold trigs on a seq track, press **FX** (the send FX's
  P2..P5 as before) and **FX again**: the **INS** lock page (A..D INSERT A, E..H INSERT B, the TYPE's labels; FX once
  more goes back). Console: `seq=lock:T:S:25..32:V`. A lock holds until the track's next step without it, STOP restores
  the knobs, a shared voice's lock belongs to the track that set it. An older Sidecar drops these locks from the file.
- **LFOs**: targets `I1..I8` (voice chains) and `B1..B3` (bus chains), params 0..7 = A P1..P4, B P1..P4 (names "ACUT"),
  an offset on top of the knob / lock.

## UI

- **Rytm remote page INSERT** (UP + FX, UP + AMP its OUT view; off the LEFT / RIGHT ring, no page dots - ux1027: UP +
  FX again leaves, as NO; UP + AMP again re-aims the OUT view at the selected track; listed once the Rytm has named a track on a
  Sidecar machine (0026's 33, rui1025+) or a chain or a post EQ is set). **RIGHT** from any of its views opens the
  **EQ view** (ins1112, above): A HPF, B LPF, C LOW, D HIGH; LEFT or NO back, RIGHT again nothing (ux1027),
  YES swaps the voice's EQ for the bus's. TRACK view: the Rytm's selected track's
  voice (FUNC + G another track), a strip `[A TYPE] > [B TYPE] > sends`, A..D P1..P4 (values as the effect reads them:
  Hz, dB, ms), E SLOT, F TYPE, G BUS, H SEND. **YES**: BUS view (A..D, E SLOT, F TYPE, G BUS 1..3, H ROUTE for bus 3);
  NO / YES back. `30 02` opens it directly (`kPageIns`).
- **Pi touchscreen INSERT FX** page (both themes, OLED default; page 9): the voice's two slots as dials with labels and
  values, a TYPE key per slot (tap: the next TYPE), V1..V8 keys, the three sends as bars, each bus's chain and return.
  It follows the Rytm's selected track. Open it from Settings > Send FX > **Inserts**, or with the Rytm's UP + T5
  (`49 1 5` since rui1052, `49 1 6` before it); UP + FX (rui1046; TRK + FX before) opens the Rytm's own INSERT page.
- **Web**: `/status` carries `"ins"` (every chain, params as set and as playing, sends, bus 3's route); the LFO tab
  lists the new targets. Editing on the web page: the console (`ins=...`), no dedicated tab yet.
- **Console**: `ins`, `ins=V3:A:type:filter`, `ins=V3:A:p1:90`, `ins=V3:B:off`, `ins=V3:send:2:100`,
  `ins=B1:A:type:eq`, `ins=b3:route:post`, `ins=reset`; ins1112: `ins=V3:eq:hpf:40`, `ins=b1:eq:lpf:90`,
  `ins=V3:eq:off`.

### Rytm mod changes wanted (rytm-mods 0026, not in this branch)

1. **TRK + FX** -> `30 02 01` (the INSERT page), as TRK + LFO sends `30 01 01`. The host side is in place. **Built:
   rytm-mods rui1041** (0026 design.md "1041": stock binds nothing to TRK + FX [measured]; again with the page open:
   `30 02 01` again; a held trig + FX without TRK stays the INS lock key).
2. **UP + T6..T10** -> `49 1 6..10`, next to rui1036b's UP + T1..T5 (ui-batch, for rui1038). rui.c's combo is
   `if (code <= 4 && UP held) { eat; dp.page_req = code + 1; }`: only T1..T5 (key codes 0x00..0x04) are forwarded, T6..T10
   (0x05..0x09) still place trigs. The change: `code <= 9` (the message is the same, `F0 7D 52 54 49 01 01 <page> F7`, page
   6..10); rui1036b's probe measured UP + T3 / T5 as no-ops in stock, T6..T10 want the same check. The host maps them:
   6 the touchscreen's INSERT FX page (`voice_pi::Request`), 8 its REC page (the Pi with a display:
   `remote_display_owns`; else REC on the Rytm's screen); 7 SAMPLE (again: AUTO), 9 SCOPE, 10 SIDECAR on the Rytm's own
   screen (`core/ui/remote.cpp display_remote_page`: the touchscreen has no such pages).
3. **rui1046: UP is the Sidecar's one modifier.** UP + FX opens the INSERT page as TRK + FX does (TRK + FX still works,
   the legacy form); UP + T11..T13 (SONG, PROJECT, POOL) join T1..T10. (rui1052 then put T1..T8 in the touchscreen
   sidebar's order - INSERT FX is UP + T5 now, SONG UP + T6; the table holds the current keys.) The page map is no
   longer written out here or in the code: the device's table `core/ui/page_keys.h` (docs/UI-THEMES.md "The device's
   shortcuts") names every UP + key and every page request, and the KEYS page, UP's map, the SIDECAR MENU and the
   touchscreen read it.
4. **rui1047: UP + AMP** -> `30 01 03 01 00`: the INSERT page on its OUT view (docs/STEREO.md); page byte 3, an older host shows SIDECAR.

Neither is required: the page is reachable by LEFT/RIGHT on the Rytm and from the touchscreen's Settings.

The strip's slot boxes show the effect's name alone (`FILTER`, `TREMOLO`); an empty slot shows `A -` / `B -` (ui-batch:
"A TREMOLO" did not fit the 38 px box). The host draws these texts (3D), not the firmware.

## Persistence

Per kit, as the LFOs: `/.rytm/ins/<project>/kNNN.txt` (kit NNN), `/.rytm/ins/<project>.txt` the project's default
(also while the Rytm has not named a kit). Lines: `V1 A FILTER 80 20 0 0`, `PEQ V1 40 0 64 70` (ins1112, only a chain
off its flat defaults), `SEND V1 0 0 90`, `ROUTE B3 POST`, and the
OUT route's `OUT V3 POST 100 64 64 0` (voice, route, LEV, PAN, WID, DEC; docs/STEREO.md; only a voice off its defaults). Written
a second after the last edit (`idle_save`); a kit or project switch writes the old kit's pending edits first.

## CPU (Pi 4)

**The POST EQ (ins1112)**, measured the same way (`test_insert` "ins1112's CPU", 8 voices x 12-frame blocks, all four
stages on - the worst case; each stage is skipped while its own knob does nothing, and the whole stage is skipped
while all four are flat):

| 8 voices, the post EQ on | Mac | Pi 4 (est. x 4.4) |
|---|---|---|
| on its own (both slots OFF) | **0.25 % of a core** (0.61 us a block) | **~1.1 %** |
| on top of COMP + EQ (the costliest pair) | **0.23 %** (0.56 us) | **~1.0 %** |
| with all four knobs flat | 0.00 % (the stage returns before it reads a sample) | 0.0 % |

Per voice that is 0.08 us of a 12-frame block on the Mac, **~0.34 us on a Pi 4**: about **0.5 % of the per-voice
guard budget** (`guard: voice budget 62 us`, the default 25 % of a 250 us block), so it does not move the budget.
The buses' are four more channels on render core 2 after the send FX, about the same again in total. Measured on the
Pi by the boot bench's fifth row, `bench ins OFF+OFF` (`pi/engine/kernel.cpp`, `bridge::ins_bench` turns the post EQ
on only for that row, so the four pairs' numbers stay comparable with the table below).



Each voice's chain runs on the render core that renders the voice (no cross-core traffic); bus 1/2 chains and bus 3
on render core 2 after the send FX. Measured on the Mac (`make -C core demo-insfx`, `test_insert`), the Pi 4 estimated
by the boot bench's PLAITS ratio (x 4.4):

| 8 voices x 2 slots | Mac | Pi 4 (est.) |
|---|---|---|
| FILTER + DELAY | 0.34 % of a core | ~1.5 % |
| COMP + EQ | 0.50 % | ~2.2 % (the costliest) |
| DRIVE + TREMOLO | 0.30 % | ~1.3 % |
| CRUSH + RINGMOD | 0.32 % | ~1.4 % |

spread over the render cores (a voice's chain is ~0.3 % of a core at worst). The kernel's boot bench logs the real
numbers on the Pi: `bench ins FILTER+DELAY: 8 voices x 2 slots x.xx % of a core, worst block y %` (core 0, after the
machine lines). Memory: 448 KB of DELAY lines (28 x 4096 floats), placed with the core's heap buffers (not in .bss).

## Left open / not built (the POST EQ)

- **No p-locks.** The step-lock param space is exactly full: `kLockParams = 32` = 16 machine P1..P16 + 8 send FX + 8
  INSERT (`core/seq/seq.h`), and `Step::slide` is a `uint32_t` mask indexed by the param number (`param & 31`), so a
  33rd param aliases param 0's slide bit. Giving the post EQ locks means widening `slide` to 64 bits and a `.sqp`
  block change, plus a ninth lock page on the Rytm. The LFO targets (params 8..11 of `I1..I8` / `B1..B3`) cover
  per-step-ish modulation in the meantime.
- **No touchscreen dials.** The INSERT FX page already carries eight dials, three send bars, three bus lines and the
  OUT row; four more dials want a layout of their own in both themes at both sizes. It shows the voice's post EQ on
  its hint line and marks a bus that has one instead.
- **Fixed shelf corners** (250 Hz and 4 kHz, the INSERT EQ type's). A FREQ knob for each would need two more params.
- **Not confirmed on hardware**: the Pi 4 figures in the table below are the Mac's scaled by the boot bench's PLAITS
  ratio. The kernel's `bench ins` prints the real ones.

## Tests (the POST EQ)

`core/tests/test_insert.cpp`, section "ins1112":

- **Where it is in the chain**, asserted and not assumed: a RINGMOD at ~1.3 kHz in slot A, then in slot B, fed a
  100 Hz sine. Everything the slot makes is up at 1.2 / 1.4 kHz, so a post-EQ **LPF at ~50 Hz silences it (-57 dB)** -
  which it can only do by running *after* the slot; a filter before it would have passed the 100 Hz input straight
  into the ring modulator. The converse too: a post-EQ **HPF at ~900 Hz, which would cut that 100 Hz input by 38 dB
  if it ran first, leaves the slot's output alone (-0.9 dB)**. AMP is slot type 9, so "after slot B" is "after the
  inserts, AMP included".
- **The send FX's return**: send FX 1 = CHORUS with no bus chain at all, one tone at a time, the return's RMS.
  Flat 120 Hz 0.1675 / 6 kHz 0.1600; the return LPF at ~1 kHz -> 0.1675 / **0.0042**; the return HPF at ~2.7 kHz ->
  **0.0003** / 0.1575. The filter is on the return itself.
- Each stage's response (HPF, LPF, the 250 Hz and 4 kHz shelves at +-15 dB, flat at 64), white noise at full scale
  through every corner of the four knobs and 64 random settings (finite, no faults, bounded), and the knob jumped
  between off and right up every 10 ms: **largest sample step 0.0144, the 220 Hz sine's own** - no click.
- **Bit for bit while flat**: the stage is skipped, `Engine::Active` is false, and the existing "the existing routing
  untouched" and "no voice sends = the same SendFx output" assertions still hold byte for byte.
- The control side (knob / LFO offset / the shelves' stored form), every one of the 4 x 128 value texts printing and
  fitting the Rytm's cell, the console (`ins=V3:eq:hpf:40`, `ins=b1:eq:lpf:90`, `ins=V3:eq:off`, the help), the LFO
  targets (12 params, `QHPF`..`QHIG`, the clamp), `/status`'s `"peq"` / `"peql"`, and the card's `PEQ` line written,
  not written when flat, and read back.
- **The UI rendered, not asserted by return value**: `build/rui_pages/ins1112_1_track_view.png`, `_2_eq_view.png`,
  `_3_eq_bus_view.png`, `_4_bus_view.png`, `_5_eq_no_slots.png` (RIGHT in, the knobs turned, YES to the bus, LEFT and
  NO out, and the EQ view with no slot set at all), plus a pixel check that the strip's POST EQ box is filled while
  the EQ is on. The touchscreen: `pi/ui/host/shots*/oled-insfx-voice.png` and `classic-insfx-voice.png` (the hint
  line and the bus line's `> POST EQ`).

## Files

`core/fx/insert.h` (DSP + API, the POST EQ's `Peq` / `RunEq` / `EqCoefs` / `EqRun`), `core/fx/insert.cpp` (values,
names, console, hooks), `core/fx/insert_card.cpp` (card),
`core/fx/amp.{h,cpp}` (AMP; `third_party/nam`), `core/tests/test_amp.cpp`, `core/tests/demo/render_amp.cpp`,
`core/ui/page_ins.cpp` (Rytm page, ins1112's `eq_draw` / `eq_strip` / the RIGHT key), `pi/ui/sidecar_insfx.{h,c}`
(touchscreen), `pi/engine/insfx_pi.{h,cpp}` (its view),
`core/tests/test_insert.cpp`, `core/tests/demo/render_insfx.cpp` (`make -C core demo-insfx OUT=dir`).

Hooks in shared files: `core/voices.cpp` (the chain after each voice; heap placement), `core/fx/send_fx.{h,cpp}`
(`fx::bus_insert`, `Process(..., vsend)`), `core/seq/seq.{h,cpp}` + `core/ui/page_seq.cpp` (INS locks), `core/lfo.{h,cpp}`
(targets), `core/engine.cpp`, `core/pool/pool.cpp`, `core/ui/remote.cpp` (load / save / kit / project, `kPageIns`),
`core/ui/extra.h`, `core/ui/page_scope.cpp` (page list), `pi/engine/sendfx_pi.{h,cpp}` (voice sends, bus 3),
`pi/engine/ob_usb.cpp` (one line), `pi/engine/bridge.{h,cpp}` + `kernel.cpp` (console, bench, display, /status),
`pi/engine/voice_pi.{h,cpp}` (UP + T5 since rui1052), `pi/ui/sidecar_ui.{h,c}` (page 9, the Inserts key), Makefiles,
`web/lfo.js`, `sim/protocol.py`; tests `test_seq`, `test_lfo`, `test_ui`, `test_sendfx_pi`, `pi/ui/host/shot.c`.
