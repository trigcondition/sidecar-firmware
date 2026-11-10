#!/bin/bash
# core/tests/bench/ufx_bench.sh [P1 P2 P3 P4] - the user effects' render cost (plugins/ufx_phaser, plugins/ufx_bbd):
# built with the Pi's flags in the rytm-circle image (Debian's aarch64 gcc), timed on this Mac's core (for ranking), then
# 200 calls traced (trace_steps.c, after 2000 to settle) and costed by a72_model.py: instructions and the Cortex-A72's
# model cycles a 12-frame block. The model counts throughput only (no dependency chains): a floor, not a clock.
set -euo pipefail
cd "$(dirname "$0")/../.."
R=$(git rev-parse --show-toplevel)
P="${*:-64 127 100 127}"
docker run --rm -v "$R:$R" -w "$PWD" rytm-circle sh -c "
  set -e; mkdir -p build/ufx-bench
  gcc -O2 -static -o build/ufx-bench/trace_steps tests/bench/trace_steps.c
  for e in ufx_phaser/phaser ufx_bbd/bbd; do
    n=\$(basename \$e)
    gcc -mcpu=cortex-a72 -O2 -fno-math-errno -ffixed-x18 -static -DRMP_NATIVE -I$R/sdk -I$R/sdk/rmp \
      -DUFX_SRC='\"$R/plugins/'\$e'.c\"' -o build/ufx-bench/\$n tests/bench/ufx_bench.c
    build/ufx-bench/\$n 200000 $P
    a=\$(nm build/ufx-bench/\$n | awk '/ ufx_call\$/ {print \$1}')
    build/ufx-bench/trace_steps build/ufx-bench/\$n.u32 \$a 2000 200 build/ufx-bench/\$n 2200 $P > /dev/null
  done"
for n in phaser bbd; do
  echo "$n (a72_model.py, 200 calls):"
  python3 tests/bench/a72_model.py "build/ufx-bench/$n" "build/ufx-bench/$n.u32" 200 -n 4 2>/dev/null ||
    docker run --rm -v "$R:$R" -w "$PWD" rytm-circle python3 tests/bench/a72_model.py "build/ufx-bench/$n" "build/ufx-bench/$n.u32" 200 -n 4
done
