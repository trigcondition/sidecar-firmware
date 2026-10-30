// pi/engine/cores.cpp - see cores.h. The block handshake: core 3 bumps s_nGen and signals (sev); cores 1 and 2
// render their halves of s_Block and publish s_nDone; core 3 waits (spinning, with a 20 ms limit), then pushes the
// block into the output stage. Nothing else touches the engine core while a block renders.
#include "cores.h"
#include "crumb.h"	// the crash breadcrumb: each core's heartbeat and section (2026-10-29)

#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/util.h>

#include "bridge.h"
#include "guard.h"
#include "midi_in.h"
#include "notes.h"
#include "paircode_pi.h"
#include "sendfx_pi.h"
#include "rec_pi.h"
#include "shared.h"
#include "user/lists.h"
#include "user/um.h"
#if SIDECAR_SEQED
#include "seqed_pi.h"
#endif

static const char From[] = "cores";

static inline u64 Ticks (void)
{
	u64 t;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t) :: "memory");
	return t;
}
static inline u64 TickRate (void)
{
	u64 f;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (f));
	return f;
}

static s16 s_Block[bridge::kBlock][bridge::kVoices];
static s16 s_BlockR[bridge::kBlock][bridge::kVoices];	// the voices' R (core/fx/out.h: PRE / POST voices in stereo)
static u8 s_St[bridge::kVoices];			// a voice's pair is a real stereo pair this block
static volatile u32 s_nGen;
static volatile u32 s_nDone[3];
static u64 s_Mach[3][bridge::kMachines];
static_assert (sizeof CEngineCores::TStats::nMach / sizeof (u64) == bridge::kMachines, "nMach is bridge::kMachines");
static u64 s_VoiceTicks[bridge::kVoices];		// touch2: per voice (each voice renders on one core only)
static volatile u64 s_nBusy[4];
static volatile unsigned s_nBlocks, s_nMaxBlock, s_nMaxPass, s_nFifoMin = ~0u, s_nFifoMax, s_nMidiBytes, s_nNotes;
static volatile unsigned s_nNoteChans;	// bit c: a note-on on MIDI channel c+1 this second
static u64 s_nLastTake;
static unsigned s_nTarget = 96;
static volatile boolean s_bTargetFixed = FALSE;	// a fifo= set it: the link's default does not
static boolean s_bNotes = TRUE;
static boolean s_bReportLog = FALSE;
static int s_nDrone = -1;			// -1 off, 0..7 one voice, 8 all
static unsigned s_nSplit = 4;			// voices 0..s_nSplit-1 on core 1, the rest on core 2
static volatile u32 s_nBenchTurn = 4;		// the core whose bench runs now (4: none, every core done)
static CEngineCores::TBench s_Bench[4][CEngineCores::BenchMachines];
static u64 s_nDroneAt;

// The SysEx messages that took core 3 over 1 ms, the last kSlowSx: {command byte, the byte after it, length, us}. The
// pattern-switch gap (~41 ms in the midi section, 2026-10-26) is one of these; /status "slowSx" names it.
static volatile unsigned s_SlowSx[CEngineCores::kSlowSx][4], s_nSlowSx;
unsigned CEngineCores::SlowSysex (TSlowSx *p)
{
	unsigned n = s_nSlowSx, k = n < kSlowSx ? n : kSlowSx;
	for (unsigned i = 0; i < k; i++)
	{
		const volatile unsigned *r = s_SlowSx[(n - k + i) % kSlowSx];
		p[i].cmd = r[0], p[i].arg = r[1], p[i].len = r[2], p[i].us = r[3];
	}
	return k;
}

static void ToCore (const u8 *pData, int nLength)
{
	if (s_bReportLog && nLength >= 23 && (pData[4] == 0x04 || pData[4] == 0x03))
	{
		const u8 *p = pData + (pData[4] == 0x04 ? 8 : 6);
		CLogger::Get ()->Write (From, LogNotice, "report %02x voice %u flags %02x P1..P4 %u %u %u %u", pData[4],
					pData[5] & 7, pData[5] & 0x78, p[0], p[2], p[4], p[6]);
	}
	u64 t0 = Ticks ();
	bridge::sysex (pData, nLength);
	unsigned nUs = (unsigned) ((Ticks () - t0) * 1000000 / TickRate ());
	if (nUs > 1000)
	{
		volatile unsigned *r = s_SlowSx[s_nSlowSx % CEngineCores::kSlowSx];
		r[0] = nLength > 4 ? pData[4] : 0, r[1] = nLength > 5 ? pData[5] : 0, r[2] = (unsigned) nLength, r[3] = nUs;
		s_nSlowSx++;
	}
}
static core::NoteAdapter s_Notes (ToCore);
// A Rytm running the mods (any F0 7D 52 54 from it) reports its voices itself (0021's 03 / 04, 0026's 33): the note
// adapter (a stock Rytm's notes on channels 1..8 as PLAITS reports) then steps aside, or each MIDI note the Rytm
// sends out over USB turned the voice it just set to JUNO / DSP SAMPLE / ... back into the adapter's PLAITS (the
// sound, and the voice pages' waveform / GRAIN view, which read the voice's state).
static volatile boolean s_bModded = FALSE;
static void FromRytm (const u8 *pData, int nLength)
{
	if (nLength >= 5 && pData[1] == 0x7D && pData[2] == 0x52 && pData[3] == 0x54) s_bModded = TRUE;
	ToCore (pData, nLength);
}
static void OnChannel (u8 s, u8 a, u8 b)
{
	if ((s & 0xF0) == 0x90 && b) { s_nNotes++; s_nNoteChans |= 1u << (s & 0x0F); }
	bridge::lfo_midi (s, a, b);		// the LFOs: a Rytm track's trig (an FX target's TRIG, core/lfo.h)
	bridge::poly_midi (s, a, b);		// poly FM: the pads' notes, live chords (core/poly.h)
	if (s_bNotes && !s_bModded) s_Notes.Channel (s, a, b);
	else if ((s & 0xF0) == 0xB0 && a == 120) sendfx_pi::AllSoundOff ();	// the send FX hear it with the notes off too
}
static MidiIn s_Midi (FromRytm, OnChannel, rec_pi::Transport);
// The Pi's own keyboards' notes for the Rytm (core/keys.h: an analog track's, a forwarded one's): one USB MIDI event
// packet each into g_KeysToRytm (core 0's transport sends them between whole messages); a full queue drops it
static volatile unsigned s_nKeysLost;
static void KeysToRytm (u8 s, u8 a, u8 b)
{
	u32 w = (u32) (s >> 4) | (u32) s << 8 | (u32) a << 16 | (u32) b << 24;
	if (!g_KeysToRytm.push (w)) s_nKeysLost++;
}
// The auto-sampler's MIDI outputs for an external synth (core/samp/autosamp.h AMidiOut; port 0 kPortUsb the computer,
// 1 kPortDev a USB-A device; kPortRytm is the core's own to_rytm): one packet each into g_AsampMidi for core 0's loop;
// n 0 asks whether the output is there (DEV: the core checks a device is plugged in)
static volatile unsigned s_nAsampLost;
static bool AsampMidiOut (int nPort, const uint8_t *m, int n)
{
	if (nPort != 0 && nPort != 1) return FALSE;
	if (n == 0) return nPort == 0 ? g_bComputerMidi : TRUE;
	if (n != 3 || m[0] < 0x80 || m[0] >= 0xF0) return FALSE;
	u32 w = (u32) ((nPort + 1) << 4 | m[0] >> 4) | (u32) m[0] << 8 | (u32) m[1] << 16 | (u32) m[2] << 24;
	if (!g_AsampMidi.push (w)) { s_nAsampLost++; return FALSE; }
	return TRUE;
}
static void KeysIn (u32 w)
{
	int src = (int) (w >> 24);
	u8 s = (u8) (w >> 16), a = (u8) (w >> 8), b = (u8) w;
	if (s == 0xF4) bridge::keys_source (src, a != 0, src < 5 ? g_KeyNames[src] : "");
	else bridge::keys_midi (src, s, a, b);
}	// PLAY/STOP: an armed recorder; clock / SPP: STUTTER's grid

static volatile CEngineCores::TStall s_Stall;
CEngineCores::TStall CEngineCores::Stall (void)
{
	TStall s;
	for (unsigned i = 0; i < sizeof s / sizeof (unsigned); i++) ((unsigned *) &s)[i] = ((volatile unsigned *) &s_Stall)[i];
	return s;
}

static volatile unsigned s_Gaps[CEngineCores::kGaps][7], s_nGaps;
static volatile boolean s_bPoll;
boolean CEngineCores::WaitPolled (void) { return s_bPoll; }
unsigned CEngineCores::Gaps (TGap *p)
{
	unsigned n = s_nGaps, k = n < kGaps ? n : kGaps;
	for (unsigned i = 0; i < k; i++)
	{
		const volatile unsigned *g = s_Gaps[(n - k + i) % kGaps];
		p[i].gap_us = g[0], p[i].at_frames = g[1], p[i].wfe_us = g[2], p[i].kctl = g[3], p[i].daif = g[4];
		p[i].sec = g[5], p[i].sec_us = g[6];
	}
	return k;
}

static unsigned ClampTarget (unsigned n) { return n < 24 ? 24 : n > 1024 ? 1024 : n; }

boolean CEngineCores::NotesOn (void)	{ return s_bNotes; }
int CEngineCores::DroneVoice (void)		{ return s_nDrone; }

void CEngineCores::Configure (unsigned nFifoTarget, boolean bNotes)
{
	s_nTarget = ClampTarget (nFifoTarget);
	g_Output.SetFifoTarget (s_nTarget);
	s_bNotes = bNotes;
	if (s_nLastTake == 0) s_nLastTake = Ticks ();	// the first stats line's interval: from the boot's Configure
}

unsigned CEngineCores::FifoTarget (void) { return s_nTarget; }


// core 0 (UseLink): a single aligned word, read by core 3 each pass
void CEngineCores::LinkFifoTarget (unsigned n)
{
	if (!s_bTargetFixed) s_nTarget = ClampTarget (n), g_Output.SetFifoTarget (s_nTarget);
}

void CEngineCores::FixFifoTarget (void) { s_bTargetFixed = TRUE; }

void CEngineCores::SetSplit (unsigned n) { s_nSplit = n < 1 ? 1 : n > 7 ? 7 : n; }

// wt gr: WAVETABLE (3 unison oscillators, SYNC) and GRANULAR (the built-in PAD, ~9 grains at once): notes.cpp's defaults;
// livein: LIVE IN on a synthetic AUDIO IN (bridge::bench feeds it), the gate on; chop: CHOP's 8 slice players (VOICES' most)
// on a synthetic break in RAM, looped, MORPH half way (bridge::bench sets them up)
const char *const CEngineCores::BenchMachine[BenchMachines] = {"plaits", "juno", "fm", "daisy", "dsps", "wt", "gr", "livein", "chop", "md"};

void CEngineCores::BenchCore (unsigned nCore)
{
	for (unsigned m = 0; m < BenchMachines; m++)
	{
		s_Bench[nCore][m].nTicks = bridge::bench (BenchMachine[m], &s_Bench[nCore][m].nWorst,
						       &s_Bench[nCore][m].nPeak);
	}
}

void CEngineCores::BenchStart (boolean bOn)
{
	__atomic_store_n (&s_nBenchTurn, bOn ? 1 : 4, __ATOMIC_RELEASE);
}

boolean CEngineCores::BenchWait (unsigned nMs)
{
	u64 t0 = Ticks (), nLimit = TickRate () / 1000 * nMs;
	while (__atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE) != 4)
	{
		if (Ticks () - t0 > nLimit) return FALSE;
	}
	return TRUE;
}

unsigned CEngineCores::BenchTurn (void) { return __atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE); }

const CEngineCores::TBench &CEngineCores::BenchResult (unsigned nCore, unsigned nMachine)
{
	return s_Bench[nCore & 3][nMachine % BenchMachines];
}

void CEngineCores::Run (unsigned nCore)
{
	// the boot bench: cores 1, 2, 3 in turn (core 0 benched before they started), then each core's loop
	while (__atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE) < nCore) asm volatile ("wfe");
	if (__atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE) == nCore)
	{
		BenchCore (nCore);
		__atomic_store_n (&s_nBenchTurn, nCore + 1, __ATOMIC_RELEASE);
		asm volatile ("dsb sy; sev");
	}
	if (nCore == 1 || nCore == 2) Worker (nCore);
	else if (nCore == 3) Control ();
	// core 0: the kernel's own loop
}

void CEngineCores::Worker (unsigned nCore)
{
	const unsigned v0 = nCore == 1 ? 0 : s_nSplit, v1 = nCore == 1 ? s_nSplit : bridge::kVoices;
	// static: they survive the guard's longjmp
	static volatile u32 s_nSeen[3];		// the last block generation this core finished
	static volatile u32 s_nGenNow[3];	// the one it renders
	static volatile unsigned s_nResume[3];	// after a fault: the voice to go on with (0: none)
	static volatile u64 s_nT0[3];		// the block's start, for the busy time
	CLogger::Get ()->Write (From, LogNotice, "core %u up: renders voices %u-%u", nCore, v0, v1 - 1);
	if (__builtin_setjmp (g_Guard.JumpBuffer (nCore)))
	{
		// a fault in a voice's render (guard.cpp returned from the exception into here, at task level): that
		// voice is muted and silent in this block; the block's voices before it keep what they rendered, the
		// ones after it render below
		const int vc = (int) g_Guard.CurrentVoice (nCore);
		unsigned v = g_Guard.Recover (nCore, bridge::machine_name (vc), bridge::machine_gen (vc));
		for (unsigned s = 0; s < bridge::kBlock; s++) s_Block[s][v] = s_BlockR[s][v] = 0;
		u64 nSPSel;
		asm volatile ("mrs %0, spsel" : "=r" (nSPSel));
		g_Guard.NoteResumed (nCore, CurrentExecutionLevel () == TASK_LEVEL && nSPSel == 0);
		s_nResume[nCore] = v + 1;
	}
	for (;;)
	{
		u32 nGen;
		unsigned vFirst;
		if (s_nResume[nCore])
		{
			nGen = s_nGenNow[nCore];
			vFirst = s_nResume[nCore];
			s_nResume[nCore] = 0;
			crumb::BeatEnter (nCore, 200);	// (back in the block a recovered fault left)
		}
		else
		{
			crumb::Enter (201);
			while ((nGen = __atomic_load_n (&s_nGen, __ATOMIC_ACQUIRE)) == s_nSeen[nCore]) asm volatile ("wfe");
			crumb::BeatEnter (nCore, 200);
			s_nGenNow[nCore] = nGen;
			s_nT0[nCore] = Ticks ();
			vFirst = v0;
		}
		for (unsigned v = vFirst; v < v1; v++)
		{
			const void *pMach = bridge::machine_name ((int) v);	// the guard's machine key: a static name,
			const u32 nMGen = bridge::machine_gen ((int) v);	// and a user plugin's slot and load
			if (g_Guard.Muted (v) && !g_Guard.Fresh (v, pMach, nMGen))
			{
				for (unsigned s = 0; s < bridge::kBlock; s++) s_Block[s][v] = s_BlockR[s][v] = 0;
				continue;
			}
			u64 t0 = Ticks ();
			g_Guard.Enter (nCore, v);
			bridge::render (s_Block, v, v + 1, s_Mach[nCore], s_BlockR, s_St);
			u64 dt = Ticks () - t0;
			g_Guard.Leave (nCore, v, (u32) dt, pMach, nMGen);
			s_VoiceTicks[v] += dt;
		}
		const u64 nBlockTicks = Ticks () - s_nT0[nCore];
		g_Guard.CoreDone (nCore, (u32) nBlockTicks);	// overloaded? (the guard counts a voice's overs only then)
		s_nBusy[nCore] += nBlockTicks;
		s_nSeen[nCore] = nGen;
		__atomic_store_n (&s_nDone[nCore], nGen, __ATOMIC_RELEASE);
		asm volatile ("dsb sy; sev");
		if (nCore == 2) sendfx_pi::Render ();	// the send FX, after the block (sendfx_pi.h)
	}
}

void CEngineCores::RenderBlock (void)
{
	bridge::seq_block ();			// the sequencer's notes due in this block (core/seq/seq.h)
	u32 nGen = s_nGen + 1;
	__atomic_store_n (&s_nGen, nGen, __ATOMIC_RELEASE);
	asm volatile ("dsb sy; sev");
	u64 t0 = Ticks (), nLimit = TickRate () / 50;		// 20 ms
	while (__atomic_load_n (&s_nDone[1], __ATOMIC_ACQUIRE) != nGen
	       || __atomic_load_n (&s_nDone[2], __ATOMIC_ACQUIRE) != nGen)
	{
		if (Ticks () - t0 > nLimit)
		{
			g_Guard.Fatal ("a render core did not finish its block in 20 ms");
			for (;;) asm volatile ("wfe");		// core 0 stops the stream and reboots
		}
	}
	unsigned nUs = (unsigned) ((Ticks () - t0) * 1000000 / TickRate ());
	if (nUs > s_nMaxBlock) s_nMaxBlock = nUs;
	u8 nSt = 0;
	for (unsigned v = 0; v < bridge::kVoices; v++) nSt |= s_St[v] ? 1u << v : 0;
	g_Output.Push (s_Block, bridge::kBlock, s_BlockR, nSt);
	s_nBlocks++;
}

// The engine's own notes for tests (:drone=V or all): every 0.5 s a note-off and a note-on (note 48 + 2v) on the
// voice(s), through the note adapter (each voice's machine as :notemach set it).
static void Drone (void)
{
	u64 now = Ticks ();
	if (s_nDrone < 0 || now - s_nDroneAt < TickRate () / 2) return;
	s_nDroneAt = now;
	for (int v = 0; v < 8; v++)
	{
		if (s_nDrone != 8 && s_nDrone != v) continue;
		s_Notes.Channel (0x80 | v, 48 + 2 * v, 0);
		s_Notes.Channel (0x90 | v, 48 + 2 * v, 100);
	}
}

void CEngineCores::Control (void)
{
	// the Rytm's SIDECAR MENU > WEB UI (core/ui/page_menu.cpp): paircode_pi's, which only asks core 0 (Poll)
	bridge::webui_init (paircode_pi::WebOn, paircode_pi::MenuWeb, paircode_pi::MenuGate, paircode_pi::MenuDevices,
			    paircode_pi::MenuNewCode, paircode_pi::MenuForget);
	// The generic timer's event stream on this core: an event every 2^13 counter ticks (152 us at the Pi 4's
	// 54 MHz), so the wfe below never sleeps longer than that. A sev from core 0 (a pull from the FIFO) can be
	// consumed by a wfe inside this core's own work (a spinlock: the logger, the card), and the next sev may be
	// a poll of core 0 away (1.2..3.3 ms measured on the TCP link): the FIFO then stayed empty and the link sent
	// silence (docs/pi-engine.md "FIFO target").
	u64 nKctl;
	asm volatile ("mrs %0, cntkctl_el1" : "=r" (nKctl));
	nKctl = (nKctl & ~(u64) 0xF0) | (12 << 4) | (1 << 2);	// EVNTI 12 (an event as bit 12 rises: every 8192 ticks), EVNTEN
	asm volatile ("msr cntkctl_el1, %0; isb" : : "r" (nKctl));
	bridge::keys_set_forward (KeysToRytm);
	bridge::asamp_set_midi_out (AsampMidiOut);
	CLogger::Get ()->Write (From, LogNotice, "core 3 up: control (fifo target %u frames, MIDI notes %s)", s_nTarget,
				s_bNotes ? "on" : "off");
	u64 nPrevT0 = 0, nPrevWfe = 0;
	unsigned nSec[6] = {}, nStreams = 0;
	for (;;)
	{
		u64 t0 = Ticks ();
		crumb::Beat (3);
		crumb::Enter (100);
		{	// the gap since the last pass began, blamed on that pass's sections (small: it ran; all zero: the core was away)
			if (g_Output.Streams () != nStreams)	// a new stream: the record starts over
			{
				nStreams = g_Output.Streams ();
				for (unsigned i = 0; i < sizeof s_Stall / sizeof (unsigned); i++) ((volatile unsigned *) &s_Stall)[i] = 0;
				s_nGaps = 0;
			}
			unsigned nGap = nPrevT0 ? (unsigned) ((t0 - nPrevT0) * 1000000 / TickRate ()) : 0;
			if (nGap > 2000)
			{
				s_Stall.gaps_over_2ms++;
				u64 nKc, nDaif;
				asm volatile ("mrs %0, cntkctl_el1" : "=r" (nKc));
				asm volatile ("mrs %0, daif" : "=r" (nDaif));
				volatile unsigned *g = s_Gaps[s_nGaps % kGaps];
				g[0] = nGap, g[1] = g_Output.Diagnostics ().since_reset, g[2] = (unsigned) (nPrevWfe * 1000000 / TickRate ());
				g[3] = (unsigned) nKc, g[4] = (unsigned) nDaif;
				unsigned m = 0;			// the section that took the pass's time
				for (unsigned i = 1; i < 6; i++) if (nSec[i] > nSec[m]) m = i;
				g[5] = m, g[6] = nSec[m];
				s_nGaps++;
			}
			if (nGap > s_Stall.gap_us)
			{
				s_Stall.gap_us = nGap, s_Stall.at_frames = g_Output.Diagnostics ().since_reset;
				for (unsigned i = 0; i < 6; i++) s_Stall.sec_us[i] = nSec[i];	// the pass that just ended: the gap is its time
			}
			nPrevT0 = t0;
			for (unsigned i = 0; i < 6; i++) nSec[i] = 0;
		}
		u64 tS = t0;
		auto Mark = [&] (unsigned i) { u64 t = Ticks (); nSec[i] = (unsigned) ((t - tS) * 1000000 / TickRate ()); tS = t; crumb::Enter (101 + i); };
		boolean bWorked = FALSE;
		u8 b;
		while (g_MidiIn.pop (&b))
		{
			bridge::rytm_byte (b);	// the Rytm pattern dump's reader first (core/seq/rytm_dump.h): its answer outgrows MidiIn
			s_Midi.Feed (b);
			s_nMidiBytes++;
			bWorked = TRUE;
		}
		Mark (0);
		u32 nKey;
		while (g_KeysIn.pop (&nKey))
		{
			KeysIn (nKey);
			bWorked = TRUE;
		}
		Mark (1);
		TCmd Cmd;
		while (g_Cmds.pop (&Cmd))
		{
			Command (Cmd.text);
			bWorked = TRUE;
		}
		Mark (2);
		Drone ();
		bridge::tick ();
		// the pairing code on the Rytm's screen when it was asked for (paircode_pi.h): the remote UI is this
		// core's, and TakeShow only reads the code, which never changes after the boot
		char PairLine[24];
		static unsigned s_nPairGen = 0;		// the SIDECAR page's PAIR line: once Init decided it, again at a NEW CODE
		if (paircode_pi::StatusLine (PairLine, sizeof PairLine, &s_nPairGen))
		{
			bridge::pair_code (PairLine);
			memset (PairLine, 0, sizeof PairLine);
		}
		static uint32_t s_nHellos = 0;			// a new hello (a connect): the code on the Rytm again
		if (bridge::rytm_hellos () != s_nHellos)
		{
			s_nHellos = bridge::rytm_hellos ();
			paircode_pi::RytmHello ();
		}
		if (paircode_pi::TakeHide ()) bridge::pair_popup_end ();	// a device paired: off the Rytm's screen
		if (paircode_pi::TakeShow (PairLine, sizeof PairLine))
		{
			if (bridge::pair_popup (PairLine)) paircode_pi::ShowDone ();
			else if (bridge::rytm_greeted ()) paircode_pi::ShowFailed ();	// it would not: retried, then said
			// else no Rytm yet: it stays owed (a try a second, none counted) until the Rytm's hello
		}
		Mark (3);			// the tick section ends here: Drone, bridge::tick and the pair popup
		unsigned nLevel = g_Output.Level ();
		if (nLevel < s_nFifoMin) s_nFifoMin = nLevel;
		if (nLevel > s_nFifoMax) s_nFifoMax = nLevel;
		while (g_Output.Level () + bridge::kBlock <= s_nTarget)
		{
			RenderBlock ();
			bWorked = TRUE;
		}
		Mark (4);
		if (bridge::idle_prepare () || bridge::idle_save ()) bWorked = TRUE;
		else
		{
			bridge::idle_stream ();
			if (um::idle ()) bWorked = TRUE;	// the user machines: one bounded step (an 8 KB read at most)
			lists::idle ();			// the pool / lists JSON for the web page, when it changed (<= 4 a second)
		}
		Mark (5);
		if (bWorked)
		{
			u64 dt = Ticks () - t0;
			s_nBusy[3] += dt;
			unsigned nUs = (unsigned) (dt * 1000000 / TickRate ());	// the longest pass: the FIFO's refill waits that long
			if (nUs > s_nMaxPass) s_nMaxPass = nUs;
		}
		else
		{
			u64 tW = Ticks ();
			if (s_bPoll) for (u64 u = tW; Ticks () - u < TickRate () / 20000;) {}	// 50 us spin: no wfe
			else asm volatile ("wfe");	// woken by the OUT fill, a MIDI byte or a command (sev)
			nPrevWfe = Ticks () - tW;
		}
	}
}

// The console's commands (console.cpp: ':' lines), run here so the engine core is touched by one core only.
void CEngineCores::Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	if (strncmp (p, "notemach=", 9) == 0 && p[9] >= '0' && p[9] <= '7' && p[10] == ':')
	{
		int v = p[9] - '0';
		if (s_Notes.SetMachine (v, p + 11)) L->Write (From, LogNotice, "notes: voice %d plays %s", v, p + 11);
		else L->Write (From, LogWarning, "notes: no machine \"%s\" (plaits juno daisy dsps fm md)", p + 11);
	}
	else if (strncmp (p, "drone=", 6) == 0)
	{
		s_nDrone = strcmp (p + 6, "all") == 0 ? 8 : strcmp (p + 6, "off") == 0 ? -1 : atoi (p + 6);
		if (s_nDrone > 8) s_nDrone = -1;
		L->Write (From, LogNotice, "drone %s", p + 6);
	}
	else if (strncmp (p, "notes=", 6) == 0)
	{
		s_bNotes = p[6] == '1';
		L->Write (From, LogNotice, "MIDI notes %s%s", s_bNotes ? "on" : "off", s_bNotes && s_bModded ? " (idle: the Rytm runs the mods and reports its voices)" : "");
	}
	else if (strcmp (p, "out=main") == 0 || strcmp (p, "out=tracks") == 0)	// (out=Vn:...: the OUT route, core/fx/out.h)
	{
		g_Output.SetMode (strcmp (p + 4, "tracks") == 0 ? Output::kTracks : Output::kMain);
		L->Write (From, LogNotice, "output: %s", g_Output.GetMode () == Output::kTracks ? "tracks" : "main");
	}
	else if (strncmp (p, "fifo=", 5) == 0)
	{
		Configure (atoi (p + 5), s_bNotes);
		s_bTargetFixed = TRUE;		// kept across link switches
		L->Write (From, LogNotice, "fifo target %u frames", s_nTarget);
	}
	else if (strncmp (p, "ctlwait=", 8) == 0)
	{
		s_bPoll = strcmp (p + 8, "poll") == 0;
		L->Write (From, LogNotice, "control loop waits by %s", s_bPoll ? "polling" : "wfe");
	}
	else if (strcmp (p, "unmute") == 0)
	{
		g_Guard.Unmute ();
		L->Write (From, LogNotice, "guard: every voice unmuted");
	}
	else if (strncmp (p, "faulttest=", 10) == 0)
	{
		g_Guard.ArmFault (atoi (p + 10) & 7);
		L->Write (From, LogNotice, "guard: voice %d faults on its next render", atoi (p + 10) & 7);
	}
	else if (strncmp (p, "overtest=", 9) == 0)
	{
		g_Guard.ArmOverrun (atoi (p + 9) & 7, CGuard::kOverBlocks + 20);
		L->Write (From, LogNotice, "guard: voice %d runs 300 us long for %u blocks", atoi (p + 9) & 7, CGuard::kOverBlocks + 20);
	}
	else if (strncmp (p, "hangtest=", 9) == 0)
	{
		g_Guard.ArmHang (atoi (p + 9) & 7);
		L->Write (From, LogNotice, "guard: voice %d runs 30 ms long once (fatal: clean stop, reboot)", atoi (p + 9) & 7);
	}
	else if (strncmp (p, "pool=", 5) == 0 || strncmp (p, "juno=", 5) == 0 || strncmp (p, "fm=", 3) == 0)
	{
		char Out[160];		// the pool and the JUNO / FM lists (core/user/lists.h)
		lists::command (p, Out, sizeof Out);
		L->Write (From, LogNotice, "%s", Out);
	}
	else if (strncmp (p, "reportlog=", 10) == 0)
	{
		s_bReportLog = p[10] == '1';
		L->Write (From, LogNotice, "report log %s", s_bReportLog ? "on" : "off");
	}
	else if (char Fx[120]; bridge::fxin_command (p, Fx, sizeof Fx))	// fxin, fxin=1|0|?: IN L/R source (core/host_link.h)
	{
		L->Write (From, LogNotice, "%s", Fx);
	}
	else if (char Lo[200]; bridge::lfo_command (p, Lo, sizeof Lo))	// lfo, lfo=N:KEY:VALUE: the LFOs (core/lfo.h)
	{
		L->Write (From, LogNotice, "%s", Lo);
	}
	else if (sendfx_pi::Command (p))		// sendfx, sendfxout=, sendlvl=, sendknob= (sendfx_pi.h)
	{
	}
	else if (strncmp (p, "um", 2) == 0 && (p[2] == 0 || p[2] == '='))	// the user machines (core/user/um.h)
	{
		char Out[120];
		if (um::command (p, Out, sizeof Out)) L->Write (From, LogNotice, "um: %s", Out);
		else L->Write (From, LogWarning, "unknown command \"%s\" (um, um=rescan, um=assign:S:ID, um=clear:S, um=save:S:NAME, um=presets)", p);
	}
	else if (bridge::rui_command (p))		// rui, rui=on|off: the remote UI (core/ui/remote.h)
	{
	}
	else if (char Sd[200]; strncmp (p, "sdtest", 6) == 0
		 && bridge::sdtest_command (p, [] { unsigned n; return rec_pi::UiState (&n) == 2; } (), Sd, sizeof Sd))	// the SD card test (core/msamp/sdtest.h)
	{
		L->Write (From, LogNotice, "%s", Sd);
	}
	else if (char Sa[480]; (strncmp (p, "samp", 4) == 0 || strncmp (p, "asamp", 5) == 0) && bridge::samp_command (p, Sa, sizeof Sa))
	{
		L->Write (From, LogNotice, "%s", Sa);	// SAMPLING, the AUTO-SAMPLER (core/samp)
	}
	else if (char Po[480]; bridge::poly_command (p, Po, sizeof Po))	// poly: poly FM's notes and live chords (core/poly.h)
	{
		L->Write (From, LogNotice, "%s", Po);
	}
#if SIDECAR_SEQED
	else if (char Sq[160]; seqed_pi::Command (p, Sq, sizeof Sq))	// seqed=...: the display's pattern edits (docs/TOUCH-EDITOR.md)
	{
		L->Write (From, LogNotice, "%s", Sq);
	}
#endif
	else
	{
		L->Write (From, LogWarning, "unknown command \"%s\"", p);
	}
}

void CEngineCores::TakeStats (TStats *s)
{
	u64 now = Ticks ();
	s->nTicks = now > s_nLastTake ? now - s_nLastTake : 1;
	s_nLastTake = now;
	for (int c = 0; c < 4; c++)
	{
		s->nBusy[c] = s_nBusy[c];
		s_nBusy[c] = 0;
	}
	for (int k = 0; k < bridge::kMachines; k++)
	{
		s->nMach[k] = s_Mach[1][k] + s_Mach[2][k];
		s_Mach[1][k] = s_Mach[2][k] = 0;
	}
	for (int v = 0; v < bridge::kVoices; v++)		// (a block's worth may straddle the take: drawing only)
	{
		s->nVoice[v] = s_VoiceTicks[v];
		s_VoiceTicks[v] = 0;
	}
	s->nBlocks = s_nBlocks; s_nBlocks = 0;
	s->nMaxBlockUs = s_nMaxBlock; s_nMaxBlock = 0;
	s->nMaxPassUs = s_nMaxPass; s_nMaxPass = 0;
	if (s_nFifoMin == ~0u)		// no control-loop pass this second: the level now
	{
		s->nFifoMin = s->nFifoMax = g_Output.Level ();
	}
	else
	{
		s->nFifoMin = s_nFifoMin;
		s->nFifoMax = s_nFifoMax;
	}
	s_nFifoMin = ~0u; s_nFifoMax = 0;
	s->nMidiBytes = s_nMidiBytes; s_nMidiBytes = 0;
	s->nSysEx = s_Midi.SysExCount ();
	s->nNotes = s_nNotes; s_nNotes = 0;
	s->nNoteChans = s_nNoteChans; s_nNoteChans = 0;
}
