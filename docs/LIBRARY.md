# File formats and the sample library (Pi)

Two foundations under every later project change on the Sidecar | Expander (Pi 4B / Pi 5):

- **the format registry** (`core/fmt/`): one table of every file the Sidecar reads or writes on the card, one header
  convention, one load path that refuses what it cannot read safely and never writes over it, one atomic write;
- **the sample library** (`core/lib/library.*`): every sample and instrument on the card known by the sha256 of its
  bytes, the path a hint, kept current in the background without ever disturbing the audio.

Design: `docs/superpowers/specs/2026-10-22-formats-and-library-design.md` (phases **F1** and **L1** are built; F2-F3,
L2-L5 follow). Tags as there: **[measured]** = run here (host tests, the card model, the Pi kernel build),
**[inferred]** = reasoned, to be checked on hardware.

## For users

- **Status:** the web page's **Status** tab, *Storage*: `Library 48,213 files (18 GB), 61 % hashed, 18,803 pending,
  212 duplicates (386 MB) · waiting: playing`. The touchscreen's **SIDECAR STATUS** page: the *Library* row under
  the versions (`48213 FILES 61%  212 DUP` / `48213 files, all hashed, 7 missing`).
- **What it does now:** after a boot the Sidecar reads its index (`/.rytm/library/`), looks at every folder of the card
  (directory entries only) and hashes what is new or changed, one 64 KB piece at a time on the card core. While the
  Rytm plays it only reads, in 16 KB pieces; while the Overbridge recorder records it stops; a project bundle job goes
  first. The first pass over a big card takes a while (about 2-3 min a GB on the Pi's card [inferred: 12-14 MB/s]);
  afterwards only changes are hashed.
- **Duplicates** are counted (the web's Library tab with the groups comes in L3). **Missing** = files the index knew
  that are no longer on the card (kept for one compaction so a move can be recognised).
- **Recordings** (`/Recordings`) are indexed, not hashed (open question 6): a take is hashed when it is used.
- **`/NAM/`** (INSERT AMP, docs/INSERT-FX.md "AMP"): NAM A2-Lite captures, `.namb` or `.nam`, read by the AMP insert
  itself (listed when the Sidecar starts and on `amp=rescan`, sorted by name, at most 64; a capture loads through the
  card queue when a slot picks it). The library indexes and hashes the folder as any other; no pool list, no bundle
  role yet (a project that uses a capture names it by file in its INSERT file, `AMP V1 A <file>`). A cab IR folder
  `/NAM/IR/` is planned, not read yet.
- Bundles now dedupe through the library: an import reuses a file already on the card with the same content (read
  again before it is reused), wherever it is. `/.rytm/bundles/imported.txt` is no longer read or written (an old one
  can be deleted).

## Console (core 0)

| Line | |
|---|---|
| `lib=status` | `library: READY, 5 files (1 MB), 4 hashed (100%), 0 pending, 0 duplicates (0 MB), 0 missing, 0 unreadable, 1 recordings indexed; log 3 lines (0 waiting), 1 compactions; verify: 0 differ` (+ `waiting: PLAYING`, + the read-only reason) |
| `lib=rescan` | walk the card again (after files were copied onto the card on a computer) |
| `lib=verify` | hash every file again; differences are logged (`verify: /Samples/x.wav differs`) and counted |
| `lib=bench` | sha256 and CRC-32 MB/s from RAM, then the card's read MB/s over the biggest file (16 MB at most), in the log |
| `lib=find:<64 hex>` | the paths holding that content |
| `lib=hash:<path>` | hash this file next |

The machines' old `lib=gr:...`, `lib=wt:...`, `lib=ms:...` pool commands are gone with the pools build: the POOL lists
have `pool=smp|wt|inst:...` (docs/POOLS.md) and the browsers `browse=gr|wt|ms:...`; `lib=changed:PATH` is still taken
as the older name of `browse=changed:PATH`.

## The format registry (F1)

`core/fmt/fmt.h`. A format = name, class (P project / S setting / D derived / X exchange), shape, the **current**
version (the only one written), the oldest it reads, a sniffer (bytes -> version, min_reader), the current version's
reader, a chain of pure vN -> vN+1 upgraders (empty in F1), a `min_reader_of`, its path rule.

- `fmt::load(format, bytes, out, info)`: sniff; **newer** with `min_reader > current` -> `kNewer` (locked: no save may
  write the file); newer with `min_reader <= current` -> `kForeignNewer` (read, never written back); older -> upgraded
  in memory (`kUpgraded`, written at the next normal save); damaged, cut short, too big, not this format ->
  `kUnreadable` (locked). `fmt::refusal()` makes rule 2's line ("made by a newer Sidecar (format sqp v4); this Sidecar
  reads up to v3. Not loaded, not changed.").
- Header conventions (spec 3.2): text `#sidecar <name> <v> [min=<n>]`, key=value `format=<name>/<v>`, JSON
  `"format":"sidecar-<name>","version":N[,"min_reader":n]`, binary `magic[4] u16 version u16 min_reader u32 header_bytes
  u32 crc32(body)`; `.sqp` keeps its version byte as "the lowest reader". Helpers to read and write each.
- `fmt::write_atomic(path, data, len)` (core 0): `<path>.tmp`, read back and compared, `<path>` -> `.prev`, `.tmp` ->
  `<path>`, `.prev` removed; `fmt::recover(path)` at boot leaves the file old or new, never half (tested with a power
  cut after every card change). `fmt::AtomicWriter` does the same in 64 KB pieces (the library's index). From the
  control core: `cardq::kAtomicReq` (the card core runs `write_atomic`).
- `fmt::path(format, key, kit)`: today's places (generation 1), cross-checked in the tests against the bundle's
  project-file table.
- Class S keeps what it does not understand: `keys.txt`, `fxin.txt`, `sendfx.txt` and the Pi's `display.txt` carry
  unknown `key=value` lines (up to 1 KB) to their next write (`fmt::kv_keep` / `kv_append`), so 0.4 -> 0.3 -> 0.4
  keeps 0.4's settings.

Registered (28): `pool` `bankpool` `picks` `slots` `slotnotes` `fxslots` `juno` `fm` `lfo` `ins` `sqp` (v4, reads
1..4: seqx's blocks; 1..3 through the upgrade chain) `tracks` `song` | `display` `keys` `fxin` `sendfx` `wifi` `project` `projects` `sdtest` | `cardindex` (v4)
`dx7idx` | `rmpa` (format 4: 3 with an RLB2 block, RMPA only) `bundle` | `library` `library-log` `library-state`; CHOP adds `slicemap` (v2, the chain from the
sampler's v1 slice maps: docs/CHOP.md section 2); the pools build adds `samplepool`
`wtpool` `instpool` `poolused` (docs/POOLS.md; the old `pool`, `bankpool`, `picks` stay registered for sniffing and
bundles, never read into the lists). Every unversioned file is v1 as it is
today; F1 changes nothing on the card. Modules still load through their own readers (F3 moves them onto
`fmt::load`); the registry's readers apply the same limits (a pool line of 159 characters, `rmp_slots_parse`,
`seq::unpack`, `song::read_json`, ...), so its verdict is the one a later load through it gives.

## The sample library (L1)

### On the card: `/.rytm/library/`

| File | Format | Contents |
|---|---|---|
| `index.bin` | `SLIB` v1 (fmt binary header, CRC-32 of the body) | u32 records, u32 path bytes, 2 x u32 0; 64-byte records sorted by sha256 (unhashed after): sha256 32, size u64, crc32, quick id u32 (the first 4 bytes of the sha256 of the first 64 KB), FAT date << 16 + time u32, flags u16, kind u8, channels u8, rate u32, path offset u32; the path table |
| `log.txt` | `#sidecar library-log 1` | the changes since `index.bin`: `+ <sha|-> <size> <fatdt> <crc> <quick> <flags> <path>`, `- <path>`, `> <old>\t<new>` (a move keeps its hash) |
| `state.txt` | `format=library-state/1` | `tree=` the walk's fingerprint, `files=`, `bytes=`, `hashed=`, `unhashed=`, `gone=`, `dups=`, `recordings=` |

Flags: UNHASHED 1, GONE 2, UNREADABLE 4, DUP 8, NOHASH 16 (a recording). The log is compacted into a new `index.bin`
(atomic, in pieces) at 2 000 lines and once after the boot's walk and hashing. A damaged `index.bin` is rebuilt (every
file hashed again); one from a newer Sidecar (version above 1) is never written over: the library then runs in RAM
only and says so (`lib=status`, `/status` `readOnly`).

Scope: the whole card minus `srclib::kHidden` (Plugins, PRESETS, JUNO, DX7, Bundles, ...), dot names and `/.rytm`;
types `.wav`, `.aif`, `.aiff`, `.sfz`; a removable volume (`USB:/...`) is not indexed.

### Keeping it current

- **Boot:** the index is read (64 KB a step) and published, then the card is walked a few directory entries a step
  (`File::nextEntry`: one `f_readdir` on the Pi, no file opened): new or changed (size, FAT time) -> UNHASHED, not seen
  -> GONE, and the fingerprint (names, sizes, FAT date + time, folder marks) computed on the way.
- **The Pi's own writes** report their finished files with the hash made while writing (no second read): web uploads
  (fsweb: the chunks' sha256), the sampler's and the auto-sampler's WAVs and `.sfz` files (`samp::WavWriter`,
  `samp::write_text`), a bundle import's files (the sha256 STAGE read back). Deletes and renames (web, import, the
  recorder's take delete) go gone / move with their hashes. The recorder's takes and the sdfiles server's files are
  reported without a hash. Needed because the Pi has no clock: Circle's `get_fattime` returns 0 until the time is set
  [measured: addon/fatfs/ffsystem.cpp], so (size, FAT time) cannot tell its own rewrites apart.
- **Hashing order:** `lib::hash_now` (a pick in a browser, any core: a lock-free ring), then the current project's
  references (the pools build: every Ref of the project's three POOL lists, members included, from `pools::refs()`),
  then new files of a gone file's size (a move's
  candidates), then the rest in walk order.

### Cores and the audio

Everything that touches the card is core 0's (`lib::step` in the kernel's loop after the bundle job's step; never
during a bundle job, never while the card queue has work or a stream is low). Core 3 (control + render) reads only the
published snapshot: `resolve`, `ref_for_path`, `find`, `gen`, `stats` - no lock, no card. Two snapshots: core 0 builds
the spare one and swaps the pointer; a reader marks the one it reads and core 0 never rebuilds a snapshot a reader
holds (it tries again a step later). A publish copies and sorts the whole index in one core-0 step: it happens at
once for a pick, a writer's file, the current project's references done and when the work is done (on an index over
20 000 files at most 4 a second), else at most once a second, and on a big index once every n / 20 000 seconds.

### The interfaces (spec 9; the pools design builds on these)

```cpp
namespace core::lib {
struct Ref { uint8_t sha[32]; bool has_sha; uint64_t size; char hint[kPath]; };   // kPath 160
bool parse_ref(const char* line, Ref* out);      // "<64 hex>|- <size> <path hint>"
int  format_ref(const Ref&, char* out, int cap);
enum State : uint8_t { kPending, kExact, kMoved, kUnverified, kChanged, kMissing };
struct Resolved { State st; char path[kPath]; uint32_t rec; };   // path: a copy
State resolve(const Ref&, Resolved* out);         // RAM only, lock-free, safe on core 3
bool  ref_for_path(const char* path, Ref* out);   // a pick (an unhashed file is bumped in the queue)
uint32_t gen();                                   // poll, then resolve again
void touched(const char* path, const uint8_t* sha_or_null, uint32_t crc_or_0);   // core 0
void moved(const char* from, const char* to);     // core 0
void hash_now(const char* path);                  // any core
int  find(const uint8_t* sha, const char** paths, int max);   // core 0 only (pointers into its snapshot)
bool find_file(const uint8_t* sha, uint64_t size, char* path, int cap, const char* near = nullptr);
Stats stats();                                    // files, unhashed, dups, bytes, state, pct, hashed, gone, ...
}
```

Resolution (spec 6.5), against the RAM index: **EXACT** the hint has that sha; **MOVED** the sha elsewhere (the copy in
the hint's folder, else the path sharing most leading folders, else the first by name); **UNVERIFIED** the sha not in
the index while the hint is there unhashed with that size (played); **CHANGED** the hint there with other content
(played and flagged, open question 4); **MISSING** only when the walk is done and nothing is left to hash; **PENDING**
while the index is not loaded, its walk not done or files still wait for their hash, and the hint not found (played by
path). A ref without a sha resolves by its path (EXACT hashed, UNVERIFIED not yet).

## Cost

| Figure | Value | Tag |
|---|---|---|
| sha256, Mac (M-series, one core, `-O2`) | ~300 MB/s | [measured: test_library `lib=bench`] |
| CRC-32 slice-by-8, Mac | ~2 GB/s | [measured] |
| sha256 / CRC-32, Pi 4 (A72, no SHA extensions) / Pi 5 | 75-120 / 400+ MB/s; Pi 5 with SHA-2 instructions more | [inferred; `lib=bench` on hardware: L5] |
| first hash under the card model (an open 1 ms, reads 20 MB/s + 0.1 ms a call) | 38.6 MB in 2.8 s of card-core time = 13.9 MB/s: the card is the limit | [measured: test_library_stall] |
| hashing while the Rytm plays | 16 KB a step, one step every 2 ms at most (8 MB/s ceiling); 0 card writes | [measured: test_library_stall] |
| control core | 0 us card time, longest pass 0.15 ms, 0 passes over 2 ms, while 40 MB were hashed | [measured: card model] |
| card core, one step | at most 64 KB read; its longest card time 3.5 ms reading, 26 ms for a compaction's close + renames (writes only while stopped) | [measured: card model] |
| RAM | 64 B + the path a file, twice (two snapshots) + the working index: ~35 MB for 100 000 files | [inferred from the layout] |
| 100 000 records | log replay + publish 53 ms (Mac); `find` 0.03 us, `resolve` 0.12 us; a reader thread resolving 1.9 M times meanwhile, all consistent | [measured: test_library] |
| a publish (sort + copy) | O(n log n) in one core-0 step: ~50 ms on the Mac for 100 000 files, ~1.5 s on a Pi 4 [inferred x30] - over the card queue's 200 ms take-back, so the streams are served by core 3 meanwhile; rate-limited by size (above); L5 to measure, an incremental publish if it shows | [inferred] |

## Tests

- `make -C core test`: `test_fmt` (inventory, paths vs the bundle table, 32 golden files in `core/tests/golden`
  (`make_golden.py`), the `.sqp` writer's round trip v1 -> v2, refusals, an upgrade chain, headers, unknown keys kept
  by keys / fxin / sendfx, `write_atomic` with a power cut after every change, `AtomicWriter`, `cardq::kAtomicReq`, a
  112 000-load in-process fuzz: no crash, no card write), `test_library` (sha256 NIST + Python hashlib, CRC-32, the ref
  text form, boot -> walk -> hash, a reboot with no change reads and writes nothing, the diff walk's add / delete /
  change in place / rename / move / duplicate, every resolution row, touched from the sampler's writer with no read,
  moved, the log's replay, a damaged and a newer index, hash_now and the current project's references first, the
  pause rules, the console, `lib=bench`, `lib=verify`, 100 000 records with a reader thread), `test_library_stall`
  (the card model: no control-core card time, no pass over 2 ms, held back while a stream is low, 16 KB reads and no
  writes while playing, nothing while recording), `test_bundle` (the library as the import's dedupe; an import's
  files known with no read).
- `make -C core fuzz-fmt` (libFuzzer + ASan + UBSan with Homebrew's clang; else a standalone ASan driver): 1.8 M
  inputs x 28 formats in 60-90 s [measured]. It found one bug, fixed: the song JSON reader converted a number past an
  int64 from a double (undefined behaviour); such a take is now refused (a regression case in test_fmt).
- `make -C pi/engine/tests` (`test_fsweb`: an upload's sha256 from its chunks, resume included; delete and rename
  reported), `node --test pi/engine/tests/page_lib.test.mjs` (the Status tab's line), host shots of the STATUS page in
  both themes and sizes (`make -C pi/ui/host shots-all`).

## Differences from the design (L1 / F1)

- **The boot check and the diff walk are one pass:** the library walks the card in pieces and computes the fingerprint
  on the way (`state.txt` `tree=`), instead of `Fs::tree_hash` first and a diff walk second. `Fs::tree_hash` is one
  blocking call (seconds on a big card: core 0's loop, and with it the card queue, would stop), and the walk reads the
  same directory entries; a fingerprint equal to the last one only skips the state's write.
- **Records:** 64 bytes without `frames` (derivable from size, channels, rate; the spec's list adds up to 68). The quick
  id is 4 bytes (the record's u32) of the sha256 over the first 64 KB.
- **Log moves** use a TAB between the two paths (`> <old>\t<new>`): paths may hold spaces.
- **`Resolved::path` is a copy** (not a pointer valid until the next `gen()`): the caller can keep it.
- **`find_file(..., near)`** ranks the copies as `resolve` does; the bundle import passes the entry's own path, so a
  duplicate in the same folder is preferred.
- **Robustness beyond the design (review, 2026-10-22):** a writer's report while the index loads is kept and applied
  after the load; files written or renamed during a walk are not taken as gone at its end, and a folder moved under a
  walk walks again; a log cut mid-line gets a newline before the next append; a log that cannot be read, or a newer
  build's log, makes the library read-only for that boot (never written over); a card write that fails rests the
  writes for a minute and compacts later (no line lost from the card, no busy loop).
- **Paths of 160 characters or more** are not indexed (`kPath`, as pool lines): a bundle import does not dedupe such
  files (the old `imported.txt` did up to 200).
- **Peaks** (`peaks.bin`) are L3 in the spec's table: not built. **SFZ group identity** is L4: not built.
- **The pause rules** are the bundle job's (`bundle_pi::io_rule`): song recording counts as "a take records"; the
  sampler's own WAV writes run on the same core between library steps.
- **`sdtest.txt`** is a measurement rewritten whole by each test: its unknown keys are not carried (the other class S
  files are).
- **`/.rytm/bundles/imported.txt`** (bundles) is replaced now, not in L4: the import asks the library (`find_file`)
  and still reads every candidate again before it reuses it.

## For the pools build (done 2026-10-22)

Built on this library (docs/POOLS.md):

- The formats `samplepool`, `wtpool`, `instpool` (class P) and `poolused` (class D) are registered in
  `core/fmt/formats.cpp` (a `#sidecar <name> 1` header required, readers through `core/lib/pools_fmt.cpp`), placed by
  `fmt::path` at generation 2 (`/.rytm/projects/<K>/...`), with golden files in `core/tests/golden/<name>/v1/` that the
  fuzz target reads. The lists are read with `fmt::load` and written with `cardq::kAtomicReq` from the control core
  (the card core does the I/O).
- Every list entry and instrument member is a `lib::Ref`, made by `ref_for_path` (an unhashed file bumped in the queue;
  `-` in the file until the library has the hash, then written at the next save) and resolved by `resolve` whenever
  `gen()` moves. The states show on every POOL surface (`!name` MISSING, `~name` CHANGED with ACCEPT).
- `Env::current_refs` (lib_pi.cpp) hashes the current project first from `pools::refs()`.
- `lib::hashes_of(path, size, sha, crc)` gives the bundle export a file's sha256 and CRC-32 from the index (no HASH
  pass for an indexed file; docs/BUNDLES.md).
- Still not done (spec L3-L5): peaks, SFZ group identity (an instrument is known by its `.sfz`'s own hash), the web
  Library tab, the Pi runs (`lib=bench` figures into this file and `docs/pi-engine.md`).
