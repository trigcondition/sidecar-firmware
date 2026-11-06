// pi/engine/usb_mode.h - what the Pi's USB-C side (Pi 4, Pi 5 with PI5_GADGET=1) shows the computer: the USB-C mode,
// a Sidecar setting (docs/usb-mirror.md "The setting"). Portable, header only (tests/test_usb_mode.cpp).
//
//   CLASS COMPLIANT ("class"; cmdline usb=sidecar): the Sidecar's own device, UAC2 12 x 12 + USB MIDI (sidecar_gadget.h)
//   OVERBRIDGE ("overbridge"; cmdline usb=mirror): the attached box itself while it is there (docs/usb-mirror.md)
//
// Where it comes from, first match wins: usbmode= in /.rytm/display.txt (stored by the web page's Sound tab, Settings >
// Sound on the touchscreen, or the Rytm's SIDECAR MENU > USB-C: the console's usbmode=), then cmdline.txt's usb=, then
// CLASS COMPLIANT. So a card with usb=mirror in cmdline.txt and nothing stored stays OVERBRIDGE, and a choice made on
// the Sidecar wins over cmdline.txt until usbmode=cmdline drops it again. MIDI only and off are still cmdline.txt's
// gadget=midi / gadget=0 (another gadget, chosen at boot): they would be further Modes here, with their words, before
// kModes.
#ifndef _usb_mode_h
#define _usb_mode_h

#include <string.h>

namespace usb_mode {

enum Mode { kSidecar, kMirror, kModes };
enum From { kDefault, kCmdline, kCard };

// "class" / "overbridge", and the cmdline's words "sidecar" / "mirror"; false: none of them (*pMode unchanged)
inline bool parse (const char *p, Mode *pMode)
{
	if (p == 0) return false;
	if (strcmp (p, "class") == 0 || strcmp (p, "sidecar") == 0) { *pMode = kSidecar; return true; }
	if (strcmp (p, "overbridge") == 0 || strcmp (p, "mirror") == 0) { *pMode = kMirror; return true; }
	return false;
}

// display.txt's value, up to the line's end ("overbridge\nbrightness=..."); false: not one of them
inline bool parse_line (const char *p, Mode *pMode)
{
	if (p == 0) return false;
	char b[16];
	unsigned n = 0;
	while (p[n] != 0 && p[n] != '\n' && p[n] != '\r' && n < sizeof b - 1) { b[n] = p[n]; n++; }
	if (p[n] != 0 && p[n] != '\n' && p[n] != '\r') return false;	// longer than any of them
	b[n] = 0;
	return parse (b, pMode);
}

// display.txt's text: its usbmode= line (at a line's start). *pFound: there is one; true: it is one of the words
inline bool from_text (const char *pText, Mode *pMode, bool *pFound)
{
	const char *p = strstr (pText, "usbmode=");
	while (p != 0 && p != pText && p[-1] != '\n') p = strstr (p + 8, "usbmode=");
	*pFound = p != 0;
	return p != 0 && parse_line (p + 8, pMode);
}

// A console line that may raise USB-C's exposure (usbmode= anything but class, sidecar or cmdline: OVERBRIDGE passes the
// box's requests through unfiltered): web.cpp's /cmd takes it only with the strict same-origin check, as midibridge=
inline bool raises (const char *pCmd)
{
	if (strncmp (pCmd, "usbmode=", 8) != 0) return false;
	const char *v = pCmd + 8;
	return strcmp (v, "class") != 0 && strcmp (v, "sidecar") != 0 && strcmp (v, "cmdline") != 0;
}

inline const char *name (Mode m)		{ return m == kMirror ? "overbridge" : "class"; }	// the setting's word
inline const char *from_name (From f)		{ return f == kCard ? "card" : f == kCmdline ? "cmdline" : "default"; }

// The mode chosen: the stored one if any (bStored), else the cmdline's usb= (anything but "mirror" is the Sidecar
// device, as before the setting), else the default. *pFrom: which of them it was.
inline Mode resolve (bool bStored, Mode Stored, const char *pCmdline, From *pFrom)
{
	if (bStored) { *pFrom = kCard; return Stored; }
	if (pCmdline != 0) { *pFrom = kCmdline; return strcmp (pCmdline, "mirror") == 0 ? kMirror : kSidecar; }
	*pFrom = kDefault;
	return kSidecar;
}

// The modes this build and card can show, bit m for Mode m: none without the Sidecar gadget (cmdline gadget=midi or 0,
// a Pi 5 built without PI5_GADGET=1); OVERBRIDGE only with the mirror's Circle patches (CIRCLE_GADGET_MIRROR)
inline unsigned modes (bool bMirrorBuilt, bool bSidecarGadget)
{
	if (!bSidecarGadget) return 0;
	return 1u << kSidecar | (bMirrorBuilt ? 1u << kMirror : 0u);
}

// What runs: a mode the build cannot show runs as CLASS COMPLIANT (the setting is kept for a build that can)
inline Mode effective (Mode m, unsigned nModes)	{ return nModes & (1u << m) ? m : kSidecar; }

}	// namespace usb_mode

#endif
