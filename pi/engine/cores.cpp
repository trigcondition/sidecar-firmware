// pi/engine/cores.cpp - see cores.h. The block handshake: core 3 bumps s_nGen and signals (sev); cores 1 and 2
// render their halves of s_Block and publish s_nDone; core 3 waits (in wfe, with a 20 ms limit), then pushes the
// block into the output stage. Nothing else touches the engine core while a block renders.
#include "cores.h"
#include "crumb.h"	// the crash breadcrumb: each core's heartbeat and section (2026-10-29)

#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/util.h>

#include "bridge.h"
#include "guard.h"
#include "idlewait.h"
#include "midi_in.h"
#include "link1.h"
#include "lagdiag.h"	// the Rytm report stream timing (RxDiag; core 3 writes, core 0 prints)
#include "notes.h"
#include "paircode_pi.h"
#include "usbc_pi.h"
#include "xypad_pi.h"
#include "sendfx_pi.h"
#include "masterfx_pi.h"
#include "rec_pi.h"
#include "shared.h"
#include "side_out.h"
#include "fifo_late.h"
#include "device/admit.h"
#include "fx/fx_guard.h"
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
// The pass s_nGen announces (published before it): the slots whose voices render in it (bit s: slot s), and kBasePass
// for the base box's pass (the send FX and the master chain render after it). The side stage's passes carry slot 1 alone.
static const u32 kBasePass = 0x80;
static const u32 kBaseBoxShift = 8;	// a base pass: the base box's slot in bits 8..15 (its send FX and master chain run after it)
// Each slot's split in the pass (bits 16.. , 4 a slot): the voices of its box on core 1, the rest on core 2. Core 3 sets it with
// the pass (core::admit::split_for: split= or half its box's voices, the smaller), so cores 1 and 2 always split a slot alike:
// a change between passes never renders a voice on both or neither
static const u32 kSplitShift = 16;
static inline unsigned PassSplit (u32 nPass, unsigned sl) { const unsigned n = nPass >> (kSplitShift + 4 * sl) & 0xF; return n ? n : 4; }
static volatile u32 s_nPass = 1 | kBasePass | 4u << kSplitShift | 4u << (kSplitShift + 4);
static u64 s_Mach[3][bridge::kMachines];
static_assert (sizeof CEngineCores::TStats::nMach / sizeof (u64) == bridge::kMachines, "nMach is bridge::kMachines");
static u64 s_VoiceTicks[bridge::kVoices];		// touch2: per voice (each voice renders on one core only)
static volatile u64 s_nBusy[4];
// CPU admission's check (core/device/admit.h): each render core's send FX and master chains, after its voices and outside
// s_nBusy (ChainCore: slot 0's on core 2, slot 1's on core 1); core 3's time waiting in RenderBlock for cores 1 and 2,
// inside s_nBusy[3]. stats shows them beside c1, c2 and c3.
static volatile u64 s_nChainTicks[4], s_nRenderWait;	// [3]: slot 0's master chain on core 3 (MasterCore), outside s_nRenderWait
// The last block generation whose chains each render core has finished (after s_nDone: core 3 does not wait for them).
// RenderBlock waits for both before a pass whose base box differs from the last pass's: AMP's master rows play on the base
// box's master chain only (core/fx/amp.cpp ctl), so a chain of the old base still running on one core never overlaps the
// new base's on the other.
static volatile u32 s_nChainDone[3];
static u32 s_nLastPassBase = ~0u;			// core 3's: the base box (kBaseBoxShift's slot) of the last base pass
// The render core that runs slot b's send FX and master chain (decided 2026-11-22: LT1 found core 2 full with both boxes'
// chains, core 1 ~70 % idle). Fixed by slot, never by which box is the base, so each box's rings (sendfx_pi, masterfx_pi:
// SPSC, one consumer) keep one render core whatever attaches or leaves. An 8-voice engine has slot 0 only: core 2.
static inline unsigned ChainCore (int b) { return bridge::kVoiceSlots > 1 && b == 1 ? 1 : 2; }
// ... and its master chain (decided 2026-11-22, LT3: the base box's master chain with its AMP pair took ~31-39 % of core 2,
// its send FX ~29-34 % more): slot 0's on core 3 in a two-slot engine, during RenderBlock's wait for cores 1 and 2 (core 3's
// own work there was 3-12 %), slot 1's on core 1 with its send FX. Fixed by slot like ChainCore (masterfx_pi's rings: one
// consumer). An 8-voice engine keeps it on core 2. core/device/admit.h master_core says the same.
// The measured guard's hot core (core/fx/fx_guard.h, decided 2026-11-23): render cores 1 and 2 when a block's voices and
// chains took kFxHotPct of a block (the voice guard's kHotPct); core 3 when slot 0's master chain took what core 3's limit
// leaves after its own control work (admit.h kC3LimitBp - kC3BaseBp, at the clock now: ~57 % of a block at 1500 MHz).
static const unsigned kFxHotPct = 80;
static inline u64 FxHotTicks (void)
{
	return TickRate () * 12 / 48000 * kFxHotPct / 100;	// (of a 12-frame block)
}
// ... and late (decided 2026-11-24 after LT12b: a slot past its budget on a loaded core made every block late, the FIFO
// drained for ~18 ms, heard as a loud hit): a render core whose block took a whole block; core 3 when its master chain
// took what a whole block leaves after core 3's own control work. A late block mutes its dearest effect at once.
static inline u64 FxLateTicks (void)
{
	return TickRate () * 12 / 48000;
}
static u64 s_nFxLate3 = 0;
// ... and the audio buffer's view (decided 2026-11-25 after LT12g P5: two slow slots put core 1 at 83 % with its chains,
// no core past a whole block, yet core 1 plus core 3's own work overran every block; the FIFO drained for 14 s and the
// guard said nothing): a base pass begun with the FIFO low (under 2 blocks), and the passes held after it, are late for the
// render core that took longer over its last base pass and side pass together (fifo_late.h blame). fxguard=fifo:off stops it.
static volatile u32 s_nFifoLate = 0;		// 1 or 2: this pass is late for that core (published with s_nGen)
static volatile u64 s_nLastBlk[2][3];		// each render core's last block, voices and chains: [0] a base pass's, [1] a side pass's
static volatile unsigned s_nFifoHeld[3], s_nFifoRunMax;	// the late log: passes late for core 1 / 2, the longest low run
// ... but only for a core over admission's limit or after a real underrun (decided 2026-11-26 after VA1, T515: fifo_late.h
// counts): the passes the FIFO's hold left on time because the late core measured within the limit with no underrun (the
// late log's "fits")
static volatile unsigned s_nFifoFits[3];
static inline u64 FxHot3Ticks (void)
{
	static unsigned s_nMHz = ~0u;
	static u64 s_n = 0;
	if (core::admit::clock () != s_nMHz)	// (the clock setting moved: core 3's own share scales with it)
	{
		s_nMHz = core::admit::clock ();
		const unsigned nBase = core::admit::core3_bp (core::admit::Load {});
		const unsigned nShare = nBase < core::admit::kC3LimitBp - 1000 ? core::admit::kC3LimitBp - nBase : 1000;
		s_n = TickRate () * 12 / 48000 * nShare / 10000;
		s_nFxLate3 = TickRate () * 12 / 48000 * (nBase < 9000 ? 10000 - nBase : 1000) / 10000;
	}
	return s_n;
}
static inline unsigned MasterCore (int b) { return bridge::kVoiceSlots > 1 ? (b == 1 ? 1 : 3) : 2; }
static_assert (core::admit::master_core (0) == (bridge::kVoiceSlots > 1 ? 3 : 2) && core::admit::master_core (1) == 1, "admit.h's master_core is MasterCore");
static volatile u64 s_nSleep[4];			// ticks in a wfe, per core, and how many wfe returned (TakeStats: the last second)
static volatile unsigned s_nWakes[4];
static unsigned s_nBlockLimitMs = 20;			// blocklimit=N (emulator gates only): RenderBlock's fatal limit
static char s_BlockLimitWhy[64] = "a render core did not finish its block in 20 ms";
static volatile boolean s_bIdleWfe = TRUE;		// idlewfe=0: core 3's render wait spins and core 0's loop sleeps as before
static volatile unsigned s_nBlocks, s_nMaxBlock, s_nMaxPass, s_nFifoMin = ~0u, s_nFifoMax, s_nMidiBytes, s_nNotes;
static volatile unsigned s_nNoteChans;	// bit c: a note-on on MIDI channel c+1 this second
static u64 s_nLastTake;
static unsigned s_nTarget = 96;
static volatile unsigned s_nRunCapped = 0, s_nFifoLateCount = 0;	// passes that stopped rendering at the cap (the FIFO still short: an overload)
static volatile boolean s_bTargetFixed = FALSE;	// a fifo= set it: the link's default does not
static boolean s_bNotes = TRUE;
static boolean s_bReportLog = FALSE;
static int s_nDrone[2] = {-1, -1};		// transport t's box's (0 the host link's, 1 the second box's): -1 off, 0..7 one voice, 8 all
static unsigned s_nSplit = 4;			// voices 0..s_nSplit-1 on core 1, the rest on core 2
static volatile u32 s_nBenchTurn = 4;		// the core whose bench runs now (4: none, every core done)
static CEngineCores::TBench s_Bench[4][CEngineCores::BenchMachines];
static u64 s_nDroneAt;

// The microsecond clock for lagdiag.h (the generic timer / its ticks a microsecond; 54 MHz on the Pi 4 and 5)
static inline u64 TicksToUs (u64 t)
{
	u64 nPerUs = TickRate () / 1000000;
	return t / (nPerUs != 0 ? nPerUs : 1);
}
static lagdiag::RxDiag s_Rx;		// the Rytm's reports and SysEx timing: core 3 (FromRytm, the pass) writes, CEngineCores::TakeRxLine reads

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
static void ToCore1 (const u8 *pData, int nLength) { bridge::sysex_link (1, pData, nLength); }
// The note adapter of each transport's box (0 the host link's, 1 the second box's, link1.h): its MIDI channels 1..8 play
// its own slot's voices, and :drone= / :notemach= (box=S: for the other box) set that box's alone (slots symmetric,
// decided 2026-11-26)
static core::NoteAdapter s_Notes[2] = {core::NoteAdapter (ToCore), core::NoteAdapter (ToCore1)};
// The two-box load test (slot=S:drone=..., slot=S:notemach=V:NAME): each slot's own adapter, its notes the Pi's own as that
// slot's box's (bridge::sysex_self_slot), so a stock box's voices play with no mod on it. -1 off, 0..7 one voice, 8 all
static void ToSlot0 (const u8 *pData, int nLength) { bridge::sysex_self_slot (0, pData, nLength); }
static void ToSlot1 (const u8 *pData, int nLength) { bridge::sysex_self_slot (1, pData, nLength); }
static core::NoteAdapter s_SlotNotes[2] = {core::NoteAdapter (ToSlot0), core::NoteAdapter (ToSlot1)};
static int s_nSlotDrone[2] = {-1, -1};
static int s_nCmdSlot = -1;			// the slot= prefix's slot while its command runs, else -1
// A Rytm running the mods (any F0 7D 52 54 from it) reports its voices itself (0021's 03 / 04, 0026's 33): the note
// adapter (a stock Rytm's notes on channels 1..8 as PLAITS reports) then steps aside, or each MIDI note the Rytm
// sends out over USB turned the voice it just set to JUNO / DSP SAMPLE / ... back into the adapter's PLAITS (the
// sound, and the voice pages' waveform / GRAIN view, which read the voice's state).
static volatile boolean s_bModded[2] = {FALSE, FALSE};	// transport t's box runs the mods
static void FromRytm (const u8 *pData, int nLength)
{
	if (nLength >= 5 && pData[1] == 0x7D && pData[2] == 0x52 && pData[3] == 0x54) s_bModded[0] = TRUE;
	s_Rx.on_sysex (TicksToUs (Ticks ()), pData, nLength);	// the arrival, before the handling below takes its time
	ToCore (pData, nLength);
}
static void OnChannel (u8 s, u8 a, u8 b)
{
	if ((s & 0xF0) == 0x90 && b) { s_nNotes++; s_nNoteChans |= 1u << (s & 0x0F); }
	bridge::lfo_midi (s, a, b);		// the LFOs: a Rytm track's trig (an FX target's TRIG, core/lfo.h)
	bridge::poly_midi (s, a, b);		// poly FM: the pads' notes, live chords (core/poly.h)
	if (s_bNotes && !s_bModded[0]) s_Notes[0].Channel (s, a, b);
	else if ((s & 0xF0) == 0xB0 && a == 120) sendfx_pi::AllSoundOff (bridge::transport_slot (0));	// the send FX hear it with the notes off too: this transport's box's (3(c))
}
// A box's realtime bytes, transport t's (0 the host link's g_MidiIn, 1 the second box's, link1.h): its own slot's clock
// first, its LFOs' and its send FX's STUTTER grid (bridge::realtime_link, 3(c) step 10). Then the Pi-wide jobs, each
// on the clock of the box it belongs to (rec_pi::Transport, decided 2026-11-26): the recorder arms on the base box's,
// a sampler take follows its own box's, the bundles' and the storage bench's waits hold while any box plays. MIDI's
// USB-A clock out (core/midim/midim.h) is the box's whose voices alone send there (none while both boxes' do).
// A box's CC 120 (All Sound Off, any channel; the Rytm's STOP STOP) on transport t other than the host link's: its own
// box's send FX fade out and clear (3(c); the host link's comes through OnChannel)
static void AllSoundOffIn (int nTransport)
{
	int nSlot = bridge::transport_slot (nTransport);
	if (nSlot >= 0) sendfx_pi::AllSoundOff (nSlot);
}
static volatile CEngineCores::TRealtime s_Rt[2] = {{0, 0, 0, 0, 0, -1}, {0, 0, 0, 0, 0, -1}};
CEngineCores::TRealtime CEngineCores::Realtime (unsigned nTransport)
{
	TRealtime r = {0, 0, 0, 0, 0, -1};
	if (nTransport < 2)
	{
		const volatile TRealtime &v = s_Rt[nTransport];
		r.nClock = v.nClock, r.nStart = v.nStart, r.nStop = v.nStop, r.nSongPos = v.nSongPos, r.nNoSlot = v.nNoSlot;
		r.nSlot = v.nSlot;
	}
	return r;
}
static void RealtimeIn (int nTransport, uint8_t s, uint16_t spp)
{
	int nSlot = bridge::realtime_link (nTransport, s, spp);
	if (nTransport >= 0 && nTransport < 2)
	{
		volatile CEngineCores::TRealtime &r = s_Rt[nTransport];
		if (nSlot < 0) r.nNoSlot++;
		else if (s == 0xF8) r.nClock++;
		else if (s == 0xFA || s == 0xFB) r.nStart++;
		else if (s == 0xFC) r.nStop++;
		else if (s == 0xF2) r.nSongPos++;
		r.nSlot = nSlot;
	}
	if (nSlot < 0) return;
	rec_pi::Transport (nSlot, s, spp);
	bridge::midim_realtime (nSlot, s);
}
static void OnRealtime (uint8_t s, uint16_t spp) { RealtimeIn (0, s, spp); }
static MidiIn s_Midi (FromRytm, OnChannel, OnRealtime);
// The second box's bytes (link1::Poll hands each on): its notes through its own note adapter, as the host link's box's
// through OnChannel (its SysEx goes to the engine in link1.cpp; here only a mods box's F0 7D 52 54 is noticed)
static void Sysex1 (const u8 *pData, int nLength)
{
	if (nLength >= 5 && pData[1] == 0x7D && pData[2] == 0x52 && pData[3] == 0x54) s_bModded[1] = TRUE;
}
static void OnChannel1 (u8 s, u8 a, u8 b)
{
	if ((s & 0xF0) == 0x90 && b) { s_nNotes++; s_nNoteChans |= 1u << (s & 0x0F); }
	if (s_bNotes && !s_bModded[1]) s_Notes[1].Channel (s, a, b);
}
static MidiIn s_Midi1 (Sysex1, OnChannel1);
static void Byte1 (uint8_t b) { s_Midi1.Feed (b); }
// The transport whose box holds the slot in hand (box=S: sets it; else the base box's): 0 when no transport's does
static int TransportInHand (void)
{
	const int nSlot = bridge::slot_in_hand ();
	for (int t = 1; t >= 0; t--) if (bridge::transport_slot (t) == nSlot) return t;
	return 0;
}
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
// The MIDI machine's USB destinations (core/midim/midim.h: 2 the computer, 3 every USB-A device; DIN is the core's own to_rytm):
// one packet each into g_AsampMidi (kernel.cpp's AsampMidiSend, core 0): a channel message or a realtime byte
static bool MidimOut (int nDest, const uint8_t *m, int n)
{
	if (nDest != 2 && nDest != 3) return FALSE;
	if (nDest == 2 && !g_bComputerMidi) return FALSE;
	if (n < 1 || n > 3 || m[0] < 0x80) return FALSE;
	bool bOff = (m[0] & 0xF0) == 0x80 || ((m[0] & 0xF0) == 0x90 && n == 3 && m[2] == 0);
	unsigned nFill = g_AsampMidi.size ();
	if (!bOff && nFill >= ((m[0] & 0xF0) == 0x90 ? 192u : 128u)) { s_nAsampLost++; return FALSE; }	// the rest of the queue is the note-offs'
	u32 nCin = m[0] >= 0xF8 ? 0x0F : m[0] >> 4;
	u32 w = (u32) ((nDest == 2 ? 1 : 3) << 4) | nCin | (u32) m[0] << 8 | (n > 1 ? (u32) m[1] << 16 : 0) | (n > 2 ? (u32) m[2] << 24 : 0);
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
static const char * volatile s_pStallStep;
CEngineCores::TStall CEngineCores::Stall (void)
{
	TStall s;
	for (unsigned i = 0; i < sizeof s / sizeof (unsigned); i++) ((unsigned *) &s)[i] = ((volatile unsigned *) &s_Stall)[i];
	return s;
}

const char *CEngineCores::StallStep (void) { const char *p = s_pStallStep; return p ? p : ""; }

static volatile unsigned s_Gaps[CEngineCores::kGaps][10], s_nGaps;
static const char * volatile s_GapStep[CEngineCores::kGaps];
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
		const char *pStep = s_GapStep[(n - k + i) % kGaps];
		p[i].step = pStep ? pStep : "", p[i].step_us = g[7], p[i].step_sum = g[8], p[i].here_us = g[9];
	}
	return k;
}

// One wfe, counted. Every caller has just checked what it waits for: an event set after that check makes this return at
// once, so the check and this wfe cannot lose a wake-up (tests/test_idlewait.cpp, the model). A core's first and last
// readings of the counter cost about 10 ns against a 250 us block.
static inline void Wfe (unsigned nCore)
{
	u64 t = Ticks ();
	asm volatile ("wfe");
	s_nSleep[nCore] += Ticks () - t;
	s_nWakes[nCore]++;
}
// Core 0's: not Wfe (0), which the event left by the network's spin lock ends at once (idlewait::nap)
void CEngineCores::Sleep (void)
{
	u64 t = Ticks ();
	idlewait::nap ([] { asm volatile ("sevl"); }, [] { asm volatile ("wfe"); });
	s_nSleep[0] += Ticks () - t;
	s_nWakes[0]++;
}
void CEngineCores::SetIdleWfe (boolean bOn) { s_bIdleWfe = bOn; }

// blocklimit=N (docs/pi-engine.md): RenderBlock's limit in ms, never below the unit's 20 ms. Only the QEMU gates set it:
// without icount the emulated counter is the host's clock, so a vCPU thread the host does not run for 20 ms fails a
// block a real core finishes in 250 us. A unit's cmdline never carries it, so the unit's limit stays 20 ms.
unsigned CEngineCores::SetBlockLimitMs (unsigned nMs)
{
	s_nBlockLimitMs = nMs < 20 ? 20 : nMs > 10000 ? 10000 : nMs;
	s_BlockLimitWhy[0] = 0;
	char *d = s_BlockLimitWhy;
	for (const char *p = "a render core did not finish its block in "; *p;) *d++ = *p++;
	char n[8]; int i = 0;
	for (unsigned v = s_nBlockLimitMs; v || i == 0; v /= 10) n[i++] = (char) ('0' + v % 10);
	while (i) *d++ = n[--i];
	for (const char *p = " ms"; *p;) *d++ = *p++;
	*d = 0;
	return s_nBlockLimitMs;
}
boolean CEngineCores::IdleWfe (void) { return s_bIdleWfe; }

// The generic timer's event stream on the calling core: an event every 2^13 counter ticks (152 us at 54 MHz, both boards),
// so a wfe never sleeps longer than that
void CEngineCores::EventStream (void)
{
	u64 nKctl;
	asm volatile ("mrs %0, cntkctl_el1" : "=r" (nKctl));
	nKctl = (nKctl & ~(u64) 0xF0) | (12 << 4) | (1 << 2);	// EVNTI 12 (an event as bit 12 rises: every 8192 ticks), EVNTEN
	asm volatile ("msr cntkctl_el1, %0; isb" : : "r" (nKctl));
}

static unsigned ClampTarget (unsigned n) { return n < 24 ? 24 : n > 1024 ? 1024 : n; }

boolean CEngineCores::NotesOn (void)	{ return s_bNotes; }
int CEngineCores::DroneVoice (void)		{ return s_nDrone[0] >= 0 ? s_nDrone[0] : s_nDrone[1]; }

void CEngineCores::Configure (unsigned nFifoTarget, boolean bNotes)
{
	s_nTarget = ClampTarget (nFifoTarget);
	g_Output.SetFifoTarget (s_nTarget);
	g_OutputSide.SetFifoTarget (s_nTarget);
	s_bNotes = bNotes;
	if (s_nLastTake == 0) s_nLastTake = Ticks ();	// the first stats line's interval: from the boot's Configure
}

unsigned CEngineCores::FifoTarget (void) { return s_nTarget; }


// core 0 (UseLink): a single aligned word, read by core 3 each pass
void CEngineCores::LinkFifoTarget (unsigned n)
{
	if (!s_bTargetFixed) s_nTarget = ClampTarget (n), g_Output.SetFifoTarget (s_nTarget), g_OutputSide.SetFifoTarget (s_nTarget);
}

void CEngineCores::FixFifoTarget (void) { s_bTargetFixed = TRUE; }

void CEngineCores::SetSplit (unsigned n) { s_nSplit = n < 1 ? 1 : n > 7 ? 7 : n; core::admit::set_split (s_nSplit); }

// wt gr: WAVETABLE (3 unison oscillators, SYNC) and GRANULAR (the built-in PAD, ~9 grains at once): notes.cpp's defaults;
// livein: LIVE IN on a synthetic AUDIO IN (bridge::bench feeds it), the gate on; chop: CHOP's 8 slice players (VOICES' most)
// on a synthetic break in RAM, looped, MORPH half way (bridge::bench sets them up)
const char *const CEngineCores::BenchMachine[BenchMachines] = {"plaits", "juno", "fm", "daisy", "dsps", "wt", "gr", "livein", "chop", "md"};

void CEngineCores::BenchCore (unsigned nCore)
{
	for (unsigned m = 0; m < BenchMachines; m++)
	{
		s_Bench[nCore][m].nTicks = bridge::bench (BenchMachine[m], &s_Bench[nCore][m].nWorst,
						       &s_Bench[nCore][m].nPeak, &s_Bench[nCore][m].nHash);
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
	// each slot's box voices split at that slot's own split, which the pass carries (PassSplit: split= or half the voices its
	// box asks for, the smaller; an A4's 4 as 2 + 2): 0..split-1 on core 1, the rest on core 2. Slot 1's are 8 + the same
	// static: they survive the guard's longjmp
	static volatile u32 s_nSeen[3];		// the last block generation this core finished
	static volatile u32 s_nGenNow[3];	// the one it renders
	static volatile u32 s_nPassNow[3];	// its pass (s_nPass), kept for a fault's resume
	static volatile bool s_nFifoLateNow[3];	// its pass late by the FIFO (s_nFifoLate)
	static volatile unsigned s_nResume[3];	// after a fault: the voice to go on with (0: none)
	static volatile u64 s_nT0[3];		// the block's start, for the busy time
	CLogger::Get ()->Write (From, LogNotice, "core %u up: renders each box's voices %s its split (split=%u, or half its voices), %s", nCore,
				nCore == 1 ? "below" : "from", s_nSplit, nCore == 1 ? "and slot 1's send FX and master chain" : MasterCore (0) == 3 ? "and slot 0's send FX (its master chain: core 3)" : "and slot 0's send FX and master chain");
	if (__builtin_setjmp (g_Guard.JumpBuffer (nCore)))
	{
		// a fault in a voice's render (guard.cpp returned from the exception into here, at task level): that
		// voice is muted and silent in this block; the block's voices before it keep what they rendered, the
		// ones after it render below
		const int vc = (int) g_Guard.CurrentVoice (nCore);
		unsigned v = g_Guard.Recover (nCore, bridge::machine_name (vc), bridge::machine_gen (vc));
		um::render_fault ((int) v);	// (a user machine's slot muted, and the slots sharing its image: core 3)
		for (unsigned s = 0; s < bridge::kBlock; s++) s_Block[s][v] = s_BlockR[s][v] = 0;
		u64 nSPSel;
		asm volatile ("mrs %0, spsel" : "=r" (nSPSel));
		g_Guard.NoteResumed (nCore, CurrentExecutionLevel () == TASK_LEVEL && nSPSel == 0);
		s_nResume[nCore] = v + 1;
	}
	for (;;)
	{
		u32 nGen;
		unsigned vFirst;	// an engine voice (a fault's resume may be in slot 1's group)
		if (s_nResume[nCore])
		{
			nGen = s_nGenNow[nCore];
			vFirst = s_nResume[nCore];	// (s_nPassNow[nCore] as it was)
			s_nResume[nCore] = 0;
			crumb::BeatEnter (nCore, 200);	// (back in the block a recovered fault left)
		}
		else
		{
			crumb::Enter (201);
			while ((nGen = __atomic_load_n (&s_nGen, __ATOMIC_ACQUIRE)) == s_nSeen[nCore]) Wfe (nCore);
			crumb::BeatEnter (nCore, 200);
			s_nGenNow[nCore] = nGen;
			s_nPassNow[nCore] = __atomic_load_n (&s_nPass, __ATOMIC_RELAXED);	// (published before nGen)
			s_nFifoLateNow[nCore] = __atomic_load_n (&s_nFifoLate, __ATOMIC_RELAXED) == nCore;	// (so is this)
			s_nT0[nCore] = Ticks ();
			vFirst = 0;
		}
		// the voices of this core: v0..v1-1 of each slot in this pass (RenderBlock: slot 1's only while a box has it, in
		// the base box's pass or, beside a Rytm, in passes of its own), at that slot's split as the pass carries it
		const u32 nPass = s_nPassNow[nCore];
		for (unsigned v = vFirst; v < bridge::kVoices; v++)
		{
			const unsigned k = v % bridge::kBoxVoices, sl = v / bridge::kBoxVoices;
			const unsigned nSp = PassSplit (nPass, sl);
			const unsigned v0 = nCore == 1 ? 0 : nSp, v1 = nCore == 1 ? nSp : bridge::kBoxVoices;
			if (!(nPass >> sl & 1) || k >= v1) { v += bridge::kBoxVoices - k - 1; continue; }
			if (k < v0) { v += v0 - k - 1; continue; }
			const void *pMach = bridge::machine_name ((int) v);	// the guard's machine key: a static name,
			const u32 nMGen = bridge::machine_gen ((int) v);	// and a user plugin's slot and load
			if (!core::device::voice_admitted ((int) v)	// CPU admission (core/device/admit.h): not its box's
			    || (g_Guard.Muted (v) && !g_Guard.Fresh (v, pMach, nMGen)))
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
		// the send FX and the master chain of the box this pass rendered (phase 3(d)): the base box's after a base pass, the
		// side stage's after its own (sendfx_pi.h, masterfx_pi.h), on that box's render core (ChainCore)
		const int nFxBox = (nPass & kBasePass) ? (int) (nPass >> kBaseBoxShift & 0xFF) : __builtin_ctz (nPass & 0x7F);
		if (ChainCore (nFxBox) == nCore)
		{
			const u64 tC = Ticks ();
			sendfx_pi::Render (nFxBox);	// the send FX, after the block
			if (MasterCore (nFxBox) == nCore)
				masterfx_pi::Render (nFxBox);	// the master chain, after the send FX (else core 3's: MasterCore)
			s_nChainTicks[nCore] += Ticks () - tC;
		}
		// the measured guard of every effect (core/fx/fx_guard.h, decided 2026-11-23): this core's block, voices and chains,
		// hot at kFxHotPct of a block (as the voice guard's 80 %): its effects' overs count, and 16 hot blocks in a row mute
		// the dearest effect on it
		const u64 nBlk = Ticks () - s_nT0[nCore];
		s_nLastBlk[(nPass & kBasePass) ? 0 : 1][nCore] = nBlk;
		core::fx::guard::core_done ((int) nCore, nBlk >= FxHotTicks (), nBlk >= FxLateTicks () || s_nFifoLateNow[nCore]);
		__atomic_store_n (&s_nChainDone[nCore], nGen, __ATOMIC_RELEASE);
		asm volatile ("dsb sy; sev");
	}
}

void CEngineCores::RenderBlock (int nSide)
{
	// the base box's pass (nSide < 0): the sequencer's block, every slot that renders but the side stage's; the side
	// stage's: slot nSide's voices alone, into its own FIFO (side_out.h)
	u32 nPass = 0;
	if (nSide < 0)
	{
		bridge::seq_block ();		// the sequencer's notes due in this block (core/seq/seq.h): once a base block
		const int nSideNow = core::device::side_slot ();
		for (int sl = 0; sl < (int) (bridge::kVoices / bridge::kBoxVoices); sl++)
			if (sl != nSideNow && bridge::slot_renders (sl)) nPass |= 1u << sl;
		nPass |= kBasePass | (u32) (bridge::out_base () / bridge::kBoxVoices) << kBaseBoxShift;
	}
	else
	{
		bridge::seq_block_side (nSide);	// 3(c): the side stage's slot's own sequencer block, in its pass
		nPass = 1u << nSide;
	}
	for (unsigned sl = 0; sl < bridge::kVoiceSlots && sl < 2; sl++)	// each slot's split (PassSplit), from what its box asks
		nPass |= (u32) core::admit::split_for (core::device::wanted ((int) sl)) << (kSplitShift + 4 * sl);
	u64 t0 = Ticks (), nLimit = TickRate () / 1000 * s_nBlockLimitMs;	// 20 ms (blocklimit= in the QEMU gates only)
	const u32 nPrev = s_nGen;
	if ((nPass & kBasePass) && (nPass >> kBaseBoxShift & 0xFF) != s_nLastPassBase)
	{	// the base box changed (attach, leave): every chain of the last pass done first (s_nChainDone), a rare wait
		if (s_nLastPassBase != ~0u
		    && !idlewait::wait ([&] { return __atomic_load_n (&s_nChainDone[1], __ATOMIC_ACQUIRE) == nPrev
						&& __atomic_load_n (&s_nChainDone[2], __ATOMIC_ACQUIRE) == nPrev; },
					[&] { return Ticks () - t0 > nLimit; }, [] { Wfe (3); }, s_bIdleWfe))
		{
			g_Guard.Fatal (s_BlockLimitWhy);
			for (;;) asm volatile ("wfe");		// core 0 stops the stream and reboots
		}
		s_nLastPassBase = nPass >> kBaseBoxShift & 0xFF;
		t0 = Ticks ();
	}
	__atomic_store_n (&s_nPass, nPass, __ATOMIC_RELAXED);	// (the release below publishes it)
	{	// a FIFO low at its own pass (the base box's at a base pass, the side stage's at a side pass), or either FIFO's
		// hold still on: late for the render core that took longer (s_nFifoLate, above). Both kinds of pass carry it:
		// the two boxes take turns on cores 1 and 2, and LT12j (2026-11-25) set it at base passes only, so the guard saw
		// the A4's effects alone while the Rytm's (the side stage's) cost the time, and its late run broke every other pass.
		// LT13 (2026-11-25): the core that took longer in the last pass alone flipped between 1 and 2 as the boxes took
		// turns, so the late rule's run never came; and a FIFO pinned low gave one dip and no hold (fifo_late.h)
		static fifo_late::Pair s_FifoLate;
		u32 nLate = 0;
		bool bDip = false;
		static u32 s_nLateMutes = 0;	// the guard's late mutes seen: a new one rearms both FIFOs (fifo_late.h rearm)
		const u32 nLateMutes = core::fx::guard::stats ().late;
		if (nLateMutes != s_nLateMutes)
		{
			s_nLateMutes = nLateMutes;
			s_FifoLate.rearm ();
		}
		const bool bOn = core::fx::guard::fifo_late ();
		const bool bSideOn = core::device::side_slot () >= 0;
		if (!bSideOn) s_nLastBlk[1][1] = s_nLastBlk[1][2] = 0;	// (no side stage: its last blocks count nothing)
		const bool bLate = nSide < 0
			? s_FifoLate.step (false, g_Output.Level (), g_Output.Streams (), bridge::kBlock, bOn, bSideOn, &bDip)
			: s_FifoLate.step (true, g_OutputSide.Level (), g_OutputSide.Streams (), bridge::kBlock, bOn, bSideOn, &bDip);
		// admission's word at the limit (fifo_late.h counts, decided 2026-11-26 after VA1): the late core must measure at or
		// over kLimitBp for its time a block (Load), or an output must have underrun lately; else the pass stays on time ("fits")
		static fifo_late::Load s_Load[3];
		static fifo_late::Under s_Under;
		for (unsigned c = 1; c <= 2; c++) s_Load[c].step (s_nLastBlk[0][c] + s_nLastBlk[1][c]);
		s_Under.step (g_Output.Underflows () + g_OutputSide.Underflows ());
		if (bLate)
		{
			nLate = fifo_late::blame (s_nLastBlk[0][1], s_nLastBlk[1][1], s_nLastBlk[0][2], s_nLastBlk[1][2]);
			if (fifo_late::counts (s_Load[nLate].bp (FxLateTicks ()), core::admit::kLimitBp, s_Under.recent ()))
				s_nFifoHeld[nLate]++;
			else
				s_nFifoFits[nLate]++, nLate = 0;
		}
		const unsigned nRun = nSide < 0 ? s_FifoLate.base.run : s_FifoLate.side.run;
		if (nRun > s_nFifoRunMax) s_nFifoRunMax = nRun;
		if (bDip) s_nFifoLateCount++;
		__atomic_store_n (&s_nFifoLate, nLate, __ATOMIC_RELAXED);	// (the release below publishes it)
	}
	u32 nGen = nPrev + 1;
	__atomic_store_n (&s_nGen, nGen, __ATOMIC_RELEASE);
	asm volatile ("dsb sy; sev");
	{	// slot 0's master chain on this core (MasterCore), while cores 1 and 2 render: the box this pass's chains are for,
		// as the render cores pick it. Its time is core 3's own: neither its render wait nor the 20 ms limit's (cores 1 and 2's)
		const int nFxBox = (nPass & kBasePass) ? (int) (nPass >> kBaseBoxShift & 0xFF) : __builtin_ctz (nPass & 0x7F);
		if (MasterCore (nFxBox) == 3)
		{
			const u64 tM = Ticks ();
			masterfx_pi::Render (nFxBox);
			const u64 dM = Ticks () - tM;
			s_nChainTicks[3] += dM;
			t0 += dM;
			const u64 nHot3 = FxHot3Ticks ();	// (sets s_nFxLate3 too)
			core::fx::guard::core_done (3, dM >= nHot3, dM >= s_nFxLate3);	// (core 3: its master chain against its share)
		}
	}
	// wfe: each worker signals (sev) after publishing s_nDone, so the wake is the time a core takes to leave wfe (well under
	// 1 us: clock gating, no retention state is enabled), and core 3's event stream bounds it at 152 us if a sev were ever
	// missed, which also keeps the 20 ms limit below alive. An event another core's sev spends here (a FIFO pull) costs
	// nothing: this path always passes again without sleeping (bWorked), and the FIFO's level is read again first.
	const boolean bDone = idlewait::wait ([&] { return __atomic_load_n (&s_nDone[1], __ATOMIC_ACQUIRE) == nGen
							   && __atomic_load_n (&s_nDone[2], __ATOMIC_ACQUIRE) == nGen; },
					      [&] { return Ticks () - t0 > nLimit; }, [] { Wfe (3); }, s_bIdleWfe);
	if (!bDone)
	{
		g_Guard.Fatal (s_BlockLimitWhy);
		for (;;) asm volatile ("wfe");		// core 0 stops the stream and reboots
	}
	const u64 nWait = Ticks () - t0;
	s_nRenderWait += nWait;
	unsigned nUs = (unsigned) (nWait * 1000000 / TickRate ());
	if (nUs > s_nMaxBlock) s_nMaxBlock = nUs;
	// the output stage: the base box's eight (bridge.h out_base), or the side stage's slot's
	const unsigned b = nSide < 0 ? (unsigned) bridge::out_base () : (unsigned) nSide * bridge::kBoxVoices;
	static s16 Box[bridge::kBlock][bridge::kBoxVoices], BoxR[bridge::kBlock][bridge::kBoxVoices];
	for (unsigned s = 0; s < bridge::kBlock; s++)
		for (unsigned k = 0; k < bridge::kBoxVoices; k++) Box[s][k] = s_Block[s][b + k], BoxR[s][k] = s_BlockR[s][b + k];
	u8 nSt = 0;
	for (unsigned k = 0; k < bridge::kBoxVoices; k++) nSt |= s_St[b + k] ? 1u << k : 0;
	(nSide < 0 ? g_Output : g_OutputSide).Push (Box, bridge::kBlock, BoxR, nSt);
	s_nBlocks++;
}

// The engine's own notes for tests (:drone=V or all): every 0.5 s a note-off and a note-on (note 48 + 2v) on the
// voice(s), through the note adapter (each voice's machine as :notemach set it).
static void Drone (void)
{
	u64 now = Ticks ();
	if ((s_nDrone[0] < 0 && s_nDrone[1] < 0 && s_nSlotDrone[0] < 0 && s_nSlotDrone[1] < 0) || now - s_nDroneAt < TickRate () / 2) return;
	s_nDroneAt = now;
	for (int t = 0; t < 2; t++)
		for (int v = 0; v < 8; v++)
		{
			if (s_nDrone[t] < 0 || (s_nDrone[t] != 8 && s_nDrone[t] != v)) continue;
			s_Notes[t].Channel (0x80 | v, 48 + 2 * v, 0);
			s_Notes[t].Channel (0x90 | v, 48 + 2 * v, 100);
		}
	for (int s = 0; s < 2; s++)		// the load test's, per slot (slot=S:drone=; a voice past its box's admitted count: dropped)
		for (int v = 0; v < 8; v++)
		{
			if (s_nSlotDrone[s] != 8 && s_nSlotDrone[s] != v) continue;
			s_SlotNotes[s].Channel (0x80 | v, 48 + 2 * v, 0);
			s_SlotNotes[s].Channel (0x90 | v, 48 + 2 * v, 100);
		}
}

void CEngineCores::Control (void)
{
	// the Rytm's SIDECAR MENU > WEB UI (core/ui/page_menu.cpp): paircode_pi's, which only asks core 0 (Poll)
	bridge::webui_init (paircode_pi::WebOn, paircode_pi::MenuWeb, paircode_pi::MenuGate, paircode_pi::MenuDevices,
			    paircode_pi::MenuNewCode, paircode_pi::MenuForget);
	bridge::usb_init (usbc_pi::Mode, usbc_pi::Modes, usbc_pi::Shown, usbc_pi::Ask, nullptr);	// SIDECAR MENU > USB-C: asks core 0
	// The generic timer's event stream on this core, so the wfe below never sleeps longer than 152 us. A sev from core 0 (a pull from the FIFO) can be
	// consumed by a wfe inside this core's own work (a spinlock: the logger, the card), and the next sev may be
	// a poll of core 0 away (1.2..3.3 ms measured on the TCP link): the FIFO then stayed empty and the link sent
	// silence (docs/pi-engine.md "FIFO target").
	EventStream ();
	bridge::keys_set_forward (KeysToRytm);
	bridge::asamp_set_midi_out (AsampMidiOut);
	bridge::midim_set_out (MidimOut);
	CLogger::Get ()->Write (From, LogNotice, "core 3 up: control (fifo target %u frames, MIDI notes %s)", s_nTarget,
				s_bNotes ? "on" : "off");
	u64 nPrevT0 = 0, nPrevWfe = 0;
	unsigned nSec[6] = {}, nStreams = 0;
	struct { const char *pName; unsigned nUs, nSum, nHere; } Slow[2] = {};	// the pass's tick and idle sections: their slowest step
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
				s_pStallStep = 0;
			}
			unsigned nGap = nPrevT0 ? (unsigned) ((t0 - nPrevT0) * 1000000 / TickRate ()) : 0;
			unsigned nLong = 0;			// the section that took the pass's time
			for (unsigned i = 1; i < 6; i++) if (nSec[i] > nSec[nLong]) nLong = i;
			const auto &S = Slow[nLong == 3 ? 0 : 1];
			const boolean bNamed = nLong == 3 || nLong == 5;
			if (nGap > 2000)
			{
				s_Stall.gaps_over_2ms++;
				u64 nKc, nDaif;
				asm volatile ("mrs %0, cntkctl_el1" : "=r" (nKc));
				asm volatile ("mrs %0, daif" : "=r" (nDaif));
				volatile unsigned *g = s_Gaps[s_nGaps % kGaps];
				g[0] = nGap, g[1] = g_Output.Diagnostics ().since_reset, g[2] = (unsigned) (nPrevWfe * 1000000 / TickRate ());
				g[3] = (unsigned) nKc, g[4] = (unsigned) nDaif;
				g[5] = nLong, g[6] = nSec[nLong];
				g[7] = bNamed ? S.nUs : 0, g[8] = bNamed ? S.nSum : 0, g[9] = bNamed ? S.nHere : 0;
				s_GapStep[s_nGaps % kGaps] = bNamed ? S.pName : "";
				s_nGaps++;
			}
			if (nGap > s_Stall.gap_us)
			{
				s_Stall.gap_us = nGap, s_Stall.at_frames = g_Output.Diagnostics ().since_reset;
				for (unsigned i = 0; i < 6; i++) s_Stall.sec_us[i] = nSec[i];	// the pass that just ended: the gap is its time
				s_Stall.step_us = bNamed ? S.nUs : 0, s_Stall.step_sum = bNamed ? S.nSum : 0, s_Stall.here_us = bNamed ? S.nHere : 0;
				s_pStallStep = bNamed ? S.pName : "";
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
		link1::Poll (RealtimeIn, AllSoundOffIn, Byte1);	// the second box's messages, as the engine's slot 1, its clock and its CC 120 (link1.h)
		Mark (0);
		s_Rx.tick (TicksToUs (Ticks ()));	// a second of the report stream ends here too (now: the messages above were stamped after t0)
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
		if (g_bRytmGone) { g_bRytmGone = false; bridge::rytm_gone (); s_Rx.forget (); }
		bridge::rytm_link (!g_bRytmOff);
		if (g_bRytmResync) { g_bRytmResync = false; bridge::rytm_resync (); }
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
		{	// its slowest step, named in the log past 2 ms (as the idle section's below)
			Slow[0].pName = bridge::idle_slowest (&Slow[0].nUs, &Slow[0].nSum, &Slow[0].nHere);
			if (nSec[3] > 2000)
				CLogger::Get ()->Write (From, LogWarning, "control: tick section %u us, its slowest step %s (%u us), named steps %u us, card here %u us",
							nSec[3], Slow[0].pName, Slow[0].nUs, Slow[0].nSum, Slow[0].nHere);
		}
		unsigned nLevel = g_Output.Level ();
		if (nLevel < s_nFifoMin) s_nFifoMin = nLevel;
		if (nLevel > s_nFifoMax) s_nFifoMax = nLevel;
		// each stage's FIFO to its target: the base box's, and beside a Rytm slot 1's (side_out.h), each paced by its
		// own box's stream; the emptier first. At most a FIFO target's worth of blocks a pass (each stage's), then this
		// loop's other work: under an overload that never fills the FIFO, an unbounded loop never left, so the console's
		// commands, the measured guard's poll and CPU admission's shed all stopped (LT12g QEMU, 2026-11-25: after
		// fxguard=clear under the full load, fxguard=report never ran and the command queue filled)
		for (unsigned nRun = 0;; nRun++)
		{
			const int nSide = core::device::side_slot ();
			const unsigned nMaxRun = (nSide >= 0 ? 2 : 1) * (s_nTarget / bridge::kBlock > 2 ? s_nTarget / bridge::kBlock : 2);
			if (nRun >= nMaxRun) { s_nRunCapped++; break; }
			const int nPick = side_out::pick (g_Output.Level (), nSide >= 0, g_OutputSide.Level (), s_nTarget, bridge::kBlock);
			if (nPick < 0) break;
			RenderBlock (nPick == 1 ? nSide : -1);
			bWorked = TRUE;
		}
		Mark (4);
		if (bridge::idle_prepare () || bridge::idle_save ()) bWorked = TRUE;
		else
		{
			bridge::idle_stream ();
			u64 tU = Ticks ();
			if (um::idle ()) bWorked = TRUE;	// the user machines: one bounded step (an 8 KB read at most)
			u64 tL = Ticks ();
			lists::idle ();			// the pool / lists JSON for the web page, when it changed (<= 4 a second)
			u64 tE = Ticks ();
			(void) tU;	// (um: timed inside, by branch)
			bridge::idle_note ("lists", (unsigned) ((tE - tL) * 1000000 / TickRate ()));
		}
		Mark (5);
		{	// 7z: an idle section past 2 ms (the FIFO's refill waits that long): its slowest step, named in the log
			Slow[1].pName = bridge::idle_slowest (&Slow[1].nUs, &Slow[1].nSum, &Slow[1].nHere);
			if (nSec[5] > 2000)
				CLogger::Get ()->Write (From, LogWarning, "control: idle section %u us, its slowest step %s (%u us), named steps %u us, card here %u us",
							nSec[5], Slow[1].pName, Slow[1].nUs, Slow[1].nSum, Slow[1].nHere);
		}
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
			else Wfe (3);			// woken by the OUT fill, a MIDI byte or a command (sev)
			nPrevWfe = Ticks () - tW;
		}
	}
}

// The console's commands (console.cpp: ':' lines), run here so the engine core is touched by one core only.
struct TSlotCmd { CEngineCores *pThis; const char *pText; };
void CEngineCores::SlotCommand (void *pArg)	// slot=S:<command>, in slot S's Scope (bridge::in_slot)
{
	TSlotCmd *c = (TSlotCmd *) pArg;
	c->pThis->Command (c->pText);
}

void CEngineCores::Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	if (bridge::box_command (p, [] (void *c, const char *q) { static_cast<CEngineCores *> (c)->Command (q); }, this))	// box=S:<command>: in that box's slot
		return;
	if (strncmp (p, "slot=", 5) == 0 && (p[5] == '0' || p[5] == '1') && p[6] == ':' && s_nCmdSlot < 0)
	{	// the two-box load test: any command as slot S's box's (ins=, mfx=, sendfx=, ...); drone= and notemach= its own adapter
		s_nCmdSlot = p[5] - '0';
		TSlotCmd c = {this, p + 7};
		if (!bridge::in_slot (s_nCmdSlot, SlotCommand, &c)) L->Write (From, LogWarning, "slot=%c: no such slot", p[5]);
		s_nCmdSlot = -1;
		return;
	}
	core::NoteAdapter &Notes = s_nCmdSlot >= 0 ? s_SlotNotes[s_nCmdSlot] : s_Notes[TransportInHand ()];
	if (bridge::admit_command (p))		// admit=report|enforce, loadtest=on|off
	{
	}
	else if (strncmp (p, "notemach=", 9) == 0 && p[9] >= '0' && p[9] <= '7' && p[10] == ':')
	{
		int v = p[9] - '0';
		if (s_nCmdSlot >= 0)	// the load test's slot=S: (its own adapter)
		{
			if (Notes.SetMachine (v, p + 11)) L->Write (From, LogNotice, "notes: slot %d voice %d plays %s", s_nCmdSlot, v, p + 11);
			else L->Write (From, LogWarning, "notes: no machine \"%s\" (%s)", p + 11, core::NoteAdapter::Names ());
		}
		else
		{
			const int t = TransportInHand ();
			if (s_Notes[t].SetMachine (v, p + 11)) L->Write (From, LogNotice, "notes: transport %d voice %d plays %s", t, v, p + 11);
			else L->Write (From, LogWarning, "notes: no machine \"%s\" (%s)", p + 11, core::NoteAdapter::Names ());
		}
	}
	else if (strncmp (p, "drone=", 6) == 0)
	{
		int d = strcmp (p + 6, "all") == 0 ? 8 : strcmp (p + 6, "off") == 0 ? -1 : atoi (p + 6);
		if (d > 8) d = -1;
		if (s_nCmdSlot >= 0)	// the load test's slot=S: (its own adapter and drone)
		{
			s_nSlotDrone[s_nCmdSlot] = d;
			if (d < 0)	// its voices' notes closed (the drone's last notes)
				for (int v = 0; v < 8; v++) Notes.Channel (0xB0 | v, 123, 0);
			L->Write (From, LogNotice, "drone %s (slot %d)", p + 6, s_nCmdSlot);
		}
		else
		{
			const int t = TransportInHand ();
			if (d < 0)		// off: its sounding notes ended
				for (int v = 0; v < 8; v++) if (s_nDrone[t] == 8 || s_nDrone[t] == v) s_Notes[t].Channel (0x80 | v, 48 + 2 * v, 0);
			s_nDrone[t] = d;
			L->Write (From, LogNotice, "drone %s (transport %d, slot %d)", p + 6, t, bridge::transport_slot (t));
		}
	}
	else if (strncmp (p, "notes=", 6) == 0)
	{
		s_bNotes = p[6] == '1';
		L->Write (From, LogNotice, "MIDI notes %s%s", s_bNotes ? "on" : "off", s_bNotes && s_bModded[0] ? " (idle: the Rytm runs the mods and reports its voices)" : "");
	}
	else if (strcmp (p, "out=main") == 0 || strcmp (p, "out=tracks") == 0)	// (out=Vn:...: the OUT route, core/fx/out.h)
	{
		const Output::Mode M = strcmp (p + 4, "tracks") == 0 ? Output::kTracks : Output::kMain;
		g_Output.SetMode (M);
		g_OutputSide.SetMode (M);	// (each stage's own device decides MAIN for a stereo mix)
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
	else if (strncmp (p, "idlewfe=", 8) == 0)	// the cmdline's idlewfe=, live: an A/B for the heat
	{
		s_bIdleWfe = p[8] != '0';
		L->Write (From, LogNotice, "idle wfe %s", s_bIdleWfe ? "on" : "off");
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
	else if (strncmp (p, "pool=", 5) == 0 || strncmp (p, "juno=", 5) == 0 || strncmp (p, "fm=", 3) == 0 || strncmp (p, "preset=", 7) == 0)
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
	else if (static char Lo[1400]; bridge::lfo_command (p, Lo, sizeof Lo))	// lfo, lfo=N:KEY:VALUE: the LFOs (core/lfo.h; 26 entries of ~40 bytes)
	{
		L->Write (From, LogNotice, "%s", Lo);
	}
	else if (char Mo[400]; bridge::mod_command (p, Mo, sizeof Mo))	// mod, mod=stats: the modulation layer (core/mod/mod.h)
	{
		L->Write (From, LogNotice, "%s", Mo);
	}
	else if (char Sc[400]; bridge::scene_command (p, Sc, sizeof Sc))	// scene, scene=...: the morph scenes (core/mod/scene.h)
	{
		if (!xypad_pi::Quiet (p)) L->Write (From, LogNotice, "%s", Sc);	// (scene=fader: a drag's lines)
	}
	else if (char Lp[400]; bridge::loop_command (p, Lp, sizeof Lp))	// loop, loop=...: the master looper (core/loop/loop.h)
	{
		L->Write (From, LogNotice, "%s", Lp);
	}
	else if (char Xy[400]; bridge::xy_command (p, Xy, sizeof Xy))	// xy, xy=...: the XY pad (core/mod/xypad.h)
	{
		if (Xy[0]) L->Write (From, LogNotice, "%s", Xy);	// (xy=pos / xy=up answer nothing: a drag's 30 lines a second)
	}
	else if (static char Le[1600]; bridge::learn_command (p, Le, sizeof Le))	// (32 routes' listing)	// learn, learn=...: MIDI learn (core/mod/learn.h)
	{
		L->Write (From, LogNotice, "%s", Le);
	}
	else if (sendfx_pi::Command (p))		// sendfx, sendfxout=, sendlvl=, sendknob= (sendfx_pi.h)
	{
	}
	else if (masterfx_pi::Command (p))		// mfx=stats (masterfx_pi.h)
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
		if (!xypad_pi::Quiet (p)) L->Write (From, LogNotice, "%s", Po);	// (a touch drag's value lines answer too: 30 a second, not logged)
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

// Core 0, once a second: the report stream's last whole second as its log line, 0 when no report came (lagdiag.h RxDiag)
unsigned CEngineCores::TakeRxLine (char *pOut, unsigned nMax)
{
	lagdiag::RxDiag::Second x;
	if (!s_Rx.take (&x)) return 0;
	return lagdiag::fmt_rx (pOut, nMax, x);
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
	s->nChain1 = s_nChainTicks[1], s_nChainTicks[1] = 0;
	s->nChain2 = s_nChainTicks[2], s_nChainTicks[2] = 0;
	s->nChain3 = s_nChainTicks[3], s_nChainTicks[3] = 0;
	s->nWait3 = s_nRenderWait, s_nRenderWait = 0;
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
	for (int c = 0; c < 4; c++)
	{
		s->nSleep[c] = s_nSleep[c];
		s_nSleep[c] = 0;
		s->nWakes[c] = s_nWakes[c];
		s_nWakes[c] = 0;
	}
	s->nBlocks = s_nBlocks; s_nBlocks = 0;
	s->nMaxBlockUs = s_nMaxBlock; s_nMaxBlock = 0;
	s->nMaxPassUs = s_nMaxPass; s_nMaxPass = 0;
	s->nRunCapped = s_nRunCapped; s_nRunCapped = 0;
	s->nFifoLate = s_nFifoLateCount; s_nFifoLateCount = 0;
	s->nFifoHeld1 = s_nFifoHeld[1]; s_nFifoHeld[1] = 0;
	s->nFifoHeld2 = s_nFifoHeld[2]; s_nFifoHeld[2] = 0;
	s->nFifoRunMax = s_nFifoRunMax; s_nFifoRunMax = 0;
	s->nFifoFits1 = s_nFifoFits[1]; s_nFifoFits[1] = 0;
	s->nFifoFits2 = s_nFifoFits[2]; s_nFifoFits[2] = 0;
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
