# core/ui: the remote UI (widgets, pages, the host)

The Sidecar draws pages on the Rytm's own 128x64 screen. The Rytm side (rytm-mods `0026-remote-ui`) is a dumb
terminal: a 1 KB shadow it blits at its 20 Hz UI tick, keys and encoders reported while the page is open. All
layout and drawing is here, shared by the Pi engine, the Mac host build and the Teensy (spec:
`docs/superpowers/specs/2026-10-16-remote-ui-research.md`).

| file | what |
|---|---|
| `ui.h`, `canvas.cpp` | `Canvas`: 128x64 1-bit in the Rytm's framebuffer layout (glass pixel x, y = byte `(7 - y/8) + 8x`, bit `y%8`); pixel, h/v line, Bresenham line, rect / fill / invert, 50 % dither, a clip rect, 1-bit glyph blit (scale 1 or 2); `diff_tiles` (the changed 8x8 tiles); text in two fonts |
| `fonts/` | `font3x5` (labels, 4 px advance) and `font5x7` (titles, values, 6 px; at scale 2 the large font). Original glyphs drawn for this project in `gen_fonts.py`, CC0 (`fonts/LICENSE`). Since 1021 only for previews and tests: on the Rytm the text is the Rytm's own (below) |
| `widgets.h/.cpp` | header (title, page dots), knob, bipolar knob, level bar, pan bar, enum list, filter curve (LP / BP / HP / notch from cutoff + resonance), envelope (ADSR / AD / AHD), oscillator shape, LFO shape with its depth band, sample waveform (min/max per column, slices, start/end) |
| `page.h`, `pages.cpp` | the page format (below), `PageView` (redraws only widgets whose inputs changed), the built-in machine table's pages: PLAITS, SIDECAR status, two gallery pages |
| `remote.h/.cpp` | the remote UI host: protocol 0x30..0x3F, page navigation, encoder edits, tile-diffed frames, keepalive |

No allocation, fixed buffers (two canvases, 2 KB: RAM2 on the Teensy). Every function is `CORE_COLD` (program
flash on the Teensy: 0 B of ITCM), tables `UI_ROM` (flash). Integer drawing; the filter curve takes a few floats a
column at redraw time with its own log2 / exp2 (no libm).

## Text: the Rytm's own fonts (1021)

The Rytm draws every piece of text itself, with its own text routine and its own fonts (rytm-mods 0026 1021, section
9 of its design.md): the frame is pixels for the graphics plus up to 64 text items (`Canvas::put_text`: slot, x, the
top row, font, alignment, inverted). The host sends the changed items as `3D` with the frame; the Rytm composes them
over the committed pixels. No glyph data of the Rytm is in this repository: a font is named by the Rytm's index
(`RFont` in `ui.h`, with its measured height and widest advance), the Rytm aligns (left / centred / right). `kRSmall`
(index 16) is the stock screens' font: title bar, labels, bottom bar, enum values. `preview()` draws the items with
the CC0 3x5 font in the same boxes, for `sim/rui_render.py` and the tests.

Geometry as the stock screens [measured in rytm-emu]: title bar rows 0..8 (inverted, text at x 2), two rows of 32x22
cells (dials of radius 7 at rows 12..23 / 34..45, labels centred at rows 26 / 48), bottom bar rows 55..63 (inverted,
the touched parameter as stock's "BD LEV:" with a level box).

## Page format

A page is the Rytm's encoder grid: the 9-row title bar and two rows of four 32x22 cells, encoders A..D over the top row,
E..H over the bottom. A **cell** is a widget on that grid (it may span columns and rows) fed by up to four of the
voice's parameters (P1..P16 = 0..15). The page also maps each encoder to a parameter and names it; the labels are
drawn under the grid column of each encoder.

```c++
struct Cell {
  uint8_t kind;                  // kKnob kKnobBi kBar kPan kList kFilter kEnv kWave kLfo kSample kText
  uint8_t col, row, cols, rows;  // col 0..3, row 0..1, span
  uint8_t in[4];                 // parameters feeding it (kNone = 0xFF unused)
  uint8_t arg;                   // per kind: EnvShape, a fixed filter type / wave / LFO shape, a sample or text id
  const char* names;             // kList: "A\0B\0...\0" (the parameter in equal zones, eng_zone)
};
struct Page {
  char title[12];
  uint8_t machine;               // the voice byte's machine (voice byte & 0x78) the page is for
  uint8_t ncells; const Cell* cells;
  uint8_t enc[8];                // encoder A..H -> parameter (kNone: none)
  char label[8][5];              // encoder labels
};
```

| kind | inputs |
|---|---|
| `kKnob` / `kKnobBi` | in[0] (bipolar shows v - 64) |
| `kBar` / `kPan` | in[0] (pan: 64 = C) |
| `kList` | in[0] in `list_count(names)` equal zones; an encoder on it steps one item a detent |
| `kFilter` | in[0] cutoff, in[1] resonance, in[2] type in 4 zones (LP BP HP NOTCH) or kNone and arg = the type |
| `kEnv` | arg = kAdsr (in A D S R), kAd (A D), kAhd (A H D); kNone reads 0 |
| `kWave` | in[0] the shape in 6 zones (SINE TRI SAW SQUARE PULSE NOISE) or kNone and arg; in[1] pulse width |
| `kLfo` | in[0] the shape in 7 zones (TRI SIN SQR SAW EXP RMP RND) or kNone and arg; in[1] depth (64 = none) |
| `kSample` | arg = the source's peak set; in[0] start, in[1] end |
| `kText` | arg = the source's first text line; a line every 6 px (status pages) |

PLAITS (`pages.cpp`, P1..P8 ENG HRM TIM MOR DEC LPG TUN LEV): ENG as the list of the 17 curated models, three
knobs, the low-pass gate's decay as an AD shape (Plaits has no filter of its own), LPG colour, TUNE bipolar, LEVEL
bar. A new built-in page: a `Cell` array, a `Page`, one row in `kTable`.

**As JSON** (for plugin manifests and a browser page designer, octadraw-style; 1:1 with the structs, not loaded yet):

```json
{"title": "PLAITS", "machine": 0,
 "enc": [0, 1, 2, 3, 4, 5, 6, 7], "label": ["ENG", "HRM", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV"],
 "cells": [{"kind": "list", "at": [0, 0, 1, 1], "in": [0], "names": ["VA VCF", "PHASEDST", "..."]},
           {"kind": "knob", "at": [1, 0, 1, 1], "in": [1]},
           {"kind": "env", "at": [0, 1, 1, 1], "in": [null, 4], "shape": "ad"},
           {"kind": "filter", "at": [0, 0, 2, 1], "in": [4, 5, 6]}]}
```

## The host (`remote.h`)

- `30 on` from the Rytm (UP + YES; TRK + YES, the legacy form) opens the SIDECAR status page and sends the whole frame (`3E`, every tile, every
  text item); `30 off` (TRK + YES, the Rytm's 1 s timeout) stops all output. Two status pages, stock geometry:
  SIDECAR, each track's number (a badge) and machine in two rows of four, FX1 / FX2 a row each (a badge, the TYPE,
  PRE / POST; "OFF" when off); SYSTEM, U1..U5, the recorder (a dot and the time while recording), Overbridge and
  underruns, each core's load as a bar and its percent, the network in the bottom bar (Ethernet / Wi-Fi up or down and
  the address only). Pages: SIDECAR, SYSTEM, then each voice whose machine has a page (the title bar's dots); LEFT
  the previous, RIGHT / DOWN / PAGE the next (UP alone nothing: the Sidecar's modifier, fix1049), NO back to SIDECAR. A voice's machine is the one the Rytm's reports name
  (the voice byte, as the engine plays it); before its first report a track has no page and "-" on SIDECAR (an analog
  track never reports), so only the tracks on a Sidecar machine have voice pages (none: SIDECAR, SYSTEM and the live
  pages only); the Pi's boot bench's own reports are forgotten (`remote_forget_voices`). 0021 reports a voice only while it sounds: a machine set on the SRC page (the list's YES)
  sends nothing [measured, rytm-emu rui1023], so its page appears at the track's next trig and follows every later
  report live (`rui_e2e.py extmach`). 1025 (rytm-mods 0026 "1025", 33 `<t0> <n> {<machine> <vb>}`): the Rytm names
  every track's machine itself, at a change (the SRC list's cursor, YES, a kit / project load) and as a snapshot after
  its hello and the page's open; vb is the voice byte the track's reports carry, 0x7F not a Sidecar machine. Once a 33
  came it decides alone: a track set to JUNO has its page at once, one set back to analog loses it at once
  (`rui_e2e.py machines`, rui1025); an older Rytm build keeps the report path above.
- An encoder on a voice page changes the voice's parameter through the report path (`0x03` into `voice_report`,
  as `notes.cpp`'s CCs). The edit is held over the Rytm's later reports of the same value and gives way when the
  Rytm's own value moves (its knob, a p-lock, another sound). The Rytm's kit does not know the edit (not saved there).
- A frame at most every 25 ms, at once after a key or a turn: widgets whose inputs changed are redrawn, the frame
  diffed against what the Rytm holds, the changed tiles sent as `38` runs (one per run of tiles in a tile row), the
  changed text items as `3D`, and one `39` (the turned knob's cell and the bottom bar first, in a commit of their own,
  when more changed); nothing when nothing changed; `3C` after 250 ms without a message.
- The local echo: each numeric encoder of a voice page is bound (`3F`) to the bottom bar ("BD HRM:" + value), so the
  Rytm shows a turn's value at its next render, before the round trip; lists are not bound.
- The catch mask (1022, `37`): the page takes only its own keys and knobs; the rest work on the Rytm as on its stock
  screen. Every page takes LEFT / RIGHT / UP / DOWN / YES / NO / PAGE (ux1027: the SEQ page YES only under a question,
  with FUNC or a trig held - else YES is the Rytm's, its YES combos; updown1029: UP / DOWN only under a question or
  with FUNC held - else the Rytm's, CHRO's octave; FUNC + DOWN the poly track's menu), the keys that open or change a stock screen
  (TRIG, SRC, SMPL, FLTR, AMP, LFO, FX, GLOBAL, SAMPLER, PLAYMODE, QPER, SONG, CHAIN) and TRK, plus the encoders it
  binds and their pushes (the status pages: all nine, LEVEL and its push too, so no knob edits the stock parameter underneath; a turn there is ignored). Passed: RECORD / PLAY / STOP (the Rytm keeps them whatever
  the mask), the pads, trig and bank keys, MUTE / CHRO / SCNE / PERF, FIX, RTRG, FILL, FUNC, TEMPO, and on a voice page LEVEL and its
  push, and any encoder the page leaves unbound (it edits the stock parameter under the page). Sent before a frame
  whenever it changes and after every open; a key's release goes where its press went (rytm-mods 0026 design.md
  section 10).
- Platform: `remote_tick(ms)` from the control loop (Pi: `bridge::tick`, core 3; Teensy: `loop()`), `remote_status`
  for the SIDECAR page (Pi: the stats line; Teensy: its stats report). Pi console: `:rui` (numbers), `:rui=on|off`.

Tools: `sim/rui_render.py` (every page to PNG, draw cost), `sim/rui_link.py` (the core as the emulator's host),
`sim/rui_e2e.py` (rytm-emu with rytm-mods' `rui1022` build: the pages, the status page, leaving, the catch mask,
latency),
`sim/rui_probe.py` (one frame of pixels + text items onto the emulated Rytm's panel: how the Rytm draws a font; its
screens are firmware output and stay out of git), `core/tests/test_ui.cpp`.

## The connect splash (1023)

When its USB port comes up, rytm-mods 0026 (1023) sends **35** `<uptime >> 7> <uptime & 0x7F> <caps>` (uptime in the
Rytm's 50 ms ticks since its UI loop started; caps bit 0: it understands 36), again 1 s and 2 s later while the host
says nothing. If the Rytm has been up >= 3 s (the link came after its boot: a cable plugged in, the Sidecar started or
restarted later) and the page is closed, the host shows the splash for 3 s: the stock title bar (`kStatusTitle`, USB at
its right), a USB trident (a 42 x 19 bitmap drawn for this project) sliding in from the left over 150 ms, CONNECTED in
the Rytm's bold font (index 7, `kRBold`), the voices and send FX, the network address (else USB AUDIO+MIDI; never the
Wi-Fi's name), a line under it running down over the 3 s, and the bottom bar "UP+YES: SIDECAR PAGES" (rui1046: from the device's table, core/ui/page_keys.h; before: "TRK + YES: SIDECAR PAGES"). The first
frame goes whole, then **36 64** (the Rytm holds it 3.2 s at most by itself), then small frames (<= 18 tiles while the
icon slides, ~1 tile a frame for the line), then **36 0** at 3 s. Otherwise the hello gets a 3C. On the Rytm every key
and knob stays its own during the splash (transport and pads play under it, a navigation key only ends it, any other
key or a turn ends it and acts); UP + YES (TRK + YES, the legacy form) opens the page over it (the host's 30 on ends the splash here too).
`sim/rui_render.py` writes its frames (splash_*ms.png); `sim/rui_e2e.py splash` runs it against the emulated Rytm.

## The LFO page (`page_lfo.cpp`)

The first page drawn by code (LFO, REC, SCOPE): the Sidecar's LFOs (core/lfo.h, docs/lfo.md). Main view: the wave and
its moving dot, A LFO B TGT C PAR D DEP (FUNC: BASE) E WAVE F RATE G SYNC H TRIG; YES the design view (the 16 steps,
T1..T16 caught there only through `ExtraPage::trigs_now`, A step B value C shape D preset), YES / NO back. ux1027: off
the LEFT / RIGHT ring (UP + LFO, the SIDECAR MENU); LEFT / RIGHT step the LFO (the design view: the step), as A; UP +
LFO again leaves. Every page off the ring (SEQ, INSERT, LFO, POLY, POOL, CHOP, MENU, KEYS) replaces the Rytm's screen:
no page dots, LEFT / RIGHT / DOWN / PAGE its own or nothing (remote.cpp `standalone`).
`remote_render` draws page `6 + k` for the page drawn by code k (5: the connect splash).

## Live pages (1023: `extra.h`, `page_*.cpp`)

Pages drawn by code, after the voice pages (SIDECAR, SYSTEM, the voice pages, then these; LEFT on SIDECAR wraps to
the last one). Each draws its whole page every frame and the diff sends what moved. Hooks in `remote.cpp`: the page
list (`page_ids`), the draw, the catch mask (the page's `encs`), its encoders and keys (a key the page takes is not
navigation), `tap_enable` while a page that reads the tap shows (off when the remote page closes).

**The input tap.** The Rytm's 12 Overbridge IN channels (MAIN L R, the 8 voices' tracks, AUDIO IN L R) from the
platform's IN path: the Pi's IN completion (`ob_usb.cpp` through `bridge::tap_in`, core 0), the Teensy's USB host
interrupt (`ob_rx`), `rui_link.py` (the emulator's IN frames, or `RUI_TAP=synth`). Only while a page reads it, and
only compares and stores: per channel the peak since the control side last took it, and the selected source's min /
max over each 8 frames into a ring (`kTapRing` entries: 2048 on the Pi and the Mac, 512 on the Teensy). Everything
else (columns, triggering, gain, meters, drawing) runs in `remote_tick` on the control side.

**SCOPE** (encoders A SRC, B TIME, C GAIN): the source (MAIN = (L + R) / 2, V1..V8 = the Sidecar voice's track
channel, IN L / R) over 128 columns, each the min / max of its entries; 21 / 43 / 85 ms a screen triggered on a rising
crossing of the columns' middle, 171 ms .. 1.4 s as a sweep with a cursor (only new columns change). GAIN AUTO picks
the power of two (x1..x64) that puts the window's peak in the upper half. Under it twelve peak meters (-48..0 dB,
fall 24 dB/s, 1 s hold): MAIN L R, voices 1..8 (the SIDECAR page's numbers), IN L R. The live part updates at most
every 66 ms (13 frames/s on the 25 ms frame grid) and only while the tiles sent stay under 900 a second (a token bucket
over the host's tile count; the panel link carries ~1560 tiles/s [rytm-mods 0026]). Measured in rytm-emu with the
synthetic pattern (`sim/rui_e2e.py scope`): 13.3 frames/s, ~430 tiles/s, ~5.3 KB/s (~400 B a frame); the Rytm's UI
keeps its 20 renders a second, a turn's frame follows its 32 within a millisecond.

**Cells drawn by code** (`page.h` kind `kDraw`, `arg` an `extra.h` DrawId, its inputs as any cell's; `draw_key` adds
what else it shows to PageView's change key; `Source::voice()` names the voice). **DSP SAMPLE** (machine 0x20, the
SD SAMPLE page P9..P16: TUN FIN SMP MOD over the waveform, STA END LOP LEV under it) and **GRAIN** (the same voice
when MOD is in GRAIN's zone and the sample RAM plays it: `page_for_voice`; POS SIZE DENS LEV): the pool slot's file as a
128-column overview (`page_wave.cpp`), found once per file on the control side (a column a millisecond from
`remote_tick` while the page is open, a column of up to 256 frames read whole, a longer one as four runs: ~130 ms a
file, 2 KB a read at most) and kept (8 files on the Pi, 1 on the Teensy: 0.5 KB of RAM2). DSP SAMPLE: STA..END solid,
the rest dithered, reversed with an arrow, a loop (LOOP, a looping ONE) as brackets and a dotted line over the
region, SLICE's slices dotted; the mode's name beside the waveform ("ONE" / "LOOP" without the sample RAM, which
streams). GRAIN: the window as a box at POS, SIZE wide (10 ms x 50^(SIZE/127) of the file), the spray dotted on from
its edges. No waveform: "SMP OFF", "EMPTY SLOT", "LOADING", "UNREADABLE" in its place. A STA turn in rytm-emu: 1..6
tiles a frame.

**REC** (`page_rec.cpp`, before SCOPE; shown only once a platform hands over a recorder with `remote_rec`: the Pi's
`rec_pi.cpp` every 200 ms and after each request; the Teensy has none and never shows it). Keys: **YES** records (idle,
or armed: at once) and stops (recording); **encoder A** turned right arms (the take starts on the Rytm's next PLAY,
stops on STOP), left disarms, **A pushed** toggles arming. NO stays "back to SIDECAR" and the arrows / PAGE navigate, as
on every page (the TRK + YES that leaves never reaches the host). A key or turn only queues a request
(`remote_rec_take`), run by the recorder's own core (the Pi's core 0: `rec_pi::Poll`), whose next state the page draws.
Playback (`remote_play` / `remote_play_take`, the Pi's `rec_play.h`): **B pushed** plays / stops the selected take's
channel into the Rytm's MAIN, **B turned** the channel (MASTER first when recorded), **C turned** the take (newest first,
the last one by default), **D turned** the level; none of them while a take records (the recorder's view stays). While
playing, or 3 s after a B / C / D turn: a play mark, the playhead's time, the take, the channel and its number, a
progress bar with the playhead, and `B STOP  D LEV n  m:ss` / `B PLAY  C TAKE  D LEV n` (or playback's error).
Shown: a record mark (filled recording, a ring armed, small idle), the take's time as large segment digits (original
drawing), the take's name, the card's free space, the meters with a dot over each channel the take records, and in the
bottom bar what YES / A do now, the hours the card holds at the selection's rate, or the recorder's error (3 s).

**Machine graphics** (`page_gfx.cpp`, original 1-bit drawings): **JUNO** (machine 0x08: PAT as the patch's name,
CUT / RES as the LP curve, ENV; ATK DEC REL as an envelope with curved segments to the patch's own sustain, held for
LEN - 127 until the note-off, its end dotted - and LEN). **6-OP FM** (0x48: the algorithm of the voice PAT picks -
packed byte 110 - over PAT / TIM: operators as boxes, carriers standing on the output line, modulators above what they
modulate, feedback as a loop at the side; the 32 topologies as data, derived once from Plaits' algorithm table; MOR,
DEC as the LPG's AD shape, LPG TUN LEV LEN). **GRAIN**'s DENS cell: the cloud, grains as Hann bumps SIZE wide, as many
as DENS, at fixed pseudo-random places.

Tools: `sim/rui_live.py` (the live pages through the host with a synthetic stream: PNGs and the link's cost),
`core/tests/test_ui_live.cpp`, `test_ui_wave.cpp`, `sim/tests/test_core_rui_live.py`, `sim/rui_e2e.py scope wave rec gfx` (`rui_link.py` RUI_TAP, RUI_VOICE1, RUI_REC).
