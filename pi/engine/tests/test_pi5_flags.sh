#!/bin/bash
# pi/engine/tests/test_pi5_flags.sh - the Pi 5 release kernel has the USB-C gadget (computer audio) and not the engine
# test mode (docs/pi5.md "USB device side", "Engine test mode"), checked without building a kernel:
#   1. build.sh's flags (BUILD_FLAGS=1: printed, nothing written): RASPPI=5 is PI5_GADGET=1 PI5_TESTMODE=0 with the
#      GADGET default sidecar; PI5_TESTMODE=1 needs PI5_GADGET=1.
#   2. the Makefile's flags (make print-flags, a stand-in CIRCLEHOME): the release build defines neither
#      SIDECAR_NO_GADGET nor SIDECAR_PI5_TESTMODE, links the gadget and not card_guard.o, no --wrap; the test build all of it.
#   3. the sources (pi5_flags_src.py): with the release build's macros no line that the compiler sees names the test
#      mode (testsecs, /usbtest.log, TestLog, the card gate), and the gadget is started; with SIDECAR_PI5_TESTMODE it is there.
#   4. usb=mirror on the Pi 5 too (docs/usb-mirror.md; in testing for 0.4.2): build.sh puts the Pi 4's gadget patches on
#      the pi5usb tree, mirror, softconnect and stray included, in the Pi 4 order after circle-pi5-gadget.patch (one list,
#      PP, whose files are put back clean when its sum changes); with a Circle checkout at hand (CIRCLE_BUILD or
#      pi/circle/build), the whole Pi 5 stack applied to a copy of it defines CIRCLE_GADGET_MIRROR and
#      CIRCLE_GADGET_SOFTCONNECT in the gadget header (also seen by the preprocessor with RASPPI=5), has what later patches
#      build on (StateDeferred, m_bForwarded, OnControlAborted: circle-gadget-stray.patch), and the gadget's sources
#      compile (clang++ -fsyntax-only for aarch64, Circle's flags): a patch that applies but does not compile is caught here.
set -euo pipefail
cd "$(dirname "$0")"
ENG=$PWD/..
fail() { echo "test_pi5_flags: FAIL: $*"; exit 1; }
CLEAN=(-u RASPPI -u PI5_GADGET -u PI5_TESTMODE -u PI5_UART -u GADGET -u STOR_EXFAT)   # the caller's own build flags left out
flags() { env "${CLEAN[@]}" "$@" BUILD_FLAGS=1 "$ENG/build.sh"; }
has() { grep -qxF "$2" <<<"$1" || fail "$3: expected '$2' in: $(tr '\n' ' ' <<<"$1")"; }

F=$(flags RASPPI=5)
has "$F" "PI5_GADGET=1" "RASPPI=5"; has "$F" "PI5_TESTMODE=0" "RASPPI=5"; has "$F" "GADGET_DEFAULT=sidecar" "RASPPI=5"
has "$F" "TREE=circle-engine-pi5usb" "RASPPI=5"; has "$F" "KIMG=kernel_2712.img" "RASPPI=5"
F=$(flags RASPPI=5 PI5_TESTMODE=1)
has "$F" "PI5_GADGET=1" "PI5_TESTMODE=1"; has "$F" "PI5_TESTMODE=1" "PI5_TESTMODE=1"
has "$F" "STAMP=RASPPI=5 PI5_UART=0 PI5_GADGET=1 PI5_TESTMODE=1" "PI5_TESTMODE=1"
F=$(flags RASPPI=5 PI5_GADGET=0)
has "$F" "PI5_GADGET=0" "PI5_GADGET=0"; has "$F" "TREE=circle-engine-pi5" "PI5_GADGET=0"
F=$(flags RASPPI=4)
has "$F" "GADGET_DEFAULT=sidecar" "RASPPI=4"; has "$F" "PI5_TESTMODE=0" "RASPPI=4"
! flags RASPPI=5 PI5_GADGET=0 PI5_TESTMODE=1 >/dev/null 2>&1 || fail "PI5_TESTMODE=1 without the gadget was accepted"
! flags RASPPI=4 PI5_TESTMODE=1 >/dev/null 2>&1 || fail "PI5_TESTMODE=1 on the Pi 4 was accepted"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
printf 'RASPPI = 5\n' > "$T/Config.mk"; : > "$T/Rules.mk"
mf() { env "${CLEAN[@]}" make -s -C "$ENG" CIRCLEHOME="$T" "$@" print-flags; }
M=$(mf PI5_GADGET=1)
grep -q -- "-DSIDECAR_NO_GADGET\|-DSIDECAR_PI5_TESTMODE" <<<"$M" && fail "release Makefile flags: $M"
grep -q "^OBJS=.*sidecar_gadget.o" <<<"$M" || fail "release build without the gadget: $M"
grep -q "card_guard.o\|--wrap" <<<"$M" && fail "release build with the card gate: $M"
M=$(mf PI5_GADGET=1 PI5_TESTMODE=1)
grep -q -- "-DSIDECAR_PI5_TESTMODE" <<<"$M" || fail "test build without SIDECAR_PI5_TESTMODE: $M"
grep -q -- "-DSIDECAR_NO_GADGET" <<<"$M" && fail "test build without the gadget: $M"
grep -q "^OBJS=.*card_guard.o" <<<"$M" && grep -q "^LDFLAGS=.*--wrap=f_open" <<<"$M" || fail "test build without the card gate: $M"
M=$(mf PI5_GADGET=0)
grep -q -- "-DSIDECAR_NO_GADGET" <<<"$M" || fail "PI5_GADGET=0 with the gadget: $M"
! mf PI5_GADGET=0 PI5_TESTMODE=1 >/dev/null 2>&1 || fail "the Makefile took PI5_TESTMODE=1 without the gadget"

python3 pi5_flags_src.py "$ENG"
bash -n "$ENG/build.sh" || fail "build.sh does not parse (an apostrophe in a comment inside its quoted Docker block?)"

P5=$(grep -E '^ *(P=[^;]*; )?PP=' "$ENG/build.sh" | grep -o 'circle-[a-z0-9-]*\.patch' | tr '\n' ' ')
[ "$P5" = "circle-gadget.patch circle-gadget-wake.patch circle-pi5-gadget.patch circle-gadget-reset.patch circle-gadget-mirror.patch circle-gadget-softconnect.patch circle-gadget-stray.patch " ] \
  || fail "the Pi 5 gadget patches in build.sh: $P5"
grep -q 'cat $PP | sed -n' "$ENG/build.sh" || fail "build.sh does not restore the Pi 5 tree's patched files from the clean tree"
P4=$(grep -o 'patch -p1 < /repo/pi/circle/circle-gadget[a-z-]*\.patch' "$ENG/build.sh" | sed 's,.*/,,' | tr '\n' ' ')
[ "$P4" = "circle-gadget.patch circle-gadget-wake.patch circle-gadget-reset.patch circle-gadget-mirror.patch circle-gadget-softconnect.patch circle-gadget-stray.patch " ] \
  || fail "the Pi 4 gadget patches in build.sh: $P4"
C=${CIRCLE_BUILD:-$ENG/../circle/build}/circle
if [ -f "$C/include/circle/usb/gadget/dwusbgadget.h" ]; then
  cp -R "$C/include" "$C/lib" "$T/"
  cp "$ENG/../circle/gadget_wake.h" "$T/include/circle/usb/gadget/gadgetwake.h"	# as build.sh
  cp "$ENG/../circle/gadget_flush.h" "$T/include/circle/usb/gadget/gadgetflush.h"
  # the gadget's sources as the Pi 5 build compiles them (Circle's Rules.mk: AArch64, no stdlib), syntax only
  SYN=0
  if command -v clang++ >/dev/null && echo 'int x;' | clang++ --target=aarch64-none-elf -x c++ -fsyntax-only - 2>/dev/null; then SYN=1
  else echo "test_pi5_flags: no clang++ for aarch64: the gadget sources not compiled (applied and grepped only)"; fi
  syn() {
    [ "$SYN" = 1 ] || return 0
    for f in dwusbgadget.cpp dwusbgadgetendpoint.cpp dwusbgadgetendpoint0.cpp; do
      (cd "$T" && clang++ --target=aarch64-none-elf -fsyntax-only -std=c++17 -ffreestanding -fno-exceptions -fno-rtti -nostdinc \
        -nostdinc++ -fsigned-char -I include -D__circle__=500000 -DRASPPI=5 -DAARCH=64 -DSTDLIB_SUPPORT=0 \
        -D__VCCOREVER__=0x04000000 "lib/usb/gadget/$f") || fail "$1: lib/usb/gadget/$f does not compile"
    done
  }
  for f in $P5; do (cd "$T" && patch -s -p1 < "$ENG/../circle/$f") || fail "$f does not apply to the Pi 5 stack"; done
  for m in CIRCLE_GADGET_MIRROR CIRCLE_GADGET_SOFTCONNECT; do
    grep -q "#define $m" "$T/include/circle/usb/gadget/dwusbgadget.h" || fail "the Pi 5 stack without $m"
  done
  grep -q StateDeferred "$T/include/circle/usb/gadget/dwusbgadgetendpoint0.h" || fail "the Pi 5 stack: no StateDeferred"
  grep -q m_bForwarded "$T/include/circle/usb/gadget/dwusbgadgetendpoint0.h" || fail "the Pi 5 stack: no m_bForwarded"
  grep -q OnControlAborted "$T/include/circle/usb/gadget/dwusbgadget.h" || fail "the Pi 5 stack: no OnControlAborted"
  if [ "$SYN" = 1 ]; then	# what the engine sees: CIRCLE_GADGET_MIRROR defined for RASPPI=5 (sidecar_gadget.h, ob_usb.cpp)
    printf '#include <circle/usb/gadget/dwusbgadget.h>\n#ifndef CIRCLE_GADGET_MIRROR\n#error no CIRCLE_GADGET_MIRROR\n#endif\n' \
      > "$T/mirror_check.cpp"
    (cd "$T" && clang++ --target=aarch64-none-elf -fsyntax-only -std=c++17 -ffreestanding -fno-exceptions -fno-rtti -nostdinc \
      -nostdinc++ -fsigned-char -I include -D__circle__=500000 -DRASPPI=5 -DAARCH=64 -DSTDLIB_SUPPORT=0 \
      -D__VCCOREVER__=0x04000000 mirror_check.cpp) || fail "RASPPI=5: CIRCLE_GADGET_MIRROR not defined"
  fi
  syn "the Pi 5 stack"
else
  echo "test_pi5_flags: no Circle checkout at $C: the Pi 5 patch stack not applied (the order checked)"
fi
echo "test_pi5_flags: ok"
