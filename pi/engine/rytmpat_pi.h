// pi/engine/rytmpat_pi.h - the display's Rytm pattern view (pi/ui/sidecar_rytmpat.h) fed from the Rytm's own pattern
// dump (core/seq/rytm_dump.h): core 0 says whether the page is open, core 3 turns the fetching on and off with it,
// decodes each good record (pi/ui/rytm_pattern.h) and publishes it behind a sequence counter (as seqed_pi.h). Built only
// with core/seq in the tree (SIDECAR_SEQED, Makefile).
#pragma once
#include "../ui/rytm_pattern.h"

namespace rytmpat_pi {

// core 3 (bridge::tick): the page's wish applied (rytm_dump::set_active), the fetch's state published
void Tick();
// core 0: which page is open (fetches while one is): 0 none, 1 the PAT page, 2 the VOICE page (its track's p-locks:
// no refetch on an edit while the Rytm plays)
void Want(int page);
// core 0: the newest decoded pattern and its number if new since *seen (then *seen is updated); false: nothing new
bool Pattern(rytm_pattern_t* out, int* index, unsigned* seen);
// core 0: the fetch's state. age_ms: since the shown record arrived (-1: none yet); pending: a fetch due or awaited;
// lost: the last request was not answered (the Rytm does not answer: not connected, or busy); stale: the Rytm's pattern
// was edited since (1: fetched once the edits settle, 2: held until STOP / the song take ends; core/seq/rytm_dump.h)
struct State {
  int age_ms;
  bool pending, lost;
  int stale;
};
State Get();

}  // namespace rytmpat_pi
