# SAMPLING, RESAMPLING, the AUTO-SAMPLER and the card queue (Sidecar Pi)

Status 2026-10-20, branch `sampling` (off `seq-mvp` 196f72e). Pi and Mac only (`CORE_MSAMP`); the Teensy (Sidecar Mini)
has none of it. No hardware was used. Tags as in `docs/MULTISAMPLE.md`:

- **[measured]**: run here (Mac unit tests and simulations) or read in our code / Circle's source.
- **[measured, Pi, earlier]**: figures measured on the Pi 4 before this work (`docs/pi-engine.md`, `docs/MULTISAMPLE.md`).
- **[inferred]**: follows from the above, not checked on a device.

## In short

| part | what | where |
|---|---|---|
| card queue | the MULTISAMPLE streams' opens and reads posted by core 3, run by core 0 before the recorder's writes (now 64 KB pieces, held back while a stream needs the card), uploads last | `core/msamp/cardq.{h,cpp}`, `core/msamp/msamp.cpp` (stream_step), `pi/engine/recorder.*`, `kernel.cpp`, `web.cpp`, `sdfiles.cpp` |
| 6-OP FM first note | a pool slot's first note was quieter: fixed (a free slot's voice re-initialised) | `core/poly.cpp` start() |
| chord before load | a sequenced chord on a MULTISAMPLE voice whose instrument still loads now plays when it is loaded (within 250 ms) | `core/msamp/msamp.cpp` live_on / loaded() |
| SAMPLING | one take from AUDIO IN L/R, MAIN, a Rytm voice output or a Sidecar voice into `/Samples/<project>/<name>.wav`, threshold / PLAY / next-bar start, pre-roll, bars at the Rytm's clock, trim, normalise, slices; MULTISAMPLE instruments at once | `core/samp/sampler.{h,cpp}`, `dsp.*`, `out.*` |
| AUTO-SAMPLER | the Sidecar's own FM / JUNO patches (offline, faster than real time), a Rytm track (MIDI notes, its Overbridge stem) or an external synth (MIDI notes out of the Pi, its audio on the Rytm's AUDIO IN) into `/MULTISAMPLE/<name>/` (WAV + .sfz, zones, layers, round robins, loops, one gain) | `core/samp/autosamp.{h,cpp}` |
| control | console `samp=...`, `asamp=...`; the web page's Record tab (Sample, Auto-sampler); the Rytm's remote SAMPLE page | `pi/engine/cores.cpp`, `www/index.html`, `core/ui/page_samp.cpp` |
| tests | `core/tests/test_sampling.cpp` (new), `test_msamp.cpp` (card queue simulation, chord before load), `test_poly.cpp` (first note), `pi/engine/tests/test_recorder.cpp` (pieces, yield) | |

## 1. Engine fixes

### a. One card-I/O queue (`core/msamp/cardq.h`)

The problem [measured: source]: Circle's EMMC driver transfers by PIO with busy-waits, FatFs holds a spin lock per volume
for a whole call. The streams read on core 3, the recorder wrote 192 KB at once on core 0 (worst write 100-137 ms
[measured, Pi, earlier]); a stream read during a write spun on core 3 until it ended (MIDI and SysEx waiting with it),
and who got the lock next was whoever spun first.

Now:

- **core 3 never touches the card for a stream.** `msamp`'s stream step posts a request (an open, or a read of up to
  32 KB into the request's own buffer) and takes the result on a later pass: one request per stream at most, 8 in flight,
  the neediest stream first; the conversion into the note's ring stays on core 3.
- **core 0 runs the queue** (`bridge::card_service(8)`) before and after the recorder's step in its loop.
- **the recorder writes 64 KB pieces** (a 192 KB chunk in three, still sector-aligned) and holds a piece back while
  `cardq::busy()` (a request waits, or the neediest stream is under half its read-ahead), unless its ring is half full;
  held-back pieces are counted (`yields` in the recorder's status).
- **uploads come last**: the web page's file writes (`web.cpp` BWrite) and the SD file port's (`sdfiles.cpp`) wait while
  `busy()`, 200 ms at most a write.
- **a held-up core 0** (a blocking TCP connect, ...): a request still waiting after 200 ms is taken back and run on core 3
  (`cardq::take_back`, counted `taken_back`), as before the queue. 200 ms is above the card's own pauses inside a write.
- `cardq=0` on the cmdline: no card core, core 3 reads the streams itself (the old path). Without a card core (the Mac's
  library and tests by default) a posted request runs at once in `post()`: the old behaviour step for step (the existing
  bit-exact streaming tests pass unchanged).
- Not moved (still on core 3, contending as before but now against 64 KB pieces): an instrument's load (the .sfz, the
  heads), DSP SAMPLE's refills, the SD card test, the project / sequencer saves.

**Test** (`test_msamp.cpp` test_card_queue) [measured, simulation]: one card timeline shared by the streams and a
recorder of 13 files (1.97 MB/s) on the Pi card's figures: reads 12 MB/s with 0.3..3 ms a read, writes 9.2 MB/s with a
100 ms pause every 2 s. MAJ7 chords on 4 voices every 2 s (16 held + 16 releasing stereo 24-bit streams, 32 at peak):

| order | underruns (note-blocks) | bandwidth steals | the recorder |
|---|---|---|---|
| the queue (streams first, 64 KB pieces) | **0**, output bit-exact with the RAM instrument | 0 | 203 pieces, held back 812 times, backlog at most 5 MB (its ring: 64 MB) |
| the recorder first, whole 192 KB chunks (the spin lock's worst order) | 5324 | 6 | - |

At 3 voices both orders pass on this card model: the queue matters when the streams need most of the card. On the Pi
the voice count with the recorder depends on the card (`sdtest`) [inferred].

### b. 6-OP FM: a pool slot's first note quieter (`core/poly.cpp`)

Root cause [measured]: `render_add` frees a released slot once it is quiet (-80 dB for 10 ms) and stops rendering it.
Its Plaits voice froze there: the DX7 operator envelopes mid-release (the modulators' often well above rest), the
operators' amplitudes and phases where they were. The slot's next note started its envelopes from those levels: a
faster, brighter, louder attack. A never-used slot starts from rest, so a slot's first note was the quieter one; the
later notes were the odd ones. Measured on the Mac (poly pool, CHD MAJ VOI 1, note 60, vel 127): ROM voice 10 with the
LPG (DEC 20): first note 6..8 dB under notes 2..4 over the first 10 ms; most voices 0.2..1.5 dB in the first 10 ms (the
attack's first 2 ms: 766 vs 2340 RMS); 200 ms windows within +-1 dB.

Fix: a note on a free slot that has played before re-initialises the slot's Plaits voice and FmMachine (Voice::Init:
6 us on the Mac; control core, between blocks, the slot not rendering). A retrigger of the same pitch or a steal keeps
the sounding state (no click). After it every note of a slot is sample-identical to its first (all 32 ROM voices of
bank 1: 0.00 dB; `test_poly` test_first_note checks the first 50 ms sample for sample on ROM voice 10, DEC 20).

### c. A sequenced chord before the sound has loaded (`core/msamp/msamp.cpp`)

The Sidecar sequencer plays a chord on a MULTISAMPLE voice note by note (`poly::seq_on` -> `msamp::live_on`); a trig
before the instrument was resident already waited (`pend`, 250 ms), but `live_on` dropped its notes. Now the voice's
live notes wait too (16 a voice, the newest of the same note kept), start when the instrument is loaded if within
`kLateMs` (250 ms; later ones counted `late`), and a note-off before that drops its note. Preloading is what selection
already did: a 0x03 report selects the instrument (`want`) and the loader starts it. `test_msamp`
test_seq_chord_before_load: three notes held and one released before the load; after it, three notes sound.
6-OP FM needs nothing: its DX7 voice is read when the voice is selected (the first report), before any note.

## 2. SAMPLING (`core/samp/sampler.h`)

| | |
|---|---|
| sources | AUDIO IN L/R (Overbridge IN 10/11, stereo), MAIN L/R (IN 0/1: resampling the whole Rytm, the Sidecar's voices and send FX in its mix), one Rytm voice output (IN 2..9: BD SD RS/CP BT LT MT/HT CH/OH CY/CB, mono), one Sidecar voice as rendered (mono; from the OUT fill) |
| start | now; a threshold (dBFS); the Rytm's PLAY; its next bar (MIDI clock, 96 ticks a 4/4 bar, PLAY starts bar 1) |
| pre-roll | 0..1000 ms kept from before the start (a ring while armed) |
| length | free (until stop, STOP for a PLAY start; at most max s, 120 at most) or N bars: with the clock running the take ends on the Rytm's bar line and is cut to exactly N bars at the tempo measured over it; without a clock N bars at `bpm:` |
| processing | free takes trimmed (the silence before: floor from the pre-roll + 12 dB, the start 2 ms before; after: + 10 ms, faded), normalised to -1 dBFS (one gain), 24-bit 48 kHz WAV |
| slices | transients (energy jumps in 5.3 ms hops, sensitivity `trans:SENSx10`, at least 60 ms apart) or a grid of N (zero crossings within 1 ms); none |
| files | `/Samples/<project>/<name>.wav` (name: `SMP_NNN` or `name:`); `/Samples/<project>/<name>.sfz`: the slice map (one region a slice: `offset`, `end`, keys 36.. = C1..; `loop_mode=one_shot`); `/MULTISAMPLE/<project>/<name>.sfz`: one zone over every key (root C4); `/MULTISAMPLE/<project>/<name>-slices.sfz`: the slices |
| available | the pools build (docs/POOLS.md section 8): the take goes into the **SAMPLES** POOL list, the first free row of the bank last shown (default A), else the next bank with room; the page and `/status` (`samp.row`) say `TAKE -> A012`; GRANULAR plays it from that row at once, DSP SAMPLE (any bank, rui1045) after the next boot (its card index of /Samples is built at boot, in Plaits' scratch before the voices take it: no rebuild at run time) [inferred: `pool_sync` finds no folder / file for it before]. The slice map (`<name>-slices.sfz`) goes into the **INSTRUMENTS** list the same way (a moment later: its .sfz is read on the card core); the one-zone `/MULTISAMPLE/<project>/<name>.sfz` is written but put in no row |

Cores [inferred: not run on the Pi]: the producer (`in_block` from the IN completion, `voice_block` from the OUT fill;
core 0) only copies frames into the take's ring and checks the start; the analysis and the files run on core 0's loop
(`samp::poll`, one 64 KB write a call, held back while the card queue is busy); settings, MIDI realtime (start, tempo)
and the POOL entries (`pools::add_take`) on core 3. The take's memory (max s + 1 s of stereo float, 23 MB for 30 s) is taken at the
first arm, not at boot.

Console:

```
samp                                   state, settings, the last take
samp=src:in|main|track:1..8|voice:1..8
samp=start:now|thr[:DB]|play|bar       samp=pre:MS   samp=len:BARS (0 free)   samp=max:S   samp=bpm:N
samp=slices:off|trans[:SENSx10]|grid:N samp=norm:on|off   samp=trim:on|off      samp=name:NAME|auto
samp=arm   samp=stop   samp=cancel
```

`/status` has `"samp":{state, src, sub, start, thr, preMs, bars, maxS, slices, grid, ms, level, peak, bpm, nSlices,
gainDb, wav, row, error}` (row: the take's SAMPLES row, `A012`; state 0 idle, 1 armed, 2 recording, 3 captured, 4 writing, 5 done, 6 failed).

Tests (`test_sampling.cpp`) [measured]: IN L/R, threshold -30 dB, 100 ms pre-roll, 8 hits after 0.5 s of floor:
trimmed to the hits (2 ms before the first), -1 dBFS, 8 slices each within 3 ms of its hit, the WAV's size exact, the
three .sfz written; the pools build: the take in SAMPLES A001, its slice map in INSTRUMENTS and played by
MULTISAMPLE (the one-zone .sfz stays on the card only, not in a row); MAIN, 2 bars from the next bar at a 125 bpm clock
(F8 every 20 ms): 3.84 s (+- 1 ms), the tempo measured 125, 8 grid slices; a Sidecar voice's JUNO note.

### The touch slicing page (pi/ui, later): what it needs

- **Data**: the slice map is the .sfz next to the WAV (`<region> sample=<name>.wav key=K offset=A end=B`, regions in
  order, key 36 + i). Reading it needs only the region lines; writing it back rewrites the file and the
  `/MULTISAMPLE/<project>/<name>-slices.sfz` copy (absolute sample path); the INSTRUMENTS row holding it would then read
  CHANGED (new content at its path) until accepted or replaced [inferred: not built].
- **Waveform**: a min/max overview per screen column of the WAV (24-bit stereo or mono); read on core 0 in pieces (the
  card queue's rules: after the streams). An endpoint to add: `/samp/peaks?path=&cols=` (not built).
- **Edits**: move a marker (snap to the nearest zero crossing within 1 ms, as `grid()`), add, delete, re-detect with a
  sensitivity or a grid count. An engine command to add: `samp=slice:PATH:trans:SENS|grid:N|set:A,B,C...` running
  `transients()` / `grid()` on the file and rewriting both .sfz (not built: the analysis functions exist in `dsp.h`).
- **Audition**: a slice plays through MULTISAMPLE (its key on the instrument's voice), or a preview path on the
  Overbridge return (rec_play-like) for a slice range: not built.
- **State**: `/status` "samp" gives the last take's path and slice count; the page lists `/Samples/<project>/*.sfz`.

## 3. AUTO-SAMPLER (`core/samp/autosamp.h`)

| | |
|---|---|
| (c) Sidecar voices | 6-OP FM (a DX7 voice by PAT: the ROM's 96, or the card's FM list) or JUNO (a factory patch by PAT), P1..P8 as given (`knob:K:V`; JUNO: the patch's own sliders), or a copy of what Sidecar voice V plays now (`voice:V`: its machine, knobs, DX7 voice / JUNO patch). Rendered offline on a private engine instance (the live voices untouched; on the Pi on core 0, whose `fm_user_bank` slot no render core uses): each take a fresh voice, note on, hold, note off, the release until 100 ms under -80 dBFS or `tail:` |
| (b) Rytm tracks | `track:T[:CH[:chro]]`: one of the 12 tracks (T 1..12: BD SD RS CP BT LT MT HT CH OH CY CB), a note on MIDI channel CH (default T: the Rytm's default track channels 1..12 [measured, emulator: docs/SONG.md 1.1]) through the host link (`core::to_rytm`, 3-byte channel messages as the LFOs' CCs), the track's Overbridge stem recorded in real time (`track_stem`: IN 2..9 = BD SD RS/CP BT LT MT/HT CH/OH CY/CB, so RS and CP, MT and HT, CH and OH, CY and CB share one) from note-on through hold + tail, a gap, the next. `chro`: the notes' own pitches (the Rytm plays the active track chromatically from its AUTO channel [inferred: the manual's AUTO channel, not run]); else note 60 every take (velocity layers and round robins only). Latency: auto = the onset (a tenth of its peak) of the first note's top-velocity take, one cut for all takes (2 ms before it); or `lat:MS` |
| (a) external synth (EXT) | `ext[:usb\|dev\|rytm][:CH][:lr\|l\|r]`: the notes lo..hi (their own pitches) on a MIDI output, channel CH, and the synth's audio on the Rytm's AUDIO IN recorded from Overbridge IN 11/12 (L, R, or both: a stereo instrument, the WAVs 2-channel), in real time as (b), the latency measured the same way. Outputs: `usb` the computer on the Sidecar's USB-C MIDI (a DAW or the OS routes it to the synth), `dev` the first USB MIDI device on the Pi's USB-A ports (a synth or a MIDI interface there; the start refuses without one), `rytm` the Rytm's USB MIDI (reaches a synth only if the Rytm passes USB MIDI on to its MIDI OUT [inferred: no such routing known on the MKII; not checked]; its track channels play its own tracks). The outputs are the platform's (`asamp_set_midi_out`: the Pi's `cores.cpp`, sent on core 0's loop); a build without one refuses `usb` / `dev` ("no MIDI out ... on this build") rather than record silence |
| plan | notes lo..hi every step (names or numbers), 1..8 velocities, 1..4 round robins, hold, tail, gap, loop on/off, name; at most 512 takes and 96 MB of takes (float mono) |
| per take | trimmed at the end (-60 dBFS after the gain, + 10 ms faded); silent takes left out ("no sound") |
| gain | one for the instrument: the loudest take to -1 dBFS (velocity layers keep their relation) |
| loops | when the hold part (from a quarter in to 20 ms before the note-off) is sustained (RMS in 8 parts within 3 dB): the period by autocorrelation, a loop of whole periods (>= 100 ms) at rising zero crossings scored by the seam's waveform and RMS match; a crossfade (equal power, the loop's last third, at most 50 ms) when the seam is not clean already; `loop_mode=loop_sustain loop_start loop_end` and the WAV's smpl chunk |
| mapping | zones split halfway between the sampled notes, velocity layers between the velocities, `seq_length / seq_position` for round robins, `ampeg_release` = the longest release captured |
| files | `/MULTISAMPLE/<name>/<name>.sfz`, `samples/<name>_<C4|Cs4>_v<vel>_rr<n>.wav` (24-bit mono 48 kHz); the .sfz names its source in a comment; the instrument goes into the INSTRUMENTS POOL list (the bank last shown) |
| progress / cancel | `asamp`: "taking C4 v100 3/24"; `asamp=cancel` stops after the current take; the files written so far removed, no .sfz |

Console:

```
asamp                                       state and plan
asamp=src:fm:PAT|juno:PAT|voice:V|track:T[:CH[:chro]]|ext[:usb|dev|rytm][:CH][:lr|l|r]   (V 1..8, T 1..12, CH 1..16)
asamp=notes:LO:HI[:STEP]  asamp=vel:V[,V..]  asamp=rr:N  asamp=hold:MS  asamp=tail:MS  asamp=gap:MS
asamp=loop:on|off  asamp=lat:MS|auto  asamp=knob:K:V  asamp=name:NAME|auto  asamp=start  asamp=cancel
```

`/status` has `"asamp":{state, done, total, written, label, name, sfz, loops, latMs, error}` (state 0 idle, 1 taking,
2 writing, 3 done, 4 failed, 5 cancelled).

Tests (`test_sampling.cpp`) [measured]:

- 6-OP FM ROM 1, C2..C6 every 12, velocities 64 / 120: 10 samples, zones 0..42 / ... / 79..127, layers 1..92 / 93..127,
  played by MULTISAMPLE. JUNO "A15 Moving Strings" C2..C6 every 6, hold 1.8 s: 9 samples, **9 loops** (4 of 9 met the
  seam score alone; the chorus and LFO move the waveform, the crossfade takes the rest), smpl chunks written.
- Speed on the Mac: the JUNO plan's 39 s of takes rendered, trimmed, looped and written in 0.19 s (~200x real time);
  on the Pi 4 about 10-40x [inferred: the voices' render cost, docs/pi-engine.md bench, one core].
- A Rytm track (a fake Rytm in the test answering the notes 6 ms late on IN 3): 2 velocities x 2 round robins, the
  notes on the given channel (CH 11), 4 note-ons and 4 note-offs, **latency measured 5.8 ms** (6 ms simulated, the
  onset at a tenth of the peak), `seq_length=2` on every region, layers 1..93 / 94..127.

Measured vs inferred for (b) on a real Rytm: nothing was run on the Rytm or in the emulator for this (rytm-emu stubs
the analog voices: its Overbridge stems carry no track audio). Inferred: a note on a track's MIDI channel triggers that
track (the Rytm's per-track channels); the track's stem on IN 2..9 carries it after ~6 ms (the Overbridge round trip
[measured, Pi, earlier: "about 6 ms"]); `chro` needs the AUTO channel and the active track.

### Fixes 2026-10-21 (branch `asamp`): the source list, EXT, "no sound"

Reported on the hardware: (1) the AUTO page's source list had no clear ends: turned all the way left it showed VOICE,
6-OP FM, JUNO ... at random; (2) no external synth; (3) no notes heard when the run started, and "err: no sound" after.
Reproduced on the Mac through the page itself (`core/tests/test_asamp.cpp` drives `kAutoPage` as the Rytm does).

| | root cause [measured: Mac, the page's code] | fix |
|---|---|---|
| 1 | `page_auto.cpp` stepped SRC as `(src + delta + 4) & 3`: it wrapped, and the Rytm's encoder deltas are relative and grow with the turn's speed (`remote.cpp`: `b[1] - 64`), so a fast turn left of -2, -5, -7 landed on TRK, VOICE, JUNO ... each time another (measured: JUNO, then -1 -1 -1 -2 -5 gave TRK VOICE JUNO TRK VOICE). Every change also reset B. SAMPLE's A, B and D wrapped the same way (`wrap()`). | SRC is an ordered list 6-OP FM, JUNO, VOICE, TRK, EXT, one kind a detent, clamped at both ends as every other Sidecar list knob (`pages.cpp` `step_value`, SCOPE); B is reset only when the kind really changes. SAMPLE's knobs clamp too. |
| 3 | TRK listed the 8 Overbridge stems and sent the note to channel stem + 1: right for BD and SD only. BT (stem 3) sent its note to channel 4 = CP, LT to BT, MT/HT to LT, CH/OH to MT, CY/CB to HT: the track that played was another one, on another stem, and the stem recorded stayed silent: every take silent, "no sound in any take". | TRK lists the Rytm's 12 tracks; the note goes to the track's channel (track N on N), the recording reads that track's stem (`track_stem`). |
| 3 | The Sidecar's engines (6-OP FM, JUNO, VOICE) render offline on a private engine instance: nothing is heard by design, while the page said RUNNING. | The running view's title says OFFLINE for them (RUNNING for TRK and EXT, which play out loud); on DONE, YES with no MULTISAMPLE track to audition on says NO MULTISAMPLE TRACK instead of doing nothing. |
| 2 | no source for an external synth (designed only). | EXT (above): MIDI out on USB-C (the computer), USB-A (a device) or the Rytm, AUDIO IN L / R / L+R. |

Not reproduced: an offline render coming out silent on the Pi (on the Mac all three engine sources record sound,
through the console and through the page). If a 6-OP FM / JUNO / VOICE run still ends "no sound" on the Pi, that is a
Pi-only difference (core 0's render) still to find.

The AUTO page now: A SRC (6-OP FM, JUNO, VOICE, TRK, EXT); B the entry: PATCH (FM, JUNO; FUNC: 8 a detent), VOICE
1..8, TRACK BD..CB, or for EXT MIDI: USB 1..16, DEV 1..16, RYTM 1..16 (FUNC: 16 a detent, the next output); B pushed:
EXT's input L+R, L, R (in the bottom bar: "YES 13T IN L+R LOOP", with the input's live level). A fresh EXT picks DEV
when a USB-A device is plugged in, else USB.

Tests (`core/tests/test_asamp.cpp`) [measured, Mac]: the list knobs (SRC from any state and any delta turned left is
6-OP FM, right EXT; one kind a detent in order; B of TRK 0..11 with its channel, of EXT 0..47, of FM 0..95, clamped;
SAMPLE's A, B, D clamped); a whole run per source kind started with YES on the page, every take's WAV checked for
sound (RMS) per channel: 6-OP FM, JUNO, a VOICE copy, TRK CP (channel 4, recorded from IN 4) and CY (channel 11, IN 9,
2 round robins) answered by a fake Rytm that sounds the track of the channel the note came on, EXT on DEV 5 L+R
(stereo WAVs, R = -L/2 frame for frame as the fake synth sends it) and on USB 1 R (mono); EXT without an output or
DEV without a device refuses to start. `test_samp_view` adds the EXT plan view (`13_auto_plan_ext.png`).

Measured vs inferred on the hardware: nothing here was run on the Rytm or the Pi. Inferred: the Rytm's tracks answer a
note on their default channels 1..12 (its stock CHANNELS [measured, emulator]); the computer's DAW must route the
Sidecar's MIDI port to the synth (and should not echo it back: the computer's notes on USB-C also play the Rytm's
selected track, `core/keys.h`); a USB-A synth takes event packets on cable 0.

### The demo

`SAMP_DEMO=out.wav core/build/test_sampling` writes the auto-sampled JUNO strings playing Cmaj7 Am7 Dsus4 G held 3 s
(longer than the 1.8 s sampled: the loops carry the sustain), then the 6-OP FM instrument's short chords, on
MULTISAMPLE (16-bit mono 48 kHz, 20 s; no clipping, sustained through the holds) [measured]. The m4a of it is
`autosampled_chords.m4a` (afconvert, AAC 192 kb/s), handed over with this branch's report.

## 4. Control surfaces

- **Console** (serial or the web page's console, `:samp...`, `:asamp...`): run on core 3 (`cores.cpp`).
- **Web page**, Record tab: "Sample" (source, start, threshold, bars, slices, Arm / Stop, the state and last take) and
  "Auto-sampler" (source, notes, velocities, hold, release, name, Start / Cancel, progress), both through `/cmd` and
  `/status`.
- **Rytm remote UI**: a SAMPLE page next to SCOPE, shown where REC is (the Pi): A source, B start, C bars, D slices,
  YES arm / record / stop, A pushed cancels (the take, else the auto-sampler); the take's time, the level, the last take
  and the tempo; the bottom bar shows the auto-sampler's progress. A separate page rather than REC's: REC's keys are all
  taken (T1..T11 channels, A arming, B..D playback).

## 5. Files changed

New: `core/msamp/cardq.{h,cpp}`, `core/samp/{dsp,out,sampler,autosamp}.{h,cpp}`, `core/ui/page_samp.cpp`,
`core/tests/test_sampling.cpp`, `docs/SAMPLING.md`.

Shared files (minimal edits): `core/msamp/msamp.cpp` (stream_step over the queue, live notes waiting for a load),
`core/poly.cpp` (start(): the slot's re-init), `core/voices.{h,cpp}` (`voice_sound`, `fm_bytes`: the auto-sampler's
copy of a voice), `core/ui/extra.h`, `core/ui/page_scope.cpp` (the page list), `core/Makefile`, `pi/engine/Makefile`,
`pi/engine/{bridge.h,bridge.cpp,cores.cpp,kernel.cpp,ob_usb.cpp,rec_pi.cpp,recorder.h,recorder.cpp,web.cpp,sdfiles.cpp}`,
`pi/engine/www/index.html`; tests `core/tests/{test_poly,test_msamp,test_ui_live}.cpp`,
`pi/engine/tests/test_recorder.cpp`. Not touched: `pi/ui/*`, `core/wavetable`, `core/granular`, `core/seq`.

## 6. Not done / next

- Run on the Pi: the queue's latencies under a real recorder take and an upload (`msamp` stats, the recorder's
  `yields`, `cardq` stats are logged / in `/status` only as described above; a `cardq` console line is not there yet).
- DSP SAMPLE at once (a run-time index append for one folder), GRANULAR's hook, the slicing page's engine commands.
- The Rytm per-note pitch by a host-link trig message (a rytm-mods change); EXT on the hardware (the Pi's USB-A synth,
  the computer's routing); the Rytm's MIDI OUT as an EXT output (does the MKII pass USB MIDI on?).
- The instrument loads (msamp's .sfz and heads) and DSP SAMPLE's refills through the queue too.
