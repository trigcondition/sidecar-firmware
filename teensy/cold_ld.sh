#!/bin/bash
# teensy/cold_ld.sh DIR FQBN SKETCH - writes DIR/<core's linker script>_cold.ld and prints the
# build.flags.ld value to build with it (pass as --build-property "build.flags.ld=<it>"):
# Teensyduino's own linker script with LittleFS's core (littlefs/lfs.c, lfs_util.c) and SdFat placed
# in program flash (.text.code, right after *(.flashmem*)) instead of ITCM. build.flags.ld and
# build.core.path come from arduino-cli's --show-properties for the same FQBN (options included), so
# the flags are the installed core's with only the -T argument swapped. Generated on every build: the
# core's file is PJRC's and nothing derived from it is committed.
# The plugin arena (rmp_arena.cpp, section .dtcm_arena: 32 KB, 8 KB aligned) goes first in DTCM, at
# 0x20000000, as the first input of .data: as the first 8 KB-aligned object of .bss it sat right after
# .data, and the padding up to the next 8 KB step was lost to the stack (6464 B in the host build; the
# serial build was 320 B away from losing 8 KB). In .data, not a NOLOAD section of its own: teensy_size
# counts only .data and .bss as RAM1 variables, so a NOLOAD arena would overstate "free for local
# variables" by 32 KB (run_sketch.sh's 40 KB check reads it). The cost: 32 KB of zeros in the flash image,
# copied at boot. .endpoint_queue's 4 KB alignment holds at 0x20008000.
# Why these may run from flash: both run only from loop() - LittleFS at slot load and install time
# (its flash program/erase routines stay in RAM: the core's eeprom.c and LittleFS.cpp are not moved),
# SdFat for card reads between requests (an 8 KB read is ~0.75 ms of SDIO transfer either way).
# Measured 2026-10-10 (host build with plugins): ITCM code 207624 -> 165944 B with this script alone
# (the intermediate, before the per-message parsers went to FLASHMEM), 157752 B with both; the rarely run
# code below: host 158600 -> 128904 B (5 -> 4 blocks of 32 KB: 32 KB more for the stack), serial
# 144408 -> 118216 B (map and nm checked: every listed function at 0x6xxxxxxx).
set -eo pipefail
USAGE="usage: cold_ld.sh DIR FQBN SKETCH"
DIR=${1:?$USAGE}
FQBN=${2:?$USAGE}
SKETCH=${3:?$USAGE}
CLI=${CLI:-"/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"}
die() { echo "cold_ld.sh: $*" >&2; exit 1; }
PROPS=$("$CLI" compile --fqbn "$FQBN" --show-properties "$SKETCH" 2>/dev/null) || die "arduino-cli --show-properties failed for $FQBN"
FLAGS=$(printf '%s\n' "$PROPS" | sed -n 's/^build\.flags\.ld=//p')
CORE=$(printf '%s\n' "$PROPS" | sed -n 's/^build\.core\.path=//p')
[ -n "$FLAGS" ] || die "no build.flags.ld in the properties of $FQBN"
[ -n "$CORE" ] || die "no build.core.path in the properties of $FQBN"
[ "$(printf '%s' "$FLAGS" | grep -o '"-T[^"]*"' | wc -l | tr -d ' ')" = 1 ] || die "build.flags.ld has no single \"-T...\" argument: $FLAGS"
SRC=$(printf '%s' "$FLAGS" | sed -E 's/.*"-T([^"]+)".*/\1/')
case "$SRC" in "$CORE"/*) ;; *) die "the linker script $SRC is not in build.core.path $CORE" ;; esac
[ -f "$SRC" ] || die "no linker script $SRC"
mkdir -p "$DIR"
OUT="$DIR/$(basename "$SRC" .ld)_cold.ld"
# Rarely run code to flash too (input sections, first match wins: .text.code comes before .text.itcm's
# *(.text*)). A direct-call graph from every *_isr, the PIT and guard handlers and the USB host's transfer
# callbacks reaches none of the list below (USB enumeration does run from the USB host interrupt, so it
# stays in ITCM). Flash code does run from interrupts all the same: the USB host interrupt reaches
# RytmOverbridge::control/advance/step/start_stream (FLASHMEM, RytmHost.cpp) by vtable dispatch, which a
# direct-call graph does not see, and through the queue_* long-branch veneers in flash. That is safe: a
# flash erase or program masks interrupts for its whole length and runs from ITCM (eeprom.c), so no
# interrupt fetches from flash while it is busy. The list:
#   printf core (sniprintf/vsniprintf: log lines), malloc/realloc/free (LittleFS handles, File objects),
#   the EH unwinder (referenced through setjmp, never run; its own .ARM.exidx/.ARM.extab entries go with
#   it: a PREL31 from the ITCM table cannot reach flash, and nothing unwinds through it; strcmp's assembly
#   has one too), strstr/strcmp, USBHost_t36's mass-storage
#   driver (only File/MSCFile vtables keep it), static constructors (once at boot), Print and Time
#   (Serial.print, LittleFS timestamps), rmp_store (loop only, like LittleFS itself), and the sampler's
#   playback modes and PSRAM allocator (SampleModes, SampleRam: they run only with PSRAM fitted - from
#   flash then, at a CPU cost not measured here). Read-only data to flash: CrashReport's text (printed
#   once at boot, from setup()) - .rodata is in DTCM otherwise (.data), where it shortens the stack.
#   1021 (host build: ITCM headroom 1832 B): LittleFS's drivers for external SPI / FRAM / QSPI chips (their
#   read/prog/erase/wait and flexspi2_ip_read: kept by LittleFS.cpp's vtables, the Sidecar mounts only
#   LittleFS_Program, whose routines stay in RAM; the QSPI chip is FlexSPI2, not the program flash),
#   SPIClass::begin/end (setup, a mount), the DX7 patch unpacker (plaits::fm::Patch::Unpack: a bank load,
#   loop only), and the core's boot-only pwm_init/flexpwm_init/usb_init_serialnumber. The Rytm's notes in
#   (MIDIDeviceBase::read and the rest of the USB host MIDI path) stay in ITCM.
#   1018 (mini-diet; stereo build: RAM1 locals 34816 B at the floor): every string literal (.rodata*.str1*: log
#   texts, names, the console's argument strings; 8.8 KB of DTCM, all of it merged into one block there) and
#   SdFat's and littlefs's read-only tables to flash. Reading them from flash is only slower, never wrong, except
#   while the flash is programmed or erased: that code (the core's eeprom.c, which LittleFS.cpp's LittleFS_Program
#   and rmp_store call with interrupts off) runs from ITCM and must not read flash, so those two objects keep
#   theirs in DTCM (EXCLUDE_FILE). One line per entry below (split on newlines: EXCLUDE_FILE has spaces).
#   ITCM (stereo build headroom 4504 B): USBHost_t36's enumeration, pipe setup and teardown, its begin() and
#   memory pools, MIDIDeviceBase::init/claim, IntervalTimer::beginCycles/end (rmp_guard's timer, set up once at
#   boot): about 4.3 KB, run once per device connected or at boot (from the USB host interrupt for enumeration:
#   flash code runs from it already, as above). The note-in path (MIDIDeviceBase::read, rx_data, rx_callback,
#   USBHost::isr, queue_*Transfer, followup_*) and the transfer callbacks stay in ITCM.
#   Also: libm's large-argument
#   reduction for sinf/cosf (__kernel_rem_pio2f, 1.7 KB of ITCM, and its two_over_pi table, 0.8 KB of DTCM: only
#   for |x| above about 200: slower there, the same results).
COLD='*/libraries/LittleFS/littlefs/*.o(.text*)
*/libraries/SdFat/*.o(.text*)
*libc.a:libc_a-svfiprintf.o(.text*)
*libc.a:libc_a-mallocr.o(.text*)
*libc.a:libc_a-reallocr.o(.text*)
*libc.a:libc_a-freer.o(.text*)
*libc.a:libc_a-callocr.o(.text*)
*libgcc.a:unwind-arm.o(.text* .ARM.exidx* .ARM.extab*)
*libgcc.a:pr-support.o(.text* .ARM.exidx* .ARM.extab*)
*libgcc.a:libunwind.o(.text* .ARM.exidx* .ARM.extab*)
*libc.a:libc_a-strstr.o(.text*)
*libc.a:libc_a-strcmp.o(.text* .ARM.exidx*)
*/libraries/USBHost_t36/MassStorageDriver.cpp.o(.text*)
*(.text.startup*)
*core.a:Print.cpp.o(.text*)
*core.a:Time.cpp.o(.text*)
*/libraries/RytmPlugins/rmp_store.cpp.o(.text*)
*(.text._ZN4rytm11SampleModes*)
*(.text._ZN4rytm9SampleRam*)
*core.a:CrashReport.cpp.o(.rodata*)
*(.text._ZN17LittleFS_SPIFlash*)
*(.text._ZN16LittleFS_SPIFram*)
*(.text._ZN18LittleFS_QSPIFlash*)
*(.text._ZL16flexspi2_ip_readmmPvm)
*(.text._ZN8SPIClass5beginEv)
*(.text._ZN8SPIClass3endEv)
*(.text._ZN6plaits2fm5Patch6UnpackEPKh)
*core.a:*(.text.pwm_init)
*core.a:*(.text.flexpwm_init)
*core.a:*(.text.usb_init_serialnumber)
*(EXCLUDE_FILE(*eeprom.c.o *LittleFS.cpp.o) .rodata*.str1*)
*/libraries/SdFat/*.o(.rodata*)
*/libraries/LittleFS/littlefs/*.o(.rodata*)
*(.text._ZN7USBHost5beginEv)
*(.text._ZN7USBHost10new_DeviceEmmm)
*(.text._ZN7USBHost11delete_PipeEP11Pipe_struct)
*(.text._ZN7USBHost13claim_driversEP13Device_struct)
*(.text._ZN7USBHost17disconnect_DeviceEP13Device_struct)
*(.text._ZN7USBHost17enumeration_errorEPK15Transfer_struct)
*(.text._ZN7USBHost19enumeration_receiveEPK15Transfer_struct)
*(.text._ZN7USBHost20enumeration_transmitEP13Device_struct)
*(.text._ZN7USBHost23driver_ready_for_deviceEP9USBDriver)
*(.text._ZN7USBHost27add_qh_to_periodic_scheduleEP11Pipe_struct)
*(.text._ZN7USBHost32init_Device_Pipe_Transfer_memoryEv)
*(.text._ZN7USBHost33allocate_interrupt_pipe_bandwidthEP11Pipe_structmm)
*(.text._ZN7USBHost36convertStringDescriptorToASCIIStringEhP13Device_structPK15Transfer_struct)
*(.text._ZN7USBHost8new_PipeEP13Device_structmmmmm)
*(.text._ZN7USBHost16contribute_PipesEP11Pipe_structm)
*(.text._ZN7USBHost20contribute_TransfersEP15Transfer_structm)
*(.text._ZN7USBHost25contribute_String_BuffersEP8strbuf_tm)
*(.text._ZN14MIDIDeviceBase4initEv)
*(.text._ZN14MIDIDeviceBase5claimEP13Device_structiPKhm)
*(.text._ZN13IntervalTimer11beginCyclesEN6teensy16inplace_functionIFvvELj16ELj8EEEm)
*(.text._ZN13IntervalTimer3endEv)
*(.rodata.two_over_pi)
*libm.a:libm_a-kf_rem_pio2.o(.text* .rodata* .ARM.exidx*)'
awk -v cold="$(printf '%s' "$COLD" | tr '\n' '|')" '{ print }
     /^\t\.data : \{$/ { print "\t\tKEEP(*(.dtcm_arena))"; d++ }
     /^\t\t\*\(\.flashmem\*\)$/ { k = split(cold, c, "|"); for (i = 1; i <= k; i++) print "\t\t" c[i]; n++ }
     END { if (n != 1) exit 1; if (d != 1) exit 2 }' "$SRC" > "$OUT" || die "no single '*(.flashmem*)' line or no single '.data : {' line in $SRC"
printf '%s\n' "${FLAGS/\"-T$SRC\"/\"-T$OUT\"}"
