#!/bin/bash
# Copy DaisySP's physical-modelling parts (Electrosmith, MIT) and the DAISYSP
# voice (sim/daisysp/daisy_engine.h) into libraries/DaisySP as an Arduino
# library, from the pinned checkout that sim/daisysp/build.sh sets up.
# Git-ignored; rerun freely.
set -e
# GNU sed (Linux) wants "sed -i" where BSD sed (macOS) wants "sed -i ''": the scripts are written for the latter
if sed --version >/dev/null 2>&1; then sed() { if [ "${1:-}" = -i ] && [ -z "${2+x}" -o -z "${2:-}" ]; then shift 2; command sed -i "$@"; else command sed "$@"; fi; }; fi
D=$(cd "$(dirname "$0")" && pwd)
"$D/../sim/daisysp/build.sh" >/dev/null   # makes sure the vendor checkout exists
V=$(cd "$D/../sim/daisysp/vendor/DaisySP/Source" && pwd -P)
L=$D/libraries/DaisySP
rm -rf "$L"; mkdir -p "$L/src"
cd "$V"
# daisy_engine.h's own resonator and string (BlockResonator, BlockString) need
# only DaisySP's filters, delay line and DC blocker (DRIP is not a model any more)
for f in Filters/svf.* Filters/onepole.h Utility/dcblock.* Utility/dsp.h Utility/delayline.h; do
  mkdir -p "$L/src/$(dirname "$f")"; cp "$f" "$L/src/$f"
done
# DaisySP's own build puts Utility/ on the include path; Arduino only has src/.
for f in $(grep -rl '#include "dsp.h"' "$L/src" | grep -v '/Utility/'); do
  sed -i '' 's|#include "dsp.h"|#include "Utility/dsp.h"|' "$f"
done
cp "$D/../sim/daisysp/daisy_engine.h" "$L/src/"
cat > "$L/src/DaisySP.h" <<'H'
// Arduino finds a library by a header at the top of src/: include this first.
#pragma once
#include "daisy_engine.h"
H
cat > "$L/library.properties" <<'P'
name=DaisySP
version=0.0.0
author=Electrosmith (DaisySP, MIT); rytm-teensy (daisy_engine.h)
maintainer=rytm-teensy
sentence=DaisySP physical models for the DAISYSP voice
paragraph=
category=Signal Input/Output
url=https://github.com/electro-smith/DaisySP
architectures=*
P
echo "set up $L"
