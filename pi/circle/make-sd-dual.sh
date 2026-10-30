#!/bin/bash
# Make ONE SD card set that boots on a Raspberry Pi 4B and on a Raspberry Pi 5 (docs/pi5.md, "One card for both"):
# OUT/sidecar-sd/ (copy its contents to the root of a FAT32 (MBR) card), OUT/sidecar-pi-sd-<commit>.zip with its
# .sha256, OUT/BUILD-INFO-pi.txt. OUT default pi/circle/out.
#   ./make-sd-dual.sh [K4 SDBOOT K5]
#     K4      the Pi 4 kernel (pi/engine/kernel8-rpi4.img, RASPPI=4 pi/engine/build.sh), wrapped as engine.img
#     SDBOOT  the Pi 4's sdboot (pi/circle/out/sdboot.img, build-sdboot.sh)
#     K5      the Pi 5 kernel (pi/engine/kernel_2712.img, RASPPI=5 pi/engine/build.sh), as kernel_2712.img
# Environment (optional): CIRCLE_BUILD (default pi/circle/build), OUT, CMDLINE (default "sdport=0"), CONTENT (a
# sidecar-content-* folder: JUNO/, DX7/, MULTISAMPLE/ if present, README.txt, NOTICE.txt, copied to the card; README/NOTICE as
# STARTER-CONTENT-*.txt), NO_DISPLAY=1 (both boards' HDMI lines commented out), HDMI_RES=800x480 (default: the
# Waveshare 5"/7" HDMI LCDs) or 1024x600 (a 1024x600 panel such as the GeeekPi EP-0194: the HDMI mode; the engine then
# draws its 1024x600 layouts, docs/pi-display.md "Screen sizes").
# The two kernels must carry the same Sidecar version stamp ("0.1.0 (<commit>)"), not -dirty.
# The Pi 4 boots exactly as the 0.1.0 Pi 4 card did (sdboot.img -> engine.img, its settings, now under [pi4]); the
# Pi 5 boots kernel_2712.img directly ([pi5], no sdboot there yet). The Pi 5 keeps its boot firmware in its EEPROM:
# it reads config.txt, its own .dtb files and overlays/bcm2712d0.dtbo, and ignores start4.elf/fixup4.dat/armstub.
# The Pi 5 also gets HDMI lines of its own ([pi5]: hdmi_force_hotplug=1, the one that matters; framebuffer_width/height
# and the 800x480 mode, ignored by the Pi 5's firmware and kept as harmless).
# DSI displays need no config.txt line on either board (comments in the file; docs/pi-display.md).
# Wi-Fi chip firmware for both boards in firmware/ (the same three RPi-Distro/firmware-nonfree files under each
# board's names, as Circle's addon/wlan/firmware/Makefile), only used with Wi-Fi on. No Wi-Fi credentials.
# Never writes a card.
set -euo pipefail
cd "$(dirname "$0")"
K4=${1:-../engine/kernel8-rpi4.img}
SDB=${2:-out/sdboot.img}
K5=${3:-../engine/kernel_2712.img}
BUILD=${CIRCLE_BUILD:-$PWD/build}
OUT=${OUT:-$PWD/out}
CMDLINE=${CMDLINE:-sdport=0 socmaxtemp=78}   # socmaxtemp: Circle drops the clock to 600 MHz above it (default 60 C: in normal use); 78 is its maximum, the firmware still limits at 80 / 85 C
# A release card has the pairing code ON (docs/pi-engine.md "Pairing code"): a builder's own CMDLINE with paircode= in
# it (a dev card's gate turned off, or a fixed code) must not end up on cards for others. DEV=1 makes such a card on purpose.
case " $CMDLINE " in *" paircode="*) [ -n "${DEV:-}" ] || { echo "CMDLINE has paircode=: not on a release card (DEV=1 for a dev card)"; exit 1; };; esac
HDMI_RES=${HDMI_RES:-800x480}
case "$HDMI_RES" in 800x480|1024x600) ;; *) echo "HDMI_RES: 800x480 or 1024x600"; exit 1;; esac
HW=${HDMI_RES%x*}; HH=${HDMI_RES#*x}
for f in "$K4" "$SDB" "$K5"; do [ -s "$f" ] || { echo "missing $f"; exit 1; }; done
B=$BUILD/circle/boot
P4="start4.elf fixup4.dat bcm2711-rpi-4-b.dtb armstub8-rpi4.bin"
P5="bcm2712-rpi-5-b.dtb bcm2712d0-rpi-5-b.dtb"
for f in $P4 $P5 bcm2712d0.dtbo LICENCE.broadcom COPYING.linux; do
  [ -s "$B/$f" ] || { echo "missing $B/$f (Circle's boot/: make -C build/circle/boot firmware, armstub)"; exit 1; }
done

# the version stamp: the same in both kernels, not dirty; each kernel for its board
stamp() { { strings -a "$1" || true; } | sed -n 's/.*==== rytm-pi engine ==== Sidecar \([^,]*\),.*/\1/p' | sed -n 1p; }
S4=$(stamp "$K4"); S5=$(stamp "$K5")
[ -n "$S4" ] && [ "$S4" = "$S5" ] || { echo "the kernels' stamps differ: Pi 4 '$S4', Pi 5 '$S5'"; exit 1; }
case "$S4" in *dirty*) echo "the kernels are built from a dirty tree ($S4)"; exit 1;; esac
P5S='gadget: none on this board (Pi 5 build)'
! grep -aqF "$P5S" "$K4" || { echo "$K4 is a Pi 5 kernel"; exit 1; }
grep -aqF "$P5S" "$K5" || { echo "$K5 is not a Pi 5 kernel"; exit 1; }
COMMIT=$(echo "$S4" | sed -n 's/.*(\([0-9a-f]*\)).*/\1/p')

# Wi-Fi chip firmware: RPi-Distro/firmware-nonfree at the commit Circle's addon/wlan/firmware/Makefile pins, once
# into build/fw5/ (as make-sd-pi5.sh), with the licence text from that commit's debian/copyright
WFW=$(grep -E '^FIRMWARE \?=' "$BUILD/circle/addon/wlan/firmware/Makefile" | awk '{print $3}')
WURL=https://github.com/RPi-Distro/firmware-nonfree/raw/$WFW
mkdir -p "$BUILD/fw5"
N=brcmfmac43455-sdio.raspberrypi,5-model-b
for x in bin:cypress/cyfmac43455-sdio-minimal.bin txt:brcm/brcmfmac43455-sdio.txt clm_blob:cypress/cyfmac43455-sdio.clm_blob; do
  [ -s "$BUILD/fw5/$N.${x%%:*}" ] || { echo "downloading $N.${x%%:*}"; curl -fsSL -o "$BUILD/fw5/$N.${x%%:*}" "$WURL/debian/config/brcm80211/${x#*:}"; }
done
[ -s "$BUILD/fw5/copyright-$WFW" ] || { echo "downloading debian/copyright"; curl -fsSL -o "$BUILD/fw5/copyright-$WFW" "$WURL/debian/copyright"; }

SD=$OUT/sidecar-sd
rm -rf "$SD"; mkdir -p "$SD/overlays" "$SD/firmware"
for f in $P4 $P5 LICENCE.broadcom COPYING.linux; do cp "$B/$f" "$SD/"; done
cp "$B/bcm2712d0.dtbo" "$SD/overlays/"
for e in bin txt clm_blob; do
  cp "$BUILD/fw5/$N.$e" "$SD/firmware/brcmfmac43455-sdio.$e"       # the Pi 4's names (Circle RASPPI <= 4)
  cp "$BUILD/fw5/$N.$e" "$SD/firmware/$N.$e"                        # the Pi 5's (RASPPI 5)
done
# the Cypress licence (the .bin and .clm_blob), as debian/copyright states it; the .txt (NVRAM) is GPL-2 (COPYING.linux)
awk 'prev == "License: binary-redist-Cypress" && /CYPRESS WIRELESS/ {p=1} p && /^[A-Za-z-]+:/ {exit} p {print} {prev=$0}' \
  "$BUILD/fw5/copyright-$WFW" | sed 's/^ \.$//; s/^ //' > "$SD/firmware/LICENCE.cypress"
grep -q 'CYPRESS WIRELESS CONNECTIVITY DEVICES' "$SD/firmware/LICENCE.cypress" || { echo "no Cypress licence text found"; exit 1; }
cat > "$SD/firmware/NOTICE" <<EOF
Wi-Fi chip firmware (CYW43455), used only when Wi-Fi is turned on. From https://github.com/RPi-Distro/firmware-nonfree
commit $WFW, debian/config/brcm80211/, unmodified, the same three files under the Pi 4's and the Pi 5's names:
  brcmfmac43455-sdio.bin, $N.bin            cypress/cyfmac43455-sdio-minimal.bin  (LICENCE.cypress)
  brcmfmac43455-sdio.clm_blob, $N.clm_blob  cypress/cyfmac43455-sdio.clm_blob     (LICENCE.cypress)
  brcmfmac43455-sdio.txt, $N.txt            brcm/brcmfmac43455-sdio.txt           (GPL-2: ../COPYING.linux;
                                            Copyright 2018 Linaro Ltd, 2018 Raspberry Pi (Trading) Ltd.)
EOF

python3 -c "import sys; sys.path.insert(0, '.'); import sdboot; sys.stdout.buffer.write(sdboot.wrap(open(sys.argv[1], 'rb').read()))" "$K4" > "$SD/engine.img"
cp "$SDB" "$SD/sdboot.img"
cp "$K5" "$SD/kernel_2712.img"
D=""; [ "${NO_DISPLAY:-0}" = 1 ] && D="# "
{
cat <<CFG
# Sidecar | Expander, one card for the Raspberry Pi 4B and the Raspberry Pi 5.
# Each board's firmware reads these lines in order: after [pi4] only a Pi 4 (4B, 400, CM4) uses the lines, after [pi5]
# only a Pi 5 (5, 500, CM5), after [all] both. A later setting wins over an earlier one.
[all]
arm_64bit=1
kernel_address=0x80000
initial_turbo=0

[pi4]
armstub=armstub8-rpi4.bin
max_framebuffers=2
# A DSI display (the official 7" v1 or a compatible) needs no line: the firmware drives it and the engine prefers it
# (docs/pi-display.md). Upside down: lcd_rotate=2 here. Never ignore_lcd=1, disable_touchscreen=1, a vc4-kms-*
# overlay or display_auto_detect=1 (Circle has no KMS: the screen would stay dark).

# The Pi 4's ${HW}x${HH} HDMI display (optional; without a display these lines do no harm). The engine draws the
# frame buffer's size if it is 800x480 or 1024x600, else 800x480 centred. A 1024x600 panel (GeeekPi EP-0194):
# hdmi_cvt=1024 600 60 6 0 0 0 (HDMI_RES=1024x600 when the card is made); a 800x480 one: hdmi_cvt=800 480 60 6 0 0 0.
${D}[pi4]
${D}hdmi_force_hotplug=1
${D}hdmi_group=2
${D}hdmi_mode=87
${D}hdmi_cvt=${HW} ${HH} 60 6 0 0 0
${D}disable_overscan=1

# The Pi 5: its boot firmware is in its EEPROM; it starts kernel_2712.img directly (no sdboot on the Pi 5 yet).
[pi5]
kernel=kernel_2712.img
enable_uart=1
disable_splash=1
boot_delay=0
os_check=0
# A DSI display on CAM/DISP 0 or 1 needs no line either: the engine drives it itself (Circle's addon/rp1dsi) and
# prefers it to HDMI. Upside down: dsi_rotate=180 in cmdline.txt (lcd_rotate does nothing on the Pi 5); dsi=0 there:
# HDMI only. No vc4-kms-* overlay or display_auto_detect=1 (they are for Linux).

# The Pi 5's HDMI display (micro-HDMI 0, next to USB-C, on before power-on; docs/pi5.md "HDMI display"): only
# hdmi_force_hotplug=1 matters (measured). The Pi 5's firmware picks the mode itself (1920x1080 on an 800x480 LCD) and
# the engine scales its 800x480 UI to it (1024x600 when the display's EDID prefers 1024x600, or res=1024x600 in
# cmdline.txt); the other five lines are ignored by the Pi 5 (measured), kept as harmless.
${D}[pi5]
${D}hdmi_force_hotplug=1
${D}framebuffer_width=${HW}
${D}framebuffer_height=${HH}
${D}hdmi_group=2
${D}hdmi_mode=87
${D}hdmi_cvt=${HW} ${HH} 60 6 0 0 0

# The Pi 4: sdboot.img checks and starts engine.img (the block below; sdboot.py disable/enable edits it).
CFG
python3 -c "import sys; sys.path.insert(0, '.'); import sdboot; sys.stdout.write(sdboot.block())"
} > "$SD/config.txt"
echo "$CMDLINE" > "$SD/cmdline.txt"
# each board's settings as its firmware reads config.txt (sections [all]/[pi4]/[pi5], the last value wins); the
# board's kernel, initramfs and armstub must be on the card
python3 - "$SD" <<'PY'
import os, sys
sd = sys.argv[1]
def settings(board):
    on, out = True, {}
    for l in open(os.path.join(sd, "config.txt")):
        l = l.split("#", 1)[0].strip()
        if not l:
            continue
        if l.startswith("["):
            on = l in ("[all]", f"[{board}]")
        elif on:
            k, v = l.split("=", 1) if "=" in l else l.split(" ", 1)
            out[k.strip()] = v.strip()
    return out
for b in ("pi4", "pi5"):
    s = settings(b)
    need = [s.get("kernel", "")] + [s[k].split(" ")[0] for k in ("initramfs", "armstub") if k in s]
    for f in need:
        if not os.path.isfile(os.path.join(sd, f)):
            sys.exit(f"{b}: {f or 'no kernel='} is not on the card")
    print(f"{b} boots:", " ".join(f"{k}={v}" for k, v in s.items()))
PY

if [ -n "${CONTENT:-}" ]; then
  cp -R "$CONTENT/JUNO" "$CONTENT/DX7" "$SD/"
  [ -d "$CONTENT/MULTISAMPLE" ] && cp -R "$CONTENT/MULTISAMPLE" "$SD/"   # MULTISAMPLE's instruments (Pi), if the set has them
  cp "$CONTENT/README.txt" "$SD/STARTER-CONTENT-README.txt"
  cp "$CONTENT/NOTICE.txt" "$SD/STARTER-CONTENT-NOTICE.txt"
fi
find "$SD" -name .DS_Store -delete
[ ! -e "$SD/wifi.txt" ] && [ ! -e "$SD/.rytm" ] && [ ! -e "$SD/sidecar-wifi.txt" ] || { echo "a Wi-Fi file is in the card set"; exit 1; }

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
Z=$OUT/sidecar-pi-sd-$COMMIT.zip
rm -f "$OUT"/sidecar-pi-sd-*.zip "$OUT"/sidecar-pi-sd-*.zip.sha256
python3 - "$SD" "$Z" <<'PY'
import os, sys, zipfile
sd, z = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    for dp, dns, fns in os.walk(sd):
        dns.sort()
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            zi = zipfile.ZipInfo(os.path.relpath(p, sd), date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(zi, open(p, "rb").read())
PY
( cd "$OUT" && echo "$(sha "$Z")  $(basename "$Z")" > "$Z.sha256" )
FWC=$(grep -E '^FIRMWARE \?=' "$B/Makefile" | awk '{print $3}')
cat > "$OUT/BUILD-INFO-pi.txt" <<EOF
Sidecar | Expander, Raspberry Pi 4B and Raspberry Pi 5 SD card (one card for both)
version: Sidecar $S4
Pi 4 kernel sha256: $(sha "$K4") ($(wc -c < "$K4" | tr -d ' ') B)
engine.img sha256: $(sha "$SD/engine.img")
sdboot.img sha256: $(sha "$SD/sdboot.img")
kernel_2712.img (Pi 5) sha256: $(sha "$SD/kernel_2712.img") ($(wc -c < "$SD/kernel_2712.img" | tr -d ' ') B)
firmware: raspberrypi/firmware $FWC (Pi 4 boot files, all .dtb and overlays/bcm2712d0.dtbo)
Wi-Fi firmware: RPi-Distro/firmware-nonfree $WFW
cmdline.txt: $CMDLINE
addresses in the Pi 4 kernel: $(strings -a "$K4" | grep -xE '169\.254\.[0-9]+\.[0-9]+|192\.168\.[0-9]+\.[0-9]+' | sort -u | tr '\n' ' ')
addresses in the Pi 5 kernel: $(strings -a "$K5" | grep -xE '169\.254\.[0-9]+\.[0-9]+|192\.168\.[0-9]+\.[0-9]+' | sort -u | tr '\n' ' ')
EOF
cat "$OUT/BUILD-INFO-pi.txt"
( cd "$SD" && find . -type f -not -path './JUNO/*' -not -path './DX7/*' -not -path './MULTISAMPLE/*' | sort | xargs ls -l | awk '{print $5, $9}' )
echo "card folder: $SD ($(du -sh "$SD" | cut -f1), $(find "$SD" -type f | wc -l | tr -d ' ') files); zip: $Z"
