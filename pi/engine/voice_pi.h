// pi/engine/voice_pi.h - the display's VOICE page (pi/ui/sidecar_voice.h) and the Rytm's display-page requests
// (rytm-mods rui1036b: UP + T1..T5 -> 49 1 <page>), on core 0: the Rytm's selected track as core/ui/remote.cpp has it
// from 49 2 / 3 (plain reads of a few words: drawing only, as wgview_pi), the track's Overbridge stem from the input tap
// (core/ui/extra.h), and its p-locks from the Rytm's own pattern (rytmpat_pi's decoded dump).
#pragma once
#include <stdint.h>
#include "../ui/rytm_pattern.h"
#include "../ui/sidecar_voice.h"
#include "machines.h"

namespace voice_pi {

// ~10 times a second while the VOICE page shows: o from the newest data; pat the newest decoded Rytm pattern (null
// none), its number. o->gen changes only when something to draw changed.
void Fill(sidecar_voice_view_t* o, const rytm_pattern_t* pat, int pattern);
// A HOME tile's track (0..11) shown on VOICE instead of the Rytm's selected track, until the Rytm selects another one
// (-1: follow the Rytm). Core 0 (the display's command, voicepick=T).
void Pick(int track);
// The page the Rytm asked for since *seen (sidecar_ui's page number, -1 none; *seen updated): 1 HOME, 2 SEQ, 3 PAT,
// 4 VOICE (the machine view for a GRANULAR / WAVETABLE track, else VOICE), 5 SET, 6 INSERT FX (fx-ins: 9), 8 REC (11,
// sidecar_rec.h), 9 SCOPE (12, sidecar_scope.h), 10 SIDECAR STATUS (13, sidecar_status.h); 7 none (SAMPLE / AUTO: the
// Rytm's own screen, core/ui/remote.cpp). Also while on VOICE / the machine view (`voice_mode`): a track change on
// the Rytm, or a machine change of the selected track, moves between the two as the track's machine needs.
int Request(uint32_t* seen, bool voice_mode, uint32_t* track_seen);
// The page VOICE means for the selected track now: 7 the machine view, 8 VOICE, 17 CHOP
int VoicePage();
// The page for a track's machine byte (remote_track_machine: -1 unknown, 0x7F analog): 7 GRANULAR / WAVETABLE, 17 CHOP,
// else 8.
inline int PageForMachine(int vb) {
  int m = vb >= 0 && vb != 0x7F ? vb & 0x78 : -1;
  return m == core::kGranMachine || m == core::kWaveMachine ? 7 : m == core::kChopMachine ? 17 : 8;
}
// The pages that follow the Rytm's selected track (and its machine): the machine view (7), VOICE (8) and CHOP (17) are
// separate pages of the touchscreen, so a track whose machine needs another of them moves the display there. Request's
// `voice_mode`. (CHOP missing here was the bug: the display went into CHOP and never came out again.)
inline bool FollowsTrack(int page) { return page == 7 || page == 8 || page == 17; }
void Seen(uint32_t* seen, uint32_t* track_seen);   // the counts now (the first call: nothing is a new request)
bool ComboMap();   // UX review N3: UP held alone on the Rytm (rui1044): the touchscreen shows the UP + T map too
void OwnPages();   // a display is up: UP + T6 T7 T9 T10 T12 T13 open its SONG REC SCOPE STATUS PROJECT POOL, no longer the Rytm's own

}  // namespace voice_pi
