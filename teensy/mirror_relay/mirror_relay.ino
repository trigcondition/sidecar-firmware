// teensy/mirror_relay - usb=mirror through a Teensy 4.1 (docs/teensy-mirror.md, pi/engine/relay_proto.h).
// USB1 (micro-USB) goes to a Pi USB-A port: the link. USB2 (5-pin header, D+ D- GND only, VHST never connected) goes to
// the computer and shows the box the Pi mirrors. Build: teensy/mirror_relay/build.ps1 (USB type Serial: the core's
// USB1 stack is taken over at start, so the serial port is gone once it runs; the program button still flashes).
#include "relay.h"

void setup ()
{
	pinMode (LED_BUILTIN, OUTPUT);
	relay_begin ();
}

void loop ()
{
	// LED: off = no link, blinking = link up and no computer, on = the computer configured the box
	relay_poll ();
	static uint32_t t;
	if (millis () - t < 250) return;
	t = millis ();
	digitalWriteFast (LED_BUILTIN, relay_shown () ? HIGH : relay_link_up () ? ((t / 250) & 1) : LOW);
}
