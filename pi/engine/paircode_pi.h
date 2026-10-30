// pi/engine/paircode_pi.h - the pairing code's Pi side (the portable part is paircode.h; docs/pi-engine.md "Pairing
// code"): the card (FatFs "SD:"), the hardware random numbers, the clock, the boot messages, /status, the STATUS page's
// row and the Rytm's popup, and the console's `pair` commands.
//
// Everything here runs on core 0 (the kernel's loop and the web and SD-file tasks, which are Circle tasks on core 0 and
// never preempt each other): one Gate with no lock. The Rytm's popup is the exception - it is drawn by the control
// core - so the code is handed over through Show()/TakeShow() and nothing else of the gate is touched there.
#ifndef _pi_engine_paircode_pi_h
#define _pi_engine_paircode_pi_h

#include <circle/string.h>
#include <circle/types.h>

#include "paircode.h"
#include "../ui/sidecar_status.h"

namespace paircode_pi {

// Once at boot, after the card is mounted and before the network starts: cmdline.txt's paircode=, the code itself and
// the remembered devices from /.rytm/pair.txt. Logs the gate's state, never the code.
// bDisplay: a panel came up. bWanted: cmdline.txt asked for one (display=1, the default). With no display at all the
// code would be nowhere to read, so it goes to the card's root (paircode::kRecoursePath) - said out loud, because
// bWanted && !bDisplay means a panel was configured and did not come up, which is a surprise, not a choice. With a
// display, a file left by an earlier headless boot is deleted.
void Init (const char *pCmdline, bool bDisplay, bool bWanted);
paircode::Gate &Gate ();
void Random (u8 *pOut, unsigned n);		// the hardware generator mixed with the timer (the code's, the bridge's token)
void Save ();					// /.rytm/pair.txt, only when something changed
void Status (CString *pOut);			// /status's ",\"pair\":{...}" - the code is never in it
void WarnTick ();				// once a minute: the loud line while the gate is off
void UiFill (sidecar_status_view_t *v);		// the STATUS page's PAIR CODE row
bool Command (const char *p);			// `pair`, `pair=show`, `pair=revoke` (false: not one of ours)
void Show ();					// ask for the code on the Rytm's screen (any core)
// An unpaired page asked for the code (POST /pair/show: the dialog opening with no usable saved pairing, or its button):
// show it, at most once every kShowGapS. Not on GET / any more - a page load never carries the token, so a PAIRED
// owner's every reload flashed the code over the Rytm. /pair/show is open to anyone on the network, so without the
// gap an idle attacker could put a box over the Rytm's screen whenever the user is playing.
// Nothing leaks either way (the code is already on the touchscreen and neither screen is visible from the network),
// so this is about disruption, not disclosure. The gap is a fixed 10 s (paircode::ShowGap): a burst of reloads
// collapses into one (Show() counts requests, not modals), and the page says how long is left (the answer's wait).
constexpr unsigned kShowGapS = paircode::kShowGapS;
bool ShowUnpaired (unsigned *pWait);		// true: shown; false: refused, *pWait the seconds left of the gap (0: gate off)
void Hide ();					// a device paired (core 0): off the Rytm's screen, and the show gap reset
// The control core, at each new Rytm hello (a connect, a Rytm restart): the code on its screen again while the gate is
// on and no device is paired yet - a headless Sidecar's only screen is the Rytm's. At most once a minute (the Rytm says hello again 1 s and 2 s
// later, and after a silence), and never before Init.
void RytmHello ();
// The control core: the SIDECAR status page's line (core::ui::remote_pair_code, UP + T10 on the Rytm, and the
// SIDECAR MENU's WEB UI page). true when it changed since *pGen (Init ran, a NEW CODE): pLine the grouped code, or ""
// (the gate off, the RNG failed); false: nothing new.
bool StatusLine (char *pLine, unsigned nSize, unsigned *pGen);

// ---- the Rytm's SIDECAR MENU > WEB UI (core/ui/page_menu.cpp; cores.cpp hands these to core::ui::remote_webui).
// Any core: each only asks, and Poll (core 0, where the gate lives) does it.
constexpr const char *kWebOff = "the web UI is off on the Rytm (SIDECAR MENU > WEB UI)";
bool WebOn ();				// the web page and port 7310 from the network serve (default on; pair.txt web=0)
void MenuWeb (bool bOn);		// takes effect at once; Poll keeps it on the card (not with the gate off / no RNG:
					// pair.txt is never touched then, so OFF holds for that boot only)
int MenuGate ();			// 0 off (cmdline), 1 on, 2 fixed, 3 the RNG failed
int MenuDevices ();
bool MenuNewCode ();			// false: not on this gate (a fixed code, off, no RNG)
void MenuForget ();
void Poll ();				// core 0's loop
bool TakeHide ();				// the control core: true once per Hide()
// The control core: true and the line to show (a try a second). The request stays owed until ShowDone, because the
// Rytm drops a popup it cannot show; ShowFailed retries and, after ten, gives up with a line the user can act on.
bool TakeShow (char *pLine, unsigned nSize);
void ShowDone ();
void ShowFailed ();

}

#endif
