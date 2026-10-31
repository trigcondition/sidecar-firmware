// pi/engine/blackbox_pi.h - the black box's card side (blackbox.h): core 0's loop appends the log to
// /.rytm/blackbox.log (blackbox.1.log the file before). /status "blackbox" has its counts; GET /blackbox reads them
// (?old=1 the one before, ?off=N from N, else the last 64 KB; the first line "SIZE FROM").
#pragma once

namespace blackbox_pi {
void Poll (void);			// core 0's loop: starts 5 s into the boot, then writes when due (blackbox.h)
void Flush (void);			// a planned reboot (StopAndReboot): the last lines out first
unsigned Status (char *pOut, unsigned nMax);
// GET /blackbox (pairing-gated; web.cpp): the file's bytes from nOff (~0: its last nMax), *pSize its size; 0 none
unsigned Read (bool bOld, unsigned long long nOff, char *pOut, unsigned nMax, unsigned long long *pSize, unsigned long long *pFrom);	// the /status object: {"on":..,"bytes":..,"lost":..,"writes":..,"fails":..}
}
