#!/bin/bash
# tools/run_portable_tests.sh - the tests that need no hardware, no firmware and no cross toolchain; the CI's "test"
# check runs exactly this (Linux and macOS):
#   - pi/engine/tests: the Pi engine's portable parts (ASan/UBSan; the ones that need the core's Mac library or a
#     Circle checkout only where those are there);
#   - teensy/libraries/RytmPlugins/extras: the metadata, pool, presets, protocol and resolve unit tests (the arenas,
#     loader, loader_a64, manifest and slots tests read built .rmp packages, so need an ARM toolchain: not here);
#   - teensy/libraries/RytmHost/extras/ob_block_test.cpp: the Overbridge block codec;
#   - tools/tests/test_rmp_api.c: the plugin ABI's layout;
#   - web/ and pi/engine/tests/*.test.mjs: the web UIs' logic (Node 20 or newer).
# The engine core's tests (make -C core engines && make -C core test) build on macOS only for now (Apple's JIT API in
# core/host/um_host.cpp); the CI runs them in a separate macOS job.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
CXX=${CXX:-c++}
CC=${CC:-cc}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# The Pi tests that need more than a compiler run when it is there: test_bundleweb and test_poolweb link the core's
# Mac library (macOS, after make -C core engines), the FatFs tests copy Circle's addon/fatfs (pi/circle/build.sh's
# checkout); without it they are named as skipped.
PT=$R/pi/engine/tests
skip=()
{ [ "$(uname)" = Darwin ] && [ -f "$R/core/build/libengines.a" ]; } || skip+=(test_bundleweb test_poolweb)
[ -f "$R/pi/circle/build/circle/addon/fatfs/ff.c" ] || skip+=(test_stor_fatfs test_stor_fatfs_exfat test_stor_fatfs_lfn3)
tests=()
for f in "$PT"/test_*.cpp test_stor_fatfs_exfat test_stor_fatfs_lfn3; do
  t=$(basename "$f" .cpp)
  [ "$t" = test_content ] && continue
  [[ " ${skip[*]} " == *" $t "* ]] || tests+=("build/$t")
done
[ ${#skip[@]} -eq 0 ] || echo "pi/engine/tests: skipped here: ${skip[*]}"
make -C "$PT" CXX="$CXX" TESTS="${tests[*]}"

P=$R/teensy/libraries/RytmPlugins
run() { local name=$1; shift; (cd "$P/extras" && "$CXX" -std=c++17 -O1 -Wall -I"$P/src" "$@" -o "$T/$name" && "$T/$name"); echo "$name: PASS"; }
run meta_test                "$P/extras/meta_test.cpp" "$P/src/rmp_meta.cpp" "$P/src/rmp_manifest.cpp" "$P/src/rmp_loader.cpp"
run pool_test                "$P/extras/pool_test.cpp" "$P/src/rmp_pool.cpp"
run presets_test             "$P/extras/presets_test.cpp" "$P/src/rmp_presets.cpp"
run proto_test               "$P/extras/proto_test.cpp" "$P/src/rmp_proto.cpp" "$P/src/rmp_manifest.cpp" "$P/src/rmp_loader.cpp"
run resolve_test             "$P/extras/resolve_test.cpp" "$P/src/rmp_resolve.cpp" "$P/src/rmp_slots.cpp" "$P/src/rmp_proto.cpp" "$P/src/rmp_manifest.cpp" "$P/src/rmp_loader.cpp"
run ob_block_test   -I"$R/teensy/libraries/RytmHost" "$R/teensy/libraries/RytmHost/extras/ob_block_test.cpp"

"$CC" -std=c11 -Wall -Wextra -o "$T/test_rmp_api" "$R/tools/tests/test_rmp_api.c"
"$T/test_rmp_api"; echo "test_rmp_api: PASS"

(cd "$R" && node --test web/*.test.mjs pi/engine/tests/*.test.mjs)
echo "ALL PORTABLE TESTS PASS"
