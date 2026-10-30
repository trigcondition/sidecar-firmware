# POOLS: the project's three POOL lists (SAMPLES, WAVETABLES, INSTRUMENTS)

Built 2026-10-22 on the branch `pools` (phases 0-5 of the design, this file is phase 6). The design:
`docs/superpowers/specs/2026-10-22-pools-tonverk-design.md` ("the spec"); the vocabulary: `docs/UX-REVIEW-2026-10-22.md`
P1 (one noun, POOL) and P3 (replacing a row in use). Tags: **[measured]** run in this repo's tests or read in the code
on this branch; **[inferred]** reasoned, not run. Nothing here was run on a Pi or a Rytm. Pi and Mac only
(`CORE_MSAMP`); the Teensy (Sidecar Mini) is frozen at 0.1.0 and has none of it. Nothing of Elektron's code is used:
the Tonverk was studied through its public manual for behaviour only.

This file replaces, for the host side, the per-machine pools, bank A's composed list, the LIBRARY window inside the
lists and the touchscreen's picks described in `docs/SRC-BANKS.md` and the gr-lib sections of the machine docs.

## In short

- **One noun, POOL.** A project has three POOL lists: **SAMPLES** (GRANULAR's SRC, DSP SAMPLE's SMP, the sampler's
  takes), **WAVETABLES** (WAVETABLE's TBL) and **INSTRUMENTS** (MULTISAMPLE's INST). Each machine sees only its own list.
- **8 banks x 128 rows each, OFF at row 0 of every bank.** A row's address is `code = bank * 128 + row`, the Rytm's 14-bit
  P1 word as rui1038 defined it (`srclib::code_of`): nothing changes on the wire. Rows never move: loading never
  renumbers, unloading leaves a hole, and a lock on a hole plays silence.
- **A row holds a sample, an input, a built-in or an instrument**, referenced by content hash (`lib::Ref`). A list holds
  one entry per hash: loading a file that is already there gives its existing row.
- **Files:** `/.rytm/projects/<K>/samples.pool.txt`, `wavetables.pool.txt`, `instruments.pool.txt` and the used set
  `pool-used.txt`, read and written by the card core only. A project without them opens with the template. The old
  per-machine pool files are not read (no migration).
- **The pool RAM:** one decoded sample cache keyed by hash, shared by GRANULAR, DSP SAMPLE and MULTISAMPLE (a file used
  by two machines is in RAM once), budget `poolram=MB` (1 GB on 2 / 4 GB boards, 3 GB on 8 GB), shown as MB and % in
  every POOL view.
- **Every surface shows the same rows:** the Rytm's bank list (FUNC + YES in every bank), the Rytm's POOL page, the
  touchscreen's POOL view and POOL page, the web's Pool tab and the Files tab's +SMP / +WT / +INST / Fill bank, the
  console's `pool=` lines.
- **P3:** replacing or unloading a row in use asks first, is refused while the Rytm plays, and can be undone (one level).

## 1. The three lists

| list | console tag | format | the Rytm's 47 list | machines | entry types |
|---|---|---|---|---|---|
| SAMPLES | `smp` | `samplepool` 1 | 0 | GRANULAR (SRC, 14-bit), DSP SAMPLE (SMP, 14-bit since rui1045), the sampler's takes | sample, input, built-in |
| WAVETABLES | `wt` | `wtpool` 1 | 1 | WAVETABLE (TBL, 14-bit) | sample, built-in |
| INSTRUMENTS | `inst` | `instpool` 1 | 2 | MULTISAMPLE (INST, 14-bit), the auto-sampler, the sampler's slice maps, CHOP's slice maps (docs/CHOP.md: found by the sample's hash) | instrument |

`core/lib/pools.h` `List` (`kSamples 0`, `kTables 1`, `kInstr 2`) equals the Rytm's 47 list numbers and srclib's
browser kinds. 8 x 127 = 1016 usable rows a list [measured: `kBanks`, `kRows`].

### Entry types

| type | what | file | in which list |
|---|---|---|---|
| `sample` | a WAV / AIFF (`.wav .aif .aiff`) by its `lib::Ref` | yes | SAMPLES, WAVETABLES (any sample can be a table) |
| `input` | GRANULAR's live sources: `IN MAIN BD SD RS-CP BT LT MT-HT CH-OH CY-CB` | no | SAMPLES |
| `builtin` | SAMPLES: `PAD BELLS`; WAVETABLES: `BASIC PWM VOCAL ORGAN FM SYNC` | no | SAMPLES, WAVETABLES |
| `instrument` | an `.sfz` by its Ref, plus one Ref per member sample in the `.sfz`'s order | yes | INSTRUMENTS |

- **Inputs and built-ins** are ordinary rows without a file. They go into an empty row from the browser's two virtual
  folders at its root, `@INPUTS/` (SAMPLES) and `@BUILT-IN/` (SAMPLES and WAVETABLES). A file cannot be loaded over such
  a row (`BUILT-IN`, or `BUILT-IN: UNLOAD FIRST` on the Rytm); unloading it frees the row.
- **DSP SAMPLE and inputs / built-ins:** silent: such a row is an empty slot of DSP SAMPLE's mirror
  (`core/pool/pool.cpp` `pool_sync`) [measured: test_srclib].
- **Instrument members** take no rows. They are Refs inside the entry (`m` lines), resolved through the library like any
  sample, and shared in the pool RAM with the SAMPLES list by hash. At most 1024 members an instrument and 16384 in the
  list (`kMembersPer`, `kMembers`). Folder instruments (WAVs without an `.sfz`) are not offered.
- **One entry per hash.** `set_file` / `add_file` look the content up first (`find_hash`: by sha256, or by path while the
  library has not hashed the file). A file already in the list answers `kExisting` with its row (`IN POOL C004` on the
  Rytm, `IN C001` on the touchscreen); nothing changes. For an instrument the identity is the `.sfz`'s own hash.

### The template

A project with no list file at the new place opens with (`pools.cpp` `template_of`) [measured: test_pools, the golden
`samplepool/v1/template.txt`, `wtpool/v1/template.txt`]:

- SAMPLES: H001..H010 the ten inputs, H011 PAD, H012 BELLS. They sit in bank H so that bank A (DSP SAMPLE's whole reach
  before rui1045) holds only samples.
- WAVETABLES: A001..A006 BASIC PWM VOCAL ORGAN FM SYNC.
- INSTRUMENTS: empty.

The template is not written to the card by itself; the list file appears at the first edit [inferred: `template_of`
does not mark the list dirty]. Defaults: GRANULAR's SRC default is 10 (A010, an empty row: silent until something is
loaded), WAVETABLE's TBL default is 1 since rui1043 (A001 = BASIC), MULTISAMPLE's INST default 0 (A000, OFF).

### No migration

The old files (`/.rytm/pools/<K>.txt`, `/.rytm/<granular|wavetable|multisample>/<K>.pool.txt`, GRANULAR's picks
`/.rytm/granular/<K>.txt`) stay on the card and are not read. There are only test projects; such a project opens with
the template. An older build still opens its own view of the project from the old files.

## 2. Files and formats

| file | format | class | written |
|---|---|---|---|
| `/.rytm/projects/<K>/samples.pool.txt` | `samplepool` 1 | P (project) | 200 ms after the last edit (edits coalesce) |
| `/.rytm/projects/<K>/wavetables.pool.txt` | `wtpool` 1 | P | the same |
| `/.rytm/projects/<K>/instruments.pool.txt` | `instpool` 1 | P | the same |
| `/.rytm/projects/<K>/pool-used.txt` | `poolused` 1 | D (derived: losing it costs only preloading) | 1 s after a new code is seen, never while the Rytm plays |

`<K>` is the project key (`project_key()`). The formats are registered in `core/fmt/formats.cpp` (a `#sidecar <name> 1`
header required) and placed by `fmt::path` (`core/fmt/fmt.cpp`, generation 2). Reads are `cardq` open / read / close
requests, writes one `cardq::kAtomicReq` (`fmt::write_atomic`'s rename scheme); a read that finds no file tries its
`.prev` (a cut between the renames). A loaded list is parsed a piece a step, and a save is formatted a piece a step,
so neither holds up the control core. The control core never touches the card for them [measured: test_loadstall].
A list file holds at most 4 MB (`kFileMax`).

- **A file that is there but cannot be opened or read** (a card error: an existence check, `cardq` `kExistsReq`, after
  a failed open, or a read error) locks the list. The template is used only when neither the file nor its `.prev`
  exists, so a card error never gets a template written over a real list.
- **A list write that fails three times in a row is given up** (`kMaxFails`): the list stays in RAM, and a project
  switch is not blocked by it. The used set keeps retrying (class D).
- **Resolution-only changes** (a hash filled in for a `-` ref, a MOVED hint rewritten) are written later and never while
  the Rytm plays. The cached `frames channels rate` go out with the list's next save.

Text, sparse (only filled rows, in row order), one entry a line, a member line starting with a space. A row is the bank
letter and three digits; row 000 is never written. The reference is the formats spec's text form (`lib::format_ref`:
`<sha256 hex | -> <size> <path hint>`); after a tab the display name (39 characters at most) and, for samples, the
cached `frames channels rate`. Example (from the golden files in `core/tests/golden/`, hashes shortened):

```
#sidecar samplepool 1
A001 sample 9f2c...e1a0 412044 /Samples/Drums/kick.wav	kick	103000 2 48000
A012 sample - 96044 /Samples/LIVE/take 3.wav	take 3
H001 input IN
H011 builtin PAD
```
```
#sidecar instpool 1
B003 instrument 4be0...0711 900 /MULTISAMPLE/Piano/Piano.sfz	Piano
 m 77aa...22c3 1920044 /MULTISAMPLE/Piano/Piano_C3.wav
 m - 1920044 /MULTISAMPLE/Piano/Piano_C4.wav
```
```
#sidecar poolused 1
smp A001 12
wt B009 1
inst B003 40
```

- `-` is a reference whose hash is not known yet (a file the library has not hashed). The library fills it in when it
  has hashed the file and the next save writes it.
- Refused (the list **locked**: shown, never written, no edits, `POOL LOCKED`): a version newer than this build, a
  damaged file, a row the list does not take, an unknown type or name, a member without an instrument before it, more
  than the limits. A **foreign-newer** file (a newer build's, readable) is read and its edits stay in RAM (`read_only`).
  Both follow the format registry's rules (`fmt::load`).
- Line formats and the parser: `core/lib/pools_fmt.{h,cpp}` (pure: also the fuzz target's, `make -C core fuzz-fmt`).

## 3. Resolution and states

Every file entry's Ref is resolved through the library (`lib::resolve`: RAM only, lock-free) whenever `lib::gen()`
moves; `Entry::path` is where it plays from, `Entry::st` its state. An instrument's state is the worst of its `.sfz`
and its members'.

| state | plays | the Rytm's bank list | the Rytm's POOL page | touchscreen | web |
|---|---|---|---|---|---|
| EXACT | yes | the name | its size, or `RAM` when resident | the name, `RAM` | `EXACT` |
| MOVED | yes, from where the file is now (the hint is refreshed at the next save) | the name | as EXACT | the name | `MOVED` |
| UNVERIFIED, PENDING | yes, by path | the name | as EXACT | the name | the state |
| CHANGED (the file at its path has other content) | yes, flagged | `~name` | `CHANGED` | `~name` | `CHANGED`, Accept |
| MISSING | no (silent) | `!name` | `MISSING` | `!name` in the theme's alert colour | `MISSING` |

- **Accept** (`pool=K:accept:A010`, the touchscreen's ACCEPT, the web's Accept) makes a CHANGED entry take its file's
  new content (`accept_changed`).
- A member found elsewhere (MOVED) plays from there; the `.sfz` is not rewritten. A missing member: the instrument plays
  without that region and is flagged.
- While loading, the Rytm's bank list shows `LOADING` (GRANULAR: `TOO LONG: FIRST 87 S` for a sample cut at 87 s;
  WAVETABLE: `TOO BIG: 8 MB MAX`).

## 4. The used set, SELECT UNUSED, P3

### The used set

`pool-used.txt` counts, per list and row, how often a code was seen playing in this project. A row is "seen" when a
machine takes a report for it, or the Rytm's bank list commits it: the Rytm's 0x03 / 0x04 reports and the pads' notes
reach `gr::report`, `wt::report`, `msamp::report`, each of which calls `pools::seen`; DSP SAMPLE's reports count its SMP's
row and bank (`voices.cpp`); a YES commit in the bank list counts its row (`page_wavegrain.cpp`) [measured: the
callers]. A new code marks the file to be written a
second later; a count alone waits for the next new code or a project switch (which flushes). It is not written while the
Rytm plays. A damaged file is ignored (rebuilt as rows play).

### SELECT UNUSED

The rows that hold a file (a sample or an instrument) and are not in use (no voice on them now, never seen playing in
this project). Inputs and built-ins are never "unused": they hold no RAM. Every surface shows the warning
`NOT SEEN PLAYING - A RYTM LOCK MAY STILL USE IT` (`pools::kUnusedWarning`; the Rytm page's footer: `UNSEEN: A LOCK MAY
USE IT`): the Sidecar cannot read the Rytm's kits or patterns, so a parameter lock that has not played since the project
was opened is not known.

### P3: a row in use

"In use" is `users(l, c) > 0` (voices on that row now, `voice_on`; a voice counts only while it plays that list's
machine) or `used(l, c)` (seen in this project).

- **Ask first.** `set_file`, `set_special` and `unload` on a row in use answer `kErrUsed` (`IN USE: REPLACE?`) unless
  confirmed. Each surface asks: the Rytm's bank list `C012 USED 3: YES=SET` / `C012 IN USE: YES=SET` (a second YES on
  the same file sets it), the POOL page `2 IN USE: YES AGAIN`, the touchscreen `ROW C012 USED BY 3 SOUNDS.` / `ROW C012
  SEEN PLAYING IN THIS PROJECT.`, the web a confirm dialog; the console takes `:yes`.
- **Refused while the Rytm plays** (`kErrPlaying`, the transport from `rytm_dump::is_playing`): the bank list says
  `PLAYING: STOP FIRST` (the result text everywhere), the POOL page `IN USE: STOP FIRST`. There is no FUNC override on
  any surface: stop the Rytm first. Only the console's `:force`, the old `pool=S:path` line and DSP SAMPLE's own ASSIGN
  (12, an explicit choice on the Rytm's screen) bypass it.
- **Undo, one level a list** (`pools::undo`): the last replace or unload of that list comes back (an instrument with its
  members). It is refused when it would make a duplicate (the same content is in another row by now). The POOL page's
  UNDO, the touchscreen's UNDO in the message row, the web's Undo, `pool=K:undo`.
- An empty row is never guarded; loading into it needs no confirm.

## 5. The pool RAM

`core/pool/pcache.{h,cpp}` (the spec's 4.1).

- **Budget:** `poolram=MB` in `cmdline.txt`; default 1024 MB on a 2 / 4 GB board, 3072 MB on 8 GB (total memory 6000 MB
  or more), at most the high heap's free space less 64 MB at the time it is taken (else the low heap's less 192 MB)
  (`pi/engine/platform_pi.cpp` `pool_ram_budget`). One arena of that size, allocated once by the first machine to start.
  Mac: `CORE_POOL_RAM_MB` (default 64).
- **One entry per content:** keyed by the Ref's sha256 (a Ref without a hash yet: its path and size), int16 at the file's
  own rate **in the file's channels** (docs/STEREO.md: 1 mono, 2 stereo interleaved L R, 3+ its first two; a TRK voice
  sums at read, `pcache::mono_at`, the value the summed load gave; `poolmono=1` sums at load as before), either **whole** (GRANULAR, DSP SAMPLE, MULTISAMPLE's RAM mode; at
  most 2^22 frames = 87 s at 48 kHz, the rest cut) or a **prefix** (MULTISAMPLE's head; the rest streams). A whole entry
  satisfies a prefix want; a whole want on a prefix grows it into a new block (the old one kept while a reader pins it).
  A machine's own fill (MULTISAMPLE's head) and a whole load never run on one entry at once: a queued load is taken over
  by the fill, a want during a fill is deferred and grows the entry to whole after it, a forget during a fill is done at
  its end. A read error fails the load (never published as whole).
- **Shared:** a WAV used as a GRANULAR source and as a MULTISAMPLE member is one entry, bytes counted once
  [measured: test_pcache "2 shared"]. MULTISAMPLE matches the entry's members to the `.sfz`'s samples by file name (the
  i-th member when the names agree, else the member of that name), so an `.sfz` edited since it went in, or a sample
  left out, still finds the right member.
- **Eviction:** a block that does not fit evicts the least recently used window of unpinned entries next to a hole (no
  compaction; a window is 64 blocks at most). A pin (a voice or slot reading the block) keeps it. Private blocks (MULTISAMPLE's loop parts past the
  head) are counted in the budget and never evicted. Prefetches never evict.
- **Loads** go through the card core (`cardq`), one 32 KB read converted a step; wants before prefetches.
- **Unload:** an unloaded row's audio is dropped at once when no other row (a SAMPLES row or an instrument's member)
  holds the same content and nothing pins it, else when unpinned (`pcache::forget`).
- **Preload at project load:** after the lists are read, the SAMPLES rows of the used set, the most seen first, as
  prefetches (free space only: up to the budget, nothing evicted) (`pools.cpp` `preload_step`). DSP SAMPLE also
  prefetches its bank A files under `/Samples` whenever bank A changes (`pool_sync`). WAVETABLES and INSTRUMENTS are not
  preloaded: a table loads on use, an instrument loads its heads on use.
- **WAVETABLE's tables** keep their own memory (6 built-ins plus 4 card slots of mip frames, 7200 KB a slot on the Mac
  [measured: test_loadstall's log], set aside at boot). The figure counts them in the budget and shows them apart.
- **The figure** (`pools::ram()`): budget = the arena + the tables' memory; used = the cache's bytes; tables apart;
  `pct = (used + tables) * 100 / budget`. Shown as `RAM 412 MB 40%` (touchscreen), MB and % in the Rytm page's header
  and the web's RAM field.

Measured on the Mac (`./core/build/test_pcache`, the binary built 2026-10-22 04:58, before a9820ae which changed only
an initializer for the Pi's toolchain) [measured]:

```
test_pcache: 6 x 384 KB + 96 KB into 2 MB: 1 evictions, 1969 KB used
test_pcache: PASS (8 loads, 1 grows, 2 shared)
```

## 6. The interfaces

### The Rytm's bank list (host-drawn, `core/ui/page_wavegrain.cpp`)

- Knob A on the SRC page (4A) opens the machine's own list: tabs `< A B C D E F G H >`, rows `A001 name`, the cursor
  inverted with its state (LOADING, its length, MISSING). LEFT / RIGHT change the bank, the knob and UP / DOWN the row,
  YES chooses (48), NO cancels. A bank shown is the bank takes go to (`pools::shown`).
- **Names (47):** every bank in the 47 form with its bank byte. Bank A sends all 128 rows named (`OFF`, the name, an
  empty row `A005`); banks B..H the rows that hold an entry (the Rytm names OFF and empty rows itself). Names carry the
  state prefix (`!name`, `~name`). rui1043 shows bank A like B..H on the Rytm, also before the first 47 after a cold
  boot (`A005`, no IN / MAIN texts) [measured in rytm-emu by rytm-mods 0026 "1043"; not on hardware].
- **FUNC + YES in every bank** (`4A ... 3`): the machine's browser (srclib) aimed at the row. The root lists the list's
  virtual folders (GRANULAR `@INPUTS/` `@BUILT-IN/`, WAVETABLE `@BUILT-IN/`) and then the card's folders (the Sidecar's
  system folders left out); a folder lists its subfolders and the files the list takes (`.wav .aif .aiff`; MULTISAMPLE
  `.sfz`). YES on a folder opens it, on a file sets the row (`SET C005`, `READING C005` for an `.sfz`), NO goes up. The
  cursor resting previews a file already in the list from its row. Refused: row 000 (`000 IS OFF`), an input /
  built-in row (`BUILT-IN: UNLOAD FIRST`), a locked list (`POOL LOCKED`). A file already in the list moves the list to
  its row (`IN POOL C004`). P3 as in section 4.

### The Rytm's POOL page (host-drawn, `core/ui/page_pool.cpp`)

An extra remote page after AUTO, shown where SAMPLE is (the Pi's machines). Header: `POOL`, the list and the RAM figure.
Five rows (`A001 name`, a selection `*`, the cursor inverted with its state: `RAM`, `LOADING`, `MISSING`, `CHANGED`,
`LIVE`, `BUILT-IN`, or the size; other rows in use show `USED`). Footer: `D <action>  YES DO`, or the question.

| control | does |
|---|---|
| knob A | the list: SAMPLES / TABLES / INSTR |
| knob B | the bank A..H (the bank takes go to) |
| knob C, UP / DOWN | the row (FUNC: 8 at a time); in the browser, the item |
| knob D | the action: SELECT, UNUSED, ALL, NONE, UNLOAD, REPLACE, UNDO |
| YES | does the action: SELECT toggles the row; UNUSED selects the unused rows (the warning in the footer); ALL the bank's filled rows; NONE clears; UNLOAD the selection (or the row), P3; REPLACE opens the list's browser aimed at the row (YES into a folder or sets the file, P3; NO up, at the top back to the rows); UNDO |

### The touchscreen (`pi/ui/sidecar_wgview.c`, `pi/engine/wgview_pi.cpp`)

The machine view's **POOL** key shows the selected voice's list (GRANULAR SAMPLES, WAVETABLE WAVETABLES, MULTISAMPLE
INSTRUMENTS): bank tabs A..H, rows `000 OFF`, `001 name`, `---`, the states and an in-use marker (filled: voices on it
now, `x3`; an outline: seen playing), the RAM figure in the title. A tap on a filled row plays it on the voice (the
Rytm's P1 through 48: `gr=pick:V:C005`, `wt=pick:...`, `msamp=pick:...`); on an empty row FILES aimed at it, or FILL B
the folder into the bank's empty rows (not for instruments). A long press: REPLACE / UNLOAD / ACCEPT with the P3 confirm
and UNDO; SELECT UNUSED with the warning. **ALL POOLS** (or Settings > Pool) opens the **POOL page** (page 15): the same
with the three lists' tabs and their counts; DONE goes back where it came from. Details and the shots:
`docs/pi-display.md` "POOL".

### The web page (`pi/engine/www/index.html`, `pi/engine/poolweb.cpp`)

- **Pool tab:** SAMPLES / WAVETABLES / INSTRUMENTS, banks A..H, the project, the RAM figure, the rows (row, name,
  state, size, played count, voices now), Accept (CHANGED), Replace (from a small file browser), Unload, Select unused
  (the warning), Select none, Unload selected, Undo, Refresh. P3: a row in use asks, then sends `:yes`; refused while the
  Rytm plays. It polls `GET /pool?l=smp|wt|inst&b=A..H[&u=1]` every 2 s while shown: one bank as JSON with the lists'
  status and the RAM figure (`u=1` adds SELECT UNUSED's rows and the warning). `/pool` only reads (core 0, racy for
  display); edits are the console's `pool=` lines through `/cmd`, run on the control core.
- **Files tab:** **+SMP** / **+WT** next to a WAV / AIFF, **+INST** next to an `.sfz` (`pool=K:add:B:PATH`: the first
  free row of the POOL bank chosen above the table; a file already in that list keeps its row), **Fill bank** on a folder
  (`pool=K:fill:B:DIR`; empty rows only, name order; SAMPLES and WAVETABLES only: INSTRUMENTS are refused with "add
  each .sfz with +INST"). "Pool & presets" is now **Presets**. `bank_json` carries `ro` (a foreign-newer list: edits in
  RAM only).

### The console (`pools::command`, the control core; the web's `/cmd`, the serial `:` lines)

| line | does |
|---|---|
| `pool` / `pool=status` | the project, the three counts, reads, saves (failed), templates, fills, the lock reason |
| `pool=K:set:A010:PATH[:yes\|:force]` | a file into the row (a replace when the row holds one; P3) |
| `pool=K:replace:A010:PATH[:yes\|:force]` | the same |
| `pool=K:add:B:PATH` | a file into bank B's first free row (`IN POOL` with its row when it is there already) |
| `pool=K:clear:A010[:yes][,A011..]`, `pool=K:unload:...` | rows emptied (the card file and the library stay; P3 per row) |
| `pool=K:fill:B:FOLDER` | the folder's files into bank B's empty rows, name order, 16 files a step (smp, wt); a missing folder fills nothing |
| `pool=smp:input:H001:IN`, `pool=K:builtin:H011:PAD` | an input / built-in into an empty row |
| `pool=K:assign:A010:I[:yes]` | item I of the list's browser into the row (the touchscreen's FILES) |
| `pool=K:accept:A010` | a CHANGED row takes the file's new content |
| `pool=K:shown:B` | the bank a view shows (takes go there) |
| `pool=K:unused` | SELECT UNUSED's rows, with the warning |
| `pool=K:list[:B]` | the rows in the log |
| `pool=K:undo` | the last replace / unload back |
| `pool=K:ram:A010\|all:on\|off` | always in RAM (docs/STEREO.md "Streaming budget"): MULTISAMPLE holds the row's instrument (or the whole list's) whole, never streamed; kept in `pool-used.txt` (`ram inst A010`, `ram inst *`) |
| `browse=gr\|wt\|ms:cd:PATH\|up\|root\|open:I`, `browse=changed:PATH` | the browsers (srclib) |
| `pool=S:path` (S 1..127, the path under `/Samples`; `pool=S:` empties) | DSP SAMPLE's older line, kept for the page: SAMPLES row A00S, forced (no P3) |

K is `smp`, `wt` or `inst`. The older `lib=gr|wt|ms:` pool commands are gone; `lib=` is the library index's console
(`docs/LIBRARY.md`).

## 7. DSP SAMPLE on every bank (rui1045)

> **Where you pick the sample differs per machine (on purpose, for now).** WAVETABLE and MULTISAMPLE open their POOL
> list from **encoder A** of their page; **DSP SAMPLE** opens it from **encoder D (SMP) on its SMPL page**, because its
> SRC page keeps room for the resonator (the user, 2026-10-29). FUNC + YES on DSP SAMPLE's SRC page opens the sample
> browser. A later clean-up may line the machines up (docs/BACKLOG.md "DSP SAMPLE's picker").

- SMP (P11, word 19) holds `index << 8 | bank << 5` like GRANULAR's SRC (rytm-mods 0026 / 0021 "1045": its own record,
  fine-flagged, so a p-lock keeps the bank over a power-off; 0029 keeps the bank on a turn). It plays **SAMPLES row
  `bank` / `index`** of all eight banks. An old kit or lock (7 bits) reads as bank A.
- **On the Rytm:** turning SMP (knob D of DSP SAMPLE's SMPL page; knob A of its SRC page stays ENG, the engine) opens
  the same bank list as GRANULAR's SRC (4A with P11's value; the Rytm shows the Sidecar's frame over the SMPL page while
  it is open): the tabs A..H, the SAMPLES names, FUNC + YES loads a file into the row, YES commits `48 <t> 10 <index>
  <bank << 4> 1` (with trigs held a P11 lock). The knob's own text is the SAMPLES names of the bank the Rytm holds (47
  list 0, as GRANULAR's SRC: C005 / the name / OFF). The list page: `SAMPLE`, the SMPL page's labels
  (`page_wavegrain.cpp` `kDsBankPage`; no GRANULAR preview on a DSP SAMPLE voice).
- **FUNC + YES on DSP SAMPLE's SRC page** (rui1049 + fix1049; the hardware pass 2026-10-23: the SRC view said SMP OFF,
  FUNC + YES there opened nothing and SMP on the SMPL page only stepped through the empty rows of a new project's
  SAMPLES list): the Rytm opens the bank list on SMP's row (4A on, P11) with the host's file browser at once (4A on 3,
  why 6). When SMP is OFF the host aims the browser at the bank's first free row (A001 in an empty list; `BANK FULL:
  PICK A ROW` when none is free). YES on a file under `/Samples` sets that row (`SET A001`), the list stays open on it,
  YES chooses it (48: SMP = A001); the view then draws its waveform and the pad plays it. The same FUNC + YES on the SRC
  view of GRANULAR, WAVETABLE and MULTISAMPLE opens their list's browser on P1's row. In the list itself FUNC + YES on
  row 000 stays refused (`000 IS OFF`).
- **What a user does** with samples on the card and an empty SAMPLES list (every project made before the pools, or new):
  on the DSP SAMPLE track's SRC page FUNC + YES, pick the file, YES, YES. More at once: the web page's Pool tab or the
  Files tab's +SMP / Fill bank, or the touchscreen's POOL page. There is no migration from the old per-machine pool file
  (`/.rytm/pools/<K>.txt`, section 1): its slots are not read.
- `pool[]` mirrors the eight banks (`kPoolLines` = 8 x 127, `pool_sync` when the list's `sig` moves): slot = bank x 127
  + row (`pool_slot`; bank A's rows are slots 1..127 as before, so the console's `pool=S:path`, the user machines' pool
  slots and the 14 table keep meaning bank A). A sample row its resolved file, anything else an empty slot.
- **The 14 table** (bank A only) is still sent for the Rytm builds before rui1045, whose SMP opened stock's sample
  list from it; rui1045 never shows it. **0F LOAD TO PROJ** (the Rytm's +Drive TEENSY browser) puts the file into bank
  A's first free row (`add_file`: one entry per hash); **12 ASSIGN** (stock's list's FUNC + YES, before rui1045) sets row
  A00S (`set_file` with `kForced`).
- A file under `/Samples` that the card index holds plays as before (stream or sample RAM, now the pool RAM). A file
  **outside `/Samples`**: the 14 table sends the name only (folder -1), and DSP SAMPLE plays it from the pool RAM only,
  never streamed (`voices.cpp` `ram_key`: `SampleRamPool::kSlotKey - line`).
- `sampleram=0` still turns DSP SAMPLE's RAM off (stream only); any other value only enables it, the size is the pool
  RAM's (`sample_ram_pool.cpp`).
- The old `/.rytm/pools/<K>.txt` is not read on the Pi / Mac (the Teensy keeps it).

## 8. Takes and auto-sampled instruments

- A take (`core/samp/sampler.cpp`) goes into SAMPLES, the first free row of the bank last shown (default A, which also
  makes it DSP SAMPLE's), else the next bank with room (`pools::add_take`). The page and `/status` (`samp.row`) say
  `TAKE -> A012`. Its slice map (`/MULTISAMPLE/<project>/<name>-slices.sfz`) goes into INSTRUMENTS the same way (the
  `.sfz` read on the card core: set a moment later).
- An auto-sampled instrument (`core/samp/autosamp.cpp`) goes into INSTRUMENTS the same way.
- Instrument adds that arrive while the card job is busy (a list being written, another `.sfz` being read) are queued and
  started in turn; a take or instrument whose bank is full goes to the next bank with room.
- The take's own record buffer is the sampler's, not the pool RAM's.

## 9. Bundles

The three list files and `pool-used.txt` travel as opaque `kind: pool` files with their formats in the manifest; the
collector takes the samples and instruments (members included) from `pools::refs()` (an instrument copied with its
members in one window); when `refs()` reports an incomplete pass (-1: a row kept changing) the collector also reads the
list files. sha256 and CRC-32 come from the
library; no pool path rewriting on import; no bank A record or pinning. See `docs/BUNDLES.md` "Pool lists".

## 10. Tests [measured]

All in `make -C core test` unless named otherwise.

| test | checks |
|---|---|
| `core/tests/test_pools.cpp` | the template (no migration), a card error locking the list (never the template over it), resolution-only changes held while playing, the member table's compaction, an instrument queued while the card job is busy, a voice off the machine holding no row, undo refusing a duplicate, a fill on its own browser (a missing folder fills nothing), OFF at row 0, the entry types each list takes, one entry per hash, the files written by the card core and read back the same (the golden files round-trip), a newer / damaged file locked and never written, every resolve state on an entry and a member, a `-` ref filled once hashed, `refs()` with members, the used set and SELECT UNUSED, P3 (asks, refused while playing unless forced, undo), a fill, an instrument's members from its `.sfz`, a project switch |
| `core/tests/test_fmt.cpp` + `core/tests/golden/{samplepool,wtpool,instpool,poolused}/v1/` | the registry's golden harness; `make -C core fuzz-fmt` reads the lists too |
| `core/tests/test_pcache.cpp` | whole loads through the card core, int16 mono, one entry per content, prefix grown to whole, LRU never of a pinned entry, the budget never exceeded, prefetches never evicting, private blocks, forget |
| `core/tests/test_srclib.cpp` | the browser (600 files lazily, `@INPUTS/`, `@BUILT-IN/`), the codes on every machine, GRANULAR on C002, OFF / empty / missing silent, a fill, MULTISAMPLE B001, WAVETABLE D009, GRANULAR and DSP SAMPLE on the same bank A row, an input silent on DSP SAMPLE (an empty slot), DSP SAMPLE's C007 (slot 2 x 127 + 7, `pool_slot`) |
| `core/tests/test_pool_page.cpp` | the Rytm POOL page: tabs, RAM, rows, SELECT, UNUSED, UNLOAD with P3, REPLACE through the browser, UNDO; PNGs to `build/rui_pool` (`pool_1_samples.png` ...) |
| `core/tests/test_loadstall.cpp` | list loads, saves and the used set on the card core only; no control-core card time |
| `core/tests/test_ui.cpp`, `test_wg_view.cpp` | the Rytm bank list on the pool rows: FUNC + YES in bank A, row 000 and built-in rows refused (test_ui); the list views on SAMPLES rows, one entry per file (test_wg_view) (the bank list's own P3 texts are not checked by a UI test: P3 is checked in `test_pools` and `test_pool_page`) |
| `core/tests/test_bundle_pools.cpp` | the bundle collector on `refs()` |
| `pi/engine/tests/test_poolweb.cpp` (`make -C pi/engine/tests`) | `GET /pool`'s JSON |
| `pi/engine/tests/page_pool.test.mjs` (`node --test`) | the Pool tab and the Files buttons against a fake console and `mock_web.py` |
| `pi/ui/host/shot.c` (`make -C pi/ui/host`) | the touchscreen POOL view and page, both themes |

`./core/build/test_loadstall` (built 2026-10-22 04:57, before a9820ae) [measured, Mac, the host's card model]:

```
test_loadstall: 1 MB WAV (C001): 423 control passes, longest 699 us (card 0 us of it), 0 over 2 ms (0 us of silence); control-core card time 0 us; card core 90505 us (longest service 32125 us)
test_loadstall: 10 MB WAV (C002): 1001 control passes, longest 541 us (card 0 us of it), 0 over 2 ms (0 us of silence); control-core card time 0 us; card core 581148 us (longest service 22125 us)
test_loadstall: three commits back to back: 1 pool writes
test_loadstall: project switch and back (the lists read): 36 control passes, longest 184 us (card 0 us of it), 0 over 2 ms (0 us of silence); control-core card time 0 us; card core 20752 us (longest service 3376 us)
test_loadstall: PASS
```

## 11. Differences from the design

What the code does where it differs from the spec [measured: read in the code on this branch].

- **Mono cache.** The spec's cache holds "native rate and channels"; `pcache` holds int16 **mono** (stereo summed once,
  at load). Every machine here plays mono (GRANULAR, DSP SAMPLE, MULTISAMPLE's voice channel), so one mono copy is kept.
- **The POOL page's actions are on knob D**, done with YES. The spec said "RIGHT for SELECT ..."; LEFT / RIGHT do nothing
  there (ux1027: the page replaces the Rytm's screen, no LEFT / RIGHT siblings; before, they were the ring's).
- **Preload covers the SAMPLES list only** (the used set's rows, most seen first, plus DSP SAMPLE's files).
  WAVETABLES load on use; INSTRUMENTS load their heads on use. The spec's "the Sidecar sequencer's and songs'
  references" are not preloaded separately.
- **Instrument identity is the `.sfz`'s own hash.** The spec's group identity (a sha256 over the `.sfz`'s and its
  members' hashes) is library L4 and not built; two `.sfz` files with the same text are one entry.
- **DSP SAMPLE outside `/Samples` plays from RAM only** (no stream), and its page shows no waveform (EMPTY SLOT). A file under
  `/Samples` that the boot-time card index does not hold (a take written since boot) gets no folder / file and is silent
  on DSP SAMPLE until the next boot [inferred: `pool_sync` / `ram_key`].
- **The used set** is fed by the machines' reports (GRANULAR, WAVETABLE, MULTISAMPLE, DSP SAMPLE) and the bank list's
  YES commits, not by the touchscreen's or the console's 48 writes, the Sidecar's sequencer or songs as separate
  sources. It is written only when a new code is seen (a count alone waits) and never while the Rytm plays.
- **No FUNC override:** the UX review's "refuse while playing unless FUNC is held" is not built. Every surface refuses
  a row in use while the Rytm plays (`PLAYING: STOP FIRST`); only the console's `:force`, `pool=S:path` and DSP SAMPLE's
  ASSIGN bypass it.
- **P3 counts "seen in this project"** as in use, not only voices now: once a row has played, every replace asks.
- **FILL is not done for INSTRUMENTS** (`pools::fill` refuses the list; the touchscreen and the web do not offer it).
- **Bank A's total in the 47** is 127 (the spec said `total = 128`); rui1043 no longer reads `total` for the text.
- **The pool RAM's wavetable figure** is memory set aside at boot outside the `poolram=` arena: the shown budget is the
  arena plus it, so `poolram=` alone does not bound the tables.
- **WAVETABLE keeps 4 card slots** (its own LRU) and GRANULAR views 8 card sources at once (pinned blocks of the pool
  RAM); the spec did not fix these numbers.
- **The old console `pool=S:path`** (DSP SAMPLE slot S, a path under `/Samples`) stays, as SAMPLES row A00S, forced
  (it skips P3).

## 12. What to check on hardware

- A Pi 4 (2 / 4 GB) and a Pi 5 (8 GB) boot with the default `poolram=` (log `pcache: 1024 MB of pool RAM` / 3072) and
  with an explicit value; the RAM figure on every surface agrees.
- A 1 GB-class list load, save and project switch with the Rytm streaming 8 voices: 0 underruns (the Mac model says no
  control-core card time; the real card's times are unmeasured).
- FUNC + YES into A005 and C001 of each list from the Rytm (rui1043), the refusals (000, built-in, locked), `IN POOL`,
  the P3 ask and `PLAYING: STOP FIRST` with the transport running.
- Bank A's names after a cold boot on rui1043 (no IN / MAIN), and WAVETABLE's new track default (A001 BASIC).
- DSP SAMPLE (rui1045): SMP on the SMPL page opens the bank list, every bank, FUNC + YES into C001 and H020, the knob's
  name text, a p-lock on bank C over a power cycle, an old project's SMP read as bank A; a file outside `/Samples`
  plays from RAM; an input row silent; LOAD TO PROJ into bank A.
- The same WAV in SAMPLES and as an instrument member: one copy in the figure.
- A take: `TAKE -> A012` on the page, playable at once by GRANULAR; DSP SAMPLE after the next boot (any bank).
- The preload after a project switch (the most played rows resident before their first trig) and SELECT UNUSED's rows
  against what the Rytm's patterns actually lock.
- The touchscreen POOL view and page, and the web Pool tab, on a real panel and browser while the Rytm plays.
- The self-test's DSP SAMPLE note (`selftest=1`): `bridge::selftest_content` now writes
  `/.rytm/projects/_default/samples.pool.txt` with the test WAV in A001 (when no `/.rytm/project` exists); its note 48
  should play 220 Hz.
- A card error on a list file (pull the card mid-read): the list locks, nothing is written over it.
