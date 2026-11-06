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
volatile bool s_bWriting;		// inside WriteOnce: a panic from the card's own path must not write again

boolean WriteOnceInner (void);

boolean WriteOnce (void)
{
	s_bWriting = true;
	boolean b = WriteOnceInner ();
	s_bWriting = false;
	return b;
}

boolean WriteOnceInner (void)
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

#ifdef SIDECAR_PI5_TESTMODE
// testsecs=: the log into a file made beforehand, only its bytes overwritten (see docs/pi5.md "Engine test mode"): no
// cluster is allocated, the FATs stay as they are. f_close still rewrites the file's directory entry, every 10 s: a reset
// during that one sector write could damage the root directory. No file: no log. Without testcard=1 the engine has no
// card and Poll () does not run: a card of Raspberry Pi OS is mounted here for the moment of each write. With testcard=1
// the engine's mount is there (a mount here would pull it from under the engine) and Poll () runs too: this has its own
// place in the log.
void TestLog (bool bMounted)
{
	static FATFS s_Fs;
	static uint64_t s_nPos, s_nTestDone, s_nTestLost;
	if (!bMounted && f_mount (&s_Fs, "SD:", 1) != FR_OK) return;
	FIL f;
	if (f_open (&f, "SD:/usbtest.log", FA_WRITE | FA_OPEN_EXISTING) == FR_OK)
	{
		const uint64_t nSize = f_size (&f);
		for (int i = 0; i < 64 && s_nPos < nSize; i++)
		{
			unsigned n = blackbox::take (crumb::area (), &s_nTestDone, s_Buf, blackbox::kChunk, &s_nTestLost);
			if (n == 0) break;
			if (n > nSize - s_nPos) n = (unsigned) (nSize - s_nPos);
			UINT w = 0;
			if (f_lseek (&f, s_nPos) != FR_OK || f_write (&f, s_Buf, n, &w) != FR_OK || w == 0) break;
			s_nPos += w;
		}
		f_close (&f);
	}
	if (!bMounted) f_mount (0, "SD:", 0);
}
#endif

void Flush (void)
{
	for (int i = 0; i < 4 && s_State == kOn && crumb::area ()->log_total != s_nDone; ++i)
		if (!WriteOnce ()) break;
}

// A panic (CLogger's panic handler, kernel.cpp): its line is in the crumb's ring but would never reach the card, as
// core 0 halts before its loop's next write and the reset loses the ring (the Pi 4's sdboot / rpiboot path keeps no
// RAM). Written out only on core 0, with the black box on, no write of its own under way and no disk transfer in
// flight (a panic from the card's path itself). A write that hangs ends as the panic would anyway: the watchdog.
void PanicFlush (void)
{
	unsigned long nMpidr;
	asm volatile ("mrs %0, mpidr_el1" : "=r" (nMpidr));
	if ((nMpidr & 0xFF) != 0 || s_State != kOn || s_bWriting || crumb::area ()->disk.busy != 0) return;
	Flush ();
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
