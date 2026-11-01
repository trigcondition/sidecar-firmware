// pi/engine/main.cpp - Circle's entry.
#include <circle/startup.h>

#include "kernel.h"

int main (void)
{
	// On the heap, not on main's stack: CKernel is about 120 KB and the stack 128 KB. What was left was too little, so a
	// DMA_BUFFER in a USB send landed under MEM_KERNEL_END, which Circle's xHCI asserts: the Pi 5 stopped at its first
	// USB MIDI message to the Rytm.
	CKernel *pKernel = new CKernel;
	if (!pKernel->Initialize ())
	{
		halt ();
		return EXIT_HALT;
	}
	pKernel->Run ();
	halt ();
	return EXIT_HALT;
}
