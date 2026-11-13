// pi/engine/web_cmd.h - which console commands the web may send (POST /cmd and the /ws "cmd" request, web.cpp and
// web_ws.cpp): one list for both, so they cannot drift apart. Portable, header only (tests/test_ws.cpp).
// The same-origin checks are web.cpp's (they need the request's headers); this is only the command's own text.
#ifndef _web_cmd_h
#define _web_cmd_h

#include <string.h>

namespace webcmd {

// Why the web may not send this command, 0 when it may. bBody: it came in a POST's body or a /ws message (never a URL:
// wifi= carries a key). bOsUpdate: usbmidi=os: is on (sidecar_gadget.h's OsUpdate).
//   midibridge=, usbmidi= (and its older name osupdate=): they let OS upgrades through; POST /midibridge/set (strict
//   same origin), cmdline.txt or the serial console only
//   link=tcp:<address> while usbmidi=os: is on: it would send the computer's MIDI (an OS image) to any host
//   wdtest: hangs core 0 on purpose (the watchdog's test)
inline const char *refused (const char *Cmd, bool bBody, bool bOsUpdate)
{
	// (named Cmd as in web.cpp: pi/engine/tests/page_pair.test.mjs reads these conditions)
	if (Cmd == 0 || Cmd[0] == 0) return "an empty command";
	if (strcmp (Cmd, "wdtest") == 0) return "wdtest is the serial console's only";
	if ((!bBody && strncmp (Cmd, "wifi=", 5) == 0) || strncmp (Cmd, "midibridge=", 11) == 0
	    || strncmp (Cmd, "usbmidi=", 8) == 0 || strncmp (Cmd, "osupdate=", 9) == 0
	    || (strncmp (Cmd, "link=tcp:", 9) == 0 && bOsUpdate))
		return "not taken on /cmd (wifi= in the URL, midibridge=, usbmidi=, osupdate=, link=tcp: with usbmidi=os:)";
	return 0;
}

}	// namespace webcmd

#endif
