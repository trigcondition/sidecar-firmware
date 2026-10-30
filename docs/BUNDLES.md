# Project bundles (Pi)

A Sidecar project as one file, `<NAME> <YYYYMMDD-HHMM>.sidecar.zip`: its Sidecar files, every sample it uses, its song
takes, the user machines it plays and, if you add it, the Rytm project file. Move it to another Sidecar (Pi 4B or
Pi 5), or keep it as a backup, and import it there: the same sounds in the same slots, the same patterns and songs.

Design: `docs/superpowers/specs/2026-10-22-project-bundles-pi-design.md`. Built: phases 0-5 (the web page, the
touchscreen's and the Rytm's PROJECT pages, exact machine versions, USB sticks), and the bundle on the project's POOL
lists (the pools design's phase 5, `docs/superpowers/specs/2026-10-22-pools-tonverk-design.md` section 6: "Pool lists"
below); phase 6 (the hardware runs) is to do. Tags as there: **[measured]** = run here (host
tests, the Pi kernel build), **[inferred]** = reasoned, to be checked on hardware.

## For users

**Where:** the touchscreen's **PROJECT** page (tap the home page's PROJECT panel), the Rytm's own screen (the
Sidecar's **PROJECT** page: TRK + YES, then LEFT - it is the last page), and `http://sidecar.local`, the **Project**
tab. All three drive the same job; one job runs at a time.

### On the touchscreen

- The list: every project the card knows (the Rytm's current one `*`, an imported one not yet named by the Rytm
  `imp.`), the size of its Sidecar files, when it was last used (the date when the Sidecar knew the time, else the
  order `#N`). A tap selects one; the right side shows its details. UP / DOWN page through a long list.
- The picker at the top: **SD card** or a mounted **USB stick** (with free space). It is where EXPORT writes and where
  IMPORT looks. **EJECT** (with a stick picked) stops a job using the stick, then unmounts it: safe to pull.
- **EXPORT > CARD / USB** makes the selected project's bundle in `/Bundles` on the card or `/SIDECAR` on the stick.
- **IMPORT...** lists the bundles there (the stick: its `/SIDECAR` and its root). Tap one, **IMPORT IT**: every byte is
  checked first (nothing is written), then the summary: the name it imports as (`NAME 2` when the card has a project
  of that name), new and reused MB, the contents, the machines (code) with a key each - tap one to leave it out -, and
  the Rytm step. **IMPORT** or **CANCEL**.
- **DELETE** (not the current project): a confirmation whose DELETE key comes on after a second. Only the project's
  Sidecar files go; samples stay.
- While a job runs: its phase, a bar, MB of MB, **WAITING (PLAYING)** / **(RECORDING)** while paused (it goes on by
  itself), **CANCEL** (not during COMMIT, which takes milliseconds). Done: what happened and, after an import, **"Save
  your Rytm project as NAME 2"**. An error: what happened and what to do, **OK**.

### On the Rytm's screen

- The list with encoder **A** (or UP / DOWN); encoder **H** picks **CARD** or a USB stick (the header shows it, the
  line under the list its free space and, with a stick in, every choice: `CARD 37G FREE H:CARD/USB` on the card,
  `USB 14G H:CARD T5:EJECT` on the stick - fix1049: before, only `H: USB` was named, and with the stick picked nothing
  said how to get back to CARD; the EXPORT question names them too, H works there). **T1** EXPORT (YES / NO), **T2** IMPORT (the bundles there: YES checks the one
  under the cursor; then the summary: YES imports, NO cancels, **T3** leaves the bundle's machines out or puts them
  back), **T3** INFO, **T4** DELETE (YES, then YES again on SURE?; never the current project), **T5** EJECT the stick.
  The trig keys' LEDs: T1 blue, T2 green, T4 red, T5 amber with a stick; off while a job runs.
- A job: its phase, a bar, MB / MB, WAITING (PLAYING); **NO** cancels. Done / failed: YES or NO goes back.
- The page posts its commands to the Sidecar's core 0; the Rytm's audio core never touches the card or a stick.

### Renamed on the Rytm (UX review B1)

The Sidecar finds a project's files by the Rytm's project name. When the Rytm names a project that has no Sidecar files
right after one that had some (a SAVE PROJECT AS, a rename), the Sidecar asks first on the Rytm's PROJECT page (it
opens by itself) and keeps the old project's state meanwhile:

    RENAMED ON THE RYTM?  FROM <OLD>  TO <NEW>  n SIDECAR FILES THERE: YES MOVES THEM ACROSS
    YES: MOVE  NO: NEW PROJ

- **YES** moves every per-project file and folder (poly tracks, the pool lists, songs, LFOs, inserts, the slot maps)
  from the old name to the new one - only names change on the card, nothing is copied or deleted - and then switches.
  If a move fails half way, the ones already moved go back and nothing else happens.
- **NO** (or no answer within two minutes, or the Rytm naming another project or the old one again) switches as
  before: the new name starts empty, the old project keeps its files.
- No question when the new name has files of its own, the old one has none, or both names make the same card key.
- It is a move: a project SAVED AS a copy leaves the original without its Sidecar files after YES. Answer NO to keep
  them with the original (the copy starts empty), or export the original as a bundle first.
- The Rytm's content is not compared (a pattern dump costs the Rytm 15 KB and its UI's time; NO is harmless), so a NEW
  PROJECT made on the Rytm right after one with files is asked about too: answer NO.
- Code: `core/bundle/rename.{h,cpp}` (every look and move through the card queue, `cardq` kExistsReq / kRenameReq),
  `core/pool/pool.cpp` set_project / project_switch, the prompt in `core/ui/page_project.cpp`; tests
  `core/tests/test_rename.cpp`.

### USB sticks

- **FAT32 with the MBR scheme** (docs/STORAGE.md "Using a USB drive"); exFAT and GPT are not read in this build
  (`STOR_EXFAT`, off by default - tested only on the Mac). Plug it into a blue USB 3 port (Pi 4).
- Export writes `<project> <date>.sidecar.zip` into the stick's `/SIDECAR` folder (made when missing) as a `.part`
  file, renamed when complete. Import reads a bundle **straight from the stick** (no copy to the card first).
- Stick I/O runs only at full rate: **while the Rytm plays, a song take or the recorder records, nothing is read from
  or written to the stick** (the job shows WAITING and goes on at STOP).
- **Eject** before pulling it (the touchscreen's EJECT, the Rytm page's T5, the web page's *Eject*). Pulled during a
  job: the job stops at its next piece ("USB stick removed"); an import leaves the card exactly as before (its staging
  folder removed); an export leaves its `.part` on the stick, listed as incomplete (delete it on a computer).

### On the web page

- **Projects** lists every project the card knows (the Rytm's current one marked `*`, imported ones not yet named by
  the Rytm marked `imp.`), with its Sidecar files and their size.
  - **Download** makes the bundle and the browser saves it. Nothing is written to the card.
  - **To card** writes it to `/Bundles/` on the SD card (copy it off with the Files tab or a card reader).
  - **To USB** (with a stick mounted) writes it to the stick picked under *USB stick* (its `/SIDECAR`).
  - **Rytm file…** adds the Rytm project file you exported with Elektron Transfer: it goes to
    `/Bundles/Rytm/<project>.<ext>` and into that project's next export.
  - **Delete** removes the project's Sidecar files (pool lists, slot maps, LFO / INSERT settings, patterns, songs). Its
    samples stay on the card. The Rytm's current project cannot be deleted.
- **Bundles on the card** lists `/Bundles/*.sidecar.zip`. **Import a bundle…** (or drop one on the list) uploads it to
  `/Bundles/incoming/`, checks every byte (one wrong byte refuses it, nothing written), then shows the summary:
  - the name it imports as: its own, or `NAME 2` when a project of that name is on the card (you can type another;
    "replace" exports the existing one to `/Bundles/.backup/` first);
  - how much is new and how much is already on the card (a sample with the same content is never copied twice);
  - the **machines (code)** in the bundle: installed by default, untick one to leave it out; a machine of the same
    version that differs from the installed one is a *conflict* (the installed one stays);
  - **the Rytm step:** "Send `<file>` to the Rytm with Elektron Transfer, then save it on the Rytm as `<NAME>`." The
    Sidecar never sends anything to the Rytm. Save the Rytm project under exactly that name: the Sidecar finds its
    files by the project's name.
- **USB stick**: the mounted sticks (a picker with free space), **Eject**, and the stick's bundles (its `/SIDECAR` and
  root): **Import** reads one straight from the stick.
- While a job runs the tab shows its phase and progress. **WAITING (PLAYING)**: the Rytm plays, card writes wait for
  STOP (reads go on slowly). **WAITING (RECORDING)**: the recorder records, the job waits. It continues by itself.
- **Large downloads:** a browser streams a bundle of any size to disk only on a secure page (https, or `localhost`
  through a tunnel); on plain `http://sidecar.local` the page collects the bundle in memory first, up to 1 GB. For a
  bigger one use **To card**.
- On a Mac or PC any bundle opens as a normal zip. `python3 tools/bundle.py verify|inspect|list|extract BUNDLE`
  checks one (every file's sha256) and lists or unpacks it.

What a bundle does **not** hold: the Overbridge recorder's takes (`/Recordings`), this Sidecar's own settings
(display, keys, Wi-Fi - never secrets), the FX return route (recorded as information only).

## For developers

### Code

| File | What |
|---|---|
| `core/project_key.{h,cpp}` | `project_key()`: the one name -> card key rule (was 7 copies); `project_key_song_v0()` (the song folder's old rule, for its migration); `project_key_sampler()` (the sampler's `/Samples/<name>/`) |
| `core/bundle/projects.{h,cpp}` | `/.rytm/projects.txt`, the project index (6.1): written through the card queue; core 0 changes it through a request ring the control core drains; read with a seqlock; the wall clock (`POST /time`) |
| `core/bundle/sha256.*`, `zip.*`, `json.*`, `manifest.*` | the container: own SHA-256, the stored zip writer (`layout` + `render`: any byte range of the archive from the entries alone) and reader (end record, central directory, local headers), the manifest and its bounded reader |
| `core/bundle/bundle.h` | the job API; `bundle.cpp` (state, project table, list, delete, recovery), `export.cpp`, `import.cpp` |
| `pi/engine/bundle_pi.{h,cpp}` | the Pi glue: the environment (pause rules, the card queue, the current project, the browsers' listings after an import), one step per core 0 loop pass |
| `pi/engine/bundleweb.{h,cpp}`, `www/bundle-sw.js`, the Project tab in `www/index.html` | the web endpoints, the download's service worker, the page |
| `core/bundle/feed.{h,cpp}` | the PROJECT pages' feed: core 0 publishes the projects, the sticks, a source's bundles, the job and an import's summary (seqlock); the `bundle=...` console lines; a ring for the Rytm page's commands |
| `core/ui/page_project.cpp` | the Rytm's PROJECT remote page (`kProjectPage`, after SONG) |
| `pi/ui/sidecar_project.{h,c}` | the touchscreen's PROJECT page (page 14) |
| `core/host/platform_host.cpp` `host_set_volume`, `pi/engine/platform_pi.cpp` | paths that name a drive (`USB:/...`): the Mac's are folders, the Pi's FatFs volumes in 64 KB pieces through an aligned block |
| `tools/bundle.py`, `tools/make_bundle_fixture.py`, `core/tests/demo/bundle_tool.cpp` | a Mac reader / checker; a fixture card; the job on a Mac over a card folder (`make -C core bundle-tool`) |

### Phase 0: one project key, the index, `/Bundles`

- Every per-project file now uses `project_key()` (pool, LFO, INSERT, sequencer, JUNO, FM, songs; the slot maps,
  fxslots, GRANULAR picks and the machines' pools already used the pool file's name). The rule: `A-Z a-z 0-9 - _
  space .` kept, anything else `_`, 23 characters at most, trailing spaces / dots trimmed, `_default` when empty.
- **Songs** used another rule (spaces and dots as `_`, no trim, `_none`). `song::folder()` renames the old folder to
  the key's once per project, when the key's does not exist (both there: the key's is used, the old one left alone).
- Changes on the card: song folders of names with spaces / dots (renamed once, above); the LFO and INSERT files of a
  card with no project named yet (`/.rytm/lfo/.txt` before, `/.rytm/lfo/_default.txt` now, as the others).
- `/.rytm/projects.txt`: `key TAB rytm name TAB last-used counter TAB last date or - TAB flags` (`i`: imported, not yet
  named by the Rytm). Updated on each project change (`set_project`), written by `idle_save` through one `cardq`
  `kWriteReq` (no card time on the control core with a card core: `test_project_key`).
- `/Bundles` is hidden from the machines' library (`srclib::hidden_name`) and kept by the web file browser
  (`fsweb::kept_dir`: its files can go, the folder cannot). `.staging`, `.backup` under it start with a dot.

### Format (phase 1)

Container rules (design 3.1) [measured: `test_bundle_zip`, Python's `zipfile`, `unzip -t`]: stored entries, no data
descriptors, no ZIP64 (at most 4 GiB - 64 KiB, refused with what takes the space), UTF-8 names, every entry's data on a
4096-byte boundary (the `0xD935` extra field), `manifest.json` first. The layout inside is the design's 3.2
(`project/...`, `files/<card path>`, `plugins/<id>-<a.b.c>.<target>.rmp`, `rytm/<name>`).

The manifest (design 3.3) with these additions:

- **`formats`**: `sqp`, `song`, `slots` as before, plus each pool list's format and version as its file's header
  names it (`"samplepool":1,"wtpool":1,"instpool":1,"poolused":1`; the formats spec's registry). A reader keeps names
  it does not know (`Manifest::formats`).
- **`format` per entry** (`{"name":"sqp","version":3}`, `samplepool`, `wtpool`, `instpool`, `poolused`, `slots`,
  `song`, `rmp`, ...): the per-file format versioning seam. `min_reader` is the highest `reader_for(format, version)`
  of the entries (1 for everything today); the manifest's own format is `format`/`version`.
- **sha256 is a sample's identity**; the card path(s) are where it was. Same content at two paths: one entry, both
  paths.
- `missing`: list entries (or instrument members) whose file was not on the card (listed, not an error).
- No more `pools` descriptors and no `bank_a` record (the pools design 6): the lists are opaque files and every row's
  address is the same on any card, so there is no bank A to record or pin. An older bundle's `pools` and `bank_a` are
  read past.

### Jobs (phase 1)

One job at a time; `bundle::step()` does one piece: at most one 64 KB read or write (16 KB reads while the Rytm plays),
or a few file opens, or 16 renames.

- **Export:** FLUSH (the current project: the idle saves get 1.5 s, the pool lists' pending writes up to 5 s more; a
  recording song take refuses it) -> COLLECT (the project files into a RAM snapshot, read in pieces; the pool lists'
  samples and instruments with their members, below; slot maps / .sfz parsed; the sampler's takes; packages by id +
  version; the Rytm file) -> HASH (sha256 + CRC-32 of each file the library has not hashed; duplicates merged) -> WRITE (`/Bundles/<name>.part`,
  each entry's sha256 checked again as written, then renamed) or SERVE (`serve()`: any range; each file's size checked
  before its first byte).
- **Import:** VERIFY (central directory, local headers, the manifest, every entry's sha256 and CRC; nothing written; the
  `.sfz` texts kept in RAM) -> PLAN (the name; per sample: the same content at its own path -> reuse; the sample
  library's file with that content (`lib::find_file`, docs/LIBRARY.md; the copy nearest its own path; else
  `Env::find_by_hash`) -> reuse after a rehash; its path free,
  under `/Samples`, not a machine's scanned folder -> written there; else under `/Samples/Projects/<key>/`; an
  instrument moves with the samples it names relatively; space) -> READY (the summary) -> [BACKUP] -> STAGE (each new
  file to `/Bundles/.staging/<n>/fNNNN`, read back, sha256 checked; packages checked as `fsweb::check_package` does) ->
  COMMIT (`commit.txt`: `D`/`d`/`R` lines then `END`, written in pieces; then the renames) -> LINK (the index; rescans).
- **Boot recovery** (`bundle::recover()`, in `engine_begin` before the card is indexed): a staging folder whose
  `commit.txt` ends in `END` is rolled forward (every line again: a rename whose source is gone is skipped), any other
  is deleted [measured: `test_bundle` cuts the power after each of an import's 140 card changes: 45 cuts leave the card
  as before, 42 as after, none in between].
- **Delete:** the project's files (the table both directions use, the old per-machine pools included), then its own
  folder `/.rytm/projects/<key>` when nothing else is in it; never the current project, never samples. Keys are
  checked (`project_key(key) == key`): nothing else reaches a card path.
- **Replace:** the old project is exported to `/Bundles/.backup/` first; commit.txt deletes only the old files the
  import does not write again (a replayed delete must not remove a renamed file: `test_bundle` cuts the power after
  each of a replace's 99 changes); files an export does not carry (an open song journal, dot-files) stay.
- **Before COMMIT** (`C_CHECK`): every path planned as free is still free (a web upload in the meantime stops the
  import), and a replaced project has not become the Rytm's current one.
- The sequencer's folder is carried whole: `pNNN.sqp` (working copy), `pNNN.sqs` (saved state) and `pNNN.prv` (the
  previous copy) as kind `seq` with their format and packing version, `tracks.txt`.

### Never disturbing the audio (phase 2)

- The job and the web endpoints run on **core 0** (its loop: one step a pass, between the card queue's services; the
  web tasks: `/bundle/zip` ranges). Core 3 only reads `bundle::status()`; `transport()` stores one flag.
- A step runs only while `cardq::busy()` is false (no stream below its low mark, no request waiting).
- Pause rules (decided 2026-10-22): stopped: full rate; the Rytm playing or a song take recording: reads only, 16 KB,
  one every 2 ms; the Overbridge recorder recording: nothing.
- `test_bundle_stall` [measured on the host card model]: a 12 MB export and import with a GRANULAR load and a stream
  going low every 200 passes: 0 us of card time on the control core, the longest control pass 0.18 ms, bundle steps held
  back while the stream was low, every step at most 64 KB read / written, the longest read step 8 ms of card time, a
  commit step (16 renames) 64 ms; playing: no card change at all, reads in 16 KB pieces; recording: not a byte. The card
  model charges a file's written sectors at its close (2 ms a sector), so a write step's modeled time is not
  meaningful; at the recorder's measured 9.2 MB/s a 64 KB piece is ~7 ms [inferred].

### Web API (phase 2; `pi/engine/bundleweb.h`)

| Endpoint | |
|---|---|
| `GET /bundle/projects` | `[{"key","rytm","used","date","imported","current","files","bytes"}]` |
| `GET /bundle/list?src=card` | `[{"name","path","size","date","incomplete"}]` in `/Bundles`; `src=usb[&vol=USB:]`: the stick's `/SIDECAR` and root (`[]` without a stick) |
| `GET /bundle/status` | `{"ok","job","phase","done","total","key","file","name","msg","waiting","bytes","seq","clock"}` |
| `GET /bundle/plan` | the import summary (READY) / done screen (DONE): name, clash, new / reused bytes, files, relocated, machines with `action` (install, reuse, conflict, side-by-side, left out, not for the Pi, refused), missing, warnings, `rytm`, `step` |
| `GET /bundle/zip?off=&len=` | SERVE: the archive's bytes, at most 64 KB; empty with `Retry-After` while paused; 404 when not serving |
| `GET /bundle-sw.js` | the download's service worker (scope `/bundle-dl/`) |
| `POST /bundle/export` | `key=&dest=browser|card|usb[&vol=USB:][&takes=0]` (usb: the first stick when `vol` is left out) |
| `POST /bundle/verify` | `path=/Bundles/...sidecar.zip` or `USB:/[SIDECAR/]...sidecar.zip` |
| `POST /bundle/import` | `[name=][&mode=new|replace][&packages=0|1][&skip=<slot bits>][&slot=1..128]` (a changed option plans again first; `slot=`: a card keyed by the Rytm's slots - the plan's `slot` and `slots`, a slot with Sidecar files only with `mode=replace`, which moves them to the project trash in the import's own commit) |
| `POST /bundle/cancel`, `/bundle/finish` | cancel (before COMMIT), dismiss the done / failed screen |
| `POST /bundle/delete` | `key=&confirm=<key>` |
| `POST /time` | `epoch=<s since 1970 UTC>`: the page's clock (bundle names, the index's dates) |
| `GET /usb/status` | `{"present","vols":[{"drive","name","free","total","free_known"}],"job":<the job's drive>}` |
| `POST /usb/eject` | `[vol=USB:]`: a job on it cancelled first, then unmounted (409 with why not) |

Every POST must pass the strict same-origin check (as the Wi-Fi settings): refused with `{"ok":false,...}`. Answers are
HTTP 200 with JSON (Circle's error pages carry no message), except `/bundle/zip`'s 404.

### Phase 3: the PROJECT pages

- **The feed** (`core/bundle/feed.h`): core 0 (`bundle_pi::poll`, after each step) runs `feed::tick()`: it drains the
  Rytm page's command ring, reads the lists when owed (a page opened - `bundle=list:<src>` -, a job ended, a stick came
  or went; never while a job runs or the streams need the card) and publishes a `Feed` (about 15 KB) when anything
  changed. Any core reads it with `feed::copy()` (a seqlock). The commands are console lines, so the serial console
  drives the same pages: `bundle`, `bundle=list[:card|USB]`, `export:<card|USB>:<key>`, `verify:<path>`,
  `import[:<skip bits>[:<packages>]]`, `delete:<key>`, `cancel`, `finish`, `eject:<USB>`.
- **Rows:** `project_rows()` (the current project first, then the last used, then by name) and `bundle_rows()` (the
  newest first, each with its path), `plan_info()` (the summary as a struct).
- **The Rytm's page** draws from the feed and posts commands; it takes T1..T5 (`trigs_now`), encoders A and H, UP /
  DOWN, YES, NO - all in the catch mask the host already sends (rytm-mods 0026 design.md section 10: key codes
  0x00..0x0F are the trig keys; YES / NO / UP / DOWN are in every page's mask). **No Rytm-side change is needed**
  [measured: `sim/rui_e2e.py project` on rui1042: the mask `encs [0, 7]` with T1..T5, the keys reach the page].
- **The touchscreen's page** runs on core 0 with the rest of LVGL; the kernel copies the feed into its view five times a
  second while it shows (`bundle_pi::ui_fill`); its commands go through the console (`bundle=...`).
- Shown while the card is up (`feed::set_available`, from `card_ok` at boot).

### Phase 4: exactness (machine versions side by side)

- Every installed version of a user machine id is kept (`core/user/um.cpp`; an import installs another version as
  `/Plugins/<id>-<a.b.c>.rmp` beside the installed one). The newest is the id's default: the only one in the Rytm's
  list, the one a fresh assignment takes; the older ones are listed as `"older": true`.
- A slot plays **the exact version its slot map names** (`U<n> <id> <a.b.c>`, which a bundle carries byte for byte)
  when it is installed; else the newest, if the wanted version is in its compatible range; else MISSING ("the slot
  wants X, installed Y"). The slot JSON says what plays (`file`, `play_ver`) [measured: `sim/tests/test_core_um.py
  test_versions_side_by_side`].
- Bank A pinning: dropped with the pools redesign (`2026-10-22-pools-tonverk-design.md` 6); the bank A record
  (`bank_a`, `Env::bank_a`) is gone too ("Pool lists" below).

### Pool lists (the pools design's phase 5)

The project's three POOL lists and its used set (core/lib/pools.h, docs/POOLS.md) travel as opaque files; the
collector asks for their references and never reads their lines itself.

| Card | Bundle | Kind, format |
|---|---|---|
| `/.rytm/projects/<K>/samples.pool.txt` | `project/samples.pool.txt` | pool, `samplepool` v1 |
| `/.rytm/projects/<K>/wavetables.pool.txt` | `project/wavetables.pool.txt` | pool, `wtpool` v1 |
| `/.rytm/projects/<K>/instruments.pool.txt` | `project/instruments.pool.txt` | pool, `instpool` v1 |
| `/.rytm/projects/<K>/pool-used.txt` | `project/pool-used.txt` | pool, `poolused` v1 |
| a sample / wavetable / instrument a list names, an instrument's members | `files/<card path>` | sample, sfz |

- **Collect:** the Rytm's current project (its lists in the pool module) through `pools::refs()` - every Ref, members
  included -, after FLUSH waited for `pools::saving()` so the carried files say the same; any other project through
  `pools::text::parse` on the snapshot of its list files. Each Ref is resolved by the library (`lib::resolve`): the
  bundle carries the file where it plays from now (a MOVED file at its new path), a MISSING one is listed in `missing`
  by its hint. An instrument's `.sfz` is a kind `sfz` entry and its members its group (they move with it on import);
  the `.sfz` is still parsed as before for `abs_refs` and any sample it names that the list does not [measured:
  `test_bundle_pools`, `test_bundle`].
- **Hashes:** a file the library has hashed (`lib::hashes_of`: hashed, on the card, the same size, its CRC known)
  takes its sha256 and CRC-32 from there; HASH reads only the others [measured: `test_bundle_pools`: an export of
  292 KB of samples reads 293 KB in all - the WRITE pass and the project files]. WRITE still checks each entry's sha256
  as it goes: a record gone stale behind the library's back (a file changed by a card reader) fails the export
  ("changed during export"), and the library is told to hash that file again, so the next export is right [measured].
  SERVE (browser downloads) checks sizes only, as before: a stale record there gives a bundle the import refuses
  [inferred: rare - every Sidecar writer tells the library, the boot walk catches card-reader changes].
- **Import:** the list files are written as they are - no path rewriting: their Refs resolve by hash wherever the
  import placed a file (relocated under `/Samples/Projects/<key>/` or reused elsewhere), and the hints are refreshed at
  the list's next save. `.sfz` absolute paths are still rewritten. The import's files go into the library with their
  sha256 (`lib::touched`), so they resolve at once [measured: `test_bundle` exports the imported project again with
  every sample, the relocated ones found by hash].
- **Old bundles** (before the redesign): their per-machine pool files (`project/pool.txt`, `granular.pool.txt`,
  `wavetable.pool.txt`, `multisample.pool.txt`, `granular.picks.txt`) are written where they were, byte for byte, and
  never read (no migration, the user's decision); their `pools` / `bank_a` are read past [measured:
  `test_bundle_pools`]. A project whose card still has such files: they are not exported (no longer read), stay on a
  replace, and go with a delete. They stay in the format registry for sniffing.

### Phase 5: USB sticks

- The volume layer is `stor_pi` (docs/STORAGE.md); the bundles only use it: `Env::volumes / vol_ok / vol_eject`
  (`bundle_pi.cpp`: the mounted `USB:`, `USB2:`, `USB3:`, their free space as counted at mount, `stor::eject`).
- A core path may name a drive: `USB:/SIDECAR/x.sidecar.zip`. On the Pi `core::File` passes it to FatFs as is and does
  every read and write on such a file in pieces of at most 64 KB through one 64 KB-aligned block in the low heap (the
  xHCI TRB rule, whatever buffer the caller has). Card paths never hold a `:`.
- A job's volume is `Status::vol`. Its stick I/O (an export's WRITE, an import's VERIFY, its instrument and package
  reads in PLAN, STAGE's reads) runs in **32 KB pieces, only at full rate** (`IoRule::write_piece` non-zero: not while
  the Rytm plays, a song take or the recorder records). A job whose stick is no longer mounted fails at its next step
  (an import while it still needs the stick, i.e. before COMMIT): the export's `.part` stays on the stick; the import's
  staging folder is removed and an empty `/Bundles` too - the card is exactly as before [measured: `test_bundle_usb`
  pulls the stick during an export's WRITE and during an import's VERIFY and STAGE].
- `bundle::eject(drive)` (the bundle UIs' EJECT): a job on that drive is cancelled first (not in COMMIT: "busy"), then
  `stor::eject`. The console's `stor=eject:USB` does not know the job: the job then fails at its next step as on a pull.
- The free space checked before an export to a stick is stor's (counted at mount, not updated by later writes); a
  short write later fails the job ("USB stick full or removed").
- **The end of a stick export** (2026-10-29: core 0 hung there on the Pi with a 111 MB and a 75 KB export alike, the
  watchdog reset it, the `.part` stayed): the last piece, the close (`E_CLOSE`: the size from the open handle, no second
  open) and the rename (`E_RENAME`) each run in a pass of their own. Before each file-system call of a stick job's end
  (and at its WRITE) a breadcrumb goes to the card, `/.rytm/bundle-mark.txt` ("export USB: <file>: f_rename of the
  .part (step N)"), removed when the job ends; `recover()` at the next boot logs one a reset left ("bundle: boot: the
  last run stopped in ...") and shows it as the bundle status's message (the web page's Project tab shows it while
  IDLE). The first breadcrumb comes before the stick's first call (the free names' f_stat, f_mkdir, f_open); a
  cleanup (a failure, a cancel) has its own, removed last; an import's backup (an export to the card) writes none.
  `File::close()` on the Pi is FatFs's `f_close() == FR_OK`: a close that did not keep the file fails the export
  [measured: `test_bundle_usb`, `host_fail_close`].
- The bundle lists (`bundle_rows`, `bundles_json`) read a folder's entries as its listing has them (`nextEntry`: one
  `f_readdir` each), not an open and an `f_stat` by path per entry (each walks the folder from its start: O(n^2) sector
  reads, seconds over USB in a full stick root) [measured: `test_bundle_usb`, 400 files].
- **Delete** in the web page's bundle lists (the card's and the stick's): `POST /bundle/remove path=...`
  (`bundle::remove_bundle`): `/Bundles/<name>`, `<drive>/<name>`, `<drive>/SIDECAR/<name>`, a `.sidecar.zip` or an
  interrupted export's `.sidecar.zip.part`; never the file a running job uses [measured: `test_bundle_usb`,
  `test_bundleweb`].
- exFAT stays **off** (`STOR_EXFAT=0`): tested on the Mac only (docs/STORAGE.md). A stick must be FAT32 / MBR.

### Tests

`make -C core test` (`test_project_key`, `test_bundle_zip`, `test_bundle`, `test_bundle_pools` - the pool lists:
refs() and the files' parser, members, the library's hashes, a stale record, an old bundle -, `test_bundle_stall` - with an export to a
stick and an import from it -, `test_bundle_usb`, `test_bundle_ui`: the feed and the Rytm page driven by its keys, its
own card time 0; `PROJECT_SHOTS=<dir>` writes its screens), `python3 -m unittest sim/tests/test_bundle.py` (Python's `zipfile` and `hashlib` read the core's bundles; `tools/bundle.py`), `make -C
pi/engine/tests` (`test_bundleweb`: every range boundary of the served archive, same-origin refusals, an import and a
delete through the endpoints; needs `make -C core engines lib`), `node --test pi/engine/tests/page_project.test.mjs`
(the tab's rendering, the stick picker, the service worker's ranges, pause and retry), `make -C pi/ui/host shots-all`
(`*-project*.png` in both themes and sizes; the taps checked against the commands they send), `sim/tests/
test_core_um.py` (phase 4), `RYTM_EMU=<rytm-emu> RUI_TAG=rui1042 python3 sim/rui_e2e.py project` (the Rytm page in
the emulator, on a `cp -c` clone of the tag's card).

### Memory

The job's buffers are taken at its first use and kept: about 90 MB (a 32 MB project-file snapshot, 8 MB of an import's
instruments, 4 MB each for texts and the dedupe index, the entry tables) [inferred from the sizes in
`bundle_int.h`]. The boot recovery takes 4 MB only when a staging folder exists.

### Not yet (phase 6) and differences from the design

- Phase 6: the hardware runs (below, "To test on the Pi").
- The project list's size is the project's **Sidecar files** only (the design: plus the samples it uses - that needs a
  COLLECT per project); the touchscreen's and the Rytm's lists show the same as the web page.
- The Rytm project file is taken from the card's `/Bundles/Rytm` only (the design: also a stick's `/SIDECAR/Rytm`).
- The Rytm page's header reads `PROJECT` and the destination (the page dots leave no room for `SIDECAR PROJECT 3/9`);
  T5 EJECT is an addition. INFO is one page (the files, size, last used, current / imported).
- The touchscreen shows up to four of a bundle's machines in the summary (the web page all of them).
- An interrupted **export** is not resumed (design 9): its `.part` stays and is listed as incomplete; export again.
- The export's snapshot of the project files is in RAM, not in `/Bundles/.work` (no card writes while the Rytm plays).
- **Replace** of the Rytm's current project is refused (switch the Rytm to another project first); replacing any other
  project works, with the automatic backup.
- An absolutely named `.sfz` sample (`sample=/...`) is placed on its own and its value rewritten if it moves; the
  samples an instrument names relatively move with it. A sample two instruments both name relatively moves with the
  first; if only the second is relocated, its path to that sample breaks (rare; to fix with the pool redesign).
- The dedupe index is the sample library (docs/LIBRARY.md, since 2026-10-22; `/.rytm/bundles/imported.txt` is no
  longer read or written): any file on the card the library has hashed can be reused, whoever wrote it, and an
  import's files go into the library with the sha256 STAGE read back (no second read). A file the library has not
  hashed yet (the first pass after a boot) is written again rather than reused.
- Relocated paths that would pass a path hint's 159 characters (`lib::kPath`) use `/Samples/Projects/<key, 8 characters>~<hash>`.
- Not built here (from the 1042 merge): SAVE PROJECT and project copy / rename following the Rytm. They take
  `project_key()` for both names and can copy a project's files with the export / import tables (the "Project files"
  table in `bundle.cpp`) without a zip.
- The LINK step's print check (0x25 against the manifest's `prints` when the Rytm names the imported project) is not
  built; the index's `i` flag is cleared when the Rytm names it.

### To test on the Pi (phase 6)

Pi 4B first, then Pi 5; the Rytm in Overbridge mode, 8 voices incl. DSP SAMPLE and a streaming MULTISAMPLE; a FAT32
(MBR) USB 3 stick in a blue port. For each, watch `/status` (`bundle`, `stor`, underruns, core0 loop, `pass max`):

1. Touchscreen PROJECT page: opens from the home page's PROJECT panel in OLED and CLASSIC (800x480 and 1024x600 if you
   have it); the list matches the web page; taps hit (rows, picker, keys); progress updates smoothly.
2. Export a ~1 GB project to the card, stopped: 0 underruns, 0 Overbridge gaps, cardq `taken_back` 0, `pass max` < 2 ms.
3. The same with a pattern playing: the job shows WAITING (PLAYING) in WRITE, goes on at STOP; no underruns.
4. Export to the USB stick (touchscreen, Rytm page with encoder H, web To USB): the file in `/SIDECAR`, opens on a Mac
   (`python3 tools/bundle.py verify`); the time it takes; `/status` core0 loop max while it writes.
5. Import from the stick onto another card (or after deleting the project): summary, machines, the done screen's
   "save your Rytm project as"; then load the Rytm project and check the sounds.
6. Pull the stick during an export and during an import's STAGE: the job says "USB stick removed", the card is
   unchanged (an import), the `.part` left on the stick; the stick mounts again when replugged.
7. EJECT from each UI with and without a job running; `stor` shows "ejected", replug mounts it.
8. The Rytm page: TRK + YES, LEFT: PROJECT; T1..T5 and their LEDs, encoder A / H, YES / NO, the asks; DELETE twice.
9. Exactness: a project whose slot map names an older machine version installed beside a newer one plays the older
   (`/um` JSON `play_ver`); a fresh slot takes the newest.
10. A Pi 4 bundle imported on a Pi 5 (and back).
11. Power pulled mid-import (boot recovery: the card as before or after).
