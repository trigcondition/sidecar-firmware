// pi/engine/insfx_pi.h - the display's INSERT FX page (pi/ui/sidecar_insfx.h) from core/fx/insert.h, on core 0: plain
// reads of the control side's bytes (drawing only, as voice_pi), the Rytm's selected track from core/ui/remote.cpp.
#pragma once
#include "../ui/sidecar_insfx.h"

namespace insfx_pi {
// ~10 times a second while the page shows: o from the newest values; o->gen changes only when something to draw changed
// peaks (optional): each voice's pair as the output stage sent it since the last call (Output::TakeVoicePeaks)
void Fill(sidecar_insfx_view_t* o, const float (*peaks)[2] = nullptr);
}  // namespace insfx_pi
