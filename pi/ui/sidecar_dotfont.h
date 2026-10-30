// pi/ui/sidecar_dotfont.h - the OLED theme's pixel fonts (pi/ui/dotfont.py, docs/UI-THEMES.md "Font"): plain tables, no
// LVGL, so the boot screen (sidecar_splash.c) draws with them too. sidecar_theme.c wraps them as LVGL fonts.
#ifndef SIDECAR_DOTFONT_H
#define SIDECAR_DOTFONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uint32_t	cp;		// the codepoint (upper case only; dotfont_find maps a..z)
	uint8_t		w;		// columns, 1..8 (rows: up to 10)
	uint8_t		row[10];	// h rows, bit w-1 = the leftmost column
} dotglyph_t;

typedef struct
{
	uint8_t			h;	// rows
	uint16_t		n;	// glyphs, sorted by codepoint
	const dotglyph_t	*g;
} dotfont_t;

extern const dotfont_t dotfont_5x7;	// the text face
extern const dotfont_t dotfont_3x5;	// the micro face
extern const dotfont_t dotfont_4x6;	// one step below 5x7 (the 3x3 grid's body face)
extern const dotfont_t dotfont_5x9;	// the body and title face: 5x7 stretched to 9 rows

const dotglyph_t *dotfont_find (const dotfont_t *f, uint32_t cp);	// 0: not in the font ('?' is the caller's)
int dotfont_width (const dotfont_t *f, const char *utf8, int bold);	// the text's width in dots (scale 1)
// Draws utf8 at (x,y) (the glyph box's top left) into an RGB565 buffer, scaled by s (each dot s x s), bold: each
// column twice. Returns the x after the text.
int dotfont_draw (const dotfont_t *f, const char *utf8, int x, int y, int s, int bold, uint16_t c,
		  uint16_t *fb, unsigned pitch, unsigned w, unsigned h);
uint32_t dotfont_utf8 (const char **p);					// the next codepoint, 0 at the end

#ifdef __cplusplus
}
#endif

#endif
