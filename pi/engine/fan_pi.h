// pi/engine/fan_pi.h - the Pi 5 fan header's pin (RASPPI=5 builds only; the speed: fan.h; docs/pi5.md "The fan").
// The header's PWM line is RP1 GPIO45 (function pwm1), RP1 PWM1's fourth channel, 24 kHz, inverted (pin high = fan
// off), as Raspberry Pi's device tree drives it under Linux. The tach (RP1 GPIO29) is counted outside the ARM; Linux
// reads the result at PWM1 + 0x3c, and so does Rpm() (0 if nothing fills it under Circle: to be seen on hardware).
// Core 0 only.
#ifndef _fan_pi_h
#define _fan_pi_h

namespace fan_pi {

bool Start (void);			// the clock, the channel (duty 0: off), then the pin; false: the clock did not start
void SetDuty (unsigned nDuty);		// 0..fan::kRange
unsigned Rpm (void);			// the fan's RPM as the RP1 reports it, 0: none (or not started)
bool Started (void);

}	// namespace fan_pi

#endif
