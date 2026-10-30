// pi/engine/scope_pi.h - the display's SCOPE page (pi/ui/sidecar_scope.h) filled on core 0 from the Overbridge input
// tap (core/ui/extra.h: the IN path's min / max entries of one source, 8 frames each) and the recorder's meters
// (recorder.h meter_peaks, the REC page's): the trace for the page's source and timebase, triggered on a rising
// crossing for the short timebases, a sweep for the long ones - core/ui/page_scope.cpp's rules at 256 columns. Hosted
// (the core's flags); plain reads of the tap's ring (drawing only).
#pragma once
#include <stdint.h>
#include "../ui/sidecar_scope.h"

namespace scope_pi {

// ~15 times a second while the page shows: o from the newest entries. want: the page's source (0..10, -1 the Rytm's
// selected track), tb its timebase; peaks the recorder's meter peaks for the IN channels (MAIN L, MAIN R, the 8 stems,
// IN L, IN R; 24-bit, since the last call). o->gen changes when new columns came (or the source / timebase changed).
void Fill(sidecar_scope_view_t* o, int want, int tb, const uint32_t peaks[SSC_CH]);
// The page left: the tap off again (the VOICE page and the Rytm's own SCOPE turn it on when they read it)
void Leave();
// The scope's source for Rytm track t (0..11): its voice's stem (BD SD RS/CP BT LT MT/HT CH/OH CY/CB: 1..8)
int SourceOfTrack(int t);

}  // namespace scope_pi
