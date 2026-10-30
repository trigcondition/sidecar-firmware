#!/bin/bash
# pi/engine/build.sh - the Pi engine's Circle tree and kernel, in Docker (pi/circle's rytm-circle image).
#   ./build.sh          the tree once (pi/circle/build/circle-engine: a copy of pi/circle/build/circle - which has
#                       spike2's two USB patches - configured --multicore, kernel up to 8 MB, the gadget vendor ID
#                       for the optional Task 8 and the USB-C gadget, gadget.h; FatFs and the SD driver built), the engines' sources (on the Mac:
#                       core/engines/setup_engines.sh uses BSD sed), then the kernel -> kernel8-rpi4.img
#   ./build.sh kernel   the engines' sources and the kernel only
# PI_IP, PI_NETMASK, EMUHOST (default 192.168.77.2, 255.255.255.0, 192.168.77.1:7301): the kernel's defaults for
# cmdline.txt's ip=, netmask=, emuhost= (hosted/net_defaults.h, rewritten only when it changes; cmdline.txt wins).
# GADGET (default sidecar): the USB-C gadget when cmdline.txt has no gadget= (sidecar, midi: P1's echo, 0: off).
# SIDECAR_CH_IN (default 12): the Sidecar's UAC2 IN channels, 12 or 20 (the Pi's voices on 13-20). SIDECAR_NAME (default
# "Sidecar | Expander (Pi)"): its USB product name.
# RASPPI (default 4): 5 builds for the Pi 5 (docs/pi5.md) in its own Circle tree, pi/circle/build/circle-engine-pi5
# (made from pi/circle/build/circle as circle-engine is; the host-side patches, no gadget patch), -> kernel_2712.img,
# the USB-C gadget compiled out. PI5_UART (default 0): the Pi 5's console UART, 0 = GPIO14/15 (pins 8/10, the Pi 4's
# wiring), 10 = the 3-pin debug connector. The objects here are one board's at a time: switching RASPPI (or PI5_UART)
# removes them first, so the next build is a full one.
# STOR_EXFAT (default 0; docs/STORAGE.md): 1 builds against a Circle tree of its own (<tree>-exfat, made from
# pi/circle/build/circle as the others) whose FatFs has FF_FS_EXFAT 1 and FF_LBA64 1 (exFAT and GPT drives) - every
# library that uses FatFs (the wlan addon, wpa_supplicant) compiled with it, as their FIL / FATFS layouts change. Tested
# on the Mac only (make -C pi/engine/tests: test_stor_fatfs_exfat, fatfs-mac); not on a Pi yet.
set -euo pipefail
cd "$(dirname "$0")"
REPO=$(git rev-parse --show-toplevel)
"$REPO/core/engines/setup_engines.sh" "$PWD/engines" >/dev/null
"$REPO/tools/gen_version.sh"   # core/version.h: VERSION, the commit (docs/versioning.md); on the host, the container has no git
R=${RASPPI:-4}
case $R in 4) TREE=circle-engine; KIMG=kernel8-rpi4.img;; 5) TREE=circle-engine-pi5; KIMG=kernel_2712.img;;
  *) echo "RASPPI must be 4 or 5"; exit 1;; esac
P5U=${PI5_UART:-0}
EXF=${STOR_EXFAT:-0}
case $EXF in 0) ;; 1) TREE=$TREE-exfat;; *) echo "STOR_EXFAT must be 0 or 1"; exit 1;; esac
STAMP="RASPPI=$R PI5_UART=$P5U$( [ "$EXF" = 1 ] && echo ' STOR_EXFAT=1' || true)"
if [ "$(cat hosted/.board 2>/dev/null || echo 'RASPPI=4 PI5_UART=0')" != "$STAMP" ]; then
  echo "board changed to $STAMP: removing the other board's objects"
  rm -rf hosted; rm -f ./*.o ./*.d ../ui/*.o ../ui/*.d
fi
mkdir -p hosted
echo "$STAMP" > hosted/.board
NETH=$(printf '#define NET_DEFAULT_IP\t\t"%s"\n#define NET_DEFAULT_NETMASK\t"%s"\n#define NET_DEFAULT_EMUHOST\t"%s"\n#define GADGET_DEFAULT\t\t"%s"\n' \
  "${PI_IP:-192.168.77.2}" "${PI_NETMASK:-255.255.255.0}" "${EMUHOST:-192.168.77.1:7301}" "${GADGET:-sidecar}")
[ "$(cat hosted/net_defaults.h 2>/dev/null)" = "$NETH" ] || printf '%s\n' "$NETH" > hosted/net_defaults.h
SCH=$(printf '#define SIDECAR_CH_IN\t%s\n#define SIDECAR_NAME\t"%s"\n' "${SIDECAR_CH_IN:-12}" "${SIDECAR_NAME:-Sidecar | Expander (Pi)}")	# IN 12, or 20 (the Pi's voices on 13-20); the device's name
[ "$(cat hosted/sidecar_config.h 2>/dev/null)" = "$SCH" ] || printf '%s\n' "$SCH" > hosted/sidecar_config.h
# the web page (web.h): www/index.html built in as a raw string (rewritten only when it changes), each
# /*@include web/X.js@*/ line replaced by that file of the repo, its exports dropped (the LFO tab: shared with web/)
PAGE=$(REPO="$REPO" perl -pe 'if (m{^\s*/\*\@include (\S+)\@\*/\s*$}) { my $f = "$ENV{REPO}/$1"; open(my $h, "<", $f) or die "no $f\n"; local $/; $_ = <$h>; s/^export //mg; }' www/index.html)
WEBH=$(printf 'static const char WebPage[] = R"SIDECARPAGE(%s)SIDECARPAGE";' "$PAGE")
[ "$(cat hosted/web_page.h 2>/dev/null)" = "$WEBH" ] || printf '%s\n' "$WEBH" > hosted/web_page.h
# the same page gzip-compressed (what "/" sends, web.cpp; deterministic: -n, no name or time) as WebPageGz[]
GZH=$(printf '%s' "$PAGE" | gzip -9n | perl -e 'local $/; my @b = unpack("C*", <STDIN>); print "static const unsigned char WebPageGz[] = {";
  for my $i (0..$#b) { print "\n" if $i % 32 == 0; print $b[$i], ","; } print "\n};";')
[ "$(cat hosted/web_page_gz.h 2>/dev/null)" = "$GZH" ] || printf '%s\n' "$GZH" > hosted/web_page_gz.h
# the bundle download's service worker (bundleweb.cpp, docs/BUNDLES.md): www/bundle-sw.js as BundleSw[]
SWH=$(printf 'static const char BundleSw[] = R"SIDECARSW(%s)SIDECARSW";' "$(cat www/bundle-sw.js)")
[ "$(cat hosted/bundle_sw.h 2>/dev/null)" = "$SWH" ] || printf '%s\n' "$SWH" > hosted/bundle_sw.h
docker image inspect rytm-circle >/dev/null 2>&1 || docker build -t rytm-circle "$REPO/pi/circle"
BUILD=${CIRCLE_BUILD:-$REPO/pi/circle/build}  # as flash.sh: a worktree may point this at the main checkout's build/
"$REPO/pi/ui/fetch-lvgl.sh" "$BUILD"		# LVGL for the display (docs/pi-display.md), once
# Wi-Fi (docs/research/pi-wifi.md): Circle's addon/wlan needs its hostap submodule (rsta2/hostap, BSD), fetched once on
# the host (the container has no git credentials); the tree is a plain copy, so it is cloned into the tree directly
HOSTAP=$BUILD/$TREE/addon/wlan/hostap
if [ -d "$BUILD/$TREE" ] && [ ! -f "$HOSTAP/wpa_supplicant/Makefile.circle" ]; then
  rm -rf "$HOSTAP" && git clone -q https://github.com/rsta2/hostap.git "$HOSTAP" && git -C "$HOSTAP" checkout -q 966cab4a3bc958e7ff304a46d91a6172a1bfecaf
fi
docker run --rm -v "$REPO":/repo -v "$BUILD":/repo/pi/circle/build -w /repo/pi/engine -e MODE="${1:-all}" -e R=$R -e TREE=$TREE -e P5U=$P5U -e EXF=$EXF rytm-circle bash -euo pipefail -c '
T=/repo/pi/circle/build/$TREE
J=$(nproc)
grep -q "KERNEL_MAX_SIZE=0x1000000" $T/Config.mk 2>/dev/null || { [ ! -d $T ] || echo "tree $T: another KERNEL_MAX_SIZE, rebuilding it"; rm -f $T/.engine-ready; MODE=all; }
if { [ "$MODE" = all ] || [ ! -d $T ]; } && [ ! -f $T/.engine-ready ]; then
  [ -d /repo/pi/circle/build/circle ] || { echo "run pi/circle/build.sh first (the Circle checkout)"; exit 1; }
  rm -rf $T && cp -a /repo/pi/circle/build/circle $T
  cd $T
  if [ "$EXF" = 1 ]; then	# STOR_EXFAT: exFAT and GPT in this tree'"'"'s FatFs (docs/STORAGE.md)
    sed -i -e "s/^#define FF_FS_EXFAT.*/#define FF_FS_EXFAT\t1/" -e "s/^#define FF_LBA64.*/#define FF_LBA64\t\t1/" addon/fatfs/ffconf.h
    grep -q "^#define FF_FS_EXFAT.1" addon/fatfs/ffconf.h || { echo "ffconf.h: FF_FS_EXFAT not set"; exit 1; }
    grep -q "^#define FF_LBA64.*1" addon/fatfs/ffconf.h || { echo "ffconf.h: FF_LBA64 not set"; exit 1; }
  fi
  grep -q RytmObDeviceCreate lib/usb/usbdevicefactory.cpp || patch -p1 < /repo/pi/circle/spike2/circle-usbdevicefactory.patch
  grep -q XHCI_EP_MAX_URBS include/circle/usb/xhciendpoint.h || patch -p1 < /repo/pi/circle/spike2/circle-xhci-urbqueue.patch
  ./makeall --nosample clean >/dev/null 2>&1 || true
  ./configure -f -r $R -p aarch64-none-elf- --multicore --kernel-max-size 16 \
    -d USB_GADGET_VENDOR_ID=0x1209 -d USB_GADGET_DEVICE_ID_BASE=0x0001
  ./makeall --nosample -j$J
  make -C addon/fatfs -j$J
  make -C addon/SDCard -j$J
  touch .engine-ready
fi
# the USB-C gadget (pi/circle/circle-gadget.patch: IRQ stats, the MIDI device accessor, isochronous endpoints;
# idempotent, also on a tree made before it), then its library (up to date: nothing to do)
cd $T
if [ "$R" = 4 ]; then	# Pi 5: Circle has no gadget there (docs/pi5.md)
  grep -q GetIRQStats include/circle/usb/gadget/dwusbgadget.h || patch -p1 < /repo/pi/circle/circle-gadget.patch
  make -C lib/usb/gadget -j$J >/dev/null
fi
# f_expand in FatFs (pi/circle/circle-fatfs-expand.patch: FF_USE_EXPAND 1 - the recorder reserves the space of each
# take file up front, pi/engine/recorder.h; FIL / FATFS keep their layout and nothing else calls it, so other kernels
# built from the tree only gain one unused function); idempotent, then FatFs alone is rebuilt (its objects depend on
# ffconf.h: make rebuilds those four, nothing is cleaned)
# (a tree that has FF_USE_EXPAND 1 already, set some other way: left as it is)
grep -q circle-fatfs-expand.patch addon/fatfs/ffconf.h || grep -q "^#define FF_USE_EXPAND.1" addon/fatfs/ffconf.h \
  || patch -p1 < /repo/pi/circle/circle-fatfs-expand.patch
make -C addon/fatfs -j$J >/dev/null
# mDNS for sidecar.local (pi/circle/circle-mdns.patch: every question, the QU bit, AAAA answered with an NSEC)
grep -q circle-mdns.patch lib/net/mdnsdaemon.cpp || patch -p1 < /repo/pi/circle/circle-mdns.patch
# the HTTP daemon keeps Host, Origin and Referer (pi/circle/circle-http-origin.patch: for the same-origin check in web.cpp of
# the Wi-Fi settings POSTs; idempotent)
grep -q circle-http-origin include/circle/net/httpdaemon.h || patch -p1 < /repo/pi/circle/circle-http-origin.patch
# and the X-Sidecar-Auth header (pi/circle/circle-http-auth.patch: the pairing code'"'"'s session token, paircode.h; after
# circle-http-origin.patch, idempotent). web.cpp calls GetRequestAuth(), so a tree without this patch does not build.
grep -q circle-http-auth include/circle/net/httpdaemon.h || patch -p1 < /repo/pi/circle/circle-http-auth.patch
make -C lib/net -j$J >/dev/null
# the HID endpoint recovery and diagnostics (pi/circle/circle-hid.patch: a halted xHCI interrupt endpoint is reset
# and resubmitted from the main loop; counters for the display log line; idempotent), then lib/usb
grep -q GetHIDStats include/circle/usb/usbhiddevice.h || patch -p1 < /repo/pi/circle/circle-hid.patch
# the USB touch screen (pi/circle/circle-touch.patch, docs/pi-display.md "Supported screens"): the descriptor parser
# pi/circle/hid_digitizer.h (copied in when it changed), the touch driver on boot keyboard/mouse interfaces with a
# digitizer (GeeekPi 0416:c169), report protocol, Input Mode; after circle-hid.patch, idempotent
cmp -s /repo/pi/circle/hid_digitizer.h include/circle/usb/hiddigitizer.h || cp /repo/pi/circle/hid_digitizer.h include/circle/usb/hiddigitizer.h
grep -q circle-touch.patch lib/usb/usbtouchscreen.cpp || patch -p1 < /repo/pi/circle/circle-touch.patch
# then absolute-pointer panels (circle-touch-pointer.patch: the parse alone decides, a read-only multi-touch probe)
grep -q circle-touch-pointer.patch lib/usb/usbtouchscreen.cpp || patch -p1 < /repo/pi/circle/circle-touch-pointer.patch
# then every report of a transfer (several back to back) and a lost release after a gap (circle-touch-multi.patch)
grep -q circle-touch-multi.patch lib/usb/usbtouchscreen.cpp || patch -p1 < /repo/pi/circle/circle-touch-multi.patch
# then the escalation from that endpoint recovery to a forced device re-enumeration when it gets nowhere
# (circle-hid-reenum.patch: CUSBHIDDevice::Poll removes the device and has the parent hub enumerate it again, like a
# replug). Its own marker and its own file on purpose: circle-hid.patch is already applied in every existing tree, so
# an edit to that patch would never be re-applied (its GetHIDStats marker greps TRUE) and would silently not reach
# the kernel. This one is cut against the tree as the patches above leave it, so it applies to a fresh tree and to an
# existing one alike; idempotent
grep -q circle-hid-reenum include/circle/usb/usbhiddevice.h || patch -p1 < /repo/pi/circle/circle-hid-reenum.patch
# then the reworked escalation (circle-hid-reenum2.patch): the policy moved out to pi/circle/hid_reenum.h (copied in
# as hiddigitizer.h is, and replayed on the Mac by pi/engine/tests/test_hid_reenum.cpp), the given-up clock no longer
# believes the endpoint state its own reset produced, and the ladder ends in a VBUS drop on the hub port.
# A second file rather than an edit to circle-hid-reenum.patch, for the same reason that one is not an edit to
# circle-hid.patch: the first version is already applied in every existing tree, so its marker greps TRUE and an
# edited patch would never be re-applied - it would silently not reach the kernel. A chain of patches each with its
# own marker (as circle-touch -> -pointer -> -multi above) applies to a fresh tree and to an existing one alike and
# needs no tree rebuild; idempotent
cmp -s /repo/pi/circle/hid_reenum.h include/circle/usb/hidreenum.h || cp /repo/pi/circle/hid_reenum.h include/circle/usb/hidreenum.h
grep -q circle-hid-reenum2 include/circle/usb/usbhiddevice.h || patch -p1 < /repo/pi/circle/circle-hid-reenum2.patch
# then the re-enumeration'"'"'s use-after-free (circle-hid-reenum3.patch): CUSBTouchScreenDevice::Poll touched its
# members (a spin lock first) after CUSBHIDDevice::Poll had deleted it, GetReEnumCount moves before each removal,
# and the time the main loop is held is logged. Its own file and marker for the same reason as -reenum2; idempotent
grep -q circle-hid-reenum3 include/circle/usb/usbhiddevice.h || patch -p1 < /repo/pi/circle/circle-hid-reenum3.patch
make -C lib/usb -j$J >/dev/null
# the WLAN driver (the Plan 9 ether4330 port) and wpa_supplicant, once (libwlan.a, libwpa_supplicant.a)
[ -f addon/wlan/libwlan.a ] || make -C addon/wlan -j$J >/dev/null
[ -f addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a ] || make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle -j$J >/dev/null
# Pi 5: the DSI display driver (Circle'"'"'s addon/rp1dsi, unchanged; docs/pi5.md "DSI display"), once
[ "$R" != 5 ] || [ -f addon/rp1dsi/librp1dsi.a ] || make -C addon/rp1dsi -j$J >/dev/null
# Pi 5: the NVMe SSD driver (Circle'"'"'s addon/nvme, unchanged, experimental; docs/STORAGE.md), once
[ "$R" != 5 ] || [ -f addon/nvme/libnvme.a ] || make -C addon/nvme -j$J >/dev/null
/repo/pi/ui/lvgl-in-tree.sh $T			# addon/lvgl/lvgl and liblvgl.a, once per LVGL commit
make -C /repo/pi/engine CIRCLEHOME=$T PI5_UART=$P5U -j$J
# The kernel (text + data + bss from 0x80000) must fit Circle'"'"'s KERNEL_MAX_SIZE: past it the bss runs into the
# stacks and the Pi dies before the serial console (2026-10-23: 8.43 MB against 8 MB, a black screen, no log).
EL=/repo/pi/engine/kernel8-rpi4.elf; [ "$R" = 5 ] && EL=/repo/pi/engine/kernel_2712.elf
MAX=$(sed -n "s/.*-DKERNEL_MAX_SIZE=\(0x[0-9a-fA-F]*\).*/\1/p" $T/Config.mk)
END=0x$(aarch64-none-elf-nm $EL | awk "/ _end\$/ {print \$1}")
USED=$((END - 0x80000))
echo "kernel size: $((USED / 1024)) KB of $((MAX / 1024)) KB (KERNEL_MAX_SIZE)"
[ "$USED" -le "$((MAX))" ] || { echo "kernel too big: $USED > $((MAX)) bytes (raise --kernel-max-size, then rebuild the tree)"; exit 1; }
'
ls -l $KIMG
