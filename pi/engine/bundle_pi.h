// pi/engine/bundle_pi.h - project bundles (core/bundle, docs/BUNDLES.md) in the Pi kernel. Hosted (bundle_pi.cpp is
// compiled with the core); everything here is core 0's except transport(), which the MIDI realtime path calls from
// core 3 (one volatile store).
//   - the job runs in core 0's loop: poll(), one step (one 64 KB piece at most) a pass, only while the card queue is
//     idle (cardq::busy: no stream below its low mark, no request waiting) - never on the control core;
//   - the pause rules: stopped: full rate; the Rytm playing or a song take recording: reads only, 16 KB, one every
//     2 ms (writes wait: core 3 still reads DSP SAMPLE's streams through FatFs itself, and a card's own write pause
//     under FatFs's volume lock would make that read spin); the Overbridge recorder recording: nothing at all;
//   - the web page's endpoints (bundleweb.cpp) run in core 0's web tasks against the same job.
#ifndef _bundle_pi_h
#define _bundle_pi_h
#include <stdint.h>

#include "../ui/sidecar_project.h"

namespace bundle_pi {
void init(void (*command)(const char* cmd));   // CKernel::Initialize, after bridge::begin (the boot recovery ran in it)
void transport(uint8_t status);                // 0xFA / 0xFB play, 0xFC stop (any core)
bool poll(int recorder_state);                 // core 0's loop: rec_pi::UiState (2: recording); true: a step ran
int status_json(char* out, int cap);           // ,"bundle":{...} for /status
// the PROJECT pages (core/bundle/feed.h): the console's bundle=... lines (core 0; true when it was one, answered in
// the log), and the touchscreen page's view from the published feed
bool command(const char* line);
void ui_fill(sidecar_project_view_t* v);
// the pause rules now (the bundle job's, shared with the sample library's hashing: lib_pi.cpp); core 0
void io_rule(uint32_t* read_piece, uint32_t* write_piece, uint32_t* read_gap_us, const char** why);
}

#endif
