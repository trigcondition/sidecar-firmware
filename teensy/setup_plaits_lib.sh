#!/bin/bash
# Copy Plaits + the stmlib parts it needs (Mutable Instruments, MIT, (c) Emilie
# Gillet) into libraries/Plaits as an Arduino library, from the pinned checkout
# that sim/plaits/build.sh sets up (eurorack 08460a6). Git-ignored; rerun freely.
#   setup_plaits_lib.sh --if-stale   only when libraries/Plaits is missing or this repo's own files in it
#                                    (sim/plaits/fm_machine.h, the shims user_data.h and stmlib's random.h)
#                                    changed since it was made
#                                    (run_sketch.sh, core/engines/setup_engines.sh: a stale copy built the old FM)
set -e
D=$(cd "$(dirname "$0")" && pwd)
L=$D/libraries/Plaits
SHIM=$D/../sim/plaits/shim/plaits/user_data.h
RNG=$D/../sim/plaits/shim/stmlib/utils/random.h
if [ "${1:-}" = --if-stale ] && [ -d "$L" ] && cmp -s "$D/../sim/plaits/fm_machine.h" "$L/src/fm_machine.h" &&
   cmp -s "$SHIM" "$L/user_data.h.src" && cmp -s "$RNG" "$L/src/stmlib/utils/random.h"; then
  exit 0
fi
"$D/../sim/plaits/build.sh" >/dev/null   # makes sure the vendor checkout exists
V=$(cd "$D/../sim/plaits/vendor/eurorack" && pwd -P)
rm -rf "$L"; mkdir -p "$L/src"
cd "$V"
for f in $(find plaits/dsp -name '*.cc' -o -name '*.h') plaits/resources.cc plaits/resources.h \
         $(find stmlib -maxdepth 1 -name '*.h') $(find stmlib/dsp stmlib/utils -name '*.h') \
         stmlib/dsp/units.cc stmlib/utils/random.cc; do
  mkdir -p "$L/src/$(dirname "$f")"; cp "$f" "$L/src/$f"
done
# user_data.h reads custom wavetables from an STM32 flash address (it would fault
# here): the simulator's stand-in (no user data, except a DX7 bank handed over by
# the 6-OP FM machine, fm_machine.h), which comes along.
cp "$SHIM" "$L/src/plaits/user_data.h"
cp "$SHIM" "$L/user_data.h.src"   # the shim as copied, for --if-stale (outside src/: not compiled)
# stmlib's Random with a state per render core when RYTM_RENDER_CORES > 1 (the Pi); unset, stmlib's own
cp "$RNG" "$L/src/stmlib/utils/random.h"
# A 6-OP FM voice load unpacks only the voice it plays: Plaits' six-op engine
# unpacks all 32 patches of a bank on a reload (~18 % of a 12-sample block,
# eight voices loading on one step 170 %); handed fm_user_bank() it unpacks
# the one voice into patch 0 or 1 (fm_user_slot(), see below) from a 128-byte stage.
# The simulator's build (rytm-emu's vendor checkout) keeps 32 copies.
sed -i '' 's|^#endif$|#define RYTM_FM_SINGLE_PATCH 1\n#endif|' "$L/src/plaits/user_data.h"
S="$L/src/plaits/dsp/engine2/six_op_engine.cc"
sed -i '' 's|^#include "plaits/resources.h"$|#include "plaits/resources.h"\n#include "plaits/user_data.h"|' "$S"
# Handed fm_user_bank(), the one voice goes into patch fm_user_slot() & 1 (0 or 1, alternated by fm_machine.h) and
# only the engine's voices on that patch unload (all of them with fm_user_slot() & 2: a first load): the other voice, ringing out the previous note on the other patch, keeps
# it. Stock Plaits unloads both, which cut that note's tail to silence in one block - the click on a PAT p-lock.
perl -0pi -e 's|(void SixOpEngine::LoadUserData\(const uint8_t\* user_data\) \{\n)|$1  if (user_data \&\& user_data == fm_user_bank()) {   // rytm: one voice, into patch fm_user_slot()\n    fm::Patch* target = \&patches_[fm_user_slot() \& 1];\n    target->Unpack(user_data);\n    for (int i = 0; i < kNumSixOpVoices; ++i) {\n      if ((fm_user_slot() \& 2) ? true : voice_[i].patch() == target) voice_[i].UnloadPatch();\n    }\n    return;\n  }\n|' "$S"
grep -q 'rytm: one voice, into patch fm_user_slot()' "$S" || { echo "six_op_engine.cc: patch failed"; exit 1; }
cp "$D/../sim/plaits/fm_machine.h" "$L/src/fm_machine.h"
# Voice::Init runs at startup and when a voice returns from a plugin (voiceloop): cold, so it
# lives in program flash, not in ITCM (RAM1 is tight; see run_sketch.sh's ITCM check).
V2="$L/src/plaits/dsp/voice.cc"
sed -i '' 's|^void Voice::Init(BufferAllocator\* allocator) {$|__attribute__((section(".flashmem"))) void Voice::Init(BufferAllocator* allocator) {|' "$V2"
grep -q 'section(".flashmem"))) void Voice::Init' "$V2" || { echo "voice.cc: Voice::Init patch failed"; exit 1; }
# The PLAITS machine's curated models (rytm-mods 0021, 2026-10-09; the voice
# loop's kPlaitsModels): NOISE (17) and the drums (21 BASS DRUM, 22 SNARE, 23
# HI-HAT; the MD user machine plays drums) are left out of the build - a silent engine
# holds their places, so the other engines keep their numbers. The 6-OP FM
# engines (2..4) stay: the 6-OP FM machine plays engine 2.
P="$L/src/plaits/dsp"
rm -f "$P"/engine/noise_engine.* "$P"/engine/bass_drum_engine.* "$P"/engine/snare_drum_engine.* \
      "$P"/engine/hi_hat_engine.* "$P"/drums/*bass_drum.h "$P"/drums/*snare_drum.h "$P"/drums/hi_hat.h
cat > "$P/engine/null_engine.h" <<'N'
// rytm-teensy: an engine left out of the build (silence), holding its number.
#pragma once
#include <string.h>
#include "plaits/dsp/engine/engine.h"
namespace plaits {
class NullEngine : public Engine {
 public:
  virtual void Init(stmlib::BufferAllocator* allocator) { (void)allocator; }
  virtual void Reset() {}
  virtual void LoadUserData(const uint8_t* user_data) { (void)user_data; }
  virtual void Render(const EngineParameters& parameters, float* out, float* aux, size_t size,
                      bool* already_enveloped) {
    (void)parameters;
    (void)already_enveloped;
    memset(out, 0, size * sizeof(float));
    memset(aux, 0, size * sizeof(float));
  }
};
}  // namespace plaits
N
sed -i '' -E -e '/#include "plaits\/dsp\/engine\/(noise|bass_drum|snare_drum|hi_hat)_engine.h"/d' \
    -e 's|#include "plaits/dsp/engine/engine.h"|#include "plaits/dsp/engine/engine.h"\
#include "plaits/dsp/engine/null_engine.h"|' \
    -e '/^  (NoiseEngine noise|BassDrumEngine bass_drum|SnareDrumEngine snare_drum)_engine_;$/d' \
    -e 's|^  HiHatEngine hi_hat_engine_;$|  NullEngine null_engine_;   // NOISE, BASS DRUM, SNARE, HI-HAT: not built|' "$P/voice.h"
sed -i '' -E -e 's/&(noise|bass_drum|snare_drum|hi_hat)_engine_,/\&null_engine_,/' "$P/voice.cc"
[ "$(grep -c '&null_engine_' "$P/voice.cc")" = 4 ] && ! grep -qE '^  (NoiseEngine|HiHatEngine|BassDrumEngine|SnareDrumEngine) ' "$P/voice.h" \
  || { echo "voice.h/voice.cc: the curation patch failed"; exit 1; }
# The DX7 banks (12 KB, read only when a 6-op patch loads) in flash: on the
# Teensy 4 a const array is copied into RAM1 at boot otherwise.
R="$L/src/plaits/resources.cc"
sed -i '' 's/^const uint8_t syx_bank_\([0-2]\)\[\] = {/const uint8_t syx_bank_\1[] PROGMEM = {/' "$R"
sed -i '' '1s|^|#include <avr/pgmspace.h>\n|' "$R"
# The big wavetables (WAVETABLE, CHORDS, WAVE TERRAIN; 50 KB) and the speech
# engine's LPC word banks (11 KB) in flash too: read through the cache, they
# cost those models little (2026-10-09, sim/stress.py pw pc pt ps) and free RAM1.
sed -i '' 's/^const int16_t wav_integrated_waves\[\] = {/const int16_t wav_integrated_waves[] PROGMEM = {/' "$R"
W="$L/src/plaits/dsp/speech/lpc_speech_synth_words.cc"
sed -i '' 's/^const uint8_t bank_\([0-4]\)\[\] = {/const uint8_t bank_\1[] PROGMEM = {/' "$W"
sed -i '' '1s|^|#include <avr/pgmspace.h>\n|' "$W"
[ "$(grep -c 'PROGMEM = {' "$R" "$W" | awk -F: '{s+=$2} END {print s}')" = 9 ] || { echo "resources: PROGMEM patch failed"; exit 1; }
cat > "$L/src/Plaits.h" <<'H'
// Arduino finds a library by a header at the top of src/: include this first.
#pragma once
#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"
#include "fm_machine.h"
H
cp "$V/LICENSE" "$L/LICENSE" 2>/dev/null || true
cat > "$L/library.properties" <<P
name=Plaits
version=0.0.0
author=Emilie Gillet (Mutable Instruments)
maintainer=rytm-teensy (local copy)
sentence=Plaits macro-oscillator DSP, eurorack 08460a6
paragraph=MIT
category=Signal Input/Output
url=https://github.com/pichenettes/eurorack
architectures=*
P
echo "Plaits library in $L ($(find "$L/src" -name '*.cc' | wc -l | tr -d ' ') sources)"
