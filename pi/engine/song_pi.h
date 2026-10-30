// pi/engine/song_pi.h - the display's SONG page (pi/ui/sidecar_song.h) from SONG RECORD (core/song/song.h): core 3
// builds the view (the state, the take's lanes) while the page wants it and publishes it behind a sequence counter;
// core 0 (the display) takes the newest. The page's keys come back as console lines ("song=...", core 3's queue).
#pragma once
#include "../ui/sidecar_song.h"

namespace song_pi {

void Tick();                                          // core 3 (bridge::tick): ~10 views a second while wanted
bool View(sidecar_song_view_t* out, unsigned* seen);  // core 0: the newest view if *seen differs (then updated)
void Want(bool on);                                   // core 0: the page shows (the lanes are built only then)

}  // namespace song_pi
