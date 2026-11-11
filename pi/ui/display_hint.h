// pi/ui/display_hint.h - the Pi 5's HDMI fallback when the screen did not answer at power-on: the firmware read no EDID
// and set up 640x480, which Circle on the Pi 5 cannot change (docs/pi5.md "HDMI display"). Plain C, no Circle, so
// pi/engine/tests/test_display_hint.cpp checks it on the Mac. sidecar_display.cpp logs it, draws SIDECAR_NOEDID_TEXT on
// the boot screen (sidecar_splash_hint) and puts SIDECAR_NOEDID_STATUS in /status ("displayHint").
#ifndef SIDECAR_DISPLAY_HINT_H
#define SIDECAR_DISPLAY_HINT_H

// the boot screen's lines ('\n' between them; the dot font has capitals only)
#define SIDECAR_NOEDID_TEXT	"No screen info at power-on.\nPower the screen first,\nthen the Pi."
// /status "displayHint"
#define SIDECAR_NOEDID_STATUS	"No screen info at power-on: power the screen first, then the Pi (docs/pi5.md, HDMI display)"

// 1: the frame buffer is the firmware's 640x480 fallback and the EDID gave nothing usable (no header, or no preferred
// mode). A screen that does send 640x480 as its preferred mode is left alone (edid_ok).
static inline int sidecar_no_edid_fallback (unsigned w, unsigned h, int edid_ok)
{
	return w == 640 && h == 480 && !edid_ok;
}

#endif
