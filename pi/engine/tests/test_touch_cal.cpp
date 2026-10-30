// test_touch_cal.cpp - pi/ui/touch_cal.c (the 4-point touch calibration, Settings > Display) on the Mac: straight,
// inverted, swapped, offset panels; bad point sets refused; touchcal= parse / format
#include "../../ui/touch_cal.c"
#include "check.h"
#include <cstdio>
#include <cstring>

// a simulated panel: screen thousandths -> raw (offset, scale, optional swap / inversion)
static void panel (int fx, int fy, int swap, int inv_x, int inv_y, int *rx, int *ry)
{
	int a = 1200 + fx * 30, b = 900 + fy * 31;	// raw along the screen's x / y
	if (inv_x) a = 32767 - a;
	if (inv_y) b = 32767 - b;
	*rx = swap ? b : a;
	*ry = swap ? a : b;
}

static void roundtrip (int swap, int inv_x, int inv_y)
{
	int raw[4][2];
	for (int i = 0; i < 4; i++)
	{
		int fx, fy;
		touch_cal_target (i, &fx, &fy);
		panel (fx, fy, swap, inv_x, inv_y, &raw[i][0], &raw[i][1]);
	}
	touch_cal_t c;
	CHECK (touch_cal_compute (raw, &c));
	CHECK (c.swap == swap);
	// the targets land on their pixels (1024x600: 10 % = 102 / 60), the centre on the centre, the corners on the corners
	for (int i = 0; i < 4; i++)
	{
		int fx, fy, x, y;
		touch_cal_target (i, &fx, &fy);
		touch_cal_map (&c, raw[i][0], raw[i][1], 1024, 600, &x, &y);
		CHECK (x >= fx * 1023 / 1000 - 2 && x <= fx * 1023 / 1000 + 2);
		CHECK (y >= fy * 599 / 1000 - 2 && y <= fy * 599 / 1000 + 2);
	}
	int rx, ry, x, y;
	panel (500, 500, swap, inv_x, inv_y, &rx, &ry);
	touch_cal_map (&c, rx, ry, 1024, 600, &x, &y);
	CHECK (x >= 509 && x <= 513 && y >= 297 && y <= 301);
	panel (0, 0, swap, inv_x, inv_y, &rx, &ry);
	touch_cal_map (&c, rx, ry, 1024, 600, &x, &y);
	CHECK (x <= 2 && y <= 2);
	panel (1000, 1000, swap, inv_x, inv_y, &rx, &ry);
	touch_cal_map (&c, rx, ry, 1024, 600, &x, &y);
	CHECK (x >= 1021 && y >= 597);
	// beyond the edge: clamped
	touch_cal_map (&c, swap ? ry : (inv_x ? 0 : 32767), swap ? (inv_x ? 0 : 32767) : ry, 1024, 600, &x, &y);
	CHECK (x == 1023);
	// touchcal= round trip
	char b[64];
	touch_cal_format (&c, b, sizeof b);
	touch_cal_t d;
	CHECK (touch_cal_parse (b, &d));
	CHECK (!memcmp (&c, &d, sizeof c));
}

int main ()
{
	for (int s = 0; s < 2; s++)
		for (int ix = 0; ix < 2; ix++)
			for (int iy = 0; iy < 2; iy++) roundtrip (s, ix, iy);

	// the GeeekPi's linear default (0..32767 = the whole screen) is what a perfect calibration gives
	int raw[4][2];
	for (int i = 0; i < 4; i++) { int fx, fy; touch_cal_target (i, &fx, &fy); raw[i][0] = fx * 32767 / 1000; raw[i][1] = fy * 32767 / 1000; }
	touch_cal_t c;
	CHECK (touch_cal_compute (raw, &c) && !c.swap);
	CHECK (c.x0 > -50 && c.x0 < 50 && c.x1 > 32717 && c.x1 < 32817);

	// refused: all four in one place, a target missed (the bottom left touched at the top left's place)
	int same[4][2] = {{100, 100}, {102, 101}, {101, 99}, {100, 102}};
	CHECK (!touch_cal_compute (same, &c));
	int missed[4][2] = {{3000, 3000}, {29000, 3000}, {29000, 29000}, {29000, 3100}};
	CHECK (!touch_cal_compute (missed, &c));

	// parse: negatives, a line end; refused: too few, zero spans, a bad swap, words
	CHECK (touch_cal_parse ("-120,32900,40,32700,0\n", &c) && c.x0 == -120 && c.y1 == 32700 && !c.swap);
	CHECK (!touch_cal_parse ("1,2,3", &c));
	CHECK (!touch_cal_parse ("5,5,0,100,0", &c));
	CHECK (!touch_cal_parse ("0,100,0,100,2", &c));
	CHECK (!touch_cal_parse ("off", &c));
	CHECK (!touch_cal_parse ("", &c));
	char b[64];
	c.x0 = -7; c.x1 = 32767; c.y0 = 0; c.y1 = -32768; c.swap = 1;
	touch_cal_format (&c, b, sizeof b);
	CHECK (!strcmp (b, "-7,32767,0,-32768,1"));

	printf ("test_touch_cal: OK\n");
	return 0;
}
