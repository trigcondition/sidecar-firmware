// pi/engine/guard.h - the voice guard (spec "Errors and safety"): each voice's render is timed. A voice is muted only
// when it hurts the audio: over its budget while its render core is overloaded (the core's previous block took at least
// kHotPct of a block), kOverBlocks blocks in a row (50 ms) - a burst (a machine starting, a poly switch, a preset load)
// never mutes (a burst longer than the 96-frame FIFO's ~8 blocks may glitch; it is not muted for it). Not seen: load
// spread over voices each under budget, and core 2's send FX after its block - those glitch, nothing mutes. A fault while a voice renders mutes it at once (the exception handler
// jumps back into its render core's loop). A muted voice is silent, shown as !<MACHINE> in the stats and MUTED on the
// touchscreen, until :unmute or until the voice's machine changes (a new machine starts clean: Fresh). Anything fatal
// (an exception elsewhere, a render core that does not finish a block) asks core 0 to stop the stream and reboot.
// The Teensy-era rule (25 % of a block, three blocks in a row) muted voices on the Pi that had plenty of room.
// The bookkeeping is inline and portable: tests/test_guard.cpp runs it on the Mac.
#ifndef _guard_h
#define _guard_h
#include <circle/exceptionhandler.h>
#include <circle/logger.h>
#include <circle/types.h>

class CGuard
{
public:
	static const unsigned Voices = 8;
	static const unsigned kOverBlocks = 200;	// blocks in a row over budget on an overloaded core: 50 ms
	static const unsigned kHotPct = 80;
	static_assert (kOverBlocks <= 255, "m_nOver is a u8");		// a core is overloaded when its block took this % of a block

	void SetBudget (unsigned nTicks)	{ m_nBudget = nTicks; }
	void SetBlock (unsigned nTicks)		{ m_nHot = (u32) ((unsigned long long) nTicks * kHotPct / 100); }	// a 12-frame block
	// a render core, after its block: whether it was overloaded (counts for its voices' next block)
	void CoreDone (unsigned nCore, u32 nTicks)	{ m_bHot[nCore & 3] = nTicks >= m_nHot; }
	unsigned Budget (void) const		{ return m_nBudget; }
	void Enter (unsigned nCore, unsigned nVoice);			// a render core, before the voice renders
	// after, with its time and its machine: the key Fresh compares - bridge::machine_name's static name and
	// bridge::machine_gen (a user voice's slot and the plugin loader's epoch: another plugin, or one reloaded, is new)
	void Leave (unsigned nCore, unsigned nVoice, u32 nTicks, const void *pMach = 0, u32 nGen = 0)
	{
		m_bArmed[nCore] = FALSE;
		if (nTicks > m_nMaxTicks[nVoice]) m_nMaxTicks[nVoice] = nTicks;
		if (nTicks > m_nBudget && m_bHot[nCore & 3])
		{
			if (m_nOver[nVoice] < kOverBlocks) m_nOver[nVoice]++;
			if (m_nOver[nVoice] >= kOverBlocks) Mute (nVoice, "over budget on an overloaded core", pMach, nGen);
		}
		else
		{
			m_nOver[nVoice] = 0;
		}
	}
	// a render core, before it skips a muted voice: its machine changed since the mute -> unmuted, true (render it)
	boolean Fresh (unsigned nVoice, const void *pMach, u32 nGen = 0)
	{
		if (!Muted (nVoice) || (pMach == m_pMuteMach[nVoice] && nGen == m_nMuteGen[nVoice])) return FALSE;
		m_nOver[nVoice] = 0;
		if (m_nOverVoice == nVoice) m_nOverBlocks = 0;	// a pending :overtest is the old machine's
		m_nMaxTicks[nVoice] = 0;
		__atomic_and_fetch (&m_nMuted, ~(1u << nVoice), __ATOMIC_RELEASE);
		__atomic_or_fetch (&m_nFreed, 1u << nVoice, __ATOMIC_RELEASE);
		return TRUE;
	}
	u32 TakeFreed (void)			{ return __atomic_exchange_n (&m_nFreed, 0, __ATOMIC_ACQ_REL); }	// core 0: log once
	boolean Muted (unsigned nVoice) const	{ return (m_nMuted >> nVoice) & 1; }
	u32 MutedMask (void) const		{ return m_nMuted; }
	u32 TakeNews (void)			{ return __atomic_exchange_n (&m_nNews, 0, __ATOMIC_ACQ_REL); }
	const char *Why (unsigned nVoice) const	{ return m_pWhy[nVoice]; }
	u32 MaxTicks (unsigned nVoice) const	{ return m_nMaxTicks[nVoice]; }
	boolean Armed (unsigned nCore) const	{ return m_bArmed[nCore]; }
	unsigned CurrentVoice (unsigned nCore) const { return m_nCurrent[nCore]; }	// set by Enter
	void **JumpBuffer (unsigned nCore)	{ return m_JmpBuf[nCore]; }
	void NoteFault (unsigned nCore, u64 nPC) { m_nFaultPC[nCore] = nPC; m_nFaults[nCore]++; }
	void NoteResumed (unsigned nCore, boolean bTaskLevel) { m_bResumedOK[nCore] = bTaskLevel; }	// after Recover
	unsigned Recover (unsigned nCore, const void *pMach = 0, u32 nGen = 0)	// after the jump: the voice that faulted, muted
	{
		m_bArmed[nCore] = FALSE;
		unsigned v = m_nCurrent[nCore];
		Mute (v, "crashed", pMach, nGen);
		return v;
	}
	void Fatal (const char *pWhy)		{ if (m_pFatal == 0) m_pFatal = pWhy; }
	const char *FatalWhy (void) const	{ return m_pFatal; }
	void LogNews (CLogger *pLogger);	// core 0: each new mute once
	void Unmute (void)			// the control core, between blocks; also ends a pending :overtest
	{
		m_nOverBlocks = 0;		// a muted voice's remaining test blocks did not run (it was not rendered)
		for (unsigned v = 0; v < Voices; v++) m_nOver[v] = 0, m_nMaxTicks[v] = 0;
		for (unsigned c = 0; c < 4; c++) m_bHot[c] = FALSE;
		__atomic_store_n (&m_nMuted, 0, __ATOMIC_RELEASE);
	}
	// tests (console): the voice's next render faults (a write to an unmapped address) / runs 300 us longer for
	// nBlocks blocks / runs 30 ms long once (a render core that misses the 20 ms block limit: fatal)
	void ArmFault (unsigned nVoice)		{ m_nFaultTest = nVoice + 1; }
	void ArmOverrun (unsigned nVoice, unsigned nBlocks) { m_nOverVoice = nVoice; m_nOverBlocks = nBlocks; }
	void ArmHang (unsigned nVoice)		{ m_nHangTest = nVoice + 1; }

private:
	void Mute (unsigned nVoice, const char *pWhy, const void *pMach, u32 nGen)
	{
		m_pWhy[nVoice] = pWhy;
		m_pMuteMach[nVoice] = pMach;
		m_nMuteGen[nVoice] = nGen;
		__atomic_or_fetch (&m_nMuted, 1u << nVoice, __ATOMIC_RELEASE);
		__atomic_or_fetch (&m_nNews, 1u << nVoice, __ATOMIC_RELEASE);
	}

	unsigned m_nBudget = 6750;		// 50 % of a 12-frame block at 54 MHz (cmdline voicebudget=)
	u32 m_nHot = 10800;			// kHotPct of a 12-frame block at 54 MHz (SetBlock)
	volatile boolean m_bHot[4] = {FALSE, FALSE, FALSE, FALSE};
	volatile u32 m_nMuted = 0, m_nNews = 0, m_nFreed = 0;
	const char *m_pWhy[Voices] = {0};
	const void *m_pMuteMach[Voices] = {0};
	u32 m_nMuteGen[Voices] = {0};
	u8 m_nOver[Voices] = {0};
	u32 m_nMaxTicks[Voices] = {0};
	volatile boolean m_bArmed[4] = {FALSE, FALSE, FALSE, FALSE};
	volatile unsigned m_nCurrent[4] = {0, 0, 0, 0};
	void *m_JmpBuf[4][5];			// __builtin_setjmp's five words, one buffer per core
	volatile u64 m_nFaultPC[4] = {0, 0, 0, 0};
	volatile unsigned m_nFaults[4] = {0, 0, 0, 0};
	volatile boolean m_bResumedOK[4] = {FALSE, FALSE, FALSE, FALSE};	// the worker back in EL1t, IRQs as before
	const char *volatile m_pFatal = 0;
	volatile unsigned m_nFaultTest = 0, m_nOverVoice = 0, m_nOverBlocks = 0, m_nHangTest = 0;
};

extern CGuard g_Guard;

class CEngineExceptionHandler : public CExceptionHandler
{
public:
	void Throw (unsigned nException, TAbortFrame *pFrame) override;
};

#endif
