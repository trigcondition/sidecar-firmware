// pi/engine/chopview_pi.h - the display's CHOP page (pi/ui/sidecar_chop.h) filled from core/chop on core 0 (racy reads of
// the control core's map and the sources' samples: drawing only). Hosted (the core's flags).
#pragma once
#include "../ui/sidecar_chop.h"

namespace chopview_pi {

// The voice to show: the Rytm's selected track's when it plays CHOP, else `want` (the display's choice) when it does,
// else the first voice that does. The outline for the page's window (sidecar_chop_range). o->gen changes only when
// something to draw changed. Call ~10 times a second while the page is shown.
void Fill(sidecar_chop_view_t* o, int want);
// The page a CHOP voice's VOICE request opens (17: sidecar_ui.h)
constexpr int kPage = 17;

}  // namespace chopview_pi
