// pi/engine/seditview_pi.h - the display's SAMPLE EDIT page (pi/ui/sidecar_sedit.h) filled from core/samp/sedit on core 0
// (racy reads of the control core's edit state: drawing only, as chopview_pi). Hosted (the core's flags).
#pragma once
#include "../ui/sidecar_sedit.h"

namespace seditview_pi {

// A copy of sedit::view(); o->valid while an edit is open. o->gen changes only when something to draw changed. Call
// ~10 times a second (the kernel also opens and leaves the page by o->valid).
void Fill(sidecar_sedit_view_t* o);
// An edit is open (VOICE, from the Rytm's UP + T4 or a track change, then shows it: voice_pi::VoicePage)
bool Open();
// The page an open edit shows (21: sidecar_ui.h)
constexpr int kPage = 21;

}  // namespace seditview_pi
