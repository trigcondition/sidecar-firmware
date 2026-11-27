# UI themes: the Pi display's design system

The Sidecar Pi's touchscreen (800x480, 1024x600 or 1280x720, LVGL 9.4, `pi/ui`; docs/pi-display.md) has three themes (theme v2: SIDECAR and GRID share one layout language), each laid out
for all three sizes ("Sizes" below):

| theme | what | default |
|---|---|---|
| **OLED** | The whole screen as a zoomed-in Elektron OLED: 400x240 dots, each a 2x2-pixel cell with a darker grid between, black, our own pixel font, dotted outlines, grey dithered title bars, inverted selection, knobs as dot rings, greys plus the Rytm panel's FUNC teal and blue print. A fixed navigation column on the right. | yes |
| **SIDECAR** | The web app's palette in GRID's layout language ([SIDECAR and GRID](#sidecar-and-grid-theme-v2)): a UV night-blue screen (`#08071a`), panels (`#110f28`, a `#6a64a0` frame), 2 px edges (3 px in the high-contrast look), a small radius like the Rytm's pads and buttons, no pills and no dot font. Keys and segments are separate framed boxes (selected: filled lavender, dark text), knobs 270° arcs with square ends and a chalk tick, meters boxes with a block of colour, the piano roll's selected note coral. Text in **Arimo Bold in mixed case** (panel labels in capitals), 14 / 19 / 24 px and 44 for the hero number at 1280x720. A header with the "Docked" logo, SIDECAR and the page's name; from 1024x600 a T1..T8 rail on the right, at 800x480 the same keys as a row in the header. Palettes: Standard, **High blue**, **High green** and **Viper** ([High contrast](#high-contrast-sidecar-051), [Viper](#viper-contrast-option)). It replaced CLASSIC (the Rytm's hardware look) under the same id. | |
| **GRID** | The style guide's look: a dark army-green screen (`#081008`), signal green only for the active and the selected, sage notes, 2 px borders, Arimo Bold in capitals at the same sizes, panels and the keys of a group on one grid, the same radius rule. | |

**Switching:** Settings > Display > **UI style: OLED / SIDECAR / GRID**, or the console / web command `uistyle=oled|sidecar|guide`. SIDECAR has a high-contrast option, Settings > Display > **Contrast** ([High contrast](#high-contrast-sidecar-051)).
- Applied at once, no reboot (`sidecar_ui_style`).
- Stored as `uistyle=oled|sidecar|guide` in `/.sidecar/display.txt`, with the screensaver, brightness and touch test settings.
- **From CLASSIC:** SIDECAR took CLASSIC's id 1, so a card or cmdline.txt that says `uistyle=classic` (or `1`) comes up in SIDECAR, and the next save of the display settings writes `uistyle=sidecar`. `classic` stays accepted by the command as well. A build from before SIDECAR does not know `sidecar`, and one from before GRID does not know `guide`: they fall back to OLED, the default. **SQUARE is gone** (it was in the 0.5.1 test build only): a stored or commanded `uistyle=square` (or its number, `2`) loads as GRID, and so does the old GRID number `3`; the next save writes `uistyle=guide` (`ui_style.h`, `tests/test_ui_style.cpp`). The ids are now OLED 0, SIDECAR 1, GRID 2. **GUIDE was renamed GRID** (what the Display page and these docs show); the stored word stays `guide` (so a saved `uistyle=guide` still loads GRID and a save writes `guide`), `uistyle=grid` is accepted too. The words and the file's line are `pi/engine/ui_style.h` (`tests/test_ui_style.cpp`).
- `uistyle=` in cmdline.txt overrides the file. It is also the only way to give the boot screen the SIDECAR look (`uistyle=sidecar` or `guide`: the logo over the wordmark on the night blue; GRID has no boot screen of its own), because the boot screen is drawn before the card is read.

Every page exists in both themes: home, Settings (Sound, Send FX, Display, About), SEQ (the piano roll with chord stamp, strum, humanise, key, swing, velocity lane, page strip), RYTM (the pattern and its p-locks), REC (the Overbridge recorder: "REC" below), SCOPE and SIDECAR STATUS ("SCOPE", "SIDECAR STATUS" below), the machine view (GRANULAR, WAVETABLE, MULTISAMPLE), the boot screen and the power warning.

The display has no LFO, Wi-Fi or preset/pool browser pages. Those live on the web page and the Rytm's remote pages, which are outside `pi/ui` and unchanged.

Renders: `make -C pi/ui/host` writes `pi/ui/host/shots/{oled,sidecar}-*.png` (`make shots-grid`: `grid-*`; `make shots-hc`: the high contrast looks), `make -C pi/ui/host shot.png SIZE=1024x600` the same at 1024x600 (`shots-1024x600/`), `SIZE=1280x720` at 1280x720, `make -C pi/ui/host bounds` compares the 1024x600 and 1280x720 layouts (`check_bounds.py`), `make -C pi/ui/host splash.png` the boot screens and `make -C pi/ui/host anim` an animation of the OLED piano roll (`seq-oled.mp4`, `seq-oled.gif`).

## Sizes (800x480, 1024x600, 1280x720)

- **Which:** the display hands LVGL a screen of 800x480 or 1024x600 (docs/pi-display.md "Screen sizes": the frame
  buffer's size, `res=`, else 800x480 centred), or 1280x720 on Touch Display 2. `sidecar_ui_create` passes it to
  `sidecar_theme_screen`: 1024x600 (or more) gives the wide layouts, 1280x720 (or more) the big ones.
- **The theme table carries the size:** `TH->w, h` (OLED 400x240 or 512x300 dots, SIDECAR 800x480 or 1024x600 px less the rail, `TH->nav`),
  `TH->bw, bh` (the 800x480 layout's size in the theme's pixels) and `TH->dx, dy` (what the screen adds: 0 x 0, or OLED
  112 x 60 dots, SIDECAR 128 x 120 px: 224 less the 96 px rail). The dot stays 2x2 pixels, the fonts the same faces: text keeps its physical
  size, touch targets stay at least 21 dots (42 px) / 48 px.
- **One layout, grown:** every page is written once, as its 800x480 layout plus `dx`/`dy` (no second copy). What grows
  gets the extra room, what sits at an edge moves with it:
  - Panels and lists get wider and taller; key rows keep their height and get wider keys; the OLED navigation column
    moves to the right edge, its five keys 59 dots high instead of 47.
  - Settings pages: positions and heights stretched to the taller panel (`VS`, `CY`/`CH` in `sidecar_ui.c`).
  - SEQ (`sidecar_seqed.c` `geo_size`): the roll gets the width in whole dots a step (OLED 12 instead of 9 at two pages,
    SIDECAR whole pixels a step from the width left of the rail) and two thirds of the height (32 rows of 5 dots instead
    of 24; SIDECAR 360 px of rows instead of 280), the lane the rest (OLED 64 dots instead of 44: the tools' controls taller); the page strip's cells 27 dots wide.
  - RYTM (`sidecar_rytmpat.c` `geo_size`): 20 dots a step instead of 16 (SIDECAR: the width left of the rail, in whole pixels), rows of 13 dots (28 px), up
    to 6 lock lanes instead of 4, a wider step card.
  - VOICE, MACHINE, INSERT FX, SONG: the picture, the list (more rows), the timeline get the room; the overlays keep their
    size, centred.
  - SCOPE: the trace gets two thirds of the extra height, the meters the rest (taller ladders), every column wider;
    the keys row keeps its height. SIDECAR STATUS: wider columns, taller rows.
  - REC: the channel rows get the height (OLED 28 dots instead of 21, SIDECAR 57 px instead of 42), the columns and
    their meters the width (more segments); the transport row keeps its height; the takes' rows grow the same way
    (SIDECAR: 8 rows of 54 px instead of 7 of 44); the delete's confirmation keeps its size, centred.
- **Unchanged at 800x480:** the OLED host renders at 800x480 are pixel-identical to before the change.
- **1280x720 (640x360 dots, SIDECAR 1280x720 px, the rail 120 of them):** the panel is 7" like the 1024x600 ones, so its pixels are smaller
  (0.12 mm against 0.15) and the same pixel sizes would read a fifth smaller. The layout is not a scaled picture:
  - **`LU (n)`** (`sidecar_theme.h`), n x 5/4 here and n elsewhere, wraps the base layout's numbers. The base comes out at
    500x300 dots / 1000x600 px; `TH->dx, dy` is what is left on top (140 x 60 dots, 280 x 120 px) and goes where it goes at
    1024x600. A key's height, a row, a margin are in LU; a line stays one dot wide, a grid stays in whole dots.
  - **Faces:** OLED micro 5x9, body and title 6x11 (DOT 5x9 a column wider and two rows taller, `dotfont.py`), big 6x11 bold at
    2x. SIDECAR and GRID: Arimo Bold in four steps (micro, body, title, hero), a set per screen size, the steps of 1280x720 as designed
    and smaller ones below ("Type" under SIDECAR and GRID): 14 / 19 / 24 / 44 px at 1280x720, 12 / 16 / 20 / 36 at 1024x600, 11 / 14 / 18 / 30 at 800x480.
    The pages take them from `TH->f_*`; `sidecar_theme_set` picks the set.
  - **Touch:** a key row 26 dots (52 px), SIDECAR 60 px or more; `TH->gap` one more; the tightest rows (SEQ's tool column, a few short keys in the lanes, CHOP's tabs) 40-46 px. The nav icons have each dot doubled.
  - SEQ: OLED 16 dots a step at two pages (8 at four, whole), SIDECAR whole pixels from the width left of the rail. RYTM: 25 dots a step, SIDECAR whole pixels from the width left of the rail.
  - SCOPE, VOICE and GRANULAR draw more pixels than their view has columns: only here the columns are joined by a slope.
  - `make -C pi/ui/host bounds`: every object of every page against 1024x600: on the screen, the page filling it, no
    control lost, every touch target at least 1.12 x its size there (or 60 px), no clickable siblings overlapping.

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
- **1024x600:** 512x300 dots, the same 2x2 cell ("Sizes"). **1280x720:** 640x360 dots, the same cell.
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
- **Brightness** is applied in the expansion as well: the lit dots are scaled, and no dimming layer is drawn. SIDECAR keeps a black dimming layer.

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
- **One accent, the FUNC key's teal.** rytm-emu's `#2B5A6E` (the Rytm's FUNC key) at full dot brightness, with its hue kept, is `#5ABCE6`. Its light tint `#9CD7F0` marks the selection and anything that needs attention.
- **One secondary, the panel's blue print** (`#2C5C9E`, the panel's sub-label print), brightened, for single notes and FX 1.

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

**SIDECAR** takes the web app's palette (`UV_*` in `sidecar_theme.c`); every pair checked for contrast at the size it is
used (WCAG: text 4.5:1, controls 3:1):

| token | colour | used for | contrast |
|---|---|---|---|
| screen | `#08071a` | the background | |
| card | `#110f28`, border `#2c2950` | panels, windows, rows, the rail `#0d0a2a` | |
| control border | `#6a64a0` | key and segment outlines | 3.5:1 on a card |
| text | `#eeedff` | values, names | 16:1 on a card |
| labels | `#bdb9e8` | captions, hints, secondary text | 10:1 |
| faint | `#8a85c0` | disabled text | 5.5:1 |
| accent (lavender) | `#9a97ff` | selection (dark text on it), values, arcs, FX 1, the A side, meters | 7.3:1; `#08071a` on it 7.8:1 |
| coral | `#ff7a5c` | recording, the selected note, FM carriers, FX 2, the B side, LEV knobs, alerts, a hot meter's top | 7.3:1 |
| magenta | `#d46cff` | p-locks, a meter between -12 and -3 dB, the crossfader's middle | 6.6:1 |
| header | `#1a1150` -> `#14103a` -> `#2a1033` -> `#120e36` | the title bar, a flat gradient of three fills (no blur, no shadow) | |

### High contrast (SIDECAR, 0.5.1)

An option of the SIDECAR theme, after the web app's `data-theme="contrast"` (`web-next/src/styles/tokens.css`): the same
layouts, fonts and sizes, only colours, border widths and a few treatments change. `THEME_SIDECAR` stays the theme's id (pages that branch on
`TH->id` need nothing), `TH->hc` says which look (0 standard, 1 blue, 2 green) and `TH->edge_w` is the card / control border width (1, or 2 here).

**The setting:** Settings > Display > **Contrast: Standard / High blue / High green / Viper** (a row of the SIDECAR Display page only;
the OLED page has none), the console / web command `uicontrast=off|blue|green|viper`, stored as `uicontrast=off|blue|green|viper` in
`/.sidecar/display.txt` (default `off`; a card without the key, or a word not known, reads as `off`; the OLED theme ignores it and keeps
the stored value). The status carries it (`ui_contrast`, 0 / 1 / 2 / 3 (viper), -1 not known; the numbers of the older looks keep their meaning), like `ui_look`. A change rebuilds and redraws
the screen as a theme change does (`sidecar_theme_contrast`, then `rebuild`). The words and the file's line are
`pi/engine/ui_contrast.h` (`tests/test_ui_contrast.cpp`). The Rytm's SIDECAR MENU does not list the theme or the OLED look, so it has no
contrast entry either (it would need a Rytm mods change).

| role | standard | high contrast |
|---|---|---|
| screen, cards, rail, header | night blue, a glow in the header | `#000000`, no texture, no glow (a white line under the header) |
| card and control edges | `#2c2950` / `#6a64a0`, 1 px | `#ffffff`, 2 px |
| card's pill label | accent fill, dark text | black fill, white 2 px edge, white text |
| text / labels / disabled | `#eeedff` / `#bdb9e8` / `#8a85c0` | `#ffffff` / `#e6e6e6` / `#a6a6a6` |
| accent | `#9a97ff` | `#8fd3ff` (blue) or `#80d553` (green, the Rytm's mute-pad colour; 11.6:1 on black) |
| selected key / tab / segment | accent fill, dark text | accent fill, black text |
| coral roles (recording, warnings, selected note, LEV arcs, missing items, a hot meter's top), p-locks | `#ff7a5c`, magenta `#d46cff` | `#ffd166` |
| logo | its hues | plain white dots |
| knob | track `#2c2950`, value accent, light dot | track `#767676` (4.6:1), value accent, white dot with a black edge |
| piano roll | lavender rows and lines | rows `#000` / `#161616`, root rows the accent at 16 %, lines `#333` step / `#777` beat / `#fff` page, play head white, notes the accent with black text, the selected note yellow; keys `#e6e6e6` and black, each edged white |
| meters, sliders | filled tracks | a white outline, the accent inside (yellow past 90 %); a slider's track `#767676` |
| disabled key | `#2c2950` outline | `#767676` outline, `#a6a6a6` text |

Disabled keys keep a solid outline in the muted track colour and the faint text colour (high contrast: `#767676` and `#a6a6a6`, both readable on black); the dashed edge belongs to the OLED look's dotted disabled keys only. A p-lock keeps the Rytm page's own cues besides its colour (the dotted underline under a trig). What is still drawn in its own colours in this look: the boot screen (`sidecar_splash.c`, drawn before the setting is read), and the help
texts that name colours ("Lavender: FX 1, coral: FX 2" on Send FX).

The shots: `make -C pi/ui/host shots-hc` (`sidecar-hc-*`, `sidecar-hcg-*` beside the others; `./shot DIR [WxH] hc|hcg` is one look), and the plain
`./shot` also taps the Contrast row (`uicontrast=`, the rebuild, a stored value, the OLED theme ignoring it). `./shot` checks the
high-contrast tables against the contrast targets (text 4.5:1, controls and lines 3:1, WCAG) and fails on a miss.

Touch targets are 46 px or more at 800x480 and 1024x600 (`TH->touch`), 57 px at 1280x720 (the T1..T8 rail's keys
72 px tall at 1024x600, 86 at 1280x720); `make -C pi/ui/host bounds` checks them against 1024x600.

### SIDECAR and GRID (theme v2)

Two themes after the interface style guide (the compact sequencer, the palette, Arimo, the grid), next to OLED. **They share the layouts, the
painters and the type**: the pages test `oled ()` only (`THEME_BOXED (id)`), and a table and a palette differ: `THEME_GRID = 2`, `THEME_SIDECAR = 1`, `uistyle=guide|sidecar`.
SIDECAR keeps its own palettes (Standard, High blue, High green: `contrast_apply`, `sidecar_theme_contrast`, SIDECAR only: on the Display page the Contrast row is greyed in
GRID and OLED); GRID has the green palette below. (SIDECAR's earlier look, pills and cards with the dot face, and the 0.5.1 test build's SQUARE are gone.)

**GRID palette** (the guide's eight colours, then the extras it has no token for; `grid_apply` sets the painters' `UV_*`). The dark values are
RGB565-exact (R and B multiples of 8, G of 4), because LVGL runs RGB565 on the Pi and the host: the expansion does not replicate bits, so an arbitrary
hex shows 1-7 levels darker than written, and dark tints skew green. What the table says is what the screen shows.

| role | hex (v1 in brackets) | used for |
|---|---|---|
| screen, panels, header | `#081008` (`#171916`, shown as `#101810`) | the army-green dark behind all text |
| signal green | `#31bd3e` | the selected key, tab and segment (filled, ink text), active values, knob arcs, status; only the active and the selected |
| sage | `#8dc494` | notes (60 % to 100 % of it by velocity), the logo, a track's machine, FUNC key text |
| chalk | `#f0f0e8` (`#ecefe8`) | primary text, the play head: never `#ffffff` |
| muted | `#c8d0c0` (`#b0bead`) | secondary text, panel labels |
| border | `#687c68` (`#4b5449`) | panel and control borders (2 px, 3 px on a lit or selected pad) |
| deep fill | `#182018` (`#2b342b`) | the piano roll's grid, an empty meter or knob track, the keys of the roll |
| ink | `#081008` (`#0f120e`) | text on green |
| disabled | `#98a090` (`#7e8a7c`) | disabled text |
| amber (extra) | `#e8c25a` | warning, held, p-lock, recording text, a level knob, a meter past 90 % |
| brick (extra) | `#d9674f` | an error, DELETE / KILL keys, FX 2, the record LED |

The piano roll's rows (`#101810` white keys, `#080c08` black keys, `#182c18` the root), the beat line (`#384838`) and the muted tone (`#889888`) are mixes of these. Contrast
(WCAG, v1 to v2): chalk on the screen 15.2 to 16.9, muted 9.1 to 12.2, disabled 4.8 to 7.1, the border 2.3 to 4.3 (a control edge needs 3), ink on green 7.7.

**Type:** Arimo **Bold** only (`pi/ui/fonts/gen-arimo.sh`, `sidecar_arb{11,12,14,16,18,19,20,24,30,36,44}.c`, SIL OFL 1.1, `licenses/OFL-Arimo.txt`), LVGL tables 4 bpp. Four
steps, a set per screen size (`kFace` in `sidecar_theme.c`, `TH->f_*`):

| step (`TH->`) | 1280x720 | 1024x600 | 800x480 | used for |
|---|---|---|---|---|
| micro (`f_micro`) | 14 | 12 | 11 | labels, captions, key subs |
| body (`f_small`, `f_body`) | 19 | 16 | 14 | controls, body text, status |
| title (`f_big`, `f_title`, `f_mid`, `f_num`) | 24 | 20 | 18 | page names, main values, big key labels, a knob's value |
| hero (`f_hero`) | 44 | 36 | 30 | HOME's pattern (A04) |

HOME takes one step more: its supporting text (captions, links) is the body face and its values (machines, perform tiles, health, recorder) the title face. The sets
are what the screens can hold: the 1280x720 set is the design, the smaller screens (which have the same base layout in `LU ()` less room) get smaller steps rather than a shrunk look.
**GRID writes everything in capitals** without touching a string: its faces are `up_*` wrappers (`sidecar_theme.c`) that ask the Arimo table for A-Z when a-z is asked, so the pages'
text stays as written (and `shot`'s text checks still match it); **SIDECAR writes mixed case** (its panel labels stay capitals). The Bold tables hold the ASCII range, the degree and plus-minus
signs but no symbols: the play / arrow / trash glyphs come from the Regular tables (`sidecar_ar13/18/28`, which carry the eleven Font Awesome symbols) as each face's fallback.
Tracking: 2 px on the wordmark, 1 px on panel labels (LVGL's `letter_space` is whole pixels), 0 elsewhere. There is no pixel (dot) font anywhere in SIDECAR or GRID.

**Shape:** a small radius like the Rytm's pads and buttons, `r = clamp (min (w, h) / 10, 3, 6)` and at most a third of the thin side (`rad ()`): a 54 px key 5, a 40 px cell 4, a 10 px bar 3 at
1280x720; a panel gets one more (a big panel 7). Borders keep their width (`TH->edge_w`: 2 px; 3 px in the high contrast looks; a lit or selected pad one more). Circles stay for
the track selectors (`TH_KEY_ROUND`: SEQ's T9 / T10 ..., a circle of the key's smaller side) and for status dots and LEDs. Arc ends are square; a knob's value is a short chalk tick across the arc.

**Components:** a key is a box with a 2 px edge and chalk text; the selected one is filled (green in GRID, the accent in SIDECAR) with ink text; the main action has an accent edge, DANGER a warm one,
a disabled key a track-coloured edge and faint text; pressed is a 30 % tint. A pad (a track on HOME) is outlined: 3 px selected, in its LED's colour when lit. The keys of a selector group are
**separate keys 6 px apart** (`th_seg`). A navigation key (the header row at 800x480, the rail from 1024x600) shows its name and its T key small at the top right. A panel is a frame (`edge_w`,
`rad () + 1`), its label in micro capitals at the top left inside; `snap_edges` moves a panel's frame to the middle of the gap (up to 16 px) to a panel beside or above / below it, where the
neighbour's frame goes too, so one line stands between two panels (the frames of neighbours are drawn one `edge_w` apart so the shared line keeps its width). A dialog's window is filled with the panel colour.
The header is the screen's colour over an `edge_w` line (high contrast: the accent), SIDECAR in the title face with 2 px tracking, the page's name in the title face, the Rytm's state in the body face muted
with its dot; the power warning in the warm colour with ink text. A meter is a box with an `edge_w` frame and a block of colour (a thin one: a bar on a track), a slider a 2 px track with a square handle.

**The high contrast looks** (SIDECAR, `contrast_apply`) keep their colours (black, white text and edges, one accent, yellow for the warm roles) in this layout: 3 px edges, the accent line under the header and along the rail,
mixed-case Arimo Bold. High green (`#80d553`, the Rytm's mute-pad colour) is rendered and checked like High blue (`#8fd3ff`).

### Viper (contrast option)

The fourth Contrast value (`uicontrast=viper`, number 3), SIDECAR only like the others: phosphor **greens only**, no red,
yellow or blue anywhere, in the same layout (`viper_apply`, `OS` in `sidecar_theme.c`). All values are RGB565-exact.

| role | hex | used for |
|---|---|---|
| bg | `#001000` | screen and every panel |
| graticule / track | `#0c3410` | scope divisions, empty meter / track, roll steps (decor, never text) |
| dim | `#28a030` | borders (2 px), disabled text |
| mid | `#48d050` | secondary text, labels, hints |
| bright | `#78f880` | primary text, traces, meters, accent, selection fill (dark text) |
| peak | `#c0ffc0` | alerts, REC, warnings, the play head, the selected note's frame |

WCAG: bright on bg 14.5, mid 9.7, dim 5.7, peak 17.0, ink on a bright fill 14.5, the border on bg 5.7. **Warnings, REC and "bad"** have no hue to use: they are the peak green as an
**inverted** bar (dark text on a peak fill: health lines, the REC banner and the recorder line, the power warning in the header), and **every inverted bar has notched corners**
(a stair-stepped cut at both top corners, `notch`), so an alert is told from a selection (a bright, plain fill) by shape and not only by a 1.18:1 difference in colour. The bar is drawn by `label_alert`, not as the label's background: it is as wide as the text (not the space the label was given) and padded round it (the label's box is only its font's line height, which cut the descenders and put the first glyph against the notch; the pale slab across a whole panel on HOME was this). `th_alert_bar` and a check in `shot` keep it that way. Danger keys keep
a peak edge. Panels at least 150 x 100 px carry a graticule: 8 columns of 1 px divisions, a brighter centre cross with minor ticks, under the widgets. Meters, knob arcs and the
roll are bright traces over graticule-coloured tracks.

**Removing SQUARE:** the theme enum (`THEME_SQUARE`, `THEME_IS_SQ`), its table and `square_apply`, the `s_Up` faces and `sidecar_ar11` (SQUARE's micro), the Space Grotesk tables and the
dot-face hero of the old SIDECAR, `uistyle=square` and the Display row's item, `shot`'s `sq` run, `shots-sq`, `ui_style.h`'s `kSquare`. A stored `square` or `2` loads as GRID.

The shots: `make -C pi/ui/host shots-grid [SIZE=1024x600|1280x720]` writes `grid-*.png` beside the others (`./shot DIR [WxH] grid` is one theme, as `hc` / `hcg` are looks of SIDECAR), and `bounds`
runs OLED + SIDECAR, both high contrast looks and GRID at 800x480, 1024x600 and 1280x720 (the text-fit and check runs, then the layout comparison).

### The pixel accent (SIDECAR, 0.5.2)

The Rytm's pixel font (the OLED look's 5x9 and 5x7 dot tables) comes back as a sparing accent in the four SIDECAR contrasts, drawn bold (two columns a stroke) as round dots, integer scales
only (`f_pxv`, `f_pxh`, `f_pxs`; 0 in GRID and OLED): the **value** face (5x9 at 2 / 2 / 3 on 800x480 / 1024x600 / 1280x720) for the knob values inside the VOICE and INSERT FX arcs and the SEQ
tempo, the **hero** face (5x9 at 3 / 4 / 5) for HOME's pattern, the **small** face (5x7 at 2) for INSERT FX's value row under the captions (in the window ink, with a little more air than the
Arimo row had) and the header strip. The strip (1024x600 and 1280x720, none at 800x480) is a small black box in the header with the Rytm's top line: the pattern, the tempo and PLAY / PAUS /
STOP / REC (`---` for what is not known, `----` without the Rytm). The tempo is `bpm10` in `sidecar_ui_status_t` (from `core::seq::tempo()`, 0 unknown) and in the remote UI's STATUS view
(appended to the live section, schema 1.1: an older app ignores it, an older Pi leaves it 0). Labels, lists, buttons and the other values stay Arimo.

## Sections and sub-page rows (0.5.1)

The eight navigation keys T1..T8 are **sections**. Under the header (SIDECAR, GRID: 58 px from the top; OLED: under the page's title bar)
every section but SEQ and PAT has a **row of sub-pages**, keys 44 px high (OLED 22 dots): the first is where the T key lands, and a section
remembers the last pill used (the T key lands there). Every page that existed before is a pill of one section; nothing was dropped. The page
numbers (`sidecar_ui_show`, the Rytm's UP + T requests in `voice_pi`) are unchanged.

| section | pills | what a pill is |
|---|---|---|
| T1 HOME | Overview, Status, Project, Perform | Overview the new landing page (below), Status the SIDECAR STATUS page, Project the PROJECT page (bundles), Perform the XY pad with its quick keys (below) |
| T2 SEQ | (own row) | the pattern editor; its header row holds the tracks (T9 / T10 ...), the pattern's pages and RYTM |
| T3 PAT | (own row) | the Rytm's pattern, read only; its header row holds the pages 1 2 3 4 ALL and SEQ |
| T4 VOICE | Sound, Insert, Source, Scope | VOICE; INSERT FX (moved here from FX); the machine view with POOL, PRESET, CHOP and SAMPLE EDIT under it; SCOPE |
| T5 FX | Sends, Master, (Morph), XY | Send FX (moved here from Settings); MASTER FX; MORPH (in 0.5.2; hidden in a build without `SIDECAR_MORPH`); the XY pad |
| T6 SONG | Timeline, Setup | the timeline; the MIDI CONFIG how-to (the CFG key became the pill) |
| T7 REC | Record, Takes | the recorder; its takes list (the TAKES key became the pill) |
| T8 SET | Sound, MIDI, Display, Pool, System, About | the settings pages (Send FX moved out; the touch test and calibration stay on Display, power and service on System) |

`show_page` is the one place that knows it: a page maps to its section and pill (`page_sec`, `page_pill`), lights the section's key, shows that
section's row (`SEG_SUB + section`, built by `sub_build`), lights the pill and stores it as the section's memory. SONG's Setup and REC's Takes
are views of their page (`sidecar_song_explain`, `sidecar_rec_takes`), so their pill follows the view (also when a dialog's CANCEL closes it).
POOL and PRESET light VOICE > Source when the machine view opened them, else SET > Pool.

**Room.** The row takes 58 px (OLED 25 dots) of the 428 the pages had, so every page below it lays itself out for `TH->pdy` = `TH->dy` less that
(the pages stretch with it, as they stretch with `dy` at 1024x600), and starts at y 110 (OLED: the page's title bar stays on top and everything
else of the page moves down by 25 dots, `oled_under_row`). The settings pages start at 110, HOME's panels at 110. The lists of the machine view,
POOL and PRESET have rows of 36 px (OLED 18 dots) at every size, so fewer show at 800x480 and UP / DOWN page through them; a REC take list shows six rows at 800x480 (OLED seven of 18 dots).
MASTER FX and MORPH have their numbers in `LU ()` now, which they did not (they failed the `bounds` check at every theme before).

**Themes.** SIDECAR and GRID draw the keys as framed boxes with the small radius, OLED as its dotted keys, the lit one
inverted. FX > Sends follows the board: two FX cards (name, POST / PRE, the BASE and WIDTH dials with their values), the Kill tails card and the
send levels; the hint line of SIDECAR and GRID is the micro status line at the bottom left.

### HOME: Overview and Perform (0.5.1)

**Overview** (page 0, boards T2 and T12; one builder, `home_overview`, for all three themes, OLED in its dots). From top to bottom, three rows
(at 800x480 110 / 152 / 72 px, the extra of a larger screen goes to the first two and, from 1024x600, the status line; SIDECAR and GRID
set the captions and supporting lines in the body face and the values in the title face):
- **Now**: the pattern (A04: OLED the dot face, SIDECAR and GRID the hero face, 44 px at 1280x720), the transport word, the Rytm's project name. A tap opens Status.
- **Perform**, three tiles: **XY pad** (HOLD on / off and the first target of X and of Y; a tap opens Perform), **Send FX 1** and **Send FX 2** (the
  TYPE and POST / PRE; a tap opens FX > Sends). The XY view is built by the engine while Overview shows (`sidecar_xy_wanted`).
- **Voices**: eight tiles in the Rytm's track order (a tap opens VOICE on it, as before): the voice (RS/CP), its machine, the render load as a bar and
  a figure, `poly` for a poly track, and in trouble (the guard muted it: `muted`) the tile in the danger colour.
- **Status line**. At 800x480 one card with three columns: **Recorder** (a dot and REC 0:01:23 / REC armed / off; a tap opens REC), **Health**,
  **Links** (host name; address and the USB-C gadget). From 1024x600 three titled cards (Recorder, Health with a CPU line, Links with Rytm /
  USB-C / Web rows). **Health** names the one thing wrong: "All fine", or "1 thing to look at" / "N things" with the first (underruns, power,
  a core over 85 %, temperature, the Rytm link full, a muted voice, a user machine missing, bad, out of room ...).
- The touch test is no longer a panel here: the first tap anywhere still stores `touchtest=0` and its ring shows under the finger until then; the
  test stays under SET > Display. The old Project card (the slots U1..U5) is HOME > Project's.

**Perform** (page 24 = `PG_PERFORM`, board T5 and T12): the XY pad (sidecar_xy.c's drawing and logic: a second pad object, the same view, the same
`xy=pos / up / hold` lines; X and Y's targets in two corners, the position as `X 75  Y 75`), **Hold** and **Setup** at the right end of the sub-page
row (Setup opens FX > XY on its setup), the **Targets** (X and Y slots, depth) and the **Quick keys**: **Kill tails** (`killfx`) and **REC** (a take
running: `rec=stop`; else the REC page, which asks first when the card may fall behind).

**Left out (no data or no engine support in the UI today):** MORPH tile and fader (not on HOME: the MORPH page has them), LOOP tile, loop keys and Loop mute (the
looper is not in 0.5.2: no page, no console command), LFO tile ("5 of 8 on": the LFO state is not in the UI's status), Song arm (the song view is fed only while SONG
shows, so no armed state), BPM, the kit name ("shared set": only the project name exists), the recorder's file name, bit depth and the card's free
space (REC's view only), a per-voice live level (only the selected voice's stem has one; the bar is the render load), the header's "A04 playing
121.5" on Perform.

**Touch sizes.** Keys are 44 px (OLED 22 dots) and list rows at least 36 px (OLED 18 dots) again where Task 2's 58 px loss had pushed them under,
by laying the page out again (nothing overlaps or is cut): INSERT FX (the title shares the voice keys' row, the knobs are shorter), MASTER FX (the
title goes at 800x480; L/R PAIR at the key height in the gap above the EQ line), XY (HOLD, SETUP, the slot and picker keys), CHOP (the bank keys at
the key height; where four rows of pads would be under it, 800x480, the pads are two rows of eight and the wave gives the height; the slice list's
rows 36 px under a header of the font's height), SAMPLE EDIT on the OLED, the machine view's POOL, the POOL page and PRESET (36 px rows: the list
pages with UP / DOWN), REC's OLED take rows, and SET > Display (every row 44 px, 48 px apart). Not changed: SEQ's tool column and arp rows (a dense
editor of its own, 23 to 26 px keys before this work too), the XY setup's depth sliders (a 12 px track with the slider's wider touch area), the
take rows' DELETE keys (the row's height, 36 to 42 px, with their touch area widened to the row).

## Components

Every widget is a plain `lv_obj` drawn in its `DRAW_MAIN` by the current theme's painter. There are no LVGL themes and no styles per state, so a switch only changes a table pointer and rebuilds the pages.

| widget | OLED | SIDECAR |
|---|---|---|
| `th_key` (kinds: key, accent, danger, FUNC, trig, pad, nav) | black, a 1-dot frame; checked: inverted (light grey, black text); pressed: dithered; disabled: a dotted frame, faint text; accent: a double frame; danger: the teal's tint; an LED: a corner block, or a bar under the text on trig and nav keys | a pill (radius 12-15) with a `#6a64a0` outline; checked: filled lavender, dark text; FUNC / accent tinted, danger coral; trig keys with a bar, pads a tile with an outline; an LED a corner dot; nav: the rail's key, the page's a light pill with dark text, its T number small in the corner |
| `th_seg` (one checked; rows with `"\n"`) | items as keys | items as keys |
| `th_panel` (group / window / bare) | a dotted frame, the title in a notch of the top edge | a card, its title a lavender pill (dark text) straddling the top edge |
| `th_titlebar` (+ LED, alert) | the dithered bar, bold black text left, text right, an LED block | the gradient header with a hairline under it; an alert: coral, dark text |
| `th_meter` (bar, vertical, knob) | LED ladders of dots; the knob is a dot ring (19 dots on a big knob, 13 on a small one, lit to the value) around a dotted cap with a black pointer | rounded bars (lavender, magenta above 70 %, coral above 90 %); the knob a 270° arc (track `#2c2950`, value lavender, coral for a level) with the value inside in the dot face |
| `th_slider` | a dotted track, a solid value, a 5-dot handle | a rounded track, a lavender value, a round light handle |
| `th_fill`, `th_frame`, `th_text`, `th_paint_key`, `th_paint_knob` | painters for the code-drawn areas (the roll, the grid, the strip) | the same |

**Layout.** Each page lays itself out per theme. `sidecar_ui.c` has an `oled_*` and an `sc_*` builder for every page, which create the same objects, so the status update stays one code path. `sidecar_seqed.c` and `sidecar_rytmpat.c` keep a geometry table per theme (`kGeoOled`, `kGeoSidecar`), and their drawing code asks the theme for colours, fonts and line styles.

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
  MULTISAMPLE keyboard as a piano (seven white keys an octave, 7-9 dots each, the black keys narrower over them; SIDECAR
  keeps equal keys), its zones and the envelope; the 6-OP FM algorithm; the DAISYSP modes or string; the scope. The
  p-locks heatmap at the foot.
- **MACHINE (GRANULAR / WAVETABLE):** the picture 346 x 178 (the granular waveform as joined top and bottom outlines),
  or the source list: rows of 18 dots, the bank tabs A-H; keys of 34 dots under it.
- **MACHINE (MULTISAMPLE)** (touch2): the instrument's zones (two rows alternating, as tall as their velocity layers; the
  blue print by layers, white while a key in it sounds; the root a tick) over a piano of its span in whole octaves (the
  keys sounding teal, each C named); SOURCES the same bank list as GRANULAR's, of INST.
- **SEQ tools** (touch2): DRAW CHORD STRUM HUMAN KEY SWING, 40 dots each at 400x240 (SIDECAR: a column of six 42 px keys);
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
  - SIDECAR: the trace in a card in the accent, the meters as pill keys with rounded ladders (lavender, magenta above
    -12 dB, coral above -3 dB, a light held peak, a coral clip lamp; the channel shown: the key selected), the keys 52 px.
- **SIDECAR STATUS** (touch2; `sidecar_status.c`; UP + T10, or a tap on the home page's ENGINE panel; no navigation key):
  two columns of dotted panels, captions in the micro face, values in the body face - VOICES (the eight voices by
  track: the track's name in the blue print, the machine, a dot ladder of the render load, full at 50 % of a core, the
  percent), SEND FX, VERSIONS | CPU (four ladders, then the longest block / pass, underruns, FIFO and temperature),
  LINKS. Errors and limits (MUTED, underruns, a core over 80 %, a pass over 2 ms, a block over 8 budgets, the network
  down, under 1 GB free, an update over 16 ms) in the teal's tint (SIDECAR: coral). SIDECAR: the same panels as cards.
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
    start. The playing row: a play triangle (paused: two bars) in the tint, a frame (OLED) or a tinted row (SIDECAR),
    the length column turned into `elapsed / length` over the size and files columns, and a thin progress bar under
    the name (dotted track, the played part solid; SIDECAR a rounded track, the played part lavender). Idle rows carry a
    faint triangle. Playback goes on when the page is left and shows again on return; it ends at the take's end,
    when a recording starts or arms, and when its take is deleted. A tap while a take records does nothing. Only the
    bar's strip is redrawn as the position moves (at most 10 a second); the label changes once a second.
  - SIDECAR: the same content as cards: coral segment digits while recording (a light coral armed), REC as a pill key
    with a coral record dot, the channels as pill keys with a coral dot when recorded and the meter in a rounded dark
    window (lavender, magenta above -12 dB, coral above -3 dB, a light held peak, a coral clip block).
  - Cost: the meters are the only thing redrawn 25 times a second, each cell's strip alone when only it moved
    (`meter_dyn`; the draw skips the other cells), the clock once a second. Mac (`./shot` prints it): a frame with all 23
    meters moving 24 us OLED / 35 us CLASSIC (Pi 4 est. 0.8 / 1.2 ms; SIDECAR: "Cost"), 1024x600 34 / 68 us (1.1 / 2.3 ms); the full page
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
| T5 | the touchscreen's FX: Send FX, as the sidebar's FX key (INSERT FX before 0.5.1.1) | |
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
and SIDECAR; SIDECAR writes the page's names in capitals in its header row at 800x480). A page
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
POLY CONFIG, ARP, SAMPLE, AUTO SAMPLE, SONG, REC, SCOPE, LFO, MORPH (0.5.2), SAMPLE EDIT, PROJECT, STATUS, SYSTEM, USER SLOTS, MIDI (0.5.0, docs/pi-engine.md "Keyboards and MIDI settings"), USB-C, WEB UI and KEYS (ALL COMBOS) (`test_ui` holds
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

Measured on the Mac by `./shot` (it prints them). The tables after the first name CLASSIC, the theme SIDECAR replaced; the
first one has SIDECAR's figures against CLASSIC's last run (same Mac, the same day).

SIDECAR against CLASSIC (Mac us, Pi 4 est. ms at x34.1; one run each, a few % of noise):

| redraw | CLASSIC 800x480 | SIDECAR 800x480 | CLASSIC 1024x600 | SIDECAR 1024x600 | CLASSIC 1280x720 | SIDECAR 1280x720 |
|---|---|---|---|---|---|---|
| home, full | 459 (15.6) | 469 (16.0) | 606 (20.7) | 608 (20.7) | 804 (27.4) | 880 (30.0) |
| SEQ, full | 531 (18.1) | 553 (18.9) | 700 (23.9) | 764 (26.0) | 905 (30.8) | 1056 (36.0) |
| SEQ grid | 142 (4.9) | 143 (4.9) | 216 (7.4) | 197 (6.7) | 281 (9.6) | 253 (8.6) |
| SEQ playing frame | 37 (1.28) | 36 (1.21) | 41 (1.41) | 39 (1.31) | 40 (1.35) | 39 (1.32) |
| RYTM, full | 489 (16.7) | 513 (17.5) | 630 (21.5) | 696 (23.7) | 856 (29.2) | 958 (32.7) |
| REC, full | 610 (20.8) | 637 (21.7) | 785 (26.8) | 848 (28.9) | 1026 (35.0) | 1156 (39.4) |
| REC, all 23 meters | 46 (1.6) | 46 (1.6) | 94 (3.2) | 75 (2.5) | 126 (4.3) | 109 (3.7) |
| SCOPE, full | 496 (16.9) | 501 (17.1) | 660 (22.5) | 684 (23.3) | 914 (31.2) | 952 (32.5) |
| STATUS, full | 626 (21.3) | 633 (21.6) | 772 (26.3) | 795 (27.1) | 1031 (35.2) | 1114 (38.0) |
| WAVETABLE frame | 304 (10.4) | 285 (9.7) | 473 (16.1) | 402 (13.7) | 684 (23.3) | 582 (19.8) |

The full redraws (page changes) cost 0-17 % more than CLASSIC's: the larger corner radius, the arcs and the header's
three gradient fills. What repeats (the playing frame, the meters, the machine view) costs the same or less. No shadow,
blur or opacity layer is drawn.

1280x720 against 1024x600 (a Mac run of each, Mac us; about 1.35 x):

| redraw | OLED 1024x600 | OLED 1280x720 | CLASSIC 1024x600 | CLASSIC 1280x720 |
|---|---|---|---|---|
| home, full | 430 | 574 | 610 | 823 |
| SEQ, full | 450 | 596 | 857 | 1071 |
| SCOPE, full | 443 | 591 | 683 | 985 |
| SCOPE, a trace + the meters (15 a second) | 314 | 411 | 493 | 508 |
| STATUS, full | 737 | 794 | 798 | 1244 |
| REC, full | 695 | 746 | 958 | 1303 |
| REC, all 23 meters | 52 | 72 | 98 | 129 |

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

**Why OLED is about 2x cheaper:** LVGL renders 96,000 dots instead of 384,000 pixels (42,560 on the 3x3 grid, which made it about 3x cheaper), and the expansion is 4 stores a dot. Every full redraw stays under 15 ms in both themes at 800x480; at 1024x600 OLED stays under 9 ms, CLASSIC's full redraws reach about 17 ms (page changes only; SIDECAR: the table above).

**Theme switch.** It runs on core 0's loop only. Rendering (cores 1-2), control (core 3) and the IRQ-driven audio paths are untouched, so the audio cannot glitch.
- **Rebuild:** the pages are rebuilt with invalidation off, about 0.8-1.4 ms on the Mac. That is ~10 ms on the Pi 4 if CPU-bound (inferred, x8) and ~48 ms at the draw factor (x34, the upper bound).
- **Redraw:** in bands, one band per core 0 Update (4 for OLED, 6 for SIDECAR): at most 0.11 ms on the Mac, ~4 ms on the Pi 4. On screen it is a wipe from the top.
- **Delay:** core 0's loop (MIDI to the Rytm, the console, the TCP link) waits once, for the rebuild.
- **To check on hardware:** the `longest update` in the engine's 10 s line, during a switch.

**Pi 5 on scaled HDMI:** the expanded 800x480 goes through the existing scaler (`CSidecarDisplay::Put`), so a dot becomes a cell of 4-6 frame-buffer pixels with its grid lines scaled along. That costs the same as SIDECAR's scaled full redraw.

## Adding a theme

1. **Table.** Add a `sidecar_theme_t` to `sidecar_theme.c` (`THEME_*` id, name, dot size, UI size, colour tokens, fonts, radius, gap, touch) and return it from `sidecar_theme_get`.
2. **Painters.** (SIDECAR and GRID: `sq_key`, the `flat ()` branches, `grid_apply`.) Give `th_paint_key`, `panel_draw`, `title_draw`, `meter_draw`, `slider_draw` and `th_paint_knob` a branch for it, or reuse one of the two looks with other tokens.
3. **Layout.**
   - The layouts grow from `TH->dx, dy` ("Sizes"); a new theme with its own grid needs `kGeo*` tables in
     `sidecar_seqed.c` and `sidecar_rytmpat.c` (their `geo_size` grows them for 1024x600), and builders in `sidecar_ui.c`.
   - A theme with the same pixel size reuses the OLED or SIDECAR layout (`oled ()` asks for the id).
   - A new screen size: `sidecar_theme_screen` and `sidecar_theme_set` (the table's `w, h, dx, dy`), and the checks in
     `shot.c` (`DX`, `DY`).
4. **Expansion.** A dot size other than 1 or 2 needs `sidecar_dot_expand` generalised; today it is written for 2.
5. **A variant of a theme** (SIDECAR's high contrast) is no new theme: `contrast_apply` in `sidecar_theme.c` overrides the table's colours
   and the painters' palette (`UV_*`, a runtime palette) after `sidecar_theme_set` copies the base; a painter that needs more than a
   colour reads a field (`TH->hc`, `TH->edge_w`), a page never a literal. Wiring as for a theme: a `display.txt` key in the kernel's `Known[]`, a console command, a status field, a Display row.
6. **Wiring.** Add the name to the Display page's `kStyleMap`, to `uistyle=` in `pi/engine/kernel.cpp` (command, `display.txt`), and to the host render's `theme_run`.

## Files

| file | what |
|---|---|
| `pi/ui/sidecar_theme.{h,c}` | the theme tables, the dot fonts as LVGL fonts, widgets, painters |
| `pi/ui/sidecar_rec.{h,c}` | the REC page (the Overbridge recorder), filled by `pi/engine/rec_pi.cpp` (`UiFill`) |
| `pi/ui/sidecar_scope.{h,c}` | SCOPE, filled by `pi/engine/scope_pi.cpp` (the tap) and `rec_pi.cpp` (`ScopePeaks`: the recorder's meters) |
| `pi/ui/sidecar_status.{h,c}` | SIDECAR STATUS, filled by `pi/engine/kernel.cpp` (the stats, the links) and `status_pi.cpp` (machines, the mod level) |
| `pi/ui/sidecar_dot.{h,c}` | the dot expansion, the pattern colours, the LED palette, plain dot-font drawing (no LVGL) |
| `pi/ui/dotfont.py`, `sidecar_dotfont.{h,c}` | the pixel fonts (generated) |
| `pi/ui/fonts/gen-arimo.sh`, `fonts/sidecar_arb*.c`, `sidecar_ar*.c` | SIDECAR's and GRID's text faces: Arimo Bold 11-44 px, and the Regular 13 / 18 / 28 px tables with the 11 Font Awesome symbols (generated; SIL OFL 1.1) |
| `pi/engine/ui_style.h` | the `uistyle=` words and display.txt's line (`tests/test_ui_style.cpp`) |
| `pi/ui/sidecar_logo.h` | the "Docked" logo's dots (the header, the SIDECAR boot screen) |
| `pi/ui/sidecar_ui.{h,c}` | the pages, both layouts, `sidecar_ui_style`, `sidecar_ui_dot` / `origin` / `to_ui` / `bright` |
| `pi/ui/sidecar_seqed.c`, `sidecar_rytmpat.c` | the pattern pages with a geometry table per theme |
| `pi/ui/sidecar_splash.{h,c}` | the boot screen in both looks (`sidecar_splash_style`) |
| `pi/ui/sidecar_display.cpp` | the Pi's flush (expansion), pointer and second finger in UI pixels; the UI's size (`ChooseUi`, `res=`, the EDID) and its place (centred) |
| `pi/engine/kernel.cpp` | `uistyle=`, `display.txt`, the status field, the boot screen's look from the cmdline |
| `pi/ui/host/shot.c` | the host render, every theme: every page, the controls' commands, the SEQ gestures, the costs, the animation |

## STAGE look and the alert grammar (UX review L1, L2)

- **Display > LOOK: STUDIO / STAGE** (OLED; `uilook=studio|stage`, stored in `/.sidecar/display.txt`). STUDIO is the dot
  look above and stays the default (the review asks for the setting, not a new default). STAGE draws every dot as its
  whole 2x2 cell (no grid lines) and lifts every grey a step (`v -> 0x30 + v x 0xD0 / 0xFF`, `sidecar_dot_look`), for a
  stage and an angle. The bold face of the review is not in STAGE yet: every layout is measured for the regular face and
  the text-fit check would fail (deferred).
- **Alerts** (OLED): the attention colour (`TH->bad`, the FUNC teal's tint) is never plain text: `th_label_color`
  inverts the label (black on teal), so teal means "look here". HOME's U1..U5 errors blink for 3 s, then stay inverted,
  with the reason spelled out; loading is dim with moving dots. Playing is white (REC's playing take), the selection
  inverted grey, single notes and FX 1 the panel's blue. SIDECAR uses coral for the same tokens (an inverted label: dark on coral).
