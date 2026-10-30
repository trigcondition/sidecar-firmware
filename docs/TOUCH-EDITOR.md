# Touch pattern editor on the Sidecar Pi's display (design + prototype, 2026-10-20)

The Pi display (800x480, touch, LVGL 9.4; docs/pi-display.md) sits right in front of the Analog Rytm MKII. This doc
covers two new pages for it:

- **A. SEQ**: an editor for the Sidecar's own poly seq tracks (core/seq, docs/SEQUENCER.md). It is a piano roll with
  touch editing, chord stamps, strum, humanise, a velocity lane and undo.
- **B. RYTM**: a read-only view of the Rytm's own pattern: 12 tracks, trigs, note, velocity, length, micro-timing,
  conditions, and the p-locks as a heatmap. It also covers where that data can come from, and a design for editing
  it later.

The title bar's **Seq** button opens SEQ. On SEQ, **RYTM >** opens the Rytm view, and its **SEQ** button goes back.

Tags (as in SEQUENCER.md / POLYPHONY.md):
- **[measured]**: run, here or in rytm-emu.
- **[inferred]**: reasoned, not checked.
- **[public]**: from public manuals or community format docs, recalled, not checked against a device.

No hardware was used. Nothing was flashed. No firmware bytes are in the code or here. The Rytm view's sample pattern
is made up (`pi/ui/host/rytm_fake.c`); only the record layout is the Rytm's (POLYPHONY.md).

## Screens (rendered by the real UI code, `make -C pi/ui/host`)

Since ui-oled the pages have two themes (docs/UI-THEMES.md): the files below are `shots/classic-*.png`; the OLED theme's
are `shots/oled-*.png` (the same states on 400x240 dots, notes in the trig LEDs' colours), and `make -C pi/ui/host anim`
renders the OLED piano roll playing (`seq-oled.mp4`, `seq-oled.gif`).

| file | what |
|---|---|
| `seq-roll.png` | The piano roll, 2 of 4 pages, playing. A strummed C minor progression (Cm7, Abmaj7, Fm7, G7: struck up on 1, broken down on the "and" of 3) and a humanised melody. The C minor scale is highlighted. |
| `seq-edit.png` | One page: a note being dragged later and up. Its old place shows as an outline. The card shows note, velocity, step, length, micro, and a delete button. |
| `seq-chord.png` | The CHORD tool: chord row, inversion, strum amount. F MIN7 was stamped by a real tap on page 2. |
| `seq-velocity.png` | All 4 pages. The selected note's velocity bar is being dragged down to 40 (a real drag through LVGL's pointer). |
| `rytm-pattern.png` | The Rytm view, page 1. CH is selected: AMP DEC / FLT FRQ / SMP STA / AMP PAN lanes. The step card shows step 4 (75 %, micro +4/24). |
| `rytm-all.png` | All 64 steps at once: the tom fill and snare roll (FILL conditions), the SD's FLT FRQ ramp. |

The host render also checks the gestures against the model and fails if one does not land: tap = add, tap on the
selected note = delete, drag = move (micro kept), UNDO, the chord stamp's four notes and their strum, the lane drag's
velocity.

## A. The SEQ editor

### Layout (page coordinates, below the 52 px title bar)

```
 y 4..52    SIDECAR SEQ  > 121.5  1-2/4  SW 62% | the seq tracks T9 T10 - - | page strip 1..8 | RYTM >
            project  pattern  track
 y 56..70   ruler: P1 5 9 13 P2 ...
 y 70..350  keys (x 8..48: C labels, scale marks) | grid (x 52..692): pitch rows 14 px, 1..8 pages across 640 px
 y 354..424 velocity lane (DRAW), or the tool's controls (CHORD / STRUM / HUMAN / KEY / SWING)
 x 700..792 DRAW CHORD STRUM HUMAN KEY SWING (42 px each) | SNAP 1/16 | UNDO REDO
```

Every control is at least 48 px high, as on the other pages. The two exceptions are the page strip cells (26 x 48)
and UNDO/REDO (44 x 48).

### Safe while the Rytm plays (UX review T5)

- Opened while the Rytm plays, the page is in **VIEW**: the DRAW key reads VIEW, the grid selects notes and scrolls but
  does not edit; a tap that would have edited shows a toast, `VIEW: TAP DRAW TO EDIT`.
- A tap on DRAW (or any tool) edits again. After 10 s without an edit while the Rytm plays, VIEW comes back. Stopped,
  the grid always edits.
- While the Rytm plays, a new note takes a **long press** (LVGL's long press, 400 ms) and so does deleting the selected
  note; a plain tap shows `HOLD TO ADD (PLAYING)` / `HOLD TO DELETE (PLAYING)`. Dragging a note (move, resize) stays a
  drag. CHORD / STRUM / HUMAN stamps by a tap only while stopped.
- Every touch edit shows a toast for 1.5 s: `+ D#4 S5`, `- D#4 S5`, `> D#4 S6` (moved), `LEN ..`, `VEL ..`.
- Code: `pi/ui/sidecar_seqed.c` (lock_set, toast, grid_cb's LONG_PRESSED); tests: `pi/ui/host/shot.c` (seq-view,
  seq-toast).

### What it shows

- **Notes** are bars from their onset (step + micro) to their length.
  - Colour is the velocity, from dark green (1) to light green (127).
  - A muted step is grey.
  - Note names show inside bars that are wide enough.
- **Micro-timing**: a note off its step's grid line gets a stem from the line to its onset, plus a tick on the line. A
  strummed chord shows as a staircase of stems.
- **Scale/key**: rows in the scale are lighter and the root rows tinted green. The keys column marks scale notes, with
  the root in the accent colour. The key is a display preference for now (KEY tool). A per-pattern key would be a
  core/seq field: proposed to seqhost, not added.
- **Play head**: a 2 px line, from core/seq's `play_pos(track)`, interpolated. The page strip shows the playing page
  with a dot.
  - **Follow**: while playing, the window jumps to the page that holds the play head. It waits 3 s after the last
    touch, so it does not fight the finger.
- **Header**: SIDECAR SEQ, transport and tempo, the visible pages (`1-2/4`), the track's swing when it has one
  (`SW 62%`; OLED: `SW62` in the title bar), the Rytm project, the pattern (A04), and the track. The chips are the seq tracks (at most 4, core/seq kTracks); a chip switches the shown track.

### Touch

| gesture | DRAW | CHORD | STRUM / HUMAN |
|---|---|---|---|
| tap on an empty cell | add a note: snapped onset, last length and velocity used (else the track's defaults) | stamp the chord on that step, root = the row's note | that step |
| tap on a note | select it (card) | (as empty) | (as empty) |
| tap on the selected note | delete it | | |
| drag a note | move it in time and pitch. It moves by whole snap units, so its micro offset stays (a strum stays intact). FREE moves it freely. | | |
| drag the selected note's right edge | resize (snapped) | | |
| drag on empty grid | scroll: pages sideways, pitch up/down | | |
| drag the keys column | scroll pitch | | |
| page strip: tap / drag across | show that page / that range (1..8 pages) | | |
| two fingers | pinch: horizontal span = pages (1..8), vertical = row height (8..28 px) | | |
| velocity lane: drag a bar | that note's velocity (the selected note first) | | |

- **SNAP** cycles 1/16 (the step), 1/32, 1/64, FREE. A new note's onset snaps down (the cell tapped), a moved note
  snaps by delta.
- **Chord stamp**: the chord row comes from core/poly's chord table (`kChordNames` via the view). The first 10 are
  shown; `...` swaps in the rest. Inversion R/1/2/3. Strum 0/6/12/24 per note (1/96 step). The stamp lasts 2 steps
  at least. It is core/seq's own `set_chord`, so the Rytm's CHD/INV re-expansion stays consistent.
- **Strum** (broken chords): an amount (0..48/96 a note) and a direction (UP from the lowest note, DOWN from the
  highest). Tap a step, or APPLY VIEW for every step with 2+ notes in the window (core/seq `apply_strum`).
- **Humanise**: TIME +-0..16/96 and VEL +-0..24 per note, on a tapped step or the whole view. The pseudo-random
  sequence is fixed and seeded per gesture, so a result is reproducible and undoable.
- **Swing** (touch2, the SWING tool): the shown seq track's own swing - the host sequencer's, not the Rytm's - 50..80 %
  (every second step late by swing / 50 - 1 of a step; core/seq `Track::swing`, saved with the pattern). Presets 50 54
  58 62 66 70 75 80, and - / + by 1 %; each sends `seq=swing:T:N` (T 1..12), the view (`sidecar_seq_view_t::swing`,
  seqed_pi.cpp) shows it back. The info line says the value and what it does; the header shows it whatever the tool.
  Grid taps under SWING do what they do under KEY (nothing is stamped). Host check: `make -C pi/ui/host` taps a preset,
  +, - and straight and reads core/seq's track back (`seqdemo_swing`).
- **Undo/redo**: every gesture is one level (a chord stamp, a strum over the view). There are 24 levels shared by the
  tracks, each kept per (track, pattern), so undo never edits the wrong pattern. The history lives on core 3 next to
  the model (24 copies of a ~7 KB track, allocated at the first edit).
- **Pinch**: the display forwards the touch controller's second finger (`sidecar_display.cpp`: nID 1 of Circle's USB
  HID and the DSI touch handlers, `sidecar_seqed_touch2`). LVGL's pointer stays single-touch. Multi-touch reporting
  of the Waveshare HID panel and the FT5x06 is [inferred]: both report several contacts on other systems. Without it,
  the page strip and the scroll gestures do the same job.

### Data path (threads)

```
core 0 (LVGL)                         core 3 (control: the console's queue)              core/seq
 gesture end -> "seqed=move:8:2:72:3:73:1" -> seqed_pi::Command -> seq_touch::apply -> add_note / remove_pitch /
   local guess drawn at once                                     (+ undo level)         set_chord / apply_strum
                                                             -> seq::touched ()  -> trig LEDs, remote SEQ page, save
 sidecar_seqed_view <- seqed_pi::View <- seqlock buffer <- seqed_pi::Tick (bridge::tick, 5 ms; on a change only)
 sidecar_seqed_playhead <- seqed_pi::Playhead <- seq::play_pos (shown track)
```

- One command line per gesture, the same path as the Settings page's controls (`UiCommand` -> `CConsole::Command`).
  Edits are therefore logged and scriptable from the serial console and the web page. A drag previews locally and
  sends once, on release.
- The command set (`pi/ui/seq_touch.h`, integers; T = Rytm track 0..11):
  `seqed=add:T:S:P:M:V:L`, `del:T:S:P`, `move:T:S:P:S2:P2:M2`, `len:T:S:P:L`, `vel:T:S:P:V`,
  `chord:T:S:ROOT:CHD:INV:V:L:STRUM`, `strum:T:S0:S1:A`, `human:T:S0:S1:TIME:VEL:SEED`, `undo:T`, `redo:T`;
  `seqed=show:T` (the shown track), `seqed` (status).
- The edits go into core/seq's pattern itself (`edit_track` + `touched`, agreed with seqhost). The Rytm's trig LEDs,
  the remote SEQ page and the debounced save therefore follow, and edits made on the Rytm show here: `stats().edits`
  changes, so the view is republished.
- `Track*` is fetched again for every command: the pattern buffers swap at a pattern change (seqhost).
- Files: `pi/ui/sidecar_seqed.{h,c}` (the page, LVGL only), `pi/ui/seq_touch.{h,cpp}` (commands, undo and view; pure,
  on core/seq), `pi/engine/seqed_pi.{h,cpp}` (core 3 / core 0 glue). The glue builds only when `core/seq/seq.cpp` is
  in the tree (`SIDECAR_SEQED`, pi/engine/Makefile). Without it the page says "No Sidecar sequencer in this build",
  as in `pi/lvgl-test`.

## B. The Rytm's own pattern (read only)

### Where the host can get it

| | (1) Stock SysEx pattern dump | (2) Mod reports of every edit | (3) A new small mod message: "send the working pattern" |
|---|---|---|---|
| Needs a mod | no, works on stock OS | yes, a hook in every writer (trig place/clear, NOTE/VEL/LEN/micro, lock set/clear, copy/paste/clear, pattern load) | yes, one handler |
| Data | the 13101-byte stored record [public: libanalogrytm `ar_pattern_t`, 0x332D B; our own docs measured the same layout and size in RAM and on the card] | deltas only: the host must already hold the base | the record (the mod runs the stock `pattern_track_save` / lock rebuild into a buffer), or the live arrays |
| Freshness | on request; edits on the Rytm are not pushed | live | on request, plus an optional "dirty" bit pushed |
| Risk | the request bytes and the edit-buffer number are [public, to verify]; a dump while playing is untested here (the stress test only measured incoming dumps) | most code in the most places: every writer has to be found and hooked, miss one and the view is wrong | low: read-only, one place; the cost is a mod message |
| Effort | host 1-1.5 days (request, 7-bit unpack, checksum, parse, check in rytm-emu with stock 1.73), plus 0.5 day for triggers | 4-6 days mod + host, plus the emulator checks | 1-2 days mod + host |

**Recommendation: (1) now, (3)'s "dirty" bit later.**
- The stock dump needs no mod, which matters for testers on stock firmware. It gives exactly the record that
  `rytm_pattern_decode` already parses (`pi/ui/rytm_pattern.c`, tested on a synthesised record).
- The host asks for it when:
  - the RYTM page opens;
  - the pattern changes (seq mod `2D 02`, or the host's own pattern tracking);
  - about 1 s after the Rytm's last edit activity, while the page is open. The host sees key and encoder activity
    through the remote UI and seq messages (0x31 / 0x32) when those mods run; otherwise a REFRESH tap or a slow poll
    (5 s, stopped) does it.
- Later, a 1-byte "pattern dirty" report from a mod (3) replaces the guessing. Option (2) is not worth its risk for a
  read-only view.

The dump, as recalled from libanalogrytm-style community docs [public, to verify in rytm-emu before any code relies
on it; behaviour/format only, no code copied]:
- **Request:** `F0 00 20 3C 07 00 64 01 01 <nr> 00 00 00 05 F7`.
  - 0x64 is the pattern request.
  - `<nr>` is 0..127 for a stored pattern, `nr | 0x80` for the working (edit) buffer, which is the one wanted.
- **Answer:** `F0 00 20 3C 07 00 54 01 01 <nr> 00 00 00 05 <payload> <checksum 2> <length 2> F7`.
  - 0x54 is the pattern dump.
  - The payload is 7-bit packed (one MSB byte, then 7 data bytes): about 14.97 KB for the 13101-byte record.
  - `rytm_sysex_unpack`/`_pack` are implemented and round-trip tested.
  - The checksum and length fields are left to the emulator check.
- **Cost:** about 15 KB over USB MIDI, once per pattern change or edit burst [inferred: tens of ms]. The Rytm-side
  cost of answering while playing is to be measured in rytm-emu (`--usb-host --usb-midi-real`), as the stress test
  did for incoming dumps.

What the record decoder takes from the layout (POLYPHONY.md, [measured] unless marked):
- trig bits (14 a step, packed; bit order MSB-first [inferred]);
- NOTE as `0x80 | note`; VEL; LEN;
- micro as 6-bit two's complement plus 2 flag bits (unit 1/24 step [public]);
- the condition byte at +0x1f0 [inferred];
- track defaults; speed; the track length at +0x276 [inferred];
- 72 lock slots `{type, track, 64 values}` with `0x80 0x80` fine continuations.

The lock type's display name and the condition code's text follow public parameter and TRC lists [inferred order,
marked in the code]. They must be checked in rytm-emu before anything writes them.

### The view

- **Grid**: 12 tracks x 16 steps (one page, 40 px cells) or 64 (ALL, 10 px).
  - A trig's colour is its velocity, and an accent adds a white cap. Its bar sits shifted by its micro-timing.
  - A thin bar under the trig is its length (about LEN/32 steps [scale inferred]).
  - A dotted underline marks a condition, an amber outline a trigless lock, an amber corner dot a p-lock on that step.
  - Steps past a track's length are dark.
- **P-lock lanes** for the selected track (the first 4 parameters): amber strength = value, values printed at page
  zoom, names at the right. The title line counts the track's locked parameters and the pattern's used lock slots
  (of 72).
- **Step card**: note, velocity, length (`*` = the track default), micro, condition, flags, the number of locks, and the
  first lock's value.
- Tap a row name or a cell to select; pages 1-4 / ALL; the Rytm's play position as a line.
- Files: `pi/ui/rytm_pattern.{h,c}` (record <-> arrays, 7-bit packing, names; pure C, no stdio), `pi/ui/sidecar_rytmpat.{h,c}`.
  The view is fed by `sidecar_rytmpat_set`. The host render feeds it the made-up pattern through encode -> decode,
  and (shots rytm-dump / rytm-fetching / rytm-stale) through the Rytm's own dump answer read by core/seq/rytm_dump.
- On the Pi (integ-0.3): `pi/engine/rytmpat_pi.cpp`. While the PAT page is open (`sidecar_rytmpat_shown`, core 0) core 3
  keeps `rytm_dump` active: a fetch when the page opens, on each pattern change (2D 02) and 300 ms after the mod's DIRTY
  (2D 0B, rui1035; rui1034 sends none: then only on open / pattern change). Each good record is decoded on core 3 and
  published behind a sequence counter; the header says `fetching...` or `no answer, N s old` in the warning colour
  (`sidecar_rytmpat_where`), else `from the Rytm [N s ago]`. Nothing is asked while the page is closed.

### Editing Rytm patterns from the touchscreen (design only)

Two paths:

1. **Through the stock writers, by a mod message.** Generalise the 0x28 write-back:
   - New message: `28 03 <track> <step> <field> <value...>`, with field = NOTE, VEL, LEN, micro, condition, a trig
     on/off, a p-lock (type, value) or a lock clear.
   - The mod applies it on the Rytm's UI task through the same stock functions the panel uses: `trig_place` /
     `trig_flags_set` (SEQUENCER.md 3.3), `lock_pool_set` (POLYPHONY.md P-locks), as 0026 already does for CHD/INV
     plus NOTE.
   - It answers with a status (`28 04`: ok, no slot, bad), and the host re-reads the pattern (B above) to confirm.
   - Risks:
     - Concurrency with the sequencer ISR. Writing on the UI task, as the panel does, avoids that [inferred].
     - The 72-slot lock pool can run full. Report it, never evict.
     - Edits land in the working pattern only, unsaved until the user saves, as panel edits do.
     - A mod bug can corrupt the project in RAM. Mitigations: only stock writers, every value clamped, rytm-emu test
       runs that save, power off, cold boot and compare.
   - Effort: about 1 week including the emulator checks. Medium risk.
2. **A whole-pattern SysEx send** to the edit buffer (stock accepts pattern dumps [public]):
   - No mod needed, but it replaces the entire working pattern: any edit made on the Rytm since the last read is lost.
   - What happens on the Rytm while it plays is unknown (a reload glitch at the pattern boundary is possible
     [inferred]).
   - Acceptable only for "push this whole pattern" actions, never for small live tweaks.

**Recommended later: path 1** for small edits (step values, conditions, lock values). It keeps the Rytm the owner of
its pattern, with the same undo semantics as its panel. Path 2 should stay out.

## Cost: touch must not disturb the audio

- **Where it runs**: LVGL, the touch events and these pages run on core 0's loop. Rendering is on cores 1-2, control on
  core 3, and the audio paths are IRQ-driven (docs/pi-display.md "Cost"). A slow redraw delays core 0's loop (MIDI out
  to the Rytm, the console, the TCP link), never a render block.
  - The only cross-core work is one command line per gesture (queued) and, on a change, an 8.6 KB view copy behind a
    seqlock (no lock held).
- **Measured on the Mac** (`./shot` prints it). Pi 4 estimates use the home page's first full draw (7.4 ms measured on
  the Pi 4) as the scale factor, about x32, which is conservative (a cold first draw):

  | redraw | Mac | Pi 4 (est.) |
  |---|---|---|
  | home page, full | 229 us | 7.4 ms (measured) |
  | SEQ page, full (page change, pattern change) | 262 us | ~8.5 ms |
  | SEQ grid (an edit's full refresh, a scroll, a zoom) | 78 us | ~2.5 ms |
  | a note-drag frame (old and new rectangle) | 3 us | ~0.1 ms |
  | the play head moving a step (two 2-4 px strips) | 8 us | ~0.3 ms |
  | RYTM page, full (open, new data, selection) | 511 us | ~16 ms |

- **While playing**, the steady cost is the play head: at most 30 redraws a second of two narrow strips, about 1 % of
  core 0. Edits invalidate only the rectangles they touch; the drawing callbacks cull to the redrawn area, so 1024
  notes cost a bounding-box test each.
  - Full redraws happen only on page, zoom, scroll or pattern changes, as on the existing pages.
  - The RYTM page's full redraw (~16 ms est.) is the largest. It happens only when opened or refreshed. If it shows on
    the stats line's `longest update`, its 12x64 cells can be cached into a canvas.
- **Pi 5 on HDMI (scaled)**: the 800x480 UI is scaled up to the firmware's frame buffer (1920x1080 on the Waveshare:
  x2.4 / x2.25, `CSidecarDisplay::Put`). Every flushed UI pixel becomes about 5.4 frame-buffer pixels, so a full
  redraw moves about 4.1 MB.
  - Scaling is a CPU row copy per UI row, plus DMA.
  - The play head's strips stay small (about 8 x 660 frame-buffer pixels).
  - Edits and full redraws cost several times the Pi 4's [inferred: not measured on hardware]. Still core 0 only.
- **1024x600** (a 1024x600 HDMI panel, docs/pi-display.md "Screen sizes"): the roll shows 32 rows instead of 24 at
  12 dots a step (OLED; CLASSIC 27 px), and a full redraw costs about 1.3x the 800x480 one (docs/UI-THEMES.md "Cost").
- **DSI** (official 7" and compatibles): native 800x480 on both boards, no scaling, the Pi 4 numbers apply.
  - On the Pi 5 the touch is polled over I2C from the display's `Update` every 10 ms, already counted in the
    `longest update` line.
- **To check on hardware**: the engine's 10 s line (`longest update N us`) with the SEQ page open while playing and
  dragging; underruns must stay 0.

## Build and tests

```
make -C pi/ui/host test                    # seq_touch on core/seq (commands, undo per track and pattern, strum,
                                           # humanise, chord stamp, bounds), the Rytm record (round trip, offsets, 7-bit)
make -C pi/ui/host                         # shot.png ... seq-*.png rytm-*.png; fails if a SEQ gesture misses the model
# from a worktree: B=<main>/pi/circle/build ENG=<main>/core/build/engines
pi/engine/build.sh kernel; RASPPI=5 pi/engine/build.sh kernel; pi/lvgl-test/build.sh   # all three build
```

## Status and next steps

- Rebased onto seq-mvp (2f84f74). Verified on the Mac against core/seq (pattern.cpp, poly.cpp): the gestures, the
  model, the renders. The Pi 4 and Pi 5 engine kernels build with `SIDECAR_SEQED` (the glue in), and `pi/lvgl-test`
  builds (its Makefile also gained the missing `sidecar_splash.o`). Not run on hardware.
- Next:
  1. Check the page on the Pi with the engine running: edits reaching the trig LEDs and the remote SEQ page, the
     play head, `longest update`.
  2. The Rytm SysEx fetch (B (1)): wired (above); check it on the Pi with the Rytm (the age / fetching texts).
  3. A per-pattern key/scale and a chord-stamp length in core/seq (proposed to seqhost).
  4. Measure `longest update` on the Pi 4 (DSI and HDMI) and the Pi 5 (scaled HDMI) with the SEQ page while playing.
