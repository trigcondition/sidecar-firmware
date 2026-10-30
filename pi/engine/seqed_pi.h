// pi/engine/seqed_pi.h - the touch pattern editor's engine side on the Pi (docs/TOUCH-EDITOR.md): the display's
// "seqed=..." lines applied on core 3 (the console's queue) to core/seq's pattern, and the shown track published for
// core 0 (the display) behind a sequence counter. Built only with core/seq in the tree (SIDECAR_SEQED, Makefile).
#pragma once
#include "../ui/sidecar_seqed.h"

namespace seqed_pi {

// core 3 (the console's command chain): a "seqed=..." line or "seqed" (the status); false: not one
bool Command(const char* p, char* out, int cap);
// core 3 (bridge::tick): the view published again when the pattern, the shown track or the transport changed
void Tick();
// core 0: the newest view, if its number differs from *seen (then *seen is updated); false: nothing new
bool View(sidecar_seq_view_t* out, unsigned* seen);
// core 0: the play head of the shown track in steps x 1000, -1 none
int Playhead();

}  // namespace seqed_pi
