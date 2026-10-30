// pi/ui/sidecar_splash.c - the boot screen (sidecar_splash.h): plain pixels into the RGB565 frame buffer, before LVGL.
#include "sidecar_splash.h"
#include "sidecar_wordmark.h"	// "Sidecar" in the docs site's wordmark face, 4-bit alpha (wordmark.py)
#include "sidecar_dot.h"	// the OLED theme's dot grid and pixel font

static int s_Style;		// 0 OLED (the default theme), 1 CLASSIC

void sidecar_splash_style (int style) { s_Style = style == 1; }

// ---- OLED: the boot screen on the dot grid (400x240 dots on 800x480, each a 2x2 cell: sidecar_dot.h), as the UI will be

static void odot (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, int x, int y, uint16_t c)
{
	const int ox = ((int) w - (int) w / SIDECAR_DOT * SIDECAR_DOT) / 2;
	int px = ox + x * SIDECAR_DOT, py = y * SIDECAR_DOT;
	if (x < 0 || y < 0 || px + 1 >= (int) w || py + 1 >= (int) h) return;
	uint16_t *p = fb + (unsigned) py * pitch + (unsigned) px;
	uint16_t hc = (uint16_t) ((c >> 1) & 0x7BEF);
	p[0] = c; p[1] = hc; p[pitch] = hc; p[pitch + 1] = (uint16_t) ((c >> 2) & 0x39E7);
}

// text in the 5x9 face at dot (x,y), each font dot s x s grid dots, bold: a column wider, gap: dots between letters
static int otext (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, const char *t, int x, int y, int s, int bold, int gap, uint16_t c)
{
	uint32_t cp;
	while ((cp = dotfont_utf8 (&t)) != 0)
	{
		const dotglyph_t *g = dotfont_find (&dotfont_5x9, cp);
		if (!g) continue;
		int gw = g->w + (bold ? 1 : 0);
		for (int r = 0; r < dotfont_5x9.h; r++)
		{
			unsigned bits = g->row[r];
			if (bold) bits |= bits << 1;
			for (int k = 0; k < gw; k++)
				if (bits >> (gw - 1 - k) & 1)
					for (int j = 0; j < s; j++)
						for (int i = 0; i < s; i++) odot (fb, pitch, w, h, x + k * s + i, y + r * s + j, c);
		}
		x += gw * s + gap;
	}
	return x;
}

static int otext_w (const char *t, int s, int bold, int gap)
{
	int n = 0;
	uint32_t cp;
	while ((cp = dotfont_utf8 (&t)) != 0)
	{
		const dotglyph_t *g = dotfont_find (&dotfont_5x9, cp);
		if (g) n += (g->w + (bold ? 1 : 0)) * s + gap;
	}
	return n ? n - gap : 0;
}

#define O_INK	0xE73C		// #e6e6e6
#define O_DIM	0x8C51		// #8a8a8a
#define O_OFF	0x2124		// #262626: a segment not yet lit
#define O_SEG	0xFFFF		// white: the trig LEDs' play colour (palette 111)

static void oled_layout (unsigned w, unsigned h, int *gw, int *gh)
{
	*gw = (int) w / SIDECAR_DOT;
	*gh = (int) h / SIDECAR_DOT;
}

static void oled_draw (uint16_t *fb, unsigned pitch, unsigned w, unsigned h)
{
	int gw, gh;
	oled_layout (w, h, &gw, &gh);
	for (unsigned j = 0; j < h; j++)
		for (unsigned i = 0; i < w; i++) fb[j * pitch + i] = 0;
	const char *name = "SIDECAR";
	int tw = otext_w (name, 4, 1, 4), x = (gw - tw) / 2, y = gh / 2 - 52;
	otext (fb, pitch, w, h, name, x, y, 4, 1, 4, O_INK);
	const char *sub = "EXPANDER";
	int sw = otext_w (sub, 1, 0, 3), sx = (gw - sw) / 2, sy = y + 36 + 10;
	otext (fb, pitch, w, h, sub, sx, sy, 1, 0, 3, O_DIM);
	for (int i = x; i < sx - 8; i += 2) odot (fb, pitch, w, h, i, sy + 4, O_DIM);		// dotted rules either side
	for (int i = sx + sw + 8 + ((sx + sw + 8 - x) & 1); i < x + tw; i += 2) odot (fb, pitch, w, h, i, sy + 4, O_DIM);
}

// the boot stages: 8 segments of 21 x 4 dots under the name, lit one a stage
static void oled_progress (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, unsigned step, unsigned steps)
{
	int gw, gh;
	oled_layout (w, h, &gw, &gh);
	const int n = (int) steps, sw = 21, gap = 3, tot = n * (sw + gap) - gap;
	int x0 = (gw - tot) / 2, y0 = gh / 2 + 27;
	for (int k = 0; k < n; k++)
		for (int j = 0; j < 4; j++)
			for (int i = 0; i < sw; i++)
				odot (fb, pitch, w, h, x0 + k * (sw + gap) + i, y0 + j, k < (int) step ? O_SEG : O_OFF);
}

// ---- CLASSIC: the wordmark (as before)

// the docs site's dark theme in RGB565, the greys kept grey (as sidecar_ui.c)
#define BG	0x1082		// #101010
#define FG	0xEF5D		// #e8e8e8: the wordmark
#define DIM	0x8C51		// #888888: "EXPANDER"
#define EDGE	0x39C7		// #383838: the rules, the bar's track
#define ACC	0x2DEB		// #2dbd5c: the bar

// 5x7 glyphs, one byte a row (bit 4 = the left column): only the letters of "EXPANDER"
static const struct { char c; unsigned char row[7]; } Font[] = {
	{'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
	{'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
	{'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
	{'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
	{'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
	{'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
	{'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
};

static void rect (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, int x, int y, int rw, int rh, uint16_t c)
{
	for (int j = y; j < y + rh; j++)
	{
		if (j < 0 || j >= (int) h) continue;
		uint16_t *p = fb + (unsigned) j * pitch;
		for (int i = x; i < x + rw; i++)
			if (i >= 0 && i < (int) w) p[i] = c;
	}
}

// the text's width in pixels at scale s, letter gap g (in units of s)
static int text_width (const char *t, int s, int g)
{
	int n = 0;
	for (const char *p = t; *p; p++) n++;
	return n ? n * 5 * s + (n - 1) * g * s : 0;
}

static int text (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, const char *t, int x, int y, int s, int g, uint16_t c)
{
	for (; *t; t++, x += (5 + g) * s)
	{
		for (unsigned k = 0; k < sizeof Font / sizeof Font[0]; k++)
		{
			if (Font[k].c != *t) continue;
			for (int r = 0; r < dotfont_5x9.h; r++)
				for (int b = 0; b < 5; b++)
					if (Font[k].row[r] & (0x10 >> b)) rect (fb, pitch, w, h, x + b * s, y + r * s, s, s, c);
		}
	}
	return x;
}

// fg over bg at alpha a (0..15), per RGB565 channel
static uint16_t mix (uint16_t bg, uint16_t fg, unsigned a)
{
	int r = (bg >> 11) + (((fg >> 11) - (bg >> 11)) * (int) a) / 15;
	int g = ((bg >> 5) & 63) + ((((fg >> 5) & 63) - ((bg >> 5) & 63)) * (int) a) / 15;
	int b = (bg & 31) + (((fg & 31) - (bg & 31)) * (int) a) / 15;
	return (uint16_t) ((r << 11) | (g << 5) | b);
}

// the wordmark (WordmarkA4: high nibble = left pixel) at (x,y) in c over BG
static void wordmark (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, int x, int y, uint16_t c)
{
	const unsigned stride = (WORDMARK_A4_W + 1) / 2;
	for (int j = 0; j < WORDMARK_A4_H; j++)
	{
		if (y + j < 0 || y + j >= (int) h) continue;
		uint16_t *p = fb + (unsigned) (y + j) * pitch;
		for (int i = 0; i < WORDMARK_A4_W; i++)
		{
			unsigned v = WordmarkA4[j * stride + i / 2], a = i & 1 ? v & 15 : v >> 4;
			if (a && x + i >= 0 && x + i < (int) w) p[x + i] = mix (BG, c, a);
		}
	}
}

// the layout, from the screen's size: the wordmark centred, "EXPANDER" between two rules under it (the docs site's
// home page), the bar under that, all as wide as the wordmark
static void layout (unsigned w, unsigned h, int *pX, int *pY, int *pBarY)
{
	*pX = ((int) w - WORDMARK_A4_W) / 2;
	*pY = (int) h / 2 - WORDMARK_A4_H;
	*pBarY = (int) h / 2 + 46;
}

void sidecar_splash_draw (uint16_t *fb, unsigned pitch, unsigned w, unsigned h)
{
	if (!s_Style) { oled_draw (fb, pitch, w, h); oled_progress (fb, pitch, w, h, 0, 8); return; }
	int x, y, by;
	layout (w, h, &x, &y, &by);
	rect (fb, pitch, w, h, 0, 0, (int) w, (int) h, BG);
	wordmark (fb, pitch, w, h, x, y, FG);
	const int s = 2, g = 3, ty = (int) h / 2 + 12;		// EXPANDER: 5x7 at 2x, wide tracking
	int tw = text_width ("EXPANDER", s, g), tx = ((int) w - tw) / 2;
	text (fb, pitch, w, h, "EXPANDER", tx, ty, s, g, DIM);
	rect (fb, pitch, w, h, x, ty + 7, tx - 14 - x, 1, EDGE);
	rect (fb, pitch, w, h, tx + tw + 14, ty + 7, x + WORDMARK_A4_W - (tx + tw + 14), 1, EDGE);
	rect (fb, pitch, w, h, x, by, WORDMARK_A4_W, 2, EDGE);
}

void sidecar_splash_progress (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, unsigned step, unsigned steps)
{
	if (!s_Style) { if (steps) oled_progress (fb, pitch, w, h, step > steps ? steps : step, steps); return; }
	int x, y, by;
	layout (w, h, &x, &y, &by);
	if (steps == 0) return;
	if (step > steps) step = steps;
	rect (fb, pitch, w, h, x, by, (int) ((unsigned) WORDMARK_A4_W * step / steps), 2, ACC);
}
