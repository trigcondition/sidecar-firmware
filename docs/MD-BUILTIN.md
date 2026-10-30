# Built-in MD on voice byte 0x40: the Rytm interface (rytm-mods 0021, machine 42)

Status: host side implemented on rytm-teensy branch `nodigidrum` (core/md/, tests core/tests/test_md.cpp); a copy of this
document is docs/MD-BUILTIN.md. The mod side (0021) is to be built from
this document. Date 2026-10-29. All messages are `F0 7D 52 54 <cmd> ... F7`, every data byte 7-bit.

## 0. Summary for the mods agent

- Machine 42 (RS FM 2's slot, records 390..397 today, voice byte flag `0x40`) stops being DIGIDRUM and becomes **MD**:
  the combined mdkit drum machine. It has 3 ENGINEs (FM, PI, TRX), each with its own list of TYPEs, and 8 controls
  whose meaning and label depend on ENGINE + TYPE.
- The flag stays `0x40`. Every other machine keeps its number and flag.
- The Rytm must not hard-code MD's names. It asks the host for them with the user slots' ASK `0x19` and gets the user
  slots' `0x1A` / `0x1B` replies. MD has its own "slot" address range, `0x40..0x42` (section 3).
- **Rytm-side changes:**
  1. The page layout below (P1..P8 SYN, P9 ENGINE, P10 TYPE, P11 LEV on the SMPL page).
  2. Every 03 / 04 report for machine 42 carries the **MD layout tag `0x224D`** (section 2).
  3. Labels from 0x19 / 0x1A / 0x1B (section 3).
  4. No kept sample layer: `ext_layer_kept(0x40)` returns 0. The Rytm's sample layer is silenced as for every other
     EXT machine.
  5. Stored DIGIDRUM sounds are converted once (section 4).

### Status and decisions (2026-10-29)

- **Not in the 0.3.0 Rytm flash** (the user's decision): 0021 has 318 B of code space left (18,224 of 18,542 B in its
  caves), the MD page needs ~1.5-3 KB. Plan for the 0.3.1 flash: move 0021 into elekloader's DDR area
  (`--area-elek`), keeping its three fixed-address boxes readable (.usm 0x4001a23c read by 0026 / 0029, .ush
  0x4001a3dc read by 0026, the 'JUNO' mailbox in cave12 read by 0022) - split placement in build.py, or the readers
  through a pointer. Until then the Rytm shows DIGIDRUM's page; the host plays MD from P1 (section 4).
- **Settled with the mods side** (they refine section 4):
  1. A kit load converts at **the first UI tick after the load** (0021's load hook runs in the audio interrupt; sound
     words are written on the UI task only). A trig in that window reports tag 0, which the host plays through the
     DIGIDRUM mapping - no host change.
  2. Conversion and the fresh FM BD set-up run **only on a confirmed machine choice** (YES, the SOUND menu's MACHINE
     item), never while the machine list's cursor previews machine 42.
  3. **The converted marker is the sound's own tag word** (sound slot 47, already saved / loaded / copied by 0021):
     tag == 0x224D means converted. No spare byte; the report's tag is 0x224D exactly when the sound is converted.
  4. 0021's 1A / 1B handlers (us_meta_in, us_list_in) drop slot bytes above 5 today: MD's 0x40..0x42 need their own
     path (part of the code-space need).

## 1. The parameters (the sound's P1..P16, as 03 / 04 carry them)

Each P is the record's value 0..127 plus its fraction byte (`raw[2k]`, `raw[2k+1]`), as today. MD reads the integer
part only; the fraction is ignored.

| P | page | name | range / meaning | default | 0021 record (suggested) |
|---|---|---|---|---|---|
| P1..P8 | SRC (SYN) A..H | the TYPE's controls 1..8 | 0..127 each, straight into mdkit's `set_control(k, v)`. A TYPE has 3..8 controls; a control it lacks is inert, and its label comes back empty ("unused") | the TYPE's own default (section 3: the 1A knob's `default`) | the machine's 8 SYN records (today 390..397) |
| P9 | SMPL A | ENGINE | 3 zones, `e = (v * 3) >> 7`: 0..42 FM, 43..85 PI, 86..127 TRX | 0 (FM) | the first SMPL record |
| P10 | SMPL B | TYPE | zones of ENGINE e's TYPE count n_e (FM 8, PI 9, TRX 13): `t = (v * n_e) >> 7` | 0 (BD in every ENGINE) | the second SMPL record |
| P11 | SMPL C | LEV | 0..127, output trim (level = v / 127) | 100 | the third SMPL record |
| P12..P16 | SMPL D..H | unused | send 0; the host ignores them | 0 | - |

- **Where a list knob stops.** Use a list knob's zone lo for each name: `lo(z, n) = (z * 128 + n - 1) / n`
  (integer division). ENGINE: 0, 43, 86. FM TYPEs: 0, 16, 32, ... 112. PI: 0, 15, 29, 43, 57, 72, 86, 100, 114.
  TRX: 0, 10, 20, 30, 40, 50, 60, 69, 79, 89, 99, 109, 119.
- **TYPE keeps its raw value across an ENGINE change** and is read in the new ENGINE's zones (the plugin's rule, so
  host and Rytm agree). The Rytm may instead reset TYPE to 0 on an ENGINE change. Both are fine, because the host
  only reads the value.
- **The velocity** of a 04 is the hit's level: MD's gain = LEV/127 x vel/127. The note is ignored ("pitch none"): PTCH
  is P1.
- **0x11 NOTE_OFF** is ignored (drums: the voice's own decay).
- **A TYPE or ENGINE change** fades the old voice out over 2 ms and sets the new one up. When the change comes with
  its trig (a p-lock), the old voice's tail plays under the new hit. All of this is host side; the Rytm only reports.

## 2. Telling the MD page from the old DIGIDRUM page: the sound tag

The 43-byte 04 and the 41-byte 03 end with the 14-bit sound tag, `<tag hi7> <tag lo7>` just before F7 (spec 2.6, as
the user slots). For machine 42 the tag is **not** the sound's stored tag:

- **The new 0021** sends `hi7 = 0x44`, `lo7 = 0x4D` ("DM": `tag = 0x44 << 7 | 0x4D = 0x224D`) in every 03 / 04 for
  machine 42 whose sound is converted (section 4: in practice every one, since conversion runs at load). This is a constant the mod writes, not a stored value.
- **The host:** machine 0x40 with tag `0x224D` is read in the layout of section 1. Anything else (tag 0, another tag,
  or a report without a tag: a 41-byte 04 or a 39-byte 03) is read in the **legacy DIGIDRUM layout** (section 4). An old 0021 and an
  old kit therefore keep playing sensibly before the flash.
- **Byte example**, the new 04: voice 4 (track LT), note 60, velocity 100, FM BD with all eight controls 64, ENGINE 0,
  TYPE 0, LEV 100:

  ```
  F0 7D 52 54 04 44 3C 64  40 00 40 00 40 00 40 00 40 00 40 00 40 00 40 00  00 00 00 00 64 00 00 00 00 00 00 00 00 00 00 00  44 4D F7
  ```

  The voice byte is `0x40 | 4`. Then P1..P8, then P9 0, P10 0, P11 100, P12..P16 0, then the tag. 43 bytes.

## 3. Names and labels: ASK 0x19, METADATA 0x1A, LIST 0x1B with MD's "slot" 0x40 + ENGINE

These are the user slots' messages (sim/protocol.py, teensy/libraries/RytmPlugins/src/rmp_proto.h), version byte 1,
with one change: the slot byte is **`0x40 + e`** (e = P9's zone 0..2; ENGINE's count 3 is fixed and part of this spec).
The answer depends only on (ENGINE, TYPE), never on host state, so the Rytm may cache it.

### Generations

Each label set has a generation `g = 1 + set + 32 * T`:
- `set` = MD's TYPE index over all ENGINEs: FM 0..7, PI 8..16, TRX 17..29 (`set = base[e] + t`, with
  base = 0, 8, 17).
- `T` = the host's table version 0..3. It is 0 now and moves only when the tables change (a new mdkit commit or new
  label words).

So g runs 1..126 and never 0 ("the Rytm knows nothing"). The Rytm keeps the g it last received for the page. A
**different g means a different set**: the Rytm needs no table of its own to tell when the TYPE zone changed.

### ASK (Rytm -> host), 11 bytes

```
F0 7D 52 54 19 01 <slot 0x40+e> <gen> <what> <arg> F7
```

- `what 0` (METADATA), `arg` = P10's integer part 0..127 (the TYPE value; the host computes t).
  - The host answers `1A ... 7F` ("unchanged", 10 bytes) when `gen` equals that set's g.
  - Otherwise it sends the whole 1A, with the head, then parts 1..nparts-1.
  - When to send it: when the MD page opens, when P9 or P10 changes (rate-limit to one ask per ~50 ms while a knob
    turns), and when the track's sound changes (kit load, track select).
- `what 1` (ENUM NAMES), `arg` = the knob index 0..15 (P = arg + 1). The host answers a 1B for that knob, see below.
  - `arg 8` gives the ENGINE names (FM PI TRX).
  - `arg 9` gives ENGINE e's TYPE names (the slot byte's e).
  - Other knobs have no names: no answer.
- The host ignores `what 2` and `what 3` for these slots.

### METADATA (host -> Rytm), the head (43 bytes)

```
F0 7D 52 54 1A 01 <slot 0x40+e> <g> 00 <state 01> <flags 01> <nknobs 10> <nparts 05>
  <name 10: "MD"> <short 4: "MD"> <p2 title 13: "MD ENGINE"> <fp hi 00> <fp lo 00> F7
```

- state is always 1 READY.
- **fp is 0.** A built-in has no plugin fingerprint. On slots 0x40..0x42 the mod must not run the user slots'
  fingerprint logic: no compare of a sound's stored tag with the 1A's fp (no `!NAME` for MD), and never a write of the
  1A's fp into a sound. The MD tag 0x224D (section 2) is a constant the mod puts into machine 42's reports, and only
  for a converted sound (section 4). It is never taken from a 1A.
- flags bit 0 is set: page 2 holds knobs (P9..P16), and there is no sample layer.
- nknobs is 16 (P1..P16, every P present: the unused ones as empty labels), so nparts = 1 + 16/4 = 5.
- The text fields are NUL-padded ASCII.

### METADATA, parts 1..4 (each `F0 7D 52 54 1A 01 <slot> <g> <part> {24 bytes} x 4 F7`, 106 bytes)

Each knob entry is `<p> <look> <kind> <role> <min> <max> <default> <enum count> <long 12> <short 4>` (look 0 dial 1 centred
2 fader 3 list; kind 0 number 1 bipolar 2 enum 3 list - the user slots' values, rmp_format.h):

| p | look | kind | role | min | max | default | enum count | long / short |
|---|---|---|---|---|---|---|---|---|
| 1..8 (a control the TYPE has) | 0 dial | 0 number | 0 | 0 | 127 | the TYPE's default | 0 | the TYPE's label, e.g. "Pitch" / "PTCH" |
| 1..8 (a control it lacks) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | "" / "" (unused: draw greyed) |
| 9 | 3 list | 2 enum | 0 | 0 | 127 | 0 | 3 | "Engine" / "ENG" |
| 10 | 3 list | 2 enum | 0 | 0 | 127 | 0 | n_e (8, 9 or 13) | "Type" / "TYPE" |
| 11 | 2 fader | 0 number | 0 | 0 | 127 | 100 | 0 | "Level" / "LEV" |
| 12..16 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | "" / "" (unused) |

Part k (1..4) holds P(4k-3)..P(4k).

### LIST NAMES (host -> Rytm)

```
F0 7D 52 54 1B 01 <slot 0x40+e> <list> <g> <replace 1> <total> {<index> <13 name bytes>} x <= 8 F7
```

- `list` = the asked knob index: 8 for ENGINE, 9 for TYPE.
- `g` echoes the ASK's `gen`. The Rytm asks with the generation of the 1A it holds for the page, so the 1B passes a
  per-slot check against that 1A's generation. When the ASK's `gen` is 0 (the Rytm knows nothing yet), `g` is the
  generation of `set = base[e]` (the ENGINE's first TYPE). The names depend only on e, so any generation of that
  ENGINE is correct for them.
- `total` is 3 for ENGINE, n_e for TYPE. The 8-a-message rule and replace / total work as in 16/17 / user slots: a
  list over 8 names comes in two messages, the second with replace 0.
- Names are at most 7 characters, NUL-padded to 13.
- ENGINE: `FM` `PI` `TRX`.
- TYPE (the same lists as the MD user machine, `plugins/mdkit/md.json`):
  - FM: BD SD XT CP RS CB HH CY
  - PI: BD SD MT ML MA RS RC CC HH
  - TRX: BD SD XT CP RS CB CH OH CY MA CL XC B2

### The label words (for reference; the host is the source, from plugins/mdkit/md.json via plugins/mdkit/md_tables.h)

The host sends these. The mod must not copy them. The full table is `plugins/mdkit/md_tables.h`; for example FM BD is
`Pitch PTCH, Decay DEC, Ramp RAMP, Ramp Decay RDEC, Mod MOD, Mod Freq MFRQ, Mod Decay MDEC, Mod Feedback MFB`.

## 4. Old DIGIDRUM sounds (the migration)

- **The host (both firmware generations):** for a 0x40 report without the MD tag, it reads DIGIDRUM's layout:
  - P1 is DIGIDRUM's ENG, 11 zones `z = (v * 11) >> 7`.
  - P2..P8 are DIGIDRUM's controls 1..7.
  - P9..P16 are ignored (the old Rytm sample page).
- **The mapping**, used by the host and to be used by the mod:

| DIGIDRUM zone z (P1 lo) | DIGIDRUM engine | MD ENGINE (P9) | MD TYPE (P10) | set | P2..P8 kept as MD P1..Pk | the rest |
|---|---|---|---|---|---|---|
| 0 (0) | EFM-BD | FM (0) | BD (0) | 0 | k = 7 | P8 MD default |
| 1 (12) | EFM-SD | FM | SD (16) | 1 | 7 | default |
| 2 (24) | EFM-XT | FM | XT (32) | 2 | 7 | default |
| 3 (35) | EFM-CP | FM | CP (48) | 3 | 7 | default |
| 4 (47) | EFM-RS | FM | RS (64) | 4 | 7 | default |
| 5 (59) | EFM-CB | FM | CB (80) | 5 | 7 (the TYPE has 7) | - |
| 6 (70) | EFM-HH | FM | HH (96) | 6 | 7 | default |
| 7 (82) | EFM-CY | FM | CY (112) | 7 | 7 (the TYPE has 7) | - |
| 8 (94) | TRX-RS | TRX (86) | RS (40) | 21 | 3 | - |
| 9 (105) | TRX-CB | TRX (86) | CB (50) | 22 | 3 (PTCH DEC ENH) | DAMP TONE BUMP at MD defaults |
| 10 (117) | TRX-CL | TRX (86) | CL (99) | 27 | 6 | - |

  - P11 LEV = 100 and P12..P16 = 0.
  - "Kept" means DIGIDRUM's control names match MD's for that TYPE, by name (DIGIDRUM's labels against mdkit's
    control names). For TRX-CB, DIGIDRUM had PTCH DEC ENH TONE BUMP and mdkit has PTCH DEC ENH DAMP TONE BUMP, so only
    the first three are kept.
  - Values pass 1:1 (0..127). Whether they sound the same was not checked by ear.
- **The new 0021: converting a stored DIGIDRUM sound.** The mod does this on its own, with no Sidecar needed: the
  defaults are the table below, compiled into the mod.
  - **What marks a sound converted:** a flag bit in a spare byte of the sound record (the mod's choice of byte). The
    bit is set by the conversion and by a fresh MD set-up, and never otherwise.
  - **When to convert:** whenever a sound becomes the sound of a track whose machine is 42 and its flag is clear.
    That is:
    1. at a kit load (every track on machine 42, all at once, before the first 03 / 04 of that kit);
    2. when a sound is loaded or pasted into a track on machine 42 (sound pool, copy / paste, reload);
    3. when the track's machine is switched to 42 on the SRC list. Here the record holds another machine's values:
       set it up fresh as FM BD (P1..P8 = set 0's defaults, P9 = 0, P10 = 0, P11 = 100, P12..P16 = 0), then set the flag.
    Nothing is converted at a trig or a knob turn.
  - **How (cases 1 and 2):**
    - z = (P1 * 11) >> 7, and from the mapping table: set, ENGINE value, TYPE value and keep k.
    - New P1..Pk = old P2..P(k+1). New P(k+1)..P8 = the set's defaults below.
    - P9 = the ENGINE value, P10 = the TYPE value, P11 = 100, P12..P16 = 0, all fraction bytes 0.
    - Set the flag.
  - **The tag:** a sound with the flag set is reported with the MD tag (section 2). A sound without it (an old Rytm,
    or a conversion not yet run) is reported with tag 0, and the host then reads DIGIDRUM's layout and plays it by the
    mapping.
- **MD's defaults per set** (generated by plugins/mdkit/gen_tables.py from voices.json, mdkit's own defaults, so this
  table and the host's 1A `default` column always agree; "-" is a control the TYPE lacks: write 0, it is inert):

<!-- md-defaults begin (plugins/mdkit/gen_tables.py) -->
| set | ENGINE | TYPE | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | FM | BD | 0 | 64 | 32 | 127 | 35 | 14 | 64 | 24 |
| 1 | FM | SD | 27 | 60 | 14 | 79 | 0 | 5 | 127 | 29 |
| 2 | FM | XT | 25 | 50 | 127 | 111 | 5 | 4 | 79 | 0 |
| 3 | FM | CP | 50 | 40 | 127 | 24 | 115 | 36 | 87 | 8 |
| 4 | FM | RS | 67 | 10 | 8 | 15 | 38 | 25 | 59 | 0 |
| 5 | FM | CB | 54 | 61 | 92 | 56 | 17 | 112 | 42 | - |
| 6 | FM | HH | 26 | 54 | 26 | 47 | 0 | 86 | 93 | 75 |
| 7 | FM | CY | 64 | 72 | 96 | 92 | 45 | 32 | 64 | - |
| 8 | PI | BD | 64 | 35 | 100 | 0 | 127 | 100 | - | - |
| 9 | PI | SD | 64 | 64 | 10 | 40 | 90 | 80 | 80 | - |
| 10 | PI | MT | 20 | 20 | 127 | 127 | 127 | 80 | 90 | 64 |
| 11 | PI | ML | 0 | 64 | 127 | 0 | - | - | - | - |
| 12 | PI | MA | 90 | 0 | 72 | 96 | 64 | - | - | - |
| 13 | PI | RS | 90 | 48 | 64 | 64 | 64 | 64 | - | - |
| 14 | PI | RC | 64 | 70 | 96 | 32 | 127 | 127 | 64 | 127 |
| 15 | PI | CC | 64 | 70 | 90 | 32 | 127 | 127 | 64 | 127 |
| 16 | PI | HH | 64 | 70 | 100 | 30 | 127 | 127 | 60 | 127 |
| 17 | TRX | BD | 64 | 64 | 0 | 0 | 0 | 0 | 0 | 0 |
| 18 | TRX | SD | 35 | 7 | 0 | 64 | 127 | 0 | 100 | 90 |
| 19 | TRX | XT | 70 | 25 | 50 | 30 | 64 | 32 | 0 | - |
| 20 | TRX | CP | 50 | 100 | 7 | 127 | 40 | 64 | 0 | 90 |
| 21 | TRX | RS | 50 | 0 | 50 | - | - | - | - | - |
| 22 | TRX | CB | 100 | 50 | 127 | 127 | 0 | 0 | - | - |
| 23 | TRX | CH | 0 | 8 | 0 | 127 | 0 | - | - | - |
| 24 | TRX | OH | 50 | 60 | 8 | 127 | 0 | - | - | - |
| 25 | TRX | CY | 127 | 0 | 0 | 127 | 0 | 127 | - | - |
| 26 | TRX | MA | 32 | 32 | 0 | 0 | 64 | 64 | 64 | 64 |
| 27 | TRX | CL | 64 | 32 | 64 | 0 | 64 | 0 | - | - |
| 28 | TRX | XC | 64 | 32 | 32 | 100 | 0 | 0 | 0 | - |
| 29 | TRX | B2 | 60 | 65 | 41 | 62 | 91 | 44 | 56 | 58 |
<!-- md-defaults end -->

## 5. Other host behaviour the Rytm can rely on

- The Sidecar treats 0x40 as a Sidecar machine. The SEQ page's make prompt names it "MD, 1 NOTE": MD is mono, one
  note a voice, and the sequencer's notes trigger it.
- A trig on machine 42 needs no answer, and the host sends nothing unasked for MD. There is no 1D generation for
  MD: the generations above are static.
- 0x15 / 0x16 / 0x17 (JUNO / FM names) are not used for MD.
- The Teensy host has no built-in MD (a PI voice's state does not fit). There, a 0x40 voice stays silent and the
  host does not answer slots 0x40..0x42.
