#!/bin/bash
# tools/golden_a64.sh [--record|--dump] - the LFO golden (core/tests/test_mod_golden.cpp, plan-050 Task 1) built with
# aarch64 gcc and the Pi engine's floating-point flags (pi/engine/Makefile HOSTED_FLAGS: -O2 -ffp-contract=fast
# -fno-math-errno -fsigned-char), run in the rytm-circle image (docker run --platform linux/arm64), against its own file
# core/tests/golden/mod/lfo_trace_a64.bin (-DGOLDEN_A64): the two compilers may fuse multiply-adds differently, so each
# matches itself across a change. No argument: compare (prints "test_mod_golden: ok"); --record writes the file;
# --dump prints every pass's group hashes. Needs `make -C core engines` first (the engines' sources in core/build/engines).
# The compiler is the image's native aarch64-linux g++ (Debian 12.2): the kernel's aarch64-none-elf-g++ (13.3) builds
# bare-metal objects that cannot run in Linux.
# GOLDEN_CPU=a72|a76: the kernel's own flags on top (circle Rules.mk ARCHCPU -mcpu=cortex-a72 / -a76 -mlittle-endian,
# the Pi 4 / the Pi 5; core/engines/engines.mk HOT_O3 and HOT_O3_ENG at -O3), objects in core/build/a64-<cpu>; the same
# file is compared (the LFOs' arithmetic must not depend on the CPU tuning). Objects in core/build/a64 (kept between runs, rebuilt when a source or
# a header they include changes).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-}
IMAGE=${GOLDEN_IMAGE:-rytm-circle:latest}
[ -d "$R/core/build/engines/Plaits" ] || { echo "golden_a64: run 'make -C core engines' first" >&2; exit 1; }
CPU=${GOLDEN_CPU:-}
case "$CPU" in "") B=build/a64; CPUF=;; a72|a76) B=build/a64-$CPU; CPUF="-mcpu=cortex-$CPU -mlittle-endian";;
  *) echo "golden_a64: GOLDEN_CPU is a72 or a76" >&2; exit 1;; esac
mkdir -p "$R/core/$B"
# the test build's sources and include flags, from core/Makefile
make -s -C "$R/core" print-src > "$R/core/$B/src.mk"
printf 'B := %s\nCPUF := %s\n' "$B" "$CPUF" >> "$R/core/$B/src.mk"
cat > "$R/core/$B/Makefile.new" <<'EOF'
include $(dir $(lastword $(MAKEFILE_LIST)))src.mk
.DELETE_ON_ERROR:
CXXFLAGS := $(CPUF) -std=c++17 -O2 -DCORE_UI_TOOLS=1 -DCORE_POLY=1 -DCORE_MSAMP=1 -w -ffp-contract=fast -fno-math-errno -fsigned-char \
            -include string.h -include stdlib.h -I. -Ihost -I$(RMPSRC) -I../sdk $(ENG_INC) $(ENG_FLAGS) -DGOLDEN_A64 -MMD -MP
CFLAGS := $(CPUF) -std=c99 -O2 -ffp-contract=fast -fno-math-errno -fsigned-char -w -MMD -MP $(ENG_INC)
OBJ := $(patsubst %,$(B)/obj/%.o,$(subst ../,_up/,$(CORE_SRC) $(ENG_SRC) $(ENG_C_SRC)))
$(B)/test_mod_golden: tests/test_mod_golden.cpp $(OBJ)
	g++ $(CXXFLAGS) -MF $(B)/test_mod_golden.d -o $@ $^ -lpthread
-include $(OBJ:.o=.d) $(B)/test_mod_golden.d
$(B)/obj/_up/%.cpp.o: ../%.cpp
	@mkdir -p $(dir $@)
	@g++ $(CXXFLAGS) -c -o $@ $<
$(B)/obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@g++ $(CXXFLAGS) -c -o $@ $<
$(B)/obj/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@g++ $(CXXFLAGS) -c -o $@ $<
$(B)/obj/%.c.o: %.c
	@mkdir -p $(dir $@)
	@gcc $(CFLAGS) -c -o $@ $<
# the kernel's -O3 files (engines.mk; the later -O wins), only with GOLDEN_CPU
ifneq ($(CPUF),)
$(patsubst %,$(B)/obj/%.o,$(HOT_O3)) $(patsubst %,$(B)/obj/build/engines/%.o,$(HOT_O3_ENG)): CXXFLAGS += -O3
endif
EOF
# the flags changed (this script edited, or the sources): the objects are stale (-MMD tracks headers, not flags)
if ! cmp -s "$R/core/$B/Makefile.new" "$R/core/$B/Makefile" || ! cmp -s "$R/core/$B/src.mk" "$R/core/$B/src.mk.used"; then
  rm -rf "$R/core/$B/obj" "$R/core/$B/test_mod_golden"
fi
mv "$R/core/$B/Makefile.new" "$R/core/$B/Makefile"
cp "$R/core/$B/src.mk" "$R/core/$B/src.mk.used"
# (a second make, serial, when the first fails: twice the link of a fresh build failed once with no message on Docker
# Desktop's bind mount and passed when run again)
docker run --rm --platform linux/arm64 -v "$R:$R" -w "$R/core" "$IMAGE" bash -c \
  "{ make -s -k -f $B/Makefile -j\$(nproc) $B/test_mod_golden || make -s -f $B/Makefile $B/test_mod_golden; } && $B/test_mod_golden $MODE"
