# Pi 4 + Circle spike (bare metal)

Plan: `docs/pi-circle-spike.md`. This directory holds step 1 (toolchain, boot, serial) and the step 2 USB driver skeleton.

## Build
Docker (arm64 Debian + Arm GNU Toolchain 13.3.rel1 `aarch64-none-elf`), Circle tag `Step51.1.1` (RASPPI=4, AArch64):

    ./build.sh          # first run ~3 min; clones Circle to build/ (git-ignored, ~115 MB)
    ./build.sh spike    # only spike1/spike2 again

Outputs in `out/`: `spike1-rpi4.img`, `spike1-qemu.img` (Circle configured `--qemu`), `spike2-rpi4.img`,
`sample-01-gpiosimple.img`, `sample-34-sounddevices.img`, `sample-29-miniorgan.img`.
`spike1/` = banner, boot timestamp, one line per second on the serial console. `spike2/` = see below.

## QEMU
    ./qemu.sh [seconds]     # qemu-system-aarch64 -M raspi4b -kernel out/spike1-qemu.img -serial stdio

## SD card
    ./make-sd.sh [spike1|spike2|sample-34-sounddevices|...]
Writes `out/sd/` (copy to a FAT32 card) and `out/sd.img` (64 MB FAT32, `dd` it): `start4.elf`, `fixup4.dat`,
`bcm2711-rpi-4-b.dtb` (Raspberry Pi firmware, pinned commit from Circle's boot/Makefile), Circle's
`armstub8-rpi4.bin`, `config.txt` (`arm_64bit=1`, `enable_uart=1`), `kernel8-rpi4.img`.
Serial: 3.3 V USB-TTL, GND pin 6, adapter RX to pin 8 (GPIO14), adapter TX to pin 10 (GPIO15), 115200 8N1, no VCC.

## Verified (no hardware yet)
- Circle libs, 3 samples, spike1 and spike2 build clean with the Arm toolchain (no `PREFIX64` tricks needed).
- spike1 boots on QEMU 11.1.2 `raspi4b` (2 GB, Cortex-A72 model) and prints the banner and 1 Hz ticks over UART0.
  QEMU logs unimplemented GIC-400 priority registers (`gic_dist_writeb: Bad offset`) with `-d guest_errors`: harmless.
  The clock values there are QEMU's, so boot time can't be measured in it.
- QEMU has no xHCI/VL805 on raspi4b, no I2S/HDMI audio and no real UART timing: step 2 and the audio/boot-time
  measurements need the real Pi. The SD files are built but not tried on a real card.
- spike2 only compiles and links (`RytmObDeviceCreate` resolved from the weak factory hook).

## spike2 (step 2 skeleton)
`rytmobdevice.cpp` is a `CUSBFunction` for 1935:0b48. It needs a 6-line hook in Circle's `CUSBDeviceFactory`
(`circle-usbdevicefactory.patch`, applied by `build.sh` to `build/circle`, the weak symbol `RytmObDeviceCreate`; `circle-usbdevicefactory2.patch`, applied by `pi/engine/build.sh`, widens it to every Elektron id, the driver asks the device profiles).
It logs the configuration descriptor, sends SET_INTERFACE alt 3 for interfaces 1 and 2, creates endpoints
0x83 (IN) and 0x03 (OUT), and keeps 2 URBs of 368 B in flight on each, counting blocks, frame-counter gaps
(+7), bad headers and errors, and sending a 1 kHz tone on MAIN L/R. Block format: `teensy/libraries/RytmHost/ob_block.h`.

Circle limits found by reading its source (details in the file header):
- `CXHCIEndpoint::TransferAsync` allows only 2 URBs per endpoint, one Normal TRB with IOC each: one interrupt
  per 125 us packet and a 250 us deep pipeline. Likely the first thing to break; fix = multi-TRB queueing in Circle.
- Interval: for HS, xHCI interval = bInterval-1, so bInterval 1 gives every microframe: fine. MaxBurst is fixed 0
  (OK for 368 B). The Pi <= 3 DWC host clamps interrupt endpoints to 20 ms: Pi 4 xHCI only.
- Alternate settings > 0 are skipped by `CUSBDevice::Initialize`; the vendor match runs only for the device's first
  function, so the driver parses the raw configuration descriptor itself. The MIDI interface (class 1/3) is
  claimed by Circle's own `CUSBMIDIHostDevice`.
- No halt recovery (CLEAR_FEATURE) hook is exposed on `CUSBEndpoint`; `CUSBRequest` is allocated per packet.

## Open questions for the Pi
1. Do the descriptors match `ob_block.h` (iface 1/2 alt 3, 0x83/0x03, 368 B, bInterval 1) and does SET_INTERFACE succeed?
2. Does the 2-URB pipeline sustain 8000 packets/s per direction without gaps (check the `gaps` counter over 10 min)?
3. Does the Rytm need the OUT stream running before it sends (RytmHost starts both together)?
4. Does the xHCI interrupt-completion load leave room for audio on core 0 (multicore, `ARM_ALLOW_MULTI_CORE`)?
5. Boot to first log line / first sound (target < 3 s); I2S vs HDMI audio path.
6. Do the Rytm's MIDI SysEx (mods) work through Circle's `CUSBMIDIHostDevice` at the same time?
