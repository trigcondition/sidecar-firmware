// pi/engine/loopview_pi.h - the display's LOOP page (pi/ui/sidecar_loop.h) from the looper (core/loop/loop.h): core 3 builds
// the view (FillView, ~20 a second while the page wants it) and publishes it behind a sequence counter, as morph_pi does;
// core 0 (the display) takes the newest. The page's edits come back as console lines ("loop=...").
#pragma once
#include "../ui/sidecar_loop.h"

namespace loopview_pi {

void FillView(sidecar_loop_view_t* o, int sel);   // the looper now; o->gen a hash of the rest (changes when anything drawn changes)
void Tick();                                      // core 3 (bridge::tick)
bool View(sidecar_loop_view_t* out, unsigned* seen);   // core 0: the newest view if *seen differs (then updated)
void Want(bool on, int sel);                      // core 0: the page shows, the grab it has selected

}  // namespace loopview_pi
