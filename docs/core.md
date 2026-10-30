# The engine core (`core/`)

Portable C++17: the eight voices, the host link, presets (JUNO and FM lists, JUNO knob write-back), the pool, the
sample RAM and the user slots without plugins. Built from one source for the Teensy (`teensy/voiceloop`), the Mac
(tests, `sim/coreengine.py`) and the Pi (`pi/engine`, docs/pi-engine.md: 2026-10-14-pi-engine-usb). Spec:
`docs/superpowers/specs/2026-10-14-pi-engine-design.md`; how it came out of voiceloop:
`docs/superpowers/plans/2026-10-14-engine-core-extraction.md`.

## Layout
| file | what |
|---|---|
| `platform.h` | what the core needs from a board (clock, cycles, log, seed, sample RAM, files through `core_port.h`) |
| `engine.h` | the platform layer's API: `engine_begin`, `engine_sysex`, `render_voices`, `idle_prepare/save/stream` |
| `hooks.h` | the platform's entry points: `to_rytm`, the user slots (machine plugins), the transport guess |
| `machines.h` | the voice byte's machines, the user slots, `eng_zone` (PLAITS' curated models), `kVoices`/`kBlock` |
| `voices.*` | the voices: reports, renders, DSP SAMPLE streaming, JUNO / 6-OP FM selection |
| `host_link.*`, `slots.*`, `sysex.*` | the host's SysEx in; the empty user slots; every message out |
| `lfo.*` | the Sidecar's LFOs on voice / send FX parameters and the Rytm's own (CCs), synced to its clock, saved per project (docs/lfo.md) |
| `notes.*` | `core::NoteAdapter`: a stock Rytm's MIDI notes as the mods' reports (below, "MIDI notes") |
| `fx/` | `fx::SendFx` + `fx::Control`: the Sidecar send FX (below, "Send FX"); `send_fx_card.cpp` its card files |
| `ui/` | the remote UI: 1-bit canvas, fonts, widgets, page declarations, the host for the Rytm's remote page (`core/ui/README.md`) |
| `main_out.h` | `core::MainOut`: the voices summed on Overbridge MAIN L/R for a stock Rytm (below, "MAIN output") |
| `pool/` | the card index, the project's pool and listing, the sample RAM (`sample_ram_pool.*`, `rytm::SampleRamPool`) |
| `presets/` | the JUNO and DX7 libraries and lists (`juno_lib.h`, `dx7_lib.h`, `md5.h`) |
| `engines/` | `stream_voice.h`; `setup_engines.sh` + `engines.mk` give non-Arduino builds the Teensy's curated engines |
| `host/` | the Mac port, `newlib_rand.cpp`, `core_c.cpp` (ctypes) |

`core/host_link.*` is the SysEx dispatcher (what was `report()` in voiceloop.ino): whole messages in, any
transport. The Teensy's USB host transport is a different file and stays in the sketch:
`teensy/voiceloop/usb_host.h` (it was `voiceloop/host_link.h` before the extraction, renamed to keep the two apart).
`core::fxin` (same files) holds the IN L/R source: host link 0x26 FXIN / 0x27 FXIN_STATE, the setting in `/.rytm/fxin.txt`, the
console's `fxin` (docs/pi-engine.md, "IN L/R source").

The Teensy gets the core as a generated Arduino library: `teensy/setup_core_lib.sh` copies `core/` and
`teensy/platform/` into `teensy/libraries/RytmCore` (git-ignored; `run_sketch.sh` runs it on every build).

## MIDI notes
`core/notes.*` (`core::NoteAdapter`, moved from `pi/engine/notes.*` on 2026-10-15) turns MIDI channel messages
into the mod firmware's reports, so a stock Rytm's sequencer plays the voices: channel N (1..8) is voice N-1,
note-on -> 0x04 TRIG+PARAMS with the voice's P1..P16, the sounding note's off (or velocity 0) -> 0x11 (an older
note's off is ignored), CC 16..31 -> P1..P16 and a 0x03. `SetMachine(v, name)` picks the voice's machine and its
sounding defaults (`plaits juno daisy dsps grain fm md wt gr livein chop`; `Names()`; `md`, the built-in MD, replaced DIGIDRUM's `dd` 2026-10-29). The reports go to the board's callback,
`core::engine_sysex` or a wrapper: the adapter keeps only its own per-voice P values and last note, no render
state, so one instance on the control side (never per render core, `RYTM_RENDER_CORES` untouched). Cold
(`CORE_COLD`): one call per MIDI message.

| board | wiring | default | runtime switch |
|---|---|---|---|
| Pi engine | `cores.cpp` `OnChannel` (the Rytm's USB MIDI, `midi_in`) | on | console `:notes=1/0`, `:notemach=V:NAME` |
| Teensy voiceloop, host build | `usb_host.h`: the Rytm's USB MIDI note-on/off and CC -> `midi_channel()` | off | serial `:notes=1/0`, `:notemach=V:NAME`, `:notes` |
| Teensy voiceloop, serial build | the `'M' <status> <d1> <d2>` frame | off | the same `:` lines |

Off by default on the Teensy so the mod firmware's trig reports alone play the voices (its notes, if any, would
trig them twice); `-DRYTM_NOTES=1` boots with it on. The Teensy counts note-ons either way (`midi notes N,
channels X` after the cpu line when any came). Tests: `core/tests/test_notes.cpp` (the reports' bytes, then each
machine on voice 0 through the engine: it sounds, the other voices stay silent); the Pi's `test_content` and
`pi_run.py engines` use the same class.

## MAIN output
A stock Rytm's OS plays only the Overbridge MAIN L/R inputs; the mod build plays each voice's own track channel.
`core/main_out.h` (`core::MainOut`, header-only, from the Pi's `out=main` in `pi/engine/output.cpp`) is the
stock-Rytm path: the 8 voices summed at x0.5, a linear fade-in over 200 ms after each `Reset()` (a new stream,
the switch to main) and a hard limit at -12 dBFS (0.25) on every sample. `Next()` gives one frame as float,
`Next16()` as int16 (|x| <= 8192), `Gain()` + `Sum()` the two halves (the Pi fades its tracks mode with the
same gain), `TakePeak()` the largest |sample| since the last call.

| board | wiring | default | runtime switch |
|---|---|---|---|
| Pi engine | `output.cpp` `Output::Pull` (main: `MainOut`, tracks: each voice limited on its channel) | main | console `:out=main`, `:out=tracks` |
| Teensy voiceloop, host build | `usb_host.h` `main_mix()` (program flash) on the ring before `pushVoices`; `RytmOverbridge::setMainOnly` sends slot 0 alone, on MAIN L/R | tracks (`-DRYTM_OUT_MAIN=1`: main) | serial `:out=main`, `:out=tracks`, `:out` |
| Teensy voiceloop, serial build | none (the serial link carries the 8 voices; the Mac decides) | - | `:out` lines answered "the host build's" |

The Teensy's tracks mode stays as it was (no limit, no fade: the mod build's levels unchanged). A switch drops
the frames queued in the Overbridge FIFO (made for the other layout, so a raw voice never reaches MAIN): a few
ms of silence at most, possibly counted as an xrun [inferred: not yet measured with a Rytm]. In the host build without a stream, `'N'` frames carry what the Rytm would get
(out=main: the MAIN mix in slot 0, the others 0), which `rmp_e2e.py --host` step 14 checks. Tests:
`core/tests/test_main_out.cpp`, `pi/engine/tests/test_output.cpp`, `teensy/libraries/RytmHost/extras/ob_block_test.cpp`
(`kVoiceToNone`).

## Send FX
`core/fx/send_fx.h` (M2 of `docs/superpowers/specs/2026-10-16-sidecar-send-fx-research.md`, route A; optional and
Pi-focused). SEND FX 1 and 2: input = sum over the 8 voice stems (Overbridge IN 2..9) of stem x the track's send
level (squared, 0..127; a shared voice RS/CP, MT/HT, CH/OH, CY/CB takes the louder track), the effect, stereo out,
wet only, soft-limited at -6 dBFS on the 2/3 return (the Rytm adds it at ~0.16) and -12 dBFS on 0/1.

- **Values** (`fx::Control`, a few bytes, in every build): host link 0x20 SENDFX (`host_link.cpp` ->
  `fx::take_sendfx`): tracks 0..11 = sound index 0 hi/lo = SEND FX 1/2; track 12 = each FX's TYPE + P1..P5 from FX
  words 29/38/39 (FX 1) and 0/40/41 (FX 2), TYPE in the stock-stored hi half of 29 / 0, so it travels with the kit;
  track 13 = the kit's two user-effect tags. Layout, ids and messages: `sim/protocol.py` ("Sidecar send FX").
- **TYPE ids:** 0 off, 1 CHORUS, 2 COMB, 16 CRUSH, 17 RINGMOD, 18 HALO (every build), 5 SPACE, 6 RINGS, 8 CLOUDSEED,
  15 STUTTER (only with `RYTM_SENDFX_LARGE`, the default on the Pi and the Mac, not on the Teensy), the other ids of 3..63 reserved built-ins (named, silent,
  reported MISSING), 64..67
  UFX1..UFX4, user-effect slots per project like the user machines (`/.rytm/fxslots/<project>.txt`, loaded on 0x10;
  the kit's tag must match the slot's; no user-effect plugins exist yet, so they are MISSING).
- **To the Rytm:** 0x21 SENDFX NAMES (the TYPE knob's list) and 0x22 SENDFX STATE (per FX: id, READY/MISSING, name,
  for `!NAME`) on 0x23 ASK, on a project change, and 0x22 when an FX's id or state changes.
- **Labels to the Rytm (0x24 SENDFX LABELS, 2026-10-16):** after the 0x21s, one message per TYPE this build plays: the
  four P2..P5 labels (<= 4 chars) and the zones of zoned knobs (<= 12: knob, lo, base, show, step, 5-char name), from the
  table next to each effect (`fx/knob_labels.h`; `kChorusLabels`, `kCombLabels` in `send_fx.h`, `kRingsLabels`,
  `kCloudSeedLabels`, `kStutterLabels` in their headers; `sim/protocol.py SENDFX_LABEL_SETS` mirrors them, the tests
  compare). A Teensy build (no RINGS / CLOUDSEED / STUTTER) sends CHORUS, COMB, CRUSH, RINGMOD and HALO only. The Rytm keeps 8 TYPEs' labels (rytm-mods 0029 `sfu.c` NLT; a 24 past them is dropped there) and the Pi plays 9
  with labels since SPACE (2026-10-23): the host sends the TYPEs on FX 1 and FX 2 first, then the others by id up to 8,
  and when a slot moves to a TYPE left out it sends the list and the labels again with that one first
  (`host_link.cpp` kRytmLabelTables). Raising NLT in a later rytm-mods build lifts it (then set kRytmLabelTables). rytm-mods 0029 shows them
  (RINGS P3 `MODAL 12`, STUTTER P3 `1/16`); P1 (ROUTE) it shows as PRE / POST by itself. **step** (2026-10-18, the zone
  byte's bit 3): a pure zone selector (STUTTER ENG, LEN) is stepped by the Rytm one zone per detent like its own enum
  knobs, storing each zone's lo (rytm-mods 0029 sfx-enum-knobs); knobs with a number inside their zones stay continuous;
  an older Rytm ignores the bit.
- **DSP** (`fx::SendFx`, header-only, no allocation; 49.9 KB, or 5.88 MB with SPACE, RINGS, CLOUDSEED and STUTTER (2026-10-23): every type's
  state in each slot, so both slots can run any type, two CLOUDSEEDs included): every knob's 0 is a usable default;
  10 ms smoothing on every parameter and send (RINGS and CLOUDSEED: per block); a TYPE change fades out 10 ms and the
  new effect fades in from a clean state; the left effect is zeroed afterwards in pieces of at most 2 KB a frame
  (CLOUDSEED's 1.25 MB over ~50 12-frame blocks, never in one), and a TYPE whose clear is not done waits silent;
  denormals flushed in every feedback path; a NaN or runaway clears both effects (counted).

  P1 of every FX is **ROUTE** (per slot): **0..63 PRE**, returned on host -> Rytm 2/3 (MAIN FX), which enters before the
  Rytm's analog distortion and compressor (the default, and what kits already sounded like); **64..127 POST**, returned
  on 0/1 (MAIN), added after them (clean). Measured 2026-10-16 on the real Rytm (OS 1.73, Overbridge mode): OUT 1-2 stayed
  clean with DIST at max, OUT 3-4 got gritty and louder with DIST up; no firmware mod is involved. A P1 change
  crossfades between the pairs over 384 frames (8 ms), no click. STUTTER follows its slot's P1 (PRE by default: the mix
  goes through the analog distortion + compressor). P1 shares TYPE's word (kit only, not lockable). The effects use P2..P5, all
  p-lockable:

  | TYPE | P2 | P3 | P4 | P5 |
  |---|---|---|---|---|
  | 1 CHORUS | rate 0.2..5 Hz | depth 25..100 % of 6 ms | delay 8..25 ms | feedback 0..0.8 |
  | 2 COMB | tune 55 Hz x 2^(v/24) | decay: T60 0.2 s x 40^((v&63)/63), 0.2..8 s; 64..127 negative (octave down, odd harmonics) | damping 0..0.95 | spread (R up to 3 % lower) |
  | 6 RINGS | pitch C2 + v/2 semitones | model x structure: zone 0 MODAL (2 voices), 32 MODAL MONO (odd L / even R), 64 STRINGS (2 x 4 sympathetic), 96 CHORD; within it structure 0.25..1 (CHORD: the chord) | brightness 0.35..1 | decay (Rings' damping 0.4..0.9) |
  | 8 CLOUDSEED | character x size: zone 0 ROOM, 32 HALL, 64 DARK PLATE, 96 AMBIENCE; within it size x1..x2.5 | decay: the preset's T60 x10^(v/127) | damping: in-loop low-pass / 16^(v/127) darker | modulation depth x1..x5, rate x1..x3 |
  | 5 SPACE | TIME: 0..63 free, the shortest head 40..400 ms (exp); 64..127 SYNC, the shortest head 1/16, 1/8T, 1/8, 1/8D of the MIDI clock | RPT: REPEAT in percent, 100 the edge, up to 127 the runaway (bounded) | MODE (stepped): H1, H2, H3, H1+2, H2+3, H123, H23+S, H123S (+S: spring) | AGE: 0 new .. 127 worn: loop low-pass 10 -> 2 kHz, wow up to +-0.6 % (~AGE^2), flutter up to +-0.2 % |
  | 18 HALO | DCY: T60 0.3 s x 100^(v/126), 0.3..30 s; 127 FRZ (FREEZE) | MOD: 0 still .. 127 deep (+-1 ms at ~1.1 Hz) | TONE: loop low-pass 1.2..10 kHz, input high-pass 30..250 Hz | SHIM: octave-up shimmer 0 off .. 127 |

  **SPACE** (`fx/tape_fx.h`, written for the Sidecar; Airwindows TapeDelay2 and Dub Station (MIT) and ChowDSP's tape
  model (GPL-3) read for ideas only, no code): an RE-201-style tape echo, `RYTM_SENDFX_LARGE` only (the Teensy reports
  it MISSING).
  - **The tape:** one mono loop of 2^17 floats (512 KB a slot, 2.73 s at 48 kHz; 545 KB with the spring), three
    playback heads at 1 : 2 : 3 of the shortest, 4-point cubic reads with the delay ramped per sample. TIME is tape
    speed: all heads scale together and a change glides (~100 ms), so the echoes on tape bend in pitch, never jump.
    SYNC takes the tempo from `Control::clock` (24 ppqn, the last up to 24 ticks; 120 BPM before a clock, the last
    tempo after it stops); a delay the tape cannot hold is clamped (H3 <= 2.72 s).
  - **The record path:** input + REPEAT x (the selected heads' sum / their count, so 100 % is the edge in every MODE)
    -> 45 Hz high-pass -> AGE's low-pass (the loss per pass) -> saturation (`soft_limit` toward 0.7). Above 100 % a
    faint hiss (-80 dB x (REPEAT - 100 %)) seeds the runaway; at or below it silence in is silence out. A multi-head
    runaway fades while TIME is swept (each head is pitched by its own delay's change) and builds again when TIME
    rests; H1 howls through a sweep (the dub siren).
  - **Wow and flutter:** 0.55 Hz sine + a slow random drift, and 6.4 Hz; the depth set from the pitch deviation (the
    same at every TIME), the later heads a little more wow; once a block, ramped per sample.
  - **The spring** (H23+S, H123S): per side a 36 / 41 ms loop through 12 first-order allpasses (the chirp), a 4.5 kHz
    damping, feedback 0.8 (~1.2 s), fed by the input and the echoes (120 Hz .. 5 kHz); it runs only in a spring mode
    and is cleared when the mode leaves it.
  - **The return:** H1 centre, H2 left of it, H3 right, the spring L / R. Tests: `tests/test_tape.cpp`; demo:
    `make -C core demo-tape` (`space_demo.wav`). CPU (Mac, the x13.2 Pi 4 estimate) a 12-frame block: H1 0.14 us
    (1.9 us, 0.7 % of a core), H123 + spring 0.31 us (4.1 us, 1.6 %), two of them 0.57 us (7.5 us, 3.0 %), with
    CLOUDSEED AMBIENCE on FX 2 1.44 us (19 us, 7.6 %).

  **HALO** (`fx/halo_fx.h`, written for the Sidecar; the idea is the published figure-8 plate tank ("Effect Design
  Part 1", JAES 1997), with its own sizes, filters, modulation, shimmer and layout): a small lush modulated plate the
  Rytm's own reverb cannot do - very long smooth tails, FREEZE, SHIMMER, wide stereo - built for the Teensy's RAM:
  - **8.3 KB a slot, no new RAM:** one 4096-sample ring of IEEE half floats (x 4096: 11-bit precision at every level,
    no saturation below +-16; 16-bit integers truncated a long tail away at ~-50 dB, T60 halved - measured), every line
    a fixed region of the ring with one pointer (diffusers, the tank, the resamplers' histories); the small types
    (CHORUS COMB CRUSH RINGMOD HALO) share one block a slot in `SendFx` (only one plays at a time: the new one is built
    and cleared in place when it starts), so the Teensy's SendFx stayed 17.3 KB (17284 B, 8 B less than before).
  - **The tank at 24 kHz:** 31-tap half-band down, 27-tap up (polyphase); two branches (modulated allpass -> delay ->
    allpass -> delay, each feeding the other), 4 input diffusers, a ~150 ms loop; the tail's length is feedback.
    Four cubic-interpolated modulated reads, four LFOs at unrelated rates (linear interpolation's modulated top loss
    cut the 2..4 kHz T60 to a third: measured). TONE's low-pass in the loop, its high-pass on the input only (in the
    loop it shortened the low mids). FREEZE: input out, feedback 1, damping out (a 1 Hz DC blocker stays: a held
    filter state fed DC in - measured +7 dB a minute); measured -2..-5 dB over a minute (the cubic reads' top loss).
  - **SHIMMER:** two taps in a branch delay moving at twice the speed (octave up, integer steps), sine windows,
    band-limited 200 Hz..5 kHz (below, the bottom maps onto itself and piled up there; above, the octave aliases back
    onto fs/3), fed into the diffusers at 0.8 x SHIM x the branch's decay gain: it blooms and decays with the tail. A
    ceiling (branch ends' peak a block above 2.0, 0.5 when frozen with SHIMMER: the next block's feedback turned down)
    holds anything left; FREEZE + SHIMMER is a rising cloud at a fixed level.
  - **Measured (core/tests/small, Mac):** T60 0.33 / 0.89 / 2.8 / 9.6 / 22.7 s at DECAY 0 / 32 / 64 / 100 / 126 (TONE
    127, MOD 64; nominal 0.3 / 1.0 / 3.1 / 11.6 / 30); L/R correlation in the tail ~0.03; a DECAY 0 tail exactly 0 after
    5 s; 3 minutes at full knobs (and frozen) on full-scale noise bounded; a clean start after CHORUS in the shared block;
    All Sound Off clears the tail. CPU on the Teensy, estimated: the M7 build run in an ARM emulator (unicorn) counts
    5664 instructions per 12 frames for HALO alone (SHIM on), x1.45 cycles per instruction (calibrated on chorus + comb:
    4775 instructions there vs 6932 cycles measured on the device) = ~8200 cycles, **~5.5 % of the CPU per slot**; the
    whole SendFx with HALO + CHORUS ~9.2 %, HALO x2 ~13.9 % (chorus + comb 4.6 % measured). Mac: 0.34 us a 12-frame
    block alone. Software half-float conversion off the M7 (the Pi, the Mac), VCVTB on it.

  CHORUS and COMB lost their level knob (unity; the sends set the level).

  COMB's loop gain comes from the decay time, `g = 10^(-3 period / T60)` (and the linear interpolation's loss at the
  sounding pitch made up), so a note rings equally long at any P2 tune (measured T60 within 1 % at 55 and 554 Hz).
  RINGS (`fx/rings_fx.h`) is a port of Mutable Instruments Rings' modal resonator and sympathetic strings in its
  external-exciter mode (MIT, `fx/THIRD_PARTY_LICENSES.txt`): the send excites it; an onset in the send (a fast
  envelope over twice a slow one, at most every 50 ms) is a strum: the next voice takes the input and P2's note, the
  last one rings on, so a p-locked P2 plays overlapping notes. CLOUDSEED (`fx/cloudseed_fx.h`) is a port of Ghost
  Note Audio's Cloud Seed (MIT) with fixed 48 kHz buffers (pre-delay 100 ms, 4..6 early stages, 6 late lines up to
  360 ms with 2..3 diffuser stages, 1.25 MB a slot), presets in its own parameters and the macros above. A change of
  RINGS' model zone (P3) or CLOUDSEED's character zone (P2), p-locked or turned, goes through the same fade and clear
  as a TYPE change: no click, a ~10..30 ms gap. The Mac (M-series, `-O2`) per
  12-frame block: chorus + comb 0.3 us, RINGS 0.9..1.1 us, CLOUDSEED 1.0..1.3 us, two AMBIENCEs 2.5 us; x13.2 (M2's
  Mac / Pi 4 ratio) puts two CLOUDSEEDs at ~13 % of a Pi 4 core (test_send_fx prints the table).
- **Per-core rule:** one `SendFx`, owned by one core (the Pi: render core 2, after its block). It shares nothing
  with the voices, so `RYTM_RENDER_CORES` does not apply. The Teensy builds the values and the host link only
  (cold code); it places no `SendFx` (CPU/RAM: optional there) and builds without RINGS and CLOUDSEED
  (`RYTM_SENDFX_LARGE` 0 under Teensyduino): its 0x21 lists OFF CHORUS COMB, 6 and 8 report MISSING.
- **Console** (`fx::command`, the board logs the reply): `sendfx`, `sendfx=K:TYPE` (a name or an id), `sendfx=off`,
  `sendfxout=slot|fx|main` (a global override, for testing, saved in `/.rytm/sendfx.txt` by `idle_save`: `slot`, the default, lets each FX's P1 decide; `fx` or `main` force every FX, STUTTER included, onto 2/3 or 0/1 and win over P1), `sendlvl=K:T|all:V`, `sendknob=K:I:V` (I 0 =
  TYPE). Wired on the Pi (docs/pi-engine.md "Send FX").
- **Tests:** `core/tests/test_send_fx.cpp` (0x20, the commands, 0x21/0x22 through `host_sysex`, the mix math,
  impulse and sine responses, the comb's T60 at 55 / 554 Hz, RINGS' pitch per model and its strums, CLOUDSEED's T60
  against P3 per preset and its L/R decorrelation, the TYPE and structure-zone fades, P1 ROUTE (0 -> 2/3, 127 -> 0/1 for every TYPE incl. STUTTER, the override, the crossfade step), the spread clear, limits, NaN/subnormals,
  every knob at 0, the CPU per block for each effect),
  `sim/tests/test_core_sendfx.py` (the core against `protocol.py`), `pi/engine/tests/test_sendfx_pi.cpp`.

## Ports
Each board has a `core_port.h`: `core::File` with SdFat's names (the Teensy's *is* `FsFile`), `core::Fs`,
`CORE_COLD` (Teensy: program flash), `CORE_RAM2` (Teensy: RAM2), `CORE_STR` (Teensy: PSTR); the Mac's port also
supplies newlib's `sniprintf`.
The card fingerprint (`Fs::tree_hash`) is per board: a card moved between boards rebuilds its index once.
The Teensy's port is `teensy/platform/` (`core_port.h`, `platform_teensy.cpp`: the clock, the cycle counter, the
random seed, the log sink, the PSRAM region, SdFat's raw FAT32/exFAT tree hash); the Mac's is `core/host/`.

## Rules
- Module state, one engine per program.
- One `render_voices` call per block for all 8 voices, or (the Pi engine) disjoint voice ranges on several cores
  at once: the build sets `RYTM_RENDER_CORES` to the core count and the platform defines `rytm_render_core()` (the
  calling core's index), so the state voices share is per core:
  - the 6-OP FM reload stage `fm_stage` and `plaits::fm_user_bank`;
  - GRAIN's grain count (`SampleModes::ActiveGrains`): each core has the whole `kGrainBudget` (32) for its own
    voices, so the Pi can sound up to 32 grains a render core (a Pi core has the CPU; the Teensy's one count is
    unchanged). A grain gives its count back to the core that started it: the control core's Trigger/Stop stop
    grains a render core counted;
  - stmlib's `Random` state (shim `stmlib/utils/random.h`) and `rand()` (`core/host/newlib_rand.cpp`), so a core's
    noise depends only on its own voices and Pi renders are repeatable.

  `core/tests/mc/` checks it on two threads, bit-identical to one thread rendering the halves in turn
  (`test_render_cores`: FM reloads, PARTICLE, DAISYSP; `test_grain_cores`: GRAIN at the budget, retriggered and stopped from a third, control thread; on the Pi
  `pi_run.py --selftest engines` plays `grain`, retriggered twice a second). Each core
  passes its own `mach[]` (two cores with voices of one machine race on one array). The Teensy leaves
  `RYTM_RENDER_CORES` unset (one). Nothing else runs during a render.
- Hot code (`render_voices` and what it calls) has no `CORE_COLD`; everything per message or per card job does.
  A function moved out of voiceloop whose callers were all `FLASHMEM` loses its inlining and lands in ITCM by
  default: mark it `CORE_COLD` and compare the ITCM symbol lists (`arm-none-eabi-nm` on the build's ELF), not
  just the total.
- New SysEx goes through `sysex.*` with a test against `sim/protocol.py`.

## Tests
- `make -C core engines` once (and again after any engine or sim header edit: `build/engines` is a copy), then
  `make -C core test` (ALL CORE TESTS PASS) and `make -C core lib`.
- `core/tests/test_lfo.cpp` (in `make -C core test`) and `sim/rui_e2e.py lfo`: the LFOs (docs/lfo.md, "Tests").
- `uv run python -m unittest sim/tests/test_core_sysex.py sim/tests/test_core_stream.py sim/tests/test_core_engine.py sim/tests/test_core_sendfx.py`.
- `uv run python sim/core_golden.py check-host`: the Mac against the Teensy's golden renders (tolerance in the file).
- `teensy/gate.sh quick|full`: the Teensy on the core - sizes against `sim/golden/baseline.json`, the golden
  renders bit for bit, `rmp_e2e.py` serial and `--host`, plaits_bench, and (full) the plugin-less build
  (`-DRYTM_PLUGINS=0`) compiled and booted.

## The gate's numbers (before -> after the extraction)
Before: Task 1's baseline (2026-10-14, 4fe96b0, voiceloop unchanged). After: Task 15's `gate.sh full`
(2026-10-15, on e1d14d2: Tasks 2-14 and the final-review fixes). Measured: the build logs'
`RAM1`/`RAM2` lines and `${TMPDIR}/rytm-gate/*.log`.

| | before (Task 1) | after (Task 15) |
|---|---|---|
| serial: ITCM code / blocks / headroom | 118216 B / 4 / 12856 B | 118280 B (+64) / 4 / 12792 B |
| serial: RAM1 locals, RAM2 free | 117408 B, 18112 B | 117344 B (-64), 18112 B (+0) |
| host (`-DRYTM_USB_HOST=1`): ITCM code / blocks / headroom | 128904 B / 4 / 2168 B | 128840 B (-64) / 4 / 2232 B |
| host: RAM1 locals, RAM2 free | 88896 B, 18112 B | 88864 B (-32), 18112 B (+0) |
| plugin-less (`-DRYTM_PLUGINS=0`): ITCM / locals / RAM2 free | (not recorded) | 108312 B in 4 blocks, 22760 B headroom / 155456 B / 41248 B; boots |
| golden renders (9 scenarios, `check-teensy`) | captured | bit-identical audio, SysEx identical (0x1A-0x1D left out) |
| `rmp_e2e.py` serial / `--host` | 153/153, 153/153 | 153/153, 153/153 |
| plaits_bench: mean per voice, worst model | 4.98 %, MODAL 10.37 % | 4.98 %, MODAL 10.37 % (each model's avg within 0.01 points) |
| `check-host` (Mac core vs the Teensy's renders) | - | worst voice 87.9 dB (mixed); fm, digidrum, user_empty exact |
| `stress.py run c d --secs 20`: Teensy dropouts | 0, 0 (docs/PLAN.md 3c, 2026-10-09: cpu 39.1 %, peak 42 / 46 %) | 0, 0 (cpu 38.9 %, peak 42 % both) |

The ITCM symbol lists (`arm-none-eabi-nm`, names and sizes) of the serial and host builds are the same as after
Task 11: nothing from Tasks 12-14 landed in ITCM. The stress run's "+host" dropouts are the Mac's own stalls
(a 133 ms gap in d), not the Teensy's.
