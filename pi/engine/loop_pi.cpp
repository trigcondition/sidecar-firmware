// pi/engine/loop_pi.cpp - see loop_pi.h. Circle's side: the boot reservation and the KEEP writer's card backend.
#include "loop_pi.h"
#include "masterfx_pi.h"

#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/string.h>

#include "loop/loop.h"
#include "sidecar_features.h"
#include "loop_keep.h"
#include "rec_pi.h"

namespace loop_pi {

static const char From[] = "loop";
static loop_pi::Keep s_Keep;

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
	// from the heap, before the pool RAM is taken (platform_pi.cpp pool_ram_budget reads what is left); never freed
	u8 *pRing = (u8 *) CMemorySystem::HeapAllocate ((size_t) kRingBytes + 4096, HEAP_ANY);
	u8 *pStore = pRing ? (u8 *) CMemorySystem::HeapAllocate (((size_t) nMB << 20) + 4096, HEAP_ANY) : 0;
	if (!pRing || !pStore)
	{
		log->Write (From, LogWarning, "loop off: no room for a %u MB ring and a %u MB store (loopram=%u)", (unsigned) (kRingBytes >> 20), nMB, nMB);
		return;
	}
	pRing = (u8 *) (((uintptr) pRing + 4095) & ~(uintptr) 4095);
	pStore = (u8 *) (((uintptr) pStore + 4095) & ~(uintptr) 4095);
	core::loop::init ((int32_t *) pRing, kRingFrames, pStore, (uint64_t) nMB << 20);
	log->Write (From, LogNotice, "loop ring %u MB, grabs %u MB", (unsigned) (kRingBytes >> 20), nMB);
	s_Keep.init (rec_pi::CardBackend (), core::loop::keep_finished);
	core::loop::set_out_gain (masterfx_pi::kStemOnPost);	// POST reaches the Rytm's main 10.9 dB down: the loop at the level it was heard
	core::loop::set_pre_gain (masterfx_pi::kStemOnPost / masterfx_pi::kPreOnPost);	// OUT = PRE reaches it 2.0 dB down: +2.0 dB
}

bool WantsIn () { return core::loop::wants_in (); }

void InBlock (const int32_t *pLR, unsigned nFrames, unsigned nLost, uint64_t nArrival)
{
	core::loop::in_block (pLR, (int) nFrames, nArrival, nLost);
}

void OutMix (int32_t (*F)[12], unsigned nFrames, uint64_t nRenderFrame)
{
	core::loop::out_mix (F, (int) nFrames, nRenderFrame);
}

void Poll ()
{
	if (!core::loop::on ()) return;
	if (!s_Keep.busy ())
	{
		core::loop::KeepJob Job;
		if (core::loop::keep_take (&Job)) s_Keep.begin (Job);
	}
	if (s_Keep.busy ()) s_Keep.poll ();
}

bool Busy () { return s_Keep.busy (); }

}  // namespace loop_pi
