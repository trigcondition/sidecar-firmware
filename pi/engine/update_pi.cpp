// pi/engine/update_pi.cpp - see update_pi.h
#include "update_pi.h"

#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <string.h>

#include "version.h"

extern const fsweb::Backend *g_pWebCard;	// web.cpp: the card's Backend on FatFs (fsweb.h), shared with /fs

static const char From[] = "update";

namespace update_pi {
namespace {

boolean s_bCard, s_bEnabled, s_bTrial;
const char *s_pOffWhy = "";
volatile int s_nReboot;
volatile unsigned s_nRebootAt;
#if SIDECAR_DIRTY
const char kCommit[] = SIDECAR_COMMIT "-dirty";
#else
const char kCommit[] = SIDECAR_COMMIT;
#endif

// The trial's commit: once it has run upd::kCommitAfterS (core 0's scheduler runs this task, so a dead core 0 - or a
// watchdog reset - never commits, and the next boot is the old kernel's)
class CCommitTask : public CTask
{
public:
	CCommitTask (void)	{ SetName ("update"); }
	void Run (void) override
	{
		CScheduler *S = CScheduler::Get ();
		while (!upd::commit_due (Env ())) S->MsSleep (1000);
		CLogger::Get ()->Write (From, LogNotice, "the trial of %s (%s) has run %u s: making it permanent", SIDECAR_VERSION, kCommit, upd::kCommitAfterS);
		boolean bOK = upd::commit (Env ());
		char Json[700];
		upd::state_json (Env (), Json, sizeof Json);
		if (bOK) CLogger::Get ()->Write (From, LogNotice, "committed: %s", Json);
		else CLogger::Get ()->Write (From, LogError, "the commit did not finish (the next boot sorts it out): %s", Json);
	}
};

}  // namespace

upd::Env Env (void)
{
	upd::Env e = {g_pWebCard, RASPPI >= 5 ? 5 : 4, SIDECAR_VERSION, kCommit, s_bCard && s_bEnabled,
		      !s_bCard ? "no SD card: nothing to update" : s_pOffWhy, s_bTrial, CTimer::Get ()->GetUptime ()};
	return e;
}

boolean Notice (char *pLine1, char *pLine2, unsigned nCap)
{
	return upd::notice (Env (), pLine1, pLine2, (int) nCap);
}

boolean Trial (void) { return s_bTrial; }

void Boot (boolean bCard, boolean bEnabled, const char *pOffWhy, boolean bTrial)
{
	s_bCard = bCard, s_bEnabled = bEnabled, s_pOffWhy = pOffWhy, s_bTrial = bTrial;
	if (!bCard) return;
	boolean bWaits = upd::boot (Env ());
	char Json[700];
	upd::state_json (Env (), Json, sizeof Json);
	if (bTrial || bWaits || strstr (Json, "\"state\":\"idle\"") == 0) CLogger::Get ()->Write (From, LogNotice, "%s%s", bWaits ? "a trial boot, the commit in 30 s: " : "", Json);
	if (bTrial && !bWaits) CLogger::Get ()->Write (From, LogWarning, "updtrial=1 but no trial waits (cmdline-try.txt left over?): nothing will be committed");
	if (bWaits) new CCommitTask;
}

void RequestReboot (upd::Reboot Kind)
{
	s_nRebootAt = CTimer::GetClockTicks ();
	s_nReboot = Kind;
}

upd::Reboot TakeReboot (void)
{
	if (s_nReboot == upd::kNone || CTimer::GetClockTicks () - s_nRebootAt < 500000) return upd::kNone;
	upd::Reboot r = (upd::Reboot) s_nReboot;
	s_nReboot = upd::kNone;
	return r;
}

}  // namespace update_pi
