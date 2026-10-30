# mdkit machines: MD (and MD-PI, MD-EFM, MD-TRX)

User machines for the Pi Sidecar built from [mdkit](https://github.com/soejrd/mdkit) (Soejrd, CC0 1.0): standalone C99
drum voices modelled on the Machinedrum's EFM, TRX and PI families. The developer agreed to this use. Design:
`docs/superpowers/specs/2026-10-26-mdkit-machines-design.md`.

**MD** (2026-10-27, the user: "they could easily be 1 machine") is the three families in one: on the SMPL page, P9
ENGINE picks the family (FM, PI, TRX) and P10 TYPE the drum among that ENGINE's. MD-PI, MD-EFM and MD-TRX (one machine
a family, P9 TYPE) are still built for the Pis that have them.

| Machine | id | file | TYPEs | state a voice | image |
|---|---|---|---|---|---|
| MD | `mdkit.md` | `out/mdk.rmp` | ENGINE FM: as MD-EFM, PI: as MD-PI, TRX: as MD-TRX (30) | 25 968 B | 568 KB |
| MD-PI | `mdkit.pi` | `out/mdk_pi.rmp` | BD SD MT ML MA RS RC CC HH | 25 576 B | 396 KB |
| MD-EFM | `mdkit.efm` | `out/mdk_efm.rmp` | BD SD XT CP RS CB HH CY | 2 576 B | 60 KB |
| MD-TRX | `mdkit.trx` | `out/mdk_trx.rmp` | BD SD XT CP RS CB CH OH CY MA CL XC B2 | 2 192 B | 156 KB |

Pi only (RMPA): a PI voice's state is over the Teensy's 16 KB, and the packages are built and checked for the Pi.
Limits (an RMPA image at most 1 MB: tools/rmp_pack.py MAX_IMAGE, core/user/um.cpp kPkgMax; an upload at most 2 MB,
pi/engine/fsweb.cpp; 4 MB code and 4 MB data arenas, pi/engine/um_pi.cpp; a state at most 64 KB): MD's 568 KB image
fits, 44 KB less than the three apart (612 KB: one copy of the glue and the resampler table).

## MD's knobs

| Page | P | Knob | |
|---|---|---|---|
| SRC (SYN) | P1..P8 | the voice's own controls | named after ENGINE + TYPE (the tables below: MD-EFM's for FM, MD-PI's, MD-TRX's) |
| SMPL | P9 | ENGINE | list: FM PI TRX |
| SMPL | P10 | TYPE | list: the ENGINE's drums (FM 8, PI 9, TRX 13) |
| SMPL | P11 | LEV | output trim, default 100 |

- **TYPE follows ENGINE** (the manifest's `type_labels` with `follows` / `lists`, an RLB2 block, docs/plugins.md
  section 3): the host gives P10 the ENGINE's TYPE count and names in the slot's 1A / 1B, so the Rytm shows that list.
  MD is a format-4 package (format 3 with the RLB2 block): a Pi engine from before b6cfec3 refuses it ("format not
  supported") rather than show FM's 8 TYPE names for every ENGINE. Install it with an engine built from mdkit-one.
- **An ENGINE change keeps TYPE's value** and reads it in the new ENGINE's zones (the same place along the list: 0 is
  BD everywhere, 127 the last; what the Rytm shows and what plays always agree). It is a TYPE change like any other: a
  2 ms fade, then the new voice set up from a clean state (its state zeroed, `init`, P1..P8 given). Only the voice set
  up renders (one render call a block); the others cost nothing.
- **A change that comes with its trig** (a p-locked ENGINE or TYPE on a step): the trig renders the old voice's next
  2 ms first, fading out, and plays it under the new hit (no hard cut; the trig's call costs up to 8 extra render
  blocks: 13.7 us on the Mac at worst). MD-PI/EFM/TRX keep their old behaviour (a hard cut), byte for byte.

## Build, check, install

```
# mdkit: vendored at 96157fe in third_party/mdkit (third_party/mdkit/VENDORED.md); ./fetch.sh only checks it
make packages                  # the four, make a64 each (aarch64-none-elf-gcc, else the rytm-circle docker image) -> out/
make check                     # tests/mdk_check.cpp: the packages run on this arm64 Mac against mdkit's own voices
```

Install: upload `out/mdk.rmp` (or `out/mdk_pi.rmp`, `out/mdk_efm.rmp`, `out/mdk_trx.rmp`) into `/Plugins` on the Pi's web page
(http://sidecar.local, User machines' upload, or Files into `/Plugins`; the page rescans the machines itself), or copy
them into `/Plugins` on the card. Then assign them to user slots from the Rytm's slot menu (or the web page's U1..U5).

## The knobs (MD-PI, MD-EFM, MD-TRX; MD: the same, P9 ENGINE / P10 TYPE / P11 LEV)

- **SRC page, P1..P8:** the TYPE's own controls 1:1 (each 0..127 straight into mdkit's `set_control`). A TYPE with
  fewer than 8 leaves the rest unlabelled and inert.
- **SMPL page (params), P9 TYPE:** a list knob of the family's voices (zones of 128 / count). **P10 LEV:** output trim
  (0..127, default 100; the trig's velocity scales each hit). P11..P16 unused.
- **Labels follow TYPE** (the manifest's `type_labels`, docs/plugins.md section 3): a Sidecar with this release
  renames P1..P8 on the Rytm's machine page and the touchscreen to the TYPE's controls. An older Pi engine shows the
  static family-wide labels below for every TYPE (the knobs still do what the TYPE's own labels say).
- The knobs keep their values across a TYPE change: SD after BD plays with BD's knob positions, not SD's defaults
  (the defaults in the tables are mdkit's).
- Drums: `pitch: none` (the host sends note 60; tune with the TYPE's PTCH), mono (`max_voices` 1), a trig is the voice's
  trigger.

The tables: each TYPE's controls with mdkit's default.

### MD-PI (`mdkit.pi`, `out/mdk_pi.rmp`)

| TYPE | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 |
|---|---|---|---|---|---|---|---|---|
| BD | PTCH 64 | DEC 35 | HARD 100 | HAMR 0 | TENS 127 | DAMP 100 | - | - |
| SD | PTCH 64 | DEC 64 | HARD 10 | RING 40 | TENS 90 | RVOL 80 | RDEC 80 | - |
| MT | PTCH 20 | DEC 20 | HARD 127 | HAMR 127 | TUNE 127 | DAMP 80 | SIZE 90 | POS 64 |
| ML | PTCH 0 | DEC 64 | HARD 127 | TENS 0 | - | - | - | - |
| MA | GRNS 90 | DEC 0 | GLEN 72 | SIZE 96 | HARD 64 | - | - | - |
| RS | PTCH 90 | DEC 48 | HARD 64 | RING 64 | RVOL 64 | RDEC 64 | - | - |
| RC | PTCH 64 | DEC 70 | HARD 96 | RING 32 | AG 127 | AU 127 | BR 64 | GRAB 127 |
| CC | PTCH 64 | DEC 70 | HARD 90 | RING 32 | AG 127 | AU 127 | BR 64 | GRAB 127 |
| HH | PTCH 64 | DEC 70 | CLSN 100 | RING 30 | AG 127 | AU 127 | BR 60 | CLOS 127 |

Static labels (an older host): PTCH DEC HARD RING AG AU BR GRAB; defaults those of BD.

### MD-EFM (`mdkit.efm`, `out/mdk_efm.rmp`)

| TYPE | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 |
|---|---|---|---|---|---|---|---|---|
| BD | PTCH 0 | DEC 64 | RAMP 32 | RDEC 127 | MOD 35 | MFRQ 14 | MDEC 64 | MFB 24 |
| SD | PTCH 27 | DEC 60 | NOIS 14 | NDEC 79 | MOD 0 | MFRQ 5 | MDEC 127 | HPF 29 |
| XT | PTCH 25 | DEC 50 | RAMP 127 | RDEC 111 | MOD 5 | MFRQ 4 | MDEC 79 | CLIC 0 |
| CP | PTCH 50 | DEC 40 | CLPS 127 | CDEC 24 | MOD 115 | MFRQ 36 | MDEC 87 | HPF 8 |
| RS | PTCH 67 | DEC 10 | MOD 8 | HPF 15 | SNAR 38 | SPTC 25 | SDEC 59 | SMOD 0 |
| CB | PTCH 54 | DEC 61 | SNAP 92 | FB 56 | MOD 17 | MFRQ 112 | MDEC 42 | - |
| HH | PTCH 26 | DEC 54 | TREM 26 | TFRQ 47 | MOD 0 | MFRQ 86 | MDEC 93 | FB 75 |
| CY | PTCH 64 | DEC 72 | FB 96 | HPF 92 | MOD 45 | MFRQ 32 | MDEC 64 | - |

Static labels (an older host): PTCH DEC RAMP HPF MOD MFRQ MDEC CLIC; defaults those of BD.

### MD-TRX (`mdkit.trx`, `out/mdk_trx.rmp`)

| TYPE | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 |
|---|---|---|---|---|---|---|---|---|
| BD | PTCH 64 | DEC 64 | RAMP 0 | RDEC 0 | STRT 0 | NOIS 0 | HARM 0 | CLIP 0 |
| SD | PTCH 35 | DEC 7 | BUMP 0 | BENV 64 | SNAP 127 | TONE 0 | TUNE 100 | CLIP 90 |
| XT | PTCH 70 | DEC 25 | RAMP 50 | RDEC 30 | DAMP 64 | DIST 32 | DTYP 0 | - |
| CP | CLPY 50 | TONE 100 | HARD 7 | RICH 127 | RATE 40 | ROOM 64 | RSIZ 0 | RTUN 90 |
| RS | PTCH 50 | DEC 0 | DIST 50 | - | - | - | - | - |
| CB | PTCH 100 | DEC 50 | ENH 127 | DAMP 127 | TONE 0 | BUMP 0 | - | - |
| CH | GAP 0 | DEC 8 | HPF 0 | LPF 127 | MTAL 0 | - | - | - |
| OH | GAP 50 | DEC 60 | HPF 8 | LPF 127 | MTAL 0 | - | - | - |
| CY | RICH 127 | DEC 0 | TOP 0 | TTUN 127 | SIZE 0 | PEAK 127 | - | - |
| MA | ATT 32 | SUS 32 | REV 0 | DAMP 0 | RATL 64 | RTYP 64 | TONE 64 | HARD 64 |
| CL | PTCH 64 | DEC 32 | DUAL 64 | ENH 0 | TUNE 64 | CLIC 0 | - | - |
| XC | PTCH 64 | DEC 32 | RAMP 32 | RDEC 100 | DAMP 0 | DIST 0 | DTYP 0 | - |
| B2 | PTCH 60 | DEC 65 | RAMP 41 | HOLD 62 | TICK 91 | NOIS 44 | DIRT 56 | DIST 58 |

Static labels (an older host): PTCH DEC RAMP RDEC DAMP DIST DTYP CLIP; defaults those of BD.

Long names (12 characters, the Rytm's long label) come from `gen.py`'s table (PTCH Pitch, DEC Decay, MFRQ Mod Freq,
...); a few whose meaning mdkit's source does not make plain keep their letters (AG, AU, CLSN).

## How it works

- `mdk.c` (MIT) is the glue: one state per voice holds the union of the family's mdkit voice states. mdkit runs at its
  own 44 100 Hz, its arithmetic and timing untouched; render() makes the host's 48 kHz 12-frame blocks with a
  polyphase windowed-sinc resampler (147/160, 32 taps, 160 phases, `mdk_fir.h` from `gen.py`): flat to 16 kHz,
  -0.24 dB at 18 kHz, -4.6 dB at 20 kHz, images above 22 kHz at -22 .. -80 dB. Latency 16 core samples (0.36 ms).
- A TYPE change while the voice may ring fades the output out over 2 ms (96 samples); the next params() or trig sets
  the new voice up (its state zeroed, `init`, P1..P8 given); a trig during the fade sets it up at once. Returning to the
  playing TYPE during the fade fades it back in. Nothing is allocated (the package links no host symbols).
- **TRX-CP, TRX-MA, TRX-CL latch their controls at a trigger** from mdkit's control clock (mdkit's own player says
  "Parameter changes take effect on the next trigger"): a value set right before a trig - a p-lock, the first hit
  after a TYPE change or a project load - is heard from the trig after. The other voices take a control at once;
  several smooth it at mdkit's control rate. This is mdkit's behaviour, kept bit for bit.
- **The PI cymbals' memory** (`patch_pi.py`): mdkit gives PI-CC, PI-HH and PI-RC a 65 536-word delay memory (256 KB a
  voice, over the host's 64 KB), of which they use 1024 words (measured: every control at 0 and 127 and 300 random
  settings with changes mid-note touch offsets 22016..23039 only). The build keeps a 4096-word window around them; a
  read outside answers 0, a write outside is dropped. `make check` compares these three bit for bit with the
  unwindowed voices, and `tests/pi_window.c` fuzzes mdkit's own cymbals (random controls, pre-rolls, up to four trigs,
  changes mid-note) and checks their four delay pages never leave the window.
- The voices are compiled `-std=c99 -ffp-contract=fast -O3`: mdkit's own build (clang) fuses multiply-adds, gcc's
  ISO C would not, and EFM-BD then differs in the last bit; with it every voice is bit-exact (`make check`). -O3
  vectorises mdkit's init loops: a TYPE's set-up (the new voice's state zeroed, 8 bytes at a time, and its init) on the
  control core went from 19.5 to 5.1 us on the Mac (MD-PI's worst), at ~10 % more code.
- Threading: the host calls a voice's params / note_on / note_off (control core) and render (a render core) in
  lockstep, never at once, so the state has no locks.

## Checks (`make check`, tests/mdk_check.cpp; and sim/tests/test_core_mdkit.py on the core lib)

- each package verifies, places and init()s as the Pi's loader does (MAP_JIT on the arm64 Mac);
- every TYPE sounds on a trig (48 kHz, through the resampler);
- the core-rate output before the resampler is bit-exact against mdkit's own `render` (cc -std=c99 -O2, unwindowed):
  at the defaults, with each control at 0 and 127 over two hits, and with P2 moved mid-note;
- P1..P8 reach `set_control` 1:1; the Ps past a TYPE's control count are inert (bit-identical output);
- after the resampler: the error against a 512-tap reference of the same design is at least 50 dB below the signal
  under 16 kHz (measured 57 dB for the hats, 74..106 dB for the rest);
- a TYPE change mid-note fades to silence within 2 ms with no step larger than the signal's own, the next trig is
  bit-exact, nothing is written outside the state (guard bytes);
- on the core lib (`test_core_mdkit.py`): the three scan as fitting, assign, every TYPE plays through the Sidecar's
  voice, the slot's 1A names P1..P8 after the TYPE.
- the same against a core lib from before the labels extension (commit 7cf02eb, 2026-10-26): the three load, every
  TYPE plays, 1A shows the static labels (run by hand once; an older Pi engine behaves so).

CPU (this Mac, the RMPA code, a 12-frame block): MD-PI 0.3..1.4 us, MD-EFM 0.5..2.7 us (EFM-CY the most), MD-TRX
0.4..1.0 us per voice; a TYPE's set-up at most 5.1 us (MD-PI). The Pi's figure is the web page's measured `ns`.

## Credits

The voices: mdkit, Copyright (c) 2026 Soejrd, CC0 1.0 Universal (NOTICE). mdkit is independent of and not endorsed by
Elektron; Machinedrum and Elektron name the instrument and its maker. The glue: MIT, Copyright 2026 trigcondition and
contributors.
