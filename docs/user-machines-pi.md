# User machines on the Pi Sidecar

Status: 2026-10-17, branch `pi-user-machines`. Tested on the Mac and in the emulator only, not on Pi hardware yet.

The Rytm's user slots U1..U5 (rytm-mods 0021-ext, `slots=1`) play loadable machine plugins on the Pi. They use
the same plugin C ABI (`sdk/rmp/rmp_api.h`) and the same slot protocol (`sim/protocol.py` 19..1E) as the Teensy.
One plugin source builds for both boards: `make` gives the Teensy package, `make a64` the Pi package.

## Decision: a real loader, not presets of built-in engines

The other option was to make user machines named presets of the built-in engines. That would ship sooner, but it
gives you no new sounds and splits the plugin story between the Teensy and the Pi. A real loader turned out to
be affordable:

- **Circle's MMU allows it.** `lib/translationtable64.cpp` maps all RAM read-write (`AP=RW_EL1`) with 64 KB
  pages. Everything above `_etext` is PXN, so not executable at EL1.
  - `pi/engine/um_pi.cpp` takes 4 MB of the heap, aligned to 64 KB, and clears PXN on those pages only. It walks
    TTBR0_EL1, then the level-2 table (512 MB per entry), then the level-3 table (8192 pages of 64 KB each).
  - It then runs `tlbi vaae1is` for each page, then `dsb ish; isb`.
  - The translation table is shared by all four cores, and the TLB invalidation is broadcast.
- **Cache maintenance after a load:** for each line, `dc cvau` (clean the D-cache to the point of unification),
  then `dsb ish`; for each line, `ic ivau` (invalidate the I-cache), then `dsb ish; isb`. Line sizes come from
  CTR_EL0.
  - The render cores (1 and 2) run an `isb` before they first call code placed since their last block (`um::Exec::barrier`).
- **W^X:** a package has two parts.
  - Code and read-only data go into the executable arena.
  - Data and bss go into ordinary RW memory (non-executable).
  - Plugin code never writes its own executable pages. The arm64 Mac requires this (MAP_JIT allows each thread
    either writing or executing at a time, never both). On the Pi it is defence in depth.
- **The same packages run on the Mac.** The core lib (`core/host/um_host.cpp`: a MAP_JIT region with an RW
  region mapped right after it) runs the exact RMPA images. So the tests and the emulator run real AArch64
  plugin code, not stand-ins.

Not done this round:
- **No fault guard around the control-core calls.** `init`, `voice_init`, `params`, `note_on` and `note_off` run on
  core 3 without one. A plugin that faults there stops the engine: a fatal exception, a clean stop and a reboot.
  `render` runs inside the existing per-voice guard (`pi/engine/guard.h`). A fault or an overrun there mutes the
  voice, as for the built-in machines.
- **Pool access from the sample RAM only** (2026-10-18). `pool_info` / `pool_read` answer from the sample RAM's copy
  of the pool slot (`sampleram=`, 256 MB by default): the control core resolves and pins it, a render core reads it
  (`RMP_POOL_PSRAM`, mono int16 as the RAM holds it). The first `pool_info` for a slot answers NOT_READY; the
  sample is there once loaded. Without sample RAM (`sampleram=0`), or a sample not in RAM after 10 s, it answers
  EMPTY. The Teensy's head/ring reader for boards without PSRAM is not ported.

Presets (2026-10-18), as the Teensy's (`teensy/libraries/RytmPlugins/src/rmp_presets.h`, the same code):
`/PRESETS/<id>/<name>.txt` ("P<n> <value>" lines) and `<name>.<preset_ext>` (the plugin's `load_entry`), one list
in name order, at most 127. 1A's flag 4 (presets exist) and its generation follow the list; 19 what 2 is answered
with 1B 0x20 (16 names a page); the PRE knob's entry loads between requests and the preset-role knobs are written
back with 1C (held over the reports for 300 ms); a p-locked PRE on a trig plays its entry without write-back; a
trig's note waits for its preset load (2 ms at most). The scan reads 4 entries a step (the Pi's `openNext` opens
each). Console: `um=save:<slot>:<name>` writes a generic preset of what the slot's last voice plays now (its preset-
and offset-role knobs); `um=presets` scans every list again (the web page sends it after a rename or delete under
`/PRESETS`). `sim/tests/test_core_presets.py` covers it on the Mac with `v2_test`.

## The package format: RMPA (AArch64)

This is `tools/rmp_pack.py`, `teensy/libraries/RytmPlugins/src/rmp_format.h` and `rmp_loader.cpp`. A Teensy
package is RMP1 and is unchanged. An RMPA package is the same 96-byte header, manifest (format 3) and plugin
table, with these differences:

| | RMP1 (Teensy) | RMPA (Pi, arm64 Mac) |
|---|---|---|
| magic | `RMP1` 0x31504D52 | `RMPA` 0x41504D52 |
| ELF | ARM, Thumb-2, `-mword-relocations` | AArch64, `-fno-pic -mcmodel=small -ffixed-x18 -mno-outline-atomics` |
| linker script | `sdk/rmp/plugin.ld` | `sdk/rmp/plugin_a64.ld` (data on its own page) |
| `rmp_plugin` size | 72 | 128 (`RMP_PLUGIN_BYTES_A64`) |
| relocations | ABS32, MOVW, MOVT | `RMP_REL_ABS64` (3): a 64-bit word. If its value is below `data_start` it moves by the code base, otherwise by the data base. `RMP_REL_A64_ADRP_DATA` (4): an ADRP in the code that points into the data part, re-aimed at the same byte |
| header `reserved[0..3]` | 0 | `data_start`, page-aligned |

The loader keeps both parts on 4096-byte boundaries, so `:lo12:` offsets and B/BL inside a part need no fixing.
The packer refuses anything else that crosses between the parts.

The target check is `rmp_host_target`. `rmp_verify_header` refuses the other board's package with
`RMP_ERR_TARGET`. An older Teensy refuses RMPA as `RMP_ERR_MAGIC`.

## On the Pi's card

```
/Plugins/<anything>.rmp          the installed machines (the package's id names it, not the file name)
/.rytm/slots/<PROJECT>.txt       U1..U5 per project, "U<n> <id> <a.b.c>" (rmp_slots_parse, as the Teensy)
/.rytm/slots/<PROJECT>.inf       where each assignment came from (rmp_resolve.h; "Which machine a slot plays")
```

## Which machine a slot plays (slot-resolve, 2026-10-19)

The slot list (U1..U5 -> package) behaves as one instrument across projects, and a project shared between Sidecars
finds its machines by its sounds' fingerprints. The rules are portable code shared by the Pi, the Mac core lib and
the Teensy: `teensy/libraries/RytmPlugins/src/rmp_resolve.{h,cpp}` (tests: `extras/resolve_test.cpp`). Per slot, when
a project loads:

1. **Explicit empty stays.** A slot the user cleared (web UI, console `um=clear:<n>`, the Rytm's slot menu clear) or
   "start this project empty" (`um=empty`, all five) is never filled by inheritance or fingerprints. The sounds'
   fingerprint is still noted, so the UI can say what they want.
2. **Fingerprints.** The fingerprints the project's sounds use on the slot (0x25 SLOT PRINTS, below; with an older
   Rytm lazily from a trig's tag):
   - the slot's assignment stands if any of its sounds uses it;
   - otherwise the most used fingerprint wins (count, then the lower fingerprint) if exactly one installed package has
     it. The map is changed and saved. A saved, imported or inherited assignment replaced this way is logged and shown
     as "updated from the project's sounds" (flag `updated`, until the next load);
   - a collision (several installed packages with that fingerprint) takes the package whose id the slot's note names
     (an imported list, an earlier resolve). Otherwise nothing is assigned and the slot is flagged `collision`;
   - a fingerprint no installed package has: flagged `unknown`, the UI asks for "a machine with fingerprint NNNN"
     (and its name, if an imported list gave it). A saved assignment stays; an inherited one is dropped.
3. **The project's own map** (saved or imported) wins for every slot with no fingerprint information. Nothing else
   overwrites it.
4. **Inherit.** A project with neither a map nor a notes file takes the previous project's assignment for each slot
   with no fingerprint, and saves it, so it becomes the project's own (saved from then on: a later load treats it as
   the project's own map). It waits 1.5 s after the project change for a 0x25 (the Rytm sends one right after its
   10), so a shared project never loads the inherited machines first; with an older Rytm the inheritance comes
   after those 1.5 s. Nothing inherited: no file is written, and the project stays fresh. A note saying "empty"
   beside a used map (an older build edited the map) loses to the map; the notes are written only once the map is.

Lazily (no 0x25 from this Rytm for the project): a trig whose tag does not match its slot fills an empty slot, or
replaces an inherited one. That trig is silent; the slot plays from the next trig on. A saved slot is never changed
by one trig.

Files: the map stays `/.rytm/slots/<PROJECT>.txt` in the old format, so an older build still reads it. Next to it,
`/.rytm/slots/<PROJECT>.inf` holds the notes, one line per slot: `U<n> <src> <flags> <fp> <id|-> <a.b.c|-> [<name>]`,
with src `none|saved|sounds|inherited|empty|imported`. The notes are advisory: a missing or damaged file only loses
the "where from" information.

**0x25 SLOT PRINTS** (Rytm -> host, rytm-mods 0021 branch `slot-resolve`, builds sfx1019 / rui1019; `sim/protocol.py`
`slot_prints`): `25 01 <seq> <flags> <n> {<slot 1..5> <fp hi7> <fp lo7> <count>}`. These are the distinct (slot,
fingerprint) pairs of every tagged U-sound in the project's kits, with a count each (at most 16; flags bit 0 means
more were left out). The Rytm sends it right after a `10 <project>` under one MIDI lock (try-lock: it never waits),
once a project or kit load has settled, and again every ~5 s with the same seq. The host resolves from the first one
after a project change, and from any whose content differs from the last one used. A repeat is ignored, so a user's
explicit change is not undone by the periodic resend. An older host ignores 0x25 (core/host_link.cpp, the null
hook).

**JSON** (`GET /um`, and the Teensy's `@`-lines) adds, per slot: `src`, `updated`, `collision`, `unknown`, `mixed`
(its sounds use more than one fingerprint), `want_fp`, `want_n` (how many sounds), `want_id`, `want_ver` and
`want_name`. The `plugins` object adds `prints` (`rytm`: a 0x25 was used for this project, `waiting`, or `trigs`: an
older Rytm), `fresh` and `resolves`.

**Console:** `um=put:<n>:<id|->:<a.b.c>[:<name>]` sets one imported entry (the package need not be installed: it is
MISSING by name until it is). `um=empty` starts the project empty. The Teensy's web console uses `plugin=put:...` and
`plugin=empty` (webui.h).

**The slot list as a file** (Export / Import on the Pi's page and on the Teensy's web console; `web/slotlist.js`
and the same functions in `pi/engine/www/index.html`):
`{"format":"sidecar-slots","version":1,"project":"NAME","slots":[{"slot":1,"id","ver","name","fp"} | {"slot":5,"empty":true}]}`.
Slots the list does not name are left alone. A plain map file (`U<n> <id> <a.b.c>` lines) also imports. Import
applies each entry with `put` and reports, by name, the machines that are not installed.

**Pi display:** the home page's PROJECT panel shows the Rytm's project and U1..U5 as the Rytm shows them (`!NAME`
missing, `NAME...` loading, `---` empty). The data comes from `um::ui_view`, published with the JSON.

## The glue: `core/user/um.cpp`

This file is portable: it runs on the Pi as a hosted object and in the Mac core lib. It is not in the Teensy
core library, because `teensy/setup_core_lib.sh` leaves `core/user/` out.

- **Scan** of `/Plugins`, one bounded step at a time (at most 8 KB read per step), on the control core between
  blocks. For each file it checks the header, the CRC, the entry table, the manifest, the target, whether the
  state fits (64 KB per voice) and whether the code fits.
  - One id in several versions (bundles phase 4, docs/BUNDLES.md "Exactness"): every version is kept side by side
    (`/Plugins/<id>-<a.b.c>.rmp` from a bundle's import, any file name otherwise). The **newest** is the id's
    default: the only one in the Rytm's list and the one a fresh assignment takes; older ones are listed as
    `"older": true`. A **slot plays the exact version its map names** (`U<n> <id> <a.b.c>`) when that one is
    installed; else the newest, if the version wanted is in its compatible range (`compatible_from` .. its version);
    else the slot is MISSING ("the slot wants X, installed Y"). The slot JSON says what it plays (`file`,
    `play_ver`). The same id and version twice: the first copy loads, the other is listed as a duplicate.
  - Up to 32 packages go into the Rytm's installed list (1B 0x21). A name that repeats in its first 9 characters
    gets `~2`, as on the Teensy.
- **Slots:**
  - Assign (1E op 0, which checks the installed generation) or clear (op 1). The map is written by a core-0 task
    (`um_pi.cpp`), so the control core never waits on a card write.
  - The answer is a 1D carrying the slot's result once the slot has settled, or LOADING after 400 ms (the Rytm's 2 s cover its list ask and this answer together). A list asked
    for while the first scan runs is sent when the scan ends (an empty list would turn the Rytm's YES into a clear).
  - Op 2, install from the card, answers NOT ON CARD: on the Pi the card is the store.
  - 1A carries the slot's description and its fingerprint (`rmp_plugin_fp`, only when READY).
  - A package that is missing, or outside the slot's version range, is MISSING, and the Rytm shows `!NAME`.
- **Generations** use `rmp_meta`. A 1D goes out when something changes, at most once a second after a 10 or a 19,
  and every 5 s in any case.
- **Voices:**
  - Plugin state lives in 64 KB per voice from the heap, not the built-in engines' scratch.
  - seqx (E0): a package whose header opts in (`max_voices` > 1 in its manifest: RMP_HF_INSTANCES) plays a sequencer
    poly track's notes on **instances**: 32 more states of 64 KB from the heap at boot (core/inst.h's pool), each the
    slot's package on its own (`um::inst_*`: voice_init / voice_enter at its first note, voice_leave when the slot's
    image changes; the voice's params as its own state gets them). Older packages stay mono (the voice's one state).
  - The sound tag is checked as on the Teensy: a trig whose tag is another machine's is silent until a matching
    trig.
  - Output is clipped and a NaN becomes 0.
- **Loading:** the package is read 8 KB per step, its CRC checked, the code part placed in the code arena (4 MB)
  and the data part in the data arena (4 MB), the caches synchronised, and `init()` called once.
  - A slot with the same package as another READY slot shares its image.
  - When the arenas fill up, everything is loaded again once (the arenas are reset with no voice playing); if it
    still does not fit, the slot is NO_ROOM.
- **Console** (serial, or the web page's command box): `um`, `um=rescan`, `um=assign:<1..5>:<id>`, `um=clear:<1..5>`.

## JSON (shared with the Teensy's web console, docs/webui-protocol.md on teensy-webui-plugins)

`GET /um` on the Pi returns an array of these objects (the Teensy prints the same objects as `@`-lines):

```
{"t":"plugin","id","ver":"a.b.c","size","api","image","bss","state","cpu","ns","blocks","name","slots":[1..5],
 "fit":bool,"why","file","target":"a64"|"arm32","fp","status":"ok"|reason}
{"t":"slot","slot":1..5,"id","ver","state":"EMPTY|READY|MISSING|BAD|NO_ROOM|LOADING|MUTED","name","fp","ns","blocks"}
{"t":"plugins","n","project","arena":[size,used],"state_limit","block_ns","listed","gen","dir","scanning"}
{"t":"mem","k":"code|data|state|sd","size","used"}      then {"t":"mem_end"}  (the Pi adds "sd" last)
```

- `cpu` is the package's declared figure (per mille of one Teensy voice budget).
- `ns` is measured: the mean time of one voice's render call (kBlock = 12 frames) since the slot loaded, and
  `blocks` how many it made (0: not played yet). On a plugin it is the slowest slot playing it. `block_ns` is a
  block's real time (250000 ns at 48 kHz), so `ns / block_ns` is the share of one render core per voice. While
  something plays, the JSON is published again once a second.

## The web page (sidecar.local)

The page is `pi/engine/www/index.html`; the server side is `pi/engine/web.cpp` and `pi/engine/fsweb.cpp`.

- **User machines:**
  - The installed list: name, version, size, fingerprint, code+data, state per voice, CPU (measured per voice once
    it has played, in % of one Pi core and ns per block; the declared figure below it), the slots that use it, and
    whether it fits (with the reason when it doesn't).
  - Delete, Rescan, and upload of `.rmp` files into `/Plugins`.
  - The current project's U1..U5, with an assign/clear select for each.
  - Usage bars for code, data, state and SD.
- **Files:** browse, download (64 KB ranges), upload (several files, drag and drop, 48 KB chunks), delete, new
  folder, rename, and free space.
  - Folders: a dropped folder (or "Upload folder") is walked whole (webkitGetAsEntry), its folders made first
    (`mkdir?p=1`), then each file in order, with progress per file and overall and one Cancel for the lot.
    macOS junk (`.DS_Store`, `._*`) is skipped.
  - Dot-files (`.Spotlight-V100`, `.fseventsd`, ...) are hidden unless "show hidden" is on; Go to `.rytm` still
    opens it.
  - `GET /fs/ls?path=`, `GET /fs/get?path=&off=&len=`, `POST /fs/put?path=&off=&total=&crc=` (multipart, one
    part), `POST /fs/del?path=`, `/fs/mkdir?path=[&p=1]` (p=1: every missing folder on the way, 200 "made" or
    "exists"), `/fs/mv?path=&to=`.
  - Every POST answer is the text `<code> <message>` with HTTP 200. Circle's own error pages carry no message,
    so the code goes in the body.
- **Safe writes:**
  - An upload goes to `<path>.part`. Each 4 KB piece is read back and compared, and a running CRC is kept. The
    last chunk's CRC must match the client's, and only then does the `.part` replace the file.
  - `409 expected N` means resume from offset N.
  - A package for `/Plugins` is checked before it goes in place: header, size, CRC, ABI version and target. A
    Teensy package is refused with "build it for the Pi with `make a64`".
  - Boot files are read-only, shown with a lock, and never written, deleted or renamed: config.txt, cmdline.txt,
    sdboot*, engine.img*, kernel*.img, start*.elf, fixup*.dat, bootcode.bin, armstub*, tryboot.txt*, *.dtb and
    overlays/, and config.txt.bak (the restore backup; config.txt.prev stays editable). sdfiles, on TCP 7310,
    installs them. The listing marks them `"ro":1`.
  - The Sidecar's own folders `/.rytm`, `/Samples` and `/Plugins` are kept: never deleted or renamed (403), their
    contents free. The listing marks them `"ro":2`. Both rules are fsweb.cpp's, not only the page's.
- **Core 0:**
  - Uploads run in the HTTP worker tasks on core 0, in 4 KB pieces with a scheduler yield between them, and one
    request at a time from the page.
  - Overbridge and the USB-C gadget run in core 0's interrupts, which the tasks never mask.
  - The open risk is FatFs's volume lock. While core 0 writes a piece (and while an SD card's write latency
    spikes), core 3's card reads for DSP SAMPLE streaming wait. Measure this on hardware with
    `pi_run.py gaps` while a 20 MB upload runs, before relying on uploads during play.
  - Mac test (`make -C pi/engine/tests`): a 20 MB upload in 427 chunks of 48 KB, 0.48 s, longest chunk 4 ms on
    the Mac's SSD. The card's own speed is not measured.

## The starter set (plugins/sc_*)

All are our own code, MIT. GRAIN's sample is our own synthesis, CC0 (`plugins/sc_grain/LICENSE-texture.txt`,
generated by `make_texture.py`).

| Machine | id | What it is |
|---|---|---|
| ACID | sc.acid | 303-style bass: saw/square, resonant 4-pole, env mod, accent, slide, drive |
| WAVEPAD | sc.wavepad | morphing wavetable pad: 3 detuned voices, LFO on the morph, sub osc, slow envelope |
| METAL | sc.metal | metallic FM percussion: inharmonic ratio list (BELL..TINE), index decay, pitch drop |
| SNARE | sc.snare | snare / noise designer: two tuned bodies, bandpassed noise, snap |
| PLUCK | sc.pluck | Karplus-Strong: tuning compensated for the loop filter's phase delay (0 cents), pick position, body |
| GRAIN | sc.grain | granular texture over a built-in CC0 sample: position, spray, size, density, pitch, reverse |

To build and install:
- `tools/um_starter.sh [CARD_DIR]` builds all six with `make a64` (Docker's aarch64-none-elf-gcc when it is not
  on PATH) and copies them to `CARD_DIR/Plugins/<name>.rmp`.
- Or upload the `plugins/sc_*/sc_*.a64.rmp` files on the web page.

## Tests

- `teensy/libraries/RytmPlugins/extras/loader_a64_test.cpp`: verify, the target check, the two-part placement,
  every ABS64 and ADRP checked, refusals. The Teensy's loader, slots, manifest, meta and proto tests still pass.
- `sim/tests/test_core_um.py`: real RMPA packages in the core lib.
  - The scan and the list, assign and clear with their answers, 1A with the fingerprint, the map file.
  - The sine at 440 Hz, the sound tag, enum names, MISSING after a delete, a stale list refused.
  - The version range, duplicates, junk, a damaged file, a Teensy package.
  - The console and the JSON; the six starter machines all sound.
- `pi/engine/tests/test_fsweb.cpp`: paths, boot files, the 20 MB upload, resume, a wrong CRC, a full card,
  package checks, list/mkdir/rename/delete, the kept folders and config.txt.bak, a folder upload (mkdirs + put).
- `sim/um_e2e.py`: the emulated Rytm (sfx1017) against the core lib over rytm-emu's Overbridge link.
  - The track menu's YES steps U1 through the installed machines (ACID, GRAIN, METAL).
  - BD on U1 plays ACID.
  - The package is deleted on the core's card: U1 goes MISSING, the machine list shows `!ACID`, and BD is silent.
  - Screens go to `sim/build/um-e2e/` (git-ignored).
