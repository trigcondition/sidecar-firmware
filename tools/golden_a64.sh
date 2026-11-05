#!/bin/bash
# tools/golden_a64.sh [--record|--dump] - the LFO golden (core/tests/test_mod_golden.cpp, plan-050 Task 1) built with
# aarch64 gcc and the Pi engine's floating-point flags (pi/engine/Makefile HOSTED_FLAGS: -O2 -ffp-contract=fast
# -fno-math-errno -fsigned-char), run in the rytm-circle image (docker run --platform linux/arm64), against its own file
# core/tests/golden/mod/lfo_trace_a64.bin (-DGOLDEN_A64): the two compilers may fuse multiply-adds differently, so each
# matches itself across a change. No argument: compare (prints "test_mod_golden: ok"); --record writes the file;
# --dump prints every pass's group hashes. Needs `make -C core engines` first (the engines' sources in core/build/engines).
# The compiler is the image's native aarch64-linux g++ (Debian 12.2): the kernel's aarch64-none-elf-g++ (13.3) builds
# bare-metal objects that cannot run in Linux. Objects in core/build/a64 (kept between runs, rebuilt when a source or
# a header they include changes).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-}
IMAGE=${GOLDEN_IMAGE:-rytm-circle:latest}
[ -d "$R/core/build/engines/Plaits" ] || { echo "golden_a64: run 'make -C core engines' first" >&2; exit 1; }
B=build/a64
mkdir -p "$R/core/$B"
# the test build's sources and include flags, from core/Makefile
make -s -C "$R/core" print-src > "$R/core/$B/src.mk"
cat > "$R/core/$B/Makefile" <<'EOF'
include build/a64/src.mk
B := build/a64
.DELETE_ON_ERROR:
CXXFLAGS := -std=c++17 -O2 -DCORE_UI_TOOLS=1 -DCORE_POLY=1 -DCORE_MSAMP=1 -w -ffp-contract=fast -fno-math-errno -fsigned-char \
            -include string.h -include stdlib.h -I. -Ihost -I$(RMPSRC) -I../sdk $(ENG_INC) $(ENG_FLAGS) -DGOLDEN_A64 -MMD -MP
CFLAGS := -std=c99 -O2 -ffp-contract=fast -fno-math-errno -fsigned-char -w -MMD -MP $(ENG_INC)
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
EOF
docker run --rm --platform linux/arm64 -v "$R:$R" -w "$R/core" "$IMAGE" bash -c \
  "make -s -k -f $B/Makefile -j\$(nproc) $B/test_mod_golden && $B/test_mod_golden $MODE"
