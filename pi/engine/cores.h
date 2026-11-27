// pi/engine/cores.h - the engine's cores (spec "The Pi engine at run time"): cores 1 and 2 render voices 0-3 and
// 4-7 of each block (cmdline split=N moves the boundary); core 3 runs the control loop - the host's MIDI bytes and the console's commands into the
// engine core, blocks while the output FIFO is below its target, card work (DSP SAMPLE, the project) otherwise.
// Core 0 is the kernel's: USB, the console, the stats.
#ifndef _cores_h
#define _cores_h
#include <circle/memory.h>
#include <circle/multicore.h>
#include <circle/types.h>

class CEngineCores : public CMultiCoreSupport
{
public:
	struct TStats
	{
		u64 nTicks;			// generic-timer ticks since the last TakeStats
		u64 nBusy[4];			// ticks working, per core
		u64 nChain1;			// core 1's send FX and master chains: slot 1's (not in nBusy[1]: after its voices)
		u64 nChain2;			// core 2's: slot 0's send FX (its master chain too on an 8-voice engine; not in nBusy[2])
		u64 nChain3;			// core 3's: slot 0's master chain in a two-slot engine (inside nBusy[3], outside nWait3)
		u64 nWait3;			// core 3 waiting for cores 1 and 2 in RenderBlock (inside nBusy[3])
		u64 nSleep[4];			// ticks asleep in a counted wfe, per core
		unsigned nWakes[4];		// how many of those wfe returned
		u64 nMach[7];			// render ticks per machine (bridge::stat_name order, bridge::kMachines)
		u64 nVoice[16];			// touch2: render ticks per engine voice (bridge::kVoices; the STATUS page shows the base box's)
		unsigned nBlocks, nMaxBlockUs, nMaxPassUs, nFifoMin, nFifoMax, nMidiBytes, nSysEx, nNotes, nNoteChans;
		unsigned nRunCapped;	// control passes that stopped rendering at the cap, the FIFO still short (an overload)
		unsigned nFifoLate;	// base passes begun with the FIFO under a block (late for the busier render core)
		unsigned nFifoHeld1, nFifoHeld2;	// passes late by the FIFO (fifo_late.h) for core 1, core 2
		unsigned nFifoRunMax;	// the longest run of passes begun with a FIFO low
		unsigned nFifoFits1, nFifoFits2;	// passes the FIFO's hold left on time: core 1 / 2 within admission's limit, no underrun
	};

	// The boot bench (bridge::bench, docs/pi-engine.md "Measurements"): each machine on one voice, on every core
	// in turn - core 0 in the kernel's Initialize (BenchCore (0)), then cores 1, 2 and 3 as each starts, before
	// its loop (one core at a time: the engine core is not re-entrant).
	static const unsigned BenchMachines = 10;
	static const char *const BenchMachine[BenchMachines];	// plaits juno fm daisy dsps wt gr livein chop md
	struct TBench
	{
		u64 nTicks;		// render ticks for 1 s of audio (4000 blocks)
		u32 nWorst;		// the longest block's ticks
		int nPeak;		// voice 0's largest |sample| (0: the machine rendered silence)
		u32 nHash;		// its output's FNV-1a: two kernels' benches compared bit for bit
	};

	CEngineCores (CMemorySystem *pMemorySystem) : CMultiCoreSupport (pMemorySystem) {}
	static void Configure (unsigned nFifoTarget, boolean bNotes);
	static void SetSplit (unsigned nVoicesCore1);	// cmdline split=N: voices 0..N-1 on core 1, N..7 on core 2
	static void BenchCore (unsigned nCore);		// this core's bench (core 0 calls it; Run does for 1-3)
	static void BenchStart (boolean bOn);		// core 0, before Initialize: cores 1-3 bench in turn (or not)
	static boolean BenchWait (unsigned nMs);	// core 0: every core benched (FALSE: timed out)
	static unsigned BenchTurn (void);		// the core benching now (4: all done)
	static const TBench &BenchResult (unsigned nCore, unsigned nMachine);
	void Run (unsigned nCore) override;
	static void TakeStats (TStats *pStats);		// core 0
	static unsigned TakeRxLine (char *pOut, unsigned nMax);	// core 0: the Rytm report stream's last second as a log line (lagdiag.h), 0 none
	static unsigned FifoTarget (void);
	// The control loop's longest gap between two pass starts since the last Output::Reset (a new stream), with where that
	// pass's time went: the sections midi keys cmds tick render idle (us each). at_frames: frames pulled since the Reset
	// when the gap ended; gaps_over_2ms: how many gaps were that long. step_us, step_sum, here_us: of its longest section,
	// tick or idle (none: 0): its slowest named step, all its named steps, the control core's own time on the card
	// (cardq::here_take). Racy reads from core 0 (a diagnostic).
	struct TStall { unsigned gap_us, at_frames, gaps_over_2ms, sec_us[6], step_us, step_sum, here_us; };
	static TStall Stall (void);
	// Each transport's MIDI realtime as RealtimeIn took it (CK1, 2026-11-26: does a box's clock reach the Pi at all?):
	// counts since boot of F8, FA / FB, FC, F2, and of bytes whose transport carried no slot (dropped). Racy reads.
	struct TRealtime { unsigned nClock, nStart, nStop, nSongPos, nNoSlot; int nSlot; };
	static TRealtime Realtime (unsigned nTransport);
	static const char *StallStep (void);		// that slowest step's name, "" none. Kept here, /log's 64 KB are gone in 30 s
	// The last kGaps gaps over 2 ms since the Reset, oldest first: {gap us, frames pulled at its end, us of that pass spent
	// in its wfe (gap minus it: the pass itself), cntkctl_el1 then (the event stream's bits), DAIF (interrupt masks),
	// the pass's longest section (0 midi, 1 keys, 2 cmds, 3 tick, 4 render, 5 idle) and its us, then as TStall its slowest
	// step's name ("" none) and us, the named steps' us, the card's us on the control core}.
	static const unsigned kGaps = 8;
	struct TGap { unsigned gap_us, at_frames, wfe_us, kctl, daif, sec, sec_us; const char *step; unsigned step_us, step_sum, here_us; };
	static unsigned Gaps (TGap *p);			// returns how many (0..kGaps)
	// The SysEx messages from the Rytm that took core 3 over 1 ms, oldest first: {command (byte 4), the next byte (a
	// voice, a track..), length, us}.
	static const unsigned kSlowSx = 8;
	struct TSlowSx { unsigned cmd, arg, len, us; };
	static unsigned SlowSysex (TSlowSx *p);		// returns how many (0..kSlowSx)
	static boolean WaitPolled (void);		// :ctlwait=poll - the control loop spins instead of wfe (an A/B for the gaps)
	// Idle sleep (docs/pi-engine.md "Idle sleep"). idlewfe=0 on the cmdline, :idlewfe=0 on the console: core 3 spins while
	// the render cores work and core 0's loop waits in MsSleep, as before; the render cores' and the control loop's own wfe
	// were always there and stay.
	static void SetIdleWfe (boolean bOn);
	static boolean IdleWfe (void);
	// blocklimit=N on the cmdline (QEMU gates only, docs/pi-engine.md): a render core that has not finished its block
	// after N ms is fatal (default and minimum 20, the unit's; at most 10000). Returns the limit set.
	static unsigned SetBlockLimitMs (unsigned nMs);
	static void EventStream (void);			// the calling core: a wfe sleeps 152 us at most
	static void Sleep (void);			// core 0: one counted sleep (idlewait::nap)
	static boolean NotesOn (void);			// notes=, for the web page (read only)
	static int DroneVoice (void);			// -1 off, 0..7 one voice, 8 all
	// The transport's default FIFO target (kernel.cpp UseLink: usb FifoUsb, tcp FifoTcp), unless a fifo= (the
	// cmdline's, or the console's at any time) set it: that one stays until the next fifo=.
	static void LinkFifoTarget (unsigned nFrames);
	static void FixFifoTarget (void);		// the cmdline gave fifo=

private:
	void Worker (unsigned nCore);
	void Control (void);
	void RenderBlock (int nSide);		// -1 the base box's block; a slot: the side stage's (side_out.h)
	void Command (const char *pText);
	static void SlotCommand (void *pArg);	// slot=S:<command> in slot S's Scope (cores.cpp, bridge::in_slot)
};

#endif
