// pi/engine/fan_pi.cpp - the Pi 5 fan header's pin (fan_pi.h). Registers as Linux's drivers/pwm/pwm-rp1.c uses them.
// Circle's CPWMOutput is not used: its Start asserts that no channel of the block runs (the firmware's boot-time fan
// check may have left one) and programs all four channels, which would also clear the RPM word at + 0x3c.
#include "fan_pi.h"
#include "fan.h"
#include <circle/bcm2712.h>
#include <circle/gpioclock.h>
#include <circle/gpiopin.h>
#include <circle/memio.h>
#include <circle/synchronize.h>
#include <circle/types.h>

namespace {

const unsigned kPin = 45;				// FAN_PWM
const unsigned kCh = 3;					// the device tree's "pwms = <&rp1_pwm1 3 ...>"
const uintptr kGlobal = ARM_PWM1_BASE + 0x00;
const uintptr kCtrl = ARM_PWM1_BASE + 0x14 + kCh * 0x10;
const uintptr kRangeReg = ARM_PWM1_BASE + 0x18 + kCh * 0x10;
const uintptr kPhase = ARM_PWM1_BASE + 0x1C + kCh * 0x10;
const uintptr kDuty = ARM_PWM1_BASE + 0x20 + kCh * 0x10;
const uintptr kRpm = ARM_PWM1_BASE + 0x3C;		// the device tree's rpm-offset
const u32 kSetUpdate = 1u << 31, kEnable = 1u << kCh;
const u32 kTrailingEdge = 1, kInvert = 1u << 3, kFifoPopMask = 1u << 8;	// pwm-rp1.c's PWM_CHANNEL_DEFAULT is pop mask + mode 1

CGPIOClock *s_pClock = 0;
CGPIOPin *s_pPin = 0;
bool s_bStarted = false;

void Update (void)
{
	write32 (kGlobal, read32 (kGlobal) | kSetUpdate);	// the new range / duty / enable take effect together
	DataSyncBarrier ();
}

}	// namespace

bool fan_pi::Start (void)
{
	if (s_bStarted) return true;
	if (s_pClock == 0) s_pClock = new CGPIOClock (GPIOClockPWM1, GPIOClockSourceXOscillator);
	if (s_pClock == 0 || !s_pClock->Start (1)) return false;	// the 50 MHz crystal, divided by 1 (fan::kClockHz); first, as
									// Linux and Circle do: the block's update latch needs its clock
	write32 (kGlobal, read32 (kGlobal) & ~kEnable);		// this channel only: stopped while it is set up
	Update ();
	write32 (kCtrl, kTrailingEdge | kInvert | kFifoPopMask);
	write32 (kRangeReg, fan::kRange);
	write32 (kPhase, 0);
	write32 (kDuty, 0);					// off until the first step
	write32 (kGlobal, read32 (kGlobal) | kEnable);
	Update ();
	if (s_pPin == 0)					// the pin last: it leaves its boot state for a running PWM
	{
		s_pPin = new CGPIOPin (kPin, GPIOModeAlternateFunction0);	// pwm1 (pinctrl-rp1.c)
		if (s_pPin != 0) s_pPin->SetPullMode (GPIOPullModeDown);	// as rp1.dtsi's rp1_pwm1_gpio45
	}
	s_bStarted = s_pPin != 0;
	return s_bStarted;
}

void fan_pi::SetDuty (unsigned nDuty)
{
	if (!s_bStarted) return;
	if (nDuty > fan::kRange) nDuty = fan::kRange;
	write32 (kDuty, nDuty);
	Update ();
}

unsigned fan_pi::Rpm (void)
{
	return s_bStarted ? read32 (kRpm) : 0;
}

bool fan_pi::Started (void)
{
	return s_bStarted;
}
