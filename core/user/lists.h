// core/user/lists.h - the project's pool and its JUNO / FM lists as a page shows and edits them (the Pi's web page,
// pi/engine/web.cpp; the Mac's core lib, sim/tests/test_core_lists.py). The lists themselves are core/pool's and
// core/presets' (the Teensy's too); this is the board-with-a-page side: a status JSON and console commands. Like
// core/user/um.h: everything on the control core (the Pi's core 3) between blocks, but json_copy (any core).
//
//   JSON  {"project","card","sram":[total,used],"pool":[{"slot","path","name","ok"}],"npool",
//          "juno":{"total","list":[{"slot","name","used"}]},"fm":{"total","rom","list":[...]},
//          "voices":[{"v","m","pat"}]}  (pool slots 1..127 with a path, juno/fm slots 0.. as PAT picks them;
//          "rom": the FM list is empty, so PAT picks Plaits' 96 ROM voices; voices' "m": PLAITS JUNO FM DAISY
//          DSPS SAMPLER DD U1..U5)
#pragma once

namespace lists {

// Console (the Pi's ':' lines and web page, 191 characters at most), true when it was one of these; out gets a
// one-line answer:
//   pool=<1..127>:<path under /Samples>    the current project's pool slot = that file (14 to the Rytm)
//   pool=<1..127>:   or  pool=<slot>:-      the slot emptied
//   juno=ren:<slot>:<name>   juno=del:<slot>   juno=swap:<a>:<b>   the project's JUNO list (16 to the Rytm)
//   juno=save:<voice 0..7>[:<name>]   the patch a JUNO voice plays now, its six knobs as they are, into the list
//   fm=ren:<slot>:<name>   fm=del:<slot>   fm=swap:<a>:<b>   the project's FM list (17 to the Rytm)
//   preset=juno|fm:add|set:<gen>:<folder>:<row>[:<at>]   a row of the preset index (presets::view's folders, gen its walk) into the
//                          list: add the first empty slot, set slot <at> (REPLACE); the answer is presets::result_text
//   fm=save:<voice 0..7>   the ROM voice an FM voice plays now (the list empty) into the list
bool command(const char* c, char* out, int cap);
void idle();                    // the control core: the JSON again when something in it changed (at most every 250 ms)
void publish_now();             // the control core: the JSON again at once
int json_copy(char* out, int cap);   // any core: the last JSON (0: none yet, or cap too small)

}  // namespace lists
