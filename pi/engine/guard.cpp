// pi/engine/guard.cpp - see guard.h.
#include "guard.h"

#include <circle/multicore.h>
#include <circle/synchronize.h>
#if RASPPI >= 4
#include <circle/bcm2711.h>
#include <circle/interrupt.h>
#include <circle/memio.h>
#include <circle/memorymap.h>
#endif

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
	for (unsigned c = 2; c <= 3; c++)	// Plugin API 3: a guarded plugin call that faulted (once each)
	{
		if (m_nCtlLastPC[c])
		{
			pLogger->Write ("guard", LogWarning, "core %u: a plugin call faulted at PC 0x%lx, resumed %s (%u in all)", c,
					(unsigned long) m_nCtlLastPC[c], m_bCtlResumedOK[c] ? "at task level (EL1t)" : "NOT at task level",
					m_nCtlFaults);
			m_nCtlLastPC[c] = 0;
		}
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

// Plugin API 3: a fault inside a guarded plugin call on core 3 comes back here, at task level, as ResumeWorker does.
static void __attribute__ ((noinline, noreturn)) ResumeControl (void)
{
	GUARD_LONGJMP (g_Guard.ControlJumpBuffer (CMultiCoreSupport::ThisCore ()));
}

int CGuard::ControlCall (void (*pFunc) (void *), void *pParam, u64 *pPC)
{
	const unsigned c = CMultiCoreSupport::ThisCore ();
	int r = ControlCallOn (c, pFunc, pParam, pPC);
	if (r)		// the evidence the hardware check wants: back at task level, on SP_EL0, as the render guard's resume
	{
		u64 nSPSel;
		asm volatile ("mrs %0, spsel" : "=r" (nSPSel));
		m_bCtlResumedOK[c & 3] = CurrentExecutionLevel () == TASK_LEVEL && nSPSel == 0;
		m_nCtlLastPC[c & 3] = *pPC;
	}
	return r;
}

// The timed call's limit (Exec::guard_timed): core 3's EL1 virtual timer. Circle's CTimer has the physical one (CNTP,
// PPI 14) on core 0; nothing uses CNTV. Its PPI's enable and priority are banked per core, so SetupTimed runs on core 3.
#if RASPPI >= 4
static const unsigned kTimerIRQ = 16 + 11;		// GIC_PPI (11): CNTV
static volatile boolean s_bTimedReady = FALSE;

static void TimerStart (u32 nUs)
{
	u64 f;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (f));
	u64 n = (u64) nUs * f / 1000000;
	if (n == 0) n = 1;
	if (n > 0x7FFFFFFF) n = 0x7FFFFFFF;		// TVAL is a signed 32-bit down-count
	asm volatile ("msr cntv_tval_el0, %0\n"
		      "msr cntv_ctl_el0, %1\n"		// ENABLE, IMASK clear
		      "isb" :: "r" (n), "r" ((u64) 1) : "memory");
}

static void TimerStop (void)
{
	asm volatile ("msr cntv_ctl_el0, xzr\n"
		      "isb" ::: "memory");
}

// Core 3's CNTV interrupt, from Circle's InterruptHandler (IRQStub -> InterruptHandler -> CallIRQHandler), in EL1h on
// the core's exception stack, IRQs masked (FIQ unmasked by IRQStub).
static void TimerIRQ (void *)
{
	TimerStop ();			// first: the line is level-sensitive, it would come back after the EOI
	const unsigned nCore = CMultiCoreSupport::ThisCore ();
	if (nCore != 3) return;		// (the PPI is enabled on core 3 only)
	// IRQStub's frame, from the top of SP_EL1 (startup64.S: core n's is MEM_EXCEPTION_STACK + n * EXCEPTION_STACK_SIZE,
	// and an interrupt from EL1t finds it there, empty): stp x29, x30 at top - 16, then stp elr_el1, spsr_el1 at top - 32
	// (elr the lower word); its x0..x28 (and the q registers with SAVE_VFP_REGS_ON_IRQ) below that
	const u64 nTop = MEM_EXCEPTION_STACK + (u64) nCore * EXCEPTION_STACK_SIZE;
	u64 nSP;
	asm volatile ("mov %0, sp" : "=r" (nSP));
	if (nSP >= nTop || nSP < nTop - EXCEPTION_STACK_SIZE) return;	// not the stack we think (SP_EL1 set elsewhere): go on
	const volatile u64 *pFrame = (const volatile u64 *) (nTop - 32);
	const u64 nELR = pFrame[0], nSPSR = pFrame[1];
	if (!g_Guard.TakeTimeout (nCore, nELR, nSPSR)) return;	// a late one (the call returned) or not at task level: Circle EOIs
	// Leave the call as Throw does after a fault: eret into ResumeControl in the interrupted mode and DAIF (EL1t, IRQs
	// on), SP_EL1 back at its top; ResumeControl longjmps into ControlCallOn on SP_EL0. Circle's handler, which would EOI,
	// is not returned to: EOI here. FIQs masked again first, as IRQStub does before its own eret: an FIQ between the
	// msr's below and the eret would overwrite elr_el1 / spsr_el1 (FIQStub does not save them).
	asm volatile ("msr daifset, #1" ::: "memory");
	write32 (ARM_GICC_BASE + 0x010, kTimerIRQ);	// GICC_EOIR (a PPI: no CPU ID field)
	DataSyncBarrier ();
	asm volatile ("msr elr_el1, %0\n"
		      "msr spsr_el1, %1\n"
		      "mov sp, %2\n"
		      "eret"
		      :: "r" ((u64) &ResumeControl), "r" (nSPSR), "r" (nTop) : "memory");
	__builtin_unreachable ();
}

void CGuard::SetupTimed (void)
{
	if (CMultiCoreSupport::ThisCore () != 3 || s_bTimedReady) return;
	TimerStop ();			// (the firmware may leave it on: an interrupt before the first call would be a late one anyway)
	// this core's banked priority of the PPI: Circle's default 0xA0, as CInterruptSystem::Initialize set core 0's bank
	const uintptr nPrio = ARM_GICD_BASE + 0x400 + (kTimerIRQ / 4) * 4;
	const unsigned nShift = (kTimerIRQ % 4) * 8;
	write32 (nPrio, (read32 (nPrio) & ~(0xFFu << nShift)) | (0xA0u << nShift));
	// the handler (one table for all cores) and the enable, in this core's bank of GICD_ISENABLER0 (written from here)
	CInterruptSystem::Get ()->ConnectIRQ (kTimerIRQ, TimerIRQ, 0);
	s_bTimedReady = TRUE;
}

int CGuard::ControlCallTimed (void (*pFunc) (void *), void *pParam, u64 *pPC, u32 nUs)
{
	const unsigned c = CMultiCoreSupport::ThisCore ();
	if (c != 3 || !s_bTimedReady) return ControlCall (pFunc, pParam, pPC);
	int r = ControlCallTimedOn (c, pFunc, pParam, pPC, nUs, TimerStart, TimerStop);
	if (r)		// as ControlCall: back at task level, on SP_EL0?
	{
		u64 nSPSel;
		asm volatile ("mrs %0, spsel" : "=r" (nSPSel));
		m_bCtlResumedOK[3] = CurrentExecutionLevel () == TASK_LEVEL && nSPSel == 0;
		if (r == 1) m_nCtlLastPC[3] = *pPC;	// (a stop is logged by its caller, um.cpp)
	}
	return r;
}
#else
void CGuard::SetupTimed (void) {}
int CGuard::ControlCallTimed (void (*pFunc) (void *), void *pParam, u64 *pPC, u32)
{
	return ControlCall (pFunc, pParam, pPC);
}
#endif

static void SpinCall (void *pUs)	{ Spin (*(unsigned *) pUs); }

void CGuard::TimedTest (CLogger *pLogger)
{
	struct { unsigned nSpin, nLimit; int nWant; } T[2] = {{5000, 1000, 2}, {200, 1000, 0}};
	for (auto &t : T)
	{
		u64 nPC = 0;
		u64 t0 = Ticks ();
		int r = ControlCallTimed (SpinCall, &t.nSpin, &nPC, t.nLimit);
		unsigned nUs = (unsigned) ((Ticks () - t0) / TicksPerUs ());
		u64 nSPSel;
		asm volatile ("mrs %0, spsel" : "=r" (nSPSel));
		const boolean bTask = CurrentExecutionLevel () == TASK_LEVEL && nSPSel == 0;
		if (r == 2)
			pLogger->Write ("guard", LogNotice, "timedtest: a %u us spin, limit %u us: stopped after %u us at PC 0x%lx, "
					"resumed at task level: %s (%s)", t.nSpin, t.nLimit, nUs, (unsigned long) nPC,
					bTask ? "yes" : "no", r == t.nWant ? "ok" : "FAIL");
		else
			pLogger->Write ("guard", LogNotice, "timedtest: a %u us spin, limit %u us: returned %d after %u us, at task "
					"level: %s (%s)", t.nSpin, t.nLimit, r, nUs, bTask ? "yes" : "no", r == t.nWant ? "ok" : "FAIL");
	}
}

void CEngineExceptionHandler::Throw (unsigned nException, TAbortFrame *pFrame)
{
	unsigned nCore = CMultiCoreSupport::ThisCore ();
	if ((nCore == 2 || nCore == 3) && g_Guard.ControlArmed (nCore))
	{
		// a user plugin's guarded call faulted (API 3's guard): back into ControlCall, the slot muted by its caller
		g_Guard.NoteControlFault (nCore, pFrame->elr_el1);
#if RASPPI >= 4
		if (g_Guard.TimedArmed (nCore)) TimerStop ();	// a timed call: its timer must not fire on the way back
#endif
		u64 nSPSR = pFrame->spsr_el1, nStack = pFrame->sp_el1;		// read before the frame is dropped
		asm volatile ("msr elr_el1, %0\n"
			      "msr spsr_el1, %1\n"
			      "mov sp, %2\n"
			      "eret"
			      :: "r" ((u64) &ResumeControl), "r" (nSPSR), "r" (nStack) : "memory");
		__builtin_unreachable ();
	}
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
