# SRC-BANKS: 8 banks x 128 sources for GRANULAR (SRC), WAVETABLE (TBL) and MULTISAMPLE (INST)

> **Superseded for the host side by `docs/POOLS.md`** (the pools build, 2026-10-22). Still valid below: the 14-bit word
> (section 1), the Rytm side (rui1038, rui1040: the list, 4A / 47 / 48, FUNC + YES), the song recorder's `src` events
> and fix1041's card-core loading (section 7). Gone on the host: the per-machine pool files, bank A's composed list
> (LIVE / BUILT-IN / pool / scans / TAKES / LIBRARY), the touchscreen's picks over P1 and the `lib=gr|wt|ms:` commands.
> Each machine now plays a row of its project's POOL list (SAMPLES / WAVETABLES / INSTRUMENTS), bank A like B..H with
> row 000 OFF, and FUNC + YES works in every bank. The rest of this file is kept as the history of the banks work.

Built 2026-10-21: rytm-mods `rui1038` (0026 + 0029, design.md "1038") and this branch (`banks`). Tags: **[measured]** run
in rytm-emu (seq-p2, DTEMU_STRICT=1) or in this repo's tests; **[inferred]** reasoned, not run. Stock = Analog Rytm MKII
OS 1.73. Nothing of Elektron's code is used: the Digitakt 2 / Tonverk were studied for behaviour (the list's look and
keys) only, the Rytm only through its own runtime calls.

## In short

- **One parameter, 14 bits.** P1 of the three machines holds `index << 8 | bank << 5`: index 0..127 in the integer
  byte, bank 0..7 (A..H) in the fraction's top 3 bits. On the wire (0x03 / 0x04 reports, 2A, 48) `hi = index`,
  `lo = bank << 4`. The host's **source code** is `bank * 128 + index` (core/lib/srclib.h `code_of`, `code_of14`).
- **Bank A is the list as it was** (every existing kit and project reads the same source): GRANULAR's LIVE and
  BUILT-IN entries, then its pool slots (SRC 12 + s), the scanned entries and the LIBRARY window; WAVETABLE's built-ins
  then TBL 6 + s; MULTISAMPLE INST s. **Banks B..H** are fixed rows: row 0 is OFF (silence), rows 1..127 are slots of the
  project's pool file (`/.rytm/<machine>/<PROJECT>.pool.txt`, line `128 * bank + row`); an empty row is silent too.
- **The Rytm only selects; the touchscreen and the web fill.** Turning knob A on the Rytm's SRC page (when the Sidecar
  draws it) opens a list: tabs `< A B C D E F G H >`, rows `000 OFF`, `001 name` ...; LEFT / RIGHT change the bank, the
  knob and UP / DOWN the row, YES chooses, NO cancels. With trigs held in GRID REC, YES locks the choice on those steps.
- **Saved p-locks keep the bank** (P1's record has the fine flag: stock saves a second, fine lock sequence) [measured].
- **The song recorder records SRC / TBL / INST at 14 bits** from the Sidecar's own reports (2A, its 48 writes), never
  from the Rytm's CC / NRPN, and replays it with 48 (as a knob turn: the kit keeps it).

## 1. Why one word (the research, condensed)

The Rytm's stock sample list (SMPL page, knob D) is one list of 128 entries (OFF + the project's 127 slots), no banks
[measured]. Every Rytm parameter is a 16-bit 8.8 word; a saved p-lock keeps 7 bits, or 14 with the record's fine flag
(+0x14, as stock STA / END) [measured, the serializer]. The Digitakt 2 keeps bank and slot as two parameters with the
bank set from the list; the Rytm has no free per-sound word, so the same pair goes into one word. 1024 = 10 bits fit the
14.

The four checks done before building [measured, rytm-mods 0026 design.md "1038"]:

| check | result | consequence |
|---|---|---|
| a coarse knob's step keeps the fraction? | stock: yes (0x05a0 -> 0x07a0); 0029's stepped path: no (-> 0x0600); the fine flag does not change the step | 0029 got a keep-the-bank rule (ext_steps bit 11); all three knobs step one index a detent |
| a fine-flagged lock over power-off + cold boot | P1's slot + the fine slot (0x80 0x80, 0x50) saved; the lock back at 0x05a0; without the flag 7 bits only | P1's record gets the fine flag |
| what stock sends on MIDI | NRPN mode: MSB = index, LSB = the fraction's top 7 bits (the bank survives); CC mode: CC 16 = the index only | the recorder ignores both for these machines and uses the 14-bit reports |
| the page's text with a fraction | the name / `F002` / `OFF`, never `index.fraction` | - |

## 2. The Rytm side (rui1038)

- **Record / knob:** P1's shared record: fine flag 1, range 0..0x7fe0 (bank H's 127). Knob A: 0029 mode 3 (all 128) +
  bit 11: one index a detent, the bank kept (also in a lock made by a turn with a trig held).
- **Names (47):** `47 01 <list> <replace> <total> <bank> {<row> <13 bytes>} x 0..8` (the old form without the bank byte is
  bank A). The Rytm holds one bank's names a list; the knob text: bank A as before; B..H index 0 `OFF`, the held name of
  that row, else `B005`.
- **The list (4A):** `4A 01 <track> <on> <hi> <lo> <why>` Rytm -> host: on 1 opened by knob A's first turn (it moves
  nothing, as stock's SAMPLE SLOT list), on 0 closed: why 1 YES, 2 NO, 3 another knob, 4 another key, 5 the view ended.
  While open knob A's turns go to the host (32 0), LEFT RIGHT UP DOWN too (31). Only while the host draws the SRC view
  (an old host / no Sidecar: the knob is stock's).
- **Commit (48 + flags):** `48 01 <track> 0 <index> <bank << 4> <flags 1>`; with trigs held at the YES (GRID REC) the Rytm
  locks P1 on those steps (answer status 4; 3 the pattern's 72 lock slots full), else it writes the kit (status 0).
- **UP + T6..T10** -> `49 01 01 6..10` (stock binds nothing there [measured, 5 contexts]).

## 3. The host side (this branch)

### Pool and lists (core/lib/srclib)

- The pool file keeps bank A's slots in lines 0..31 (unchanged) and banks B..H in lines `128 * bank + row`
  (kPoolLines = 1024; the file grows only as far as its last used line). Slots never move; filling never renumbers.
- `compose_banks` appends banks B..H to each machine's list: entries 0..127 bank A (holes past its composed end),
  entry `128 * bank + row` bank B..H (row 0 `OFF` = kLibOff, a file = a playable entry, else a hole). `entry_of(code,
  na)`: bank A's index clamps to the composed list (P1 past its end plays the last, as before), banks B..H map 1:1.
- The machines (granular.cpp, wavetable.cpp, msamp.cpp) select by the code: `srclib::code_of(p[0])` (the report's
  integer + fraction / 128), picks keep their base as a code, an OFF / empty bank row is silent (a trig plays nothing),
  a pick of such a row only writes P1. Files load into RAM only when picked or used (the LRU slots: 4 / 4 / 8); the
  bank list's cursor resting 300 ms previews (and so loads) GRANULAR / WAVETABLE rows.
- Console: `lib=K:fill:B:/Folder` (the folder's files, name order, into the bank's empty rows; a file already in the
  bank keeps its row), `lib=K:set:B:R:/path`, `lib=K:clear:B:R`, `lib=K:assign:B:R:I` (the browser's item I),
  `lib=K:fill:B` (the browser's folder), `lib=K:bank:B` (K: gr wt ms; B: B..H).

### The Rytm's list (core/ui/page_wavegrain.cpp, remote.cpp)

- 4A opens it for the track (`src_bank_list`); the SRC view then draws the bank page (`src_bank_page`): the tabs line,
  five rows around the cursor (`%03d name`, the cursor inverted with its LOADING / length note), a scroll bar. Keys
  (`src_bank_key`) and knob A (`src_bank_enc`: one row a count, FUNC x 8) move the cursor; why 1 commits it with
  `remote_track_write(t, 0, row, bank << 4, 1)`. After a commit the old SOURCE list does not flash.
- 47: `src_names_tick` sends the bank the Rytm needs: the open list's bank, else the bank of the SRC view's P1 (2A); bank
  A in the old form (older firmware keeps working), B..H with the bank byte and only the rows that hold a file.
- 2A's fraction is kept (`srcl`): `TrackSource::code` gives the pages the 14-bit value; `Source::code` (default) the
  voice's 14-bit report.

### Touchscreen and web

- **Touchscreen (pi/ui/sidecar_wgview.c):** SOURCES opens the banks: a tab row A..H that stays on top, then the bank's
  rows (`000 OFF`, `001 name`, empty `---`). A tap on a row with a source picks it (the Rytm's P1 gets the bank). A tap on
  an empty row of B..H opens FILES (the card's folders, a plain browser) aimed at that row: a file goes into it; **FILL
  B** fills the bank's empty rows from the folder. Bank A's FILES keeps the old behaviour (into the pool, picked). The
  provider (pi/engine/wgview_pi.cpp) fills the shown bank's names and states. MULTISAMPLE (touch2) has the same list
  in the machine view (a MULTI voice on the home page, or the VOICE page's MULTISAMPLE panel): INST's instruments in
  bank A, `.sfz` files in FILES, `msamp=pick:V:E` / `msamp=lib:V:I` and `lib=ms:...`.
- **Web (Files tab):** a folder row has **Fill bank** (asks "gr C" / "wt D" / "ms B"), the files keep +GR / +WT / +MS
  (bank A's pool).

### Everything that tracks the playing source uses 14 bits

- song recorder (core/song): a new event `src` (`{"trk":t,"v":<index << 7 | bank << 4>}`) recorded from
  `ui::remote_track_src14` (the SRC view's 2A and the host's own 48 writes), polled every 10 ms; the Rytm's NRPN 1:0 and
  CC 16 on a track whose machine is one of the three are not recorded; replayed as 48 (the kit keeps it).
- the remote pages (SRC view, the MULTISAMPLE INST name), the Pi VOICE panel's INST name, the touchscreen's list: the
  code, never the 7-bit value alone when the bank is known.
- Not changed: the SEQ page's lock pages read P1 as 7 bits (`remote_track_param`); a host-sequenced SRC lock keeps bank
  A [inferred: not used with banks yet].

## 4. Tests [measured]

- rytm-mods: `tools/emu/bk_emu.py rui1038 all` (knob, range, text, lock + power-off, list, listlock, upt, nrpn),
  `rled_emu.py rui1038 stockup`, the 14 suites.
- this repo: `make -C core test` (test_srclib `test_banks`: code mapping, fill / set / assign, the pool file's lines, a
  voice on C002 loads and plays, OFF and an empty row silent, MULTISAMPLE B001 and WAVETABLE D009; test_song `test_src`;
  test_ui the 4A page, keys, 47 with the bank byte, the commit's 48), `node --test pi/engine/tests/page_pool.test.mjs`,
  `make -C pi/ui/host shot.png` (the bank tabs, FILES aimed at a row, FILL), `sim/rui_e2e.py srclist` (the real core
  against rui1038 in rytm-emu).

## 5. Deviations from the approved design, and limits

- **Bank A row 000 is not OFF.** The decision said "index 0 = OFF in every bank"; bank A keeps today's entry 0 (GRANULAR
  IN, WAVETABLE's first built-in, MULTISAMPLE pool slot 0) so that every existing kit plays what it played (and new
  WAVETABLE / MULTISAMPLE tracks, whose default is 0, are not silent). Banks B..H have 000 OFF.
- The list opens only while the Sidecar draws the SRC view; without it knob A is stock's (one index a detent, bank kept).
- Locks from the list need GRID REC (the held-trig bitmap); steps past 16 [inferred] by the bitmap's absolute steps.
- MULTISAMPLE: no list preview. (touch2 added its touchscreen list: the machine view, docs/pi-display.md "MULTISAMPLE".)
- Names on the Rytm: one bank a list at a time; a bank the Rytm has no names for reads `B005` until the host sends it.
- Preloading the files of every kit's and pattern's locks at project load is not done: a file loads at its first
  selection, report or preview (the Pi's slots: 4 / 4 / 8).

## 6. rui1040: FUNC + YES - a file into a bank row from the Rytm; the tabs (2026-10-21, emulator only)

- **FUNC + YES on a row** of the Rytm's bank list (banks B..H, rows 001..127) opens the card's files on the Rytm's
  screen, aimed at that row: the header `C001 /folder`, the folders (`NAME/`) then the files the machine reads (srclib's
  library browser: GRANULAR / WAVETABLE .wav .aif .aiff; MULTISAMPLE .sfz), five rows, the cursor inverted with OPEN /
  SET. Knob A or UP / DOWN move, RIGHT or YES enter a folder, YES on a file sets the row (`srclib::bank_set`: the pool
  file's line 128 * bank + row; loaded on use), LEFT or NO go up a folder and at the top back to the list. After a set
  the list is back on that row with the file's name (`SET C001` in the tabs' place for 2 s); YES then chooses it as
  usual. A file resting 300 ms is previewed (GRANULAR / WAVETABLE) when the machine's list has it: its LIBRARY window
  lists the same browser's folder.
- Bank A and row 000 are refused (`BANK A: TOUCHSCREEN`, `000 IS OFF`): bank A is the composed list (built-ins, pool,
  library). A MULTISAMPLE "file" is an .sfz instrument; folder instruments (a folder of WAVs without an .sfz) are not
  offered, as on the touchscreen.
- The browser is the machine's own (shared with the touchscreen's FILES and the list's LIBRARY window): entering a
  folder on the Rytm moves those too.
- Wire: Rytm -> host `4A 01 <track> 3 <hi> <lo> 0` (FUNC + YES); host -> Rytm `4A 01 <track> <1 open / 0 closed>` (YES /
  NO the host's while open; the Rytm gives them back after 1 s without an answer). rytm-mods 0026 "1040".
- **The tabs:** each bank letter is its own Rytm text item at a fixed 9 px pitch, the selected one in a filled box with
  its letter inverted; rui1039's one-string row with an overlaid inverted letter put the box on the next letter (the
  Rytm's glyphs are narrower than the host's 5 px estimate) [measured, rytm-emu screenshots].
- Tests: core/tests/test_ui.cpp (tabs, FUNC + YES -> 4A 0 1, NO back -> 4A 0 0, bank A refused), `sim/rui_e2e.py
  srclist srcbrowse` (the real core against rui1040: C001 = /Banks/beta.wav, then committed), rytm-mods `bk_emu.py
  browse`.

## 7. fix1041: loading a sample never stops the audio (2026-10-21, host model + Mac tests)

**The report (rui1040 + fix1040 on the Pi):** all sound stops briefly at YES on a file in the bank list's browser
(FUNC + YES, then YES: the file into the row), not while browsing or previewing.

**Root cause [measured on the host card model; the Pi itself not run].** The YES runs on the Pi's control core (core 3:
the Rytm's MIDI, the UI, the card work between blocks), which also renders every audio block, and the output FIFO holds
96 frames: any pass longer than 2 ms is silence (docs/pi-engine.md "FIFO target"). The commit,
`srclib::bank_set`, wrote the machine's pool file there and then: its folders made, the file opened for writing
(truncated), up to 1024 lines written, closed (FatFs's f_sync: the data sectors, the FAT, the directory entry) - with
Circle's EMMC driver busy-waiting and FatFs holding the volume's lock for each whole call. The row's file then loaded
on the same core (an open + header read, then 32 KB reads, one a step).
`core/tests/test_loadstall.cpp` models the card's time per operation (core/host `host_card_model`: an open 1 ms, a
truncate or a sector written 2 ms, reads at 20 MB/s + 0.1 ms a call, a directory entry 0.1 ms [inferred from the
measured figures: an open + header ~3 ms and 32 KB reads ~1.5-2 ms (docs/MULTISAMPLE.md), a DSP SAMPLE refill 1.2 ms
a pass (docs/pi-engine.md); a card's own write pauses (100-137 ms inside the recorder's writes) are not modeled]) and
runs the commit and the load as the control core's passes:

| | longest control pass | passes over 2 ms | silence | card time on the control core |
|---|---|---|---|---|
| before, 1 MB WAV (C001; the first write also makes `/.rytm/granular`) | 24.7 ms (21.5 ms the pool file) | 2 (the write; the open + header) | 23.6 ms | 80 ms |
| before, 10 MB WAV (C002, stereo 24-bit) | 11.1 ms (9.0 ms the pool file) | 2 | 9.9 ms | 568 ms |
| after, 1 MB | 0.21 ms (no card time) | 0 | 0 | 0 (80 ms on the card core) |
| after, 10 MB | 0.16 ms | 0 | 0 | 0 (568 ms on the card core) |

The swap itself was already safe: a load goes into a slot no voice plays (`free_slot`: not selected, not used for
100 ms), published with one store of its frame count; a voice on the old sample keeps its own slot.

**The fix: the card work is the card core's (core 0's loop, `core/msamp/cardq.h`), never the control core's.**
- `cardq` got two requests: `kWriteReq` (a whole small file: its folders, opened for writing, written, closed) and
  `kCloseReq`, and `cardq::async()`: a load calls it each step with the same request until it is done; it never waits
  or spins (a full queue: posted at a later call). Without a card core (the Mac's tests, `cardq=0`) it runs at once: the
  old path.
- **Pool files** (`srclib::pool_save`: a bank row set, a fill, a pool slot added / removed, the touchscreen's FILES and
  the web's +GR / Fill bank alike): the pool is marked dirty with its file name; the text is formatted when the card
  core is free and written there. Saves while a write waits are one later write (three commits back to back: 2 writes).
  A project switch waits for the old project's pending write first.
- **GRANULAR and WAVETABLE loads:** the open, the header, each chunk and the close are card-core requests; the control
  core converts and publishes between them. "LOADING" shows on the row meanwhile (the list's entry state, as before).
- Test: `make -C core test` (`test_loadstall`: no control-core card time, no pass over 2 ms, both files load and play,
  the pool file written by the card core with the rows, the coalesced writes; the figures above printed).

**Not moved (still on the control core; each a bounded step, but card time there):** MULTISAMPLE's instrument loads
(the .sfz, each sample's header, the RAM / head reads: one read a step), a folder's listing for a bank fill
(`bank_fill` reads the folder in one go: ~127 directory entries), the pool files' reads at a project change
(`pool_read`, one pool a step), the library's listings (a few entries a step), and the other small card writes:
GRANULAR's picks (`/.rytm/granular/<project>.txt`, a touchscreen pick), the LFO, INSERT and send FX settings, the
keyboard settings, the sequencer's and the song's files (written a second after the last edit). Each costs ~10-20 ms of
the control core on the card model when it runs; none is on the bank list's YES. Moving them is the same pattern
(`cardq::async`).

## 8. 0.5.0: the sample browser - preview, quick load, file menu (host side)

The bank list's files (section 6) get the Digitakt II's preview and the Octatrack's quick load. Every new key needs the
0.5.0 0026: inside its open list / browser a press made with FUNC held arrives as `31 <code> 2` (release `31 <code> 0`),
and its hello carries caps bit 5 (32, `ui::kCapsBrowseFunc` in core/ui/extra.h). The host takes the new keys when the
hello has that bit, or once a `2` came since the last hello. An older 0026 never sends 2 and FUNC + YES reaches the host
as YES; with it every key works as before (LEFT / RIGHT banks, RIGHT on a file nothing, YES sets the row), and so do the
JUNO / FM preset lists with any 0026 (the 0.5.0 0026 closes a preset list on FUNC + NO itself, 4A on 0 why 2, as a NO).
All of it works the same on a SAMPLER track (docs/SAMPLER.md) as on DSP SAMPLE: its SMP list is the SAMPLES pool, and
YES commits the row to SMP (P11).

| Where | Key | Action |
|---|---|---|
| List | UP / DOWN, knob A | row (unchanged) |
| List | YES / NO | commit / close (unchanged, the Rytm's) |
| List | FUNC + YES | the files (unchanged) |
| List | RIGHT | the files: the folder of the last file loaded (cd there first), the cursor on the file after it |
| List | FUNC + LEFT / RIGHT | bank - / + (moved from LEFT / RIGHT) |
| List | LEFT | nothing (reserved) |
| List | FUNC + UP | the bank's row 000 (OFF); YES then sets it |
| List | FUNC + NO | the cursor to row 000 (OFF), as FUNC + UP; YES then confirms the unload (nothing changes at once) |
| Files | UP / DOWN, knob A | cursor; FUNC + UP / DOWN a page of 8 (a move stops the preview) |
| Files | YES | folder: open; file: set the row, back to the list (unchanged) |
| Files | RIGHT | folder: open; file: load it into the row, back to the list on the next EMPTY row (onwards, then the next banks; none: POOL FULL) |
| Files | FUNC + YES | preview while YES is held (as the stock browser; YES's release stops it) |
| Files | knob H | PREVIEW LEVEL, -40 .. -6 dB a step (default -9), said in the header ("PREVIEW LEVEL -8 DB"); a Sidecar setting, /.rytm/preview.txt |
| Files | FUNC + RIGHT | folder: open; file: the FILE MENU |
| Files | LEFT / NO | up a folder; at the top back to the list (unchanged) |

A used row asks once ("C003 USED 1: RIGHT=SET"), and pressing the same key again replaces it (the YES ask is unchanged).
A file that is already in the pool (the same hash) is not loaded twice. RIGHT says "IN POOL B003", stays on the empty
row and goes on after that file. Loading two files takes two presses each (RIGHT loads, RIGHT goes back in).
The rows RIGHT loads in one opening of the list are the track's quick-load batch: FILL's SAMPLE (docs/FILL.md) takes
them as its FROM / TO when it opens on that track (`fill_batch`), so loading a set of files and pressing UP + RIGHT, YES
p-locks them over the track's trigs.

**FILE MENU** (FUNC + RIGHT on a file; UP / DOWN or knob A an item, YES / RIGHT do it, NO / LEFT close it): LOAD (as
YES), LOAD TO NEXT EMPTY (into the first empty row from the list's row on; the list on that row; none: POOL FULL),
PREVIEW, SAMPLE EDIT (SAMPLES lists only: loads into the row - a used row asks first, and its YES loads and opens -
then opens the editor on that pool row once its file is in - `sedit_page_open(-1,
code)`; the list's page gives way, the Rytm's list closes on its next key), INFO (rate, bits, MO / ST, length in the
header's note, e.g. "48K 16B MO 0.2S"; read from the file's head, nothing loaded).

**The preview** (core/samp/preview.h) is a dedicated DRY voice on the POST pair (host -> Rytm 0/1), mixed after
everything else by the output fill (pi/engine/ob_usb.cpp, after a take's playback). It uses no track voice, INSERT, send
FX or pool row. It streams the file's head through cardq (open, a 64 KB head read, then 64 KB pieces, one request per
idle step, core/engine.cpp idle_stream) into a buffer of 480 000 frames of int16 L R (1.92 MB), taken once at boot from
the sample-RAM arena (`msamp_ram_alloc`; only the 64 KB read stage is static, so the kernel grows by 64 KB). A WAV whose
`data` chunk lies past the first 64 KB (a big JUNK / LIST chunk) is found by walking the chunk headers over further
reads. It plays once, at most 10 s of the file (a 96 kHz file: 5 s), at PREVIEW LEVEL, resampled linearly to 48 kHz.
PREVIEW LEVEL (the owner's hardware test: the first build's fixed -18 dB was too quiet) is knob H in the files, -40 ..
-6 dB in 1 dB steps (paced as a list knob), default -9 dB, kept in /.rytm/preview.txt (`level=-9`; a Sidecar setting,
not per kit; saved from the idle chain as the other settings files, lines a newer build wrote kept). It is added after
the output limit (POST's voices and a send FX return there each limited to -12 dBFS): at -9 dB a full-scale file stays
under full scale on top of both at their limits (0.25 + 0.25 + 0.35), as long as no computer USB OUT, master FX return
or take playback is on 0/1 too (those add on the same pair); at -6 dB the sum can reach full scale with both at their
ceilings. The add saturates, so nothing wraps. A level change ramps over one output block. The 0026 forwards knob H (32 7) only
while the host's browser is open (ls_key's browse, after 4A on 1); in the list, and with an older 0026, knob H closes
the list as any other knob. A mono file plays on both sides,
and a file with more channels plays its first two. The sound starts after 8192 frames are loaded and fades 5 ms in and
5 ms out. If the card falls behind, it holds the last frame, ramps down and fades back in when the data arrives (one
underrun counted per hold). The loader does not overwrite the buffer before the audio has taken up a new preview (or
50 ms passed). Whether it sounds (PLAY in the header) comes from its status and a timed stop, not from the audio
callback, so a stalled output cannot leave PLAY on. FUNC + YES plays it while YES is held: YES's release (31 44 0,
which the 0026 forwards whether FUNC is still held or not [measured, rytm-emu bk_emu.py browsefunc / browseknob]) stops
it; FUNC's own release is never forwarded, so letting go of FUNC first keeps it playing until YES. The menu's PREVIEW
stays a toggle (a second PREVIEW, or the rest below, stops it). It stops on YES's release, any
cursor move, a load, LEFT / NO, the browser or the list closing, and another preview, which fades the old one out
before reusing the buffer. Files: anything core/wavetable/wav_read.h reads. TABLES play as they are (the frames one
after the other). INSTRUMENTS (.sfz) and the virtual items say NO PREVIEW. The 300 ms rest-preview of GRANULAR /
WAVETABLE pool files (section 6) is unchanged, except that it is skipped while the dry preview sounds.

Tests: core/tests/test_preview.cpp (POST only, L = R for mono, stereo kept, 44.1 kHz resampled, the 10 s cap, the 5 ms
stop ramp, a new start fading the old, INFO with no sound, non-audio files, the hold when the card falls behind, no
allocation in mix) and core/tests/test_browser.cpp (an old 0026 against the 0.5.0 one through the real 4A / 31 / hello
messages: each key above, the re-entry cursor, the next-empty skip, the used-row ask, the menu drawn and each item,
the preview stops (also on the Rytm's 4A close), no pool row used, POOL FULL, a file already in the pool, the editor's
row, FUNC + NO to OFF and its YES, the re-entry from another folder, the old 0026 YES ask). test_preview also runs
cardq in worker mode (a request in flight, a start and a stop meanwhile), a far `data` chunk, and PLAY without audio.
preview-hold: test_browser holds FUNC + YES (31 44 2 then 31 44 0: the release stops it; a move while held stops it, the
late release does nothing), turns knob H (the level, its note, the clamp, the save; knob H in the list is not the
browser's); test_preview the level (default, clamp, the ramp, preview.txt saved and read back with other lines kept).
