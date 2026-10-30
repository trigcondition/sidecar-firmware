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
		u64 nMach[7];			// render ticks per machine (bridge::stat_name order, bridge::kMachines)
		u64 nVoice[8];			// touch2: render ticks per voice (the display's STATUS page)
		unsigned nBlocks, nMaxBlockUs, nMaxPassUs, nFifoMin, nFifoMax, nMidiBytes, nSysEx, nNotes, nNoteChans;
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
	static unsigned FifoTarget (void);
	// The control loop's longest gap between two pass starts since the last Output::Reset (a new stream), with where that
	// pass's time went: the sections midi keys cmds tick render idle (us each). at_frames: frames pulled since the Reset
	// when the gap ended; gaps_over_2ms: how many gaps were that long. Racy reads from core 0 (a diagnostic).
	struct TStall { unsigned gap_us, at_frames, gaps_over_2ms, sec_us[6]; };
	static TStall Stall (void);
	// The last kGaps gaps over 2 ms since the Reset, oldest first: {gap us, frames pulled at its end, us of that pass spent
	// in its wfe (gap minus it: the pass itself), cntkctl_el1 then (the event stream's bits), DAIF (interrupt masks),
	// the pass's longest section (0 midi, 1 keys, 2 cmds, 3 tick, 4 render, 5 idle) and its us}.
	static const unsigned kGaps = 8;
	struct TGap { unsigned gap_us, at_frames, wfe_us, kctl, daif, sec, sec_us; };
	static unsigned Gaps (TGap *p);			// returns how many (0..kGaps)
	// The SysEx messages from the Rytm that took core 3 over 1 ms, oldest first: {command (byte 4), the next byte (a
	// voice, a track..), length, us}.
	static const unsigned kSlowSx = 8;
	struct TSlowSx { unsigned cmd, arg, len, us; };
	static unsigned SlowSysex (TSlowSx *p);		// returns how many (0..kSlowSx)
	static boolean WaitPolled (void);		// :ctlwait=poll - the control loop spins instead of wfe (an A/B for the gaps)
	static boolean NotesOn (void);			// notes=, for the web page (read only)
	static int DroneVoice (void);			// -1 off, 0..7 one voice, 8 all
	// The transport's default FIFO target (kernel.cpp UseLink: usb FifoUsb, tcp FifoTcp), unless a fifo= (the
	// cmdline's, or the console's at any time) set it: that one stays until the next fifo=.
	static void LinkFifoTarget (unsigned nFrames);
	static void FixFifoTarget (void);		// the cmdline gave fifo=

private:
	void Worker (unsigned nCore);
	void Control (void);
	void RenderBlock (void);
	void Command (const char *pText);
};

#endif
