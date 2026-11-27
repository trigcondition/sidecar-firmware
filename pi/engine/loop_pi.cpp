// pi/engine/loop_pi.cpp - see loop_pi.h. Circle's side: the boot reservation and the KEEP writer's card backend.
#include "loop_pi.h"
#include "masterfx_pi.h"

#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/string.h>

#include "fx/out.h"
#include "loop/loop.h"
#include "sidecar_features.h"
#include "loop_keep.h"
#include "rec_pi.h"

namespace loop_pi {

static const char From[] = "loop";
static loop_pi::Keep s_Keep;
static unsigned s_nKeepSlot = 0;	// the slot whose job s_Keep writes
static unsigned s_nNextSlot = 0;	// the slot asked first at the next free pass (each in turn)
static void KeepDone (uint32_t id, bool ok, const char *take) { core::loop::keep_finished ((int) s_nKeepSlot, id, ok, take); }

void Initialize()
{
	CLogger *log = CLogger::Get ();
	if (!SIDECAR_LOOPER)
	{	// 0.5.2: the looper is off (core/sidecar_features.h): no ring, no grab store (the pool gets that RAM), core::loop::init never runs
		log->Write (From, LogNotice, "loop off (not in this release)");
		return;
	}
	u64 nMemMB = CMemorySystem::Get ()->GetMemSize () >> 20;
	int nOpt = CKernelOptions::Get ()->GetAppOptionDecimal ("loopram", 0xFFFFFFFFu) == 0xFFFFFFFFu
			   ? -1 : (int) CKernelOptions::Get ()->GetAppOptionDecimal ("loopram", 0);
	unsigned nMB = StoreMB (nMemMB, nOpt);
	if (!nMB)
	{
		log->Write (From, LogNotice, "loop off (%s)", nOpt == 0 ? "loopram=0" : "1 GB board: loopram=MB turns it on");
		return;
	}
	// one looper per box: each slot its own ring and half the store (a box may attach in either slot: both reserved at boot).
	// From the heap, before the pool RAM is taken (platform_pi.cpp pool_ram_budget reads what is left); never freed
	const unsigned nEach = nMB / core::loop::kSlots;
	u8 *pRing[core::loop::kSlots], *pStore[core::loop::kSlots];
	for (int s = 0; s < core::loop::kSlots; s++)
	{
		pRing[s] = (u8 *) CMemorySystem::HeapAllocate ((size_t) kRingBytes + 4096, HEAP_ANY);
		pStore[s] = pRing[s] ? (u8 *) CMemorySystem::HeapAllocate (((size_t) nEach << 20) + 4096, HEAP_ANY) : 0;
		if (!pRing[s] || !pStore[s])
		{	// (never freed: what was taken stays the heap's; no slot gets a looper the other has not)
			log->Write (From, LogWarning, "loop off: no room for %d rings of %u MB and stores of %u MB (loopram=%u)",
				    core::loop::kSlots, (unsigned) (kRingBytes >> 20), nEach, nMB);
			return;
		}
		pRing[s] = (u8 *) (((uintptr) pRing[s] + 4095) & ~(uintptr) 4095);
		pStore[s] = (u8 *) (((uintptr) pStore[s] + 4095) & ~(uintptr) 4095);
	}
	for (int s = 0; s < core::loop::kSlots; s++)
	{
		core::loop::init (s, (int32_t *) pRing[s], kRingFrames, pStore[s], (uint64_t) nEach << 20);
		SetBox (s, &core::out::kRytmMk2);	// (until a box's stream starts there: the Rytm's)
	}
	log->Write (From, LogNotice, "loop ring %u MB x %d, grabs %u MB x %d", (unsigned) (kRingBytes >> 20), core::loop::kSlots, nEach, core::loop::kSlots);
	s_Keep.init (rec_pi::CardBackend (), KeepDone);
}

void SetBox (unsigned nSlot, const core::out::Device *pDev)
{
	if (pDev == 0) pDev = &core::out::kRytmMk2;
	core::loop::set_returns ((int) nSlot, pDev->post, pDev->pre);
	if (pDev == &core::out::kRytmMk2)
	{
		core::loop::set_out_gain ((int) nSlot, masterfx_pi::kStemOnPost);	// POST reaches the Rytm's main 10.9 dB down: the loop at the level it was heard
		core::loop::set_pre_gain ((int) nSlot, masterfx_pi::kStemOnPost / masterfx_pi::kPreOnPost);	// OUT = PRE reaches it 2.0 dB down: +2.0 dB
	}
	else
	{	// not measured on another box (a named gap): unity until loop=cal measures its POST return (postgain)
		core::loop::set_out_gain ((int) nSlot, 1.0f);
		core::loop::set_pre_gain ((int) nSlot, 1.0f);
	}
}

bool WantsIn (unsigned nSlot) { return core::loop::wants_in ((int) nSlot); }

void InBlock (unsigned nSlot, const int32_t *pLR, unsigned nFrames, unsigned nLost, uint64_t nArrival)
{
	core::loop::in_block ((int) nSlot, pLR, (int) nFrames, nArrival, nLost);
}

void OutMix (unsigned nSlot, int32_t (*F)[12], unsigned nFrames, uint64_t nRenderFrame)
{
	core::loop::out_mix ((int) nSlot, F, (int) nFrames, nRenderFrame);
}

void Poll ()
{
	if (!s_Keep.busy ())
	{
		for (int k = 0; k < core::loop::kSlots; k++)
		{	// each slot asked first in turn: one box's run of KEEPs never holds the other's back
			unsigned s = (s_nNextSlot + k) % core::loop::kSlots;
			core::loop::KeepJob Job;
			if (!core::loop::on ((int) s) || !core::loop::keep_take ((int) s, &Job)) continue;
			s_nKeepSlot = s, s_nNextSlot = (s + 1) % core::loop::kSlots;
			s_Keep.begin (Job);
			break;
		}
	}
	if (s_Keep.busy ()) s_Keep.poll ();
}

bool Busy () { return s_Keep.busy (); }

}  // namespace loop_pi
