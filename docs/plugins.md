# Machine plugins (user machines)

A machine plugin is a sound generator that runs on the Teensy in one of the five user slots U1..U5 (voice flags
0x50..0x70, `sim/protocol.py`). It is compiled for the Cortex-M7, packaged as a `.rmp` file, stored in the Teensy's
program flash and copied into an executable RAM arena when a project that names it is loaded. This document is the
developer reference for sub-project 1 (loader, store, slots, guard, Noise Plethora packages) and the Teensy side of
sub-project 2 (user slots on the Rytm, plugin API 2, presets); the designs and the Rytm side are in
`docs/superpowers/specs/2026-10-10-user-machines-design.md` and `docs/superpowers/specs/2026-10-10-user-slots-rytm-design.md`.

Contents: 1 What a machine is, 2 Building one, 3 Packaging, 4 Installing for development, 5 The slot map file,
6 How voiceloop runs machines, 7 The guard, 8 Testing, 9 Measured numbers, 10 Noise Plethora packages and licences,
11 User slots on the Rytm, 12 Plugin API 2, 13 Presets, 14 Status.

## 1. What a machine is

The ABI is `sdk/rmp/rmp_api.h` (RMP_API 2, `RMP_API_MIN` 1; the Teensy library has the same header in
`teensy/libraries/RytmPlugins/src/`). A plugin exports exactly one `rmp_plugin` table named `rmp_entry`
(`RMP_ENTRY`); the host passes it an `rmp_host`. Plain C, fixed-size types, no host symbols.

**API 2** (`RMP_API 2`, 2026-10-10): every addition sits in a reserved slot, sizes unchanged (`RMP_PLUGIN_BYTES` 72).
API-1 plugins still load and play: the package header's `api` is the entry table's own, and the host gives an API-1
plugin an API-1 host table (`api` 1, no v2 functions). Section 12.

- `rmp_host`: `api`, `sample_rate` (48000), `max_block` (the largest `n` that `render` gets; 12 today), `log(msg)` (a line in
  the Teensy's log; not from `render`), `random(&state)` (xorshift32 on the caller's state), and from API 2
  `pool_info`/`pool_read` in `reserved[0..1]` (section 12; read them with `rmp_pool_info_of`/`rmp_pool_read_of`, NULL
  for an API-1 table), 9 more reserved.
- `rmp_plugin`: `magic` (`RMP_PLUGIN_MAGIC`), `api`, `state_bytes` (per voice, 8-byte aligned by the host, zeroed before
  `voice_init`), `cpu_permille` (declared CPU per voice in 1/1000 of the Teensy: the guard's budget), then
  `init(host)` (once after loading; nonzero = refuse, the plugin is unloaded), `voice_init(state)` (a voice starts
  using the machine), `note_on(state, note, velocity)` (note in semitones, 60 = C4; velocity 0..1), `note_off(state)`
  (the gate closed, Rytm 0x11), `params(state, p)` (P1..P16 as floats, each 0..127 plus the 7-bit fraction: value +
  fraction/128; P1..P8 are the SRC page, P9..P16 the SMPL page), `render(state, out, n)` (n = 1..max_block samples, mono,
  written, not added, nominal level +-1) and 8 reserved pointers: API 2's `load_entry`, `voice_enter`, `voice_leave`
  in `reserved[1..3]` (`RMP_FN(f)` puts a function there; NULL turns the feature off; the host reads them only from
  an `api` 2 table), `parse` (`[0]`) and `[4..7]` later.
- The entry table sits at the image's `entry_offset` (4-aligned; the table's 72 bytes must lie inside the image).
- State is per voice: all 8 voices may play the same slot, each with its own `state_bytes` of state. Keep everything
  mutable in the state: the image's `.data`/`.bss` exist (bss is zeroed at load) but are shared by all voices of the slot
  and by the slots naming the same package.

## 2. Building a machine

### 2.1 The Makefile pattern

```make
# plugins/sine_test/Makefile
NAME := sine_test
SRCS := sine.c
include ../../sdk/rmp/plugin.mk
```

`sdk/rmp/plugin.mk` takes `NAME`, `SRCS` (C and C++), optionally `CFLAGS_EXTRA`, `INCS`, `MANIFEST` (default
`manifest.json`) and `BUILD` (the object directory, default `build`; one per package when a Makefile builds several).
Targets: `$(NAME).rmp` (the Teensy package) and `lib$(NAME).dylib` (the same source built natively for the Mac, with
`-DRMP_NATIVE`, for the test harness); a plain `make` is `make all` (package and Mac library); `make clean`. The ARM toolchain is Teensyduino's
`arm-none-eabi-gcc` (`ARMBIN`, default the newest `~/Library/Arduino15/packages/teensy/tools/teensy-compile/*/arm/bin`, chosen with `sort -V`); the
packer runs from the repo's `.venv` (`PY`).

What the build does: `-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -O2`, `-ffunction-sections
-fdata-sections`, **`-mword-relocations`** (literal pools hold absolute addresses, which the relocations then fix up), no
exceptions/RTTI/unwind tables, then a link with `sdk/rmp/plugin.ld` into ONE section `.image` at address 0 (entry table
first, code, rodata, data, bss as NOBITS at the end) with `--emit-relocs --gc-sections -nostdlib -nostartfiles`.
`rmp_pack.py` turns the relocations into the package's relocation table, so the loader can place the image at any address.
C is compiled as C with `-ffreestanding`; C++ is compiled without it (libstdc++'s `<math.h>`/`<cmath>` refuse
`-ffreestanding`) and with `-fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-builtin` (so `mem*` is not
recognised and turned into a call to itself; C uses `-fno-tree-loop-distribute-patterns` for the same reason). Header
dependencies are tracked (`-MMD -MP`).

### 2.2 Rules

- **No static constructors or destructors.** A non-empty `.init_array` is refused by the packer. Construct objects in
  `voice_init` (or `init`), never at namespace scope; no function-local statics with dynamic initialisation.
- **No host symbols.** An undefined symbol is refused by the packer. Everything comes from the plugin itself or the SDK
  shim (below); the host gives you `rmp_host` and nothing else (no `malloc`, no `printf`).
- **`-mword-relocations`** is mandatory (plugin.mk sets it). The packer keeps only `R_ARM_ABS32`,
  `R_ARM_THM_MOVW_ABS_NC` and `R_ARM_THM_MOVT_ABS` as relocations (PC-relative ones stay valid when the whole image moves)
  and refuses any other type or any other allocated output section.
- **C++ entry linkage:** declare the table with `RMP_ENTRY` (`extern "C"` under C++, so `rmp_entry` is a global unmangled
  symbol); the native build keys on `RMP_NATIVE`.
- **`state_bytes <= 16 KB`** (16384) to run without PSRAM: the voice's state lives in that voice's own 16 KB engine
  scratch (section 6). Over that, the slot is BAD unless a PSRAM state pool serves it.
- **Declare `cpu_permille` from the PEAK per-render-call cycles**, not the average: the guard limit for one render is
  `2 x cpu_permille x 150 + 2000` cycles (a 12-sample block is 150000 cycles at 600 MHz), so
  `cpu_permille >= peak / 150 / 2` with margin. The host refuses (BAD) more than 500.
- Never write outside your state and your image; the guard catches faults and overruns, not stray writes (section 7).
- Size: the packer allows an image up to 1 MB, but a package must fit the store's 64 KB cap and, with the other slots'
  images, the 32 KB arena.

### 2.3 The SDK shim

`sdk/rmp/shim/rmp_libc.c` (always linked) supplies `memcpy`/`memset`/`memmove`/`memcmp` and `__errno`;
`sdk/rmp/shim/rmp_cxx.cpp` the C++ stubs (`operator delete`, ...), always linked and dropped by `--gc-sections` when unused.
The link adds `-lm -lgcc`: float math (`sinf`, ...) comes from newlib's libm. Watch what a libm call drags in: with
plugin.mk's `-fno-builtin`, `pow(x, 2)` linked libm's double `pow` with its tables (image 14616 B instead of ~6.4 KB), which
is why the Noise Plethora Atari package turns C++ builtins back on (`ARMCXX += -fbuiltin`).

### 2.4 The Teensy Audio shim (for Audio-library based machines)

`sdk/rmp/shim/audio/` replaces the core's `AudioStream` for a plugin voice: the update list and the block pool belong to an
`AudioGraph` inside the voice state (blocks of 16 to 128 samples at 48 kHz, per package); the Audio objects' `.cpp` files
(MIT, PJRC) are compiled unchanged from the installed Teensyduino (the newest, picked with `sort -V`). Semantics kept from
the core: updates in construction order, active objects only; transmit/receive/release reference counting;
`receiveWritable` copies shared blocks; `allocate()` returns NULL when the pool is empty. `random()` runs on the graph's
own xorshift state, seeded from the order of `voice_init` calls (reproducible, one sequence per voice). Natively the
`Arduino.h` shim defines `__ARM_ARCH_7EM__` and supplies portable `dspinst` functions (rounded MAC in uint64), so the Mac
build runs the same Teensy 4 code paths. `Audio.h` offers the generators and processing the Noise Plethora plugins use
(waveform, sine, whitenoise, pinknoise, pwm, dc, mixer, filter_variable, multiply, wavefolder); effect_combine,
effect_bitcrusher and amplifier were dropped (nothing uses them), as were freeverb, granular, flange and delay. The Audio
objects rely on zeroed storage (some members are left to zero-initialisation, as globals on a Teensy): the host zeroes a
voice's state, and a package that constructs an object again (an algorithm swap) zeroes the buffer first. PJRC's MIT
notices are in `sdk/rmp/shim/audio/NOTICE`. `AudioSynthNoiseWhite` seeds from a plugin-global instance counter, so a noise
voice's sequence depends on how many noise objects were constructed before it (no live state is shared).

**Block size.** The host calls `render()` with 12 samples. The block size is a per-package Makefile variable,
`NP_BLOCK` (a multiple of 16 from 16 to 128; `AudioStream.h` refuses others; changing it rebuilds the package). 12 itself
is impossible: `synth_dc` and `synth_pinknoise` fill 16 samples per loop iteration. At 16 a render call runs at most one
graph pass (3 calls in 4 run one); 48-sample blocks made every 4th call a whole pass (~4x the average call) while the guard
limits each call. Natively, 16 of the 21 Noise Plethora plugins produce bit-identical output at 16 and at 48. The other
5 have a feedback loop or an object updated before its source, so the graph contains a one-block delay and the sound
depends on the block size; spectra compared with 128-sample blocks (NP's own size), as similarity 0..1 of smoothed
magnitude spectra:

| plugin | 128 vs 48 | 128 vs 16 | 128 vs 12 |
|---|---|---|---|
| radioOhNo | 0.969 | 0.944 | (uses dc) |
| xModRingSqr | 0.758 | 0.818 | 0.812 |
| XModRingSine | 0.976 | 0.242 | 0.152 |
| CrossModRing | 0.997 | 0.997 | 0.998 |
| Atari | 0.757 | 0.599 | 0.539 |

XModRingSine's zero-crossing rate is 3040/s at 16 against 3775/s at 128. Fidelity wins: those five plugins (radioOhNo,
xModRingSqr, XModRingSine, CrossModRing, Atari) build at 128, the others at 16. Atari at 128 equals the 128-sample
reference bit for bit through the harness (max difference 0, similarity 1.000). At 128 with 12-sample render calls a pass
runs about every 10.7th call, so that call is the peak. Native cost per sample, 16-sample against 48-sample blocks:
1.0-1.9x (Atari 2.0 vs 1.6 ns, clusterSaw 16.4 vs 10.6, WalkingFilomena 14.9 vs 7.8, phasingCluster 35.1 vs 26.1).

## 3. Packaging

`tools/rmp_pack.py ELF MANIFEST.json OUT.rmp` (plugin.mk runs it for `make NAME.rmp`; errors are `PackError`, exit
non-zero). The format is `teensy/libraries/RytmPlugins/src/rmp_format.h`; `tools/tests/test_rmp_pack.py` compiles that
header and compares the layout.

- **Format 3** (`RMP_FORMAT`; the loader accepts `RMP_FORMAT_MIN` 1 to 3: they differ only in the manifest,
  so installed format-1 and format-2 packages stay valid and read with format 3's defaults). A 96-byte header
  (magic "RMP1", format, api - the entry table's own, 1 or 2 -, id, version, compatible_from, image_size,
  bss_size, entry_offset, reloc_count, manifest_size, flags, CRC-32 of the body after the header, 20 reserved
  bytes), the image, the relocations (8 bytes each: offset | type << 28, then the value; ABS32, MOVW, MOVT
  with full values), the manifest: an 84-byte head (name 10, short 4, licence 48, page2, knob count, page-2
  title 13, native preset extension 7) and 26-byte knobs (p, look, text, enum_first, enum_count, role, min,
  max, default, reserved, long 12, short 4), then the enum name table. Formats 1/2: a 64-byte head, 20/22-byte
  knobs.
- **Manifest fields** (`manifest.json`; unknown keys are refused): `id` (the store's file-name rule: at most 31 printable
  ASCII characters, no space, `/`, `\`, leading `.` or `..`), `version` and `compatible_from` (`a.b.c`, each 0..255;
  `compatible_from` defaults to `version` and may not be newer), `name` (at most 10 characters), `short` (at most 4),
  `licence` (at most 48), `page2` (`"sample"` or `"params"`: whether the SMPL page is a sample layer or 8 more knobs) and
  `knobs`, a list of at most 32: `p` (1..16, each once), `long` (at most 12), `short` (at most 4), `look` (`dial` `centred`
  `fader` `list`), `text` (`number` `bipolar` `enum` `list` `note` `time`) and, for look `list`, **`names`**: 1..16 value
  names of 1..7 printable ASCII characters each (at most 255 names per package). A knob is 26 bytes in the manifest (22
  in format 2; `enum_first`/`enum_count` index a name table of NUL-padded 8-byte entries after the knobs); the format-1
  knob (20 bytes) has no names.
  Format 3 adds per knob `role` (`free` default, `preset`: a preset sets it and it is written back to the
  Rytm, `offset`: stays where it is, applied on top), `min`, `max`, `default` (0..127; defaults 0, 127 and 64
  clamped into the range), text kinds `preset` (the knob that picks the preset; at most one, role `free`) and
  `pool` (a pool slot, 0 OFF); per package `page2_title` (at most 13; "" shows the machine's name) and
  `preset_ext` (the native presets' file extension: letters and digits, not `txt`).
  seqx (E0, 2026-10-22): **`max_voices`** (1..8, default 1) and **`pitch`** (`note` default, `none` a fixed-pitch drum:
  the host sends note 60). They go into the header's `flags` (`rmp_format.h` RMP_HF_*: bit 0 RMP_HF_INSTANCES when
  `max_voices` > 1, bits 4..6 `max_voices` - 1, bits 8..9 the pitch), outside the CRC; an older host ignores them and
  plays the machine mono. `max_voices` > 1 is the opt-in to **instancing**: on a Sidecar poly track the host runs up to
  that many states of the plugin on one voice (the track's notes, a unison stack; docs/SEQUENCER.md "Built: seqx"), so
  the plugin must keep everything render() writes in its state (constants and the host table in globals are fine).
  Packages without the keys (every older one) stay mono.
- **`type_labels`** (2026-10-26, `plugins/mdkit`: one machine whose TYPE knob picks a voice with its own controls):
  labels that follow a list knob. `{"knob": 9, "params": [1, ..., 8], "sets": [[["Pitch", "PTCH"], ..., null], ...]}`:
  `knob` a list knob with names, `params` 1..8 of the other knobs, `sets` one per name of `knob`, each holding one
  `[long, short]` (12 / 4 characters) or `null` (unlabelled, inert for that value) per entry of `params`. The packer
  appends it to the manifest as a block after the name table (`rmp_format.h` RMP_LABELS_*: knob, nsets, nparams, 0,
  the Ps, nsets x nparams 16-byte entries, then a 6-byte footer: the block's length and the magic `RLB1`). The package
  **stays format 3**: an older host (the Teensy, a Pi engine before this) never reads past the name table, takes the
  knobs' own static labels (give them a sensible family-wide set) and plays the package as before. A card host
  (`core/user/um.cpp`, the Pi and the Mac core lib) reads the block at the scan (`rmp_manifest_labels`) and names a slot's
  knobs after the value its list knob has, zone `(value * nsets) >> 7` (`rmp_labels_set`, the way a plugin reads a list
  knob): the value of the voice the Rytm has selected when it plays the slot, else of the voice whose knobs the user
  moved last on the slot (a Rytm report without a trig; never the Sidecar's LFOs or step locks, which call the plugin
  with `core::user_report_internal` set), else of the first voice on it. The set shown moves at most every 150 ms (a
  1D each time). A change is a new description, so the slot's
  generation moves, a 1D goes out and the Rytm asks 1A again: its user-machine page and the touchscreen (which draws
  the Rytm's labels) follow the value, within ~100 ms (meta_poll). The slot JSON adds `lset` (the set shown, -1 the
  static labels). One slot has one set of labels: two tracks on the same slot with different TYPEs show the selected
  (or last edited) track's. Tests: `tools/tests/test_rmp_pack.py` Labels, `extras/manifest_test.cpp`,
  `sim/tests/test_core_labels.py`.
- **`type_labels` with `follows` and `lists`** (2026-10-27, `plugins/mdkit`'s MD: ENGINE picks the TYPE list): a list
  knob whose names follow another list knob. `{"knob": 10, "follows": 9, "lists": [["BD", ...], ...], "params": [...],
  "sets": [...]}`: `follows` another list knob with names, `lists` one list of names (1..16, 7 characters each) per name
  of `follows`, `lists[0]` equal to `knob`'s own static names (what an older host shows), `sets` one per name of all the
  lists together in order (32 at most). The packer writes an RLB2 block instead of RLB1 (`rmp_format.h`
  RMP_LABELS_MAGIC2: knob, nsets, nparams, dep, ndep, the lists' counts, the Ps, the names, the label entries, the same
  footer with `RLB2`). The card host reads it at the scan, picks the set from both values (`rmp_labels_pick`: `follows`'
  zone v among its names, then `base[v]` + `knob`'s zone among `count[v]`), gives `knob` the enum count `count[v]` in the
  slot's 1A (`rmp_labels_apply`: the zones the Rytm shows) and answers its 1B with `lists[v]` (`rmp_labels_enum`; with no
  voice on the slot, the static names). Such a package is **format 4** (RMPA only; review 2026-10-27): format 3's
  manifest with the RLB2 block, so a host from before it (format 3 at most: the Pi engine before b6cfec3, the Teensy)
  refuses it ("format not supported") instead of showing the first list's names for every value of `follows`. Every
  other package stays format 3, byte for byte; an RLB1 package reads as before. The set shown is part of the slot's
  print (two lists of one count with the same labels still move the generation), and it is dropped when a rescan
  gives the slot another package. A plugin reads
  `knob` in `follows`' zones too: `(value * count[v]) >> 7`. Tests: `test_rmp_pack.py` Labels.test_type_lists,
  `manifest_test.cpp`, `test_core_labels.py` test_list_follows_another.
- The packer refuses: static constructors, host (undefined) symbols, other relocation types, other loadable sections,
  PC-relative targets outside `.image`, a malformed or truncated ELF, an image over 1 MB, an unsafe id.
- Example: `plugins/sine_test/manifest.json` (three knobs).

## 4. Installing for development

`tools/rmp_install.py` talks to voiceloop over the serial link (the bridge, `sim/teensy_bridge.py`, must not be running:
one program owns the port; without `--port` it takes the first `/dev/cu.usbmodem*`):

```
tools/rmp_install.py [--port PORT] install FILE.rmp       store it in the Teensy's flash
tools/rmp_install.py [--port PORT] assign SLOT ID VERSION  U<SLOT> (1..5) of the current project plays ID (a.b.c)
tools/rmp_install.py [--port PORT] clear SLOT              U<SLOT> plays nothing
tools/rmp_install.py [--port PORT] info                    the device-info report ('D'), printed as JSON
tools/rmp_install.py [--port PORT] export ID [DIR]         a package from the store to DIR/<id>-<a.b.c>.rmp and to the
                                                           card's /MACHINES/<id>-<a.b.c>.rmp (a backup; a 1E install
                                                           copies it back); only with the transport stopped
tools/rmp_install.py [--port PORT] wipe --yes              format the store (every package lost: export them first);
                                                           the only way past a "foreign store layout" at boot
```

Frames to the Teensy: `'I'` total u32 LE (begin), `'J'` n u16 LE + n bytes (a chunk, n <= 256), `'K'` (commit), `'U'` slot
(1..5) len id a b c (assign; len 0 clears), `'D'` (device-info report), `'W'` mode len path n u16 LE + n bytes (a file
on the card under `/PRESETS/` or `/MACHINES/`: mode 0 create, 1 append, 2 delete, 3 a preset scan fault for the tests;
n <= 200; `rmp_install.py put`), `'T'` 0|1 (the transport stopped / running: `sim/teensy_bridge.py` relays MIDI
Stop / Start, Continue), `'Z'` len id (remove a package; `rmp_install.py remove`), `'E'` len id (export: the package
as log lines `export: <id> <offset> <hex>`, 64 bytes each, then `export: <id> done <size> <crc32> <a.b.c> card
<ok|failed|none|skipped|kept>` - none: no card, skipped: the package's header does not verify, kept: a `<path>.tmp`
from an earlier export is on the card and the card is left alone (deal with it by hand: it may be the only complete
copy); the tool checks the size and CRC-32 and exits 0 for `ok` or `none`; the card copy is written as `<path>.tmp`
and renamed over the old one only when complete - a failed write removes the `.tmp`, a failed remove of the old copy
or a failed rename keeps it, logged with the step that failed; one export holds the Teensy's loop for the whole
package, ~1000 log lines for 64 KB, so export only with the transport stopped), `'F'` `WIPE` (format the store). With the USB-host build add `--host` (`tools/rmp_install.py --host info`): the tool sends `'P'` and skips the plain-text log up to the framed `pong`, after which the Teensy frames its log until a reboot (`sim/rmp_e2e.py --host` likewise; its sine checks need no Rytm: without Overbridge streaming `'N'` renders the voices). The Teensy answers with log lines; the tool prints the `plugins:`/`plugin:`
ones and exits 0 on success ("plugins: installed <id>", "plugins: U<n> <id> ready", "plugins: U<n> cleared"), 1 on a
failure line, a timeout, or when the Teensy is in safe mode ("plugins: safe mode - not loaded"). The serial reader drops
a frame that stops for 100 ms (aborting an open install) and skips an oversized `'J'` payload unparsed (giving up on it after 100 ms without progress).

**The device-info report (`'D'`, `rmp_install.py info`).** One JSON document, sent as log lines `info: <i>/<n> <chunk>` (140
characters a line; `Link.info()` in `tools/rmp_install.py` joins them). It is built over loop passes between requests, so
it cannot stall audio (measured: 18 passes, 37 ms in all, the longest pass 3.7 ms: a flash file open). Fields: `fw` (core
version), `api` (2), `formats` ([1,3]: the oldest and newest the loader takes), `psram_mb`, `state_limit` (16384, or the PSRAM pool when fitted) and `state_psram`,
`itcm_blocks`, `arena` (size, used), `project`, `slots` (for each of U1..U5: id, version, state READY/MISSING/BAD/
NO_ROOM/MUTED/LOADING/EMPTY, declared `permille`, measured `avg_cyc`/`peak_cyc` per 12-sample block since the load, `calls`,
and `avg_permille`/`peak_permille` = cycles / 150), `store` (size, free, packages with id, version, size; 12 at most,
`more`), `sd` (a card is present), `machines` (the ids found in the card's `/MACHINES`, 12 at most, `sd_more`; empty when
the folder does not exist) and `build_us`, `passes`, `pass_max_us`, `stage_us`. `sim/rmp_e2e.py` step 5 checks it.

**Installing stalls the Teensy (a 64 KB erase: ~0.25-0.28 s with interrupts off, below): not while it plays.** The store (`rmp_store.h`) is a 4 MB
LittleFS in program flash (works without an SD card), files `/machines/<id>.rmp`, one version per id, installs capped at
64 KB. An install is chunked, verified in the flash file with a streaming CRC (512-byte stack chunks, no malloc) and
committed with an atomic rename; an abort or a leftover install file leaves the old package. Main loop only, one
install at a time.

**The store's size is fixed (`RMP_STORE_BYTES`, 1 MB until 2026-10-10, then 4 MB at 0x603C0000).** The store sits at
the top of program flash, so a build with another size has its base elsewhere, and a smaller store's base lies inside a
larger one (1 MB: 0x606C0000, inside the 4 MB store). LittleFS 2.4's mount does not check the superblock's block count,
and the core's `LittleFS_Program::begin` formats whenever a mount fails: an unguarded build with another size **may mount
stale data** (an old 1 MB store's leftovers in the 4 MB store's blocks, and then write into them) **or reformat**. So
`rmp_store_begin` probes the raw flash before mounting (`rmp_store_probe.h`): our base and every known size's base
(`RMP_STORE_KNOWN_SIZES`). It mounts only its own superblock (littlefs 2.x, 64 KB blocks, our block count) with no larger
store's metadata below it (any CRC-valid commit in a larger base's pair {0, 1}, a superblock found along its tails or
not), formats only a region that is entirely erased (first boot; the watchdog fed before the format and before every
erase), and otherwise mounts nothing and formats nothing: the boot log says `plugins: foreign store layout ...` with
the probe's findings (`plugins: store probe: ours; 4096K@603c0000 lfs2.0 64x64K; 1024K@606c0000 none`, a larger base
with a commit but no superblock as `commit, no superblock`), the user slots stay MISSING, and only an explicit wipe
(`rmp_install.py wipe --yes`, frame `'F'` `WIPE`; refused while the transport runs, an install is open, a slot loads
or the device-info report runs) formats it, then erases a larger known store's superblock pair below ours, or every later boot would find
it again (in that order a wipe cut short still finds the larger store and refuses). The probe finds a superblock as
`lfs_mount` does, from the pair {0, 1} along the metadata tails (littlefs 2.4's superblock expansion leaves {0, 1} with
only a hard tail), and stops at the first pair that holds one, where `lfs_rawmount` walks the rest of the chain:
damage further along reads OURS here and fails the mount, and a mount that fails on our own superblock stays
unmounted (never formatted).
**Builds older than the user-slots merge (2026-10-11; 9870991 and older use 1 MB at 0x606C0000) have no guard: do not
flash one on a Teensy with this store** - it would mount or format at 0x606C0000, inside this store. The guard cannot see that damage
afterwards (our superblock at 0x603C0000 is intact). `run_sketch.sh` prints where the program image ends against the
store's base (`RMP_STORE_BYTES` from `RMP_EXTRA_FLAGS` when set there), warns within 256 KB, and stops before the upload
when the image ends inside the store or less than 64 KB below it, or when it cannot check (no nm or ELF, the define not
read; `RMP_ALLOW_NO_IMAGE_CHECK=1` uploads anyway). The three overrides (`RMP_ALLOW_NO_IMAGE_CHECK`,
`RMP_ALLOW_ITCM_GROWTH`, `RMP_ALLOW_LOW_LOCALS`) act only when set to exactly `1`; `0` or any other value keeps the
check fatal.

**The erase stall.** The core's `eeprom.c` erases and programs program flash from RAM with interrupts masked until the
flash is ready. A 64 KB block erase (LittleFS taking a fresh block for a file, or compacting a directory) holds
interrupts off for 0.25-0.28 s (measured 2026-10-10 with the cycle counter; the flash's datasheet maximum is far
longer): `millis()`/`micros()` lose that time, so voiceloop times the 1E deadlines (`plug_ms`, `kPlugAnsMs` 1000 ms) and
the install logs with the cycle counter (`ARM_DWT_CYCCNT`, wrap-safe), and nothing is serviced meanwhile - **in the
USB-host build the Overbridge output has a gap of about the erase time during an install**. Measured wall times (serial
build): a 5 KB install from the card 272 ms in 3 passes (one pass 266 ms: the erase), a 60 KB one 372 ms in 30 passes
(longest 266 ms), a serial install of 5 KB 564 ms (one 'J' write 277 ms: the file's fresh block; the commit 278 ms:
an erase in the rename's commit). So an install from the card is refused while the
transport runs (STOP TO INSTALL) and a copy under way pauses for it. Every erase feeds the watchdog first (`rmp_store.cpp`'s
erase hook), so two erases in one LittleFS operation each get the whole 2 s.

The Teensy 4.1's LittleFS blocks are **64 KB** (the core's `SECTOR_SIZE`): the 4 MB store is 64 blocks, and every package
(any size) takes at least one, the directories a pair each (measured 2026-10-10 on the 1 MB store of then: 9 packages in
14 blocks, 128 KB free - room for two more; hence 4 MB). The core mounts with `metadata_max` 0 (a whole block), so a directory's commit log grew to tens of KB
before LittleFS compacted it, and every lookup scans that log: a `LittleFS::open` (a stat, then the open) took 5.1 ms, an
`lfs_file_open` 2.2 ms, a directory entry 1.1 ms (`openNextFile` 3.5 ms: it opens each entry), `usedSize` 6.9 ms; the
device-info report's passes reached 7.1 ms (dropouts in `stress.py u`). `rmp_store.cpp` mounts with `metadata_max` 4096
(`RMP_STORE_META_MAX`; a commit past it compacts: one 64 KB erase, in an install or a remove - an install every ~6 took
~540 ms instead of ~270 ms in a churn test) and reads and lists through `lfs_*` directly (no stat before an open, no open of
each directory entry). Measured over 40 install/remove cycles: an open 0.07-0.2 ms, a whole 10-entry listing 0.6 ms at
most, `lfs_fs_size` 0.56 ms at most. `cache_size` 512 or 1024 instead of 128 was no faster (slightly slower: the cost is the
log scan, not the reads) and is left at the core's 128.

## 5. The slot map file

Which machine each user slot plays belongs to a project: `/.rytm/slots/<PROJECT>.txt` on the SD card. One line per used slot:

```
U<n> <id> <a.b.c>
```

Fields separated by spaces or tabs; `n` in 1..5; `id` passing the id rule; `a`, `b`, `c` 1..3 digits, each <= 255, no leading
zero except "0" itself (so parse and format round-trip exactly). Blank lines are skipped; CRLF and a last line without a
newline are fine. **Anything else makes the whole parse fail (no slot used)**: a stray line, a slot out of range or named
twice, an unsafe id or a NUL inside the id, a missing/extra/too large version part. The slot's version must lie in the
installed package's `[compatible_from, version]` range, else the slot is MISSING.

The map is saved as `<PROJECT>.txt.tmp` (written, byte count checked, synced), the old file removed, then the `.tmp`
renamed; a reload that finds only the `.tmp` (a reset interrupted the rename) finishes the rename. An assign builds the
entry locally and changes the map only after it is validated and saved (a refused assign leaves the slot as it was); the
same id and version on a playing slot does not reload; with no card or a failed write nothing is assigned and it is
logged. The save is spread over three loop passes.

## 6. How voiceloop runs machines

`RYTM_PLUGINS` (default 1 in both builds since 2026-10-10: the USB-host build is the final setup;
`-DRYTM_PLUGINS=0` builds without them). Both builds link with `teensy/cold_ld.sh` (LittleFS's core and
SdFat in program flash, the plugin arena first in DTCM at 0x20000000) and keep the per-message parsers and the
rarely run code (`FLASHMEM`, strings `PSTR`) in flash. Measured 2026-10-11 (8fdbc0e, Teensyduino 1.62,
`opt=o2std`), as `run_sketch.sh` prints them:

| build | ITCM code | blocks | headroom | RAM1 free for locals | RAM2 free (malloc) | image ends |
|---|---|---|---|---|---|---|
| serial (`run_sketch.sh voiceloop`) | 118216 B | 4 | 12856 B | 117408 B | 18112 B | 0x6006B400 |
| host (`RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1"`) | 128904 B | 4 | 2168 B | 88896 B | 18112 B | 0x60070400 |
| serial without plugins (`-DRYTM_PLUGINS=0`) | 108120 B | 4 | 22952 B | 155520 B | 41248 B | (not measured) |

The host build's headroom is small: about 2 KB more ITCM code takes a 5th block (32 KB less stack), so new code goes
to `FLASHMEM` with `PSTR` strings. `run_sketch.sh` builds per checkout and per `RMP_EXTRA_FLAGS` (its own build folder:
the serial and host builds do not rebuild over each other), warns under 4 KB of ITCM headroom, refuses to upload when
the block count passes the recorded one (voiceloop serial 4, host 4; `RMP_ALLOW_ITCM_GROWTH=1` overrides) and under 40 KB
of locals, checks the arena's address and
stops before the upload when the program image ends inside the plugin store or under 64 KB below it, or when it
cannot check (section 4). Source: `teensy/voiceloop/voiceloop.ino`, library `teensy/libraries/RytmPlugins/`.

- **Loading.** The project's slot map is loaded from `loop()`, between requests, at boot, at a project change, after an
  assign and after an install. A full load resets the arena and loads each slot's machine **streaming and incrementally**:
  one slot at a time over several loop passes (a few ms each): pass 1 reads the header (opens the store file, 0.1-0.2 ms on
  the program-flash store, section 4; a 512-byte read ~30 us), then n CRC passes of 8192 bytes each, then the placement
  (`rmp_place_stream` reads the image straight into the arena, zeroes bss and applies the relocations 32 at a time; the
  package is read twice, a CRC pass and a place pass, and is never in RAM whole), then the next pass runs `init()` through
  the guard (100 ms limit; nonzero = BAD). The slot plays nothing (LOADING) meanwhile and everything else keeps playing;
  one cached store handle made this dropout-free (file opens had been 50 of a 52 ms load). Two slots naming the same id share
  one image (and one `init`); a package that fails as a whole (BAD, NO_ROOM) is tried once. An assign or install of ONE slot
  appends to the arena without a reset (the replaced slot's old image stays allocated until the next full load); when the
  arena is full the host falls back to a full reload.
  Slot states: EMPTY, MISSING (not installed, unsafe id, or version outside the range), BAD (unreadable, header/CRC wrong,
  id mismatch, relocation error, wrong entry magic/api, over the state limit, or `cpu_permille` over 500), NO_ROOM (arena
  full), READY, MUTED, LOADING. `state_bytes` and `cpu_permille` are copied at load (the header in the arena is
  plugin-writable memory); the budget arithmetic is 64-bit.
- **Voices and state.** A trig on voice flag 0x50..0x70 goes to `plug_report`. The voice's state is **the voice's own 16 KB
  scratch** `shared[voice]` (the Plaits engine memory, in RAM2): zeroed and `voice_init` called when the voice starts a
  slot, then `params` (P1..P16) and `note_on`; `note_off` on 0x11. A state over 16384 B needs PSRAM (`rmp_state`, a bump
  allocator reset at a project change; there is no other state pool), else the slot is BAD ("needs N B state, the voice has
  16384"). Because the state occupies Plaits' scratch, **when a voice leaves the user slots (back to PLAITS or 6-OP FM) its
  Plaits voice is initialised again** (`plug_voice_reinit`, ~129 us, output bit-identical to a fresh voice; done on the
  engine switch, so nothing sits on the render path). A voice moved to another slot starts the new machine afresh. A trig on
  a slot that plays nothing is logged once per reload ("not playing (see plugins:)" or "empty").
- **The sound tag** (spec 2.6, docs/superpowers/specs/2026-10-10-user-slots-rytm-design.md). A 43-byte 0x04 / 41-byte
  0x03 ends with the sound's plugin fingerprint (0 untagged; shorter reports read as 0); a READY slot's 1A part 0 (always
  43 bytes) carries `rmp_plugin_fp(id)` (FNV-1a32 of the id mod 16383, + 1). A 0x04 whose tag is neither 0 nor the READY
  slot's fingerprint releases the voice (`note_off`, `plug_leave`, its held note dropped), then sets its mismatch bit:
  silent, its 0x03s dropped, until a matching or untagged 0x04 starts it afresh; logged once per mismatch ("trig voice 5
  U1 rytm.sine_test: sound tag 12437, the slot's 716 - silent until a matching trig"). A slot not READY compares nothing;
  `plug_leave`, a slot's load and the slot becoming READY clear the bits. 0x11 is handled as always.
- **Rendering.** `render` is called with 12-sample blocks through the guard; the output is clipped to +-1 and a NaN becomes
  0. Plugin CPU is shown under "load user" in the periodic log with the longest render.
- **Safety.** See section 7. A fault or timeout mutes every slot sharing the image; three renders in a row over the limit
  mute the slot. Mutes are logged from `loop()` ("plugins: U2 <id> CRASHED - muted", "timed out - muted",
  "over budget - muted"), never inside an audio frame.
- **Housekeeping.** Load-time code (loader, slots, store, `*_begin`) runs from program flash (`RMP_COLD`/`FLASHMEM`) with
  integer-only `sniprintf`, so voiceloop fits ITCM in 4 blocks (the table above). Card and store work runs from
  `loop()` only when the next request is not due: the serial build waits on `!Serial.available()`, the host build on
  `host_waiting()` (Overbridge streaming: the FIFO at its target; not streaming: `!Serial.available()`, the serial
  link's `'N'` requests being the clock then). Timing that spans a flash erase uses `plug_ms()` (milliseconds from the
  cycle counter, accumulated every loop pass and before every erase; wrap-safe at 7.16 s), not `millis()`. Flash code does run from interrupts: the USB
  host interrupt calls `RytmOverbridge::control`/`advance`/`step`/`start_stream` (FLASHMEM) through the driver's vtable,
  and the `queue_*` long-branch veneers sit in flash. That is safe because a flash erase or program masks interrupts for
  its whole length and runs from ITCM (the core's `eeprom.c`): no interrupt can fetch from flash while it is busy. A stale `CrashReport` is consumed once at setup
  as log lines. `RYTM_STACK_DEBUG` (0) paints the stack and logs its high-water mark and the RAM2 heap. Measured
  2026-10-11 on the host build (`RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1 -DRYTM_STACK_DEBUG=1"`, ITCM 129048 B, headroom
  2024 B): 7584 of 88864 B used at boot (setup), and never 256 B more (the logging step) through the host `rmp_e2e`
  (153 checks, ALL PASS) and `stress.py run u c` - the 40 KB locals floor leaves a wide margin.

## 7. The guard

`rmp_guard_call(fn, arg, limit_cycles)` (`teensy/libraries/RytmPlugins/src/rmp_guard.h`) wraps every call into a plugin.

Behaviour:

- A fault inside the call (HardFault, MemManage, BusFault, UsageFault; also with interrupts masked, which escalates to a
  HardFault; null writes, any bus error) returns `RMP_GUARD_FAULT` to the call site through a longjmp trampoline (naked,
  nothing pushed on the plugin stack); PRIMASK, BASEPRI and FPSCR are restored on every return. `dsb`/`isb` after the call
  land imprecise BusFaults inside the window. Stacking/unstacking errors are checked before the frame is read. Faults
  outside a plugin go to the core's own handler (the crash report is kept).
- An overrun: a 2 kHz IntervalTimer (PIT) pends NVIC IRQ 71 (a free vector; `IRQ_SOFTWARE` belongs to the Audio library),
  whose handler rewrites the return path; the call returns `RMP_GUARD_TIMEOUT`. Nested calls are refused
  (`RMP_GUARD_BUSY`).
- Limits voiceloop passes: `init` 60M cycles (100 ms); `voice_init`, `params`, `note_on`, `note_off` 300000 cycles (0.5 ms);
  `render` `2 x cpu_permille x 150 + 2000` cycles. **Crash: the slot is muted.** **Timeout: muted.** **Over budget** (a
  render that returns but used more than its limit) **three times in a row: muted.** A muted slot stays silent until it is
  assigned again (its state is abandoned, `voice_init` runs before reuse); the Teensy keeps running and the other slots keep
  playing.
- **Watchdog.** RTWDOG, 2000 ms in voiceloop (`RYTM_WDOG_MS`; the library default is 100 ms; the 32.76 kHz LPO clock
  measured), armed at the end of setup after the boot work and fed once per `loop()` pass and per `send_frames` piece. A
  reset caused by it (SRC_SRSR bit 7) boots into **safe mode**: no user machines that boot ("plugins: safe mode - not loaded"
  on each reload); `rmp_guard_begin` clears the sticky bit so the next boot is normal. Measured worst `loop()` passes:
  3.7 ms (a slot load pass: the store's file open), 0.25-0.28 s an install pass with a 64 KB erase (section 4; every
  erase feeds the watchdog first, so a pass with two erases is safe too), 3.1 ms an 'N' of 1024 frames on 8 sine_test
  voices, 2.7 ms `project_load` (5-slot pool); a pass over 500 ms is logged when it is the longest yet ("plugins: a
  loop pass took N us").

Limits (what the guard does not do):

- Resolution: the overrun check runs on a 2 kHz tick, so a runaway is cut up to ~1 tick (300k cycles at 600 MHz, about 2
  audio blocks' worth of CPU) past its limit (measured: a 600k limit returned at 684k-752k cycles).
- Timeouts only from thread mode (`loop()`) with BASEPRI 0: the guard's interrupt is at the lowest priority (240 of 16
  levels). From an ISR, faults are caught, but an endless loop ends in a watchdog reset.
- Code running with interrupts disabled (PRIMASK/BASEPRI) cannot be cut: the watchdog resets the Teensy.
- No isolation: a plugin runs privileged and can overwrite host memory, the vector table, the MPU or the watchdog; the
  guard only catches what faults or overruns (write-fencing is deferred to the open SDK).
- A timeout (or fault) inside a host callback (`h_log`, ...) abandons that callback halfway; the plugin's own state is
  abandoned too (mute the slot, `voice_init` before reuse).
- The RTWDOG timeout must cover voiceloop's longest blocking step (card listing alone measured 81 ms at boot; index build,
  project/pool loads are longer): feed inside those loops, start the guard after them, or pass a larger `wdog_ms`.
  `rmp_guard_feed` is a no-op after `begin(0)`; `rmp_guard_timer_ok()` reports a failed `tick.begin` (no timeouts then).
- Read the reset reason (`rmp_guard_reset_reason`) before printing `CrashReport`, whose print clears SRC_SRSR.

## 8. Testing

- **Mac, no hardware** (`teensy/libraries/RytmPlugins/extras/`): `loader_test.cpp` (verify, place, relocate, corrupt
  packages, formats 1 to 3 accepted, 0 and 4 refused), `slots_test.cpp` (map parsing, load decisions, sharing, the
  streaming and incremental paths, on an in-memory store and a malloc'd arena), `manifest_test.cpp`, `proto_test.cpp`
  (and `-DPROTO_DUMP`, compared byte for byte with Python by `sim/tests/test_protocol_us.py`; the `rmp_plugin_fp`
  vectors), `resolve_test.cpp` (which machine a slot plays when a project loads: the 0x25 parser, the notes file,
  own map, fingerprints, collision, inherit, explicit empty, saved vs fingerprint, import), `meta_test.cpp`, `presets_test.cpp`, `pool_test.cpp` and `store_probe_test.cpp` (the store-layout probe on
  littlefs images made with the core's own littlefs: blank, ours, a larger or smaller store, superblock expansion,
  torn commits at every byte, an interrupted wipe). Build lines are in each file's header, for example
  `c++ -std=c++17 -O1 -I../src loader_test.cpp ../src/rmp_loader.cpp -o /tmp/loader_test`.
  `tools/tests/test_rmp_pack.py` (builds sine_test with the ARM toolchain, packs it, checks header, relocations,
  manifest, negative cases and the layout against `rmp_format.h`) and `tools/tests/test_rmp_api.c` (ABI offsets).
- **Native harness** (`sim/rmpvoice.py`, `sim/rmp/rmp_native.c`): a machine's `lib<name>.dylib` through the same
  `rmp_plugin` table. `sim/tests/test_rmp.py` (`cd sim/tests && PYTHONPATH=.. ../../.venv/bin/python -m unittest test_rmp`;
  13 tests): sine_test pitch, tune and note-off; Noise Plethora Atari and the three packages (every algorithm sounds without
  NaN, no pool allocation failure, voices independent, ALG sweep, a swap equals a fresh voice for the 12 deterministic
  algorithms); the NP tests skip without the Teensyduino Audio library or the ARM toolchain. V2Test (8 tests): an API-1
  plugin unchanged, voice_enter/leave, native and generic presets, p-lock without write-back, the pool without PSRAM
  streamed exactly, a jump outside the head, PSRAM random access. `sim/tests/test_protocol_us.py`: the 19..1E messages
  and the sound tag against the C++ builders. All of `sim/tests` (`cd sim && sampler/build.sh && PYTHONPATH=.
  ../.venv/bin/python -m unittest discover tests`): 60 tests, 2 skipped (NP without `vendor/`).
- **On the Teensy**: `teensy/rmp_test` (a sketch: arena, MPU, store and the guard's four fault modes, then sine at 1.00 % of
  the Teensy) and `sim/rmp_e2e.py` (talks to voiceloop over the serial link without the emulator; needs `rytm.sine_test` and
  `rytm.fault_test` installed: 440 Hz sine on U1, null-write crash on U2 muted, endless loop timed out, long render over
  budget; the Teensy stays alive and U1 keeps playing; step 5 the device-info report; step 6 `rytm.state_test` (below);
  steps 7..13 need `plugins/v2_test` built: metadata and generations, presets, a pool knob (in project E2ETEST), the slot
  menu's 1E, install from the card, the sound tag, a Rytm that starts while the Teensy runs; `--host` for the USB-host
  build; exit 0 when ALL PASS, 153 checks on both builds).
- **Monday check: PSRAM and big state** (`plugins/state_test`, `rytm.state_test`: a sine_test variant whose per-voice state
  is 20000 B, more than the 16384 B a voice has). Install it (`make -C plugins/state_test state_test.rmp`,
  `tools/rmp_install.py install plugins/state_test/state_test.rmp`), then `sim/rmp_e2e.py` step 6 decides by the report's
  `psram_mb`: without PSRAM U3 must be refused (BAD, log "U3 rytm.state_test needs 20000 B state, the voice has 16384";
  measured and passing on the stock board); with PSRAM (`psram_mb` > 0) U3 must be READY and 8 voices must play 440 Hz.
  The PSRAM path itself (one `rmp_state` block per voice, reload only when stale blocks exist) is untested until then.
- **Regenerating `teensy/rmp_test`'s packages** (`sine_pkg.h`, `fault_pkg.h`, git-ignored C arrays after a plugin rebuild):
  `xxd -i plugins/sine_test/sine_test.rmp | sed 's/sine_test_rmp/sine_pkg/g' > teensy/rmp_test/sine_pkg.h` and the same for
  `fault_test` / `fault_pkg`.
- **After merging the Voice::Init patch.** `teensy/setup_plaits_lib.sh` now patches `plaits::Voice::Init` into program flash
  (an ITCM saving). `teensy/libraries/Plaits` is generated and git-ignored: in the main checkout run
  `rm -rf teensy/libraries/Plaits && teensy/setup_plaits_lib.sh` once so the patch applies (the script fails loudly if it
  does not). `teensy/run_sketch.sh` prints the ITCM line ("ITCM N B in K blocks, headroom P B, locals F B") and refuses to
  upload with under 40 KB of locals (`RMP_ALLOW_LOW_LOCALS=1` overrides); `RMP_EXTRA_FLAGS="-DRYTM_PLUGINS=0"` builds
  without the plugins.
- **Stress** (`sim/stress.py run u`, `sim/stress.py run np --np a|b|c`): scenario `u` plays 8 user-slot sine_test voices with
  sweeps and note-offs, the fault machine on U2 crashes mid-run (muted), is re-assigned and times out (muted), U1 keeps
  playing; user CPU 13.1 % for 8 sine voices (the only dropouts were the 47 ms slot reload, measured before the
  incremental loads; 13.5 % and 0 dropouts on both builds, 2026-10-11). Scenario `c` (8 DSP SAMPLE streams) 0 dropouts on
  both builds. Scenario `np`: section 9.5 (`np --np c` 2026-10-11: 0 dropouts on both builds, user CPU at most 28.3 %).

## 9. Measured numbers

### 9.1 The arena (Teensy 4.1, -O2, 600 MHz)

With PSRAM fitted there is a second code arena in PSRAM (overflow from this one; longer guard limits for its slots):
[psram-testing.md](psram-testing.md). Without PSRAM nothing below changes.

Copied code runs from RAM as fast as from ITCM. A 4096-iteration int16 loop (`sum_loop`, no `bl`, checked with objdump), once
from ITCM (reference) and once copied to a RAM buffer with an MPU region added (spike sketch `teensy/arena_spike`, scratch,
not committed):

```
reference: result 25163776, 20517 cycles     (ITCM)
DTCM: result 25163776, 20517 cycles
OCRAM: result 25163776, 20550 cycles
```

OCRAM is +33 cycles = +0.2 % (the first cache fill). The core's own MPU uses regions 0..10 (`SCB_MPU_CTRL = ENABLE` only,
no PRIVDEFENA); a higher region number wins on overlap, so the arena's regions 13 and 14 override the core's NOEXEC RAM
regions. Loading: `memcpy`, `arm_dcache_flush(dst, n)`, `dsb; isb`, invalidate the I-cache (`SCB_ICIALLU` = 0xE000EF50,
not defined by the core headers), then call `dst | 1` (Thumb). A pure function called through a plain function pointer is
hoisted out of a DWT cycle measurement; call through a `volatile` function pointer.

**Current arena:** DTCM, 32 KB (`RMP_ARENA_BYTES`), 8 KB-aligned (`RMP_ARENA_ALIGN`, one MPU subregion), MPU regions 13 and
14 (`rmp_arena.h`). A 32 KB-aligned arena landed on the first 32 KB line after `.data` (0x20010000 with voiceloop's 35.5 KB
`.data`): ~29 KB of padding. Instead the arena is covered by 64 KB regions with 8 KB subregions: region 13 on the window
`w0 = arena & ~0xFFFF`, region 14 on `w0 + 64 KB`, each with only the subregions overlapping `[arena, arena + 32 KB)` enabled
(SRD bit k = 1 disables subregion k, which falls through to the core's NOEXEC DTCM region); region 14's RASR is 0 when one
window holds the arena. RASR = TEX(1) C B AP(3) SIZE(15) SRD ENABLE; set with PRIMASK masked and MPU_CTRL saved and
restored; `rmp_arena_mpu_readback` reports both regions. Measured: `rmp_test` (arena 0x20004000, one window, R13 RASR
030bc31f = subregions 2..5) and voiceloop (then at 0x2000A000 across 0x20010000: R13 SRD 0x1F, R14 SRD 0xFE) run code at the
arena's first and last halfword; a jump to arena + 32 KB or arena - 4 comes back from `rmp_guard_call` as FAULT (IACCVIOL).
**Placement in voiceloop:** the arena (`rmp_arena_mem`, extern "C") is in its own section `.dtcm_arena`, which
`teensy/cold_ld.sh` makes the first input of `.data`, so it sits at 0x20000000 (one window: region 13 with subregions
0..3, region 14 off) and no padding before it is lost to the stack. As the first 8 KB-aligned object of `.bss` it followed
`.data` up to the next 8 KB step (6464 B of padding in the host build; the serial build was 320 B from losing 8 KB). It is
in `.data`, not a NOLOAD section, because `teensy_size` counts only `.data` and `.bss` as RAM1 variables (a NOLOAD arena
overstated free locals by 32 KB); the cost is 32 KB of zeros in flash, copied at boot. `teensy/run_sketch.sh` checks the
address with nm after every build (a warning when it is not 0x20000000, a note when it cannot check). Allocation is a bump with a
whole reset (`rmp_arena_alloc`: 8-aligned, overflow-safe, nullptr when full). OCRAM also works as an arena and is the fallback
if DTCM gets short.

### 9.2 How big is Noise Plethora? (measured, arm-none-eabi, -O2, one object per plugin)

Scope: only NP's sound generators are used as voices. The 9 effect-based algorithms are dropped (freeverb: BasuraTotal,
S_H, satanWorkout, Rwalk_BitCrushPW, Rwalk_LFree; granular: grainGlitch, grainGlitchII, grainGlitchIII; flange:
Rwalk_SineFMFlange). 21 remain: bank 1 radioOhNo, xModRingSqr, XModRingSine, CrossModRing, resonoise, basurilla; bank 2 all
10; bank 3 Atari, WalkingFilomena, arrayOnTheRocks, existencelsPain, whoKnows.

Plugin code is the `.text` of each plugin's own object, with `REGISTER_PLUGIN` defined as a bare `new Plugin` factory (the
std::map/std::function registry glue was about 1.35 KB per plugin: XModRingSine 1936 -> 592 B, radioOhNo 2988 -> 1644 B,
phasingCluster 8864 -> 7516 B). Audio code is the Teensy Audio library objects the plugins use (synth_waveform 3988 B, which
holds Waveform, WaveformModulated and WaveformPWM; mixer 388; SineModulated 676; StateVariable 624; multiply 156; NoiseWhite
216; Dc 400; wavefolder 116; pink noise 1236 + 768 B tables; freeverb 7064; granular 792; flange 492; delay 516; bitcrusher
196). Read-only tables are tiny: AudioWaveformSine 514 B, step_table (band-limit) 516 B, sinTable_q15 1026 B. All 21 kept
plugins together: 68.2 KB plugin code, 6.6 KB audio objects (waveform, mixer, NoiseWhite, Dc, SineModulated,
StateVariable, multiply, wavefolder) = 74.8 KB in one blob, which cannot fit one 32 KB arena (and not two: 2 x 32 = 64 KB),
hence three packages.

Per plugin (code B / per-voice state B = `sizeof`, at 128-sample blocks): radioOhNo 1644/3964, xModRingSqr 780/1948,
XModRingSine 592/196, CrossModRing 1688/3964, resonoise 1264/1224, basurilla 1780/1568, clusterSaw 4984/15340, pwCluster
2628/5988, crCluster2 2732/6348, sineFMcluster 3776/8348, TriFMcluster 3776/8348, PrimeCluster 5672/15692, PrimeCnoise
5676/15692, FibonacciCluster 5576/15692, partialCluster 5560/15692, phasingCluster 7516/22060, Atari 676/1876,
WalkingFilomena 5616/7380, arrayOnTheRocks 2076/1860, existencelsPain 2092/4604, whoKnows 2096/4604. (Audio blocks from the
graph's pool come on top.)

Grouping (3 is the minimum; each package carries its own copy of the audio objects it uses, so the package totals add up to
84.1 KB against the 74.8 KB single blob; bank 2's ten clusters alone are 45.4 KB of code and are split over A and B). The
spike's estimate, before the shim and the block sizes; the built packages are in 9.4:

| package | plugins | plugin code | audio objects | total (spike) | largest per-voice state (spike) |
|---|---|---|---|---|---|
| A: prime/fibonacci/partial clusters | PrimeCluster, PrimeCnoise, FibonacciCluster, partialCluster | 22.5 KB | 4.6 KB (waveform, mixer, NoiseWhite) | 27.1 KB | 15692 B (all four) |
| B: saw/pw/FM clusters | phasingCluster, clusterSaw, sineFMcluster, TriFMcluster, crCluster2, pwCluster | 25.4 KB | 4.8 KB (waveform, mixer, Dc) | 30.2 KB | 22060 B (phasingCluster) |
| C: ring/mod, noise and the rest | radioOhNo, xModRingSqr, XModRingSine, CrossModRing, resonoise, basurilla, Atari, WalkingFilomena, arrayOnTheRocks, existencelsPain, whoKnows | 20.3 KB | 6.6 KB (waveform, mixer, NoiseWhite, Dc, SineModulated, StateVariable, multiply, wavefolder) | 26.9 KB | 7380 B (WalkingFilomena) |

### 9.3 Audio shim sizes (measured)

Atari (`plugins/noiseplethora`, NP_BLOCK 128): image 6460 B (+ 12 B bss), 38 relocations; state 3264 B per voice (plugin 1876 B
+ a 4-block pool of 260 B + graph, tap, cord, pointers). All 21 kept plugins compile against the shim and run natively
without an allocation failure (64-block pool). Per-voice `sizeof` on the Teensy at 16-sample blocks and the most blocks a
graph held at once: radioOhNo 2172 B/4, xModRingSqr 1052/3, XModRingSine 196/2, CrossModRing 2172/4, resonoise 776/5,
basurilla 1568/3, clusterSaw 8172/16, pwCluster 3300/7, crCluster2 3660/6, sineFMcluster 5660/6, TriFMcluster 5660/6,
PrimeCluster 8524/16, PrimeCnoise 8524/16, FibonacciCluster 8524/16, partialCluster 8524/16, phasingCluster 14892/16, Atari
980/2, WalkingFilomena 7380/16, arrayOnTheRocks 1412/1, existencelsPain 2812/8, whoKnows 2812/8. At 128 (the five feedback
plugins): radioOhNo 3964 B/4, xModRingSqr 1948/3, XModRingSine 196/2, CrossModRing 3964/4, Atari 1876/2. A voice's state is
that plus 4 + 2 x NP_BLOCK bytes per pool block (`AUDIO_GRAPH_BLOCKS`; 864 B for 24 blocks at 16, 6240 B at 128; size the
pool per package from the block counts) plus ~130 B at 16 (~350 B at 128: the tap holds one block).

### 9.4 The packages A, B, C (as built, format 2)

| package | id, name | algorithms (ALG order) | block | pool | image + bss | .rmp | state per voice | arena use on the Teensy |
|---|---|---|---|---|---|---|---|---|
| A | befaco.np.a, NP CLUST1 | PrimeCluster PrimeCnoise FibonacciCluster partialCluster | 16 | 20 | 23272 + 16 B | 24160 B | 9372 B | 23416 of 32768 B |
| B | befaco.np.b, NP CLUST2 | phasingCluster clusterSaw sineFMcluster TriFMcluster crCluster2 pwCluster | 16 | 20 | 26556 + 12 B | 27652 B | 15740 B | 26704 B |
| C | befaco.np.c, NP VOICES | radioOhNo xModRingSqr XModRingSine CrossModRing resonoise basurilla Atari WalkingFilomena arrayOnTheRocks existencelsPain whoKnows | 128 | 20 | 27880 + 16 B | 29616 B | 12932 B | 28024 B |

(Atari alone: `befaco.np.atari`, 6992 B .rmp.) All three fit the 32 KB arena (largest C, 27.9 KB) and **every state is under
16 KB (A 9.4, B 15.7, C 12.9 KB), so all 8 voices of every package play on a stock Teensy 4.1 without PSRAM** (state =
largest member + pool + ~140 B for A/B, ~360 B for C; the pool is the members' measured maximum, 16 blocks for the
prime/fibonacci/partial/phasing/saw clusters and WalkingFilomena, + 4). `cpu_permille` is set from the Teensy's PEAK
per-call cycles: A 62, B 76, C 204.

### 9.5 CPU on the Teensy (stress.py scenario np, 8 voices, no PSRAM)

`sim/stress.py run np --np a|b|c` on the Teensy without PSRAM (after the state moved into the voices' own 16 KB): the
package on U1 of the STRESS project, **all 8 voices playing**, P1 stepping through every algorithm (5 s each), X/Y swept by
0x03 at the Rytm's rate. "User CPU" is the "load user" figure of the log periods inside the algorithm's step; the longest
render call (12 samples; the call that runs a graph pass) is given as a percentage of the guard limit
`2 x cpu_permille x 150 + 2000`: A 20600 cycles, B 24800, C 63200. 0 dropouts, no fault or timeout (ALG swaps included),
no "no state room"; minimum buffer 10.0 ms (A), 9.3 ms (B), 10.7 ms (C); levels -13 dBFS (A, B) and -11 dBFS (C) with 8 voices.

| pkg | ALG | algorithm | user CPU, 8 voices | longest render call, % of the guard limit |
|---|---|---|---|---|
| A | 0 | PrimeCluster | 64.1 % | 78 % |
| A | 1 | PrimeCnoise | 64.1 % | 76 % |
| A | 2 | FibonacciCluster | 37.1 % | 46 % |
| A | 3 | partialCluster | 37.1 % | 45 % |
| B | 0 | phasingCluster | 79.6 % | 81 % |
| B | 1 | clusterSaw | 37.5 % | 40 % |
| B | 2 | sineFMcluster | 36.2 % | 35 % |
| B | 3 | TriFMcluster | 36.3 % | 35 % |
| B | 4 | crCluster2 | 31.0 % | 31 % |
| B | 5 | pwCluster | 21.9 % | 22 % |
| C | 0 | radioOhNo | 15.9 % | 33 % |
| C | 1 | xModRingSqr | 11.4 % | 18 % |
| C | 2 | XModRingSine | 9.7 % | 12 % |
| C | 3 | CrossModRing | 15.9 % | 32 % |
| C | 4 | resonoise | 14.4 % | 28 % |
| C | 5 | basurilla | 10.3 % | 14 % |
| C | 6 | Atari | 10.2 % | 14 % |
| C | 7 | WalkingFilomena | 15.6 % | 31 % |
| C | 8 | arrayOnTheRocks | 10.3 % | 14 % |
| C | 9 | existencelsPain | 28.1 % | 71 % |
| C | 10 | whoKnows | 28.0 % | 71 % |

Every algorithm runs real time with 8 voices on its own (worst phasingCluster, 79.6 %); with other machines playing, the
clusters (A 0/1, B 0) leave the least room. Package C's 128-sample pass comes every ~10.7th call; staggering the voices'
first pass was not needed (the host's 512-frame buffer absorbs it; the worst calls, existencelsPain and whoKnows, use 71 % of
the limit).

The earlier single-voice figures (1-2 voices on the old 24 KB pool) gave the peak call that `cpu_permille` is derived
from: peak x 1.5, so A (13695 x 1.5 - 2000) / 300 = 62, B 76, C 204 (all under the 500 limit), i.e. limits of
20600 / 24800 / 63200 cycles. Natively an ALG change inside `params()` takes 0.1-1.2 us median (worst 30 us, scheduling noise), far
below the guard's 0.5 ms. sine_test declares 25 permille (measured ~17).

## 10. Noise Plethora packages and licences

`plugins/noiseplethora/np_bank.cpp` builds a group of NP algorithms (`banks.h`, the ALG order) as one machine: P1 ALG (one
zone per algorithm, names in the manifest as `names`), P2 X, P3 Y, P8 LEV, NP's output gains kept (PrimeCluster, PrimeCnoise
0.8 via `NP_GAIN`, all others 1.0). The voice holds a buffer the size of the package's largest algorithm; an ALG change in
`params()` destroys the cord and the object (virtual destructor: its connections disconnect and release their blocks),
zeroes the buffer, restarts the graph (empty pool and update list) and constructs the new algorithm there (same `random()`
seed). An ALG change is a hard cut; a NaN ALG selects zone 0. `make packages` in `plugins/noiseplethora` builds all four
(`PKG=atari|a|b|c`, objects in `build/<pkg>/`); the Makefile wipes `build/<pkg>` when the package's configuration (block,
pool, permille, flags) differs from `build/<pkg>/cfg.txt`, and copies only the `P_<name>.hpp` it builds from `vendor/np` (any
other NP file is refused). `./fetch.sh` fetches NP at 6dea196 into `vendor/` (git-ignored, never committed). NP's own
`Plugin.hpp`/`PluginFactory.hpp` (CC BY-NC-SA framework, with a std::map registry and a function-local static) are not
used: `shim/Plugin.hpp` replaces them. NP was tuned at 44117.647 Hz; at 48 kHz its pitches are 8.8 % higher.

**Licences and attribution.** `plugins/noiseplethora/NOTICE` lists every packaged algorithm's author and licence from its
header (source https://github.com/Befaco/Noise_plethora at 6dea196). 20 of the 21 algorithms are "Copyright (c) 2021 Befaco /
Jeremy Bernstein" (WalkingFilomena: "Befaco / Julia Mugica / Ivan Paz"), GPL-3.0-or-later. **FibonacciCluster**
(`P_FibonacciCluster.hpp`, "Copyright (c) 2021 Befaco / Jeremy Bernstein", "Licensed under Creative Commons CC-BY-SA v4") is
CC BY-SA 4.0, which Creative Commons declares one-way compatible with GPLv3: package A (with FibonacciCluster) is therefore
GPL-3.0 (version 3 only; manifest licence "GPL-3.0; Fibonacci CC BY-SA 4.0; T.Audio MIT"); B, C and atari stay
GPL-3.0-or-later. **This reading of package A's licence is to be confirmed by the user before it is distributed.** NP's
firmware and framework (CC BY-NC-SA 3.0) are not used. The packages also link Teensy Audio Library objects (MIT, PJRC):
`sdk/rmp/shim/audio/NOTICE`.

### 10.1 The small packages (2026-10-18) and why a slot is not playing

**Why.** Measured on the Teensy: A 23288, B 26568, C 27896, atari 6472 B of arena, so no two NP packages shared the 32 KB
arena (C + atari = 34368 B): the second got NO_ROOM. What made them big (`.elf` symbol sizes): the constructors (a
cluster's is ~2.2 KB: every Audio object's and every AudioConnection's constructor inlined; PrimeCluster 2180 B, process
1268, destructor 372, init 332), each package's own copy of the Audio objects (waveform + band-limit tables ~4.7 KB,
mixer, noise, filter...), and for WalkingFilomena newlib's sinf/cosf with soft-double helpers (~2.8 KB). Tables are small
(~1.5 KB); the plugin ABI has no flash-data access, and none is needed.

**What changed.** (1) `np_bank.cpp` / `np_single.cpp` (the NP classes' constructors, destructors, init, process, the glue;
not per sample) are compiled `-Os` on the Teensy (`NP_COLD` in the Makefile); the Audio objects' `update()` stays `-O2`.
`shim/Plugin.hpp` adds `pow(float, int)` as a double multiply, so `pow(knob, 2)` stays `x * x` at `-Os` (else libm's
double pow, ~9 KB). (2) Smaller groupings, eight packages (`make packages`; ids new, block size per algorithm as before):

| package | id, name | algorithms (ALG order) | block | permille | image + bss (arena) | state per voice |
|---|---|---|---|---|---|---|
| prime | befaco.np.prime, NP PRIME | PrimeCluster PrimeCnoise | 16 | 100 | 10908 + 12 B | 9372 B |
| fibo | befaco.np.fibo, NP FIBO | FibonacciCluster partialCluster | 16 | 62 | 10776 + 16 B | 9372 B |
| phase | befaco.np.phase, NP PHASE | phasingCluster clusterSaw | 16 | 130 | 11532 + 12 B | 15740 B |
| fm | befaco.np.fm, NP FM | sineFMcluster TriFMcluster | 16 | 76 | 10740 + 12 B | 6508 B |
| pw | befaco.np.pw, NP PW | crCluster2 pwCluster | 16 | 76 | 9908 + 12 B | 4508 B |
| ring | befaco.np.ring, NP RING | radioOhNo xModRingSqr XModRingSine CrossModRing arrayOnTheRocks | 128 | 400 | 12512 + 8 B | 9516 B |
| noise | befaco.np.noise, NP NOISE | resonoise basurilla Atari existencelsPain whoKnows | 128 | 400 | 13132 + 12 B | 10156 B |
| walk | befaco.np.walk, NP WALK | WalkingFilomena | 128 | 400 | 10736 + 16 B | 12932 B |
| atari | befaco.np.atari, NP ATARI | Atari | 128 | 60 | 6016 + 8 B | 3264 B |

Any two fit the arena (largest pair noise + ring 25.7 KB); three fit when they add up to 32 KB, e.g. prime + fibo + pw
(31.6 KB), walk + fm + pw (31.4 KB), atari + any two (at most 31.7 KB). Every state is under 16 KB (no PSRAM needed). The
first grouping is still built (`make legacy`, same ids and ALG order, now A 15192, B 17488, C 22688 B), so a project that
uses befaco.np.a/b/c keeps playing while those stay installed. **Migration** (a slot on A/B/C to the small set; the
ALG index in the new package): A 0,1 -> prime 0,1; A 2,3 -> fibo 0,1; B 0,1 -> phase 0,1; B 2,3 -> fm 0,1; B 4,5 -> pw
0,1; C 0-3 -> ring 0-3; C 8 (ArrayOR) -> ring 4; C 4,5,6 -> noise 0,1,2; C 9,10 -> noise 3,4; C 7 (Walking) -> walk 0.
The Pi gets the same set as RMPA (`make packages-a64`, `-O2`, np_<pkg>.a64.rmp; the docker build mounts Teensyduino's
Audio library read-only).

**Same sound.** Bit-identical to the first grouping, checked two ways: the Thumb-2 images themselves run in an ARM emulator
(unicorn, Cortex-A AArch32 with VFP: the same instructions; the host's call sequence, X/Y moved every 0.1 s, some runs with
an ALG swap) - every algorithm, old package against new (and A/B/C at -Os against A/B/C at -O2, and atari), 128 runs of
1.5 s, all bit-identical; and the native libraries (`libnp_<pkg>.dylib`, 2 s at two X/Y settings), 84 runs, all identical.
AudioSynthNoiseWhite's instance counter (a plugin global: which noise objects were constructed before) was reset to 0
before the algorithm was built in both, as the counter depends on the package's ALG 0, not on the algorithm.

**Render limits.** `cpu_permille` is at least twice the longest render call measured with 8 voices (9.5): a C call of
79464 cycles was seen once against the old 63200-cycle limit (1.8 x its 44872 worst), so the 128-sample packages (ring,
noise, walk, C) declare 400 (limit 122000 cycles, 1.5 x that spike); prime 100 (2 x 16068), phase and B 130 (2 x 20088),
A 100, atari 60 (2 x 8848); fibo, fm, pw keep 62 / 76. permille only sets the guard's limit (and what is reported).

**Why a slot is not playing.** The Rytm shows a non-READY slot as "!" + its 1A name (host data; the Rytm build is
unchanged), so the Teensy and the Pi send the reason as the name: `<TAG> <short name>`, at most 9 characters
(`rmp_why_name`, rmp_proto): `!ROOM NPPR` NO_ROOM, `!CRSH NPPR` muted after a fault, `!TIME NPPR` muted after a timeout
or three renders over budget, `!MISS NPPR` MISSING, `!BAD NPPR` BAD, `!SAFE NPPR` safe mode. A MUTED slot keeps why it
was muted (`plug_mute_why`); the reason is part of the slot's meta fingerprint, so a change reaches the Rytm. The web
page (`slot` objects) and `info` carry it in words as `why`: "no room: needs 23288 B, 584 B free", "muted: timed out",
"bad: init() refused", "missing: not installed..." (`rmp_loaded.need` is what the package asked the arena for). The Pi's
display shows the same `!ROOM NPPR`.

### 10.2 The mdkit machines (2026-10-26)

`plugins/mdkit` builds three Pi machines (RMPA) from mdkit (https://github.com/soejrd/mdkit, CC0 1.0, fetched at a
pinned commit by `fetch.sh`): MD-PI (`mdkit.pi`), MD-EFM (`mdkit.efm`), MD-TRX (`mdkit.trx`), one per family. P9 TYPE
(a list knob) picks the voice, P1..P8 are its controls 1:1, P10 LEV; the labels follow TYPE (`type_labels`, section
3). mdkit's 44.1 kHz is kept, a per-voice resampler makes 48 kHz. `make packages` writes `out/mdk_<fam>.rmp`; `make
check` runs them on the Mac against mdkit's own render (bit-exact before the resampler). `plugins/mdkit/README.md` has
the TYPE lists, the controls and what to expect (three TRX voices latch their controls at a trigger, as mdkit does).

**MD** (`mdkit.md`, `out/mdk.rmp`, 2026-10-27) is the three in one machine: P9 ENGINE (FM, PI, TRX) and P10 TYPE on the
SMPL page, P10's list following ENGINE (`type_labels` with `follows`), P1..P8 the voice's controls named after ENGINE +
TYPE, P11 LEV. One 568 KB image (the RMPA limit is 1 MB), format 4 (an older engine refuses it); only the voice set
up renders. A TYPE or ENGINE change that comes with its trig fades the old voice out under the new hit (2 ms).

**MD is also built in** (2026-10-29, in place of DIGIDRUM, soejrd/digidrum's TRX / EFM): voice byte 0x40, 0021's
machine 42, on every Pi and the Mac core with no plugin to install and no user slot taken (`core/md/md.h`; the Rytm
interface: docs/MD-BUILTIN.md). The same voices and the same code: `mdk.c` is compiled into the core
(core/engines/setup_engines.sh's `MDKit/mdk_builtin.c`), its labels come from `md.json` through `md_tables.h`
(`gen_tables.py`). mdkit itself is vendored in `third_party/mdkit` (no network). A kit with DIGIDRUM on a track plays it
as MD: DIGIDRUM's ENG picks the closest ENGINE / TYPE (EFM-BD..CY -> FM BD..CY, TRX-RS / CB / CL -> TRX RS / CB / CL),
its controls carried over where they are the same. The MD user machine and MD-PI / MD-EFM / MD-TRX stay installable
as before. Not on the Teensy (a PI voice's state is over its RAM): 0x40 is silent there.

## 11. User slots on the Rytm (sub-project 2)

The Rytm (rytm-mods 0021, machines 35..39 as U1..U5) shows each slot's machine, knobs and status from the Teensy: the
protocol 19..1E in `sim/protocol.py` and `rmp_proto.h` (version byte 1 after the command; unknown versions and commands
ignored on both sides; spec 2.1). Everything below is answered from `loop()` between requests (`plug_slots_pass`,
`plug_deadline_pass`, `plug_1d_pass`); the report path only parses and queues, and answers an "unchanged".

- **Descriptions.** The Teensy reads the manifest of a slot's installed package (formats 1..3; `rmp_manifest`, streamed
  from the store, one knob a read) after the slot loads, one slot a loop pass; a package that is not installed gets a
  name from its id (`befaco.np.atari` -> `ATARI`). The descriptions live in RAM2 (`plug_desc`, DMAMEM).
- **Generations.** `rmp_meta` fingerprints what 1A would say per slot every 10 ms (protocol state, flags, id, version,
  description, the preset list) and bumps the slot's generation (1..127) on a change; the installed list has its own,
  bumped by installs and removals. They start at boot from a seed (the i.MX RT's TRNG, else the cycle counter after
  the card mount; logged as `plugins: generations seeded from ...`), each 1..127, not at 1: a powered Rytm keeps its
  cache across a Teensy reboot. The odds that one still collides: a slot's new start equals the generation the Rytm
  holds for it about 1 time in 127, so with five slots and the store roughly 6/127 (~4.6 %) per reboot that something
  does - the Rytm then takes its cached 1A (or installed list) as current until that slot's next change. For assigns
  the Rytm side's per-track pre-assign generation is the mitigation. A 1E answered while its slot's description or
  preset scan is pending gets a new generation at once (`rmp_meta_bump`) and an invalidated fingerprint, so the
  settled slot always bumps again and pushes a 1D, even when it says what it said before. A change pushes a 1D (13
  bytes, no answer pair). The Rytm asks 19 what 0 for a slot whose generation it lacks: at the current generation the answer is the 10-byte "unchanged" (from the report path),
  else 1A part 0 (always 43 bytes: state, flags, names, the plugin fingerprint when READY, section 6) and the knob parts.
  1A flags: bit 0 page 2 is `params`, bit 1 a MISSING slot's package is on the card's `/MACHINES`, bit 2 presets exist.
- **The 1D a fresh Rytm needs.** A Rytm that starts while the Teensy runs has seen no 1D (0021's `us.seen` 0: no 1E from
  its slot menu, every slot's 1A asked again and again). So a 10 `<project>` and a 19 what 3 (the slot menu's
  installed-list ask) make a 1D owed, and any other 19 too when no 1D went out for 3 s; an owed 1D goes out at most once
  a second, from `plug_1d_pass` right after the 1E deadlines and ahead of the reload, the slot loads and the saves (a 10
  naming another project gets its 1D before that project's slots load); any 1D (a push, a 1E's answer) pays it. With
  none owed, `plug_1d_pass` still sends the 13-byte 1D when the last is 5 s old, so a lost 1D is caught up. The
  USB-host build also pushes a 1D when the Rytm's MIDI attaches. An extra 1D is harmless: the Rytm stores the
  generations and re-asks only the slots whose generation moved.
- **Asks** (19): what 1 the enum names of a knob (read from the package on demand, 1B list k, 8 names a message), what 2
  a page of 16 preset names (1B 0x20, section 13; a page above 7 is ignored), what 3 the installed machines (1B 0x21,
  built one package a pass when the store changed; **the first 32 packages in store order only** - a store with more
  lists 32, logs "the Rytm's list holds the first 32 of N", and the rest cannot be assigned from the Rytm; while a card
  copy holds the store and the list is stale, the last list built goes out at its own generation, so a 1E with it is
  refused NOT INSTALLED; names unique in their first 9 characters - what the Rytm keeps and matches - a
  duplicate listed as `V2 TEST~2`, `~3`, ..., and the slot playing it says the same name in its 1A).
- **Assign** (1E), run from `plug_deadline_pass`, ahead of the reload, load and save passes (the Rytm waits 2 s for the
  answer; a 10 `<project>` sent just before is reloaded first, so the 1E reads that project's map): op 0 assigns entry
  arg of the installed list (refused, NOT INSTALLED, when the Rytm's list generation is stale or the list is not built),
  op 1 clears, both through the slot map's save and the incremental load. The 1D answer pair follows when the slot has
  settled (READY, NOT INSTALLED, NO ROOM, BAD - mapped from what 1A says, so safe mode's MUTED answers BAD) - loaded, described, its preset folder scanned and its card lookup done,
  so the answer's generation for the slot is the one its next 1A carries (0021's `us_tick3` gives the tracks the new
  machine's defaults at that generation: an older one would hand them the previous machine's); BAD at once when the assign is refused or the map's save fails; at 1000 ms
  after the 1E (`kPlugAnsMs`, timed with `plug_ms`) a slot still loading, copying or saving answers LOADING (a clear:
  READY), a map that did not change and no save under way BAD. An answer for a slot not fingerprinted yet (its
  description or preset scan pending: a LOADING) carries a new generation for it (`rmp_meta_bump`), never the one the
  Rytm holds for the previous machine; the settled slot's 1A pushes another. A LOADING answer ends the 1E: a later
  failure shows only in the slot's 1A. A 1E on a slot whose card copy runs answers BAD at once (the copy's own answer stays pending).
- **Install from the card** (1E op 2): copies the slot's package from `/MACHINES/<id>-<a.b.c>.rmp` into the store, 2 KB
  a pass with the install's CRC check, the first chunk's header checked against the slot's id and version (else BAD).
  Only while the transport is stopped (else STOP TO INSTALL): MIDI Start/Continue without a Stop (the USB host build
  sees the Rytm's own; the serial build gets `'T'` frames from `sim/teensy_bridge.py`), or a trig within the last
  second. A copy under way pauses while the transport runs (its 1E then answers LOADING at the deadline). NOT ON CARD
  when the file is not there; READY when the package was installed meanwhile, or when a project change took the slot
  during the copy (the package is in). **An install's flash erases (~0.25-0.28 s each with interrupts off, section 4)
  stop everything: in the USB-host build the Overbridge output has a gap that long** - another reason for the transport
  rule.
- **Without plugins** (`-DRYTM_PLUGINS=0`) U1..U5 are silent and every slot is EMPTY at generation 1: a 19 what 0 gets
  the 43-byte part 0 with state EMPTY (or "unchanged" at generation 1), a 19 what 3 an empty installed list (1B 0x21:
  slot 0, generation 1, replace 1, total 0 - so the slot menu's YES sends a 1E clear), any 1E NOT INSTALLED, and a 10
  the 13-byte 1D (every generation 1, at most once a second) so the Rytm's `seen` is set and its 1A asks stop. A 19
  what 3 gets that 1D ahead of the empty list too (same limit), and `loop()` sends it every 5 s anyway, so a Rytm that
  started after the Teensy (and never sent a 10) still gets `seen`.
- Development frames for tests (section 4): `'W'` writes a file under `/PRESETS/` or `/MACHINES/` (`rmp_install.py put`),
  `'Z'` removes a package (`rmp_install.py remove`), `'T'` the transport.

## 12. Plugin API 2: native presets, voices, the pool

- `load_entry(state, bytes, len, values[16])` (`rmp_plugin.reserved[1]`): a native preset file's bytes (4 KB at most; a
  larger native file is refused); set the voice's hidden state and fill `values` for the preset-role knobs (-1
  untouched); 0 = taken. Called from `loop()` between render calls, through the guard (0.5 ms).
- `voice_enter(state)` (`reserved[2]`) after `voice_init` each time a voice starts the slot; `voice_leave(state)`
  (`reserved[3]`) before the voice plays another machine or slot, the slot reloads or the voice is released by a sound
  tag mismatch (section 6; not for a muted slot). In `voice_leave`, `pool_info` answers EMPTY and `pool_read` 0.
- `pool_info(slot, &info)` / `pool_read(slot, pos, buf, n)` (`rmp_host.reserved[0..1]`): the project's pool slot 1..127
  for the voice being called. **Without PSRAM** (and before a sample is resident) the voice reads through its own DSP
  SAMPLE buffers (`rmp_pool`): the first 8 KB with random access (4096 frames of 16-bit mono, 2048 of 16-bit stereo,
  1365 of 24-bit stereo), the rest streamed in order through the 16 KB ring, 8 KB a read between requests; a frame
  outside head and ring restarts the stream there (silence until refilled, ~0.75 ms a read); one sample a voice at a
  time; `pool_info` answers NOT_READY until the head is read (a job from `loop()`), EMPTY for a slot without a readable
  sample. `info.where` says `RMP_POOL_HEAD` and `head_frames`. **With PSRAM** and the sample resident: `RMP_POOL_PSRAM`,
  all of it (16-bit mono as the PSRAM pool holds it), pinned while the voice reads it - **untested: no PSRAM fitted**.
  `pool_read` delivers the first two stored channels interleaved, -1..1, and returns the frames it delivered: play
  silence for the rest. Call `pool_info` about once a block, not per sample (it runs from flash).
- Test plugin: `plugins/v2_test` (every API-2 feature; `sim/tests/test_rmp.py` V2Test, `sim/rmp_e2e.py` 7..11).

## 13. Presets

`/PRESETS/<id>/` on the card: generic presets `<name>.txt` with one `P<n> <value>` line per knob (others ignored; a file
is read up to 4 KB), native ones `<name>.<preset_ext>` (the manifest's), listed together in name order (127 at most, 13
characters shown) after the slot's description, 16 directory entries a loop pass. The PRE knob (kind `preset`) picks
entry `min(PRE, n - 1)`: a PRE turn (0x03) or the voice's first report loads it and writes the preset-role knobs back to
the Rytm (1C), holding them over the incoming reports until a report carries them all or 300 ms passed
(`RMP_PV_HOLD_MS`); `offset` knobs stay and apply on top, `free` knobs are never touched. A PRE p-locked on a trig plays
that preset on the voice without a write-back until the next trig (or a knob that moves). A trig waiting for its
preset holds its note (bounded, `plug_note_overdue`). Native: `load_entry`'s values are used the same way (ignored on a
p-lock's knobs only for the write-back). Memory (RAM2, DMAMEM): one shown-names list for all slots (1781 B: the slot
scanned last; 1B 0x20 is sent while it holds the asked slot's list, else the folder is scanned again), a folder index
per slot (388 B each), a preset state per voice (104 B), the knob roles per slot (48 B), and a 4 KB file buffer.

## 14. Status (2026-10-11)

Built, and tested on the Mac (section 8: the unit tests, the native harness, the C++ messages against `sim/protocol.py`)
and on a Teensy 4.1 without PSRAM in both builds (serial and USB host): everything in sections 11..13 and the store guard
of section 4. On the hardware: `sim/rmp_e2e.py` ALL PASS on
both builds (153 checks: steps 1..13), `sim/stress.py run u`, `c` and `np --np c` with 0 dropouts on both builds; the
'E' export's "kept" refusal (a `.tmp` put on the card by hand).

Not tested, or only inferred:
- **The real Rytm.** Everything so far is rytm-emu plus the Teensy (`rmp_e2e.py` plays the Rytm's side over the serial
  link; the host build's runs had no Rytm or Overbridge device attached, the `'X'` frames standing for its USB MIDI).
  The Rytm side (rytm-mods-us 0021) has its own tests, in rytm-emu.
- **Tag survival through a track sound's copy/paste** on the Rytm (spec 2.6's hardware checklist; inferred from a raw
  memcpy of the sound record).
- **The PSRAM paths** (`pool_info`'s RMP_POOL_PSRAM, a PSRAM state over 16 KB: `rmp_e2e.py` step 6 checks only the
  refusal without PSRAM).
- **The wipe's refusals** (while a slot loads, an install is open or the device-info report runs) and an export's
  failed rename: not run on hardware (a wrong path would wipe the store).

The Teensy's state after these tests: the user-slots serial build (merged into main 2026-10-11, so main's build is
the same store layout and guard), project PRESETS with U1 = `rytm.sine_test`, U2 = `rytm.fault_test`, all 8 packages
installed (befaco.np.a/atari/b/c, rytm.fault_test/sine_test/state_test/v2_test). Builds from before the merge (1 MB
store at 0x606C0000, no layout guard) must not be flashed on it: their store lies inside the 4 MB store (section 4).
