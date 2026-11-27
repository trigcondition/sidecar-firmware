// pi/engine/ui_style.h - the display's theme words (docs/UI-THEMES.md): uistyle=oled|sidecar|guide in cmdline.txt,
// the console command and /.sidecar/display.txt. The ids are the ones of sidecar_theme.h (THEME_*). Portable, header only
// (tests/test_ui_style.cpp).
//
//   oled     0  the default (a card without the key, or a word not known, reads as this)
//   sidecar  1  ("classic", the name of the theme SIDECAR replaced, and "1" are read as this)
//   grid     2  GRID, the theme that was called GUIDE: its stored word stays "guide" (a saved display.txt or cmdline.txt keeps working,
//               and name () still writes it), "grid" is read as well; "square" and "3" too (the 0.5.1 test build's SQUARE theme is gone:
//               its word and id, and the old id of GUIDE, come up in GRID)
#ifndef _ui_style_h
#define _ui_style_h

#include <string.h>

namespace ui_style {

enum Style { kOled = 0, kSidecar = 1, kGrid = 2, kStyles };

inline const char *name (int s)
{
	return s == kSidecar ? "sidecar" : s == kGrid ? "guide" : "oled";
}

// a word (the console command's value, or a stored one up to its end), "0".."3" as the old ones (2: GRID, 3 legacy: the 0.5.1 test build's GUIDE, also GRID); -1: none of them
inline int parse (const char *p)
{
	if (p == 0) return -1;
	if (strcmp (p, "oled") == 0 || strcmp (p, "0") == 0) return kOled;
	if (strcmp (p, "sidecar") == 0 || strcmp (p, "classic") == 0 || strcmp (p, "1") == 0) return kSidecar;
	if (strcmp (p, "guide") == 0 || strcmp (p, "grid") == 0 || strcmp (p, "square") == 0 || strcmp (p, "2") == 0 || strcmp (p, "3") == 0) return kGrid;
	return -1;
}

// a word at p that ends at a line's end (display.txt's value, cmdline.txt's option: a NUL, LF or CR); -1: none of them
inline int parse_line (const char *p)
{
	if (p == 0) return -1;
	char b[10];
	unsigned n = 0;
	while (p[n] != 0 && p[n] != '\n' && p[n] != '\r')
	{
		if (n >= sizeof b - 1) return -1;	// longer than any word
		b[n] = p[n];
		n++;
	}
	b[n] = 0;
	return parse (b);
}

// display.txt's text: its uistyle= line (at a line's start); a missing line or a word not known is -1 (the caller's default)
inline int from_text (const char *pText)
{
	const char *p = strstr (pText, "uistyle=");
	while (p != 0 && p != pText && p[-1] != '\n') p = strstr (p + 8, "uistyle=");
	return p == 0 ? -1 : parse_line (p + 8);
}

}	// namespace ui_style

#endif
