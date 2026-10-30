// core/bundle/rename.h - a project renamed (or saved under another name) on the Rytm keeps its Sidecar files (UX review
// B1; docs/BUNDLES.md "Rename on the Rytm"). Pi and Mac only (CORE_MSAMP).
//
// The Sidecar finds a project's files by the Rytm's project name (core/project_key.h). A SAVE PROJECT AS on the Rytm
// therefore used to look like data loss: the new name had none of the poly tracks, pools, songs, LFOs or inserts. Now,
// when the Rytm names a project (its 10) whose key has no Sidecar files while the project before had some, the
// project switch waits and the Rytm's PROJECT page asks:
//
//     RENAMED FROM <OLD>?   YES: MOVE ITS SIDECAR FILES   NO: A NEW PROJECT
//
// YES first lets every unsaved edit of the old project reach its files (the sequence, the LFOs, the inserts, the pool
// lists: FLUSH, at most kFlushMs), holds every other save while it moves (idle_save), then moves every per-project file and folder of the old key (bundle.cpp's kProjectFiles, the pool lists' folder) to
// the new key - only names change on the card, nothing is copied or deleted - then makes the switch, so the new name
// loads them. Should a move fail half way, the ones already moved are moved back and nothing else happens (the old
// project keeps all its files; the switch goes on as NO). NO (or no answer within kAskMs, or the Rytm naming yet
// another project) switches to the new name as before: nothing moved. While it asks, the Sidecar
// keeps the old project's state (for a rename that is the same content).
//
// No question while SONG RECORD records, arms or replays (its journal stays open in the songs folder): the switch then
// goes as before.
//
// What it does not do: compare the Rytm's content (the review's pattern-dump hash). A dump costs the Rytm 15 KB and its
// UI's time, and the question is harmless: NO changes nothing. So it asks whenever the old project had files and the
// new name has none; a NEW PROJECT made on the Rytm is answered with NO. Moving rather than copying: a project SAVED
// AS a copy leaves the original without its Sidecar files - the prompt says MOVE, and answering NO keeps them.
//
// The control core (where set_project runs). Every card operation (the looks, the renames) goes through the card queue
// (core/msamp/cardq.h), one at a time, a step each tick: the control core never touches the card nor waits for it.
#pragma once
#include <stdint.h>

#include "../project_key.h"

namespace core {
namespace rename {

constexpr uint32_t kAskMs = 120000;   // no answer for two minutes: NO

enum State : uint8_t { kIdle = 0, kAsk };

// set_project's: the Rytm named `to` while `from` is the project. true: the switch is this module's - it looks at the
// card (both keys' files), then switches at once (nothing to ask) or asks first (the PROJECT page); false: switch now
// (no card, the same key). A repeat of the same `to` meanwhile keeps waiting; another name cancels the question.
bool intercept(const char* from, const char* to);
bool busy();                  // looking, asking or moving
bool moving();                // FLUSH done, looking again / moving / undoing: idle_save holds every other save
                              // meanwhile, and a kit change is only noted (lfo / ins kit_changed: taken at the switch)
void back();                  // set_project's: the Rytm named the current project again - a pending question is dropped
// The answer (the Rytm's PROJECT page; tests): yes moves the files first. Either way the switch to `to` follows
// (pool.cpp project_switch). Returns the files and folders moved (0 for NO or a move that failed and was undone).
int answer(bool yes);
void tick(uint32_t ms);       // the control loop: the looks and moves (a card-queue step a pass), the ask's timeout (NO)
int state();
const char* from();           // the Rytm names of the ask ("" none)
const char* to();
int files();                  // how many files / folders the old key has (the prompt shows it)
const char* last();           // the last outcome, one line ("MOVED 7 TO SHARED SET 2", "MOVE FAILED: NOTHING MOVED")
uint32_t asks();              // a count that moves with every new ask (the PROJECT page opens on it)
void reset();                 // tests

}  // namespace rename
}  // namespace core
