// teensy/mirror_relay/relay.h - see relay.cpp
#pragma once
#include <stdint.h>

extern volatile uint32_t relay_stage;	// 1 USB1 taken over, 2 USB2 PHY, 5 running; 0x82 PLL7 no lock
void relay_begin (void);
void relay_poll (void);			// from loop(): the reboot into the bootloader
bool relay_link_up (void);		// the Pi configured the link
bool relay_shown (void);		// the computer configured the mirrored box
void rlog (const char *fmt, ...);	// a line for the Pi to read (kReqLog)
