# MULTISAMPLE: a multisampled instrument machine for the Sidecar (Pi), streaming from SD, and an auto-sampler

Status 2026-10-20, branch `multisample` (off `poly-fm` b898a5f). Pi and Mac only (`CORE_MSAMP`); the Teensy (Sidecar Mini)
is frozen and has none of it. No hardware was used. Tags:

- **[measured]**: run here (Mac unit tests, the emulator) or read in our code / Circle's source.
- **[measured, Pi, earlier]**: measured on the Pi 4 before this work (`docs/pi-engine.md`, `docs/sampler-plan.md`).
- **[public]**: public documents and forums, not checked on a device.
- **[inferred]**: follows from the above, not checked.

## In short

- The pools build (2026-10-22, docs/POOLS.md): INST is a row of the project's **INSTRUMENTS** POOL list (an `.sfz`
  anywhere on the card, by its hash, with its member samples resolved by their own hashes); the samples live in the
  shared pool RAM with GRANULAR's and DSP SAMPLE's (one copy a content). The memory figures below the pools build's
  note are the earlier `msampram=` arena's.
- **Format: SFZ** (the native one, a stated subset, our own parser), WAV/AIFF samples of any common depth and rate.
  Decent Sampler `.dspreset` is the second format to add (XML, same model); SoundFont 2 later, if asked for.
  Tonverk's own `.elmulti` (TOML) is not publicly specified; a converter is possible later.
- **Machine**: polyphonic, 16 notes a voice / 64 in all, key and velocity zones, layers, round robin, random, loops
  (continuous / sustain / one-shot), release triggers, per-note AHDSR from `ampeg_*` scaled by the knobs, chords on
  P13..P16 exactly as poly FM (CHD INV SPR VOI, the same table and code).
- **Streaming by default** (the user's requirement, MPC-style): opening an instrument reads the `.sfz` and each
  sample's first 0.5 s ("head"), plus a loop that lies past the head; the rest streams from the card into a per-note
  ring while it plays. `sidecar_load=ram` (or a small instrument, <= 8 MB) loads everything; refused (streamed, with a
  warning) when it does not fit.
- **SD card test** (`sdtest`): measures the card on the engine's own card path and turns it into a verdict ("OK for N
  stereo streaming voices + recorder" / "OK without the recorder" / "too slow: RAM mode or a faster card"), the
  read-ahead and the stream count, stored on the card and used by the streaming engine.
- **PoC**: `core/tests/test_msamp.cpp` (84 checks: parser, machine, streaming) and `test_sdtest.cpp` (32) pass with the
  rest of `make -C core test`; the Pi portable tests and page tests pass; both Pi kernels build. The streamed instrument plays **bit-exact** what the RAM one plays, also on a simulated card with
  jittered latency and 100 ms recorder-like stalls (16 stereo 24-bit notes, no underrun) [measured, simulation]. On the
  Rytm side there is no MULTISAMPLE machine yet (a rytm-mods 0021 change): `msamp=voice:V` plays it on voice V for
  now (see "How to try it").
- **Auto-sampler**: designed below (spec + effort), not built.

## 1. Formats [public]

| format | what it is | openness | libraries | verdict |
|---|---|---|---|---|
| **SFZ** (`.sfz` + WAV/FLAC/OGG/AIFF) | plain-text mapping: `<control> <global> <master> <group> <region>` headers, `opcode=value` | open, documented at sfzformat.com (community spec; v1 by rgc:audio, v2 by Cakewalk/ARIA) | the widest: free and commercial libraries ship it (Sonatina, VSCO 2 CE, Salamander piano, Versilian, Karoryfer, Pianobook exports), sfizz / LinuxSampler / Sforzando / ARIA play it; Elektron's Tonverk imports it | **native** |
| **Decent Sampler** `.dspreset` (XML) + samples | `<DecentSampler><groups><group><sample path rootNote loNote hiNote loVel hiVel start end loopStart loopEnd .../>` with ADSR on groups, UI and effects elements | the format is documented by Decent Samples (the player is free, not open source) | the Pianobook community, many free instruments | **second** (same zone model: a small XML reader maps it to `Region`) |
| **SoundFont 2** `.sf2` | one RIFF file: samples (16/24-bit) + presets/instruments/generators/modulators | open spec (E-mu/Creative SoundFont 2.04) | General MIDI sets, many old libraries | later (one file is easy to copy; the generator model is larger; streaming one big file works the same) |
| **Tonverk** `.elmulti` | TOML: `[[key-zones]]` with `pitch`, `key-center`, `[[key-zones.velocity-layers]]` (`velocity`, `strategy` e.g. `'Forward'`), `[[...sample-slots]]` (`sample` = a WAV in the same folder) | not officially documented; known from users' files on Elektronauts; the Tonverk manual's Appendix E "SFZ Support" calls the native format the primary one and imports simple SFZ | the Tonverk factory library; community tools (Multisample Architect exports `.elmulti`; tv7 renders DX7 patches) | read-only converter later, if users ask; the zone model maps 1:1 |

How Tonverk users make multisamples [public, forums]: the Tonverk's own sampling (its file names carry the note
number and name, an auto-sampling convention), a DAW plus a hand-written SFZ dropped in a folder on the card (the
Tonverk lists it among its multi-sampler instruments), or community converters. Users report Elektron's middle C is C3
(SFZ's C4 = 60), and that 44.1 kHz sets are better converted to 48 kHz: here any rate plays (the rate is part of the
playback step).

**Licences.** No third-party parser: `core/msamp/sfz.cpp` (parser), `msamp.cpp` (WAV/AIFF reader, machine, streaming)
and `sdtest.cpp` are written here, GPL-2.0-or-later like the project (the Pi binary is GPL-3 because of Circle). For
reference only, not used: sfizz (BSD-2-Clause, compatible), LinuxSampler (GPL with a commercial exception: avoid).
The test instrument is ours (synthesised, CC0).

## 2. The SFZ subset (`core/msamp/sfz.h`) [measured: tests]

| kind | supported | notes |
|---|---|---|
| headers | `<control> <global> <master> <group> <region>` | inheritance control > global > master > group > region; a new `<global>` / `<master>` resets what is below it. `<curve> <effect> <midi> <sample>` skipped with their opcodes (counted); unknown headers counted as malformed |
| `<control>` | `default_path`, `note_offset`, `octave_offset`, **`sidecar_load=ram\|stream\|auto`** (ours; other players ignore unknown opcodes) | |
| mapping | `sample`, `lokey`, `hikey`, `key`, `pitch_keycenter`, `lovel`, `hivel` | keys as numbers or names (`c4` = 60, `c#4`, `db4`, octaves -1..9) |
| pitch | `tune` (cents), `transpose`, `pitch_keytrack` | |
| level | `volume` (dB), `amplitude` (%), `amp_veltrack` (%), `pan` (kept, unused: the voice channel is mono) | velocity curve: (vel/127)^2 scaled by veltrack x VEL knob |
| playback | `offset`, `end` (`end=-1` drops the region), `loop_mode` (`no_loop one_shot loop_continuous loop_sustain`), `loop_start`, `loop_end` (aliases `loopmode loopstart loopend`) | no `loop_mode`: the file's loop (WAV `smpl`) if any, else none (SFZ's rule) |
| triggers | `trigger=attack\|release` (`first`, `legato` read as attack), `seq_length`, `seq_position`, `lorand`, `hirand` | round robin by a counter per region (as ARIA/sfizz); up to 4 layers a key |
| envelope | `ampeg_attack`, `ampeg_hold`, `ampeg_decay`, `ampeg_sustain`, `ampeg_release` | linear attack, exponential decay / release (-60 dB at the time) |
| syntax | `//` and `/* */` comments, `#define $NAME value`, CR/LF, file names with spaces (`sample` runs to the next opcode or the line's end), `\` in paths | `#include` refused (counted, line skipped) |
| paths | `default_path + sample` relative to the `.sfz`'s folder; a leading `/` = the card's root; `..` resolved, above the root refused | |
| files | WAV (PCM 8/16/24/32-bit, float 32, extensible), AIFF/AIFC (NONE, sowt, fl32); mono or stereo (more: the first two) | held and streamed as int16 mono at the file's own rate (24-bit rounded, stereo summed: the Overbridge return per voice is mono) |
| limits | 1024 regions, 512 samples, 256 KB of text an instrument; 128 instruments listed | |

Not supported (ignored, counted, the first one logged on load): filters (`fil_*`, `cutoff`), LFOs/EGs other than amp,
`group`/`off_by` choke groups, CC conditions (`locc`/`hicc`), crossfades (`xf*`), `sw_*` keyswitches, `rt_decay`,
FLAC/OGG files. The load log line names how many and the first: `... 3 opcodes ignored (first: fil_type) ...`.

## 3. The machine (`core/msamp/msamp.h`)

### Parameters

| | P1..P8 (SRC page) | P9..P16 (page 2) |
|---|---|---|
| A..D | INST (the list entry), VEL (sensitivity), LEV (100 = unity), RTRG (>= 64: release regions on) | TUNE (semitones, 64 = 0), FINE (+-1 semitone), STRT (start offset; a streamed sample's start stays inside its head), LOOP (AUTO / OFF / ON zones) |
| E..H | ATK, DEC, SUS, REL (64 = the instrument's `ampeg_*`, times x 2^((v-64)/16), SUS below 64 scaled down, above toward 100 %) | CHD, INV, SPR, VOI (poly FM's chord params: `core/poly.h`) |

### Notes and voices

- Per voice up to 16 notes (64 at most in all; a voice's own oldest steals first). A note = one region playing: Hermite
  interpolation, the sample rate folded into the step, its own AHDSR. No voice steals another's notes: a note's render
  state is touched by one render core only (cores 1 and 2 split the voices). Sample notes cost far less than FM notes
  (no 16 KB Plaits scratch each), so the pool is separate from poly FM's and bigger.
- A trig releases the voice's earlier trig notes (their tails go on) and starts the chord's notes (CHD OFF = one note);
  each key's regions by key / velocity / trigger, round robin and random. 0x11 releases them; with RTRG on, the release
  regions of those keys start (their note-on velocity).
- Control core -> render core: a sequence-locked mailbox per note (start, release and kill counters); the render core
  reports a finished note. Nothing allocates after boot.
- Live pad chords (test-1031): poly FM's MIDI binding takes MULTISAMPLE voices too. A channel binds to the voice by
  its 0x04s as for FM (`core/poly.h`); the bound pads play `msamp::live_on` notes at once (held; a trig or 0x11 does
  not release them, their note-off does, with RTRG their release regions), the voice's own 0x04 for a pad is swallowed
  (`poly::take_live`), and the window's chord is written back with 0x28 to the track on that voice whose 33 machine is
  MULTISAMPLE (or 6-OP FM; else its first). The Rytm side must accept 0x28 for a MULTISAMPLE track (rytm-mods
  poly-chord answers status 3 for a track it does not count as Sidecar) [inferred: not run on the Rytm].

### Instruments and memory

- **The pools build:** INST (the 14-bit word) is `code = bank * 128 + row` of the INSTRUMENTS list (row 000 OFF in
  every bank; an empty or MISSING row silent; INST's default 0 is A000, OFF). An instrument entry is the `.sfz`'s Ref
  plus one Ref per member sample, read from the `.sfz` on the card core when it is loaded into a row (`READING`, then
  set); a member moved on the card plays from where it is now, a missing one leaves its regions out (the instrument is
  flagged). Rows are filled from the Rytm (FUNC + YES in every bank: the card's `.sfz` files; the POOL page), the
  touchscreen (the POOL view, FILES), the web (Files: **+INST**; the Pool tab) or the console (`pool=inst:...`); the
  auto-sampler's instruments and the sampler's slice maps go in by themselves (the bank last shown). No FILL for
  instruments.
- **Memory in the pools build:** the heads (and RAM-mode samples) are entries of the shared pool RAM
  (`core/pool/pcache.h`, `poolram=MB`): a head is a prefix entry, a whole entry another machine holds serves it without
  a stream, and a loop part past the head is a private block counted in the same budget. 8 instruments are resident at
  once (`kSlots`); an instrument's cap is the whole pool RAM. Heads load on use: the INSTRUMENTS list is not preloaded.
  The `msampram=` arena below is no longer taken (`msamp_ram_total` has no caller) [measured: the code].
- Before the pools build: `/MULTISAMPLE/*.sfz` and `/MULTISAMPLE/<folder>/*.sfz` (one level; `.`-names hidden), sorted
  by path, INST = entry; any change under `/MULTISAMPLE` rescanned the list. `msamp=rescan` now only tries failed
  instruments again.
- Before the pools build: one arena from the board at boot, 8 equal instrument slots: a slot is the per-instrument cap. Pi: `msampram=MB`
  (default 512) from Circle's **high heap** (above 1 GB; the Pi 4 here has 4 GB [measured, Pi, earlier]) less 64 MB,
  else the low heap less 192 MB (a 1 GB Pi). 512 MB = 8 x 64 MB [inferred: not run on the Pi]. Plus 16 MB of stream rings
  (128 notes x 64 K frames) and ~1.6 MB of tables. The boot log says `msamp: 512 MB for instruments, 8 slots of 64 MB`.
- A slot is reused least recently used, never while a voice selects it or a note plays from it. A trig before its
  instrument is loaded plays when it is, if within 250 ms; later ones are counted as late.
- Load log: `msamp: loaded /MULTISAMPLE/SidecarTest/SidecarTest.sfz (RAM): 11 regions, 11 samples (0 not loaded),
  1197 KB in RAM of 65536 KB (whole: 1197 KB), N ms; 0 opcodes ignored, ...`. A sample over the cap: `... left out`.

## 4. Streaming from SD

### The model

| part | what | sizes |
|---|---|---|
| head | each sample's first `head_ms` (default 500, `msamp=head:MS`), int16 mono, preloaded at open | 47 KB a sample at 48 kHz; a sample at most 25 % longer than its head is held whole |
| loop part | a loop past the head is preloaded too (up to 1 M frames = 22 s at 48 kHz) | so loops never stream; a longer loop plays unlooped (counted `loops_dropped`) |
| ring | per note, 64 K frames (128 KB), filled from the card in reads of up to 32 KB (the source's own format, converted) | 0.68 s at 2x 48 kHz |
| read-ahead | a stream is topped up while it has less than `ahead_ms` ahead (default 300; the SD test sets it) | head time counts |
| streams | at most `max_streams` notes stream at once (default 32; the SD test sets it); beyond: the oldest released streaming note goes, else the oldest | |

Opening an instrument = the `.sfz` text, one header read a sample, then the heads (and loop parts). The ring holds
"path frames": the frames after the head, the preloaded loop part left out, so reading and writing never alias.
`sidecar_load=ram` / auto (whole <= 8 MB) loads every sample whole; refused with a warning, and streamed, when it does
not fit (the pools build: the pool RAM).

**Underruns.** A note whose ring runs dry outputs silence for the missing frames with a 1 ms fade out and in, keeps its
time (the writer skips ahead to where the note reads), and counts an underrun (note-blocks, in `msamp` and the stats
line). A note's start that is not in yet (an SFZ `offset` past the head) waits instead.
**Bandwidth stealing.** When the neediest stream is below 30 ms ahead, the oldest released streaming note is killed
(one per 30 ms); below 10 ms, the oldest held one too. Counted (`bandwidth steals`).

### Where it runs on Circle [measured: source; inferred: behaviour]

- The card work runs on **core 3** (the control core) between blocks, in `core::idle_stream()`: the SD test step,
  then a MULTISAMPLE stream read or load step, then the DSP SAMPLE streams. One card operation a call. The render cores
  (1, 2) never touch the card: they read RAM (heads, loop parts, rings) only.
- Circle's EMMC driver (`addon/SDCard/emmc.cpp`) transfers by **PIO with busy-waits** (`TimeoutWait` on the interrupt
  register per block, the CPU copies the words) [measured: source]; a 32 KB read occupies core 3 for the whole transfer.
- FatFs is built re-entrant (`FF_FS_REENTRANT 1`) with a `CGenericLock` per volume, a **spin lock** without
  `NO_BUSY_WAIT` [measured: source]. The recorder writes 192 KB at a time from **core 0** (`rec_pi::Poll`), worst write
  100-137 ms [measured, Pi, earlier]: a stream read on core 3 that comes during such a write spins until it ends. The
  read-ahead must cover that (the SD test measures it: the "mixed" phase), and core 3's other work (MIDI, SysEx)
  waits meanwhile [inferred].
- Done on branch `sampling` (docs/SAMPLING.md "One card-I/O queue", `core/msamp/cardq.h`): the stream opens and reads
  are posted by core 3 and run by core 0's loop before the recorder's writes, which are now 64 KB pieces held back while
  a stream needs the card; uploads last; `cardq=0` keeps the path described above. Loads and DSP SAMPLE's refills stay
  on core 3.

### Throughput and voice counts

Per streaming note at unity pitch: stereo 24-bit 48 kHz = 288 KB/s (9 reads of 32 KB a second), stereo 16-bit 192 KB/s,
mono 16-bit 96 KB/s; double one octave up. The voice counts the test derives (60 % of the card's time for streams):

| card | figures | voices alone | with the recorder (13 files, 1.97 MB/s) |
|---|---|---|---|
| the Pi's card **if** it reads as the Teensy's did | 22.9 MB/s sequential, 4 KB reads avg 0.78 ms, worst 3.96 ms [measured, Teensy SDIO, `docs/sampler-plan.md`] | ~40 stereo 24-bit (throughput bound) | depends on the write stalls |
| the Pi's card, recorder side | writes 9.2 MB/s inside 192 KB writes, worst write 100 ms (13 files), 137 ms (23 files) [measured, Pi, earlier] | - | the write share 21 %; with 32 KB reads of ~2 ms and 4 ms under writes: **~10** stereo 24-bit voices, read-ahead 235 ms [inferred: `judge()` on these figures, a test case] |
| a fast A2 card (40 / 80 MB/s, 0.8 ms reads) | [inferred] | 64 (the pool's cap) | ~50 |
| a slow card (3 / 8 MB/s, 8 ms reads, 400 ms stalls) | [inferred] | 8 | 0: "OK without the recorder" |

Pi read figures under Circle are **not measured yet**: that is what `sdtest` is for (run it once on the Pi; the log
line and `/.rytm/sdtest.txt` give the numbers to put here). Pi 5: the same driver path (Circle's EMMC on the Pi 5's
SDHCI), no numbers [inferred].

### Open time [measured: Mac; inferred: Pi]

- The test instrument (11 samples, 1.2 MB): RAM automatically; on the Pi about 11 header opens + 11 data opens (~3 ms
  each, the Teensy's open + header figure) + 40 reads of 32 KB (~1.5 ms) = **~0.13 s** [inferred].
- A large library, e.g. a 480-sample piano (16 velocities x 30 keys, stereo 24-bit 48 kHz, ~1.9 GB as files): STREAM
  opens 480 headers + 480 heads of 0.5 s (144 KB of file each, 5 reads) = 480 x (3 + 3 + 7.5 ms) = **~6.5 s**, 22 MB
  of heads; RAM would need ~1 GB as int16 mono and ~95 s at 20 MB/s (refused by the 64 MB cap) [inferred]. `head:250`
  halves the head time.

### Tests [measured]

`core/tests/test_msamp.cpp` (Mac): the streamed SidecarTest (heads + preloaded loop parts) equals the RAM one sample for
sample, with release regions. A simulation instrument (four 10 s stereo 24-bit samples, RAM 3.7 MB vs STREAM heads
187 KB) played as MAJ7 chords on 4 voices (16 held + 16 releasing streams, peak 32):

| card model | result |
|---|---|
| none (every step at once) | bit-exact with RAM, 0 underruns, 35 MB streamed |
| 12 MB/s, 0.3..3 ms a read, a 100 ms stall every 2 s (the recorder) | **bit-exact with RAM, 0 underruns, 0 steals** |
| 2 MB/s, 5..20 ms a read, 250 ms stalls every 1 s | underruns (88 811 note-blocks) and 46 bandwidth steals: reported, as intended |

## 5. SD card test (`core/msamp/sdtest.h`)

- **What it measures**, through the engine's own card path (core::File = FatFs + Circle's EMMC on the Pi), on core 3
  between blocks, one operation a step: sequential write (64 KB writes) and read (64 KB), random reads of 4 / 32 / 64 KB
  (200 each, IOPS, p50 / p99 / worst), and a mixed phase: random 32 KB reads while 192 KB writes go to a second file
  (the recorder's pattern), the reads' worst latency.
- **Files**: `/.rytm/sdtest.bin` (min(256 MB, a quarter of the free space), at least 32 MB; refused below 64 MB free:
  Circle's FatFs has no page cache, so 256 MB is not cached [inferred]) and `/.rytm/sdtest2.bin`; both removed at the
  end, on a cancel and before a new run. Nothing else is touched.
- **Verdict** (`judge()`): voices = min(0.6 / (9 x mean 32 KB read time), 0.6 x read MB/s / 0.288); with the recorder,
  the same on 0.6 - (1.97 MB/s / write MB/s) and the mixed reads' mean. Read-ahead = 1.5 x the worst read (with the
  writes) + 30 ms, 100..600 ms; a worst read the 600 ms cannot cover makes it "too slow". Levels: "OK for N stereo
  streaming voices + recorder (M without)", "OK for N ... without the recorder (M with it)", "too slow for streaming
  (N voices): use RAM mode or a faster card (A1/A2, U3/V30)". Stored in `/.rytm/sdtest.txt` (key=value), read at
  boot, given to the streaming engine (`ahead_ms`, `streams`).
- **UI**: console `sdtest` (the last result), `sdtest=start` (refused while MULTISAMPLE notes play or a take records;
  `sdtest=start!` runs anyway: audio may drop), `sdtest=cancel`; the web page's Status tab "SD card speed" (Run test
  with a confirmation, progress, Cancel, verdict, figures; `/status` `"sdtest":{...}`); the Rytm's remote SYSTEM page
  shows a verdict item (`SD OK 10` = streams with the recorder, `SD 23 NR` without, `SD SLOW`, `SD 40%` while it
  runs, `SD -` untested). Starting it from the remote page is not wired yet (the SYSTEM page takes no keys).
- **Log**: `sdtest: write 10.0 MB/s, read 14.2 MB/s, 32 KB reads 358 IOPS p99 3 ms worst 3 ms, with writes worst 123 ms:
  OK for 23 stereo streaming voices without the recorder (6 with it), read-ahead 215 ms` (a fake card's figures).
- **Tests** `core/tests/test_sdtest.cpp`: judge() for a fast A2 card, the Pi's recorder figures, a slow and a far too
  slow card and a fast card with a 500 ms worst read; the runner on a fake card with set speeds (phases, figures near
  the fake's, the stalls only in the mixed phase, the result file, cleanup, cancel, too little room, a small card's
  size, a slow card's verdict, load()).

User docs draft (for the Starlight site): "**Is my SD card fast enough?** Open sidecar.local, Status, SD card speed,
Run test (about a minute; do not play meanwhile). Green: stream multisamples while recording. Orange: stream, but not
while recording. Red: load instruments into RAM (`sidecar_load=ram` in the .sfz) or use an A1/A2, U3/V30 card. The
result is kept on the card; run it again after changing cards."

## 6. Built: the PoC

| part | file |
|---|---|
| SFZ parser, paths, note names | `core/msamp/sfz.{h,cpp}`, `core/msamp/text.h` (own number/char helpers: the Pi's hosted objects have no ctype/strto*) |
| WAV/AIFF reader, instruments, notes, streaming, console | `core/msamp/msamp.{h,cpp}` |
| SD card test | `core/msamp/sdtest.{h,cpp}` |
| hooks | `core/voices.cpp` (machine flag `kMultiMachine` 0x30 in `machines.h`, `msamp=voice:V` override, report / note-off / silence / render), `core/engine.cpp` (begin, idle), `core/ui/remote.cpp` (SYSTEM item) |
| platform | `core/platform.h` `msamp_ram_total/alloc`, `card_free_bytes` (Pi: `platform_pi.cpp`, high heap, `f_getfree`; Mac: `platform_host.cpp`, `CORE_MSAMP_RAM_MB`) |
| Pi | `bridge.cpp` (console `msamp`/`sdtest`), `cores.cpp` (sdtest busy = a take records), `kernel.cpp` (`/status` sdtest), `web.cpp` (rescan on `/MULTISAMPLE` changes), `www/index.html` (SD card speed) |
| Mac host | `core/host/core_c.cpp` `core_c_msamp_command`; `sim/poly_live.py --msamp 0` |
| test instrument | `tools/make_multisample_test.py` -> `content/MULTISAMPLE/SidecarTest/` (5 keys x 2 layers, loop_sustain with exact-period loops in `smpl`, `tune` per zone, a 44.1 kHz stereo 24-bit release breath; CC0, 1.3 MB) |
| tests | `core/tests/test_msamp.cpp`, `core/tests/test_sdtest.cpp` |

### test-1031: the Rytm's machine, the SRC view

- The voice byte's 0x30 (`kMultiMachine`) plays MULTISAMPLE: voices.cpp already took it; now the remote UI does too
  (33's tracks, `machine_of_state`, the SIDECAR page's "MSMPL"). `msamp=voice:V` stays as the fallback until the
  Rytm's 0021 reports 0x30 (the SRC view shows MULTISAMPLE for a Sidecar track on V then).
- The SRC view's MULTISAMPLE page (`core/ui/page_gfx.cpp`): INST as the instrument's name (the card's list entry,
  `msamp::entry_name`) with its place in the list, LEV, RTRG (OFF / ON), the ATK DEC SUS REL envelope. The Rytm's
  own INST value list is not sent (no host -> Rytm list for MULTISAMPLE exists in 0021 yet, as 16 / 17 do for
  JUNO / FM): the name is on the page.
- With a chord on (CHD, or the pads played one), 6-OP FM and MULTISAMPLE show the chord in the bottom row: the
  root's name with its octave and the chord (C4 MAJ: C4 = note 60), INV SPR VOI, a 3-octave keyboard with the chord's
  notes and each octave's C named under it, the voice's notes sounding now (LIVE while the pads play). The keyboard's
  window is the voice's own and stays put while the notes fit in it (a lone pad note shows in its own octave). `core/tests/test_chord_view.cpp` renders it to
  `core/build/rui_chord/*.png`.

### How to try it

- Pi: copy `content/MULTISAMPLE` to the card's root (or the web Files tab: Upload folder into `/MULTISAMPLE`). On a
  track, pick a Sidecar machine (6-OP FM: its page 2 E..H are already labelled CHD INV SPR VOI by the poly-chord mod),
  then on the console `msamp=voice:V` (V = the track's voice, 0 = BD). Load an instrument into a row
  (`pool=inst:set:A001:/MULTISAMPLE/SidecarTest/SidecarTest.sfz`, or +INST in the web Files tab); P1 (FM's PAT)
  selects the row. `msamp`
  shows the stats, `msamp=list` the instruments, `sdtest=start` the card test.
- Mac + emulator: `make -C core lib`, `uv run python sim/poly_live.py --msamp 0 --wav OUT.wav` (a temp card with
  `content/MULTISAMPLE`), then rytm-emu with `--usb-host --usb-midi-real --ob-link`.
- The Rytm-side machine (name, labels, value texts INST names) is a rytm-mods 0021 change: the machine flag 0x30 is
  reserved for it here.

Emulator run: "Measurements" below.

## 7. Auto-sampler (the design; built on branch `sampling`: docs/SAMPLING.md, `core/samp/autosamp.h`)

Goal: make a multisample automatically: play notes, record them, trim, loop, name and write `WAV + .sfz` into
`/MULTISAMPLE/<name>/`, ready for INST.

### Sources

| | source | trigger | record from | notes |
|---|---|---|---|---|
| (a) | an external synth | MIDI notes from the Sidecar: a USB MIDI device on the Pi's USB-A (Circle's USB MIDI host driver), or the Rytm's MIDI OUT (the Rytm forwards USB MIDI to DIN when its MIDI port config sends USB to OUT [inferred: needs checking in the Rytm's MIDI routing]) | the Rytm's AUDIO IN L/R, which already arrive at the host in the Overbridge stream (IN 10/11) | the synth's audio into the Rytm's inputs |
| (b) | the Rytm's own analog voices / kits | a note on the track's MIDI channel (the Rytm plays its track from MIDI notes on the auto/track channel), or a host-link trig (needs a mod message "trig track T note N vel V"; the MIDI path works on a stock Rytm) | the per-track Overbridge outputs (BD..CB, IN 2..9) or MAIN L/R | per track, so all 12 tracks of a kit can be sampled in one pass (one note per track per round) |
| (c) | the Sidecar's own voices (e.g. a JUNO patch) | the engine directly (a 0x04-equivalent call on a free voice, no Rytm) | the voice's rendered block (the recorder's VOICE1..8 sources) | runs faster than real time if wanted (offline render), no latency |

### The take

- **Plan**: note range (lo..hi) and step (every n semitones: 1, 3, 4, 6, 12), velocity layers (1..8, values spread or
  given), round robins (1..4), note length (hold) and release tail capture (max tail s), gap between notes.
- **Record**: reuse the recorder's ring and writer (`pi/engine/recorder.*`: IN channels to SD at 24-bit, sector-aligned),
  one take per (note, velocity, rr), from note-on minus a pre-roll to note-off plus the tail; or one long take with
  markers (simpler for the ring; cut afterwards).
- **Latency**: measured once per session with a click (send a note to a test patch or loop the Rytm's output to its
  input), or known: Overbridge IN round trip ~6 ms [measured, Pi, earlier: "about 6 ms" in docs/pi-engine.md] + the
  synth's own; the cut starts at the detected onset anyway.
- **Silence detection / trim**: onset = first frame above -50 dBFS (or the noise floor + 12 dB, measured in the
  pre-roll) minus 2 ms; end = last frame above the floor, then a 10 ms fade. A take with nothing above the floor:
  flagged ("no sound"), skipped.
- **Gain**: a level check pass (the loudest planned note) suggests input gain; normalise per instrument (one gain for
  all, so velocity layers keep their relation), peak to -1 dBFS; never per sample unless asked.
- **Loops** (sustained sounds): search a window in the sustain part for loop points: candidate ends at zero crossings
  with the same slope, length a whole number of detected periods (autocorrelation of the sustain), score by the
  waveform difference across the seam (and RMS match); write the best as SFZ `loop_start/loop_end` + the WAV `smpl`
  chunk, `loop_mode=loop_sustain`; optional crossfade loop (rewrites the sample around the seam).
- **Mapping**: each sampled note is a zone's keycenter; zones split halfway between neighbours; velocity layers split
  between the recorded velocities; round robins as `seq_length/seq_position`.
- **Naming**: `/MULTISAMPLE/<name>/<name>.sfz`, samples `samples/<name>_<note name><octave>_v<vel>_rr<n>.wav` (e.g.
  `Juno_C4_v100_rr1.wav`; SFZ note names, C4 = 60); the .sfz names its source and date in a comment.
- **UI**: the remote UI page "SAMPLE" (source, range, step, layers, length, START / CANCEL, progress "C4 v100 3/24",
  the level meter), the web UI (a form with the same fields, progress over `/status`, the result's link into the Files
  tab), console `asamp=...`. Cancel stops after the current note, keeps nothing (or "keep so far").
- **Output**: 24-bit 48 kHz WAV (mono for Rytm tracks, stereo for IN L/R and MAIN), the .sfz; the instrument goes
  into the INSTRUMENTS POOL list (the first free row of the bank last shown, a moment later: its .sfz is read on the
  card core) and is selectable from that row.

### Effort and order [inferred]

| step | days |
|---|---|
| (c) internal voices: offline render, trim, normalise, mapping, WAV + SFZ writer, console | 2 |
| loop finder (autocorrelation + seam score) and tests on synthetic tones | 1.5 |
| (a) external synth via USB MIDI on the Pi (Circle's MIDI host) + IN L/R recording through the recorder ring, latency | 2.5 |
| (b) Rytm tracks via MIDI notes, per-track outputs, a kit in one pass | 1.5 |
| remote UI page + web form + progress/cancel | 2 |
| hardware testing (a synth, the Rytm), tuning the thresholds | 2 |
| total | ~11.5 |

Order: (c) first (no hardware, proves the writer and the loop finder), then (b) (the Rytm is always there), then (a).

## Measurements

| what | result |
|---|---|
| `make -C core test` | ALL CORE TESTS PASS, including test_msamp and test_sdtest [measured, Mac] |
| Pi 4 kernel (`pi/engine/build.sh kernel`, PI_IP 169.254.61.60/16, EMUHOST 169.254.61.59:7301) | builds [measured] |
| Pi 5 kernel (`RASPPI=5`) | builds [measured] |
| emulator: rytm-emu (worktree rytm-emu-polychord, `fw/AR173_us_rui1030poly.syx`, `sections/us_rui1030poly.snap`, a copy of its card), `dtboot --usb-host --usb-midi-real --ob-link`, presses SRC SRC DOWNx23 YES (BD = PLAITS), BD twice, CHRO, BD RS CH, then BD+RS+CH 1 ms apart; host `sim/poly_live.py --msamp 0 --wav` | the first trig waited for SidecarTest (loaded in 44 ms of emulated time, RAM, 1197 KB), then played; 8 trigs, 14 notes, peak 3 at once (the pad chord: three 0x04s, each releasing the one before, the tails overlapping), 0 underruns. The 57 s WAV: 261.6 Hz for BD (note 60), 293.7 Hz for RS in CHRO (62), 415.3 Hz for CH (68), each exactly on pitch [measured]. Chords from CHD were not exercised there (PLAITS' word 21 = 0 = OFF); they are in test_msamp |

Not measured: anything on the Pi or a real Rytm; the Pi's SD read figures (run `sdtest`); the high heap's free size on
the Pi 4 4 GB under this kernel (the boot log line says it).
