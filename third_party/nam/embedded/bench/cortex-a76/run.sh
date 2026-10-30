#!/bin/sh
# Build and run a76_bench for both models, pinned to one core.
#
#   ./run.sh                 # core 4 (an A76 on RK3588), block 48
#   BLOCK=16 ./run.sh        # block 16: the engines are built for it
#   CORE=0 BLOCK=128 ./run.sh
#
# BLOCK sets both the benchmark's block size and the engines' compile-time
# settings: NAM_A2*_BLOCK_HINT (tile shapes) and NAM_A2*_MAX_BUFFER_SIZE.
# Measurements in the README: GCC 13, -O3 -ffast-math -mcpu=cortex-a76.
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
CORE=${CORE:-4}
BLOCK=${BLOCK:-48}
MAXBUF=${MAXBUF:-$BLOCK}
CFLAGS=${CFLAGS:-"-O3 -ffast-math -mcpu=cortex-a76"}
R=../..
E=$R/engines/cortex-a76
DEFS="-DNAM_A2LITE_BLOCK_HINT=$BLOCK -DNAM_A2FULL_BLOCK_HINT=$BLOCK"
DEFS="$DEFS -DNAM_A2LITE_MAX_BUFFER_SIZE=$MAXBUF -DNAM_A2FULL_MAX_BUFFER_SIZE=$MAXBUF"
mkdir -p build
$CC $CFLAGS $DEFS -I. -I$R/example -I$E/a2lite -I$E/a2full -I$R/namb a76_bench.c $R/example/a2_engine.c \
    $E/a2lite/nam_a2lite.c $E/a2full/nam_a2full.c $R/namb/namb_reader.c -lm -o build/a76_bench
for m in a2lite a2full; do
  echo "== $m (core $CORE, block $BLOCK)"
  taskset -c "$CORE" build/a76_bench $R/weights/$m.namb "$BLOCK"
done
