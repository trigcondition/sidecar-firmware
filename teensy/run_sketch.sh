#!/bin/bash
# RMP_EXTRA_FLAGS="-DRYTM_PLUGINS=0" ./run_sketch.sh voiceloop builds without the machine plugins;
# RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1" ./run_sketch.sh voiceloop is the USB-host build (with plugins).
# RMP_CORE=uac2 ./run_sketch.sh uac2_probe builds with the patched core copy (setup_uac2_core.sh), USB type
# USB_SERIAL_UAC2 (Serial + USB Audio Class 2); the installed core is not touched.
# RMP_NO_UPLOAD=1 ./run_sketch.sh voiceloop compiles and checks only.
# Compile, upload and show the serial output of a sketch in this folder:
#   teensy/run_sketch.sh plaits_bench [SECS]
# Uses the Arduino IDE's arduino-cli and Teensyduino (teensy:avr), -O2.
# The overrides RMP_ALLOW_ITCM_GROWTH, RMP_ALLOW_LOW_LOCALS and RMP_ALLOW_NO_IMAGE_CHECK act only when set to
# exactly 1 (0, empty or anything else keeps the check fatal).
set -eo pipefail
D=$(cd "$(dirname "$0")" && pwd)
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$D/setup_plaits_lib.sh" --if-stale   # remade when missing or when fm_machine.h / the user_data.h shim changed
[ -d "$D/libraries/DaisySP" ] || "$D/setup_daisysp_lib.sh"
"$D/setup_juno_lib.sh" >/dev/null   # always: the voice lives in sim/juno
"$D/setup_sampler_lib.sh" >/dev/null   # always: the playback modes live in sim/sampler
"$D/setup_core_lib.sh" >/dev/null   # always: the engine core lives in core/ (with teensy/platform/)
CORE_PROPS=()
case "${RMP_CORE:-}" in
  "") ;;
  uac2)
    CP=$("$D/setup_uac2_core.sh" | tail -1)
    # the precompiled Arduino.h header must come from the same core (the stock hook reads the installed one)
    CORE_PROPS=(--build-property "build.core.path=$CP" --build-property "build.usbtype=USB_SERIAL_UAC2"
      --build-property "recipe.hooks.sketch.prebuild.1.pattern=\"{teensytools.path}precompile_helper\" \"{build.core.path}\" \"{build.path}\" \"{compiler.path}{build.toolchain}{build.command.g++}\" -x c++-header {build.flags.optimize} {build.flags.common} {build.flags.dep} {build.flags.cpp} {build.flags.cpu} {build.flags.defs} -DARDUINO={runtime.ide.version} -DARDUINO_{build.board} -DF_CPU={build.fcpu} -D{build.usbtype} -DLAYOUT_{build.keylayout} \"-I{build.core.path}\" \"{build.path}/pch/Arduino.h\" -o \"{build.path}/pch/Arduino.h.gch\"") ;;
  *) echo "RMP_CORE=$RMP_CORE: unknown (uac2 or empty)"; exit 1 ;;
esac
# RMP_NO_UPLOAD=1: compile and run the checks only (no board needed, nothing uploaded)
PORT=
[ "$RMP_NO_UPLOAD" = 1 ] || PORT=$("$CLI" board list 2>/dev/null | awk '/teensy/ {print $1; exit}')
[ "$RMP_NO_UPLOAD" = 1 ] || [ -n "$PORT" ] || { echo "no Teensy found (micro-USB cable plugged in? press its button once if it was running other code)"; exit 1; }
# per checkout: every worktree has its own build folder (a shared one let two runs upload each other's build),
# and per RMP_EXTRA_FLAGS (a short hash of them: the serial and host builds do not rebuild over each other)
BP="${TMPDIR:-/tmp}/rytm-build/$(printf '%s' "$D" | shasum | cut -c1-8)/$1-$(printf '%s' "$RMP_EXTRA_FLAGS$RMP_CORE" | shasum | cut -c1-6)"
LOG=$(mktemp)
FQBN=teensy:avr:teensy41:opt=o2std
# LittleFS's core and SdFat in program flash (cold_ld.sh: the core's own build.flags.ld for this FQBN,
# only its -T swapped): the host build with plugins fits RAM1 only so
LDFLAGS=$(CLI="$CLI" "$D/cold_ld.sh" "$BP.ld" "$FQBN" "$D/$1")
"$CLI" compile --fqbn "$FQBN" --libraries "$D/libraries" "${CORE_PROPS[@]}" ${RMP_EXTRA_FLAGS:+--build-property "build.flags.defs=-D__IMXRT1062__ -DTEENSYDUINO=160 $RMP_EXTRA_FLAGS"} \
  --build-property "build.flags.ld=$LDFLAGS" --build-path "$BP" "$D/$1" 2>&1 | tee "$LOG"
# ITCM check: code + padding is a whole number of 32 KB blocks of RAM1; a 7th block takes 32 KB of
# the stack's room, silently at build time (a fault at run time). Fail below 40 KB of locals
# (RMP_ALLOW_LOW_LOCALS=1 overrides; back to 40 KB after the size diet (2026-10-18): the stereo release build has 41568 B; the stack's measured high-water mark is 7.6 KB, docs/plugins.md). Warn under 4 KB of headroom (the next block is close). Fatal when the
# block count is above the one recorded for the build (voiceloop, measured 2026-10-10 on 9870991: serial 4,
# host 4; other sketches and flags: none recorded): RMP_ALLOW_ITCM_GROWTH=1 uploads anyway.
R1=$(grep -E "RAM1: variables" "$LOG" | tail -1 || true)
if [ -n "$R1" ]; then
  CODE=$(echo "$R1" | sed -E 's/.*code:([0-9]+).*/\1/'); PAD=$(echo "$R1" | sed -E 's/.*padding:([0-9]+).*/\1/')
  LOC=$(echo "$R1" | sed -E 's/.*free for local variables:([0-9]+).*/\1/')
  BLK=$(( (CODE + PAD) / 32768 ))
  echo "ITCM $CODE B in $BLK blocks, headroom $PAD B, locals $LOC B"
  WANT=
  if [ "$1" = voiceloop ]; then
    case "$RMP_EXTRA_FLAGS" in
      "") WANT=4 ;;                                    # serial, with plugins
      "-DRYTM_USB_HOST=1") WANT=4 ;;                   # host, with plugins (the final setup)
    esac
  fi
  if [ -n "$WANT" ] && [ "$BLK" -gt "$WANT" ]; then
    if [ "$RMP_ALLOW_ITCM_GROWTH" != 1 ]; then
      echo "ERROR: ITCM takes $BLK blocks, $WANT recorded for this build: 32 KB less stack. Move cold code to FLASHMEM (or record the new count here; RMP_ALLOW_ITCM_GROWTH=1 uploads anyway)."
      rm -f "$LOG"; exit 1
    fi
    echo "WARNING: ITCM takes $BLK blocks, $WANT recorded for this build: 32 KB less stack (RMP_ALLOW_ITCM_GROWTH)."
  fi
  [ "$PAD" -lt 4096 ] && echo "WARNING: ITCM headroom $PAD B, under 4 KB: a little more ITCM code takes another 32 KB block from the stack."
  if [ "$LOC" -lt 40960 ] && [ "$RMP_ALLOW_LOW_LOCALS" != 1 ]; then
    echo "ERROR: only $LOC B left for local variables (stack): ITCM code crossed into another 32 KB block. Move cold code to FLASHMEM or set RMP_ALLOW_LOW_LOCALS=1."
    rm -f "$LOG"; exit 1
  fi
fi
rm -f "$LOG"
# The plugin arena (rmp_arena.cpp) must be first in DTCM (cold_ld.sh's .dtcm_arena at 0x20000000): in
# .bss, 8 KB aligned, it follows .data up to the next 8 KB step - padding taken from the stack, and 8 KB
# more when .data grows past a step. Warn when it is anywhere else (its padding, and how close .data is
# to the next step). A note when the check cannot run in a build with plugins (no nm, no ELF, no symbol).
NM=$(ls ~/Library/Arduino15/packages/teensy/tools/teensy-compile/*/arm/bin/arm-none-eabi-nm 2>/dev/null | tail -1)
AR=; ED=; SYMS=
if [ -n "$NM" ] && [ -f "$BP/$1.ino.elf" ]; then
  SYMS=$("$NM" "$BP/$1.ino.elf")
  AR=$(echo "$SYMS" | awk '$3 == "rmp_arena_mem" { print $1 }')
  ED=$(echo "$SYMS" | awk '$3 == "_edata" { print $1 }')
fi
# The program image must end well below the plugin store (RMP_STORE_BYTES at the top of the core's 0x7C0000 bytes
# of program flash: rmp_store.h's value, or RMP_EXTRA_FLAGS' -DRMP_STORE_BYTES=<bytes> when set there): an image
# that reaches it overwrites the store at upload (and the store refuses to mount). Fatal (no upload) when the image
# ends inside the store or less than 64 KB below it; a warning under 256 KB. Fatal too when the check cannot run in a
# build that mounts the store (no nm, no ELF, no _flashimagelen, RMP_STORE_BYTES not read from rmp_store.h):
# RMP_ALLOW_NO_IMAGE_CHECK=1 uploads anyway.
SBYTES=
if [[ "$RMP_EXTRA_FLAGS" =~ -DRMP_STORE_BYTES=([^[:space:]]+) ]]; then
  SV="${BASH_REMATCH[1]%[uU]}"
  if [[ "$SV" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then SBYTES=$(( SV ))
  else echo "ERROR: RMP_STORE_BYTES=${BASH_REMATCH[1]} in RMP_EXTRA_FLAGS: give it as one integer (bytes) for the image-end check."; exit 1; fi
else
  SMB=$(sed -nE 's/^#define RMP_STORE_BYTES \(([0-9]+)u \* 1024u \* 1024u\).*/\1/p' "$D/libraries/RytmPlugins/src/rmp_store.h")
  [ -n "$SMB" ] && SBYTES=$(( SMB * 1048576 ))
fi
IMG_CHECKED=
if [ -n "$SYMS" ] && [ -n "$SBYTES" ]; then
  FIL=$(echo "$SYMS" | awk '$3 == "_flashimagelen" { print $1 }')
  if [ -n "$FIL" ]; then
    IMG_CHECKED=1
    SBASE=$(( 0x60000000 + 0x7C0000 - SBYTES )); IEND=$(( 0x60000000 + 16#$FIL ))
    printf 'image ends 0x%08x, plugin store (%d KB) at 0x%08x: %d KB between\n' $IEND $(( SBYTES / 1024 )) $SBASE $(( (SBASE - IEND) / 1024 ))
    if [ $(( SBASE - IEND )) -lt 65536 ]; then
      echo "ERROR: the program image ends inside the plugin store or less than 64 KB below it: not uploaded."; exit 1
    fi
    [ $(( SBASE - IEND )) -lt 262144 ] && echo "WARNING: the program image ends within 256 KB of the plugin store."
  fi
fi
if [ -z "$IMG_CHECKED" ] && [[ "$RMP_EXTRA_FLAGS" != *RYTM_PLUGINS=0* ]] && grep -qs "rmp_store" "$D/$1"/*.ino; then
  WHY="no _flashimagelen in the ELF"
  [ -n "$SBYTES" ] || WHY="RMP_STORE_BYTES not read from rmp_store.h"
  [ -f "$BP/$1.ino.elf" ] || WHY="no $BP/$1.ino.elf"
  [ -n "$NM" ] || WHY="no arm-none-eabi-nm"
  if [ "$RMP_ALLOW_NO_IMAGE_CHECK" != 1 ]; then
    echo "ERROR: where the program image ends against the plugin store was not checked ($WHY): not uploaded (RMP_ALLOW_NO_IMAGE_CHECK=1 uploads anyway)."; exit 1
  fi
  echo "WARNING: where the program image ends against the plugin store was not checked ($WHY); uploading (RMP_ALLOW_NO_IMAGE_CHECK)."
fi
if [ -n "$AR" ] && [ -n "$ED" ]; then
  if [ $((16#$AR)) -ne $((0x20000000)) ]; then
    PADB=$(( 16#$AR - 16#$ED )); MARGIN=$(( (8192 - (16#$ED % 8192)) % 8192 ))
    PADT="$PADB B of padding after .data (0x$ED)"
    [ $PADB -lt 0 ] && PADT="before the end of .data (0x$ED), no padding"   # elsewhere in .data, or another region
    echo "WARNING: the plugin arena is at 0x$AR, not first in DTCM (0x20000000): $PADT, $MARGIN B before the next 8 KB step$([ $MARGIN -lt 1024 ] && echo ' - under 1 KB'). Check cold_ld.sh's .dtcm_arena."
  else
    echo "arena 0x$AR (first in DTCM, no padding), .data ends 0x$ED"
  fi
elif [[ "$RMP_EXTRA_FLAGS" != *RYTM_PLUGINS=0* ]] && grep -qs "rmp_" "$D/$1"/*.ino; then
  WHY="no rmp_arena_mem or _edata in the ELF"
  [ -f "$BP/$1.ino.elf" ] || WHY="no $BP/$1.ino.elf"
  [ -n "$NM" ] || WHY="no arm-none-eabi-nm"
  echo "note: the plugin arena's placement was not checked ($WHY)"
fi
[ "$RMP_NO_UPLOAD" = 1 ] && { echo "RMP_NO_UPLOAD=1: built in $BP, not uploaded"; exit 0; }
"$CLI" upload --fqbn teensy:avr:teensy41 -p "$PORT" --input-dir "$BP" "$D/$1"
cd "$D/.." && uv run python teensy/serial_read.py --secs "${2:-60}"
