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
# RASPPI (default 4): 5 builds for the Pi 5 (docs/pi5.md) -> kernel_2712.img, with the USB-C gadget (PI5_GADGET below,
# default 1 on the Pi 5). PI5_UART (default 0): the Pi 5's console UART, 0 = GPIO14/15 (pins 8/10, the Pi 4's
# wiring), 10 = the 3-pin debug connector. The objects here are one board's at a time: switching RASPPI (or PI5_UART,
# PI5_GADGET) removes them first, so the next build is a full one.
# PI5_GADGET (RASPPI=5 only, default 1): 1 the USB-C gadget compiled in (docs/pi5.md "USB device side"), in a Circle tree
# of its own, pi/circle/build/circle-engine-pi5usb (circle-gadget.patch, circle-gadget-wake.patch, circle-pi5-gadget.patch,
# circle-gadget-reset.patch, then the usb=mirror patches as on the Pi 4: circle-gadget-mirror.patch,
# circle-gadget-softconnect.patch, circle-gadget-stray.patch, docs/usb-mirror.md; Overbridge passthrough on the Pi 5 is in
# testing for 0.4.2; a changed patch is put in again). The first run also stops at the hostap clone, as for any
# new tree: run it twice. On at run time as on the Pi 4 (GADGET default sidecar; gadget=0 in cmdline.txt turns it off).
# 0: the gadget compiled out, in pi/circle/build/circle-engine-pi5 (the host-side patches only), as before 0.4.0.
# PI5_TESTMODE=1 (needs PI5_GADGET=1, default 0): the engine test mode for tryboots of Raspberry Pi OS (docs/pi5.md "Engine
# test mode": testsecs=, its watchdog and reboot, /usbtest.log, testcard=1's card gate). Never on a release card:
# make-sd-dual.sh refuses a kernel with "testsecs=" in it. Same Circle tree as PI5_GADGET=1.
# BUILD_FLAGS=1: print the flags this build would use (tree, kernel, PI5_*, GADGET default, the make line) and stop,
# before anything is written (pi/engine/tests/test_pi5_flags.sh).
# STOR_EXFAT (default 0; docs/STORAGE.md): 1 builds against a Circle tree of its own (<tree>-exfat, made from
# pi/circle/build/circle as the others) whose FatFs has FF_FS_EXFAT 1 and FF_LBA64 1 (exFAT and GPT drives) - every
# library that uses FatFs (the wlan addon, wpa_supplicant) compiled with it, as their FIL / FATFS layouts change. Tested
# on the Mac only (make -C pi/engine/tests: test_stor_fatfs_exfat, fatfs-mac); not on a Pi yet.
set -euo pipefail
cd "$(dirname "$0")"
REPO=$(git rev-parse --show-toplevel)
R=${RASPPI:-4}
case $R in 4) TREE=circle-engine; KIMG=kernel8-rpi4.img;; 5) TREE=circle-engine-pi5; KIMG=kernel_2712.img;;
  *) echo "RASPPI must be 4 or 5"; exit 1;; esac
P5U=${PI5_UART:-0}
P5G=${PI5_GADGET:-$( [ "$R" = 5 ] && echo 1 || echo 0)}
case $P5G in 0) ;; 1) [ "$R" = 5 ] || { echo "PI5_GADGET needs RASPPI=5"; exit 1; }; TREE=circle-engine-pi5usb;;
  *) echo "PI5_GADGET must be 0 or 1"; exit 1;; esac
P5T=${PI5_TESTMODE:-0}
case $P5T in 0) ;; 1) [ "$P5G" = 1 ] || { echo "PI5_TESTMODE needs PI5_GADGET=1 (and RASPPI=5)"; exit 1; };;
  *) echo "PI5_TESTMODE must be 0 or 1"; exit 1;; esac
EXF=${STOR_EXFAT:-0}
case $EXF in 0) ;; 1) TREE=$TREE-exfat;; *) echo "STOR_EXFAT must be 0 or 1"; exit 1;; esac
STAMP="RASPPI=$R PI5_UART=$P5U$( [ "$EXF" = 1 ] && echo ' STOR_EXFAT=1' || true)$( [ "$P5G" = 1 ] && echo " PI5_GADGET=1 PI5_TESTMODE=$P5T" || true)"
# (PI5_TESTMODE is in the stamp even at 0: objects from a PI5_GADGET=1 build made before the split carry the test mode)
GDEF=${GADGET:-sidecar}		# the USB-C gadget when cmdline.txt has no gadget=: sidecar on both boards
if [ "${BUILD_FLAGS:-0}" = 1 ]; then
  printf 'TREE=%s\nKIMG=%s\nPI5_GADGET=%s\nPI5_TESTMODE=%s\nGADGET_DEFAULT=%s\nSTAMP=%s\nMAKE=make -C /repo/pi/engine CIRCLEHOME=<tree> PI5_UART=%s PI5_GADGET=%s PI5_TESTMODE=%s\n' \
    "$TREE" "$KIMG" "$P5G" "$P5T" "$GDEF" "$STAMP" "$P5U" "$P5G" "$P5T"
  exit 0
fi
"$REPO/core/engines/setup_engines.sh" "$PWD/engines" >/dev/null
"$REPO/tools/gen_version.sh"   # core/version.h: VERSION, the commit (docs/versioning.md); on the host, the container has no git
if [ "$(cat hosted/.board 2>/dev/null || echo 'RASPPI=4 PI5_UART=0')" != "$STAMP" ]; then
  echo "board changed to $STAMP: removing the other board's objects"
  rm -rf hosted; rm -f ./*.o ./*.d ../ui/*.o ../ui/*.d
fi
mkdir -p hosted
echo "$STAMP" > hosted/.board
NETH=$(printf '#define NET_DEFAULT_IP\t\t"%s"\n#define NET_DEFAULT_NETMASK\t"%s"\n#define NET_DEFAULT_EMUHOST\t"%s"\n#define GADGET_DEFAULT\t\t"%s"\n' \
  "${PI_IP:-192.168.77.2}" "${PI_NETMASK:-255.255.255.0}" "${EMUHOST:-192.168.77.1:7301}" "$GDEF")
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
docker run --rm -v "$REPO":/repo -v "$BUILD":/repo/pi/circle/build -w /repo/pi/engine -e MODE="${1:-all}" -e R=$R -e TREE=$TREE -e P5U=$P5U -e P5G=$P5G -e P5T=$P5T -e EXF=$EXF rytm-circle bash -euo pipefail -c '
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
# USB-A MIDI out without waiting (circle-midi-async.patch: one OUT URB in flight per device, midiout_q.h and kernel.cpp s AsampMidiSend); idempotent.
# Before the gadget library: its CUSBMIDIDevice comes from the same header, and a first build would allocate the smaller class
grep -q circle-midi-async $T/include/circle/usb/usbmidi.h || (cd $T && patch -p1 < /repo/pi/circle/circle-midi-async.patch)
# the USB-C gadget (pi/circle/circle-gadget.patch: IRQ stats, the MIDI device accessor, isochronous endpoints;
# idempotent, also on a tree made before it), then its library (up to date: nothing to do)
cd $T
if [ "$P5G" = 1 ]; then	# Pi 5, PI5_GADGET=1: all the patches, again when one changed (the patched files back from the clean tree first)
  # usb=mirror (docs/usb-mirror.md) on the Pi 5 too, in testing for 0.4.2: the same DWC2 core and driver as the Pi 4 with
  # circle-pi5-gadget.patch on top, so circle-gadget-mirror.patch, -softconnect.patch and -stray.patch go on unchanged
  # after circle-gadget-reset.patch, in the Pi 4 order, and CIRCLE_GADGET_MIRROR is defined here. A tree patched before
  # (0.4.1 without them, or an older version of one) has a different sum: every patched file back clean, then all of PP
  P=/repo/pi/circle; PP="$P/circle-gadget.patch $P/circle-gadget-wake.patch $P/circle-pi5-gadget.patch $P/circle-gadget-reset.patch"
  PP="$PP $P/circle-gadget-mirror.patch $P/circle-gadget-softconnect.patch $P/circle-gadget-stray.patch"; S=$(cat $PP $P/gadget_wake.h $P/gadget_flush.h | md5sum)
  if [ "$(cat .pi5-gadget-sum 2>/dev/null)" != "$S" ]; then
    for f in $(cat $PP | sed -n "s,^+++ b/,,p" | sort -u); do cp /repo/pi/circle/build/circle/$f $f; done
    cp $P/gadget_wake.h include/circle/usb/gadget/gadgetwake.h
    cp $P/gadget_flush.h include/circle/usb/gadget/gadgetflush.h
    for f in $PP; do patch -p1 < $f; done
    echo "$S" > .pi5-gadget-sum
  fi
  make -C lib/usb/gadget -j$J >/dev/null
elif [ "$R" = 4 ]; then	# the Pi 4 (a Pi 5 with PI5_GADGET=0: no gadget, docs/pi5.md)
  grep -q GetIRQStats include/circle/usb/gadget/dwusbgadget.h || patch -p1 < /repo/pi/circle/circle-gadget.patch
  # then the bus resume (circle-gadget-wake.patch: its own file and marker, as circle-hid-reenum2.patch; idempotent)
  cmp -s /repo/pi/circle/gadget_wake.h include/circle/usb/gadget/gadgetwake.h || cp /repo/pi/circle/gadget_wake.h include/circle/usb/gadget/gadgetwake.h
  grep -q circle-gadget-wake lib/usb/gadget/dwusbgadget.cpp || patch -p1 < /repo/pi/circle/circle-gadget-wake.patch
  # then the reset at IRQ level without 1 ms waits (circle-gadget-reset.patch, gadget_flush.h; idempotent)
  cmp -s /repo/pi/circle/gadget_flush.h include/circle/usb/gadget/gadgetflush.h || cp /repo/pi/circle/gadget_flush.h include/circle/usb/gadget/gadgetflush.h
  grep -q circle-gadget-reset lib/usb/gadget/dwusbgadget.cpp || patch -p1 < /repo/pi/circle/circle-gadget-reset.patch
  # then the two gadget changes of usb=mirror (pi/circle/circle-gadget-mirror.patch, docs/usb-mirror.md): an IN and an OUT EP of
  # one number (0x83 + 0x03), and a class or vendor request answered later (forwarded to the box); a gadget that uses
  # neither runs as before. Its own file and marker, as -reenum2; idempotent
  grep -q circle-gadget-mirror lib/usb/gadget/dwusbgadget.cpp || patch -p1 < /repo/pi/circle/circle-gadget-mirror.patch
  # then leaving the bus and coming back as a replug (pi/circle/circle-gadget-softconnect.patch): usb=mirror switches
  # between the box and the Sidecar device when the box comes and goes; its own file and marker; idempotent
  grep -q circle-gadget-softconnect lib/usb/gadget/dwusbgadget.cpp || patch -p1 < /repo/pi/circle/circle-gadget-softconnect.patch
  # then a SETUP that comes while EP0 waits for a deferred answer (nothing armed) is read back from where the core
  # wrote it instead of asserted, and a completion with no transfer armed is counted and ignored; nothing else of
  # the EP0 or endpoint flow of Circle changes (pi/circle/circle-gadget-stray.patch: the Pi 4 crashed so at a live switch,
  # 2026-11-06; tests/test_gadget_ep0.cpp); its own marker; idempotent
  # (a tree patched with an earlier version of it - the one of 0.4.1 rc5, which broke the data endpoints - is refused: the marker
  # would keep it; build from a fresh Circle copy)
  if grep -q circle-gadget-stray lib/usb/gadget/dwusbgadgetendpoint.cpp && ! grep -q FindStraySetup lib/usb/gadget/dwusbgadgetendpoint0.cpp; then
    echo "circle-gadget-stray.patch: this Circle tree has an earlier version (0.4.1 rc5): build from a fresh Circle copy"; exit 1
  fi
  grep -q circle-gadget-stray lib/usb/gadget/dwusbgadgetendpoint.cpp || patch -p1 < /repo/pi/circle/circle-gadget-stray.patch
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
# then the header names case-insensitive (pi/circle/circle-http-case.patch: Content-Type and Content-Length; Chrome sends
# "content-type" for a fetch() with a Headers object, and the form was then not read: /cmd refused every paired change;
# after circle-http-auth.patch, idempotent)
grep -q circle-http-case lib/net/httpdaemon.cpp || patch -p1 < /repo/pi/circle/circle-http-case.patch
# then the WebSocket upgrade (pi/circle/circle-http-ws.patch: the Upgrade / Sec-WebSocket-* headers and the
# UpgradeConnection hook that hands the socket to web_ws.cpp, docs/web-ws.md; after circle-http-case.patch, idempotent).
# web.cpp overrides UpgradeConnection, so a tree without this patch does not build.
grep -q circle-http-ws include/circle/net/httpdaemon.h || patch -p1 < /repo/pi/circle/circle-http-ws.patch
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
# then any Elektron device reaches the Overbridge driver, which asks the device profiles (circle-usbdevicefactory2.patch, not an edit
# to the spike2 patch: the same reason as -reenum2; a box with no audio row is 0 and Circle goes on to its interfaces); idempotent
grep -q circle-usbdevicefactory2 lib/usb/usbdevicefactory.cpp || patch -p1 < /repo/pi/circle/circle-usbdevicefactory2.patch
# a device that drops off USB and comes back before the change is handled (the Rytm rebooting after an OS update) is
# enumerated again (pi/circle/circle-replug.patch: the hub and xHCI root port; idempotent)
grep -q circle-replug lib/usb/usbstandardhub.cpp || patch -p1 < /repo/pi/circle/circle-replug.patch
# then a failed enumeration tried again (pi/circle/circle-address-retry.patch: Cannot set device address after a pull and
# replug left the port dead until a reboot; twice more, each with a port reset, a fresh slot and 200 ms, only after a fast
# failure; a hub port given up on is disabled and skipped by rescans until a replug; hub and xHCI root port; after
# circle-replug, idempotent). A tree patched by its first build (ba8a58d, no m_bPortGivenUp) cannot take it: stop and say so
if ! grep -q circle-address-retry lib/usb/usbstandardhub.cpp; then
  patch -p1 < /repo/pi/circle/circle-address-retry.patch
elif ! grep -q m_bPortGivenUp lib/usb/usbstandardhub.cpp; then
  echo "$T: an unreleased circle-address-retry (ba8a58d) is in this Circle tree: remove the tree and build again (a fresh copy)"; exit 1
fi
# the queued URBs of a halted xHCI endpoint dropped after its reset (pi/circle/circle-xhci-dropurbs.patch: CObUsb::PollHalt, the
# Overbridge OUT after a transfer error); idempotent
grep -q circle-xhci-dropurbs include/circle/usb/xhciendpoint.h || patch -p1 < /repo/pi/circle/circle-xhci-dropurbs.patch
# then the USB MIDI bulk OUT of the Rytm reset when a transfer error halted it (pi/circle/circle-midi-reset.patch: CUSBMIDIDevice
# ResetOutHalted, after circle-midi-async.patch and the dropurbs one; CObUsbTransport::Poll); idempotent
grep -q circle-midi-reset include/circle/usb/usbmidi.h || patch -p1 < /repo/pi/circle/circle-midi-reset.patch
# then v2 (pi/circle/circle-midi-reset2.patch: CLEAR_FEATURE(ENDPOINT_HALT) to the Rytm before the reset, the state read after it,
# OutHalted). A tree patched by the unreleased 8f5c3ab build of circle-midi-reset (ENDPOINT_HALT already in usbmidihost.cpp, no v2
# marker) cannot take it: stop and say so. Idempotent
if ! grep -q circle-midi-reset2 include/circle/usb/usbmidi.h; then
  if grep -q ENDPOINT_HALT lib/usb/usbmidihost.cpp; then echo "$T: an unreleased circle-midi-reset (8f5c3ab) is in this Circle tree: remove the tree and build again (a fresh copy)"; exit 1; fi
  patch -p1 < /repo/pi/circle/circle-midi-reset2.patch
fi
# the hub'"'"'s status-change request is armed again after an error (pi/circle/circle-hub-rearm.patch: Circle dropped it on
# any xHCI error, and the hub reported no port change until a reboot - a Rytm restarting after an OS update was never
# seen again; the xHCI endpoint is reset and the request re-armed from a timer, nothing is sent to the hub; after
# circle-replug, idempotent)
grep -q circle-hub-rearm lib/usb/usbstandardhub.cpp || patch -p1 < /repo/pi/circle/circle-hub-rearm.patch
# a hub that is deleted drops the port-status events it queued, and its recovery timer is cancelled with IRQs off
# (pi/circle/circle-hub-rearm2.patch: the recovery of an unplugged hub could otherwise leave an event for freed memory;
# after circle-hub-rearm, idempotent)
grep -q circle-hub-rearm2 lib/usb/usbhostcontroller.cpp || patch -p1 < /repo/pi/circle/circle-hub-rearm2.patch
make -C lib/usb -j$J >/dev/null
# the WLAN driver (the Plan 9 ether4330 port) and wpa_supplicant, once (libwlan.a, libwpa_supplicant.a)
[ -f addon/wlan/libwlan.a ] || make -C addon/wlan -j$J >/dev/null
[ -f addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a ] || make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle -j$J >/dev/null
# Pi 5: the DSI display driver (Circle'"'"'s addon/rp1dsi; docs/pi5.md "DSI display"), once. Its one change: the frame
# buffer'"'"'s pointer and pitch in the header (pi/circle/circle-rp1dsi-fb.patch, inline, for the rotating blit of a Touch
# Display 2: no object changes, so a built library stands; idempotent, its own file and marker), then the Touch Display 2
# fixes (pi/circle/circle-rp1dsi-td2.patch: objects change, so a tree patched here rebuilds the library; the 7" keeps its flags)
[ "$R" != 5 ] || grep -q circle-rp1dsi-fb addon/rp1dsi/rpitouchscreen.h || patch -p1 < /repo/pi/circle/circle-rp1dsi-fb.patch
[ "$R" != 5 ] || grep -q circle-rp1dsi-td2 addon/rp1dsi/rpitouchscreen2.cpp || { patch -p1 < /repo/pi/circle/circle-rp1dsi-td2.patch && rm -f addon/rp1dsi/librp1dsi.a addon/rp1dsi/*.o; }
[ "$R" != 5 ] || [ -f addon/rp1dsi/librp1dsi.a ] || make -C addon/rp1dsi -j$J >/dev/null
# Pi 5: the NVMe SSD driver (Circle'"'"'s addon/nvme, unchanged, experimental; docs/STORAGE.md), once
[ "$R" != 5 ] || [ -f addon/nvme/libnvme.a ] || make -C addon/nvme -j$J >/dev/null
/repo/pi/ui/lvgl-in-tree.sh $T			# addon/lvgl/lvgl and liblvgl.a, once per LVGL commit
make -C /repo/pi/engine CIRCLEHOME=$T PI5_UART=$P5U PI5_GADGET=$P5G PI5_TESTMODE=$P5T -j$J
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
