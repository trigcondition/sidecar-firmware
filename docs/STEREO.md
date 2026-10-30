# Stereo samples and the OUT route (TRK / PRE / POST)

The Pi engine (Pi 4 / Pi 5, and the Mac tests) plays stereo samples in stereo and can route any Sidecar voice past the
Rytm's mono track: straight onto the Rytm's main mix, before or after its master effects. The Sidecar Mini (Teensy)
stays mono (frozen at 0.1.0). Design: `docs/superpowers/specs/2026-10-22-stereo-post-design.md` (its nine open questions
decided 2026-10-22, its addendum on the streaming budget).

Tags as in the other docs: **[measured]** run on the Mac or the hardware and recorded; **[code]** read in the source;
**[inferred]** follows from those, not run on the real Rytm; **[open]** to be measured on the real Rytm (the steps are
in "Measurements" below).

---

## 1. For players

### The OUT route

Every Sidecar voice has an **OUT** setting, stored with the kit (in the kit's INSERT file, next to the insert effects):

| OUT | where the voice goes | the Rytm's FLTR / AMP / OVER / VOL / PAN / DEL / REV | master distortion + compressor | stereo |
|---|---|---|---|---|
| **TRK** (default) | its own track (as before) | yes | yes | no |
| **PRE** | the Rytm's main mix, before the master FX (Overbridge 2/3) | no | yes | yes |
| **POST** | the Rytm's main mix, after the master FX (Overbridge 0/1) | no | no | yes |

On PRE and POST the voice is a stereo pair: a stereo sample plays in stereo (DSP SAMPLE, MULTISAMPLE, GRANULAR), every
other machine plays centred. Its INSERT effects run in stereo too. Switching OUT crossfades over 8 ms (no click); a held
note keeps sounding, with or without the Rytm's track chain - a level step is expected.

What PRE / POST leave out: the track's filter, amp envelope, overdrive, volume, pan and the Rytm's delay / reverb sends
(the computer can only reach the Rytm's effect returns, not its effect inputs). What stays:

- **The track's FX1 / FX2 send knobs** still feed the Sidecar's send FX: the voice stands in for its (now silent) stem.
- **The INSERT page's bus sends** (bus 1 / 2 / 3), as before.
- **Mute.** The Rytm's MUTE only stops trigs; a loop on PRE / POST would ring on, so a muted track fades its PRE / POST
  voice out in 5 ms (and back in when unmuted). A shared voice (RS/CP, MT/HT, CH/OH, CY/CB) follows the track the
  Sidecar sequencer last played it from; for the Rytm's own trigs (its report does not name the track) it is muted when
  both of its tracks are.
- **Envelopes.** MULTISAMPLE, GRANULAR and the synth machines have their own. DSP SAMPLE has none (the Rytm's AMP shaped
  it): on PRE / POST it plays to the sample's end or the note-off (a 2 ms release), and **DEC** cuts it like the AMP's
  decay (OFF, or 20 ms .. 10 s to -60 dB).

The OUT settings:

| | what | range |
|---|---|---|
| OUT | TRK / PRE / POST | not p-lockable (a structural change) |
| LEV | the voice's level on PRE / POST | 0..127, 100 = unity (as on TRK, before the Rytm's VOL) |
| PAN | a stereo voice's balance, a mono one's pan (equal power, centre = unity) | L64 .. C .. R63 |
| WID | stereo width (mid / side) | 0 mono, 64 as the sample, 127 the sides twice |
| DEC | DSP SAMPLE's decay on PRE / POST | OFF, 20 ms .. 10 s |

LEV, PAN and WID are LFO targets (**O1..O8**: params LEV, PAN, WID).

**Where to set it.**

- **On the Rytm** (the Sidecar's remote INSERT page): the TRACK view's strip ends in the **OUT box** (`OUT POST`). Press
  **YES twice** (TRACK -> BUS -> OUT) for the **OUT view**: A OUT, B LEV, C PAN, D WID, E DEC (DSP SAMPLE only); the line
  under the title says what PRE / POST leave out. YES or NO goes back. **UP + AMP** (rytm-mods rui1047) opens the INSERT page directly on the
  OUT view of the selected track (`30 1 3 1 0`; UP + FX is the TRACK view); the KEYS page, UP's map and the touchscreen's map list it.
- **On the touchscreen**: the INSERT FX page's **OUT row**: a key (a tap: TRK -> PRE -> POST), LEV / PAN / WID with their
  values, the pair's L / R meters while it is on PRE / POST.
- **The console**: `out=V3:post`, `out=V3:lev:100`, `out=V3:pan:64`, `out=V3:wid:64`, `out=V3:dec:off`, `out=V3`,
  `out=list`. (`out=main` / `out=tracks` stay the output stage's mode.)
- **The web page**: `/status` `"ins"` carries `"out":[{"route":"post","lev":100,"pan":64,"wid":64,"dec":0,"open":1}, ...]`;
  the LFO tab lists O1..O8.

A kit file line: `OUT V3 POST 100 64 64 0` (voice, route, LEV, PAN, WID, DEC). An older Sidecar does not read the line:
it keeps such a kit file as it is (never written over, as with any line it does not know) and plays the voice on TRK.

### Stereo samples

The pool RAM keeps a file's channels: a stereo file is stereo in RAM (twice the bytes of a mono one), a file with three
or more channels keeps its first two as L / R. On TRK every machine plays the sum of the two, exactly as before (a
16-bit file bit for bit; a 24-bit one within 1 LSB). `poolmono=1` (cmdline.txt, or the console) sums stereo files at
load as before: half the RAM, PRE / POST voices then play them centred.

The pool lists tag every sample whose channels are known **ST** or **M** (the Rytm's POOL page, the touchscreen's POOL
page, the web Pool tab); the web tab adds **RAM** to a row marked "always in RAM".

MULTISAMPLE's regions read `pan`, `width` and `position` (sfz) on PRE / POST: a stereo sample's balance, width and place,
a mono one's pan. GRANULAR on PRE / POST reads a stereo source's two channels and pans each grain at random within
SPRY x WID (a mono source spreads too; WID 0: no spread).

### Streaming budget

Stereo samples stream like mono ones, from the card, with their heads in RAM (MULTISAMPLE's model). Each volume has a
**streaming budget**: how many streams of a format it carries with a margin, from what was measured on it - the storage
bench (`stor=bench[:VOL]`), for the card also the SD card test (`sdtest=start`) - else a cautious default per kind of
drive. A stereo MULTISAMPLE instrument whose volume cannot carry 8 of its voices is **held in RAM** instead (when it fits
the pool RAM's rule for instruments): its INST list note says `RAM (BUDGET)`, `/status` `"stream"` counts it. The
console's `stream` prints every volume's budget (stereo / mono 24-bit 48 kHz streams).

**Always in RAM.** A pool row, or a whole list, can be held whole whatever the budget says (short loops, hard-hit
material): `pool=inst:ram:A003:on`, `pool=smp:ram:all:on` (`:off` takes it back). Kept in the project's
`pool-used.txt` (`ram inst A003`, `ram smp *`); an older Sidecar drops those lines (the file is derived data).

### Recording

A voice on PRE / POST when a take starts records its own file in stereo (`13_VOICE1.wav` .. `20_VOICE8.wav`, L and R as
rendered). Two more sources: **23 PRE** and **24 POST**, the pairs as sent to 2/3 and 0/1 (`rec=ch:pre,post`, the web
page's record list). The sampler's "Sidecar voice" source records stereo for a voice on PRE / POST.

### Measurements (to do on the real Rytm) [open]

These are not measured yet (no hardware at the time of writing); the defaults follow the design's inferences. The engine
measures them itself: `stereo=measure` plays test tones and impulses and reads them back from the Rytm's MAIN L/R.

1. Connect the Rytm (Overbridge, as for playing), start the Sidecar. Stop the Rytm. Master FX neutral for the first run
   (distortion 0, compressor off). Note the Rytm's USB audio settings (its USB IN routing / level options).
2. On the console: `stereo=measure`. It runs about 4 s: silence, a 1 kHz tone on POST, then on PRE, then an impulse on
   POST. Then `stereo=result`:
   `measure: MAIN IN floor -95.0 dB; POST -6.0 dB, PRE -15.9 dB; impulse POST 40 frames`.
   - POST / PRE `(NOT IN MAIN IN)`: that pair does not reach what the Rytm sends back as MAIN L/R (the design's question
     3; then the recorder's MASTER and the sampler's MAIN miss PRE / POST voices: report it).
   - The two levels: how the Rytm's USB IN settings scale 0/1 and 2/3 (question 2).
3. With the track: pick track BD, put a Sidecar machine on it (voice 1). Set its FLTR open (LP, cutoff max, RES 0), OVER 0,
   AMP ATK 0 / HOLD max / DEC max, VOL 100, PAN centre, DEL and REV sends 0. Trigger BD once (its VCA stays open while
   the hold runs). Then `stereo=measure:trk:1` (about 6 s) and `stereo=result`:
   `... TRK -2.0 dB; impulse POST 40 frames, TRK 60 frames (TRK - POST 20); to match TRK: stereo=trim:post:40 stereo=trim:pre:139`.
   - `TRK - POST`: the alignment of a TRK voice and a PRE / POST voice in frames (expected within one Rytm block, 32
     frames [inferred]).
   - The trims make LEV 100 on POST / PRE as loud in MAIN as the same voice on TRK.
4. Repeat step 2 (and 3) for each of the Rytm's USB IN routing settings; note which one the Sidecar should use.
5. Keep the trims: `trimpre=` / `trimpost=` in cmdline.txt (tenths of a dB, e.g. `trimpost=40`); `stereo=trim:pre|post:N`
   sets them for the session. `stereo` shows them.
6. Send the `stereo=result` lines and the settings used: they go into this section and the design's phase 0.

The model the measurement was tested against (`pi/engine/tests/test_out_route.cpp`): MAIN IN = 0.5 x POST + 0.16 x PRE
+ 0.8 x the track, 40 / 40 / 60 frames late: it reports -6.0 / -15.9 / -1.9 dB and 40 / 60 frames [measured on the Mac].

---

## 2. For developers

### The signal flow [code]

```
render (core/voices.cpp render_voices, the render cores)
  TRK voice:      mono into ring (as before), R plane = L
  PRE/POST voice: L into ring, R into ring_r (nm::render_add2 / DSP SAMPLE's stereo / L = R for the others)
  -> soft limit -> ins::voice_out (ProcessMono, or ProcessVoiceStereo: the R side's state copied from L at the switch)
  -> the CC 120 ramp on both planes
Output::Push (core 3): FIFO frames of 8 x L, 8 x R, the real-stereo bits (2048 frames, 34 B each)
Output::Pull (core 0, pi/engine/output.cpp + core/fx/out_mix.h Mixer)
  every voice TRK and no crossfade: the old path, bit for bit
  else per voice: phases TRK / PRE / POST (8 ms, sin-shaped: equal power); TRK share (L + R) / 2 -> its channel;
  PRE / POST: the 2x2 matrix (WID, PAN, LEV; smoothed 3 ms), the mute gate (5 ms) -> the PRE / POST pair;
  each pair x its trim, soft-limited (linear to 70 % of 0.5 / 0.25, a tanh knee above) -> added onto 2/3, 0/1
  taps: the voices' mono (IN 13..20, the bus sends), L / R planes (the recorder, the sampler), the stem stand-ins (feed:
  (L + R) / 2 x LEV x gate x the non-TRK share), the pairs as sent (sources 23 / 24)
FillOut (ob_usb.cpp): gadget OUT, sendfx_pi::FromVoices(V, feed) -> AddReturn -> take playback -> recorder / sampler
sendfx_pi::FromRytm: each queued stand-in added into its stem's slot (the same pairing and lag as the bus sends)
```

### Files

| file | what |
|---|---|
| `core/fx/out.h`, `out.cpp` | the settings (Control bytes the render cores and core 0 read), the console (`out=`, `stereo`, `poolmono`), the kit line, /status |
| `core/fx/out_mix.h` | the Mixer (header-only, libm: hosted only; `pi/engine/output.h` holds it opaquely so the Circle side needs no libm) |
| `core/fx/out_measure.{h,cpp}` | `stereo=measure` (core 0's OUT and IN sides, the result) |
| `core/pool/pcache.{h,cpp}` | `Info::ch` / `fch`, `mono_at`, `set_mono`, stereo whole loads (`wavread::to_stereo16`) and fills |
| `core/pool/stream_budget.{h,cpp}` | the streaming budget's model, per-volume bench numbers, `stream` |
| `sim/sampler/sample_modes.h` | DSP SAMPLE's RAM modes: `Set(..., ch)`, `Render(..., outr)` |
| `core/engines/stream_voice.h` | DSP SAMPLE's stream: `render(..., outr)` (`run2<>` for 16 / 24-bit stereo) |
| `core/msamp/msamp.cpp` | heads / loop parts / rings in the sample's layout (a stereo ring holds half the frames), `render_add2`, pan / width / position, forced RAM, always in RAM |
| `core/granular/granular.cpp` | `Src::ch`, `play_grain<LIVE, CH>` with a right accumulator, per-grain pan (its own random stream) |
| `core/fx/insert.{h,cpp}` | a second DELAY line per voice slot (+256 KB), `ProcessVoiceStereo`, `voice_out(..., ring_r)` |
| `core/lib/pools.*`, `pools_fmt.*` | the `ram` flags (`ram K ROW` / `ram K *` in pool-used.txt), `bank_json`'s channels and RAM fields |
| `pi/engine/output.{h,cpp}` | the FIFO's R plane, `Taps`, the measure's test signals, `TakeVoicePeaks` |
| `pi/engine/recorder.*`, `core/rec/rec_names.*` | sources 23 / 24, stereo voice files (latched at the take's start) |

### Bit-for-bit guarantees [measured: the tests]

- Every voice on TRK: `Output::Pull` is the old code path; `test_out_route` compares it frame by frame with a copy of
  the old stage (tracks and main, with and without an R plane).
- A TRK voice's render with the R plane given equals its render without it (`test_msamp` stereo).
- A stereo entry's mono read = the summed load for 16-bit files read by the loader that summed them before
  (`pcache::mono_at`, DSP SAMPLE, GRANULAR: (L + R) / 2 truncated = pcache's `to_mono`; MULTISAMPLE: `>> 1`, as its
  `convert`: `test_stereo`, `test_msamp`). Across loaders (an entry MULTISAMPLE filled read by GRANULAR, or the other way)
  and for 24 / 32-bit files the mono value can differ by 1 LSB from before.
- The voice chains' mono path is unchanged (`Effect`'s `bus` argument stays -1 without R).

### Tests

| test | what |
|---|---|
| `core/tests/test_stereo.cpp` | pcache stereo / poolmono / 3+ channels / stereo fills, no card time on core 3; SampleModes stereo exact, mono = summed; StreamVoice stereo exact; DEC's times; the streaming budget's model and a 60 s simulation (within the budget no stream runs dry, far over it they do); the costs |
| `core/tests/test_msamp.cpp` (stereo) | RAM and STREAM on POST sample-exact vs the file, TRK = the summed load, pan / width, forced RAM over the budget, always in RAM |
| `core/tests/test_granular.cpp` (stereo) | a stereo source's L / R apart, the mono sum, per-grain spread (WID 0: none) |
| `core/tests/test_insert.cpp` | a voice's stereo chain = the bus chain for every TYPE; the OUT kit line round trip; the console; the OUT view (text fit); LFO O3 |
| `pi/engine/tests/test_out_route.cpp` | TRK bit for bit; TRK / PRE / POST on their channels; out=main; POST sample-exact; no clicks switching every 100 ms; the mute gate; WID / PAN / LEV; the limit; the measurement against a Rytm model |
| `pi/engine/tests/test_sendfx_pi.cpp` | the stem stand-in lands in its stem, frame-aligned |
| `pi/engine/tests/test_recorder.cpp` | a POST voice's stereo file, the PRE / POST pairs, mono again on TRK |
| `web/lfo.test.mjs`, `pi/engine/tests/page_pool.test.mjs` | O1..O8, the ST / M / RAM tags |

### Limits worth knowing

- In `out=main` mode the TRK voices' MAIN sum keeps its hard -12 dBFS limit; the POST pair (soft-limited to -12 dBFS
  itself) is added after it, so 0/1 can reach about -6 dBFS with POST voices (plus the send FX returns, as before).
- A take's stereo voice files follow the voices' routes when it is armed (`rec=arm`), not when PLAY starts it.
- MULTISAMPLE decides RAM / STREAM (the budget, always in RAM) when it loads an instrument: a change acts at its next
  load (`msamp=rescan`). Over the budget wins over an explicit `sidecar_load=stream` too (streams the volume cannot carry would run dry).
- The PAN law follows the voice's pair: balance while it is a real stereo pair, equal power for a mono one (kept
  through silent blocks).

### Costs

Measured on the Mac (`test_stereo`, `test_out_route`), the Pi 4 estimated from PLAITS' boot-bench ratio (0.73 % of a
core a voice there; the same method as INSERT-FX.md's; the Mac's timing varies run to run, the ratio with it) [measured
on the Mac 2026-10-22, the Pi 4 inferred]:

| item | Mac | Pi 4 (est.) |
|---|---|---|
| the OUT mixer, all 8 voices on POST (core 0) | 0.26-0.40 % of a core | ~1.5-2 % of core 0 |
| INSERT COMP + EQ, 8 voices, mono / stereo | 0.44 / 0.70 % | ~3 / ~5 % (the render cores) |
| DSP SAMPLE from RAM, a voice, mono / stereo | 0.013 / 0.016 % | ~0.09 / ~0.11 % |
| MULTISAMPLE, one note, 1 s of the render (stereo / mono) | 2.0 / 1.9 ms | (as the Mac's ratio) |
| GRANULAR, a 20 grains/s cloud, 1 s (stereo / mono) | 0.89 / 0.87 ms | (as the Mac's ratio) |

Memory:
the FIFO 32 -> 68 KB; the voices' second DELAY lines +256 KB; the pcache doubles per stereo file (the budget is the limit).

---

### Device-specific (the Analog Rytm MKII)

The host side is device-agnostic: the OUT route speaks of a device's **voices**, **tracks** and **returns**, and every
channel number comes from one table, `core::out::Device` (`core/fx/out.h`; `device()` is the device connected). A
future device (an Analog Four MKII) adds its row and its own firmware mod. What is the Rytm's alone, in that row or in
the measurements:

- the host -> device channels: the main return (POST) 0/1, the FX return (PRE) 2/3 (before the master distortion and
  compressor), the voices' own channels 4..11 in the order BD SD RS/CP BT LT MT/HT CH/OH CY/CB;
- the tracks that play each voice (12 tracks, four shared voices) for the mute gate;
- AUDIO IN L / R as Overbridge IN 10 / 11 (`audio_in`: what LIVE IN plays, docs/LIVE-IN.md) and the Rytm's own
  OUT -> IN loop, 289 frames (`loop_frames` [measured 2026-10-15, docs/pi-engine.md]: LIVE IN's latency estimate);
- MAIN L/R as Overbridge IN 0/1 (the measurement's read-back) and the voice stems as IN 2..9 (the send FX's, already
  the send FX's own Rytm-specific code);
- the inferences of section 1's measurements (the returns' gains, the alignment within one Rytm block of 32 frames).

## 3. Decided 2026-10-22 (the design's open questions)

1. TRK / PRE / POST, default TRK; no automatic switch when a stereo file is picked (the ST tag hints).
2. The Rytm's USB IN routing: measured in phase 0 (`stereo=measure`); the setting the Sidecar needs goes here; a warning
   on the OUT view when the global dump disagrees comes after the measurement.
3. MAIN IN and the POST return: measured in phase 0; if MAIN IN lacks it, the recorder adds the POST pair to MASTER.
4. The send FX stay mono-input (the voice's mono sum feeds them).
5. 3+ channel files: their first two as L / R.
6. pcache: native channels by default; `poolmono=1` for small boards.
7. DSP SAMPLE on PRE / POST: DEC now, AMP FOLLOW later (after SONG RECORD's MIDI CONFIG prompt).
8. Shared voices and mutes: the track that triggered it last (where known; else both muted - see Mute above).
9. OUT per voice, per kit.

## 4. Not in this build (and why)

- **AMP FOLLOW** (the design's phase 6): later, by decision.
- **GRANULAR's live sources in stereo**: the capture ring stays mono (the sum); a stereo capture ring is a change of
  its own.
- **The SRC view's `ST` / `>POST` marks** on the Rytm: the design leaves them optional ("else not shown").
- **The touchscreen REC page's grid** has no free cell for PRE / POST: they are on the web page's record list and the
  console (`rec=ch:+pre,+post`).
- **The touchscreen STATUS page** does not show the budget's forced count: `/status` `"stream"` and the console's
  `stream` do.
- **The UAC2 gadget's PRE / POST channels**: outside the design's build order.
- **The USB IN routing warning on the OUT view**: waits for the measurement (decision 2).
