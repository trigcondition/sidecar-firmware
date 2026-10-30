// pi/engine/status_pi.h - the core's part of the display's SIDECAR STATUS page (pi/ui/sidecar_status.h), on core 0:
// each voice's machine and the rytm-mods level the Rytm's messages show (core/ui/remote.h: which of the mods'
// messages have come; no build number is sent, so the level is inferred). kernel.cpp fills the rest (the cores, the
// links, the card, the versions). Hosted (the core's flags); plain reads (drawing only).
#pragma once
#include "../ui/sidecar_status.h"

namespace status_pi {

void Core(sidecar_status_view_t* o);   // o->mach, o->rytm_mod

}  // namespace status_pi
