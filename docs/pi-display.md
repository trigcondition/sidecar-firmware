# Pi display

The Pi engine draws a Sidecar screen on an 800x480 or a 1024x600 display (see "Screen sizes"): HDMI with USB touch, or a
DSI display (the official 7" display and compatibles, see "Supported screens"): on the Pi 4 the firmware drives it, on the Pi 5 Circle's
`addon/rp1dsi` ("DSI on the Pi 5"). The UI is built with LVGL 9.4.0 (MIT),
the version Circle 51.1.1's `addon/lvgl` submodule pins. The code is in `pi/ui`:
- `sidecar_ui.c`: the screen. It uses LVGL only, so the same file also builds on the Mac.
- `sidecar_display.cpp`: the Circle side. It is based on Circle's `addon/lvgl/lvgl.cpp` (GPLv3).

## The screen

The display has two themes, OLED (the default: the screen drawn as an Elektron-style dot-matrix OLED, a navigation
column HOME / SEQ / PAT / SET on the right) and CLASSIC (the layout below, drawn as the Rytm MKII's hardware). Settings >
Display > UI style switches them live; docs/UI-THEMES.md has the design system. The list below is CLASSIC's layout; OLED
shows the same content on 400x240 dots (512x300 on a 1024x600 screen).

- Title bar "Sidecar | Expander", with "Rytm connected" (green) or "Rytm not connected" (red) on the right.
- TRACKS: voices 1-8 with their current machine (PLAITS, FM, JUNO, DSPS, DAISY, MD, USER; MD in DIGIDRUM's place since 2026-10-29).
- STATUS:
  - Overbridge blocks since boot.
  - Underruns (red once nonzero).
  - Load bars for cores 1-3. Core 0 is not measured and shows "-".
  - The status updates once a second, from the stats line.
- TOUCH TEST:
  - A TAP button with a counter.
  - A 0-127 slider.
  - The touch readout `raw x,y -> screen x,y`.
  - A green ring under the finger.
  - The link (usb, tcp or self-test) in the panel's title row.
  - **Touch works - hide** (48 px high) hides the panel and stores `touchtest=0`, so it stays hidden after a reboot.
  - Hidden, a SIDECAR panel takes its place: network (IP), web page address, link and USB-C mode, and the recorder's state. No Wi-Fi name or key.
  - **Settings > About > Touch test (home): Hide / Show** (or the console command `touchtest=0|1`) switches it.

The title bar's **Settings** button (48 px high, top right) opens the settings pages and becomes **Home**; a tab strip
(Sound, Send FX, Display, About) switches between them, and Settings reopens the page last used. Every control is at least 48 px
high and sends a console command through `CConsole::Command`, the web page's path (`UiCommand` in kernel.cpp logs
`display: command "..."`). Commands only queue (core 3 applies them), so a tap never waits for the engine. About 150 ms
after a command, and every second, `CKernel::PushUi` reads the state back the way `/status` does, so a change from the web
page, the serial console or the display shows on all of them.
- **Sound**:
  - MIDI notes Off/On, Output Main/Tracks, Computer audio Off/On (disabled without the USB-C gadget), FIFO target
    with - and + (steps of 8 frames).
  - Voice 1-8, then that voice's Machine (PLAITS JUNO FM DAISY DSPS GRAIN MD; MD replaced DD, DIGIDRUM, 2026-10-29; none checked for a user machine) and
    Drone (Off, This voice, All). DSPS and GRAIN are one machine in the status, so the one picked stays checked.
- **Send FX**:
  - FX 1 / FX 2 TYPE and the 12 tracks' send levels, read only: the Rytm's kit owns them, and a console override
    (`sendlvl=`) lasts only until the next 0x20 message.
  - **Kill tails** sends `killfx`: the CC 120 request (`core::fx::all_sound_off`, both FX fade out in about 5 ms, clear and
    return). Needs a build with the cc120-kill change.
- **Display**: UI style (OLED / CLASSIC, `uistyle=`), screensaver, the home page's touch test, brightness.
- **About**: the web page address, IP, whether the web server runs (cmdline `web=`, not switchable at run time), link,
  USB-C gadget mode, the engine's build time, Circle and LVGL versions.

**REC** (the Overbridge recorder, docs/pi-engine.md "Recording"; docs/UI-THEMES.md "REC"): UP + T7 on the Rytm, or a
tap on the home page's REC status (OLED: the title bar's right half; CLASSIC: the Status panel's REC time, the SIDECAR
panel's Recorder row). REC / STOP, ARM, the take's time, the take, 48 kHz and the bit depth, the card's free space and
the time it holds; the 23 channels with a meter each (tap: on / off, idle only); TAKES: the newest takes with Delete
(a confirmation first). No navigation key: HOME (OLED) or Home (CLASSIC) goes back.

**SCOPE** (touch2; `pi/ui/sidecar_scope.c`, filled by `pi/engine/scope_pi.cpp`): UP + T9 on the Rytm, or a tap on the
VOICE page's scope. An oscilloscope of one Overbridge channel - TRACK (the Rytm's selected track's stem, following it),
MAIN, a stem, IN L / R (the FX return while it is on) - over the eleven channels' level meters (MAIN as L / R; a tap on
a meter shows that channel, TRACK follows again). The timebase (43 ms .. 2.7 s a screen: triggered on a rising crossing
up to 85 ms, a sweep from 171 ms), GAIN (AUTO without hunting, or x1 .. x64), FREEZE (the trace held, the meters go on).
The trace is the Overbridge input tap the Rytm's own SCOPE page reads (core/ui/extra.h, the same timebases at 256
columns), the meters are the recorder's (recorder.h `meter_peaks`, the REC page's: on while REC or SCOPE shows). ~15
traces a second while shown; the tap is off again when the page is left.

**SIDECAR STATUS** (touch2; `pi/ui/sidecar_status.c`, filled by kernel.cpp and `pi/engine/status_pi.cpp`): UP + T10 on the
Rytm, or a tap on the home page's ENGINE panel (OLED) / Status panel below the REC time (CLASSIC). Read only:
- VOICES: each voice named by its track(s), in the Rytm's track order, with its machine and render load (a share of one
  render core, from `cores.cpp`'s per-voice ticks; a bar full at 50 %), MUTED when the voice guard muted it.
- SEND FX: FX 1 / FX 2 TYPE and PRE / POST.
- CPU: cores 1-3 (core 0 not measured), the longest block against the 250 us budget, the longest control pass, the
  underruns, the FIFO's minimum against its target, the SoC's temperature (right of the FIFO row; red with HOT while the
  temperature warning is on: from 75 C, off below 70 C; the same warning shows as a banner over the title bar).
- LINKS: the Rytm, the Overbridge stream, the USB-C gadget and computer audio, the network (Wi-Fi / Ethernet, up / down,
  the address; never the Wi-Fi's name or key), the web page, the card's free space (the recorder's reading, every
  10 s), the display's longest update (the current 10 s window of the log line's figure).
- VERSIONS: the Sidecar build, Circle, LVGL, and the rytm-mods level the Rytm's messages show (`RUI1036+ (SEEN)`:
  inferred from which of the mods' messages came - 35 hello, 33, 29 / 2A, 49; no build number is sent).
Twice a second while shown; only changed labels and bars redraw.

With the display up, UP + T7 / T9 / T10 open REC / SCOPE / STATUS on the touchscreen and the Rytm's screen stays as it is
(`voice_pi::OwnPages`, `core::ui::remote_display_owns`); without a display (display=0) they open the Rytm's own pages,
and UP's map leaves out the keys that would open nothing at all (rui1052; docs/UI-THEMES.md "The device's shortcuts").

**MULTISAMPLE** in the machine view (touch2): a tap on a MULTI voice on the home page, or on the VOICE page's MULTISAMPLE
panel (`TAP: BANKS`), opens the machine view for it: the instrument's key zones over a piano of its span (the keys
sounding lit), and its POOL (below).

**POOL** (the pools design, docs/superpowers/specs/2026-10-22-pools-tonverk-design.md; core/lib/pools.h): the machine
view's POOL key shows the selected voice's own list - GRANULAR the SAMPLES, WAVETABLE the WAVETABLES, MULTISAMPLE the
INSTRUMENTS - as a tab row A..H (all banks alike; a change sends `pool=smp|wt|inst:shown:B`, takes go to the bank last
shown) and the bank's rows `000 OFF`, `001 name`, `---`. Row states: LOADING, RAM, `!name` MISSING (the theme's alert),
`~name` CHANGED, and an in-use marker (filled: voices on it now, `x3` right; an outline: seen playing in this project).
The title shows the list and the pool RAM (`RAM 412 MB 40%`, `RAM -` until the shared cache counts).
- a tap on a row with content: the voice plays it (`gr=pick:V:C005`, `wt=pick:V:C005`, `msamp=pick:V:C005`: the Rytm's P1);
  on an empty row: FILES aimed at it - the list machine's browser (`browse=gr|wt|ms:open:I`, `up`, `root`; the root shows
  `@INPUTS/` and `@BUILT-IN/` where the list takes them), a file or virtual item goes into the row
  (`pool=smp|wt|inst:assign:C005:I`, then the next empty row), FILL B the folder's files into the bank's empty rows
  (`pool=smp|wt:fill:B:/folder`; not for instruments). A file already in the list shows its row (`IN C001`); a tap goes there.
- a long press on a filled row: REPLACE (FILES aimed at it), UNLOAD (`pool=K:unload:C005`), ACCEPT a changed file
  (`pool=K:accept:C005`). A row in use asks first (`ROW C012 USED BY 3 SOUNDS.` / `ROW C012 SEEN PLAYING IN THIS
  PROJECT.`), YES sends with `:yes`; while the Rytm plays a row in use is refused. The message row then offers UNDO
  (`pool=K:undo`).
- SELECT UNUSED: the rows not seen playing, selected (a tap deselects one), the warning `NOT SEEN PLAYING - A RYTM LOCK
  MAY STILL USE IT`; UNLOAD N asks, then sends `pool=K:unload:A010:yes,A011:yes,..` (19 rows a command, 8 commands a tap).
- the POOL page (page 15: Settings > Pool, or the machine view's ALL POOLS): the same with the three lists' tabs
  (SAMPLES / TABLES / INSTR, their counts) on top; a tap on a filled row opens its menu; DONE goes back.

`make -C pi/ui/host` renders every page on the Mac in both themes (no window, no SDL): `pi/ui/host/shots/oled-*.png` and
`classic-*.png` (home, sound, fx, display, about, hidden, home-info, power, seq-*, rytm-*), with made-up status values and a
touch at 392,145. It taps the controls through LVGL's pointer input and fails if one does not send its command.
`make -C pi/ui/host shot.png SIZE=1024x600` does the same on a 1024x600 screen (`shots-1024x600/`, the same checks with
the taps moved as the layout moves), `make -C pi/ui/host shots-all` both sizes, `make -C pi/ui/host splash-wide` the boot
screen at 1024x600 (`obj/splash-1024x600.png`).

Cost: the display's update is core 0's loop work. At rest, the longest update in the 10 s log line is about 300 us (the
once-a-second state refresh with the settings widgets); the first full draw after boot takes about 7.4 ms. A page change
redraws the area it uncovers in 48-line bands, about the cost of that first draw.

## Supported screens

| Screen | Status |
|---|---|
| Waveshare 5" / 7" HDMI LCD with USB touch (800x480) | Works (measured on the 5" (H)). |
| GeeekPi EP-0194 7" IPS HDMI (1024x600, USB capacitive touch, HDMI in) | Display measured (`display: 1024x600`). Touch (USB 0416:c169 "TSTP MTouch"): stock Circle took it for a keyboard (`input none`). Its boot-keyboard interface's 84-byte descriptor (measured) is an absolute mouse (ID 2, button 1 = finger, X/Y 0..32767): `circle-touch-pointer.patch` binds it as a single-touch screen ("USB touch screens" below), to be confirmed on the panel. config.txt as in "Screen sizes". |
| Official Raspberry Pi 7" Touch Display v1 (DSI, 800x480) | Expected on both boards, not yet measured here. Pi 4: firmware frame buffer + `CRPiTouchScreen` (firmware touch buffer). Pi 5: Circle's `addon/rp1dsi` (display and touch, no firmware). |
| OSOYOO 7" IPS DSI V4.0 (800x480, 15-pin DSI) | Expected, to be confirmed: sold as plug-and-play on Raspberry Pi OS, which normally means it emulates the official display (ATTiny regulator at I2C 0x45, TC358762 bridge, FT5x06 touch at 0x38). The boot log names what answers ("DSI on the Pi 5"). |
| Raspberry Pi Touch Display 2 | Pi 4: not supported (needs a KMS DSI panel driver and a Goodix touch driver, which Circle does not have on the Pi 4). Pi 5: `addon/rp1dsi` drives it (*inferred*, untested); it is 720x1280 portrait, which the 800x480 layout is not made for. |

### USB touch screens

`pi/circle/circle-touch.patch` (applied by `pi/engine/build.sh`, after `circle-hid.patch`) with the parser
`pi/circle/hid_digitizer.h` (copied into the Circle tree as `include/circle/usb/hiddigitizer.h`; unit test
`pi/engine/tests/test_hid_digitizer.cpp`):

- Some panels' touch controllers declare their only HID interface a boot keyboard (`int3-1-1`) and send multi-touch
  digitizer reports on it, with report IDs (GeeekPi 7", 0416:c169). Stock Circle chose its keyboard driver for them.
  Now a boot keyboard or mouse interface whose report descriptor has a Digitizer Touch Screen (or Pen) application gets
  the touch driver (log: `Touch screen on a boot HID interface: using the touch driver`). 0416:c169 is also taken by
  vendor/product, as a backup.
- The touch driver sets report protocol (not boot), and the Device Configuration's Input Mode feature to 2
  (touch screen) if the descriptor has one. It parses Usage lists, report IDs, Tip Switch / In Range, Contact
  Identifier, X / Y with their logical ranges, Contact Count (hybrid mode: one frame over several reports), up to 10
  contacts (the first two used: the pointer and the SEQ page's pinch).
- Absolute pointers (`pi/circle/circle-touch-pointer.patch`, after `circle-touch.patch`): a Generic Desktop Mouse or
  Pointer application whose X/Y are absolute is a single-touch source, button 1 the finger (the GeeekPi 7"'s
  descriptor, measured: Mouse, report ID 2, 3 buttons, X/Y 16-bit 0..32767, a relative wheel and AC Pan; 8-byte
  reports). Relative mice keep the mouse driver. The factory decides by the parse alone: a descriptor it cannot use
  keeps the keyboard/mouse driver, as a failed `Configure()` makes the hub drop the device and find it again on every
  scan (the removed / found loop seen with the first kernel).
- A panel with both an absolute-pointer and a digitizer interface: the touch screen with the most contacts is bound.
- Single touch: `CSidecarDisplay::GetTouchContacts()` / `sidecar_touch_contacts()` (C) is 1 for such a panel (or a
  mouse), 2+ for a multi-touch digitizer or the DSI touch, 0 with no input. The UI can offer double-tap or a zoom slider
  instead of pinch when it is 1 (not used yet). The `touch screen bound` line ends with `single touch` or `multi-touch`.
- Multi-touch probe (single-touch devices, read only, NOTICE): `utouch: probe: N configuration(s)`, each configuration
  in hex with its `probe: interface I alt A: class C-S-P` lines, then `probe: feature report 1..5` (GET_REPORT reads; a
  stall is `none`). Nothing is written to the device. The sibling 0416:c168 is reported with two HID interfaces
  (boot keyboard with an 84-byte descriptor, boot mouse with a 223-byte one and an OUT endpoint; FreeBSD bug 264379),
  so a second interface may carry a multi-touch digitizer.
- Several reports in one transfer (`pi/circle/circle-touch-multi.patch`): the GeeekPi sends 64-byte transfers with
  four 8-byte reports back to back, the release often the last one (measured). Every report of our ID and size is
  used, in order, up to a short remainder or another byte. Before this only the first was, so releases were lost and
  the previous button stayed pressed. A device that repeats its report while held (seen: an unchanged report while
  down) and then sends none for 150 ms has lost its release: the contacts are released (`utouch: no report for 150
  ms while down: released (N times)`). A long press keeps its repeats coming, so it is not cut.
- A tap whose press and release both fall between two LVGL reads is shown pressed for one read, then released, so
  it still clicks.
- The log has each press and release with its raw and screen values: `touch: down raw X,Y -> x,y`, `touch: up ...`.
- The raw range is the descriptor's X/Y logical maximum, so the touch maps to the screen without `touchscreen=`.
- The boot log, once per plug-in: `utouch: ven416-c169 int3-1-1 report descriptor (N bytes):` and its hex lines,
  `utouch: touch screen ven416-c169: report ID R, N bytes, C contact(s) a report, X 0..MAX Y 0..MAX, ...`, possibly
  `utouch: input mode set to touch screen`, then `utouch: input report 1 (N bytes):` ... 6 (the first reports, hex),
  then `touch screen bound (touch1), raw range 0..MAX x MAX` and `touch: USB`. A descriptor that names the digitizer
  page but has nothing usable is dumped as `ufactory: digitizer page, no usable touch collection: ...`.

### Touch calibration

**Settings > Display > Touch cal: CALIBRATE** (CLASSIC: **Touch: Calibrate**) shows a cross at 10 % in from each corner
in turn (top left, top right, bottom right, bottom left). Touch each one precisely; the raw values at each release are
taken (`pi/ui/touch_cal.c`, unit test `pi/engine/tests/test_touch_cal.cpp`):

- Which raw axis runs along the screen's x (swapped axes), and per axis the raw values at the screen's edges
  (an inverted axis: its edge values reversed).
- Stored as `touchcal=X0,X1,Y0,Y1,SWAP` in `/.rytm/display.txt` (also a console command; `touchcal=off` or **RESET**
  goes back to the default). Logged as `display: touchcal=...` and `touch: calibration x X0..X1, y Y0..Y1`.
- It wins over cmdline `touchscreen=` and the descriptor's range. It maps straight to the UI's pixels (with a UI
  centred on a bigger frame buffer too).
- Four points that do not make a screen (a cross missed, all at one place) are refused: the crosses start again.
  **CANCEL** in the middle ends it unchanged.
- The page shows CALIBRATED or DEFAULT.

### Screen sizes

The UI has two sizes, each with its own layout in both themes (docs/UI-THEMES.md "Sizes"): **800x480** (the Waveshare
LCDs, DSI displays; unchanged) and **1024x600** (OLED 512x300 dots; a 7" 1024x600 panel such as the GeeekPi EP-0194).
The size follows the **frame buffer**, which the firmware sets from config.txt before Circle starts:

- Frame buffer 800x480 or 1024x600: the UI at that size, 1:1.
- Any other size (Pi 4): the 800x480 UI drawn 1:1 in the middle, black borders, no scaling (no blur). The log names the
  frame buffer's size and the EDID's preferred mode, so the config.txt line to fix it is clear.
- Pi 5 on HDMI: the firmware sets the mode itself (1920x1080 measured on the Waveshare, `hdmi_cvt` ignored; docs/pi5.md
  "HDMI display"), so the UI is scaled to it as before. It is 1024x600 (scaled x1.875 x1.8) when the display's EDID
  prefers 1024x600, else 800x480. A frame buffer of exactly 1024x600 or 800x480 is not scaled. [inferred: the Pi 5's
  answer to the EDID property is not yet seen on hardware.]
- **Override:** `res=800x480`, `res=1024x600` or `res=auto` in `/.rytm/display.txt` (a line of its own, added by hand; kept
  when the engine rewrites the file) or in `cmdline.txt` (wins). Read once at boot, before LVGL starts: a change needs a
  reboot. A size that does not fit the frame buffer is refused (log line) and the choice is automatic. Example: a
  1024x600 frame buffer with `res=800x480` shows the 800x480 layout centred (bigger margins, the same pixel sizes).
- The boot screen (before the card is read) is drawn on the whole frame buffer at its own size.

**The GeeekPi EP-0194 (1024x600, expected; to be confirmed on the panel):** config.txt's HDMI block becomes
```
[pi4]
hdmi_force_hotplug=1
hdmi_group=2
hdmi_mode=87
hdmi_cvt=1024 600 60 6 0 0 0
disable_overscan=1
```
(`pi/circle/make-sd-dual.sh` writes it with `HDMI_RES=1024x600`; the default stays 800x480.) Nothing in cmdline.txt or
display.txt is needed: the frame buffer is then 1024x600 and the UI follows. On the Pi 5 only `hdmi_force_hotplug=1`
matters (it is in the `[pi5]` block already); if the EDID check does not pick 1024x600 there, put `res=1024x600` in
cmdline.txt. Touch: the panel's USB touch to a USB-A port; the touch range is mapped to the whole screen (below).

**Can the boot pick it up by itself?** Partly:
- The engine does: whatever size the frame buffer is, 800x480 and 1024x600 get their own layout, anything else the
  centred 800x480 UI, and the Pi 5 uses the EDID.
- The Pi 4's firmware decides the frame buffer before Circle runs. With the `hdmi_group/hdmi_mode/hdmi_cvt` lines it is
  always that mode. Without them (only `hdmi_force_hotplug=1`) the firmware uses the display's EDID preferred mode, so a
  panel that reports 1024x600 gets 1024x600 with no config.txt edit at all; the Waveshare LCDs need the lines (their
  EDID is not relied on), so the card keeps them. [inferred]
- One card for both panels: the Pi 4's firmware has an `[EDID=<name>]` config.txt filter (the monitor name the EDID
  reports). After one boot on the GeeekPi the log shows `display: EDID: <name> preferred 1024x600 (60 Hz)`; a section
  `[EDID=<name>]` with `hdmi_cvt=1024 600 60 6 0 0 0` after the `[pi4]` block then switches the mode only for that
  panel. [inferred: the filter is the firmware's documented one, not tried here.]

The log (Pi 4, GeeekPi, expected):

    display: firmware 1 display(s): 0=HDMI0 -> frame buffer on 0 1024x600
    display: EDID: <name> preferred 1024x600 (60 Hz), the frame buffer's size
    display: UI 1024x600 on the frame buffer's 1024x600 (res=800x480|1024x600 in /.rytm/display.txt or cmdline.txt)
    display: 1024x600, LVGL 9.4
    engine: display: 1024x600, input touch (USB), longest update N us      (every 10 s)

Another frame buffer size: `display: UI 800x480 centred on the frame buffer's 1280x720 ...`; with an EDID preferring
1024x600 the EDID line adds `: for 1024x600 set config.txt hdmi_cvt=1024 600 60 6 0 0 0`.

### How the display is picked

On the Pi 5 see "DSI on the Pi 5" below. On the Pi 4:

- Circle uses the firmware's (legacy) frame buffer. On the Pi 4 the firmware drives a DSI display itself only if it
  detects the official 7" display (or a compatible) at boot. At boot the engine asks the firmware which displays it has
  and logs it, for example: `display: firmware 1 display(s): 0=HDMI0 -> frame buffer on 0 800x480`.
- With a DSI display and HDMI both connected, the engine draws on the DSI display (`0=HDMI0 1=DSI -> frame buffer on 1`).
  `dsi=0` in cmdline.txt keeps the frame buffer on display 0 (as before) and turns the DSI touch off.
- config.txt (the dual card's, `pi/circle/make-sd-dual.sh`) needs no change: `[pi4] max_framebuffers=2` gives each
  display its own frame buffer; the `[pi4]` `hdmi_force_hotplug=1` and `hdmi_cvt 800 480` lines are for the Waveshare
  (with a DSI display the firmware then reports both, and the engine picks DSI).
  - Do not add `ignore_lcd=1` (the firmware then ignores the DSI display) or `disable_touchscreen=1` (no touch).
  - Do not add `dtoverlay=vc4-kms-dsi-7inch`, `vc4-kms-v3d` or `display_auto_detect=1`: on Raspberry Pi OS the
    overlay hands the display to the Linux KMS driver; Circle has no such driver, so the screen would stay dark.
  - Optional (under `[pi4]`): `lcd_rotate=2` turns the DSI display (and the firmware's touch) by 180 degrees, for an upside-down case
    mount. `display_default_lcd=0` makes HDMI the firmware's default display.

### DSI touch

- If the firmware reports a DSI display, the engine creates Circle's `CRPiTouchScreen`. The firmware reads the
  display's touch controller (FT5406) over the DSI connector's I2C and keeps the points in a buffer; the engine polls
  it from core 0's main loop, every 10 ms (`Update`). The coordinates are the panel's 0..799 / 0..479: no calibration.
- A USB touch screen has priority: while one is bound, the DSI touch's events are dropped.
- If x or y is mirrored: `dsitouch_inv=1` (x), `2` (y) or `3` (both) in cmdline.txt. Swapped axes are not handled.
- The log says which touch is active: `touch: USB`, `touch: DSI (firmware)` or `touch: none`. It also logs
  `touch: DSI (firmware) touch buffer set up`, `touch: DSI (firmware) not available` (the firmware refused the buffer;
  Circle's `ft5406` module logs why), or `touch: no DSI display reported, no firmware touch`.

### DSI on the Pi 5

The Pi 5's firmware does not drive a DSI display: the engine drives it itself with Circle's `addon/rp1dsi` (Circle
51.1.1, GPL-3.0, unchanged; `pi/engine/build.sh` builds `librp1dsi.a` in the Pi 5 tree). All *inferred* from Circle's
sources; not yet run on hardware.

- At boot (the boot screen), unless `dsi=0`: the engine reads the display regulator (I2C address 0x45, register 1) on
  each DSI port, **CAM/DISP 0 first, then CAM/DISP 1** (`dsi_port=0` or `1` in cmdline.txt: only that one). DISP0's I2C
  is RP1's I2C6 (GPIO38/39), DISP1's I2C4 (GPIO40/41), rp1dsi's mapping; neither is on the 40-pin header.
- The regulator's id picks the driver (rp1dsi's own choice): low nibble 1/4 = Touch Display 2 7", 8/9 = Touch Display 2
  5", anything else = the v1 path (ATTiny regulator, TC358762 bridge, 800x480 at 30 MHz, FT5x06 touch at 0x38).
- Found: the UI is drawn on it at its own size (800x480: **no scaling**; the HDMI scaler is not used), HDMI is not
  used. Not found on either port: HDMI as before (the scaler, "HDMI display" in docs/pi5.md).
- Touch: rp1dsi's FT5x06 driver, polled over I2C from the display's `Update` (core 0, every 10 ms; each poll is an
  I2C transfer, its cost shows in the stats line's `longest update`). Coordinates are panel pixels. A USB touch
  screen still has priority. If the display does not start with touch (rp1dsi accepts only a "generic FT5x06";
  EDT M06/M09/M12 and FT5x26 ids are refused with `Model is not supported`), it is started again without touch, so the
  picture still works.
- `dsi_rotate=180` in cmdline.txt: picture and touch turned by 180 degrees together (rp1dsi's `SetRotation`: the
  ATTiny's scan direction and the touch coordinates). `dsitouch_inv=1|2|3` mirrors x / y / both on top, as on the Pi 4.
  (The Pi 4's `lcd_rotate=2` in config.txt does nothing on the Pi 5.)
- Screensaver: the DSI backlight off (rp1dsi's `SetBacklightBrightness (0)`), back to 200 (rp1dsi's default) on wake.
- No config.txt line is needed or used for it. Do not add `dtoverlay=vc4-kms-dsi-7inch` or `display_auto_detect=1`:
  they only edit Linux's device tree, Circle ignores them; harmless, but they suggest Linux KMS is involved.
- The log:

      display: DSI: DISP0: display regulator id 0xNN (official 7" v1 or compatible: TC358762, FT5x06)
      rp1touch1: Model is generic FT5x06 (NN)          (or an EDT model name)
      display: DSI: DISP0 800x480 (rp1dsi, no scaling), rotation 0, touch FT5x06 polled
      engine: boot screen: 800x480 in N us
      display: 800x480, LVGL 9.4
      display: touch: DSI (rp1dsi)
      engine: display: 800x480, input touch (DSI), longest update N us | ...      (every 10 s)

  Nothing on a port: `display: DSI: DISP1: no display regulator at I2C4 0x45`. Nothing on both: each bus is scanned,
  `display: DSI: DISP0 I2C6 answers at: 0x.. 0x..` (or `none`), then `display: DSI: no display found, HDMI`.

### Cabling (DSI)

- **Pi 5**: its two connectors (`CAM/DISP 0`, `CAM/DISP 1`, printed on the board) are the 22-pin 0.5 mm kind. A display
  with a 15-pin 1 mm ribbon (the official v1, the OSOYOO) needs a **22-pin to 15-pin display cable** ("Raspberry Pi 5
  display cable"); the Pi 4's 15-pin ribbon does not fit. Either connector works (the engine tries both); `CAM/DISP 0`
  is tried first.
- **Pi 4**: the 15-pin `DISPLAY` connector (not `CAMERA`), the panel's own ribbon.
- Ribbon orientation (*inferred*; check the panel's manual picture): lift the connector's latch, slide the ribbon in
  fully with its bare contacts facing the connector's contact pins (look into the slot), press the latch down. The
  15-pin end at the panel the same way. A ribbon the wrong way round: nothing answers on I2C (`no display regulator`).
- Power: the official v1 and compatibles take 5 V from the Pi's header (pins 2 or 4 = 5 V, a GND pin) on jumper wires,
  not over the ribbon. The serial console uses pins 6 (GND), 8, 10: put the display's GND on pin 9, 14 or 20 if pin 6
  is taken.
- Connect everything with the Pi off.

### If the DSI screen is not firmware-compatible (fallback, not implemented)

- Symptom (Pi 4): the boot log shows no `DSI` in the firmware's display list, and the screen stays dark (or shows only
  its backlight). On Raspberry Pi OS it then works only with a KMS overlay.
- Symptom (Pi 5): `no display regulator at ... 0x45` on both ports and the scan lines; or the regulator answers but
  `rp1touch1`/`rp1dsi` log an error (bridge or DSI host init) and `DSI: DISP0: not started`. The scan's addresses tell
  what is on the bus: 0x45 the official-style regulator, 0x38 an FT5x06 touch (only after the regulator releases its
  reset), 0x14 or 0x5D a Goodix touch; any other address: look up the chip on the panel's board.
- Plan A (expected for the OSOYOO V4.0): it emulates the official display, and works as above.
- Plan B: some 7" panels use `dtoverlay=vc4-kms-dsi-ili9881-7inch` (an ILI9881C panel). That is not
  firmware-compatible: the firmware does not drive it.
- What would be needed for plan B:
  - A bare-metal DSI host driver for the BCM2711 (DSI1 on the Pi 4's 15-pin connector): the DSI PHY and clock setup,
    the panel's init sequence (from the Linux panel driver), the video timing, and the HVS/pixel-valve path from the
    frame buffer to DSI. The firmware does none of this for such a panel.
  - Its touch controller (often Goodix GT911 or FocalTech FT5x06 on I2C) as a Circle I2C driver.
  - Look up first: the panel controller (ILI9881C or other), the touch controller and its I2C address (on Raspberry Pi OS:
    `i2cdetect -y 10` and the overlay in `/boot/firmware/config.txt`).
  - Estimate: the touch driver a few days; the DSI host and panel weeks, without documentation for the BCM2711's display
    pipeline. Circle has a DSI driver only for the Pi 5 (`addon/rp1dsi`, RP1). Cheaper: an HDMI screen.

## The boot screen

- Before LVGL: `pi/ui/sidecar_splash.c` draws "SIDECAR / EXPANDER" in the OLED theme's dot-matrix font with a row of 8 segments (CLASSIC, with cmdline `uistyle=classic`: the wordmark and a thin bar) straight into the frame buffer, right after the kernel's first log line (`boot screen: 800x480 in 9585 us`). The bar fills one of 8 steps per boot stage (SD card, send FX, the core's card, the console, cores 1-3, the bench, USB, LVGL); LVGL's home screen replaces it at its first redraw. `display=0`: no boot screen either.
- `config.txt`'s `disable_splash=1` (sdboot's block, docs/pi-engine.md) removes the firmware's rainbow square, so the screen stays black until the boot screen.
- On the Mac: `make -C pi/ui/host splash.png`.

## Two kernels

| kernel | what | build | flash |
|---|---|---|---|
| `pi/lvgl-test` | the screen and touch only: single core, no engine (966 KB). "Rytm connected" here means any USB MIDI device (umidi1); the tracks show "-" | `pi/lvgl-test/build.sh` | `CIRCLE_TREE=circle-engine pi/circle/flash.sh ../lvgl-test 60` |
| `pi/engine` | the engine with the display (`display=1`, the default; `display=0` turns it off) | `pi/engine/build.sh kernel` | `CIRCLE_TREE=circle-engine pi/circle/flash.sh ../engine 60` |

- Both kernels build against the `circle-engine` tree. LVGL is added to that tree once:
  - `pi/ui/fetch-lvgl.sh` fetches it into `pi/circle/build/lvgl-src`.
  - `pi/ui/lvgl-in-tree.sh` copies it into `addon/lvgl/lvgl` and builds `liblvgl.a`.
- Nothing else in the tree changes.
- From a worktree, set `CIRCLE_BUILD=<main checkout>/pi/circle/build` for both `build.sh` and `flash.sh`.
- Try `pi/lvgl-test` first: if the display or touch misbehaves, the engine is not involved.
- Both kernels answer `RYTM-REBOOT`, so the next flash needs no power cycle.

## Set up (by hand, once)

1. **config.txt** on the SD card. These are Waveshare's lines for the Raspberry Pi, plus a forced hotplug and no overscan.
   The card's config.txt ends in a `[pi5]` section, so add the block **at the very end, starting with `[all]`**:
   ```
   [all]
   hdmi_force_hotplug=1
   hdmi_group=2
   hdmi_mode=87
   hdmi_cvt=800 480 60 6 0 0 0
   disable_overscan=1
   ```
   - The firmware reads these before Circle's bootloader starts, and a serial upload does not change them.
   - Circle then asks the firmware for its display size (`CBcmFrameBuffer`, width/height 0) and gets 800x480.
   - If config.txt cannot be changed, `width=800 height=480` in `cmdline.txt` asks for an 800x480 frame buffer. The
     HDMI mode then comes from the display's EDID [untested].
   - A 1024x600 panel (GeeekPi EP-0194): `hdmi_cvt=1024 600 60 6 0 0 0` instead ("Screen sizes"); `width=1024 height=600`
     in cmdline.txt is the same fallback [untested].
2. **Cabling**, all with the Pi off:
   - HDMI: the display's HDMI to the Pi's **micro-HDMI 0**, the port next to USB-C power. Circle's display 0 is HDMI0.
   - Touch: the display's **Touch** micro-USB to a free **USB-A** port on the Pi. The Rytm stays on its own USB-A port.
     The touch port also powers the display, about 0.4 A at 5 V.
   - Power: the Pi's 5 V 3 A supply. If the display flickers, feed it 5 V 2 A on its own power port instead.
   - Connect the display before power-on. The frame buffer is set up at boot, and HDMI hotplug is not followed.
3. **Flash** (above). After a power cycle the Pi waits in the bootloader.

## What you should see

- The serial log shows `display: firmware N display(s): ...` (above), `display: EDID: ...`, `display: UI 800x480 ...`
  ("Screen sizes"), `display: 800x480, LVGL 9.4` (1024x600 on such a panel), then
  `touch screen bound (touch1)` and `touch: USB` once the panel has enumerated.
  - `pi/lvgl-test` logs `input touch (USB), umidi1 yes|no, core 0 busy N%, longest update N us` every 10 s.
  - The engine logs `display: 800x480, input touch (USB), longest update N us` every 10 s (`touch (DSI)` with the
    DSI touch, `mouse1`, `none`).
    - After it: `touch: ev +N rep +N raw X,Y up|DOWN` (touch events and USB reports since the last line, the last raw point) and
      `hid err N (xhci N, last code C) resub N reset N (+N) fail N ep state S pend N ignored N reenum N (try N/4 held Nms key K)`. State 1 is running, 2 halted.
    - Touch used to die within a minute: a transfer error (code 36, a split transaction error on the panel behind the
      hub) halts the xHCI interrupt endpoint and stock Circle never resets it. `pi/circle/circle-hid.patch` resets and
      resubmits it from the main loop: each error then shows as `err`, `resub` and `reset` all +1 and the touch goes on.
      `fail` or `GAVE-UP` above 0 means the recovery itself does not work.
    - Changing the touch rotation in the GeeekPi case's own OSD menu puts the panel into a state that endpoint reset
      cannot undo, and a cold power cycle does not clear it: thousands of resets and resubmits all reported success,
      `ep state` stayed 2, `rep` stayed 0, and only pulling the USB cable and putting it back fixed it.
      `pi/circle/circle-hid-reenum.patch` escalates to the software equivalent of that replug. 10 s after the
      endpoint recovery has gone into `GAVE-UP` and achieved nothing, `CUSBHIDDevice::Poll` removes the device (the
      hub port is disabled and the xHCI slot freed) and has the parent hub enumerate it again with a fresh address
      and device context; plug-and-play re-creates the touch device and the display re-binds it. Measured
      2026-10-25: that works - counters to zero, `ep state 1`, `pend 1`, no cable touched - but this panel re-halts
      5 to 10 s later every time. So the ladder is four tries per device: two plain, then two that also drop VBUS on
      the hub port for 300 ms, which is the one thing a replug does that removing the device cannot imitate.
      `REENUM-EXHAUSTED` says the budget is spent and only the endpoint-level loop is still running; the budget is
      handed back once the endpoint has been armed with no new error for 30 s. When to escalate and how far is in
      `pi/circle/hid_reenum.h` (a plain header, copied into the Circle tree by `build.sh` as `hid_digitizer.h` is),
      and `pi/engine/tests/test_hid_reenum.cpp` replays the measured timeline against it on the Mac.
    - Reading the stats when it is not working: `try N/4` is the budget spent, `held Nms` is how long the
      given-up run has held (the escalation waits for 10000), and `key K` is the device's place in the USB tree,
      which is what the budget is keyed on - `key 0` would mean the topology is unknown and nothing will ever
      escalate. If `held` sits near 0 while `GAVE-UP` shows, the given-up run is being broken up and the escalation
      will never fire: that was the 2026-10-25 bug, where `Poll` treated the endpoint state its own
      `ResetFromHalted` had produced as health and rewound the clock, so the ladder stalled at `reenum 2` with no
      `REENUM-EXHAUSTED`. The serial log also carries
      `usbhid: Report endpoint stays halted: re-enumerating the device[ with a port power cycle] (try N of 4)`.
    - The port power cycle needs a hub that can switch port power, so every hub logs its descriptor at boot:
      `usbhub: N port(s), characteristics 0x00XX (power switching individual|ganged|none, over-current ..., ...),
      bPwrOn2PwrGood N (N ms), bHubContrCurrent N mA, removable 0xNN, power mask 0xNN`. With `power switching none`
      the last two tries fall back to the plain re-enumeration and say
      `usbhid: No port power cycle: this hub has no logical power switching`. With `ganged` the hub may drop every
      port, so anything else on it is re-enumerated too (plug-and-play handles that as a replug).
- The screen: dark (the docs site's greys and green accent), with the "Sidecar" wordmark and EXPANDER in the title bar. Touching it moves the green ring, the TAP counter counts, and the slider
  follows the finger.
- Check calibration and orientation with the readout:
  - The screen values should match the finger's position, with 0,0 at the top left.
  - Circle's USB touch driver reports the raw HID values. Their range is the descriptor's logical maximum (the
    `raw range` of the `touch screen bound` line). Without one: if they stay inside the frame buffer's size (800x480,
    1024x600), they are taken as pixels. Once one exceeds the screen, the range becomes 0-4095, or 0-32767 if a value
    exceeds 4095. The range covers the whole frame buffer; with the UI centred on a bigger one its offset is taken off
    (a touch on the black border lands on the UI's edge). The Pi 5's scaled HDMI: the range covers the UI.
  - If the result is off, set `touchscreen=minx,maxx,miny,maxy` (raw values from the readout) in `cmdline.txt`.

## Screensaver and brightness

- **Screensaver:** after 1 hour (default) without interaction the screen goes black. Interaction is a touch on
  the panel or a change made from the display; Rytm activity does not keep it awake.
  - While black, LVGL is not run (no drawing on core 0). The frame buffer is filled black and the firmware's
    blank-screen property (0x00040002) is called. With `hdmi_blanking=1` in config.txt the firmware turns the HDMI
    output off (measured: not yet on the Waveshare 5" HDMI LCD (H); whether its backlight follows is to be seen on
    the real panel).
  - Any touch wakes it, and that press only wakes the screen: the pointer reads released until the finger is up.
  - The log gets one line when it blanks and one when it wakes (`screensaver: ...`).
  - Settings: Off / 15 min / 1 h / 4 h on the display's About page and the web page's Sound tab; the console
    command is `screensaver=MIN` (0 = never, at most 1440).
- **Brightness:** 5-100 %, default 100 %, applied live. Even at 5 % the picture stays readable (the dimming layer is at most 70 % dark), so the slider can always be found again.
  - It dims the picture, not the backlight: a black layer over the screen (LVGL's top layer); in the OLED theme the
    dots themselves are dimmed as they are expanded.
  - Waveshare's wiki for the 5inch HDMI LCD (H) lists only the OSD buttons and a hardware change (remove a
    22 ohm resistor, wire the pad to a GPIO for PWM; "may damage the LCD"). It lists no USB HID, DDC/CI or other
    software control, so the software layer is the method used.
  - Settings: the slider on the display's About page and on the web page's Sound tab; the console command is
    `brightness=PCT`.
- **Touch test:** shown by default; `touchtest=0` hides it (the Touch works button, the console, or `touchtest=0` in the file), `touchtest=1` shows it again (About page toggle or console).
- Settings > **System** (the former Service tab: FIFO target and the test drone for testing) also holds the CPU clock, 1500 / 1200 / 1000 MHz (`cpumhz=`, docs/pi-engine.md "CPU clock"); it is stored in the same file when it is not the default.
- The settings are stored in `/.rytm/display.txt` (`screensaver=60`, `brightness=100`, `touchtest=1`, `uistyle=oled`, and
  `res=` when one was added by hand: "Screen sizes"), written by core 0 as a whole file (tmp + rename). `screensaver=`, `brightness=`, `touchtest=` and `uistyle=` in cmdline.txt override
  the file at boot.
- **UI style:** `uistyle=oled|classic` (console, web page, the Display page): the theme, switched live (docs/UI-THEMES.md).
- `/status` carries `screensaver`, `brightness`, `touchtest` (1 shown, 0 hidden), `display` (a display is attached) and `blank`.

## Cost

- **Kernel image:**
  - LVGL and the UI add about 0.69 MB: the engine went from 790 KB to 1.48 MB.
  - text+data+bss is now 2.56 MB, of which the bss is 1.09 MB.
  - Adding the send FX's 2.7 MB of bss gives about 5.3 MB, inside the 8 MB `KERNEL_MAX_SIZE`.
  - Since pi-heap (2026-10-17) the send FX (4.7 MB) and the core's big buffers (578 KB: core/heap_buffers.h) come from Circle's heap at boot (kernel.cpp BootAlloc), not .bss: `_end` went from 0x82ee88 to 0x30e6c8 (bss 5.95 MB to 575 KB).
  - LVGL is about 549 KB of code and 99 KB of fonts (Montserrat 12-24), plus 1 KB of bss.
- **Heap**, outside the kernel image (Circle's heap):
  - One draw buffer of 48 lines: 77 KB from `HEAP_DMA30`.
  - LVGL's objects use `malloc` (`LV_STDLIB_CLIB`): tens of KB for this screen.
- **The frame buffer:** 800x480x16 bit = 750 KB (1024x600: 1.2 MB). It lives in the GPU's memory (`gpu_mem`), not in
  the ARM heap. At 1024x600 the draw buffer is 98 KB and the OLED dot band 66 KB (77 / 51 KB at 800x480).
- **1024x600 redraws** (1.6x the pixels; Mac `./shot` x34, Pi 4 *estimated*, not measured): a full redraw costs about
  1.3x the 800x480 one: OLED home 6.6 -> 8.7 ms, SEQ 5.7 -> 7.7 ms, RYTM 5.6 -> 7.3 ms; CLASSIC home 12.9 -> 16.8 ms, SEQ
  13.1 -> 17.2 ms, RYTM 12.7 -> 16.5 ms. The SEQ grid refresh 1.4 -> 2.2 ms (OLED) / 3.8 -> 5.7 ms (CLASSIC). What runs
  while playing hardly changes (a playing frame 0.45 -> 0.5 ms OLED, 0.84 -> 0.95 ms CLASSIC). A theme switch's longest
  band: 4.3 -> 5.0 ms (to CLASSIC), 2.4 -> 2.9 ms (to OLED). A CLASSIC full redraw at 1024x600 (~17 ms) is the longest
  core 0 update; it happens only on a page change.
- **Time:**
  - Everything runs on core 0's loop: `CSidecarDisplay::Update`, LVGL's timer handler, and the touch events, which
    arrive in the USB IRQ.
  - Cores 1-3 (render, control) are not touched. The audio paths are IRQ-driven and do not wait on the loop.
  - LVGL runs at most every 10 ms and redraws only the areas that changed, at up to 30 fps.
  - The once-a-second status update is a few labels. The first full-screen draw at boot is the longest [the time is
    logged as "longest update"; not yet measured on the Pi].
  - While LVGL draws, core 0's loop is late: MIDI to the Rytm, the console and the TCP link wait.
- The design spec put the UI on core 3. Core 3 is the engine's control core: it paces the render blocks, so LVGL does
  not go there.
