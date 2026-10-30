// pi/engine/um_pi.cpp - see um_pi.h. Circle's flags.
#include "um_pi.h"

#include <circle/logger.h>
#include <circle/new.h>
#include <circle/sched/scheduler.h>
#include <circle/spinlock.h>
#include <circle/synchronize.h>
#include <circle/util.h>
#include <fatfs/ff.h>

#include "fmt/fmt.h"
#include "user/um.h"

static const char From[] = "um";

// ---- the code arena: 4 MB of the heap, 64 KB-aligned, its pages made executable (PXN cleared)
static const unsigned ArenaSize = 4 << 20;
static const unsigned Page = 64 * 1024;		// Circle's AArch64 granule (translationtable64.cpp)
static u8 *s_pArena = 0;
static unsigned s_nUsed = 0;

// Circle's table: TTBR0_EL1 -> a level-2 table (512 MB an entry) -> level-3 tables (8192 pages of 64 KB). The kernel
// maps everything from _etext up PXN (never executable at EL1); the arena's pages get PXN cleared, the TLB entries
// invalidated on every core (inner shareable). Normal memory, RW: the code is written once a load, then only run.
static boolean MakeExecutable (uintptr nStart, uintptr nEnd)
{
	u64 nTTBR;
	asm volatile ("mrs %0, ttbr0_el1" : "=r" (nTTBR));
	u64 *pL2 = (u64 *) (uintptr) (nTTBR & 0x0000FFFFFFFF0000ULL);
	for (uintptr a = nStart; a < nEnd; a += Page)
	{
		u64 d2 = pL2[a >> 29];
		if ((d2 & 3) != 3) return FALSE;			// not a table descriptor
		u64 *pL3 = (u64 *) (uintptr) (d2 & 0x0000FFFFFFFF0000ULL);
		u64 &d3 = pL3[(a >> 16) & 8191];
		if ((d3 & 3) != 3) return FALSE;			// not a page descriptor
		d3 &= ~(1ULL << 53);					// PXN
	}
	asm volatile ("dsb ishst" ::: "memory");
	for (uintptr a = nStart; a < nEnd; a += Page)
		asm volatile ("tlbi vaae1is, %0" : : "r" (a >> 12) : "memory");
	asm volatile ("dsb ish; isb" ::: "memory");
	return TRUE;
}

static u8 *Alloc (u32 nBytes)
{
	nBytes = (nBytes + 4095) & ~4095u;
	if (!s_pArena || nBytes > ArenaSize - s_nUsed) return 0;
	u8 *p = s_pArena + s_nUsed;
	s_nUsed += nBytes;
	return p;
}
static void Reset (void) { s_nUsed = 0; }
// The code just written: each D-cache line cleaned to the point of unification, then each I-cache line invalidated
// (both broadcast to the inner-shareable domain: the render cores), then this core's pipeline (isb).
static void Sync (void *p, u32 nBytes)
{
	u64 nCtr;
	asm volatile ("mrs %0, ctr_el0" : "=r" (nCtr));
	uintptr dline = 4u << ((nCtr >> 16) & 15), iline = 4u << (nCtr & 15);
	uintptr a0 = (uintptr) p, a1 = a0 + nBytes;
	for (uintptr a = a0 & ~(dline - 1); a < a1; a += dline) asm volatile ("dc cvau, %0" : : "r" (a) : "memory");
	asm volatile ("dsb ish" ::: "memory");
	for (uintptr a = a0 & ~(iline - 1); a < a1; a += iline) asm volatile ("ic ivau, %0" : : "r" (a) : "memory");
	asm volatile ("dsb ish; isb" ::: "memory");
}
static void Barrier (void) { asm volatile ("isb" ::: "memory"); }

// ---- small card writes (the slot maps), done by a core-0 task: the control core never waits on a card write
static const unsigned WriteMax = 512;
struct TWrite { char path[64]; char data[WriteMax]; unsigned n; };
static TWrite s_Write[4];
static volatile unsigned s_nWriteHead = 0, s_nWriteTail = 0;	// a ring: core 3 writes the head, core 0 the tail
static volatile unsigned s_nWritesDone = 0, s_nWritesFailed = 0;

static boolean QueueWrite (const char *pPath, const void *pData, u32 n)
{
	unsigned h = s_nWriteHead, t = __atomic_load_n (&s_nWriteTail, __ATOMIC_ACQUIRE);
	if (h - t >= 4 || n > WriteMax || strlen (pPath) >= sizeof s_Write[0].path) return FALSE;
	TWrite &w = s_Write[h & 3];
	strcpy (w.path, pPath);
	memcpy (w.data, pData, n);
	w.n = n;
	__atomic_store_n (&s_nWriteHead, h + 1, __ATOMIC_RELEASE);
	return TRUE;
}

// 0.3.0 power-cut safety: the one atomic write (core/fmt/fmt.h write_atomic), not the tmp-unlink-rename this did
// before. It writes <path>.tmp, reads it back and compares it, keeps the old file as <path>.prev until .tmp is in
// place, and makes the folders - so a cut anywhere leaves the file old or new, never missing. It did unlink the old
// file and then rename over it: a cut in between left no file at all, which for /.rytm/slots/<K>.txt is the U-slot
// map and for /.rytm/display.txt the screen settings. A cut between the renames leaves a .prev that the reader's
// fmt::recover puts back. (Called by core 0's loop for display.txt and by the umwrite task for the slot maps.)
boolean SdWriteWhole (const char *pPath, const char *pData, unsigned n)
{
	return core::fmt::write_atomic (pPath, pData, n) ? TRUE : FALSE;
}

void CUserMachinesWriter::Run (void)
{
	for (;;)
	{
		while (s_nWriteTail != __atomic_load_n (&s_nWriteHead, __ATOMIC_ACQUIRE))
		{
			TWrite &w = s_Write[s_nWriteTail & 3];
			boolean bOK = SdWriteWhole (w.path, w.data, w.n);
			if (bOK) s_nWritesDone++;
			else s_nWritesFailed++;
			CLogger::Get ()->Write (From, bOK ? LogNotice : LogWarning, "%s %s", w.path, bOK ? "written" : "NOT written");
			__atomic_store_n (&s_nWriteTail, s_nWriteTail + 1, __ATOMIC_RELEASE);
		}
		CScheduler::Get ()->MsSleep (20);
	}
}

static um::Exec s_Exec = {Alloc, Reset, 0, Sync, Barrier, ArenaSize, QueueWrite, 0};

// The heap for the hosted um (the package buffer, the data arena, the voices' state): Circle's, at boot only.
extern "C" void *um_heap (u32 nBytes) { return new u8[nBytes]; }

boolean CUserMachines::Initialize (void)
{
	u8 *pRaw = new u8[ArenaSize + Page];
	if (!pRaw) return FALSE;
	s_pArena = (u8 *) (((uintptr) pRaw + Page - 1) & ~(uintptr) (Page - 1));
	if (!MakeExecutable ((uintptr) s_pArena, (uintptr) s_pArena + ArenaSize))
	{
		CLogger::Get ()->Write (From, LogError, "code arena: page table not as expected - user machines off");
		s_pArena = 0;
		return FALSE;
	}
	CLogger::Get ()->Write (From, LogNotice, "code arena %u KB at %lx (executable)", ArenaSize >> 10, (unsigned long) (uintptr) s_pArena);
	um::begin (&s_Exec, "/Plugins");
	new CUserMachinesWriter;
	return TRUE;
}

unsigned CUserMachines::WritesDone (void) { return s_nWritesDone; }
unsigned CUserMachines::WritesFailed (void) { return s_nWritesFailed; }
