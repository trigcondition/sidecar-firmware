# UI themes: the Pi display's design system

The Sidecar Pi's touchscreen (800x480 or 1024x600, LVGL 9.4, `pi/ui`; docs/pi-display.md) has two themes, each laid out
for both sizes ("Sizes" below):

| theme | what | default |
|---|---|---|
| **OLED** | The whole screen as a zoomed-in Elektron OLED: 400x240 dots, each a 2x2-pixel cell with a darker grid between, black, our own pixel font, dotted outlines, grey dithered title bars, inverted selection, knobs as dot rings, greys plus the Rytm panel's FUNC teal and blue print. A fixed navigation column on the right. | yes |
| **CLASSIC** | The 800x480 layout of before, its controls drawn as the Analog Rytm MKII's hardware (as rytm-emu's panel draws it): dark rubber keys with their print, trig keys with LEDs, pads, knob caps with LED rings, the teal FUNC key, the light grey panel with dark print and blue sub-labels, black display windows for the data. | |

**Switching:** Settings > Display > **UI style: OLED / CLASSIC**, or the console / web command `uistyle=oled|classic`.
- Applied at once, no reboot (`sidecar_ui_style`).
- Stored as `uistyle=oled|classic` in `/.rytm/display.txt`, with the screensaver, brightness and touch test settings.
- `uistyle=` in cmdline.txt overrides the file. It is also the only way to give the boot screen the CLASSIC look, because the boot screen is drawn before the card is read.

Every page exists in both themes: home, Settings (Sound, Send FX, Display, About), SEQ (the piano roll with chord stamp, strum, humanise, key, swing, velocity lane, page strip), RYTM (the pattern and its p-locks), REC (the Overbridge recorder: "REC" below), SCOPE and SIDECAR STATUS ("SCOPE", "SIDECAR STATUS" below), the machine view (GRANULAR, WAVETABLE, MULTISAMPLE), the boot screen and the power warning.

The display has no LFO, Wi-Fi or preset/pool browser pages. Those live on the web page and the Rytm's remote pages, which are outside `pi/ui` and unchanged.

Renders: `make -C pi/ui/host` writes `pi/ui/host/shots/{oled,classic}-*.png`, `make -C pi/ui/host shot.png SIZE=1024x600` the same at 1024x600 (`shots-1024x600/`), `make -C pi/ui/host splash.png` the boot screens and `make -C pi/ui/host anim` an animation of the OLED piano roll (`seq-oled.mp4`, `seq-oled.gif`).

## Sizes (800x480, 1024x600)

- **Which:** the display hands LVGL a screen of 800x480 or 1024x600 (docs/pi-display.md "Screen sizes": the frame
  buffer's size, `res=`, else 800x480 centred). `sidecar_ui_create` passes it to `sidecar_theme_screen`: 1024x600 (or
  more) gives the wide layouts.
- **The theme table carries the size:** `TH->w, h` (OLED 400x240 or 512x300 dots, CLASSIC 800x480 or 1024x600 px),
  `TH->bw, bh` (the 800x480 layout's size in the theme's pixels) and `TH->dx, dy` (what the screen adds: 0 x 0, or OLED
  112 x 60 dots, CLASSIC 224 x 120 px). The dot stays 2x2 pixels, the fonts the same faces: text keeps its physical
  size, touch targets stay at least 21 dots (42 px) / 48 px.
- **One layout, grown:** every page is written once, as its 800x480 layout plus `dx`/`dy` (no second copy). What grows
  gets the extra room, what sits at an edge moves with it:
  - Panels and lists get wider and taller; key rows keep their height and get wider keys; the OLED navigation column
    moves to the right edge, its five keys 59 dots high instead of 47.
  - Settings pages: positions and heights stretched to the taller panel (`VS`, `CY`/`CH` in `sidecar_ui.c`).
  - SEQ (`sidecar_seqed.c` `geo_size`): the roll gets the width in whole dots a step (OLED 12 instead of 9 at two pages,
    CLASSIC 27 instead of 20) and two thirds of the height (32 rows of 5 dots instead of 24; CLASSIC 360 px of rows instead of
    280), the lane the rest (OLED 64 dots instead of 44: the tools' controls taller); the page strip's cells 27 dots wide.
  - RYTM (`sidecar_rytmpat.c` `geo_size`): 20 dots a step instead of 16 (CLASSIC 52 / 40), rows of 13 dots (28 px), up
    to 6 lock lanes instead of 4, a wider step card.
  - VOICE, MACHINE, INSERT FX, SONG: the picture, the list (more rows), the timeline get the room; the overlays keep their
    size, centred.
  - SCOPE: the trace gets two thirds of the extra height, the meters the rest (taller ladders), every column wider;
    the keys row keeps its height. SIDECAR STATUS: wider columns, taller rows.
  - REC: the channel rows get the height (OLED 28 dots instead of 21, CLASSIC 57 px instead of 42), the columns and
    their meters the width (more segments); the transport row keeps its height; the takes' rows grow the same way
    (CLASSIC: 8 rows of 54 px instead of 7 of 44); the delete's confirmation keeps its size, centred.
- **Unchanged at 800x480:** the host renders at 800x480 are pixel-identical to before the change.

## The grid (OLED)

- **The UI is drawn at half the screen's size, then expanded to dots.**
  - LVGL's display is 400x240 UI pixels ("dots") on an 800x480 screen, exactly: no margin.
  - The flush expands each dot into a 2x2 cell of physical pixels (`sidecar_dot_expand`, `pi/ui/sidecar_dot.c`): the
    dot's colour in the top-left pixel, half of it in the pixels right of and below it, a quarter in the corner.
- **Why 2x2 cells (since 2026-10-21; before: 266x160 dots of 3x3 pixels, 2x2 lit and a 1-pixel black gap):**
  - 266x160 was too coarse for detailed drawings: the MULTISAMPLE keyboard and zones, the FM algorithm, the piano roll,
    the waveforms and the knobs came out jagged or had to be simplified.
  - Three cells were prototyped on the same content (host renders, the old shots resampled):

    | cell | lit | look |
    |---|---|---|
    | `[c 0; 0 0]`: one lit pixel, a black gap | 25 % | clearly dots, but dark: everything a step dimmer than the 3x3 grid, and the dark greys vanish off-axis |
    | `[c c; c c]`: full blocks | 100 % | no dot look at all |
    | `[c c; 0.3c 0.3c]`: scanlines | 65 % | a CRT, not an OLED |
    | **`[c c/2; c/2 c/4]`** | **56 %** | **the dots still read as dots, with a soft darker grid between them; brighter than the 3x3 grid (44 %)** |

  - The chosen cell keeps the dot-matrix look and is brighter than before (the panel's dark greys are lost at a slight
    angle, so brightness may only go up). It costs one shift and mask a pixel (`c >> 1 & 0x7BEF`, `c >> 2 & 0x39E7`).
  - The dots are 0.26-0.38 mm apart on a 5-7" 800x480 panel: the grid is finer, the look is the same.
  - The 3x3 mode was not kept: every page's OLED layout would have needed both sizes.
- **1024x600:** 512x300 dots, the same 2x2 cell ("Sizes").
- **Touch:** the pointer is divided the same way (`sidecar_ui_to_ui`). The smallest target is 21 dots = 42 px, as before.
  - The page-strip cells (18 x 22 dots, 36 x 44 px) are the narrowest, about as wide as before (39 px).
- **Pattern colours.** Four exact RGB565 values (no drawing produces them otherwise in this theme: 1-bit fonts, square corners, no anti-aliasing) become patterns during the expansion, by the dot's position on the grid:

  | token | becomes | used for |
  |---|---|---|
  | `DOT_DITHER` | `#E0E0E0` / `#C8C8C8` checkerboard (a step lighter than a checked key, so black text reads) | title bars, a pressed key |
  | `DOT_DOTTED` | every other dot `#B4B4B4` | panel frames, nav keys, disabled keys, beat lines, the strum stem |
  | `DOT_SHADE` | dark checkerboard (`#404040`) | nothing at the moment: busy behind dense content, so the out-of-scale rows, the selected Rytm track, the song's lanes and the voice page's heatmap use even greys (`#262626`, `#1C1C1C`, `#303030`) |
  | `DOT_ALERT` | the teal and its tint | the power warning's bar |

  A dithered bar therefore costs one flat fill.
- **Black is black:** the background (`bg`) is `#000000`, an unlit dot is written as 0 (its grid lines then too). A
  dark navy look on an HDMI panel is the panel's black level / backlight.
- **Title bars:** 13 dots high, the text starts 7 dots in from the left edge.
- **Brightness** is applied in the expansion as well: the lit dots are scaled, and no dimming layer is drawn. CLASSIC keeps its black dimming layer.

## Font (OLED)

`pi/ui/dotfont.py` is the source. It draws the glyphs as ASCII art and generates `sidecar_dotfont.c`. To change the font, edit the art there and run the script.

- **The faces were designed for Sidecar.** No third-party font is used and nothing was traced, so there is no licence to note.
  - Squared caps with Elektron-like arches on N, M and W, and squared digits.
  - Lower case draws as upper case.
  - The LVGL symbols the pages use (play, pause, stop, the arrows, up, down, trash, OK, warning) have their own glyphs.
- **The faces, as LVGL fonts** (`sidecar_theme.c`): A8 glyphs of only 0 and 255 (crisp dots), with one dot of line spacing above and below.

  | face | rows | role | on screen |
  |---|---|---|---|
  | `DOT 5x9` | 9, proportional 1-5 columns | `f_small`, `f_body`, `f_title`: text, keys, values, title bars | 18 px caps (the 3x3 grid's 4x6 body: 18 px) |
  | `DOT 5x9` bold, 2x | 18 | `f_big` | 36 px |
  | `DOT 5x7` | 7 | `f_micro`: captions, rulers, the machine names, the nav column's labels, note names in the roll | 14 px (the 3x3 grid's 3x5: 15 px) |

  `DOT 5x9` is `DOT 5x7` stretched by two rows (`dotfont.py` `stretch59`: one row in the upper half, one in the lower,
  a row that repeats its neighbour where there is one), with its own art for the glyphs that do not stretch well
  (diagonals: K V X Y Z 7 / < >, the symbols). The text keeps its physical height and gets narrower (12 px a character
  instead of 15), so more fits on a line. `DOT 4x6` and `DOT 3x5` stay in the font file (the 3x3 grid's faces).

  History: the body text was 5x7 at 3 px first, then 4x6 after the first look on a real 5" panel (18 px caps read well
  at arm's length); the 2x2 grid keeps that 18 px.

- `sidecar_dotfont.h` and `sidecar_dot.c` have no LVGL in them, so the boot screen draws with the same faces.

## Colours

Both themes take their colours from one token table, `sidecar_theme_t` in `sidecar_theme.h`.

**OLED** is dot-matrix greys with the Rytm MKII's **panel** colours as its only hues: no LED hues, no red, no amber.

- **Greys:**
  - off: `#000000`
  - faint: `#5A5A5A`, for disabled items and unlit ladder or ring dots
  - dim: `#A8A8A8`, for captions
  - mid: `#C8C8C8`, for key frames and knob caps
  - (2026-10-20, after the Pi: a step lighter than the first cut; the 4x6 body face is thinner, so the same greys read
    darker. Lines and keys moved with them: beat `#5C5C5C`, step `#282828`, page `#B0B0B0`, piano keys `#848484` /
    `#303030`, muted `#707070`.)
  - bright: `#E6E6E6` (the panel's light print), for text
  - white: for what plays
- **One accent, the FUNC key's teal.** rytm-emu's `#2B5A6E` (CLASSIC's `HW_FUNC`) at full dot brightness, with its hue kept, is `#5ABCE6`. Its light tint `#9CD7F0` marks the selection and anything that needs attention.
- **One secondary, the panel's blue print** (`#2C5C9E`, CLASSIC's `sub`), brightened, for single notes and FX 1.

Different states are told apart mostly by brightness, dither and outline, not by hue. The piano roll matches the look of the SEQ animation (`seq-oled.mp4`).

| state | look |
|---|---|
| a note | blue print, brightness = velocity (`th_oled_secondary`) |
| a chord (a step with 2+ notes) | FUNC teal, brightness = velocity (`th_oled_accent`) |
| playing | white (the notes under the play head, the playing page's LED bar) |
| held / selected | the teal's light tint, a white frame; the card's frame and its lane bar the same |
| muted | grey |
| Rytm trigs | blue print by velocity; an accent: a white top row; a lock trig: a dotted outline; a condition: a dotted underline |
| p-locks | grey bars, height and brightness = the value; the lane names and the lock marker teal |
| REC | a white LED blinking with the take's seconds (title bar, HOME's bar) |
| REC page: recording | the REC key lit teal with a black stop square, the clock in the teal's tint, the armed channels' squares the tint |
| REC page: armed | a ring on the REC key, its centre blinking in the tint; DISARM's LED the tint; the clock white at 0:00:00 |
| REC page: a meter | ladder segments: dim grey below -18 dB, white to -6 dB, teal above; a white held peak; the clip lamp the tint (3 s); a channel not recorded: every segment faint |
| connected | a teal LED in the title bar |
| errors (a missing user machine, underruns), KILL TAILS, delete | the teal's tint |
| the power warning | the title bar dithered teal / tint, black text |
| send FX | FX 1 blue print, FX 2 white |

`th_led` maps the palette indices the pages pass to these colours in OLED: the white ramp becomes white, green becomes teal, everything else the tint.

The trig keys on the Rytm itself keep core/seq's LED colours (note 47, chord 79, play 111, held 63). The screen follows the same order (note / chord / play / held) in panel colours.

**CLASSIC** takes its colours from rytm-emu's `src/gui/hw.rs` (our own code):
- Panel `#B3B5B8`.
- Keys `#2B2D30`, with `#E8E8E8` print, a `#7E8084` drop shadow and a `#141517` edge.
- The FUNC key `#2B5A6E`, pads `#E4E5E6`.
- Dark print `#25272A`, blue sub-labels `#2C5C9E`.
- Display windows `#08090A` with a `#3A3C40` bezel.
- A lit key: a 2 px frame in its LED's colour, a glow ring, and LED-tinted print.

## Components

Every widget is a plain `lv_obj` drawn in its `DRAW_MAIN` by the current theme's painter. There are no LVGL themes and no styles per state, so a switch only changes a table pointer and rebuilds the pages.

| widget | OLED | CLASSIC |
|---|---|---|
| `th_key` (kinds: key, accent, danger, FUNC, trig, pad, nav) | black, a 1-dot frame; checked: inverted (light grey, black text); pressed: dithered; disabled: a dotted frame, faint text; accent: a double frame; danger: the teal's tint; an LED: a corner block, or a bar under the text on trig and nav keys | the rubber key; FUNC teal; trig keys with an underlined number; pads light and lit through by their LED; checked = the LED lit (green, or the key's own LED) |
| `th_seg` (one checked; rows with `"\n"`) | items as keys | items as keys |
| `th_panel` (group / window / bare) | a dotted frame, the title in a notch of the top edge | group: a printed hairline frame and dark title; window: the Rytm's display bezel |
| `th_titlebar` (+ LED, alert) | the dithered bar, bold black text left, text right, an LED block | the emulator's dark header strip |
| `th_meter` (bar, vertical, knob) | LED ladders of dots; the knob is a dot ring (19 dots on a big knob, 13 on a small one, lit to the value) around a dotted cap with a black pointer | recessed grooves lit in LED colour; the knob is a knob cap lit from the top left inside an LED ring |
| `th_slider` | a dotted track, a solid value, a 5-dot handle | a groove, an LED fill, a small key as the handle |
| `th_fill`, `th_frame`, `th_text`, `th_paint_key`, `th_paint_knob` | painters for the code-drawn areas (the roll, the grid, the strip) | the same |

**Layout.** Each page lays itself out per theme. `sidecar_ui.c` has an `oled_*` and a `classic_*` builder for every page, which create the same objects, so the status update stays one code path. `sidecar_seqed.c` and `sidecar_rytmpat.c` keep a geometry table per theme (`kGeoOled`, `kGeoClassic`), and their drawing code asks the theme for colours, fonts and line styles.

The OLED screen (400x240 dots):
- **Navigation column** at x 355-399: HOME, SEQ, PAT, SONG, SET, each 45 x 47 dots: a 15 x 11 dot icon (a house, a
  piano roll, trig keys, sliders; `kIcon` in `sidecar_theme.c` also has a back arrow) over its name in the micro face.
  The page shown is inverted. The HOME key's LED bar blinks white while recording.
- **Title bar:** each page starts with one, 352 x 13 dots.
- **Home:** four panels of 174 dots.
  - VOICES: 8 keys (83 x 21); a tap opens that voice's Sound settings.
  - ENGINE: four core-load knobs, Overbridge blocks, underruns.
  - PROJECT: U1..U5.
  - SIDECAR: net, web, link, REC; the touch test in its place until it is hidden.
- **Settings:** a tab row (SOUND, SEND FX, DISPLAY, ABOUT) under the title bar, 22 dots high.
- **SEQ:**
  - The track chips, the page strip (trig keys, 20 dots a page) and SNAP in one row (y 15-36).
  - The roll: grid at x 27-314, y 48-167: 24 pitch rows of 5 dots (20 of 4 before), 9 dots a step at two pages
    (6 before); keys at x 0-23. Pinch: rows 4-15 dots; at 9 dots and more the note names show in the bars.
  - Pitch up / pages / pitch down (34 dots wide) right of the grid.
  - The lane or the tool's controls (y 171-214), and the tools with UNDO and REDO (y 218-239).
- **RYTM:** pages and ALL; a grid of 12 x 16 cells, 16 x 10 dots each (4 x 10 at ALL); the step card at the right, in
  words now (NOTE, VEL, LEN, MIC, the condition, the flags, LOCKS, the first lock by name); the lock lanes at the bottom.
- **VOICE:** the eight knobs in one row (43 x 52), the machine's panel the full width under them (349 x 110): the
  MULTISAMPLE keyboard as a piano (seven white keys an octave, 7-9 dots each, the black keys narrower over them; CLASSIC
  keeps equal keys), its zones and the envelope; the 6-OP FM algorithm; the DAISYSP modes or string; the scope. The
  p-locks heatmap at the foot.
- **MACHINE (GRANULAR / WAVETABLE):** the picture 346 x 178 (the granular waveform as joined top and bottom outlines),
  or the source list: rows of 18 dots, the bank tabs A-H; keys of 34 dots under it.
- **MACHINE (MULTISAMPLE)** (touch2): the instrument's zones (two rows alternating, as tall as their velocity layers; the
  blue print by layers, white while a key in it sounds; the root a tick) over a piano of its span in whole octaves (the
  keys sounding teal, each C named); SOURCES the same bank list as GRANULAR's, of INST.
- **SEQ tools** (touch2): DRAW CHORD STRUM HUMAN KEY SWING, 40 dots each at 400x240 (CLASSIC: a column of six 42 px keys);
  SWING's lane: the info line, then - / the presets 50 .. 80 / + (21 dots tall).
- **SCOPE** (touch2; `sidecar_scope.c`; UP + T9, or a tap on VOICE's scope; no navigation key):
  - The trace (y 15-147, the content's width): min / max columns joined to their neighbours (no gaps on a slope), in
    the light print (FROZEN: the teal's tint); the graticule one fill a line (eight time divisions and the quarter
    lines in the dark checkerboard `DOT_SHADE`, the middle and the frame `DOT_DOTTED`); in its corners the source
    (`TRACK BD` while following), the gain, the time a division, TRIG / FROZEN, the selected channel's peak (teal above
    -30 dB). A sweep leaves a two-column gap at its cursor.
  - The meters (y 151-213): eleven columns of 27 dots (MAIN two ladders L / R), LED ladders of 2-dot segments (dim grey
    below -18 dB, white to -6 dB, teal above; a white held peak; a teal clip lamp on top), each in a dotted frame with
    the channel under it (narrow columns: the first track's name, `RS`); the channel shown: a white frame and its name
    inverted; a press: dithered. The twelfth place is TRACK (FUNC, the followed track under it; checked while following).
  - The keys (y 218-239): FREEZE (FUNC, checked while frozen), the timebase < its value >, GAIN (AUTO, X1 .. X64).
  - CLASSIC: the trace in the Rytm's display window (its white; the graticule dark grey lines), the meters as rubber keys
    with recessed LED ladders (green, amber above -12 dB, red above -3 dB, a white held peak, a red clip lamp; the
    channel shown: the key lit), the keys 52 px.
- **SIDECAR STATUS** (touch2; `sidecar_status.c`; UP + T10, or a tap on the home page's ENGINE panel; no navigation key):
  two columns of dotted panels, captions in the micro face, values in the body face - VOICES (the eight voices by
  track: the track's name in the blue print, the machine, a dot ladder of the render load, full at 50 % of a core, the
  percent), SEND FX, VERSIONS | CPU (four ladders, then the longest block / pass, underruns, FIFO and temperature),
  LINKS. Errors and limits (MUTED, underruns, a core over 80 %, a pass over 2 ms, a block over 8 budgets, the network
  down, under 1 GB free, an update over 16 ms) in the teal's tint (CLASSIC: red). CLASSIC: the same panels as display
  windows on the grey panel.
- **INSERT FX, SONG:** the same layout as before at the new scale; SONG's lane names 45 dots wide (7 characters).
  SONG: UP + T6 on the Rytm (rui1052; its navigation key captioned T6); PROJECT UP + T12, POOL UP + T13.
- **REC** (`sidecar_rec.c`; UP + T7 on the Rytm, or a tap on the home page's REC status; no navigation key, none checked):
  - The transport row (y 15-60): REC / STOP 62 x 46 (a record disc; recording: lit teal, STOP and a square; armed: REC
    NOW and a ring), ARM / DISARM 46 x 46, the take's time as segment digits 10 x 18 (original drawing, the unlit
    segments a dark ghost, as on an LED window) with the take and a status line under it, four micro lines (48K and
    the bit depth, the files, the card's free space, the time it holds at the selection; the time in the tint under
    10 minutes), TAKES 51 x 46.
  - The channels: three columns of eight 21-dot rows under printed group names: RYTM STEMS (BD .. CY/CB, the IN
    stems), SIDECAR VOICES (each named for its track(s) and in the Rytm's track order, so a voice sits beside its
    track's stem), MIX + RETURNS (MASTER, MAIN L / R, IN L / R or FX L / R, FX 1 / FX 2, then ALL / NONE). A row: the
    arm square (filled teal when recorded), the name, a meter of 2-dot segments (stereo sources: two bars, L over R),
    the clip lamp. A tap toggles the channel (`rec=ch:+ID` / `-ID`); only while idle (armed or recording: the
    recorder refuses a new selection, the rows show it disabled).
  - TAKES: the newest takes (name, length, size, files) with DEL (TH_KEY_DANGER, 48 x 19 drawn, 21 dots of touch);
    DEL opens a confirmation (CANCEL / DELETE, a layer behind it takes every other tap as a cancel) that sends
    `rec=del:TAKE`. The last take is in the tint.
  - TAKES playback (the recorder's playback, `pi/engine/rec_play.h`, to the Rytm's MAIN): a tap on a take's row (21
    dots, 28 on the wide screen; the row's whole width except DEL) plays it (`play=toggle:TAKE`: its MASTER mix if the
    take has one, else its first file), a second tap pauses it, a tap on the paused take goes on from there, a tap on
    another take switches to it from its start. STOP (`play=stop`, in ARM's place while TAKES shows) resets to the
    start. The playing row: a play triangle (paused: two bars) in the tint, a frame (OLED) or a tinted row (CLASSIC),
    the length column turned into `elapsed / length` over the size and files columns, and a thin progress bar under
    the name (dotted track, the played part solid; CLASSIC a recessed track, the played part green). Idle rows carry a
    faint triangle. Playback goes on when the page is left and shows again on return; it ends at the take's end,
    when a recording starts or arms, and when its take is deleted. A tap while a take records does nothing. Only the
    bar's strip is redrawn as the position moves (at most 10 a second); the label changes once a second.
  - CLASSIC: the same content in the Rytm's hardware look: a display window with red segment digits while recording
    (amber armed), REC as a rubber key with a red record lamp and its red LED frame while recording, the channels as
    rubber keys with a round red LED when recorded and the meter in a recessed black window (green, amber above -12 dB,
    red above -3 dB, a white held peak, a red clip block), the group names in the panel's blue print.
  - Cost: the meters are the only thing redrawn 25 times a second, each cell's strip alone when only it moved
    (`meter_dyn`; the draw skips the other cells), the clock once a second. Mac (`./shot` prints it): a frame with all 23
    meters moving 24 us OLED / 35 us CLASSIC (Pi 4 est. 0.8 / 1.2 ms), 1024x600 34 / 68 us (1.1 / 2.3 ms); the full page
    250 / 460 us (8.5 / 15.8 ms), 1024x600 345 / 576 us (11.8 / 19.7 ms; a page change only).

## The device's shortcuts (rui1046; UP + AMP rui1047; POLY CONFIG rui1048; the order rui1052)

The Rytm's shortcuts are one table, `core/ui/page_keys.h` (plain C: the core and the Pi's UI read it), not text in the
pages: a future device adds its own table and its firmware mod (the protocol rule; as `core::out::Device` for the OUT
route). The Rytm's (rytm-mods 0026 rui1046) has one modifier, **UP**: TRK is the stock Rytm's busiest key, and UP held
alone already shows the map.

**rui1052: T1..T8 are the touchscreen's sidebar, top to bottom.** The eight navigation keys (HOME SEQ PAT VOICE FX SONG
REC SET, `pi/ui/sidecar_ui.c kNavPage`) and UP + T1..T8 are now one order, so the key next to a sidebar item is the one
that opens it; T9..T13 are the pages with no sidebar row, on the keys they have had since rui1046. Five rows moved:
INSERT FX T6 -> T5, SET T5 -> T8, SONG T11 -> T6, REC T8 -> T7, and SAMPLE T7 -> T11, into the row SONG left. SCOPE,
STATUS, PROJECT and POOL keep their keys, so there is less to relearn. Host side only: the Rytm sends `49 1 <code>`
where the code is the trig number and nothing else hard-codes a key, so no `.syx` and no reflash.

| UP + | what | older form (still works) |
|---|---|---|
| T1..T4 | the touchscreen's HOME, SEQ, PAT, VOICE | |
| T5 | the touchscreen's INSERT FX (the sidebar's FX) | |
| T6, T7 | SONG, REC (the touchscreen's when it has them, else the Rytm's screen) | |
| T8 | the touchscreen's SET | |
| T9, T10 | SCOPE, the Sidecar's STATUS (the same rule as T6 / T7) | |
| T11 | SAMPLE on the Rytm's screen (again: AUTO) | |
| T12, T13 | PROJECT, POOL (the same rule as T6 / T7) | |
| YES | the Sidecar's pages in / out (YES on SIDECAR: the MENU) | TRK + YES |
| TRIG | the selected track's SEQ page (poly track) | TRK + TRIG |
| SRC | the selected track's source list (the POOL bank list of GRANULAR / WAVETABLE / MULTISAMPLE / DSP SAMPLE) | |
| LFO | the Sidecar's LFO page | TRK + LFO |
| FX | the INSERT page | TRK + FX |
| AMP | the INSERT page on its **OUT view** (the stereo / POST route: OUT, LEV, PAN, WID, DEC) of the selected track (new in rui1047; again while open: back to that view) | |
| REC / STOP / PLAY with a trig held first (a poly track) | copy / paste / clear the held steps | trig + FUNC + REC / STOP / PLAY |

Who reads the table: the Rytm screen's KEYS page (every row, then the older forms), UP's map (UP held alone: every
UP + key on one screen, two columns; the trigs lit in their pages' colours), the SIDECAR MENU's shortcut column, the
splash's footer (`UP+YES: SIDECAR PAGES`), the SEQ page's LFO hint, the page requests (`49 1 <code>`:
`core/ui/remote.cpp display_remote_page`, `pi/engine/voice_pi.cpp Request` / `OwnPages`), the touchscreen's navigation
captions (each key's corner says its own key: T1..T8 down the column) and its map (UP held: the same two columns, OLED
and CLASSIC; CLASSIC writes the page's words in its case, abbreviations of up to three letters as they are). A page
request whose page is not in the list now (PROJECT without the bundle feed, POOL without the pools) pops
`PROJECT: NOT HERE NOW` on the Rytm instead of opening on another page; SONG is asked to show (`song=show`: its ARM).

**UP's map adapts to what is connected (rui1052).** The map is one screen - two columns of nine, 18 rows, all there is -
and which screen a page belongs to is the table row's `rytm` field, never its key number.

- **With a touchscreen up** (`core::ui::remote_display_present`: the Pi gave the glass its pages through
  `voice_pi::OwnPages`, which happens only once the panel has *initialised* - a panel configured and failed is no
  panel): as before. The pages only the glass has are one row of their keys, `T1-5,8 TOUCH` - 13 page requests plus the
  6 combos with a map text are 19, one over the 18 - and the touchscreen's own map lists them one by one. 14 rows.
- **Without one** (a Sidecar Mini, `display=0`, or a panel that did not come up): those keys show nothing on either
  screen, so they are left off the map altogether and their trigs stay unlit, and the rows they free carry the UP
  combos that are otherwise off the map - the step-edit trio `T+REC COPY`, `T+STOP PASTE`, `T+PLAY CLEAR` (a trig held
  first, `sc_combo_t.alone`). 16 rows, two spare. The other combos the KEYS page lists are not UP combos at all
  (FUNC + CHRO, a push on SRC, stock's own gestures): under a heading that reads `UP + KEY` they would name the wrong
  key, and a full key plus its page does not fit a 12-character column either, so they stay on the KEYS page.

`core/tests/test_ui` renders both (`build/rui_pages/up_map.png`, `up_map_alone.png`) and checks the rows and the trig
colours; `core/tests/test_page_keys` holds the table to the sidebar's order and both maps to the 18 rows.

**POLY CONFIG (rui1048)** is no UP combo: it is the last item of the Rytm's own track menu (**FUNC + CHRO**, after
TRIGS SETUP, PAD SCALE, RELOAD TRACK, SAVE TRACK and U1..U5; FUNC + DOWN moves five rows), where a Rytm user looks for a
track's settings. YES on it sends `30 1 <0x20 + track> 1 0` (`kPagePoly`): the host opens its POLY CONFIG page for that
track (`core/ui/page_poly.cpp`: VOICES, ALLOCATION, UNISON, DETUNE, SPREAD, GROUP and the info line; NO back to the
stock screen). The table's row `poly` (`FUNC+CHRO`, `MENU: POLY CONFIG`) names it on the KEYS page and in the SIDECAR
MENU's shortcut column (the MENU lists POLY CONFIG first: the selected track's page, NO back to SIDECAR); the SEQ
page's make prompt says `FUNC+CHRO: POLY CONFIG`. An older host reads page 0x20 + t as its SIDECAR page.

**Pages only their keys open, and NO back (rui1051, the user's report on rui1050).** SEQ (UP + TRIG), INSERT (UP + FX,
UP + AMP), POOL (UP + T13, or SRC LIST's) and CHOP (a push on its SRC view) work on what the selected track holds, so
they are not in the SIDECAR MENU and not on the LEFT / RIGHT ring (`page_keys.h sc_rytm_mk2_only`, read by
`sc_shortcut_only`; `core/ui/remote.cpp page_ids`): they open from that track by their keys. The SIDECAR MENU lists
POLY CONFIG, SAMPLE, AUTO SAMPLE, SONG, REC, SCOPE, LFO, PROJECT, STATUS, SYSTEM and KEYS (ALL COMBOS) (`test_ui` holds
the rule, not the count). NO on a page opened by a shortcut goes back to where it was opened from: the stock screen
(the page closes, `3A 0 0`, one NO not two) when it was opened there, the Sidecar page under it when it was opened over
one (two levels kept: SCOPE, then UP + LFO, then UP + T12: NO PROJECT -> LFO -> SCOPE -> SIDECAR); a page opened from
the SIDECAR MENU goes back to SIDECAR as before, and LEFT / RIGHT forget the way back (NO from a page reached by the
ring is SIDECAR, as before). A page's own NO comes first (the INSERT page's OUT / BUS view back to TRACK, LFO's design
view, POLY CONFIG from the track menu back to stock).

**The SEQ page's own gestures (rui1050)** are KEYS rows, no UP combo (UP's map stays one page): `TRIG` (the track pages:
DEFAULTS, SCALE, RANDOM), `T+TRIG` (a held step's TRIG, RTRG, FX), `LONG PUSH` (a lock page: that lock's COND), `FUNC+PUSH`
(its SLIDE). UP held alone over an open page (rytm-mods rui1050's `49 1 5`) shows the map over it and the page again at
the release.

UP + AMP is `30 1 3 1 0` (the page byte 3, after UP + FX's 2 and UP + LFO's 1; `core/ui/remote.cpp kPageInsOut`): the host opens
the INSERT page and sets its OUT view for the Rytm's selected track (`ins_open_out`). An older host does not know page 3 and
shows the SIDECAR page. The Rytm's UP map has 18 rows, so its first row says `T1-6 TOUCH` for the pages that only the
touchscreen has (the touchscreen's own map still lists each).

Renders of the Rytm-screen pages (host renderer, no firmware data): `core/build/rui_pages/` (`make -C core test`:
menu_*, keys_*, up_map, out_view, out_track_view, project_rename_prompt; poly_* and make_* the POLY CONFIG page and the
make prompt, test_poly_page), `core/build/seq/trig_*` (the poly track's TRIG
view), `core/build/rui_pool/`, `core/build/rui_project/`.

## Cost

Measured on the Mac by `./shot` (it prints them).

touch2's pages (Mac us; Pi 4 est. at the draw factor x34, an upper bound):

| redraw | OLED 800x480 | CLASSIC 800x480 | OLED 1024x600 | CLASSIC 1024x600 |
|---|---|---|---|---|
| SCOPE, full (a page change) | 170 us (5.8 ms) | 347 us (11.8 ms) | 233 us (7.9 ms) | 477 us (16.3 ms) |
| SCOPE, a triggered trace + the meters (15 a second) | 127 us (4.3 ms) | 162 us (5.5 ms) | 180 us (6.2 ms) | 246 us (8.4 ms) |
| SCOPE frozen: the meters' strips | 58 us (2.0 ms) | 59 us (2.0 ms) | 78 us (2.7 ms) | 83 us (2.8 ms) |
| SIDECAR STATUS, full | 261 us (8.9 ms) | 404 us (13.8 ms) | 325 us (11.1 ms) | 497 us (16.9 ms) |
| SIDECAR STATUS, a second's update | 35 us (1.2 ms) | 32 us (1.1 ms) | 32 us (1.1 ms) | 32 us (1.1 ms) |

- A sweep (171 ms and longer) redraws only the columns written since the last trace, the gap and the peak's corner; a
  triggered trace (43 / 85 ms) is new every time and redraws the trace's area. SCOPE at 15 traces a second costs core 0
  at most ~65 ms a second (OLED 800x480, the x34 bound), about a tenth of what the REC page's meters cost per frame
  times its 25 frames, so the `longest update` line stays about one trace.
- The theme switch's rebuild grew by the two pages' objects (~0.4 ms on the Mac, ~3-14 ms on the Pi 4 between the
  CPU-bound and draw-bound estimates); the wipe's bands are unchanged. The flush and the dot expansion are included.

**How the Pi 4 is estimated** (all Pi figures are inferred, not measured on hardware):
- Draw-bound work uses the factor docs/TOUCH-EDITOR.md uses: the old home page took 217 us on this Mac and 7.4 ms (measured) on the Pi 4, x34.
- The OLED expansion is plain CPU work, so x34 overstates it.

| redraw | OLED Mac | OLED Pi 4 est. | CLASSIC Mac | CLASSIC Pi 4 est. |
|---|---|---|---|---|
| home, full | 205 us | 7.0 ms | 330-390 us | 11-13 ms |
| SEQ, full | 175 us | 6.0 ms | 380 us | 13 ms |
| SEQ grid (an edit's refresh, a scroll) | 42 us | 1.4 ms | 116 us | 4.0 ms |
| a note-drag frame | 4 us | 0.13 ms | 5 us | 0.16 ms |
| the play head's strip | 8 us | 0.27 ms | 10 us | 0.34 ms |
| a playing frame (the head and the notes lighting under it) | 14 us | 0.5 ms | 23 us | 0.8 ms |
| RYTM, full | 170 us | 5.8 ms | 370-390 us | 12.5-13.3 ms |

At **1024x600** (1.6x the pixels) a full redraw costs about 1.3x (the text and keys do not grow, the panels' fills do):

| redraw, 1024x600 | OLED Mac | OLED Pi 4 est. | CLASSIC Mac | CLASSIC Pi 4 est. |
|---|---|---|---|---|
| home, full | 250-260 us | 8.7 ms | 485-500 us | 16.8 ms |
| SEQ, full | 225 us | 7.7 ms | 500-510 us | 17.2 ms |
| SEQ grid | 65 us | 2.2 ms | 168 us | 5.7 ms |
| a playing frame | 15 us | 0.5 ms | 28 us | 0.95 ms |
| RYTM, full | 212 us | 7.3 ms | 485 us | 16.5 ms |
| GRANULAR / WAVETABLE frame | 145 / 200 us | 4.9 / 6.9 ms | 245 / 360 us | 8.3 / 12.2 ms |

**Why OLED is about 2x cheaper:** LVGL renders 96,000 dots instead of 384,000 pixels (42,560 on the 3x3 grid, which made it about 3x cheaper), and the expansion is 4 stores a dot. Every full redraw stays under 15 ms in both themes at 800x480; at 1024x600 OLED stays under 9 ms, CLASSIC's full redraws reach about 17 ms (page changes only).

**Theme switch.** It runs on core 0's loop only. Rendering (cores 1-2), control (core 3) and the IRQ-driven audio paths are untouched, so the audio cannot glitch.
- **Rebuild:** the pages are rebuilt with invalidation off, about 0.8-1.4 ms on the Mac. That is ~10 ms on the Pi 4 if CPU-bound (inferred, x8) and ~48 ms at the draw factor (x34, the upper bound).
- **Redraw:** in bands, one band per core 0 Update (4 for OLED, 6 for CLASSIC): at most 0.11 ms on the Mac, ~4 ms on the Pi 4. On screen it is a wipe from the top.
- **Delay:** core 0's loop (MIDI to the Rytm, the console, the TCP link) waits once, for the rebuild.
- **To check on hardware:** the `longest update` in the engine's 10 s line, during a switch.

**Pi 5 on scaled HDMI:** the expanded 800x480 goes through the existing scaler (`CSidecarDisplay::Put`), so a dot becomes a cell of 4-6 frame-buffer pixels with its grid lines scaled along. That costs the same as CLASSIC's scaled full redraw.

## Adding a theme

1. **Table.** Add a `sidecar_theme_t` to `sidecar_theme.c` (`THEME_*` id, name, dot size, UI size, colour tokens, fonts, radius, gap, touch) and return it from `sidecar_theme_get`.
2. **Painters.** Give `th_paint_key`, `panel_draw`, `title_draw`, `meter_draw`, `slider_draw` and `th_paint_knob` a branch for it, or reuse one of the two looks with other tokens.
3. **Layout.**
   - The layouts grow from `TH->dx, dy` ("Sizes"); a new theme with its own grid needs `kGeo*` tables in
     `sidecar_seqed.c` and `sidecar_rytmpat.c` (their `geo_size` grows them for 1024x600), and builders in `sidecar_ui.c`.
   - A theme with the same pixel size reuses the OLED or CLASSIC layout (`oled ()` asks for the id).
   - A new screen size: `sidecar_theme_screen` and `sidecar_theme_set` (the table's `w, h, dx, dy`), and the checks in
     `shot.c` (`DX`, `DY`).
4. **Expansion.** A dot size other than 1 or 2 needs `sidecar_dot_expand` generalised; today it is written for 2.
5. **Wiring.** Add the name to the Display page's `kStyleMap`, to `uistyle=` in `pi/engine/kernel.cpp` (command, `display.txt`), and to the host render's `theme_run`.

## Files

| file | what |
|---|---|
| `pi/ui/sidecar_theme.{h,c}` | the theme tables, the dot fonts as LVGL fonts, widgets, painters |
| `pi/ui/sidecar_rec.{h,c}` | the REC page (the Overbridge recorder), filled by `pi/engine/rec_pi.cpp` (`UiFill`) |
| `pi/ui/sidecar_scope.{h,c}` | SCOPE, filled by `pi/engine/scope_pi.cpp` (the tap) and `rec_pi.cpp` (`ScopePeaks`: the recorder's meters) |
| `pi/ui/sidecar_status.{h,c}` | SIDECAR STATUS, filled by `pi/engine/kernel.cpp` (the stats, the links) and `status_pi.cpp` (machines, the mod level) |
| `pi/ui/sidecar_dot.{h,c}` | the dot expansion, the pattern colours, the LED palette, plain dot-font drawing (no LVGL) |
| `pi/ui/dotfont.py`, `sidecar_dotfont.{h,c}` | the pixel fonts (generated) |
| `pi/ui/sidecar_ui.{h,c}` | the pages, both layouts, `sidecar_ui_style`, `sidecar_ui_dot` / `origin` / `to_ui` / `bright` |
| `pi/ui/sidecar_seqed.c`, `sidecar_rytmpat.c` | the pattern pages with a geometry table per theme |
| `pi/ui/sidecar_splash.{h,c}` | the boot screen in both looks (`sidecar_splash_style`) |
| `pi/ui/sidecar_display.cpp` | the Pi's flush (expansion), pointer and second finger in UI pixels; the UI's size (`ChooseUi`, `res=`, the EDID) and its place (centred) |
| `pi/engine/kernel.cpp` | `uistyle=`, `display.txt`, the status field, the boot screen's look from the cmdline |
| `pi/ui/host/shot.c` | the host render, both themes: every page, the controls' commands, the SEQ gestures, the costs, the animation |

## STAGE look and the alert grammar (UX review L1, L2)

- **Display > LOOK: STUDIO / STAGE** (OLED; `uilook=studio|stage`, stored in `/.rytm/display.txt`). STUDIO is the dot
  look above and stays the default (the review asks for the setting, not a new default). STAGE draws every dot as its
  whole 2x2 cell (no grid lines) and lifts every grey a step (`v -> 0x30 + v x 0xD0 / 0xFF`, `sidecar_dot_look`), for a
  stage and an angle. The bold face of the review is not in STAGE yet: every layout is measured for the regular face and
  the text-fit check would fail (deferred).
- **Alerts** (OLED): the attention colour (`TH->bad`, the FUNC teal's tint) is never plain text: `th_label_color`
  inverts the label (black on teal), so teal means "look here". HOME's U1..U5 errors blink for 3 s, then stay inverted,
  with the reason spelled out; loading is dim with moving dots. Playing is white (REC's playing take), the selection
  inverted grey, single notes and FX 1 the panel's blue. CLASSIC keeps the hardware's LED colours for the same tokens.
