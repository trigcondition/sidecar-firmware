"""Draft wire protocol between patched Rytm firmware and the Teensy (v0, 2026-10-07).

Everything is SysEx with the non-commercial manufacturer ID 0x7D, which the stock
OS discards (docs/measure-emulator.md), so a patched Rytm can claim it:

    F0 7D 52 54 <cmd> ... F7          ("RT")

Teensy -> Rytm, cmd 0x01 AUDIO: one 32-sample block (one Rytm audio block,
0.667 ms at 48 kHz) for one voice, samples as 14-bit two's complement split into
two 7-bit bytes (high first):

    F0 7D 52 54 01 <voice 0..7> <seq 0..127> <32 x (hi, lo)> F7     = 72 bytes

Rytm -> Teensy, sent by the EXT machine's engine (rytm-mods 0021, ext_render;
implemented 2026-10-08, P1..P8 only):

  cmd 0x04 TRIG+PARAMS: a trig, with the parameters it plays with, so p-locks
  land on their step:

    F0 7D 52 54 04 <voice 0..7> <note> <velocity> <P1..P16> F7      = 41 bytes

  cmd 0x03 PARAMS: P1..P16 changed (knob, LFO, slide); at most once every 16
  audio blocks (about 90/s) per voice:

    F0 7D 52 54 03 <voice 0..7> <P1..P16> F7                        = 39 bytes

  The sound tag (2026-10-10, rytm-mods 0021 sound-tag-research.md): a Rytm with
  the tag appends two bytes before F7 to both, <tag hi> <tag lo> = the owner
  track's sound tag (tag >> 7, tag & 0x7F; 0x04 43 bytes, 0x03 41), for every
  machine it reports. For the user slots (0x50..0x70) it is the fingerprint
  (plugin_fp) of the plugin the sound was made for, 0 untagged. A host plays a
  user-slot voice only when the tag is 0 or the fp of the plugin it runs in that
  slot; otherwise the voice stays silent until a trig whose tag matches (or is
  0). Shorter reports (older firmware) carry no tag: 0. Where the Rytm keeps it:
  sound index 0 until 2026-10-16; since then (rytm-mods sendfx-report, M1b) the
  sound's stored slot 47, so index 0 is SEND FX 1/2 on every track (0x20 below).
  The wire format is unchanged.

  cmd 0x11 NOTE_OFF: the voice's gate closed - pad released, the sequencer
  trig's LEN elapsed, or the sequencer stopped (rytm-mods 0021-ext,
  2026-10-09; older firmware never sends it):

    F0 7D 52 54 11 <voice 0..7> F7                                  = 7 bytes

  The voice byte is the plain physical voice, no machine flags. It only
  follows a trig on that voice, never between a trig and its own retrig; a
  new trig (0x04) always reopens the voice. JUNO's LEN 127 holds until it.

  Voice (0x04, 0x03) is the physical voice (rytm-mods machines.md) in its
  low 3 bits; the machine is (voice byte & 0x78), matched exactly: 0x00
  PLAITS, 0x08 JUNO, 0x10 DAISYSP, 0x20 DSP SAMPLE, 0x40 DIGIDRUM, 0x48 6-OP
  FM (2026-10-09), 0x50 0x58 0x60 0x68 0x70 the user slots U1..U5 (2026-10-10:
  machine plugins, docs/plugins.md - the project's /.rytm/slots/<PROJECT>.txt
  names the machine each slot plays; P1..P16 go to the plugin's params(), the
  trig's note and velocity to its note_on(), 0x11 to its note_off(); a host
  without plugins plays them as PLAITS); any other value is taken as PLAITS. In detail: 0x10 when the
  track plays DAISYSP (machine 51), 0x20 for DSP SAMPLE (machine 45: P1..P8 ENG
  STR BRI DMP MIX FB TUN LEV; P9..P16 its SD SAMPLE page by word: 17 TUN 18 FIN
  19 SMP 20 FLD 21 STA 22 END 23 LOP 24 LEV, a file on the host's card ringing
  the resonator), 0x40 for MD (since 2026-10-29, the built-in mdkit drums, core/md/md.h; the interface:
  docs/MD-BUILTIN.md): with the sound tag MD_LAYOUT_TAG (0x224D) P1..P8 the TYPE's
  controls, P9 ENGINE (FM PI TRX), P10 TYPE, P11 LEV; without it DIGIDRUM's old page
  (soejrd/digidrum's TRX/EFM machines until 2026-10-29: P1 ENG in 11 zones, P2..P8 its
  controls, P9..P16 the Rytm's sample page), which the host maps onto MD), 0x08 for JUNO (a Juno-106 voice,
  sim/juno/juno_voice.h: P1..P8 PAT CUT RES ENV ATK DEC REL LEN - PAT the
  factory patch 0..127, CUT..REL the patch's VCF FREQ, RES, ENV, A, D, R
  sliders as absolute values 0..127 (2026-10-09; before: offsets, 64 = the
  patch) - 15 puts a patch's own on the knobs -, LEN the gate (127: until the
  note-off 0x11, or the next trig); the note the trig's; when
  the project has a JUNO list - Juno-106 patch dumps from the card's /JUNO,
  added with LOAD TO PROJ - PAT steps through it instead of the factory
  patches: slot min(PAT, total - 1)), 0x48 for 6-OP FM (machine 40,
  2026-10-09: Plaits' six-op FM engine playing DX7 voices, sim/plaits/
  fm_machine.h: P1..P8 PAT TIM MOR DEC LPG TUN LEV LEN, P9..P12 unused,
  P13..P16 CHD INV SPR VOI on the Pi (core/poly.h, chord_expand below) - PAT
  the voice, entry min(PAT, total - 1) of the effective FM list (below),
  taken at a trig (a p-locked PAT too) or when the voice becomes FM: a 0x03's
  PAT waits for the next trig, as loading a voice cuts the sounding note; TIM
  Plaits' TIMBRE and MOR its MORPH, offsets on the voice as stored (64 = as
  stored); DEC and LPG the decay and colour of Plaits' low-pass gate after the
  voice, pinged by each trig (DEC 127: no LPG, the voice's own envelopes
  alone, so held notes sustain); TUN semitones from 64 (as PLAITS); LEV the
  output level; LEN the gate as JUNO's (127: until the note-off 0x11 or the
  next trig; lower a timed gate, 20 ms * 2^(LEN/14), or the note-off,
  whichever first); the note the trig's, the velocity the DX7's; no knob sync
  for FM, nothing like 15); else PLAITS, machine 44 (ENG in 17 zones:
  PLAITS_MODELS, Plaits' models without its 6-OP FMs, NOISE and drums).
  PLAITS: P1..P8
  ENG HRM TIM MOR DEC LPG TUN LEV, P9..P16 its page 2 (FM FIN AUX TMD MMD VEL
  GLD VOL). DAISYSP: P1..P8 ENG STR BRI DMP INP EXC TUN LEV (INP: INT, NAT or
  NOISE - played by the Rytm on the voice itself - or another analog voice's
  Overbridge output, channels 2..9, exciting it), P9..P16 its EXCITER page by
  sample word 17..24: XTN XDC FB OUT XP4 XLV XP5 WID (OUT >= 64: resonance to
  MAIN, else to the voice with FB of the loop kept). [Older: P9..P16 were then
  the sound's own sample page). ENG spans 0..127 in equal model zones,
  eng_zone(value, N) = min((value * N) >> 7, N - 1) on the value's integer
  part (2026-10-09, curated: N = 17 PLAITS, 11 DIGIDRUM, 3 DAISYSP and DSP
  SAMPLE - below; before: 21, 16 and 4). Each P is 14-bit, two
  bytes: the value 0..127, then its fraction in 1/128 steps (the stored
  parameter's low byte; slides and the LFO move it). LFO modulation included.

DSP SAMPLE's SD SAMPLE page picks from the project's pool (rytm-mods
0021/0022/0023, 2026-10-09): SMP (word 19, the value 0..127) is pool slot SMP -
0 OFF (no sample, silent), N slot N; an empty slot, or N beyond the pool, is
silent. FLD (word 20) is not used - except by a host with PSRAM (docs/
psram.md, 2026-10-09): there it is MOD, DSP SAMPLE's playback mode in 7
zones, (v * 7) >> 7: ONE LOOP SLICE GRAIN STRCH PLUCK WAVE (ONE = the
streamed voice's behaviour; STA END LOP mean per mode what psram.md's table
says).

Retired: the folder walk (2026-10-08) - 09 <list> <track> (names), 0A (the
host's name lists), 0B / 0C <track> 00 (YES into a subfolder, NO up). Hosts
still accept 09/0B/0C from older firmware and ignore them; 0A is not sent.

The TEENSY folder in the Rytm's own file browser (rytm-mods 0023, 2026-10-08):

    F0 7D 52 54 0D <folder hi> <lo> F7         Rytm -> host: list this folder
                                               (0 = the card's /Samples)
    F0 7D 52 54 0E <folder hi lo> <parent hi lo> <first hi lo> <total hi lo>
       { <kind> <id hi lo> <size: 4 x 7 bits, high first> <name len> <name> } F7
                                               host -> Rytm, 6 entries a message:
                                               kind 1 a subfolder (id: its folder
                                               number), 0 a file (id: position);
                                               names up to 23 chars, no .wav
    F0 7D 52 54 0F <folder hi lo> <file hi lo> F7
                                               Rytm -> host: LOAD TO PROJ on a
                                               TEENSY file - add it to the pool's
                                               first empty slot (or a new one at
                                               the end), unless it is there
                                               already; then 14
    F0 7D 52 54 10 <name> F7                   Rytm -> host: the Rytm's project
                                               (its name, up to 23 chars); the
                                               pool is the project's own
                                               (/.rytm/pools/<name>.txt: slot N is
                                               line N-1, a path under /Samples,
                                               an empty line an empty slot).
                                               Sent before each 0F / 12 and every
                                               few seconds; 14 follows the first
                                               10 and a changed project, not a
                                               repeat
    F0 7D 52 54 12 <slot> <folder hi lo> <file hi lo> F7
                                               Rytm -> host: ASSIGN - pool slot
                                               1..127 = that TEENSY file (folder
                                               and file as in 0D/0E/0F), whatever
                                               it held, even if the file is in
                                               another slot too; the pool file
                                               grows with empty lines as needed.
                                               A /JUNO patch is ignored. Then 14
    F0 7D 52 54 13 F7                          Rytm -> host: ASK - send 14 and
                                               16 now
    F0 7D 52 54 14 <replace 1/0> <count>
       { <slot> <folder hi lo> <file hi lo> <13 name bytes> } F7
                                               host -> Rytm: the pool table,
                                               non-empty slots in slot order, 6
                                               a message; replace 1 on a push's
                                               first message (the Rytm clears its
                                               table), 0 on the rest; an empty
                                               pool is one message, 1, count 0.
                                               Name: the file name without
                                               extension, ASCII 0x20..0x7e (else
                                               '_'), cut to 13, NUL padded.
                                               Folder/file 0x3fff (7f 7f) each:
                                               the file is not on the card now

JUNO (rytm-mods 0021, 2026-10-09), host -> Rytm:

    F0 7D 52 54 15 <voice 0..7> <CUT> <RES> <ENV> <ATK> <DEC> <REL> F7
        KNOBS: a JUNO voice's patch changed - its PAT (a 0x03 report) picks
        another patch, the first report or trig that makes the voice JUNO, or
        the JUNO list entry PAT points at changed; not on a 0x04 trig whose
        p-locked PAT differs. The values (0..127) are the patch's own VCF FREQ,
        RES, ENV, A, D, R (bytes 5, 6, 7, 11, 12, 14 of its 18): the Rytm
        writes them into the track's sound, so its knobs show the patch. Until
        a report carries all six as sent (or ~300 ms passed) the host ignores
        the six knobs' incoming values and plays the patch's own.
    F0 7D 52 54 16 <replace 1/0> <total> { <slot> <13 name bytes> } F7
        NAMES: the project's JUNO list, 8 entries a message; total = its
        slots (empty ones included; 0: no list - the Rytm shows the factory
        names A11..B88), entries every slot 0..total-1 in order; replace 1 on
        a push's first message (the Rytm clears its list), 0 on the rest;
        total 0 is one message, 1, 0, no entries. Name: the patch's, ASCII
        0x20..0x7e (else '_'), cut to 13, NUL padded; an empty slot 13 NULs.
        Sent with every 14 after a new project / the first 10, on 13, and
        after a 0F that added a JUNO patch to the list. The list holds at
        most 127 slots (PAT 0..126; 127 plays the last).

6-OP FM (rytm-mods 0021 machine 40, 2026-10-09):

    F0 7D 52 54 17 <replace 1/0> <total> { <slot> <13 name bytes> } F7
        host -> Rytm, FM NAMES: the effective FM list's names, layout and
        rules as 16 (8 entries a message, every slot 0..total-1, replace 1 on
        a push's first message, an empty slot 13 NULs; total <= 127). Sent
        after a new project / the first 10 (with 14 and 16), after a 0F that
        added a DX7 voice to the FM list, and on 18.
    F0 7D 52 54 18 F7
        Rytm -> host, FM ASK: send 17 now (the Rytm's PAT text asks when its
        name cache, shared between JUNO and FM, holds the other list).

The effective FM list: the project's FM list (/.rytm/fm/<PROJECT>.txt, a
line per slot: the bank's MD5 as 32 hex digits, a tab, the voice 0..31, a
tab, its name; an empty line an empty slot, silent) when it has any slot,
else Plaits' 96 ROM voices (its three DX7 banks, in order). The MD5 is the
bank's 4096 voice bytes' (without a .syx's header and checksum), so a moved or
renamed bank is found again; Plaits' ROM banks count as banks too. The card's
/DX7 holds packed 32-voice DX7 banks (4104-byte .syx, F0 43 0n 09 20 00 +
4096 bytes + checksum + F7, or a bare 4096-byte file), indexed at boot (an
MD5 and the 32 names per bank, cached in /.rytm/dx7.bin behind a hash of
/DX7's listing). In TEENSY a folder DX7 follows JUNO: its subfolders mirror
/DX7's, and each bank is a folder of its 32 voices (kind 0, size 128, named
by the voice's 10-character name); 0F on a voice adds it to the project's FM
list (its first empty slot or a new one; not again if it is there), then 17.
0F on anything else under DX7, and 12 on any JUNO or DX7 entry, is ignored.
TEENSY folder numbers: /Samples' first, then /JUNO's (folders and banks),
then /DX7's (folders and banks, breadth first in the card's order).

Folders from cards.folders() on are /JUNO's (core/presets/juno_lib.h),
shown as a folder JUNO first in TEENSY: their entries are Juno-106 patches
(kind 0, size 24); 0F on one adds it to the project's JUNO list
(/.rytm/juno/<PROJECT>.txt: 18 bytes as hex and the name per slot). A .syx
holding several patch messages (a bank, e.g. 128 dumps; 2026-10-09) is a
subfolder named after the file, its entries the patches in file order, named
by the dump's patch number and the file ("A11 Factory" .. "B88 Factory", "#N
<file>" for a 31 manual dump); 0F on one adds that patch alike.
Folder numbers: breadth first from /Samples, only folders with .wav files below,
subfolders and files in name order (the Teensy's index; the simulator alike).

Looper mode (rytm-mods 0022-looper):

  Rytm -> Teensy  cmd 0x05 MODE:  F0 7D 52 54 05 <on 0/1> <page> F7
                  cmd 0x06 PAD:   F0 7D 52 54 06 <slot> <down 1/0> F7
                                  (slot = page * 12 + pad, pads BD..CB)
  Teensy -> Rytm  cmd 0x07 SLOT:  F0 7D 52 54 07 <slot> <state> F7
                                  state 0 empty, 1 queued, 2 recording,
                                  3 playing, 4 stopped; the pad shows it
                  cmd 0x08 MUTES: F0 7D 52 54 08 <on 0/1> F7
                                  1 keep the mutes and mute all tracks (a loop
                                  took over), 0 restore them

cmd 0x02 TRIG (a trig without parameters) is unused, kept reserved.

The AUDIO direction is unused: Overbridge's own audio stream carries the voices
(docs/overbridge-voice-inputs.md).

User slots on the Rytm (2026-10-10; rytm-mods 0021's machines 35..39 as U1..U5,
docs/superpowers/specs/2026-10-10-user-slots-rytm-design.md 2.1). Every message
carries a version byte (1) right after the command; a receiver ignores a version
or a command it does not know. Text ASCII 0x20..0x7e (else '_'), NUL padded.
Generations 1..127 per slot, kept by the Teensy, bumped whenever what 1A says
changes; 0 = the Rytm knows nothing.

    19 <ver> <slot 1..5|0> <gen> <what> <arg>          Rytm -> Teensy, ASK
        what 0 the slot's metadata (1A; "unchanged" when gen matches), 1 enum
        names of knob arg 0..15 (1B list arg), 2 the slot's preset list page arg
        (16 names a page, 1B 0x20), 3 the installed machines (slot 0, 1B 0x21)
    1A <ver> <slot> <gen> <part> ...                   Teensy -> Rytm, METADATA
        part 0x7F unchanged; part 0: <state> <flags> <nknobs> <nparts> <name 10>
        <short 4> <p2 title 13> [<fp hi> <fp lo>] (the slot's plugin_fp, 2026-10-10;
        absent: 0); parts 1..nparts-1: up to 4 knobs of 24 bytes,
        <p> <look> <kind> <role> <min> <max> <default> <enum count> <long 12>
        <short 4>. State 0 EMPTY 1 READY 2 LOADING 3 MISSING 4 BAD 5 NO_ROOM
        6 MUTED; flags bit 0 page 2 = params, 1 the missing machine is on the
        card's /MACHINES, 2 presets exist; look 0 dial 1 centred 2 fader 3 list;
        kind 0 number 1 bipolar 2 enum 3 list 4 note 5 time 6 preset 7 pool;
        role 0 free 1 preset 2 offset
    1B <ver> <slot> <list> <gen> <replace> <total> {<index> <13 name bytes>}
                                                       Teensy -> Rytm, LIST NAMES
        8 a message, rules as 16/17; list 0x00..0x0F knob list's enum names,
        0x20 the slot's presets, 0x21 the installed machines (slot 0, gen = the
        store's)
    1C <ver> <voice 0..7> <slot> <n> {<knob 0..15> <value 0..127>}
                                                       Teensy -> Rytm, KNOBS
    1D <ver> <gen U1> .. <gen U5> <installed gen> [<slot> <result>]
                                                       Teensy -> Rytm, SLOTS
        result 0 READY 1 NOT INSTALLED 2 NO ROOM 3 STOP TO INSTALL 4 NOT ON CARD
        5 BAD 6 LOADING
    1E <ver> <slot 1..5> <op> <arg> <installed gen>    Rytm -> Teensy, ASSIGN
        op 0 assign entry arg of the installed list, 1 clear, 2 install the
        slot's missing machine from the card's /MACHINES

Sidecar send FX (2026-10-16, rytm-mods 0025-sendfx-report; docs/superpowers/specs/
2026-10-16-sidecar-send-fx-storage.md). 0x1F is reserved for the profile message
(2026-10-11-platform-architecture-design.md), so the next free id is 0x20:

    20 <ver 1> <track 0..11> <hi> <lo>                 Rytm -> host, SENDFX
    20 <ver 1> 12 {<hi> <lo>} x 6
        the trig array's words (after p-locks, before the LFO) as the trig loop
        last wrote them: tracks 0..11 sound index 0, track 12 (FX) FX words 0, 29,
        38, 39, 40, 41. hi = word bits 8..14, lo = bits 1..7 (a stock lock's value
        and its fine slot). Sound index 0: hi = SEND FX 1, lo = SEND FX 2 (the
        storage spec, section 6). Sent from the audio interrupt in the block a
        changed value's trig plays, and one track in turn every `rr` blocks
        (default 64, ~43 ms) so a host that attaches has every value within ~0.6 s.
    20 <ver 1> 13 <tag1 hi7> <tag1 lo7> <tag2 hi7> <tag2 lo7>
        the kit's 14-bit user-effect tags for FX 1 / FX 2 (0 = none; proposed for
        M4, non-lockable kit storage to be chosen with the M4 agent).

    Track 12's values (2026-10-16, user: the TYPE lives in the kit, so it travels
    with the kit and its patterns): each custom FX = TYPE + P1..P5, one 7-bit value
    each. FX 1: TYPE = word 29 hi, P1 = 29 lo, P2/P3 = 38 hi/lo, P4/P5 = 39 hi/lo.
    FX 2: TYPE = word 0 hi, P1 = 0 lo, P2/P3 = 40 hi/lo, P4/P5 = 41 hi/lo. TYPE and P1
    sit in the stock-stored words (option A), P2..P5 need 0025's 38..41 storage
    (B). A lo half is a lock's fine slot: a coarse-only lock leaves it 0. (Mapping
    proposed by the M2 Sidecar side; the M4 page layout to confirm.)
    TYPE ids (stable, SENDFX_TYPES): 0 off; 1..63 built in, no tag (1 CHORUS,
    2 COMB, 16 CRUSH, 17 RINGMOD, 18 HALO in every build, the Teensy too; 5 SPACE,
    6 RINGS, 8 CLOUDSEED, 15 STUTTER on the Pi; the rest reserved for planned effects - a Sidecar without one
    plays silence and reports it MISSING); 64..67 the user-FX slots UFX1..UFX4,
    as the user machines' U1..U5: the project's slot map on the Sidecar card
    (/.rytm/fxslots/<project>.txt: line n = "<tag 4 hex> <name>" or empty) says
    which effect a slot holds; the kit's tag (track 13) must match it, else that
    FX is silent and MISSING (!NAME). A TYPE change (a kit load, a p-lock) is
    never abrupt: the old effect fades out over 10 ms, the new one starts from
    a clean state and fades in over 10 ms.

    21 <ver 1> <replace> <total> {<id> <13 name bytes>}   host -> Rytm, SENDFX NAMES
        the TYPE knob's list (OFF, the built-in effects this Sidecar plays, the
        project's filled UFX slots), 8 entries a message, rules as 16/17; sent on
        a 0x23 and on a project change (0x10)
    22 <ver 1> <fx 0|1> <id> <state> <name 10>             host -> Rytm, SENDFX STATE
        FX k's TYPE as the Sidecar resolves it; state as 1A's (0 EMPTY = off,
        1 READY, 3 MISSING: the Rytm shows !NAME); sent when FX k's id or state
        changes (0x20 track 12/13), on 0x23 and on a project change
    23 <ver 1>                                             Rytm -> host, SENDFX ASK
    24 <ver 1> <id> <label 4> x 4 {<knob | show << 2 | step << 3> <lo> <base> <name 5>}   host -> Rytm, SENDFX LABELS
        TYPE id's knob labels for P2..P5 (ASCII, NUL padded, as the stock pages'
        4) and its zones (at most 12): knob 0..3 = P2..P5, a zone runs from lo to
        the next zone's lo - 1 of that knob (or 127), lo ascending; the Rytm
        shows its name, with show also v - lo + base ("MODAL 12"); a knob without
        zones shows its number. step (2026-10-18, set on every zone of a knob that
        is a pure zone selector): the Rytm (0029 sfx-enum-knobs) steps that knob one
        zone per detent, as its own enum knobs, and stores each zone's lo; without
        it the knob stays continuous (an older Rytm ignores the bit). One message a TYPE this Sidecar plays (OFF has
        none), after the 21s (a 21 with replace clears the Rytm's labels), on
        0x23 and on a project change; tables next to each effect
        (core/fx/knob_labels.h). Without any 24 the Rytm keeps P2..P5 generic.
        P1 is ROUTE on every TYPE: the Rytm shows PRE (0..63) / POST (64..127)
        itself.

Slot fingerprints (2026-10-19, rytm-mods 0021 slot-resolve; docs/user-machines-pi.md, "Which machine a slot
plays"). 0x25 is the next free id (0x1F stays reserved for the profile message, 0x20..0x24 are the send FX):

    25 <ver 1> <seq> <flags> <n> {<slot 1..5> <fp hi7> <fp lo7> <count>}   Rytm -> host, SLOT PRINTS
        the fingerprints (the sound tag, plugin_fp) the project's sounds use on each user slot: every
        sound of the 128 resident kits and the sound pool whose machine is U1..U5 and whose tag is nonzero,
        one entry per distinct (slot, fp), count = how many sounds (1..127, capped), slots ascending, at
        most 16 entries (flags bit 0: more were left out). n 0 = no tagged U-sound in the project. Sent
        right after a 10 <project> under one MIDI lock (try-lock: never waits), once a project or kit load
        has settled, and again every ~5 s with seq unchanged while nothing changed (a host that came up
        later); seq (0..127) moves on whenever the content changes. A host resolves the project's slots
        from the first 25 after a 10 and from any 25 whose content differs from the last one it used; a
        repeat is ignored. An older host ignores 0x25; an older Rytm never sends it (the host then
        resolves lazily from the trigs' tags, 0x04/0x03).

IN L/R source (2026-10-18, rytm-mods 0030-fx-in; docs/recorder-mini.md, "IN L/R"). 0x26 / 0x27:

    26 <v>      host -> Rytm, FXIN: 1 the delay + reverb return (stereo) on the Overbridge IN L/R (channels 10 / 11)
                instead of the codec inputs, 0 the stock AUDIO IN, 0x7F ask (no change)
    27 <s>      Rytm -> host, FXIN_STATE: 1 on, 0 off; answers every 26 (asks too) and is sent unasked each time the
                Rytm's USB port comes up (it resets itself to off on a disconnect: the cue to send 26 01 again).
                An older Rytm (no 0030) never answers: the host shows "not supported by this Rytm build".
                No version byte. Never send channels 10 / 11 back into the Rytm through Overbridge (feedback).

Project slot reports (2026-10-27, rytm-mods rui1052 0026-remote-ui; core/bundle/prjslot.h,
docs/superpowers/specs/2026-10-27-project-slot-keys-design.md). Rytm -> host only, F0 7D 52 54 4B 01 <op> ... F7;
slots 1-based in two 7-bit bytes (n >> 7, n & 0x7F), 0 = none. The hello's (35) caps bit 2 says the Rytm sends them.

    4B 01 00 <slot hi lo> <name>       ACTIVE: the active slot and its name (on a change, after each hello, every 5 s)
    4B 01 01 <from hi lo> <to hi lo>   SAVE (from == to: a save of the active project; else SAVE AS / SAVE TO)
    4B 01 02 <src hi lo> <dst hi lo>   PASTE (src: the slot COPY took, 0 unknown)
    4B 01 03 / 04 / 05 <0 0> <slot>    CLEAR (also INIT NEW, FUNC + PLAY) / DELETE / RENAME
    4B 01 06 <n hi lo> <0 0>           LOST: n events dropped
    4B 01 07 <0 0> <slot>              WRITE: a slot written by a path not followed (logged)
    4B 01 08 <first hi lo> <n> {<occ> <name 15>} x n   LIST (rui1052b): all 128 slots, 8 a message, after each hello's
                                       ACTIVE and after a LOST; each list sent twice (rui1052c): the host acts only on
                                       two identical whole lists in a row (all 16, END count = occupied, > 0)
    4B 01 09 <count hi lo> <0 0>       LIST END (0x3fff: the Rytm could not read its table: no list)
    4B 01 0A <0 0> <slot>              LOAD: LOAD PROJECT only (rui1052c: the setter called from 0x400b405a; the same
                                       slot again too; never from the saves or CREATE NEW)
4B 0B..3F are reserved for projects.
4C: the kit reports (rytm-mods rui1052d; core/bundle/kits.h), 4C 01 <op> <kit hi lo> <src hi lo> (kits 1-based):
    01 SAVE <kit> <src>   SAVE KIT / YES + KIT (src the active kit before; SAVE KIT to another kit makes it active)
    02 RELOAD <kit>       RELOAD KIT / NO + KIT
    03 RESTORE <kit>      the kept-aside copy back (rytm-mods 0010's EDITS RESTORED; no rui build sends it: 0010 and
                          0021 exclude each other)
    04 RELOAD ALL <kit>   the KIT menu's RELOAD set to ALL
4D 01 <dropped hi lo> <over hi lo>: rui1052d (round 6) - key presses 0026 dropped (each with its release, its queue
    full) and its waits past a stock dump run out (2 s with the flag never clear); on a change, at most once a second.
0x4E is the next free id.
"""
PRJ = 0x4B   # the project slot reports (above)

HDR = bytes([0xF0, 0x7D, 0x52, 0x54])
AUDIO, TRIG, PARAMS, TRIG_PARAMS = 0x01, 0x02, 0x03, 0x04
NOTE_OFF = 0x11
POOL_ASSIGN, POOL_ASK, POOL_TABLE = 0x12, 0x13, 0x14
JUNO_KNOBS, JUNO_NAMES = 0x15, 0x16
FM_NAMES, FM_ASK = 0x17, 0x18
# The curated ENG lists (rytm-mods 0021, 2026-10-09): the knob's value (P's
# integer part, 0..127) in N equal zones: zone = (value * N) >> 7, at most N - 1.
# PLAITS N = 17, zone -> Plaits model PLAITS_MODELS[zone]: VA VCF, PHASE DIST,
# WAVE TERRAIN, STRING MACH, CHIPTUNE, VA, WAVESHAPE, 2-OP FM, GRANULAR,
# HARMONIC, WAVETABLE, CHORDS, SPEECH, SWARM, PARTICLE, STRING, MODAL (no 6-OP
# FM 1-3: the 6-OP FM machine; no NOISE; no drums: DIGIDRUM's then, the MD user machine's now).
# DAISYSP and DSP SAMPLE N = 3: MODAL STRING RESON.
PLAITS_MODELS = (0, 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20)
DAISY_MODELS = ("MODAL", "STRING", "RESON")


def eng_zone(value, n):
    """A P value (0..127.99) -> its ENG zone 0..n-1."""
    v = max(0, min(127, int(value)))
    return min((v * n) >> 7, n - 1)


# the machine: voice byte & MACHINE_MASK, matched exactly
MACHINE_MASK = 0x78
PLAITS, JUNO, DAISYSP, DSP_SAMPLE, DIGIDRUM, FM = 0x00, 0x08, 0x10, 0x20, 0x40, 0x48   # 0x40: MD now (DIGIDRUM's old flag)
MD, MD_LAYOUT_TAG = 0x40, 0x224D   # docs/MD-BUILTIN.md
USER_SLOTS = (0x50, 0x58, 0x60, 0x68, 0x70)   # U1..U5: machine plugins (teensy/voiceloop, RYTM_PLUGINS)
JUNO_KNOB_BYTES = (5, 6, 7, 11, 12, 14)   # CUT RES ENV ATK DEC REL in a patch's 18
BLOCK = 32


def audio_block(voice: int, seq: int, samples) -> bytes:
    """samples: 32 ints in -8192..8191 (14-bit)."""
    out = bytearray(HDR)
    out += bytes([AUDIO, voice & 7, seq & 0x7F])
    for s in samples:
        v = max(-8192, min(8191, int(s))) & 0x3FFF
        out += bytes([v >> 7, v & 0x7F])
    out.append(0xF7)
    return bytes(out)


def parse(msg: bytes):
    """-> (cmd, payload bytes) for an RT message, else None."""
    if len(msg) < 6 or msg[:4] != HDR or msg[-1] != 0xF7:
        return None
    return msg[4], msg[5:-1]


def decode_note_off(payload: bytes):
    """NOTE_OFF's payload -> the voice 0..7."""
    return payload[0] & 7


def decode_audio(payload: bytes):
    voice, seq = payload[0], payload[1]
    data = payload[2:]
    samples = []
    for i in range(0, len(data), 2):
        v = data[i] << 7 | data[i + 1]
        samples.append(v - 0x4000 if v & 0x2000 else v)
    return voice, seq, samples


def pool_assign(slot: int, folder: int, file: int) -> bytes:
    """12 ASSIGN: pool slot 1..127 = TEENSY folder's file."""
    return HDR + bytes([POOL_ASSIGN, slot & 0x7F, (folder >> 7) & 0x7F, folder & 0x7F,
                        (file >> 7) & 0x7F, file & 0x7F, 0xF7])


def decode_pool_table(payload: bytes):
    """14's payload -> (replace, [(slot, folder, file, name)])."""
    replace, count, out = payload[0], payload[1], []
    for k in range(count):
        e = payload[2 + 18 * k:2 + 18 * (k + 1)]
        name = bytes(e[5:18]).rstrip(b"\0").decode("ascii")
        out.append((e[0], e[1] << 7 | e[2], e[3] << 7 | e[4], name))
    return bool(replace), out


def name13(name: str):
    """A name for 14 / 16: ASCII 0x20..0x7e (other bytes of its UTF-8 '_'),
    cut to 13 bytes, NUL padded."""
    b = [c if 0x20 <= c <= 0x7E else ord("_") for c in name.encode()[:13]]
    return b + [0] * (13 - len(b))


def juno_knobs(voice: int, patch18) -> bytes:
    """15 KNOBS: a patch's CUT RES ENV ATK DEC REL to the Rytm's voice."""
    return HDR + bytes([JUNO_KNOBS, voice & 7] + [patch18[i] & 0x7F for i in JUNO_KNOB_BYTES] + [0xF7])


def decode_juno_knobs(payload: bytes):
    """15's payload -> (voice, [CUT, RES, ENV, ATK, DEC, REL])."""
    return payload[0], list(payload[1:7])


def machine(voice_byte: int) -> int:
    """A 0x04/0x03 report's voice byte -> its machine (PLAITS, JUNO, DAISYSP,
    DSP_SAMPLE, DIGIDRUM, FM); any other value PLAITS."""
    m = voice_byte & MACHINE_MASK
    return m if m in (JUNO, DAISYSP, DSP_SAMPLE, DIGIDRUM, FM) else PLAITS


def juno_names(names, cmd=JUNO_NAMES) -> list:
    """16 NAMES messages for a JUNO list (cmd FM_NAMES: 17 for the FM list):
    names[slot] a name or None (empty). An empty list is one message,
    replace 1, total 0."""
    total = len(names)
    msgs = []
    for first in range(0, max(total, 1), 8):
        m = bytearray(HDR) + bytes([cmd, 1 if first == 0 else 0, total])
        for slot in range(first, min(first + 8, total)):
            m += bytes([slot] + (name13(names[slot]) if names[slot] is not None else [0] * 13))
        m.append(0xF7)
        msgs.append(bytes(m))
    return msgs


def fm_names(names) -> list:
    """17 FM NAMES messages for the effective FM list (as juno_names)."""
    return juno_names(names, FM_NAMES)


def decode_juno_names(payload: bytes):
    """16's (or 17's) payload -> (replace, total, [(slot, name)])."""
    replace, total, out = payload[0], payload[1], []
    for k in range((len(payload) - 2) // 14):
        e = payload[2 + 14 * k:2 + 14 * (k + 1)]
        out.append((e[0], bytes(e[1:14]).rstrip(b"\0").decode("ascii")))
    return bool(replace), total, out


# ---- user slots (2026-10-10): 19..1E, see the docstring ----
ASK, META, LIST, KNOBS, SLOTS, ASSIGN = 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E
PROTO_VER = 1
ASK_META, ASK_ENUM, ASK_PRESETS, ASK_INSTALLED = 0, 1, 2, 3
LIST_PRESETS, LIST_INSTALLED = 0x20, 0x21
ST_EMPTY, ST_READY, ST_LOADING, ST_MISSING, ST_BAD, ST_NO_ROOM, ST_MUTED = range(7)
ST_NAMES = ("EMPTY", "READY", "LOADING", "MISSING", "BAD", "NO_ROOM", "MUTED")
FLAG_PARAMS, FLAG_ON_CARD, FLAG_PRESETS = 1, 2, 4
RES_READY, RES_NOT_INSTALLED, RES_NO_ROOM, RES_STOP_TO_INSTALL, RES_NOT_ON_CARD, RES_BAD, RES_LOADING = range(7)
OP_ASSIGN, OP_CLEAR, OP_INSTALL = 0, 1, 2
LOOKS = ("dial", "centred", "fader", "list")
KINDS = ("number", "bipolar", "enum", "list", "note", "time", "preset", "pool")
ROLES = ("free", "preset", "offset")
META_UNCHANGED = 0x7F


def _msg(cmd, body):
    return bytes(HDR) + bytes([cmd, PROTO_VER]) + bytes(body) + b"\xf7"


def text_bytes(s, n):
    """n bytes of text: ASCII 0x20..0x7e (else '_'), cut, NUL padded; a NUL ends it."""
    out = []
    for c in s.encode()[:n]:
        if c == 0:
            break
        out.append(c if 0x20 <= c <= 0x7E else ord("_"))
    return out + [0] * (n - len(out))


def _text(b):
    return bytes(b).split(b"\0")[0].decode("ascii", "replace")


def _ok(payload, n=None, at_least=None):
    """A payload of our version and of length n (or at least at_least)."""
    if not payload or payload[0] != PROTO_VER:
        return False
    return len(payload) == n if n is not None else len(payload) >= at_least


def _ix(table, v):
    return table.index(v) if isinstance(v, str) else int(v)


def ask(slot, gen, what, arg=0):
    """19 ASK (Rytm -> Teensy)."""
    return _msg(ASK, [slot & 0x7F, gen & 0x7F, what & 0x7F, arg & 0x7F])


def assign(slot, op, arg, installed_gen):
    """1E ASSIGN (Rytm -> Teensy)."""
    return _msg(ASSIGN, [slot & 0x7F, op & 0x7F, arg & 0x7F, installed_gen & 0x7F])


def decode_ask(payload):
    if not _ok(payload, 5):
        return None
    return dict(slot=payload[1], gen=payload[2], what=payload[3], arg=payload[4])


def decode_assign(payload):
    if not _ok(payload, 5):
        return None
    return dict(slot=payload[1], op=payload[2], arg=payload[3], installed_gen=payload[4])


def meta_unchanged(slot, gen):
    """1A part 0x7F: the Rytm's generation is current."""
    return _msg(META, [slot & 0x7F, gen & 0x7F, META_UNCHANGED])


def plugin_fp(pkg_id):
    """A plugin's fingerprint (the sound tag, 2026-10-10): FNV-1a 32 of the package id's bytes as stored (case
    kept; not the version: an upgrade keeps its sounds), mod 16383, + 1 - 1..16383 (14 bits; 0 means untagged).
    The Teensy's C++ plugin_fp must match: shared vectors in tests/test_protocol_us.py (spec 2.6)."""
    h = 0x811C9DC5
    for c in pkg_id.encode():
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h % 16383 + 1


def meta_messages(slot, gen, state, flags=0, name="", short="", p2_title="", knobs=(), fp=None):
    """1A part 0 and the knob parts. knobs: dicts p, look, kind, role, min, max, default,
    enum_count, long, short (look/kind/role by name or number; defaults dial, number, free,
    0, 127, 64, 0). fp: the slot's plugin fingerprint (plugin_fp), two more bytes at the end of
    part 0 (fp >> 7, fp & 0x7F); None: the older part 0 without them."""
    knobs = list(knobs)
    nparts = 1 + (len(knobs) + 3) // 4
    slot &= 0x7F
    out = [_msg(META, [slot, gen & 0x7F, 0, state & 0x7F, flags & 7, len(knobs) & 0x7F, nparts & 0x7F] + text_bytes(name, 10) +
                text_bytes(short, 4) + text_bytes(p2_title, 13) + ([] if fp is None else [fp >> 7 & 0x7F, fp & 0x7F]))]
    for part in range(1, nparts):
        body = [slot, gen & 0x7F, part & 0x7F]
        for k in knobs[4 * (part - 1):4 * part]:
            body += [x & 0x7F for x in (k["p"], _ix(LOOKS, k.get("look", 0)), _ix(KINDS, k.get("kind", 0)),
                                        _ix(ROLES, k.get("role", 0)), k.get("min", 0), k.get("max", 127),
                                        k.get("default", 64), k.get("enum_count", 0))]
            body += text_bytes(k.get("long", ""), 12) + text_bytes(k.get("short", ""), 4)
        out.append(_msg(META, body))
    return out


def decode_meta(payload):
    """1A's payload -> dict: slot, gen, part and unchanged=True, or part 0's state, flags, nknobs,
    nparts, name, short, p2_title, fp (0 from a part 0 without it), or a knob part's knobs
    (look/kind/role as names). None when malformed (a length that does not fit the part, a
    look/kind/role out of range)."""
    if not _ok(payload, at_least=4):
        return None
    d = dict(slot=payload[1], gen=payload[2], part=payload[3])
    if d["part"] == META_UNCHANGED:
        if len(payload) != 4:
            return None
        d["unchanged"] = True
        return d
    if d["part"] == 0:
        if len(payload) not in (4 + 31, 4 + 33):
            return None
        b = payload[4:]
        d.update(state=b[0], flags=b[1], nknobs=b[2], nparts=b[3], name=_text(b[4:14]), short=_text(b[14:18]),
                 p2_title=_text(b[18:31]), fp=b[31] << 7 | b[32] if len(b) == 33 else 0)
        return d
    d["knobs"] = []
    b = payload[4:]
    if not b or len(b) % 24 or len(b) > 4 * 24:
        return None
    for i in range(0, len(b), 24):
        e = b[i:i + 24]
        if e[1] >= len(LOOKS) or e[2] >= len(KINDS) or e[3] >= len(ROLES):
            return None
        d["knobs"].append(dict(p=e[0], look=LOOKS[e[1]], kind=KINDS[e[2]], role=ROLES[e[3]], min=e[4], max=e[5],
                               default=e[6], enum_count=e[7], long=_text(e[8:20]), short=_text(e[20:24])))
    return d


def list_messages(slot, lst, gen, names, first=0, total=None, replace=True):
    """1B LIST NAMES: entries first.. (names[i] is entry first + i; None an empty entry), 8 a
    message; replace on the first message only (when replace); total defaults to first + len(names).
    An empty list is one message."""
    total = first + len(names) if total is None else total
    msgs = []
    for off in range(0, max(len(names), 1), 8):
        body = [slot & 0x7F, lst & 0x7F, gen & 0x7F, 1 if replace and off == 0 else 0, max(0, min(total, 127))]
        for i, n in enumerate(names[off:off + 8]):
            body += [(first + off + i) & 0x7F] + (text_bytes(n, 13) if n is not None else [0] * 13)
        msgs.append(_msg(LIST, body))
    return msgs


def decode_list(payload):
    if not _ok(payload, at_least=6) or (len(payload) - 6) % 14 or len(payload) - 6 > 8 * 14:
        return None
    b = payload[6:]
    entries = [(b[i], _text(b[i + 1:i + 14])) for i in range(0, len(b), 14)]
    return dict(slot=payload[1], list=payload[2], gen=payload[3], replace=bool(payload[4]), total=payload[5],
                entries=entries)


def knobs(voice, slot, pairs):
    """1C KNOBS: (knob 0..15, value 0..127) pairs for the voice's owner track's slot machine."""
    pairs = list(pairs)[:16]
    return _msg(KNOBS, [voice & 7, slot & 0x7F, len(pairs)] + [x for k, v in pairs for x in (k & 0x0F, v & 0x7F)])


def decode_knobs(payload):
    if not _ok(payload, at_least=4) or len(payload) != 4 + 2 * payload[3]:
        return None
    n = payload[3]
    return dict(voice=payload[1], slot=payload[2], knobs=[(payload[4 + 2 * i], payload[5 + 2 * i]) for i in range(n)])


def slots(gens, installed_gen, slot=0, result=0):
    """1D SLOTS: the five generations and the installed list's; slot 1..5 adds the answer pair to a 1E."""
    return _msg(SLOTS, [g & 0x7F for g in gens] + [installed_gen & 0x7F] + ([slot & 0x7F, result & 0x7F] if slot else []))


def decode_slots(payload):
    if not _ok(payload, at_least=7) or len(payload) not in (7, 9):
        return None
    return dict(gens=list(payload[1:6]), installed=payload[6],
                answer=(payload[7], payload[8]) if len(payload) == 9 else None)


# ---- the reports' sound tag (2026-10-10): the Rytm (rytm-mods 0021 ext_render) appends the owner track's
# sound word 0 - the plugin fingerprint a U-sound was saved with, 0 untagged - as two 7-bit bytes ----
def _p14(ps):
    return [b for v in ps for b in (v >> 7 & 0x7F, v & 0x7F)]


def _tag(tag):
    return [] if tag is None else [tag >> 7 & 0x7F, tag & 0x7F]


def trig_params(voice_byte, note, vel, ps, tag=None):
    """0x04 TRIG+PARAMS: ps 16 (or 8) 14-bit values; tag None: no tag bytes (older firmware)."""
    return HDR + bytes([TRIG_PARAMS, voice_byte & 0x7F, note & 0x7F, vel & 0x7F] + _p14(ps) + _tag(tag) + [0xF7])


def params_report(voice_byte, ps, tag=None):
    """0x03 PARAMS: as trig_params, without the note."""
    return HDR + bytes([PARAMS, voice_byte & 0x7F] + _p14(ps) + _tag(tag) + [0xF7])


def _report(payload, head):
    """head bytes, then P1..P8 or P1..P16 and an optional 2-byte tag -> (params, tag), else None."""
    n = len(payload) - head
    if n not in (16, 32, 34):
        return None
    b = payload[head:]
    ps = [b[i] << 7 | b[i + 1] for i in range(0, min(n, 32), 2)]
    return ps, (b[32] << 7 | b[33] if n == 34 else 0)


def decode_trig_params(payload):
    """0x04's payload -> dict voice, machine (voice byte & MACHINE_MASK), note, vel, params, tag (0 absent)."""
    r = _report(payload, 3)
    if r is None:
        return None
    return dict(voice=payload[0] & 7, machine=payload[0] & MACHINE_MASK, note=payload[1], vel=payload[2],
                params=r[0], tag=r[1])


def decode_params(payload):
    """0x03's payload -> dict voice, machine, params, tag (0 absent)."""
    r = _report(payload, 1)
    if r is None:
        return None
    return dict(voice=payload[0] & 7, machine=payload[0] & MACHINE_MASK, params=r[0], tag=r[1])


# ---- Sidecar send FX (2026-10-16): 20 SENDFX, Rytm -> host (rytm-mods 0025-sendfx-report) ----
SENDFX = 0x20
SENDFX_FX_WORDS = (0, 29, 38, 39, 40, 41)   # track 12's words, in message order
FX_TRACK = 12
TAG_TRACK = 13
SENDFX_NAMES, SENDFX_STATE, SENDFX_ASK = 0x21, 0x22, 0x23
# stable TYPE ids (core/fx/send_fx.cpp kBuiltins); BUILT = the ones this release plays
SENDFX_TYPES = {0: "OFF", 1: "CHORUS", 2: "COMB", 3: "DELAY", 4: "PINGPONG", 5: "SPACE", 6: "RINGS",
                7: "S4 RESO", 8: "CLOUDSEED", 9: "PLASMA", 10: "PLATE", 11: "HALL", 12: "SHIMMER",
                13: "FLANGER", 14: "PHASER", 15: "STUTTER", 16: "CRUSH", 17: "RINGMOD",
                18: "HALO"}
SENDFX_BUILT = (1, 2, 16, 17, 18, 5, 6, 8, 15)   # 1, 2, 16, 17, 18 everywhere (the Teensy's small set); 5, 6, 8, 15 the Pi
SENDFX_BUILT_TEENSY = (1, 2, 16, 17, 18)       # (and the Mac): a Teensy build reports them MISSING
# fx-ins (core/fx/insert.h, docs/INSERT-FX.md): the INSERT slots' TYPE ids (stable: the card's files name them), the
# seq p-lock params 24..31 lock the track's voice's INSERT A / B P1..P4 (core/seq/seq.h kLockIns; SEQ_LOCK_INS below)
INSERT_TYPES = {0: "OFF", 1: "FILTER", 2: "CRUSH", 3: "DRIVE", 4: "EQ", 5: "COMP", 6: "DELAY", 7: "TREMOLO", 8: "RINGMOD", 9: "AMP"}
SENDFX_STUTTER = 15   # a master insert, not a send (core/fx/stutter_fx.h): one FX slot at a time, a second MISSING
SENDFX_USER_FIRST, SENDFX_USER_SLOTS = 64, 4
# track 12 pairs (words 0, 29, 38, 39, 40, 41) -> FX k's [TYPE, P1..P5] as (pair, half 0 hi / 1 lo)
SENDFX_KNOBS = (((1, 0), (1, 1), (2, 0), (2, 1), (3, 0), (3, 1)), ((0, 0), (0, 1), (4, 0), (4, 1), (5, 0), (5, 1)))


def sendfx(track, words):
    """20 SENDFX (Rytm -> host): track 0..11 one 16-bit word (sound index 0), 12 six (FX words 0, 29, 38..41)."""
    return _msg(SENDFX, [track & 0x7F] + [b for w in words for b in ((w >> 8) & 0x7F, (w >> 1) & 0x7F)])


def sendfx_tags(tag1, tag2):
    """20 SENDFX track 13: the kit's 14-bit user-effect tags for FX 1 and 2."""
    return _msg(SENDFX, [TAG_TRACK, (tag1 >> 7) & 0x7F, tag1 & 0x7F, (tag2 >> 7) & 0x7F, tag2 & 0x7F])


def decode_sendfx(payload):
    """20's payload -> dict track, pairs [(hi, lo)], words [(hi << 8) | (lo << 1)] (track 13: tags); None if
    malformed. Track 12 also gives knobs: [[TYPE, P1..P5] for FX 1, FX 2]."""
    if not payload or payload[0] != PROTO_VER or len(payload) < 2:
        return None
    t, b = payload[1], payload[2:]
    if t > TAG_TRACK or len(b) != (12 if t == FX_TRACK else 4 if t == TAG_TRACK else 2) or any(x > 0x7F for x in b):
        return None
    pairs = [(b[i], b[i + 1]) for i in range(0, len(b), 2)]
    if t == TAG_TRACK:
        return dict(track=t, tags=[(h << 7) | lo for h, lo in pairs])
    d = dict(track=t, pairs=pairs, words=[(h << 8) | (lo << 1) for h, lo in pairs])
    if t == FX_TRACK:
        d["knobs"] = [[pairs[p][h] for p, h in k] for k in SENDFX_KNOBS]
    return d


def sendfx_names(entries) -> list:
    """21 SENDFX NAMES: entries [(id, name)] in list order, 8 a message (the first replaces)."""
    total, msgs = len(entries), []
    for first in range(0, max(total, 1), 8):
        body = [1 if first == 0 else 0, total]
        for i, nm in entries[first:first + 8]:
            body += [i] + name13(nm)
        msgs.append(_msg(SENDFX_NAMES, body))
    return msgs


def decode_sendfx_names(payload):
    """21's payload -> (replace, total, [(id, name)])."""
    if not payload or payload[0] != PROTO_VER:
        return None
    replace, total, b = payload[1], payload[2], payload[3:]
    out = [(b[k], bytes(b[k + 1:k + 14]).rstrip(b"\0").decode("ascii")) for k in range(0, len(b) - 13, 14)]
    return bool(replace), total, out


def sendfx_state(fx, type_id, state, name):
    """22 SENDFX STATE: FX fx (0, 1), its TYPE id, the state (ST_EMPTY, ST_READY, ST_MISSING), a 10-char name."""
    return _msg(SENDFX_STATE, [fx, type_id, state] + name13(name)[:10])


def decode_sendfx_state(payload):
    """22's payload -> dict fx, id, state, name; None if malformed."""
    if not payload or payload[0] != PROTO_VER or len(payload) != 14:
        return None
    return dict(fx=payload[1], id=payload[2], state=payload[3], name=bytes(payload[4:14]).rstrip(b"\0").decode("ascii"))


def sendfx_ask():
    """23 SENDFX ASK (Rytm -> host): answered by 21, the 24s and both 22s."""
    return _msg(SENDFX_ASK, [])


SENDFX_LABELS = 0x24
LABEL_CHARS, ZONE_CHARS, MAX_ZONES = 4, 5, 12
# The core's tables (core/fx/knob_labels.h users: send_fx.h, rings_fx.h, cloudseed_fx.h, stutter_fx.h): TYPE id ->
# (labels P2..P5, [(knob 0..3, lo, base, show, name, step)]). step: the knob is a pure zone selector (STUTTER ENG and
# LEN); zones with a number inside (show) stay continuous.
SENDFX_LABEL_SETS = {
    1: (("RATE", "DPTH", "DLY", "FDBK"), []),
    2: (("TUNE", "DCY", "DAMP", "SPRD"), [(1, 0, 0, 1, "POS", 0), (1, 64, 0, 1, "NEG", 0)]),
    16: (("RATE", "BITS", "DRV", "TONE"), []),
    17: (("FREQ", "SPRD", "SHPE", "LFO"), []),
    18: (("DCY", "MOD", "TONE", "SHIM"), [(0, 127, 0, 0, "FRZ", 0)]),   # HALO: DECAY 127 = FREEZE
    5: (("TIME", "RPT", "MODE", "AGE"), [(0, 64, 0, 0, "1/16", 0), (0, 80, 0, 0, "1/8T", 0), (0, 96, 0, 0, "1/8", 0),
                                         (0, 112, 0, 0, "1/8D", 0), (2, 0, 0, 0, "H1", 1), (2, 16, 0, 0, "H2", 1),
                                         (2, 32, 0, 0, "H3", 1), (2, 48, 0, 0, "H1+2", 1), (2, 64, 0, 0, "H2+3", 1),
                                         (2, 80, 0, 0, "H123", 1), (2, 96, 0, 0, "H23+S", 1),
                                         (2, 112, 0, 0, "H123S", 1)]),   # SPACE: TIME's note values, MODE stepped
    6: (("PTCH", "MODL", "BRIT", "DCY"), [(1, 0, 0, 1, "MODAL", 0), (1, 32, 0, 1, "MONO", 0),
                                          (1, 64, 0, 1, "STRNG", 0), (1, 96, 0, 1, "CHORD", 0)]),
    8: (("CHAR", "DCY", "DAMP", "MOD"), [(0, 0, 0, 1, "ROOM", 0), (0, 32, 0, 1, "HALL", 0), (0, 64, 0, 1, "PLATE", 0),
                                         (0, 96, 0, 1, "AMBNC", 0)]),
    15: (("ENG", "LEN", "D/P", "GATE"), [(0, 0, 0, 0, "OFF", 1), (0, 64, 0, 0, "ON", 1), (1, 0, 0, 0, "1/64", 1),
                                         (1, 19, 0, 0, "1/32", 1), (1, 37, 0, 0, "1/16", 1), (1, 55, 0, 0, "1/8", 1),
                                         (1, 74, 0, 0, "1/4", 1), (1, 92, 0, 0, "1/2", 1), (1, 110, 0, 0, "1BAR", 1),
                                         (2, 0, 0, 0, "NONE", 0), (2, 1, 1, 1, "DCY", 0), (2, 64, 1, 1, "PTCH", 0)]),
}
ZONE_STEP = 8   # the zone byte's step bit (knob | show << 2 | step << 3)


def sendfx_labels(type_id, labels, zones):
    """24 SENDFX LABELS: labels (4 strings, P2..P5), zones [(knob 0..3, lo, base, show, name[, step])] (<= 12); a
    5-tuple is an older Sidecar's zone (no step bit)."""
    if len(labels) != 4 or len(zones) > MAX_ZONES:
        raise ValueError("4 labels, at most 12 zones")
    body = [type_id & 0x7F]
    for lab in labels:
        body += text_bytes(lab, LABEL_CHARS)
    for z in zones:
        k, lo, base, show, name = z[:5]
        step = z[5] if len(z) > 5 else 0
        if not 0 <= k <= 3:
            raise ValueError("knob 0..3")
        body += [k | (4 if show else 0) | (ZONE_STEP if step else 0), lo & 0x7F, base & 0x7F] + \
            text_bytes(name, ZONE_CHARS)
    return _msg(SENDFX_LABELS, body)


def decode_sendfx_labels(payload):
    """24's payload -> (id, [4 labels], [(knob, lo, base, show, name, step)]); None if malformed."""
    if not payload or payload[0] != PROTO_VER or len(payload) < 18 or (len(payload) - 18) % 8:
        return None
    b = bytes(payload)
    txt = lambda x: x.split(b"\0")[0].decode("ascii")   # noqa: E731
    labels = [txt(b[2 + 4 * j:6 + 4 * j]) for j in range(4)]
    zones = [(b[k] & 3, b[k + 1], b[k + 2], (b[k] >> 2) & 1, txt(b[k + 3:k + 8]), (b[k] >> 3) & 1)
             for k in range(18, len(b), 8)]
    return b[1], labels, zones


def sendfx_steps(type_id, knob, sets=None):
    """The stored values of P(knob + 2) under TYPE type_id when the Rytm steps it (its zones' lo, in order), else None
    (a continuous knob: no step bit, or no labels for the type)."""
    sets = SENDFX_LABEL_SETS if sets is None else sets
    zs = [z for z in sets.get(type_id, ((), []))[1] if z[0] == knob]
    if not zs or not any(len(z) > 5 and z[5] for z in zs):
        return None
    return [z[1] for z in zs]


def sendfx_value_text(type_id, knob, v, sets=None):
    """What the Rytm shows for P(knob + 2) = v under TYPE type_id (the 0029 rule): a zone's name (with show, then
    v - lo + base, a space when it fits 7 characters), else the number; None without labels for the type."""
    sets = SENDFX_LABEL_SETS if sets is None else sets
    if type_id not in sets:
        return None
    zs = [z for z in sets[type_id][1] if z[0] == knob and z[1] <= v]
    if not zs:
        return str(v)
    lo, base, show, name = zs[-1][1:5]
    if not show:
        return name
    num = str(v - lo + base)
    return name + (" " if len(name) + 1 + len(num) <= 7 else "") + num


def sendfx_route_text(v):
    """P1 (ROUTE) as the Rytm shows it: PRE 0..63 (Overbridge OUT 3-4, before the analog dist/comp), POST 64..127."""
    return "PRE" if v < 64 else "POST"


# ---- Remote UI (2026-10-16, rytm-mods 0026-remote-ui; docs/superpowers/specs/2026-10-16-remote-ui-research.md) ----
# The Rytm as a terminal for pages the Sidecar draws: 0x30..0x3F (clear of 0x20..0x23 send FX; 0022's router passes
# this range to 0026 with rui=1). Every message carries <ver 1> after the command.
#   host -> Rytm  38 <seq> <col0 0..15> <page0 0..7> <ncols 1..16> <npages 1..8> <packed>   framebuffer bytes of a
#                    tile rectangle into staging: column-major, byte = page + 8 x column (page 7 = the top band,
#                    glass pixel (x, y from the top) = byte (7 - y // 8) + 8 x, bit y % 8), 8 x ncols x npages bytes
#                 39 <seq>          commit staging -> the screen (when seq is the 38s'); a partial frame never shows
#                 3A <page> <on>    open (1) / close (0) the remote page
#                 3C                keepalive: the Rytm leaves after 1 s without any valid 3x
#   Rytm -> host  30 <page> <on> <reason>   0 user (TRK + YES), 1 host (3A), 2 timeout, 3 local escape (TRK + YES)
#                 31 <code >> 7> <code & 0x7F> <down>   every key while the page is on (the firmware's key codes:
#                                           YES 0x44, NO 0x45, FUNC 0x80, TRK 0x86, pads 0x90..0x9B)
#                 32 <encoder> <delta + 64> <func>   turns summed per 50 ms tick, delta -63..63
# packed: 7 bytes -> 8, groups of up to 7 bytes, each a byte of their MSBs (bit i = byte i's bit 7) then the low 7s.
RUI_MODE, RUI_KEY, RUI_ENC = 0x30, 0x31, 0x32
RUI_RECT, RUI_COMMIT, RUI_OPEN, RUI_KEEPALIVE = 0x38, 0x39, 0x3A, 0x3C
RUI_TEXT, RUI_TEXT_CLEAR, RUI_ECHO = 0x3D, 0x3E, 0x3F   # 1021: text drawn by the Rytm's own fonts, the local echo
RUI_MASK = 0x37   # 1022: the catch mask (which keys and encoders the page takes; the rest work as on the stock screen)
RUI_HELLO, RUI_SPLASH = 0x35, 0x36   # 1023: the Rytm's hello when its USB port came up; the host's connect splash
RUI_MACHINES = 0x33   # 1025: Rytm -> host <t0> <n> {<machine 0..52> <vb>} x n: the tracks' machines (n 1 a change, 12 a snapshot
                      # after each hello, the page's open, a kit / project load); vb the voice byte its 03 / 04 carry
                      # (voice | the machine flag), 0x7F not a Sidecar machine (rytm-mods 0026 design.md "1025")
RUI_NOT_OURS = 0x7F
RUI_FIRST, RUI_LAST = 0x30, 0x3F
# 1027 SRC view (rytm-mods 0026 rui1027, provisional): Rytm -> host 29 <track 0..11> <on 0/1> the stock SRC page of the
# track shown / left (again after each hello); 2A <track> <first 0..15> <n> {<int 0..127> <frac 0..127>} x n its
# P<first+1>.. as the active kit has them (P1..P8 the SRC knobs A..H = kit words 0..7, P9..P16 the SMPL page's = words
# 17..24; all 16 after each 29 t 1, then the changed span each tick: n 1 a knob's change). No host -> Rytm 29 (0022
# passes only 30..3F): the host answers a 29 t 1 with a whole frame (3E 3D.. 38.. 39; the Rytm shows it from that 39,
# while 3C keeps coming) or with nothing (no page: stock); 3A x 0 with the page closed gives stock back. The view only
# draws: no 37, no 3F, no 34; the TRK + YES page comes first (no 29 0 for it; after it the host's next commit).
RUI_SRC_VIEW, RUI_SRC_VALS = 0x29, 0x2A
# 1027d diagnostic, Rytm -> host 2B <state> <views> <vt0..vt4> <age>: state bits 0 view on, 1 covered, 2 committed,
# 3 host idle >= 1 s, 4 TRK + YES open, 5 splash, 6 the host's frame shown; the controller list's length; the top
# view's vtable (vt0 << 28 | vt1 << 21 | vt2 << 14 | vt3 << 7 | vt4: 40234cfc SamplerLedView = plain SRC); age: 50 ms ticks since it changed (127 max)
RUI_SRC_DIAG = 0x2B
# 3D <seq> <slot 0..63> <x> <y top> <font 0..16> <flags> <chars 0..32>  a text slot into staging (empty: none),
#    committed with the frame by 39; drawn by the Rytm's own text routine in its own fonts (flags bits 0-1 align:
#    0 left of x, 1 centred on x, 2 right ending at x; bit 2 inverted: clear text on a set box). Fonts: the Rytm's
#    17 font objects in boot order (core/ui/rytm_fonts.h has the measured metrics).
# 3E <seq>  every staging text slot empty (the start of a whole frame)
# 3F <enc> <slot | 7F none> <value> <lo> <hi> <flags> <prefix 0..11>  the encoder's local echo: a turn on the Rytm
#    shows prefix + the new value in that slot at its next render, 400 ms before the host's own (flags bit 0: FUNC
#    x 8, bit 1: shown as value - 64 with a sign)
# 37 <e0> <e1> <e2> <k0 .. k36>  1022: the catch mask. Encoder i (0..15) is the host's when bit i % 7 of e(i // 7) is
#    set, key code c (0..255) when bit c % 7 of k(c // 7) is (bytes after the last sent: 0). The rest go to the Rytm's
#    own UI as on the stock screen. Whatever the mask: RECORD / PLAY / STOP always the Rytm's, TRK + YES always 0026's,
#    a caught FUNC reaches both sides (stock keeps its combos); a key's release goes where its press went. Before
#    any 37 (an older host) every key and encoder is caught but the transport; the page leaving by the timeout forgets it.
# 35 <uptime >> 7> <uptime & 0x7F> <caps>  1023 (Rytm -> host): hello, 0.5 s after the Rytm's USB port came up (again
#    1 s and 2 s later while no valid host message came); uptime in 50 ms ticks since 0026 started (saturated at
#    16383); caps bit 0: 36 understood, bit 1 (rui1049): the Rytm keeps 16 send FX label tables (0x24), else 8
# 36 <ticks 0..127>  1023 (host -> Rytm): show the last committed frame as a splash for ticks x 50 ms with the page
#    closed (0: end it). Every key and knob stays the Rytm's (none reaches the host); transport and pads play under it,
#    a navigation key (LEFT RIGHT UP DOWN YES NO) only ends it, any other key or a turn ends it and reaches the Rytm;
#    1 s without a host message ends it too. Ignored while the page is open or before any commit.
RUI_REASONS = ("user", "host", "timeout", "escape")
# The firmware's key codes the remote pages name (rytm-emu src/panel.rs RYTM_KEYS, measured there)
RUI_KEYS = dict(T1=0x00, T2=0x01, T3=0x02, T4=0x03, T5=0x04, T6=0x05, T7=0x06, T8=0x07, T9=0x08, T10=0x09, T11=0x0A,
                T12=0x0B, T13=0x0C, T14=0x0D, T15=0x0E, T16=0x0F, FX=0x10, MUTE=0x11, CHRO=0x12, SCNE=0x13, PERF=0x14,
                TRIG=0x30, SRC=0x31, SMPL=0x32, FLTR=0x33, AMP=0x34, LFO=0x35, LEFT=0x40, RIGHT=0x41, UP=0x42,
                DOWN=0x43, YES=0x44, NO=0x45, RECORD=0x50, PLAY=0x51, STOP=0x52, PUSH_A=0x70, PUSH_B=0x71,
                PUSH_C=0x72, PUSH_D=0x73, PUSH_E=0x74, PUSH_F=0x75, PUSH_G=0x76, PUSH_H=0x77, PUSH_LEVEL=0x78,
                FUNC=0x80, SONG=0x81, CHAIN=0x82, TEMPO=0x83, PAGE=0x84, RTRG=0x85, TRK=0x86, BD=0x90, SD=0x91,
                RS=0x92, CP=0x93, BT=0x94, LT=0x95, MT=0x96, HT=0x97, CH=0x98, OH=0x99, CY=0x9A, CB=0x9B,
                GLOBAL=0xB0, QPER=0xB1, FILL=0xB2, FIX=0xB3, SAMPLER=0xB4, PLAYMODE=0xB5, BANK_A=0xC0, BANK_B=0xC1,
                BANK_C=0xC2, BANK_D=0xC3, BANK_E=0xC4, BANK_F=0xC5, BANK_G=0xC6, BANK_H=0xC7)


def pack7(data) -> list:
    out = []
    for i in range(0, len(data), 7):
        g = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << k for k, b in enumerate(g)))
        out += [b & 0x7F for b in g]
    return out


def unpack7(data) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 8):
        hi, g = data[i], data[i + 1:i + 8]
        out += bytes(b | (((hi >> k) & 1) << 7) for k, b in enumerate(g))
    return bytes(out)


def rui_rect(seq, col0, page0, ncols, npages, fb) -> bytes:
    """38: the rectangle of the 1024 B framebuffer fb (tile columns col0.., pages page0..)."""
    raw = [fb[p + 8 * x] for x in range(col0 * 8, (col0 + ncols) * 8) for p in range(page0, page0 + npages)]
    return _msg(RUI_RECT, [seq & 0x7F, col0, page0, ncols, npages] + pack7(raw))


def rui_commit(seq) -> bytes:
    return _msg(RUI_COMMIT, [seq & 0x7F])


def rui_open(page, on) -> bytes:
    return _msg(RUI_OPEN, [page & 0x7F, 1 if on else 0])


def rui_text(seq, slot, x, y, font, flags, text) -> bytes:
    t = [c for c in text.encode("ascii", "replace")[:32] if 0x20 <= c <= 0x7E]
    return _msg(RUI_TEXT, [seq & 0x7F, slot, x, y, font, flags & 0x7F] + t)


def rui_text_clear(seq) -> bytes:
    return _msg(RUI_TEXT_CLEAR, [seq & 0x7F])


def rui_echo(enc, slot, value, lo, hi, flags, prefix) -> bytes:
    t = [c for c in prefix.encode("ascii", "replace")[:11] if 0x20 <= c <= 0x7E]
    return _msg(RUI_ECHO, [enc, slot, value, lo, hi, flags & 0x7F] + t)


def rui_splash(ticks) -> bytes:
    return _msg(RUI_SPLASH, [max(0, min(127, ticks))])


def rui_keepalive() -> bytes:
    return _msg(RUI_KEEPALIVE, [])


def rui_mask(encs=(), keys=()) -> bytes:
    """37: the catch mask, encoders 0..15 and key codes 0..255 the page takes (the key bytes up to the highest code)."""
    e, k = [0, 0, 0], [0] * 37
    for i in encs:
        e[i // 7] |= 1 << (i % 7)
    for c in keys:
        k[c // 7] |= 1 << (c % 7)
    n = max([c // 7 + 1 for c in keys] or [0])
    return _msg(RUI_MASK, e + k[:n])


def decode_rui_mask(payload):
    """37's payload -> (set of encoders, set of key codes), None if malformed."""
    if not payload or payload[0] != PROTO_VER or not 4 <= len(payload) <= 41:
        return None
    b = payload[1:]
    encs = {7 * j + i for j in range(3) for i in range(7) if b[j] >> i & 1 and 7 * j + i < 16}
    keys = {7 * j + i for j in range(len(b) - 3) for i in range(7) if b[3 + j] >> i & 1 and 7 * j + i < 256}
    return encs, keys


def decode_rui(cmd, payload):
    """A Rytm -> host remote-UI payload -> dict; None if malformed."""
    if not payload or payload[0] != PROTO_VER:
        return None
    b = payload[1:]
    # page: 0 the default (SIDECAR), 1 the LFO page (rui1026's TRK + LFO), 2 INSERT (TRK / UP + FX), 3 its OUT view (UP +
    # AMP, rui1047), 0x10 + t the SEQ page of track t (rui1037's TRK + TRIG, rui1036's FUNC + LFO; the host asks to make t a
    # seq track when it is none), 0x20 + t POLY CONFIG of track t (rui1048: the track menu's item)
    if cmd == RUI_MODE and len(b) == 3:
        return dict(page=b[0], on=b[1], reason=RUI_REASONS[b[2]] if b[2] < len(RUI_REASONS) else b[2])
    if cmd == RUI_KEY and len(b) == 3:
        return dict(key=b[0] << 7 | b[1], down=b[2])
    if cmd == RUI_ENC and len(b) == 3:
        return dict(enc=b[0], delta=b[1] - 64, func=b[2])
    if cmd == RUI_HELLO and len(b) == 3:
        return dict(up=b[0] << 7 | b[1], caps=b[2])
    if cmd == RUI_SRC_VIEW and len(b) == 2 and b[0] < 12 and b[1] <= 1:
        return dict(track=b[0], on=b[1])
    if cmd == RUI_SRC_DIAG and len(b) == 8:
        return dict(state=b[0], views=b[1], top=b[2] << 28 | b[3] << 21 | b[4] << 14 | b[5] << 7 | b[6], age=b[7])
    if cmd == RUI_SRC_VALS and len(b) >= 5 and b[0] < 12 and b[2] >= 1 and b[1] + b[2] <= 16 and len(b) == 3 + 2 * b[2]:
        return dict(track=b[0], first=b[1], values=[b[3 + 2 * i] + b[4 + 2 * i] / 128 for i in range(b[2])])
    if cmd == RUI_MACHINES and len(b) >= 4 and b[1] >= 1 and b[0] + b[1] <= 12 and len(b) == 2 + 2 * b[1]:
        return dict(t0=b[0], tracks=[[b[2 + 2 * i], b[3 + 2 * i]] for i in range(b[1])])
    return None


def rui_src_view(track, on) -> bytes:
    """29 (Rytm -> host, 1027): the SRC page of `track` shown / left."""
    return _msg(RUI_SRC_VIEW, [track & 0x7F, 1 if on else 0])


def rui_src_vals(track, first, values) -> bytes:
    """2A (Rytm -> host, 1027): `track`'s P<first+1>.. (floats 0..127.99: the integer, then the 1/128 fraction)."""
    out = [track & 0x7F, first & 0x7F, len(values)]
    for v in values:
        iv = max(0, min(127, int(v)))
        out += [iv, max(0, min(127, int((v - iv) * 128)))]
    return _msg(RUI_SRC_VALS, out)


def rui_machines(t0, tracks) -> bytes:
    """33 (Rytm -> host, 1025): tracks [(machine, vb)] from track t0."""
    return _msg(RUI_MACHINES, [t0, len(tracks)] + [x & 0x7F for m, vb in tracks for x in (m, vb)])


# ---- slot fingerprints (2026-10-19): 25, see the docstring ----
SLOT_PRINTS = 0x25
SLOT_PRINTS_MAX = 16
SP_TRUNCATED = 1


def slot_prints(entries, seq=0, flags=0):
    """25 SLOT PRINTS (Rytm -> host): entries [(slot 1..5, fp 1..16383, count)], at most 16 (more: flags bit 0)."""
    entries = list(entries)
    if len(entries) > SLOT_PRINTS_MAX:
        entries, flags = entries[:SLOT_PRINTS_MAX], flags | SP_TRUNCATED
    body = [seq & 0x7F, flags & 0x7F, len(entries)]
    for slot, fp, count in entries:
        body += [slot & 0x7F, fp >> 7 & 0x7F, fp & 0x7F, max(1, min(127, count))]
    return _msg(SLOT_PRINTS, body)


def decode_slot_prints(payload):
    """25's payload -> dict seq, flags, entries [(slot, fp, count)], else None (wrong version or length)."""
    if not _ok(payload, at_least=4):
        return None
    n = payload[3]
    if n > SLOT_PRINTS_MAX or len(payload) != 4 + 4 * n:
        return None
    ents = []
    for k in range(n):
        e = payload[4 + 4 * k:8 + 4 * k]
        if not 1 <= e[0] <= 5:
            return None
        ents.append((e[0], e[1] << 7 | e[2], e[3]))
    return dict(seq=payload[1], flags=payload[2], entries=ents)


# ---- Poly FM (2026-10-19, rytm-teensy core/poly.h; docs/POLYPHONY.md section 6) ----
# The 6-OP FM machine's page 2 (SMPL) P13..P16 = sound words 21..24: CHD INV SPR VOI, each its 7-bit value as a direct
# index; beyond the range the first entry (an old sound's stock STA END LOP LEV, 0 120 0 100: OFF 0 0 AUTO).
CHORD_NAMES = ("OFF", "MAJ", "MIN", "SUS2", "SUS4", "7", "MAJ7", "MIN7", "DIM", "AUG", "6", "MIN6", "9", "ADD9", "MIN9", "5")
CHORD_INTERVALS = ((0,), (0, 4, 7), (0, 3, 7), (0, 2, 7), (0, 5, 7), (0, 4, 7, 10), (0, 4, 7, 11), (0, 3, 7, 10),
                   (0, 3, 6), (0, 4, 8), (0, 4, 7, 9), (0, 3, 7, 9), (0, 4, 7, 10, 14), (0, 4, 7, 14), (0, 3, 7, 10, 14),
                   (0, 7))
CHORD_INVS, CHORD_SPRS, CHORD_VOI_MAX = 4, 3, 6
# 28 (its own id: 0x30..0x3F are all 0026's): host -> Rytm 28 01 <track> <root> <CHD> <INV> <SPR> <VOI> (13 bytes, any
# other length refused) the live-recorded step's NOTE and chord locks; Rytm -> host 28 02 <track> <step | 7F> <status>
# (rytm-mods poly-chord)
CHORD = 0x28
CHORD_WRITE, CHORD_REPLY = 0x01, 0x02
CHORD_STATUS = ("ok", "no recent step", "lock slots full", "not recording / not a Sidecar track")


def chord_params(p):
    """P1..P16 (P13..P16 at p[12..15]) -> (chd, inv, spr, voi), each read as core/poly.cpp chord_of does."""
    def ix(x, n):
        v = int(x)
        return v if 0 <= v < n else 0
    return ix(p[12], len(CHORD_NAMES)), ix(p[13], CHORD_INVS), ix(p[14], CHORD_SPRS), ix(p[15], CHORD_VOI_MAX + 1)


def chord_expand(root, chd, inv=0, spr=0, voi=0):
    """The notes core/poly.cpp plays for a trig's note `root` (ascending, distinct, in 0..127)."""
    if chd == 0:
        return [root] if 0 <= root <= 127 else []
    iv = CHORD_INTERVALS[chd]
    n = len(iv)
    b = sorted(set(x + (12 if i < inv % n else 0) for i, x in enumerate(iv)))
    m = voi if 1 <= voi <= CHORD_VOI_MAX else n
    t = sorted(set(b[k % n] + 12 * (k // n) for k in range(m)))
    m = len(t)
    t = sorted(set(x + 12 * ((k * (spr + 1)) // m) for k, x in enumerate(t)))
    return [root + x for x in t if 0 <= root + x <= 127]


def chord_value_text(k, v):
    """The Rytm's value text for chord param k (0 CHD, 1 INV, 2 SPR, 3 VOI) at 7-bit value v."""
    if k == 0:
        return CHORD_NAMES[v] if 0 <= v < len(CHORD_NAMES) else "OFF"
    if k == 1:
        return "INV%d" % (v if 0 <= v < CHORD_INVS else 0)
    if k == 2:
        return "SPR%d" % (v if 0 <= v < CHORD_SPRS else 0)
    n = v if 0 <= v <= CHORD_VOI_MAX else 0
    return "AUTO" if n == 0 else str(n)


def chord_write(track, root, chd, inv, spr, voi) -> bytes:
    """28 01: the step just live-recorded on track (0..11) gets NOTE root and the CHD INV SPR VOI locks."""
    return bytes(HDR) + bytes([CHORD, CHORD_WRITE] + [x & 0x7F for x in (track, root, chd, inv, spr, voi)]) + b"\xf7"


def chord_reply(track, step, status) -> bytes:
    """28 02 (Rytm -> host): step 0..63 or 0x7F (none), status CHORD_STATUS's index."""
    return bytes(HDR) + bytes([CHORD, CHORD_REPLY, track & 0x7F, step & 0x7F, status & 0x7F]) + b"\xf7"


def decode_chord(payload):
    """28's payload (after the id) -> ("write", track, root, chd, inv, spr, voi) / ("reply", track, step, status);
    None if malformed."""
    if len(payload) == 7 and payload[0] == CHORD_WRITE:
        return ("write",) + tuple(payload[1:])
    if len(payload) == 4 and payload[0] == CHORD_REPLY:
        return ("reply",) + tuple(payload[1:])
    return None


# ---- IN L/R source (2026-10-18): 26 / 27, no version byte (rytm-mods 0030-fx-in), see the docstring ----
FXIN, FXIN_STATE = 0x26, 0x27
FXIN_INPUTS, FXIN_FX, FXIN_ASK = 0, 1, 0x7F


def fxin(v):
    """26 FXIN (host -> Rytm): FXIN_FX the FX return on IN L/R, FXIN_INPUTS the AUDIO IN, FXIN_ASK ask (also True / False)."""
    v = FXIN_FX if v is True else FXIN_INPUTS if v is False else int(v)
    if v not in (FXIN_INPUTS, FXIN_FX, FXIN_ASK):
        raise ValueError("fxin: 0, 1 or 0x7F")
    return bytes(HDR) + bytes([FXIN, v]) + b"\xf7"


def fxin_state(on):
    """27 FXIN_STATE (Rytm -> host): on is True / False."""
    return bytes(HDR) + bytes([FXIN_STATE, 1 if on else 0]) + b"\xf7"


def decode_fxin(payload):
    """26's payload (the bytes after the id) -> FXIN_INPUTS / FXIN_FX / FXIN_ASK, else None."""
    return payload[0] if len(payload) == 1 and payload[0] in (FXIN_INPUTS, FXIN_FX, FXIN_ASK) else None


def decode_fxin_state(payload):
    """27's payload -> True (FX return on IN L/R) / False, else None."""
    return bool(payload[0]) if len(payload) == 1 and payload[0] in (0, 1) else None


# ---- The Sidecar sequencer (2026-10-20, docs/SEQUENCER.md; rytm-mods 0026 "seq", host core/seq/seq.h) ----
# The Rytm stays transport master; the host sequences its own poly notes on the Rytm tracks it marks (2E) and follows
# the Rytm's step clock. Every message F0 7D 52 54 <cmd> <ver 1> <body> F7. blk4: the Rytm's audio block counter
# (0x419ca2ac, 1500/s = 32 frames at 48 kHz), its low 28 bits as 4 x 7 bits, most significant first.
#   Rytm -> host  2C 01 <blk4> <n> {<track> <step> <len> <speed>} x n
#                 step ticks in this audio block, one per track per step that fires (trig_fire 0x4009a9bc, trig or not,
#                 step 0 at PLAY too): track 0..11 (only tracks in the 2E mask or its clock mask; no master tick: the
#                 master counter runs a 24-PPQN tick ahead [measured, seqmod]); step 0..63 the Rytm track's own step firing now (a 2x track
#                 sends two per 1x step); len 1..64 the track's length (+0x2c5); speed 0..6 = 2x 3/2x 1x 3/4x 1/2x
#                 1/4x 1/8x (SEQ_SPEEDS). A full queue on the Rytm drops ticks and counts them (2D 09).
#                 2D 01 <kind> <args>   state and events:
#                    01 TRANSPORT <state 0 stop / 1 play / 2 pause> <blk4>   (play after pause = continue)
#                    02 PATTERN <now 0..127> <next 0..127 | 7F>   next: the pattern after this one (0x412f3e91; 7F the
#                                                       same as now), on every change (polled at 20 Hz) and in each resync
#                    03 SWITCH <from> <to> <blk4>       the moment (0x4009bb2a); the new pattern's step-0 ticks follow
#                    05 MUTES <mask lo7> <mask hi6>     bit t = track t
#                    06 REC <live 0/1> <grid 0/1>
#                    07 TEMPO <t2 t1 t0>                bpm x 120 as 21 bits, 3 x 7, most significant first
#                    08 SEQMODE <on 0/1> <track 0..11 | 7F>   on = the current track is in the mask and GRID REC is on:
#                                                       T1..T16 (0x00..0x0F) and PAGE (0x84) come as 31 (press and
#                                                       release) with the page closed, A..H as 32 while a caught trig
#                                                       is held; the host opens its SEQ page (3A 0 1) meanwhile
#                    09 POS <blk4> <playing 0/1/2> <master step & 7F> <dropped ticks 0..127, saturated>   once a second
#                                                       while playing, and in each resync (master step: display only)
#                    04 SCALE <track 0..11 | 7F the pattern> <len lo7> <len hi7> <speed 0..6> <mode> [<chg lo7> <chg hi7>]
#                                                       (seq-p2 / rytm-mods 1033) the working pattern's lengths and speeds,
#                                                       polled at 20 Hz: on a change and in each resync (13 of them); a
#                                                       track's len is its own (+0x2c5), the pattern's its master length;
#                                                       mode 1 = per-track lengths (then a queued pattern starts at a
#                                                       multiple of chg [code]); chg only on the pattern's
#                    0A EDIT <op> <pattern> <track | 7F> <ok 0/1> <src 0 stock / 1 seq mode> <a0> <a1> <a2>   (1033) a
#                                                       stock edit function ran (src 0: op after it returned, ok its result)
#                                                       or seq mode's own gesture (src 1: a host-held trig / PAGE + FUNC +
#                                                       RECORD / STOP / PLAY, caught); a = a0 | a1 << 7 | a2 << 14: the
#                                                       undo's kind (1 clear, 2 paste), the held trig mask (src 1), else 0.
#                                                       ops SEQ_EDIT_OPS: copy / paste / clear / undo x pattern / track /
#                                                       page, trigs copy / paste / clear; pattern = the working pattern
#                    0B DIRTY <pattern>                 (1033) the working pattern's bytes changed (a rolling checksum,
#                                                       about 1 s; at most every 200 ms): refetch its dump
#                    0C FILL <held 0/1>                 (1033) the FILL key
#                    0D SOLO <mask lo7> <mask hi6>      (1049) the soloed tracks, bit t = track t (0x407c23c5, BE word),
#                                                       polled as 05 and in each resync after it; none soloed: 00 00. The
#                                                       host silences the tracks not soloed as it does the muted ones; an
#                                                       older host ignores 0D, an older Rytm never sends it
#   host -> Rytm  2E 01 <mask lo7: tracks 0..6> <mask hi5: tracks 7..11> <flags> [<clock lo7> <clock hi5>]
#                 the tracks the Sidecar sequences (their trig keys / LEDs in seq mode, live record writes no stock trig
#                 on them; pads still send notes) and optionally clock-only tracks (ticked in 2C, nothing else); hi bytes
#                 above 0x1F refused. flags bit 1: resync now (2D 08 01 02 05 06 07 09 at once), bit 0 reserved (0).
#                 Any 2E (mask 0 too) turns the reports on; the first after a hello gets a full resync. The Rytm forgets
#                 it at its hello (35) and when the port drops: sent again after each 35.
#                 (1033) 2E may carry <pin lo7> <pin hi5> after the clock bytes (7 bytes): more tracks whose voices are
#                 pinned (the analog pin: amp held open, filter envelope 0, VCA at velocity 127: the host's poly
#                 tracks); every seq track is pinned unless flags bit 2. Older builds take 3 / 5 bytes only.
#                 46 01 <silent lo7> <silent hi5>   (1033) the pinned tracks on which nothing of the host's sounds: their
#                 VCA closes (stock's decay) until a 46 without them (one start, then held again)
#                 2F 01 <p1> <p2> <p3> <p4>   the 4 PAGE LEDs, page 1..4 (panel LEDs 43 42 41 40), palette 0..111 as
#                 34's, 7F stock's; honoured while seq mode is on or the remote page is open; only a 2F / 34 sent after
#                 the host took the LEDs counts, seq mode keeps them across 3A open / close. The trig LEDs: 34.
SEQ_TICK, SEQ_STATE, SEQ_MASK, SEQ_PAGE_LEDS = 0x2C, 0x2D, 0x2E, 0x2F
SEQ_TRANSPORT, SEQ_PATTERN, SEQ_SWITCH, SEQ_MUTES, SEQ_REC, SEQ_TEMPO, SEQ_SEQMODE, SEQ_POS = 1, 2, 3, 5, 6, 7, 8, 9
SEQ_SCALE, SEQ_EDIT, SEQ_DIRTY, SEQ_FILL, SEQ_SOLO = 4, 0x0A, 0x0B, 0x0C, 0x0D
SEQ_SILENT = 0x46
SEQ_FLAG_NO_PIN = 4
SEQ_EDIT_OPS = {1: "COPY PAT", 2: "PASTE PAT", 3: "CLEAR PAT", 4: "UNDO PAT", 5: "COPY TRK", 6: "PASTE TRK",
                7: "CLEAR TRK", 8: "UNDO TRK", 9: "COPY PAGE", 10: "PASTE PAGE", 11: "CLEAR PAGE", 12: "UNDO PAGE",
                13: "COPY TRIGS", 14: "PASTE TRIGS", 15: "CLEAR TRIGS",
                16: "RELOAD PAT", 17: "SAVE PAT", 18: "RELOAD TRK", 19: "SAVE TRK"}   # 1042 (rytm-mods 0026)
SEQ_STOP, SEQ_PLAY, SEQ_PAUSE = 0, 1, 2
SEQ_NONE = 0x7F
SEQ_FLAG_RESYNC = 2
SEQ_SPEEDS = (2.0, 1.5, 1.0, 0.75, 0.5, 0.25, 0.125)   # the Rytm's speed index -> steps per 1x step
SEQ_BLOCK_FRAMES = 32   # blk4 counts 32-frame audio blocks
SEQ_SUB = 96            # the host's sub-step unit (lengths, micro timing): 1/96 of a step


def blk4(b):
    b &= (1 << 28) - 1
    return [(b >> 21) & 0x7F, (b >> 14) & 0x7F, (b >> 7) & 0x7F, b & 0x7F]


def unblk4(p):
    return p[0] << 21 | p[1] << 14 | p[2] << 7 | p[3]


def seq_tick(blk, ticks) -> bytes:
    """2C: ticks = [(track, step, len, speed), ...] fired in audio block blk."""
    body = blk4(blk) + [len(ticks)]
    for t, s, ln, sp in ticks:
        body += [t & 0x7F, s & 0x7F, ln & 0x7F, sp & 0x7F]
    return _msg(SEQ_TICK, body)


def seq_transport(state, blk) -> bytes:
    return _msg(SEQ_STATE, [SEQ_TRANSPORT, state] + blk4(blk))


def seq_pattern(now, nxt=SEQ_NONE) -> bytes:
    return _msg(SEQ_STATE, [SEQ_PATTERN, now & 0x7F, nxt & 0x7F])


def seq_switch(frm, to, blk) -> bytes:
    return _msg(SEQ_STATE, [SEQ_SWITCH, frm & 0x7F, to & 0x7F] + blk4(blk))


def seq_mutes(mask) -> bytes:
    return _msg(SEQ_STATE, [SEQ_MUTES, mask & 0x7F, (mask >> 7) & 0x3F])


def seq_solo(mask) -> bytes:   # 1049
    return _msg(SEQ_STATE, [SEQ_SOLO, mask & 0x7F, (mask >> 7) & 0x3F])


def seq_rec(live, grid) -> bytes:
    return _msg(SEQ_STATE, [SEQ_REC, 1 if live else 0, 1 if grid else 0])


def seq_tempo(bpm) -> bytes:
    t = int(round(bpm * 120)) & ((1 << 21) - 1)
    return _msg(SEQ_STATE, [SEQ_TEMPO, t >> 14, (t >> 7) & 0x7F, t & 0x7F])


def seq_seqmode(on, track=SEQ_NONE) -> bytes:
    return _msg(SEQ_STATE, [SEQ_SEQMODE, 1 if on else 0, track & 0x7F])


def seq_pos(blk, playing, master_step, dropped=0) -> bytes:
    return _msg(SEQ_STATE, [SEQ_POS] + blk4(blk) + [playing, master_step & 0x7F, min(dropped, 127)])


def seq_mask(mask, flags=0, clock=None, pin=None) -> bytes:
    """2E (host -> Rytm): mask bit t = track t (0..11); clock: the clock-only tracks (None: not sent); pin: more pinned
    tracks (1033; None: not sent, the 3 / 5-byte form older builds take)."""
    body = [mask & 0x7F, (mask >> 7) & 0x1F, flags & 0x7E]
    if clock is not None or pin is not None:
        c = clock or 0
        body += [c & 0x7F, (c >> 7) & 0x1F]
    if pin is not None:
        body += [pin & 0x7F, (pin >> 7) & 0x1F]
    return _msg(SEQ_MASK, body)


def seq_silent(mask) -> bytes:
    """46 (host -> Rytm, 1033): the pinned tracks with nothing sounding (their VCA closed)."""
    return _msg(SEQ_SILENT, [mask & 0x7F, (mask >> 7) & 0x1F])


def seq_scale(track, length, speed, mode, chg=None) -> bytes:
    """2D 04 (Rytm -> host): track 0..11 or SEQ_NONE (the pattern, with chg)."""
    body = [SEQ_SCALE, track & 0x7F, length & 0x7F, (length >> 7) & 0x7F, speed & 0x7F, mode & 0x7F]
    if track == SEQ_NONE:
        c = chg or 0
        body += [c & 0x7F, (c >> 7) & 0x7F]
    return _msg(SEQ_STATE, body)


def seq_edit(op, pattern, track=SEQ_NONE, ok=1, src=0, arg=0) -> bytes:
    """2D 0A (Rytm -> host): an edit (SEQ_EDIT_OPS)."""
    return _msg(SEQ_STATE, [SEQ_EDIT, op, pattern & 0x7F, track & 0x7F, 1 if ok else 0, src & 1, arg & 0x7F,
                            (arg >> 7) & 0x7F, (arg >> 14) & 0x7F])


def seq_dirty(pattern) -> bytes:
    return _msg(SEQ_STATE, [SEQ_DIRTY, pattern & 0x7F])


def seq_fill(held) -> bytes:
    return _msg(SEQ_STATE, [SEQ_FILL, 1 if held else 0])


def seq_page_leds(c) -> bytes:
    """2F (host -> Rytm): the 4 PAGE LEDs (palette 0..111, SEQ_NONE = stock's)."""
    return _msg(SEQ_PAGE_LEDS, [x & 0x7F for x in c[:4]])


def decode_seq(cmd, payload):
    """A 2C / 2D / 2E / 2F payload (after the id) -> dict; None if malformed (core/seq/seq.cpp takes the same)."""
    if not payload or payload[0] != PROTO_VER:
        return None
    b = payload[1:]
    if cmd == SEQ_TICK:
        if len(b) < 5 or len(b) != 5 + 4 * b[4] or b[4] < 1:
            return None
        ticks = [tuple(b[5 + 4 * i:9 + 4 * i]) for i in range(b[4])]
        if any(t[0] > 11 or t[3] > 6 for t in ticks):
            return None
        return dict(kind="tick", blk=unblk4(b), ticks=ticks)
    if cmd == SEQ_STATE and b:
        k, a = b[0], b[1:]
        if k == SEQ_TRANSPORT and len(a) == 5 and a[0] <= 2:
            return dict(kind="transport", state=a[0], blk=unblk4(a[1:]))
        if k == SEQ_PATTERN and len(a) == 2:
            return dict(kind="pattern", now=a[0], next=None if a[1] == SEQ_NONE else a[1])
        if k == SEQ_SWITCH and len(a) == 6:
            return dict(kind="switch", frm=a[0], to=a[1], blk=unblk4(a[2:]))
        if k == SEQ_MUTES and len(a) == 2:
            return dict(kind="mutes", mask=a[0] | a[1] << 7)
        if k == SEQ_REC and len(a) == 2:
            return dict(kind="rec", live=a[0], grid=a[1])
        if k == SEQ_TEMPO and len(a) == 3:
            return dict(kind="tempo", bpm=(a[0] << 14 | a[1] << 7 | a[2]) / 120)
        if k == SEQ_SEQMODE and len(a) == 2:
            return dict(kind="seqmode", on=a[0], track=None if a[1] == SEQ_NONE else a[1])
        if k == SEQ_POS and len(a) == 7:
            return dict(kind="pos", blk=unblk4(a), playing=a[4], master_step=a[5], dropped=a[6])
        if k == SEQ_SCALE and (len(a) == 5 and a[0] < 12 or len(a) == 7 and a[0] == SEQ_NONE):
            d = dict(kind="scale", track=None if a[0] == SEQ_NONE else a[0], length=a[1] | a[2] << 7, speed=a[3],
                     mode=a[4])
            if len(a) == 7:
                d["chg"] = a[5] | a[6] << 7
            return d
        if k == SEQ_EDIT and len(a) == 8 and a[0] in SEQ_EDIT_OPS:
            return dict(kind="edit", op=a[0], name=SEQ_EDIT_OPS[a[0]], pattern=a[1],
                        track=None if a[2] == SEQ_NONE else a[2], ok=a[3], src=a[4], arg=a[5] | a[6] << 7 | a[7] << 14)
        if k == SEQ_DIRTY and len(a) == 1:
            return dict(kind="dirty", pattern=a[0])
        if k == SEQ_FILL and len(a) == 1:
            return dict(kind="fill", held=a[0])
        if k == SEQ_SOLO and len(a) == 2:
            return dict(kind="solo", mask=a[0] | a[1] << 7)
        return None
    if cmd == SEQ_MASK and len(b) in (3, 5, 7) and b[1] <= 0x1F and (len(b) == 3 or b[4] <= 0x1F) and \
            (len(b) < 7 or b[6] <= 0x1F):
        return dict(kind="mask", mask=b[0] | b[1] << 7, flags=b[2], clock=b[3] | b[4] << 7 if len(b) >= 5 else None,
                    pin=b[5] | b[6] << 7 if len(b) == 7 else None)
    if cmd == SEQ_SILENT and len(b) == 2 and b[1] <= 0x1F:
        return dict(kind="silent", mask=b[0] | b[1] << 7)
    if cmd == SEQ_PAGE_LEDS and len(b) == 4:
        return dict(kind="page_leds", leds=list(b))
    return None


# ---- The Rytm's own pattern, by the stock SysEx dump (seq-p2; verified in rytm-emu on OS 1.73, rytm-mods sq_emu.py
# "fetch"; the format from public community knowledge, no code copied). The request for pattern n; the active pattern's
# answer is its working buffer as edited, saved or not [measured]; 0x80 | n answers the same but is not a MIDI data byte
# (a byte-stream MIDI path cuts the SysEx there [measured: rytm-emu's link]): ask for the active pattern by number:
#   F0 00 20 3C 07 00 64 01 01 <n> 00 00 00 05 F7
# The answer (14988 bytes): F0 00 20 3C 07 00 54 01 01 <pattern 0..127> 00 <00 00 00 05: the record's 4-byte header>
#   <the record's other 13097 bytes, 7-bit packed: 14968 bytes> <checksum hi7 lo7: the sum of the bytes from <pattern>
#   up to the checksum, & 0x3FFF> <length hi7 lo7: the message's size - 10> F7
# The record: 13101 bytes (docs/TOUCH-EDITOR.md, pi/ui/rytm_pattern.h's layout).
RYTM_DUMP_REQ = bytes.fromhex("f000203c0700640101800000" "0005f7")   # the 0x80 form (raw USB-MIDI paths only)


def rytm_dump_request(pattern):
    """the request for pattern 0..127 (the active one: its working buffer)"""
    return RYTM_DUMP_REQ[:9] + bytes([pattern & 0x7F]) + RYTM_DUMP_REQ[10:]
RYTM_RECORD = 13101


def rytm_pack7(raw):
    """Elektron's 7-bit packing: each group of up to 7 bytes after a byte of their top bits (the first byte's in bit 6)."""
    out = bytearray()
    for i in range(0, len(raw), 7):
        g = raw[i:i + 7]
        out.append(sum(((b >> 7) & 1) << (6 - k) for k, b in enumerate(g)))
        out += bytes(b & 0x7F for b in g)
    return bytes(out)


def rytm_unpack7(p):
    out = bytearray()
    for i in range(0, len(p), 8):
        hi = p[i]
        out += bytes(b | ((hi << (k + 1)) & 0x80) for k, b in enumerate(p[i + 1:i + 8]))
    return bytes(out)


def rytm_dump(record, pattern=0) -> bytes:
    """A stock pattern dump of a 13101-byte record (as the Rytm answers RYTM_DUMP_REQ)."""
    body = bytes([pattern & 0x7F, 0]) + record[:4] + rytm_pack7(record[4:])
    cs = sum(body[1:]) & 0x3FFF   # the pattern number not in it [measured on a real Rytm MKII]
    m = bytes([0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x54, 0x01, 0x01]) + body + bytes([cs >> 7, cs & 0x7F])
    n = len(m) + 3 - 10
    return m + bytes([n >> 7 & 0x7F, n & 0x7F, 0xF7])


def rytm_dump_parse(m):
    """A stock pattern dump -> (pattern, the 13101-byte record), None when it is not one or its checksum / length is
    wrong."""
    if len(m) < 20 or m[:9] != bytes([0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x54, 0x01, 0x01]) or m[-1] != 0xF7:
        return None
    if (m[-5] << 7 | m[-4]) != sum(m[10:-5]) & 0x3FFF or (m[-3] << 7 | m[-2]) != len(m) - 10:
        return None
    rec = bytes(m[11:15]) + rytm_unpack7(m[15:-5])
    return (m[9], rec) if len(rec) == RYTM_RECORD else None


# The pattern file: /.rytm/seq/<project>/pNNN.sqp, NNN = 000..127 (A01 = 000), little-endian, sparse:
#   header  'SQP1' <u8 version 1> <u8 ntracks> <u16 flags 0> <u32 crc32 of everything after the header>
#   track   <u8 rytm track 0..11> <u8 machine hint (voice byte & 0x78, 0x7F unknown)> <u16 length: 0 AUTO (the Rytm
#           track's), else 1..128 steps> <u8 speed 0 (follow the Rytm)> <u8 swing: 0 straight, 51..80 (% of a step
#           pair, Elektron's scale: every second step late by swing / 50 - 1 of a step; seq-p2)> <u8 default note> <u8 default vel>
#           <u8 default len (1/96 step units / 4)> <u16 steps used>, then the used steps, ascending:
#   step    <u16 index 0..127> <u8 flags: bit 0 trig on, bit 1 mute> <s8 strum (1/96 step a note, the CHD entry's)>
#           <u8 chd 0..15> <u8 inv> <u8 prob 0..100 (100)> <u8 notes 1..8>, then each note:
#           <u8 pitch> <u8 vel> <u16 len (1/96 step)> <s8 micro (-96..96, 1/96 step from the step's start)>, then
#           <u8 locks 0> (reserved)
#   end     'END\0'
# A step's notes keep their own micro offsets: a broken chord / strum is its notes at 0, +s, +2s ...; loose timing is any
# micro. Unknown versions are refused (an empty pattern).
# v2 (seq-ux, rui1036): a step's locks are 3 bytes {param, value lo, value hi} (value 14 bits: int << 7 | frac; param 0..15
# the machine's P1..P16, 16..23 FX1 / FX2 P2..P5); a step with locks and no notes is stored (a lock trig). v1 files
# (locks {param, value}, never written) still read, a v1 lock's value as the integer part.
SQP_MAGIC, SQP_VERSION, SQP_END = b"SQP1", 4, b"END\x00"   # rui1040: v3 = flags bits 2..7 the trig condition
# seqx (E2): v4 = v3 + an optional trailer of tagged blocks before END: b"XB" <u16 count>, then per block <u8 id>
# <u8 flags> <u16 len> <payload> (core/seq/pattern.cpp). Written only when there is a block (else v2 / v3 as before).
SQP_BLK_SEED, SQP_BLK_TRACK_SEED, SQP_BLK_POLY = 1, 2, 3
# seqtop (docs/SEQUENCER.md "Built: seqtop"): blocks that change how the pattern plays, written required (flags 1 | 4):
#   4 a track's speed: <track> <id: SEQ_SPEEDS' index, 0 FOLLOW>
#   5 a track's play direction: <track> <id: SEQ_DIRS' index, 0 FWD>
#   6 the pattern's key and scale: <key 0..11> <scale: SEQ_SCALES' index, 0 CHR> <qnt 0 OFF 1 EDIT 2 PLAY> (required only
#     with PLAY: the others are an edit aid)
#   7 a track's random notes (R1): <track> <mode 0 OFF 1 STEPS 2 TRACK> <low note> <octaves 1..4> <weight: SEQ_WEIGHTS'
#     index> <16 bytes: the steps whose notes are random, bit s % 8 of byte s / 8> (per-step)
#   8 a track's SEED MOD (R1b): <track> <src 0 off, 1..16 the voice's P1..P16> <range 2..64> <quant 0 STEP 1 LOOP 2 BAR>
#     <affects 0 NOTES 1 +CHANCE 2 +ORDER 3 ALL>
#   9 a track's conditional locks (V1): <track> {<step> <param> <cond: the COND knob's code, not PRE / NEI>}.. (per-step)
#  10 a track's ratchets / arps (P4 / M4): <track> {<step> <cnt> <rate> <vcrv> <pcrv s8> <mode> <oct> <gate>}.. (per-step;
#     SEQ_RT_MODES, SEQ_RT_RATES, SEQ_VCRVS)
SQP_BLK_SPEED, SQP_BLK_DIR, SQP_BLK_SCALE, SQP_BLK_RND, SQP_BLK_SMOD, SQP_BLK_LOCK_COND = 4, 5, 6, 7, 8, 9
#  11 a track's sliding locks (X1): <track> {<step> <param>}.. (per-step; a step's slide=[param, ..], params it locks)
#  12 a track's FX column (H1): <track> {<step> <cmd 1> <val 1> <cmd 2> <val 2>}.. (per-step; any step, an empty one's
#     jump too: a step's fx=((cmd, val), (cmd, val)); SEQ_FX_CMDS; OCT / TRS signed, LOP (x - 1) << 4 | (n - 1))
SQP_BLK_RTRG, SQP_BLK_SLIDE, SQP_BLK_FX = 10, 11, 12
#  13 a track's poly flag in this pattern (per pattern poly, 2026-10-26): <track> <on 0|1>, not required; written only
#     where the rule for a file without it (a track holding steps is on, an empty one off) would read it otherwise
#     (a track's on=0|1)
SQP_BLK_ON = 13
SEQ_FX_CMDS = ("---", "RTG", "DLY", "CUT", "CHN", "RND", "RNV", "OCT", "TRS", "JMP", "JRN", "LOP", "SKP", "DIR", "FIL")
SEQ_RT_MODES = ("CHORD", "WALK", "UP", "DOWN", "UPDN", "RND", "PLAYED", "CONV")
SEQ_RT_RATES = ("AUTO", "2", "1", "2/3", "1/2", "1/3", "1/4", "1/6", "1/8")
SEQ_VCRVS = ("FLAT", "UP", "DOWN", "UPDN")
SEQ_RND_MODES = ("OFF", "STEPS", "TRACK")
SEQ_WEIGHTS = ("FLAT", "ROOT", "CHORD", "LOW", "HIGH", "STEP")
SEQ_RND_DEFAULT = (0, 48, 2, 0)            # mode, low, octaves, weight: no block
SEQ_SMOD_DEFAULT = (0, 16, 1, 0)           # src, range, quant (LOOP: Q10), affects
SEQ_SCALES = ("CHR", "MAJ", "MIN", "DOR", "PENT", "PHR", "LYD", "MIX", "LOC", "HMIN", "MMIN", "PMAJ", "BLUES", "WHOLE")
SEQ_SCALE_MASKS = (0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x4A9, 0x5AB, 0xAD5, 0x6B5, 0x56B, 0x9AD, 0xAAD, 0x295, 0x4E9, 0x555)
SEQ_DIRS = ("FWD", "REV", "PEND", "PING", "RND", "BROWN")   # P1 (core/seq/rng.h kDir*)
SEQ_SPEEDS = ("FOLLOW", "1/8", "1/4", "1/2", "3/4", "3/2", "2", "2/3", "5/4", "7/8")   # P2: Sidecar steps per Rytm step
SQP_BLK_REQUIRED, SQP_BLK_PER_STEP, SQP_BLK_TRACK = 1, 2, 4
SQP_ALLOCS = ("REASSIGN", "ROTATE", "RESET")   # PolyCfg.alloc (core/seq/seq.h; the Analog Four's POLY CONFIG names)
SEQ_LOCKS, SEQ_LOCK_FX, SEQ_LOCK_INS, SEQ_LOCK_PARAMS = 12, 16, 24, 32   # fx-ins: 24..31 the INSERT params
SEQ_MAX_STEPS, SEQ_MAX_NOTES = 128, 8


def sqp_pack(tracks, seed=None, blocks=(), scale=None):
    """tracks: [dict(track, hint=0x7F, length=0, defaults=(60, 100, 24), steps={index: dict(flags=1, strum=0, chd=0,
    inv=0, prob=100, notes=[(pitch, vel, len, micro), ...][, locks=[(param, value14), ...]])}[, poly=(voices, alloc,
    unison, detune, spread, group)][, tseed=(own, seed, same)] (a lock (param, value14[, cond]))[, speed=id (SEQ_SPEEDS)][, dir=id (SEQ_DIRS)][, rnd=(mode, low, oct, weight)][, smod=(src, range, quant,
    affects)])] (a step's rnd=1: its notes random; rt=(cnt, rate, vcrv, pcrv, mode, oct, gate) its ratchet / arp) -> the file's bytes (v2; v3 with a condition; v4 with
    a block). seed: (fixed, seed) the pattern's (E1); scale: (key, scale, qnt) the pattern's (seqtop M1); blocks: more raw blocks (id, flags, payload) after the known ones
    (an unknown one, as a newer Sidecar would write it)."""
    import struct
    import zlib
    body = bytearray()
    for t in tracks:
        steps = {i: s for i, s in t.get("steps", {}).items() if s.get("notes") or s.get("locks")}
        dn, dv, dl = t.get("defaults", (60, 100, 24))
        body += struct.pack("<BBHBBBBBH", t["track"], t.get("hint", 0x7F), t.get("length", 0), 0, t.get("swing", 0), dn,
                            dv, dl // 4, len(steps))
        for i in sorted(steps):
            s = steps[i]
            ns = s["notes"][:SEQ_MAX_NOTES]
            body += struct.pack("<HBbBBBB", i, s.get("flags", 1), s.get("strum", 0), s.get("chd", 0), s.get("inv", 0),
                                s.get("prob", 100), len(ns))
            for p, v, ln, mi in ns:
                body += struct.pack("<BBHb", p, v, ln, mi)
            lk = s.get("locks", [])[:SEQ_LOCKS]
            body += bytes([len(lk)])
            for prm, val, *_ in lk:
                body += struct.pack("<BH", prm, val & 0x3FFF)
    bl = []   # E2: the known blocks in the core's order (the pattern's seed, then per track its seed, its poly config)
    if seed and (seed[0] or seed[1]):
        bl.append((SQP_BLK_SEED, 0, struct.pack("<BH", 1 if seed[0] else 0, seed[1])))
    if scale and any(scale):
        bl.append((SQP_BLK_SCALE, SQP_BLK_REQUIRED if scale[2] == 2 else 0, bytes(scale)))
    for t in tracks:
        if "on" in t and bool(t["on"]) != any(s.get("notes") or s.get("locks") for s in t.get("steps", {}).values()):
            bl.append((SQP_BLK_ON, SQP_BLK_TRACK, bytes([t["track"], 1 if t["on"] else 0])))
        ts = t.get("tseed")
        if ts and any(ts):
            bl.append((SQP_BLK_TRACK_SEED, SQP_BLK_TRACK, struct.pack("<BBHB", t["track"], 1 if ts[0] else 0, ts[1],
                                                                       1 if ts[2] else 0)))
        pc = t.get("poly")
        if pc and (pc[0] or pc[1] or pc[2] > 1 or pc[3] or pc[4] or pc[5]):
            bl.append((SQP_BLK_POLY, SQP_BLK_TRACK, bytes([t["track"]]) + bytes(pc)))
        if t.get("speed"):
            bl.append((SQP_BLK_SPEED, SQP_BLK_TRACK | SQP_BLK_REQUIRED, bytes([t["track"], t["speed"]])))
        if t.get("dir"):
            bl.append((SQP_BLK_DIR, SQP_BLK_TRACK | SQP_BLK_REQUIRED, bytes([t["track"], t["dir"]])))
        rm = bytearray(16)
        for i, st in t.get("steps", {}).items():
            if st.get("rnd") and st.get("notes"):   # (as the core: a flag on a step with notes)
                rm[i // 8] |= 1 << (i % 8)
        rnd = tuple(t.get("rnd", SEQ_RND_DEFAULT))
        if rnd != SEQ_RND_DEFAULT or any(rm):
            bl.append((SQP_BLK_RND, SQP_BLK_TRACK | SQP_BLK_REQUIRED | SQP_BLK_PER_STEP,
                       bytes([t["track"]]) + bytes(rnd) + bytes(rm)))
        lc = b"".join(bytes([i, x[0], x[2]]) for i, st in sorted(t.get("steps", {}).items())
                      for x in st.get("locks", [])[:SEQ_LOCKS] if len(x) > 2 and x[2] and (st.get("notes") or st.get("locks")))
        if lc:
            bl.append((SQP_BLK_LOCK_COND, SQP_BLK_TRACK | SQP_BLK_REQUIRED | SQP_BLK_PER_STEP, bytes([t["track"]]) + lc))
        sl = b"".join(bytes([i, x[0]]) for i, st in sorted(t.get("steps", {}).items())
                      for x in st.get("locks", [])[:SEQ_LOCKS] if x[0] in st.get("slide", ()))
        if sl:
            bl.append((SQP_BLK_SLIDE, SQP_BLK_TRACK | SQP_BLK_REQUIRED | SQP_BLK_PER_STEP, bytes([t["track"]]) + sl))
        fx = b"".join(bytes([i, st["fx"][0][0], st["fx"][0][1] & 255, st["fx"][1][0], st["fx"][1][1] & 255])
                      for i, st in sorted(t.get("steps", {}).items()) if st.get("fx") and (st["fx"][0][0] or st["fx"][1][0]))
        if fx:
            bl.append((SQP_BLK_FX, SQP_BLK_TRACK | SQP_BLK_REQUIRED | SQP_BLK_PER_STEP, bytes([t["track"]]) + fx))
        rt = b"".join(struct.pack("<BBBBbBBB", i, *st["rt"]) for i, st in sorted(t.get("steps", {}).items())
                      if st.get("notes") and st.get("rt") and (st["rt"][0] >= 2 or st["rt"][4] >= 2))
        if rt:
            bl.append((SQP_BLK_RTRG, SQP_BLK_TRACK | SQP_BLK_REQUIRED | SQP_BLK_PER_STEP, bytes([t["track"]]) + rt))
        sm = tuple(t.get("smod", SEQ_SMOD_DEFAULT))
        if sm != SEQ_SMOD_DEFAULT:
            bl.append((SQP_BLK_SMOD, SQP_BLK_TRACK | SQP_BLK_REQUIRED, bytes([t["track"]]) + bytes(sm)))
    bl += list(blocks)
    if bl:
        body += b"XB" + struct.pack("<H", len(bl))
        for bid, fl, pay in bl:
            body += struct.pack("<BBH", bid, fl, len(pay)) + bytes(pay)
    body += SQP_END
    ver = 3 if any(s.get("flags", 1) >> 2 and (s.get("notes") or s.get("locks")) for t in tracks
                   for s in t.get("steps", {}).values()) else 2   # (no condition: v2, as the core writes)
    if bl:
        ver = SQP_VERSION
    return SQP_MAGIC + struct.pack("<BBHI", ver, len(tracks), 0, zlib.crc32(bytes(body))) + bytes(body)


def sqp_unpack(data, extra=None):
    """The file's bytes -> tracks as sqp_pack takes them (a step's locks only when it has some; v4: a track's poly and
    tseed); None when it is not a valid version 1..4 file. extra (a dict): gets "seed" (fixed, seed) and "blocks" [(id,
    flags, payload)] - the blocks this mirror does not know, as sqp_pack's blocks takes them; "scale" (key, scale, qnt)."""
    import struct
    import zlib
    if len(data) < 12 or data[:4] != SQP_MAGIC:
        return None
    ver, nt, _, crc = struct.unpack_from("<BBHI", data, 4)
    body = data[12:]
    if ver not in (1, 2, 3, SQP_VERSION) or zlib.crc32(body) != crc or body[-4:] != SQP_END:
        return None
    o, out = 0, []
    for _ in range(nt):
        tr, hint, length, _, swing, dn, dv, dl, ns = struct.unpack_from("<BBHBBBBBH", body, o)
        o += 11
        steps = {}
        for _ in range(ns):
            i, fl, strum, chd, inv, prob, nn = struct.unpack_from("<HBbBBBB", body, o)
            o += 8
            notes = []
            for _ in range(nn):
                notes.append(struct.unpack_from("<BBHb", body, o))
                o += 5
            nl, o = body[o], o + 1
            locks = []
            for _ in range(nl):
                if ver >= 2:
                    locks.append(struct.unpack_from("<BH", body, o))
                    o += 3
                else:
                    locks.append((body[o], body[o + 1] << 7))
                    o += 2
            steps[i] = dict(flags=fl, strum=strum, chd=chd, inv=inv, prob=prob, notes=[tuple(n) for n in notes])
            if locks:
                steps[i]["locks"] = [tuple(x) for x in locks]
        t = dict(track=tr, hint=hint, length=length, defaults=(dn, dv, dl * 4), steps=steps)
        if swing:
            t["swing"] = swing
        out.append(t)
    if ver >= 4 and body[o:o + 2] == b"XB":   # E2: the blocks (known ones onto their tracks; others in "blocks")
        nb, o = struct.unpack_from("<H", body, o + 2)[0], o + 4
        seed, more, scale = None, [], None
        for _ in range(nb):
            bid, fl, ln = struct.unpack_from("<BBH", body, o)
            pay, o = bytes(body[o + 4:o + 4 + ln]), o + 4 + ln
            tr = next((t for t in out if fl & SQP_BLK_TRACK and pay and t["track"] == pay[0]), None)
            if bid == SQP_BLK_SEED:
                seed = (pay[0], pay[1] | pay[2] << 8)
            elif bid == SQP_BLK_SCALE:
                scale = tuple(pay[:3])
            elif bid == SQP_BLK_TRACK_SEED and tr is not None:
                tr["tseed"] = (pay[1], pay[2] | pay[3] << 8, pay[4])
            elif bid == SQP_BLK_ON and tr is not None:
                tr["on"] = pay[1]
            elif bid == SQP_BLK_POLY and tr is not None:
                tr["poly"] = tuple(pay[1:7])
            elif bid == SQP_BLK_SPEED and tr is not None:
                tr["speed"] = pay[1]
            elif bid == SQP_BLK_DIR and tr is not None:
                tr["dir"] = pay[1]
            elif bid == SQP_BLK_RND and tr is not None:
                if tuple(pay[1:5]) != SEQ_RND_DEFAULT:
                    tr["rnd"] = tuple(pay[1:5])
                for i in range(128):
                    if pay[5 + i // 8] >> (i % 8) & 1 and i in tr["steps"]:
                        tr["steps"][i]["rnd"] = 1
            elif bid == SQP_BLK_SMOD and tr is not None:
                tr["smod"] = tuple(pay[1:5])
            elif bid == SQP_BLK_FX and tr is not None:
                for e in range(1, len(pay) - 4, 5):
                    st = tr["steps"].setdefault(pay[e], dict(flags=1, strum=0, chd=0, inv=0, prob=100, notes=[]))
                    sgn = lambda c, v: v - 256 if c in (7, 8) and v > 127 else v   # OCT / TRS
                    st["fx"] = ((pay[e + 1], sgn(pay[e + 1], pay[e + 2])), (pay[e + 3], sgn(pay[e + 3], pay[e + 4])))
            elif bid == SQP_BLK_SLIDE and tr is not None:
                for e in range(1, len(pay) - 1, 2):
                    if pay[e] in tr["steps"]:
                        tr["steps"][pay[e]].setdefault("slide", []).append(pay[e + 1])
            elif bid == SQP_BLK_RTRG and tr is not None:
                for e in range(1, len(pay) - 7, 8):
                    if pay[e] in tr["steps"]:
                        tr["steps"][pay[e]]["rt"] = struct.unpack_from("<BBBbBBB", pay, e + 1)
            elif bid == SQP_BLK_LOCK_COND and tr is not None:
                for e in range(1, len(pay) - 2, 3):
                    st = tr["steps"].get(pay[e])
                    if st and "locks" in st:
                        st["locks"] = [(x[0], x[1], pay[e + 2]) if x[0] == pay[e + 1] else x for x in st["locks"]]
            else:
                more.append((bid, fl, pay))
        if extra is not None:
            extra["seed"], extra["blocks"] = seed, more
            if scale:
                extra["scale"] = scale
    return out


def seq_strum(notes, strum):
    """A broken chord: the notes (pitch, vel, len, micro) of a step re-timed at micro 0, strum, 2 strum ... from the
    lowest (strum < 0: from the highest), clamped to -96..96 (core/seq: the STR encoder)."""
    order = sorted(range(len(notes)), key=lambda i: notes[i][0], reverse=strum < 0)
    out = list(notes)
    for k, i in enumerate(order):
        p, v, ln, _ = notes[i]
        out[i] = (p, v, ln, max(-96, min(96, k * abs(strum))))
    return out


# ---- rui1036 (rytm-mods 0026 "1036"): the source lists' names and the host's parameter write
RUI_NAMES, RUI_WRITE = 0x47, 0x48
RUI_PAGE_INS_OUT = 3         # 30's page 3 (rui1047, UP + AMP): the INSERT page on its OUT view (an older host: SIDECAR)
RUI_PAGE_SEQ = 0x10          # 30's page 0x10 + t: TRK + TRIG (rui1036: FUNC + LFO), the SEQ page of track t
RUI_PAGE_POLY = 0x20         # 30's page 0x20 + t (rui1048): POLY CONFIG of track t (the Rytm's track menu item, FUNC + CHRO;
                             # an older host: SIDECAR)


def rui_names(lst, replace, total, first, names) -> bytes:
    """host -> Rytm 47 <list 0 GRANULAR SRC / 1 WAVETABLE TBL / 2 MULTISAMPLE INST> <replace> <total> {<slot> <13 name
    bytes>} x <= 8: the Rytm's value text of that knob at value v is name[min(v, total - 1)] (9 characters kept)."""
    body = [lst & 3, 1 if replace else 0, total & 0x7F]
    for i, n in enumerate(names[:8]):
        body += [(first + i) & 0x7F] + list(n.upper().encode("ascii", "replace")[:13].ljust(13, b"\0"))
    return _msg(RUI_NAMES, body)


def rui_write(track, param, value, frac=None, flags=None) -> bytes:
    """host -> Rytm 48 <track 0..11> <param 0..15> <value> [<frac> [<flags>]]: P(param + 1) of the track's sound set as a
    knob turn sets it (kit and live sound). The Rytm answers 48 <track> <param> <status 0 written, 1 refused, 2 no such
    param; rui1038: 3 the pattern's lock slots full, 4 locked>. rui1038: flags bit 0 - the bank list's commit: the steps
    held at its YES get a lock of P1 instead of the kit."""
    tail = ([(frac or 0) & 0x7F] if frac is not None or flags is not None else []) + ([flags & 0x7F] if flags is not None else [])
    return _msg(RUI_WRITE, [track, param, value & 0x7F] + tail)


# ---- rui1038 (rytm-mods 0026 "1038", docs/SRC-BANKS.md): the source banks
RUI_BANK_LIST = 0x4A
BANK_WHY = {0: "open", 1: "yes", 2: "no", 3: "knob", 4: "key", 5: "view", 6: "srcyes"}   # 6 (rui1049, on 3): FUNC + YES
#                                                                                     on the SRC view: the host aims an OFF
#                                                                                     entry at the bank's first free row


def src_code(index, bank) -> int:
    """GRANULAR SRC / WAVETABLE TBL / MULTISAMPLE INST: bank 0..7 (A..H) * 128 + index 0..127 (the word index << 8 |
    bank << 5: on the wire hi = index, lo = bank << 4)."""
    return (bank & 7) * 128 + (index & 127)


def src_wire(code) -> tuple:
    """the source code -> (hi, lo) as 48 / 2A / 03 carry it"""
    return code & 127, (code >> 7 & 7) << 4


def src_of_wire(hi, lo) -> int:
    return ((lo & 0x7F) >> 4) * 128 + (hi & 127)


def rui_names_bank(lst, replace, bank, names) -> bytes:
    """host -> Rytm 47 <list> <replace> <127> <bank 1..7> {<row> <13 name bytes>} x <= 8: bank B..H's names (the Rytm
    keeps one bank a list; rows without a name read as B005 / OFF). names: [(row, name)]."""
    body = [lst & 3, 1 if replace else 0, 127, bank & 7]
    for row, n in names[:8]:
        body += [row & 0x7F] + list(n.upper().encode("ascii", "replace")[:13].ljust(13, b"\0"))
    return _msg(RUI_NAMES, body)


def decode_bank_list(payload):
    """Rytm -> host 4A's payload (<ver> <track> <on> <hi> <lo> <why>, as parse() gives it) -> dict(track, on, code, why)
    or None"""
    if not payload or payload[0] != PROTO_VER:
        return None
    b = payload[1:]
    if len(b) != 5 or b[0] > 11 or (b[1] > 1 and b[1] != 3):   # rui1040: on 3 = FUNC + YES (the row's files)
        return None
    return dict(track=b[0], on=b[1], code=src_of_wire(b[2], b[3]), why=BANK_WHY.get(b[4], b[4]))


def rui_bank_browse(track, on):
    """rui1040, host -> Rytm 4A: <track> <browse 0/1> - the host's file browser over the bank list open (YES / NO the
    host's, 31) or closed (the list's own again)."""
    return bytes([0xF0, 0x7D, 0x52, 0x54, RUI_BANK_LIST, PROTO_VER, track & 0x7F, 1 if on else 0, 0xF7])
