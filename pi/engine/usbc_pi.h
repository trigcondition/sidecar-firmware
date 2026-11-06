// pi/engine/usbc_pi.h - the USB-C mode (usb_mode.h) for the other cores: the Rytm's SIDECAR MENU > USB-C
// (core/ui/page_menu.cpp, core::ui::RemoteUsb; cores.cpp hands these to it). Core 0 owns the mode (kernel.cpp: the
// console's usbmode=, display.txt) and publishes what the page shows; the page only asks, and core 0's loop applies
// the ask as the console's usbmode= would (TakeAsk). Plain words: a stale value at worst.
#ifndef _usbc_pi_h
#define _usbc_pi_h

namespace usbc_pi {

// core 0: the mode set (usb_mode::Mode, -1 none), the modes this unit can show (usb_mode::modes: bit m), what the
// computer sees now (0 the Sidecar device, 1 the box, 2 off the bus between the two)
void Publish (int nMode, unsigned nModes, int nShown);
int Mode ();
unsigned Modes ();
int Shown ();
void Ask (int nMode);			// any core: core 0 sets it (TakeAsk)
int TakeAsk ();				// core 0's loop: the mode asked for since the last call, -1 none

}

#endif
