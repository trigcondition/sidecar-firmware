#!/bin/bash
# Make the Pi 5's ready-to-copy SD card folder, out/pi5-sd/ (docs/pi5.md): copy its contents to the root of a FAT32
# (MBR) card. Needs RASPPI=5 pi/engine/build.sh first (pi/engine/kernel_2712.img).
#   ./make-sd-pi5.sh [KERNEL]     KERNEL default ../engine/kernel_2712.img
# The Pi 5 keeps its boot firmware in its EEPROM: the card needs no start*.elf/fixup*.dat/armstub, only the device trees
# (from Circle's boot/, the firmware commit its boot/Makefile pins), the D0 overlay, config.txt and the kernel. Wi-Fi's
# chip firmware (firmware/, the Pi 5's file names, RPi-Distro/firmware-nonfree at the commit Circle's
# addon/wlan/firmware/Makefile pins) is downloaded once into build/fw5/ and only used with Wi-Fi on. No cmdline.txt
# (the build's defaults), no Wi-Fi credentials.
set -euo pipefail
cd "$(dirname "$0")"
K=${1:-../engine/kernel_2712.img}
BUILD=${CIRCLE_BUILD:-$PWD/build}
[ -f "$K" ] || { echo "missing $K: run RASPPI=5 pi/engine/build.sh first"; exit 1; }
B=$BUILD/circle/boot
for f in bcm2712-rpi-5-b.dtb bcm2712d0-rpi-5-b.dtb bcm2712d0.dtbo LICENCE.broadcom COPYING.linux; do
  [ -s "$B/$f" ] || { echo "missing $B/$f (Circle's boot/: make -C build/circle/boot firmware)"; exit 1; }
done
WFW=$(grep -E '^FIRMWARE \?=' "$BUILD/circle/addon/wlan/firmware/Makefile" | awk '{print $3}')
WURL=https://github.com/RPi-Distro/firmware-nonfree/raw/$WFW/debian/config/brcm80211
mkdir -p "$BUILD/fw5"
N=brcmfmac43455-sdio.raspberrypi,5-model-b
for x in bin:cypress/cyfmac43455-sdio-minimal.bin txt:brcm/brcmfmac43455-sdio.txt clm_blob:cypress/cyfmac43455-sdio.clm_blob; do
  [ -s "$BUILD/fw5/$N.${x%%:*}" ] || { echo "downloading $N.${x%%:*}"; curl -fsSL -o "$BUILD/fw5/$N.${x%%:*}" "$WURL/${x#*:}"; }
done

O=out/pi5-sd
rm -rf "$O"; mkdir -p "$O/overlays" "$O/firmware"
cp "$B"/{bcm2712-rpi-5-b.dtb,bcm2712d0-rpi-5-b.dtb,LICENCE.broadcom,COPYING.linux} "$O/"
cp "$B/bcm2712d0.dtbo" "$O/overlays/"
cp "$BUILD/fw5/$N".* "$O/firmware/"
cp "$K" "$O/kernel_2712.img"
cat > "$O/config.txt" <<'CFG'
# Sidecar Pi engine, Raspberry Pi 5 (docs/pi5.md)
arm_64bit=1
kernel_address=0x80000
initial_turbo=0
enable_uart=1
disable_splash=1
boot_delay=0
os_check=0

[pi5]
kernel=kernel_2712.img
CFG
echo "kernel: $(basename "$K") $(wc -c < "$K" | tr -d ' ') bytes, built $(date -r "$K" '+%Y-%m-%d %H:%M')" > "$O/KERNEL.txt"
( cd "$O" && find . -type f | sort | xargs ls -l | awk '{print $5, $9}' )
echo "SD folder: $PWD/$O ($(du -sh "$O" | cut -f1))"
