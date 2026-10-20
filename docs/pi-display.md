# Pi display

The Pi engine draws a Sidecar screen on an 800x480 display: HDMI with USB touch, or a DSI display (the official 7"
display and compatibles, see "Supported screens"): on the Pi 4 the firmware drives it, on the Pi 5 Circle's
`addon/rp1dsi` ("DSI on the Pi 5"). The UI is built with LVGL 9.4.0 (MIT),
the version Circle 51.1.1's `addon/lvgl` submodule pins. The code is in `pi/ui`:
- `sidecar_ui.c`: the screen. It uses LVGL only, so the same file also builds on the Mac.
- `sidecar_display.cpp`: the Circle side. It is based on Circle's `addon/lvgl/lvgl.cpp` (GPLv3).

## The screen

- Title bar "Sidecar | Expander", with "Rytm connected" (green) or "Rytm not connected" (red) on the right.
- TRACKS: voices 1-8 with their current machine (PLAITS, FM, JUNO, DSPS, DAISY, DD, USER).
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
(Sound, Send FX, About) switches between them, and Settings reopens the page last used. Every control is at least 48 px
high and sends a console command through `CConsole::Command`, the web page's path (`UiCommand` in kernel.cpp logs
`display: command "..."`). Commands only queue (core 3 applies them), so a tap never waits for the engine. About 150 ms
after a command, and every second, `CKernel::PushUi` reads the state back the way `/status` does, so a change from the web
page, the serial console or the display shows on all of them.
- **Sound**:
  - MIDI notes Off/On, Output Main/Tracks, Computer audio Off/On (disabled without the USB-C gadget), FIFO target
    with - and + (steps of 8 frames).
  - Voice 1-8, then that voice's Machine (PLAITS JUNO FM DAISY DSPS GRAIN DD; none checked for a user machine) and
    Drone (Off, This voice, All). DSPS and GRAIN are one machine in the status, so the one picked stays checked.
- **Send FX**:
  - FX 1 / FX 2 TYPE and the 12 tracks' send levels, read only: the Rytm's kit owns them, and a console override
    (`sendlvl=`) lasts only until the next 0x20 message.
  - **Kill tails** sends `killfx`: the CC 120 request (`core::fx::all_sound_off`, both FX fade out in about 5 ms, clear and
    return). Needs a build with the cc120-kill change.
- **About**: the web page address, IP, whether the web server runs (cmdline `web=`, not switchable at run time), link,
  USB-C gadget mode, the engine's build time, Circle and LVGL versions.

`make -C pi/ui/host` renders the layout on the Mac (no window, no SDL): `pi/ui/host/shot.png` (home), `shot-sound.png`,
`shot-fx.png` and `shot-about.png` (plus `shot-hidden.png` and `shot-about-hidden.png`: the touch test hidden), with made-up status values and a touch at 392,145. It then taps the controls through
LVGL's pointer input and prints the command each one sends.

Cost: the display's update is core 0's loop work. At rest, the longest update in the 10 s log line is about 300 us (the
once-a-second state refresh with the settings widgets); the first full draw after boot takes about 7.4 ms. A page change
redraws the area it uncovers in 48-line bands, about the cost of that first draw.

## Supported screens

| Screen | Status |
|---|---|
| Waveshare 5" / 7" HDMI LCD with USB touch (800x480) | Works (measured on the 5" (H)). |
| Official Raspberry Pi 7" Touch Display v1 (DSI, 800x480) | Expected on both boards, not yet measured here. Pi 4: firmware frame buffer + `CRPiTouchScreen` (firmware touch buffer). Pi 5: Circle's `addon/rp1dsi` (display and touch, no firmware). |
| OSOYOO 7" IPS DSI V4.0 (800x480, 15-pin DSI) | Expected, to be confirmed: sold as plug-and-play on Raspberry Pi OS, which normally means it emulates the official display (ATTiny regulator at I2C 0x45, TC358762 bridge, FT5x06 touch at 0x38). The boot log names what answers ("DSI on the Pi 5"). |
| Raspberry Pi Touch Display 2 | Pi 4: not supported (needs a KMS DSI panel driver and a Goodix touch driver, which Circle does not have on the Pi 4). Pi 5: `addon/rp1dsi` drives it (*inferred*, untested); it is 720x1280 portrait, which the 800x480 layout is not made for. |

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

- Before LVGL: `pi/ui/sidecar_splash.c` draws "SIDECAR | EXPANDER" (a 5x7 bitmap font, scaled 4x at 800x480) and a thin bar on a near-black screen straight into the frame buffer, right after the kernel's first log line (`boot screen: 800x480 in 9585 us`). The bar fills one of 8 steps per boot stage (SD card, send FX, the core's card, the console, cores 1-3, the bench, USB, LVGL); LVGL's home screen replaces it at its first redraw. `display=0`: no boot screen either.
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
2. **Cabling**, all with the Pi off:
   - HDMI: the display's HDMI to the Pi's **micro-HDMI 0**, the port next to USB-C power. Circle's display 0 is HDMI0.
   - Touch: the display's **Touch** micro-USB to a free **USB-A** port on the Pi. The Rytm stays on its own USB-A port.
     The touch port also powers the display, about 0.4 A at 5 V.
   - Power: the Pi's 5 V 3 A supply. If the display flickers, feed it 5 V 2 A on its own power port instead.
   - Connect the display before power-on. The frame buffer is set up at boot, and HDMI hotplug is not followed.
3. **Flash** (above). After a power cycle the Pi waits in the bootloader.

## What you should see

- The serial log shows `display: firmware N display(s): ...` (above), `display: 800x480, LVGL 9.4`, then
  `touch screen bound (touch1)` and `touch: USB` once the panel has enumerated.
  - `pi/lvgl-test` logs `input touch (USB), umidi1 yes|no, core 0 busy N%, longest update N us` every 10 s.
  - The engine logs `display: 800x480, input touch (USB), longest update N us` every 10 s (`touch (DSI)` with the
    DSI touch, `mouse1`, `none`).
    - After it: `touch: ev +N rep +N raw X,Y up|DOWN` (touch events and USB reports since the last line, the last raw point) and
      `hid err N (xhci N, last code C) resub N reset N (+N) fail N ep state S pend N ignored N`. State 1 is running, 2 halted.
    - Touch used to die within a minute: a transfer error (code 36, a split transaction error on the panel behind the
      hub) halts the xHCI interrupt endpoint and stock Circle never resets it. `pi/circle/circle-hid.patch` resets and
      resubmits it from the main loop: each error then shows as `err`, `resub` and `reset` all +1 and the touch goes on.
      `fail` or `GAVE-UP` above 0 means the recovery itself does not work.
- The screen: dark (the docs site's greys and green accent), with the "Sidecar" wordmark and EXPANDER in the title bar. Touching it moves the green ring, the TAP counter counts, and the slider
  follows the finger.
- Check calibration and orientation with the readout:
  - The screen values should match the finger's position, with 0,0 at the top left.
  - Circle's USB touch driver reports the raw HID values. If they stay inside 800x480, they are taken as pixels.
    Once one exceeds the screen, the range becomes 0-4095, or 0-32767 if a value exceeds 4095.
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
  - It dims the picture, not the backlight: a black layer over the screen (LVGL's top layer).
  - Waveshare's wiki for the 5inch HDMI LCD (H) lists only the OSD buttons and a hardware change (remove a
    22 ohm resistor, wire the pad to a GPIO for PWM; "may damage the LCD"). It lists no USB HID, DDC/CI or other
    software control, so the software layer is the method used.
  - Settings: the slider on the display's About page and on the web page's Sound tab; the console command is
    `brightness=PCT`.
- **Touch test:** shown by default; `touchtest=0` hides it (the Touch works button, the console, or `touchtest=0` in the file), `touchtest=1` shows it again (About page toggle or console).
- The settings are stored in `/.rytm/display.txt` (`screensaver=60`, `brightness=100`, `touchtest=1`), written by core 0 as a
  whole file (tmp + rename). `screensaver=`, `brightness=` and `touchtest=` in cmdline.txt override the file at boot.
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
- **The frame buffer:** 800x480x16 bit = 750 KB. It lives in the GPU's memory (`gpu_mem`), not in the ARM heap.
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
