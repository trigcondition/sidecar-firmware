// pi/ui/touch_cal.c - see touch_cal.h
#include "touch_cal.h"

static int iabs (int v) { return v < 0 ? -v : v; }

void touch_cal_target (int i, int *fx, int *fy)
{
	*fx = i == 1 || i == 2 ? 900 : 100;
	*fy = i >= 2 ? 900 : 100;
}

int touch_cal_compute (const int raw[TOUCH_CAL_POINTS][2], touch_cal_t *out)
{
	// which raw axis changes from the left targets (0, 3) to the right ones (1, 2): that one runs along x
	int dxx = iabs ((raw[1][0] + raw[2][0]) - (raw[0][0] + raw[3][0]));
	int dyx = iabs ((raw[1][1] + raw[2][1]) - (raw[0][1] + raw[3][1]));
	int swap = dyx > dxx;
	int a[4], b[4];
	for (int i = 0; i < 4; i++) { a[i] = raw[i][swap]; b[i] = raw[i][!swap]; }
	int left = (a[0] + a[3]) / 2, right = (a[1] + a[2]) / 2;
	int top = (b[0] + b[1]) / 2, bottom = (b[2] + b[3]) / 2;
	int sx = right - left, sy = bottom - top;		// raw over 80 % of the screen (signed: inverted axes)
	if (iabs (sx) < 16 || iabs (sy) < 16) return 0;
	// the two targets of an edge agree to within a quarter of the span (else a target was missed)
	if (iabs (a[0] - a[3]) * 4 > iabs (sx) || iabs (a[1] - a[2]) * 4 > iabs (sx)) return 0;
	if (iabs (b[0] - b[1]) * 4 > iabs (sy) || iabs (b[2] - b[3]) * 4 > iabs (sy)) return 0;
	out->swap = swap;
	out->x0 = left - sx / 8;				// the targets are at 10 % and 90 %: the edges 1/8 of the span out
	out->x1 = right + sx / 8;
	out->y0 = top - sy / 8;
	out->y1 = bottom + sy / 8;
	return 1;
}

static int map1 (int r, int r0, int r1, int n)
{
	if (r1 == r0 || n <= 1) return 0;
	long long v = (long long) (r - r0) * (n - 1) / (r1 - r0);
	return v < 0 ? 0 : v > n - 1 ? n - 1 : (int) v;
}

void touch_cal_map (const touch_cal_t *c, unsigned raw_x, unsigned raw_y, int w, int h, int *x, int *y)
{
	int a = c->swap ? (int) raw_y : (int) raw_x, b = c->swap ? (int) raw_x : (int) raw_y;
	*x = map1 (a, c->x0, c->x1, w);
	*y = map1 (b, c->y0, c->y1, h);
}

int touch_cal_parse (const char *s, touch_cal_t *out)
{
	int v[5], n = 0;
	if (!s) return 0;
	while (n < 5)
	{
		int neg = 0, d = 0, x = 0;
		if (*s == '-') { neg = 1; s++; }
		while (*s >= '0' && *s <= '9' && d < 9) { x = x * 10 + (*s++ - '0'); d++; }
		if (!d) return 0;
		v[n++] = neg ? -x : x;
		if (n < 5) { if (*s != ',') return 0; s++; }
	}
	if (*s && *s != '\n' && *s != '\r') return 0;
	if (v[0] == v[1] || v[2] == v[3] || (v[4] != 0 && v[4] != 1)) return 0;
	out->x0 = v[0]; out->x1 = v[1]; out->y0 = v[2]; out->y1 = v[3]; out->swap = v[4];
	return 1;
}

void touch_cal_format (const touch_cal_t *c, char *buf, int n)
{
	// no printf on every target: a small integer writer
	int p = 0;
	const int v[5] = {c->x0, c->x1, c->y0, c->y1, c->swap};
	for (int k = 0; k < 5; k++)
	{
		char t[12];
		int m = 0, x = v[k];
		unsigned u = x < 0 ? (unsigned) -x : (unsigned) x;
		do { t[m++] = (char) ('0' + u % 10); u /= 10; } while (u);
		if (x < 0) t[m++] = '-';
		if (k && p < n - 1) buf[p++] = ',';
		while (m && p < n - 1) buf[p++] = t[--m];
	}
	if (n > 0) buf[p < n ? p : n - 1] = 0;
}
