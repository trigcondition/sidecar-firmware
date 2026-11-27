// pi/engine/slip_detect.h - finds a steady test sine's slips in a stream of samples (docs/teensy-mirror.md, T344):
// the relay's loop through the A4 slipped 1-4 samples now and then; watched where the Pi sees the audio (the computer's
// OUT as it arrives, the box's IN as it arrives), a slip shows which leg it happened on. Portable; tests/test_slip_detect.cpp.
//
// Rising zero crossings (interpolated, with hysteresis) against the sine's period: each crossing's offset from the
// expected place; an offset that moves by more than kSlipSamples from the last one is a slip (its size kept, in
// hundredths of a sample) and the expectation follows it. A slow drift (two clocks) is followed, not counted.
#ifndef _slip_detect_h
#define _slip_detect_h

#include <stdint.h>

struct SlipDetect
{
	static constexpr double kSlipSamples = 0.3;	// a slip right at a crossing splits across two of them
	static constexpr unsigned kEvents = 8;

	double period = 48000.0 / 997.0;	// samples per cycle of the test sine
	int32_t thr = 1 << 20;		// ~-66 dBFS for 32-bit samples: well above the idle noise			// hysteresis: the sample must have gone below -thr before a rising crossing counts
	uint64_t n = 0;				// samples fed
	int32_t last = 0;
	bool armed = false, have = false;
	double expect = 0;			// where the next crossing is due (sample index)
	unsigned crossings = 0, slips = 0, lost = 0;	// lost: a crossing not where any is due (no sine, or a big gap)
	int32_t ev_size[kEvents];		// hundredths of a sample (+: late, -: early)
	uint32_t ev_ms[kEvents];		// the caller's time of each (uptime ms)
	unsigned ev_n = 0;

	void reset () { n = 0; armed = have = false; crossings = slips = lost = ev_n = 0; }

	void feed (int32_t s, uint32_t now_ms)
	{
		if (s < -thr) armed = true;
		if (armed && last < 0 && s >= 0)
		{
			armed = false;
			double t = (double) n - 1 + (double) (-last) / (double) ((int64_t) s - last);	// the crossing, interpolated
			crossings++;
			if (!have) { have = true; expect = t + period; }
			else
			{
				double d = t - expect;
				double k = d / period;
				long whole = (long) (k < 0 ? k - 0.5 : k + 0.5);	// crossings skipped (a gap): whole periods
				double r = d - whole * period;
				if (whole != 0) lost++;
				if (r > kSlipSamples || r < -kSlipSamples)
				{
					slips++;
					ev_size[ev_n % kEvents] = (int32_t) (r * 100.0 + (r < 0 ? -0.5 : 0.5));
					ev_ms[ev_n % kEvents] = now_ms;
					ev_n++;
					expect = t + period;		// follow the slip
				}
				else expect = t + period + r * 0.01;	// follow a slow drift
			}
		}
		last = s;
		n++;
	}
};

#endif
