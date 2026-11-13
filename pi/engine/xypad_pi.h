// pi/engine/xypad_pi.h - the display's XY page (pi/ui/sidecar_xy.h) from the XY pad (core/mod/xypad.h): core 3 builds the
// view (FillView, ~20 a second while the page wants it) and publishes it behind a sequence counter, as morph_pi does;
// core 0 (the display) takes the newest. The page's touches and edits come back as console lines ("xy=...").
#pragma once
#include "../ui/sidecar_xy.h"

namespace xypad_pi {

void FillView(sidecar_xy_view_t* o, bool names = false);   // the pad now; o->gen a hash of the rest; names: the
                                                           // picker's tables built again (else the last ones)
void Tick();                                         // core 3 (bridge::tick)
bool View(sidecar_xy_view_t* out, unsigned* seen);   // core 0: the newest view if *seen differs (then updated)
void Want(bool on, bool names = false);              // core 0: the page shows; names: its picker is open (the
                                                     // tables built at once, then every 250 ms; not while closed)
unsigned NamesBuilt();                               // tests: how often the tables were built
bool Quiet(const char* cmd);                         // a drag's line (xy=pos, xy=up, scene=fader, ~33 a second):
                                                     // the display neither logs it nor holds its status push back

}  // namespace xypad_pi
