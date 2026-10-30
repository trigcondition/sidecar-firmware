# GRANULAR: a granular machine for the Sidecar (Pi), on card samples or the Rytm's live audio

## In short

- A polyphonic granular machine on the Pi engine (`core/granular/`, Pi and Mac only: `CORE_MSAMP` builds). Machine flag
  **0x38** (`core/machines.h` `kGranMachine`); until the Rytm's 0021 has the machine, `gr=voice:V` plays it on voice V
  whatever built-in machine the Rytm reports. It supersedes GRAIN (0x18, DSP SAMPLE's grain mode as a placeholder
  machine the Rytm never sent): GRAIN stays as it was; 0x18 is CHOP's since rui1051 (docs/CHOP.md), GRAIN's code kept
  under a value no voice byte carries.
- Sources (SRC): a row of the project's **SAMPLES** POOL list (docs/POOLS.md): the Rytm's **live** Overbridge IN
  stream (AUDIO IN L+R, MAIN, or any of the 8 voice stems) recorded into the voice's own 5.5 s ring; two built-ins
  generated here (PAD, BELLS); or a sample (any WAV / AIFF on the card, by its hash, whole in the shared pool RAM:
  grains read anywhere).
- Grains: position, spray (position and timing jitter), size 5 ms .. 1 s, density 1 .. 200 a second, per-grain random
  pitch, reverse probability, four windows (HANN TRI TUKEY PERC), freeze, feedback (live). Each note is its own grain
  stream at its pitch (key 60 = the source as it is), so chords (P13..P16), trigs, the Sidecar sequencer and the pads
  play it. The cloud's level stays even across size and density.
- CPU-bounded: at most `gr=grains:N` grains a voice (default 64, up to 128); a grain over the cap is left out and
  counted. [measured: Mac] 2.4 ns a grain-sample; [inferred: Pi 4] ~20 ns, 64 grains ~6 % of a core, the bench's
  cloud (~9 grains) ~1.0 %; Pi 5 about 2.5x less.

Reference feel: the Elektron Tonverk's granular machine (public information only: grains with position, size,
density, spray and pitch on samples, live input and a freeze); nothing of its code or exact parameter set is used.

## 1. Sources

SRC (P1, the 14-bit word `index << 8 | bank << 5`) is a row of the project's **SAMPLES** POOL list, `code = bank * 128
+ row` (docs/POOLS.md; `core/lib/pools.h`). GRANULAR shares that list with DSP SAMPLE (every bank since rui1045) and the sampler's takes.

| row type | source | how |
|---|---|---|
| input `IN` | the Rytm's AUDIO IN L + R (Overbridge channels 10, 11; FX_L / FX_R while `fxin` routes the FX return there) | live |
| input `MAIN` | MAIN L + R (channels 0, 1) | live |
| inputs `BD SD RS-CP BT LT MT-HT CH-OH CY-CB` | the voice stems (channels 2..9) | live |
| built-in `PAD` | two detuned band-limited saws at C4, their brightness swelling over 4 s | generated here |
| built-in `BELLS` | 8 strikes of a bell's inharmonic partials at C4, 4 s | generated here |
| sample | a WAV / AIFF anywhere on the card, by its `lib::Ref` (resolved by hash: a moved file plays from where it is) | whole in the pool RAM |
| OFF (row 000 of every bank), an empty row, a MISSING file | silence | |

The template puts the inputs in H001..H010 and PAD / BELLS in H011 / H012. SRC's default 10 (A010) is an empty row: a
new GRANULAR track is silent until something is loaded. Rows never move: loading, a take or a fill never renumbers a
source.

### Filling the list

- **On the Rytm:** knob A on the SRC page opens the bank list (tabs A..H, rows `A001 name`); FUNC + YES on a row opens
  the browser (`@INPUTS/`, `@BUILT-IN/`, then the card's folders) and YES on a file sets the row, in every bank. The
  POOL page (after AUTO) shows, selects, unloads and replaces rows of all three lists.
- **On the touchscreen:** the MACHINE view's POOL key (the voice's SAMPLES list), FILES aimed at an empty row, FILL;
  the POOL page from ALL POOLS or Settings > Pool.
- **Web:** Files tab **+SMP** on a WAV / AIFF, **Fill bank** on a folder; the Pool tab.
- **Console:** `pool=smp:set|add|fill|unload|input|builtin|assign:...` (docs/POOLS.md "The console").

Replacing or unloading a row in use asks first and is refused while the Rytm plays (P3, docs/POOLS.md section 4).

### Choosing a source

- **On the Rytm** (its SRC page shows the Sidecar's view, rui1027): turn **SRC**: the bank list shows the rows around the
  value, the selection inverted with its state (LOADING, TOO LONG: FIRST 87 S, its length once in RAM, `!name`
  missing, `~name` changed). The value is the selection: the Rytm's own SRC, kept in the kit. A turn that rests 0.3 s
  previews the source (a ~0.9 s note at C4 with the voice's other knobs; `gr=audition:off` turns that off). The page
  shows the source's name top left over its outline, the bottom bar `BD SRC:<name>`.
- **On the touchscreen:** a tap on a filled row of the POOL view writes the Rytm's SRC through 48 (`gr=pick:V:C005`)
  and previews it. There are no picks over SRC any more: the choice is the Rytm's P1.

- **Live**: the Pi's IN completion (core 0, `pi/engine/ob_usb.cpp`, next to the recorder's and the remote UI's taps) hands
  each Overbridge block to `gr::live_in` while a voice reads a live source (`gr::live_wanted`): the ten sources go as
  int16 into a 170 ms capture ring (one writer). Each GRANULAR voice on a live source copies its source from there
  into its own float ring (`kRing` = 2^18 frames, 5.46 s) at every block on its render core, plus the feedback (FB x
  the voice's last block, soft-clipped), unless frozen. Grains read only what is in the ring and will not be
  overwritten while they play (behind the write head by their length, ahead of the oldest frame by their length).
  Careful: MAIN or the stem of the track the Sidecar voice returns into contains the voice's own output: a loop.
- **Card**: loaded whole into the shared pool RAM (`core/pool/pcache.h`: one copy a content, shared with DSP SAMPLE
  and MULTISAMPLE's members; loads through the card core, one 32 KB read a step) as int16 mono at its own rate (stereo
  summed), at most 2^22 frames (87 s at 48 kHz; longer is cut, logged). 8 card sources are viewed at once (`kSlots`),
  each a pinned block, so a source a voice reads is never evicted. Formats: as `core/wavetable/wav_read.h` (PCM 8..32,
  float 32/64, extensible). A trig while its sample loads waits for it (up to 250 ms). The project's most played
  SAMPLES rows are preloaded after a project load (free pool RAM only).

## 2. Parameters

| P | name | what | range |
|---|---|---|---|
| 1 | SRC | the source (above) | min(SRC, count - 1) |
| 2 | POS | card: the playhead's start (0 the start, 127 the end); live: how far back the grains read | live: 0 newest .. 127 ~5 s ago |
| 3 | SPRY | spray: each grain's position jittered (card +-, live back) and its start time +-50 % x SPRY | 0 .. 2 s (squared) |
| 4 | SIZE | the grain's length | 5 ms .. 1 s, exponential |
| 5 | DENS | grains a second, each note | 1 .. 200, exponential |
| 6 | PRND | per-grain random pitch | 0 .. +-12 semitones |
| 7 | REV | the probability a grain plays reversed | 0 .. 100 % |
| 8 | WIN | the window, 4 zones | HANN TRI TUKEY (flat half) PERC (3 % attack, exponential fall) |
| 9 | ATK | the note's attack | 1 ms .. 10 s |
| 10 | REL | the note's release (full while held) | 1 ms .. 10 s |
| 11 | HOLD | 0..119: card - the playhead's speed, 1x at 0 (the sample time-stretched at the note's pitch) slowing to 0 at 119; live - feedback, the voice's output into its ring (0 .. 0.95, soft-clipped). 120..127 FREEZE: card - the playhead stops at POS; live - the ring stops recording, the grains play what it holds | "0".."99", FRZ |
| 12 | ANIM | the cloud animated (Tonverk-like; integ-0.3): 0 off; depth = v/127, the cycle in 8 steps of 16 (8BAR 4BAR 2BAR 1BAR 1/2 1/4 1/8 1/16) on the Sidecar's clock (the Rytm's MIDI clock, song-position locked while it plays; else on at the last tempo, 120 BPM until measured). Moves the head (card +-1/4 of the sample, 4 s at most; live up to depth x the ring further back), the size (+-0.6 oct, 2nd harmonic), the density (+-0.4 oct, 3rd) and adds spray (+0.15 x depth) | OFF, the cycle |
| 13..16 | CHD INV SPR VOI | the chord (`core/poly.h`) | as MULTISAMPLE / 6-OP FM |

- Pitch: 2^((key - 60 + PRND x random) / 12) x the source's rate / 48 kHz; no TUNE knob (the note is the pitch, a
  drone plays on note 60). "Stereo spread": the voice's return is mono, so a per-grain pan summed back is nothing; left out.
- Level: each grain at 1.6 / sqrt(max(1, DENS x SIZE)) (the overlap; 1.6 for a Hann window's mean power), x velocity
  (0.25 + 0.75 v^2), x the note's envelope; the voice's notes x the headroom gain 1/sqrt(sum of envelopes) as
  MULTISAMPLE's, then voices.cpp's soft limiter.

## 2b. Clicks (integ-0.3)

Root causes found with the click detector in `test_granular` (`test_clicks`: the second difference over a sine
source's smooth bound, 9 cases) [measured: Mac], worst sample before -> after, x the smooth bound (a click > 4):

| case | before | cause | fix |
|---|---|---|---|
| grains across a card sample's end | 204 (29 clicks a second) | a grain wrapped from the sample's end to its start mid-window (a step whenever the end is not at a zero crossing; every scan cycle) | grains are placed inside the sample (clamped), wrap only when the grain is longer than the sample |
| silence (CC 120, machine change) | 91 | the note's grains stopped at once | they fade out over 3 ms (orphans) |
| SRC switched | 86 | the old source's grains stopped at once | they fade out on the old source (a `prev` hazard keeps a card slot; a slot switched away from is not reloaded for 100 ms) |
| steal (7th note on 6) | 53 | the stolen note's envelope restarted at 0 under its grains | its grains fade out over 3 ms, the new note starts its own |
| short REL / ATK 0 | 12 | the envelope's corners (1 ms linear attack, the exponential release's start) | the envelope is smoothed (one pole, ~0.7 ms) |
| live thaw | 11 | the ring jumped where writing resumed | seams (thaw, new source, first block) fade out / in over 2 ms |
| sparse PERC | 8.6 | PERC's 3 % linear attack (0.3 ms on a 10 ms grain), its tail ending at 0.008 | a raised-cosine 5 % attack, and every grain gets a 2 ms smoothstep edge fade in and out (half the grain if shorter) on top of its window |

Also: a live grain stays ahead of the oldest frame by its length even while frozen (a thaw restarts the writes there).
The grain cap leaves grains out (never cuts one): no clicks at the cap. After: every case under 1.4. CPU per
grain-sample 2.4 -> 2.9 ns (Mac).

## 2c. Displays (integ-0.3)

- **Rytm remote, SRC view** (`core/ui/page_wavegrain.cpp`, `kDrawGran` / `kDrawWt`): the top row is a picture over P1..P4's
  labels, the bottom row P5..P8's dials. GRANULAR: the source's outline (dotted), the head (POS + ANIM + scan, a solid
  line, capped while ANIM is on), each sounding grain a 2 px bar as tall as its level (a notch: reversed), LIVE / FRZ in
  the corner; a new picture at most every 83 ms (~12 fps) while it changes (`wavegrain_key`). WAVETABLE: the frame at
  POS (morphed) and the table's frames as ticks with the place. `core/tests/test_wg_view.cpp` renders them (PNG).
- **Pi display** (`pi/ui/sidecar_wgview.{h,c}`, page 7; filled by `pi/engine/wgview_pi.cpp` on core 0 ~10 times a
  second while shown, redrawn only on a change): GRANULAR the same picture large (grains' brightness = level, reversed
  in the secondary colour, ANIM's reach as a bracket on the head); WAVETABLE the table as a pseudo-3D stack of 9
  frames with the frame at POS in the accent colour at its depth, the place under it. Both themes (OLED default,
  CLASSIC). Open it by tapping a GRAN / WAVE voice on HOME; it follows the Rytm's selected track (its SRC view or
  voice page on the remote) when that plays WAVETABLE / GRANULAR. Cost [measured Mac, x34 Pi 4 estimate]: OLED ~3-5 ms
  a frame, CLASSIC ~11-15 ms. `make -C pi/ui/host` shoots `*-mach-granular.png` / `*-mach-wavetable.png`;
  `./shot wganim DIR N [classic]` an animation.
- API: `gr::viz` / `gr::viz_wave`, `wt::viz_pos` / `wt::viz_frame` (racy reads for drawing only).

## 3. Grains and cores

- Scheduling (the render core, each block): each sounding note keeps the time to its next grain; at each due time a
  grain starts (its first sample inside the block, exact), the next one 1/DENS later (+-50 % x SPRY). Grains are a
  voice's pool of 128 (`kMaxGrains`); one that ends or whose note ends is free. Linear interpolation, the window from
  a 512-point table.
- Notes: 6 a voice, 24 in all; a trig releases the earlier trig notes (their clouds fade with REL) and starts the
  chord; 0x11 releases; live pads / the sequencer hold theirs until their own note-off. Mailbox and hazard as
  WAVETABLE's / MULTISAMPLE's; no allocation after `begin()`.
- Memory: card slots 4 x 8 MB and the live rings 8 x 1 MB from the board's high heap (`msamp_ram_alloc`); built-ins
  2 x 384 KB; capture 160 KB.

## 4. CPU

[measured: Mac 2026-10-20, `test_granular`, `render_wavegrain --bench`; inferred: Pi via the bench ratio 8.1..8.4x of
the other machines' measured Pi 4 numbers, `docs/pi-engine.md`]

| case | Mac (% of a core) | Pi 4 1.5 GHz (estimate) | Pi 5 (estimate) |
|---|---|---|---|
| a grain-sample | 2.4 ns | ~20 ns | ~8 ns |
| bench note (PAD, 140 ms grains at 64 a second: ~9 sounding) | 0.13 % | ~1.0 % | ~0.4 % |
| 16 grains | 0.20 % | ~1.6 % | ~0.7 % |
| 64 grains (the default cap) | 0.72 % | ~6 % (4..9 %) | ~2.5 % |
| 128 grains (`gr=grains:128`) | ~1.4 % | ~12 % | ~5 % |

The Pi's boot bench has `gr` now (`pi/engine/cores.cpp`); its log gives the measured number.

## 5. Console and the Rytm side

- `gr` stats (sources, resident samples, notes, grains now / peak / started / capped, live frames, loads); `gr=voice:V`
  / `gr=voice:V:off`; `gr=list` (the SAMPLES rows that hold an entry, in the log); `gr=rescan` (failed sources tried
  again); `gr=grains:N`; `gr=pick:V:ROW` (a row `C005` or a code: writes the Rytm's SRC through 48) / `gr=pick:V:off`;
  `gr=preview:V[:ROW]`; `gr=audition:on|off`. The list itself: `pool=smp:...` (docs/POOLS.md).
- Rytm mod (rytm-mods 0021 + 0026, not in this branch): a machine slot reported with **0x38** (one of 0021's free hidden
  machines, e.g. 47 -> 0x38 in `us_flag_tab` and 0026's `mr_flag`), name `GRANULAR` / short `GRAN`. SRC knobs P1..P8
  SRC POS SPRY SIZE DENS PRND REV WIN (long: Source, Position, Spray, Grain Size, Density, Pitch Rand, Reverse,
  Window; looks: dial, dial, dial, dial, dial, dial, dial, list; defaults 10 0 20 64 64 0 0 0), page 2 ATK REL FRZ FB
  + CHD INV SPR VOI. integ-0.3: page 2 is ATK REL HOLD ANIM (long: Attack, Release, Hold/Freeze, Animate; looks: dial
  x4; defaults 10 60 0 0, the chord's 0 0 0 0). Value texts: SRC the host's row names (47, every bank with its bank byte; `OFF` at row 000, `A005` for an
  empty row; since rui1043 bank A like B..H, no IN / MAIN texts before the first 47); WIN HANN TRI TUKEY PERC (4 zones); HOLD
  "0".."99" (v x 99 / 119) for 0..119, FRZ for 120..127 (`gr::hold_text`); ANIM OFF, then 8BAR 4BAR 2BAR 1BAR 1/2 1/4
  1/8 1/16 by v / 16 (`gr::anim_text`); the chord knobs as 0026's. 0029's stepped list knobs: WIN 4, the chord's as
  MULTISAMPLE's.

## 6. Files and tests

| part | file |
|---|---|
| the machine | `core/granular/granular.{h,cpp}` |
| WAV reader (shared) | `core/wavetable/wav_read.h` |
| dispatch, page | `core/nmach.h`, `core/ui/page_wavegrain.cpp` |
| Pi live input | `pi/engine/ob_usb.cpp` -> `bridge::gr_live_in` -> `gr::live_in` |
| tests, demo | `core/tests/test_granular.cpp`, `core/tests/demo/render_wavegrain.cpp` |
| the POOL lists, the browser, the pool RAM (shared with WAVETABLE, MULTISAMPLE, DSP SAMPLE) | `core/lib/pools.{h,cpp}`, `core/lib/srclib.{h,cpp}`, `core/pool/pcache.{h,cpp}` (docs/POOLS.md); tests `core/tests/test_pools.cpp`, `test_srclib.cpp`, `test_pcache.cpp`, `test_wg_view.cpp`, `pi/ui/host/shot.c`, `pi/engine/tests/page_pool.test.mjs` |

Shared files: as listed in `docs/WAVETABLE.md` section 7.

`core/tests/test_granular.cpp` [measured] (in `make -C core test`): the SAMPLES list (inputs, built-ins and files as rows), a 24-bit
stereo 44.1 kHz sample loaded and played at its pitch (441 Hz at key 60, 882 at 72), a broken file refused; density
(5 / 20 / 80 a second: 5 / 20 / 80 grains in 1 s; a 3-note chord 3x; 50 a second of 200 ms: 10 sounding; spray keeps
the count); the live ring (every frame as fed, across the ring's wrap), freeze (the write head stops, the grains still
sound, also with no input at all; thawed it records again), feedback (the ring keeps sounding after the input stops,
silent without); reverse (a ramp read through the flat window rises forward, falls reversed); the cap (8: never more
than 8, the rest counted); live and sequencer notes, `gr=voice:V`; golden renders (a PAD cloud chord, frozen BELLS,
a scanned noise-burst sample: rms and zero crossings within 10 %); the CPU. `core/tests/test_notes.cpp` plays `gr`
through the MIDI note adapter and checks CC 120's silence.
