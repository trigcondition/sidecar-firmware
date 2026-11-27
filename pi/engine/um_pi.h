// pi/engine/um_pi.h - the user machines (core/user/um.h) on the Pi: the code arena (4 MB of the heap whose 64 KB
// pages get PXN cleared in Circle's translation table, the TLBs invalidated on every core), the cache maintenance
// after a load (dc cvau / ic ivau, broadcast; isb), and a core-0 task that writes the slot maps (the control core
// never waits on a card write). docs/user-machines-pi.md.
#ifndef _pi_engine_um_pi_h
#define _pi_engine_um_pi_h
#include <circle/sched/task.h>
#include <circle/types.h>

class CUserMachines
{
public:
	static boolean Initialize (void);	// core 0, at boot, after bridge::begin and before the cores start
	static unsigned WritesDone (void);
	static unsigned WritesFailed (void);
};

// A small card file written whole (core 0): <path>.tmp, then renamed over <path>; its folders made. The slot maps
// and /.sidecar/display.txt.
boolean SdWriteWhole (const char *pPath, const char *pData, unsigned n);

class CUserMachinesWriter : public CTask	// core 0: the queued slot-map writes
{
public:
	CUserMachinesWriter (void) { SetName ("umwrite"); }
	void Run (void) override;
};

#endif
