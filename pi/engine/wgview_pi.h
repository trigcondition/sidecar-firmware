// pi/engine/wgview_pi.h - the display's machine view and POOL (pi/ui/sidecar_wgview.h) filled from core/granular,
// core/wavetable and core/msamp (MULTISAMPLE's zones) and the POOL lists (core/lib/pools.h: one bank's rows, states, users,
// the RAM, SELECT UNUSED; srclib's browser) on core 0 (racy reads: drawing only). Hosted (the core's flags).
#pragma once
#include "../ui/sidecar_wgview.h"

namespace wgview_pi {

// The voice to show: the Rytm's selected track's (its SRC view or voice page on the remote UI) when it plays WAVETABLE
// GRANULAR or MULTISAMPLE, else `want` (the display's tap) when it does, else the first voice that does. o->gen changes only when
// something to draw changed. Call ~10 times a second while the view is shown.
void Fill(sidecar_wg_view_t* o, int want);

}  // namespace wgview_pi
