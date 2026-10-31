// pi/engine/blackbox_pi.cpp - see blackbox.h. Core 0 only (FatFs on this core, the loop's own task); each write is
// open (append) / write / close, so a reset loses at most the ~2 s not yet written. Each write is timed (mbtrace 42,
// also its crumb section): a slow card shows up in /status's mbtrace like the loop's other steps.
// No reboot loop on a card that hangs a write: the first write of a boot is bracketed by /.rytm/blackbox.try (made
// before, removed after); a boot that finds it (the one before hung there, the crumb's RAM not kept on the Pi 4's
// sdboot path) leaves the black box off and removes it, so the boot after tries again. Also off after the crumb says
// core 0 died in section 42.
#include "blackbox_pi.h"
#include "blackbox.h"
#include "bridge.h"
#include "mbtrace.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include <fatfs/ff.h>

extern "C" int core_snprintf (char *out, size_t n, const char *fmt, ...);

namespace blackbox_pi {
namespace {
const char From[] = "blackbox";
const char kPath[] = "SD:/.rytm/blackbox.log", kOld[] = "SD:/.rytm/blackbox.1.log", kTry[] = "SD:/.rytm/blackbox.try";
enum { kWait, kOn, kOff } s_State = kWait;
uint64_t s_nDone, s_nLost, s_nLostShown, s_nBytes, s_nFile, s_nLastMs, s_nRetryMs;
unsigned s_nWrites, s_nFails, s_nFailsInRow;
char s_Buf[blackbox::kChunk + 96];

uint64_t NowMs (void)
{
	uint64_t t, f;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t));
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (f));
	return t / (f / 1000);
}

void Off (const char *pWhy)
{
	s_State = kOff;
	CLogger::Get ()->Write (From, LogWarning, "the black box is off: %s", pWhy);
}

// one write: the bytes logged since the last one (a chunk at most); FALSE when the card refused it (nothing taken then)
boolean WriteOnce (void)
{
	mbtrace::Scope Mb (42, 200);
	const uint64_t nDone = s_nDone, nLost = s_nLost, nShown = s_nLostShown;
	unsigned n = 0;
	if (s_nLost != s_nLostShown)	// (the line goes before the bytes after the gap)
	{
		n = (unsigned) core_snprintf (s_Buf, 96, "[blackbox: %u bytes not kept (the card was busy)]\n",
					      (unsigned) (s_nLost - s_nLostShown));
		s_nLostShown = s_nLost;
	}
	n += blackbox::take (crumb::area (), &s_nDone, s_Buf + n, blackbox::kChunk, &s_nLost);
	if (n == 0) return TRUE;
	if (s_nFile + n > blackbox::kRotate)	// rotate: the old file aside (a rename that fails: off, not a file without end)
	{
		f_unlink (kOld);
		if (f_rename (kPath, kOld) != FR_OK) { s_nLost += n; Off ("blackbox.log could not be renamed to blackbox.1.log"); return TRUE; }
		s_nFile = 0;
	}
	FIL f;
	UINT w = 0;
	boolean bOK = f_open (&f, kPath, FA_WRITE | FA_OPEN_APPEND) == FR_OK;
	if (bOK)
	{
		bOK = f_write (&f, s_Buf, n, &w) == FR_OK && w == n;
		bOK = f_close (&f) == FR_OK && bOK;
	}
	if (!bOK && w == 0)		// nothing reached the file: taken again next time (the ring permitting)
	{
		s_nDone = nDone, s_nLost = nLost, s_nLostShown = nShown;
		return FALSE;
	}
	s_nLost += n - w;		// (a short write: the card full; the rest counted lost)
	s_nFile += w;
	s_nBytes += w;
	s_nWrites++;
	return bOK;
}

boolean Start (void)
{
	if (crumb::PrevDiedIn (42)) { Off ("the previous boot hung writing /.rytm/blackbox.log"); return FALSE; }
	mbtrace::Scope Mb (42, 200);
	f_mkdir ("SD:/.rytm");
	FILINFO Info;
	if (f_stat (kTry, &Info) == FR_OK)
	{
		f_unlink (kTry);
		Off ("the previous boot did not finish its first write to /.rytm/blackbox.log (on again next boot)");
		return FALSE;
	}
	s_nFile = f_stat (kPath, &Info) == FR_OK ? Info.fsize : 0;
	FIL f;
	if (f_open (&f, kTry, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { Off ("the card cannot be written"); return FALSE; }
	f_close (&f);
	CLogger::Get ()->Write (From, LogNotice, "the log goes to /.rytm/blackbox.log too (%u KB there; blackbox.1.log the one before)",
				(unsigned) (s_nFile / 1024));
	s_State = kOn;
	for (int i = 0; i < 8 && s_State == kOn && crumb::area ()->log_total != s_nDone; ++i)	// the boot's log so far
		if (!WriteOnce ()) break;
	f_unlink (kTry);
	return s_State == kOn;
}
}	// namespace

void Poll (void)
{
	const uint64_t nMs = NowMs ();
	if (s_State == kWait)
	{
		if (CTimer::Get ()->GetUptime () < 5) return;
		Start ();
		s_nLastMs = nMs;
	}
	if (s_State != kOn) return;
	if (nMs - s_nLastMs < s_nRetryMs) return;
	const uint64_t nPending = crumb::area ()->log_total - s_nDone;
	if (!blackbox::due (nPending, (unsigned) (nMs - s_nLastMs), bridge::card_busy ())) return;
	s_nLastMs = nMs;
	if (WriteOnce ())
	{
		s_nRetryMs = 0, s_nFailsInRow = 0;
		return;
	}
	s_nFails++;			// the card gone or full: again in 30 s; off after 10 in a row (until the next boot)
	s_nRetryMs = 30000;
	if (++s_nFailsInRow == 10) Off ("/.rytm/blackbox.log could not be written 10 times in a row");
}

void Flush (void)
{
	for (int i = 0; i < 4 && s_State == kOn && crumb::area ()->log_total != s_nDone; ++i)
		if (!WriteOnce ()) break;
}

unsigned Status (char *pOut, unsigned nMax)
{
	return (unsigned) core_snprintf (pOut, nMax, "{\"on\":%s,\"bytes\":%u,\"lost\":%u,\"writes\":%u,\"fails\":%u}",
					 s_State == kOn ? "true" : "false", (unsigned) s_nBytes, (unsigned) s_nLost, s_nWrites, s_nFails);
}

unsigned Read (bool bOld, unsigned long long nOff, char *pOut, unsigned nMax, unsigned long long *pSize, unsigned long long *pFrom)
{
	*pSize = *pFrom = 0;
	FIL f;
	if (f_open (&f, bOld ? kOld : kPath, FA_READ) != FR_OK) return 0;
	const unsigned long long nSize = f_size (&f);
	if (nOff == ~0ull) nOff = nSize > nMax ? nSize - nMax : 0;
	if (nOff > nSize) nOff = nSize;
	UINT n = 0;
	if (f_lseek (&f, nOff) != FR_OK || f_read (&f, pOut, nMax, &n) != FR_OK) n = 0;
	f_close (&f);
	*pSize = nSize, *pFrom = nOff;
	return n;
}

}	// namespace blackbox_pi
