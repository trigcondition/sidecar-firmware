// pi/engine/guard.cpp - see guard.h.
#include "guard.h"

#include <circle/multicore.h>

#include "bridge.h"
#include "crumb.h"
#include "ob_usb.h"

CGuard g_Guard;

static inline u64 Ticks (void)
{
	u64 t;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t) :: "memory");
	return t;
}

static inline u64 TicksPerUs (void)
{
	u64 f;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (f));
	return f / 1000000;			// 54 on the Pi 4
}

static void Spin (unsigned nUs)
{
	u64 t0 = Ticks (), n = nUs * TicksPerUs ();
	while (Ticks () - t0 < n) {}
}

void CGuard::Enter (unsigned nCore, unsigned nVoice)
{
	m_nCurrent[nCore] = nVoice;
	m_bArmed[nCore] = TRUE;
	if (m_nFaultTest == nVoice + 1)
	{
		m_nFaultTest = 0;
		*(volatile u32 *) 0xFFFFFFFFF000ull = 1;	// beyond every mapping: a data abort here
	}
	if (m_nOverBlocks && m_nOverVoice == nVoice)
	{
		m_nOverBlocks--;
		Spin (300);
	}
	if (m_nHangTest == nVoice + 1)
	{
		m_nHangTest = 0;
		Spin (30000);					// 30 ms: the control core's 20 ms limit is fatal
	}
}

void CGuard::LogNews (CLogger *pLogger)
{
	unsigned nPerUs = (unsigned) TicksPerUs ();
	u32 f = TakeFreed ();	// first: a voice freed and muted again in the same second ends on its mute
	for (unsigned v = 0; v < Voices; v++)
		if ((f >> v) & 1) pLogger->Write ("guard", LogNotice, "voice %u unmuted: its machine changed (now %s)", v, bridge::machine_name (v));
	u32 n = TakeNews ();
	for (unsigned v = 0; v < Voices; v++)
	{
		if (!((n >> v) & 1)) continue;
		pLogger->Write ("guard", LogWarning, "voice %u !%s muted: %s (longest render %u us, budget %u us)", v,
				bridge::machine_name (v), m_pWhy[v], m_nMaxTicks[v] / nPerUs, m_nBudget / nPerUs);
	}
	for (unsigned c = 1; c <= 2; c++)
	{
		if (m_nFaultPC[c])
		{
			pLogger->Write ("guard", LogWarning, "core %u faulted at PC 0x%lx (fault %u on this core), resumed %s", c,
					(unsigned long) m_nFaultPC[c], m_nFaults[c],
					m_bResumedOK[c] ? "at task level (EL1t)" : "NOT at task level");
			m_nFaultPC[c] = 0;
		}
	}
}

// A fault while a render core renders a voice: back to that core's loop (cores.cpp's Worker, which mutes the voice
// and renders the block's other voices). The exception came from EL1t (Circle's tasks run on SP_EL0) and runs here
// in EL1h on the core's exception stack (SP_EL1), with DAIF masked. A longjmp from here would leave the core in EL1h
// with its interrupts masked and its exception stack in use; instead this returns from the exception (eret) into
// ResumeWorker, in the interrupted context's mode and DAIF (SPSR), with SP_EL1 back at its top as Circle's stub
// found it (pFrame->sp_el1). ResumeWorker then longjmps on SP_EL0, as an ordinary task-level function.
static void __attribute__ ((noinline, noreturn)) ResumeWorker (void)
{
	__builtin_longjmp (g_Guard.JumpBuffer (CMultiCoreSupport::ThisCore ()), 1);
}

void CEngineExceptionHandler::Throw (unsigned nException, TAbortFrame *pFrame)
{
	unsigned nCore = CMultiCoreSupport::ThisCore ();
	if ((nCore == 1 || nCore == 2) && g_Guard.Armed (nCore))
	{
		g_Guard.NoteFault (nCore, pFrame->elr_el1);
		u64 nSPSR = pFrame->spsr_el1, nStack = pFrame->sp_el1;		// read before the frame is dropped
		asm volatile ("msr elr_el1, %0\n"
			      "msr spsr_el1, %1\n"
			      "mov sp, %2\n"
			      "eret"
			      :: "r" ((u64) &ResumeWorker), "r" (nSPSR), "r" (nStack) : "memory");
		__builtin_unreachable ();
	}
	// the breadcrumb (crumb.h): which core, why, where - kept over the reboot
	crumb::Fault (nCore, (u32) pFrame->esr_el1, pFrame->elr_el1, pFrame->far_el1, "an exception outside a voice's render");
	g_Guard.Fatal ("an exception outside a voice's render");
	if (nCore != 0)
	{
		for (;;) asm volatile ("wfe");		// core 0 stops the stream and reboots
	}
	CObUsb::EmergencyStop ();			// core 0 itself: no more URBs; the watchdog reboots after the panic
	CExceptionHandler::Throw (nException, pFrame);
}
