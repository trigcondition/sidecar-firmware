// pi/ui/sidecar_dot.c - see sidecar_dot.h and sidecar_dotfont.h: the dot expansion, the LED palette, the dot fonts' lookup
// and plain drawing (no LVGL: the boot screen uses these).
#include "sidecar_dot.h"
#include <string.h>

// the pattern colours (sidecar_theme.h DOT_*) as RGB565: 0x080808 0x080810 0x080818 0x080820
#define P_DITHER	0x0841
#define P_DOTTED	0x0842
#define P_SHADE		0x0843
#define P_ALERT		0x0844

static uint16_t scale565 (uint16_t c, int k)
{
	unsigned r = (unsigned) (c >> 11) * (unsigned) k >> 8, g = (unsigned) ((c >> 5) & 63) * (unsigned) k >> 8;
	unsigned b = (unsigned) (c & 31) * (unsigned) k >> 8;
	return (uint16_t) (r << 11 | g << 5 | b);
}

static int s_Look;
void sidecar_dot_look (int stage) { s_Look = stage ? 1 : 0; }
int sidecar_dot_get_look (void) { return s_Look; }

// a grey (r, g, b within a step of each other) a step lighter (STAGE)
static uint16_t lift565 (uint16_t c)
{
	unsigned r = c >> 11, g = (c >> 5) & 63, b = c & 31;
	unsigned r8 = r << 3, g8 = g << 2, b8 = b << 3;
	unsigned mx = r8 > g8 ? (r8 > b8 ? r8 : b8) : (g8 > b8 ? g8 : b8), mn = r8 < g8 ? (r8 < b8 ? r8 : b8) : (g8 < b8 ? g8 : b8);
	if (mx - mn > 12) return c;				// a colour: kept
	unsigned v = 0x30 + mx * 0xD0 / 0xFF;
	return (uint16_t) ((v >> 3) << 11 | (v >> 2) << 5 | (v >> 3));
}

void sidecar_dot_expand (const uint16_t *src, int x0, int y0, int sw, int sh, int src_pitch, uint16_t *dst, int dst_pitch,
			 uint16_t off, int bright)
{
	// the patterns' two colours (checkerboard: dot x + y even / odd), dimmed with the rest
	uint16_t pat[4][2] = {{0xE71C, 0xCE59}, {0xCE59, 0}, {0x632C, 0}, {0xAEFE, 0x55BB}};	// #e0e0e0/#c8c8c8 (a step lighter
	// than a checked key: black title text reads), #c8c8c8, #606060 (lighter: they vanished off-axis), teal tint / teal
	if (bright < 256)
		for (int i = 0; i < 4; i++) { pat[i][0] = scale565 (pat[i][0], bright); pat[i][1] = scale565 (pat[i][1], bright); }
	for (int y = 0; y < sh; y++)
	{
		const uint16_t *s = src + y * src_pitch;
		uint16_t *d0 = dst + (y * SIDECAR_DOT) * dst_pitch, *d1 = d0 + dst_pitch;
		int par = (x0 + y0 + y) & 1;
		for (int x = 0; x < sw; x++, par ^= 1)		// a dot: [c h] over [h q]
		{
			uint16_t c = s[x];
			if (c >= P_DITHER && c <= P_ALERT) c = pat[c - P_DITHER][par];
			else if (!c) c = off;
			else
			{
				if (s_Look) c = lift565 (c);
				if (bright < 256) c = scale565 (c, bright);
			}
			if (s_Look)					// STAGE: the whole cell
			{
				d0[0] = d0[1] = d1[0] = d1[1] = c;
				d0 += 2; d1 += 2;
				continue;
			}
			uint16_t h = (uint16_t) ((c >> 1) & 0x7BEF), q = (uint16_t) ((c >> 2) & 0x39E7);
			d0[0] = c; d0[1] = h;
			d1[0] = h; d1[1] = q;
			d0 += 2; d1 += 2;
		}
	}
}

uint32_t sidecar_led_rgb (int c)
{
	static const unsigned char kHue[7][3] = {{255, 40, 30}, {40, 255, 70}, {50, 110, 255}, {255, 170, 20}, {30, 230, 255},
						  {255, 50, 220}, {255, 255, 255}};
	if (c < 0 || c >= 112 || c % 16 == 0) return 0x1C1C22;
	int i = c % 16;
	int f = 120 + 880 * i * i / 225;			// 0.12 + 0.88 i^2, x1000 (sim/seq_sim.py)
	uint32_t rgb = 0;
	for (int k = 0; k < 3; k++)
	{
		int v = 28 + (kHue[c / 16][k] - 28) * f / 1000;
		rgb = rgb << 8 | (uint32_t) v;
	}
	return rgb;
}

uint32_t dotfont_utf8 (const char **pp)
{
	const unsigned char *p = (const unsigned char *) *pp;
	uint32_t c = *p;
	if (!c) return 0;
	int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
	if (n) c &= 0x3F >> n;
	p++;
	for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++, p++) c = c << 6 | (*p & 0x3F);
	*pp = (const char *) p;
	return c;
}

const dotglyph_t *dotfont_find (const dotfont_t *f, uint32_t cp)
{
	if (cp >= 'a' && cp <= 'z') cp -= 32;
	int lo = 0, hi = f->n - 1;
	while (lo <= hi)
	{
		int m = (lo + hi) / 2;
		if (f->g[m].cp == cp) return &f->g[m];
		if (f->g[m].cp < cp) lo = m + 1; else hi = m - 1;
	}
	return 0;
}

int dotfont_width (const dotfont_t *f, const char *t, int bold)
{
	int w = 0;
	uint32_t c;
	while ((c = dotfont_utf8 (&t)) != 0)
	{
		const dotglyph_t *g = dotfont_find (f, c);
		if (!g) g = dotfont_find (f, '?');
		w += g->w + 1 + (bold ? 1 : 0);
	}
	return w ? w - 1 : 0;
}

int dotfont_draw (const dotfont_t *f, const char *t, int x, int y, int s, int bold, uint16_t col,
		  uint16_t *fb, unsigned pitch, unsigned w, unsigned h)
{
	uint32_t c;
	while ((c = dotfont_utf8 (&t)) != 0)
	{
		const dotglyph_t *g = dotfont_find (f, c);
		if (!g) g = dotfont_find (f, '?');
		for (int r = 0; r < f->h; r++)
			for (int k = 0; k < g->w + (bold ? 1 : 0); k++)
			{
				int on = (k < g->w && (g->row[r] >> (g->w - 1 - k) & 1)) || (bold && k > 0 && (g->row[r] >> (g->w - k) & 1));
				if (!on) continue;
				for (int j = 0; j < s; j++)
				{
					int py = y + r * s + j;
					if (py < 0 || py >= (int) h) continue;
					for (int i = 0; i < s; i++)
					{
						int px = x + k * s + i;
						if (px >= 0 && px < (int) w) fb[(unsigned) py * pitch + (unsigned) px] = col;
					}
				}
			}
		x += (g->w + 1 + (bold ? 1 : 0)) * s;
	}
	return x;
}
