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

The same engine as on the Pi 4 (`docs/pi-engine.md`), built with `RASPPI=5`, with one part taken out:

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
| USB-C gadget (UAC2 12/12 + MIDI to the computer) | yes | **compiled out** (`SIDECAR_NO_GADGET`); the boot log says `gadget: none on this board (Pi 5 build)` |
| serial console | GPIO14/15, 921600 | GPIO14/15, 921600 (default, `PI5_UART=0`), or the 3-pin debug connector (`PI5_UART=10`) |
| `sdboot` (serial bootloader + `engine.img`) and `flash.sh` | yes | not yet: see "Updating the kernel" |

Circle patches on the Pi 5 tree: the two spike2 xHCI patches (in the `pi/circle/build/circle` checkout it is copied
from), `circle-hid.patch`, `circle-mdns.patch`, `circle-http-origin.patch`. Not `circle-gadget.patch` (DWC2 gadget
files, which Circle builds only for the Pi <= 4).

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
as built above). Options go in a one-line `cmdline.txt` as on the Pi 4 (`docs/pi-engine.md`, "Options"); `gadget=`
is ignored on the Pi 5. No Wi-Fi credentials are on the card; Wi-Fi is off until it is turned on from the web page
or with `sidecar-wifi.txt` (`docs/pi-engine.md`, "Wi-Fi without a cable").

The card is also the engine's card (`/Samples`, `/.rytm/`, recordings): the first boot writes the self-test content.

If the Pi 5 shows its boot diagnostics screen on HDMI instead of booting, the EEPROM may be too old for Circle's
kernel: update it with Raspberry Pi Imager ("Misc utility images", "Bootloader"), then retry. Power: the official
27 W supply; with a 3 A supply the Pi 5 limits its USB ports to 600 mA in total, enough with the Rytm on its own
power. Measured 2026-10-19: fed from a hub port (an Overhub on the 27 W adapter: no 5 A USB-PD, reduced-power mode)
Wi-Fi ran badly (~25 `httpd: Receive failed (-12)` in 3 minutes, the page 5.4 s and "503 busy"); on the adapter directly
none of that (0 in a session, the page 0.12-0.15 s). The engine logs the supply at boot, on a change and once a minute
(`power: throttled 0x... (under-voltage now/boot, ...) | max_current N mA | ok` or the warning; `max_current` is the
firmware's `/chosen/power/max_current`, Pi 5 only, inferred from its device tree and not yet seen on this board), the
web page shows a Power tile and a warning, the display's bottom line `POWER TOO WEAK: use 27 W adapter` in red. The
warning names each board's official supply: Pi 5 "use the 27 W (5 V 5 A) adapter directly", Pi 4 "use the official 15 W
(5.1 V 3 A) supply" (display: `POWER TOO WEAK: use 15 W supply`).

## One card for both: Pi 4B and Pi 5

`pi/circle/make-sd-dual.sh K4 SDBOOT K5` (the release: `sidecar-expander/scripts/package_pi.sh`, which builds all
three from one commit) makes `out/sidecar-sd/`, `out/sidecar-pi-sd-<commit>.zip` (+ `.sha256`) and
`out/BUILD-INFO-pi.txt`. It refuses kernels whose version stamps differ, a `-dirty` stamp, or a kernel for the wrong
board.

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
`cmdline.txt` (`sdport=0`; `gadget=` is ignored on the Pi 5), the starter content (`JUNO/`, `DX7/`,
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
  it, by what the image is (the Pi 5 build's `gadget: none on this board (Pi 5 build)` string): a Pi 4 kernel ->
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
- config.txt: nothing for DSI. The `[pi5]` HDMI lines stay (an HDMI-only setup needs `hdmi_force_hotplug=1`; with a DSI
  display they do no harm: the HDMI frame buffer is never opened). No `vc4-kms-*` overlay, no `display_auto_detect`.
- Build: `pi/engine/build.sh` builds `addon/rp1dsi/librp1dsi.a` in the Pi 5 tree once (Circle unchanged); the engine
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
- **Circle: no.** `lib/usb/gadget/Makefile` builds the DWC2 gadget only for `RASPPI != 5`, `bcm2712.h` has no
  `ARM_USB_BASE`, and the README's Pi 5 column lists no gadget driver.
- **A port** (our `circle-gadget.patch` sits on top of it, unchanged, since it only touches the gadget library):
  1. `bcm2712.h`: the DWC2 base (`0x1000480000`, core/host/device/power offsets as `bcm2835.h`'s) and its IRQ number.
  2. Power: domain 6 through the firmware (the Pi 4 path is `SetPowerState (DEVICE_ID_USB_HCD)`: check that the
     Pi 5 firmware takes the same tag, else the domain-state tag).
  3. DMA addresses: the DWC2's internal DMA needs bus addresses below 4 GB; Circle's Pi 5 `BUS_ADDRESS` and where
     its coherent region sits must be checked (the Pi 4 uses the 0xC0000000 alias).
  4. `lib/usb/gadget/Makefile` for `RASPPI=5`, then the engine's Makefile keeps `gadget.o sidecar_gadget.o`.
  5. Possible core differences (FIFO sizes in `GHWCFG*`, the isochronous timing our patch relies on): read and
     log them at start.
- **Power**: in gadget mode the Pi 5 is powered from the computer's USB-C port (or a USB-C power/data splitter, or
  5 V on the header). A Mac port gives at most 3 A: enough to boot (the Pi 5 then limits its USB ports to 600 mA),
  the Rytm has its own supply.
- **Estimate**: 1-2 days to bring up Circle's MIDI gadget on the Pi 5 (steps 1-4), then about a day for our UAC2
  12/12 + MIDI on it and the Mac tests (`pi/engine/tests/sidecar_*_test.py`), unless the core differs. A risk worth
  a first spike: Circle's own `sample/29-miniorgan` as a MIDI gadget on the Pi 5.

## Test checklist (Pi 5, Rytm, Mac)

1. **Boot log**: serial at 921600 on pins 6/8/10. Expect Circle's banner, the engine's `==== rytm-pi engine` line,
   `gadget: none on this board (Pi 5 build)`, the network line (`169.254.61.60`), then a stats line a second. Note the
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
10. Not applicable on the Pi 5 build: the USB-C Sidecar tests (`sidecar_*_test.py`, `gadget_midi_test.py`).
