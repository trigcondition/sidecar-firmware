// pi/engine/update_pi.h - the web update's Pi side (update.h, docs/web-update.md): the boot's check (kernel.cpp, after
// the card is mounted, before the web server), the trial's commit as a task on core 0 once the trial has run
// upd::kCommitAfterS, and the reboot the page's install asks for (core 0's loop takes it half a second later, so the
// answer is out first). web.cpp serves /update/* with Env().
#ifndef _pi_engine_update_pi_h
#define _pi_engine_update_pi_h

#include <circle/types.h>

#include "update.h"

namespace update_pi {

// bCard: the card is mounted; bEnabled / pOffWhy: cmdline webupdate=, the test modes; bTrial: cmdline updtrial=1 (only
// the trial's cmdline-try.txt has it). Runs upd::boot and, when a trial waits, starts the commit task.
void Boot (boolean bCard, boolean bEnabled, const char *pOffWhy, boolean bTrial);
upd::Env Env (void);
boolean Trial (void);
void RequestReboot (upd::Reboot Kind);	// the web request (install, previous)
upd::Reboot TakeReboot (void);			// core 0's loop

}  // namespace update_pi

#endif
