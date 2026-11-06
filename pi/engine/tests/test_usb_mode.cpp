// pi/engine/tests/test_usb_mode.cpp - usb_mode.h: the USB-C mode's words, display.txt's line, where the mode comes from
// (the card, then cmdline usb=, then CLASS COMPLIANT), and what runs on a unit that cannot show it.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "usb_mode.h"

using namespace usb_mode;

// as kernel.cpp LoadDisplaySettings
static bool stored (const char *pText, Mode *pMode)
{
	bool bFound;
	return from_text (pText, pMode, &bFound);
}

int main ()
{
	// the words: the setting's and the cmdline's older ones
	Mode m = kSidecar;
	CHECK (parse ("overbridge", &m) && m == kMirror);
	CHECK (parse ("class", &m) && m == kSidecar);
	CHECK (parse ("mirror", &m) && m == kMirror);
	CHECK (parse ("sidecar", &m) && m == kSidecar);
	m = kMirror;
	CHECK (!parse ("", &m) && !parse ("OVERBRIDGE", &m) && !parse ("midi", &m) && !parse (0, &m) && m == kMirror);
	CHECK (!strcmp (name (kSidecar), "class") && !strcmp (name (kMirror), "overbridge"));
	CHECK (!strcmp (from_name (kCard), "card") && !strcmp (from_name (kCmdline), "cmdline") && !strcmp (from_name (kDefault), "default"));

	// display.txt: the value to the line's end, at a line's start only
	CHECK (stored ("screensaver=60\nusbmode=overbridge\nbrightness=100\n", &m) && m == kMirror);
	CHECK (stored ("usbmode=class\r\n", &m) && m == kSidecar);
	CHECK (stored ("brightness=100\nusbmode=overbridge", &m) && m == kMirror);	// no newline at the end
	CHECK (!stored ("screensaver=60\nbrightness=100\n", &m));
	CHECK (!stored ("xusbmode=overbridge\n", &m));						// not at a line's start
	CHECK (!stored ("usbmode=overbridgeX\n", &m) && !stored ("usbmode=\n", &m) && !stored ("usbmode=overbridge-and-more-text\n", &m));
	CHECK (stored ("xusbmode=class\nusbmode=overbridge\n", &m) && m == kMirror);		// the real one after a lookalike
	bool bFound = false;
	CHECK (!from_text ("usbmode=midi\n", &m, &bFound) && bFound);				// there, not a word: warned, dropped
	CHECK (!from_text ("brightness=5\n", &m, &bFound) && !bFound);

	// where it comes from: the card wins; then cmdline usb= (anything but mirror is the Sidecar device, as before the
	// setting); then CLASS COMPLIANT. The owner's card: usb=mirror in cmdline.txt, nothing stored: OVERBRIDGE
	From f = kCard;
	CHECK (resolve (false, kSidecar, 0, &f) == kSidecar && f == kDefault);
	CHECK (resolve (false, kSidecar, "mirror", &f) == kMirror && f == kCmdline);
	CHECK (resolve (false, kSidecar, "sidecar", &f) == kSidecar && f == kCmdline);
	CHECK (resolve (false, kMirror, "typo", &f) == kSidecar && f == kCmdline);
	CHECK (resolve (true, kSidecar, "mirror", &f) == kSidecar && f == kCard);	// chosen on the Sidecar: over usb=mirror
	CHECK (resolve (true, kMirror, 0, &f) == kMirror && f == kCard);
	CHECK (resolve (true, kMirror, "sidecar", &f) == kMirror && f == kCard);

	// what this unit can show, and what runs
	CHECK (modes (true, true) == 3u && modes (false, true) == 1u);
	CHECK (modes (true, false) == 0u && modes (false, false) == 0u);			// gadget=midi / 0, a Pi 5 without the gadget
	CHECK (effective (kMirror, 3u) == kMirror && effective (kSidecar, 3u) == kSidecar);
	CHECK (effective (kMirror, 1u) == kSidecar && effective (kMirror, 0u) == kSidecar);	// no mirror in the build: the Sidecar device
	CHECK (kModes == 2);

	// /cmd (web.cpp): a line that raises USB-C's exposure needs the strict same-origin check, as midibridge=
	CHECK (raises ("usbmode=overbridge") && raises ("usbmode=mirror") && raises ("usbmode=") && raises ("usbmode=junk"));
	CHECK (!raises ("usbmode=class") && !raises ("usbmode=sidecar") && !raises ("usbmode=cmdline") && !raises ("usbmode"));
	CHECK (!raises ("notes=1") && !raises ("usbmidi=os:on"));		// (usbmidi= is refused on /cmd outright)

	puts ("usb_mode: PASS");
	return 0;
}
