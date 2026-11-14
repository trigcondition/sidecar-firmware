#!/bin/bash
# Copy the Teensy's Plaits library (Mutable Instruments eurorack 08460a6 + stmlib, MIT, (c) Emilie
# Gillet; made by teensy/setup_plaits_lib.sh, the same curated sources the Teensy builds) into
# spike3/plaits/ for Circle, and undo its Teensy-only bits:
#   * <avr/pgmspace.h> and PROGMEM (resources in flash on the Teensy; plain const data here);
#   * section(".flashmem") on Voice::Init.
# Git-ignored (spike3/.gitignore); rerun freely.
#   ./setup_plaits.sh [PLAITS_LIB]     default: <repo>/teensy/libraries/Plaits, else the main
#                                      checkout's (a worktree has no git-ignored copy)
set -euo pipefail
# GNU sed (Linux) wants "sed -i" where BSD sed (macOS) wants "sed -i ''": the scripts are written for the latter
if sed --version >/dev/null 2>&1; then sed() { if [ "${1:-}" = -i ] && [ -z "${2+x}" -o -z "${2:-}" ]; then shift 2; command sed -i "$@"; else command sed "$@"; fi; }; fi
D=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$D" rev-parse --show-toplevel)
SRC=${1:-$REPO/teensy/libraries/Plaits}
if [ ! -d "$SRC/src/plaits" ]; then
  MAIN=$(git -C "$D" worktree list --porcelain | awk '/^worktree /{print $2; exit}')
  SRC=$MAIN/teensy/libraries/Plaits
fi
[ -d "$SRC/src/plaits" ] || { echo "no Plaits library: run teensy/setup_plaits_lib.sh first"; exit 1; }
L=$D/plaits
rm -rf "$L"; mkdir -p "$L"
cp -R "$SRC/src/plaits" "$SRC/src/stmlib" "$L/"
cp "$SRC/LICENSE" "$L/" 2>/dev/null || true
for f in "$L/plaits/resources.cc" "$L/plaits/dsp/speech/lpc_speech_synth_words.cc"; do
  sed -i '' -e '/^#include <avr\/pgmspace.h>$/d' -e 's/\] PROGMEM = {/] = {/' "$f"
done
sed -i '' 's|^__attribute__((section(".flashmem"))) void Voice::Init|void Voice::Init|' "$L/plaits/dsp/voice.cc"
! grep -rqE 'PROGMEM|pgmspace|flashmem' "$L" || { echo "Teensy-only bits left"; exit 1; }
echo "Plaits for Circle in $L ($(find "$L" -name '*.cc' | wc -l | tr -d ' ') sources, from $SRC)"
