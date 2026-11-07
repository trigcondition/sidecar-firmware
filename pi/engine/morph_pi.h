// pi/engine/morph_pi.h - the display's MORPH page (pi/ui/sidecar_morph.h) from the scenes (core/mod/scene.h): core 3
// builds the view (FillView, ~20 a second while the page wants it) and publishes it behind a sequence counter, as
// song_pi does; core 0 (the display) takes the newest. The page's edits come back as console lines ("scene=...").
#pragma once
#include "../ui/sidecar_morph.h"

namespace morph_pi {

void FillView(sidecar_morph_view_t* o, int top);      // the scenes now, the selected scene's locks from row top; o->gen
                                                      // a hash of the rest (changes when anything drawn changes)
void Tick();                                          // core 3 (bridge::tick)
bool View(sidecar_morph_view_t* out, unsigned* seen); // core 0: the newest view if *seen differs (then updated)
void Want(bool on, int top);                          // core 0: the page shows, its list from row top

}  // namespace morph_pi
