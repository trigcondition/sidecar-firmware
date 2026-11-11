# The Pi engine on a Raspberry Pi 5 (first build, 2026-10-19)

Status: **runs on a Pi 5** (*measured* 2026-10-19: Rytm, voices, remote UI, web UI over Ethernet, replug, Wi-Fi);
the HDMI display shows the UI with touch (measured, at 1920x1080 unscaled); the 800x480 UI scaled to it is measured working (slightly soft); the DSI display (official 7" v1 and compatibles, through Circle's `addon/rp1dsi`) is built, not yet run ("DSI display"). Everything below about the hardware is *inferred* from Circle's sources,
its README and the Pi 5 device tree, unless marked *measured*. The Pi 4 build of the `pi5` branch was unchanged (same
symbols and sizes as `final-010`, measured by comparing the two `kernel8-rpi4.elf` with `nm -S`). In 0.2.0 (`release-020`:
`pi5` + `display-dsi` + `web-wifi`) the Pi 4 kernel differs from 0.1.0 only by those merges (`nm -S`, measured): the
page also gzip-compressed (`WebPageGz`, 39 KB) and the web server's counters, 3 s timeout and busy line; the supply's
state (`ReadPower`, `powerstate::*`); the display's `Put` (a pass-through to the frame buffer on the Pi 4: no scaling,
the firmware's DSI as in 0.1.0) and the red POWER line. The USB-C gadget, sdboot and the boot files are as in 0.1.0.

## What the Pi 5 build is

The same engine as on the Pi 4 (`docs/pi-engine.md`), built with `RASPPI=5`:

| | Pi 4 | Pi 5 build |
|---|---|---|
| kernel | `kernel8-rpi4.img` | `kernel_2712.img` (about 2.3 MB) |
| Circle tree | `pi/circle/build/circle-engine` | `pi/circle/build/circle-engine-pi5` (its own copy, `configure -r 5`) |
| USB host to the Rytm (Overbridge, MIDI) | VL805 xHCI | RP1's two xHCI controllers (Circle's `CUSBSubSystem`): any of the 4 USB-A ports |
| voices, FX, SD card, recorder | yes | yes |
| Ethernet / web UI / mDNS `sidecar.local` | BCM54213 | RP1's MACB/GEM (Circle picks it) |
| Wi-Fi | CYW43455 | the same chip, firmware files with the Pi 5's names (in the SD set): **works** (*measured* 2026-10-19: credentials entered on the web page, saved, connected) |
| HDMI display (LVGL) | yes | needs `hdmi_force_hotplug=1`; the firmware's frame buffer is 1920x1080, the 800x480 UI is scaled to it ("HDMI display"); `display=0` turns it off |
| DSI display + touch | the firmware's (frame buffer + FT5406 buffer) | Circle's `addon/rp1dsi` (display and FT5x06 touch, no firmware), preferred over HDMI, 800x480 unscaled ("DSI display"; built, not run) |
| USB-C gadget (UAC2 12/12 + MIDI to the computer) | yes | **yes, in testing** (0.4.0): on by default as on the Pi 4, `gadget=0` in `cmdline.txt` turns it off; needs a powered USB-C hub or dock that passes data, since USB-C is also the Pi 5's power input ("USB device side"). `PI5_GADGET=0` compiles it out (`SIDECAR_NO_GADGET`; boot log `gadget: none on this board (Pi 5 build)`) |
| serial console | GPIO14/15, 921600 | GPIO14/15, 921600 (default, `PI5_UART=0`), or the 3-pin debug connector (`PI5_UART=10`) |
| `sdboot` (serial bootloader + `engine.img`) and `flash.sh` | yes | not yet: see "Updating the kernel" |

Circle patches on the Pi 5 tree (`circle-engine-pi5usb`): the two spike2 xHCI patches (in the `pi/circle/build/circle`
checkout it is copied from), `circle-hid.patch`, `circle-mdns.patch`, `circle-http-origin.patch`, and for the gadget
`circle-gadget.patch`, `circle-gadget-wake.patch`, `circle-pi5-gadget.patch`, `circle-gadget-reset.patch` (Circle builds
its DWC2 gadget only for the Pi <= 4 without them). A `PI5_GADGET=0` build uses `circle-engine-pi5`, without the gadget
patches.

Build flags for the Pi 5 (`pi/engine/build.sh`; `BUILD_FLAGS=1` prints what a build would use and stops):

| flags | kernel | for |
|---|---|---|
| `RASPPI=5` (`PI5_GADGET=1` is the default) | the engine with the USB-C gadget, `GADGET` default `sidecar` | release cards (`make-sd-dual.sh`), `pi5-try` |
| `RASPPI=5 PI5_GADGET=1 PI5_TESTMODE=1` | the same plus the engine test mode (`testsecs=`, the test watchdog, `/usbtest.log`, `testcard=1`'s card gate) | tryboots of Raspberry Pi OS only: `make-sd-dual.sh` refuses it (`testsecs=`) |
| `RASPPI=5 PI5_GADGET=0` | no gadget (`SIDECAR_NO_GADGET`), as before 0.4.0 | a fallback; `make-sd-dual.sh` takes it only with `PI5_NOGADGET=1` |

`pi/engine/tests/test_pi5_flags.sh` (in `make -C pi/engine/tests`) checks this table without building a kernel: build.sh's
flags, the Makefile's defines, objects and link flags, and the sources' `#if` lines for each build.

## Build

    # the Pi 5 kernel (the first run also makes the Circle tree, about 2 minutes; run it twice if it stops at
    # "addon/wlan/hostap/wpa_supplicant: No such file": the hostap clone happens once the tree exists)
    RASPPI=5 CIRCLE_BUILD=<circle build dir> PI_IP=169.254.61.60 PI_NETMASK=255.255.0.0 EMUHOST=169.254.61.59:7301 \
      pi/engine/build.sh            # later: ... pi/engine/build.sh kernel
    # the SD card folder
    CIRCLE_BUILD=<circle build dir> pi/circle/make-sd-pi5.sh    # -> pi/circle/out/pi5-sd/

`<circle build dir>` is the main checkout's `pi/circle/build` for a worktree. Without `RASPPI` (or `RASPPI=4`) the
build is the Pi 4's, as before. One worktree holds one board's objects at a time: switching `RASPPI` (or `PI5_UART`)
removes the other board's `.o` files first (`hosted/.board`), so the next build is a full one (about 15 s, measured).
Both kernels stay side by side in `pi/engine/`.

## The SD card

The Pi 5 keeps its boot firmware in its EEPROM: the card needs no `start*.elf`, `fixup*.dat` or armstub. A Pi 4
card from 0.1.0 does **not** boot a Pi 5 (its sdboot block is under `[all]`, so a Pi 5 would also take
`kernel=sdboot.img`): use this Pi 5 card, or the card for both boards below.

Copy the **contents** of `pi/circle/out/pi5-sd/` to the root of a FAT32 (MBR) card:

| file | from |
|---|---|
| `kernel_2712.img` | the engine (`RASPPI=5 pi/engine/build.sh`) |
| `config.txt` | `make-sd-pi5.sh` (below) |
| `bcm2712-rpi-5-b.dtb`, `bcm2712d0-rpi-5-b.dtb`, `overlays/bcm2712d0.dtbo` | Circle's `boot/` (the Raspberry Pi firmware commit Circle 51.1.1 pins) |
| `firmware/brcmfmac43455-sdio.raspberrypi,5-model-b.{bin,txt,clm_blob}` | Wi-Fi chip firmware (RPi-Distro/firmware-nonfree, the commit Circle's `addon/wlan/firmware/Makefile` pins); only used with Wi-Fi on |
| `LICENCE.broadcom`, `COPYING.linux`, `KERNEL.txt` | licences; the kernel's size and build time |

`config.txt`:

    arm_64bit=1
    kernel_address=0x80000
    initial_turbo=0
    enable_uart=1
    disable_splash=1
    boot_delay=0
    os_check=0          # the Pi 5 firmware's OS compatibility check, off for a bare-metal kernel

    [pi5]
    kernel=kernel_2712.img

No `cmdline.txt`: the build's defaults apply (`ip=169.254.61.60`, `netmask=255.255.0.0`, `emuhost=169.254.61.59:7301`,
as built above). Options go in a one-line `cmdline.txt` as on the Pi 4 (`docs/pi-engine.md`, "Options"), `gadget=`
too (ignored only by a `PI5_GADGET=0` kernel). No Wi-Fi credentials are on the card; Wi-Fi is off until it is turned on from the web page
or with `sidecar-wifi.txt` (`docs/pi-engine.md`, "Wi-Fi without a cable").

The card is also the engine's card (`/Samples`, `/.rytm/`, recordings): the first boot writes the self-test content.

If the Pi 5 shows its boot diagnostics screen on HDMI instead of booting, the EEPROM may be too old for Circle's
kernel: update it with Raspberry Pi Imager ("Misc utility images", "Bootloader"), then retry. Power: the official
27 W supply; with a 3 A supply the Pi 5 limits its USB ports to 600 mA in total, enough with the Rytm on its own
power. Measured 2026-10-19: fed from a hub port (an Overhub on the 27 W adapter: no 5 A USB-PD, reduced-power mode)
Wi-Fi ran badly (~25 `httpd: Receive failed (-12)` in 3 minutes, the page 5.4 s and "503 busy"); on the adapter directly
none of that (0 in a session, the page 0.12-0.15 s). The engine logs the supply at boot, on a change and once a minute
(`power: throttled 0x... (under-voltage now/boot, ...) | max_current N mA | ok` or the warning; `max_current` is the
firmware's `/chosen/power/max_current`, Pi 5 only), the web page shows a Power tile and a warning, the display's bottom
line `POWER TOO WEAK: use 27 W adapter` in red. The warning is for under-voltage only. A `max_current` below 5000 with no
under-voltage is a note on the Power tile and in the log that names the measured current: `3 A mode (no 5 A USB-PD)`
with the USB ports' 600 mA in all at 3000, `1.5 A mode (no 5 A USB-PD)` with "use the 27 W adapter directly" below
3 A (one hub's USB-C port reports 1500). One Pi 5 on the 27 W adapter, plugged in directly, reports 3000 with no
under-voltage ever. The
warning names each board's official supply: Pi 5 "use the 27 W (5 V 5 A) adapter directly", Pi 4 "use the official 15 W
(5.1 V 3 A) supply" (display: `POWER TOO WEAK: use 15 W supply`).

## One card for both: Pi 4B and Pi 5

`pi/circle/make-sd-dual.sh K4 SDBOOT K5` (the release: `sidecar-expander/scripts/package_pi.sh`, which builds all
three from one commit) makes `out/sidecar-sd/`, `out/sidecar-pi-sd-<commit>.zip` (+ `.sha256`) and
`out/BUILD-INFO-pi.txt`. It refuses kernels whose version stamps differ, a `-dirty` stamp, a kernel for the wrong
board, a kernel with the engine test mode (the string `testsecs=`: `PI5_TESTMODE=1`, or a `PI5_GADGET=1` build from
before 0.4.0), and a Pi 5 kernel without the USB-C gadget (`PI5_GADGET=0`) unless `PI5_NOGADGET=1` is set. The Pi 5
kernel of a release is thus the plain `RASPPI=5 pi/engine/build.sh` one: gadget in, computer audio on by default.

`config.txt` (the firmware reads it top to bottom; after `[pi4]` only a Pi 4B/400/CM4 uses the lines, after `[pi5]`
only a Pi 5/500/CM5, after `[all]` both; a later value wins):

    [all]
    arm_64bit=1
    kernel_address=0x80000
    initial_turbo=0

    [pi4]
    armstub=armstub8-rpi4.bin
    max_framebuffers=2

    [pi4]                          # the 800x480 HDMI display (as 0.1.0's, which had [all]: Pi 4 only here)
    hdmi_force_hotplug=1
    hdmi_group=2
    hdmi_mode=87
    hdmi_cvt=800 480 60 6 0 0 0
    disable_overscan=1

    [pi5]
    kernel=kernel_2712.img
    enable_uart=1
    disable_splash=1
    boot_delay=0
    os_check=0

    [pi5]                          # the Pi 5's 800x480 HDMI display ("HDMI display" below)
    hdmi_force_hotplug=1
    framebuffer_width=800
    framebuffer_height=480
    hdmi_group=2
    hdmi_mode=87
    hdmi_cvt=800 480 60 6 0 0 0

    # sidecar sdboot begin (...)
    [pi4]                          # sdboot.py's block (block()): [pi4] since this card, [all] before
    kernel=sdboot.img
    initramfs engine.img 0x02000000
    disable_splash=1
    boot_delay=0
    # sidecar sdboot end

What each board ends up with (the script prints this from the file and checks the files are there):

- **Pi 4**: `arm_64bit=1 kernel_address=0x80000 initial_turbo=0 armstub=armstub8-rpi4.bin max_framebuffers=2`, the
  five HDMI lines, `kernel=sdboot.img initramfs engine.img 0x02000000 disable_splash=1 boot_delay=0`: the same
  settings as the 0.1.0 Pi 4 card (compared). `start4.elf`/`fixup4.dat`/`bcm2711-rpi-4-b.dtb` from the card, sdboot
  checks and starts `engine.img`. `enable_uart`, `os_check` and the Pi 5's kernel are never seen.
- **Pi 5**: `arm_64bit=1 kernel_address=0x80000 initial_turbo=0 kernel=kernel_2712.img enable_uart=1
  disable_splash=1 boot_delay=0 os_check=0`, the settings of `out/pi5-sd/` (compared), which boots on hardware, plus
  the six HDMI lines of "HDMI display" below (not yet measured with them). Its EEPROM firmware picks
  `bcm2712[d0]-rpi-5-b.dtb` and `overlays/bcm2712d0.dtbo`; the armstub, `initramfs`, `max_framebuffers` and
  `disable_overscan` are Pi 4 only.

Side by side, no name in common: Pi 4 `start4.elf fixup4.dat bcm2711-rpi-4-b.dtb armstub8-rpi4.bin sdboot.img
engine.img`; Pi 5 `kernel_2712.img bcm2712-rpi-5-b.dtb bcm2712d0-rpi-5-b.dtb overlays/bcm2712d0.dtbo`; Wi-Fi in
`firmware/`, the same three RPi-Distro files under both names (Circle's `ether4330.c`: `brcmfmac43455-sdio.*` for
RASPPI <= 4, `brcmfmac43455-sdio.raspberrypi,5-model-b.*` for 5) with `LICENCE.cypress` and `NOTICE`. Shared:
`cmdline.txt` (`sdport=0`; `gadget=0` turns the USB-C gadget off on both boards), the starter content (`JUNO/`, `DX7/`,
`STARTER-CONTENT-*.txt`), `/Plugins`, `/Samples`, `/.rytm` (Wi-Fi settings too: one network for both boards).

**Plugins**: the same RMPA packages load on both (*inferred*): they are built `-mcpu=cortex-a72` (ARMv8.0, which
the Pi 5's Cortex-A76, ARMv8.2, runs), the loader is the same `core/user/um.cpp` with `rmp_host_target =
RMP_TARGET_A64` in both kernels, and the code arena's page-table walk (`um_pi.cpp`: TTBR0 -> level 2, 512 MB entries
-> level 3, 64 KB pages) matches Circle's `translationtable64.cpp` for RASPPI 5 (256 level-2 entries instead of
128). The Pi 5's "0 packages in /Plugins" came after the arena was made executable (um::begin runs only then), with
an empty `/Plugins`.

### Updating a card for both

- **Card swap** (the release's way: `cmdline.txt` has `sdport=0`, so TCP 7310 is off): unzip the new set over the
  card; each board's files are replaced together.
- `sdboot.py install IMG [IMG]` (with `sdport` on, or `SIDECAR_LINK=serial`) puts each image where its board boots
  it, by what the image is (the Pi 5 build's `board: Raspberry Pi 5 build (kernel_2712.img)` line, or the older kernels' `gadget: none on this board (Pi 5 build)`): a Pi 4 kernel ->
  `engine.img` (wrapped), a Pi 5 kernel -> `kernel_2712.img` (plain, `.prev` kept). Run from either board, the other
  board's kernel stays as it was, so the card still boots it. `install` refuses anything that is not a Pi engine
  kernel; `pi5-try` refuses a Pi 4 kernel and writes `[pi5] kernel=sdboot-try.img`; `pi5-commit` refuses a
  `sdboot-try.img` that is not a Pi 5 kernel; `tryboot`/`commit` refuse a `sdboot-try.img` larger than 256 KB (a Pi 5
  kernel left there by `pi5-try`), so a Pi 4 never tries a Pi 5 kernel as its sdboot.
- The web page has no kernel update (boot files are read-only in its file browser, `fsweb.h`).

**Untested on hardware** (all of the above is static: the config model, the file names, a mock of `sdboot.py`): the
dual card on either board; `[pi4]`/`[pi5]` as read by the Pi 4's `start4.elf` (0641c5bf) and the Pi 5's EEPROM;
the Wi-Fi firmware under the Pi 5's names; `sdboot.py install` / `pi5-try` / `pi5-commit` on a Pi 5; tryboot on the
Pi 5 at all.
## HDMI display

*Measured* (2026-10-19, Pi 5, Waveshare HDMI LCD with its USB touch):

1. Card from `make-sd-pi5.sh` (no HDMI lines): no picture ("no signal"); log `display: firmware 0 display(s): (display
   id not answered) -> frame buffer on 0 0x0 FAILED`. The firmware saw no display at power-on, so Circle made no
   frame buffer (and the USB touch, `WaveShare WS170120`, was never bound).
2. With `hdmi_force_hotplug=1` (and the lines below) in `[pi5]`: picture and USB touch work, but the firmware's frame
   buffer is **1920x1080** (`display: 1920x1080, input touch (USB)`): it ignored `framebuffer_width/height` and
   `hdmi_cvt`. The 800x480 UI sat in the top-left corner, small (the panel downscales the 1080p signal).

On the Pi 5 Circle cannot choose the HDMI mode: the firmware sets it at power-on and the kernel only gets that frame
buffer (Circle's Pi 5 notes: "no configuration in config.txt, cannot set display resolution from application").
No documented Pi 5 option was found that gives the firmware frame buffer a custom size: reports say `hdmi_mode`,
`hdmi_force_mode`, `hdmi_timings` and `gpu_mem` have no effect on the Pi 5 (Raspberry Pi forum t=380434, Volumio
forum), and here `framebuffer_width/height` and `hdmi_cvt` did not either (*measured*). `hdmi_force_hotplug=1` is
the line that matters.

So the engine scales on the Pi 5 (`pi/ui/sidecar_display.cpp`, `RASPPI >= 5` only; the Pi 4 draws as before):

- LVGL draws at the **UI size**: `width=`/`height=` (or `res=800x480|1024x600`) from `cmdline.txt` if given, else when
  the frame buffer is bigger than 1024x600 **1024x600** if the HDMI display's EDID prefers 1024x600 (a 1024x600 panel;
  *inferred*, docs/pi-display.md "Screen sizes"), else **800x480**; else the frame buffer's size (then nothing is
  scaled). Never bigger than the frame buffer. The pages have an 800x480 and a 1024x600 layout; a bigger UI size draws
  the 1024x600 one centred in it.
- Each flushed area is scaled nearest-neighbour to the whole frame buffer, per axis (800x480 on 1920x1080: x 2.40,
  y 2.25; the panel stretches the 16:9 signal back to its 5:3). Only LVGL's dirty areas are scaled; each UI row is
  scaled once, its repeated rows copied, 64 frame-buffer rows per DMA copy. The boot screen is drawn at the UI size
  and scaled the same way. A full-screen redraw writes 4 MB of frame buffer (cost *not measured*: see the stats
  line's `longest update`).
- Touch is mapped to the UI size (the digitizer covers the panel, so this needs no calibration); a mouse's frame
  buffer coordinates are scaled down to it.
- With 0 displays reported the engine logs why and asks the firmware again when the UI starts (about 1.8 s later).

config.txt lines for the Pi 5 (for the dual card: in `[pi5]`, or under `[all]` as on the Pi 4). Only
`hdmi_force_hotplug=1` is known to be needed; the others did nothing on the Pi 5 here, harmless:

    [pi5]
    hdmi_force_hotplug=1

cmdline.txt: nothing needed (800x480 is the default UI size, 1024x600 on a panel whose EDID prefers it). A shared card's
Pi 4 `width=800 height=480` gives the same; `res=1024x600` the 1024x600 layout scaled. `width=1920 height=1080` draws the
1024x600 layout unscaled, centred.

Connect the LCD to **micro-HDMI 0** (next to the USB-C power) before power-on. Circle's own note (`doc/issues.txt`):
"The WLAN connection is delicate on the Raspberry Pi 5, when a HDMI display is used at the same time": check Wi-Fi
with the display on.

Expected log:

    display: UI 800x480 scaled to the frame buffer's 1920x1080 (x 2.40, y 2.25; width=/height= in cmdline.txt set the UI's size)
    display: firmware 1 display(s): ... -> frame buffer on 0 1920x1080
    engine: boot screen: 1920x1080 in ... us
    display: 800x480, LVGL 9.N
    display: touch screen bound (touch1)
    engine: display: 800x480, input touch (USB), longest update N us      (the stats line)

### Only the backlight, or 640x480: the screen did not answer at power-on

*Reported* (a user, Waveshare 10.1" HDMI LCD (B), 1280x800, Pi 5): the panel showed only its backlight. The log:

    display: firmware 1 display(s): 0=HDMI0 -> frame buffer on 0 640x480
    display: EDID: not available (forced hotplug without a display, or the firmware did not answer)
    display: HDMI 640x480 with no EDID: the screen did not answer at power-on. Power the screen first, then the Pi (HDMI0, next to USB-C; config.txt [pi5] hdmi_force_hotplug=1). docs/pi5.md "HDMI display"
    display: res=1280x800 does not fit the frame buffer's 640x480: automatic

The firmware reads the screen's EDID once, at power-on. When the screen is not fully powered yet (a panel fed from
the Pi's USB, or switched on after the Pi), the EDID comes back empty (its header `00 00 00` instead of
`00 FF FF ...`) and the firmware falls back to **640x480**. Circle cannot change the Pi 5's HDMI mode afterwards (above),
so the engine cannot fix it at run time; many panels (this one among them) show no picture at 640x480.

Since 0.5.0 the engine says so when the frame buffer is 640x480 and the EDID gave no preferred mode (the decision:
`pi/ui/display_hint.h`, tested in `pi/engine/tests/test_display_hint.cpp`; the Pi 4 is unchanged):

- the warning above in the log (`/log`, the serial console);
- on the boot screen, under the progress bar, large: "No screen info at power-on. Power the screen first, then the
  Pi." (in the dot font's capitals), kept at least 5 s; then the normal UI at 640x480, for a screen that does show it;
- `/status` gets `"displayHint":"No screen info at power-on: ..."` (absent otherwise; the web pages do not show it yet).

The fix: **power the screen first, then the Pi** (its own supply, or plugged in and on before the Pi's USB-C), with
the screen on **HDMI0** (next to USB-C) and `hdmi_force_hotplug=1` in `[pi5]`. A screen whose EDID really prefers
640x480 gets no hint.

## DSI display

Built, **not yet run** (everything here *inferred* from Circle 51.1.1's `addon/rp1dsi`, `sample/28-touchscreen` and
`addon/lvgl/sample`). Details, log lines and fallbacks: docs/pi-display.md, "DSI on the Pi 5" and "Cabling (DSI)".

- The display: the official 7" Touch Display v1 or a compatible (the OSOYOO 7" IPS DSI V4.0 is sold as one) on
  `CAM/DISP 0` or `CAM/DISP 1`, with a 22-pin to 15-pin Pi 5 display cable; 5 V from the header.
- The pick, at boot: a DSI display found (its regulator answers at I2C 0x45, DISP0 tried first, then DISP1) -> the UI on
  it at 800x480, no scaling, HDMI unused; else HDMI with the scaler ("HDMI display"). `dsi=0` in cmdline.txt: HDMI only,
  no DSI probe. `dsi_port=0|1`: one port only.
- Upside down in the case: `dsi_rotate=180` in cmdline.txt (picture and touch together). Mirrored touch only:
  `dsitouch_inv=1` (x), `2` (y), `3`.
- Touch Display 2 (720x1280 portrait, mounted landscape): the 1280x720 UI (`res=1024x600`: the 1024x600 one centred), rotated in software, `dsi_rotate=270`
  (default) or `90`; docs/pi-display.md, "Touch Display 2 on the Pi 5".
- config.txt: nothing for DSI. The `[pi5]` HDMI lines stay (an HDMI-only setup needs `hdmi_force_hotplug=1`; with a DSI
  display they do no harm: the HDMI frame buffer is never opened). No `vc4-kms-*` overlay, no `display_auto_detect`.
- Build: `pi/engine/build.sh` applies `pi/circle/circle-rp1dsi-fb.patch` and `circle-rp1dsi-td2.patch` and builds
  `addon/rp1dsi/librp1dsi.a` in the Pi 5 tree once; the engine
  links it before `libinput` (both have a `CRPiTouchScreen`; rp1dsi's is the display, libinput's is the Pi 4's firmware
  touch and is not linked on the Pi 5).

## Serial cable

Circle's own default on the Pi 5 is the 3-pin debug connector (its serial device 10, a JST-SH socket between the
two micro-HDMI ports). This build moves the console to **GPIO14/15** (serial device 0, RP1's UART0) so the Pi 4's
cable and wiring work unchanged:

| adapter (3.3 V) | Pi 5 header |
|---|---|
| GND | pin 6 |
| RX | pin 8 (GPIO14, the Pi's TX) |
| TX | pin 10 (GPIO15, the Pi's RX) |
| VCC | not connected |

**921600 baud, 8N1** (the engine's console and its log; `:sdf` for `sdboot.py` over serial). The Pi 5 firmware's own
boot messages do not appear there (they go to the debug connector, and only with `uart_2ndstage=1`): the first line on
the cable is Circle's. To log on the debug connector instead (a Raspberry Pi Debug Probe or a JST-SH cable; pinout:
https://datasheets.raspberrypi.com/debug/debug-connector-specification.pdf): `PI5_UART=10 RASPPI=5 pi/engine/build.sh kernel`.

## Updating the kernel

`sdboot` and `flash.sh` are not available on the Pi 5 yet:

- Circle's serial bootloader for the Pi 5 (`tools/bootloader`, `kernel_2712.img`) talks on the **debug connector**
  only (its `periph.c`: `PBASE + 0x1001000` = UART10), not on GPIO14/15.
- `sdboot.patch`'s watchdog is the BCM2711's (`PBASE + 0x10001c`); the BCM2712's is at `0x107d200000`
  (= its `PBASE + 0x120001c`). `build-sdboot.sh` builds `kernel8-rpi4.img` only.
- So a Pi 5 sdboot needs: the patch's watchdog address per SoC, `build-sdboot.sh` building `kernel_2712.img`, and the
  engine on the same UART as the bootloader (`PI5_UART=10` and a debug-connector cable, or a bootloader patched to
  RP1's UART0 on GPIO14/15, which also needs RP1's GPIO function select). About half a day, then a hardware test.

What replaces it for now:

1. **Card swap**: copy a new `kernel_2712.img` to the card in the Mac.
2. **Over the running engine** (Ethernet TCP 7310, or the serial cable's `:sdf` when TCP does not connect; the same
   file service as the Pi 4's, `pi/engine/sdfiles.h`), new `sdboot.py` commands:

       uv run --with pyserial python pi/circle/sdboot.py pi5-try [IMG]    # IMG -> sdboot-try.img, tryboot.txt, one-shot tryboot
       uv run --with pyserial python pi/circle/sdboot.py pi5-commit       # it worked: sdboot-try.img -> kernel_2712.img (.prev kept)
       uv run --with pyserial python pi/circle/sdboot.py pi5-previous     # the previous kernel back (the two swap)

   `pi5-try` boots the new kernel once (the firmware's tryboot flag, mailbox tag 0x00038064; a power cycle boots
   `kernel_2712.img` as before), so a broken kernel costs a power cycle, not the card. Not measured on the Pi 5
   (tryboot exists on the Pi 5's firmware; Circle's mailbox is the same).

## USB device side (the computer's USB-C) on the Pi 5

- **Hardware: yes** (*measured* in the device tree `bcm2712-rpi-5-b.dtb`): BCM2712 has a DWC2 OTG core,
  `usb@480000` (`compatible = "brcm,bcm2835-usb"`, the Pi 4's driver), registers at `0x10_0048_0000`, GIC SPI 73,
  power domain 6 (USB) of the firmware's power driver, a `usb-nop-xceiv` PHY (nothing to program), `status =
  "disabled"` by default. It is wired to the USB-C port, the Pi 5's power input (Linux uses it as a gadget with
  `dtoverlay=dwc2,dr_mode=peripheral`: *inferred*, not tried here).
- **Circle: not by default.** `lib/usb/gadget/Makefile` builds the DWC2 gadget only for `RASPPI != 5`, and `bcm2712.h`
  has no `ARM_USB_BASE`. `RASPPI=5 pi/engine/build.sh` builds it (`PI5_GADGET=1`, the default since 0.4.0), in a Circle
  tree of its own (`circle-engine-pi5usb`, the host-side patches + `circle-gadget.patch` + `circle-gadget-wake.patch` +
  `circle-pi5-gadget.patch` + `circle-gadget-reset.patch`, then the `usb=mirror` patches as on the Pi 4, see below).
- **Overbridge passthrough (`usb=mirror`): Pi 4 since 0.4.1; Pi 5 since 0.4.2.** The mirror
  (docs/usb-mirror.md: the computer sees the attached box itself) is in the Pi 5's kernel again (0.4.1's left it out),
  with the same patches and the same USB-C mode switch as the Pi 4: OVERBRIDGE on the web page's Sound tab, Settings >
  Sound and the Rytm's SIDECAR MENU > USB-C, `/status` `usbMode.modes` `["class","overbridge"]` (`"board":5`). Power: a
  Pi 5 that uses its USB-C for data gets about 1.5 A from a computer or hub port: on a powered hub port (about 1.5 A) the 0.4.2 test unit ran without under-voltage;
  while an earlier setup on a hub port was power-starved and reset when Overbridge started, and a direct Apple Silicon port did not enumerate it; for full power it needs a
  USB-C power + data splitter with USB-PD pass-through (docs/usb-mirror.md "The Pi 5").
- **On by default (0.4.0, in testing).** The Pi 5's `GADGET` default is `sidecar`, as on the Pi 4: UAC2 12 in / 12 out
  and USB MIDI on USB-C, the web page's Sound tab and the touchscreen's Settings > Sound offer computer audio as on the
  Pi 4 (both go by `/status` "sidecar" / "mac": the Sidecar device is up). `gadget=0` in `cmdline.txt` turns it off,
  `gadget=midi` gives P1's MIDI echo. The USB-C port is also the Pi 5's power input, so the computer reaches it through a
  **powered USB-C hub or dock that passes data and powers the Pi** (for example a dock with USB-PD pass-through that
  can give 5 A; a Mac port alone gives 3 A at most, and the Pi 5 then limits its USB-A ports to 600 mA). A Pi 5 that
  uses its USB-C for data gets about 1.5 A from a computer or hub port; full power needs a USB-C power+data splitter
  with USB-PD pass-through. Measured so far
  through an Overhub II at 480 Mb/s (the bring-up kernel and the engine's test kernels, "Engine test mode"); the release
  kernel with a computer on USB-C has not yet run on hardware. Kernels before 0.4.0 that had the gadget compiled in
  (`PI5_GADGET=1`, run on a Pi 5 in 2026-11) defaulted to `gadget=0`; they ran on the official 27 W supply with no boot,
  power or engine problem, but with the gadget not started.
- **A power supply alone on USB-C** (no computer; read from the code, not yet measured with the gadget started): the
  core is set up and its pull-up is on, but no host ever sends a USB reset, so the device stays unconfigured. The
  gadget has no timer and no retry: Circle's DWC2 driver has only the bus's interrupts (reset, enumeration done,
  suspend, wake-up, endpoints; not the start of frame), and an idle line makes at most one suspend. Nothing spins: core
  0's loop polls one flag (`UpdatePlugAndPlay`) and drains an empty MIDI queue; the IN ring takes the Rytm's frames
  (12 channels a frame, the oldest dropped, as on the Pi 4) and the computer's OUT adds nothing to the Rytm's stream
  (no OUT alternate setting). The audio cores (1-2) and the control core (3) never touch the gadget. The log has the
  boot's `gadget: USB-C Sidecar up` and then the gadget's stats line once a minute (not each second) while no computer
  has configured it. `/status` says `"gadget":"sidecar","sidecar":true,"usbHost":false`; the page's USB-C card says
  "no computer on USB-C". The same core and driver run this way on every Pi 4 on its own supply.
  The USB-PD negotiation (the Pi 5's 5 A) is the boot firmware's, on the CC lines, before the kernel starts; the
  gadget drives only D+ / D- and its power domain (the firmware's "set power state", device 3, as Linux's dwc2), so
  nothing in the code touches it; the engine only reads the firmware's `max_current` (`/status` "power").
- **What was measured before** (branch `pi5-usbc`, kernels 1-6, 2026-10-19, powered and connected directly from a
  MacBook; its `docs/pi5.md` has the detail): the core reads fine at `0x10_0048_0000`: `0x4F54280A`, HWCFG2 `0x228DDD50`,
  HWCFG3 `0x0FF000E8`, HWCFG4 `0x1FF00020`, 4080 FIFO words, 8 EPs, dedicated FIFOs, IRQs arrive on SPI 73. The firmware
  answers every power tag with state 0, the SD's too, so the answer says nothing. It leaves the core in host mode
  (GUSBCFG `20402700`, DCTL `2`, SOFs counting, so the PHY clock runs). Forced to device mode the core shows B-session
  valid, its pull-up on and DSTS suspended (line idle, not SE0: an unconnected pull-up would read as a USB reset), and yet
  no USB reset came and the Mac saw nothing, with or without the dwc2 overlay applied. Kernel 7's result was never
  recorded. Under Raspberry Pi OS the same board works through a hub (`dtoverlay=dwc2,dr_mode=peripheral`, 2026-10-31).
- **What Linux does** (read in `drivers/usb/dwc2`, `raspberrypi-power.c` and the Pi 5 device tree): nothing else in
  hardware. `usb@480000` has no `resets`; its PHY is a `usb-nop-xceiv` with no reset GPIO, no supply and no registers;
  `clk-usb` is a fixed clock; `power-domains = <&power 6>` is the firmware's "set power state", device 3, state 1. So
  what differs from Circle's kernels is the register sequence, the mailbox call's exact form or the cabling. Circle's
  own lacks: DCTL.PwrOnPrgDone pulsed, GOTGINT / GINTSTS cleared before the connect, the global NAKs cleared, the
  tag's request length 0. Those are `usbinit=4`, the default now; `pi/circle/pi5gadget` walks through all the ways.
- **The port** (`pi/circle/circle-pi5-gadget.patch`, on top of `circle-gadget.patch`):
  1. `bcm2835.h`, `bcm2711int.h`: the base `0x1000480000` (offsets as `bcm2835.h`'s), GIC SPI 73; `CDWHCIRegister` holds
     64-bit addresses.
  2. Power: the mailbox tag above, first, then the core is read whatever the answer.
  3. DMA: `/axi`'s `dma-ranges` are 1:1 for the low 64 GB, so a DMA address is the ARM physical address, not the Pi 4's
     `0xC0000000` alias, and the registers are 32-bit: below 4 GB, asserted (`new` takes from `HEAP_LOW`, below 1 GB).
     The DT does not mark the core `dma-coherent`: cache maintenance as on the Pi 4.
  4. `lib/usb/gadget/Makefile` for `RASPPI=5`; the model check; any `OT2` core; HWCFG logged, FIFO RAM checked.
  5. `InitCorePi5` (`usbinit=` in `cmdline.txt`): the ways of leaving the firmware's host mode, `Reconnect (N)` to try
     another at run time.
- **Bring-up kernel** (`pi/circle/pi5gadget`, `pi/circle/build-pi5gadget.sh` -> `pi/circle/out/pi5gadget/kernel_2712.img`):
  Circle's USB MIDI gadget alone, single core, no engine, every USB event on the serial console (Circle's own trace with
  `USB_GADGET_DEBUG`), the computer's MIDI echoed (`pi/engine/tests/gadget_midi_test.py`). While no USB reset comes it
  tries each `usbinit=` way for 6 s, so one boot answers for all of them; a watchdog (15 s) ends a hang and it reboots
  after `testsecs=` (300 s), which ends a tryboot. Its `cmdline.txt` options and serial keys are at the top of `kernel.cpp`.
  *Measured* 2026-10-31, through the Overhub II: it enumerates on the Mac as "Sidecar" 1209:0001 at 480 Mb/s, configured,
  within about 15 s of the reboot; tryboot returns to Linux after `testsecs=`. Which `usbinit=` way did it: not known (no
  serial then).
- **Engine test mode** (`testsecs=S` in `cmdline.txt`, `PI5_GADGET=1 PI5_TESTMODE=1` builds only; before 0.4.0 every
  `PI5_GADGET=1` build had it): the engine on a card that is not
  Sidecar's, with no Rytm: a tryboot of Raspberry Pi OS. `Fs::begin ()` refuses, so the engine runs as with no card and
  writes nothing to that system's boot partition; the watchdog runs from the start of the boot; at S seconds it reboots.
  The log goes to `/usbtest.log` on that partition, every 10 s, only if the file exists: a file made beforehand under
  Linux, whose bytes are overwritten, so no cluster is allocated and the FATs are not written. Each write still ends by
  rewriting the file's directory entry, one sector of the root directory: a reset in that moment could damage it, a small
  risk to that system's boot partition. `make-sd-dual.sh` refuses a kernel with the test mode (`testsecs=`). Build:
  `PI5_GADGET=1 PI5_TESTMODE=1 RASPPI=5 pi/engine/build.sh` (same Circle tree; switching the flag rebuilds the objects).
  `testcard=1` with `testsecs=`: the engine uses the card as usual, so projects, presets and plugins can be tested on a tryboot.
  The watchdog and the reboot stay, `/usbtest.log` goes on, and the black box writes `/.rytm/blackbox.log`. FatFs's name-changing
  calls go through a gate in these kernels only (`card_guard.h`, `--wrap`): on the card only `/.rytm` and fsweb's content folders
  may change. `config.txt`, `cmdline.txt`, the kernels, `overlays/` and anything else at the root are refused with `FR_DENIED`
  and a `cardguard: testcard: ...` log line, whichever path asks: port 7310's PUT and DEL, the Wi-Fi import `sidecar-wifi.txt`,
  and the pairing recourse `sidecar-paircode.txt`. So a boot with no display writes no code down: use the Rytm's screen, the
  serial console, or devices already paired in `/.rytm/pair.txt`. `/.rytm` is readable under Linux, Wi-Fi key and paired hashes
  included. Unlike the mode without it this writes FATs: a reset during a write can damage that partition, and it is small.
  `uac2test=1` starts the test source (`:uac2test=on`: tones on IN 1-10, IN 11 / 12 the loopback of OUT 1 / 12, the generic
  timer the clock), `midifake=1` the MIDI loop. In the test source each second logs every OUT channel's rising zero
  crossings and peak: `sidecar: test out Hz/peak: 1:1009/251 2:1103/251 ...` (peak in 1/1000 of full scale, -12 dBFS = 251).
- **Power**: with a computer on USB-C the Pi 5 is powered through that cable: a powered USB-C hub or dock that
  passes data (the way to test it), the computer's own port, a USB-C power/data splitter, or 5 V on the header. A Mac port gives at most 3 A: enough to boot (the Pi 5 then limits its USB ports to 600 mA),
  the Rytm has its own supply.
- **Estimate**: 1-2 days to bring up Circle's MIDI gadget on the Pi 5 (steps 1-4), then about a day for our UAC2
  12/12 + MIDI on it and the Mac tests (`pi/engine/tests/sidecar_*_test.py`), unless the core differs. A risk worth
  a first spike: Circle's own `sample/29-miniorgan` as a MIDI gadget on the Pi 5.

## Power button

The Pi 5's own power button shuts the Sidecar down cleanly (the Pi 5 build only; the Pi 4 has no such button):

- **Short press** while it runs: the screen goes dark, the audio to the Rytm fades out over 20 ms (no click), the
  Overbridge stream stops, a recording take is closed (its WAV files valid), the settings and the edits that wait to be
  saved (LFOs, INSERT FX, the sequencer's patterns, the project, `display.txt`) are written, then the black box, then the
  board turns off (PSCI SYSTEM_OFF). The card's work gets 3 s at most: a hung card does not keep the board on (a take cut
  there is repaired at the next boot, as after a power loss). The log says `power-off: the card's work done in N ms`.
- **Press again** while it is off: the Pi 5's firmware powers it back on and the Sidecar boots.
- **Long press** (held a few seconds): the power chip's hard off. It skips all of the above, as pulling the cable does.

A press only counts once the button has been seen released since the boot, so the press that powered the board on
does not turn it off again. The button is read every 20 ms; Circle's 50 ms debounce runs only when it reads pressed.
The console (and the web page's `/cmd`) can do the same: `:poweroff=1`. The Pi 4 build answers `poweroff: refused,
the Pi 5 only`.

## Test checklist (Pi 5, Rytm, Mac)

1. **Boot log**: serial at 921600 on pins 6/8/10. Expect Circle's banner, the engine's `==== rytm-pi engine` line,
   `board: Raspberry Pi 5 build (kernel_2712.img)`, `gadget: USB-C Sidecar up (12 in / 12 out, ...)`, the network line (`169.254.61.60`), then a stats line a second. Note the
   time from power-on to the banner. No output at all: try `PI5_UART=10` and the debug connector (or `uart_2ndstage=1`
   to see the firmware's own lines there), then `display=0` in `cmdline.txt`.
2. **Self-test** (no Rytm): the console's self-test (`docs/pi-engine.md`, "The self-test") runs and the stats line shows
   the cores' load; the Pi 5 has 4 Cortex-A76 cores at 2.4 GHz, so the loads should be below the Pi 4's.
3. **Rytm enumerates**: plug the Rytm (Overbridge mode) into a USB-A port (try a black USB 2 one first, then a blue
   one). Expect the Overbridge stream (blocks counted, no gaps) and `umidi` found. Replug test below.
4. **Voices play**: a voice machine on a Rytm track (the mods' patched firmware), its notes heard from the Rytm's
   outputs; send FX return; no underruns in the stats line over 10 minutes (the `gaps` check of `pi_run.py`).
5. **Web UI**: Mac on a self-assigned address on the same cable, `http://169.254.61.60` and `http://sidecar.local`;
   status, live log, the console's controls. Then Wi-Fi from the page (credentials typed by the user there only).
6. **Recorder**: arm, record a take with PLAY/STOP, the WAV files on the card (`docs/pi-engine.md`, "Recording"),
   checked on the Mac.
7. **Replug**: unplug and replug the Rytm 5 times, also during playback: the stream comes back each time, no hang,
   no watchdog reset.
8. **Display**: the "HDMI display" config.txt lines, the LCD on micro-HDMI 0 before power-on: the boot screen, LVGL's home screen, touch; Wi-Fi still connected with the display on.
   **DSI**: the 7" DSI display on CAM/DISP 0 (then 1), HDMI unplugged: the `display: DSI:` lines, the boot screen and
   home screen at 800x480 on the panel, touch at the finger (else `dsi_rotate=180` / `dsitouch_inv=`), the stats
   line's `input touch (DSI)` and `longest update`; with HDMI also plugged in, the UI stays on DSI; `dsi=0` gives HDMI.
9. **Update path**: `sdboot.py pi5-try` with a rebuilt kernel, check it boots, power-cycle (the old kernel), then
   `pi5-try` again and `pi5-commit`.
10. **USB-C Sidecar** (a powered USB-C hub or dock between the computer and the Pi that passes data): the USB-C tests
    (`sidecar_*_test.py`, `gadget_midi_test.py`); `/status` `usbHost` true once the computer has configured it.
