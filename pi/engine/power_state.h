// pi/engine/power_state.h - the Pi's power supply as the firmware reports it (portable: tests/test_power_state.cpp).
// get_throttled (mailbox tag 0x00030046, Pi 4 and Pi 5): bit 0 under-voltage now, 1 ARM frequency capped now,
// 2 throttled now, 3 soft temperature limit now; bits 16..19 the same "since boot" (sticky). Pi 5 only: the firmware's
// device tree /chosen/power/max_current (mA): 5000 with a 5 A USB-PD supply (the official 27 W adapter), 3000 without
// the PD negotiation (a hub port, a phone charger: reduced-power mode, USB ports limited). Measured 2026-10-19: a Pi 5
// fed from an Overhub port ran Wi-Fi badly ("httpd: Receive failed", 503s, slow page); on the 27 W adapter directly,
// none of it. The max_current reading is inferred from the firmware's DT (not yet seen on this board): -1 = absent.
#ifndef _power_state_h
#define _power_state_h


namespace powerstate {

struct State
{
	bool known = false;		// get_throttled answered
	unsigned throttled = 0;		// its raw value
	int max_ma = -1;		// Pi 5's /chosen/power/max_current, -1: not there (Pi 4) or not read
	int board = 4;			// 4 or 5 (the kernel's RASPPI): which supply the warning names
};

enum : unsigned { kUvNow = 1u << 0, kCapNow = 1u << 1, kThrNow = 1u << 2, kTempNow = 1u << 3,
		  kUvBoot = 1u << 16, kCapBoot = 1u << 17, kThrBoot = 1u << 18, kTempBoot = 1u << 19 };

// The one warning the page, the display and the log show, naming the board's official supply (Pi 4: 15 W, 5.1 V 3 A;
// Pi 5: 27 W, 5 V 5 A); 0 when the supply looks fine
const char *warning (const State &s);
// The display's short form (its bottom line is about 36 characters at 12 px); 0 when fine
const char *short_warning (const State &s);
// /status's member, without a leading comma: "power":{...}. Returns the full length (the output cut to nSize - 1 bytes and terminated).
int json (const State &s, char *pOut, unsigned nSize);
// The serial log's line (after "power: ")
int line (const State &s, char *pOut, unsigned nSize);

}	// namespace powerstate

#endif
