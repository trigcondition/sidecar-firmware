// pi/engine/lib_pi.h - the sample library (core/lib/library.h, docs/LIBRARY.md) in the Pi kernel. Hosted (compiled
// with the core); everything here is core 0's:
//   - begin() at boot, after bridge::begin (the card mounted, the bundles' boot recovery done): the index's load and the
//     card's walk start at the next polls (nothing read in begin);
//   - poll() in core 0's loop after the bundle job's step: one library step (one 64 KB piece at most), only while the
//     card queue is idle, never while a bundle job runs; the bundle job's pause rules (bundle_pi::io_rule): the Rytm
//     playing or a song take recording: 16 KB reads one every 2 ms, no card writes; the Overbridge recorder recording:
//     nothing; a project change re-reads the current project's references (hashed first);
//   - the console's lib=status|rescan|verify|bench|find:|hash: (the pools' pool=smp|wt|inst:..., srclib's browse=gr|wt|ms:...
//     and lib=changed:PATH stay the control core's);
//   - /status "library" and the touchscreen STATUS page's LIBRARY row.
#ifndef _lib_pi_h
#define _lib_pi_h
#include "../ui/sidecar_status.h"

namespace lib_pi {
void begin(bool card_ok);
bool poll();
bool command(const char* line);         // true: a library command (answered in the log)
int status_json(char* out, int cap);    // ,"library":{...}
void ui_fill(sidecar_status_view_t* v); // the STATUS page's LIBRARY row
}

#endif
