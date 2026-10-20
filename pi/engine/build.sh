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
set -euo pipefail
cd "$(dirname "$0")"
REPO=$(git rev-parse --show-toplevel)
"$REPO/core/engines/setup_engines.sh" "$PWD/engines" >/dev/null
"$REPO/tools/gen_version.sh"   # core/version.h: VERSION, the commit (docs/versioning.md); on the host, the container has no git
R=${RASPPI:-4}
case $R in 4) TREE=circle-engine; KIMG=kernel8-rpi4.img;; 5) TREE=circle-engine-pi5; KIMG=kernel_2712.img;;
  *) echo "RASPPI must be 4 or 5"; exit 1;; esac
P5U=${PI5_UART:-0}
STAMP="RASPPI=$R PI5_UART=$P5U"
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
docker image inspect rytm-circle >/dev/null 2>&1 || docker build -t rytm-circle "$REPO/pi/circle"
BUILD=${CIRCLE_BUILD:-$REPO/pi/circle/build}  # as flash.sh: a worktree may point this at the main checkout's build/
"$REPO/pi/ui/fetch-lvgl.sh" "$BUILD"		# LVGL for the display (docs/pi-display.md), once
# Wi-Fi (docs/research/pi-wifi.md): Circle's addon/wlan needs its hostap submodule (rsta2/hostap, BSD), fetched once on
# the host (the container has no git credentials); the tree is a plain copy, so it is cloned into the tree directly
HOSTAP=$BUILD/$TREE/addon/wlan/hostap
if [ -d "$BUILD/$TREE" ] && [ ! -f "$HOSTAP/wpa_supplicant/Makefile.circle" ]; then
  rm -rf "$HOSTAP" && git clone -q https://github.com/rsta2/hostap.git "$HOSTAP" && git -C "$HOSTAP" checkout -q 966cab4a3bc958e7ff304a46d91a6172a1bfecaf
fi
docker run --rm -v "$REPO":/repo -v "$BUILD":/repo/pi/circle/build -w /repo/pi/engine -e MODE="${1:-all}" -e R=$R -e TREE=$TREE -e P5U=$P5U rytm-circle bash -euo pipefail -c '
T=/repo/pi/circle/build/$TREE
J=$(nproc)
if { [ "$MODE" = all ] || [ ! -d $T ]; } && [ ! -f $T/.engine-ready ]; then
  [ -d /repo/pi/circle/build/circle ] || { echo "run pi/circle/build.sh first (the Circle checkout)"; exit 1; }
  rm -rf $T && cp -a /repo/pi/circle/build/circle $T
  cd $T
  grep -q RytmObDeviceCreate lib/usb/usbdevicefactory.cpp || patch -p1 < /repo/pi/circle/spike2/circle-usbdevicefactory.patch
  grep -q XHCI_EP_MAX_URBS include/circle/usb/xhciendpoint.h || patch -p1 < /repo/pi/circle/spike2/circle-xhci-urbqueue.patch
  ./makeall --nosample clean >/dev/null 2>&1 || true
  ./configure -f -r $R -p aarch64-none-elf- --multicore --kernel-max-size 8 \
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
# mDNS for sidecar.local (pi/circle/circle-mdns.patch: every question, the QU bit, AAAA answered with an NSEC)
grep -q circle-mdns.patch lib/net/mdnsdaemon.cpp || patch -p1 < /repo/pi/circle/circle-mdns.patch
# the HTTP daemon keeps Host, Origin and Referer (pi/circle/circle-http-origin.patch: for the same-origin check in web.cpp of
# the Wi-Fi settings POSTs; idempotent)
grep -q circle-http-origin include/circle/net/httpdaemon.h || patch -p1 < /repo/pi/circle/circle-http-origin.patch
make -C lib/net -j$J >/dev/null
# the HID endpoint recovery and diagnostics (pi/circle/circle-hid.patch: a halted xHCI interrupt endpoint is reset
# and resubmitted from the main loop; counters for the display log line; idempotent), then lib/usb
grep -q GetHIDStats include/circle/usb/usbhiddevice.h || patch -p1 < /repo/pi/circle/circle-hid.patch
make -C lib/usb -j$J >/dev/null
# the WLAN driver (the Plan 9 ether4330 port) and wpa_supplicant, once (libwlan.a, libwpa_supplicant.a)
[ -f addon/wlan/libwlan.a ] || make -C addon/wlan -j$J >/dev/null
[ -f addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a ] || make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle -j$J >/dev/null
# Pi 5: the DSI display driver (Circle'"'"'s addon/rp1dsi, unchanged; docs/pi5.md "DSI display"), once
[ "$R" != 5 ] || [ -f addon/rp1dsi/librp1dsi.a ] || make -C addon/rp1dsi -j$J >/dev/null
/repo/pi/ui/lvgl-in-tree.sh $T			# addon/lvgl/lvgl and liblvgl.a, once per LVGL commit
make -C /repo/pi/engine CIRCLEHOME=$T PI5_UART=$P5U -j$J
'
ls -l $KIMG
