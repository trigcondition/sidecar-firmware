// pi/engine/tests/test_slip_detect.cpp - slip_detect.h: make build/test_slip_detect
#include "slip_detect.h"
#include "check.h"
#include <cmath>
#include <cstdio>

int main ()
{
	const double P = 48000.0 / 997.0, A = 1e8;
	{	// a clean sine: crossings, no slip
		SlipDetect d;
		for (int i = 0; i < 48000; i++) d.feed ((int32_t) (A * sin (2 * M_PI * i / P)), 0);
		CHECK (d.crossings > 990 && d.slips == 0 && d.lost == 0);
	}
	{	// one sample dropped at 1 s, two repeated at 2 s: two slips, -1 and +2 (early, late)
		SlipDetect d;
		long src = 0;
		for (int i = 0; i < 3 * 48000; i++)
		{
			if (i == 48010) src++;
			if (i == 96010 || i == 96011) src--;
			d.feed ((int32_t) (A * sin (2 * M_PI * src / P)), (uint32_t) (i / 48));
			src++;
		}
		printf ("slips %u lost %u: %d@%u %d@%u %d\n", d.slips, d.lost, d.ev_size[0], d.ev_ms[0], d.ev_size[1], d.ev_ms[1], d.ev_size[2]);
		CHECK (d.slips == 2);
		CHECK (d.ev_size[0] <= -90 && d.ev_size[0] >= -110 && d.ev_ms[0] >= 1000 && d.ev_ms[0] < 1003);
		CHECK (d.ev_size[1] >= 190 && d.ev_size[1] <= 210);
	}
	{	// below the hysteresis (idle noise): no crossings, nothing counted
		SlipDetect d;
		for (int i = 0; i < 48000; i++) d.feed ((int32_t) (1e5 * sin (2 * M_PI * i / P)), 0);
		CHECK (d.crossings == 0 && d.slips == 0);
	}
	{	// a slow drift (100 ppm) is followed, not counted
		SlipDetect d;
		for (int i = 0; i < 10 * 48000; i++) d.feed ((int32_t) (A * sin (2 * M_PI * i * 1.0001 / P)), 0);
		CHECK (d.slips == 0);
	}
	puts ("test_slip_detect: ok");
	return 0;
}
